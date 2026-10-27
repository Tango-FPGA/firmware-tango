// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 the Tango authors
/*
 * test_coremenu_zip.c -- host tests for the browser-level zip state machine in tango_coremenu.c
 * (enter_zip/leave_zip, the TANGO_EV_SELECT/BACK wiring): entering a .zip from a core's F-entry file browser,
 * seeing its matching contents, loading a member, and the repeated enter/exit state-cleanup cases:
 * a different zip after closing one, backing out without
 * selecting anything, and re-entering the same zip twice. Drives tango_coremenu.c through its public event API
 * exactly as test_tango.c does (tango_core_toggle/tango_core_event), not by calling its static helpers.
 * Tango, our own code.
 */
#include <stdio.h>
#include <string.h>
#include "../tango_coremenu.h"
#include "fake_io.h"
#include "fake_zip_fixtures.h"

static int fails = 0, checks = 0;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)
#define CHECKS(a, b) CHECK(!strcmp((a), (b)))

static int row_is(const tango_page_t *p, int r, int kind, const char *label)
{
    if (r >= p->nrows) return 0;
    return p->rows[r].kind == kind && (!label || !strcmp(p->rows[r].label, label));
}

/* ---- a tiny path -> byte-blob zipfs, so the one fake can serve two different fixture archives by name ---- */
typedef struct { const char *path; const unsigned char *data; unsigned len; } blob_t;
static const blob_t *BLOBS; static int N_BLOBS; static int Z_OPEN_CALLS, Z_CLOSE_CALLS;
static const blob_t *cur_blob;
static int mz_open(void *ctx, const char *path, uint32_t *size)
{
    (void)ctx; Z_OPEN_CALLS++;
    for (int i = 0; i < N_BLOBS; i++) if (!strcmp(BLOBS[i].path, path)) { cur_blob = &BLOBS[i]; *size = BLOBS[i].len; return 1; }
    cur_blob = NULL;
    return 0;
}
static int mz_read(void *ctx, uint32_t off, uint8_t *buf, unsigned n)
{
    (void)ctx;
    if (!cur_blob || off > cur_blob->len) return -1;
    unsigned avail = cur_blob->len - off; if (n > avail) n = avail;
    memcpy(buf, cur_blob->data + off, n);
    return (int)n;
}
static void mz_close(void *ctx) { (void)ctx; Z_CLOSE_CALLS++; cur_blob = NULL; }

/* ---- the fake platform's load hooks ---- */
static int LOADS, ZLOADS; static char LAST_PATH[300], LAST_MEMBER[300]; static uint8_t LAST_IDX;
static int cm_load_file(void *ctx, const char *p, uint8_t idx)
{ (void)ctx; LOADS++; strncpy(LAST_PATH, p, sizeof LAST_PATH - 1); LAST_IDX = idx; return 0; }
static int cm_load_zip_file(void *ctx, const char *zip_path, const char *member, uint8_t idx)
{ (void)ctx; ZLOADS++; strncpy(LAST_PATH, zip_path, sizeof LAST_PATH - 1); strncpy(LAST_MEMBER, member, sizeof LAST_MEMBER - 1); LAST_IDX = idx; return 0; }

static const char *CARD[] = {
    "games/", "games/NES/", "games/NES/single.zip", "games/NES/multi.zip", "games/NES/real.nes",
};
static const blob_t BLOB_TABLE[] = {
    { "games/NES/single.zip", FX_SINGLE, FX_SINGLE_len },
    { "games/NES/multi.zip", FX_MULTI, FX_MULTI_len },
};

/* opens the OSD, navigates to the (only) F row and selects it: the common setup for every case below.
 * Defensive about the OSD's starting state (toggle() closes an already-open OSD instead of opening it) since a
 * previous case may have left it open on some other page (e.g. case 4 leaves it open on page 0 after Back). */
static void open_f_browser(tango_coremenu_t *cm, tango_osd_t *osd)
{
    if (cm->mode != TANGO_CM_CLOSED) tango_core_toggle(cm);
    CHECK(cm->mode == TANGO_CM_CLOSED);
    tango_core_toggle(cm);
    CHECK(cm->mode == TANGO_CM_PAGE);
    CHECK(tango_core_event(cm, TANGO_EV_SELECT) == TANGO_CM_REDRAWN);
    CHECK(cm->mode == TANGO_CM_FILES);
    (void)osd;
}

