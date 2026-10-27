// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 the Tango authors
/*
 * tango_padlearn.c -- see tango_padlearn.h. Tango, our own code.
 */
#include <string.h>
#include <stdio.h>
#include <strings.h>
#include "tango_padlearn.h"

void pl_begin(pl_t *l)
{
    memset(l, 0, sizeof *l);
    l->t.mode = PDM_TABLE;
    l->minlen = PD_RPT_BYTES;
}

/* the bitwise majority of n samples; `var` gets the bits on which they disagree */
static void majority(const pl_sample_t *s, int n, uint8_t *out, uint8_t *var, int *minlen)
{
    for (int i = 0; i < PD_RPT_BYTES; i++) {
        uint8_t m = 0, v = 0;
        for (int b = 0; b < 8; b++) {
            int ones = 0;
            for (int k = 0; k < n; k++) ones += (s[k].b[i] >> b) & 1;
            if (ones * 2 > n) m |= (uint8_t)(1 << b);
            if (ones != 0 && ones != n) v |= (uint8_t)(1 << b);
        }
        out[i] = m; if (var) var[i] = v;
    }
    if (minlen) { *minlen = PD_RPT_BYTES; for (int k = 0; k < n; k++) if (s[k].len < *minlen) *minlen = s[k].len; }
}

static uint32_t getbits(const uint8_t *b, int bit, int w)
{
    uint32_t v = 0;
    for (int k = 0; k < w; k++) v |= (uint32_t)((b[(bit + k) >> 3] >> ((bit + k) & 7)) & 1) << k;
    return v;
}

/* the numeric axis field of axis ordinal k, or NULL (no layout, a 1-bit or over-16-bit "axis": those are judged bit by bit) */
static const hp_field_t *axis_num(const hp_layout_t *L, int k)
{
    if (!L || k < 0 || k >= L->naxis || k >= HP_MAX_AXES || L->aidx[k] < 0) return NULL;
    const hp_field_t *f = &L->f[L->aidx[k]];
    return (f->size >= 2 && f->size <= 16 && f->bit + f->size <= 8 * PD_RPT_BYTES) ? f : NULL;
}

/* the field's value in offset-binary form (a signed field has its sign bit flipped, so rest is near the middle and "more" is always a bigger number) */
static int axis_val(const hp_field_t *f, const pl_sample_t *s, uint32_t *v)
{
    if (f->bit + f->size > 8 * (int)s->len) return 0;
    *v = getbits(s->b, f->bit, f->size);
    if (f->sgn) *v ^= 1u << (f->size - 1);
    return 1;
}

static uint32_t median_of(uint32_t *v, int n)
{
    for (int i = 1; i < n; i++) { uint32_t x = v[i]; int j = i - 1; while (j >= 0 && v[j] > x) { v[j + 1] = v[j]; j--; } v[j + 1] = x; }
    return v[n / 2];
}

void pl_base_make(pl_base_t *b, const hp_layout_t *L, const pl_sample_t *s, int n)
{
    int ml;
    memset(b->ax_idle, 0, sizeof b->ax_idle); memset(b->ax_spread, 0, sizeof b->ax_spread);
    majority(s, n, b->idle.b, b->noisy, &ml);
    b->idle.len = (uint8_t)ml; b->have = 1;
    if (!L) return;
    for (int k = 0; k < L->naxis && k < HP_MAX_AXES; k++) {
        const hp_field_t *f = axis_num(L, k); uint32_t v[PLS_IDLE_SAMPLES_MAX]; int m = 0, lo = 0, hi = 0;
        if (!f) continue;
        for (int q = 0; q < n && m < PLS_IDLE_SAMPLES_MAX; q++) if (axis_val(f, &s[q], &v[m])) m++;
        if (!m) continue;
        lo = (int)v[0]; hi = (int)v[0];
        for (int q = 1; q < m; q++) { if ((int)v[q] < lo) lo = (int)v[q]; if ((int)v[q] > hi) hi = (int)v[q]; }
        b->ax_idle[k] = (uint16_t)median_of(v, m); b->ax_spread[k] = (uint16_t)(hi - lo);
    }
}

/* an axis ordinal's threshold: beyond this distance from its rest median the axis counts as moved */
static uint32_t axis_thr(const pl_base_t *b, const hp_field_t *f, int k)
{
    /* an eighth of the LOGICAL range (a 16-bit field that only ever spans -1800..1800 has a range of 3600, not 65536), and three times the spread at rest */
    uint32_t span = f->lmax > f->lmin ? (uint32_t)(f->lmax - f->lmin) : (1u << f->size);
    uint32_t t = span / 8u, t2 = 3u * b->ax_spread[k] + 1u;
    if (t < 1) t = 1;
    return t > t2 ? t : t2;
}

