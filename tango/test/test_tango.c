// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 the Tango authors
/*
 * test_tango.c -- host tests for the Tango launcher firmware modules (layout, protocol, browser, launcher).
 * Tango; the Template_MiSTer CONF_STR test data is marked where it is defined (NOTICE).
 *
 *   ./test_tango <streams-dir> <layout-dir>
 * Writes the SPI streams the FPGA testbench plays (<streams-dir>/s*.bin) and layout dumps
 * (<layout-dir>/layout_*.bin); menu-tango tools/osd-model/tango_stream_check.py then proves both against the model.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdarg.h>
#include "../tango_launcher.h"
#include "../tango_coremenu.h"
#include "../tango_padmap.h"
#include "../tango_save.h"
#include "../tango_kbd.h"
#include "../tango_volume.h"
#include "../tango_about.h"
#include "fake_io.h"

static int fails = 0, checks = 0;
#define BLANKSPACE_T 10
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)
#define CHECKS(a, b) do { checks++; if (strcmp((a), (b))) { fails++; printf("FAIL %s:%d  \"%s\" != \"%s\"\n", __FILE__, __LINE__, (a), (b)); } } while (0)

/* ---------------------------------------------------------------- the card the streams use */
static const char *CARD[] = {   /* listed in this (unsorted) order, like a FAT directory */
    "menu.fs", "Tango.ini", "_Utility/", "config/", "_Console/", "games/", "System Volume Information/",
    ".Trashes/", "_Arcade/", "._menu.fs", "_Computer/", "boot.rom", "notes.txt",
    "_Console/SNES_20260912.fs", "_Console/NES_20260921.fs", "_Console/readme.txt",
    "_Console/Game Boy_20260903.fs", "_Console/Atari 2600_20260802.fs", "_Console/._NES_20260921.fs",
    "_Console/PC Engine_20260630.fs", "_Console/Master System_20260719.fs", "_Console/menu_20260101.fs",
    "_Console/Saves/",
    "_Arcade/cores/", "_Arcade/cores/Arcade-foo_20260101.fs", "_Computer/C64_20260101.bin",
    "config/inputs/", "games/NES/",
};
#define NCARD ((int)(sizeof CARD / sizeof CARD[0]))

static int loads; static char loaded[300];
static int fake_load(void *ctx, const char *path) { (void)ctx; loads++; strncpy(loaded, path, sizeof loaded - 1); return TANGO_LOAD_OK; }

static tango_pmcap_t fake_pm_store; static tango_pmcap_t *fake_pmcap(void *c) { (void)c; return &fake_pm_store; }
static int fake_load_missing(void *ctx, const char *path) { (void)ctx; loads++; strncpy(loaded, path, sizeof loaded - 1); return TANGO_LOAD_NOT_FOUND; }

static tango_launcher_t L;   /* big: static */

/* ---------------------------------------------------------------- unit tests */
static void test_names(void)
{
    char alt[64], date[16];
    CHECK(tango_is_core_file("NES_20260921.fs"));
    CHECK(tango_is_core_file("x.BIN"));
    CHECK(tango_is_core_file("Game Boy_20260903.FS"));
    CHECK(!tango_is_core_file("menu.fs"));
    CHECK(!tango_is_core_file("MENU.BIN"));
    CHECK(!tango_is_core_file("menu_20260101.fs"));
    CHECK(!tango_is_core_file(".hidden.fs"));
    CHECK(!tango_is_core_file("readme.txt"));
    CHECK(!tango_is_core_file("a.fs.txt"));
    CHECK(!tango_is_core_file("boot.rom"));
    CHECK(tango_is_core_file("Galaxian (Namco, Set 1).mra"));     /* an .mra lists like a core */
    CHECK(tango_is_core_file("x.MRA"));
    tango_display_name("NES_20260921.fs", alt, sizeof alt, date);          CHECKS(alt, "NES"); CHECKS(date, "260921");
    tango_display_name("Game Boy_20260903.fs", alt, sizeof alt, date);     CHECKS(alt, "Game Boy"); CHECKS(date, "260903");
    tango_display_name("Foo.bin", alt, sizeof alt, date);                  CHECKS(alt, "Foo"); CHECKS(date, "------");
    tango_display_name("Galaxian (Namco, Set 1).mra", alt, sizeof alt, date); CHECKS(alt, "Galaxian (Namco, Set 1)"); CHECKS(date, "------");
    tango_display_name("Bar_20.fs", alt, sizeof alt, date);                CHECKS(alt, "Bar_20"); CHECKS(date, "------");
    tango_display_name("X_2000_20260101.bin", alt, sizeof alt, date);      CHECKS(alt, "X"); CHECKS(date, "00_20260101");  /* MiSTer: first "_20" */
    /* DirentComp */
    CHECK(tango_dirent_less("..", "", 1, "_Arcade", "", 1));
    CHECK(!tango_dirent_less("_Arcade", "", 1, "..", "", 1));
    CHECK(tango_dirent_less("_Zeta", "", 1, "Alpha", "260101", 0));        /* folders before files */
    CHECK(tango_dirent_less("_Arcade", "", 1, "_Computer", "", 1));
    CHECK(tango_dirent_less("_Computer", "", 1, "_Console", "", 1));
    CHECK(tango_dirent_less("_Console", "", 1, "_Utility", "", 1));
    CHECK(tango_dirent_less("Mega", "260101", 0, "Mega Man", "250101", 0)); /* common prefix: shorter first */
    CHECK(tango_dirent_less("nes", "250101", 0, "NES", "260101", 0));      /* same name: older date first */
    CHECK(!tango_dirent_less("NES", "260101", 0, "nes", "250101", 0));
    CHECK(tango_dirent_less("Mr", "250101", 0, "Mr. Do", "260101", 0));    /* ".xxx" tail ignored: "Mr. Do" ~ "Mr" */
    CHECK(!tango_dirent_less("Mr. Do", "260101", 0, "Mr", "250101", 0));
}

/* a pad as MiSTer's OSD sees it (tango_padkeys_step; input.cpp joy_digital 2368-2540, input_cb 3620-3690) */
static tango_padkeys_out_t po;
static int pad_evs(tango_padkeys_t *k, uint16_t s, int n, ...)      /* step; 1 if the events are exactly the n given */
{
    tango_padkeys_step(k, s, &po);
    va_list ap; va_start(ap, n); int ok = po.n == n;
    for (int i = 0; i < n; i++) { tango_ev_t e = (tango_ev_t)va_arg(ap, int); if (i < po.n && po.ev[i] != e) ok = 0; }
    va_end(ap);
    return ok;
}
static void test_padkeys(void)
{
    tango_padkeys_t k; memset(&k, 0, sizeof k);
    const uint16_t A = TANGO_JOY_A, B = TANGO_JOY_B, X = TANGO_JOY_X, Y = TANGO_JOY_Y, L = TANGO_JOY_L, R = TANGO_JOY_R,
                   SEL = TANGO_JOY_SELECT, STA = TANGO_JOY_START;
    /* directions: one event per press, none while held, the release stops the repeat */
    CHECK(pad_evs(&k, TANGO_JOY_DOWN, 1, TANGO_EV_DOWN) && !po.dir_released);
    CHECK(pad_evs(&k, TANGO_JOY_DOWN, 0) && !po.dir_released);
    CHECK(pad_evs(&k, 0, 0) && po.dir_released);
    CHECK(pad_evs(&k, TANGO_JOY_UP | TANGO_JOY_LEFT, 2, TANGO_EV_UP, TANGO_EV_LEFT));
    CHECK(pad_evs(&k, TANGO_JOY_LEFT, 0) && !po.dir_released);                   /* one of the two still held */
    CHECK(pad_evs(&k, 0, 0) && po.dir_released);
    CHECK(pad_evs(&k, TANGO_JOY_RIGHT, 1, TANGO_EV_RIGHT)); pad_evs(&k, 0, 0);
    /* A = Enter on its press; its release is reported (the Reboot row waits for it) */
    CHECK(pad_evs(&k, TANGO_JOY_UP | A, 2, TANGO_EV_UP, TANGO_EV_SELECT));
    CHECK(pad_evs(&k, TANGO_JOY_UP, 0) && po.a_released && po.dir_released == 0);
    pad_evs(&k, 0, 0);
    /* B = Back on its RELEASE (KEY_BACK acts on its upstroke) */
    CHECK(pad_evs(&k, B, 0));
    CHECK(pad_evs(&k, 0, 1, TANGO_EV_BACK));
    /* A and B in one report: Backspace only, and neither release acts */
    CHECK(pad_evs(&k, A | B, 1, TANGO_EV_BACKSPACE));
    CHECK(pad_evs(&k, A, 0)); CHECK(pad_evs(&k, 0, 0) && po.a_released);
    CHECK(pad_evs(&k, A | B, 1, TANGO_EV_BACKSPACE)); CHECK(pad_evs(&k, B, 0)); CHECK(pad_evs(&k, 0, 0));
    /* A first, then B: Enter, then Backspace (MiSTer's Enter fires at A's press); B's release is silent */
    CHECK(pad_evs(&k, A, 1, TANGO_EV_SELECT)); CHECK(pad_evs(&k, A | B, 1, TANGO_EV_BACKSPACE));
    CHECK(pad_evs(&k, A, 0)); CHECK(pad_evs(&k, 0, 0));
    /* B first, then A: Backspace only -- no Enter, no Back */
    CHECK(pad_evs(&k, B, 0)); CHECK(pad_evs(&k, A | B, 1, TANGO_EV_BACKSPACE));
    CHECK(pad_evs(&k, B, 0)); CHECK(pad_evs(&k, 0, 0));
    CHECK(pad_evs(&k, B, 0)); CHECK(pad_evs(&k, 0, 1, TANGO_EV_BACK));          /* the chord is over: B alone works again */
    /* X = Tab, Y = nothing, L = minus, R = plus */
    CHECK(pad_evs(&k, X, 1, TANGO_EV_TAB)); CHECK(pad_evs(&k, X, 0)); CHECK(pad_evs(&k, 0, 0));
    CHECK(pad_evs(&k, Y, 0)); CHECK(pad_evs(&k, 0, 0));
    CHECK(pad_evs(&k, L, 1, TANGO_EV_MINUS)); CHECK(pad_evs(&k, L | R, 1, TANGO_EV_PLUS)); CHECK(pad_evs(&k, 0, 0));
    /* Select = the grave key on its release; Start alone nothing; Select+Start is the menu combo and no key */
    CHECK(pad_evs(&k, SEL, 0) && !po.combo); CHECK(pad_evs(&k, 0, 1, TANGO_EV_RECENT));
    CHECK(pad_evs(&k, STA, 0) && !po.combo); CHECK(pad_evs(&k, 0, 0));
    CHECK(pad_evs(&k, SEL, 0)); CHECK(pad_evs(&k, SEL | STA, 0) && po.combo); CHECK(pad_evs(&k, SEL | STA, 0) && !po.combo);
    CHECK(pad_evs(&k, STA, 0) && !po.combo); CHECK(pad_evs(&k, 0, 0));                  /* Select down first: no recents after the combo */
    CHECK(pad_evs(&k, STA, 0)); CHECK(pad_evs(&k, SEL | STA, 0) && po.combo); CHECK(pad_evs(&k, SEL, 0)); CHECK(pad_evs(&k, 0, 0));
    CHECK(pad_evs(&k, SEL, 0)); CHECK(pad_evs(&k, 0, 1, TANGO_EV_RECENT));            /* ... and Select alone again works */
    /* a state taken over while buttons are held: their releases do nothing */
    tango_padkeys_sync(&k, A | B); CHECK(pad_evs(&k, 0, 0) && po.a_released);
    tango_padkeys_sync(&k, B); CHECK(pad_evs(&k, 0, 0));
    tango_padkeys_sync(&k, SEL); CHECK(pad_evs(&k, 0, 0));
    tango_padkeys_sync(&k, 0); CHECK(pad_evs(&k, B, 0)); CHECK(pad_evs(&k, 0, 1, TANGO_EV_BACK));
    /* a game's B held while the OSD opens (tango_fw_joystick16: sync while the OSD is hidden, and at the combo's edge): its release in
     * the menu does nothing, so the OSD does not close at once; Select and Start held through the opening make no recents key either */
    CHECK(pad_evs(&k, B, 0)); tango_padkeys_sync(&k, B); CHECK(pad_evs(&k, 0, 0));                     /* hidden: B down, sync, B up */
    CHECK(pad_evs(&k, SEL | STA, 0) && po.combo); tango_padkeys_sync(&k, SEL | STA);                  /* the combo while hidden */
    CHECK(pad_evs(&k, STA, 0)); CHECK(pad_evs(&k, 0, 0));
    CHECK(pad_evs(&k, B, 0)); CHECK(pad_evs(&k, B | SEL, 0)); CHECK(pad_evs(&k, B | SEL | STA, 0) && po.combo);
    tango_padkeys_sync(&k, B | SEL | STA);                                                              /* B held through the combo's opening */
    CHECK(pad_evs(&k, B | SEL, 0)); CHECK(pad_evs(&k, B, 0)); CHECK(pad_evs(&k, 0, 0));               /* no BACK, no RECENT */
    CHECK(pad_evs(&k, B, 0)); CHECK(pad_evs(&k, 0, 1, TANGO_EV_BACK));                                  /* in the menu B works again */
    /* key repeat: MiSTer repeats in file lists (every key) and typed characters; Tango: file lists take every list key, elsewhere Up / Down */
    CHECK(tango_ev_repeats(TANGO_EV_UP, 0) && tango_ev_repeats(TANGO_EV_DOWN, 0) && !tango_ev_repeats(TANGO_EV_LEFT, 0));
    CHECK(!tango_ev_repeats(TANGO_EV_PLUS, 0) && !tango_ev_repeats(TANGO_EV_RIGHT, 0));
    CHECK(tango_ev_repeats(TANGO_EV_LEFT, 1) && tango_ev_repeats(TANGO_EV_RIGHT, 1) && tango_ev_repeats(TANGO_EV_PGUP, 1) &&
          tango_ev_repeats(TANGO_EV_MINUS, 1) && tango_ev_repeats(TANGO_EV_PLUS, 1));
    CHECK(!tango_ev_repeats(TANGO_EV_SELECT, 1) && !tango_ev_repeats(TANGO_EV_TAB, 1) && !tango_ev_repeats(TANGO_EV_BACK, 1) &&
          !tango_ev_repeats(TANGO_EV_MENU, 1) && !tango_ev_repeats(TANGO_EV_BACKSPACE, 1));
    CHECK(TANGO_REPEAT_DELAY_MS == 500 && TANGO_REPEAT_RATE_MS == 50);                 /* osd.h REPEATDELAY, REPEATRATE */
}

static void test_joy_and_menu(void)
{
    test_padkeys();

    rec_link_t r; tango_link_t link; char buf[32];
    rec_link_init(&r, &link);
    r.confstr = "MENU;;";                 CHECK(tango_core_is_menu(&link, buf, sizeof buf) == 1);
    r.confstr = "menu;UART31250,MIDI;";   CHECK(tango_core_is_menu(&link, buf, sizeof buf) == 1);
    r.confstr = "NES;;O[1],Foo,A,B;";     CHECK(tango_core_is_menu(&link, buf, sizeof buf) == 0);
    r.confstr = "MENUX;;";                CHECK(tango_core_is_menu(&link, buf, sizeof buf) == 0);
    r.confstr = NULL;                     CHECK(tango_core_is_menu(&link, buf, sizeof buf) == 1);  /* no CONF_STR */
    r.confstr = "\xff\xff";               CHECK(tango_core_is_menu(&link, buf, sizeof buf) == 1);
    tango_osd_t o;
    r.caps_ok = 0;                        CHECK(tango_osd_probe(&o, &link) == 0);                  /* stock core */
    r.caps_ok = 1;                        CHECK(tango_osd_probe(&o, &link) == 1);
    CHECK(o.caps[2] == 1 && o.caps[3] == 4 && o.caps[4] == 0);

    /* Menu colour presets: pages.palette() */
    static const uint8_t PAL[16][3] = TANGO_PAL_INIT;
    uint8_t out[16][3], amber[3] = { 242, 163, 58 }, red[3] = { 235, 72, 72 }, k[3];
    tango_palette_from_accent(PAL, amber, out);
    CHECK(out[TANGO_C_BAR][0] == 242 && out[TANGO_C_BARTXT][0] == 30 && out[TANGO_C_BARTXT][1] == 20 && out[TANGO_C_BARTXT][2] == 7);
    tango_palette_from_accent(PAL, red, out);
    CHECK(out[TANGO_C_ACC][0] == 235 && out[TANGO_C_ACCTXT][0] == 29 && out[TANGO_C_ACCTXT][1] == 9 && out[TANGO_C_ACCTXT][2] == 9);
    tango_palette_k(PAL, k); CHECK(k[0] == 10 && k[1] == 12 && k[2] == 16);     /* (14,16,22)*3>>2 */
}

static void test_browser(void)
{
    static tango_browser_t b;
    fake_card_t c; tango_fs_t fs;
    fake_card_init(&c, &fs, CARD, NCARD);
    CHECK(tango_browser_scan(&b, &fs, "") == 4);
    const char *want_root[] = { "_Arcade", "_Computer", "_Console", "_Utility" };
    for (int i = 0; i < 4; i++) { tango_browser_select(&b, i); CHECKS(tango_browser_alt(&b, tango_browser_cur(&b)), want_root[i]); }
    tango_browser_select(&b, 0);
    tango_browser_prev(&b); CHECK(b.sel == 3);       /* wraps */
    tango_browser_next(&b); CHECK(b.sel == 0);
    tango_browser_select(&b, 2);
    CHECK(tango_browser_chdir(&b, &fs, "_Console") == 1);
    CHECKS(b.path, "_Console");
    CHECK(b.n == 7 && b.sel == 0);
    const char *want_con[] = { "..", "Atari 2600", "Game Boy", "Master System", "NES", "PC Engine", "SNES" };
    for (int i = 0; i < 7; i++) { tango_browser_select(&b, i); CHECKS(tango_browser_alt(&b, tango_browser_cur(&b)), want_con[i]); }
    tango_browser_select(&b, 4);
    CHECKS(tango_browser_name(&b, tango_browser_cur(&b)), "NES_20260921.fs");
    CHECKS(tango_browser_cur(&b)->date, "260921");
    CHECK(tango_browser_chdir(&b, &fs, "..") == 1);    /* back: the folder we left is selected */
    CHECKS(b.path, ""); CHECK(b.sel == 2);
    CHECK(tango_browser_chdir(&b, &fs, "..") == 0);    /* nothing above the root */
    CHECK(tango_browser_chdir(&b, &fs, "_Arcade") == 1);
    CHECK(b.n == 1);                                   /* ".." only: "cores" has no '_' */
    tango_browser_chdir(&b, &fs, "..");
    CHECK(tango_browser_chdir(&b, &fs, "_Computer") == 1 && b.n == 2);
    tango_browser_select(&b, 1); CHECKS(tango_browser_cur(&b)->date, "260101");

    /* a long folder: paging and MiSTer's window (flist_center_selected) */
    static const char *LONG[32]; static char names[32][40];
    for (int i = 0; i < 30; i++) { snprintf(names[i], sizeof names[i], "_Big/Core %02d_20260101.fs", i); LONG[i] = names[i]; }
    fake_card_init(&c, &fs, LONG, 30);
    CHECK(tango_browser_scan(&b, &fs, "_Big") == 31);  /* ".." + 30 */
    tango_browser_next_page(&b); CHECK(b.sel == 14 && b.first == 1);
    tango_browser_next_page(&b); CHECK(b.sel == 28 && b.first == 15);
    tango_browser_next_page(&b); CHECK(b.sel == 30 && b.first == 17);
    tango_browser_next(&b);      CHECK(b.sel == 0 && b.first == 0);     /* wraps to the top */
    tango_browser_prev(&b);      CHECK(b.sel == 30 && b.first == 17);
    tango_browser_prev_page(&b); CHECK(b.sel == 16 && b.first == 16);
    tango_browser_prev_page(&b); CHECK(b.sel == 2 && b.first == 2);
    tango_browser_prev_page(&b); CHECK(b.sel == 0 && b.first == 0);

    /* empty card: MiSTer's "No files!" */
    static const char *EMPTY[] = { "config/", "notes.txt" };
    fake_card_init(&c, &fs, EMPTY, 2);
    CHECK(tango_browser_scan(&b, &fs, "") == 0);
    tango_page_t p; tango_browser_page(&b, &p, "TANG CONSOLE", NULL, 0);
    CHECK(p.nrows == 1 && p.rows[0].kind == TR_CENTER); CHECKS(p.rows[0].label, "No files!");
    /* a core at the root is listed after the folders */
    static const char *ROOTCORE[] = { "Zed_20260101.fs", "_Console/", "Abc.bin" };
    fake_card_init(&c, &fs, ROOTCORE, 3);
    CHECK(tango_browser_scan(&b, &fs, "") == 3);
    tango_browser_select(&b, 0); CHECKS(tango_browser_alt(&b, tango_browser_cur(&b)), "_Console");
    tango_browser_select(&b, 1); CHECKS(tango_browser_alt(&b, tango_browser_cur(&b)), "Abc");
    tango_browser_select(&b, 2); CHECKS(tango_browser_alt(&b, tango_browser_cur(&b)), "Zed");
}

/* ---------------------------------------------------------------- layout dumps (checked by the model) */
static const char *layout_dir;
static void dump_layout(const char *name, const tango_page_t *p)
{
    static tango_tables_t t;
    CHECK(tango_layout_page(p, &t) == 0);
    CHECK(t.clipped == 0);
    char path[512]; snprintf(path, sizeof path, "%s/layout_%s.bin", layout_dir, name);
    FILE *f = fopen(path, "wb"); if (!f) { printf("cannot write %s\n", path); fails++; return; }
    uint8_t h[2] = { (uint8_t)t.nglyphs, (uint8_t)(t.nglyphs >> 8) }; fwrite(h, 1, 2, f);
    for (int i = 0; i < t.nglyphs; i++) { uint32_t v = t.glyphs[i]; uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) }; fwrite(b, 1, 4, f); }
    for (int i = 0; i < 64; i++) { uint32_t v = t.rows[i]; uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) }; fwrite(b, 1, 4, f); }
    fclose(f);
}
#define N(a) ((int)(sizeof a / sizeof a[0]))
#define PAGE(nm, ban, ti, ar, se, rw, lg) do { tango_page_t p = { ban, ti, NULL, ar, se, -1, rw, N(rw), lg, N(lg), 0 }; dump_layout(nm, &p); } while (0)
static void test_layouts(void)
{
    /* transcribed from menu-tango tools/osd-model/launcher_pages.py (PAGES, INCORE_PAGES, TEMPLATE_PAGES) */
    static const tango_legend_t KB[] = { { "A", "open" }, { "Menu", "settings" }, { "\x83\x7f", "page" } };
    static const tango_row_t r01[] = { { TR_DIR, 0, "Arcade", 0 }, { TR_DIR, 0, "Console", 0 }, { TR_DIR, 0, "Computer", 0 }, { TR_DIR, 0, "Utility", 0 } };
    PAGE("01_cores", "TANG CONSOLE", "Cores", 0, 1, r01, KB);
    static const tango_row_t rroot[] = { { TR_DIR, 0, "Arcade", 0 }, { TR_DIR, 0, "Computer", 0 }, { TR_DIR, 0, "Console", 0 }, { TR_DIR, 0, "Utility", 0 } };
    { tango_page_t p = { "TANG CONSOLE", "Cores", NULL, 0, 2, -1, rroot, N(rroot), KB, N(KB), 0 };
      dump_layout("root_mister_order", &p); }
    static const tango_row_t r02[] = { { TR_UP, 0, "..", 0 }, { TR_FILE, 0, "Atari 2600", "26.08.02" }, { TR_FILE, 0, "Game Boy", "26.09.03" },
        { TR_FILE, 0, "Master System", "26.07.19" }, { TR_FILE, 0, "NES", "26.09.21" }, { TR_FILE, 0, "PC Engine", "26.06.30" }, { TR_FILE, 0, "SNES", "26.09.12" } };
    { tango_page_t p = { "TANG CONSOLE", "Cores", NULL, 0, 4, -1, r02, N(r02), KB, N(KB), 0 };
      dump_layout("02_cores_console", &p); }
    static const tango_legend_t l03[] = { { "A", "load" }, { "Menu", "back" } };
    static const tango_row_t r03[] = { { TR_FILE, 0, "NES", 0 }, { TR_FILE, 0, "Arcade Game Demo", 0 }, { TR_FILE, 0, "Game Boy", 0 } };
    PAGE("03_recent_cores", "TANG CONSOLE", "Recent Cores", 0, 0, r03, l03);
    static const tango_legend_t l04[] = { { "A", "select" }, { "Menu", "cores" }, { "\x83", "misc." } };
    static const tango_row_t r04[] = { { TR_CENTER, 0, "Tango v0.1", 0 }, { TR_BLANK, 0, 0, 0 }, { TR_CENTER, 0, "Storage: SD card", 0 },
        { TR_CENTER, 0, "Available space: 29gb", 0 }, { TR_SEP, 0, 0, 0 }, { TR_SUB, 0, "Remap keyboard", 0 }, { TR_SUB, 0, "Define joystick buttons", 0 },
        { TR_SEP, 0, 0, 0 }, { TR_ACT, 0, "Reboot (hold for cold reboot)", 0 }, { TR_BLANK, 0, 0, 0 }, { TR_BLANK, 0, 0, 0 }, { TR_BLANK, 0, 0, 0 }, { TR_ACT, 0, "exit", 0 } };
    PAGE("04_system_settings", "TANG CONSOLE", "System Settings", TANGO_ARROW_LEFT, 6, r04, l04);
    static const tango_legend_t l06[] = { { "Enter", "finish" } };
    static const tango_row_t r06[] = { { TR_BLANK, 0, 0, 0 }, { TR_CENTER, TANGO_C_TEXT, "Press key to change", 0 }, { TR_CENTER, 0, "on any keyboard", 0 },
        { TR_BLANK, 0, 0, 0 }, { TR_CENTER, 0, "Enter \x7f Finish", 0 }, { TR_ACT, 0, "finish", 0 } };
    PAGE("06_keyboard", "TANG CONSOLE", "Keyboard", 0, 5, r06, l06);
    static const tango_legend_t l10[] = { { "\x83\x7f", "move" }, { "A", "pin" }, { "B", "back" } };
    static const tango_row_t r10[] = { { TR_OPT, 0, "Player 1", "DualShock 4  (pinned)" }, { TR_OPT, 0, "Player 2", "8BitDo SN30 Pro" },
        { TR_DIS, 0, "Player 3", "none" }, { TR_DIS, 0, "Player 4", "none" }, { TR_SEP, 0, 0, 0 }, { TR_INFO, 0, "Pins follow the pad model (VID:PID).", 0 } };
    PAGE("10_players", "TANG CONSOLE", "Players", TANGO_ARROW_LEFT, 0, r10, l10);
    static const tango_legend_t l11[] = { { "A", "select" }, { "\x83", "core" }, { "Menu", "close" } };
    static const tango_row_t r11[] = { { TR_SUB, 0, "Core", 0 }, { TR_BLANK, 0, 0, 0 }, { TR_SUB, 0, "Define NES buttons", 0 }, { TR_SUB, 0, "Button/Key remap", 0 },
        { TR_ACT, 0, "Reset player assignment", 0 }, { TR_BLANK, 0, 0, 0 }, { TR_SUB, 0, "Video processing", 0 }, { TR_BLANK, 0, 0, 0 },
        { TR_ACT, 0, "Reset settings", 0 }, { TR_ACT, 0, "Save settings", 0 }, { TR_BLANK, 0, 0, 0 }, { TR_SUB, 0, "Help", 0 }, { TR_ACT, 0, "About", 0 },
        { TR_BLANK, 0, 0, 0 }, { TR_ACT, 0, "Reboot (hold \x7f cold reboot)", 0 }, { TR_ACT, 0, "\x83  exit", 0 } };
    PAGE("11_incore_system", "NINTENDO NES", "System", TANGO_ARROW_NONE, 2, r11, l11);
    static const tango_legend_t l13[] = { { "F12", "advanced" }, { "Esc", "clear" }, { "Enter", "finish" } };
    static const tango_row_t r13[] = { { TR_BLANK, 0, 0, 0 }, { TR_BLANK, 0, 0, 0 }, { TR_CENTER, TANGO_C_TEXT, "Supported mapping:", 0 }, { TR_BLANK, 0, 0, 0 },
        { TR_CENTER, 0, "Button(s)/Key(s) -> Key(s)", 0 }, { TR_CENTER, 0, "Button(s) -> Button(s)", 0 }, { TR_BLANK, 0, 0, 0 },
        { TR_CENTER, 0, "F12 \x7f Advanced", 0 }, { TR_CENTER, 0, "Esc \x7f Clear", 0 }, { TR_CENTER, 0, "Enter \x7f Finish", 0 }, { TR_ACT, 0, "Cancel", 0 } };
    PAGE("13_incore_button_key_remap", "NINTENDO NES", "Button/Key remap", 0, 10, r13, l13);
    static const tango_legend_t l14[] = { { "A", "select" } };
    static const tango_row_t r14[] = { { TR_CENTER, TANGO_C_TEXT, "Reset settings?", 0 }, { TR_BLANK, 0, 0, 0 }, { TR_ACT, 0, "yes", 0 }, { TR_ACT, 0, "no", 0 } };
    PAGE("14_incore_reset_settings", "NINTENDO NES", "Reset", 0, 3, r14, l14);
    static const tango_legend_t l15[] = { { "A", "change" }, { "\x7f", "system" }, { "\x83", "misc." }, { "Menu", "close" } };
    /* the Tango Template core's page 0 (TANGO_TEMPLATE_CONF below): 18 rows, the page scrolls (top < 0: the model's own formula) */
    static const tango_row_t r15[] = { { TR_BLANK, 0, 0, 0 }, { TR_OPT, 0, "Aspect ratio", "Original" }, { TR_OPT, 0, "Scale", "Normal" }, { TR_OPT, 0, "Scanlines", "None" },
        { TR_OPT, 0, "TV Mode", "NTSC" }, { TR_OPT, 0, "Pattern", "Gradient" }, { TR_OPT, 0, "Source size", "320x240" }, { TR_OPT, 0, "Audio test", "Off" },
        { TR_OPT, 0, "Stereo Mix", "None" }, { TR_BLANK, 0, 0, 0 }, { TR_SUB, 0, "Test Page 1", 0 }, { TR_BLANK, 0, 0, 0 }, { TR_SUB, 0, "Test Page 2", 0 },
        { TR_BLANK, 0, 0, 0 }, { TR_BLANK, 0, 0, 0 }, { TR_ACT, 0, "Reset", 0 }, { TR_ACT, 0, "Reset and close OSD", 0 }, { TR_ACT, 0, "\x83  exit", "\x7f" } };
    PAGE("15_core_template_page0", "TEMPLATE", "Template", 0, 1, r15, l15);
    static const tango_legend_t l16[] = { { "A", "change" }, { "\x83", "back" } };
    static const tango_row_t r16[] = { { TR_BLANK, 0, 0, 0 }, { TR_CENTER, 0, "-= Options in page 1 =-", 0 }, { TR_BLANK, 0, 0, 0 }, { TR_OPT, 0, "Option 1-1", "Off" },
        { TR_DIS, 0, "Load *.BIN", 0 }, { TR_OPT, 0, "Option 1-2", "Off" }, { TR_BLANK, 0, 0, 0 }, { TR_BLANK, 0, 0, 0 }, { TR_BLANK, 0, 0, 0 }, { TR_BLANK, 0, 0, 0 }, { TR_BLANK, 0, 0, 0 }, { TR_BLANK, 0, 0, 0 }, { TR_BLANK, 0, 0, 0 }, { TR_ACT, 0, "back", 0 } };
    PAGE("16_core_template_page1", "TEMPLATE", "Test Page 1", 0, 3, r16, l16);
    /* a scrolled list with MiSTer's window kept by the BL616: top 7, selection 20, position hint "21/30" */
    static tango_row_t rl[30]; static char nm[30][12];
    for (int i = 0; i < 30; i++) { snprintf(nm[i], sizeof nm[i], "Core %02d", i); rl[i].kind = TR_FILE; rl[i].label = nm[i]; rl[i].value = "26.01.01"; }
    { tango_page_t p = { "TANG CONSOLE", "Cores", NULL, 0, 20, 7, rl, 30, KB, N(KB), 0 }; dump_layout("long_list", &p); }

    /* glyph-budget canary: the glyph-budget widening (TANGO_GLYPH_SLOTS 512->1024,
     * TANGO_ROW_GLYPHS 29->67) is pointless if nothing ever actually asks the row table to address
     * past the OLD per-bank ceiling of 512. A full 14-row (TANGO_VIS) screen of real, near-67-glyph
     * No-Intro NES names -- the same names tested per-row by "long_names" below, here all on one
     * page with no truncation headroom -- is exactly the kind of on-screen content that exposes a
     * firmware/bitstream mismatch as ghosting: firmware that assumes 1024 slots/bank paired with an
     * FPGA bitstream built with 512 slots/bank -- the *logic* on both sides is fine in
     * isolation, but only content this large pairs them together. The CHECK below
     * keeps this honest: if a future change shrinks this page back under 512 glyphs, it fails loudly
     * instead of silently stopping to stress the capacity it exists to stress. */
    {
        static const char *ni[] = {
            "Adventures of Rocky and Bullwinkle and Friends, The (USA).nes",
            "Advanced Dungeons & Dragons - Dragons of Flame (Japan).nes",
            "Bill & Ted's Excellent Video Game Adventure (USA).nes",
            "Captain America and the Avengers (USA) (Rev A) (Beta).nes",
            "Chip 'n Dale - Rescue Rangers 2 (USA) (Virtual Console).nes",
            "Dr. Seuss's Adventures of the Cat in the Hat, The (USA).nes",
            "Indiana Jones and the Temple of Doom (USA) (Rev A) (Tengen).nes",
            "Legend of Zelda, The (USA) (Rev A) (GameCube Edition).nes",
            "Mighty Morphin Power Rangers - The Movie (USA) (Proto).nes",
            "Super Mario Bros. + Duck Hunt + World Class Track Meet (USA).nes",
            "Teenage Mutant Ninja Turtles II - The Arcade Game (USA).nes",
            "Who Framed Roger Rabbit (USA) (Rev A) (Collector Edition).nes",
            NULL,   /* filled below: 59 'W's + ".nes" (menu-tango tools/osd-model/tango_stream_check.py _NI) */
            "Short (USA).nes",
        };
        static char w59[64]; memset(w59, 'W', 59); strcpy(w59 + 59, ".nes"); ni[12] = w59;
        static tango_row_t gr[N(ni)];
        for (int i = 0; i < N(ni); i++) { gr[i].kind = TR_FILE; gr[i].label = ni[i]; gr[i].value = NULL; }
        static const tango_legend_t glg[] = { { "A", "open" }, { "\x83\x7f", "page" }, { "B", "back" } };
        tango_page_t p = { "NINTENDO NES", "Select", NULL, 0, 0, -1, gr, N(gr), glg, N(glg), 0 };
        tango_tables_t t;
        CHECK(tango_layout_page(&p, &t) == 0);
        CHECK(t.clipped == 0);
        CHECK(t.nglyphs > 512);   /* the actual ceiling the 512-slot RTL's row table could address */
        printf("glyph_bank_capacity: %d glyphs (> 512 old-RTL ceiling, <= 1024 TANGO_GLYPH_SLOTS)\n", t.nglyphs);
        dump_layout("glyph_bank_capacity", &p);
    }
}

