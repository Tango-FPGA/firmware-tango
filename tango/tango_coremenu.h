// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 the Tango authors
/*
 * tango_coremenu.h -- the in-core OSD: MiSTer's menu for a running core, built from its CONF_STR.
 *
 * Tango. Our own code, written from MiSTer's behaviour (Main_MiSTer master):
 *   menu.cpp 1905-2240   MENU_GENERIC_MAIN1/2: page 0 = the core's CONF_STR entries from entry 2 on;
 *                        H/h/D/d hide/grey by status_menumask bit (UIO_GET_OSDMASK); P#,Title = a sub-page
 *                        link on page 0; P# prefix = the entry lives on page #; O/o option "Name" + value;
 *                        T/t, R/r trigger; F load "<Text> *.EXT" / "Load *.EXT" (GetExt, 337-351);
 *                        S mount; C cheats; "-" blank / text; blank rows up to the last line (2219), then
 *                        "exit" (page 0) or "back" (sub-page) on it; no entries on page 0 = the System page
 *   menu.cpp 2296-2630   MENU_GENERIC_MAIN3: Menu = close; Back, Left on a sub-page, or Enter on the last row
 *                        = page 0 / close; Enter on an option = next value (value 0 if the next is empty);
 *                        T/R = bit to 1, then 0 (R also closes); P = open the page; F = file browser in
 *                        games/<core> with the F's extensions, ioctl_index = menusub + 1 or the F digit;
 *                        Right on page 0 = System
 *   menu.cpp 2640-2690   the loaded file's index = user_io_ext_idx << 6 | ioctl_index; the OSD hides
 *   menu.cpp 2863-3180   MENU_COMMON1/2 "System": Core (Cores browser), Define <core> buttons, Button/Key
 *                        remap, Reset player assignment, Video processing, Reset settings, Save settings,
 *                        Help, About, Reboot (hold = cold reboot; release = load menu.rbf), exit on the last
 *                        line, MiSTer's blank rows included; Left = page 0
 *   menu.cpp 5909-5970   MENU_RESET1/2: "Reset settings?" yes / no (cursor on no); yes = status reset + save,
 *                        OSD closes; no / Menu = System
 *   menu.cpp 585-615, 3108-3120, 3172; osd.h 11-12   Reboot: release = load menu.rbf; held: hold_cnt starts at 1,
 *                        +1 every 50 ms after 500 ms, cold reboot at hold_cnt / 3 > 5 (about 1.3 s)
 *   menu.cpp 384-421     Up/Down move over the selectable rows and wrap
 *   user_io.cpp 148-158, 526-640, 1578-1595, 1752   status word (16 bytes), <NAME>[_v<N>].CFG in config/
 *                        (file_io.cpp 847-869: FileLoadConfig/FileSaveConfig, raw bytes), loaded at core start
 *                        and sent with status[0] (reset) set, released at the end of the init; UIO_SET_STATUS2
 *                        sends the whole word
 * Look: the Tango pages (menu-tango tools/osd-model/launcher_pages.py INCORE_PAGES, TEMPLATE_PAGES; rows exactly as MiSTer): options
 * show their value right-aligned (no ':'), a "-,Text" line is centred.
 *   menu.cpp 2998-3019, 4188-4431  Define <core> buttons: tango_padmap.[ch]
 *   menu.cpp 1905-1908, 2263-2293, 8654   MENU_SAVE_CHECK / WAIT: opening the OSD within 500 ms of a save write shows
 *                        "Saving..." (row 8) until the 500 ms are over, then page 0; Menu closes
 *   menu.cpp 4030-4165   MENU_MISC1/2 (Left on page 0, cursor on exit): Information, Core Volume, Global Volume, exit; Left/minus
 *                        quieter, Right/plus louder, Select mutes the global volume; Menu or exit close the OSD; Right on
 *                        exit goes back to page 0.  Tango's B goes back to page 0 as well.  No Config row (alternate inis).
 *   menu.cpp 1954-1965, 2366   the literal "DIP" CONF_STR entry (confstr.h CS_DIP): a page-0-only row,
 *                        hidden unless arcade_sw()->dip_num > 0
 *   menu.cpp 3510-3613   MENU_ARCADE_DIP1/2: every <dip> as one row (vertically centred if they all fit
 *                        one screen), a closing "Reset to apply" row; Enter on a dip cycles its value and
 *                        sends it right away (arcade_sw_send); Menu/Left/Back save and return to page 0;
 *                        Enter on "Reset to apply" closes outright with a status[0] reset pulse
 *   menu.cpp 5964-5970   "Reset settings" also resets the arcade DIPs to their <switches default="">
 * Differences, all Tango's: Button/Key remap, Reset player
 * assignment, Video processing, Help, Cheats and S (mount) entries are not built: selecting them shows
 * "not available yet". The file browser starts at the card root when games/<core> is missing.
 * About is Tango's own page (tango_about.h, built by tango_coremenu.c build_about): the wordmark, TangoFPGA.com,
 * "Tango FPGA v<yymmdd> (g<git short hash>[-dirty])" (the BL616 firmware that is running, from
 * tango_core_platform_t.fw_id), "<core name> <core version>" (CONF_STR V,) and a credits ticker, so the firmware on a board
 * can be told from the screen.
 */
