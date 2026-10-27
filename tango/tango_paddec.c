// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 the Tango authors
/*
 * tango_paddec.c -- see tango_paddec.h. Tango, our own code.
 */
#include <string.h>
#include <stdio.h>
#include "tango_paddec.h"

/* ------------------------------------------------------------------ entries, evaluator, wire */
uint64_t pd_ent_pack(const pd_ent_t *e)
{
    return ((uint64_t)(e->out & 31) << 31) | ((uint64_t)(e->cmp & 7) << 28) | ((uint64_t)(e->byte & 15) << 24) |
           ((uint64_t)(e->shift & 7) << 21) | ((uint64_t)(e->widthm1 & 15) << 17) | ((uint64_t)(e->bias & 1) << 16) | e->thr;
}

void pd_ent_unpack(uint64_t w, pd_ent_t *e)
{
    e->out = (uint8_t)((w >> 31) & 31); e->cmp = (uint8_t)((w >> 28) & 7); e->byte = (uint8_t)((w >> 24) & 15);
    e->shift = (uint8_t)((w >> 21) & 7); e->widthm1 = (uint8_t)((w >> 17) & 15); e->bias = (uint8_t)((w >> 16) & 1);
    e->thr = (uint16_t)(w & 0xFFFF);
}

static int ent_true(const pd_ent_t *e, const uint8_t *b)
{
    uint32_t w = (uint32_t)b[e->byte] | ((e->byte + 1 < PD_RPT_BYTES ? (uint32_t)b[e->byte + 1] : 0u) << 8) |
                 ((e->byte + 2 < PD_RPT_BYTES ? (uint32_t)b[e->byte + 2] : 0u) << 16);
    uint32_t mask = (1u << (e->widthm1 + 1)) - 1u;
    uint32_t f = (w >> e->shift) & mask;
    if (e->bias) f ^= 1u << e->widthm1;
    switch (e->cmp) {
    case PDC_NZ:  return f != 0;
    case PDC_Z:   return f == 0;
    case PDC_GE:  return f >= e->thr;
    case PDC_LE:  return f <= e->thr;
    case PDC_EQ:  return f == e->thr;
    case PDC_HAT: {
        uint32_t base = (e->thr >> 8) & 0xF;
        if (f < base) return 0;
        uint32_t n = f - base;
        return n <= 7 && ((e->thr >> n) & 1);
    }
    default: return 0;
    }
}

int pd_eval(const pd_table_t *t, const uint8_t *rpt, int len, uint32_t *raw)
{
    uint8_t b[PD_RPT_BYTES];
    for (int i = 0; i < PD_RPT_BYTES; i++) b[i] = (i < len) ? rpt[i] : 0;     /* bytes at or after the length read 0 */
    if (t->mode == PDM_QUIET) { *raw = 0; return 1; }
    if (len < t->min_len) return 0;
    if (t->rid_en && b[0] != t->rid_val) return 0;
    uint32_t r = 0;
    for (int i = 0; i < t->count && i < PD_MAXENT; i++)
        if (ent_true(&t->e[i], b)) r |= 1u << t->e[i].out;
    *raw = r;
    return 1;
}

int pd_cycles(const pd_table_t *t) { return t->count + 4; }

int pd_wire(const pd_table_t *t, uint8_t *out, int cap)
{
    int n = 5 + 5 * t->count;
    if (cap < n) return -1;
    out[0] = t->mode; out[1] = (uint8_t)(t->rid_en & 1); out[2] = t->rid_val; out[3] = t->min_len; out[4] = t->count;
    for (int i = 0; i < t->count; i++) {
        uint64_t w = pd_ent_pack(&t->e[i]);
        for (int k = 0; k < 5; k++) out[5 + 5 * i + k] = (uint8_t)(w >> (8 * (4 - k)));
    }
    return n;
}

int pd_unwire(const uint8_t *in, int n, pd_table_t *t)
{
    memset(t, 0, sizeof *t);
    if (n < 5) return 0;
    t->mode = in[0]; t->rid_en = in[1] & 1; t->rid_val = in[2]; t->min_len = in[3]; t->count = in[4];
    if (t->count > PD_MAXENT || n < 5 + 5 * t->count) return 0;
    for (int i = 0; i < t->count; i++) {
        uint64_t w = 0;
        for (int k = 0; k < 5; k++) w = (w << 8) | in[5 + 5 * i + k];
        pd_ent_unpack(w, &t->e[i]);
    }
    return 1;
}

