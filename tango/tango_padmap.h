// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 the Tango authors
/*
 * tango_padmap.h -- MiSTer's "Define <core> buttons" and the core pad map, for Tango.
 *
 * Tango. Parts of tango_padmap.c are copied or closely paraphrased from Main_MiSTer (GPL-3.0); its
 * header lists them, with the upstream notices. GPL-3.0-or-later with the whole firmware (NOTICE).
 * The rules, from Main_MiSTer master 5a3a086:
 *   menu.cpp 2998-3019      System row 2: parse_buttons(), start_map_setting(joy_bcount ? joy_bcount + 4 : 8)
 *   menu.cpp 4188-4431      MENU_JOYDIGMAP..4: title "Define buttons"; row 3 "Press: <name>" (RIGHT LEFT DOWN UP,
 *                           then the core's J names, or BUTTON A/B/X/Y without them: joy_button_map, menu.cpp 267);
 *                           row 5 "Joystick ID: vvvv:pppp"; row 7 "User/Menu > Undefine" once past the first
 *                           button; rows 8/9 "Menu-hold > Cancel", "Enter > Finish"; row 10 "F12 > Clear all";
 *                           "Clearing" / "Canceling" on row 4; the end of the first pass asks "Do you want to setup
 *                           alternative buttons?" No / Yes (Yes = a second pass into the high 16 bits)
 *   menu.cpp 1394-1395, 1404-1407  Esc (release) = menu = cancel, Enter = select = finish
 *   input.cpp 1740-1768, 1873-1907  start_map_setting / finish_map_setting (save unless dismissed)
 *   input.cpp 1846-1854, 5519       the file: <core>_input_<vid>_<pid>_v3.map in config/inputs/, uint32 map[32]
 *   input.cpp 3199-3216             the OSD button combo: a short press = skip (undefine), held 1 s = cancel
 *   input.cpp 3272-3432, 3562-3585  capture: the first press picks the device and clears its map; a button is
 *                           taken on its press and the prompt advances on its release (one key at a time); a code
 *                           already used in this pass is refused; J names "-" are skipped
 *   input.cpp 3754                  a core map entry: low 16 = code, high 16 = alternative; either sets bit i
 *   input.cpp 3059-3080, joymapping.cpp 38-218  no file: map_joystick() guesses from the system map and J/jn/jp
 *   input.cpp 6477-6540             get_btn / parse_buttons: J, jn, jp entries of CONF_STR
 * Tango (menu-tango docs/TANGO-SPEC.md "Controllers"): the pads are read by the FPGA; codes here are 0x100 + the raw bit (the
 * raw word of HID REPORT), >= 256 as MiSTer's joystick codes; the system map is the FPGA's fixed default decode.
 * Differences: no keyboard as a mapped device; the menu button is two keys (Share + Options), so the first of them,
 * taken when pressed, is given back when the second completes the combo (as MiSTer's one-key menu button is never
 * taken); a core without a J entry and without a file keeps the FPGA's default
 * core map (MiSTer's guess would map the d-pad only, and every Tango core so far relies on the default buttons).
 */
#ifndef TANGO_PADMAP_H
#define TANGO_PADMAP_H

#include <stdint.h>
#include "tango_layout.h"
#include "tango_padlearn.h"

#define TANGO_PM_BTNS     32
#define TANGO_PM_NAMES    28
#define TANGO_PM_CODE(r)  (0x100u + (r))
/* MiSTer SYS_BTN_* (input.h 37-48) */
enum { PM_RIGHT, PM_LEFT, PM_DOWN, PM_UP, PM_A, PM_B, PM_X, PM_Y, PM_L, PM_R, PM_SELECT, PM_START };
/* the raw word's bits (spec "Raw word") */
#define TANGO_RAW_SHARE   12
#define TANGO_RAW_OPTIONS 13

/* the system map (MiSTer mmap / def_mmap): the FPGA's fixed DS4 decode as codes, indexed by SYS_BTN_* */
extern const uint32_t tango_pm_mmap[TANGO_PM_BTNS];

/* parse_buttons: the fields of the CONF_STR "J..." entry after its first comma (up to 28). Returns joy_bcount. */
int  tango_pm_bnames(const char *conf, char names[TANGO_PM_NAMES][32]);
/* the same without the table (static RAM is heap here, build.sh): the count, and one name (returns its length, 0 = none) */
int  tango_pm_bcount(const char *conf);
int  tango_pm_bname(const char *conf, int idx, char out[32]);
/* map_joystick(map, mmap) with the CONF_STR's J / jn / jp (gamepad_defaults = 0: MiSTer.ini's default) */
void tango_pm_guess(const char *conf, uint32_t map[TANGO_PM_BTNS]);
/* map_joystick over the pad's own system map: mmap[0..11] = what its Define joystick buttons took (tango_pm_sysmap_valid), else tango_pm_mmap */
void tango_pm_guess_mm(const char *conf, const uint32_t *mmap, uint32_t map[TANGO_PM_BTNS]);
int  tango_pm_sysmap_valid(const uint32_t m[TANGO_PM_BTNS]);      /* the twelve buttons all hold a raw code (0x100 + bit) */
/* <core>_input_<vid>_<pid>_v3.map (get_map_name, not unique) */
void tango_pm_mapname(const char *core, uint16_t vid, uint16_t pid, char *out, int cap);
/* a core map as SET_PADMAP pairs (primary, alternative) for joystick bits 0..31: FF = none. Returns 32. */
int  tango_pm_pairs(const uint32_t map[TANGO_PM_BTNS], uint8_t pairs[2 * TANGO_PM_BTNS]);