/* ---------------------------------------------------------------- the streams */
static FILE *open_stream(const char *dir, const char *name)
{
    char path[512]; snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "wb"); if (!f) { printf("cannot write %s\n", path); fails++; }
    return f;
}

static void test_streams(const char *dir)
{
    rec_link_t r; tango_link_t link; fake_card_t c; tango_fs_t fs;
    tango_platform_t plat = { .fs = &fs, .load_core = fake_load };

    /* s0: a fresh FPGA, the golden page straight away (Console folder, NES selected) */
    rec_link_init(&r, &link); fake_card_init(&c, &fs, CARD, NCARD);
    r.out = open_stream(dir, "s0_golden_cores_console.bin");
    CHECK(tango_launcher_detect(&L, &link) == 1);
    tango_launcher_start_at(&L, &plat, "_Console", 4);
    fclose(r.out);
    printf("s0 golden: %ld transfers, %ld bytes, %d glyph entries\n", r.transfers, r.bytes, L.tables.nglyphs);

    /* s1: power-up -> the root page */
    rec_link_init(&r, &link); fake_card_init(&c, &fs, CARD, NCARD);
    r.out = open_stream(dir, "s1_powerup_root.bin");
    CHECK(tango_launcher_detect(&L, &link) == 1);
    tango_launcher_start(&L, &plat);
    CHECK(L.br.n == 4 && L.br.sel == 0);
    fclose(r.out);
    printf("s1 power-up root: %ld transfers, %ld bytes\n", r.transfers, r.bytes);

    /* s2: Down, Down (Console), A (open), Down x4 (NES) = the golden page */
    long t0 = r.transfers, b0 = r.bytes;
    r.out = open_stream(dir, "s2_open_console_nes.bin");
    CHECK(tango_launcher_event(&L, TANGO_EV_DOWN));
    CHECK(tango_launcher_event(&L, TANGO_EV_DOWN));
    CHECK(tango_launcher_event(&L, TANGO_EV_SELECT));
    CHECKS(L.br.path, "_Console");
    for (int i = 0; i < 4; i++) CHECK(tango_launcher_event(&L, TANGO_EV_DOWN));
    CHECK(L.br.sel == 4);
    fclose(r.out);
    printf("s2 open Console, NES: %ld transfers, %ld bytes\n", r.transfers - t0, r.bytes - b0);

    /* s3: one key down (NES -> PC Engine): only rows that differ from the hidden bank */
    t0 = r.transfers; b0 = r.bytes;
    r.out = open_stream(dir, "s3_key_down.bin");
    CHECK(tango_launcher_event(&L, TANGO_EV_DOWN));
    fclose(r.out);
    printf("s3 key down: %ld transfers, %ld bytes (%u glyph entries, %u rows re-sent)\n",
           r.transfers - t0, r.bytes - b0, L.osd.last_glyph_entries, L.osd.last_row_entries);
    /* the hidden bank still holds the page from two presses ago (Master System selected): rows 5 and 7
     * differ from it (row 6, NES, is sent too because one equal row between two runs costs less than a header) */
    CHECK(L.osd.last_row_entries == 3 && L.osd.last_glyph_entries == 38);   /* rows 5-7 (there is no detail line) */

    /* s4: back up to NES and press A: "Loading NES..." then the loader */
    t0 = r.transfers; b0 = r.bytes; loads = 0;
    r.out = open_stream(dir, "s4_load_nes.bin");
    CHECK(tango_launcher_event(&L, TANGO_EV_UP));
    CHECK(tango_launcher_event(&L, TANGO_EV_SELECT));
    fclose(r.out);
    CHECK(loads == 1); CHECKS(loaded, "/sd/_Console/NES_20260921.fs");
    printf("s4 load NES: %ld transfers, %ld bytes, load_core(\"%s\")\n", r.transfers - t0, r.bytes - b0, loaded);

    /* the launcher's own behaviour: B goes up and re-selects the folder */
    r.out = NULL;
    CHECK(tango_launcher_event(&L, TANGO_EV_BACK));
    CHECK(L.br.path[0] == 0 && L.br.sel == 2);
    CHECK(tango_launcher_event(&L, TANGO_EV_BACK) == 1 && L.mode == TL_SYSTEM && L.sel[TL_SYSTEM] == 13);   /* at the root: MiSTer's B = Menu */
    CHECK(tango_launcher_event(&L, TANGO_EV_BACK) == 1 && L.mode == TL_CORES && L.br.path[0] == 0 && L.br.sel == 0);   /* back from Settings: a fresh scan, the first entry */

    /* a core that isn't on the card any more: the launcher stays and says so; the next key clears it */
    tango_page_t p;
    plat.load_core = fake_load_missing;
    CHECK(tango_launcher_event(&L, TANGO_EV_SELECT));                 /* into _Console (selected after B) */
    CHECK(tango_launcher_event(&L, TANGO_EV_BACK));
    tango_browser_chdir(&L.br, &fs, "_Computer"); tango_browser_select(&L.br, 1);
    CHECK(tango_launcher_event(&L, TANGO_EV_SELECT));
    CHECKS(loaded, "/sd/_Computer/C64_20260101.bin");
    tango_launcher_page(&L, &p);
    CHECK(p.nlegend == 1); CHECKS(p.legend[0].key, "Could not load"); CHECKS(p.legend[0].label, "C64");
    CHECK(tango_launcher_event(&L, TANGO_EV_UP));
    tango_launcher_page(&L, &p);
    CHECK(p.nlegend == 3);                                            /* A open, Menu settings, page: "` recent" hidden */
    CHECKS(p.legend[1].label, "settings");
    /* no card: "No SD card" */
    static const char *NONE[] = { "x" };
    fake_card_t c2; tango_fs_t fs2; fake_card_init(&c2, &fs2, NONE, 0);
    c2.paths = NONE; c2.npaths = 0;
    tango_browser_scan(&L.br, &fs2, "_Gone");
    tango_launcher_page(&L, &p);
    CHECK(p.nrows == 1); CHECKS(p.rows[0].label, "No SD card");

    /* diagnostic rows: a blank row + dim rows after the list, while they fit; the longest
     * lines tango_fw.c can print fit the row (no glyph clipped) */
    tango_browser_scan(&L.br, &fs, "");
    static const char *const D[4] = {
        "Power-up: id 0001081B  4587520 B  crc32 FFFFFFFF  99999 ms  gap 999999 us",
        "    not DONE  E 00026230  W 00026230",
        "Launch (restart): id 0001081B  5505024 B  crc32 FFFFFFFF  99999 ms  gap 999999 us",
        "    not DONE  R 00026020 E 00000220 W 00000220 4K 00000260 64K 00000260" };
    tango_launcher_diag(&L, D, 4);
    tango_launcher_page(&L, &p);
    CHECK(p.nrows == L.br.n + 5 && p.rows[L.br.n].kind == TR_BLANK && p.rows[L.br.n + 4].kind == TR_INFO);
    static tango_tables_t tt; tango_layout_page(&p, &tt); CHECK(tt.clipped == 0);
    /* a test card's root (9 entries) + 4 of the longest lines + the longest note: nothing clipped */
    static const char *BENCH[] = { "_Arcade/", "_Arcade/Arcade Game Demo_20260928.bin", "BG.BIN", "BG1.BIN", "BG565.BIN", "BG565_1.BIN",
                                   "FONT.BIN", "menu_dbg.bin", "menu_link.bin", "menu_normal.bin", "menu.bin" };
    { fake_card_t cb; tango_fs_t fb; fake_card_init(&cb, &fb, BENCH, 11); tango_browser_scan(&L.br, &fb, ""); }
    CHECK(L.br.n == 9);
    tango_launcher_message(&L, "Could not load", "Arcade Game Demo: not DONE  R 00026020 E 00020230 W 00020230");
    tango_launcher_page(&L, &p); tango_layout_page(&p, &tt);
    CHECK(tt.clipped == 0); printf("bench root + diagnostics: %d glyph entries of %d\n", tt.nglyphs, TANGO_GLYPH_SLOTS);
    for (int i = 0; i < 4; i++) { int w = tango_text_width(D[i]); CHECK(w <= TANGO_VX - TANGO_TX); if (w > TANGO_VX - TANGO_TX) printf("diag line %d: %d px\n", i, w); }
    /* a longer list: only the rows that fit, no blank row */
    {
        static const char *BIG[13]; static char nm[13][24];
        for (int i = 0; i < 12; i++) { snprintf(nm[i], sizeof nm[i], "CORE%02d.BIN", i); BIG[i] = nm[i]; }
        fake_card_t cb; tango_fs_t fb; fake_card_init(&cb, &fb, BIG, 12); tango_browser_scan(&L.br, &fb, "");
        tango_launcher_page(&L, &p);
        CHECK(L.br.n == 12 && p.nrows == 14 && p.rows[12].kind == TR_INFO && p.rows[13].kind == TR_INFO);
        CHECKS(p.rows[12].label, D[0]);
    }
    /* the legend lines the firmware cycles through fit next to their key */
    {
        const char *K[2] = { "Restart", "Could not load" };
        const char *T[2] = { "5505024 B  crc32 FFFFFFFF  gap 999999 us  4K 00000220  64K 00000220",
                             "Arcade Game Demo: not DONE  R 00026020 E 00000220 W 00000220" };
        for (int i = 0; i < 2; i++) CHECK(tango_text_width(K[i]) + 6 + tango_text_width(T[i]) <= TANGO_VX - TANGO_TX);
    }
    tango_launcher_diag(&L, D, 0);
    tango_launcher_page(&L, &p); CHECK(p.nrows == L.br.n);
}

/* ---------------------------------------------------------------- the launcher's System Settings and Misc. Options */
static int fs_free_calls, fs_free_val; static int fs_reboots[8], fs_nreboots;
static int vol_applies, vol_apply_core;
static void fake_vol_apply(void *ctx, int core) { (void)ctx; vol_applies++; vol_apply_core = core; }
static int fake_free_mb(void *ctx) { (void)ctx; fs_free_calls++; return fs_free_val; }
static void fake_reboot_cb(void *ctx, int cold) { (void)ctx; if (fs_nreboots < 8) fs_reboots[fs_nreboots++] = cold; }
static const char *FWID = "g0f1e2d3-dirty 2026-10-12 21:30";
static const char *MENU_CONF = "MENU;;O[127:126],Aspect ratio,Original,Full Screen;V,v260928";

static int row_k(const tango_page_t *p, int r, int kind, const char *label)
{
    return r < p->nrows && p->rows[r].kind == kind && (label ? (p->rows[r].label && !strcmp(p->rows[r].label, label)) : 1);
}

static void test_settings(const char *dir)
{
    rec_link_t r; tango_link_t link; fake_card_t c; tango_fs_t fs; tango_page_t p;
    tango_platform_t plat = { .fs = &fs, .load_core = fake_load, .free_mb = fake_free_mb, .reboot = fake_reboot_cb, .pmcap = fake_pmcap };
    static tango_vol_t vol;                                                       /* core attenuation 2, global 3: launcher_pages 05b */
    tango_vol_init(&vol, 0); tango_vol_load(&vol, 3, 2); tango_vol_caps(&vol, 0, 0);
    tango_platform_t platv = plat; platv.vol = &vol; platv.vol_apply = fake_vol_apply;

    /* s5: Menu opens System Settings (cursor on exit), Up (Reboot), Left (Misc., exit), Up (Players), Menu (Cores) */
    rec_link_init(&r, &link); fake_card_init(&c, &fs, CARD, NCARD);
    r.out = open_stream(dir, "s5_system_settings.bin");
    CHECK(tango_launcher_detect(&L, &link) == 1);
    fs_free_calls = 0; fs_free_val = 29 * 1024 + 17; fs_nreboots = 0;
    tango_launcher_start(&L, &platv);
    tango_launcher_set_info(&L, FWID, MENU_CONF);
    CHECKS(L.menu_ver, "v260928"); CHECKS(L.fw_id, FWID);
    CHECK(L.mode == TL_CORES);
    CHECK(tango_launcher_event(&L, TANGO_EV_MENU) == 1 && L.mode == TL_SYSTEM);
    CHECK(fs_free_calls == 1);
    tango_launcher_page(&L, &p);
    CHECKS(p.title, "System Settings"); CHECK(p.arrow == TANGO_ARROW_LEFT); CHECK(p.nrows == TANGO_VIS && p.sel == 13);
    CHECK(row_k(&p, 0, TR_BLANK, 0)); CHECK(row_k(&p, 1, TR_CENTER, "Firmware g0f1e2d3-dirty 2026-10-12 21:30"));
    CHECK(p.rows[1].col == TANGO_C_TEXT);
    CHECK(row_k(&p, 2, TR_CENTER, "Menu core, version tag v260928")); CHECK(row_k(&p, 4, TR_CENTER, "Storage: SD card"));
    CHECK(row_k(&p, 5, TR_CENTER, "Available space: 29gb"));
    CHECK(row_k(&p, 7, TR_DIS, "Remap keyboard")); CHECK(row_k(&p, 8, TR_SUB, "Define joystick buttons"));
    CHECK(row_k(&p, 9, TR_ACT, "About")); CHECK(row_k(&p, 10, TR_BLANK, 0));
    CHECK(row_k(&p, 11, TR_ACT, "Reboot (hold \x7f cold reboot)")); CHECK(row_k(&p, 13, TR_ACT, "exit"));
    CHECK(p.nlegend == 3 && !strcmp(p.legend[1].key, "Menu") && !strcmp(p.legend[1].label, "cores"));
    dump_layout("04b_system_settings_built", &p);
    CHECK(tango_launcher_event(&L, TANGO_EV_UP) == 1 && L.sel[TL_SYSTEM] == 11);
    CHECK(tango_launcher_event(&L, TANGO_EV_LEFT) == 1 && L.mode == TL_MISC && L.sel[TL_MISC] == 13);
    tango_launcher_page(&L, &p);
    CHECKS(p.title, "Misc. Options"); CHECK(p.arrow == TANGO_ARROW_RIGHT); CHECK(p.nrows == TANGO_VIS && p.sel == 13);
    CHECK(row_k(&p, 1, TR_CENTER, "Information")); CHECK(row_k(&p, 2, TR_CENTER, "HDMI 1280x720"));
    CHECK(row_k(&p, 5, TR_DIS, "Config")); CHECK(row_k(&p, 10, TR_DIS, "Boot core")); CHECK(row_k(&p, 11, TR_DIS, "Players"));
    CHECK(row_k(&p, 7, TR_VOL, "Core Volume")); CHECK(row_k(&p, 8, TR_VOL, "Global Volume"));
    CHECK(row_k(&p, 13, TR_ACT, "exit"));
    dump_layout("05b_misc_options_built", &p);
    CHECK(tango_launcher_event(&L, TANGO_EV_UP) == 1 && L.sel[TL_MISC] == 11);
    CHECK(tango_launcher_event(&L, TANGO_EV_MENU) == 1 && L.mode == TL_CORES);
    fclose(r.out); r.out = NULL;
    CHECK(fs_free_calls == 1);                                      /* asked once, cached across the page changes */

    /* s6: the About page through the SPI stream: Menu (System Settings), Up, Up (About), Select, one ticker step, Left (back, cursor on About), Menu */
    rec_link_init(&r, &link); fake_card_init(&c, &fs, CARD, NCARD);
    r.out = open_stream(dir, "s6_about.bin");
    CHECK(tango_launcher_detect(&L, &link) == 1);
    tango_launcher_start(&L, &platv); tango_launcher_set_info(&L, FWID, MENU_CONF);
    tango_launcher_tick(&L, 10000);
    tango_launcher_event(&L, TANGO_EV_MENU); tango_launcher_event(&L, TANGO_EV_UP); tango_launcher_event(&L, TANGO_EV_UP);
    CHECK(L.sel[TL_SYSTEM] == 9);
    CHECK(tango_launcher_event(&L, TANGO_EV_SELECT) == 1 && L.mode == TL_ABOUT);
    CHECK(tango_launcher_tick(&L, 10000 + 60) == 1);
    CHECK(tango_launcher_event(&L, TANGO_EV_LEFT) == 1 && L.mode == TL_SYSTEM && L.sel[TL_SYSTEM] == 9);
    CHECK(tango_launcher_event(&L, TANGO_EV_MENU) == 1 && L.mode == TL_CORES);
    fclose(r.out); r.out = NULL;

    /* Misc. Options' volume rows: Left / minus quieter, Right / plus louder, Select mutes the global one (menu.cpp 4116-4126) */
    tango_launcher_start(&L, &platv); tango_launcher_set_info(&L, FWID, MENU_CONF);
    tango_vol_load(&vol, 0, 0); vol_applies = 0;
    tango_launcher_event(&L, TANGO_EV_MENU); tango_launcher_event(&L, TANGO_EV_LEFT);
    CHECK(L.mode == TL_MISC);
    CHECK(tango_launcher_event(&L, TANGO_EV_UP) && tango_launcher_event(&L, TANGO_EV_UP) && L.sel[TL_MISC] == 10);   /* Boot core */
    CHECK(tango_launcher_event(&L, TANGO_EV_UP) && L.sel[TL_MISC] == 8);                                            /* Global Volume */
    CHECK(tango_launcher_event(&L, TANGO_EV_LEFT) == 1 && vol_applies == 1 && vol_apply_core == 0 && (vol.vol & 7) == 1);
    CHECK(tango_launcher_event(&L, TANGO_EV_MINUS) == 1 && (vol.vol & 7) == 2);
    CHECK(tango_launcher_event(&L, TANGO_EV_RIGHT) == 1 && (vol.vol & 7) == 1 && L.mode == TL_MISC);               /* Right louder, not System */
    CHECK(tango_launcher_event(&L, TANGO_EV_PLUS) == 1 && (vol.vol & 7) == 0);
    tango_launcher_page(&L, &p); CHECK(row_k(&p, 8, TR_VOL, "Global Volume") && strlen(p.rows[8].value) == 8);
    CHECK(tango_launcher_event(&L, TANGO_EV_SELECT) == 1 && (vol.vol & 0x10) && vol_applies == 5);                  /* mute */
    tango_launcher_page(&L, &p); CHECK(row_k(&p, 8, TR_OPT, "Global Volume")); CHECKS(p.rows[8].value, "< Mute >");
    dump_layout("05c_misc_options_muted", &p);
    CHECK(tango_launcher_event(&L, TANGO_EV_LEFT) == 1 && !(vol.vol & 0x10) && (vol.vol & 7) == 0);                 /* a step unmutes only */
    CHECK(tango_launcher_event(&L, TANGO_EV_SELECT) && (vol.vol & 0x10) && tango_launcher_event(&L, TANGO_EV_SELECT) && !(vol.vol & 0x10));
    CHECK(tango_launcher_event(&L, TANGO_EV_UP) && L.sel[TL_MISC] == 7);                                            /* Core Volume */
    vol_applies = 0;
    CHECK(tango_launcher_event(&L, TANGO_EV_LEFT) == 1 && vol_applies == 1 && vol_apply_core == 1 && (vol.core & 7) == 1);
    CHECK(tango_launcher_event(&L, TANGO_EV_SELECT) == 0 && vol_applies == 1);                                      /* Select on Core Volume: nothing */
    for (int i = 0; i < 9; i++) tango_launcher_event(&L, TANGO_EV_LEFT);
    CHECK((vol.core & 7) == 6);                                                                                      /* the bar stops at 6 */
    CHECK(vol.pending);                                                                                              /* the firmware's save timer is armed */
    CHECK(tango_launcher_event(&L, TANGO_EV_MENU) && L.mode == TL_CORES);

    /* back from the settings the Cores list is scanned again from the root, nothing selected but the first entry (menu.cpp 1641-1642: SelectFile("", SCANO_CORES);
     * get_rbf_dir() is "" for the menu core), as in MiSTer */
    tango_launcher_start_at(&L, &plat, "_Console", 4);
    tango_launcher_set_info(&L, FWID, MENU_CONF);
    CHECK(tango_launcher_event(&L, TANGO_EV_MENU) && L.mode == TL_SYSTEM);
    CHECK(tango_launcher_event(&L, TANGO_EV_BACK) && L.mode == TL_CORES);        /* B on System: back to the Cores list */
    CHECKS(L.br.path, ""); CHECK(L.br.sel == 0 && !L.br.filtered);
    tango_launcher_start_at(&L, &plat, "_Console", 4);
    CHECK(tango_launcher_event(&L, TANGO_EV_MENU) && L.mode == TL_SYSTEM);
    CHECK(tango_launcher_event(&L, TANGO_EV_MENU) && L.mode == TL_CORES);        /* Menu on System: the Cores list */
    CHECKS(L.br.path, ""); CHECK(L.br.sel == 0);
    tango_launcher_start_at(&L, &plat, "_Console", 4);
    CHECK(tango_launcher_event(&L, TANGO_EV_MENU) && L.mode == TL_SYSTEM);
    CHECK(tango_launcher_event(&L, TANGO_EV_SELECT) && L.mode == TL_CORES);      /* exit (the cursor starts there) */
    CHECKS(L.br.path, ""); CHECK(L.br.sel == 0);
    tango_launcher_start_at(&L, &plat, "_Console", 4);                           /* Misc. Options too */
    tango_launcher_event(&L, TANGO_EV_MENU); tango_launcher_event(&L, TANGO_EV_LEFT); CHECK(L.mode == TL_MISC);
    CHECK(tango_launcher_event(&L, TANGO_EV_MENU) && L.mode == TL_CORES); CHECKS(L.br.path, "");
    tango_launcher_start_at(&L, &plat, "_Console", 4);
    CHECK(tango_launcher_event(&L, TANGO_EV_BACK) == 1 && L.br.path[0] == 0 && L.mode == TL_CORES);   /* B in a folder: up one folder */
    CHECK(tango_launcher_event(&L, TANGO_EV_BACK) == 1 && L.mode == TL_SYSTEM);                       /* at the root: System Settings */
    CHECK(tango_launcher_event(&L, TANGO_EV_BACK) == 1 && L.mode == TL_CORES);

    /* up / down skip the blank and centred rows and wrap: exit -> Reboot -> Define -> Remap -> exit */
    tango_launcher_event(&L, TANGO_EV_MENU);
    static const int WALK[] = { 11, 9, 8, 7, 13, 11 };
    for (int i = 0; i < 6; i++) { CHECK(tango_launcher_event(&L, TANGO_EV_UP) && L.sel[TL_SYSTEM] == WALK[i]); }
    for (int i = 4; i >= 0; i--) { CHECK(tango_launcher_event(&L, TANGO_EV_DOWN) && L.sel[TL_SYSTEM] == WALK[i]); }
    CHECK(L.sel[TL_SYSTEM] == 11);

    /* placeholders say so; the next key clears the message */
    CHECK(tango_launcher_event(&L, TANGO_EV_UP) && L.sel[TL_SYSTEM] == 9);                                        /* About (not a placeholder) */
    CHECK(tango_launcher_event(&L, TANGO_EV_UP) && L.sel[TL_SYSTEM] == 8);                                        /* Define */
    CHECK(tango_launcher_event(&L, TANGO_EV_SELECT) == 1 && L.mode == TL_DEFINE && tango_launcher_capturing(&L));   /* Define joystick buttons: the Menu entry */
    tango_launcher_page(&L, &p); CHECKS(p.title, "Define buttons");
    CHECK(tango_launcher_event(&L, TANGO_EV_BACK) == 1 && L.mode == TL_SYSTEM && L.sel[TL_SYSTEM] == 8);          /* the keyboard's Esc / Back cancels */
    tango_launcher_page(&L, &p);
    CHECK(p.nlegend == 1 && !strcmp(p.legend[0].key, "Define buttons") && !strcmp(p.legend[0].label, "cancelled: nothing saved"));
    CHECK(tango_launcher_event(&L, TANGO_EV_UP) == 1 && L.sel[TL_SYSTEM] == 7);
    tango_launcher_page(&L, &p); CHECK(p.nlegend == 3);
    CHECK(tango_launcher_event(&L, TANGO_EV_SELECT) == 1);                       /* Remap keyboard */
    tango_launcher_page(&L, &p); CHECKS(p.legend[0].label, "Remap keyboard");
    CHECK(tango_launcher_event(&L, TANGO_EV_RIGHT) == 1 && L.mode == TL_SYSTEM);   /* clears the message only */
    CHECK(tango_launcher_event(&L, TANGO_EV_RIGHT) == 0);                          /* nothing on System */

    /* Misc.: Left / Right / Select on a placeholder say so; Right on exit goes to System (the Define row); exit leaves */
    CHECK(tango_launcher_event(&L, TANGO_EV_LEFT) && L.mode == TL_MISC && L.sel[TL_MISC] == 13);
    CHECK(tango_launcher_event(&L, TANGO_EV_UP) && L.sel[TL_MISC] == 11);                   /* Players */
    CHECK(tango_launcher_event(&L, TANGO_EV_RIGHT) == 1 && L.mode == TL_MISC);
    tango_launcher_page(&L, &p); CHECKS(p.legend[0].key, "Not available yet"); CHECKS(p.legend[0].label, "Players");
    CHECK(tango_launcher_event(&L, TANGO_EV_UP) && L.sel[TL_MISC] == 10);                   /* Boot core */
    CHECK(tango_launcher_event(&L, TANGO_EV_UP) && L.sel[TL_MISC] == 5);                    /* Config */
    CHECK(tango_launcher_event(&L, TANGO_EV_SELECT) == 1);
    tango_launcher_page(&L, &p); CHECKS(p.legend[0].label, "Config");
    CHECK(tango_launcher_event(&L, TANGO_EV_UP) && L.sel[TL_MISC] == 13);                   /* wraps to exit */
    CHECK(tango_launcher_event(&L, TANGO_EV_RIGHT) && L.mode == TL_SYSTEM && L.sel[TL_SYSTEM] == 8);
    CHECK(tango_launcher_event(&L, TANGO_EV_LEFT) && L.mode == TL_MISC && L.sel[TL_MISC] == 13);
    CHECK(tango_launcher_event(&L, TANGO_EV_SELECT) && L.mode == TL_CORES);

    /* Reboot: released at once = the menu core, held for tango_reboot_cold = cold; any other key cancels */
    fs_nreboots = 0;
    tango_launcher_event(&L, TANGO_EV_MENU); tango_launcher_event(&L, TANGO_EV_UP);       /* Reboot */
    CHECK(L.sel[TL_SYSTEM] == 11);
    tango_launcher_tick(&L, 1000);
    CHECK(tango_launcher_event(&L, TANGO_EV_SELECT) == 0 && tango_launcher_reboot_pending(&L));
    CHECK(tango_launcher_tick(&L, 1400) == 0 && tango_launcher_tick(&L, 2000) == 0);       /* 1000 ms: not yet */
    CHECK(fs_nreboots == 0);
    CHECK(tango_launcher_select_released(&L) == 1 && fs_nreboots == 1 && fs_reboots[0] == 0);
    CHECK(!tango_launcher_reboot_pending(&L));
    CHECK(tango_launcher_select_released(&L) == 0);                                        /* nothing pending */
    tango_launcher_start(&L, &plat); tango_launcher_set_info(&L, FWID, MENU_CONF);         /* held: cold at tango_reboot_cold's time */
    tango_launcher_event(&L, TANGO_EV_MENU); tango_launcher_event(&L, TANGO_EV_UP);
    fs_nreboots = 0;
    tango_launcher_tick(&L, 5000);
    CHECK(tango_launcher_event(&L, TANGO_EV_SELECT) == 0);
    unsigned cold_at = 0; for (unsigned t = 0; t < 4000 && !cold_at; t += 10) if (tango_reboot_cold(t)) cold_at = t;
    CHECK(cold_at >= 1200 && cold_at <= 1400);
    CHECK(tango_launcher_tick(&L, 5000 + cold_at - 10) == 0 && fs_nreboots == 0);
    CHECK(tango_launcher_tick(&L, 5000 + cold_at) == 1 && fs_nreboots == 1 && fs_reboots[0] == 1);
    CHECK(tango_launcher_select_released(&L) == 0);                                        /* the release after a cold reboot: nothing */
    tango_launcher_start(&L, &plat); tango_launcher_set_info(&L, FWID, MENU_CONF);         /* cancelled by another key */
    tango_launcher_event(&L, TANGO_EV_MENU); tango_launcher_event(&L, TANGO_EV_UP);
    fs_nreboots = 0;
    tango_launcher_tick(&L, 100); tango_launcher_event(&L, TANGO_EV_SELECT);
    tango_launcher_event(&L, TANGO_EV_UP);
    CHECK(!tango_launcher_reboot_pending(&L) && tango_launcher_select_released(&L) == 0 && fs_nreboots == 0);
    /* a platform with no reboot hook: the row says so */
    { tango_platform_t p2 = plat; p2.reboot = NULL; tango_launcher_start(&L, &p2); tango_launcher_event(&L, TANGO_EV_MENU);
      tango_launcher_event(&L, TANGO_EV_UP); CHECK(tango_launcher_event(&L, TANGO_EV_SELECT) == 1 && !tango_launcher_reboot_pending(&L));
      tango_launcher_page(&L, &p); CHECKS(p.legend[0].key, "Not available yet"); }

    /* free space: mb under 10 GB, gb above, "unknown" without a hook or when the card will not say */
    { tango_platform_t p2 = plat;
      static const struct { int mb; const char *txt; } F[] = { { 512, "Available space: 512mb" }, { 10 * 1024 - 1, "Available space: 10239mb" },
                                                                { 10 * 1024, "Available space: 10gb" }, { 119 * 1024 + 900, "Available space: 119gb" },
                                                                { -1, "Available space: unknown" } };
      for (int i = 0; i < N(F); i++) { fs_free_val = F[i].mb; tango_launcher_start(&L, &p2); tango_launcher_event(&L, TANGO_EV_MENU);
        tango_launcher_page(&L, &p); CHECKS(p.rows[5].label, F[i].txt); }
      p2.free_mb = NULL; tango_launcher_start(&L, &p2); tango_launcher_event(&L, TANGO_EV_MENU);
      tango_launcher_page(&L, &p); CHECKS(p.rows[5].label, "Available space: unknown"); }

    /* build info: no firmware id / no menu CONF_STR say so; V is cut to the field */
    tango_launcher_start(&L, &plat);
    tango_launcher_set_info(&L, NULL, NULL);
    tango_launcher_event(&L, TANGO_EV_MENU); tango_launcher_page(&L, &p);
    CHECKS(p.rows[1].label, "Firmware unknown"); CHECKS(p.rows[2].label, "Menu core, version unknown");
    tango_launcher_set_info(&L, "", "MENU;;V,0123456789012345678901234567890123456789");
    CHECK(strlen(L.menu_ver) == 31);
    tango_launcher_page(&L, &p); CHECKS(p.rows[1].label, "Firmware unknown");
    tango_launcher_set_info(&L, FWID, "MENU;;-;V,v270101;");   CHECKS(L.menu_ver, "v270101");
    tango_launcher_set_info(&L, FWID, "MENU;;");               CHECKS(L.menu_ver, "");
    /* the widest rows fit one centred row */
    tango_launcher_set_info(&L, "g0f1e2d3-dirty 2026-10-12 21:30", "MENU;;V,0123456789012345678901234567890");
    tango_launcher_page(&L, &p);
    for (int i = 1; i <= 2; i++) CHECK(tango_text_width(p.rows[i].label) <= TANGO_VX - TANGO_TX);

    /* a message in the legend row (the cycle of a failed launch) draws the current page */
    tango_launcher_message(&L, "Could not load", "x");
    tango_launcher_page(&L, &p); CHECK(p.nlegend == 1 && p.rows == L.srow);
    printf("settings: sizeof(tango_launcher_t) %u bytes (the launcher's pages cost %u)\n", (unsigned)sizeof(tango_launcher_t),
           (unsigned)(sizeof(tango_launcher_t) - offsetof(tango_launcher_t, mode)));
}

