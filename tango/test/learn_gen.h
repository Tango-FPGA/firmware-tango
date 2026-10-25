// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 the Tango authors
/* learn_gen.h -- idle / pressed report generators shared by test_padlearn.c and test_plsess.c: from a reference decode table T they build the report a pad would send at rest and with one input held. */
#ifndef LEARN_GEN_H
#define LEARN_GEN_H
#include <string.h>
#include "../tango_paddec.h"
#include "../tango_padlearn.h"

static inline void setf(uint8_t *r, int bit, int w, uint32_t v)
{
    for (int i = 0; i < w; i++) { int b = bit + i; if ((v >> i) & 1) r[b >> 3] |= (uint8_t)(1 << (b & 7)); else r[b >> 3] &= (uint8_t)~(1 << (b & 7)); }
}

typedef struct { uint8_t r[PD_RPT_BYTES]; int len; } rep_t;

/* make entry e's condition false (idle) or true (pressed) in the report */
static inline void put_idle(const pd_ent_t *e, rep_t *r)
{
    int bit = e->byte * 8 + e->shift, w = e->widthm1 + 1;
    uint32_t mid = 1u << (w - 1);
    switch (e->cmp) {
    case PDC_NZ: setf(r->r, bit, w, 0); break;
    case PDC_Z:  setf(r->r, bit, w, 1); break;
    case PDC_GE: case PDC_LE:
        if (e->out == RW_L2 || e->out == RW_R2) setf(r->r, bit, w, 0);
        else setf(r->r, bit, w, e->bias ? 0 : mid);       /* rests at the centre: signed 0, or offset binary mid */
        break;
    case PDC_HAT: setf(r->r, bit, w, (e->thr >> 8) ? 0 : 15); break;       /* the null state */
    default: break;
    }
}

static inline void put_pressed(const pd_ent_t *e, rep_t *r)
{
    int bit = e->byte * 8 + e->shift, w = e->widthm1 + 1;
    uint32_t mid = 1u << (w - 1), max = (1u << w) - 1;
    switch (e->cmp) {
    case PDC_NZ: setf(r->r, bit, w, 1); break;
    case PDC_Z:  setf(r->r, bit, w, 0); break;
    case PDC_GE: setf(r->r, bit, w, e->bias ? (max ^ mid) : max); break;
    case PDC_LE: setf(r->r, bit, w, e->bias ? mid : 0); break;
    case PDC_HAT: {
        int base = e->thr >> 8, steps8 = (e->thr & 0xFF) > 0x0F || (e->thr & 0x83) == 0x83 || (e->thr & 0xE0) == 0xE0 || (e->thr & 0x38) == 0x38 || (e->thr & 0x0E) == 0x0E;
        int pure = -1;
        switch (e->out) { case RW_UP: pure = 0; break; case RW_RIGHT: pure = steps8 ? 2 : 1; break; case RW_DOWN: pure = steps8 ? 4 : 2; break; default: pure = steps8 ? 6 : 3; }
        setf(r->r, bit, w, (uint32_t)(base + pure));
        break;
    }
    default: break;
    }
}

static inline void build_idle(const pd_table_t *T, int rid, int len, rep_t *r)
{
    memset(r, 0, sizeof *r); r->len = len;
    if (rid) r->r[0] = (uint8_t)rid;
    for (int i = 0; i < T->count; i++) put_idle(&T->e[i], r);
}

static inline void press_out(const pd_table_t *T, int out, rep_t *r)
{
    for (int i = 0; i < T->count; i++) if (T->e[i].out == out) put_pressed(&T->e[i], r);
}

static inline pl_sample_t to_sample(const rep_t *r) { pl_sample_t s; memset(&s, 0, sizeof s); memcpy(s.b, r->r, PD_RPT_BYTES); s.len = (uint8_t)r->len; return s; }

static inline int has_out(const pd_table_t *t, int out) { for (int i = 0; i < t->count; i++) if (t->e[i].out == out) return 1; return 0; }

static inline uint32_t eval_masked(const pd_table_t *t, const rep_t *r, int *acc)
{
    uint32_t raw = 0;
    *acc = pd_eval(t, r->r, r->len, &raw);
    return raw;
}

/* noise: a counter byte that is not part of any entry, and the low bit of every axis field flickering */
static inline void add_noise(const pd_table_t *T, rep_t *r, int k)
{
    int used[PD_RPT_BYTES] = { 0 };
    for (int i = 0; i < T->count; i++) { int b0 = T->e[i].byte, b1 = b0 + (T->e[i].shift + T->e[i].widthm1) / 8; for (int b = b0; b <= b1 && b < PD_RPT_BYTES; b++) used[b] = 1; }
    for (int b = PD_RPT_BYTES - 1; b > 0; b--) if (!used[b] && b < r->len) { r->r[b] = (uint8_t)(k * 37 + 11); break; }
    for (int i = 0; i < T->count; i++) {
        const pd_ent_t *e = &T->e[i];
        if ((e->cmp == PDC_GE || e->cmp == PDC_LE) && e->widthm1 >= 7 && (k & 1)) r->r[e->byte] ^= (uint8_t)(1u << e->shift);
    }
}


#endif