/* ------------------------------------------------------------------ appenders */
int pd_add(pd_table_t *t, const pd_ent_t *e)
{
    if (t->count >= PD_MAXENT) return 0;
    t->e[t->count++] = *e;
    return 1;
}

int pd_add_bit(pd_table_t *t, int out, int bit, int active_low)
{
    pd_ent_t e; memset(&e, 0, sizeof e);
    if (bit < 0 || bit >= 8 * PD_RPT_BYTES) return 0;
    e.out = (uint8_t)out; e.cmp = active_low ? PDC_Z : PDC_NZ; e.byte = (uint8_t)(bit / 8); e.shift = (uint8_t)(bit % 8); e.widthm1 = 0;
    return pd_add(t, &e);
}

/* quarter points of a logical range, in the unsigned domain the engine compares in */
static int axis_ent(pd_ent_t *e, int out, int bit, int size, int lmin, int lmax)
{
    memset(e, 0, sizeof *e);
    if (size < 1 || size > 16 || bit < 0 || bit + size > 8 * PD_RPT_BYTES || lmax <= lmin) return 0;
    e->out = (uint8_t)out; e->byte = (uint8_t)(bit / 8); e->shift = (uint8_t)(bit % 8); e->widthm1 = (uint8_t)(size - 1);
    e->bias = lmin < 0;
    return 1;
}

static int to_u(int v, int size, int sgn)
{
    int u = sgn ? v + (1 << (size - 1)) : v;
    if (u < 0) u = 0;
    if (u > 0xFFFF) u = 0xFFFF;
    return u;
}

/* mode: +1 high half (GE hi), -1 low half (LE lo), 0 trigger pressed high (GE lmin+q), 2 trigger pressed low (LE lmax-q) */
static int add_axis(pd_table_t *t, int out, int bit, int size, int lmin, int lmax, int mode)
{
    pd_ent_t e;
    if (!axis_ent(&e, out, bit, size, lmin, lmax)) return 0;
    int r = lmax - lmin, q = r / 4;
    if (q < 1) q = 1;
    if (mode > 0 && mode != 2) { e.cmp = PDC_GE; e.thr = (uint16_t)to_u(lmax - q, size, e.bias); }
    else if (mode < 0) { e.cmp = PDC_LE; e.thr = (uint16_t)to_u(lmin + q, size, e.bias); }
    else if (mode == 0) { e.cmp = PDC_GE; e.thr = (uint16_t)to_u(lmin + q, size, e.bias); }
    else { e.cmp = PDC_LE; e.thr = (uint16_t)to_u(lmax - q, size, e.bias); }
    return pd_add(t, &e);
}

int pd_add_axis(pd_table_t *t, int out, int bit, int size, int lmin, int lmax, int dir)
{
    return add_axis(t, out, bit, size, lmin, lmax, dir);
}

int pd_add_hat(pd_table_t *t, int out, int bit, int size, int lmin, int lmax, int dirmask)
{
    pd_ent_t e; memset(&e, 0, sizeof e);
    int steps = lmax - lmin + 1;
    if (size < 3 || size > 8 || bit < 0 || bit + size > 8 * PD_RPT_BYTES || lmin < 0 || lmin > 15) return 0;
    uint8_t set = 0;
    if (steps >= 8) {                       /* N NE E SE S SW W NW */
        if (dirmask & 1) set |= 0x83;       /* up:    NW N NE   = 7 0 1 */
        if (dirmask & 2) set |= 0x0E;       /* right: NE E SE   = 1 2 3 */
        if (dirmask & 4) set |= 0x38;       /* down:  SE S SW   = 3 4 5 */
        if (dirmask & 8) set |= 0xE0;       /* left:  SW W NW   = 5 6 7 */
    } else if (steps == 4) {                /* N E S W */
        if (dirmask & 1) set |= 0x01;
        if (dirmask & 2) set |= 0x02;
        if (dirmask & 4) set |= 0x04;
        if (dirmask & 8) set |= 0x08;
    } else return 0;
    e.out = (uint8_t)out; e.cmp = PDC_HAT; e.byte = (uint8_t)(bit / 8); e.shift = (uint8_t)(bit % 8); e.widthm1 = (uint8_t)(size - 1);
    e.thr = (uint16_t)((lmin << 8) | set);
    return pd_add(t, &e);
}