/* ---------------------------------------------------------------- the in-core OSD (tango_coremenu) */
/* MiSTer-devel/Template_MiSTer Template.sv 58-86 (the same string as test_confstr.c): test data copied as text,
 * GPL-2.0-or-later as Template.sv's header states (NOTICE).  TANGO_TEMPLATE_CONF below carries the same page 1 / page 2
 * entries, as the Tango Template core's CONF_STR does */
static const char *TEMPLATE_CONF =
    "Template;;-;O[122:121],Aspect ratio,Original,Full Screen,[ARC1],[ARC2];O[2],TV Mode,NTSC,PAL;"
    "O[4:3],Noise,White,Red,Green,Blue;-;P1,Test Page 1;P1-;P1-, -= Options in page 1 =-;P1-;"
    "P1O[5],Option 1-1,Off,On;d0P1F1,BIN;H0P1O[10],Option 1-2,Off,On;-;P2,Test Page 2;P2-;"
    "P2-, -= Options in page 2 =-;P2-;P2S0,DSK;P2O[7:6],Option 2,1,2,3,4;-;-;T[0],Reset;R[0],Reset and close OSD;v,0;";
/* the Tango Template core's CONF_STR as READ_CFG returns it (690 bytes): menu-tango template/rtl/tango_core_template.v 88-118 with its macros
 * expanded by sys-tango build_tools/confstr_expand.py at the pinned sys-tango: `TANGO_VIDEO_CONF = sys-tango tango_video_opts.vh
 * TANGO_VO_E_ASPECT, _SCALE, _SL; "V,vcosim0" is the BUILD_DATE of the menu repository's co-simulation, which checks
 * its READ_CFG against this same expansion.  The pages launcher_pages.py TEMPLATE_PAGES models (15_core_template_page0)
 * and the co-simulation shows are of this string; TEMPLATE_CONF above (MiSTer's own Template.sv) stays for the behaviour checks */
static const char *TANGO_TEMPLATE_CONF =
    "Template;;-;O[127:126],Aspect ratio,Original,Full Screen,4:3,16:9;O[125:124],Scale,Normal,V-Integer,Narrower HV-Integer,Wider HV-Integer;"
    "O[123:122],Scanlines,None,25%,50%,75%;O[2],TV Mode,NTSC,PAL;O[44:41],Pattern,Gradient,Pixel grid;"
    "O[47:45],Source size,320x240,256x240 NES,256x224 SNES,384x224 CPS,160x144 GB;"
    "O[50:48],Audio test,Off,1 kHz square,440 Hz square,Sweep,Noise,440 Hz left;O[52:51],Stereo Mix,None,25%,50%,100%;-;"
    "P1,Test Page 1;P1-;P1-, -= Options in page 1 =-;P1-;P1O[5],Option 1-1,Off,On;d0P1F1,BIN;H0P1O[10],Option 1-2,Off,On;-;"
    "P2,Test Page 2;P2-;P2-, -= Options in page 2 =-;P2-;P2S0,DSK;P2O[7:6],Option 2,1,2,3,4;-;-;T[0],Reset;R[0],Reset and close OSD;"
    "J,A,B;v,1;V,vcosim0";
static const char *CM_CARD[] = {
    "menu.bin", "games/", "games/Template/", "games/Template/zeta.bin", "games/Template/Alpha.BIN",
    "games/Template/notes.txt", "games/Template/More/", "games/Template/More/c.bin", "_Console/", "_Console/NES_20260921.fs",
    "config/",
};
static int cm_loads, cm_files, cm_saves, cm_reboots; static char cm_path[600]; static uint8_t cm_idx, cm_saved[16]; static char cm_savename[64];
static int cm_load_core(void *ctx, const char *p) { (void)ctx; cm_loads++; strncpy(cm_path, p, sizeof cm_path - 1); return 0; }
static void cm_reboot(void *ctx, int cold) { (void)ctx; (void)cold; cm_reboots++; }
static int cm_load_file(void *ctx, const char *p, uint8_t idx) { (void)ctx; cm_files++; strncpy(cm_path, p, sizeof cm_path - 1); cm_idx = idx; return 0; }
static int cm_save(void *ctx, const char *name, const uint8_t st[16]) { (void)ctx; cm_saves++; memcpy(cm_saved, st, 16); strncpy(cm_savename, name, sizeof cm_savename - 1); return 0; }
static int pm_saves, pm_changed; static char pm_name[96]; static uint32_t pm_map[32];
static int cm_save_map(void *ctx, const char *name, const uint32_t map[32]) { (void)ctx; pm_saves++; strncpy(pm_name, name, sizeof pm_name - 1); memcpy(pm_map, map, sizeof pm_map); return 0; }
static void cm_maps_changed(void *ctx) { (void)ctx; pm_changed++; }
static int cm_dir_exists(void *ctx, const char *path)
{
    (void)ctx; size_t l = strlen(path);
    for (int i = 0; i < (int)(sizeof CM_CARD / sizeof CM_CARD[0]); i++) if (!strncmp(CM_CARD[i], path, l) && CM_CARD[i][l] == '/') return 1;
    return 0;
}
static int row_is(const tango_page_t *p, int r, int kind, const char *label, const char *value)
{
    if (r >= p->nrows || p->rows[r].kind != kind) return 0;
    if ((label ? label : "")[0] || (p->rows[r].label && p->rows[r].label[0])) { if (!label || !p->rows[r].label || strcmp(label, p->rows[r].label)) return 0; }
    const char *v = p->rows[r].value ? p->rows[r].value : "";
    return !strcmp(v, value ? value : "");
}
static void cm_dump(const char *name, tango_coremenu_t *cm) { tango_page_t p; tango_core_page(cm, &p); dump_layout(name, &p); }

static struct { const uint8_t *d; unsigned n, pos; int fail_at; } src;
static int src_read(void *ctx, uint8_t *b, unsigned n)
{
    (void)ctx;
    if (src.fail_at >= 0 && src.pos >= (unsigned)src.fail_at) return -1;
    if (n > src.n - src.pos) n = src.n - src.pos;
    memcpy(b, src.d + src.pos, n); src.pos += n; return (int)n;
}
static uint32_t crc32_of(const uint8_t *d, unsigned n)
{
    uint32_t c = 0xFFFFFFFFu;
    for (unsigned i = 0; i < n; i++) { c ^= d[i]; for (int b = 0; b < 8; b++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1))); }
    return c ^ 0xFFFFFFFFu;
}

/* ---- System > About (tango_about.h): wordmark, TangoFPGA.com, the firmware build, the core, the ticker, back ---- */
static void goto_about(tango_coremenu_t *cm)
{
    if (cm->mode == TANGO_CM_CLOSED) tango_core_toggle(cm);
    if (cm->mode == TANGO_CM_PAGE) tango_core_event(cm, TANGO_EV_RIGHT);                      /* page 0 -> System */
    while (cm->sel < cm->nrows - 1 && strcmp(cm->lab[cm->sel], "About") != 0) tango_core_event(cm, TANGO_EV_DOWN);
    tango_core_event(cm, TANGO_EV_SELECT);                                                    /* -> About */
}
/* the glyph entries of layout row `lr` (count, first) */
static void lrow(const tango_tables_t *t, int lr, int *count, int *first)
{
    uint32_t lo = t->rows[2 * lr], hi = t->rows[2 * lr + 1];
    *count = (int)((lo >> 9) & 0x7F); *first = (int)(lo & 0x1FF) | (int)((hi >> 30) & 1) << 9;
}
static void about_rows_check(const tango_page_t *p, const char *ver, const char *core, int pos)
{
    CHECKS(p->title, "About"); CHECK(p->arrow == TANGO_ARROW_NONE); CHECK(p->nrows == TANGO_ABOUT_ROWS && p->sel == -1);
    CHECK(row_is(p, 0, TR_BLANK, 0, 0));
    for (int i = 1; i <= 5; i++) CHECK(p->rows[i].kind == TR_LOGO && strlen(p->rows[i].label) == 55);
    CHECK(p->rows[6].kind == TR_CENTER && p->rows[6].col == TANGO_C_ACC && tango_text_width(p->rows[6].label) == 660);   /* the strip, as wide as the wordmark */
    CHECK(row_is(p, 7, TR_BLANK, 0, 0)); CHECK(row_is(p, 8, TR_CENTER, "TangoFPGA.com", 0)); CHECK(p->rows[8].col == TANGO_C_TEXT);
    CHECK(row_is(p, 9, TR_BLANK, 0, 0));
    CHECK(row_is(p, 10, TR_CENTER, ver, 0)); CHECK(row_is(p, 11, TR_CENTER, core, 0));
    CHECK(row_is(p, 12, TR_BLANK, 0, 0));
    CHECK(p->rows[13].kind == TR_SCROLL && p->scroll == pos && p->rows[13].label == tango_about_ticker);
    CHECK(p->nlegend == 1 && !strcmp(p->legend[0].label, "back"));
}
static void about_checks(tango_coremenu_t *cm, const tango_link_t *link, tango_osd_t *osd, tango_browser_t *br,
                         const tango_core_platform_t *plat)
{
    static const char FW[] = "g0f1e2d3-dirty 2026-10-12 21:30";   /* a fixed id, in the shape mk_fwid.sh prints */
    static const char CONF[] = "NES;;O[1],X,A,B;V,v260930;";
    tango_page_t p; static tango_tables_t t;
    tango_core_platform_t pf = *plat;

    /* the id given: the page of tango_about.h, the ticker at its start (the text begins at the left edge: pos = the row's width) */
    pf.fw_id = FW; tango_core_init(cm, link, osd, br, &pf, CONF, NULL);
    cm->now_ms = 1000;
    goto_about(cm); CHECK(cm->mode == TANGO_CM_ABOUT);
    tango_core_page(cm, &p);
    about_rows_check(&p, "Tango FPGA v261012 (g0f1e2d3-dirty)", "NES v260930", TANGO_VX - TANGO_TX);
    CHECK(tango_about_pos(0) == TANGO_VX - TANGO_TX);
    cm_dump("cm_about", cm);                                  /* tango_stream_check.py draws the same page from the model */
    /* laid out: every glyph placed (none clipped or shadowed); the wordmark is 5 rows of 3 bricks per pixel; the text centred */
    CHECK(tango_layout_page(&p, &t) == 0 && t.clipped == 0 && t.dropped == 0);
    {
        int c, f;
        for (int k = 1; k <= 5; k++) {
            int n = 0; for (const char *q = p.rows[k].label; *q; q++) n += *q == '#';
            lrow(&t, 2 + k, &c, &f);
            CHECK(c == 3 * n);
            for (int g = 0; g < c; g++) { uint32_t e = t.glyphs[f + g]; CHECK(((e >> 22) & 31) == 4 && ((e >> 11) & 0x7FF) == tango_font_glyph[TANGO_ICON_BRICK].colbase); }
        }
    }
    /* a wordmark pixel is 12 px: the bricks of one pixel sit at x, x + 4 and x + 8, the row is centred in [TX, VX] */
    {
        int c, f; lrow(&t, 2 + 1, &c, &f);
        int x0 = (TANGO_TX + TANGO_VX - 12 * 55) / 2, first_cell = (int)(strchr(p.rows[1].label, '#') - p.rows[1].label);
        CHECK((int)(t.glyphs[f] & 0x7FF) == x0 + 12 * first_cell && (int)(t.glyphs[f + 1] & 0x7FF) == x0 + 12 * first_cell + 4 && (int)(t.glyphs[f + 2] & 0x7FF) == x0 + 12 * first_cell + 8);
        CHECK(x0 >= TANGO_TX && x0 + 12 * 55 <= TANGO_VX);
    }
    {   /* the version row is centred and inside the row */
        int c, f; lrow(&t, 2 + 10, &c, &f);
        const char *v = "Tango FPGA v261012 (g0f1e2d3-dirty)"; int w = tango_text_width(v), x0 = (TANGO_TX + TANGO_VX - w) / 2;
        CHECK(c == (int)strlen(v) && (int)(t.glyphs[f] & 0x7FF) == x0 && x0 >= TANGO_TX && x0 + w <= TANGO_VX);
    }
    {   /* the ticker row at the start: whole glyphs inside [TX, VX] only, the first at TX */
        int c, f; lrow(&t, 2 + 13, &c, &f);
        CHECK(c > 60 && c <= 127 && (int)(t.glyphs[f] & 0x7FF) == TANGO_TX);
        int last = (int)(t.glyphs[f + c - 1] & 0x7FF); CHECK(last < TANGO_VX);
    }

    /* the ticker: one step every 60 ms, 3 px, the text leaves to the left and re-enters from the right; every position lays out
     * whole (no glyph clipped or shadowed, never more than the row's 127 entries) */
    CHECK(tango_core_wait_ms(cm, 1000) == TANGO_ABOUT_STEP_MS && tango_core_wait_ms(cm, 1059) == 1 && tango_core_wait_ms(cm, 1060) == TANGO_ABOUT_STEP_MS);
    CHECK(tango_core_tick(cm, 1059) == TANGO_CM_NOTHING);
    CHECK(tango_core_tick(cm, 1060) == TANGO_CM_REDRAWN);
    tango_core_page(cm, &p); CHECK(p.scroll == TANGO_VX - TANGO_TX + 3);
    CHECK(tango_core_tick(cm, 1100) == TANGO_CM_NOTHING);                      /* still the same step */
    CHECK(tango_core_tick(cm, 1000 + 60 * 10) == TANGO_CM_REDRAWN);
    tango_core_page(cm, &p); CHECK(p.scroll == TANGO_VX - TANGO_TX + 30);
    {
        int period = tango_text_width(tango_about_ticker) + TANGO_VX - TANGO_TX, maxc = 0, seen_empty = 0, seen_wrap = 0, prev = -1;
        for (int step = 0; step <= period / 3 + 2; step++) {
            int pos = tango_about_pos(step);
            if (prev >= 0 && pos < prev) seen_wrap++;
            prev = pos;
            tango_row_t rows[TANGO_ABOUT_ROWS]; char txt[2][64];
            tango_about_rows(rows, txt, "NES", "v260930", FW);
            tango_page_t q = { "NES", "About", NULL, TANGO_ARROW_LEFT, -1, -1, rows, TANGO_ABOUT_ROWS, p.legend, p.nlegend, 0, tango_about_pos(step) };
            CHECK(tango_layout_page(&q, &t) == 0 && t.clipped == 0 && t.dropped == 0);
            int c, f; lrow(&t, 2 + 13, &c, &f);
            if (c > maxc) maxc = c;
            if (!c) seen_empty = 1;
            for (int g = 0; g < c; g++) { int x = (int)(t.glyphs[f + g] & 0x7FF); CHECK(x >= TANGO_TX && x < TANGO_VX); }
        }
        CHECK(maxc <= 127 && seen_empty && seen_wrap == 1);
        printf("about: ticker %d px + %d px window, %d steps a cycle, at most %d glyphs in the row\n", tango_text_width(tango_about_ticker), TANGO_VX - TANGO_TX, period / 3, maxc);
    }
    /* dumps for the model: the ticker half way out and entering from the right */
    cm->about_t0 = 0; cm->now_ms = 60 * 100;
    tango_core_tick(cm, 60 * 100); cm_dump("cm_about_step100", cm);
    tango_core_tick(cm, 60 * 1400); cm_dump("cm_about_step1400", cm);
    tango_core_event(cm, TANGO_EV_BACK); CHECK(cm->mode == TANGO_CM_SYSTEM);

    /* no id (NULL: a build made without build.sh, a host test) or an empty or unparsable one: the version row says so */
    pf.fw_id = NULL; tango_core_init(cm, link, osd, br, &pf, CONF, NULL);
    goto_about(cm); tango_core_page(cm, &p);
    about_rows_check(&p, "Tango FPGA (build unknown)", "NES v260930", TANGO_VX - TANGO_TX);
    CHECK(tango_layout_page(&p, &t) == 0 && t.clipped == 0 && t.dropped == 0);
    pf.fw_id = ""; tango_core_init(cm, link, osd, br, &pf, CONF, NULL);
    goto_about(cm); tango_core_page(cm, &p); CHECK(row_is(&p, 10, TR_CENTER, "Tango FPGA (build unknown)", 0));
    { char big[100]; memset(big, 'a', sizeof big - 1); big[sizeof big - 1] = 0;
      pf.fw_id = big; tango_core_init(cm, link, osd, br, &pf, CONF, NULL);
      goto_about(cm); tango_core_page(cm, &p); CHECK(row_is(&p, 10, TR_CENTER, "Tango FPGA (build unknown)", 0)); }
    CHECK(strlen(tango_about_ticker) > 300);
    {   char v[64];
        tango_about_version(v, sizeof v, "gnogit 2026-01-02 03:04"); CHECKS(v, "Tango FPGA v260102 (gnogit)");
        tango_about_version(v, sizeof v, "g0a1b2c3 2026-10-12 21:30"); CHECKS(v, "Tango FPGA v261012 (g0a1b2c3)");
        tango_about_version(v, sizeof v, "g0a1b2c3 2026-10"); CHECKS(v, "Tango FPGA (build unknown)");
        tango_about_version(v, sizeof v, "g0a1b2c3 2026/10/12 21:30"); CHECKS(v, "Tango FPGA (build unknown)");
        tango_about_version(v, sizeof v, NULL); CHECKS(v, "Tango FPGA (build unknown)"); }

    /* a core with no V, entry: the version says so, with or without an id */
    pf.fw_id = FW; tango_core_init(cm, link, osd, br, &pf, "Bare;;O[1],X,A,B;", NULL);
    goto_about(cm); tango_core_page(cm, &p);
    CHECK(row_is(&p, 11, TR_CENTER, "Bare (version unknown)", 0)); CHECK(row_is(&p, 10, TR_CENTER, "Tango FPGA v261012 (g0f1e2d3-dirty)", 0));
    CHECK(tango_layout_page(&p, &t) == 0 && t.clipped == 0 && t.dropped == 0);

    /* the widest version row the build script can make (g + 7 hex digits + -dirty, every digit as wide as the widest) fits the row */
    {
        char wide = '0'; for (const char *c = "0123456789abcdef"; *c; c++) { char a[2] = { *c, 0 }, b[2] = { wide, 0 }; if (tango_text_width(a) > tango_text_width(b)) wide = *c; }
        char worst[] = "Tango FPGA v000000 (g0000000-dirty)"; for (char *c = worst; *c; c++) if (*c == '0') *c = wide;
        CHECK(tango_text_width(worst) <= TANGO_VX - TANGO_TX);
        int all = 1; for (const char *c = tango_about_ticker; *c; c++) if (!tango_font_glyph[(unsigned char)*c].width) all = 0;
        for (const char *c = "Tango FPGA (build unknown)TangoFPGA.com version unknown"; *c; c++) if (!tango_font_glyph[(unsigned char)*c].width) all = 0;
        CHECK(all);                                            /* the ticker and the rows use only glyphs the font has */
        printf("about: the widest version row is %d px of the %d px row\n", tango_text_width(worst), TANGO_VX - TANGO_TX);
    }
}

