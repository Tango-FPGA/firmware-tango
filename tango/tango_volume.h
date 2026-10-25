// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 the Tango authors
/*
 * tango_volume.h -- MiSTer's Core Volume and Global Volume as a portable state machine (no I/O).
 *
 * Tango. Our own code, written from MiSTer's behaviour (Main_MiSTer master c3dc8ed, audio.cpp):
 *   audio.cpp 24-31, 108-137   send_volume: with no audio filter in the core the one byte of UIO_AUDVOL is the global
 *                              attenuation plus the core attenuation (the sum is held at 7 and the clamp is written back
 *                              into the global one); with the filter the core volume travels as the first byte of
 *                              UIO_SET_AFILTER (setFilter) and UIO_AUDVOL carries the global volume alone
 *   audio.cpp 139-165          set_volume: Select = mute toggle; any step while muted only unmutes; louder = attenuation - 1,
 *                              quieter = + 1, 0..7
 *   audio.cpp 167-188, 143-149 set_core_volume: attenuation 0..6; with boost (the core's SET_AFILTER reply bit 1, never in
 *                              the menu core) louder past 0 adds a boost step (bits 6:5, up to 2), quieter removes one first
 *   audio.cpp 229-261          saved a second after the last change: config/Volume.dat (the global byte, & 0x17) and
 *                              config/<core>_volume.cfg (the core byte); load_volume reads Volume.dat always and the core's
 *                              file for every core but the menu core
 *   menu.cpp 4030-4098, 4100-4140   Misc. Options: "Core Volume" and "Global Volume" rows (left/minus quieter, right/plus
 *                              louder; Select mutes the global one, "< Mute >" in place of the bar)
 * The bytes go to the FPGA as SYS 0x26 (the global volume {mute 4, att 3:0}; menu-tango docs/TANGO-SPEC.md "Link commands") and, when the
 * core has the audio filter, as the volume byte of SYS 0x39 (tango_vproc.c).  The page text: a bar of 8 cells, filled
 * cells = 8 - attenuation (glyphs 0x84 filled, 0x85 empty, as the model's 'vol' row), "+" per boost step.
 */
#ifndef TANGO_VOLUME_H
#define TANGO_VOLUME_H

#include <stdint.h>

#define TANGO_VOL_SAVE_MS 1000    /* vol_set_timeout: GetTimer(1000) */

typedef struct {
    uint8_t  vol;                 /* vol_att: bit 4 mute, bits 2:0 attenuation 0..7 */
    uint8_t  core;                /* corevol_att: bits 2:0 attenuation 0..6, bits 6:5 boost steps */
    uint8_t  has_filter;          /* the core answered SET_AFILTER */
    uint8_t  has_boost;           /* ... with bit 1, and it is not the menu core */
    uint8_t  is_menu;
    uint8_t  pending;             /* a change waits for its save */
    uint32_t due_ms;
} tango_vol_t;

/* a core start: global 0 (full volume), core 0 */
void tango_vol_init(tango_vol_t *v, int is_menu);
/* load_volume's two files: the global byte and the core byte, -1 = no such file (a menu core never uses the second) */
void tango_vol_load(tango_vol_t *v, int global, int core);
/* what the probe said (setFilter 25-29); drops boost the core cannot do */
void tango_vol_caps(tango_vol_t *v, int has_filter, int has_boost);

/* one step: cmd +1 louder, -1 quieter; the global one also cmd 0 = mute toggle.  Returns 1 if the value changed.
 * A change starts the one-second save timer. */
int  tango_vol_global_step(tango_vol_t *v, int cmd, uint32_t now_ms);
int  tango_vol_core_step(tango_vol_t *v, int cmd, uint32_t now_ms);

/* the byte of SYS 0x26 (writes the clamp back into the global volume as send_volume does) and the volume byte of SYS 0x39 */
uint8_t tango_vol_audvol(tango_vol_t *v);
uint8_t tango_vol_corebyte(const tango_vol_t *v);
uint8_t tango_vol_file_global(const tango_vol_t *v);      /* the bytes the two files hold */
uint8_t tango_vol_file_core(const tango_vol_t *v);

/* the save timer: 1 once, when the second after the last change is over; wait_ms = ms until then, -1 = nothing pending */
int  tango_vol_save_due(tango_vol_t *v, uint32_t now_ms);
int  tango_vol_wait_ms(const tango_vol_t *v, uint32_t now_ms);

/* the Misc. Options value text: a bar ("\x84" x filled, "\x85" x empty, "+" per boost step) or "< Mute >" (global only).
 * out >= 16 bytes. */
void tango_vol_text(const tango_vol_t *v, int core, char *out);

#endif