static void finish_header(pd_table_t *t, const hp_layout_t *L)
{
    t->mode = PDM_TABLE;
    t->rid_en = L && L->rid != 0; t->rid_val = L ? L->rid : 0;
    int last = 0;
    for (int i = 0; i < t->count; i++) {
        int lb = t->e[i].byte + (t->e[i].shift + t->e[i].widthm1) / 8 + 1;
        if (lb > last) last = lb;
    }
    t->min_len = (uint8_t)last;
}

/* ------------------------------------------------------------------ built-in tables */
void pd_quiet(pd_table_t *t) { memset(t, 0, sizeof *t); t->mode = PDM_QUIET; }

void pd_builtin_ds4(pd_table_t *t)
{
    memset(t, 0, sizeof *t);
    t->mode = PDM_TABLE; t->rid_en = 0; t->min_len = 8;      /* a DS4 report is 64 bytes; byte 7 must exist */
    /* byte 5 = {triangle, circle, cross, square, hat[3:0]} (tango_ds4_joy.v 19-21, 88-95) */
    pd_add_hat(t, RW_UP,    5 * 8, 4, 0, 7, 1);
    pd_add_hat(t, RW_RIGHT, 5 * 8, 4, 0, 7, 2);
    pd_add_hat(t, RW_DOWN,  5 * 8, 4, 0, 7, 4);
    pd_add_hat(t, RW_LEFT,  5 * 8, 4, 0, 7, 8);
    pd_add_bit(t, RW_WEST,  5 * 8 + 4, 0);      /* square */
    pd_add_bit(t, RW_SOUTH, 5 * 8 + 5, 0);      /* cross */
    pd_add_bit(t, RW_EAST,  5 * 8 + 6, 0);      /* circle */
    pd_add_bit(t, RW_NORTH, 5 * 8 + 7, 0);      /* triangle */
    /* byte 6 = {R3, L3, options, share, R2, L2, R1, L1} */
    static const uint8_t b6[8] = { RW_L1, RW_R1, RW_L2, RW_R2, RW_BACK, RW_START, RW_L3, RW_R3 };
    for (int i = 0; i < 8; i++) pd_add_bit(t, b6[i], 6 * 8 + i, 0);
    /* byte 7 = {counter[5:0], touchpad, PS} */
    pd_add_bit(t, RW_GUIDE, 7 * 8 + 0, 0);
    pd_add_bit(t, RW_MISC,  7 * 8 + 1, 0);
}

void pd_builtin_xinput(pd_table_t *t)
{
    memset(t, 0, sizeof *t);
    t->mode = PDM_TABLE; t->rid_en = 1; t->rid_val = 0; t->min_len = 14;
    /* byte 0 = message type 0, byte 1 = length 20 (mcu_hw.c xbox_parse 366); wButtons = bytes 3:2 little endian */
    pd_add_bit(t, RW_UP,    2 * 8 + 0, 0); pd_add_bit(t, RW_DOWN,  2 * 8 + 1, 0);
    pd_add_bit(t, RW_LEFT,  2 * 8 + 2, 0); pd_add_bit(t, RW_RIGHT, 2 * 8 + 3, 0);
    pd_add_bit(t, RW_START, 2 * 8 + 4, 0); pd_add_bit(t, RW_BACK,  2 * 8 + 5, 0);
    pd_add_bit(t, RW_L3,    2 * 8 + 6, 0); pd_add_bit(t, RW_R3,    2 * 8 + 7, 0);
    pd_add_bit(t, RW_L1,    3 * 8 + 0, 0); pd_add_bit(t, RW_R1,    3 * 8 + 1, 0);
    pd_add_bit(t, RW_GUIDE, 3 * 8 + 2, 0);
    pd_add_bit(t, RW_SOUTH, 3 * 8 + 4, 0); pd_add_bit(t, RW_EAST,  3 * 8 + 5, 0);
    pd_add_bit(t, RW_WEST,  3 * 8 + 6, 0); pd_add_bit(t, RW_NORTH, 3 * 8 + 7, 0);
    pd_add_axis(t, RW_L2, 4 * 8, 8, 0, 255, 0);                       /* triggers: bytes 4, 5, 0..255 */
    pd_add_axis(t, RW_R2, 5 * 8, 8, 0, 255, 0);
    static const struct { int bit, hi, lo; } ST[4] = { { 6 * 8, RW_LSR, RW_LSL }, { 8 * 8, RW_LSU, RW_LSD },     /* LX; LY is up-positive */
                                                       { 10 * 8, RW_RSR, RW_RSL }, { 12 * 8, RW_RSU, RW_RSD } };
    for (int i = 0; i < 4; i++) {                                      /* sticks: signed 16 bit little endian */
        pd_add_axis(t, ST[i].hi, ST[i].bit, 16, -32768, 32767, +1);
        pd_add_axis(t, ST[i].lo, ST[i].bit, 16, -32768, 32767, -1);
    }
}