static void test_coremenu(void)
{
    static tango_coremenu_t cm; static tango_osd_t osd; static tango_browser_t br;
    static rec_link_t r; tango_link_t link; fake_card_t card; tango_fs_t fs; tango_page_t p;
    rec_link_init(&r, &link); r.confstr = TEMPLATE_CONF; r.menumask = 0;
    fake_card_init(&card, &fs, CM_CARD, (int)(sizeof CM_CARD / sizeof CM_CARD[0]));
    tango_core_platform_t plat = { .fs = &fs, .load_core = cm_load_core, .reboot = cm_reboot,
                                   .load_file = cm_load_file, .save_cfg = cm_save, .dir_exists = cm_dir_exists };
    static const uint8_t PAL[16][3] = TANGO_PAL_INIT;

    /* READ_CFG through the proto call, then core start: status 0, [0] pulsed (user_io.cpp 1595, 1752) */
    static char conf[TANGO_CONF_MAX];
    CHECK(tango_sys_read_confstr(&link, conf, sizeof conf) == (int)strlen(TEMPLATE_CONF));
    CHECK(!strcmp(conf, TEMPLATE_CONF));
    CHECK(tango_osd_probe(&osd, &link));
    tango_osd_start(&osd, PAL);
    r.status_writes = 0;
    tango_core_init(&cm, &link, &osd, &br, &plat, conf, NULL);
    CHECK(r.status_writes == 2);
    { int z = 1; for (int i = 0; i < 16; i++) if (r.status[i]) z = 0; CHECK(z); }
    CHECKS(cm.name, "Template"); CHECKS(cm.cfgname, "Template.CFG"); CHECKS(cm.banner, "TEMPLATE");
    CHECK(cm.mode == TANGO_CM_CLOSED);

    /* Start+Select: page 0 = launcher_pages 15_core_template_page0 */
    CHECK(tango_core_toggle(&cm) == 1);
    tango_core_page(&cm, &p);
    CHECKS(p.title, "Template"); CHECK(p.sel == 1); CHECK(p.nrows == 14);
    CHECK(row_is(&p, 0, TR_BLANK, 0, 0)); CHECK(row_is(&p, 1, TR_OPT, "Aspect ratio", "Original"));
    CHECK(row_is(&p, 2, TR_OPT, "TV Mode", "NTSC")); CHECK(row_is(&p, 3, TR_OPT, "Noise", "White"));
    CHECK(row_is(&p, 5, TR_SUB, "Test Page 1", 0)); CHECK(row_is(&p, 7, TR_SUB, "Test Page 2", 0));
    CHECK(row_is(&p, 10, TR_ACT, "Reset", 0)); CHECK(row_is(&p, 11, TR_ACT, "Reset and close OSD", 0));
    CHECK(row_is(&p, 12, TR_BLANK, 0, 0)); CHECK(row_is(&p, 13, TR_ACT, "\x83  exit", "\x7f"));   /* the last line (2219) with both cues (OSD_ARROW_RIGHT | OSD_ARROW_LEFT) */
    /* the legend promises only keys that work: Right = system, Left = misc. (Misc. Options exists), Menu = close */
    CHECK(p.nlegend == 4 && !strcmp(p.legend[0].label, "change") && !strcmp(p.legend[1].label, "system") && !strcmp(p.legend[2].label, "misc.") && !strcmp(p.legend[3].label, "close"));

    /* Down, A on TV Mode: status[2] = 1 sent to the core as the whole word */
    tango_core_event(&cm, TANGO_EV_DOWN); CHECK(cm.sel == 2);
    r.status_writes = 0;
    CHECK(tango_core_event(&cm, TANGO_EV_SELECT) == TANGO_CM_REDRAWN);
    CHECK(r.status_writes == 1 && r.status[0] == 0x04);
    tango_core_page(&cm, &p); CHECK(row_is(&p, 2, TR_OPT, "TV Mode", "PAL"));
    tango_core_event(&cm, TANGO_EV_SELECT); CHECK(r.status[0] == 0x00);              /* (1+1) & 1 = 0: NTSC */
    /* Noise [4:3]: four values, wraps */
    tango_core_event(&cm, TANGO_EV_DOWN); CHECK(cm.sel == 3);
    for (int i = 0; i < 3; i++) tango_core_event(&cm, TANGO_EV_SELECT);
    CHECK(r.status[0] == 0x18); tango_core_page(&cm, &p); CHECK(row_is(&p, 3, TR_OPT, "Noise", "Blue"));
    tango_core_event(&cm, TANGO_EV_SELECT); CHECK(r.status[0] == 0x00);
    /* Aspect ratio [122:121]: [ARC1]/[ARC2] need MiSTer.ini ratios Tango has not: Original <-> Full Screen */
    tango_core_event(&cm, TANGO_EV_UP); tango_core_event(&cm, TANGO_EV_UP); CHECK(cm.sel == 1);
    tango_core_event(&cm, TANGO_EV_SELECT); CHECK(r.status[15] == 0x02);
    tango_core_event(&cm, TANGO_EV_SELECT); CHECK(r.status[15] == 0x00);
    /* an out-of-range value shows value 0 (menu.cpp 1935) */
    cm.status[15] = 0x04; tango_core_toggle(&cm); tango_core_toggle(&cm);          /* x = 2 = "[ARC1]" */
    tango_core_page(&cm, &p); CHECK(row_is(&p, 1, TR_OPT, "Aspect ratio", "Original")); cm.status[15] = 0;
    /* Up from row 1 wraps to the exit row; Down wraps back */
    CHECK(cm.sel == 1); tango_core_event(&cm, TANGO_EV_UP); CHECK(cm.sel == 13);
    tango_core_event(&cm, TANGO_EV_DOWN); CHECK(cm.sel == 1);

    /* T[0] Reset: bit 0 pulsed, OSD stays; R[0]: pulsed, OSD closes */
    while (cm.sel != 10) tango_core_event(&cm, TANGO_EV_DOWN);
    r.status_writes = 0; tango_core_event(&cm, TANGO_EV_SELECT);
    CHECK(r.status_writes == 2 && r.status[0] == 0 && cm.mode == TANGO_CM_PAGE);
    tango_core_event(&cm, TANGO_EV_DOWN); r.status_writes = 0;
    CHECK(tango_core_event(&cm, TANGO_EV_SELECT) == TANGO_CM_CLOSED_NOW);
    CHECK(r.status_writes == 2 && cm.mode == TANGO_CM_CLOSED);

    /* page 1: 16_core_template_page1 (menumask 0: "d0" greyed, "H0" shown) */
    tango_core_toggle(&cm);
    while (cm.sel != 5) tango_core_event(&cm, TANGO_EV_DOWN);
    tango_core_event(&cm, TANGO_EV_SELECT);
    tango_core_page(&cm, &p);
    CHECKS(p.title, "Test Page 1"); CHECK(p.sel == 3); CHECK(p.nrows == 14);
    CHECK(row_is(&p, 1, TR_CENTER, "-= Options in page 1 =-", 0)); CHECK(row_is(&p, 3, TR_OPT, "Option 1-1", "Off"));
    CHECK(row_is(&p, 4, TR_DIS, "Load *.BIN", 0)); CHECK(row_is(&p, 5, TR_OPT, "Option 1-2", "Off")); CHECK(row_is(&p, 13, TR_ACT, "back", 0));
    cm_dump("cm_template_page1", &cm);
    /* the greyed F does nothing */
    tango_core_event(&cm, TANGO_EV_DOWN); CHECK(cm.sel == 4);
    CHECK(tango_core_event(&cm, TANGO_EV_SELECT) == TANGO_CM_NOTHING && cm.mode == TANGO_CM_PAGE);
    /* menumask bit 0 set: F enabled, Option 1-2 hidden; the open page follows the core (menu.cpp 2640) */
    r.menumask = 1; CHECK(tango_core_poll_mask(&cm) == 1);
    tango_core_page(&cm, &p); CHECK(p.nrows == 14); CHECK(row_is(&p, 4, TR_ACT, "Load *.BIN", 0)); CHECK(row_is(&p, 5, TR_BLANK, 0, 0)); CHECK(row_is(&p, 13, TR_ACT, "back", 0));
    CHECK(tango_core_poll_mask(&cm) == 0);
    /* F: the browser in games/Template, BIN only, names without the extension; ioctl_index = the F digit 1 */
    CHECK(tango_core_event(&cm, TANGO_EV_SELECT) == TANGO_CM_REDRAWN && cm.mode == TANGO_CM_FILES);
    CHECKS(br.path, "games/Template");
    tango_core_page(&cm, &p);
    CHECKS(p.title, "Select"); CHECK(p.nrows == 4);
    CHECK(row_is(&p, 0, TR_UP, "..", 0)); CHECK(row_is(&p, 1, TR_DIR, "More", 0));
    CHECK(row_is(&p, 2, TR_FILE, "Alpha", 0)); CHECK(row_is(&p, 3, TR_FILE, "zeta", 0));
    cm_dump("cm_template_select", &cm);
    tango_core_event(&cm, TANGO_EV_DOWN); tango_core_event(&cm, TANGO_EV_DOWN);
    cm_dump("cm_template_select_file", &cm);                                           /* Alpha selected: the detail line */
    tango_core_event(&cm, TANGO_EV_DOWN);
    cm_files = 0;
    CHECK(tango_core_event(&cm, TANGO_EV_SELECT) == TANGO_CM_CLOSED_NOW);
    CHECK(cm_files == 1); CHECKS(cm_path, "/sd/games/Template/zeta.bin"); CHECK(cm_idx == 1); CHECK(cm.mode == TANGO_CM_CLOSED);
    /* back from a sub-page with B, Left; back row */
    r.menumask = 0; tango_core_toggle(&cm);
    while (cm.sel != 5) tango_core_event(&cm, TANGO_EV_DOWN);
    tango_core_event(&cm, TANGO_EV_SELECT); tango_core_event(&cm, TANGO_EV_LEFT);
    CHECK(cm.page == 0 && cm.sel == 5);
    tango_core_event(&cm, TANGO_EV_SELECT); tango_core_event(&cm, TANGO_EV_BACK); CHECK(cm.page == 0 && cm.sel == 5);
    /* page 2: S0 mount listed, "Option 2" with values 1..4 */
    tango_core_event(&cm, TANGO_EV_DOWN); CHECK(cm.sel == 7); tango_core_event(&cm, TANGO_EV_SELECT);
    tango_core_page(&cm, &p); CHECKS(p.title, "Test Page 2");
    CHECK(row_is(&p, 3, TR_ACT, "Mount *.DSK", 0)); CHECK(row_is(&p, 4, TR_OPT, "Option 2", "1"));
    tango_core_event(&cm, TANGO_EV_DOWN); tango_core_event(&cm, TANGO_EV_SELECT); CHECK(r.status[0] == 0x40);
    tango_core_event(&cm, TANGO_EV_SELECT); tango_core_event(&cm, TANGO_EV_SELECT); tango_core_event(&cm, TANGO_EV_SELECT);
    CHECK(r.status[0] == 0x00);
    tango_core_event(&cm, TANGO_EV_DOWN); tango_core_event(&cm, TANGO_EV_SELECT); CHECK(cm.page == 0 && cm.sel == 7);  /* back row */

    /* Right: System (launcher_pages 11_incore_system, this core's name) */
    CHECK(tango_core_event(&cm, TANGO_EV_RIGHT) == TANGO_CM_REDRAWN && cm.mode == TANGO_CM_SYSTEM);
    tango_core_page(&cm, &p);
    CHECKS(p.title, "System"); CHECK(p.arrow == TANGO_ARROW_NONE); CHECK(p.sel == 0); CHECK(p.nrows == 16);
    CHECK(row_is(&p, 2, TR_SUB, "Define Template buttons", 0)); CHECK(row_is(&p, 13, TR_BLANK, 0, 0));
    CHECK(row_is(&p, 14, TR_ACT, "Reboot (hold \x7f cold reboot)", 0)); CHECK(row_is(&p, 15, TR_ACT, "\x83  exit", 0));
    cm_dump("cm_template_system", &cm);
    /* Left: back to page 0, menusub 0 (menu.cpp 3132-3150) */
    tango_core_event(&cm, TANGO_EV_LEFT); CHECK(cm.mode == TANGO_CM_PAGE && cm.sel == 1);
    tango_core_event(&cm, TANGO_EV_RIGHT);
    /* Save settings: config/Template.CFG = the 16 raw bytes; then page 0 */
    cm.status[0] = 0x04; cm_saves = 0;
    while (cm.sel != 9) tango_core_event(&cm, TANGO_EV_DOWN);
    tango_core_event(&cm, TANGO_EV_SELECT);
    CHECK(cm_saves == 1); CHECKS(cm_savename, "Template.CFG"); CHECK(cm_saved[0] == 0x04); CHECK(cm.mode == TANGO_CM_PAGE && cm.page == 0);
    /* Reset settings: the Reset page with "no" selected (launcher_pages 14) */
    tango_core_event(&cm, TANGO_EV_RIGHT);
    while (cm.sel != 8) tango_core_event(&cm, TANGO_EV_DOWN);
    tango_core_event(&cm, TANGO_EV_SELECT);
    CHECK(cm.mode == TANGO_CM_RESET); tango_core_page(&cm, &p); CHECKS(p.title, "Reset"); CHECK(p.sel == 3);
    cm_dump("cm_template_reset", &cm);
    tango_core_event(&cm, TANGO_EV_SELECT); CHECK(cm.mode == TANGO_CM_SYSTEM && cm.sel == 8);      /* no */
    tango_core_event(&cm, TANGO_EV_SELECT); tango_core_event(&cm, TANGO_EV_UP); CHECK(cm.sel == 2);
    cm_saves = 0; r.status_writes = 0;
    CHECK(tango_core_event(&cm, TANGO_EV_SELECT) == TANGO_CM_CLOSED_NOW);                           /* yes */
    CHECK(cm_saves == 1 && cm_saved[0] == 0 && r.status_writes == 1 && r.status[0] == 0 && cm.mode == TANGO_CM_CLOSED);
    /* Core: the Cores browser at the root; a core = plat->load_core */
    tango_core_toggle(&cm); tango_core_event(&cm, TANGO_EV_RIGHT); CHECK(cm.sel == 0);
    tango_core_event(&cm, TANGO_EV_SELECT); CHECK(cm.mode == TANGO_CM_CORES);
    tango_core_page(&cm, &p); CHECKS(p.title, "Cores"); CHECK(p.nrows == 1); CHECK(row_is(&p, 0, TR_DIR, "Console", 0));
    tango_core_event(&cm, TANGO_EV_SELECT); tango_core_event(&cm, TANGO_EV_DOWN);
    cm_loads = 0; tango_core_event(&cm, TANGO_EV_SELECT);
    CHECK(cm_loads == 1); CHECKS(cm_path, "/sd/_Console/NES_20260921.fs");
    /* B in the Cores browser: up a folder, at the root System */
    tango_core_event(&cm, TANGO_EV_BACK); CHECK(cm.mode == TANGO_CM_CORES && cm.br->path[0] == 0);
    tango_core_event(&cm, TANGO_EV_BACK); CHECK(cm.mode == TANGO_CM_SYSTEM);
    /* Reboot: the caller times the hold */
    while (cm.sel != 14) tango_core_event(&cm, TANGO_EV_DOWN);
    CHECK(tango_core_event(&cm, TANGO_EV_SELECT) == TANGO_CM_REBOOT);
    CHECK(!tango_reboot_cold(0)); CHECK(!tango_reboot_cold(1299)); CHECK(tango_reboot_cold(1300));
    /* About: switches to TANGO_CM_ABOUT mode */
    tango_core_event(&cm, TANGO_EV_UP); CHECK(cm.mode == TANGO_CM_SYSTEM); CHECKS(cm.lab[cm.sel], "About");
    tango_core_event(&cm, TANGO_EV_SELECT); CHECK(cm.mode == TANGO_CM_ABOUT);
    tango_core_event(&cm, TANGO_EV_BACK); CHECK(cm.mode == TANGO_CM_SYSTEM);
    /* not-built items (Help) say so */
    while (strcmp(cm.lab[cm.sel], "Help") != 0) tango_core_event(&cm, TANGO_EV_UP);
    CHECK(tango_core_event(&cm, TANGO_EV_SELECT) == TANGO_CM_REDRAWN);
    tango_core_page(&cm, &p); CHECK(p.nlegend == 1); CHECKS(p.legend[0].key, "Not available yet");
    /* Menu closes from anywhere */
    CHECK(tango_core_event(&cm, TANGO_EV_MENU) == TANGO_CM_CLOSED_NOW && cm.mode == TANGO_CM_CLOSED);

    /* a saved .CFG at core start, and the _v<N> name */
    static const uint8_t CFG[16] = { 0x04, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x02 };
    r.status_writes = 0; tango_core_init(&cm, &link, &osd, &br, &plat, conf, CFG);
    CHECK(r.status_writes == 2 && r.status[0] == 0x04 && r.status[15] == 0x02);
    tango_core_toggle(&cm); tango_core_page(&cm, &p);
    CHECK(row_is(&p, 1, TR_OPT, "Aspect ratio", "Full Screen")); CHECK(row_is(&p, 2, TR_OPT, "TV Mode", "PAL"));
    tango_core_init(&cm, &link, &osd, &br, &plat, "Foo;;O[1],X,A,B;v,3;", NULL); CHECKS(cm.cfgname, "Foo_v3.CFG");
    tango_core_init(&cm, &link, &osd, &br, &plat, "Foo;;O[1],X,A,B;v,123;", NULL); CHECKS(cm.cfgname, "Foo_v12.CFG");   /* str[2] = 0 */
    /* core_ver extracted from V, entry */
    tango_core_init(&cm, &link, &osd, &br, &plat, "NES;;V,v260930;", NULL); CHECKS(cm.core_ver, "v260930");
    tango_core_init(&cm, &link, &osd, &br, &plat, "NES;;", NULL); CHECK(cm.core_ver[0] == 0);   /* no V, = empty */
    /* About page: System -> About via select, back via left/back. This platform has no fw_id (a host test, or a build made
     * without build.sh): the version row says the build is unknown and the page is otherwise whole */
    tango_core_init(&cm, &link, &osd, &br, &plat, "NES;;O[1],X,A,B;V,v260930;", NULL);
    tango_core_toggle(&cm);                                     /* page 0 */
    tango_core_event(&cm, TANGO_EV_RIGHT);                      /* -> System */
    while (cm.sel < cm.nrows - 1 && strcmp(cm.lab[cm.sel], "About") != 0) tango_core_event(&cm, TANGO_EV_DOWN);
    tango_core_event(&cm, TANGO_EV_SELECT);                     /* -> About */
    CHECK(cm.mode == TANGO_CM_ABOUT); tango_core_page(&cm, &p);
    CHECK(row_is(&p, 8, TR_CENTER, "TangoFPGA.com", 0)); CHECK(row_is(&p, 10, TR_CENTER, "Tango FPGA (build unknown)", 0));
    CHECK(row_is(&p, 11, TR_CENTER, "NES v260930", 0));
    CHECK(p.nrows == TANGO_VIS && p.sel == -1 && p.rows[13].kind == TR_SCROLL);
    tango_core_event(&cm, TANGO_EV_BACK); CHECK(cm.mode == TANGO_CM_SYSTEM && !strcmp(cm.lab[cm.sel], "About"));   /* the cursor is back on About */
    about_checks(&cm, &link, &osd, &br, &plat);
    /* a core with nothing on page 0: the System page (menu.cpp 2233-2239) */
    tango_core_init(&cm, &link, &osd, &br, &plat, "Bare;;V,v1;", NULL); tango_core_toggle(&cm); CHECK(cm.mode == TANGO_CM_SYSTEM);
    /* F without a digit: ioctl_index = menusub + 1; the ext index (user_io_ext_idx) goes in bits 7:6 */
    tango_core_init(&cm, &link, &osd, &br, &plat, "Two;;O[1],X,A,B;F,BINGEN,Load ROM;", NULL); tango_core_toggle(&cm);
    tango_core_page(&cm, &p); CHECK(row_is(&p, 1, TR_ACT, "Load ROM *.BIN,GEN", 0));
    CHECK(tango_ext_idx("x.gen", "BINGEN") == 1);

    /* every page build reads the menumask (MENU_GENERIC_MAIN2, menu.cpp 1912): Option 1-1 On -> the core's mask
     * bit 0 -> the rebuilt page already has "Load *.BIN" enabled and Option 1-2 hidden, no poll needed */
    r.menumask = 0; tango_core_init(&cm, &link, &osd, &br, &plat, conf, NULL); tango_core_toggle(&cm);
    while (cm.sel != 5) tango_core_event(&cm, TANGO_EV_DOWN);
    tango_core_event(&cm, TANGO_EV_SELECT); CHECK(cm.page == 1 && cm.sel == 3);
    r.menumask = 1;                                   /* what the Template core answers once status[5] is set */
    tango_core_event(&cm, TANGO_EV_SELECT);
    tango_core_page(&cm, &p); CHECK(p.nrows == 14); CHECK(row_is(&p, 3, TR_OPT, "Option 1-1", "On")); CHECK(row_is(&p, 4, TR_ACT, "Load *.BIN", 0));
    CHECK(tango_core_poll_mask(&cm) == 0);
    r.menumask = 0;

    /* ---- System > Define Template buttons: the dialog inside the in-core menu ---- */
    {
        tango_core_platform_t pl = { .fs = &fs, .load_core = cm_load_core, .reboot = cm_reboot,
                                     .load_file = cm_load_file, .save_cfg = cm_save, .dir_exists = cm_dir_exists,
                                     .save_map = cm_save_map, .maps_changed = cm_maps_changed };
        tango_core_init(&cm, &link, &osd, &br, &pl, conf, NULL); tango_core_toggle(&cm);
        tango_core_event(&cm, TANGO_EV_RIGHT); tango_core_event(&cm, TANGO_EV_DOWN); CHECK(cm.mode == TANGO_CM_SYSTEM && cm.sel == 2);
        tango_core_set_known(&cm, 1, 1);                                           /* the DualShock: the FPGA's decode (has_default_map) */
        /* MiSTer has no list: Select on the System page's Define row starts the guided walk (menu.cpp 3011-3019) */
        tango_core_raw(&cm, 1, 0x054c, 0x09cc, 1u << 5, 15);                       /* A (Circle) is held as it opens */
        CHECK(tango_core_event(&cm, TANGO_EV_SELECT) == TANGO_CM_REDRAWN && cm.mode == TANGO_CM_DEFINE && tango_core_capturing(&cm));
        tango_core_raw(&cm, 1, 0x054c, 0x09cc, 0, 20);                             /* its release is not a button */
        CHECK(cm.pm.dev == -1 && cm.pm.button == 0 && cm.pm.map[0] == 0);
        tango_core_page(&cm, &p); CHECKS(p.title, "Define buttons"); CHECKS(p.rows[3].label, "Press: RIGHT");
        CHECK(tango_core_event(&cm, TANGO_EV_DOWN) == TANGO_CM_NOTHING);          /* pad navigation is off */
        uint32_t now = 0; pm_saves = pm_changed = 0;
        int order[8] = { 1, 0, 2, 3, 4, 5, 7, 6 };                                  /* Template: 4 dirs + BUTTON A..Y */
        int res = 0;
        for (int i = 0; i < 8; i++) {
            tango_core_raw(&cm, 1, 0x054c, 0x09cc, 1u << order[i], (now += 40));
            res = tango_core_raw(&cm, 1, 0x054c, 0x09cc, 0, (now += 40));
        }
        CHECK(res == TANGO_CM_REDRAWN && pm_saves == 1 && pm_changed == 1);
        CHECKS(pm_name, "Template_input_054c_09cc_v3.map");
        CHECK(pm_map[0] == TANGO_PM_CODE(1) && pm_map[1] == TANGO_PM_CODE(0) && pm_map[6] == TANGO_PM_CODE(7) && pm_map[8] == 0);
        CHECK(cm.mode == TANGO_CM_DEFINE && cm.pm.state == PMC_ASK && !tango_core_capturing(&cm));
        CHECK(tango_core_event(&cm, TANGO_EV_SELECT) == TANGO_CM_REDRAWN);        /* No: back to the System page, cursor on the Define row (menusub 2) */
        CHECK(cm.mode == TANGO_CM_SYSTEM && cm.sel == 2 && pm_saves == 1);
        /* Esc (menu) cancels: nothing saved, the pads get their maps back, back on the System page */
        tango_core_event(&cm, TANGO_EV_SELECT); tango_core_raw(&cm, 1, 1, 1, 1, (now += 40));
        CHECK(tango_core_define_key(&cm, PMC_KEY_ESC) == TANGO_CM_REDRAWN && cm.mode == TANGO_CM_SYSTEM && cm.sel == 2 && pm_saves == 1 && pm_changed == 2);
    }

    /* ---- the download feeder against the core's IMAGE handshake ---- */
    static uint8_t data[1500]; for (unsigned i = 0; i < sizeof data; i++) data[i] = (uint8_t)(i * 7 + 3);
    tango_dl_t dl; src.d = data; src.n = sizeof data; src.pos = 0; src.fail_at = -1;
    r.dl_space = 300; r.dl_dropped = 0;
    CHECK(tango_dl_begin(&dl, &link, 0x41, sizeof data) == 0);
    CHECK(r.dl_open && r.dl_index == 0x41 && r.dl_size == sizeof data && r.dl_starts == 1);
    int k = 0, st;
    while ((st = tango_dl_pump(&dl, src_read, NULL)) == 1 && k < 100) k++;
    CHECK(st == 0); CHECK(k == 5);      /* 299 bytes offered each time: 6 chunks */
    CHECK(!r.dl_open && r.dl_ends == 1 && r.dl_got == sizeof data && r.dl_dropped == 0);
    CHECK(r.dl_crc == crc32_of(data, sizeof data));
    /* a read error ends the download */
    src.pos = 0; src.fail_at = 600; r.dl_space = 2048;
    tango_dl_begin(&dl, &link, 1, sizeof data);
    while ((st = tango_dl_pump(&dl, src_read, NULL)) == 1) ;
    CHECK(st == -1 && !r.dl_open && r.dl_ends == 2);
    CHECK(tango_dl_begin(&dl, &link, 1, 0) == -1 && r.dl_starts == 2);
}