#ifndef TANGO_COREMENU_H
#define TANGO_COREMENU_H

#include "tango_proto.h"
#include "tango_browser.h"
#include "tango_launcher.h"
#include "tango_padmap.h"
#include "tango_vproc.h"
#include "tango_volume.h"
#include "tango_zip.h"

#define TANGO_CONF_MAX 4096

typedef struct {
    const tango_fs_t *fs;                                              /* the card, directory listing */
    const tango_zipfs_t *zipfs;                                        /* the card, byte-addressable (zip
                                                                         * archives, tango_zip.h); NULL = a
                                                                         * platform with no zip support: .zip
                                                                         * entries are simply never offered */
    int  (*load_core)(void *ctx, const char *sd_path);                 /* System > Core: through a restart */
    void (*reboot)(void *ctx, int cold);                               /* System > Reboot: the menu core */
    int  (*load_file)(void *ctx, const char *sd_path, uint8_t index);  /* F: start an ioctl download, 0 = ok */
    /* F, the selected file is inside a zip (MiSTer's zip support, matched feature for
     * feature, console ROM zips only -- see tango_zip.h): zip_path is the archive's own path, in zipfs's
     * convention (relative to the card root, no "/sd/" -- not load_file's sd_path convention); member is the
     * matched entry's name exactly as tango_zip_name returned it. */
    int  (*load_zip_file)(void *ctx, const char *zip_path, const char *member, uint8_t index);
    int  (*save_cfg)(void *ctx, const char *name, const uint8_t st[16]); /* config/<name>, 0 = ok */
    int  (*dir_exists)(void *ctx, const char *path);                   /* relative to the card root */
    /* Define buttons: write config/inputs/<name> (the 128-byte map), 0 = ok; then maps_changed re-applies the
     * core maps of the connected pads (file, else the guess), after a save and after a cancel alike */
    int  (*save_map)(void *ctx, const char *name, const uint32_t map[TANGO_PM_BTNS]);
    void (*maps_changed)(void *ctx);
    /* The Define buttons editor: the saved map of a pad, 1 = a file was read into map[32] (config/inputs/<name>, else
     * config/<name>: MiSTer's old place); and Reset to default, which deletes both. Both optional (NULL = no file ever / nothing to delete). */
    int  (*load_map)(void *ctx, const char *name, uint32_t map[TANGO_PM_BTNS]);
    int  (*delete_map)(void *ctx, const char *name);
    /* 1 while a download/upload THIS module doesn't know about (the .mra ROM
     * auto-download, a zip-sourced load, an NVRAM save) is open on the same wire -- dip_select()'s own
     * tango_dl_send (index 254) must not run then: a new SELECT mid-stream would corrupt whichever session
     * is already open (tango_coremenu.c's own comment on tango_dl_send/tango_ul_recv names this exact
     * hazard). NULL means "never busy", for a platform that has no such session to report. */
    int  (*dl_busy)(void *ctx);
    void  *ctx;
    /* System > About shows it as "Tango FPGA v<yymmdd> (g<hash>[-dirty])": the BL616 firmware's own build id,
     * "g<git short hash>[-dirty] <date> <time>" (build.sh writes it into
     * tango_fwid.h, tango_fw.c passes it here).  NULL or "" = not known (a build made without build.sh, a host test):
     * the page says "Tango FPGA (build unknown)" (tango_about_version).  The launcher's System Settings shows the whole id. */
    const char *fw_id;
    /* Misc. Options (Left on page 0): Core Volume and Global Volume (tango_volume.h).  NULL vol = the page has no volume
     * rows.  After a change vol_apply sends the bytes (SYS 0x26, and 0x39 when the core has the audio filter); the one-second
     * save timer is the firmware's. */
    tango_vol_t *vol;
    void (*vol_apply)(void *ctx, int core);                            /* core = 1: the Core Volume changed */
    /* Misc. Options' Information block (tango_about.h tango_info_lines): SYS 0x70's reply byte (0 = no such block), asked once when
     * the page opens; the BL616's IPv4 address as text (returns its length, 0 = none: no IP line).  Both optional. */
    uint8_t (*vsync)(void *ctx);
    int     (*ip)(void *ctx, char *buf, int cap);
    /* The core file this boot started for the launcher (MiSTer's core_path, user_io.cpp 1178-1197): its path relative to the card root,
     * e.g. "_Console/NES_<yyyymmdd>.fs"; 1 = known.  System > Core and Alt+Menu open the Cores list in its folder with it selected (menu.cpp
     * 450-459); NULL or 0 = the card root. */
    int     (*running_core)(void *ctx, char *path, int cap);
} tango_core_platform_t;

