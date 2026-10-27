// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 the Tango authors
/*
 * tango_volume.c -- see tango_volume.h. Tango.
 *
 * Derived from Main_MiSTer (https://github.com/MiSTer-devel/Main_MiSTer, GPL-3.0; compared at c3dc8ed): these
 * functions closely paraphrase its code, so this file is GPL-3.0-or-later with the whole firmware
 * (LICENSE-GPL-3.0, NOTICE):
 *   clamp_core              audio.cpp get_core_volume (144-152)
 *   tango_vol_core_step     audio.cpp set_core_volume (187-227)
 * Upstream notices: audio.cpp has no copyright or licence header (the repository's LICENSE is GPL-3.0).
 */
#include <string.h>
#include "tango_volume.h"

static void touch(tango_vol_t *v, uint32_t now) { v->pending = 1; v->due_ms = now + TANGO_VOL_SAVE_MS; }

/* get_core_volume (audio.cpp 143-149): attenuation <= 6, boost steps <= 2 */
static uint8_t clamp_core(uint8_t c)
{
    int boost = c & 0x60;
    if (boost > 0x40) boost = 0x40;
    int att = c & 7;
    if (att > 6) att = 6;
    return (uint8_t)(att | boost);
}

void tango_vol_init(tango_vol_t *v, int is_menu)
{
    memset(v, 0, sizeof *v);
    v->is_menu = (uint8_t)(is_menu != 0);
}

void tango_vol_load(tango_vol_t *v, int global, int core)
{
    if (global >= 0) v->vol = (uint8_t)(global & 0x17);
    if (core >= 0 && !v->is_menu) v->core = (uint8_t)core;
    v->core = clamp_core(v->core);
}

void tango_vol_caps(tango_vol_t *v, int has_filter, int has_boost)
{
    v->has_filter = (uint8_t)(has_filter != 0);
    v->has_boost = (uint8_t)(has_filter && has_boost && !v->is_menu);
    if (!v->has_boost) v->core &= 7;                     /* setFilter 28 */
}

int tango_vol_global_step(tango_vol_t *v, int cmd, uint32_t now)
{
    uint8_t o = v->vol;
    v->vol &= 0x17;
    if (cmd == 0) v->vol ^= 0x10;                        /* Select: mute toggle */
    else if (v->vol & 0x10) v->vol &= 0xF;               /* a step while muted only unmutes */
    else if (cmd < 0 && v->vol < 7) v->vol++;
    else if (cmd > 0 && v->vol > 0) v->vol--;
    touch(v, now);                                       /* set_volume arms the timer even when nothing moved */
    return v->vol != o;
}

int tango_vol_core_step(tango_vol_t *v, int cmd, uint32_t now)
{
    uint8_t o = v->core;
    if (v->has_boost) {
        int boost = v->core & 0x60, att = v->core & 7;
        if (cmd < 0) {
            if (boost) { att = 0; boost -= 0x20; }
            else if (att < 6) att++;
        }
        if (cmd > 0) {
            if (boost || !att) { boost += 0x20; if (boost > 0x40) boost = 0x40; }
            else if (att > 0) att--;
        }
        v->core = (uint8_t)(att | boost);
    } else {
        int att = v->core & 7;
        if (cmd < 0 && att < 6) att++;
        if (cmd > 0 && att > 0) att--;
        v->core = (uint8_t)att;
    }
    v->core = clamp_core(v->core);
    touch(v, now);
    return v->core != o;
}

uint8_t tango_vol_audvol(tango_vol_t *v)
{
    int core = clamp_core(v->core);
    v->vol &= 0x17;
    if (!v->has_filter) {                                /* send_volume (audio.cpp 126-137) */
        if (!(v->vol & 0x10) && (v->vol & 7) + core > 7) v->vol = (uint8_t)(7 - core);
        return (uint8_t)(v->vol + core);
    }
    return v->vol;
}

uint8_t tango_vol_corebyte(const tango_vol_t *v) { return clamp_core(v->core); }
uint8_t tango_vol_file_global(const tango_vol_t *v) { return (uint8_t)(v->vol & 0x17); }
uint8_t tango_vol_file_core(const tango_vol_t *v) { return clamp_core(v->core); }

int tango_vol_save_due(tango_vol_t *v, uint32_t now)
{
    if (!v->pending || (int32_t)(now - v->due_ms) < 0) return 0;
    v->pending = 0;
    return 1;
}

int tango_vol_wait_ms(const tango_vol_t *v, uint32_t now)
{
    if (!v->pending) return -1;
    int32_t d = (int32_t)(v->due_ms - now);
    return d > 0 ? d : 0;
}

void tango_vol_text(const tango_vol_t *v, int core, char *out)
{
    int n = 0;
    if (!core && (v->vol & 0x10)) { strcpy(out, "< Mute >"); return; }
    int total, filled;
    if (core) { total = 8; filled = 8 - (v->core & 7); }
    else {
        int c = v->has_filter ? 0 : (clamp_core(v->core) & 7);   /* audio_filter_en() < 0: the global bar is shorter by the core's attenuation */
        total = 8 - c; filled = total - (v->vol & 7);
    }
    for (int i = 0; i < total; i++) out[n++] = (char)(i < filled ? 0x84 : 0x85);
    if (core) { if (v->core & 0x60) out[n++] = '+'; if (v->core & 0x40) out[n++] = '+'; }
    out[n] = 0;
}
