// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 the Tango authors
/*
 * tango_hidparse.c -- see tango_hidparse.h. Tango, our own code, from the public USB HID 1.11 specification.
 */
#include <string.h>
#include <stdlib.h>
#include "tango_hidparse.h"

/* the parser state is ~5.6 KB: allocated for one hp_parse call and freed (a static of that size costs the firmware 5.6 KB of OCRAM heap) */
#ifndef HP_MALLOC
#define HP_MALLOC malloc
#define HP_FREE free
#endif

/* evdev constants (linux/input-event-codes.h, public) */
#define K_BTN_MISC      0x100
#define K_BTN_JOYSTICK  0x120
#define K_BTN_GAMEPAD   0x130
#define K_BTN_SELECT    0x13a
#define K_BTN_START     0x13b
#define K_BTN_DPAD_UP   0x220
#define K_BTN_TRIG_HAPPY 0x2c0

#define TMP_FIELDS 192
#define MAX_USAGES 64
#define MAX_APPS   8

int hp_button_code(int app_usage, int page, int usage)
{
    if (page == 9) {                                   /* Button page: hid-input.c, HID_UP_BUTTON */
        int c = usage - 1;
        if (c < 0) return -1;
        if (app_usage == 4) return c <= 15 ? K_BTN_JOYSTICK + c : K_BTN_TRIG_HAPPY + c - 16;
        if (app_usage == 5) return c <= 15 ? K_BTN_GAMEPAD + c : K_BTN_TRIG_HAPPY + c - 16;
        return K_BTN_MISC + c;
    }
    if (page == 1) {                                   /* Generic Desktop buttons */
        switch (usage) {
        case 0x3D: return K_BTN_START;
        case 0x3E: return K_BTN_SELECT;
        case 0x90: return K_BTN_DPAD_UP;               /* D-pad Up, Down, Right, Left -> BTN_DPAD_UP, DOWN, RIGHT, LEFT */
        case 0x91: return K_BTN_DPAD_UP + 1;
        case 0x92: return K_BTN_DPAD_UP + 3;
        case 0x93: return K_BTN_DPAD_UP + 2;
        default: break;
        }
    }
    return -1;
}

int hp_axis_code(int page, int usage)
{
    if (page == 1 && usage >= 0x30 && usage <= 0x38) return usage & 0xF;     /* X Y Z Rx Ry Rz, slider (throttle), dial (rudder), wheel */
    if (page == 2) {                                                          /* Simulation controls */
        switch (usage) {
        case 0xBB: return 6;     /* throttle  -> ABS_THROTTLE */
        case 0xBA: return 7;     /* rudder    -> ABS_RUDDER   */
        case 0xC4: return 9;     /* accelerator -> ABS_GAS    */
        case 0xC5: return 10;    /* brake     -> ABS_BRAKE    */
        default: break;
        }
    }
    return -1;
}

typedef struct { uint16_t page; int32_t lmin, lmax; uint32_t size, count; uint8_t rid; } glob_t;

typedef struct {
    int      app;                       /* application ordinal (0..), -1 outside one */
    uint8_t  nf_over;
    hp_field_t f[TMP_FIELDS];
    int      nf;
    int      app_page[MAX_APPS], app_usage[MAX_APPS], napp;
    int      app_of[TMP_FIELDS];
    uint16_t app_rids[MAX_APPS];       /* bit per rid slot that has non-constant Input items in the application */
    int      rid_slot[HP_MAX_RIDS]; uint8_t rid_of[HP_MAX_RIDS]; uint32_t rid_bits[HP_MAX_RIDS]; int nrid;
} pstate_t;

static int rid_slot(pstate_t *s, uint8_t rid)
{
    for (int i = 0; i < s->nrid; i++) if (s->rid_of[i] == rid) return i;
    if (s->nrid >= HP_MAX_RIDS) return -1;
    s->rid_of[s->nrid] = rid;
    s->rid_bits[s->nrid] = rid ? 8 : 0;            /* a report with an id has the id as byte 0 */
    return s->nrid++;
}

static int32_t sext(uint32_t v, int bytes)
{
    if (bytes == 1) return (int32_t)(int8_t)v;
    if (bytes == 2) return (int32_t)(int16_t)v;
    return (int32_t)v;
}

static void add_field(pstate_t *s, const glob_t *g, uint32_t slot, uint32_t i, uint16_t page, uint16_t usage, int app)
{
    int kind = HPK_NONE;
    if (page == 9) kind = HPK_BUTTON;
    else if (page == 1 && usage == 0x39) kind = HPK_HAT;
    else if (page == 1 && ((usage >= 0x3D && usage <= 0x3E) || (usage >= 0x90 && usage <= 0x93))) kind = HPK_BUTTON;
    else if (hp_axis_code(page, usage) >= 0) kind = HPK_AXIS;
    if (kind == HPK_NONE || s->nf >= TMP_FIELDS) return;
    hp_field_t *f = &s->f[s->nf];
    memset(f, 0, sizeof *f);
    f->kind = (uint8_t)kind; f->rid = g->rid; f->size = (uint8_t)g->size; f->page = page; f->usage = usage;
    f->bit = (uint16_t)(s->rid_bits[slot] + i * g->size);
    f->lmin = g->lmin; f->lmax = g->lmax; f->sgn = g->lmin < 0;
    f->code = -1;
    s->app_of[s->nf++] = app;
}

