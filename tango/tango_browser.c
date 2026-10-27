// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 the Tango authors
/*
 * tango_browser.c -- see tango_browser.h for the MiSTer source lines each rule comes from.
 * Tango.
 *
 * Derived from Main_MiSTer (https://github.com/MiSTer-devel/Main_MiSTer, GPL-3.0; compared at c3dc8ed): these
 * functions copy or closely paraphrase its code, so this file is GPL-3.0-or-later with the whole firmware
 * (LICENSE-GPL-3.0, NOTICE):
 *   tango_ext_match         file_io.cpp ScanDirectory, the extension test (1793-1820; lines copied)
 *   tango_ext_idx           user_io.cpp user_io_ext_idx (4562-4601; lines copied)
 *   tango_browser_chdir     menu.cpp changeDir (394-434; partly verbatim), with ScanDirectory SCANF_SET_ITEM
 *                           (file_io.cpp 1931-1952) as its loop
 *   tango_dirent_less       file_io.cpp DirentComp (1348-1386)
 *   tango_display_name      file_io.cpp get_display_name (from 1422)
 *   tango_is_core_file      file_io.cpp ScanDirectory, the skip rules for files (1758-1772)
 *   tango_browser_type      file_io.cpp ScanDirectory, the key-jump branch (2023-2057)
 *   tango_browser_next_char, tango_browser_prev_char
 *                           file_io.cpp ScanDirectory SCANF_NEXT_CHAR, SCANF_PREV_CHAR (1953-2022)
 * Upstream notices: menu.cpp's header reads "Copyright 2005, 2006, 2007 Dennis van Weeren" and "Copyright 2008, 2009
 * Jakub Bednarski" (GPL version 3 or later); file_io.cpp and user_io.cpp have no copyright or licence header (the
 * repository's LICENSE is GPL-3.0).
 */
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <strings.h>
#include <stdlib.h>
#include "tango_browser.h"