/* ---------------------------------------------------------------- Define buttons (tango_padmap) */
#define C(r) TANGO_PM_CODE(r)
static int press_rel(tango_pmcap_t *c, int port, int bit, uint32_t *t)   /* a tap: press, then release */
{
    tango_pmcap_raw(c, port, 0x054c, 0x09cc, c->prev[port] | (1u << bit), (*t += 50));
    return tango_pmcap_raw(c, port, 0x054c, 0x09cc, c->prev[port] & ~(1u << bit), (*t += 50));
}
static void test_padmap(void)
{
    char n[TANGO_PM_NAMES][32]; uint32_t m[32]; char s[64]; uint8_t pr[64];
    /* parse_buttons: J fields after the first comma, unchanged (names keep "(...)"), stop at an empty one */
    CHECK(tango_pm_bnames("NES;;J1,A,B,Select,Start;V,v1;", n) == 4); CHECKS(n[0], "A"); CHECKS(n[3], "Start");
    CHECK(tango_pm_bnames("X;;JA,Fire,Paddle Btn(X),-;", n) == 3); CHECKS(n[1], "Paddle Btn(X)"); CHECKS(n[2], "-");
    CHECK(tango_pm_bnames("Template;;-;O[2],TV Mode,NTSC,PAL;", n) == 0);
    /* map_joystick: directions from the system map, names by joymapping.cpp's table */
    tango_pm_guess("NES;;J1,A,B,Select,Start;", m);
    CHECK(m[0] == C(0) && m[1] == C(1) && m[2] == C(2) && m[3] == C(3));
    CHECK(m[4] == C(5) && m[5] == C(4) && m[6] == C(12) && m[7] == C(13) && m[8] == 0);
    /* a "-" button is skipped, but map_joystick's n (the jn/jp slot) indexes nnames, which read_buttons filled per
     * button: without jn, every name after a "-" is looked up one slot late (joymapping.cpp 130-141, as MiSTer) */
    tango_pm_guess("X;;J1,Fire,-,Fire 2,Coin,Jump(Up),Mode;", m);
    CHECK(m[4] == C(5) && m[5] == 0 && m[6] == 0 && m[7] == C(4) && m[8] == C(9) && m[9] == C(5));
    tango_pm_guess("X;;J1,Jump,Shoot;jn,B,A;", m);                     /* jn names the defaults */
    CHECK(m[4] == C(4) && m[5] == C(5));
    tango_pm_guess("X;;J1,Punch,Kick;", m);                            /* unknown names: unmapped */
    CHECK(m[4] == 0 && m[5] == 0 && m[0] == C(0));
    tango_pm_guess("Template;;-;", m); CHECK(m[3] == C(3) && m[4] == 0);
    tango_pm_mapname("NES", 0x054c, 0x09cc, s, sizeof s); CHECKS(s, "NES_input_054c_09cc_v3.map");
    tango_pm_mapname("A2600", 0, 0, s, sizeof s); CHECKS(s, "A2600_input_0000_0000_v3.map");
    m[0] = C(1) | (C(7) << 16); m[31] = 0;
    CHECK(tango_pm_pairs(m, pr) == 32 && pr[0] == 1 && pr[1] == 7 && pr[3] == 0xFF && pr[62] == 0xFF);

    /* ---- the dialog: J1,Fire,Start -> RIGHT LEFT DOWN UP Fire Start (6), on USB-A2 (port 1) ---- */
    static tango_pmcap_t c; uint32_t t = 0; tango_page_t p;
    tango_pmcap_known(&c, 0, 1); tango_pmcap_known(&c, 1, 1);                       /* both pads have a system map or a decode */
    tango_pmcap_start(&c, "X;;J1,Fire,Start;");
    CHECK(c.count == 6 && c.dev == -1);
    tango_pmcap_page(&c, &p, "X", t);
    CHECKS(p.title, "Define buttons"); CHECKS(p.rows[3].label, "Press: RIGHT"); CHECK(p.rows[5].kind == TR_BLANK);
    CHECKS(p.rows[8].label, "Menu-hold \x7f Cancel"); CHECKS(p.rows[9].label, "Enter \x7f Finish");
    dump_layout("pm_right_nodev", &p);
    /* press Left for RIGHT: taken on the press, the prompt moves on the release */
    CHECK(tango_pmcap_raw(&c, 1, 0x054c, 0x09cc, 1u << 1, (t += 50)) == PMC_REDRAW);
    CHECK(c.dev == 1 && c.button == 0 && c.map[0] == C(1));
    CHECK(tango_pmcap_raw(&c, 1, 0x054c, 0x09cc, 0, (t += 50)) == PMC_REDRAW && c.button == 1);
    tango_pmcap_page(&c, &p, "X", t);
    CHECKS(p.rows[3].label, "Press: LEFT"); CHECKS(p.rows[5].label, "Joystick ID: 054c:09cc");
    CHECKS(p.rows[7].label, "User/Menu \x7f Undefine"); CHECKS(p.rows[10].label, "F12 \x7f Clear all");
    dump_layout("pm_left", &p);
    /* the other port is ignored now; a second key while one is held is ignored */
    press_rel(&c, 0, 0, &t); CHECK(c.button == 1);
    tango_pmcap_raw(&c, 1, 0x054c, 0x09cc, (1u << 0), (t += 50));
    tango_pmcap_raw(&c, 1, 0x054c, 0x09cc, (1u << 0) | (1u << 5), (t += 50));    /* Circle while Right is held */
    tango_pmcap_raw(&c, 1, 0x054c, 0x09cc, (1u << 5), (t += 50));                  /* Right released: advance */
    CHECK(c.button == 2 && c.map[1] == C(0));
    tango_pmcap_raw(&c, 1, 0x054c, 0x09cc, 0, (t += 50)); CHECK(c.button == 2);   /* Circle's release: nothing */
    /* a code already used in this pass is refused (Left again for DOWN) */
    press_rel(&c, 1, 1, &t); CHECK(c.button == 2 && c.map[2] == 0);
    press_rel(&c, 1, 2, &t); press_rel(&c, 1, 3, &t); CHECK(c.button == 4 && c.map[3] == C(3));
    tango_pmcap_page(&c, &p, "X", t); CHECKS(p.rows[3].label, "Press: Fire");
    press_rel(&c, 1, 4, &t);                                                       /* Fire = Cross */
    CHECK(press_rel(&c, 1, 13, &t) == PMC_SAVED && c.state == PMC_ASK);             /* Start = Options: the end */
    CHECK(c.map[0] == C(1) && c.map[1] == C(0) && c.map[4] == C(4) && c.map[5] == C(13) && c.map[6] == 0);
    tango_pmcap_page(&c, &p, "X", t);
    CHECKS(p.rows[6].label, "Do you want to setup"); CHECK(p.sel == 8);
    dump_layout("pm_ask", &p);
    /* a pad the system does not know (no system map, no built-in decode): MiSTer stops at the first press (menu.cpp 4366-4373) */
    { static tango_pmcap_t u; tango_page_t pu;
      tango_pmcap_start(&u, "X;;J1,Fire,Start;");
      CHECK(tango_pmcap_raw(&u, 0, 0x2dc8, 0x3106, 1u << 8, 100) == PMC_REDRAW && u.state == PMC_NEED && u.dev < 0);
      tango_pmcap_page(&u, &pu, "X", 100); CHECKS(pu.rows[6].label, "You need to define this"); CHECKS(pu.rows[7].label, "joystick in Menu core first"); CHECKS(pu.rows[9].label, "Press ESC/Enter");
      dump_layout("pm_need", &pu);
      CHECK(tango_pmcap_key(&u, PMC_KEY_DOWN) == PMC_NONE && tango_pmcap_key(&u, PMC_KEY_ENTER) == PMC_DISMISSED);
      tango_pmcap_known(&u, 0, 1); tango_pmcap_start(&u, "X;;J1,Fire,Start;");
      CHECK(tango_pmcap_raw(&u, 0, 0x2dc8, 0x3106, 1u << 9, 200) == PMC_REDRAW && u.state == PMC_MAP && u.dev == 0);   /* once its system map exists it is mapped */
      /* the system map is the base of the guess: a pad whose Fire is raw 9 */
      uint32_t mm[32] = { 0 }, g[32]; for (int i = 0; i < 12; i++) mm[i] = C(i == 4 ? 9 : i);
      CHECK(tango_pm_sysmap_valid(mm)); tango_pm_guess_mm("X;;J1,Fire,Start;", mm, g); CHECK(g[4] == C(9) && g[0] == C(0));
      mm[3] = 0; CHECK(!tango_pm_sysmap_valid(mm)); }
    /* Yes: the alternative pass, into the high 16 bits, same pad */
    CHECK(tango_pmcap_key(&c, PMC_KEY_DOWN) == PMC_REDRAW && c.ask_sel == 1);
    CHECK(tango_pmcap_key(&c, PMC_KEY_ENTER) == PMC_REDRAW && c.state == PMC_MAP && c.set == 1 && c.dev == 1 && !c.first);
    tango_pmcap_page(&c, &p, "X", t); CHECKS(p.rows[7].label, "User/Menu \x7f Undefine");   /* !joymap_first */
    press_rel(&c, 1, 7, &t);                                                       /* RIGHT alternative = Triangle */
    CHECK(c.map[0] == (C(1) | (C(7) << 16)) && c.button == 1);
    CHECK(tango_pmcap_key(&c, PMC_KEY_ENTER) == PMC_SAVED);                         /* Enter: finish, saved */
    CHECK(c.map[1] == C(0) && c.map[5] == C(13));

    /* skip: the OSD combo (Share + Options) tapped; held 1 s = cancel */
    tango_pmcap_start(&c, "X;;J1,Fire,Start;"); t = 1000;
    press_rel(&c, 1, 1, &t); CHECK(c.button == 1);
    tango_pmcap_raw(&c, 1, 1, 2, (1u << 12), (t += 50));                            /* Share: taken for LEFT... */
    CHECK(c.map[1] == C(12));
    tango_pmcap_raw(&c, 1, 1, 2, (1u << 12) | (1u << 13), (t += 50));               /* + Options: the combo */
    tango_pmcap_raw(&c, 1, 1, 2, (1u << 12), (t += 100));                           /* short: skip, undefine */
    CHECK(c.button == 2 && c.map[1] == 0);
    tango_pmcap_raw(&c, 1, 1, 2, 0, (t += 50));
    /* the combo behaves as MiSTer's one-key menu button: Share was given back, so the next button is taken normally
     * (a straight copy of input.cpp 3292-3419 would keep Share pending and ignore this press) */
    press_rel(&c, 1, 2, &t); CHECK(c.button == 3 && c.map[2] == C(2));
    press_rel(&c, 1, 3, &t); CHECK(c.button == 4 && c.map[3] == C(3));
    /* Share alone is still a button like any other */
    tango_pmcap_start(&c, "X;;J1,Fire,Start;"); t = 5000;
    press_rel(&c, 1, 12, &t); CHECK(c.button == 1 && c.map[0] == C(12));
    /* Options first, then Share: the same */
    tango_pmcap_raw(&c, 1, 1, 2, (1u << 13), (t += 50));
    tango_pmcap_raw(&c, 1, 1, 2, (1u << 13) | (1u << 12), (t += 50));
    tango_pmcap_raw(&c, 1, 1, 2, (1u << 13), (t += 100)); tango_pmcap_raw(&c, 1, 1, 2, 0, (t += 50));
    CHECK(c.button == 2 && c.map[1] == 0);
    press_rel(&c, 1, 3, &t); CHECK(c.button == 3 && c.map[2] == C(3));
    tango_pmcap_raw(&c, 1, 1, 2, (1u << 12), (t += 50));
    tango_pmcap_raw(&c, 1, 1, 2, (1u << 12) | (1u << 13), (t += 50));
    CHECK(!tango_pmcap_canceling(&c, t + 999) && tango_pmcap_canceling(&c, t + 1000));
    tango_pmcap_page(&c, &p, "X", t + 1000); CHECKS(p.rows[4].label, "Canceling"); CHECK(p.rows[3].kind == TR_BLANK);
    CHECK(tango_pmcap_raw(&c, 1, 1, 2, (1u << 13), t + 1200) == PMC_DISMISSED);    /* released after 1 s: cancel */
    /* F12 clears (once a pad is picked), Esc = cancel, Enter on the first pass saves what there is */
    tango_pmcap_start(&c, "X;;J1,Fire,Start;"); t = 0;
    CHECK(tango_pmcap_key(&c, PMC_KEY_F12) == PMC_NONE);
    press_rel(&c, 1, 1, &t); press_rel(&c, 1, 0, &t);
    CHECK(tango_pmcap_key(&c, PMC_KEY_F12) == PMC_REDRAW && c.button == 0 && c.map[0] == 0 && c.clear);
    tango_pmcap_page(&c, &p, "X", t); CHECKS(p.rows[4].label, "Clearing");
    press_rel(&c, 1, 5, &t); CHECK(!c.clear && c.map[0] == C(5));
    CHECK(tango_pmcap_key(&c, PMC_KEY_ESC) == PMC_DISMISSED);
    CHECK(tango_pmcap_key(&c, PMC_KEY_ENTER) == PMC_SAVED && c.state == PMC_MAP);  /* 054c & 09cc: even -> no question */
    /* "-" names are skipped automatically; no J = BUTTON A..Y (8) */
    tango_pmcap_start(&c, "X;;J1,Fire,-,Start;"); t = 0;
    for (int b = 0; b < 5; b++) press_rel(&c, 1, b + 4, &t);
    CHECK(c.map[4] == C(8) && c.map[5] == 0 && c.map[6] == 0 && c.button == 6);
    tango_pmcap_start(&c, "Template;;-;"); CHECK(c.count == 8);
    t = 0; for (int b = 0; b < 4; b++) press_rel(&c, 1, b, &t);
    tango_pmcap_page(&c, &p, "TEMPLATE", t); CHECKS(p.rows[3].label, "Press: BUTTON A");
    dump_layout("pm_template_a", &p);
    /* a different menu button (PS, raw bit 16): its tap skips, it held 1 s cancels, and Share + Options are ordinary buttons */
    tango_pmcap_start(&c, "X;;J1,Fire,Start;"); tango_pmcap_set_menu(&c, 1, 16, 16); t = 7000;
    press_rel(&c, 1, 1, &t); CHECK(c.button == 1);
    tango_pmcap_raw(&c, 1, 1, 2, (1u << 12) | (1u << 13), (t += 50));                 /* Share + Options together: Share is the key (no combo) */
    CHECK(c.map[1] == C(12));
    tango_pmcap_raw(&c, 1, 1, 2, 0, (t += 50)); CHECK(c.button == 2);
    tango_pmcap_raw(&c, 1, 1, 2, (1u << 16), (t += 50));                              /* PS: the menu button, never a button */
    tango_pmcap_raw(&c, 1, 1, 2, 0, (t += 100)); CHECK(c.button == 3 && c.map[2] == 0);   /* a tap: skip */
    tango_pmcap_start(&c, "X;;J1,Fire,Start;"); CHECK(c.mb[1][0] == 16 && c.mb[1][1] == 16 && c.mb[0][0] == 12 && c.mb[0][1] == 13);   /* kept across a dialog start; port 1 only */
    t = 9000; press_rel(&c, 1, 1, &t);
    tango_pmcap_raw(&c, 1, 1, 2, (1u << 16), (t += 50));
    CHECK(tango_pmcap_raw(&c, 1, 1, 2, 0, (t += 1100)) == PMC_DISMISSED);              /* held 1 s: cancel */
}

/* ---------------------------------------------------------------- saves (tango_save) and the keyboard (tango_kbd) */
static struct { char path[TANGO_SAVE_PATH]; uint8_t d[4096]; uint32_t size; int used, ro; } ff[4];
static char fdirs[8][64]; static int nfdirs;
static int ff_open(void *c, const char *p, int create, int *w, uint32_t *sz)
{
    (void)c;
    for (int i = 0; i < 4; i++) if (ff[i].used && !strcmp(ff[i].path, p)) { *w = !ff[i].ro; *sz = ff[i].size; return i; }
    if (!create) return -1;
    for (int i = 0; i < 4; i++) if (!ff[i].used) { ff[i].used = 1; strcpy(ff[i].path, p); ff[i].size = 0; *w = 1; *sz = 0; return i; }
    return -1;
}
static int ff_read(void *c, int h, uint32_t off, uint8_t *b, unsigned n)
{
    (void)c; if (off >= ff[h].size) return 0;
    if (off + n > ff[h].size) n = ff[h].size - off;
    memcpy(b, ff[h].d + off, n); return (int)n;
}
static int ff_write(void *c, int h, uint32_t off, const uint8_t *b, unsigned n)
{
    (void)c; if (off + n > sizeof ff[h].d) return -1;
    memcpy(ff[h].d + off, b, n); if (off + n > ff[h].size) ff[h].size = off + n; return (int)n;
}
static void ff_close(void *c, int h) { (void)c; (void)h; }
static int ff_mkdir(void *c, const char *p) { (void)c; if (nfdirs < 8) strcpy(fdirs[nfdirs++], p); return 0; }
static char kev[32][8]; static int nkev;
static void kcb(void *c, uint8_t code, int press, int mod) { (void)c; if (nkev < 32) snprintf(kev[nkev++], 8, "%c%s%02x", press ? '+' : '-', mod ? "m" : "", code); }

static void test_save_kbd(void)
{
    char p[TANGO_SAVE_PATH]; uint8_t fl, b[512], r[512];
    /* FileGenerateSavePath: saves/<core>/<rom name, extension replaced>.sav */
    tango_save_path("NES", "/sd/games/NES/Super Mario Bros. 3 (USA).nes", p, sizeof p);
    CHECKS(p, "saves/NES/Super Mario Bros. 3 (USA).sav");
    tango_save_path("NES", "zelda", p, sizeof p); CHECKS(p, "saves/NES/zelda.sav");
    CHECK(tango_save_wanted("NES;;FS,NESFDSNSF;F2,BIN;", 2) && !tango_save_wanted("NES;;FS,NESFDSNSF;F2,BIN;", 3));
    /* a missing save: mounted "created upon write", size 0, writable; reads 0xFF; lba 1 before lba 0 is lost */
    tango_savefs_t fs = { ff_open, ff_read, ff_write, ff_close, ff_mkdir, NULL };
    static tango_slot_t s; memset(&s, 0, sizeof s); s.h = -1; nfdirs = 0;
    CHECK(tango_save_mount(&s, &fs, "saves/NES/zelda.sav", 0, 1, &fl) == 0 && fl == 0x01 && s.missing);
    CHECK(nfdirs == 2 && !strcmp(fdirs[0], "saves") && !strcmp(fdirs[1], "saves/NES"));
    tango_save_read(&s, &fs, 3, r); CHECK(r[0] == 0xFF && r[511] == 0xFF);
    for (int i = 0; i < 512; i++) b[i] = (uint8_t)i;
    tango_save_write(&s, &fs, 1, b); CHECK(s.missing && !ff[0].used);
    tango_save_write(&s, &fs, 0, b); CHECK(!s.missing && s.size == 512 && ff[0].used && ff[0].size == 512);
    /* then it grows one sector at a time (lba <= size / 512, cangrow); a gap is refused */
    tango_save_write(&s, &fs, 3, b); CHECK(s.size == 512);
    b[0] = 0xAA; tango_save_write(&s, &fs, 1, b); CHECK(s.size == 1024 && ff[0].d[512] == 0xAA);
    tango_save_read(&s, &fs, 1, r); CHECK(r[0] == 0xAA && r[1] == 1);
    tango_save_read(&s, &fs, 5, r); CHECK(r[0] == 0 && r[511] == 0);            /* past the end of a file: 0x00 */
    /* an existing file mounted without pre (not cangrow): writes clipped to its size */
    static tango_slot_t s2; memset(&s2, 0, sizeof s2); s2.h = -1;
    CHECK(tango_save_mount(&s2, &fs, "saves/NES/zelda.sav", 1, 0, &fl) == 1024 && fl == 0x02);
    tango_save_write(&s2, &fs, 2, b); CHECK(ff[0].size == 1024);
    ff[0].ro = 1; CHECK(tango_save_mount(&s2, &fs, "saves/NES/zelda.sav", 1, 0, &fl) == 1024 && fl == 0x82);
    tango_save_write(&s2, &fs, 0, b); CHECK(ff[0].d[0] == 0x00);                  /* read-only: nothing (b[0] is AA) */
    CHECK(tango_save_mount(&s2, &fs, "saves/NES/none.sav", 1, 0, &fl) == -1);       /* no file, no pre: nothing */

    /* the keyboard: boot report changes, releases first */
    tango_kbd_t k; memset(&k, 0, sizeof k);
    uint8_t r1[8] = { 0x02, 0, 0x04, 0x05, 0, 0, 0, 0 };        /* LShift + a + b */
    nkev = 0; CHECK(tango_kbd_diff(&k, r1, kcb, NULL) == 3); CHECKS(kev[0], "+m01"); CHECKS(kev[1], "+04"); CHECKS(kev[2], "+05");
    uint8_t r2[8] = { 0x00, 0, 0x05, 0x52, 0, 0, 0, 0 };        /* Shift and a up, Up down */
    nkev = 0; tango_kbd_diff(&k, r2, kcb, NULL); CHECK(nkev == 3); CHECKS(kev[0], "-m01"); CHECKS(kev[1], "-04"); CHECKS(kev[2], "+52");
    uint8_t r3[8] = { 0, 0, 1, 1, 1, 1, 1, 1 };                /* ErrorRollOver: ignored */
    nkev = 0; CHECK(tango_kbd_diff(&k, r3, kcb, NULL) == 0 && k.prev[3] == 0x52);
    /* menu keys (menu.cpp 1332-1440) */
    CHECK(tango_kbd_menu_key(0x45, 0, 0) == TKEY_TOGGLE && tango_kbd_menu_key(0x45, 1, 0) == TKEY_NONE);   /* F12 opens on release */
    CHECK(tango_kbd_menu_key(0x45, 1, 1) == TKEY_TOGGLE && tango_kbd_menu_key(0x45, 0, 1) == TKEY_NONE);   /* closes on press */
    CHECK(tango_kbd_menu_key(0x29, 1, 1) == TKEY_NONE && tango_kbd_menu_key(0x29, 0, 1) == TKEY_MENU);     /* Esc: release */
    CHECK(tango_kbd_menu_key(0x2A, 0, 1) == TKEY_BACK && tango_kbd_menu_key(0x2C, 1, 1) == TKEY_SPACE);
    CHECK(tango_kbd_menu_key(0x50, 1, 1) == TKEY_LEFT && tango_kbd_menu_key(0x4F, 1, 1) == TKEY_RIGHT);
    CHECK(tango_kbd_menu_key(0x4E, 1, 1) == TKEY_PGDN && tango_kbd_menu_key(0x04, 1, 1) == TKEY_NONE);
    CHECK(tango_kbd_ascii(0x04) == 'a' && tango_kbd_ascii(0x1E) == '1' && tango_kbd_ascii(0x27) == '0' && !tango_kbd_ascii(0x2C));
    CHECK(tango_kbd_passthru_mod(2) && tango_kbd_passthru_mod(7) && !tango_kbd_passthru_mod(1));

    /* typing in the Cores list (menu.cpp 5642-5668, file_io.cpp 1665-1678, 2008-2040) */
    static const char *TY[] = { "_Console/", "_Console/SNES_20260912.fs", "_Console/NES_20260921.fs", "_Console/Game Boy_20260903.fs",
        "_Console/Atari 2600_20260802.fs", "_Console/Master System_20260719.fs", "_Console/Game Gear_20260101.fs", "_Console/Genesis_20260101.fs" };
    static tango_browser_t tb; fake_card_t tc; tango_fs_t tfs;
    fake_card_init(&tc, &tfs, TY, 8);
    tango_browser_set_mode(&tb, 0, ""); tango_browser_scan(&tb, &tfs, "_Console");
    /* order: .., Atari 2600, Game Boy, Game Gear, Genesis, Master System, NES, SNES */
    CHECK(tb.n == 8);
    tango_browser_type(&tb, &tfs, 'g', 10000); CHECKS(tango_browser_alt(&tb, tango_browser_cur(&tb)), "Game Boy");
    tango_browser_type(&tb, &tfs, 'e', 10500);                   /* within 2 s: filter "ge" on the file names */
    CHECK(tb.n == 2); CHECKS(tango_browser_alt(&tb, &tb.e[tb.order[0]]), "Game Gear"); CHECKS(tango_browser_alt(&tb, &tb.e[tb.order[1]]), "Genesis");
    tango_browser_type(&tb, &tfs, 'n', 13000);                   /* after 2 s: the whole list again, jump to NES */
    CHECK(tb.n == 8); CHECKS(tango_browser_alt(&tb, tango_browser_cur(&tb)), "NES");
    tango_browser_type(&tb, &tfs, 'g', 16000); CHECKS(tango_browser_alt(&tb, tango_browser_cur(&tb)), "Game Boy");   /* from sel 0 */

    /* "Saving..." (MENU_SAVE_CHECK): opening the OSD within 500 ms of a save write */
    static tango_coremenu_t cm; static tango_osd_t osd; static tango_browser_t br; static rec_link_t rl; tango_link_t link;
    rec_link_init(&rl, &link); rl.confstr = "NES;;FS,NESFDSNSF;O[1],X,A,B;";
    tango_core_platform_t pl = { .fs = &tfs };
    tango_osd_probe(&osd, &link);
    tango_core_init(&cm, &link, &osd, &br, &pl, rl.confstr, NULL);
    tango_core_saved_sector(&cm, 1000); cm.now_ms = 1200;
    CHECK(tango_core_toggle(&cm) == 1 && cm.mode == TANGO_CM_SAVING);
    tango_page_t pg; tango_core_page(&cm, &pg); CHECKS(pg.rows[8].label, "Saving..."); CHECKS(pg.title, "NES");
    dump_layout("sv_saving", &pg);
    CHECK(tango_core_event(&cm, TANGO_EV_DOWN) == TANGO_CM_NOTHING);
    CHECK(tango_core_tick(&cm, 1400) == TANGO_CM_NOTHING && tango_core_tick(&cm, 1500) == TANGO_CM_REDRAWN && cm.mode == TANGO_CM_PAGE);
    tango_core_toggle(&cm); cm.now_ms = 2000; tango_core_toggle(&cm); CHECK(cm.mode == TANGO_CM_PAGE);   /* no write: page 0 */
    tango_core_toggle(&cm); tango_core_saved_sector(&cm, 3000); cm.now_ms = 3100; tango_core_toggle(&cm);
    CHECK(tango_core_event(&cm, TANGO_EV_MENU) == TANGO_CM_CLOSED_NOW && cm.mode == TANGO_CM_CLOSED);   /* Menu closes */
}

/* ---------------------------------------------------------------- file-list limits (tango_browser) */
static char big_nm[402][260]; static const char *big_p[402];
static void test_browse_limits(void)
{
    static tango_browser_t b; fake_card_t c; tango_fs_t fs; tango_page_t p;
    unsigned cost1 = 0, cost2 = 0, old = 0; int old_fit = 0;
    for (int i = 0; i < 401; i++) {
        snprintf(big_nm[i], sizeof big_nm[i], "Legend of Zelda %03d, The (USA) (Rev A).nes", i);   /* 42 characters */
        big_p[i] = big_nm[i];
        size_t l = strlen(big_nm[i]);
        if (i < 400) { cost1 += (unsigned)l + 1; cost2 += (unsigned)l + 2; }
        old += (unsigned)(l + 1) + (unsigned)(l - 4 + 1);                       /* the old store: name + display name */
        if (old <= TANGO_BROWSE_POOL) old_fit = i + 1;
    }
    printf("list limits: 400 names of %zu chars: %u bytes (display = name), %u (split); the old store fit %d\n",
           strlen(big_nm[0]), cost1, cost2, old_fit);
    CHECK(old_fit < 400);                                     /* the old store could not hold these 400 */
    /* 400 entries, a multi-extension core (display name = file name): stored once */
    fake_card_init(&c, &fs, big_p, 400);
    tango_browser_set_mode(&b, 1, "NESFDSNSF"); tango_browser_scan(&b, &fs, "");
    CHECK(b.n == 400 && !b.truncated && b.pool_used == cost1);
    CHECK(!b.e[0].split); CHECKS(tango_browser_name(&b, tango_browser_cur(&b)), big_nm[0]);
    /* a one-extension core: the display name drops ".nes" -- the name is stored once, split after the display name */
    tango_browser_set_mode(&b, 1, "NES"); tango_browser_scan(&b, &fs, "");
    CHECK(b.n == 400 && !b.truncated && b.pool_used == cost2);
    tango_browser_select(&b, 5);
    { char want[64]; snprintf(want, sizeof want, "Legend of Zelda %03d, The (USA) (Rev A)", 5);
      CHECKS(tango_browser_alt(&b, tango_browser_cur(&b)), want); }
    CHECKS(tango_browser_name(&b, tango_browser_cur(&b)), big_nm[5]);
    tango_browser_page(&b, &p, "TEMPLATE", NULL, 0); CHECK(p.nrows == 400 && !p.right);    /* not cut: no marks */
    /* 401: cut short -- the last row says so, the title hint counts the entries with a "+" */
    fake_card_init(&c, &fs, big_p, 401);
    tango_browser_scan(&b, &fs, "");
    CHECK(b.n == 400 && b.truncated);
    tango_browser_page(&b, &p, "TEMPLATE", NULL, 0);
    CHECK(p.nrows == 401 && p.rows[400].kind == TR_INFO); CHECKS(p.rows[400].label, "... list cut short at 400");
    CHECKS(p.right, "1/400+");
    tango_browser_prev(&b);                                   /* Up from the top: the last entry */
    CHECK(b.sel == 399 && b.first == 387);                    /* the window shows the cut row as its last line */
    tango_browser_page(&b, &p, "TEMPLATE", NULL, 0); CHECKS(p.right, "400/400+");
    tango_browser_prev(&b); CHECK(b.sel == 398 && b.first == 387);   /* moving up keeps it */
    tango_browser_next(&b); tango_browser_next(&b); CHECK(b.sel == 0 && b.first == 0);   /* wraps to the top */
    /* the layout of that end, with short names (14 rows of 42-character names need more than the 512 glyph
     * slots of a bank: a separate limit, see the report) */
    {
        static char sn[401][16]; static const char *sp[401];
        for (int i = 0; i < 401; i++) { snprintf(sn[i], sizeof sn[i], "Game %03d.nes", i); sp[i] = sn[i]; }
        fake_card_init(&c, &fs, sp, 401);
        tango_browser_scan(&b, &fs, ""); tango_browser_prev(&b);
        static const tango_legend_t LG[] = { { "A", "open" }, { "B", "back" } };
        tango_browser_page(&b, &p, "TEMPLATE", LG, 2); CHECKS(p.right, "400/400+");
        dump_layout("browse_cut_end", &p);
    }
    /* the pool runs out first: 150 names of 199 characters, split (one extension) = 201 bytes each */
    static char ln[150][260]; static const char *lp[150];
    for (int i = 0; i < 150; i++) {
        memset(ln[i], 'a' + (i % 26), 190); snprintf(ln[i] + 190, 70, "%05d.nes", i); lp[i] = ln[i];
    }
    fake_card_init(&c, &fs, lp, 150);
    tango_browser_scan(&b, &fs, "");
    CHECK(b.truncated && b.n == (int)(TANGO_BROWSE_POOL / (strlen(ln[0]) + 2)));
    tango_browser_page(&b, &p, "TEMPLATE", NULL, 0);
    { char want[40]; snprintf(want, sizeof want, "... list cut short at %d", b.n); CHECKS(p.rows[b.n].label, want); }
    /* core names: the display name cuts the date code and extension (split); "_" folders are stored whole */
    static const char *CO[] = { "_Console/", "_Console/Game Boy_20260903.fs", "_Console/NES_20260921.bin", "_Console/_Handhelds/" };
    fake_card_init(&c, &fs, CO, 4);
    tango_browser_set_mode(&b, 0, ""); tango_browser_scan(&b, &fs, "_Console");
    CHECK(b.n == 4);                                          /* .., _Handhelds, Game Boy, NES */
    tango_browser_select(&b, 1);
    CHECK(!tango_browser_cur(&b)->split); CHECKS(tango_browser_alt(&b, tango_browser_cur(&b)), "_Handhelds");
    tango_browser_select(&b, 2);
    CHECK(tango_browser_cur(&b)->split); CHECKS(tango_browser_alt(&b, tango_browser_cur(&b)), "Game Boy");
    CHECKS(tango_browser_name(&b, tango_browser_cur(&b)), "Game Boy_20260903.fs");
    CHECK(b.pool_used == 3 + 11 + 22 + 18);                   /* "..", "_Handhelds", "Game Boy|_20260903.fs", "NES|_20260921.bin" */
    tango_browser_page(&b, &p, "TANG CONSOLE", NULL, 0);
    CHECKS(p.rows[1].label, "Handhelds"); CHECKS(p.rows[3].value, "26.09.21");
}

