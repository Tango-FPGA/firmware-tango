// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 the Tango authors
/*
 * test_padlearn.c -- the learn engine (tango_padlearn.c) against every fixture of pad_fixtures.h.
 * For each fixture pad the reference table T is what the descriptor builder makes of it (pd_from_layout). The test plays the part of the user and of the pad:
 * from T it builds an IDLE report and, for every raw bit T defines, the report with that one input held, feeds those to the learner as samples, and
 * requires the LEARNED table to give T's answer on a set of DISCRETE states: nothing pressed, each input alone, random sets of inputs at once (at most one
 * d-pad direction), and every learned field at its minimum, centre and maximum. The domain is discrete on purpose: the learner's thresholds are half-way,
 * the builder's are quarter points, so the two agree wherever a user can tell a press from rest, and not on arbitrary bytes.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../tango_hidparse.h"
#include "../tango_paddb.h"
#include "../tango_paddec.h"
#include "../tango_padlearn.h"
#include "pad_fixtures.h"
#include "learn_gen.h"

static int fails = 0, checks = 0;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint32_t rng_s = 0xBADC0DEu;
static uint32_t rnd(void) { rng_s ^= rng_s << 13; rng_s ^= rng_s >> 17; rng_s ^= rng_s << 5; return rng_s; }

static int learn_all(pl_t *L, const pd_table_t *T, int rid, int len, int noisy, int *learned_outs, int *nl)
{
    rep_t idle; build_idle(T, rid, len, &idle);
    pl_sample_t base[6];
    for (int k = 0; k < 6; k++) { rep_t r = idle; if (noisy) add_noise(T, &r, k); base[k] = to_sample(&r); }
    pl_begin(L); if (rid) pl_set_report_id(L, rid);
    pl_baseline(L, base, 6);
    int bad = 0; *nl = 0;
    int dp = has_out(T, RW_UP) && has_out(T, RW_RIGHT) && has_out(T, RW_DOWN) && has_out(T, RW_LEFT);
    if (dp) {
        static const int D[4] = { RW_UP, RW_RIGHT, RW_DOWN, RW_LEFT };
        pl_sample_t ps[4][3]; const pl_sample_t *pp[4];
        for (int d = 0; d < 4; d++) {
            for (int k = 0; k < 3; k++) { rep_t r = idle; press_out(T, D[d], &r); if (noisy) add_noise(T, &r, k + 7); ps[d][k] = to_sample(&r); }
            pp[d] = ps[d];
        }
        int rc = pl_learn_dpad(L, pp, 3);
        if (rc != PL_OK) { bad++; printf("   d-pad: learn rc %d\n", rc); }
        for (int d = 0; d < 4; d++) learned_outs[(*nl)++] = D[d];
    }
    for (int out = 4; out < 26; out++) {
        if (!has_out(T, out)) continue;
        pl_sample_t ps[3];
        for (int k = 0; k < 3; k++) { rep_t r = idle; press_out(T, out, &r); if (noisy) add_noise(T, &r, k + 7); ps[k] = to_sample(&r); }
        int rc = out >= RW_LSR ? pl_learn_stick(L, out, ps, 3) : pl_learn_button(L, out, ps, 3);
        if (rc != PL_OK) { bad++; printf("   raw bit %d: learn rc %d\n", out, rc); continue; }
        learned_outs[(*nl)++] = out;
    }
    return bad;
}

static void run_T(const char *name, const pd_table_t *Tin, int rid, int len, int noisy)
{
    static pd_table_t T, Lt; static pl_t L;
    T = *Tin;
    int outs[32], nl;
    int bad = learn_all(&L, &T, rid, len, noisy, outs, &nl);
    CHECK(bad == 0, "%s: %d inputs could not be learned", name, bad);
    pl_table(&L, &Lt);
    uint32_t S = 0; for (int i = 0; i < nl; i++) S |= 1u << outs[i];
    rep_t idle; build_idle(&T, rid, len, &idle);
    int nstates = 0, mism = 0, acc1, acc2;
    uint32_t want, got;
    #define COMPARE(R) do { want = eval_masked(&T, &(R), &acc1) & S; got = eval_masked(&Lt, &(R), &acc2); nstates++; \
        if (want != got || acc1 != acc2) { if (mism++ < 3) printf("   %s: state %d: want %08x got %08x (acc %d %d)\n", name, nstates, want, got, acc1, acc2); } } while (0)
    COMPARE(idle);
    for (int i = 0; i < nl; i++) { rep_t r = idle; press_out(&T, outs[i], &r); COMPARE(r); }
    for (int n = 0; n < 300; n++) {
        rep_t r = idle; int dir = -1;
        for (int i = 0; i < nl; i++) {
            if (!(rnd() & 1)) continue;
            if (outs[i] <= RW_UP && outs[i] >= RW_RIGHT) { if (dir >= 0) continue; dir = outs[i]; }
            if ((outs[i] == RW_UP || outs[i] == RW_DOWN) && dir >= 0 && dir != outs[i]) continue;
            press_out(&T, outs[i], &r);
        }
        COMPARE(r);
    }
    for (int i = 0; i < Lt.count; i++) {                                     /* every learned field at min, centre, max */
        const pd_ent_t *e = &Lt.e[i];
        if (e->widthm1 == 0) continue;
        int bit = e->byte * 8 + e->shift, w = e->widthm1 + 1; uint32_t mid = 1u << (w - 1);
        const uint32_t vals[3] = { 0, e->cmp == PDC_HAT ? 15u : mid, (1u << w) - 1 };
        for (int v = 0; v < 3; v++) {
            if (e->cmp == PDC_HAT && v != 1) continue;
            rep_t r = idle; setf(r.r, bit, w, e->bias ? vals[v] ^ mid : vals[v]);
            uint32_t wantx = eval_masked(&T, &r, &acc1) & S; got = eval_masked(&Lt, &r, &acc2); nstates++;
            /* the builder's quarter points and the learner's half-way points may differ between rest and the end stop: only the three landmarks are compared */
            if (e->cmp != PDC_HAT && wantx != got) { if (mism++ < 3) printf("   %s: entry %d field value %u: want %08x got %08x\n", name, i, vals[v], wantx, got); }
        }
    }
    CHECK(mism == 0, "%s%s: %d of %d discrete states differ", name, noisy ? " (noisy)" : "", mism, nstates);
    printf("   %-12s %2d inputs learned into %2d entries (reference %2d), %d states, %s\n", name, nl, Lt.count, T.count, nstates, noisy ? "with noise" : "clean");
}