static int better_rid(const pstate_t *s, int app, uint8_t rid, int *score)
{
    int sc = 0;
    for (int i = 0; i < s->nf; i++) if (s->app_of[i] == app && s->f[i].rid == rid) sc++;
    *score = sc;
    return sc;
}

static int parse_impl(const uint8_t *d, int n, hp_layout_t *out, pstate_t *s)
{
    memset(s, 0, sizeof *s);
    memset(out, 0, sizeof *out);
    memset(out->bidx, -1, sizeof out->bidx); memset(out->aidx, -1, sizeof out->aidx); memset(out->hidx, -1, sizeof out->hidx);
    s->app = -1;

    glob_t g = { 0, 0, 0, 0, 0, 0 };
    glob_t stack[4]; int sp = 0;
    uint32_t ulist[MAX_USAGES]; int nu = 0; int have_min = 0, have_max = 0; uint32_t umin = 0, umax = 0;
    int depth = 0;

    int i = 0;
    while (i < n) {
        uint8_t b = d[i++];
        if (b == 0xFE) {                                    /* long item: skip */
            if (i >= n) break;
            int len = d[i];
            i += 2 + len;
            continue;
        }
        int sz = b & 3; if (sz == 3) sz = 4;
        int type = (b >> 2) & 3, tag = b >> 4;
        if (i + sz > n) break;
        uint32_t v = 0;
        for (int k = 0; k < sz; k++) v |= (uint32_t)d[i + k] << (8 * k);
        i += sz;

        if (type == 1) {                                    /* global */
            switch (tag) {
            case 0: g.page = (uint16_t)v; break;
            case 1: g.lmin = sext(v, sz); break;
            case 2: g.lmax = (g.lmin < 0) ? sext(v, sz) : (int32_t)v; break;   /* hid-core: an unsigned maximum when the minimum is >= 0 */
            case 7: g.size = v; break;
            case 8: g.rid = (uint8_t)v; break;
            case 9: g.count = v; break;
            case 10: if (sp < 4) stack[sp++] = g; break;
            case 11: if (sp > 0) g = stack[--sp]; break;
            default: break;
            }
        } else if (type == 2) {                             /* local */
            switch (tag) {
            case 0: if (nu < MAX_USAGES) { ulist[nu++] = (sz == 4) ? v : (uint32_t)((uint32_t)g.page << 16 | (v & 0xFFFF)); } break;
            case 1: umin = (sz == 4) ? v : (uint32_t)((uint32_t)g.page << 16 | (v & 0xFFFF)); have_min = 1; break;
            case 2: umax = (sz == 4) ? v : (uint32_t)((uint32_t)g.page << 16 | (v & 0xFFFF)); have_max = 1; break;
            default: break;
            }
        } else if (type == 0) {                             /* main */
            if (tag == 10) {                                /* Collection */
                if (depth == 0 && v == 1) {                 /* Application */
                    if (s->napp < MAX_APPS) {
                        s->app = s->napp;
                        s->app_page[s->napp] = nu ? (int)(ulist[0] >> 16) : g.page;
                        s->app_usage[s->napp] = nu ? (int)(ulist[0] & 0xFFFF) : 0;
                        s->napp++;
                    } else s->app = -1;
                }
                depth++;
            } else if (tag == 12) {                         /* End collection */
                if (depth > 0) depth--;
                if (depth == 0) s->app = -1;
            } else if (tag == 8) {                          /* Input */
                int slot = rid_slot(s, g.rid);
                if (slot >= 0) {
                    uint32_t cnt = g.count, sizeb = g.size;
                    int constant = v & 1, variable = (v >> 1) & 1;
                    if (!constant && s->app >= 0) s->app_rids[s->app] |= (uint16_t)(1u << slot);
                    if (!constant && variable && s->app >= 0 && sizeb >= 1 && sizeb <= 32 && cnt < 256) {
                        for (uint32_t k = 0; k < cnt; k++) {
                            uint32_t u;
                            if (have_min && have_max) u = (umin + k <= umax) ? umin + k : umax;
                            else if (have_min) u = umin + k;
                            else if (nu) u = ulist[k < (uint32_t)nu ? k : (uint32_t)nu - 1];
                            else continue;
                            add_field(s, &g, (uint32_t)slot, k, (uint16_t)(u >> 16 ? u >> 16 : g.page), (uint16_t)(u & 0xFFFF), s->app);
                        }
                    }
                    s->rid_bits[slot] += cnt * sizeb;
                }
            }
            /* every main item clears the local state */
            nu = 0; have_min = have_max = 0; umin = umax = 0;
        }
    }

    /* the Application collection that is a pad */
    int app = -1;
    for (int a = 0; a < s->napp; a++) {
        if (s->app_page[a] == 1 && (s->app_usage[a] == 4 || s->app_usage[a] == 5 || s->app_usage[a] == 8)) {
            int nb = 0, nh = 0, na = 0;
            for (int k = 0; k < s->nf; k++) if (s->app_of[k] == a) {
                if (s->f[k].kind == HPK_BUTTON) nb++; else if (s->f[k].kind == HPK_HAT) nh++; else na++;
            }
            if (nb || nh || na >= 2) { app = a; break; }
        }
    }
    if (app < 0) {
        if (s->napp) {
            int p = s->app_page[0], u = s->app_usage[0];
            out->cls = (p == 1 && (u == 2 || u == 1)) ? HPC_MOUSE : (p == 1 && (u == 6 || u == 7)) ? HPC_KEYBOARD : HPC_OTHER;
            out->app_page = p; out->app_usage = u;
        }
        return 0;
    }
    out->cls = HPC_GAMEPAD; out->app_page = s->app_page[app]; out->app_usage = s->app_usage[app];

    /* the report id that carries the most fields (a pad's input report; others are ignored) */
    int best_rid = -1, best_sc = -1;
    for (int r = 0; r < s->nrid; r++) {
        int sc; better_rid(s, app, s->rid_of[r], &sc);
        if (sc > best_sc) { best_sc = sc; best_rid = r; }
    }
    if (best_rid < 0 || best_sc <= 0) return 0;
    int other = 0;
    for (int r = 0; r < s->nrid; r++) if (r != best_rid && ((s->app_rids[app] >> r) & 1)) other++;
    out->rid = s->rid_of[best_rid];
    out->nrid_other = other;
    out->bytes = (uint16_t)((s->rid_bits[best_rid] + 7) / 8);

    for (int k = 0; k < s->nf; k++) {
        if (s->app_of[k] != app || s->f[k].rid != out->rid || out->nf >= HP_MAX_FIELDS) continue;
        hp_field_t f = s->f[k];
        if (f.kind == HPK_BUTTON) f.code = hp_button_code(out->app_usage, f.page, f.usage);
        else if (f.kind == HPK_AXIS) f.code = hp_axis_code(f.page, f.usage);
        if (f.kind == HPK_BUTTON && f.code < 0) continue;
        out->f[out->nf++] = f;
    }

    /* ordinals: buttons by (code >= BTN_JOYSTICK first), axes by ABS code, hats by appearance */
    int8_t bs[HP_MAX_BTN]; int nb = 0;
    for (int k = 0; k < out->nf; k++) if (out->f[k].kind == HPK_BUTTON && nb < HP_MAX_BTN) bs[nb++] = (int8_t)k;
    #define BKEY(k) (out->f[k].code >= K_BTN_JOYSTICK ? out->f[k].code : 0x10000 + out->f[k].code)
    for (int a = 1; a < nb; a++) { int8_t t = bs[a]; int j = a - 1; while (j >= 0 && BKEY(bs[j]) > BKEY(t)) { bs[j + 1] = bs[j]; j--; } bs[j + 1] = t; }
    /* the same code twice (a duplicate usage): the first one only, as the kernel has one event code per key */
    int m = 0;
    for (int a = 0; a < nb; a++) { if (m && out->f[bs[m - 1]].code == out->f[bs[a]].code) continue; bs[m++] = bs[a]; }
    nb = m;
    for (int a = 0; a < nb; a++) out->bidx[a] = bs[a];
    out->nbtn = nb;

    int8_t as[HP_MAX_AXES]; int na = 0;
    for (int k = 0; k < out->nf; k++) if (out->f[k].kind == HPK_AXIS && na < HP_MAX_AXES) as[na++] = (int8_t)k;
    for (int a = 1; a < na; a++) { int8_t t = as[a]; int j = a - 1; while (j >= 0 && out->f[as[j]].code > out->f[t].code) { as[j + 1] = as[j]; j--; } as[j + 1] = t; }
    m = 0;
    for (int a = 0; a < na; a++) { if (m && out->f[as[m - 1]].code == out->f[as[a]].code) continue; as[m++] = as[a]; }
    na = m;
    for (int a = 0; a < na; a++) out->aidx[a] = as[a];
    out->naxis = na;

    int nh = 0;
    for (int k = 0; k < out->nf; k++) if (out->f[k].kind == HPK_HAT && nh < HP_MAX_HATS) out->hidx[nh++] = (int8_t)k;
    out->nhat = nh;
    return 1;
}

int hp_parse(const uint8_t *d, int n, hp_layout_t *out)
{
    pstate_t *s = HP_MALLOC(sizeof *s);
    if (!s) { memset(out, 0, sizeof *out); return 0; }          /* out of memory: reads as "not a pad" (the caller logs and falls back) */
    int r = parse_impl(d, n, out, s);
    HP_FREE(s);
    return r;
}