/* ---------------------------------------------------------------- long names: the per-row glyph budget (tango_layout) */
static void test_long_names(void)
{
    static const char *NI[14] = {
        "Adventures of Rocky and Bullwinkle and Friends, The (USA).nes",
        "Advanced Dungeons & Dragons - Dragons of Flame (Japan).nes",
        "Bill & Ted's Excellent Video Game Adventure (USA).nes",
        "Captain America and the Avengers (USA) (Rev A) (Beta).nes",
        "Chip 'n Dale - Rescue Rangers 2 (USA) (Virtual Console).nes",
        "Dr. Seuss's Adventures of the Cat in the Hat, The (USA).nes",
        "Indiana Jones and the Temple of Doom (USA) (Rev A) (Tengen).nes",
        "Legend of Zelda, The (USA) (Rev A) (GameCube Edition).nes",
        "Mighty Morphin Power Rangers - The Movie (USA) (Proto).nes",
        "Super Mario Bros. + Duck Hunt + World Class Track Meet (USA).nes",
        "Teenage Mutant Ninja Turtles II - The Arcade Game (USA).nes",
        "Who Framed Roger Rabbit (USA) (Rev A) (Collector Edition).nes",
        "WWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWW.nes",
        "Short (USA).nes" };
    static tango_row_t rows[14];
    for (int i = 0; i < 14; i++) { rows[i].kind = TR_FILE; rows[i].col = 0; rows[i].label = NI[i]; rows[i].value = NULL; }
    rows[3].value = "26.09.21";                               /* a value leaves the label 29 - 8 glyphs */
    static const tango_legend_t LG[] = { { "A", "open" }, { "\x83\x7f", "page" }, { "B", "back" } };
    tango_page_t p = { "NINTENDO NES", "Select", NULL, 0, 2, 0, rows, 14, LG, 3, 0 };
    static tango_tables_t t;
    CHECK(tango_layout_page(&p, &t) == 0);
    printf("long names: 14 rows of %zu-%zu characters -> %d glyphs of %d, %d clipped\n",
           strlen(NI[13]), strlen(NI[9]), t.nglyphs, TANGO_GLYPH_SLOTS, t.clipped);
    CHECK(t.clipped == 0 && t.nglyphs <= TANGO_GLYPH_SLOTS);
    /* the wider budget (67, up from 29) plus the two-sided cut leave fit() pixel-width-bound, not
     * glyph-count-bound, here, so these counts come from the font metrics, not from TANGO_ROW_GLYPHS;
     * verified against this build's actual output rather than hand-computed (fonts are proportional) */
    CHECK(((t.rows[2 * 2] >> 9) & 127) == 61);                /* row 0: front+back cut, no value */
    CHECK(((t.rows[2 * 5] >> 9) & 127) == 65);                /* row 3: front+back cut + "26.09.21" value */
    CHECK(((t.rows[2 * 15] >> 9) & 127) == 15);               /* row 13: "Short (USA).nes" as it is */
    dump_layout("long_names", &p);
    /* 14 rows of 60-character names, the widest glyphs: never over the bank */
    static char wn[14][64]; static tango_row_t wr[14];
    for (int i = 0; i < 14; i++) { memset(wn[i], 'W', 60); wn[i][60] = 0; wr[i].kind = TR_FILE; wr[i].label = wn[i]; wr[i].value = NULL; }
    tango_page_t pw = { "NINTENDO NES", "Select", NULL, 0, 0, 0, wr, 14, LG, 3, 0 };
    CHECK(tango_layout_page(&pw, &t) == 0 && t.clipped == 0 && t.nglyphs <= TANGO_GLYPH_SLOTS);
    /* MiSTer's rows (PrintDirectory / ScrollText), dumped for the model: the selected file without a date shows its end on the row below ... */
    pw.sel = 3; CHECK(tango_layout_page(&pw, &t) == 0 && t.clipped == 0); dump_layout("list_expand", &pw);
    /* ... the window moves down when that row was the last one shown ... */
    static char wn2[15][64]; static tango_row_t wr2[15];
    for (int i = 0; i < 15; i++) { memset(wn2[i], 'W', 60); wn2[i][60] = 0; wr2[i].kind = TR_FILE; wr2[i].label = wn2[i]; }
    tango_page_t pl = { "NINTENDO NES", "Select", NULL, 0, 13, 0, wr2, 15, LG, 3, 0 };
    CHECK(tango_layout_page(&pl, &t) == 0 && t.clipped == 0); dump_layout("list_expand_last", &pl);
    /* ... and a dated name too long for its row scrolls in place from character 6 (sel_scroll 7), no marker */
    static char ln[160]; memset(ln, 'W', 150); ln[150] = 0;
    static tango_row_t sr[2] = { { TR_FILE, 0, ln, "26.09.21" }, { TR_FILE, 0, "Short", "26.09.21" } };
    tango_page_t ps = { "NINTENDO NES", "Cores", NULL, 0, 0, 0, sr, 2, LG, 3, 7 };
    CHECK(tango_layout_page(&ps, &t) == 0 && t.clipped == 0); dump_layout("list_scroll", &ps);
}

/* ---------------------------------------------------------------- Core Volume / Global Volume (tango_volume.c) */
static void test_volume(void)
{
    tango_vol_t v; char t[20];
    /* a core start: full volume (global 0, core 0), no filter: the byte of SYS 0x26 is the sum */
    tango_vol_init(&v, 0);
    CHECK(tango_vol_audvol(&v) == 0 && tango_vol_corebyte(&v) == 0);
    CHECK(tango_vol_global_step(&v, 1, 100) == 0 && v.vol == 0);                       /* louder than full: nothing moves */
    for (int i = 0; i < 9; i++) tango_vol_global_step(&v, -1, 100);
    CHECK(v.vol == 7);                                                                  /* 0..7, no further */
    CHECK(tango_vol_global_step(&v, 0, 100) == 1 && v.vol == 0x17);                     /* Select: mute, attenuation kept */
    CHECK(tango_vol_global_step(&v, -1, 100) == 1 && v.vol == 7);                       /* a step while muted only unmutes */
    CHECK(tango_vol_global_step(&v, 1, 100) == 1 && v.vol == 6);
    tango_vol_global_step(&v, 0, 100); tango_vol_global_step(&v, 1, 100); CHECK(v.vol == 6);   /* ... louder, too */
    /* core volume: attenuation 0..6, no boost without the filter */
    for (int i = 0; i < 9; i++) tango_vol_core_step(&v, -1, 100);
    CHECK((v.core & 7) == 6);
    for (int i = 0; i < 9; i++) tango_vol_core_step(&v, 1, 100);
    CHECK(v.core == 0);
    /* no filter: the sum is held at 7 and the clamp is written back into the global volume (audio.cpp 126-137) */
    v.vol = 7; v.core = 3;
    CHECK(tango_vol_audvol(&v) == 7 && v.vol == 4);
    v.vol = 2; v.core = 3; CHECK(tango_vol_audvol(&v) == 5 && v.vol == 2);              /* under 7: as it is */
    v.vol = 0x17; v.core = 6; CHECK(tango_vol_audvol(&v) == 0x17 + 6 && v.vol == 0x17); /* muted: no clamp, the sum carries the mute bit */
    /* the filter: 0x26 carries the global volume alone, the core byte goes with SET_AFILTER */
    tango_vol_caps(&v, 1, 1);
    v.vol = 5; v.core = 4; CHECK(tango_vol_audvol(&v) == 5 && tango_vol_corebyte(&v) == 4 && v.has_boost);
    v.vol = 0x13; CHECK(tango_vol_audvol(&v) == 0x13);
    /* boost: louder past attenuation 0 adds a step (bits 6:5), at most 2; quieter takes one off first, then attenuates */
    v.core = 1; tango_vol_core_step(&v, 1, 0); CHECK(v.core == 0);
    tango_vol_core_step(&v, 1, 0); CHECK(v.core == 0x20);
    tango_vol_core_step(&v, 1, 0); CHECK(v.core == 0x40);
    tango_vol_core_step(&v, 1, 0); CHECK(v.core == 0x40);
    tango_vol_core_step(&v, -1, 0); CHECK(v.core == 0x20);
    tango_vol_core_step(&v, -1, 0); CHECK(v.core == 0);
    tango_vol_core_step(&v, -1, 0); CHECK(v.core == 1);
    v.core = 0x23; tango_vol_core_step(&v, -1, 0); CHECK(v.core == 0);                  /* boost off resets the attenuation to 0 */
    /* a core without boost (or the menu core) drops boost bits; the menu core never has any */
    v.core = 0x42; tango_vol_caps(&v, 1, 0); CHECK(v.core == 2 && !v.has_boost);
    tango_vol_init(&v, 1); tango_vol_caps(&v, 1, 1); CHECK(!v.has_boost);
    /* load_volume: global & 0x17, core attenuation <= 6 and boost <= 2; the menu core never loads its core file; -1 = no file */
    tango_vol_init(&v, 0); tango_vol_load(&v, 0xFF, 0x7F); CHECK(v.vol == 0x17 && v.core == 0x46);
    tango_vol_load(&v, -1, -1); CHECK(v.vol == 0x17 && v.core == 0x46);
    tango_vol_load(&v, 0x03, 0x05); CHECK(v.vol == 3 && v.core == 5);
    tango_vol_init(&v, 1); tango_vol_load(&v, 2, 4); CHECK(v.vol == 2 && v.core == 0);
    CHECK(tango_vol_file_global(&v) == 2 && tango_vol_file_core(&v) == 0);
    /* the save timer: a second after the LAST change, once */
    tango_vol_init(&v, 0);
    CHECK(tango_vol_wait_ms(&v, 50) == -1 && !tango_vol_save_due(&v, 5000));
    tango_vol_global_step(&v, -1, 100);
    CHECK(tango_vol_wait_ms(&v, 100) == 1000 && tango_vol_wait_ms(&v, 600) == 500 && tango_vol_wait_ms(&v, 5000) == 0);
    CHECK(!tango_vol_save_due(&v, 1099) && tango_vol_save_due(&v, 1100) && !tango_vol_save_due(&v, 1101) && tango_vol_wait_ms(&v, 1101) == -1);
    tango_vol_core_step(&v, -1, 2000); tango_vol_global_step(&v, -1, 2500);
    CHECK(!tango_vol_save_due(&v, 3499) && tango_vol_save_due(&v, 3500));
    tango_vol_global_step(&v, 1, 4000);                                                  /* set_volume arms it even at the end of the range */
    tango_vol_init(&v, 0); tango_vol_global_step(&v, 1, 10); CHECK(v.pending);
    /* the text of the rows: filled cells = 8 - attenuation (0x84), empty 0x85, "+" per boost step */
    tango_vol_init(&v, 0);
    tango_vol_text(&v, 1, t); CHECK(!strcmp(t, "\x84\x84\x84\x84\x84\x84\x84\x84"));
    v.core = 6; tango_vol_text(&v, 1, t); CHECK(!strcmp(t, "\x84\x84\x85\x85\x85\x85\x85\x85"));
    v.core = 2; v.vol = 3;                                                              /* no filter: the global bar is 8 - 2 long */
    tango_vol_text(&v, 0, t); CHECK(!strcmp(t, "\x84\x84\x84\x85\x85\x85"));
    v.vol = 0x13; tango_vol_text(&v, 0, t); CHECK(!strcmp(t, "< Mute >"));
    tango_vol_caps(&v, 1, 1); v.vol = 1;                                                /* filter: the full 8 */
    tango_vol_text(&v, 0, t); CHECK(!strcmp(t, "\x84\x84\x84\x84\x84\x84\x84\x85"));
    v.core = 0x20; tango_vol_text(&v, 1, t); CHECK(!strcmp(t, "\x84\x84\x84\x84\x84\x84\x84\x84+"));
    v.core = 0x42; tango_vol_text(&v, 1, t); CHECK(!strcmp(t, "\x84\x84\x84\x84\x84\x84\x85\x85++"));
    /* the link byte */
    rec_link_t r; tango_link_t link; rec_link_init(&r, &link);
    tango_sys_audvol(&link, 0x17); CHECK(r.audvol == 0x17 && r.audvol_writes == 1 && r.buf[0] == 0 && r.buf[1] == 0x26);
}

/* Left on page 0 of a core: Misc. Options (menu.cpp 4030-4165, 2640-2646) */
static void test_misc(void)
{
    static tango_coremenu_t cm; static tango_osd_t osd; static tango_browser_t br;
    static rec_link_t r; tango_link_t link; fake_card_t card; tango_fs_t fs; tango_page_t p;
    static tango_vol_t vol;
    rec_link_init(&r, &link); r.confstr = TEMPLATE_CONF; r.menumask = 0;
    fake_card_init(&card, &fs, CM_CARD, (int)(sizeof CM_CARD / sizeof CM_CARD[0]));
    tango_vol_init(&vol, 0); tango_vol_caps(&vol, 1, 1); tango_vol_load(&vol, 1, 0x20);        /* a core with the audio filter and boost */
    tango_core_platform_t plat = { .fs = &fs, .load_core = cm_load_core, .reboot = cm_reboot, .load_file = cm_load_file,
                                   .save_cfg = cm_save, .dir_exists = cm_dir_exists, .vol = &vol, .vol_apply = fake_vol_apply };
    static const uint8_t PAL[16][3] = TANGO_PAL_INIT;
    static char conf[TANGO_CONF_MAX];
    tango_sys_read_confstr(&link, conf, sizeof conf);
    tango_osd_probe(&osd, &link); tango_osd_start(&osd, PAL);
    tango_core_init(&cm, &link, &osd, &br, &plat, conf, NULL);
    tango_core_toggle(&cm);
    CHECK(tango_core_event(&cm, TANGO_EV_LEFT) == TANGO_CM_REDRAWN && cm.mode == TANGO_CM_MISC);
    tango_core_page(&cm, &p);
    CHECKS(p.title, "Misc. Options"); CHECK(p.arrow == TANGO_ARROW_RIGHT); CHECK(p.nrows == TANGO_VIS && p.sel == 13);
    CHECK(row_is(&p, 1, TR_CENTER, "Information", 0) && row_is(&p, 2, TR_CENTER, "HDMI 1280x720", 0));
    CHECK(p.rows[7].kind == TR_VOL && !strcmp(p.rows[7].label, "Core Volume") && p.rows[8].kind == TR_VOL && !strcmp(p.rows[8].label, "Global Volume"));
    CHECK(p.nlegend == 3 && !strcmp(p.legend[1].label, "select") && !strcmp(p.legend[2].label, "close"));
    dump_layout("cm_misc", &p);
    /* Up: Global Volume (the blank rows are skipped), Core Volume */
    vol_applies = 0;
    CHECK(tango_core_event(&cm, TANGO_EV_UP) == TANGO_CM_REDRAWN && cm.sel == 8);
    CHECK(tango_core_event(&cm, TANGO_EV_LEFT) == TANGO_CM_REDRAWN && vol_applies == 1 && vol_apply_core == 0 && vol.vol == 2);
    CHECK(tango_core_event(&cm, TANGO_EV_PLUS) == TANGO_CM_REDRAWN && vol.vol == 1);
    CHECK(tango_core_event(&cm, TANGO_EV_RIGHT) == TANGO_CM_REDRAWN && vol.vol == 0 && cm.mode == TANGO_CM_MISC);   /* Right = louder here */
    CHECK(tango_core_event(&cm, TANGO_EV_SELECT) == TANGO_CM_REDRAWN && (vol.vol & 0x10));
    tango_core_page(&cm, &p); CHECK(p.rows[8].kind == TR_OPT); CHECKS(p.rows[8].value, "< Mute >");
    CHECK(tango_core_event(&cm, TANGO_EV_MINUS) == TANGO_CM_REDRAWN && !(vol.vol & 0x10));
    CHECK(tango_core_event(&cm, TANGO_EV_UP) == TANGO_CM_REDRAWN && cm.sel == 7);
    vol_applies = 0;
    CHECK(tango_core_event(&cm, TANGO_EV_SELECT) == TANGO_CM_NOTHING && vol_applies == 0);       /* Select on Core Volume: nothing */
    CHECK(tango_core_event(&cm, TANGO_EV_LEFT) == TANGO_CM_REDRAWN && vol_applies == 1 && vol_apply_core == 1 && vol.core == 0);   /* the boost step goes first */
    /* exit: Right goes back to page 0; Select closes; B closes; Menu closes */
    CHECK(tango_core_event(&cm, TANGO_EV_UP) == TANGO_CM_REDRAWN && cm.sel == 13);
    CHECK(tango_core_event(&cm, TANGO_EV_RIGHT) == TANGO_CM_REDRAWN && cm.mode == TANGO_CM_PAGE && cm.page == 0);
    tango_core_event(&cm, TANGO_EV_LEFT); CHECK(cm.mode == TANGO_CM_MISC);
    CHECK(tango_core_event(&cm, TANGO_EV_BACK) == TANGO_CM_CLOSED_NOW && cm.mode == TANGO_CM_CLOSED);        /* B closes, as MiSTer's */
    tango_core_toggle(&cm); tango_core_event(&cm, TANGO_EV_LEFT);
    CHECK(tango_core_event(&cm, TANGO_EV_SELECT) == TANGO_CM_CLOSED_NOW && cm.mode == TANGO_CM_CLOSED);
    tango_core_toggle(&cm); tango_core_event(&cm, TANGO_EV_LEFT);
    CHECK(tango_core_event(&cm, TANGO_EV_MENU) == TANGO_CM_CLOSED_NOW);
    /* a sub-page's Left is still "back", and Left on an unknown row of Misc. does nothing */
    tango_core_toggle(&cm); tango_core_event(&cm, TANGO_EV_LEFT);
    CHECK(tango_core_event(&cm, TANGO_EV_LEFT) == TANGO_CM_NOTHING);                          /* exit row */
    /* no volume state in the platform: no volume rows */
    plat.vol = NULL; tango_core_init(&cm, &link, &osd, &br, &plat, conf, NULL);
    tango_core_toggle(&cm); tango_core_event(&cm, TANGO_EV_LEFT); tango_core_page(&cm, &p);
    CHECK(p.rows[7].kind == TR_BLANK && p.rows[8].kind == TR_BLANK);
}

/* ---------------------------------------------------------------- file-list keys: Home, End, Tab, Backspace, plus / minus */
/* Misc. Options' Information block (tango_about.h tango_info_lines): only what the BL616 knows */
static uint8_t info_vs; static int info_vs_calls; static const char *info_ip; static int info_ip_calls;
static uint8_t fake_vsync(void *ctx) { (void)ctx; info_vs_calls++; return info_vs; }
static int fake_ip(void *ctx, char *buf, int cap) { (void)ctx; info_ip_calls++; if (!info_ip) { buf[0] = 0; return 0; } snprintf(buf, (size_t)cap, "%s", info_ip); return (int)strlen(buf); }
static void test_info(const char *dir)
{
    (void)dir;
    char l[2][64];
    /* the lines: only what the BL616 knows -- never a pixel clock or a rate */
    CHECK(tango_info_lines(l, 0, NULL) == 1); CHECKS(l[0], "HDMI 1280x720");                                   /* no 0x70 block */
    CHECK(tango_info_lines(l, 0x20, NULL) == 1); CHECKS(l[0], "HDMI 1280x720");                                /* bit 7 clear: not a reply, the flags mean nothing */
    CHECK(tango_info_lines(l, 0x80, "") == 1); CHECKS(l[0], "HDMI 1280x720  fixed rate");                     /* a block, the user chose fixed */
    CHECK(tango_info_lines(l, 0xC0, NULL) == 1); CHECKS(l[0], "HDMI 1280x720  native rate, no plan yet");     /* native chosen, no plan in use */
    CHECK(tango_info_lines(l, 0xE2, NULL) == 1); CHECKS(l[0], "HDMI 1280x720  native rate");                  /* a plan in use */
    CHECK(tango_info_lines(l, 0xA0, NULL) == 1); CHECKS(l[0], "HDMI 1280x720  native rate");
    CHECK(tango_info_lines(l, 0xE6, NULL) == 1); CHECKS(l[0], "HDMI 1280x720  retuning ...");                  /* a retune pending / running wins */
    CHECK(tango_info_lines(l, 0, "192.0.2.10") == 2); CHECKS(l[0], "IP: 192.0.2.10"); CHECKS(l[1], "HDMI 1280x720");

    /* the launcher: the callbacks run once, as Misc. Options opens (never while a volume row is stepped), the IP line appears only with an address */
    rec_link_t r; tango_link_t link; fake_card_t c; tango_fs_t fs; tango_page_t p;
    tango_platform_t plat = { .fs = &fs, .load_core = fake_load, .free_mb = fake_free_mb, .reboot = fake_reboot_cb, .vsync = fake_vsync, .ip = fake_ip };
    static tango_vol_t vol; tango_vol_init(&vol, 0); tango_vol_load(&vol, 3, 2); tango_vol_caps(&vol, 0, 0);
    plat.vol = &vol; plat.vol_apply = fake_vol_apply;
    rec_link_init(&r, &link); fake_card_init(&c, &fs, CARD, NCARD);
    CHECK(tango_launcher_detect(&L, &link) == 1);
    info_vs = 0xE2; info_vs_calls = 0; info_ip = NULL; info_ip_calls = 0;
    tango_launcher_start(&L, &plat); tango_launcher_set_info(&L, FWID, MENU_CONF);
    tango_launcher_event(&L, TANGO_EV_MENU); CHECK(info_vs_calls == 0);                                            /* System Settings: not asked */
    tango_launcher_event(&L, TANGO_EV_LEFT); CHECK(L.mode == TL_MISC && info_vs_calls == 1);
    tango_launcher_page(&L, &p);
    CHECK(row_k(&p, 2, TR_CENTER, "HDMI 1280x720  native rate")); CHECK(row_k(&p, 3, TR_BLANK, 0) && row_k(&p, 4, TR_BLANK, 0)); CHECK(row_k(&p, 5, TR_DIS, "Config"));
    CHECK(row_k(&p, 7, TR_VOL, "Core Volume") && row_k(&p, 13, TR_ACT, "exit"));
    dump_layout("05e_misc_options_native", &p);
    tango_launcher_event(&L, TANGO_EV_UP); tango_launcher_event(&L, TANGO_EV_UP); tango_launcher_event(&L, TANGO_EV_UP);       /* Global Volume */
    tango_launcher_event(&L, TANGO_EV_LEFT); tango_launcher_event(&L, TANGO_EV_RIGHT);
    CHECK(L.mode == TL_MISC && info_vs_calls == 1);                                                                 /* asked once */
    tango_launcher_event(&L, TANGO_EV_MENU); tango_launcher_event(&L, TANGO_EV_MENU); tango_launcher_event(&L, TANGO_EV_LEFT);
    CHECK(info_vs_calls == 2);                                                                                      /* the next visit asks again */
    info_vs = 0; info_ip = "192.0.2.10";
    tango_launcher_event(&L, TANGO_EV_RIGHT); tango_launcher_event(&L, TANGO_EV_LEFT);                             /* Misc -> System -> Misc */
    tango_launcher_page(&L, &p);
    CHECK(row_k(&p, 2, TR_CENTER, "IP: 192.0.2.10")); CHECK(row_k(&p, 3, TR_CENTER, "HDMI 1280x720")); CHECK(row_k(&p, 4, TR_BLANK, 0));
    CHECK(row_k(&p, 5, TR_DIS, "Config")); CHECK(row_k(&p, 7, TR_VOL, "Core Volume"));
    dump_layout("05d_misc_options_ip", &p);
    /* a platform with no callbacks: the fixed raster, no IP */
    { tango_platform_t p2 = plat; p2.vsync = NULL; p2.ip = NULL; tango_launcher_start(&L, &p2); tango_launcher_set_info(&L, FWID, MENU_CONF);
      tango_launcher_event(&L, TANGO_EV_MENU); tango_launcher_event(&L, TANGO_EV_LEFT); tango_launcher_page(&L, &p);
      CHECK(row_k(&p, 2, TR_CENTER, "HDMI 1280x720")); CHECK(row_k(&p, 3, TR_BLANK, 0)); }

    /* the launcher's About: from System Settings, the cursor on back, the ticker moves, back returns with the cursor on About */
    tango_launcher_start(&L, &plat); tango_launcher_set_info(&L, FWID, MENU_CONF);
    tango_launcher_tick(&L, 5000);
    tango_launcher_event(&L, TANGO_EV_MENU);
    tango_launcher_event(&L, TANGO_EV_UP); tango_launcher_event(&L, TANGO_EV_UP);                                  /* Reboot, About */
    CHECK(L.sel[TL_SYSTEM] == 9);
    CHECK(tango_launcher_event(&L, TANGO_EV_SELECT) == 1 && L.mode == TL_ABOUT);
    tango_launcher_page(&L, &p);
    about_rows_check(&p, "Tango FPGA v261012 (g0f1e2d3-dirty)", "Menu core v260928", TANGO_VX - TANGO_TX);
    CHECK(tango_launcher_wait_ms(&L, 5000) == 60 && tango_launcher_wait_ms(&L, 5059) == 1);
    CHECK(tango_launcher_tick(&L, 5059) == 0);
    CHECK(tango_launcher_tick(&L, 5060) == 1);
    tango_launcher_page(&L, &p); CHECK(p.scroll == TANGO_VX - TANGO_TX + 3);
    dump_layout("l_about_step1", &p);
    CHECK(tango_launcher_event(&L, TANGO_EV_UP) == 1 && L.mode == TL_ABOUT);                                        /* no cursor: nothing moves */
    CHECK(tango_launcher_event(&L, TANGO_EV_SELECT) == 1 && L.mode == TL_SYSTEM && L.sel[TL_SYSTEM] == 9);
    tango_launcher_event(&L, TANGO_EV_SELECT); CHECK(L.mode == TL_ABOUT);
    CHECK(tango_launcher_event(&L, TANGO_EV_LEFT) == 1 && L.mode == TL_SYSTEM && L.sel[TL_SYSTEM] == 9);
    tango_launcher_event(&L, TANGO_EV_SELECT); CHECK(tango_launcher_event(&L, TANGO_EV_BACK) == 1 && L.mode == TL_SYSTEM);
    tango_launcher_event(&L, TANGO_EV_SELECT); CHECK(tango_launcher_event(&L, TANGO_EV_MENU) == 1 && L.mode == TL_SYSTEM);
    CHECK(tango_launcher_wait_ms(&L, 6000) == -1);                                                                 /* nothing ticks outside About */

    /* a core's Misc. Options: the same lines, asked when Left opens the page */
    { static tango_coremenu_t cm; static tango_osd_t osd; static tango_browser_t br; static char conf[TANGO_CONF_MAX]; fake_card_t card; tango_fs_t cfs;
      static const uint8_t PAL[16][3] = TANGO_PAL_INIT;
      rec_link_t rr; tango_link_t cl; rec_link_init(&rr, &cl); rr.confstr = TEMPLATE_CONF; rr.menumask = 0;
      fake_card_init(&card, &cfs, CM_CARD, (int)(sizeof CM_CARD / sizeof CM_CARD[0]));
      tango_core_platform_t cp = { .fs = &cfs, .load_core = cm_load_core, .reboot = cm_reboot, .load_file = cm_load_file, .save_cfg = cm_save,
                                   .dir_exists = cm_dir_exists, .vol = &vol, .vol_apply = fake_vol_apply, .vsync = fake_vsync, .ip = fake_ip };
      tango_sys_read_confstr(&cl, conf, sizeof conf); tango_osd_probe(&osd, &cl); tango_osd_start(&osd, PAL);
      tango_core_init(&cm, &cl, &osd, &br, &cp, conf, NULL); tango_core_toggle(&cm);
      info_vs = 0xA0; info_ip = NULL; info_vs_calls = 0;
      tango_core_event(&cm, TANGO_EV_LEFT); CHECK(cm.mode == TANGO_CM_MISC && info_vs_calls == 1);
      tango_core_page(&cm, &p);
      CHECK(row_is(&p, 2, TR_CENTER, "HDMI 1280x720  native rate", 0)); CHECK(row_is(&p, 3, TR_BLANK, 0, 0));
      CHECK(p.rows[7].kind == TR_VOL && p.rows[13].kind == TR_ACT);
      tango_core_event(&cm, TANGO_EV_UP); tango_core_event(&cm, TANGO_EV_LEFT); CHECK(info_vs_calls == 1);          /* a volume step does not ask again */
      info_ip = "10.0.0.7"; tango_core_event(&cm, TANGO_EV_BACK); tango_core_toggle(&cm); tango_core_event(&cm, TANGO_EV_LEFT);
      tango_core_page(&cm, &p);
      CHECK(row_is(&p, 2, TR_CENTER, "IP: 10.0.0.7", 0)); CHECK(row_is(&p, 3, TR_CENTER, "HDMI 1280x720  native rate", 0));
      CHECK(p.rows[7].kind == TR_VOL && p.rows[7].label && !strcmp(p.rows[7].label, "Core Volume"));
      cm_dump("cm_misc_ip", &cm); }
}