/* learn a fixture pad, export the SDL line, parse it, build a table from it against the descriptor: it must agree with the learned table on every input it names */
static int g_exported = 0;
static void sdl_roundtrip(const char *name, const uint8_t *d, int dn)
{
    static hp_layout_t HL; static pd_table_t T, Lt, T2; static pl_t L; pd_build_info_t info; static char line[600]; sdl_map_t m;
    if (!hp_parse(d, dn, &HL) || !pd_from_layout(&HL, &T, &info)) return;
    int len = HL.bytes > PD_RPT_BYTES ? PD_RPT_BYTES : HL.bytes, outs[32], nl;
    learn_all(&L, &T, HL.rid, len, 0, outs, &nl);
    pl_table(&L, &Lt);
    int n = pl_export_sdl(&Lt, &HL, 0x1234, 0x5678, 0x0100, name, line, sizeof line);
    CHECK(n > 0, "%s: an SDL line is written", name);
    if (n <= 0) return;
    CHECK(sdl_parse_line(line, &m) && m.vid == 0x1234 && m.pid == 0x5678 && m.ver == 0x0100, "%s: the line parses back with its ids: %s", name, line);
    int ok = pd_from_sdl(&HL, &m, &T2, &info);
    CHECK(ok, "%s: a table can be built from the exported line (%d of %d bindings unresolved)", name, info.nunres, info.nbind);
    if (!ok) return;
    rep_t idle; build_idle(&Lt, HL.rid, len, &idle);
    int cmp = 0, bad = 0, a1, a2;
    for (int out = 0; out < 18; out++) {                       /* the line carries no digital stick bits */
        if (!has_out(&T2, out) || !has_out(&Lt, out)) continue;
        rep_t r = idle; press_out(&Lt, out, &r); cmp++;
        uint32_t w = eval_masked(&Lt, &r, &a1), g = eval_masked(&T2, &r, &a2);
        if (!((w >> out) & 1) || !((g >> out) & 1)) bad++;
        rep_t z = idle; if (((eval_masked(&T2, &z, &a2) >> out) & 1)) bad++;
    }
    CHECK(bad == 0 && cmp >= 8, "%s: the exported line and the learned table agree on %d inputs (%d bad)", name, cmp, bad);
    g_exported++;
}

