// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 the Tango authors
/*
 * tango_launcher.h -- the Tango launcher (MiSTer's menu core, Cores page) as a portable state machine.
 *
 * Tango. Our own code. It owns the Cores browser and the OSD link; the firmware feeds it
 * input events and gives it a way to load a core (tango_platform_t). Everything here also runs on the
 * host, where the tests record the SPI traffic into stream files for the FPGA testbench.
 *
 * Keys (MiSTer menu.cpp 5594-5715 for the Cores browser):
 *   Up / Down           previous / next entry, wrapping (SCANF_PREV / SCANF_NEXT)
 *   Left / Right        one page back / forward (KEY_LEFT = KEY_PAGEUP, KEY_RIGHT = KEY_PAGEDOWN)
 *   A (Enter)           open the folder, ".." goes up, or load the core
 *   B (Back)            Tango: up one folder; at the root, MiSTer's own B = Menu: System Settings (as
 *                       MiSTer: MiSTer's B opens System Settings from any folder).
 *   X (Tab)             rescan from the top; on ".." that is up (menu.cpp 5558-5575); Home / End; plus / minus (a pad's L / R) the
 *                       next / previous initial letter (file_io.cpp 1953-2000); A + B together (Backspace) and B with a typed
 *                       filter active clear the filter and rescan from the top; Select / the grave key = Recent Cores (not built)
 *   Menu                MiSTer's menu core (menu.cpp 1633-1642, 7368-7510, 4030-4165): Cores -> System Settings
 *                       (cursor on exit); System: Menu / B / exit -> Cores, Left -> Misc. Options (cursor on exit),
 *                       Select on Reboot: released = reload the menu core, held (tango_reboot_cold) = cold reboot;
 *                       Misc: Menu / B / exit -> Cores, Right on a row that does not take it -> System Settings.
 *
 * Differences, all Tango's, so the pages never promise what is not built: System Settings has no Scripts or Help
 * (menu-tango docs/LAUNCHER.md) and no "Switch to USB"; Remap keyboard and Define joystick buttons, and Misc. Options'
 * Config / Boot core / Players, are dim placeholders that say "Not available yet" when selected; Misc.'s Information
 * block lists only what the firmware knows.
 */
#ifndef TANGO_LAUNCHER_H
#define TANGO_LAUNCHER_H

#include "tango_proto.h"
#include "tango_browser.h"
#include "tango_volume.h"
#include "tango_menubtn.h"
#include "tango_padmap.h"

typedef enum {
    TANGO_EV_NONE = 0, TANGO_EV_UP, TANGO_EV_DOWN, TANGO_EV_LEFT, TANGO_EV_RIGHT,
    TANGO_EV_PGUP, TANGO_EV_PGDN, TANGO_EV_SELECT, TANGO_EV_BACK, TANGO_EV_MENU,
    TANGO_EV_ALT_SELECT,         /* X: load the core through an MCU restart (a diagnosis aid) */
    TANGO_EV_MINUS, TANGO_EV_PLUS, /* MiSTer's minus / plus (menu.cpp 1426-1433): keys - and =, keypad - and +, and a
                                   * pad's L / R shoulder buttons in the menu (input.cpp 2513-2519) */
    TANGO_EV_HOME, TANGO_EV_END,   /* keys Home / End (menu.cpp 5558-5575) */
    TANGO_EV_TAB,                  /* key Tab, a pad's X (input.cpp 2504-2506): rescan from the top; on ".." that is "up" */
    TANGO_EV_BACKSPACE,            /* a pad's A and B together (input.cpp 2430-2470); in a file list: clear the filter */
    TANGO_EV_SPACE,                /* the keyboard's Space: Select everywhere, "skip this button" while Define buttons asks for one (input.cpp 2991) */
    TANGO_EV_RECENT                /* the grave key, a pad's Select (input.cpp 2517-2519): Recent Cores / Files (no page yet) */
} tango_ev_t;

/* result of tango_platform_t.load_core */
#define TANGO_LOAD_OK         0    /* the FPGA now runs the new core */
#define TANGO_LOAD_NOT_FOUND (-1)  /* nothing was sent to the FPGA: the launcher is still running */
#define TANGO_LOAD_FAILED    (-2)  /* the upload broke off: the FPGA holds no usable core */
#define TANGO_LOAD_NOT_DONE  (-3)  /* uploaded, but the FPGA did not report DONE: no usable core */