enum { TANGO_CM_CLOSED = 0, TANGO_CM_PAGE, TANGO_CM_SYSTEM, TANGO_CM_RESET, TANGO_CM_FILES, TANGO_CM_CORES, TANGO_CM_DEFINE,
       TANGO_CM_SAVING, TANGO_CM_ABOUT, TANGO_CM_VIDEO, TANGO_CM_MISC,
       TANGO_CM_DIP };      /* the DIP page, see tango_coremenu_t's own dip_* fields */

/* results of tango_core_event */
#define TANGO_CM_NOTHING   0
#define TANGO_CM_REDRAWN   1
#define TANGO_CM_CLOSED_NOW 2
#define TANGO_CM_REBOOT    3        /* Reboot selected: the caller decides short (release) or cold (hold) */
/* the DIP page was left -- "Reset to apply" (close + a status[0] reset pulse,
 * menu.cpp 3584-3592) or Menu/Back/Left back to page 0 (menu.cpp 3576-3582, which ALSO saves) -- with
 * dip_cur possibly changed since the last save. This module has no FatFs access (tango_mra.h/tango_zip.h
 * are the only file I/O it touches, and both are read-only); the OSD's own state transition is already
 * done by the time this is returned (closed-and-reset, or back on page 0, matching TANGO_CM_REBOOT's own
 * existing precedent of "the caller still has one real thing left to do") -- the caller must still call
 * tango_core_dip_cur() and persist it (tango_fw.c's own arc_save_dip_cheat). */
#define TANGO_CM_DIP_SAVE  4

#define TANGO_CM_ROWS 48