int pl_sig_of(const pl_base_t *b, const hp_layout_t *L, const uint8_t dyn[PD_RPT_BYTES], const pl_sample_t *s, pl_sig_t *out)
{
    int len = s->len < b->idle.len ? s->len : b->idle.len, any = 0;
    uint8_t nm[PD_RPT_BYTES]; memset(nm, 0, sizeof nm);
    memset(out, 0, sizeof *out);
    for (int k = 0; L && k < L->naxis && k < HP_MAX_AXES; k++) {
        const hp_field_t *f = axis_num(L, k); uint32_t v;
        if (!f) continue;
        for (int q = 0; q < f->size; q++) nm[(f->bit + q) >> 3] |= (uint8_t)(1u << ((f->bit + q) & 7));
        if (axis_val(f, s, &v)) {
            uint32_t d = v > b->ax_idle[k] ? v - b->ax_idle[k] : b->ax_idle[k] - v;
            if (d >= axis_thr(b, f, k)) { out->axes |= (uint16_t)(1u << k); any = 1; }
        }
    }
    /* with a descriptor only the bits it declares (button, hat, non-numeric axis) can be a press: a vendor byte, a padding bit or a constant that changes is
     * a counter or a sensor, however long it stays changed (a counter's top bit flips every second or so) */
    uint8_t fm[PD_RPT_BYTES]; memset(fm, L ? 0 : 0xFF, sizeof fm);
    for (int q = 0; L && q < L->nf; q++) for (int k = 0; k < L->f[q].size; k++) { int bit = L->f[q].bit + k; if (bit < 8 * PD_RPT_BYTES) fm[bit >> 3] |= (uint8_t)(1u << (bit & 7)); }
    for (int i = 0; i < len; i++) {
        out->bits[i] = (uint8_t)((s->b[i] ^ b->idle.b[i]) & ~b->noisy[i] & ~(dyn ? dyn[i] : 0) & ~nm[i] & fm[i]);
        if (out->bits[i]) any = 1;
    }
    return any;
}

int pl_sig_share(const pl_sig_t *a, const pl_sig_t *c)
{
    if (a->axes & c->axes) return 1;
    for (int i = 0; i < PD_RPT_BYTES; i++) if (a->bits[i] & c->bits[i]) return 1;
    return 0;
}

int pl_baseline(pl_t *l, const pl_sample_t *s, int n)
{
    if (n < 1) return PL_BAD;
    pl_base_make(&l->b, l->L, s, n);
    l->minlen = l->b.idle.len; l->have_idle = 1;
    return PL_OK;
}

void pl_set_report_id(pl_t *l, int rid) { l->rid_en = 1; l->rid_val = (uint8_t)rid; }

/* ---- the changed bits of one press, in groups */
typedef struct { int lo, hi, nbits, fld, num; uint32_t ival, pval; } grp_t;     /* bit offsets in the payload; fld = index into l->L->f when the group is one descriptor field, else -1 */

void pl_set_layout(pl_t *l, const hp_layout_t *L) { l->L = L; }

/* the descriptor field that holds payload bit `bit`, -1 if none (vendor bytes, padding, constants) */
static int field_at(const hp_layout_t *L, int bit)
{
    if (!L) return -1;
    for (int i = 0; i < L->nf; i++) if (bit >= L->f[i].bit && bit < L->f[i].bit + L->f[i].size) return i;
    return -1;
}

static int groups(const pl_t *l, const pl_sample_t *p, int n, uint8_t pv[PD_RPT_BYTES], grp_t *g, int gmax)
{
    uint8_t var[PD_RPT_BYTES], nm[PD_RPT_BYTES]; int ml;
    majority(p, n, pv, var, &ml);
    memset(nm, 0, sizeof nm);
    for (int k = 0; l->L && k < l->L->naxis && k < HP_MAX_AXES; k++) {              /* axis fields are judged by value below, not by bit */
        const hp_field_t *f = axis_num(l->L, k);
        if (f) for (int q = 0; q < f->size; q++) nm[(f->bit + q) >> 3] |= (uint8_t)(1u << ((f->bit + q) & 7));
    }
    int ng = 0, lastbit = -100, lastorph = -1;
    for (int bit = 0; bit < 8 * PD_RPT_BYTES; bit++) {
        int i = bit >> 3, b = bit & 7;
        if (i >= ml || i >= l->b.idle.len) break;
        if ((l->b.noisy[i] >> b) & 1 || (var[i] >> b) & 1 || (nm[i] >> b) & 1) continue;
        if (((pv[i] ^ l->b.idle.b[i]) >> b) & 1) {
            int f = field_at(l->L, bit);
            if (f >= 0) {                                                  /* a descriptor field: all its changed bits are one group */
                int k; for (k = 0; k < ng; k++) if (g[k].fld == f) break;
                if (k < ng) g[k].nbits++;
                else if (ng < gmax) { g[ng].lo = l->L->f[f].bit; g[ng].hi = l->L->f[f].bit + l->L->f[f].size - 1; g[ng].nbits = 1; g[ng].fld = f; g[ng].num = 0; ng++; }
                lastorph = -1;
            } else if (l->L) {
                /* a bit no descriptor field owns (vendor byte, padding, constant): never a press when the descriptor parsed */
            } else if (lastorph >= 0 && bit - lastbit <= 8) { g[lastorph].hi = bit; g[lastorph].nbits++; }
            else if (ng < gmax) { g[ng].lo = g[ng].hi = bit; g[ng].nbits = 1; g[ng].fld = -1; g[ng].num = 0; lastorph = ng; ng++; }
            lastbit = bit;
        }
    }
    /* the axes: a field whose median value over the press left its rest range */
    for (int k = 0; l->L && k < l->L->naxis && k < HP_MAX_AXES; k++) {
        const hp_field_t *f = axis_num(l->L, k); uint32_t v[PLS_IDLE_SAMPLES_MAX]; int m = 0;
        if (!f) continue;
        for (int q = 0; q < n && m < PLS_IDLE_SAMPLES_MAX; q++) if (axis_val(f, &p[q], &v[m])) m++;
        if (!m) continue;
        uint32_t med = median_of(v, m), idl = l->b.ax_idle[k], d = med > idl ? med - idl : idl - med;
        if (d >= axis_thr(&l->b, f, k) && ng < gmax) {
            g[ng].lo = f->bit; g[ng].hi = f->bit + f->size - 1; g[ng].nbits = f->size; g[ng].fld = l->L->aidx[k]; g[ng].num = 1; g[ng].ival = idl; g[ng].pval = med; ng++;
        }
    }
    return ng;
}