typedef struct {
    const tango_fs_t *fs;
    int  (*load_core)(void *ctx, const char *sd_path);      /* e.g. "/sd/_Console/NES_<yyyymmdd>.fs" */
    void  *ctx;
    int  (*load_core_alt)(void *ctx, const char *sd_path);  /* optional: the X-button load */
    /* optional, for System Settings: */
    int  (*free_mb)(void *ctx);                             /* free space on the card in MB, < 0 unknown; asked once
                                                             * per launcher start and cached (f_getfree can scan) */
    void (*reboot)(void *ctx, int cold);                    /* Reboot: 0 = reload the menu core, 1 = cold reboot */
    /* optional, for Misc. Options' Core Volume and Global Volume rows (tango_volume.h; NULL vol = no rows): after a change
     * the callback sends the bytes (SYS 0x26, 0x39) and the firmware owns the one-second save timer */
    tango_vol_t *vol;
    void (*vol_apply)(void *ctx, int core);                 /* core = 1: the Core Volume changed */
    /* optional, for Misc. Options' Information block (tango_about.h tango_info_lines): SYS 0x70's reply byte (0 = the core has no
     * such block), asked once when the page opens; the BL616's IPv4 address as text (returns its length, 0 = none: no IP line) */
    uint8_t (*vsync)(void *ctx);
    int     (*ip)(void *ctx, char *buf, int cap);
    /* optional, for System Settings > Define joystick buttons (the system map's Menu entry, tango_menubtn.h): the raw word of a USB port as the last
     * HID REPORT said it (1 = a pad is connected there; polled while the page is open, the launcher reads no REPORT otherwise), and writing
     * the pad's system map config/inputs/input_<vid>_<pid>_v3.map (0 = ok) */
    int     (*pad_raw)(void *ctx, int port, uint16_t *vid, uint16_t *pid, uint32_t *raw);
    tango_pmcap_t *(*pmcap)(void *ctx);                      /* the walk's state (tango_pmcap_start_sys); NULL = no Define joystick buttons */
    int     (*save_sysmap)(void *ctx, uint16_t vid, uint16_t pid, const uint32_t map[32]);   /* the whole map: [0..11] the buttons, [21] [22] the Menu entry */
    /* optional: the learner behind the walk (tango_padmap.h tango_pmlearn_t, tango_padlearn.h).  learn_begin allocates its state and registers the connected pads
     * (their parsed descriptors, ids, bcdHID); NULL = no learner (an FPGA without the decode block, no memory): the walk reads the raw word as before.
     * pad_sample is one REPORT2 payload of a port (0 = none yet); learn_end(save = 1) finishes the session on `port` (the table to the FPGA, the .dec file,
     * the SDL line) and frees the state, save = 0 drops it. */
    tango_pmlearn_t *(*learn_begin)(void *ctx);
    int     (*pad_sample)(void *ctx, int port, pl_sample_t *s);
    void    (*learn_end)(void *ctx, int save, int port);
} tango_platform_t;

/* the launcher's pages: the Cores browser, MiSTer's System Settings (MENU_SYSTEM1/2) and Misc. Options (MENU_MISC1/2) */
enum { TL_CORES = 0, TL_SYSTEM, TL_MISC, TL_ABOUT, TL_DEFINE /* Define joystick buttons: MiSTer's walk (tango_pmcap_start_sys), then the Menu entry */ };
#define TL_ROWS TANGO_VIS            /* System Settings and Misc. Options are one screen, as MiSTer's 16-line OSD */
#define TL_FREE_UNKNOWN (-2)         /* free_mb: not asked yet (-1 = asked, the card would not say) */