typedef struct {
    const tango_link_t          *link;
    tango_osd_t                 *osd;
    tango_browser_t             *br;
    const tango_core_platform_t *plat;
    char     conf[TANGO_CONF_MAX];
    char     name[32];                  /* CONF_STR entry 0 */
    char     core_ver[32];              /* CONF_STR V, version string (e.g. "v<yymmdd>") */
    char     cfgname[48];               /* <NAME>[_v<N>].CFG */
    char     banner[40];
    uint8_t  status[16];
    uint16_t hdmask;
    int      mode, page, sel, parent_sel;
    char     title[64];
    int      nrows;
    tango_row_t rows[TANGO_CM_ROWS];
    int16_t  ent[TANGO_CM_ROWS];        /* CONF_STR entry of a page row; system item id; -1 exit/back row */
    char     lab[TANGO_CM_ROWS][64], val[TANGO_CM_ROWS][48];
    char     status_key[32], status_text[96];
    uint8_t  f_index;                   /* the F entry being browsed: ioctl_index */
    char     f_exts[64];
    /* browsing inside a .zip (console zips only; tango_zip.h): in_zip is 1 while the
     * Files list shows the open archive's matching entries instead of the real folder. zip_relpath is that
     * archive's own path (zipfs's convention); real_folder is the real folder to return to ("..", or Back). */
    tango_zip_t zip;
    int      in_zip;
    char     zip_relpath[TANGO_PATH_MAX];
    char     real_folder[TANGO_PATH_MAX];
    tango_tables_t tables;
    tango_pmcap_t pm;                   /* Define <core> buttons (MENU_JOYDIGMAP) */
    uint32_t now_ms;
    int16_t  f_ent;                     /* the CONF_STR entry of the F being browsed (its S flag: a save) */
    uint32_t save_until;                /* menu_process_save: "Saving..." until then (0 = none) */
    /* MiSTer's video / audio processing (tango_vproc.c): set by the firmware when the core has the blocks; NULL = none.
     * The Video processing page (menu.cpp 3185-3400) and the System page's Audio filter rows (2925-2941, 3052-3066) */
    tango_vproc_t *vp;
    int      vp_pick;                   /* what the file browser picks for: 0 a core file, VPICK_* below */
    uint8_t  misc_vs;                   /* Misc. Options: SYS 0x70's reply when the page opened */
    uint32_t about_t0;                  /* About: when the page opened (the ticker's clock, tango_about.h) */
    int      about_step;                /* the ticker step the rows were built for */
    uint8_t  cores_direct;              /* the Cores list was opened by Alt+Menu (MENU_CORES1): its cancel closes the OSD, not System (menu.cpp 2266) */
    /* the DIP page (TANGO_CM_DIP), reached from page 0's own literal "DIP"
     * CONF_STR entry (CS_DIP, confstr.h). Set
     * once by the platform (tango_fw.c's arc_setup, via tango_core_dip_setup() below) right after a real
     * .mra's <switches> have been summarised (tango_mra_switches_summary); dip_num 0 -- the default, from
     * this struct's own memset in tango_core_init -- means "no <switches>" and hides the page's own row
     * entirely (menu.cpp 1954-1965/2366's own "!arcade_sw()->dip_num" hide rule), so every console core,
     * and any arcade core with no <switches>, never sees this page at all: zero behaviour change for them.
     * zipfs/mra_path (copied, not just pointed at -- the platform's own path buffer may be reused for a
     * LATER core's load) let build_dip() re-parse each row fresh on every rebuild, via
     * tango_mra_switches_dip_at -- one tango_mra_dip_t (~1.3 KB) on ITS OWN stack at a time, never the
     * whole ~83.5 KB tango_mra_switches_t (the RAM ceiling). */
    const tango_zipfs_t *dip_zipfs;
    char     dip_mra_path[TANGO_PATH_MAX];
    int      dip_num;
    uint64_t dip_cur;
    /* the <switches default=""> word -- kept so "Reset settings" can put dip_cur back to it (menu.cpp
     * 5964-5970, see tango_core_event's Reset-settings confirm handler) */
    uint64_t dip_def;
} tango_coremenu_t;
enum { VPICK_NONE = 0, VPICK_FLT = 1 /* + VP_HORZ..VP_ILACE */, VPICK_GAMMA = 10, VPICK_MASK, VPICK_AFILT, VPICK_PRESET };

/* Core start: keeps the CONF_STR, the name and the .CFG name; applies cfg16 (the .CFG, or NULL = all 0) with
 * status[0] set, then clear (the soft reset MiSTer holds while it starts a core); reads the menumask. */
void tango_core_init(tango_coremenu_t *cm, const tango_link_t *link, tango_osd_t *osd, tango_browser_t *br,
                     const tango_core_platform_t *plat, const char *conf, const uint8_t *cfg16);

/* the core's config file name, <NAME>[_v<N>].CFG (user_io_create_config_name(1)) */
void tango_core_cfgname(const char *conf, char *out, int cap);

