// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 the Tango authors
/*
 * tango_browser.h -- the launcher's Cores browser: MiSTer's file list, read from Main_MiSTer source.
 *
 * Tango. Our own code, written from MiSTer's behaviour (Main_MiSTer master):
 *   file_io.cpp ScanDirectory 1720-1790   what is listed with SCANO_CORES:
 *       folders only if the name starts with '_' (plus ".." below the root); "System Volume Information",
 *       dot-folders and dot-files hidden; "menu.rbf" and "menu_20*" hidden; boot.rom/bootN.rom hidden;
 *       files only with the core extension (MiSTer RBF/MRA/MGL; Tango .fs, .bin and .mra;
 *       no MGL yet)
 *   file_io.cpp get_display_name 1407-1426   core name = file name minus extension; "_20" + at least
 *       6 more characters splits off the date code (else "------")
 *   file_io.cpp DirentComp 1334-1370   ".." first, folders before files, case-insensitive name,
 *       (ignoring a 4-character ".xxx" tail), shorter first on a common prefix, then date code
 *   file_io.cpp 1880-1916, 70-124   Up/Down wrap around; Left/Right (PgUp/PgDn) move one page and clamp;
 *       flist_center_selected keeps the window (no lookahead)
 *   menu.cpp 389-429 changeDir     entering a folder selects entry 0; ".." re-selects the folder left
 *   menu.cpp 8450-8488             "_Folder" shown as "Folder"; date code " yy.mm.dd" on the right
 *   menu.cpp 8505                  empty list: "No files!"
 * Differences, all Tango's: the file list holds at most TANGO_BROWSE_MAX entries; names are drawn with
 * the proportional font, so a name is cut by pixels, not at 27 characters; there is NO detail line (MiSTer has none): a long name is trimmed at its end
 * with a marker, the selected row scrolls in place and a file without a date shows its end on the row below (menu.cpp PrintDirectory 8445-8600,
 * ScrollLongName 8399-8433; osd.cpp ScrollText 610-656).  Filter-by-typing, +/- next letter, Home / End / Tab are below (menu.cpp 5558-5668).
 */
#ifndef TANGO_BROWSER_H
#define TANGO_BROWSER_H

#include <stdint.h>
#include "tango_layout.h"

#define TANGO_BROWSE_MAX   400
#define TANGO_BROWSE_POOL  (24 * 1024)
#define TANGO_PATH_MAX     256

/* a directory source: the firmware wraps FatFs, the host tests a fake card */
typedef struct {
    int  (*open)(void *ctx, const char *path);                        /* path relative to the card root; "" = root */
    int  (*read)(void *ctx, char *name, int cap, int *is_dir);        /* 1 = an entry, 0 = end */
    void (*close)(void *ctx);
    void  *ctx;
} tango_fs_t;

typedef struct {
    uint16_t name;       /* pool offset of the entry's text: the file name as on the card, stored once */
    uint16_t alt_len;    /* MiSTer's altname (display/sort name) is always a prefix of the file name (the name,
                          * or it cut at its extension / "_20" date code): its length. When it is shorter than
                          * the name (split = 1), the pool holds  altname 0 rest-of-name 0 , so the altname is
                          * the text at `name` and the file name is the two parts joined (tango_browser_name). */
    char     date[16];   /* MiSTer's datecode ("" for folders) */
    uint8_t  is_dir, split;
} tango_dirent_t;

typedef struct {
    char            path[TANGO_PATH_MAX];      /* MiSTer's selPath: "" or "_Console" or "_Console/_Sub" */
    int             n, sel, first;             /* entries, iSelectedEntry, iFirstEntry */
    int             truncated;                 /* the folder had more entries than fit (TANGO_BROWSE_MAX or the pool) */
    int             error;                     /* the folder could not be read */
    tango_dirent_t  e[TANGO_BROWSE_MAX];
    uint16_t        order[TANGO_BROWSE_MAX];   /* sorted index into e[] */
    char            pool[TANGO_BROWSE_POOL];
    unsigned        pool_used;
    /* page model built by tango_browser_page() */
    tango_row_t     rows[TANGO_BROWSE_MAX + 1];   /* + the "list cut short" row */
    char            cut_text[40], cut_hint[24];   /* "... list cut short at N", "sel/N+" */
    char            disp[TANGO_BROWSE_MAX][10];/* " yy.mm.dd" date text */
    /* files mode (a core's F entry: SelectFile(..., ext, SCANO_DIR, ...), menu.cpp 2430-2445) */
    uint8_t         files;                     /* 0 = Cores (SCANO_CORES), 1 = files (SCANO_DIR) */
    uint8_t         nodirs;                    /* files mode 2: no folders, no ".." (ScanDirectory with options 0, as
                                                * flist_GetPrevNext scans: file_io.cpp 1752-1756, 2108) */
    char            exts[64];                  /* MiSTer's extension list, 3-character groups ("NESFDS", "BIN") */
    /* typing (menu.cpp 5642-5668): the first key jumps, keys within 2 s filter */
    char            filter[64];
    uint32_t        filter_ms;
    int             filter_on;
    uint8_t         filtered;                  /* the list on show is a typed filter's result (2+ characters within 2 s) */
    /* the selected row's long name scrolls in place (MiSTer ScrollLongName, menu.cpp 8399-8433): set by tango_browser_page */
    int16_t         scroll_sel;                /* the entry the scroll state is for */
    uint8_t         scroll_ok;                 /* that row's name does not fit: it scrolls once the delay is over */
    uint8_t         scroll_act;                /* scrolling has started */
    uint8_t         scroll_pend;               /* a new selection: tango_browser_scroll_tick stamps its time */
    uint16_t        scroll_off, scroll_len;    /* the first character of the window of "name, 10 blanks, name"; the name's length */
    uint32_t        scroll_t;                  /* when the window last moved / the selection was stamped */
} tango_browser_t;