static void drop_out(pl_t *l, int out)
{
    int k = 0;
    for (int i = 0; i < l->t.count; i++) if (l->t.e[i].out != out) l->t.e[k++] = l->t.e[i];
    l->t.count = (uint8_t)k;
}

static int add(pl_t *l, int out, int cmp, int bit, int w, int bias, int thr)
{
    pd_ent_t e; memset(&e, 0, sizeof e);
    e.out = (uint8_t)out; e.cmp = (uint8_t)cmp; e.byte = (uint8_t)(bit >> 3); e.shift = (uint8_t)(bit & 7); e.widthm1 = (uint8_t)(w - 1);
    e.bias = (uint8_t)bias; e.thr = (uint16_t)thr;
    return pd_add(&l->t, &e) ? PL_OK : PL_FULL;
}

/* the field of a group.  A descriptor field: exactly its bit offset and width (up to 16 bits; the table's window is 3 bytes).  Otherwise the aligned field
 * around the group: 8 bits in one byte, else 16 bits at the low byte (little endian) */
static int field_of(const pl_t *l, const grp_t *g, int *bit, int *w)
{
    if (g->fld >= 0) {
        const hp_field_t *f = &l->L->f[g->fld];
        if (f->size > 16 || f->size < 2 || f->bit + f->size > 8 * PD_RPT_BYTES) return 0;       /* a field that runs past the 16 bytes the decode keeps would read its missing bytes as 0 */
        *bit = f->bit; *w = f->size; return 1;
    }
    int b0 = g->lo >> 3, b1 = g->hi >> 3;
    if (b0 == b1) { *bit = b0 * 8; *w = 8; return 1; }
    if (b1 == b0 + 1) { *bit = b0 * 8; *w = 16; return 1; }
    return 0;
}

/* an analog input pushed to its end: GE/LE half-way between idle and pressed */
static int learn_analog(pl_t *l, int out, const grp_t *g, const uint8_t *pv, int stick)
{
    int bit, w;
    if (!field_of(l, g, &bit, &w)) return PL_AMBIG;
    uint32_t mid = 1u << (w - 1), idle = getbits(l->b.idle.b, bit, w), pr = getbits(pv, bit, w);
    int bias = 0;
    if (g->num) { idle = g->ival; pr = g->pval; bias = l->L->f[g->fld].sgn ? 1 : 0; goto have_values; }      /* median values, already in offset binary */
    if (g->fld >= 0) bias = l->L->f[g->fld].sgn ? 1 : 0;                                  /* the descriptor says whether the range is signed */
    else if (stick && !(idle + mid / 4 >= mid && idle <= mid + mid / 4) && (idle <= mid / 4 || idle >= 2 * mid - mid / 4)) bias = 1;    /* rests near 0 or the wrap: signed */
    if (bias) { idle ^= mid; pr ^= mid; }
have_values:
    if (idle == pr) return PL_NOCHANGE;
    drop_out(l, out);
    if (pr > idle) return add(l, out, PDC_GE, bit, w, bias, (int)((idle + pr + 1) / 2));
    return add(l, out, PDC_LE, bit, w, bias, (int)((idle + pr) / 2));
}

static int learn_one(pl_t *l, int out, const pl_sample_t *p, int n, int stick)
{
    if (!l->have_idle || n < 1) return PL_BAD;
    uint8_t pv[PD_RPT_BYTES]; grp_t g[8];
    int ng = groups(l, p, n, pv, g, 8);
    if (ng == 0) return PL_NOCHANGE;
    int pick = -1;
    if (ng == 1) pick = 0;
    else {
        int singles = 0, which = -1;
        for (int i = 0; i < ng; i++) if (g[i].nbits == 1 && g[i].lo == g[i].hi) { singles++; which = i; }
        if (singles != 1) return PL_AMBIG;
        pick = which;
    }
    if (g[pick].nbits == 1 && g[pick].lo == g[pick].hi && stick != 1) {          /* stick 1: always an axis; 2 (d-pad direction): a single bit is a button */
        int bit = g[pick].lo, now = (pv[bit >> 3] >> (bit & 7)) & 1;
        drop_out(l, out);
        return add(l, out, now ? PDC_NZ : PDC_Z, bit, 1, 0, 0);
    }
    return learn_analog(l, out, &g[pick], pv, stick != 0);
}