/* ---- the Define buttons dialog (MENU_JOYDIGMAP*) ---- */
enum { PMC_MAP = 0, PMC_ASK, PMC_NEED };                 /* the page shown: the prompts, or the alternative-buttons question */
/* results: PMC_SAVED = write the map file (then PMC_ASK shows the question, else the dialog ends);
 * PMC_EXIT = the dialog ends, the map is already saved; PMC_DISMISSED = the dialog ends, nothing saved */
enum { PMC_NONE = 0, PMC_REDRAW, PMC_SAVED, PMC_EXIT, PMC_DISMISSED };
enum { PMC_KEY_ENTER = 1, PMC_KEY_ESC, PMC_KEY_F12, PMC_KEY_UP, PMC_KEY_DOWN, PMC_KEY_SPACE };

typedef struct { uint16_t vid, pid; uint8_t conn, known; } tango_pmpad_t;   /* known: has a system map or a built-in decode (menu.cpp 4366: has_default_map) */

typedef struct {
    int      state;                  /* PMC_MAP / PMC_ASK */
    int      count;                  /* mapping_count */
    int      button;                 /* mapping_button */
    int      set;                    /* mapping_set: 0 = primary (low 16), 1 = alternative (high 16) */
    int      first;                  /* joymap_first */
    int      dev;                    /* mapping_dev: the port, -1 = none yet */
    int      clear;                  /* mapping_clear */
    uint16_t vid, pid;
    uint32_t map[TANGO_PM_BTNS];
    int      cur_key, cur_dev;       /* mapping_current_key / _dev (one key at a time) */
    int      key_mapped;
    uint32_t prev[2];                /* raw word per port as last seen */
    int      combo[2];               /* osd_combo per port: bit0 Share, bit1 Options */
    int      osd_timer_on;           /* the OSD combo is held: cancel once 1 s has passed */
    uint32_t osd_t0;
    int      ask_sel;                /* PMC_ASK: 0 = No, 1 = Yes */
    int      bcount;
    uint8_t  sys;                    /* 1 = the menu core's flow (MENU_JOYSYSMAP): the system map, the names A..Start, no combo, no alternative question */
    const char *conf;                /* the core's CONF_STR (the J names are read from it when needed) */
    /* page model */
    tango_row_t rows[14];
    char     txt[6][48];
    struct tango_pmlearn *lm;        /* the learner behind this walk (tango_pmcap_learn), NULL = presses are the raw word's edges as MiSTer's flow reads them */
    /* ---- kept across tango_pmcap_start (the pads are known before any dialog opens) ---- */
    uint8_t  mb[2][2];               /* the menu button's raw bits per USB port (the pad on that port: its system map, tango_menubtn.h); a single button has both the same */
    tango_pmpad_t pads[2];           /* USB-A1 / USB-A2 as the last HID REPORT said */
} tango_pmcap_t;

void tango_pmcap_start(tango_pmcap_t *c, const char *conf);
/* "Define joystick buttons" of the menu core (menu.cpp 7527-7552, MENU_JOYSYSMAP): start_map_setting(joy_bcount + 6) with joy_bnames = A B X Y L R Select Start:
 * RIGHT LEFT DOWN UP, then those eight, one at a time.  The result is map[0..11] (codes 0x100 + the raw bit; SYS_BTN_* order).  The Menu step
 * follows in the launcher (tango_menubtn.h tango_mbl_*) and the caller writes input_<vid>_<pid>_v3.map.  Mouse emulation, the analog-stick steps and
 * Menu: OK / Menu: Back (input.h 49-68) are not asked: Tango's pads carry one raw word, no axes, and the OSD keys are fixed (menu-tango docs/LAUNCHER.md) */
void tango_pmcap_start_sys(tango_pmcap_t *c);