/* F12 / Start+Select: opens page 0 (ENABLE 1) or closes the OSD (ENABLE 0). Returns 1 if now open. */
int  tango_core_toggle(tango_coremenu_t *cm);
/* the same with the page a closed menu opens on (user_io.cpp 4473, menu.cpp 1621-1653): page 0, the Cores list (Alt+Menu), or System (Win+Menu) */
enum { TANGO_OPEN_PAGE0 = 0, TANGO_OPEN_CORES, TANGO_OPEN_SYSTEM };
int  tango_core_toggle_how(tango_coremenu_t *cm, int how);
int  tango_core_event(tango_coremenu_t *cm, tango_ev_t ev);
/* does a held key repeat here (tango_ev_repeats: every list key in a file list, Up / Down elsewhere) */
int  tango_core_repeats(const tango_coremenu_t *cm, tango_ev_t ev);
void tango_core_redraw(tango_coremenu_t *cm);
/* the core's menumask changed? then the open page is built again (menu.cpp 2640). Returns 1 if redrawn. */
int  tango_core_poll_mask(tango_coremenu_t *cm);
/* Define buttons: a pad's HID REPORT (both ports, after every HID interrupt), keyboard keys (PMC_KEY_*), the clock
 * ("Canceling" after the combo is held 1 s). Each returns TANGO_CM_REDRAWN if the page changed. */
int  tango_core_raw(tango_coremenu_t *cm, int port, uint16_t vid, uint16_t pid, uint32_t raw, uint32_t now_ms);
int  tango_core_define_key(tango_coremenu_t *cm, int key);
int  tango_core_tick(tango_coremenu_t *cm, uint32_t now_ms);
/* ms until tango_core_tick has something to do in a file list (the next step of a scrolling long name), -1 = nothing */
int  tango_core_wait_ms(const tango_coremenu_t *cm, uint32_t now_ms);
/* 1 while the Define buttons prompts (or the editor's capture and Test) take the pad's raw buttons: menu navigation and the OSD combo are off */
int  tango_core_capturing(const tango_coremenu_t *cm);
/* the menu button's combo reached the firmware (Q_TOGGLE) while a Define dialog or the editor is open; from_kbd = F12 */
int  tango_core_menu_key(tango_coremenu_t *cm, int from_kbd);
/* a pad went away (the REPORT says it is not connected) */
void tango_core_pad_gone(tango_coremenu_t *cm, int port);
/* the raw bits of the menu button's combo (a single button: both the same); Define buttons and the editor treat them as the menu key */
void tango_core_set_menu_bits(tango_coremenu_t *cm, int port, int bit_a, int bit_b);
/* the pad on `port` has a system map or a built-in decode (tango_pmcap_known) */
void tango_core_set_known(tango_coremenu_t *cm, int port, int known);
/* a sector was written to the core's save (menu_process_save, menu.cpp 8654: 500 ms) */
void tango_core_saved_sector(tango_coremenu_t *cm, uint32_t now_ms);
/* a typed character (keyboard) in a file or core list: MiSTer's jump / filter. Returns TANGO_CM_REDRAWN or 0. */
int  tango_core_type(tango_coremenu_t *cm, char ch, uint32_t now_ms);
/* the page as it would be drawn (for tests) */
void tango_core_page(tango_coremenu_t *cm, tango_page_t *p);
/* the status word to the core (SYS SET_STATUS, the whole word as UIO_SET_STATUS2) */
void tango_core_send_status(tango_coremenu_t *cm);

/* wires the DIP page to a real .mra's <switches> -- see tango_coremenu_t's own
 * dip_* fields for the full reasoning. Call once, right after a successful tango_mra_switches_summary
 * (tango_fw.c's arc_setup); dip_num <= 0, or zipfs/mra_path NULL/empty, disables the page (its own page-0
 * row never appears, and TANGO_CM_DIP can never be entered). Safe to call again for a later core's load --
 * tango_core_init's own memset already clears the previous core's dip_num to 0 first, so skipping this
 * call entirely (every console core) is just as safe as calling it with dip_num 0 explicitly. */
void tango_core_dip_setup(tango_coremenu_t *cm, const tango_zipfs_t *zipfs, const char *mra_path,
                          int dip_num, uint64_t cur, uint64_t def);
/* the live dip_cur word, after any toggles the user has made -- read this back on TANGO_CM_DIP_SAVE and
 * persist it (tango_fw.c's own arc_save_dip_cheat, after copying it into arc_dip_cur). */
uint64_t tango_core_dip_cur(const tango_coremenu_t *cm);