static void run_fixture(const char *name, const uint8_t *d, int dn, int noisy)
{
    static hp_layout_t HL; static pd_table_t T; pd_build_info_t info;
    if (!hp_parse(d, dn, &HL) || !pd_from_layout(&HL, &T, &info)) { printf("   %-12s no reference table, skipped\n", name); return; }
    run_T(name, &T, HL.rid, HL.bytes > PD_RPT_BYTES ? PD_RPT_BYTES : HL.bytes, noisy);
}

int main(void)
{
    printf("-- learn engine against every fixture\n");
    const struct { const char *n; const uint8_t *d; int l; } F[] = {
        { "ds4", D_DS4, sizeof D_DS4 }, { "snes12", D_SNES12, sizeof D_SNES12 }, { "hat12", D_HAT12, sizeof D_HAT12 }, { "xboxhid", D_XBOXHID, sizeof D_XBOXHID },
        { "twoid", D_TWOID, sizeof D_TWOID }, { "hat4", D_HAT4, sizeof D_HAT4 }, { "dpadbtn", D_DPADBTN, sizeof D_DPADBTN },
        { "universal", D_UNIVERSAL, sizeof D_UNIVERSAL }, { "retro5", D_RETRO5, sizeof D_RETRO5 }, { "order", D_ORDER, sizeof D_ORDER } };
    for (unsigned i = 0; i < sizeof F / sizeof F[0]; i++) { run_fixture(F[i].n, F[i].d, F[i].l, 0); run_fixture(F[i].n, F[i].d, F[i].l, 1); }
    /* D_DPADBTN's shape by hand (the descriptor builder has no table for it): d-pad = 4 buttons, active-low face buttons, an analog trigger, a 16-bit signed stick */
    {
        static pd_table_t T; memset(&T, 0, sizeof T); T.mode = PDM_TABLE;
        pd_add_bit(&T, RW_UP, 0, 0); pd_add_bit(&T, RW_DOWN, 1, 0); pd_add_bit(&T, RW_RIGHT, 2, 0); pd_add_bit(&T, RW_LEFT, 3, 0);
        pd_add_bit(&T, RW_SOUTH, 4, 0); pd_add_bit(&T, RW_EAST, 5, 0); pd_add_bit(&T, RW_WEST, 6, 0); pd_add_bit(&T, RW_NORTH, 7, 0);
        pd_add_bit(&T, RW_BACK, 8, 0); pd_add_bit(&T, RW_START, 9, 0); pd_add_axis(&T, RW_L2, 16, 8, 0, 255, 0);
        pd_add_axis(&T, RW_LSR, 24, 16, -32768, 32767, +1); pd_add_axis(&T, RW_LSL, 24, 16, -32768, 32767, -1);
        run_T("dpad-buttons", &T, 0, 6, 0); run_T("dpad-buttons", &T, 0, 6, 1);
        /* active-low buttons: idle has the bit SET */
        static pd_table_t Z; memset(&Z, 0, sizeof Z); Z.mode = PDM_TABLE;
        pd_add_bit(&Z, RW_SOUTH, 3, 1); pd_add_bit(&Z, RW_EAST, 4, 1);
        run_T("active-low", &Z, 0, 2, 0);
    }
    for (unsigned i = 0; i < sizeof F / sizeof F[0]; i++) sdl_roundtrip(F[i].n, F[i].d, F[i].l);
    printf("   SDL export round trip: %d fixtures\n", g_exported);
    {   /* the .dec file */
        static pd_table_t T, T2; static uint8_t buf[600]; char nm[64];
        pd_builtin_xinput(&T);
        CHECK(pl_dec_name(0x045e, 0x028e, 0x0114, nm, sizeof nm) > 0 && !strcmp(nm, "pad_045e_028e_0114_v1.dec"), "name %s", nm);
        int n = pl_dec_save(0x045e, 0x028e, 0x0114, &T, buf, sizeof buf);
        CHECK(n == 10 + 5 + 5 * T.count, "saved %d bytes", n);
        CHECK(pl_dec_load(buf, n, 0x045e, 0x028e, 0x0114, &T2) && T2.count == T.count && !memcmp(T2.e, T.e, T.count * sizeof T.e[0]) && T2.rid_en == T.rid_en && T2.min_len == T.min_len, "round trip");
        CHECK(!pl_dec_load(buf, n, 0x045e, 0x028e, 0x0115, &T2), "another bcdDevice does not load it");
        CHECK(!pl_dec_load(buf, n - 1, 0x045e, 0x028e, 0x0114, &T2), "a truncated file does not load");
        buf[0] = 'X'; CHECK(!pl_dec_load(buf, n, 0x045e, 0x028e, 0x0114, &T2), "bad magic");
        CHECK(pl_dec_save(1, 2, 3, &T, buf, 20) == 0, "buffer too small");
    }
    {   /* ambiguity, a button whose press moves a button bit AND an analog byte, no baseline */
        static pl_t L; pl_sample_t idle[2], p[2]; memset(idle, 0, sizeof idle); memset(p, 0, sizeof p);
        idle[0].len = idle[1].len = p[0].len = p[1].len = 10;
        pl_begin(&L);
        CHECK(pl_learn_button(&L, RW_L2, p, 2) == PL_BAD, "no baseline");
        pl_baseline(&L, idle, 2);
        p[0].b[6] = p[1].b[6] = 0x04; p[0].b[8] = p[1].b[8] = 0xFF;                 /* DS4's L2: button bit 6.2 and the analog byte 8 */
        CHECK(pl_learn_button(&L, RW_L2, p, 2) == PL_OK && L.t.count == 1 && L.t.e[0].cmp == PDC_NZ && L.t.e[0].byte == 6 && L.t.e[0].shift == 2, "L2 with a button bit and an analog byte -> the button bit");
        memset(p, 0, sizeof p); p[0].len = p[1].len = 10;
        p[0].b[2] = p[1].b[2] = 0x30; p[0].b[9] = p[1].b[9] = 0x30;                 /* two multi-bit groups, no single bit */
        CHECK(pl_learn_button(&L, RW_R2, p, 2) == PL_AMBIG, "two analog groups -> ambiguous");
        memset(p, 0, sizeof p); p[0].len = p[1].len = 10;
        CHECK(pl_learn_button(&L, RW_R2, p, 2) == PL_NOCHANGE, "nothing pressed -> no change");
        p[0].b[1] = 1; p[1].b[1] = 0;                                                /* the two samples disagree: noise, not a press */
        CHECK(pl_learn_button(&L, RW_R2, p, 2) == PL_NOCHANGE, "a flickering bit is noise");
    }
    printf("%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