static int lc(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
static int ncasecmp(const char *a, const char *b, size_t n)      /* strncasecmp */
{
    for (size_t i = 0; i < n; i++) {
        int x = lc((unsigned char)a[i]), y = lc((unsigned char)b[i]);
        if (x != y) return x - y;
        if (!x) return 0;
    }
    return 0;
}
static int casecmp(const char *a, const char *b) { return ncasecmp(a, b, (size_t)-1); }
static int ends_with(const char *s, const char *ext)
{
    size_t n = strlen(s), m = strlen(ext);
    return n >= m && !casecmp(s + n - m, ext);
}

/* the Tango core extensions: MiSTer lists RBF/MRA/MGL; our bitstreams are .fs (Gowin) and .bin (raw).
 * .mra lists like a core too: picking one resolves to a bitstream under _Arcade/cores/
 * and launches that, exactly like picking an .fs/.bin directly -- tango_launcher.c's load_core() doesn't
 * care which extension it was handed, the resolution happens in the platform's loader (tango_fw.c). */
static int core_ext_len(const char *name)
{
    if (ends_with(name, ".fs")) return 3;
    if (ends_with(name, ".bin")) return 4;
    if (ends_with(name, ".mra")) return 4;
    return 0;
}

int tango_is_core_file(const char *name)
{
    if (name[0] == '.') return 0;                                   /* skip hidden files */
    /* skip non-selectable files: "menu.rbf" and "menu_20*" -- ours: menu.fs / menu.bin */
    if (!casecmp(name, "menu.fs") || !casecmp(name, "menu.bin")) return 0;
    if (!ncasecmp(name, "menu_20", 7)) return 0;
    if (!ncasecmp(name, "boot", 4)) {                               /* boot.rom, boot0.rom .. boot9.rom */
        size_t len = strlen(name);
        if ((len == 8 || (len == 9 && name[4] >= '0' && name[4] <= '9')) && ends_with(name, ".rom")) return 0;
    }
    return core_ext_len(name) > 0;
}

void tango_display_name(const char *name, char *alt, int cap, char date[16])
{
    /* memcpy(altname, d_name); for a core: cut the extension, then "_20" + >= 6 characters = date code */
    int len = (int)strlen(name) - core_ext_len(name);
    if (len > cap - 1) len = cap - 1;
    memcpy(alt, name, (size_t)len); alt[len] = 0;
    char *p = strstr(alt, "_20");
    if (p && strlen(p + 3) < 6) p = NULL;
    if (p) { *p = 0; strncpy(date, p + 3, 15); date[15] = 0; }
    else strcpy(date, "------");
}

int tango_dirent_less(const char *alt1, const char *date1, int dir1, const char *alt2, const char *date2, int dir2)
{
    /* DirentComp (file_io.cpp 1334-1370) */
    if (dir1 && !strcmp(alt1, "..")) return 1;
    if (dir2 && !strcmp(alt2, "..")) return 0;
    if (dir1 && !dir2) return 1;
    if (!dir1 && dir2) return 0;
    int len1 = (int)strlen(alt1), len2 = (int)strlen(alt2);
    if (len1 > 4 && alt1[len1 - 4] == '.') len1 -= 4;
    if (len2 > 4 && alt2[len2 - 4] == '.') len2 -= 4;
    int len = len1 < len2 ? len1 : len2;
    int ret = ncasecmp(alt1, alt2, (size_t)len);
    if (!ret) {
        if (len1 != len2) return len1 < len2;
        ret = casecmp(date1, date2);
    }
    return ret < 0;
}

static const char *alt_of(const tango_browser_t *b, const tango_dirent_t *e) { return b->pool + e->name; }
static const tango_browser_t *sort_b;
static int cmp_entries(const void *pa, const void *pb)
{
    const tango_dirent_t *a = &sort_b->e[*(const uint16_t *)pa], *b = &sort_b->e[*(const uint16_t *)pb];
    const char *aa = alt_of(sort_b, a), *ba = alt_of(sort_b, b);
    if (tango_dirent_less(aa, a->date, a->is_dir, ba, b->date, b->is_dir)) return -1;
    if (tango_dirent_less(ba, b->date, b->is_dir, aa, a->date, a->is_dir)) return 1;
    char na[TANGO_PATH_MAX]; strncpy(na, tango_browser_name(sort_b, a), sizeof na - 1); na[sizeof na - 1] = 0;
    return strcmp(na, tango_browser_name(sort_b, b));             /* MiSTer: unspecified; we pick one */
}

/* the name once: "name 0", or when the altname (a prefix of it, alt_len bytes) is shorter: "alt 0 rest 0" */
static int pool_add(tango_browser_t *b, const char *name, size_t alt_len, tango_dirent_t *e)
{
    size_t l = strlen(name), need = l + 1 + (alt_len < l ? 1 : 0);
    if (b->pool_used + need > sizeof b->pool) return 0;
    char *p = b->pool + b->pool_used;
    if (alt_len < l) {
        memcpy(p, name, alt_len); p[alt_len] = 0;
        memcpy(p + alt_len + 1, name + alt_len, l - alt_len + 1);
        e->split = 1;
    } else { memcpy(p, name, l + 1); e->split = 0; alt_len = l; }
    e->name = (uint16_t)b->pool_used; e->alt_len = (uint16_t)alt_len;
    b->pool_used += (unsigned)need;
    return 1;
}

void tango_browser_set_mode(tango_browser_t *b, int files, const char *exts)
{
    b->filter_on = 0; b->filter[0] = 0;                 /* SelectFile: filter_typing_timer = 0, filter = "" (440-441) */
    b->files = (uint8_t)(files != 0);
    b->nodirs = (uint8_t)(files == 2);
    strncpy(b->exts, exts ? exts : "", sizeof b->exts - 1); b->exts[sizeof b->exts - 1] = 0;
}

/* ScanDirectory (file_io.cpp 1770-1800): the text after the last '.' against each 3-character group;
 * a group padded with spaces is shorter; '*' matches the rest, '?' any one character */
int tango_ext_match(const char *name, const char *exts)
{
    const char *fext = strrchr(name, '.');
    if (!fext) return 0;
    fext++;
    const char *ext = exts;
    while (*ext) {
        char e[4]; memcpy(e, ext, 3); e[3] = 0;
        for (int i = 0; i < 3; i++) if (!ext[i]) { e[i] = 0; break; }
        if (e[2] == ' ') { e[2] = 0; if (e[1] == ' ') e[1] = 0; }
        int found = 1;
        for (int i = 0; i < 4; i++) {
            if (e[i] == '*') break;
            if (e[i] == '?' && fext[i]) continue;
            if (lc((unsigned char)e[i]) != lc((unsigned char)fext[i])) found = 0;
            if (!e[i] || !found) break;
        }
        if (found) return 1;
        if (strlen(ext) < 3) break;
        ext += 3;
    }
    return 0;
}

int tango_ext_idx(const char *name, const char *exts)
{
    const char *p = strrchr(name, '.');
    if (!p) return 0;
    p++;
    char e[4] = "   ";
    for (int i = 0; i < 3 && *p; i++) e[i] = *p++;
    int idx = 0;
    for (const char *ext = exts; *ext; ) {
        int found = 1;
        for (int i = 0; i < 3; i++) {
            if (ext[i] == '*') break;
            char a = ext[i] ? ext[i] : ' ';
            if (a != '?' && lc((unsigned char)a) != lc((unsigned char)e[i])) found = 0;
        }
        if (found) return idx;
        if (strlen(ext) <= 3) break;
        idx++;
        ext += 3;
    }
    return 0;
}

static void add_entry(tango_browser_t *b, const char *name, int is_dir)
{
    if (b->n >= TANGO_BROWSE_MAX) { b->truncated = 1; return; }
    tango_dirent_t *e = &b->e[b->n];
    char alt[TANGO_PATH_MAX];
    memset(e, 0, sizeof *e);
    e->is_dir = (uint8_t)is_dir;
    if (is_dir) { strncpy(alt, name, sizeof alt - 1); alt[sizeof alt - 1] = 0; }   /* folders: altname = d_name */
    else if (!b->files) tango_display_name(name, alt, sizeof alt, e->date);
    else {  /* get_display_name for a non-core file (file_io.cpp 1488-1497): keep the extension when the
             * core gave more than one, or a wildcard; else cut it */
        strncpy(alt, name, sizeof alt - 1); alt[sizeof alt - 1] = 0;
        if (strlen(b->exts) <= 3 && !strchr(b->exts, '*') && !strchr(b->exts, '?')) {
            char *dot = strrchr(alt, '.'); if (dot) *dot = 0;
        }
    }
    /* the altname is a prefix of the name (tango_display_name and the extension cut only shorten it) */
    size_t al = strlen(alt);
    if (strncmp(alt, name, al)) al = strlen(name);                 /* not a prefix: cannot happen; keep the name */
    if (!pool_add(b, name, al, e)) { b->truncated = 1; return; }
    b->order[b->n] = (uint16_t)b->n;
    b->n++;
}

/* flist_center_selected (file_io.cpp 100-124), cfg.lookahead = 0 */
static void center(tango_browser_t *b)
{
    if (!b->n) { b->sel = b->first = 0; return; }
    if (b->sel < 0) b->sel = 0;
    if (b->sel >= b->n) b->sel = b->n - 1;
    if (b->sel < b->first) b->first = b->sel;
    else if (b->sel >= b->first + TANGO_VIS) b->first = b->sel - TANGO_VIS + 1;
    int last = b->n + (b->truncated ? 1 : 0) - TANGO_VIS;       /* flist_last_first_entry (+ Tango's cut row) */
    if (last < 0) last = 0;
    if (b->first < 0) b->first = 0;
    if (b->first > last) b->first = last;
    /* Tango: a cut-short list ends with its "list cut short" row; on the last entry the window shows it too */
    if (b->truncated && b->sel == b->n - 1) { int f = b->n + 1 - TANGO_VIS; b->first = f < 0 ? 0 : f; }
}

static const char *scan_filter = NULL;          /* ScanDirectory's filter argument: set only by the typing path */
static int passes_filter(const char *name)      /* file_io.cpp 1665-1678: a case-insensitive substring of d_name */
{
    if (!scan_filter) return 1;
    size_t fl = strlen(scan_filter);
    for (const char *s = name; *s; s++) if (!strncasecmp(s, scan_filter, fl)) return 1;
    return 0;
}

int tango_browser_scan(tango_browser_t *b, const tango_fs_t *fs, const char *path)
{
    if (path != b->path) { strncpy(b->path, path, sizeof b->path - 1); b->path[sizeof b->path - 1] = 0; }
    b->n = b->sel = b->first = 0; b->truncated = 0; b->error = 0; b->pool_used = 0;
    b->filtered = scan_filter != NULL;
    b->scroll_sel = -1;                                       /* a new list: the selection's scroll starts over */
    if (!fs->open(fs->ctx, b->path)) { b->error = 1; return 0; }
    if (b->path[0] && passes_filter("..") && !b->nodirs) add_entry(b, "..", 1);   /* readdir gives ".."; skipped at the root */
    char name[TANGO_PATH_MAX]; int is_dir;
    while (fs->read(fs->ctx, name, sizeof name, &is_dir)) {
        if (!strcmp(name, ".") || !strcmp(name, "..")) continue;
        if (!passes_filter(name)) continue;
        if (is_dir) {
            if (!strcmp(name, "System Volume Information")) continue;
            if (name[0] == '.') continue;                        /* hidden folder */
            if (b->nodirs) continue;                             /* options 0: no folders (1752-1756) */
            if (!b->files && name[0] != '_') continue;           /* SCANO_CORES without SCANO_DIR: "_" folders */
            add_entry(b, name, 1);
        } else if (b->files) {
            if (name[0] == '.') continue;                        /* hidden files */
            if (b->exts[0] && !tango_ext_match(name, b->exts)) continue;
            add_entry(b, name, 0);
        } else if (tango_is_core_file(name)) add_entry(b, name, 0);
    }
    fs->close(fs->ctx);
    sort_b = b;
    qsort(b->order, (size_t)b->n, sizeof b->order[0], cmp_entries);
    return b->n;
}

int tango_browser_chdir(tango_browser_t *b, const tango_fs_t *fs, const char *dir)
{
    char curdir[128] = { 0 };
    if (!dir || !strcmp(dir, "..")) {
        if (!b->path[0]) return 0;
        char *p = strrchr(b->path, '/');
        if (p) { *p = 0; strncpy(curdir, p + 1, sizeof curdir - 1); }
        else { strncpy(curdir, b->path, sizeof curdir - 1); b->path[0] = 0; }
    } else {
        if (strlen(b->path) + strlen(dir) > sizeof b->path - 100) return 0;
        if (b->path[0]) strcat(b->path, "/");
        strcat(b->path, dir);
    }
    tango_browser_scan(b, fs, b->path);
    if (curdir[0]) {                                              /* SCANF_SET_ITEM */
        int pos = -1;
        for (int i = 0; i < b->n; i++) {
            const tango_dirent_t *e = &b->e[b->order[i]];
            if (!e->is_dir) continue;
            if (!strcmp(alt_of(b, e), curdir)) { pos = i; break; }
            if (!casecmp(alt_of(b, e), curdir)) pos = i;
        }
        if (pos >= 0) { b->sel = pos; center(b); }
    }
    return 1;
}

void tango_browser_next(tango_browser_t *b)
{
    if (!b->n) return;
    if (b->sel + 1 < b->n) b->sel++; else b->sel = 0;
    center(b);
}
void tango_browser_prev(tango_browser_t *b)
{
    if (!b->n) return;
    if (b->sel <= 0) b->sel = b->n - 1; else b->sel--;       /* SCANF_PREV at 0 = SCANF_END */
    center(b);
}
void tango_browser_next_page(tango_browser_t *b) { if (b->n) { b->sel += TANGO_VIS; center(b); } }
void tango_browser_prev_page(tango_browser_t *b) { if (b->n) { b->sel -= TANGO_VIS; center(b); } }
void tango_browser_select(tango_browser_t *b, int i) { b->sel = i; center(b); }

int tango_browser_select_file(tango_browser_t *b, const char *name)
{
    for (int i = 0; i < b->n; i++) {
        const tango_dirent_t *e = &b->e[b->order[i]];
        if (e->is_dir || casecmp(tango_browser_name(b, e), name)) continue;
        tango_browser_select(b, i);
        return 1;
    }
    return 0;
}

const tango_dirent_t *tango_browser_cur(const tango_browser_t *b)
{
    return (b->n && b->sel >= 0 && b->sel < b->n) ? &b->e[b->order[b->sel]] : NULL;
}
const char *tango_browser_name(const tango_browser_t *b, const tango_dirent_t *e)
{
    if (!e->split) return b->pool + e->name;
    static char buf[2][TANGO_PATH_MAX]; static int k;          /* two, for a comparison of two names */
    char *d = buf[k ^= 1];
    const char *a = b->pool + e->name, *rest = a + e->alt_len + 1;
    snprintf(d, TANGO_PATH_MAX, "%s%s", a, rest);
    return d;
}
const char *tango_browser_alt(const tango_browser_t *b, const tango_dirent_t *e) { return alt_of(b, e); }

/* display text: characters the font lacks become '?', so names can never draw the icon glyphs */
static char vis_label[TANGO_VIS][TANGO_PATH_MAX];
static void sanitize(char *dst, const char *src, int cap)
{
    int i = 0;
    for (; src[i] && i < cap - 1; i++) { unsigned char c = (unsigned char)src[i]; dst[i] = (c >= 0x20 && c <= 0x7e) ? (char)c : '?'; }
    dst[i] = 0;
}

void tango_browser_page(tango_browser_t *b, tango_page_t *p, const char *banner,
                        const tango_legend_t *legend, int nlegend)
{
    memset(p, 0, sizeof *p);
    p->banner = banner; p->title = b->files ? "Select" : "Cores";   /* OsdSetTitle(SCANO_CORES ? "Cores" : "Select") */
    p->legend = legend; p->nlegend = nlegend;
    p->rows = b->rows;
    if (!b->n) {                                                  /* menu.cpp 8505 */
        b->rows[0].kind = TR_CENTER; b->rows[0].col = 0;
        b->rows[0].label = b->error ? "No SD card" : "No files!"; b->rows[0].value = NULL;
        p->nrows = 1; p->sel = 0; p->top = 0;
        return;
    }
    for (int i = 0; i < b->n; i++) {
        const tango_dirent_t *e = &b->e[b->order[i]];
        tango_row_t *r = &b->rows[i];
        const char *alt = alt_of(b, e);
        r->col = 0; r->value = NULL;
        if (e->is_dir && !strcmp(alt, "..")) { r->kind = TR_UP; r->label = ".."; }
        else if (e->is_dir) { r->kind = TR_DIR; r->label = (!b->files && alt[0] == '_') ? alt + 1 : alt; }
        else {
            r->kind = TR_FILE; r->label = alt;
            if (e->date[0]) {                                     /* " yy.mm.dd" (menu.cpp 8470-8488), no lead space */
                char *d = b->disp[i];
                d[0] = e->date[0]; d[1] = e->date[1]; d[2] = '.'; d[3] = e->date[2]; d[4] = e->date[3];
                d[5] = '.'; d[6] = e->date[4]; d[7] = e->date[5]; d[8] = 0;
                for (int k = 0; k < 8; k++) { unsigned char c = (unsigned char)d[k]; if (c < 0x20 || c > 0x7e) d[k] = '?'; }
                r->value = d;
            }
        }
        if (i >= b->first && i < b->first + TANGO_VIS) {
            sanitize(vis_label[i - b->first], r->label, TANGO_PATH_MAX);
            r->label = vis_label[i - b->first];
        }
    }
    p->nrows = b->n; p->sel = b->sel; p->top = b->first;
    /* the selected row's long name (ScrollLongName): scrolls after SCROLL_START_MS when it does not fit; a new selection starts it over */
    if (b->scroll_sel != b->sel) { b->scroll_sel = (int16_t)b->sel; b->scroll_act = 0; b->scroll_off = 0; b->scroll_pend = 1; }
    b->scroll_ok = (uint8_t)tango_list_row_scrolls(&b->rows[b->sel]);
    b->scroll_len = (uint16_t)strlen(b->rows[b->sel].label);
    if (!b->scroll_ok) b->scroll_act = 0;
    p->sel_scroll = b->scroll_act ? b->scroll_off + 1 : 0;
    /* Tango only (MiSTer's list is unbounded: file_io.cpp DirItem is a std::vector): the folder had more entries
     * than fit, so a last, dim, never-selected row says so -- where the missing names would have been */
    if (b->truncated) {
        snprintf(b->cut_text, sizeof b->cut_text, "... list cut short at %d", b->n);
        tango_row_t *r = &b->rows[b->n];
        r->kind = TR_INFO; r->col = 0; r->label = b->cut_text; r->value = NULL;
        p->nrows = b->n + 1;
        /* and the title's position hint counts the entries only, with a "+": there are more on the card */
        snprintf(b->cut_hint, sizeof b->cut_hint, "%d/%d+", b->sel + 1, b->n);
        p->right = b->cut_hint;
    }
}

int tango_browser_type(tango_browser_t *b, const tango_fs_t *fs, char ch, uint32_t now_ms)
{
    if (ch <= 1) return 0;
    if (!b->filter_on || now_ms - b->filter_ms >= 2000) {       /* CheckTimer(filter_typing_timer) */
        b->filter[0] = ch; b->filter[1] = 0;
        tango_browser_scan(b, fs, b->path);                     /* SCANF_INIT: no filter, selection 0 */
        int m = toupper((unsigned char)ch);
        if ((m >= '0' && m <= '9') || (m >= 'A' && m <= 'Z')) {  /* ScanDirectory(path, key) 2008-2040 */
            int found = -1;
            for (int i = b->sel + 1; i < b->n && found < 0; i++)
                if (toupper((unsigned char)alt_of(b, &b->e[b->order[i]])[0]) == m) found = i;
            for (int i = 0; i < b->n && found < 0; i++)
                if (toupper((unsigned char)alt_of(b, &b->e[b->order[i]])[0]) == m) found = i;
            if (found >= 0) tango_browser_select(b, found);
        }
    } else {
        size_t l = strlen(b->filter);
        if (l < sizeof b->filter - 1) { b->filter[l] = ch; b->filter[l + 1] = 0; }
        scan_filter = b->filter;                                /* ScanDirectory(..., NULL, filter) */
        tango_browser_scan(b, fs, b->path);
        scan_filter = NULL;
    }
    b->filter_on = 1; b->filter_ms = now_ms;
    return 1;
}

/* ---------------------------------------------------------------- Home, End, Tab, Backspace, plus / minus */
int tango_browser_home(tango_browser_t *b, const tango_fs_t *fs)
{
    b->filter_on = 0;                                          /* filter_typing_timer = 0 */
    tango_browser_scan(b, fs, b->path);                        /* ScanDirectory(SCANF_INIT): selection 0 */
    return 1;
}

void tango_browser_end(tango_browser_t *b)
{
    b->filter_on = 0;
    if (b->n) tango_browser_select(b, b->n - 1);               /* SCANF_END */
}

int tango_browser_clear_filter(tango_browser_t *b, const tango_fs_t *fs)
{
    b->filter[0] = 0;
    return tango_browser_home(b, fs);                          /* KEY_BACKSPACE: filter[0] = 0, then SCANF_INIT */
}

int tango_browser_tab(tango_browser_t *b, const tango_fs_t *fs)
{
    tango_browser_home(b, fs);
    const tango_dirent_t *e = tango_browser_cur(b);
    if (e && e->is_dir && !strcmp(alt_of(b, e), "..")) tango_browser_chdir(b, fs, "..");   /* select = Tab on ".." */
    return 1;
}

/* the initial letter MiSTer compares: altname[0], a folder's "_" skipped in a Cores list (file_io.cpp 1953-1973) */
static int initial(const tango_browser_t *b, const tango_dirent_t *e)
{
    const char *a = alt_of(b, e);
    int c = (unsigned char)a[0];
    if (c == '_' && e->is_dir && !b->files) c = (unsigned char)a[1];
    return toupper(c);
}

int tango_browser_next_char(tango_browser_t *b)
{
    if (!b->n) return 0;
    const tango_dirent_t *cur = &b->e[b->order[b->sel]];
    int c = initial(b, cur);
    for (int i = b->sel + 1; i < b->n; i++) {
        const tango_dirent_t *t = &b->e[b->order[i]];
        if (initial(b, t) != c || t->is_dir != cur->is_dir) { tango_browser_select(b, i); return 1; }
    }
    return 1;                                                  /* the end of the list: nothing moves */
}

int tango_browser_prev_char(tango_browser_t *b)
{
    if (!b->n) return 0;
    const tango_dirent_t *cur = &b->e[b->order[b->sel]];
    int c = initial(b, cur), saw_change = 0;
    for (int i = b->sel - 1; i >= 0; i--) {
        const tango_dirent_t *t = &b->e[b->order[i]];
        if (initial(b, t) != c || t->is_dir != cur->is_dir) {
            if (saw_change) { tango_browser_select(b, i + 1); return 1; }
            saw_change = 1;
            const char *a = alt_of(b, t);
            c = (unsigned char)a[0]; if (c == '_') c = (unsigned char)a[1];       /* (as MiSTer: no directory test here) */
            c = toupper(c);
        }
    }
    return 1;
}

/* ---------------------------------------------------------------- the selected row's long name (MiSTer ScrollLongName / ScrollText) */
#define SCROLL_START_MS 1000      /* SCROLL_DELAY, osd.cpp 126: the delay after a new selection */
#define SCROLL_STEP_MS  120       /* MiSTer moves 1 pixel every 10 ms, a character (8 px) in 80 ms; Tango's proportional characters are about 1.5 times wider */
#define BLANKSPACE      10        /* osd.cpp 614 */

int tango_browser_scroll_wait(const tango_browser_t *b, uint32_t now_ms)
{
    if (!b->scroll_ok) return -1;
    if (b->scroll_pend) return 0;                                  /* tick stamps the selection's time */
    uint32_t due = b->scroll_t + (b->scroll_act ? SCROLL_STEP_MS : SCROLL_START_MS);
    int32_t d = (int32_t)(due - now_ms);
    return d > 0 ? d : 0;
}

int tango_browser_scroll_tick(tango_browser_t *b, uint32_t now_ms)
{
    if (!b->scroll_ok) return 0;
    if (b->scroll_pend) { b->scroll_pend = 0; b->scroll_t = now_ms; return 0; }     /* the selection was made just now */
    if ((int32_t)(now_ms - b->scroll_t) < (b->scroll_act ? SCROLL_STEP_MS : SCROLL_START_MS)) return 0;
    b->scroll_t = now_ms;
    if (!b->scroll_act) { b->scroll_act = 1; b->scroll_off = 0; }
    else if (++b->scroll_off >= b->scroll_len + BLANKSPACE) b->scroll_off = 0;      /* the name and 10 blanks, then again (ScrollText 641) */
    return 1;
}
