// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 the Tango authors
/*
 * padgen.h -- a PARAMETERISED, SEEDED generator of synthetic USB HID pads, for the host tests (test_padgen.c) and for the RTL bench
 * (menu-tango fpga/sim/pads/gen_<seed>.prof, written by test_padgen --emit).  Our own code.
 *
 * Why: "any pad a user brings must work" cannot be shown with a handful of hand-written fixtures.  A generated pad is a complete GROUND TRUTH:
 * the USB side (speed, bcdUSB, bMaxPacketSize0, the interrupt endpoint's address / size / interval, an OUT endpoint or not, interfaces before the
 * pad, start-up behaviour of the device), the HID report descriptor, and what each physical control does to the report.  The oracle is therefore
 * independent of the code under test: the descriptor bytes are written here, item by item, from the HID 1.11 syntax; the reports are built from
 * the field positions chosen here, not from any decode table.
 *
 * What a generated pad varies (all drawn from the seed):
 *   speed low / full; bcdUSB 1.00 1.10 2.00 2.10; bMaxPacketSize0 8 16 32 64 (low speed 8); interrupt IN endpoint 1..15, wMaxPacketSize 8..64,
 *   bInterval 1..255, an OUT endpoint present or absent; 0-2 interfaces before the pad (audio, boot keyboard, vendor); report size 1..64 bytes
 *   with or without a report id; d-pad as an 8-way hat (null state out of range at several values), a 4-way hat, the X/Y axes, or four buttons
 *   (Button page or Generic Desktop 0x90..0x93); 0..32 buttons (one usage range or one usage each), some DECLARED BUT NEVER SENT; 0-2 sticks and
 *   0-2 triggers at 8 / 10 / 12 / 16 bits, signed or unsigned, resting at the centre or at an end; constant padding bits and bytes, vendor bytes
 *   (constant, or a counter that changes on every report), field order shuffled, controls pushed past byte 15 (a known limit of the decode:
 *   PD_RPT_BYTES); idle wobble on the analog axes; a second report id interleaved; a device that NAKs for many polls before it sends, that stays
 *   silent until SET_IDLE, that STALLs string requests.
 */
#ifndef PADGEN_H
#define PADGEN_H

#include <stdint.h>
#include <string.h>
#include <stdio.h>

#define GP_MAXF 64
#define GP_MAXC 160
#define GP_MAXD 700

typedef struct { uint32_t s; uint32_t cnt; } gp_rng_t;
static inline uint32_t gp_rnd(gp_rng_t *r) { uint32_t x = r->s; x ^= x << 13; x ^= x >> 17; x ^= x << 5; r->s = x ? x : 0x9E3779B9u; return r->s; }
static inline int gp_rn(gp_rng_t *r, int n) { return n <= 1 ? 0 : (int)(gp_rnd(r) % (uint32_t)n); }
static inline int gp_pct(gp_rng_t *r, int p) { return gp_rn(r, 100) < p; }

enum { GF_BTN = 1, GF_HAT, GF_AXIS, GF_PAD, GF_VEND, GF_CONST, GF_BTNGD };       /* GF_BTNGD: Generic Desktop d-pad buttons 0x90..0x93 */
enum { CT_BTN = 1, CT_HAT, CT_AXIS };
enum { GR_DPAD = 1, GR_BTN, GR_STICK, GR_TRIG };

typedef struct {
    uint8_t  kind;                 /* GF_ */
    uint16_t bit;                  /* offset in the payload (the report id byte counts as byte 0) */
    uint8_t  size;                 /* bits per element */
    uint8_t  count;                /* elements */
    int32_t  lmin, lmax;
    uint8_t  sgn;
    uint16_t page, usage;          /* first usage (axis, hat); buttons use page 9 usages 1..count */
    uint8_t  form;                 /* buttons: 0 usage min/max, 1 one usage each.  axes: 0 one field per axis, 1 two axes in one item (count 2) */
    int32_t  nullv;                /* hat: the value that means "centred" */
    uint8_t  rest_low;             /* trigger: rests at lmin (signed) or 0 */
    uint8_t  vconst;               /* vendor: 1 constant byte value, 0 changing; then form = 0 counter (+1 per report), 1 sensor (low 3 bits random), 2 random (stress) */
} gp_field_t;

typedef struct {
    uint8_t kind;                  /* CT_ */
    uint8_t role;                  /* GR_ */
    uint8_t fld, elem;             /* field index, element inside a button block / axis pair */
    int8_t  dir;                   /* hat: 0 up 1 right 2 down 3 left (4-way) or 0..7 clockwise from up (8-way) ; axis: +1 high -1 low */
    uint8_t absent;                /* declared in the descriptor, never sent by the hardware */
    uint8_t slot;                  /* d-pad direction 0..3 = RIGHT LEFT DOWN UP, stick 0..1 * 4 + dir (R L D U), trigger 0..1, button ordinal */
} gp_ctl_t;