static void test_list_keys(void)
{
    static const char *K[] = { "_Dir1/", "_Dir2/", "Aa.BIN", "Ab.BIN", "Ba.BIN", "Bb.BIN", "Bc.BIN", "Ca.BIN",
                               "_Dir1/Zed.BIN", "_Dir1/Yak.BIN" };
    static tango_launcher_t LL; static rec_link_t r; tango_link_t link; fake_card_t c; tango_fs_t fs;
    tango_platform_t plat = { .fs = &fs, .load_core = fake_load };
    rec_link_init(&r, &link); fake_card_init(&c, &fs, K, N(K));
    CHECK(tango_launcher_detect(&LL, &link));
    tango_launcher_start(&LL, &plat);
    tango_browser_t *b = &LL.br;
    CHECK(b->n == 8 && b->sel == 0);                                            /* _Dir1 _Dir2 Aa Ab Ba Bb Bc Ca */
    /* plus / minus: the next initial letter (folders and files apart; a Cores folder's "_" skipped) -- file_io.cpp 1953-2000 */
    CHECK(tango_launcher_event(&LL, TANGO_EV_PLUS) && b->sel == 2);             /* Dir -> the first file (the type changes) */
    CHECK(tango_launcher_event(&LL, TANGO_EV_PLUS) && b->sel == 4);             /* A -> B */
    CHECK(tango_launcher_event(&LL, TANGO_EV_PLUS) && b->sel == 7);             /* B -> C */
    CHECK(tango_launcher_event(&LL, TANGO_EV_PLUS) && b->sel == 7);             /* the end: nothing moves */
    CHECK(tango_launcher_event(&LL, TANGO_EV_MINUS) && b->sel == 4);            /* C -> the FIRST of the previous letter */
    CHECK(tango_launcher_event(&LL, TANGO_EV_MINUS) && b->sel == 2);            /* B -> the first A */
    CHECK(tango_launcher_event(&LL, TANGO_EV_MINUS) && b->sel == 1);            /* as MiSTer: folders count as a type change at every step */
    CHECK(tango_launcher_event(&LL, TANGO_EV_MINUS) && b->sel == 1);
    /* End / Home */
    CHECK(tango_launcher_event(&LL, TANGO_EV_END) && b->sel == 7);
    CHECK(tango_launcher_event(&LL, TANGO_EV_HOME) && b->sel == 0);
    /* Tab: in a folder, up (the folder left is selected); at the root, the top */
    tango_browser_select(b, 1); tango_launcher_event(&LL, TANGO_EV_SELECT);     /* into _Dir2: empty */
    CHECKS(b->path, "_Dir2");
    CHECK(tango_launcher_event(&LL, TANGO_EV_TAB) && b->path[0] == 0 && b->sel == 1);
    tango_browser_select(b, 0); tango_launcher_event(&LL, TANGO_EV_SELECT);
    CHECKS(b->path, "_Dir1"); tango_browser_select(b, 2);
    CHECK(tango_launcher_event(&LL, TANGO_EV_HOME) && b->sel == 0 && b->path[0]);   /* Home: the top of this folder only */
    tango_browser_select(b, 2);
    CHECK(tango_launcher_event(&LL, TANGO_EV_TAB) && b->path[0] == 0 && b->sel == 0);
    tango_browser_select(b, 4);
    CHECK(tango_launcher_event(&LL, TANGO_EV_TAB) && b->path[0] == 0 && b->sel == 0);   /* the root: Tab = Home */
    /* a typed filter: B and A+B (Backspace) take it away; B with none goes up */
    CHECK(tango_launcher_type(&LL, 'b', 1000) && b->sel == 4 && !b->filtered);      /* the first letter jumps */
    CHECK(tango_launcher_type(&LL, 'b', 1500) && b->filtered && b->n == 1);          /* "bb" within 2 s: the list shrinks to Bb */
    CHECK(tango_launcher_event(&LL, TANGO_EV_BACK) && !b->filtered && b->n == 8 && b->sel == 0 && b->path[0] == 0 && LL.mode == TL_CORES);
    CHECK(tango_launcher_type(&LL, 'b', 5000) && tango_launcher_type(&LL, 'a', 5100) && b->filtered);
    CHECK(tango_launcher_event(&LL, TANGO_EV_BACKSPACE) && !b->filtered && b->n == 8 && b->sel == 0);
    tango_browser_select(b, 3);
    CHECK(tango_launcher_event(&LL, TANGO_EV_BACKSPACE) && b->sel == 0);            /* no filter: MiSTer rescans from the top as well */
    /* the recents key has no page yet: nothing happens, and nothing is drawn */
    CHECK(tango_launcher_event(&LL, TANGO_EV_RECENT) == 0);
    /* the keys do nothing on System Settings and Misc. Options */
    tango_launcher_event(&LL, TANGO_EV_MENU); CHECK(LL.mode == TL_SYSTEM);
    CHECK(tango_launcher_event(&LL, TANGO_EV_TAB) == 0 && tango_launcher_event(&LL, TANGO_EV_HOME) == 0 &&
          tango_launcher_event(&LL, TANGO_EV_END) == 0 && tango_launcher_event(&LL, TANGO_EV_PLUS) == 0 && tango_launcher_event(&LL, TANGO_EV_BACKSPACE) == 0);
    CHECK(tango_launcher_repeats(&LL, TANGO_EV_LEFT) == 0 && tango_launcher_repeats(&LL, TANGO_EV_DOWN) == 1);
    tango_launcher_event(&LL, TANGO_EV_MENU);
    CHECK(tango_launcher_repeats(&LL, TANGO_EV_LEFT) == 1 && tango_launcher_repeats(&LL, TANGO_EV_PLUS) == 1);
    /* the keyboard's keys */
    CHECK(tango_kbd_menu_key(0x4A, 1, 1) == TKEY_HOME && tango_kbd_menu_key(0x4D, 1, 1) == TKEY_END && tango_kbd_menu_key(0x2B, 1, 1) == TKEY_TAB);
    CHECK(tango_kbd_menu_key(0x35, 1, 1) == TKEY_RECENT && tango_kbd_menu_key(0x4A, 0, 1) == TKEY_NONE && tango_kbd_menu_key(0x2B, 0, 1) == TKEY_NONE);
    CHECK(tango_kbd_menu_key(0x2D, 1, 1) == TKEY_MINUS && tango_kbd_menu_key(0x2E, 1, 1) == TKEY_PLUS);      /* - and = */
    CHECK(tango_kbd_menu_key(0x56, 1, 1) == TKEY_MINUS && tango_kbd_menu_key(0x57, 1, 1) == TKEY_PLUS);      /* keypad */
}

/* ---------------------------------------------------------------- options stepped both ways; a core's file list keys */
static void test_core_keys(void)
{
    static tango_coremenu_t cm; static tango_osd_t osd; static tango_browser_t br;
    static rec_link_t r; tango_link_t link; fake_card_t card; tango_fs_t fs; tango_page_t p;
    rec_link_init(&r, &link); r.confstr = TEMPLATE_CONF; r.menumask = 0;
    fake_card_init(&card, &fs, CM_CARD, (int)(sizeof CM_CARD / sizeof CM_CARD[0]));
    tango_core_platform_t plat = { .fs = &fs, .load_core = cm_load_core, .reboot = cm_reboot, .load_file = cm_load_file,
                                   .save_cfg = cm_save, .dir_exists = cm_dir_exists };
    static const uint8_t PAL[16][3] = TANGO_PAL_INIT;
    static char conf[TANGO_CONF_MAX];
    tango_sys_read_confstr(&link, conf, sizeof conf);
    tango_osd_probe(&osd, &link); tango_osd_start(&osd, PAL);
    tango_core_init(&cm, &link, &osd, &br, &plat, conf, NULL);
    tango_core_toggle(&cm);
    /* page 0: Aspect ratio (row 1), TV Mode (2), Noise (3).  plus = Select = the next value; minus = the previous one (menu.cpp 2535-2570) */
    CHECK(cm.sel == 1);
    CHECK(tango_core_event(&cm, TANGO_EV_PLUS) == TANGO_CM_REDRAWN && r.status[15] == 0x02);          /* Original -> Full Screen */
    CHECK(tango_core_event(&cm, TANGO_EV_PLUS) == TANGO_CM_REDRAWN && r.status[15] == 0x00);          /* [ARC1] is empty for Tango: wraps to 0 */
    CHECK(tango_core_event(&cm, TANGO_EV_MINUS) == TANGO_CM_REDRAWN && r.status[15] == 0x02);         /* back from 0: [ARC2], [ARC1] skipped */
    CHECK(tango_core_event(&cm, TANGO_EV_MINUS) == TANGO_CM_REDRAWN && r.status[15] == 0x00);
    tango_core_event(&cm, TANGO_EV_DOWN);                                                             /* TV Mode: NTSC PAL */
    CHECK(tango_core_event(&cm, TANGO_EV_MINUS) == TANGO_CM_REDRAWN && r.status[0] == 0x04);          /* NTSC -> PAL (wraps) */
    tango_core_page(&cm, &p); CHECK(row_is(&p, 2, TR_OPT, "TV Mode", "PAL"));
    CHECK(tango_core_event(&cm, TANGO_EV_PLUS) == TANGO_CM_REDRAWN && r.status[0] == 0x00);
    tango_core_event(&cm, TANGO_EV_DOWN);                                                             /* Noise [4:3]: White Red Green Blue */
    CHECK(tango_core_event(&cm, TANGO_EV_MINUS) == TANGO_CM_REDRAWN && r.status[0] == 0x18);          /* White -> Blue */
    tango_core_page(&cm, &p); CHECK(row_is(&p, 3, TR_OPT, "Noise", "Blue"));
    CHECK(tango_core_event(&cm, TANGO_EV_MINUS) == TANGO_CM_REDRAWN && r.status[0] == 0x10);          /* Blue -> Green */
    CHECK(tango_core_event(&cm, TANGO_EV_PLUS) == TANGO_CM_REDRAWN && tango_core_event(&cm, TANGO_EV_PLUS) == TANGO_CM_REDRAWN && r.status[0] == 0x00);
    /* rows that are not options ignore them: a page link, a trigger */
    while (cm.sel != 5) tango_core_event(&cm, TANGO_EV_DOWN);
    CHECK(tango_core_event(&cm, TANGO_EV_PLUS) == TANGO_CM_NOTHING && cm.page == 0);
    while (cm.sel != 10) tango_core_event(&cm, TANGO_EV_DOWN);
    r.status_writes = 0;
    CHECK(tango_core_event(&cm, TANGO_EV_MINUS) == TANGO_CM_NOTHING && r.status_writes == 0);
    /* the keys of a core's file list (a Cores list from System > Core): Tab goes up, End / Home, plus / minus the letters */
    tango_core_event(&cm, TANGO_EV_RIGHT); CHECK(cm.mode == TANGO_CM_SYSTEM && cm.sel == 0);
    tango_core_event(&cm, TANGO_EV_SELECT); CHECK(cm.mode == TANGO_CM_CORES);
    CHECK(tango_core_repeats(&cm, TANGO_EV_PLUS) == 1 && tango_core_repeats(&cm, TANGO_EV_SELECT) == 0);
    int n = br.n;
    tango_core_page(&cm, &p);
    CHECK(tango_core_wait_ms(&cm, 0) == -1 && tango_core_tick(&cm, 5000) == TANGO_CM_NOTHING);   /* a short name: nothing scrolls */
    tango_core_event(&cm, TANGO_EV_DOWN); tango_core_event(&cm, TANGO_EV_UP);
    CHECK(tango_core_event(&cm, TANGO_EV_END) == TANGO_CM_REDRAWN && br.sel == n - 1);
    CHECK(tango_core_event(&cm, TANGO_EV_HOME) == TANGO_CM_REDRAWN && br.sel == 0);
    CHECK(tango_core_event(&cm, TANGO_EV_TAB) == TANGO_CM_REDRAWN && br.sel == 0);
    CHECK(tango_core_event(&cm, TANGO_EV_PLUS) == TANGO_CM_REDRAWN);
    CHECK(tango_core_event(&cm, TANGO_EV_BACKSPACE) == TANGO_CM_REDRAWN && br.sel == 0);
    CHECK(tango_core_event(&cm, TANGO_EV_RECENT) == TANGO_CM_NOTHING);
    tango_core_event(&cm, TANGO_EV_BACK); CHECK(cm.mode == TANGO_CM_SYSTEM);
    CHECK(tango_core_repeats(&cm, TANGO_EV_LEFT) == 0 && tango_core_repeats(&cm, TANGO_EV_UP) == 1);
}

/* ---------------------------------------------------------------- the detail line: full name, size, date */
static void test_detail(void)      /* the long-name rules: MiSTer's PrintDirectory / ScrollLongName, no detail line */
{
    static const char *K[] = { "Alpha.BIN", "A very long No-Intro style name that does not fit on one line at all (USA) (Rev A) (Proto).BIN",
                               "Short.BIN", "_Dir1/", "_Dir1/Inner.BIN",
                               "_Core Long Name That Does Not Fit In One Row Of The Cores List At All Really_20260921.bin" };
    static tango_launcher_t LD; static rec_link_t r; tango_link_t link; fake_card_t c; tango_fs_t fs; tango_page_t p;
    tango_platform_t plat = { .fs = &fs, .load_core = fake_load };
    rec_link_init(&r, &link); fake_card_init(&c, &fs, K, N(K));
    CHECK(tango_launcher_detect(&LD, &link));
    tango_launcher_start(&LD, &plat);                      /* [_Dir1] [A very long...] [Alpha] [Short]; the "_Core" is a core in the root */
    tango_browser_t *b = &LD.br;
    int nlong = -1;
    for (int i = 0; i < b->n; i++) if (b->e[b->order[i]].is_dir == 0 && strstr(tango_browser_alt(b, &b->e[b->order[i]]), "Core Long")) nlong = i;
    CHECK(b->n >= 4);
    /* no detail line on any page */
    tango_launcher_page(&LD, &p); CHECK(p.sel_scroll == 0);
    int nvl = -1;
    for (int i = 0; i < b->n; i++) if (!strncmp(tango_browser_alt(b, &b->e[b->order[i]]), "A very long", 11)) nvl = i;
    CHECK(nvl >= 0);
    tango_browser_select(b, nvl);
    /* a file with no date that does not fit: trimmed at its END with a marker; its end on the row below when selected (browse_expand) */
    tango_launcher_page(&LD, &p);
    const char *lab = p.rows[nvl].label;
    CHECK(strstr(lab, "A very long") == lab);
    tango_tables_t t1; static tango_tables_t t0;
    CHECK(tango_layout_page(&p, &t1) == 0 && t1.clipped == 0);
    CHECK(p.rows[nvl].value != NULL);                      /* a core always has its date text ("--.--.--" without a date code): a dated row never expands */
    CHECK(tango_list_row_scrolls(&p.rows[nvl]) == 1 && b->scroll_ok == 1);
    { tango_row_t nd = { TR_FILE, 0, "A very long No-Intro style name that does not fit on one line at all (USA) (Rev A) (Proto)", NULL };
      CHECK(tango_list_row_scrolls(&nd) == 0);             /* a file picker's row (no date): the two rows hold the whole name, nothing scrolls */ }
    /* a name that two rows do not hold scrolls: after 1 s, a character every step, over the name and 10 blanks */
    static char vlong[400]; memset(vlong, 'm', 300); vlong[300] = 0;
    tango_row_t vr = { TR_FILE, 0, vlong, NULL };
    CHECK(tango_list_row_scrolls(&vr) == 1);
    vr.value = "26.09.21"; vr.label = "A name too long for a row that has a date on its right side, so it scrolls";
    CHECK(tango_list_row_scrolls(&vr) == 1);               /* a dated row never expands */
    vr.label = "Short"; CHECK(tango_list_row_scrolls(&vr) == 0);
    tango_row_t dr = { TR_DIR, 0, "A folder name much too long for one row of the cores list, even with the room the chevron leaves free", NULL };
    CHECK(tango_list_row_scrolls(&dr) == 1);
    /* the launcher's own scroll: a core with a long name (dated) */
    if (nlong >= 0) {
        tango_browser_select(b, nlong);
        tango_launcher_page(&LD, &p);
        CHECK(b->scroll_ok == 1 && p.sel_scroll == 0);
        CHECK(tango_launcher_wait_ms(&LD, 0) == 0 && tango_launcher_tick(&LD, 5000) == 0);   /* the new selection is stamped by the first tick */
        CHECK(tango_launcher_wait_ms(&LD, 5000) == 1000 && tango_launcher_tick(&LD, 5999) == 0);
        long t0x = r.transfers;
        CHECK(tango_launcher_tick(&LD, 6000) == 1 && b->scroll_act && b->scroll_off == 0 && r.transfers > t0x);   /* it starts: unmarked text from character 0 */
        tango_launcher_page(&LD, &p); CHECK(p.sel_scroll == 1);
        CHECK(tango_launcher_wait_ms(&LD, 6000) == 120 && tango_launcher_tick(&LD, 6119) == 0 && tango_launcher_tick(&LD, 6120) == 1 && b->scroll_off == 1);
        tango_launcher_page(&LD, &p); CHECK(p.sel_scroll == 2);
        CHECK(tango_layout_page(&p, &t0) == 0 && t0.clipped == 0);
        unsigned now = 6120; int steps = 1, wrapped = 0;
        while (steps < 400 && !wrapped) { now += 120; tango_launcher_tick(&LD, now); steps++; if (b->scroll_off == 0) wrapped = 1; }
        CHECK(wrapped && steps == b->scroll_len + BLANKSPACE_T);         /* name + 10 blanks, then again (ScrollText 641) */
        /* a new selection starts over: trimmed again, the delay restarts */
        tango_launcher_event(&LD, TANGO_EV_UP);
        CHECK(!b->scroll_act && b->scroll_off == 0);
    }
    /* a short name: nothing scrolls, nothing waits */
    tango_browser_select(b, 0); tango_launcher_page(&LD, &p);
    CHECK(b->scroll_ok == 0 && tango_launcher_wait_ms(&LD, 99999) == -1 && tango_launcher_tick(&LD, 99999) == 0);
    /* ".." and an empty list */
    tango_browser_chdir(b, &fs, "_Dir1"); tango_launcher_page(&LD, &p); CHECK(b->scroll_ok == 0);
    { static const char *NONE[] = { "x" }; fake_card_t c2; tango_fs_t fs3; fake_card_init(&c2, &fs3, NONE, 0);
      tango_platform_t p3 = plat; p3.fs = &fs3; tango_launcher_start(&LD, &p3); tango_launcher_page(&LD, &p);
      CHECK(tango_launcher_wait_ms(&LD, 0) == -1); }
    /* the fullest page: 14 rows of 67-glyph names with their dates still fit one bank unclipped */
    {
        static tango_row_t gr[14]; static char nm[14][70];
        for (int i = 0; i < 14; i++) { memset(nm[i], 'i', 66); nm[i][66] = 0; gr[i].kind = TR_FILE; gr[i].label = nm[i]; gr[i].value = "26.09.21"; }
        static const tango_legend_t lg[] = { { "A", "open" }, { "Menu", "settings" }, { "\x83\x7f", "page" } };
        tango_page_t pw = { "NINTENDO NES", "Cores", "123/456", 0, 3, 0, gr, 14, lg, 3, 0 };
        static tango_tables_t tw; CHECK(tango_layout_page(&pw, &tw) == 0);
        CHECK(tw.clipped == 0 && tw.nglyphs <= TANGO_GLYPH_SLOTS);
        printf("detail: the fullest page (14 x 66 'i' + date) = %d of %d glyph entries, %d clipped\n", tw.nglyphs, TANGO_GLYPH_SLOTS, tw.clipped);
        /* a selected file with no date and a long name: its second line takes one row of the 14 (and the window moves when it was the last one) */
        static char big[14][100]; static tango_row_t ex[14];
        for (int i = 0; i < 14; i++) { snprintf(big[i], sizeof big[i], "Game number %02d with a rather long name that will not fit (USA) (Rev A)", i); ex[i].kind = TR_FILE; ex[i].label = big[i]; }
        tango_page_t pe = { "NINTENDO NES", "Select", NULL, 0, 13, 0, ex, 14, lg, 3, 0 };
        CHECK(tango_layout_page(&pe, &tw) == 0 && tw.clipped == 0);
    }
    printf("detail: sizeof(tango_browser_t) %u bytes\n", (unsigned)sizeof(tango_browser_t));
}


/* ---------------------------------------------------------------- the menu button (tango_menubtn.h) */
static uint32_t core_joy_from(const uint32_t map[32], uint32_t raw)     /* what the FPGA's pad map makes of a raw word (menu-tango docs/TANGO-SPEC.md SET_PADMAP) */
{
    uint8_t pr[64]; uint32_t j = 0; tango_pm_pairs(map, pr);
    for (int i = 0; i < 32; i++) if ((pr[2 * i] != 0xFF && ((raw >> pr[2 * i]) & 1)) || (pr[2 * i + 1] != 0xFF && ((raw >> pr[2 * i + 1]) & 1))) j |= 1u << i;
    return j;
}
/* the launcher's pads: two raw words the test sets, a saved system map the test reads back */
static uint32_t fake_raw[2]; static int fake_conn[2] = { 1, 1 }; static uint16_t fake_vid[2] = { 0x054c, 0x2dc8 }, fake_pid[2] = { 0x09cc, 0x3106 };
static int fake_sys_saves; static uint16_t fake_sys_vid, fake_sys_pid; static tango_mb_t fake_sys_mb; static uint32_t fake_sys_map[32];
static int fake_pad_raw(void *c, int port, uint16_t *vid, uint16_t *pid, uint32_t *raw) { (void)c; *vid = fake_vid[port]; *pid = fake_pid[port]; *raw = fake_raw[port]; return fake_conn[port]; }
static int fake_save_sysmap(void *c, uint16_t vid, uint16_t pid, const uint32_t map[32]) { (void)c; fake_sys_saves++; fake_sys_vid = vid; fake_sys_pid = pid; memcpy(fake_sys_map, map, sizeof fake_sys_map); tango_mb_from_map(&fake_sys_mb, map); return 0; }
static void test_menubtn(void)
{
    int a, b; (void)a; (void)b;
    tango_mb_t m; tango_mb_default(&m);
    CHECK(m.a == 12 && m.b == 13 && tango_mb_is_default(&m));
    char nm[56]; tango_mb_name(&m, nm); CHECKS(nm, "Share + Options");
    m.a = m.b = 16; tango_mb_name(&m, nm); CHECKS(nm, "PS"); CHECK(!tango_mb_is_default(&m));
    m.a = 8; m.b = 9; tango_mb_name(&m, nm); CHECKS(nm, "L1 + R1");
    CHECK(tango_mb_valid(4, 31) && !tango_mb_valid(3, 12) && !tango_mb_valid(12, 0) && !tango_mb_valid(12, 32));

    /* the system map: codes in [21] and [22], a lone button twice (input.cpp 3019, 3388-3389), file name get_map_name(dev, 1) */
    uint32_t map[32]; tango_mb_t r;
    tango_mb_to_map(&m, map); CHECK(map[21] == C(8) && map[22] == C(9) && map[20] == 0 && map[23] == 0);
    CHECK(tango_mb_from_map(&r, map) && r.a == 8 && r.b == 9);
    m.a = m.b = 16; tango_mb_to_map(&m, map); CHECK(map[21] == C(16) && map[22] == C(16) && tango_mb_from_map(&r, map) && r.a == 16 && r.b == 16);
    map[22] = 0; CHECK(tango_mb_from_map(&r, map) && r.a == 16 && r.b == 16);                   /* [22] empty = [21] (3019) */
    memset(map, 0, sizeof map); CHECK(tango_mb_from_map(&r, map) == 0 && tango_mb_is_default(&r));   /* no entry: the default */
    map[21] = 0x13C; map[22] = 0x13C; CHECK(tango_mb_from_map(&r, map) == 0 && tango_mb_is_default(&r));   /* a MiSTer evdev code is not ours */
    map[21] = C(2); map[22] = C(2); CHECK(tango_mb_from_map(&r, map) == 0);                      /* the d-pad cannot be it */
    tango_mb_mapname(0x054c, 0x09cc, nm, sizeof nm); CHECKS(nm, "input_054c_09cc_v3.map");

    /* run: a single button fires on its press, a pair when both are down; the rising edge only; a partial press does nothing */
    tango_mbdet_t d[2]; memset(d, 0, sizeof d);
    CHECK(tango_mbd_step(&d[0], 16, 16, 0, 1u << 16, 100) == 1 && tango_mbd_step(&d[0], 16, 16, 0, (1u << 16) | 1, 150) == 0);
    CHECK(tango_mbd_step(&d[0], 16, 16, 0, 0, 200) == 0 && tango_mbd_step(&d[0], 16, 16, 0, 1u << 16, 300) == 1);
    memset(d, 0, sizeof d);                                                                      /* L1 + R1 */
    CHECK(tango_mbd_step(&d[0], 8, 9, 0, 1u << 8, 10) == 0);                                     /* partial */
    CHECK(tango_mbd_step(&d[0], 8, 9, 0, (1u << 8) | (1u << 9), 20) == 1);                       /* both: fires */
    CHECK(tango_mbd_step(&d[0], 8, 9, 0, 1u << 9, 30) == 0 && tango_mbd_step(&d[0], 8, 9, 0, (1u << 8) | (1u << 9), 40) == 1);   /* one let go, both again: the edge again */
    memset(d, 0, sizeof d);                                                                      /* two pads never combine */
    CHECK(tango_mbd_step(&d[0], 8, 9, 0, 1u << 8, 10) == 0 && tango_mbd_step(&d[1], 8, 9, 0, 1u << 9, 20) == 0);
    CHECK(tango_mbd_step(&d[1], 8, 9, 0, (1u << 9) | (1u << 8), 30) == 1);                       /* port 2 on its own */
    CHECK(tango_mbd_wait_ms(&d[1], 0, 40) == -1);

    /* the escape: Share + Options held for TANGO_MB_ESCAPE_MS, once per press; released early = never */
    uint32_t both = (1u << 12) | (1u << 13);
    memset(d, 0, sizeof d);
    CHECK(tango_mbd_step(&d[0], 12, 13, TANGO_MB_ESCAPE_MS, both, 1000) == 0 && tango_mbd_wait_ms(&d[0], TANGO_MB_ESCAPE_MS, 1000) == 3000);
    CHECK(tango_mbd_poll(&d[0], TANGO_MB_ESCAPE_MS, 3999) == 0 && tango_mbd_poll(&d[0], TANGO_MB_ESCAPE_MS, 4000) == 1 && tango_mbd_poll(&d[0], TANGO_MB_ESCAPE_MS, 9000) == 0);
    CHECK(tango_mbd_wait_ms(&d[0], TANGO_MB_ESCAPE_MS, 9000) == -1);
    tango_mbd_step(&d[0], 12, 13, TANGO_MB_ESCAPE_MS, 0, 9100);
    CHECK(tango_mbd_step(&d[0], 12, 13, TANGO_MB_ESCAPE_MS, both, 10000) == 0 && tango_mbd_step(&d[0], 12, 13, TANGO_MB_ESCAPE_MS, 1u << 12, 11000) == 0 && tango_mbd_poll(&d[0], TANGO_MB_ESCAPE_MS, 20000) == 0);

    /* a game's Select + Start is not eaten (the FPGA's pad map is not touched by the menu button, as MiSTer hands the code on, input.cpp 3761) */
    uint32_t dmap[32]; tango_pm_default("Template;;-;", dmap);
    CHECK(core_joy_from(dmap, both) == ((1u << 10) | (1u << 11)));
    memset(d, 0, sizeof d); m.a = m.b = 16;                                                      /* PS is the menu button: Share + Options does not fire it */
    CHECK(tango_mbd_step(&d[0], m.a, m.b, 0, both, 100) == 0 && core_joy_from(dmap, both) == ((1u << 10) | (1u << 11)));
    CHECK(tango_mbd_step(&d[0], m.a, m.b, 0, both | (1u << 16), 200) == 1 && core_joy_from(dmap, both | (1u << 16)) == ((1u << 10) | (1u << 11)));   /* and PS still reaches the pad map */

    /* learning: one button */
    tango_mblearn_t l; uint32_t z[2] = { 0, 0 }; char tx[56];
    tango_mbl_start(&l, z, 1000);
    CHECK(tango_mbl_raw(&l, 0, 0, 1100) == TANGO_MBL_NONE);
    CHECK(tango_mbl_raw(&l, 0, 1u << 16, 1200) == TANGO_MBL_REDRAW && l.first == 16 && l.port == 0);
    tango_mbl_text(&l, tx); CHECKS(tx, "PS");
    CHECK(tango_mbl_raw(&l, 0, 0, 1300) == TANGO_MBL_DONE && l.a == 16 && l.b == 16 && !l.on);   /* released: a lone button, twice */
    /* a pair: the second while the first is held; done when the FIRST is released, in either order of release */
    tango_mbl_start(&l, z, 2000);
    CHECK(tango_mbl_raw(&l, 1, 1u << 8, 2100) == TANGO_MBL_REDRAW);                              /* port 2 */
    CHECK(tango_mbl_raw(&l, 1, (1u << 8) | (1u << 9), 2200) == TANGO_MBL_REDRAW && l.second == 9);
    tango_mbl_text(&l, tx); CHECKS(tx, "L1 + R1");
    CHECK(tango_mbl_raw(&l, 0, 1u << 20, 2250) == TANGO_MBL_NONE);                               /* the other pad is ignored: no combining */
    CHECK(tango_mbl_raw(&l, 1, 1u << 9, 2300) == TANGO_MBL_DONE && l.a == 8 && l.b == 9);        /* the first (L1) went up first */
    tango_mbl_start(&l, z, 3000);
    tango_mbl_raw(&l, 0, 1u << 14, 3100); tango_mbl_raw(&l, 0, (1u << 14) | (1u << 15), 3200);
    CHECK(tango_mbl_raw(&l, 0, 1u << 14, 3300) == TANGO_MBL_NONE && l.on);                       /* the second let go first: still waiting (input.cpp 3450-3452) */
    CHECK(tango_mbl_raw(&l, 0, 0, 3400) == TANGO_MBL_DONE && l.a == 14 && l.b == 15);
    /* a second button pressed after the first was released is not part of it: it ends the learn with the first alone */
    tango_mbl_start(&l, z, 4000);
    tango_mbl_raw(&l, 0, 1u << 10, 4100); CHECK(tango_mbl_raw(&l, 0, 0, 4200) == TANGO_MBL_DONE && l.a == 10 && l.b == 10);
    /* buttons already down when the learn began (the A that opened the page) are not buttons until they are let go and pressed again */
    uint32_t held[2] = { 1u << 5, 0 };
    tango_mbl_start(&l, held, 5000);
    CHECK(tango_mbl_raw(&l, 0, 1u << 5, 5100) == TANGO_MBL_NONE && l.first < 0);
    CHECK(tango_mbl_raw(&l, 0, 0, 5200) == TANGO_MBL_NONE && tango_mbl_raw(&l, 0, 1u << 5, 5300) == TANGO_MBL_REDRAW && l.first == 5);
    /* the d-pad is refused and taken nowhere */
    tango_mbl_start(&l, z, 6000);
    CHECK(tango_mbl_raw(&l, 0, 1u << 3, 6100) == TANGO_MBL_REFUSED && l.first < 0 && l.on);
    CHECK(tango_mbl_raw(&l, 0, 0, 6200) == TANGO_MBL_NONE && tango_mbl_raw(&l, 0, 1u << 12, 6300) == TANGO_MBL_REDRAW);
    /* cancel: nothing pressed for 8 s gives up; a press keeps it alive; an explicit cancel ends it */
    tango_mbl_start(&l, z, 7000);
    CHECK(tango_mbl_tick(&l, 14999) == TANGO_MBL_NONE && tango_mbl_tick(&l, 15000) == TANGO_MBL_GAVE_UP && !l.on);
    tango_mbl_start(&l, z, 16000); tango_mbl_raw(&l, 0, 1u << 6, 20000);
    CHECK(tango_mbl_tick(&l, 40000) == TANGO_MBL_NONE && l.on);                                  /* a button is held: not idle */
    tango_mbl_cancel(&l); CHECK(!l.on && tango_mbl_raw(&l, 0, 0, 40100) == TANGO_MBL_NONE);

    /* the launcher's System Settings > Define joystick buttons: MiSTer's walk (MENU_JOYSYSMAP -> MENU_JOYDIGMAP), then the Menu entry */
    static rec_link_t rl; tango_link_t link; fake_card_t card; tango_fs_t fs; tango_page_t p;
    tango_platform_t plat = { .fs = &fs, .load_core = fake_load, .free_mb = fake_free_mb, .reboot = fake_reboot_cb, .pad_raw = fake_pad_raw, .pmcap = fake_pmcap, .save_sysmap = fake_save_sysmap };
    rec_link_init(&rl, &link); fake_card_init(&card, &fs, CARD, NCARD);
    CHECK(tango_launcher_detect(&L, &link) == 1);
    tango_launcher_start(&L, &plat); tango_launcher_set_info(&L, FWID, MENU_CONF);
    tango_launcher_event(&L, TANGO_EV_MENU);                                                      /* System Settings, cursor on exit */
    tango_launcher_event(&L, TANGO_EV_UP); tango_launcher_event(&L, TANGO_EV_UP); tango_launcher_event(&L, TANGO_EV_UP);   /* Reboot, About, Define */
    CHECKS(L.srow[L.sel[TL_SYSTEM]].label, "Define joystick buttons");
    fake_raw[0] = 1u << 5; fake_raw[1] = 0; fake_sys_saves = 0; uint32_t now = 10000;              /* A is down on pad 1: it opened the page */
    fake_vid[0] = 0x054c; fake_pid[0] = 0x09cc;
    CHECK(tango_launcher_event(&L, TANGO_EV_SELECT) == 1 && L.mode == TL_DEFINE && L.dph == 0);
    CHECK(tango_launcher_wait_ms(&L, now) == 50);                                                /* the pads are polled, the gamepad flashes */
    tango_launcher_page(&L, &p); CHECKS(p.title, "Define buttons"); CHECK(p.sel == -1 && p.nlegend == 2);
    CHECKS(p.legend[0].key, "Esc"); CHECKS(p.rows[3].label, "Press: RIGHT"); CHECKS(p.rows[8].label, "Esc \x7f Cancel"); CHECKS(p.rows[9].label, "Enter \x7f Finish");
    CHECK(p.rows[5].kind == TR_BLANK && p.rows[7].kind == TR_BLANK);                              /* no pad known yet: no ID, no Undefine */
    dump_layout("l_define", &p);
    fake_raw[0] = 0; tango_launcher_tick(&L, now += 30);                                          /* the opening A goes up: not a button */
    CHECK(fake_pm_store.dev < 0 && fake_pm_store.button == 0);
    /* the order of the walk: RIGHT LEFT DOWN UP, then A B X Y L R Select Start (the menu core's names), each taken on its press, the prompt moves on at its release */
    static const int BIT[12] = { 0, 1, 2, 3, 5, 4, 7, 6, 8, 9, 12, 13 };
    static const char *const PR[13] = { "RIGHT", "LEFT", "DOWN", "UP", "A", "B", "X", "Y", "L", "R", "Select", "Start", "Menu" };
    for (int i = 0; i < 12; i++) {
        tango_launcher_page(&L, &p); char want[32]; snprintf(want, sizeof want, "Press: %s", PR[i]); CHECKS(p.rows[3].label, want);
        if (i == 0) { fake_raw[0] = 1u << BIT[0]; CHECK(tango_launcher_tick(&L, now += 30) == 1);   /* the first press picks the pad */
                      CHECK(fake_pm_store.dev == 0 && fake_pm_store.vid == 0x054c && fake_pm_store.button == 0);
                      tango_launcher_page(&L, &p); CHECKS(p.rows[5].label, "Joystick ID: 054c:09cc"); CHECK(p.rows[7].kind == TR_BLANK); dump_layout("l_define_id", &p); }
        else { fake_raw[0] = 1u << BIT[i]; tango_launcher_tick(&L, now += 30); }
        if (i == 1) {                                                                              /* one key at a time: a second button while one is held is ignored */
            fake_raw[0] |= 1u << 9; tango_launcher_tick(&L, now += 30); fake_raw[0] = 1u << BIT[i]; tango_launcher_tick(&L, now += 30); }
        fake_raw[0] = 0; tango_launcher_tick(&L, now += 30);
        CHECK(fake_pm_store.button == i + 1 || L.dph == 1);
        if (i == 0) { tango_launcher_page(&L, &p); CHECKS(p.rows[7].label, "User/Space \x7f Undefine"); }   /* past the first button */
    }
    for (int i = 0; i < 12; i++) CHECK(fake_pm_store.map[i] == TANGO_PM_CODE(BIT[i]));
    CHECK(L.dph == 1 && L.mode == TL_DEFINE);                                                     /* the Menu entry: phase 1 */
    tango_launcher_page(&L, &p); CHECKS(p.rows[3].label, "Press: Menu"); CHECKS(p.rows[12].label, "(can use 2-button combo)"); CHECKS(p.rows[5].label, "Joystick ID: 054c:09cc");
    dump_layout("l_define_menu", &p);
    fake_raw[0] = 1u << 3; CHECK(tango_launcher_tick(&L, now += 30) == 1);                        /* the d-pad: refused, said so */
    tango_launcher_page(&L, &p); CHECKS(p.rows[10].label, "The d-pad cannot be the menu button");
    fake_raw[0] = 0; tango_launcher_tick(&L, now += 30);
    fake_raw[1] = 1u << 8; tango_launcher_tick(&L, now += 30);                                    /* pad 2 does not take part */
    CHECK(L.ml.first < 0);
    fake_raw[1] = 0; fake_raw[0] = 1u << 8; CHECK(tango_launcher_tick(&L, now += 30) == 1);
    tango_launcher_page(&L, &p); CHECKS(p.rows[7].label, "L1");
    fake_raw[0] = (1u << 8) | (1u << 9); CHECK(tango_launcher_tick(&L, now += 30) == 1);
    tango_launcher_page(&L, &p); CHECKS(p.rows[7].label, "L1 + R1");
    dump_layout("l_define_pair", &p);
    CHECK(tango_launcher_event(&L, TANGO_EV_DOWN) == 1 && L.mode == TL_DEFINE);                   /* the pad's own keys do nothing here (the firmware drops them) */
    fake_raw[0] = 1u << 9; CHECK(tango_launcher_tick(&L, now += 30) == 1);                        /* the first button (L1) went up: done, with R1 still held */
    CHECK(L.mode == TL_SYSTEM);
    CHECK(fake_sys_saves == 1 && fake_sys_vid == 0x054c && fake_sys_pid == 0x09cc && fake_sys_mb.a == 8 && fake_sys_mb.b == 9);
    for (int i = 0; i < 12; i++) CHECK(fake_sys_map[i] == TANGO_PM_CODE(BIT[i]));                 /* the file: the buttons, SYS_BTN_* order */
    CHECK(fake_sys_map[21] == TANGO_PM_CODE(8) && fake_sys_map[22] == TANGO_PM_CODE(9) && fake_sys_map[12] == 0 && fake_sys_map[20] == 0);
    tango_launcher_page(&L, &p); CHECK(p.nlegend == 1 && !strcmp(p.legend[0].key, "Joystick map") && !strcmp(p.legend[0].label, "L1 + R1"));
    CHECKS(L.srow[L.sel[TL_SYSTEM]].label, "Define joystick buttons");                           /* menusub 2 (menu.cpp 4380-4384) */
    /* the gamepad helper flashes the button asked for (menu.cpp 4298-4345): two phases 100 ms apart, the line with the button blank in one of them */
    fake_raw[0] = fake_raw[1] = 0; fake_sys_saves = 0;
    tango_launcher_event(&L, TANGO_EV_SELECT); CHECK(L.mode == TL_DEFINE);
    now = 20000; fake_raw[0] = 1u << 0; tango_launcher_tick(&L, now); fake_raw[0] = 0; tango_launcher_tick(&L, now += 30);   /* RIGHT taken: asks LEFT */
    L.now_ms = 20200; tango_launcher_page(&L, &p); CHECK(p.nrows == 14 && p.rows[12].kind == TR_CENTER); dump_layout("l_define_pad_a", &p);
    L.now_ms = 20100; tango_launcher_page(&L, &p); CHECK(p.rows[12].kind == TR_BLANK && p.rows[10].kind == TR_CENTER); dump_layout("l_define_pad_b", &p);
    /* Space skips the button asked for, Esc cancels everything, Enter finishes with what was taken */
    CHECK(fake_pm_store.button == 1); tango_launcher_event(&L, TANGO_EV_SPACE); CHECK(fake_pm_store.button == 2 && L.mode == TL_DEFINE);
    tango_launcher_event(&L, TANGO_EV_BACK); CHECK(L.mode == TL_SYSTEM && fake_sys_saves == 0);
    tango_launcher_page(&L, &p); CHECKS(p.legend[0].label, "cancelled: nothing saved");
    tango_launcher_event(&L, TANGO_EV_SELECT); fake_raw[0] = 1u << 2; now += 30; tango_launcher_tick(&L, now += 30); fake_raw[0] = 0; tango_launcher_tick(&L, now += 30);
    tango_launcher_event(&L, TANGO_EV_SELECT);                                                    /* Enter: finish_map_setting(0) */
    CHECK(L.mode == TL_SYSTEM && fake_sys_saves == 1 && fake_sys_map[0] == TANGO_PM_CODE(2) && fake_sys_map[21] == 0);   /* the menu button stays the default */
    /* idle for 8 s with no pad: given up, unchanged */
    fake_raw[0] = fake_raw[1] = 0; fake_sys_saves = 0;
    tango_launcher_event(&L, TANGO_EV_SELECT); CHECK(L.mode == TL_DEFINE);
    now += 100; CHECK(tango_launcher_tick(&L, now + 7000) == 0 && L.mode == TL_DEFINE);
    CHECK(tango_launcher_tick(&L, now + 8100) == 1 && L.mode == TL_SYSTEM && fake_sys_saves == 0);
    tango_launcher_page(&L, &p); CHECKS(p.legend[0].label, "nothing pressed: unchanged");
    /* Space is Select everywhere else */
    tango_launcher_event(&L, TANGO_EV_UP); tango_launcher_event(&L, TANGO_EV_SPACE); CHECK(L.mode == TL_SYSTEM || L.mode == TL_CORES || L.mode == TL_DEFINE || L.mode == TL_ABOUT);
}