/* ------------------------------------------------------------------ database line + layout -> table */
static const uint8_t SN_RAW[SN_COUNT] = {
    [SN_A] = RW_SOUTH, [SN_B] = RW_EAST, [SN_X] = RW_WEST, [SN_Y] = RW_NORTH, [SN_BACK] = RW_BACK, [SN_GUIDE] = RW_GUIDE,
    [SN_START] = RW_START, [SN_LSTICK] = RW_L3, [SN_RSTICK] = RW_R3, [SN_LSHOULDER] = RW_L1, [SN_RSHOULDER] = RW_R1,
    [SN_DPUP] = RW_UP, [SN_DPDOWN] = RW_DOWN, [SN_DPLEFT] = RW_LEFT, [SN_DPRIGHT] = RW_RIGHT,
    [SN_LEFTX] = RW_LSR, [SN_LEFTY] = RW_LSD, [SN_RIGHTX] = RW_RSR, [SN_RIGHTY] = RW_RSD,
    [SN_LTRIGGER] = RW_L2, [SN_RTRIGGER] = RW_R2, [SN_MISC1] = RW_MISC,
};

static const hp_field_t *f_btn(const hp_layout_t *L, int n)   { return (n < HP_MAX_BTN && L->bidx[n] >= 0) ? &L->f[L->bidx[n]] : NULL; }
static const hp_field_t *f_axis(const hp_layout_t *L, int n)  { return (n < HP_MAX_AXES && L->aidx[n] >= 0) ? &L->f[L->aidx[n]] : NULL; }
static const hp_field_t *f_hat(const hp_layout_t *L, int n)   { return (n < HP_MAX_HATS && L->hidx[n] >= 0) ? &L->f[L->hidx[n]] : NULL; }

static int is_dir(int sn) { return sn >= SN_DPUP && sn <= SN_DPRIGHT; }
static int is_face(int sn) { return sn <= SN_Y; }
static int is_stick(int sn) { return sn >= SN_LEFTX && sn <= SN_RIGHTY; }
static int is_trig(int sn) { return sn == SN_LTRIGGER || sn == SN_RTRIGGER; }

static int add_binding(pd_table_t *t, const hp_layout_t *L, int sn, const sdl_bind_t *b, int force_out, int *resolved)
{
    int out = force_out >= 0 ? force_out : SN_RAW[sn];
    *resolved = 0;
    switch (b->type) {
    case SB_BTN: {
        const hp_field_t *f = f_btn(L, b->idx);
        if (!f) return 0;
        pd_ent_t e; memset(&e, 0, sizeof e);
        if (f->bit + f->size > 8 * PD_RPT_BYTES || f->size > 16) return 0;
        e.out = (uint8_t)out; e.cmp = PDC_NZ; e.byte = (uint8_t)(f->bit / 8); e.shift = (uint8_t)(f->bit % 8); e.widthm1 = (uint8_t)(f->size - 1);
        *resolved = 1;
        return pd_add(t, &e);
    }
    case SB_HAT: {
        const hp_field_t *f = f_hat(L, b->idx);
        if (!f) return 0;
        if (!pd_add_hat(t, out, f->bit, f->size, f->lmin, f->lmax, b->hatmask)) return 0;
        *resolved = 1;
        return 1;
    }
    default: break;
    }
    const hp_field_t *f = f_axis(L, b->idx);
    if (!f) return 0;
    int ok;
    if (is_stick(sn)) {                                  /* one SDL axis = two raw directions: the "positive" one is sn's own bit */
        int pos = out, neg = (sn == SN_LEFTX) ? RW_LSL : (sn == SN_LEFTY) ? RW_LSU : (sn == SN_RIGHTX) ? RW_RSL : RW_RSU;
        if (force_out >= 0) { pos = force_out; neg = force_out ^ 1; }     /* 18/19 20/21 22/23 24/25 and 0/1 2/3 come in pairs */
        if (b->inv) { int s = pos; pos = neg; neg = s; }
        ok = add_axis(t, pos, f->bit, f->size, f->lmin, f->lmax, +1) && add_axis(t, neg, f->bit, f->size, f->lmin, f->lmax, -1);
    } else if (is_trig(sn) || b->type == SB_AXIS) {      /* an analog trigger (or a full axis used as a button): pressed past a quarter */
        ok = add_axis(t, out, f->bit, f->size, f->lmin, f->lmax, b->inv ? 2 : 0);
    } else {                                             /* half axis as a button / d-pad direction */
        int hi = (b->type == SB_AXIS_POS) ^ (b->inv ? 1 : 0);
        ok = add_axis(t, out, f->bit, f->size, f->lmin, f->lmax, hi ? +1 : -1);
    }
    *resolved = ok;
    return ok;
}