int main(void)
{
    BLOBS = BLOB_TABLE; N_BLOBS = (int)(sizeof BLOB_TABLE / sizeof BLOB_TABLE[0]);

    static tango_coremenu_t cm; static tango_osd_t osd; static tango_browser_t br;
    static rec_link_t r; tango_link_t link; fake_card_t card; tango_fs_t fs;
    tango_zipfs_t zipfs = { mz_open, mz_read, mz_close, NULL };
    tango_page_t p;

    rec_link_init(&r, &link); r.confstr = "NES;;FS1,NES,Load ROM;"; r.menumask = 0;
    fake_card_init(&card, &fs, CARD, (int)(sizeof CARD / sizeof CARD[0]));
    tango_core_platform_t plat = { .fs = &fs, .zipfs = &zipfs, .load_file = cm_load_file,
                                   .load_zip_file = cm_load_zip_file };
    tango_osd_probe(&osd, &link);

    /* ---- 1. entering a single-match zip: a one-level pseudo-folder, ".." then the one ROM, feature-for-feature
     * with MiSTer's verified behaviour (always a folder-like listing, never auto-loaded) ---- */
    tango_core_init(&cm, &link, &osd, &br, &plat, r.confstr, NULL);
    open_f_browser(&cm, &osd);
    CHECKS(br.path, "games/NES");
    tango_core_page(&cm, &p);
    /* ".." + the 3 files: real.nes matches "NES" directly, single.zip/multi.zip match the "ZIP" group the F-entry
     * handler appends to cm->f_exts whenever the platform has zip support (tango_coremenu.c's CS_FILE case) */
    CHECK(p.nrows == 4);
    while (br.sel < br.n - 1 && strcmp(tango_browser_name(cm.br, tango_browser_cur(cm.br)), "single.zip"))
        tango_browser_next(cm.br);
    CHECKS(tango_browser_name(cm.br, tango_browser_cur(cm.br)), "single.zip");
    CHECK(tango_core_event(&cm, TANGO_EV_SELECT) == TANGO_CM_REDRAWN);
    CHECK(cm.in_zip == 1);
    CHECK(cm.mode == TANGO_CM_FILES);
    tango_core_page(&cm, &p);
    CHECK(row_is(&p, 0, TR_UP, "..")); CHECK(row_is(&p, 1, TR_FILE, "GAME.NES")); CHECK(p.nrows == 2);

    /* ---- 2. loading the one file inside it: load_zip_file gets the archive path + the inner member's name,
     * and cm.in_zip drops to 0 the instant the OSD closes (so a later open_browser() will not re-close cm.zip
     * out from under the now-in-flight download -- see enter_zip's header comment) ---- */
    tango_browser_next(cm.br);                 /* off ".." (sel 0) onto GAME.NES */
    CHECK(tango_browser_name(cm.br, tango_browser_cur(cm.br)) && !tango_browser_cur(cm.br)->is_dir);
    ZLOADS = 0;
    CHECK(tango_core_event(&cm, TANGO_EV_SELECT) == TANGO_CM_CLOSED_NOW);
    CHECK(ZLOADS == 1);
    CHECKS(LAST_PATH, "games/NES/single.zip");
    CHECKS(LAST_MEMBER, "GAME.NES");
    CHECK(cm.in_zip == 0);
    CHECK(cm.mode == TANGO_CM_CLOSED);

    /* ---- 3. backing out of a zip WITHOUT selecting anything: the real
     * folder's listing comes back exactly as it was ---- */
    open_f_browser(&cm, &osd);
    while (strcmp(tango_browser_name(cm.br, tango_browser_cur(cm.br)), "single.zip")) tango_browser_next(cm.br);
    CHECK(tango_core_event(&cm, TANGO_EV_SELECT) == TANGO_CM_REDRAWN && cm.in_zip == 1);
    CHECK(tango_browser_cur(cm.br)->is_dir);                      /* positioned on ".." after entering */
    CHECK(tango_core_event(&cm, TANGO_EV_SELECT) == TANGO_CM_REDRAWN);   /* select ".." -- back out, no load */
    CHECK(cm.in_zip == 0);
    CHECKS(br.path, "games/NES");
    tango_core_page(&cm, &p);
    CHECK(p.nrows == 4);     /* back to the real folder's full listing */

    /* ---- 4. re-entering the SAME zip a second time: the identical listing, not empty or doubled ---- */
    while (strcmp(tango_browser_name(cm.br, tango_browser_cur(cm.br)), "single.zip")) tango_browser_next(cm.br);
    CHECK(tango_core_event(&cm, TANGO_EV_SELECT) == TANGO_CM_REDRAWN && cm.in_zip == 1);
    tango_core_page(&cm, &p);
    CHECK(row_is(&p, 0, TR_UP, "..")); CHECK(row_is(&p, 1, TR_FILE, "GAME.NES")); CHECK(p.nrows == 2);
    /* this time leave it with the Back button instead of selecting ".." -- must clean up the same way */
    CHECK(tango_core_event(&cm, TANGO_EV_BACK) == TANGO_CM_REDRAWN);
    CHECK(cm.in_zip == 0);
    CHECK(cm.mode == TANGO_CM_PAGE);                                /* Back from Files: all the way to page 0 */

    /* ---- 5. a DIFFERENT zip after closing one: no stale entries from the first one ---- */
    open_f_browser(&cm, &osd);
    while (strcmp(tango_browser_name(cm.br, tango_browser_cur(cm.br)), "multi.zip")) tango_browser_next(cm.br);
    CHECK(tango_core_event(&cm, TANGO_EV_SELECT) == TANGO_CM_REDRAWN && cm.in_zip == 1);
    tango_core_page(&cm, &p);
    CHECK(p.nrows == 3);      /* .. + the two NES-matching entries (not readme.txt) */
    int saw_u = 0, saw_e = 0;
    for (int i = 1; i < p.nrows; i++) {
        if (!strcmp(p.rows[i].label, "Game (U).nes")) saw_u = 1;
        if (!strcmp(p.rows[i].label, "Game (E).nes")) saw_e = 1;
    }
    CHECK(saw_u && saw_e);
    /* load the second one by name, not just position, to prove tango_zip_find resolves it against THIS zip */
    while (strcmp(tango_browser_name(cm.br, tango_browser_cur(cm.br)), "Game (E).nes")) tango_browser_next(cm.br);
    ZLOADS = 0;
    CHECK(tango_core_event(&cm, TANGO_EV_SELECT) == TANGO_CM_CLOSED_NOW);
    CHECK(ZLOADS == 1);
    CHECKS(LAST_PATH, "games/NES/multi.zip");
    CHECKS(LAST_MEMBER, "Game (E).nes");

    /* ---- 6. a zip that fails to open (zipfs refuses it): a message, not a crash, the real folder stays ---- */
    {
        tango_zipfs_t badzip = { mz_open, mz_read, mz_close, NULL };
        N_BLOBS = 0;                         /* mz_open now finds nothing, for any path */
        tango_core_platform_t pl2 = { .fs = &fs, .zipfs = &badzip, .load_file = cm_load_file,
                                      .load_zip_file = cm_load_zip_file };
        tango_core_init(&cm, &link, &osd, &br, &pl2, r.confstr, NULL);
        open_f_browser(&cm, &osd);
        while (strcmp(tango_browser_name(cm.br, tango_browser_cur(cm.br)), "single.zip")) tango_browser_next(cm.br);
        CHECK(tango_core_event(&cm, TANGO_EV_SELECT) == TANGO_CM_REDRAWN);
        CHECK(cm.in_zip == 0);               /* refused: never entered */
        CHECK(cm.mode == TANGO_CM_FILES);    /* the real folder's listing is still what's shown */
        N_BLOBS = (int)(sizeof BLOB_TABLE / sizeof BLOB_TABLE[0]);
    }

    /* ---- 7. a platform with no zip support at all (zipfs == NULL): .zip files are not even offered in the
     * listing (cm->f_exts never gets "ZIP" appended, tango_coremenu.c's CS_FILE case) -- not just refused if
     * picked, genuinely absent, exactly like the documented behaviour (menu-tango docs/LAUNCHER.md "File list limits":
     * "`.zip` files are not opened... Copy games unzipped") for a platform that opts out entirely. A real ROM
     * in the same folder still works normally. ---- */
    {
        tango_core_platform_t pl3 = { .fs = &fs, .zipfs = NULL, .load_file = cm_load_file,
                                      .load_zip_file = cm_load_zip_file };
        tango_core_init(&cm, &link, &osd, &br, &pl3, r.confstr, NULL);
        open_f_browser(&cm, &osd);
        tango_core_page(&cm, &p);
        CHECK(p.nrows == 2);     /* ".." + real.nes only: single.zip/multi.zip never matched "NES" alone */
        for (int i = 0; i < p.nrows; i++) {
            CHECK(!p.rows[i].label || (strcmp(p.rows[i].label, "single.zip") && strcmp(p.rows[i].label, "multi.zip")));
        }
        while (strcmp(tango_browser_name(cm.br, tango_browser_cur(cm.br)), "real.nes")) tango_browser_next(cm.br);
        LOADS = 0;
        CHECK(tango_core_event(&cm, TANGO_EV_SELECT) == TANGO_CM_CLOSED_NOW);
        CHECK(LOADS == 1);
        CHECKS(LAST_PATH, "/sd/games/NES/real.nes");
    }

    printf("%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