typedef struct {
    tango_osd_t            osd;
    tango_browser_t        br;
    tango_tables_t         tables;
    const tango_platform_t *plat;
    uint8_t                pal[16][3];
    char                   status_key[32], status_text[TANGO_PATH_MAX];   /* legend replaced by a message */
    char                   load_path[TANGO_PATH_MAX + 8];
    char                   diag[4][96];               /* diagnostic lines under the list (tango_launcher_diag) */
    int                    ndiag;
    int                    last_load;                 /* last load_core result */
    /* System Settings / Misc. Options (tango_launcher.c): the rows are rebuilt on every page(); only the text that is
     * not a literal lives here.  RAM cost of the two pages: sizeof is checked in tango/test. */
    uint8_t                mode;                      /* TL_* */
    uint8_t                sel[5];                    /* the cursor row of each page (TL_CORES unused: br.sel) */
    uint8_t                dlearn;                    /* the Define walk has a learner (platform learn_begin) */
    uint8_t                reboot_held;               /* Select on Reboot is down: the release / the clock decide */
    uint32_t               now_ms, reboot_t0;
    int                    free_mb;
    const char            *fw_id;                     /* the BL616 firmware's build id (flash string) or NULL */
    char                   menu_ver[32];              /* the menu core's CONF_STR V text ("" = not read) */
    char                   sline[3][64];              /* version, menu core and free-space rows; Misc.'s Information lines; About's two text lines */
    uint8_t                vs;                        /* SYS 0x70's reply when Misc. Options opened (plat->vsync) */
    uint32_t               about_t0;                  /* About: when it opened (the ticker's clock, tango_about.h) */
    int                    about_step;                /* the ticker step the rows were built for */
    char                   vtxt[2][16];               /* the Core Volume and Global Volume bars */
    tango_mblearn_t        ml;                        /* Define joystick buttons, phase 1: the Menu entry's learn */
    uint8_t                dph, dflash;               /* the phase (0 buttons, 1 Menu); the gamepad's flash phase last drawn */
    uint32_t               dt0;                       /* when the walk began (8 s with no press: given up) */
    uint16_t               dvid, dpid;                /* the pad that pressed first */
    uint8_t                dhave, dnote;              /* dvid / dpid known; the d-pad was refused */
    char                   dtext[56];                 /* what is held so far */
    tango_row_t            srow[TL_ROWS];
    uint8_t                sid[TL_ROWS];              /* what Select does on a row (SI_* in tango_launcher.c) */
    int                    nsrow;
} tango_launcher_t;

/* MiSTer joystick bits (hid.c / Companion): bit0 right, 1 left, 2 down, 3 up, 4 A, 5 B, 6 X, 7 Y, 8 L, 9 R, 10 Select, 11 Start */
#define TANGO_JOY_RIGHT 0x01
#define TANGO_JOY_LEFT  0x02
#define TANGO_JOY_DOWN  0x04
#define TANGO_JOY_UP    0x08
#define TANGO_JOY_A     0x10
#define TANGO_JOY_B     0x20
#define TANGO_JOY_X     0x40
#define TANGO_JOY_Y     0x80
#define TANGO_JOY_L     0x100
#define TANGO_JOY_R     0x200
#define TANGO_JOY_SELECT 0x400
#define TANGO_JOY_START  0x800

/* A pad as the OSD sees it (MiSTer input.cpp joy_digital 2368-2540 and input_cb 3620-3690, Main_MiSTer c3dc8ed):
 *   d-pad -> arrows;  A -> Enter (on its press);  B -> Back (on its RELEASE, as KEY_BACK acts on its upstroke);
 *   A and B together -> Backspace (B first then A, A first then B, or both in one report) and neither of the two then acts alone;
 *   X -> Tab;  Y -> nothing;  L -> minus;  R -> plus;  Select -> the grave key (recents) on its release, unless Start was pressed while
 *   it was down (the menu combo Select+Start: Tango's OSD button, no key);  Start alone -> nothing.
 * One state per pad.  tango_padkeys_step takes the next 16-bit state and says what happened. */
typedef struct {
    uint16_t prev;
    uint8_t  chord;                 /* A and B were down together: nothing acts on their releases */
    uint8_t  b_down;                /* B went down alone and waits for its release */
    uint8_t  sel_down, sel_start;   /* Select is down; Start went down meanwhile */
} tango_padkeys_t;
typedef struct {
    tango_ev_t ev[8];               /* key events, in order */
    int        n;
    int        combo;               /* Select+Start just became down: the menu key's PRESS (the caller picks the event) */
    int        combo_up;            /* Select+Start stopped being both down: the menu key's RELEASE (a core's OSD opens here) */
    int        dir_released;        /* a held direction went up (stops the key repeat) */
    int        a_released;          /* A went up (the Reboot row waits for it) */
} tango_padkeys_out_t;
void tango_padkeys_sync(tango_padkeys_t *k, uint16_t state);                                    /* no events: only the state */
void tango_padkeys_step(tango_padkeys_t *k, uint16_t state, tango_padkeys_out_t *o);