/* ---- The learner behind the menu core's Define joystick buttons.
 * MiSTer's Linux hands every pad over as evdev events, whatever its descriptor, so MiSTer's Define always sees a press.  Tango's raw word is the FPGA's decode of the
 * pad's report; a pad whose decode does not move for some control (a d-pad on the axes while the descriptor declares a hat, a button past the fourteenth, a pad with no
 * usable descriptor) would give the walk nothing to take.  With a learner the walk takes presses from the REPORT2 payload instead: the rest state first, then each
 * button asked for is learned as an entry of the pad's decode table whose output is the standard raw bit of that button (tango_pm_mmap), and the saved system map is the
 * standard one (map[i] = tango_pm_mmap[i], or 0 for a button skipped).  The firmware allocates the state, fills the ports' layouts, feeds REPORT2 samples to
 * tango_pmcap_sample, and calls plsess_finish on tango_pmlearn_session when the walk ends in PMC_SAVED (or drops it on a cancel). */
typedef struct tango_pmlearn {
    plsess_t       S;
    plcap_t        C[2];
    const hp_layout_t *L[2];                         /* the ports' parsed descriptors (the caller's; NULL = learned bit by bit); 2.4 KB each, so the firmware allocates one per pad that is there */
    uint16_t       vid[2], pid[2], ver[2], ver_hid[2];
    uint8_t        use[2];                           /* the port has a pad the learner may watch */
    uint8_t        ready[2];                         /* its rest state is complete */
    uint8_t        settled[2];                       /* the pad's raw word has been 0 for 150 ms (the A that opened the page is let go) or 3 s have passed: the rest state may start */
    uint32_t       t_quiet[2], t_first[2];
    uint8_t        began;                            /* S is begun (for dev) */
    uint8_t        dpad_done;                        /* the four directions were learned as a group */
    char           msg[28];                          /* the last result, "No change: press it again" */
} tango_pmlearn_t;
void tango_pmlearn_zero(tango_pmlearn_t *lm);
/* register a pad on `port` for the walk; `layout` (may be NULL: the pad is learned bit by bit) must outlive the walk */
void tango_pmlearn_port(tango_pmlearn_t *lm, int port, uint16_t vid, uint16_t pid, uint16_t ver, uint16_t ver_hid, const hp_layout_t *layout);
void tango_pmcap_learn(tango_pmcap_t *c, tango_pmlearn_t *lm);                         /* after tango_pmcap_start_sys */
/* one REPORT2 sample of `port` (NULL: only the clock moved): PMC_NONE / PMC_REDRAW / PMC_SAVED as tango_pmcap_raw */
int  tango_pmcap_sample(tango_pmcap_t *c, int port, const pl_sample_t *s, uint32_t now_ms);
/* the walk is over: directions taken but not completed as a group (one skipped, Enter early) are learned one by one, and the ones that cannot be are removed from the map */
void tango_pmcap_learn_finish(tango_pmcap_t *c);
plsess_t *tango_pmlearn_session(tango_pmlearn_t *lm);                                  /* the session to finish (NULL: nothing was learned) */
/* The source of presses is a function the platform supplies (tango_platform_t.pad_raw in the launcher, the HID REPORT in a core): (port, vid, pid, raw word).
 * tango_pmcap_raw is the one place a press enters the dialog, so a different decode (the learner on REPORT2) replaces the source, not the dialog. */
typedef int (*tango_pmcap_source_t)(void *ctx, int port, uint16_t *vid, uint16_t *pid, uint32_t *raw);
/* one HID REPORT of a port (raw word, the pad's VID/PID) at time now_ms */
int  tango_pmcap_raw(tango_pmcap_t *c, int port, uint16_t vid, uint16_t pid, uint32_t raw, uint32_t now_ms);
int  tango_pmcap_key(tango_pmcap_t *c, int key);
/* 1 = the "Canceling" line should now show (the combo held 1 s) */
int  tango_pmcap_canceling(const tango_pmcap_t *c, uint32_t now_ms);
void tango_pmcap_page(tango_pmcap_t *c, tango_page_t *p, const char *banner, uint32_t now_ms);

/* the text of a raw code in a map ("Circle", "R1", "-" for none) */
void tango_pmed_codename(uint32_t code, char out[24]);
/* The default map of a core without a file: map_joystick's guess with J names, else the FPGA's standard twelve (the map the
 * firmware applies to a pad that has no file, tango_fw.c pm_apply) */
void tango_pm_default(const char *conf, uint32_t map[TANGO_PM_BTNS]);
/* the menu button's raw bits (the combo that skips and cancels in Define buttons) */
void tango_pmcap_set_menu(tango_pmcap_t *c, int port, int bit_a, int bit_b);
/* a REPORT's pad identity, recorded whatever the dialog (connected = 0: the pad went away) */
void tango_pmcap_pad(tango_pmcap_t *c, int port, int connected, uint16_t vid, uint16_t pid);
/* a pad has a system map or a built-in decode: Define <core> buttons may go on (menu.cpp 4366-4373 otherwise says "You need to define this joystick in Menu core first") */
void tango_pmcap_known(tango_pmcap_t *c, int port, int known);

#endif