typedef struct {
    uint32_t seed;
    /* USB side */
    int ls;                        /* low speed */
    uint16_t bcd_usb, vid, pid, ver;
    int mps0, ep_addr, ep_mps, ep_iv, has_out, n_pre;
    int hid_if;                    /* interface number of the pad */
    int pre_kind[3];               /* 0 audio control, 1 boot keyboard (with endpoint), 2 vendor */
    int nak_polls;                 /* the device NAKs this many interrupt polls before its first report */
    int needs_idle;                /* silent until SET_IDLE */
    int stall_str;                 /* STALL string descriptor requests */
    /* HID side */
    int rid;                       /* 0: no report id */
    int rlen;                      /* payload bytes sent (report id included) */
    int app;                       /* application usage: 4 joystick, 5 gamepad */
    int wrap;                      /* 0 none, 1 physical collection, 2 logical collection (the DragonRise shape) */
    int nf; gp_field_t f[GP_MAXF];
    int nc; gp_ctl_t c[GP_MAXC];
    int dpad_kind;                 /* 0 hat8, 1 hat4, 2 axes, 3 buttons (Button page), 4 buttons (GD) */
    int nbtn, nreal, nstick, ntrig;
    int sz;                        /* analog size */
    int sgn;                       /* analog signed */
    int second_rid;                /* a second report id interleaved (0: none) */
    int wobble;                    /* idle wobble amplitude on analog axes (counts at the pad's own resolution) */
    int beyond16;                  /* number of controls placed past byte 15 */
    int max_ctl_byte;              /* highest payload byte a REAL control touches */
    uint8_t d[GP_MAXD]; int dn;    /* the report descriptor */
    uint8_t dd[18], cd[160]; int cdn;   /* device and configuration descriptors */
    uint8_t idle[64];              /* the idle report, noise-free */
} gpad_t;

/* ---- descriptor item writer: signed-minimal data encoding, as real descriptors write them ---- */
static inline int gp_it(uint8_t *o, int n, int tag, int32_t v)
{
    int sz = (v >= -128 && v <= 127) ? 1 : (v >= -32768 && v <= 32767) ? 2 : 4;
    o[n++] = (uint8_t)(tag | (sz == 4 ? 3 : sz));
    for (int i = 0; i < sz; i++) o[n++] = (uint8_t)((uint32_t)v >> (8 * i));
    return n;
}
static inline int gp_itu(uint8_t *o, int n, int tag, uint32_t v)        /* unsigned data (usage pages, usages): 1, 2 or 4 bytes */
{
    int sz = v <= 0xFF ? 1 : v <= 0xFFFF ? 2 : 4;
    o[n++] = (uint8_t)(tag | (sz == 4 ? 3 : sz));
    for (int i = 0; i < sz; i++) o[n++] = (uint8_t)(v >> (8 * i));
    return n;
}
static inline int gp_it0(uint8_t *o, int n, int tag, int32_t v)       /* always one byte (short usages / collections) */
{ o[n++] = (uint8_t)(tag | 1); o[n++] = (uint8_t)v; return n; }
#define T_UPAGE 0x04
#define T_USAGE 0x08
#define T_UMIN  0x18
#define T_UMAX  0x28
#define T_LMIN  0x14
#define T_LMAX  0x24
#define T_PMIN  0x34
#define T_PMAX  0x44
#define T_UNIT  0x64
#define T_RSIZE 0x74
#define T_RCNT  0x94
#define T_RID   0x84
#define T_INPUT 0x80
#define T_COLL  0xA0

static inline void gp_setbits(uint8_t *r, int bit, int w, uint32_t v)
{
    for (int i = 0; i < w; i++) { int b = bit + i; if ((v >> i) & 1u) r[b >> 3] |= (uint8_t)(1u << (b & 7)); else r[b >> 3] &= (uint8_t)~(1u << (b & 7)); }
}

/* ------------------------------------------------------------------------------------------------------------------------------------------ */
/* the layout: fields get consecutive bit positions; ctl[] lists the physical controls                                                          */
static inline int gp_add_field(gpad_t *g, gp_field_t *f, int *bit)
{
    f->bit = (uint16_t)*bit; *bit += f->size * f->count;
    g->f[g->nf] = *f; return g->nf++;
}
static inline void gp_add_ctl(gpad_t *g, int kind, int role, int fld, int elem, int dir, int absent, int slot)
{
    gp_ctl_t *c = &g->c[g->nc++];
    c->kind = (uint8_t)kind; c->role = (uint8_t)role; c->fld = (uint8_t)fld; c->elem = (uint8_t)elem; c->dir = (int8_t)dir; c->absent = (uint8_t)absent; c->slot = (uint8_t)slot;
}