/* ScanDirectory(path, SCANF_INIT): reads and sorts the folder; selection 0. Returns the entry count. */
int  tango_browser_scan(tango_browser_t *b, const tango_fs_t *fs, const char *path);

/* changeDir(name): into a folder (d_name) or ".." up; returns 1 if the folder changed */
int  tango_browser_chdir(tango_browser_t *b, const tango_fs_t *fs, const char *name);

void tango_browser_next(tango_browser_t *b);        /* SCANF_NEXT (wraps) */
void tango_browser_prev(tango_browser_t *b);        /* SCANF_PREV (wraps) */
void tango_browser_next_page(tango_browser_t *b);   /* SCANF_NEXT_PAGE */
void tango_browser_prev_page(tango_browser_t *b);   /* SCANF_PREV_PAGE */
void tango_browser_select(tango_browser_t *b, int i);
/* the file called `name` (the file name as on the card, any case) selected; 0 = not in the list.  ScanDirectory(SCANF_SET_ITEM) for a Cores list that opens on the running core (menu.cpp 450-459) */
int  tango_browser_select_file(tango_browser_t *b, const char *name);

/* the selected entry (NULL if none) and its names */
const tango_dirent_t *tango_browser_cur(const tango_browser_t *b);
const char *tango_browser_name(const tango_browser_t *b, const tango_dirent_t *e);   /* d_name (valid until the
                                                                                       second next call) */
const char *tango_browser_alt(const tango_browser_t *b, const tango_dirent_t *e);    /* altname */

/* builds the Cores page (title "Cores", legend, rows with MiSTer's display names) into *p */
void tango_browser_page(tango_browser_t *b, tango_page_t *p, const char *banner,
                        const tango_legend_t *legend, int nlegend);

/* A typed character (GetASCIIKey) at now_ms, as MiSTer's file selector (menu.cpp 5642-5668, file_io.cpp 1665-1678,
 * 2008-2040): after 2 s without typing, the list is read again and the cursor jumps to the next entry whose name
 * starts with the character (0-9, A-Z); within 2 s, the character is added to the filter and the list shows only
 * the entries whose file name contains it (case-insensitive). Returns 1 if the page changed. */
int  tango_browser_type(tango_browser_t *b, const tango_fs_t *fs, char ch, uint32_t now_ms);

/* MiSTer's file-list keys beyond the arrows (menu.cpp 5558-5650, file_io.cpp 1895-2000; Main_MiSTer c3dc8ed):
 *   Home / Tab   rescan from the top: the selection goes to entry 0 (Tab also takes ".." when that is the entry, i.e. goes up)
 *   End          the last entry
 *   Backspace    clear the typed filter and rescan from the top
 *   plus / minus (SCANF_NEXT_CHAR / SCANF_PREV_CHAR) the next entry whose initial letter (folders and files apart) differs /
 *                the first entry of the previous letter; in a Cores list a leading "_" of a folder is skipped
 * Each returns 1 if the page changed. tango_browser_up is Tab as the launcher and the file lists use it: one folder up, or,
 * at the root (no ".." entry), the top. */
int  tango_browser_home(tango_browser_t *b, const tango_fs_t *fs);
void tango_browser_end(tango_browser_t *b);
int  tango_browser_clear_filter(tango_browser_t *b, const tango_fs_t *fs);
int  tango_browser_tab(tango_browser_t *b, const tango_fs_t *fs);
int  tango_browser_next_char(tango_browser_t *b);
int  tango_browser_prev_char(tango_browser_t *b);

/* the scroll step: 1 if the window moved (the caller redraws); wait_ms = ms until the next one, -1 = nothing scrolls */
int  tango_browser_scroll_tick(tango_browser_t *b, uint32_t now_ms);
int  tango_browser_scroll_wait(const tango_browser_t *b, uint32_t now_ms);

/* Files mode: every folder, files matching exts (3-character groups, '?' and '*' as MiSTer), names shown
 * without their extension when exts has one group; title "Select" (menu.cpp 5517). mode 0 = Cores; mode 2 = files
 * only, no folders (flist_GetPrevNext's scan). */
void tango_browser_set_mode(tango_browser_t *b, int files, const char *exts);
int  tango_ext_match(const char *name, const char *exts);          /* ScanDirectory's extension test */
int  tango_ext_idx(const char *name, const char *exts);            /* user_io_ext_idx (user_io.cpp 4516) */

/* helpers, exported for the unit tests */
int  tango_is_core_file(const char *name);                       /* listed as a core in a Cores folder */
void tango_display_name(const char *name, char *alt, int cap, char date[16]);   /* get_display_name for a core */
int  tango_dirent_less(const char *alt1, const char *date1, int dir1, const char *alt2, const char *date2, int dir2);

#endif