int pl_learn_button(pl_t *l, int out, const pl_sample_t *p, int n) { return learn_one(l, out, p, n, 0); }
int pl_learn_stick(pl_t *l, int out, const pl_sample_t *p, int n) { return learn_one(l, out, p, n, 1); }

int pl_learn_dpad(pl_t *l, const pl_sample_t *const pressed[4], int n)
{
    static const int OUT[4] = { RW_UP, RW_RIGHT, RW_DOWN, RW_LEFT };
    if (!l->have_idle || n < 1) return PL_BAD;
    uint8_t pv[4][PD_RPT_BYTES]; grp_t g[4][8]; int ng[4], allone = 1;
    for (int d = 0; d < 4; d++) {
        ng[d] = groups(l, pressed[d], n, pv[d], g[d], 8);
        if (ng[d] != 1) allone = 0;
    }
    /* a hat: one group per press, all in the same nibble (or byte), several bits each */
    if (allone) {
        int lo = g[0][0].lo, hi = g[0][0].hi, multi;
        for (int d = 0; d < 4; d++) { if (g[d][0].lo < lo) lo = g[d][0].lo; if (g[d][0].hi > hi) hi = g[d][0].hi; }
        multi = hi > lo;                    /* a hat whose null state is 0 changes a single bit for "up" (0 -> 1) but the four presses together span more */
        int w = 0, bit = 0;
        if (l->L && g[0][0].fld >= 0) {
            /* the descriptor knows: all four presses must be the same HAT field (an X / Y pair of 4-bit axes is not one) */
            int f0 = g[0][0].fld, same = 1;
            for (int d = 1; d < 4; d++) if (g[d][0].fld != f0) same = 0;
            if (same && l->L->f[f0].kind == HPK_HAT && l->L->f[f0].bit + l->L->f[f0].size <= 8 * PD_RPT_BYTES) { bit = l->L->f[f0].bit; w = l->L->f[f0].size; multi = 1; }      /* a hat that runs past the kept bytes cannot be read */
            else multi = 0;
        } else if ((lo >> 2) == (hi >> 2)) { w = 4; bit = (lo >> 2) * 4; }
        else if ((lo >> 3) == (hi >> 3)) { w = 8; bit = (lo >> 3) * 8; }
        if (w && multi) {
            uint32_t v[4]; for (int d = 0; d < 4; d++) v[d] = getbits(pv[d], bit, w);
            int steps = 0;
            if (v[1] - v[0] == 2 && v[2] - v[0] == 4 && v[3] - v[0] == 6) steps = 8;
            else if (v[1] - v[0] == 1 && v[2] - v[0] == 2 && v[3] - v[0] == 3) steps = 4;
            if (steps && v[0] <= 15) {
                for (int d = 0; d < 4; d++) drop_out(l, OUT[d]);
                for (int d = 0; d < 4; d++)
                    if (!pd_add_hat(&l->t, OUT[d], bit, w < 4 ? 4 : w, (int)v[0], (int)v[0] + steps - 1, 1 << d)) return PL_FULL;
                return PL_OK;
            }
        }
    }
    /* otherwise four independent inputs: buttons, or axes */
    int worst = PL_OK;
    for (int d = 0; d < 4; d++) {
        int r = learn_one(l, OUT[d], pressed[d], n, 2);
        if (r != PL_OK) worst = r;
    }
    return worst;
}

int pl_table(const pl_t *l, pd_table_t *out)
{
    *out = l->t;
    out->mode = PDM_TABLE; out->rid_en = l->rid_en; out->rid_val = l->rid_val;
    int last = 0;
    for (int i = 0; i < out->count; i++) {
        int lb = out->e[i].byte + (out->e[i].shift + out->e[i].widthm1) / 8 + 1;
        if (lb > last) last = lb;
    }
    out->min_len = (uint8_t)(last > 0 ? last : 1);
    return out->count;
}

int pl_changed_groups(const pl_t *l, const pl_sample_t *p, int n)
{
    uint8_t pv[PD_RPT_BYTES]; grp_t g[8];
    if (!l->have_idle || n < 1) return 0;
    return groups(l, p, n, pv, g, 8);
}

/* ------------------------------------------------------------------ the .dec file */
int pl_dec_name(uint16_t vid, uint16_t pid, uint16_t ver, char *out, int cap)
{
    return snprintf(out, (size_t)cap, "pad_%04x_%04x_%04x_v1.dec", vid, pid, ver);
}

int pl_dec_save(uint16_t vid, uint16_t pid, uint16_t ver, const pd_table_t *t, uint8_t *out, int cap)
{
    int n = 5 + 5 * t->count;
    if (cap < 10 + n) return 0;
    memcpy(out, "TDC1", 4);
    out[4] = (uint8_t)(vid >> 8); out[5] = (uint8_t)vid; out[6] = (uint8_t)(pid >> 8); out[7] = (uint8_t)pid; out[8] = (uint8_t)(ver >> 8); out[9] = (uint8_t)ver;
    if (pd_wire(t, out + 10, cap - 10) != n) return 0;
    return 10 + n;
}

int pl_dec_load(const uint8_t *in, int n, uint16_t vid, uint16_t pid, uint16_t ver, pd_table_t *t)
{
    if (n < 15 || memcmp(in, "TDC1", 4)) return 0;
    if (in[4] != (vid >> 8) || in[5] != (vid & 255) || in[6] != (pid >> 8) || in[7] != (pid & 255) || in[8] != (ver >> 8) || in[9] != (ver & 255)) return 0;
    if (!pd_unwire(in + 10, n - 10, t)) return 0;
    return t->mode == PDM_TABLE;
}