static inline void gp_make(gpad_t *g, uint32_t seed, int force_ls)      /* force_ls: -1 draw, 0 full, 1 low */
{
    gp_rng_t R = { seed * 2654435761u + 12345u, 0 }; gp_rng_t *r = &R;
    for (int i = 0; i < 8; i++) gp_rnd(r);
    memset(g, 0, sizeof *g); g->seed = seed;
    g->ls = force_ls >= 0 ? force_ls : gp_pct(r, 30);
    { static const uint16_t bu[4] = { 0x0100, 0x0110, 0x0200, 0x0210 }; g->bcd_usb = bu[gp_rn(r, 4)]; if (g->ls && g->bcd_usb >= 0x0200) g->bcd_usb = 0x0110; }
    g->vid = (uint16_t)(0x1000 + gp_rn(r, 0xE000)); g->pid = (uint16_t)(1 + gp_rn(r, 0xFFF0)); g->ver = (uint16_t)gp_rn(r, 0x300);
    { static const int m0[4] = { 8, 16, 32, 64 }; g->mps0 = g->ls ? 8 : m0[gp_rn(r, 4)]; }
    g->ep_addr = 1 + gp_rn(r, 15);
    { static const int iv[8] = { 1, 2, 4, 8, 10, 16, 32, 255 }; g->ep_iv = iv[gp_rn(r, 8)]; }
    g->has_out = gp_pct(r, 40);
    g->n_pre = gp_pct(r, 40) ? 1 + gp_rn(r, 2) : 0;
    for (int i = 0; i < 3; i++) g->pre_kind[i] = gp_rn(r, 3);
    g->nak_polls = gp_pct(r, 10) ? 1 + gp_rn(r, 150) : 0;
    g->needs_idle = gp_pct(r, 4);
    g->stall_str = gp_pct(r, 10);
    g->app = gp_pct(r, 60) ? 5 : 4;
    g->wrap = gp_rn(r, 3);
    g->rid = gp_pct(r, 35) ? 1 + gp_rn(r, 254) : 0;
    g->second_rid = (g->rid && gp_pct(r, 30)) ? ((g->rid % 250) + 3) : 0;
    { static const int sz[4] = { 8, 10, 12, 16 }; g->sz = sz[gp_rn(r, 4)]; if (g->sz == 8 && gp_pct(r, 30)) g->sz = 8; }
    g->sgn = gp_pct(r, 30);
    g->wobble = gp_pct(r, 50) ? 1 + gp_rn(r, 3) : 0;
    { int w = gp_rn(r, 100); g->dpad_kind = w < 50 ? 0 : w < 58 ? 1 : w < 80 ? 2 : w < 92 ? 3 : 4; }
    { static const int nb[] = { 0, 1, 2, 4, 6, 8, 10, 12, 14, 16, 20, 24, 32 }; g->nbtn = nb[gp_rn(r, 13)]; }
    g->nstick = gp_pct(r, 55) ? (gp_pct(r, 50) ? 2 : 1) : 0;
    g->ntrig = gp_pct(r, 30) ? (gp_pct(r, 50) ? 2 : 1) : 0;
    if (g->dpad_kind == 2 && g->nstick == 2) g->nstick = 1;           /* X/Y are the d-pad: the sticks use the next axes */
    int bit = g->rid ? 8 : 0;

    /* ---- field order: a list of blocks, shuffled, with optional padding between */
    enum { B_DPAD, B_BTN, B_STICK0, B_STICK1, B_TRIG, B_VEND, B_CONSTB, B_PADBITS };
    int order[16], no = 0;
    order[no++] = B_DPAD; order[no++] = B_BTN;
    if (g->nstick >= 1) order[no++] = B_STICK0;
    if (g->nstick >= 2) order[no++] = B_STICK1;
    if (g->ntrig) order[no++] = B_TRIG;
    if (gp_pct(r, 50)) order[no++] = B_VEND;
    if (gp_pct(r, 40)) order[no++] = B_CONSTB;
    if (gp_pct(r, 30)) order[no++] = B_PADBITS;
    if (gp_pct(r, 35)) { for (int i = no - 1; i > 0; i--) { int j = gp_rn(r, i + 1); int t = order[i]; order[i] = order[j]; order[j] = t; } }
    if (!gp_pct(r, 15)) {                                              /* vendor / constant / padding blocks mostly come last, as on real pads */
        int tmp[16], w = 0; for (int i = 0; i < no; i++) if (order[i] < B_VEND) tmp[w++] = order[i];
        for (int i = 0; i < no; i++) if (order[i] >= B_VEND) tmp[w++] = order[i];
        for (int i = 0; i < no; i++) order[i] = tmp[i];
    }
    int beyond = gp_pct(r, 6);                                       /* a long vendor block first: every real control lands past byte 15 */
    int leading = beyond ? 16 + gp_rn(r, 8) : 0;
    if (leading) { gp_field_t f; memset(&f, 0, sizeof f); f.kind = GF_VEND; f.size = 8; f.count = (uint8_t)leading; f.page = 0xFF00; f.usage = 1; f.vconst = 1; gp_add_field(g, &f, &bit); }

    int nreal_btn = 0;
    int btn_fld = -1;
    for (int oi = 0; oi < no; oi++) {
        switch (order[oi]) {
        case B_DPAD: {
            gp_field_t f; memset(&f, 0, sizeof f);
            if (g->dpad_kind <= 1) {
                f.kind = GF_HAT; f.count = 1; f.page = 1; f.usage = 0x39;
                int n8 = g->dpad_kind == 0;
                f.size = (uint8_t)(gp_pct(r, 75) ? 4 : 8);
                int base = gp_pct(r, 25) ? 1 : 0;                       /* logical range 1..8 instead of 0..7 */
                f.lmin = base; f.lmax = base + (n8 ? 7 : 3);
                { int w = gp_rn(r, 4);                                  /* the null value: out of range, several conventions */
                  if (base) f.nullv = 0;
                  else if (f.size == 4) f.nullv = (w == 0) ? 8 : (w == 1) ? 15 : (w == 2) ? (n8 ? 8 : 4) : 15;
                  else f.nullv = (w == 0) ? 8 : (w == 1) ? 0xFF : (w == 2) ? 0x0F : 0x80; }
                int fi = gp_add_field(g, &f, &bit);
                for (int d = 0; d < 4; d++) {                           /* RIGHT LEFT DOWN UP */
                    static const int d8[4] = { 2, 6, 4, 0 }, d4[4] = { 1, 3, 2, 0 };
                    gp_add_ctl(g, CT_HAT, GR_DPAD, fi, 0, n8 ? d8[d] : d4[d], 0, d);
                }
            } else if (g->dpad_kind == 2) {                             /* X and Y */
                for (int a = 0; a < 2; a++) {
                    f.kind = GF_AXIS; f.size = (uint8_t)g->sz; f.count = 1; f.page = 1; f.usage = (uint16_t)(0x30 + a);
                    f.sgn = (uint8_t)g->sgn; f.lmin = g->sgn ? -(1 << (g->sz - 1)) : 0; f.lmax = g->sgn ? (1 << (g->sz - 1)) - 1 : (1 << g->sz) - 1;
                    int fi = gp_add_field(g, &f, &bit);
                    /* X high = right, Y high = down */
                    gp_add_ctl(g, CT_AXIS, GR_DPAD, fi, 0, +1, 0, a == 0 ? 0 : 2);
                    gp_add_ctl(g, CT_AXIS, GR_DPAD, fi, 0, -1, 0, a == 0 ? 1 : 3);
                }
            } else {
                f.kind = (g->dpad_kind == 3) ? GF_BTN : GF_BTNGD; f.size = 1; f.count = 4; f.lmin = 0; f.lmax = 1;
                if (f.kind == GF_BTN) { f.page = 9; f.usage = 1; } else { f.page = 1; f.usage = 0x90; }
                int fi = gp_add_field(g, &f, &bit);
                /* Button page: usages 1..4 = up down left right (arbitrary); GD: 0x90 up 0x91 down 0x92 right 0x93 left */
                static const int el_gd[4] = { 2, 3, 1, 0 }, el_bp[4] = { 3, 2, 1, 0 };   /* element of RIGHT LEFT DOWN UP */
                for (int d = 0; d < 4; d++) gp_add_ctl(g, CT_BTN, GR_DPAD, fi, f.kind == GF_BTNGD ? el_gd[d] : el_bp[d], 0, 0, d);
            }
            break; }
        case B_BTN:
            if (g->nbtn) {
                gp_field_t f; memset(&f, 0, sizeof f);
                f.kind = GF_BTN; f.size = 1; f.count = (uint8_t)g->nbtn; f.lmin = 0; f.lmax = 1; f.page = 9; f.usage = 1; f.form = (uint8_t)(gp_pct(r, 25) ? 1 : 0);
                btn_fld = gp_add_field(g, &f, &bit);
            }
            break;
        case B_STICK0: case B_STICK1: {
            int s = order[oi] == B_STICK0 ? 0 : 1;
            static const int ux[2][2] = { { 0x30, 0x31 }, { 0x32, 0x35 } }, ux_alt[2] = { 0x33, 0x34 };
            gp_field_t f; memset(&f, 0, sizeof f);
            int pair = gp_pct(r, 25);                                   /* both axes in one Input item */
            for (int a = 0; a < 2; a++) {
                int us = (g->dpad_kind == 2 && s == 0) ? (a ? 0x35 : 0x32) : ux[s][a];
                if (g->dpad_kind == 2 && s == 1) us = ux_alt[a];
                f.kind = GF_AXIS; f.size = (uint8_t)g->sz; f.count = 1; f.page = 1; f.usage = (uint16_t)us; f.sgn = (uint8_t)g->sgn;
                f.lmin = g->sgn ? -(1 << (g->sz - 1)) : 0; f.lmax = g->sgn ? (1 << (g->sz - 1)) - 1 : (1 << g->sz) - 1; f.form = (uint8_t)(pair ? 1 : 0);
                int fi = gp_add_field(g, &f, &bit);
                gp_add_ctl(g, CT_AXIS, GR_STICK, fi, 0, +1, 0, (uint8_t)(s * 4 + (a == 0 ? 0 : 2)));      /* X high = right, Y high = down */
                gp_add_ctl(g, CT_AXIS, GR_STICK, fi, 0, -1, 0, (uint8_t)(s * 4 + (a == 0 ? 1 : 3)));
            }
            break; }
        case B_TRIG:
            for (int t = 0; t < g->ntrig; t++) {
                gp_field_t f; memset(&f, 0, sizeof f);
                f.kind = GF_AXIS; f.size = (uint8_t)(gp_pct(r, 70) ? 8 : g->sz); f.count = 1; f.sgn = 0;
                if (gp_pct(r, 25)) { f.page = 2; f.usage = (uint16_t)(t ? 0xC5 : 0xC4); } else { f.page = 1; f.usage = (uint16_t)(t ? 0x34 : 0x33); }
                if (g->dpad_kind != 2 && g->nstick < 2 && f.page == 1) f.usage = (uint16_t)(t ? 0x35 : 0x32);
                f.lmin = 0; f.lmax = (1 << f.size) - 1;
                int fi = gp_add_field(g, &f, &bit);
                gp_add_ctl(g, CT_AXIS, GR_TRIG, fi, 0, +1, 0, (uint8_t)t);
            }
            break;
        case B_VEND: { gp_field_t f; memset(&f, 0, sizeof f); f.kind = GF_VEND; f.size = 8; f.count = (uint8_t)(1 + gp_rn(r, 3)); f.page = 0xFF00; f.usage = 1; f.vconst = (uint8_t)gp_pct(r, 50);
                       { int w = gp_rn(r, 100); f.form = (uint8_t)(w < 55 ? 0 : w < 90 ? 1 : 2); } gp_add_field(g, &f, &bit); break; }
        case B_CONSTB: { gp_field_t f; memset(&f, 0, sizeof f); f.kind = GF_CONST; f.size = 8; f.count = 1; gp_add_field(g, &f, &bit); break; }
        case B_PADBITS: { gp_field_t f; memset(&f, 0, sizeof f); f.kind = GF_PAD; f.size = 1; f.count = (uint8_t)(1 + gp_rn(r, 7)); gp_add_field(g, &f, &bit); break; }
        }
    }
    /* the buttons' controls: which ones are declared but never sent */
    if (btn_fld >= 0) {
        int absent_tail = gp_pct(r, 25) ? 1 + gp_rn(r, 4) : 0;
        for (int i = 0; i < g->nbtn; i++) {
            int ab = (i >= g->nbtn - absent_tail) || gp_pct(r, 8);
            if (!ab) nreal_btn++;
            gp_add_ctl(g, CT_BTN, GR_BTN, btn_fld, i, 0, ab, (uint8_t)i);
        }
    }
    g->nreal = nreal_btn;
    /* byte padding to a total report length */
    { int used = (bit + 7) / 8; int want = used;
      if (gp_pct(r, 30)) want = used + gp_rn(r, 12);
      if (gp_pct(r, 10)) want = 20 + gp_rn(r, 45);
      if (g->ls && want > 8) want = used > 8 ? used : 8;                 /* a low-speed endpoint carries at most 8 bytes */
      if (want > 64) want = 64;
      if (want < used) want = used;
      if (want > used) { gp_field_t f; memset(&f, 0, sizeof f); f.kind = GF_VEND; f.size = 8; f.count = (uint8_t)(want - used); f.page = 0xFF00; f.usage = 2; f.vconst = (uint8_t)gp_pct(r, 50); { int w = gp_rn(r, 100); f.form = (uint8_t)(w < 55 ? 0 : w < 90 ? 1 : 2); } gp_add_field(g, &f, &bit); }
      g->rlen = (bit + 7) / 8; }
    if (g->rlen < 1) g->rlen = 1;
    if (g->ls && g->rlen > 8) { /* too long for a low-speed endpoint: make it full speed instead (an honest USB constraint, not a decode limit) */ g->ls = 0; g->mps0 = 64; if (g->bcd_usb < 0x0200 && gp_pct(r, 50)) g->bcd_usb = 0x0200; }
    { int need = g->rlen; int m = 8; while (m < need) m *= 2; g->ep_mps = g->ls ? 8 : (m > 64 ? 64 : m); if (!g->ls && gp_pct(r, 25) && g->ep_mps < 64) g->ep_mps *= 2; if (g->ls) g->ep_mps = 8; }
    /* a counter that moves once per report is only a counter the 600 ms rest window can see all of for a pad that reports every 10 ms or faster; a slower pad
     * (bInterval 16..255) gets sensor-style noise instead (a documented limit) */
    if (g->ep_iv > 10) for (int fi = 0; fi < g->nf; fi++) if (g->f[fi].kind == GF_VEND && !g->f[fi].vconst && g->f[fi].form == 0) g->f[fi].form = 1;
    /* the highest byte a real control touches (the decode keeps 16 bytes) */
    g->max_ctl_byte = 0;
    for (int i = 0; i < g->nc; i++) {
        if (g->c[i].absent) continue;
        const gp_field_t *f = &g->f[g->c[i].fld];
        int b0 = f->bit + (f->kind == GF_BTN || f->kind == GF_BTNGD ? g->c[i].elem : 0) * f->size, b1 = b0 + f->size - 1;
        if (b1 / 8 > g->max_ctl_byte) g->max_ctl_byte = b1 / 8;
    }
    g->beyond16 = g->max_ctl_byte >= 16;

    /* ---- the report descriptor ---- */
    uint8_t *o = g->d; int n = 0;
    n = gp_it0(o, n, T_UPAGE, 1); n = gp_it0(o, n, T_USAGE, g->app); n = gp_it0(o, n, T_COLL, 1);
    if (g->rid) n = gp_it0(o, n, T_RID, g->rid);
    if (g->wrap == 1) n = gp_it0(o, n, T_COLL, 0);
    if (g->wrap == 2) n = gp_it0(o, n, T_COLL, 2);
    for (int fi = 0; fi < g->nf; fi++) {
        const gp_field_t *f = &g->f[fi];
        switch (f->kind) {
        case GF_BTN: case GF_BTNGD:
            n = gp_it0(o, n, T_UPAGE, f->page);
            if (f->kind == GF_BTNGD) { for (int i = 0; i < f->count; i++) n = gp_it0(o, n, T_USAGE, 0x90 + i); }
            else if (f->form == 0) { n = gp_it0(o, n, T_UMIN, 1); n = gp_it0(o, n, T_UMAX, f->count); }
            else for (int i = 0; i < f->count; i++) n = gp_it0(o, n, T_USAGE, 1 + i);
            n = gp_it(o, n, T_LMIN, 0); n = gp_it(o, n, T_LMAX, 1); n = gp_it0(o, n, T_RSIZE, 1); n = gp_it0(o, n, T_RCNT, f->count); n = gp_it0(o, n, T_INPUT, 2);
            break;
        case GF_HAT:
            n = gp_it0(o, n, T_UPAGE, 1); n = gp_it0(o, n, T_USAGE, 0x39);
            n = gp_it(o, n, T_LMIN, f->lmin); n = gp_it(o, n, T_LMAX, f->lmax);
            if (gp_rn(&R, 2)) { n = gp_it(o, n, T_PMIN, 0); n = gp_it(o, n, T_PMAX, f->lmax - f->lmin == 7 ? 315 : 270); n = gp_it0(o, n, T_UNIT, 0x14); }
            n = gp_it0(o, n, T_RSIZE, f->size); n = gp_it0(o, n, T_RCNT, 1); n = gp_it0(o, n, T_INPUT, 0x42);
            break;
        case GF_AXIS: {
            if (f->form == 1 && fi + 1 < g->nf && g->f[fi + 1].kind == GF_AXIS && g->f[fi + 1].form == 1) {
                /* the first of a pair: both axes in one item (the second field is emitted with this one) */
                const gp_field_t *f2 = &g->f[fi + 1];
                n = gp_it0(o, n, T_UPAGE, f->page); n = gp_it0(o, n, T_USAGE, f->usage); n = gp_it0(o, n, T_USAGE, f2->usage);
                n = gp_it(o, n, T_LMIN, f->lmin); n = gp_it(o, n, T_LMAX, f->lmax); n = gp_it0(o, n, T_RSIZE, f->size); n = gp_it0(o, n, T_RCNT, 2); n = gp_it0(o, n, T_INPUT, 2);
                fi++;                                                   /* consumed */
            } else {
                n = gp_it0(o, n, T_UPAGE, f->page); n = gp_it0(o, n, T_USAGE, f->usage);
                n = gp_it(o, n, T_LMIN, f->lmin); n = gp_it(o, n, T_LMAX, f->lmax); n = gp_it0(o, n, T_RSIZE, f->size); n = gp_it0(o, n, T_RCNT, 1); n = gp_it0(o, n, T_INPUT, 2);
            }
            break; }
        case GF_PAD: n = gp_it0(o, n, T_RSIZE, 1); n = gp_it0(o, n, T_RCNT, f->count); n = gp_it0(o, n, T_INPUT, 3); break;
        case GF_CONST: n = gp_it0(o, n, T_RSIZE, 8); n = gp_it0(o, n, T_RCNT, 1); n = gp_it0(o, n, T_INPUT, 1); break;
        case GF_VEND:
            n = gp_itu(o, n, T_UPAGE, f->page); n = gp_it0(o, n, T_USAGE, f->usage); n = gp_it(o, n, T_LMIN, 0); n = gp_it(o, n, T_LMAX, 255);
            n = gp_it0(o, n, T_RSIZE, 8); n = gp_it0(o, n, T_RCNT, f->count); n = gp_it0(o, n, T_INPUT, 2);
            break;
        }
    }
    if (g->wrap) o[n++] = 0xC0;
    o[n++] = 0xC0; g->dn = n;

    /* ---- the idle report ---- */
    memset(g->idle, 0, sizeof g->idle);
    if (g->rid) g->idle[0] = (uint8_t)g->rid;
    for (int fi = 0; fi < g->nf; fi++) {
        const gp_field_t *f = &g->f[fi];
        if (f->kind == GF_HAT) gp_setbits(g->idle, f->bit, f->size, (uint32_t)f->nullv);
        else if (f->kind == GF_AXIS) {
            int tr = 0; for (int i = 0; i < g->nc; i++) if (g->c[i].fld == fi && g->c[i].role == GR_TRIG) tr = 1;
            uint32_t v = tr ? 0u : (f->sgn ? 0u : (uint32_t)(1u << (f->size - 1)) - 1u + (uint32_t)(gp_rn(&R, 2)));   /* centred: 0 / just under or at the middle */
            gp_setbits(g->idle, f->bit, f->size, v);
        } else if (f->kind == GF_VEND && f->vconst) { for (int i = 0; i < f->count; i++) gp_setbits(g->idle, f->bit + 8 * i, 8, (uint32_t)(0x11 * (i + 1) + fi)); }
        else if (f->kind == GF_CONST) gp_setbits(g->idle, f->bit, 8, (uint32_t)(0x7F - fi));
    }

    /* ---- USB descriptors ---- */
    uint8_t *dd = g->dd;
    { uint8_t t[18] = { 18, 1, (uint8_t)(g->bcd_usb & 0xFF), (uint8_t)(g->bcd_usb >> 8), 0, 0, 0, (uint8_t)g->mps0, (uint8_t)(g->vid & 0xFF), (uint8_t)(g->vid >> 8),
                        (uint8_t)(g->pid & 0xFF), (uint8_t)(g->pid >> 8), (uint8_t)(g->ver & 0xFF), (uint8_t)(g->ver >> 8), 1, 2, 0, 1 };
      memcpy(dd, t, 18); }
    { uint8_t *c = g->cd; int k = 0; int nif = g->n_pre + 1;
      uint8_t h[9] = { 9, 2, 0, 0, (uint8_t)nif, 1, 0, 0x80, 0xFA }; memcpy(c, h, 9); k = 9;
      int ifn = 0;
      for (int i = 0; i < g->n_pre; i++, ifn++) {
          int pk = g->pre_kind[i];
          if (pk == 0) { uint8_t t[9] = { 9, 4, (uint8_t)ifn, 0, 0, 1, 1, 0, 0 }; memcpy(c + k, t, 9); k += 9; }
          else if (pk == 1) {
              uint8_t t[9] = { 9, 4, (uint8_t)ifn, 0, 1, 3, 1, 1, 0 }; memcpy(c + k, t, 9); k += 9;
              uint8_t hd[9] = { 9, 0x21, 0x11, 1, 0, 1, 0x22, 63, 0 }; memcpy(c + k, hd, 9); k += 9;
              uint8_t e[7] = { 7, 5, (uint8_t)(0x80 | (1 + (g->ep_addr % 15))), 3, 8, 0, 10 }; memcpy(c + k, e, 7); k += 7;
          } else { uint8_t t[9] = { 9, 4, (uint8_t)ifn, 0, 0, 0xFF, 0, 0, 0 }; memcpy(c + k, t, 9); k += 9; }
      }
      g->hid_if = ifn;
      { uint8_t t[9] = { 9, 4, (uint8_t)ifn, 0, (uint8_t)(1 + g->has_out), 3, 0, 0, 0 }; memcpy(c + k, t, 9); k += 9; }
      { uint8_t hd[9] = { 9, 0x21, 0x11, 1, 0, 1, 0x22, (uint8_t)(g->dn & 0xFF), (uint8_t)(g->dn >> 8) }; memcpy(c + k, hd, 9); k += 9; }
      { uint8_t e[7] = { 7, 5, (uint8_t)(0x80 | g->ep_addr), 3, (uint8_t)(g->ep_mps & 0xFF), (uint8_t)(g->ep_mps >> 8), (uint8_t)g->ep_iv }; memcpy(c + k, e, 7); k += 7; }
      if (g->has_out) { uint8_t e[7] = { 7, 5, (uint8_t)g->ep_addr, 3, (uint8_t)(g->ep_mps & 0xFF), (uint8_t)(g->ep_mps >> 8), (uint8_t)g->ep_iv }; memcpy(c + k, e, 7); k += 7; }
      c[2] = (uint8_t)(k & 0xFF); c[3] = (uint8_t)(k >> 8); g->cdn = k; }
}