int pd_from_sdl(const hp_layout_t *L, const sdl_map_t *m, pd_table_t *t, pd_build_info_t *info)
{
    pd_build_info_t li; memset(&li, 0, sizeof li);
    memset(t, 0, sizeof *t);
    int core_unres = 0;
    for (int sn = 0; sn < SN_COUNT; sn++) {
        const sdl_bind_t *b = &m->b[sn];
        if (b->type == SB_NONE) continue;
        li.nbind++;
        int res = 0;
        int ok = add_binding(t, L, sn, b, -1, &res);
        if (res) {
            li.nres++;
            if (is_dir(sn)) li.has_dpad = 1;
            if (is_face(sn)) li.nface++;
        } else {
            li.nunres++;
            if (is_dir(sn) || is_face(sn)) core_unres++;
        }
        if (!ok && t->count >= PD_MAXENT) li.too_many = 1;
    }
    /* a pad whose line has no d-pad but has a left stick (SDL "dpad = left stick" pads are written the other way round by the
     * database, so this is the rare case): the stick drives the directions too */
    if (!li.has_dpad && (m->b[SN_LEFTX].type || m->b[SN_LEFTY].type)) {
        int res;
        if (m->b[SN_LEFTX].type) { add_binding(t, L, SN_LEFTX, &m->b[SN_LEFTX], RW_RIGHT, &res); li.has_dpad |= res; }
        if (m->b[SN_LEFTY].type) { add_binding(t, L, SN_LEFTY, &m->b[SN_LEFTY], RW_DOWN, &res); li.has_dpad |= res; }
    }
    finish_header(t, L);
    if (info) *info = li;
    return li.has_dpad && li.nface >= 2 && core_unres == 0 && !li.too_many && t->count > 0;
}

/* ------------------------------------------------------------------ descriptor only */
/* does the axis rest within the outer eighth of its range at either end (a trigger), judged from a report taken at rest */
static int rests_at_end(const hp_field_t *f, const uint8_t *rest, int rest_len)
{
    if (!f || !rest || f->size < 2 || f->size > 16 || f->bit + f->size > 8 * rest_len) return 0;
    int32_t v = 0; for (int k = 0; k < f->size; k++) v |= (int32_t)((rest[(f->bit + k) >> 3] >> ((f->bit + k) & 7)) & 1) << k;
    if (f->sgn && (v & (1 << (f->size - 1)))) v -= 1 << f->size;                         /* two's complement */
    int32_t span = f->lmax > f->lmin ? f->lmax - f->lmin : (1 << f->size), e = span / 8;  /* the LOGICAL range: a 16-bit field spanning -1800..1800 */
    return v < f->lmin + e ? 1 : v > f->lmax - e ? 2 : 0;       /* 1: rests low (a trigger pulled towards high), 2: rests high (pulled towards low) */
}

int pd_from_layout(const hp_layout_t *L, pd_table_t *t, pd_build_info_t *info) { return pd_from_layout_rest(L, NULL, 0, t, info); }

