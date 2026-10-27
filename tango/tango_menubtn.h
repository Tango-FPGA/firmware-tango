// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 the Tango authors
/*
 * tango_menubtn.h -- the OSD (menu) button: one pad button, or two pad buttons held together, that open and close the menu.
 *
 * Tango. Our own code, written from MiSTer's behaviour (Main_MiSTer master c3dc8ed):
 *   input.h 57, 68-69   SYS_BTN_OSD_KTGL = 20 ("20 for keyboard, 21+22 for gamepad"): the pad's menu button is mmap[21] and mmap[22]
 *   input.cpp 3005-3019 mmap is the controller's SYSTEM map: load_map(get_map_name(dev, 1)) = config/inputs/input_<vid>_<pid>[_m]_v3.map
 *                       (written by "Define joystick buttons" in the MENU CORE, one file per controller model, valid in every core), else the
 *                       gamecontrollerdb mapping, else def_mmap (2924: the guide button); a lone button is stored twice (3019)
 *   input.cpp 3187-3216 osd_combo, per device: bit 0 follows mmap[22], bit 1 follows mmap[21] (code and key down); the OSD event is
 *                       the RISING EDGE of both (old_combo != 3 && osd_combo == 3, 3207) which is only the key's PRESS: the OSD opens on the
 *                       RELEASE, osd_event 2 (3211-3214), and closes on the press: a lone button fires on its press, a pair needs both
 *                       held, a partial press does nothing, two pads never combine.  osd_event 1 -> joy_digital(BTN_OSD) (3620) -> KEY_MENU (2527)
 *   input.cpp 3210, 3217 the 1 s hold exists ONLY while mapping a core's buttons (!is_menu()): MiSTer has NO hold time for the menu button
 *   input.cpp 3299, 3365-3394, 3445-3452  LEARNING (is_menu(), the last step "Menu" of Define joystick buttons; menu.cpp 4258-4262 prints
 *                       "(can use 2-button combo)", 7546 names it "Menu"): the first button pressed becomes mmap[21] and mmap[22] (3388-3389,
 *                       mapping_current_key = 0 "allow 2 buttons to be pressed"); a SECOND DIFFERENT button pressed while the first is still held
 *                       becomes mmap[22] ("Set combo", 3445-3448); the step ends when the first button is released (3450-3452).  The duplicate
 *                       check is off for this step (3382).
 *   input.cpp 3761, 2216-2271, 6630-6646  the button code is still handed to the core's own button map as any other code: the extra
 *                       argument of joy_digital only keeps autofire's bookkeeping (BTN_OSD / osd_autofire_consumed) and the chord rules of the
 *                       Advanced map apart.  Nothing is withheld from the game.  The FPGA's pad map does the same here: the raw word reaches the core
 *                       through the core map whatever the menu button is.
 *   recovery in MiSTer: the keyboard's F12 / Menu key and the IO board's Menu button (always there), or deleting the map file.
 * Tango's differences, on purpose:
 *   - the file is the same name and place (config/inputs/input_<vid>_<pid>_v3.map, uint32 map[32], MiSTer's NUMBUTTONS) but the codes are Tango's
 *     (0x100 + raw bit, as in the core maps: MiSTer's are Linux event codes and not portable); only [21] and [22] are used, the rest stays 0
 *     (the system map's other buttons are the FPGA's fixed DualShock 4 decode, menu-tango docs/TANGO-SPEC.md "Controllers")
 *   - learning is in the LAUNCHER (System Settings > Define joystick buttons), as MiSTer's is in the menu core; it reads the raw word of both USB
 *     ports by polling HID REPORT, and a learn that sees nothing for TANGO_MB_LEARN_MS gives up (MiSTer has Esc, and Tango has no key on a pad)
 *   - the d-pad (raw bits 0-3) cannot be the menu button: it is how the menu is moved
 *   - a console has no menu button on the box and many users no keyboard, so, beyond MiSTer: holding Share + Options (the default pair) for
 *     TANGO_MB_ESCAPE_MS opens the menu whatever was defined (only a closed menu is opened); F12 on a keyboard and deleting the file stay as in MiSTer
 * The default is Share + Options (a DualShock 4's Select + Start in GET_DB9) on the rising edge.  That is the FPGA's
 * fixed decode; a controller with a defined button is read from the raw word of its USB port (HID REPORT), and while any connected controller has
 * one the GET_DB9 combo opens nothing.  The raw word is only read while a core runs (tango_fw.c); the launcher keeps Select + Start.
 */
#ifndef TANGO_MENUBTN_H
#define TANGO_MENUBTN_H

#include <stdint.h>