/* Reboot held for held_ms: 1 = cold reboot (MiSTer's hold_cnt / 3 > 5, see above) */
int tango_reboot_cold(unsigned held_ms);

/* ---- file download to the core (menu-tango docs/TANGO-SPEC.md "Cores": SDC IMAGE SELECT / STATUS / WRITE -> ioctl) ----
 * begin: SELECT index size (the core raises ioctl_download, index as MiSTer's ioctl_index);
 * pump:  on SDC IRQ bit 3 (a level while open with >= 256 bytes free): STATUS, then at most the space offered
 *        (<= 512) from the file in one WRITE; after the last byte SELECT index 0 ends the download.
 * Returns 1 while bytes remain, 0 when the download has ended, -1 if the read failed (the download is ended). */
typedef struct {
    const tango_link_t *link;
    uint8_t  index;
    uint32_t size, sent;
    int      open;
    /* called after the last byte, before SELECT 0 ends the download: MiSTer mounts the save there
     * (user_io.cpp 3017-3025: before user_io_set_download(0)) */
    void   (*before_end)(void *ctx);
    void    *end_ctx;
} tango_dl_t;
int tango_dl_begin(tango_dl_t *d, const tango_link_t *link, uint8_t index, uint32_t size);
int tango_dl_pump(tango_dl_t *d, int (*rd)(void *ctx, uint8_t *buf, unsigned n), void *ctx);
void tango_dl_end(tango_dl_t *d);

/* ---- upload from the core (menu-tango docs/TANGO-SPEC.md "File download"; NVRAM save) ----
 * Mirrors tango_dl_t's own shape exactly, reversed: begin: UL_SELECT index size (the core's ioctl_upload
 * direction, tango_core_ports.vh TANGO_IOCTL_UPLOAD) opens and tango_io.v starts pulling the whole blob from
 * the core into its buffer; pump: UL_STATUS polled until ready (the prefetch has finished), then READ up to
 * 512 bytes at a time (any size the caller wants to persist can be chunked this way; the buffer itself holds
 * the whole session already); after the last byte UL_SELECT index 0 ends the upload.
 * Returns 1 while bytes remain (or the prefetch isn't ready yet), 0 when done, -1 if wr() failed. */
typedef struct {
    const tango_link_t *link;
    uint8_t  index;
    uint32_t size, got;
    int      open;
} tango_ul_t;
int tango_ul_begin(tango_ul_t *u, const tango_link_t *link, uint8_t index, uint32_t size);
int tango_ul_pump(tango_ul_t *u, int (*wr)(void *ctx, const uint8_t *buf, unsigned n), void *ctx);
void tango_ul_end(tango_ul_t *u);

/* ---- whole-blob send/receive, synchronous (loop the pump above to completion
 * internally) -- for a value already whole in RAM and small (DIP/cheat words, a real NVRAM image): MiSTer's
 * own arcade_sw_send/arcade_nvm_load/arcade_nvm_save (mra_loader.cpp) are themselves single blocking
 * transfers, never chunked across multiple polls, so this mirrors that shape rather than tango_dl_t/
 * tango_ul_t's own per-interrupt pump shape (built for a multi-KB ROM/Files download instead).
 * Caller's responsibility (not checked here): never call either while a DIFFERENT download/upload is
 * already open (tango_fw.c's dl_file_open/dl_is_zip/dl_is_mra) -- see tango_coremenu.c's own comment at
 * these two functions for why (corruption one way, a real self-deadlock risk the other). */
/* 0 = ok, -1 = the core closed the session early (tango_dl_pump's own "got <= 0" / core-closed rule) */
int tango_dl_send(const tango_link_t *link, uint8_t index, const uint8_t *buf, uint32_t n);
/* size: the expected upload session size (e.g. the .mra's own <nvram size="">); cap: the caller's buffer
 * capacity (size > cap is refused outright, like tango_ul_begin's own TANGO_UL_MAX refusal); *got: bytes
 * actually received (== size on a 0 return). 0 = ok, -1 = refused, or the core never answered / closed early */
int tango_ul_recv(const tango_link_t *link, uint8_t index, uint32_t size, uint8_t *buf, uint32_t cap, uint32_t *got);

#endif