/* ------------------------------------------------------------------ the SDL line */
static int find_field(const hp_layout_t *L, int kind, int bit, int w)
{
    for (int i = 0; i < L->nf; i++)
        if (L->f[i].kind == kind && L->f[i].bit == bit && (kind != HPK_AXIS || L->f[i].size == w)) return i;
    return -1;
}

static int ordinal(const hp_layout_t *L, int kind, int fidx)
{
    const int8_t *tab = kind == HPK_BUTTON ? L->bidx : kind == HPK_AXIS ? L->aidx : L->hidx;
    int n = kind == HPK_BUTTON ? L->nbtn : kind == HPK_AXIS ? L->naxis : L->nhat;
    for (int i = 0; i < n; i++) if (tab[i] == fidx) return i;
    return -1;
}

int pl_export_sdl(const pd_table_t *t, const hp_layout_t *L, uint16_t vid, uint16_t pid, uint16_t ver, const char *name, char *out, int cap)
{
    static const struct { int raw; const char *n; } NM[] = {
        { RW_SOUTH, "a" }, { RW_EAST, "b" }, { RW_WEST, "x" }, { RW_NORTH, "y" }, { RW_BACK, "back" }, { RW_GUIDE, "guide" }, { RW_START, "start" },
        { RW_L3, "leftstick" }, { RW_R3, "rightstick" }, { RW_L1, "leftshoulder" }, { RW_R1, "rightshoulder" },
        { RW_UP, "dpup" }, { RW_DOWN, "dpdown" }, { RW_LEFT, "dpleft" }, { RW_RIGHT, "dpright" }, { RW_L2, "lefttrigger" }, { RW_R2, "righttrigger" } };
    if (!L) return 0;
    int n = snprintf(out, (size_t)cap, "03000000%02x%02x0000%02x%02x0000%02x%02x0000,%s,", vid & 255, vid >> 8, pid & 255, pid >> 8, ver & 255, ver >> 8, name ? name : "Learned pad");
    int wrote = 0;
    for (unsigned k = 0; k < sizeof NM / sizeof NM[0]; k++) {
        for (int i = 0; i < t->count; i++) {
            const pd_ent_t *e = &t->e[i];
            if (e->out != NM[k].raw) continue;
            int bit = e->byte * 8 + e->shift, w = e->widthm1 + 1, idx, f;
            char tok[24] = "";
            if (e->cmp == PDC_NZ && w == 1) {                                /* an active-low button (Z) has no SDL form */
                f = find_field(L, HPK_BUTTON, bit, 1); idx = f < 0 ? -1 : ordinal(L, HPK_BUTTON, f);
                if (idx >= 0) snprintf(tok, sizeof tok, "b%d", idx);
            } else if (e->cmp == PDC_GE || e->cmp == PDC_LE) {
                f = find_field(L, HPK_AXIS, bit, w); idx = f < 0 ? -1 : ordinal(L, HPK_AXIS, f);
                if (idx >= 0) {
                    int high = e->cmp == PDC_GE, trig = NM[k].raw == RW_L2 || NM[k].raw == RW_R2;
                    snprintf(tok, sizeof tok, "%sa%d", trig && high ? "" : high ? "+" : "-", idx);
                }
            } else if (e->cmp == PDC_HAT) {
                f = find_field(L, HPK_HAT, bit, w); idx = f < 0 ? -1 : ordinal(L, HPK_HAT, f);
                if (idx >= 0) snprintf(tok, sizeof tok, "h%d.%d", idx, NM[k].raw == RW_UP ? 1 : NM[k].raw == RW_RIGHT ? 2 : NM[k].raw == RW_DOWN ? 4 : 8);
            }
            if (tok[0] && n < cap) { n += snprintf(out + n, (size_t)(cap - n), "%s:%s,", NM[k].n, tok); wrote++; break; }
        }
    }
    if (n < cap) n += snprintf(out + n, (size_t)(cap - n), "platform:Linux,");
    return wrote && n < cap ? n : 0;
}

/* ====================================================================================================================================
 * the incremental capture (tango_padlearn.h)
 * ==================================================================================================================================== */
void plcap_begin(plcap_t *c, const hp_layout_t *L)
{
    memset(c, 0, sizeof *c);
    c->L = L;
    if (L && L->rid) { c->rid_en = 1; c->rid = (uint8_t)L->rid; }
}

static int plcap_mine(const plcap_t *c, const pl_sample_t *s) { return !c->rid_en || (s->len >= 1 && s->b[0] == c->rid); }

int plcap_idle(plcap_t *c, const pl_sample_t *s)
{
    if (c->have_base) return 1;
    if (!plcap_mine(c, s)) return 0;
    c->idle[c->nidle++] = *s;
    if (c->nidle >= PLS_IDLE_SAMPLES) { pl_base_make(&c->base, c->L, c->idle, c->nidle); c->have_base = 1; return 1; }
    return 0;
}

int plcap_idle_samples(const plcap_t *c, const pl_sample_t **s) { *s = c->idle; return c->nidle; }

