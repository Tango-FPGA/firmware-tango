// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 the Tango authors
/*
 * tango_layout.c -- see tango_layout.h. Tango, our own code.
 * Each block names the model line it reproduces.
 */
#include <string.h>
#include "tango_layout.h"

#define NROWS_BUILD  18      /* rows the page model uses: 0 title .. 17 legend */
#define ROW_CAP      160     /* glyphs a row may collect before sorting */
#define BANNER_CAP   64

typedef struct { int16_t x; uint8_t ch, col; } mg_t;
typedef struct {
    uint8_t exists, has_bar, has_rule;
    int16_t bx0, bx1; uint8_t bcol, binset;
    uint8_t rcol, ry;
    uint16_t n;
    mg_t g[ROW_CAP];
} mrow_t;
typedef struct {
    mrow_t row[NROWS_BUILD];
    mg_t banner[BANNER_CAP]; uint16_t nbanner; int bx;
    uint16_t clipped;
} menu_t;

static menu_t M;   /* one page at a time; the firmware lays out from one task */

static uint8_t font_ch(uint8_t ch) { return tango_font_glyph[ch].width ? ch : (uint8_t)'?'; }
static int adv(uint8_t ch) { return tango_font_glyph[font_ch(ch)].adv; }

int tango_text_width(const char *s)
{
    int w = 0;
    for (; *s; s++) w += adv((uint8_t)*s);
    return w;
}

int tango_text_width_n(const char *s, int n)
{
    int w = 0;
    while (n-- > 0 && *s) w += adv((uint8_t)*s++);
    return w;
}

uint32_t tango_glyph_entry(int x, uint8_t ch, uint8_t col)
{
    const tango_glyph_t *g = &tango_font_glyph[font_ch(ch)];
    return (uint32_t)(x & 0x7FF) | (uint32_t)g->colbase << 11 | (uint32_t)g->width << 22 | (uint32_t)(col & 15) << 27;
}

/* zosd.Menu.row(i): creates the row on first touch */
static mrow_t *mrow(int i) { mrow_t *r = &M.row[i]; r->exists = 1; return r; }

/* zosd.Menu.text(i, x, s, col): one entry per character at x, x += advance; returns the end x.
 * Tango addition: a character whose glyph would end past xmax is not placed (long names). */
static int mtext_clip(int i, int x, const char *s, uint8_t col, int xmax)
{
    mrow_t *r = mrow(i);
    for (; *s; s++) {
        uint8_t ch = font_ch((uint8_t)*s);
        int a = adv(ch);
        if (x + a > xmax || x < 0 || x >= 2048 || r->n >= ROW_CAP) { M.clipped++; x += a; continue; }
        r->g[r->n].x = (int16_t)x; r->g[r->n].ch = ch; r->g[r->n].col = col; r->n++;
        x += a;
    }
    return x;
}
static int mtext(int i, int x, const char *s, uint8_t col) { return mtext_clip(i, x, s, col, 2047); }
/* pages.fit(font, s, px, g): s cut to at most g glyphs and px pixels. A cut keeps a prefix and a suffix
 * joined by "...", weighted ~60/40 front/back (file-manager convention, e.g. Windows Explorer's path
 * compaction), not just a longer end-only cut -- e.g. "Teenage Mutant N...rnament Fighters". A list row
 * may use TANGO_ROW_GLYPHS glyph slots (label + right-hand text + icon): 14 x 67 = 938 of a bank's 1024
 * (pages.G_ROW) -- MiSTer cuts a long name at a fixed width too (menu.cpp PrintDirectory 8443-8447,
 * 8492-8495), but end-only; the wider Tango budget and real-card name-collision data (measured on a card:
 * ~26 visible characters collides 6.4% of the time, 64 collides 0.08%) are why Tango keeps both ends. */