/* ------------------------------------------------------------------------------------------------------------------------------------------ */
/* reports: the idle report with a set of controls held (pct = 100 fully, less = partially; analog only), plus noise                               */
typedef struct { int c; int pct; } gp_press_t;

static inline int gp_report(const gpad_t *g, const gp_press_t *pr, int np, gp_rng_t *noise, uint8_t *out)
{
    memcpy(out, g->idle, 64);
    if (noise) {
        noise->cnt++;
        for (int fi = 0; fi < g->nf; fi++) {
            const gp_field_t *f = &g->f[fi];
            if (f->kind == GF_VEND && !f->vconst) for (int i = 0; i < f->count; i++) {
                uint32_t v = f->form == 0 ? (noise->cnt * (2u * i + 1u) + (uint32_t)fi * 17u) & 0xFF : f->form == 1 ? (0x40u + (gp_rnd(noise) & 7u)) : (gp_rnd(noise) & 0xFFu);
                gp_setbits(out, f->bit + 8 * i, 8, v);
            }
            if (f->kind == GF_AXIS && g->wobble) {
                int tr = 0; for (int i = 0; i < g->nc; i++) if (g->c[i].fld == fi && g->c[i].role == GR_TRIG) tr = 1;
                if (tr) continue;
                int base = (int)((f->lmin + f->lmax) / 2);
                int span = f->lmax > f->lmin ? f->lmax - f->lmin : (1 << f->size);
                int w = g->wobble * (span >= 1024 ? span / 256 : 1);                  /* the wobble scales with the logical range (about 1 % of it) */
                if (w > span / 16) w = span / 16;                                     /* ... and stays well inside what the learner calls a press (an eighth of the range) */
                int v = base + gp_rn(noise, 2 * w + 1) - w;
                if (v < f->lmin) v = f->lmin;
                if (v > f->lmax) v = f->lmax;
                gp_setbits(out, f->bit, f->size, (uint32_t)v & ((1u << f->size) - 1u));
            }
        }
    }
    /* hat direction accumulation */
    for (int fi = 0; fi < g->nf; fi++) {
        const gp_field_t *f = &g->f[fi];
        if (f->kind != GF_HAT) continue;
        int up = 0, rt = 0, dn = 0, lf = 0;
        for (int p = 0; p < np; p++) {
            const gp_ctl_t *c = &g->c[pr[p].c]; if (c->fld != fi || c->kind != CT_HAT || pr[p].pct < 50) continue;
            int d8 = (g->dpad_kind == 0) ? c->dir : c->dir * 2;
            if (d8 == 0) up = 1; else if (d8 == 2) rt = 1; else if (d8 == 4) dn = 1; else if (d8 == 6) lf = 1;
        }
        if (up && dn) { up = dn = 0; } if (lf && rt) { lf = rt = 0; }
        if (!(up | rt | dn | lf)) continue;
        int d8 = up ? (rt ? 1 : lf ? 7 : 0) : dn ? (rt ? 3 : lf ? 5 : 4) : (rt ? 2 : 6);
        int v = (g->dpad_kind == 0) ? d8 : (up ? 0 : rt ? 1 : dn ? 2 : 3);       /* a 4-way hat has no diagonals: the vertical wins */
        gp_setbits(out, f->bit, f->size, (uint32_t)(v + f->lmin));
    }
    for (int p = 0; p < np; p++) {
        const gp_ctl_t *c = &g->c[pr[p].c]; const gp_field_t *f = &g->f[c->fld];
        if (c->kind == CT_BTN) {
            if (c->absent) continue;
            int b = f->bit + c->elem; out[b >> 3] |= (uint8_t)(1u << (b & 7));
        } else if (c->kind == CT_AXIS) {
            uint32_t mask = (f->size >= 32) ? 0xFFFFFFFFu : ((1u << f->size) - 1u);
            int span_hi = (int)(f->lmax), span_lo = (int)(f->lmin);
            int base = (c->role == GR_TRIG) ? f->lmin : (int)((f->lmin + f->lmax) / 2);
            int tgt = c->dir > 0 ? span_hi : span_lo;
            if (c->role == GR_TRIG) tgt = span_hi;
            int v = base + (int)(((int64_t)(tgt - base) * pr[p].pct) / 100);
            gp_setbits(out, f->bit, f->size, (uint32_t)v & mask);
        }
    }
    return g->rlen;
}