void plcap_held_begin(plcap_t *c)
{
    memset(c->dyn, 0, sizeof c->dyn); memset(c->fl, 0, sizeof c->fl);
    c->started = c->have_prev = c->prev_ch = c->n = 0; c->total = 0; c->t_start = c->t_rest = 0;
}

int plcap_held(plcap_t *c, const pl_sample_t *s, uint32_t now_ms, pl_sample_t *out, int max)
{
    if (!c->have_base || max < 1) return 0;
    if (max > PLS_MAX_SAMPLES) max = PLS_MAX_SAMPLES;
    if (!s || !plcap_mine(c, s)) {                                           /* the clock only: has the report been at rest long enough? */
        if (c->started && c->t_rest && now_ms - c->t_rest >= PLCAP_REST_MS) goto ended;
        return c->started ? PLCAP_PRESSED : 0;
    }
    pl_sig_t cur;
    if (c->have_prev) {                                                       /* bits that move between two reports before the press, or three during it, are noise */
        int len = s->len < c->prev.len ? s->len : c->prev.len;
        for (int i = 0; i < len; i++) {
            uint8_t ch = (uint8_t)((s->b[i] ^ c->prev.b[i]) & ~c->base.noisy[i] & ~c->dyn[i]);
            for (int b = 0; ch && b < 8; b++) if ((ch >> b) & 1) { if (++c->fl[i * 8 + b] >= (c->started ? 3 : 2)) c->dyn[i] |= (uint8_t)(1 << b); }
        }
    }
    int ch = pl_sig_of(&c->base, c->L, c->dyn, s, &cur);
    if (!c->started) {
        if (ch && c->have_prev && c->prev_ch) {
            pl_sig_of(&c->base, c->L, c->dyn, &c->prev, &c->sprev);
            if (pl_sig_share(&cur, &c->sprev)) {
                c->started = 1; c->t_start = now_ms; c->t_rest = 0; c->ring[0] = c->prev; c->n = 1; c->total = 1;
                if (max > 1) { c->ring[1] = *s; c->n = 2; } c->total = 2;
            }
        }
        c->prev = *s; c->prev_ch = (uint8_t)ch; c->have_prev = 1;
        return c->started ? PLCAP_PRESSED : 0;
    }
    c->prev = *s;
    if (ch) {
        c->t_rest = 0;
        if (c->n < max) c->ring[c->n++] = *s; else c->ring[c->total % max] = *s;
        c->total++;
        return PLCAP_PRESSED;
    }
    if (!c->t_rest) c->t_rest = now_ms ? now_ms : 1;
    if (now_ms - c->t_rest < PLCAP_REST_MS) return PLCAP_PRESSED;
ended:
    if (c->t_rest - c->t_start >= PLCAP_MIN_HOLD_MS) {
        int n = c->n; if (n > max) n = max;
        memcpy(out, c->ring, (size_t)n * sizeof out[0]);
        plcap_held_begin(c);
        return n;
    }
    plcap_held_begin(c);                                                      /* a flicker, not a press: back to waiting */
    return 0;
}

/* ====================================================================================================================================
 * the session layer
 * ==================================================================================================================================== */
typedef char plsess_size_check[sizeof(plsess_t) <= PLSESS_BYTES ? 1 : -1];

const plsess_item_t plsess_default_order[] = {
    { RW_RIGHT, PLI_DPAD, 0, "RIGHT" }, { RW_LEFT, PLI_DPAD, 0, "LEFT" }, { RW_DOWN, PLI_DPAD, 0, "DOWN" }, { RW_UP, PLI_DPAD, 0, "UP" },
    { RW_EAST,  PLI_BUTTON, 0, "BUTTON A" }, { RW_SOUTH, PLI_BUTTON, 0, "BUTTON B" }, { RW_NORTH, PLI_BUTTON, 0, "BUTTON X" }, { RW_WEST, PLI_BUTTON, 0, "BUTTON Y" },
    { RW_L1, PLI_BUTTON, 0, "BUTTON L" }, { RW_R1, PLI_BUTTON, 0, "BUTTON R" }, { RW_BACK, PLI_BUTTON, 0, "SELECT" }, { RW_START, PLI_BUTTON, 0, "START" },
    /* not in MiSTer's list: optional extras for pads that have them */
    { RW_L2, PLI_BUTTON, 1, "BUTTON L2" }, { RW_R2, PLI_BUTTON, 1, "BUTTON R2" }, { RW_L3, PLI_BUTTON, 1, "STICK CLICK L" }, { RW_R3, PLI_BUTTON, 1, "STICK CLICK R" },
    { RW_GUIDE, PLI_BUTTON, 1, "GUIDE" },
    { RW_LSR, PLI_STICK, 1, "Stick 1: Tilt RIGHT" }, { RW_LSL, PLI_STICK, 1, "Stick 1: Tilt LEFT" }, { RW_LSD, PLI_STICK, 1, "Stick 1: Tilt DOWN" }, { RW_LSU, PLI_STICK, 1, "Stick 1: Tilt UP" },
    { RW_RSR, PLI_STICK, 1, "Stick 2: Tilt RIGHT" }, { RW_RSL, PLI_STICK, 1, "Stick 2: Tilt LEFT" }, { RW_RSD, PLI_STICK, 1, "Stick 2: Tilt DOWN" }, { RW_RSU, PLI_STICK, 1, "Stick 2: Tilt UP" },
};
const int plsess_default_order_n = (int)(sizeof plsess_default_order / sizeof plsess_default_order[0]);