#define TANGO_ROW_GLYPHS 67
#define TANGO_FIT_MAX    256
/* width in pixels of the first n bytes of s (tango_text_width bounded to n instead of strlen) */
static int width_n(const char *s, int n) { int w = 0; while (n-- > 0) w += adv((uint8_t)*s++); return w; }
static const char *fit(char *out, const char *s, int px, int g)
{
    int n = (int)strlen(s);
    if (n <= g && tango_text_width(s) <= px) return s;
    if (n > TANGO_FIT_MAX - 4) n = TANGO_FIT_MAX - 4;
    int ell = tango_text_width("...");
    int gkeep = g - 3; if (gkeep < 0) gkeep = 0;
    int total = gkeep <= n ? gkeep : n;          /* the full string may itself be short but too wide */
    int nf = (total * 6 + 5) / 10, nb = total - nf;
    /* give ground to the glyph cap or the pixel width: shrink the back to 0 first, then the front --
     * the front is the more-recognisable end of a name, so it is protected longer under pressure */
    while ((nf > 0 || nb > 0) &&
           (nf + 3 + nb > g || width_n(s, nf) + ell + width_n(s + n - nb, nb) > px)) {
        if (nb > 0) nb--; else nf--;
    }
    while (nf > 0 && s[nf - 1] == ' ') nf--;                       /* rstrip the front piece */
    while (nb > 0 && s[n - nb] == ' ') nb--;                       /* lstrip the back piece */
    memcpy(out, s, (size_t)nf);
    memcpy(out + nf, "...", 3);
    if (nb) memcpy(out + nf + 3, s + n - nb, (size_t)nb);
    out[nf + 3 + nb] = 0;
    return out;
}
/* ---- list rows: MiSTer's file list (menu.cpp PrintDirectory 8445-8600, ScrollLongName 8399-8433), in Tango's pixels
 *   an entry that does not fit is cut at its END and ends in a marker (MiSTer: a trimmed name with char 22 at column 28; here "...", a Tango row
 *   being pixel-wide); the SELECTED row's long name scrolls in place: after 1 s it moves one character every step and starts over after the
 *   name and 10 blanks (ScrollText, osd.cpp 610-656, BLANKSPACE 10); a file with no date that does not fit (browse_expand, default on) shows
 *   its end on the row below, and scrolls only when the two rows still do not hold the whole name (ScrollLongName: len >= 55). */
#define BLANKSPACE 10
static int fit_end(char *out, const char *s, int px, int g, int *keep)
{
    int n = (int)strlen(s);
    if (n > TANGO_FIT_MAX - 4) n = TANGO_FIT_MAX - 4;
    if (n <= g && width_n(s, n) <= px) { memcpy(out, s, (size_t)n); out[n] = 0; if (keep) *keep = n; return 1; }
    int ell = tango_text_width("...");
    int k = n < g - 3 ? n : g - 3;
    if (k < 0) k = 0;
    while (k > 0 && width_n(s, k) + ell > px) k--;
    while (k > 0 && s[k - 1] == ' ') k--;
    memcpy(out, s, (size_t)k); memcpy(out + k, "...", 3); out[k + 3] = 0;
    if (keep) *keep = k;
    return 0;
}
/* the window of s that starts at character off of "s, 10 blanks, s" and fits px / g */
static void scroll_window(char *out, const char *s, int off, int px, int g)
{
    int n = (int)strlen(s);
    if (n > TANGO_FIT_MAX - 4) n = TANGO_FIT_MAX - 4;
    int total = n + BLANKSPACE, w = 0, o = 0;
    for (int i = off; o < g && o < TANGO_FIT_MAX - 1; i++) {
        int j = i % total;
        char c = j < n ? s[j] : ' ';
        int a = adv(font_ch((uint8_t)c));
        if (w + a > px) break;
        out[o++] = c; w += a;
    }
    out[o] = 0;
}
typedef struct { int x, xmax, g, expand, scrolls; char l1[TANGO_FIT_MAX], l2[TANGO_FIT_MAX]; char vbuf[96]; const char *vtxt; } lrow_t;
static int is_list_kind(int k) { return k == TR_FILE || k == TR_DIR || k == TR_UP; }
static void lrow_geom(const tango_row_t *row, lrow_t *L)
{
    const int TX = TANGO_TX, VX = TANGO_VX;
    int k = row->kind;
    const char *val = (row->value && row->value[0]) ? row->value : NULL;
    L->x = TX;
    if (k == TR_DIR || k == TR_UP) {
        char icon[2] = { (char)(k == TR_DIR ? TANGO_ICON_FOLDER : TANGO_ICON_UP), 0 };
        L->x = TX + tango_text_width(icon) + 6;
    }
    L->vtxt = NULL;
    if (k == TR_SUB || k == TR_DIR) {
        size_t m = 0;
        if (val) { size_t l = strlen(val); if (l > sizeof L->vbuf - 4) l = sizeof L->vbuf - 4; memcpy(L->vbuf, val, l); m = l; L->vbuf[m++] = ' '; L->vbuf[m++] = ' '; }
        L->vbuf[m++] = (char)TANGO_ICON_CHEV_R; L->vbuf[m] = 0; L->vtxt = L->vbuf;
    } else if (val) L->vtxt = val;
    L->xmax = L->vtxt ? VX - tango_text_width(L->vtxt) - 8 : VX;
    L->g = TANGO_ROW_GLYPHS - ((k == TR_DIR || k == TR_UP) ? 1 : 0) - (L->vtxt ? (int)strlen(L->vtxt) : 0);
}
static void lrow_analyse(const tango_row_t *row, lrow_t *L)
{
    lrow_geom(row, L);
    const char *lab = row->label ? row->label : "";
    int keep = 0, n = (int)strlen(lab);
    L->expand = L->scrolls = 0; L->l2[0] = 0;
    if (fit_end(L->l1, lab, L->xmax - L->x, L->g, &keep)) return;
    if (row->kind == TR_FILE && !L->vtxt) {                         /* browse_expand: the end of the name on the row below */
        int t = n - keep;
        if (t > TANGO_ROW_GLYPHS - 1) t = TANGO_ROW_GLYPHS - 1;
        while (t > 0 && width_n(lab + n - t, t) > TANGO_VX - TANGO_TX) t--;
        L->expand = 1;
        memcpy(L->l2, lab + n - t, (size_t)t); L->l2[t] = 0;
        L->scrolls = keep + t < n;
    } else L->scrolls = 1;
}
int tango_list_row_scrolls(const tango_row_t *row)
{
    lrow_t L;
    if (!is_list_kind(row->kind)) return 0;
    lrow_analyse(row, &L);
    return L.scrolls;
}
/* zosd.Menu.text_r(i, x_end, s, col) */
static int mtext_r(int i, int x_end, const char *s, uint8_t col) { return mtext(i, x_end - tango_text_width(s), s, col); }
/* zosd.Menu.bar(i, x0, x1, col, inset) */
static void mbar(int i, int x0, int x1, uint8_t col, int inset)
{
    mrow_t *r = mrow(i); r->has_bar = 1; r->bx0 = (int16_t)x0; r->bx1 = (int16_t)x1; r->bcol = col; r->binset = (uint8_t)inset;
}
/* zosd.Menu.rule(i, col, at=None): at defaults to row_h // 2 */
static void mrule(int i, uint8_t col, int at)
{
    mrow_t *r = mrow(i); r->has_rule = 1; r->rcol = col; r->ry = (uint8_t)(at < 0 ? TANGO_RH / 2 : at);
}
/* zosd.Menu.banner_text(s, col, y_bottom, x_inset): reads bottom to top */
static void mbanner(const char *s, uint8_t col, int y_bottom, int x_inset)
{
    int y = y_bottom;
    for (; *s; s++) {
        uint8_t ch = font_ch((uint8_t)*s);
        if (y >= 0 && M.nbanner < BANNER_CAP) {
            M.banner[M.nbanner].x = (int16_t)y; M.banner[M.nbanner].ch = ch; M.banner[M.nbanner].col = col; M.nbanner++;
        } else M.clipped++;
        y -= adv(ch);
    }
    M.bx = x_inset;
}