/* Key repeat (osd.h REPEATDELAY 500, REPEATRATE 50): MiSTer repeats only in file lists, on typed characters and on the Reboot row
 * (menu.cpp 612-621).  Tango: in a file list every arrow, page and plus / minus key; elsewhere only Up and Down. */
#define TANGO_REPEAT_DELAY_MS 500
#define TANGO_REPEAT_RATE_MS  50
int  tango_ev_repeats(tango_ev_t ev, int in_file_list);
int  tango_launcher_repeats(const tango_launcher_t *L, tango_ev_t ev);

/* Probes the core (CAPS, then READ_CFG). Returns 1 if it is a Tango launcher core. */
int  tango_launcher_detect(tango_launcher_t *L, const tango_link_t *link);

/* Core start: palette, geometry, font, then the Cores page at the card root. Shows the overlay. */
void tango_launcher_start(tango_launcher_t *L, const tango_platform_t *plat);

/* Same, starting in folder path (relative to the card root) with entry sel selected (sel < 0: MiSTer's 0). */
void tango_launcher_start_at(tango_launcher_t *L, const tango_platform_t *plat, const char *path, int sel);

/* One input event. Returns 1 if the page changed. May call plat->load_core (blocking). */
int  tango_launcher_event(tango_launcher_t *L, tango_ev_t ev);

/* Diagnostic lines shown under the list (dim, not selectable) while they fit; n = 0 removes them. */
void tango_launcher_diag(tango_launcher_t *L, const char *const *lines, int n);

/* Shows a message in the legend row (e.g. why the last load failed) until the next key. */
void tango_launcher_message(tango_launcher_t *L, const char *key, const char *text);

/* Re-lays out and sends the current page (only the rows that changed). */
void tango_launcher_redraw(tango_launcher_t *L);
/* a message in the legend row until the next key (the pad manager's "This joystick is not defined": MiSTer's Info() at the first press of a pad with no map, input.cpp 3754) */
void tango_launcher_notice(tango_launcher_t *L, const char *key, const char *text);

/* What System Settings shows about this build: the BL616 firmware's id ("g<hash>[-dirty] <date> <time>", NULL = not
 * known) and the launcher core's CONF_STR (its V,<version> entry is kept; NULL = not read).  Kept across
 * tango_launcher_start calls. */
void tango_launcher_set_info(tango_launcher_t *L, const char *fw_id, const char *conf);

/* The clock, for the held Reboot (MiSTer's hold_cnt / 3 > 5, tango_reboot_cold).  Returns 1 if it acted. */
int  tango_launcher_tick(tango_launcher_t *L, uint32_t now_ms);

/* ms until tango_launcher_tick has something to do (the next step of a scrolling long name), -1 = nothing */
int  tango_launcher_wait_ms(const tango_launcher_t *L, uint32_t now_ms);

/* 1 while Define joystick buttons reads the pad's raw buttons: the caller drops the pad's menu keys (the keyboard's Esc / Back still cancels) */
int  tango_launcher_capturing(const tango_launcher_t *L);

/* The Select button (A, Enter) went up: a Reboot row that was selected now reloads the menu core.  Returns 1 if so. */
int  tango_launcher_select_released(tango_launcher_t *L);

/* 1 while a Reboot press waits for its release or the 1.3 s of a cold reboot (the caller then polls the clock) */
int  tango_launcher_reboot_pending(const tango_launcher_t *L);

/* a typed character (keyboard): MiSTer's jump / filter in the Cores list (menu.cpp 5642-5668) */
int  tango_launcher_type(tango_launcher_t *L, char ch, uint32_t now_ms);

/* the page as currently shown (for tests) */
void tango_launcher_page(tango_launcher_t *L, tango_page_t *p);

#endif