static void say(char *msg, int cap, const char *t) { if (msg && cap > 0) snprintf(msg, (size_t)cap, "%s", t); }

int plsess_begin(plsess_t *s, uint16_t vid, uint16_t pid, uint16_t ver, const hp_layout_t *L, const char *name)
{
    memset(s, 0, sizeof *s);
    pl_begin(&s->pl);
    s->vid = vid; s->pid = pid; s->ver = ver; s->sdl_ver = ver; s->L = L;
    pl_set_layout(&s->pl, L);
    if (L && L->rid) pl_set_report_id(&s->pl, L->rid);
    snprintf(s->name, sizeof s->name, "%s", name ? name : "");
    for (char *p = s->name; *p; p++) if (*p == ',' || *p == '\n' || *p == '\r') *p = ' ';
    return PLS_OK;
}

int plsess_baseline_samples(plsess_t *s, const pl_sample_t *b, int n)
{
    if (s->state == 2) return PLS_BAD;
    if (n < 1) return PLS_NOREPORT;
    if (n > PLS_IDLE_SAMPLES_MAX) n = PLS_IDLE_SAMPLES_MAX;
    pl_baseline(&s->pl, b, n);
    s->state = 1;
    return PLS_OK;
}

int plsess_baseline(plsess_t *s, pl_capture_fn cap, void *ctx)
{
    pl_sample_t b[PLS_IDLE_SAMPLES];
    if (s->state == 2) return PLS_BAD;
    int n = cap(ctx, PLC_IDLE | (0xFF << 8), b, PLS_IDLE_SAMPLES);
    if (n < 0) return PLS_CANCEL;
    if (n == 0) return PLS_NOREPORT;
    return plsess_baseline_samples(s, b, n > PLS_IDLE_SAMPLES ? PLS_IDLE_SAMPLES : n);
}

static int result(plsess_t *s, int rc, char *msg, int cap)
{
    switch (rc) {
    case PL_OK: s->nlearned++; say(msg, cap, "OK"); return PLS_OK;
    case PL_NOCHANGE: say(msg, cap, "No change: press it again"); return PLS_NOCHANGE;
    case PL_AMBIG: say(msg, cap, "Press only this button"); return PLS_AMBIG;
    case PL_FULL: say(msg, cap, "Too many inputs"); return PLS_FULL;
    default: say(msg, cap, "Error"); return PLS_BAD;
    }
}

static int capture_rc(int n, char *msg, int cap)
{
    if (n < 0) { say(msg, cap, "Canceled"); return PLS_CANCEL; }
    say(msg, cap, "No report from the pad");
    return PLS_NOREPORT;
}

int plsess_learn(plsess_t *s, int rw, const pl_sample_t *p, int n, char *msg, int mcap)
{
    if (s->state != 1 || rw < 0 || rw > 31 || n < 1) { say(msg, mcap, "Error"); return PLS_BAD; }
    if (n > PLS_MAX_SAMPLES) n = PLS_MAX_SAMPLES;
    return result(s, rw >= RW_LSR ? pl_learn_stick(&s->pl, rw, p, n) : pl_learn_button(&s->pl, rw, p, n), msg, mcap);
}

int plsess_step(plsess_t *s, int rw, pl_capture_fn cap, void *ctx, char *msg, int mcap)
{
    pl_sample_t p[PLS_MAX_SAMPLES];
    if (s->state != 1 || rw < 0 || rw > 31) { say(msg, mcap, "Error"); return PLS_BAD; }
    int n = cap(ctx, PLC_HELD | (rw << 8), p, PLS_MAX_SAMPLES);
    if (n <= 0) return capture_rc(n, msg, mcap);
    return plsess_learn(s, rw, p, n, msg, mcap);
}

static const uint8_t DP_ORDER[4] = { RW_RIGHT, RW_LEFT, RW_DOWN, RW_UP };

/* all four directions are in: UP RIGHT DOWN LEFT are the order pl_learn_dpad wants. It takes one n for all, so use the smallest. */
static int dpad_finish(plsess_t *s, char *msg, int mcap)
{
    int n = s->dpn[0]; for (int d = 1; d < 4; d++) if (s->dpn[d] < n) n = s->dpn[d];
    const pl_sample_t *pp[4] = { s->dp[3], s->dp[0], s->dp[2], s->dp[1] };
    int rc = pl_learn_dpad(&s->pl, pp, n);
    memset(s->dpn, 0, sizeof s->dpn);
    int r = result(s, rc, msg, mcap);                                       /* counts one */
    if (r == PLS_OK) s->nlearned += 3;
    return r;
}

int plsess_dpad_part(plsess_t *s, int d, const pl_sample_t *p, int n, char *msg, int mcap)
{
    if (s->state != 1 || d < 0 || d > 3 || n < 1) { say(msg, mcap, "Error"); return PLS_BAD; }
    if (n > PLS_DPAD_SAMPLES) n = PLS_DPAD_SAMPLES;
    if (pl_changed_groups(&s->pl, p, n) == 0) { say(msg, mcap, "No change: press it again"); return PLS_NOCHANGE; }     /* the others are kept */
    memcpy(s->dp[d], p, (size_t)n * sizeof p[0]); s->dpn[d] = (uint8_t)n;
    for (int k = 0; k < 4; k++) if (!s->dpn[k]) return PLS_PARTIAL;
    return dpad_finish(s, msg, mcap);
}