/* pages.render_page(font, spec) up to (not including) Menu.render */
static void render_page(const tango_page_t *p)
{
    const int TX = TANGO_TX, VX = TANGO_VX, VIS = TANGO_VIS;
    memset(&M, 0, sizeof M);
    mbanner(p->banner ? p->banner : "", TANGO_C_ACCTXT, TANGO_H - 22,
            (TANGO_SW - TANGO_FONT_CAP) / 2 - (TANGO_FONT_ASC - TANGO_FONT_CAP));
    /* t = '\x83 ' + t (back / arrow left); t + ' \x7f' (arrow right) */
    {
        int x = TX;
        if (p->arrow == TANGO_ARROW_LEFT) x = mtext(0, x, "\x83 ", TANGO_C_TEXT);
        x = mtext(0, x, p->title ? p->title : "", TANGO_C_TEXT);
        if (p->arrow == TANGO_ARROW_RIGHT) mtext(0, x, " \x7f", TANGO_C_TEXT);
    }
    if (p->right && p->right[0]) mtext_r(0, VX, p->right, TANGO_C_DIM);
    mrule(0, TANGO_C_RULE, TANGO_RH - 3);

    int n = p->nrows, sel = p->sel, top;
    if (p->top >= 0) top = p->top;
    else if (n > VIS) {   /* top = min(max(0, sel - VIS + 3), max(0, len(rows) - VIS)) */
        int a = sel - VIS + 3; if (a < 0) a = 0;
        int b = n - VIS; if (b < 0) b = 0;
        top = a < b ? a : b;
    } else top = 0;
    {   /* the selected file's second line needs a row: when it is the last one shown, the window moves down by one (PrintDirectory's flist_iFirstEntryInc) */
        lrow_t Ls;
        if (sel >= 0 && sel < n && is_list_kind(p->rows[sel].kind)) {
            lrow_analyse(&p->rows[sel], &Ls);
            if (Ls.expand && sel - top == VIS - 1 && sel < n - 1) top++;
        }
    }

    int line = 0;
    for (int i = 0; line < VIS && top + i < n; i++) {
        const tango_row_t *row = &p->rows[top + i];
        int r = 2 + line++, on = (top + i) == sel, k = row->kind;
        const char *lab = row->label ? row->label : "";
        const char *val = (row->value && row->value[0]) ? row->value : NULL;
        if (k == TR_SEP) { mrule(r, TANGO_C_RULE, -1); continue; }
        if (k == TR_BLANK) continue;
        if (k == TR_CENTER) { mtext(r, (TX + VX - tango_text_width(lab)) / 2, lab, row->col ? row->col : TANGO_C_DIM); continue; }
        if (k == TR_LOGO) {                    /* pages.render_page 'logo' */
            int x0 = (TX + VX - 12 * (int)strlen(lab)) / 2;
            for (int j = 0; lab[j]; j++)
                if (lab[j] == '#') { static const char b3[4] = { (char)TANGO_ICON_BRICK, (char)TANGO_ICON_BRICK, (char)TANGO_ICON_BRICK, 0 }; mtext(r, x0 + 12 * j, b3, TANGO_C_ACC); }
            continue;
        }
        if (k == TR_SCROLL) {                  /* pages.render_page 'scroll' */
            int x = VX - p->scroll;
            for (const char *c = lab; *c; c++) {
                int a = adv((uint8_t)*c);
                if (x >= TX && x + a <= VX) { char one[2] = { *c, 0 }; mtext(r, x, one, TANGO_C_DIM); }
                x += a;
            }
            continue;
        }
        if (on) mbar(r, TX - 14, VX + 14, TANGO_C_BAR, 2);
        uint8_t fg  = on ? TANGO_C_BARTXT : ((k == TR_INFO || k == TR_DIS) ? TANGO_C_DIM : TANGO_C_TEXT);
        uint8_t vfg = on ? TANGO_C_BARTXT : (k == TR_VOL ? TANGO_C_ACC : TANGO_C_DIM);
        int x = TX;
        if (k == TR_DIR || k == TR_UP) {
            char icon[2] = { (char)(k == TR_DIR ? TANGO_ICON_FOLDER : TANGO_ICON_UP), 0 };
            x = mtext(r, TX, icon, on ? TANGO_C_BARTXT : TANGO_C_ACC) + 6;
        }
        if (is_list_kind(k)) {                 /* a file list row: MiSTer's trimming, scrolling and expansion */
            lrow_t L;
            lrow_analyse(row, &L);
            char win[TANGO_FIT_MAX];
            const char *txt = L.l1;
            if (on && L.scrolls && p->sel_scroll > 0) { scroll_window(win, lab, p->sel_scroll - 1, L.xmax - L.x, L.g); txt = win; }
            mtext_clip(r, x, txt, fg, L.xmax);
            if (L.vtxt) mtext_r(r, VX, L.vtxt, vfg);
            if (on && L.expand && line < VIS) {
                int r2 = 2 + line++;
                mbar(r2, TX - 14, VX + 14, TANGO_C_BAR, 2);
                mtext_clip(r2, TX, L.l2, fg, VX);
            }
            continue;
        }
        /* the right-hand text, laid out first only to know where a long label must stop */
        char vbuf[96]; const char *vtxt = NULL;
        if (k == TR_SUB || k == TR_DIR) {
            size_t m = 0;
            if (val) { size_t l = strlen(val); if (l > sizeof vbuf - 4) l = sizeof vbuf - 4; memcpy(vbuf, val, l); m = l; vbuf[m++] = ' '; vbuf[m++] = ' '; }
            vbuf[m++] = (char)TANGO_ICON_CHEV_R; vbuf[m] = 0; vtxt = vbuf;
        } else if (val) vtxt = val;
        int xmax = vtxt ? VX - tango_text_width(vtxt) - 8 : VX;
        char fitted[TANGO_FIT_MAX];
        int g = TANGO_ROW_GLYPHS - ((k == TR_DIR || k == TR_UP) ? 1 : 0) - (vtxt ? (int)strlen(vtxt) : 0);
        mtext_clip(r, x, fit(fitted, lab, xmax - x, g), fg, xmax);
        if (vtxt) mtext_r(r, VX, vtxt, vfg);
    }
    if (n > VIS) {   /* MiSTer-style position hint in the title bar */
        char pos[24];
        const char *s = p->right;
        if (!s || !s[0]) {
            /* f'{sel + 1}/{len(rows)}' */
            int a = sel + 1, b = n, m = 0; char t[12]; int tn;
            tn = 0; do { t[tn++] = (char)('0' + a % 10); a /= 10; } while (a); while (tn) pos[m++] = t[--tn];
            pos[m++] = '/';
            tn = 0; do { t[tn++] = (char)('0' + b % 10); b /= 10; } while (b); while (tn) pos[m++] = t[--tn];
            pos[m] = 0; s = pos;
        }
        mtext_r(0, VX, s, TANGO_C_DIM);
    }
    int x = TX;
    for (int i = 0; i < p->nlegend; i++) {
        x = mtext(17, x, p->legend[i].key ? p->legend[i].key : "", TANGO_C_ACC) + 6;
        x = mtext(17, x, p->legend[i].label ? p->legend[i].label : "", TANGO_C_DIM) + 22;
    }
}