int pd_from_layout_rest(const hp_layout_t *L, const uint8_t *rest, int rest_len, pd_table_t *t, pd_build_info_t *info)
{
    pd_build_info_t li; memset(&li, 0, sizeof li);
    memset(t, 0, sizeof *t);
    const hp_field_t *hat = f_hat(L, 0);
    /* with a rest report: the centred axes are the sticks (and the d-pad of a pad with no hat), the ones that rest at an end are triggers */
    const hp_field_t *cent[2] = { NULL, NULL }, *trig[2] = { NULL, NULL }; int nc = 0, nt = 0, tmode[2] = { 0, 0 };
    for (int k = 0; k < L->naxis; k++) {
        const hp_field_t *f = f_axis(L, k);
        if (!f) continue;
        int re = rests_at_end(f, rest, rest_len);
        if (re) { if (nt < 2) { tmode[nt] = re == 1 ? 0 : 2; trig[nt++] = f; } }
        else if (nc < 2) cent[nc++] = f;
    }
    /* the first report is only a guess at rest (the user may be holding a direction as the pad is plugged in): with no hat, ONE axis at an end and one centred is a
     * d-pad held to a side, not a trigger and a stick, so it is not believed.  Two axes at ends are two triggers (a d-pad on the axes would hold two directions on
     * at rest, a menu that moves by itself, so it is left to Define). */
    if (rest && !hat && nc == 1) return pd_from_layout_rest(L, NULL, 0, t, info);
    const hp_field_t *ax = cent[0], *ay = cent[1];
    if (hat) {
        static const struct { int out, mask; } D[4] = { { RW_UP, 1 }, { RW_RIGHT, 2 }, { RW_DOWN, 4 }, { RW_LEFT, 8 } };
        for (int i = 0; i < 4; i++) if (pd_add_hat(t, D[i].out, hat->bit, hat->size, hat->lmin, hat->lmax, D[i].mask)) li.has_dpad = 1;
    }
    if (ax && ay) {                                                      /* the first two centred axes: the stick; also the d-pad when there is no hat */
        int s[4] = { RW_LSR, RW_LSL, RW_LSD, RW_LSU };
        add_axis(t, s[0], ax->bit, ax->size, ax->lmin, ax->lmax, +1); add_axis(t, s[1], ax->bit, ax->size, ax->lmin, ax->lmax, -1);
        add_axis(t, s[2], ay->bit, ay->size, ay->lmin, ay->lmax, +1); add_axis(t, s[3], ay->bit, ay->size, ay->lmin, ay->lmax, -1);
        if (!hat) {
            add_axis(t, RW_RIGHT, ax->bit, ax->size, ax->lmin, ax->lmax, +1); add_axis(t, RW_LEFT, ax->bit, ax->size, ax->lmin, ax->lmax, -1);
            add_axis(t, RW_DOWN,  ay->bit, ay->size, ay->lmin, ay->lmax, +1); add_axis(t, RW_UP,   ay->bit, ay->size, ay->lmin, ay->lmax, -1);
            li.has_dpad = 1;
        }
    }
    if (trig[0]) add_axis(t, RW_L2, trig[0]->bit, trig[0]->size, trig[0]->lmin, trig[0]->lmax, tmode[0]);
    if (trig[1]) add_axis(t, RW_R2, trig[1]->bit, trig[1]->size, trig[1]->lmin, trig[1]->lmax, tmode[1]);
    /* buttons by position in the evdev order. 8 or fewer: a SNES-style pad (A B X Y L R Select Start); more: a PlayStation / Xbox
     * style pad (the next two are L2 R2, then Select Start, the sticks, Guide, misc).  A guess: Define buttons fixes it. */
    static const uint8_t SNES[8]  = { RW_SOUTH, RW_EAST, RW_WEST, RW_NORTH, RW_L1, RW_R1, RW_BACK, RW_START };
    static const uint8_t BIG[14]  = { RW_SOUTH, RW_EAST, RW_WEST, RW_NORTH, RW_L1, RW_R1, RW_L2, RW_R2, RW_BACK, RW_START, RW_L3, RW_R3, RW_GUIDE, RW_MISC };
    const uint8_t *map = L->nbtn <= 8 ? SNES : BIG;
    int nmap = L->nbtn <= 8 ? 8 : 14;
    for (int i = 0; i < L->nbtn && i < nmap; i++) {
        const hp_field_t *f = f_btn(L, i);
        if (!f || f->bit + f->size > 8 * PD_RPT_BYTES) { li.nunres++; continue; }
        if (pd_add_bit(t, map[i], f->bit, 0)) { li.nres++; if (i < 4) li.nface++; }
    }
    finish_header(t, L);
    if (info) *info = li;
    return li.has_dpad && L->nbtn >= 2 && t->count > 0;
}

void pd_dump(const pd_table_t *t, char *buf, int cap)
{
    static const char *const C[] = { "NZ", "Z", "GE", "LE", "EQ", "HAT", "?", "?" };
    int n = snprintf(buf, (size_t)cap, "mode %d rid %s%02x min %d n %d:", t->mode, t->rid_en ? "=" : "-", t->rid_val, t->min_len, t->count);
    for (int i = 0; i < t->count && n < cap - 40; i++)
        n += snprintf(buf + n, (size_t)(cap - n), " [r%d %s b%d.%d w%d%s %x]", t->e[i].out, C[t->e[i].cmp & 7], t->e[i].byte, t->e[i].shift,
                      t->e[i].widthm1 + 1, t->e[i].bias ? "s" : "", t->e[i].thr);
}