/* ---------------------------------------------------------------- how the menu is called and left */
static int rc_known; static char rc_path[96];
static int fake_running_core(void *ctx, char *path, int cap) { (void)ctx; if (!rc_known) return 0; strncpy(path, rc_path, (size_t)cap - 1); path[cap - 1] = 0; return 1; }
static void test_invocation(void)
{
    /* ---- the menu key's two edges (MiSTer: F12 on the press, F12|UPSTROKE on the release; menu.cpp 1338-1349) ---- */
    tango_mkey_t k; memset(&k, 0, sizeof k);
    CHECK(tango_mkey_press(&k, 0) == 0);                       /* closed: the press does nothing */
    CHECK(tango_mkey_release(&k) == 1);                        /* the release opens */
    CHECK(tango_mkey_press(&k, 1) == 1);                       /* open: the press closes ... */
    CHECK(tango_mkey_release(&k) == 0);                        /* ... and the release is swallowed (ignore_osd_release) */
    CHECK(tango_mkey_press(&k, 0) == 0 && tango_mkey_release(&k) == 1);   /* the next cycle works again */
    /* the OSD opened by other means under a held key (the 3 s escape): its release must not close it */
    CHECK(tango_mkey_press(&k, 0) == 0); tango_mkey_swallow(&k); CHECK(tango_mkey_release(&k) == 0);
    CHECK(tango_mkey_press(&k, 0) == 0 && tango_mkey_release(&k) == 1);   /* ... once only */
    tango_mkey_swallow(&k); CHECK(tango_mkey_press(&k, 0) == 0 && tango_mkey_release(&k) == 1);   /* nothing held: a swallow is a no-op */
    /* a release with the menu opened meanwhile by another key and no ignore: MiSTer toggles (it closes) */
    CHECK(tango_mkey_press(&k, 0) == 0 && tango_mkey_release(&k) == 1);

    /* ---- the raw pair detector reports both edges, the Select+Start combo too ---- */
    tango_mbdet_t d; memset(&d, 0, sizeof d);
    uint32_t pair = (1u << 8) | (1u << 9);
    CHECK(tango_mbd_edge(&d, 8, 9, 0, 1) == 0 && tango_mbd_edge(&d, 8, 9, 1u << 8, 2) == 0);
    CHECK(tango_mbd_edge(&d, 8, 9, pair, 3) == 1 && tango_mbd_edge(&d, 8, 9, pair, 4) == 0);
    CHECK(tango_mbd_edge(&d, 8, 9, 1u << 9, 5) == -1 && tango_mbd_edge(&d, 8, 9, 0, 6) == 0);       /* the first button up: the release edge, once */
    CHECK(tango_mbd_edge(&d, 8, 9, pair, 7) == 1 && tango_mbd_edge(&d, 8, 9, 0, 8) == -1);
    tango_padkeys_t pk; tango_padkeys_out_t po;
    tango_padkeys_sync(&pk, 0);
    tango_padkeys_step(&pk, TANGO_JOY_SELECT, &po); CHECK(!po.combo && !po.combo_up);
    tango_padkeys_step(&pk, TANGO_JOY_SELECT | TANGO_JOY_START, &po); CHECK(po.combo && !po.combo_up);
    tango_padkeys_step(&pk, TANGO_JOY_SELECT | TANGO_JOY_START, &po); CHECK(!po.combo && !po.combo_up);
    tango_padkeys_step(&pk, TANGO_JOY_START, &po); CHECK(!po.combo && po.combo_up && po.n == 0);     /* Select up first: the release, no grave key */
    tango_padkeys_step(&pk, 0, &po); CHECK(!po.combo && !po.combo_up && po.n == 0);
    tango_padkeys_step(&pk, TANGO_JOY_SELECT | TANGO_JOY_START, &po); CHECK(po.combo);               /* both at once */
    tango_padkeys_step(&pk, 0, &po); CHECK(po.combo_up);                                             /* both up at once */
    /* the keyboard: F12 and the Application key, both edges, never anything else */
    CHECK(tango_kbd_is_menu_key(0x45) && tango_kbd_is_menu_key(0x65) && !tango_kbd_is_menu_key(0x44) && !tango_kbd_is_menu_key(0x29));
    CHECK(tango_kbd_menu_key(0x65, 1, 1) == TKEY_TOGGLE && tango_kbd_menu_key(0x65, 0, 0) == TKEY_TOGGLE && tango_kbd_menu_key(0x65, 1, 0) == TKEY_NONE);

    /* ---- the in-core pages: Menu / Back as MiSTer's handlers ---- */
    static tango_coremenu_t cm; static tango_osd_t osd; static tango_browser_t br;
    static rec_link_t r; tango_link_t link; fake_card_t card; tango_fs_t fs;
    rec_link_init(&r, &link); r.confstr = TEMPLATE_CONF; r.menumask = 0;
    fake_card_init(&card, &fs, CM_CARD, (int)(sizeof CM_CARD / sizeof CM_CARD[0]));
    tango_core_platform_t plat = { .fs = &fs, .load_core = cm_load_core, .reboot = cm_reboot, .load_file = cm_load_file,
                                   .save_cfg = cm_save, .dir_exists = cm_dir_exists, .running_core = fake_running_core };
    static const uint8_t PAL[16][3] = TANGO_PAL_INIT;
    static char conf[TANGO_CONF_MAX];
    tango_sys_read_confstr(&link, conf, sizeof conf);
    tango_osd_probe(&osd, &link); tango_osd_start(&osd, PAL);
    tango_core_init(&cm, &link, &osd, &br, &plat, conf, NULL);

    /* B on System closes (menu.cpp 2971-2983), Left goes to page 0 */
    tango_core_toggle(&cm); tango_core_event(&cm, TANGO_EV_RIGHT); CHECK(cm.mode == TANGO_CM_SYSTEM);
    CHECK(tango_core_event(&cm, TANGO_EV_LEFT) == TANGO_CM_REDRAWN && cm.mode == TANGO_CM_PAGE && cm.page == 0);
    tango_core_event(&cm, TANGO_EV_RIGHT);
    CHECK(tango_core_event(&cm, TANGO_EV_BACK) == TANGO_CM_CLOSED_NOW && cm.mode == TANGO_CM_CLOSED);
    /* B on Misc. Options closes (menu.cpp 4100-4160); Right on exit goes back to page 0 */
    tango_core_toggle(&cm); tango_core_event(&cm, TANGO_EV_LEFT); CHECK(cm.mode == TANGO_CM_MISC);
    CHECK(tango_core_event(&cm, TANGO_EV_BACK) == TANGO_CM_CLOSED_NOW && cm.mode == TANGO_CM_CLOSED);
    tango_core_toggle(&cm); tango_core_event(&cm, TANGO_EV_LEFT);
    CHECK(tango_core_event(&cm, TANGO_EV_MENU) == TANGO_CM_CLOSED_NOW && cm.mode == TANGO_CM_CLOSED);
    /* B on page 0 closes, on a sub-page goes to page 0 (unchanged) */
    tango_core_toggle(&cm); CHECK(tango_core_event(&cm, TANGO_EV_BACK) == TANGO_CM_CLOSED_NOW);
    /* About: Menu, B, Left, Select go back to System, never close (menu.cpp 4560-4562) */
    for (int i = 0; i < 3; i++) {
        goto_about(&cm); CHECK(cm.mode == TANGO_CM_ABOUT);
        tango_core_event(&cm, i == 0 ? TANGO_EV_MENU : i == 1 ? TANGO_EV_BACK : TANGO_EV_LEFT);
        CHECK(cm.mode == TANGO_CM_SYSTEM && !strcmp(cm.lab[cm.sel], "About"));
    }
    tango_core_event(&cm, TANGO_EV_MENU); CHECK(cm.mode == TANGO_CM_CLOSED);       /* and Menu on System closes */
    /* Reset: Menu = "no" (unchanged) */
    tango_core_toggle(&cm); tango_core_event(&cm, TANGO_EV_RIGHT);
    while (strcmp(cm.lab[cm.sel], "Reset settings") && cm.sel < cm.nrows - 1) tango_core_event(&cm, TANGO_EV_DOWN);
    tango_core_event(&cm, TANGO_EV_SELECT); CHECK(cm.mode == TANGO_CM_RESET);
    CHECK(tango_core_event(&cm, TANGO_EV_MENU) == TANGO_CM_REDRAWN && cm.mode == TANGO_CM_SYSTEM);
    tango_core_event(&cm, TANGO_EV_MENU);
    /* the file picker (Test Page 1's F entry): Menu returns to the page that opened it, cursor on the F row, and does not close everything;
     * B does the same (with a typed filter cleared first); a click through to a folder and Menu is the same */
    r.menumask = 1;
    tango_core_toggle(&cm);
    while (cm.sel != 5) tango_core_event(&cm, TANGO_EV_DOWN);
    tango_core_event(&cm, TANGO_EV_SELECT); CHECK(cm.mode == TANGO_CM_PAGE && cm.page == 1);
    tango_core_event(&cm, TANGO_EV_DOWN); CHECKS(cm.lab[cm.sel], "Load *.BIN");
    tango_core_event(&cm, TANGO_EV_SELECT); CHECK(cm.mode == TANGO_CM_FILES);
    CHECK(tango_core_event(&cm, TANGO_EV_MENU) == TANGO_CM_REDRAWN && cm.mode == TANGO_CM_PAGE && cm.page == 1 && !strcmp(cm.lab[cm.sel], "Load *.BIN"));
    tango_core_event(&cm, TANGO_EV_SELECT); CHECK(cm.mode == TANGO_CM_FILES);
    CHECK(tango_core_event(&cm, TANGO_EV_BACK) == TANGO_CM_REDRAWN && cm.mode == TANGO_CM_PAGE && cm.page == 1);
    tango_core_event(&cm, TANGO_EV_MENU); CHECK(cm.mode == TANGO_CM_CLOSED);        /* and Menu on the page closes */
    r.menumask = 0;

    /* ---- the Cores list: opened in the running core's folder, the core selected (menu.cpp 450-459) ---- */
    rc_known = 1; strcpy(rc_path, "_Console/NES_20260921.fs");
    tango_core_toggle(&cm); tango_core_event(&cm, TANGO_EV_RIGHT); CHECK(cm.sel == 0 && !strcmp(cm.lab[0], "Core"));
    tango_core_event(&cm, TANGO_EV_SELECT);
    CHECK(cm.mode == TANGO_CM_CORES && !strcmp(br.path, "_Console") && br.sel == 1 && !strcmp(tango_browser_name(&br, tango_browser_cur(&br)), "NES_20260921.fs"));
    /* B is up a folder; at the root it leaves for System */
    CHECK(tango_core_event(&cm, TANGO_EV_BACK) == TANGO_CM_REDRAWN && cm.mode == TANGO_CM_CORES && br.path[0] == 0);
    CHECK(tango_core_event(&cm, TANGO_EV_BACK) == TANGO_CM_REDRAWN && cm.mode == TANGO_CM_SYSTEM);
    /* Menu cancels to System from any depth */
    tango_core_event(&cm, TANGO_EV_SELECT); CHECK(cm.mode == TANGO_CM_CORES && !strcmp(br.path, "_Console"));
    CHECK(tango_core_event(&cm, TANGO_EV_MENU) == TANGO_CM_REDRAWN && cm.mode == TANGO_CM_SYSTEM && cm.sel == 0);
    /* unknown core (a warm reset, a power-up into a core): the root; a folder that is gone: cut back to what exists */
    rc_known = 0; tango_core_event(&cm, TANGO_EV_SELECT); CHECK(cm.mode == TANGO_CM_CORES && br.path[0] == 0); tango_core_event(&cm, TANGO_EV_MENU);
    rc_known = 1; strcpy(rc_path, "_Gone/Sub/X_20260101.fs"); tango_core_event(&cm, TANGO_EV_SELECT);
    CHECK(cm.mode == TANGO_CM_CORES && br.path[0] == 0 && br.sel == 0); tango_core_event(&cm, TANGO_EV_MENU);
    strcpy(rc_path, "_Console/Gone_20260101.fs"); tango_core_event(&cm, TANGO_EV_SELECT);
    CHECK(cm.mode == TANGO_CM_CORES && !strcmp(br.path, "_Console") && br.sel == 0);      /* the folder, the file is not there: the top */
    tango_core_event(&cm, TANGO_EV_MENU); tango_core_event(&cm, TANGO_EV_MENU); CHECK(cm.mode == TANGO_CM_CLOSED);

    /* ---- Alt+Menu: the Cores list straight away, cancel (Menu, B at the root) closes; Win+Menu: the System page; plain: page 0 ---- */
    rc_known = 1; strcpy(rc_path, "_Console/NES_20260921.fs");
    CHECK(tango_core_toggle_how(&cm, TANGO_OPEN_CORES) == 1 && cm.mode == TANGO_CM_CORES && !strcmp(br.path, "_Console") && br.sel == 1);
    CHECK(tango_core_event(&cm, TANGO_EV_MENU) == TANGO_CM_CLOSED_NOW && cm.mode == TANGO_CM_CLOSED);
    tango_core_toggle_how(&cm, TANGO_OPEN_CORES); tango_core_event(&cm, TANGO_EV_BACK);      /* up to the root */
    CHECK(br.path[0] == 0 && cm.mode == TANGO_CM_CORES);
    CHECK(tango_core_event(&cm, TANGO_EV_BACK) == TANGO_CM_CLOSED_NOW && cm.mode == TANGO_CM_CLOSED);
    tango_core_toggle_how(&cm, TANGO_OPEN_CORES); CHECK(tango_core_toggle_how(&cm, TANGO_OPEN_PAGE0) == 0 && cm.mode == TANGO_CM_CLOSED);   /* toggles closed whatever the page */
    CHECK(tango_core_toggle_how(&cm, TANGO_OPEN_SYSTEM) == 1 && cm.mode == TANGO_CM_SYSTEM && cm.sel == 0 && !cm.cores_direct);
    tango_core_event(&cm, TANGO_EV_SELECT); CHECK(cm.mode == TANGO_CM_CORES);               /* System > Core after Alt+Menu earlier: cancel goes to System */
    tango_core_event(&cm, TANGO_EV_MENU); CHECK(cm.mode == TANGO_CM_SYSTEM);
    tango_core_event(&cm, TANGO_EV_MENU);
    CHECK(tango_core_toggle_how(&cm, TANGO_OPEN_PAGE0) == 1 && cm.mode == TANGO_CM_PAGE && cm.page == 0);
}

/* the Tango Template core's page 0 (TANGO_TEMPLATE_CONF): the layouts launcher_pages 15_core_template_page0 models, and the same
 * A on TV Mode the menu repository's co-simulation makes (Down x3 to row 4, A -> PAL, status[7:0] = 04, capture page0pal) */
static void test_coremenu_tango(void)
{
    static tango_coremenu_t cm; static tango_osd_t osd; static tango_browser_t br;
    static rec_link_t r; tango_link_t link; fake_card_t card; tango_fs_t fs; tango_page_t p;
    rec_link_init(&r, &link); r.confstr = TANGO_TEMPLATE_CONF; r.menumask = 0;
    fake_card_init(&card, &fs, CM_CARD, (int)(sizeof CM_CARD / sizeof CM_CARD[0]));
    tango_core_platform_t plat = { .fs = &fs, .load_core = cm_load_core, .reboot = cm_reboot,
                                   .load_file = cm_load_file, .save_cfg = cm_save, .dir_exists = cm_dir_exists };
    static const uint8_t PAL[16][3] = TANGO_PAL_INIT;
    static char conf[TANGO_CONF_MAX];
    CHECK(tango_sys_read_confstr(&link, conf, sizeof conf) == 690);
    CHECK(!strcmp(conf, TANGO_TEMPLATE_CONF));
    CHECK(tango_osd_probe(&osd, &link));
    tango_osd_start(&osd, PAL);
    tango_core_init(&cm, &link, &osd, &br, &plat, conf, NULL);
    CHECKS(cm.name, "Template"); CHECKS(cm.cfgname, "Template_v1.CFG"); CHECKS(cm.banner, "TEMPLATE");
    CHECK(tango_core_toggle(&cm) == 1);
    tango_core_page(&cm, &p);
    CHECKS(p.title, "Template"); CHECK(p.sel == 1); CHECK(p.nrows == 18); CHECK(p.top < 0);      /* 18 rows: the layout scrolls (the "2/18" hint) */
    CHECK(row_is(&p, 0, TR_BLANK, 0, 0)); CHECK(row_is(&p, 1, TR_OPT, "Aspect ratio", "Original"));
    CHECK(row_is(&p, 2, TR_OPT, "Scale", "Normal")); CHECK(row_is(&p, 3, TR_OPT, "Scanlines", "None"));
    CHECK(row_is(&p, 4, TR_OPT, "TV Mode", "NTSC")); CHECK(row_is(&p, 5, TR_OPT, "Pattern", "Gradient"));
    CHECK(row_is(&p, 6, TR_OPT, "Source size", "320x240")); CHECK(row_is(&p, 7, TR_OPT, "Audio test", "Off"));
    CHECK(row_is(&p, 8, TR_OPT, "Stereo Mix", "None")); CHECK(row_is(&p, 9, TR_BLANK, 0, 0));
    CHECK(row_is(&p, 10, TR_SUB, "Test Page 1", 0)); CHECK(row_is(&p, 12, TR_SUB, "Test Page 2", 0));
    CHECK(row_is(&p, 15, TR_ACT, "Reset", 0)); CHECK(row_is(&p, 16, TR_ACT, "Reset and close OSD", 0));
    CHECK(row_is(&p, 17, TR_ACT, "\x83  exit", "\x7f"));                                          /* the exit row with both cues, last */
    cm_dump("cm_template_page0", &cm);
    for (int i = 0; i < 3; i++) tango_core_event(&cm, TANGO_EV_DOWN);
    CHECK(cm.sel == 4);
    r.status_writes = 0;
    CHECK(tango_core_event(&cm, TANGO_EV_SELECT) == TANGO_CM_REDRAWN);
    CHECK(r.status_writes == 1 && r.status[0] == 0x04);
    tango_core_page(&cm, &p); CHECK(row_is(&p, 4, TR_OPT, "TV Mode", "PAL"));
    cm_dump("cm_template_page0_pal", &cm);
}

int main(int argc, char **argv)
{
    const char *streams = argc > 1 ? argv[1] : ".";
    layout_dir = argc > 2 ? argv[2] : ".";
    test_names();
    test_joy_and_menu();
    test_browser();
    test_layouts();
    test_streams(streams);
    test_settings(streams);
    test_coremenu();
    test_coremenu_tango();
    test_volume();
    test_misc();
    test_info(streams);
    test_list_keys();
    test_core_keys();
    test_detail();
    test_padmap();
    test_menubtn();
    test_save_kbd();
    test_browse_limits();
    test_long_names();
    test_invocation();
    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