/* export_hw.build_tables(): glyph list + row table */
static void build_tables(tango_tables_t *t)
{
    const int texty = (TANGO_RH + TANGO_FONT_CAP) / 2 - TANGO_FONT_ASC;
    memset(t, 0, sizeof *t);
    t->clipped = M.clipped;
    for (int i = 0; i < NROWS_BUILD; i++) {
        mrow_t *r = &M.row[i];
        if (!r->exists) continue;
        /* g = sorted(r['glyphs'], key=x) -- stable */
        for (int a = 1; a < r->n; a++) {
            mg_t v = r->g[a]; int b = a - 1;
            while (b >= 0 && r->g[b].x > v.x) { r->g[b + 1] = r->g[b]; b--; }
            r->g[b + 1] = v;
        }
        int first = t->nglyphs, count = 0;
        for (int k = 0; k < r->n; k++) {
            if (k + 1 < r->n && r->g[k + 1].x == r->g[k].x) { t->dropped++; continue; }   /* shadowed */
            if (count >= TANGO_ROW_MAXG || t->nglyphs >= TANGO_GLYPH_SLOTS - M.nbanner) { t->clipped++; continue; }
            t->glyphs[t->nglyphs++] = tango_glyph_entry(r->g[k].x, r->g[k].ch, r->g[k].col);
            count++;
        }
        /* first glyph: bits[8:0] here, bit 9 stolen from hi's bit 30 (always 0: texty is a small
         * compile-time constant, <= 5 bits, and the RTL/model never read hi's bits 31:30 -- see
         * menu-tango docs/TANGO-SPEC.md "FPGA memory" and sys/tango_menu_overlay.v's "Memories" comment) */
        uint32_t lo = (uint32_t)(first & 0x1FFu) | (uint32_t)count << 9,
                 hi = (uint32_t)texty << 24 | (uint32_t)((first >> 9) & 1u) << 30;
        if (r->has_bar) {
            lo |= 1u << 16 | (uint32_t)(r->bcol & 15) << 17 | (uint32_t)(r->bx0 & 0x7FF) << 21;
            hi |= (uint32_t)(r->bx1 & 0x7FF) | (uint32_t)(r->binset & 7) << 11;
        }
        if (r->has_rule) hi |= 1u << 14 | (uint32_t)(r->rcol & 15) << 15 | (uint32_t)(r->ry & 31) << 19;
        t->rows[2 * i] = lo; t->rows[2 * i + 1] = hi;
    }
    int first = t->nglyphs;
    for (int k = 0; k < M.nbanner && t->nglyphs < TANGO_GLYPH_SLOTS; k++)
        t->glyphs[t->nglyphs++] = tango_glyph_entry(M.banner[k].x, M.banner[k].ch, M.banner[k].col);
    /* row 31 (banner) has no bar/rule fields, so first/count pack straight into bits[9:0]/[16:10] of
     * the lo word with no stolen bit needed (unlike the content rows above) */
    t->rows[2 * TANGO_BANNER_ROW] = (uint32_t)(first & 0x3FFu) | (uint32_t)(t->nglyphs - first) << 10;
    t->rows[2 * TANGO_BANNER_ROW + 1] = (uint32_t)(M.bx & 63) << 24;
}

int tango_layout_page(const tango_page_t *p, tango_tables_t *t)
{
    if (!p || !t) return -1;
    render_page(p);
    build_tables(t);
    return 0;
}
