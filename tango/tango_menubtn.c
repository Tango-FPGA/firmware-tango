// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 the Tango authors
/* tango_menubtn.c -- see tango_menubtn.h. Tango, our own code. */
#include <stdio.h>
#include <string.h>
#include "tango_menubtn.h"
#include "tango_padmap.h"

void tango_mb_default(tango_mb_t *m) { m->a = TANGO_MB_DEF_A; m->b = TANGO_MB_DEF_B; }
int tango_mb_is_default(const tango_mb_t *m) { return m->a == TANGO_MB_DEF_A && m->b == TANGO_MB_DEF_B; }
int tango_mb_valid(int a, int b) { return a >= 4 && a < 32 && b >= 4 && b < 32; }

int tango_mb_from_map(tango_mb_t *m, const uint32_t map[32])
{
    uint32_t c1 = map[TANGO_SYS_BTN_OSD], c2 = map[TANGO_SYS_BTN_OSD + 1];
    tango_mb_default(m);
    if (!c2) c2 = c1;                                                    /* input.cpp 3019 */
    if (c1 < 0x100 || c1 >= 0x120 || c2 < 0x100 || c2 >= 0x120 || !tango_mb_valid((int)(c1 - 0x100), (int)(c2 - 0x100))) return 0;
    m->a = (uint8_t)(c1 - 0x100); m->b = (uint8_t)(c2 - 0x100);
    return 1;
}

void tango_mb_to_map(const tango_mb_t *m, uint32_t map[32])
{
    memset(map, 0, 32 * sizeof map[0]);
    map[TANGO_SYS_BTN_OSD] = TANGO_PM_CODE(m->a);
    map[TANGO_SYS_BTN_OSD + 1] = TANGO_PM_CODE(m->b);                    /* a lone button twice (3388-3389) */
}

void tango_mb_mapname(uint16_t vid, uint16_t pid, char *out, int cap) { snprintf(out, (size_t)cap, "input_%04x_%04x_v3.map", vid, pid); }

static void join(int a, int b, char out[56])
{
    char x[24], y[24];
    tango_pmed_codename(TANGO_PM_CODE(a), x);
    if (a == b || b < 0) { snprintf(out, 56, "%s", x); return; }
    tango_pmed_codename(TANGO_PM_CODE(b), y);
    snprintf(out, 56, "%s + %s", x, y);
}
void tango_mb_name(const tango_mb_t *m, char out[56]) { join(m->a, m->b, out); }

int tango_mbd_step(tango_mbdet_t *d, int a, int b, uint32_t hold_ms, uint32_t raw, uint32_t now_ms)
{
    int on = ((raw >> a) & 1) && ((raw >> b) & 1);       /* a partial press does nothing (input.cpp 3187-3208) */
    if (!on) { d->down = 0; d->fired = 0; return 0; }
    if (d->down) return 0;
    d->down = 1; d->fired = 0; d->t0 = now_ms;
    if (!hold_ms) { d->fired = 1; return 1; }            /* the rising edge (3207) */
    return 0;
}
int tango_mbd_edge(tango_mbdet_t *d, int a, int b, uint32_t raw, uint32_t now_ms)
{
    int was = d->down;
    if (tango_mbd_step(d, a, b, 0, raw, now_ms)) return 1;
    return was && !d->down ? -1 : 0;
}

int tango_mkey_press(tango_mkey_t *m, int osd_open)
{
    m->down = 1;
    if (!osd_open) return 0;                  /* closed: the press does nothing (menu.cpp 1338-1344) */
    m->ignore = 1;                            /* ignore_osd_release */
    return 1;
}
int tango_mkey_release(tango_mkey_t *m)
{
    int act = !m->ignore;                     /* menu.cpp 1345-1349 */
    m->ignore = 0; m->down = 0;
    return act;
}
void tango_mkey_swallow(tango_mkey_t *m) { if (m->down) m->ignore = 1; }

int tango_mbd_poll(tango_mbdet_t *d, uint32_t hold_ms, uint32_t now_ms)
{
    if (!d->down || d->fired || !hold_ms || now_ms - d->t0 < hold_ms) return 0;
    d->fired = 1;
    return 1;
}
int tango_mbd_wait_ms(const tango_mbdet_t *d, uint32_t hold_ms, uint32_t now_ms)
{
    if (!d->down || d->fired || !hold_ms) return -1;
    uint32_t el = now_ms - d->t0;
    return el >= hold_ms ? 0 : (int)(hold_ms - el);
}

/* ---- learning (input.cpp 3299, 3365-3394, 3445-3452) ---- */
void tango_mbl_start(tango_mblearn_t *l, const uint32_t raw[2], uint32_t now_ms)
{
    memset(l, 0, sizeof *l);
    l->on = 1; l->port = -1; l->first = l->second = -1; l->t0 = now_ms;
    l->held[0] = raw[0]; l->held[1] = raw[1];
}
void tango_mbl_cancel(tango_mblearn_t *l) { l->on = 0; l->port = -1; l->first = l->second = -1; }

int tango_mbl_raw(tango_mblearn_t *l, int port, uint32_t raw, uint32_t now_ms)
{
    if (!l->on || port < 0 || port > 1) return TANGO_MBL_NONE;
    if (l->port >= 0 && port != l->port) return TANGO_MBL_NONE;          /* one pad: two pads never combine */
    l->held[port] &= raw;                                                /* what went up is a button again */
    uint32_t held = l->held[port];
    int r = TANGO_MBL_NONE;
    if (l->first < 0) {                                                   /* the first fresh press */
        for (int b = 0; b < 32; b++) {
            if (!((raw >> b) & 1) || ((held >> b) & 1)) continue;
            if (b < 4) return TANGO_MBL_REFUSED;
            l->first = (int8_t)b; l->port = (int8_t)port; l->t0 = now_ms; r = TANGO_MBL_REDRAW;
            break;
        }
        if (l->first < 0) return r;
    }
    if (l->second < 0)                                                    /* a second, different button while the first is held (3445) */
        for (int b = 4; b < 32; b++)
            if (b != l->first && ((raw >> b) & 1) && !((held >> b) & 1) && ((raw >> l->first) & 1)) { l->second = (int8_t)b; l->t0 = now_ms; r = TANGO_MBL_REDRAW; break; }
    if (!((raw >> l->first) & 1)) {                                       /* the first button released: done (3450-3452) */
        l->a = (uint8_t)l->first; l->b = (uint8_t)(l->second >= 0 ? l->second : l->first);
        l->on = 0;
        return TANGO_MBL_DONE;
    }
    return r;
}

int tango_mbl_tick(tango_mblearn_t *l, uint32_t now_ms)
{
    if (!l->on || l->first >= 0 || now_ms - l->t0 < TANGO_MB_LEARN_MS) return TANGO_MBL_NONE;
    l->on = 0; return TANGO_MBL_GAVE_UP;
}

void tango_mbl_text(const tango_mblearn_t *l, char out[56])
{
    out[0] = 0;
    if (l->first >= 0) join(l->first, l->second, out);
}