#define TANGO_SYS_BTN_OSD   21           /* MiSTer SYS_BTN_OSD_KTGL + 1: mmap[21], mmap[22] */
#define TANGO_MB_ESCAPE_MS  3000         /* Share + Options held this long opens a closed menu whatever was defined */
#define TANGO_MB_LEARN_MS   8000         /* a learn with no button pressed for this long is given up */
#define TANGO_MB_DEF_A      12           /* the default pair: raw bits Share, Options */
#define TANGO_MB_DEF_B      13

typedef struct { uint8_t a, b; } tango_mb_t;      /* raw bits; a single button has both the same */

void tango_mb_default(tango_mb_t *m);
int  tango_mb_is_default(const tango_mb_t *m);
int  tango_mb_valid(int a, int b);                /* two raw bits 4..31 */
/* the system map (uint32 map[32]) -> the definition: codes 0x100 + raw bit in [21] and [22] ([22] = [21] when empty, 3019); anything else = default.
 * 1 = the map defined one */
int  tango_mb_from_map(tango_mb_t *m, const uint32_t map[32]);
void tango_mb_to_map(const tango_mb_t *m, uint32_t map[32]);
void tango_mb_mapname(uint16_t vid, uint16_t pid, char *out, int cap);        /* input_<vid>_<pid>_v3.map (get_map_name(dev, 1)) */
void tango_mb_name(const tango_mb_t *m, char out[56]);                       /* "Share + Options", "PS" */

/* the raw-word detector, one per USB port: a pair (a, b), both down, rising edge (hold_ms 0), or held for hold_ms (the escape) */
typedef struct { uint32_t t0; uint8_t down, fired; } tango_mbdet_t;
int  tango_mbd_step(tango_mbdet_t *d, int a, int b, uint32_t hold_ms, uint32_t raw, uint32_t now_ms);   /* 1 = fire now */
int  tango_mbd_poll(tango_mbdet_t *d, uint32_t hold_ms, uint32_t now_ms);                              /* 1 = held for hold_ms now (once per press) */
int  tango_mbd_wait_ms(const tango_mbdet_t *d, uint32_t hold_ms, uint32_t now_ms);                     /* ms until poll has something to do, -1 = nothing */

/* press = +1 / release = -1 / nothing = 0 of the pair (a, b) both down (hold_ms 0 only): the two edges MiSTer's osd_event 1 and 2 are (input.cpp 3206-3215) */
int  tango_mbd_edge(tango_mbdet_t *d, int a, int b, uint32_t raw, uint32_t now_ms);

/* The menu key's two edges, whatever made them (a pad's combo or button, a keyboard's F12 / Menu key): MiSTer's rule.
 * user_io_kbd sends F12 on the press and F12|UPSTROKE on the release; menu.cpp 1338-1349:
 * the press acts only if the OSD is shown (it closes, and sets ignore_osd_release); the release acts unless that flag is set (it opens),
 * and clears it.  `down` = a menu key is held now (so a 3 s escape that opens the menu under a held key can swallow that key's release). */
typedef struct { uint8_t ignore, down; } tango_mkey_t;
int  tango_mkey_press(tango_mkey_t *m, int osd_open);     /* 1 = act now (the menu closes) */
int  tango_mkey_release(tango_mkey_t *m);                 /* 1 = act now (the menu opens; a release nothing swallowed) */
void tango_mkey_swallow(tango_mkey_t *m);                 /* the menu was opened by other means under a held key: its release does nothing */

/* ---- learning: "Press: Menu (can use 2-button combo)" ---- */
enum { TANGO_MBL_NONE = 0, TANGO_MBL_REDRAW, TANGO_MBL_DONE, TANGO_MBL_REFUSED, TANGO_MBL_GAVE_UP };
typedef struct {
    uint8_t  on;
    int8_t   port;                /* the pad that pressed first, -1 = none yet */
    int8_t   first, second;       /* the raw bits taken, -1 = none */
    uint32_t held[2];             /* per port: the bits already down when the learn began: not buttons until released and pressed again */
    uint32_t t0;                  /* the last press, or the start */
    uint8_t  a, b;                /* the result when DONE */
} tango_mblearn_t;
void tango_mbl_start(tango_mblearn_t *l, const uint32_t raw[2], uint32_t now_ms);
void tango_mbl_cancel(tango_mblearn_t *l);
/* a report; DONE when the first button is released (a, b = the definition); REFUSED = the d-pad was pressed (taken nowhere) */
int  tango_mbl_raw(tango_mblearn_t *l, int port, uint32_t raw, uint32_t now_ms);
int  tango_mbl_tick(tango_mblearn_t *l, uint32_t now_ms);                    /* GAVE_UP after TANGO_MB_LEARN_MS with nothing pressed */
/* the text of what is held so far: "Share" or "Share + Options" ("" = nothing yet) */
void tango_mbl_text(const tango_mblearn_t *l, char out[56]);

#endif