int plsess_dpad_flush(plsess_t *s)
{
    static const uint8_t OUT[4] = { RW_RIGHT, RW_LEFT, RW_DOWN, RW_UP };
    int mask = 0;
    if (s->state != 1) return 0;
    for (int d = 0; d < 4; d++) {
        if (!s->dpn[d]) continue;
        if (learn_one(&s->pl, OUT[d], s->dp[d], s->dpn[d], 2) == PL_OK) mask |= 1 << d;
    }
    memset(s->dpn, 0, sizeof s->dpn);
    return mask;
}

int plsess_dpad(plsess_t *s, pl_capture_fn cap, void *ctx, char *msg, int mcap)
{
    if (s->state != 1) { say(msg, mcap, "Error"); return PLS_BAD; }
    for (int d = 0; d < 4; d++) {
        if (s->dpn[d]) continue;
        pl_sample_t p[PLS_DPAD_SAMPLES];
        int n = cap(ctx, PLC_HELD | (DP_ORDER[d] << 8), p, PLS_DPAD_SAMPLES);
        if (n <= 0) { if (n < 0) memset(s->dpn, 0, sizeof s->dpn); return capture_rc(n, msg, mcap); }
        if (n > PLS_DPAD_SAMPLES) n = PLS_DPAD_SAMPLES;
        if (pl_changed_groups(&s->pl, p, n) == 0) { say(msg, mcap, "No change: press it again"); return PLS_NOCHANGE; }     /* the others are kept */
        memcpy(s->dp[d], p, (size_t)n * sizeof p[0]); s->dpn[d] = (uint8_t)n;
    }
    return dpad_finish(s, msg, mcap);
}

void plsess_set_sdl_version(plsess_t *s, uint16_t ver) { s->sdl_ver = ver; }

int plsess_skip(plsess_t *s, int rw)
{
    if (s->state == 2 || rw < 0 || rw > 31) return PLS_BAD;
    s->nskipped++;
    return PLS_OK;
}

void plsess_abort(plsess_t *s) { s->state = 2; memset(s->dpn, 0, sizeof s->dpn); }

/* ---- the user database: replace-or-append one line, streaming, constant memory ---- */
#define GUIDLEN 32
#define HEADLEN (GUIDLEN + 1 + (int)sizeof(PLS_NAME_TAG) - 1)
typedef struct { const plsess_files_t *f; char b[128]; int n, ok; char last; } outbuf_t;
static void ob_flush(outbuf_t *o) { if (o->ok && o->n) o->ok = o->f->db_write(o->f->ctx, o->b, o->n); o->n = 0; }
static void ob_put(outbuf_t *o, const char *p, int n)
{
    for (int i = 0; i < n; i++) { if (o->n == (int)sizeof o->b) ob_flush(o); o->b[o->n++] = p[i]; o->last = p[i]; }
}

int pl_sdl_db_update(const plsess_files_t *f, const char *guid, const char *line)
{
    char in[128], head[HEADLEN];
    int hl = 0, mode = 0;                                   /* 0: collecting the head of a line, 1: copying the rest, 2: dropping the rest */
    outbuf_t o; o.f = f; o.n = 0; o.ok = 1; o.last = '\n';
    if (!f->db_open_new || !f->db_open_new(f->ctx)) return 0;
    if (f->db_open_old && f->db_open_old(f->ctx)) {
        int n;
        while (o.ok && (n = f->db_read(f->ctx, in, (int)sizeof in)) > 0) {
            for (int i = 0; i < n; i++) {
                char ch = in[i];
                if (mode == 0) {
                    if (ch == '\n') { ob_put(&o, head, hl); ob_put(&o, "\n", 1); hl = 0; continue; }        /* a short line: never ours */
                    head[hl++] = ch;
                    if (hl == HEADLEN) {
                        int same = !strncasecmp(head, guid, GUIDLEN) && head[GUIDLEN] == ',' && !strncmp(head + GUIDLEN + 1, PLS_NAME_TAG, sizeof(PLS_NAME_TAG) - 1);
                        if (same) mode = 2; else { ob_put(&o, head, hl); mode = 1; }
                        hl = 0;
                    }
                } else if (mode == 1) {
                    ob_put(&o, &ch, 1);
                    if (ch == '\n') mode = 0;
                } else if (ch == '\n') mode = 0;
            }
        }
        if (mode == 0 && hl > 0) ob_put(&o, head, hl);                                   /* a short unterminated last line */
        if (f->db_close_old) f->db_close_old(f->ctx);
    }
    if (o.last != '\n') ob_put(&o, "\n", 1);
    ob_put(&o, line, (int)strlen(line)); ob_put(&o, "\n", 1);
    ob_flush(&o);
    int ok = o.ok && f->db_commit && f->db_commit(f->ctx);
    if (!ok && f->db_discard) f->db_discard(f->ctx);
    return ok;
}