/* ------------------------------------------------------------------------------------------------------------------------------------------ */
/* A REAL descriptor adopted as the pad's HID side: the fields come from the parsed layout, the controls are assigned by position (a hat or the first two axes is the
 * d-pad, the buttons are buttons, the remaining axes are sticks in pairs and triggers), the USB side is drawn from the seed as for a generated pad.  The ground truth
 * of what the "hardware" does is therefore this adapter's, not the learner's or the table builder's.  Needs tango_hidparse.h (hp_layout_t). */
#ifdef TANGO_HIDPARSE_H
static inline int gp_adopt(gpad_t *g, const uint8_t *d, int dn, const hp_layout_t *L, uint32_t seed)
{
    gp_make(g, seed, 0);
    if (dn > GP_MAXD || L->nf > GP_MAXF) return 0;
    g->nf = 0; g->nc = 0; g->nbtn = g->nreal = g->nstick = g->ntrig = 0; g->max_ctl_byte = 0; g->beyond16 = 0; g->second_rid = 0; g->wobble = (int)(seed % 3);
    memcpy(g->d, d, (size_t)dn); g->dn = dn; g->rid = L->rid; g->rlen = L->bytes > 64 ? 64 : (L->bytes < 1 ? 1 : L->bytes);
    if (g->rlen > g->ep_mps) g->ep_mps = g->rlen > 32 ? 64 : 32;
    memset(g->idle, 0, sizeof g->idle); if (g->rid) g->idle[0] = (uint8_t)g->rid;
    int fi_of[HP_MAX_FIELDS];
    for (int q = 0; q < L->nf; q++) {
        const hp_field_t *h = &L->f[q]; gp_field_t f; memset(&f, 0, sizeof f);
        f.kind = h->kind == HPK_BUTTON ? GF_BTN : h->kind == HPK_HAT ? GF_HAT : GF_AXIS;
        f.bit = h->bit; f.size = h->size; f.count = 1; f.lmin = h->lmin; f.lmax = h->lmax; f.sgn = h->sgn; f.page = h->page; f.usage = h->usage;
        if (f.kind == GF_HAT) { int top = (1 << f.size) - 1; f.nullv = h->lmin > 0 ? 0 : (h->lmax + 1 <= top ? h->lmax + 1 : top); }
        fi_of[q] = g->nf; g->f[g->nf++] = f;
    }
    /* the idle report: hats at their null value, centred axes (axes become triggers below and then rest low) */
    int hat = -1, nax = 0, axq[HP_MAX_FIELDS], nb = 0, bq[HP_MAX_FIELDS];
    for (int q = 0; q < L->nf; q++) { if (L->f[q].kind == HPK_HAT && hat < 0) hat = q; else if (L->f[q].kind == HPK_AXIS) axq[nax++] = q; else if (L->f[q].kind == HPK_BUTTON) bq[nb++] = q; }
    int ax_used = 0, slot = 0;
    if (hat >= 0) {
        const hp_field_t *h = &L->f[hat]; int n8 = (h->lmax - h->lmin) >= 7; g->dpad_kind = n8 ? 0 : 1;
        static const int d8[4] = { 2, 6, 4, 0 }, d4[4] = { 1, 3, 2, 0 };
        for (slot = 0; slot < 4; slot++) gp_add_ctl(g, CT_HAT, GR_DPAD, fi_of[hat], 0, n8 ? d8[slot] : d4[slot], 0, slot);
    } else if (nax >= 2) {
        g->dpad_kind = 2; ax_used = 2;
        for (int a = 0; a < 2; a++) { gp_add_ctl(g, CT_AXIS, GR_DPAD, fi_of[axq[a]], 0, +1, 0, a == 0 ? 0 : 2); gp_add_ctl(g, CT_AXIS, GR_DPAD, fi_of[axq[a]], 0, -1, 0, a == 0 ? 1 : 3); }
    } else if (nb >= 4) {
        g->dpad_kind = 3;                                              /* the first four buttons are the d-pad */
        for (slot = 0; slot < 4; slot++) gp_add_ctl(g, CT_BTN, GR_DPAD, fi_of[bq[slot]], 0, 0, 0, slot);
        nb -= 4; for (int i = 0; i < nb; i++) bq[i] = bq[i + 4];
    } else return 0;
    for (int i = 0; i < nb; i++) { gp_add_ctl(g, CT_BTN, GR_BTN, fi_of[bq[i]], 0, 0, 0, (uint8_t)i); g->nbtn++; g->nreal++; }
    int left = nax - ax_used, k = ax_used, nst = 0, ntr = 0;
    while (left >= 2 && nst < 2) {
        for (int a = 0; a < 2; a++) { gp_add_ctl(g, CT_AXIS, GR_STICK, fi_of[axq[k + a]], 0, +1, 0, (uint8_t)(nst * 4 + (a == 0 ? 0 : 2))); gp_add_ctl(g, CT_AXIS, GR_STICK, fi_of[axq[k + a]], 0, -1, 0, (uint8_t)(nst * 4 + (a == 0 ? 1 : 3))); }
        k += 2; left -= 2; nst++;
    }
    while (left >= 1 && ntr < 2) { if (!L->f[axq[k]].sgn) { gp_add_ctl(g, CT_AXIS, GR_TRIG, fi_of[axq[k]], 0, +1, 0, (uint8_t)ntr); ntr++; } k++; left--; }
    g->nstick = nst; g->ntrig = ntr;
    /* idle values */
    for (int fi = 0; fi < g->nf; fi++) {
        const gp_field_t *f = &g->f[fi];
        if (f->kind == GF_HAT) gp_setbits(g->idle, f->bit, f->size, (uint32_t)f->nullv);
        else if (f->kind == GF_AXIS) {
            int tr = 0; for (int i = 0; i < g->nc; i++) if (g->c[i].fld == fi && g->c[i].role == GR_TRIG) tr = 1;
            int cen = (f->lmin + f->lmax) / 2 + (int)(seed & 1 && f->lmax > f->lmin ? 1 : 0);
            gp_setbits(g->idle, f->bit, f->size, tr ? (uint32_t)f->lmin : (uint32_t)cen & ((1u << f->size) - 1u));
        }
    }
    for (int i = 0; i < g->nc; i++) if (!g->c[i].absent) { const gp_field_t *f = &g->f[g->c[i].fld]; int b1 = f->bit + f->size - 1; if (b1 / 8 > g->max_ctl_byte) g->max_ctl_byte = b1 / 8; }
    g->beyond16 = g->max_ctl_byte >= 16;
    return 1;
}
#endif

/* a control the decode can reach: its last bit lies in the first PD_RPT_BYTES (16) bytes of the payload */
static inline int gp_reachable(const gpad_t *g, int i)
{
    const gp_ctl_t *c = &g->c[i]; const gp_field_t *f = &g->f[c->fld];
    int b1 = f->bit + (c->kind == CT_BTN ? c->elem : 0) * f->size + f->size - 1;
    return b1 / 8 < 16;
}

#endif
