// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 the Tango authors
/*
 * tango_layout.h -- Tango page layout: a page description in, the overlay's hardware tables out.
 *
 * Tango. Our own code. A C port of the OSD model, line for line:
 *   menu-tango tools/osd-model/pages.py   render_page()        which row gets which text, bar, rule, icon
 *   menu-tango tools/osd-model/zosd.py    Menu.text/text_r/bar/rule/banner_text   glyph placement by advance width
 *   menu-tango tools/osd-model/export_hw.py build_tables()     glyph list + row table words
 * The font metrics and constants are generated from the model (tango_font_data.c, tango_model.h),
 * so a page laid out here gives byte-identical tables to export_hw.py (proved by the host tests).
 * The model places glyphs by advance width only (no kerning pairs), and so does this code.
 */
#ifndef TANGO_LAYOUT_H
#define TANGO_LAYOUT_H

#include <stdint.h>
#include "tango_font.h"

#define TANGO_GLYPH_SLOTS  1024  /* glyph list entries per bank (sys/tango_menu_overlay.v glist, 2 banks) */
#define TANGO_ROW_WORDS    64    /* row table words per bank (32 rows x 64 bits) */
#define TANGO_ROW_MAXG     127   /* count field is 7 bits */

/* row kinds: pages.render_page's row types */
typedef enum {
    TR_BLANK = 0,   /* ('blank',)          nothing */
    TR_SEP,         /* ('sep',)            divider rule */
    TR_CENTER,      /* ('center', text[, col])  centred text, never selected */
    TR_UP,          /* ('up', '..')        up icon + text */
    TR_DIR,         /* ('dir', name[, v])  folder icon + text, chevron right */
    TR_FILE,        /* ('file', name[, v]) text, value right */
    TR_SUB,         /* ('sub', label[, v]) text, chevron right */
    TR_ACT,         /* ('act', label)      text */
    TR_OPT,         /* ('opt', label, v)   text, value right */
    TR_INFO,        /* ('info', text)      dim text */
    TR_DIS,         /* ('dis', label[, v]) dim text */
    TR_VOL,         /* ('vol', label, n, max): text, a volume bar right (glyphs 0x84 / 0x85 in the value, accent colour) */
    TR_LOGO,        /* ('logo', pattern): the About wordmark: each '#' of label is one 12 px cell (three TANGO_ICON_BRICK), the row centred, accent colour */
    TR_SCROLL       /* ('scroll', text, pos): a ticker: text starts at VX - tango_page_t.scroll and moves left; only whole glyphs inside [TX, VX] are placed */
} tango_rowkind_t;

typedef struct {
    uint8_t     kind;     /* tango_rowkind_t */
    uint8_t     col;      /* TR_CENTER colour; 0 = TANGO_C_DIM (the model's default) */
    const char *label;
    const char *value;    /* NULL or "" = none */
} tango_row_t;     /* 12 bytes: the browser keeps hundreds of them in RAM, so no field is added here (heap budget, build.sh) */

typedef struct {
    const char *key;      /* drawn in TANGO_C_ACC */
    const char *label;    /* drawn in TANGO_C_DIM */
} tango_legend_t;

enum { TANGO_ARROW_NONE = 0, TANGO_ARROW_LEFT, TANGO_ARROW_RIGHT };

typedef struct {
    const char           *banner;   /* rotated text on the strip, e.g. "TANG CONSOLE" */
    const char           *title;
    const char           *right;    /* title-row right text, or NULL */
    uint8_t               arrow;    /* TANGO_ARROW_*: left = spec 'back' or arrow 'left' */
    int                   sel;
    int                   top;      /* first visible row; < 0 = the model's own formula */
    const tango_row_t    *rows;
    int                   nrows;
    const tango_legend_t *legend;
    int                   nlegend;
    int                   sel_scroll; /* the selected list row's name scrolls: 0 = not yet (trimmed with a marker), n > 0 = from character n - 1 (MiSTer's ScrollText) */
    int                   scroll;   /* the position (pixels) of the page's TR_SCROLL row, if it has one (tango_about.h) */
} tango_page_t;

/* the tables one bank holds, exactly as export_hw.build_tables() returns them */
typedef struct {
    uint32_t glyphs[TANGO_GLYPH_SLOTS];
    uint16_t nglyphs;
    uint32_t rows[TANGO_ROW_WORDS];  /* word 2r = entry[31:0], word 2r+1 = entry[63:32]; row 31 = banner */
    uint16_t dropped;                /* entries shadowed by a later one at the same x (not written) */
    uint16_t clipped;                /* glyphs left out because a row or the list was full */
} tango_tables_t;

/* Lays out a page. Returns 0, or -1 if the page could not be laid out at all. */
int tango_layout_page(const tango_page_t *p, tango_tables_t *t);

/* 1 if the list row (TR_FILE / TR_DIR / TR_UP) is too long for its row and its name has to scroll when selected (a file without a date shows its end on the
 * row below first and scrolls only if the two rows do not hold it): the browser's scroll timer asks this */
int tango_list_row_scrolls(const tango_row_t *row);

/* Width of a string in pixels (sum of advances), as zosd.Font.width. */
int tango_text_width(const char *s);
/* the same for the first n bytes of s */
int tango_text_width_n(const char *s, int n);

/* Glyph-list entry, as export_hw.gent(): x | colbase << 11 | width << 22 | colour << 27 */
uint32_t tango_glyph_entry(int x, uint8_t ch, uint8_t col);

#endif
