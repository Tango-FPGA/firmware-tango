// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 the Tango authors
/*
 * test_padgen.c -- the "any HID pad a user brings" campaign: N seeded, generated pads (padgen.h) through the REAL pad manager, the REAL capture
 * function (padmgr_capture, on the fake FPGA's clock, with the 16-byte REPORT2 truncation) and the REAL Define-buttons session (plsess_*).
 * For every pad:
 *   - the descriptor parser (hp_parse) must find every control the generator put in the descriptor, at the right bit offset and size;
 *   - the table the manager builds from the descriptor alone (no database line, no learning) must be quiet at rest;
 *   - a simulated user walks MiSTer's Define order (plsess_default_order), pressing the pad's own controls (in a random assignment), with noise:
 *     a counter byte, wobble on the analog axes, a transient first sample, a second report id interleaved;
 *   - the learned table, as stored in the (fake) FPGA, is checked against the GROUND TRUTH: each learned input alone gives exactly its bit, rest gives
 *     0, an analog control at 20 % gives nothing and at 90 % gives its bit, two controls together give the OR.
 * Failure classes are counted and an example seed is printed for each; "known limits" (a control past byte 15, a declared-absent control the user
 * cannot press) are listed separately.  Reproducible: the seed alone rebuilds the pad.
 *
 *   ./test_padgen [N=200] [first_seed=1] [-ls|-fs] [-v] [-seed S] [-emit DIR] [-fail-ok]
 * -emit DIR writes DIR/gen_<seed>.prof (the USB-side description for the RTL bench: menu-tango fpga/sim/tb_devs.inc "dev=file:") and DIR/vec_gen_<seed>.txt (the
 * learned table + the reports to replay, in pad_rtl_vectors.c's format).
 */
#define main test_padmgr_main
#include "test_padmgr.c"
#undef main
#include "../tango_hidparse.h"
#include "padgen.h"
#include "real_kernel_pads.h"
#include "real_pads.h"
#include "../tango_padmap.h"

/* ---- failure classes */
enum { C_PARSE = 0, C_AUTO_PHANTOM, C_BASELINE, C_STEP, C_WRONG, C_IDLE, C_COMBO, C_FINISH, C_PARTIAL, C_NCLASS };
static const char *CNAME[C_NCLASS] = { "descriptor parse (a control missing or misplaced)", "descriptor-only table: phantom input at rest",
    "Define baseline failed", "a real, reachable control could not be learned", "learned input gives the wrong raw bits",
    "learned table: phantom input at rest", "two controls together do not OR", "plsess_finish incomplete", "analog partial press (20 %) triggers / 90 % does not" };
enum { L_BEYOND16 = 0, L_ABSENT, L_NCLASS };
static const char *LNAME[L_NCLASS] = { "a real control lies past byte 15 (decode keeps 16 bytes)", "declared-but-absent controls (user skips them)" };

static const uint8_t *g_real_d = NULL; static int g_real_n = 0;       /* -real: the HID side of the pad is this real descriptor (gp_adopt) */
static int g_nodesc = 0, g_quiet = 0, g_dialog = 0, g_stuck = 0;      /* -stuck (with -dialog): the FPGA's decoded word reads a held direction bit all the time (a pad whose decode misreads its rest state): the dialog's 3 s settle fallback */       /* -dialog: the walk is the menu core's Define dialog (tango_pmcap_sample) instead of bare plsess calls */       /* -nodesc: the descriptor is withheld from the learner and the capture (a pad whose descriptor does not parse): bit-diff mode */
typedef struct {
    int cls[C_NCLASS]; int lim[L_NCLASS]; uint32_t first_cls[C_NCLASS], first_lim[L_NCLASS];
    int pads, clean, ls_pads, learned_inputs, asked_inputs, auto_dead, auto_inputs;
} stats_t;

typedef struct {
    rig_t *R; const gpad_t *g;
    int t, counter; gp_rng_t noise;
    int ctl;                 /* the control being held in the current press window, -1 none */
    int press_from, press_to;
    int transient;           /* the first ticks of the press are partial (analog) / idle (digital) */
    int tick_in_press;
    int second_every;        /* every Nth tick the second report id is sent */
    int cancel_at;
} pgclk_t;

static void pg_clk_hook(void *c, unsigned ms)
{
    pgclk_t *k = c; k->t += (int)ms; k->counter++;
    { int iv = k->g->ep_iv ? k->g->ep_iv : 1; int adv = (int)ms / iv; if (adv > 1) k->noise.cnt += (uint32_t)(adv - 1); }   /* the pad reports every bInterval ms: a counter moves that many steps between two polls */
    uint8_t r[64]; gp_press_t pr[2]; int np = 0;
    int pressed = (k->ctl >= 0 && k->t >= k->press_from && k->t < k->press_to);
    if (pressed) {
        int pct = 100;
        if (k->transient && k->tick_in_press < 2) pct = k->tick_in_press == 0 ? 0 : 55;      /* the press has not registered yet / half-way */
        k->tick_in_press++;
        pr[np].c = k->ctl; pr[np].pct = pct; np++;
    } else k->tick_in_press = 0;
    int len = gp_report(k->g, pr, np, &k->noise, r);
    fpga_send_report(&k->R->fp, 0, r, len);
    if (k->second_every && (k->counter % k->second_every) == 0 && k->g->second_rid) {     /* another report id (a consumer-control or vendor report) */
        uint8_t o[8]; o[0] = (uint8_t)k->g->second_rid; for (int i = 1; i < 8; i++) o[i] = (uint8_t)gp_rnd(&k->noise);
        fpga_send_report(&k->R->fp, 0, o, 3 + (int)(k->counter % 5));
    }
}
static int pg_cancel(void *c) { pgclk_t *k = c; return k->cancel_at && k->t >= k->cancel_at; }

typedef struct { pgclk_t *K; padmgr_cap_t *C; int assign[32]; int nreach_fail; int dbg; } pgcap_t;
static int pg_cap(void *c, int phase, pl_sample_t *out, int max)
{
    pgcap_t *P = c; pgclk_t *K = P->K;
    if (PLC_KIND(phase) == PLC_HELD) {
        int rw = PLC_RW(phase);
        K->ctl = (rw >= 0 && rw < 32) ? P->assign[rw] : -1;
        K->press_from = K->t + 420; K->press_to = K->t + 760; K->tick_in_press = 0;
    } else K->ctl = -1;
    int n = padmgr_capture(P->C, phase, out, max);
    if (P->dbg) { printf("    capture phase %d rw %d ctl %d -> %d samples:", PLC_KIND(phase), PLC_RW(phase), K->ctl, n); for (int i = 0; i < n && i < 6; i++) { printf(" ["); for (int b = 0; b < out[i].len && b < 16; b++) printf("%02x", out[i].b[b]); printf("]"); } printf("\n"); }
    return n;
}

static int pg_press_raw(const gpad_t *g, const pd_table_t *T, const gp_press_t *pr, int np, uint32_t *raw)
{
    uint8_t r[64]; int len = gp_report(g, pr, np, NULL, r);
    *raw = 0; return pd_eval(T, r, len, raw);
}

static const char *RWN[] = { "RIGHT", "LEFT", "DOWN", "UP", "SOUTH", "EAST", "WEST", "NORTH", "L1", "R1", "L2", "R2", "BACK", "START", "L3", "R3", "GUIDE", "MISC",
                             "LSR", "LSL", "LSD", "LSU", "RSR", "RSL", "RSD", "RSU" };

/* a pad -> one campaign run; returns 1 if no failure class was hit */
static int run_pad(uint32_t seed, int force_ls, int verbose, const char *emit_dir, stats_t *st)
{
    static gpad_t g; static rig_t R; static hp_layout_t HL; static plsess_t S; static cardf_t F; static pgclk_t K; static padmgr_cap_t C; static pgcap_t P;
    if (g_real_d) { static hp_layout_t RL; if (!hp_parse(g_real_d, g_real_n, &RL) || !gp_adopt(&g, g_real_d, g_real_n, &RL, seed)) { st->pads++; st->clean++; return 1; } }
    else gp_make(&g, seed, force_ls);
    if (g_quiet) { g.wobble = 0; for (int fi = 0; fi < g.nf; fi++) if (g.f[fi].kind == GF_VEND) g.f[fi].vconst = 1; }       /* -quiet: no wobble, no counters or sensors */
    int failed = 0; int hit[C_NCLASS] = { 0 }; int lim_hit[L_NCLASS] = { 0 };
    #define FAILC(cl, ...) do { if (!hit[cl]) { hit[cl] = 1; st->cls[cl]++; if (!st->first_cls[cl]) st->first_cls[cl] = seed; } failed = 1; if (verbose) { printf("   seed %u: [%s] ", seed, CNAME[cl]); printf(__VA_ARGS__); printf("\n"); } } while (0)
    st->pads++; if (g.ls) st->ls_pads++;

    if (verbose > 1) {
        printf("seed %u: %s speed, bcdUSB %04x, mps0 %d, ep %d mps %d iv %d, out %d, pre %d, rid %d (second %d), rlen %d, dpad_kind %d, nbtn %d (real %d) sticks %d trig %d sz %d sgn %d wobble %d\n",
               seed, g.ls ? "low" : "full", g.bcd_usb, g.mps0, g.ep_addr, g.ep_mps, g.ep_iv, g.has_out, g.n_pre, g.rid, g.second_rid, g.rlen, g.dpad_kind, g.nbtn, g.nreal, g.nstick, g.ntrig, g.sz, g.sgn, g.wobble);
        printf("  descriptor (%d): ", g.dn); for (int i = 0; i < g.dn; i++) printf("%02x", g.d[i]); printf("\n");
        for (int fi = 0; fi < g.nf; fi++) printf("  f%d kind %d bit %d size %d count %d lmin %d lmax %d usage %04x:%04x form %d\n", fi, g.f[fi].kind, g.f[fi].bit, g.f[fi].size, g.f[fi].count, g.f[fi].lmin, g.f[fi].lmax, g.f[fi].page, g.f[fi].usage, g.f[fi].form);
    }
    /* ---- 1. the descriptor parser against the generator's field list */
    int parsed = hp_parse(g.d, g.dn, &HL);
    if (!parsed) FAILC(C_PARSE, "hp_parse refused the descriptor (cls %d, %d bytes)", HL.cls, g.dn);
    else {
        if (verbose > 1) { printf("  parsed: rid %d bytes %d nbtn %d naxis %d nhat %d\n", HL.rid, HL.bytes, HL.nbtn, HL.naxis, HL.nhat); for (int q = 0; q < HL.nf; q++) printf("   p%d kind %d bit %d size %d lmin %d lmax %d page %x usage %x\n", q, HL.f[q].kind, HL.f[q].bit, HL.f[q].size, HL.f[q].lmin, HL.f[q].lmax, HL.f[q].page, HL.f[q].usage); }
        if (HL.rid != g.rid) FAILC(C_PARSE, "report id %d (want %d)", HL.rid, g.rid);
        for (int fi = 0; fi < g.nf; fi++) {
            const gp_field_t *f = &g.f[fi];
            int want_kind = (f->kind == GF_BTN || f->kind == GF_BTNGD) ? HPK_BUTTON : f->kind == GF_HAT ? HPK_HAT : f->kind == GF_AXIS ? HPK_AXIS : 0;
            if (!want_kind) continue;
            int n = (want_kind == HPK_BUTTON) ? f->count : 1;
            for (int e = 0; e < n; e++) {
                int bit = f->bit + e * f->size, found = 0;
                for (int q = 0; q < HL.nf; q++) if (HL.f[q].kind == want_kind && HL.f[q].bit == bit && HL.f[q].size == f->size) found = 1;
                if (!found) { FAILC(C_PARSE, "field kind %d bit %d size %d not in the parsed layout", f->kind, bit, f->size); break; }
            }
        }
    }

    /* ---- 2. the pad, the manager, the table built from the descriptor alone */
    rig_start(&R, 0, 0); padmgr_core_start(R.m); memset(&F, 0, sizeof F);
    plug(&R, 0, g.vid, g.pid, g.ver, 3, 0, 0, g.d, g.dn, 1);
    uint8_t idle[64]; int rlen = gp_report(&g, NULL, 0, NULL, idle);
    fpga_send_report(&R.fp, 0, idle, rlen); snap(&R, 0);
    if (R.fp.p[0].mode == PDM_TABLE) {
        uint32_t raw = 0; pd_eval(&R.fp.p[0].tab, idle, rlen, &raw);
        if (raw) FAILC(C_AUTO_PHANTOM, "raw %08x with nothing pressed", raw);
        for (int i = 0; i < g.nc; i++) {
            const gp_ctl_t *c = &g.c[i]; if (c->absent || !gp_reachable(&g, i)) continue;
            gp_press_t p1 = { i, 100 }; uint32_t r2; pg_press_raw(&g, &R.fp.p[0].tab, &p1, 1, &r2); st->auto_inputs++; if (!r2) st->auto_dead++;
        }
    }

    /* ---- 3. the user's assignment: item (raw bit) -> control */
    memset(P.assign, 0xFF, sizeof P.assign);
    { int dpad[4] = { -1, -1, -1, -1 }, stick[2][4] = { { -1, -1, -1, -1 }, { -1, -1, -1, -1 } }, trig[2] = { -1, -1 }; int btn[GP_MAXC], nb = 0;
      for (int i = 0; i < g.nc; i++) {
          const gp_ctl_t *c = &g.c[i];
          if (c->role == GR_DPAD) dpad[c->slot] = i;
          else if (c->role == GR_STICK) stick[c->slot / 4][c->slot % 4] = i;
          else if (c->role == GR_TRIG) trig[c->slot] = i;
          else if (c->role == GR_BTN && !c->absent) btn[nb++] = i;
      }
      gp_rng_t ur = { seed * 40503u + 7u, 0 }; for (int i = nb - 1; i > 0; i--) { int j = gp_rn(&ur, i + 1); int t = btn[i]; btn[i] = btn[j]; btn[j] = t; }
      for (int d = 0; d < 4; d++) P.assign[d] = dpad[d];
      for (int s = 0; s < 2; s++) for (int d = 0; d < 4; d++) P.assign[RW_LSR + s * 4 + d] = stick[s][d];
      static const int order[] = { RW_EAST, RW_SOUTH, RW_NORTH, RW_WEST, RW_L1, RW_R1, RW_BACK, RW_START, RW_L2, RW_R2, RW_L3, RW_R3, RW_GUIDE };
      int bi = 0;
      for (unsigned o = 0; o < sizeof order / sizeof order[0]; o++) {
          int rw = order[o];
          if ((rw == RW_L2 || rw == RW_R2) && trig[rw == RW_L2 ? 0 : 1] >= 0) { P.assign[rw] = trig[rw == RW_L2 ? 0 : 1]; continue; }
          if (bi < nb) P.assign[rw] = btn[bi++];
      }
    }
    int nabsent = 0; for (int i = 0; i < g.nc; i++) if (g.c[i].absent) nabsent++;
    if (nabsent) { st->lim[L_ABSENT]++; if (!st->first_lim[L_ABSENT]) st->first_lim[L_ABSENT] = seed; }

    /* ---- 4. Define: baseline, then MiSTer's order */
    memset(&K, 0, sizeof K); K.R = &R; K.g = &g; K.ctl = -1; K.noise.s = seed * 977u + 3u;
    K.transient = gp_pct(&(gp_rng_t){ seed * 31u + 5u, 0 }, 30); K.second_every = g.second_rid ? 3 : 0; K.cancel_at = 0;
    R.e.rl.delay_hook = pg_clk_hook; R.e.rl.delay_ctx = &K;
    memset(&C, 0, sizeof C); C.m = R.m; C.port = 0; C.cancelled = pg_cancel; C.cctx = &K; C.press_timeout_ms = 900; C.L = parsed && !g_nodesc ? &HL : NULL; C.rid_en = parsed && !g_nodesc && HL.rid != 0; C.rid = parsed ? HL.rid : 0;
    P.K = &K; P.C = &C; P.dbg = verbose > 1;
    fpga_send_report(&R.fp, 0, idle, rlen);
    static tango_pmcap_t PMC; static tango_pmlearn_t LM; plsess_t *Sp = &S;
    int learned[32]; int nlearned = 0; memset(learned, 0, sizeof learned); int rc = PLS_OK;
    if (g_dialog) {
        /* the menu core's Define joystick buttons: tango_pmcap_sample on a 20 ms clock, a user who presses the control of each prompt (and Space for a control the pad lacks) */
        memset(&LM, 0, sizeof LM); tango_pmlearn_zero(&LM);
        tango_pmcap_start_sys(&PMC); tango_pmlearn_port(&LM, 0, g.vid, g.pid, g.ver, g.bcd_usb == 0 ? 0 : 0x0110, parsed && !g_nodesc ? &HL : NULL); tango_pmcap_learn(&PMC, &LM);
        gp_press_t pr[1]; int sj = 0, cur_btn = -1, press_a = 0, press_b = 0, tries = 0; uint32_t tt = 0; int done = 0, space_after = -1;
        for (int it = 0; it < 6000 && !done; it++) {
            tt += 20;
            if (PMC.button != cur_btn) {                               /* a new prompt: schedule the user's press */
                cur_btn = PMC.button; tries = 0; space_after = -1;
                press_a = (int)tt + (PMC.dev < 0 ? (g_stuck ? 3700 : 900) : 300); press_b = press_a + 340;       /* the first press comes after the page's settle (150 ms) and rest state (12 reports): a user needs about a second to find the control */
            } else if ((int)tt > press_b + 300 && cur_btn < 12) {      /* the same prompt still: the learner did not take it */
                if (++tries >= 3) space_after = 1; else { press_a = (int)tt + 200; press_b = press_a + 340; }
            }
            int rwk = cur_btn >= 0 && cur_btn < 12 ? (int)(tango_pm_mmap[cur_btn] - 0x100u) : -1;
            int ctlk = rwk >= 0 ? P.assign[rwk] : -1;
            if (cur_btn >= 0 && cur_btn < 12 && (ctlk < 0 || space_after > 0)) {    /* no such control, or it could not be learned: Space skips */
                int r = tango_pmcap_key(&PMC, PMC_KEY_SPACE);
                if (rwk >= 0 && ctlk >= 0 && !gp_reachable(&g, ctlk)) { if (!lim_hit[L_BEYOND16]) { lim_hit[L_BEYOND16] = 1; st->lim[L_BEYOND16]++; if (!st->first_lim[L_BEYOND16]) st->first_lim[L_BEYOND16] = seed; } }
                else if (rwk >= 0 && ctlk >= 0) FAILC(C_STEP, "button %d (control %d) not learned after 3 tries: \"%s\"", cur_btn, ctlk, LM.msg);
                if (r == PMC_SAVED) done = 1;
                cur_btn = -1; continue;
            }
            int np = 0;
            if (ctlk >= 0 && (int)tt >= press_a && (int)tt < press_b) { int pct = 100; if (K.transient && (int)tt < press_a + 60) pct = (int)tt < press_a + 20 ? 0 : 55; pr[np].c = ctlk; pr[np].pct = pct; np++; }
            uint8_t rb[64]; int rl = gp_report(&g, pr, np, &K.noise, rb);
            K.noise.cnt += (uint32_t)(20 / (g.ep_iv ? g.ep_iv : 1));
            if (g.second_rid && (it % 3) == 0) { uint8_t o[8]; o[0] = (uint8_t)g.second_rid; for (int i = 1; i < 8; i++) o[i] = (uint8_t)gp_rnd(&K.noise); pl_sample_t so; memset(&so, 0, sizeof so); memcpy(so.b, o, 8); so.len = 6; tango_pmcap_sample(&PMC, 0, &so, tt); }
            tango_pmcap_raw(&PMC, 0, g.vid, g.pid, g_stuck ? (1u << 18) : 0u, tt);      /* the firmware feeds the FPGA's decoded word every report (tango_fw.c raw_in): it sets the settle gate's `prev` */
            pl_sample_t sm; memset(&sm, 0, sizeof sm); memcpy(sm.b, rb, PD_RPT_BYTES); sm.len = (uint8_t)(rl > PD_RPT_BYTES ? PD_RPT_BYTES : rl);
            int r = tango_pmcap_sample(&PMC, 0, &sm, tt);
            if (r == PMC_SAVED) done = 1;
            (void)sj;
        }
        tango_pmcap_learn_finish(&PMC);
        Sp = tango_pmlearn_session(&LM);
        if (!done && Sp) FAILC(C_STEP, "the dialog did not finish (button %d of %d)", PMC.button, PMC.count);
        if (!Sp) {                                                  /* no press was ever taken: only a control past byte 15 explains it */
            int unreach = 0; for (int d = 0; d < 4; d++) if (P.assign[d] >= 0 && !gp_reachable(&g, P.assign[d])) unreach = 1;
            if (!unreach) FAILC(C_BASELINE, "no session was begun (no press taken)");
            else if (!lim_hit[L_BEYOND16]) { lim_hit[L_BEYOND16] = 1; st->lim[L_BEYOND16]++; if (!st->first_lim[L_BEYOND16]) st->first_lim[L_BEYOND16] = seed; }       /* the walk waits for a first press that never shows: "nothing pressed: unchanged" */
            Sp = &S;
        }
        else {
            if (verbose > 1) { printf("  dialog map:"); for (int b = 0; b < 12; b++) printf(" %x", PMC.map[b]); printf("  msg \"%s\"\n", LM.msg); }
            for (int b = 0; b < 12; b++) if (PMC.map[b]) { learned[tango_pm_mmap[b] - 0x100u] = 1; nlearned++; }
            nlearned = 0; for (int b = 0; b < 32; b++) nlearned += learned[b];
            if (!lim_hit[L_BEYOND16] && 0) {}
        }
    } else {
    plsess_begin(&S, g.vid, g.pid, g.ver, parsed && !g_nodesc ? &HL : NULL, "gen");
    rc = plsess_baseline(&S, pg_cap, &P);
    if (rc != PLS_OK) FAILC(C_BASELINE, "baseline rc %d", rc);
    else {
        char msg[40];
        for (int i = 0; i < plsess_default_order_n; i++) {
            const plsess_item_t *it = &plsess_default_order[i];
            if (it->kind == PLI_DPAD) {
                if (it->rw != RW_RIGHT) continue;
                int all = P.assign[RW_RIGHT] >= 0 && P.assign[RW_LEFT] >= 0 && P.assign[RW_DOWN] >= 0 && P.assign[RW_UP] >= 0;
                if (!all) { for (int d = 0; d < 4; d++) plsess_skip(&S, d); continue; }
                int tries = 0;
                do { rc = plsess_dpad(&S, pg_cap, &P, msg, sizeof msg); } while (rc != PLS_OK && rc != PLS_CANCEL && ++tries < 3);
                st->asked_inputs += 4;
                if (rc == PLS_OK) { for (int d = 0; d < 4; d++) learned[d] = 1; nlearned += 4; st->learned_inputs += 4; }
                else {
                    int reach = 1; for (int d = 0; d < 4; d++) if (!gp_reachable(&g, P.assign[d])) reach = 0;
                    if (reach) FAILC(C_STEP, "d-pad not learned (rc %d: \"%s\")", rc, msg);
                    else { if (!lim_hit[L_BEYOND16]) { lim_hit[L_BEYOND16] = 1; st->lim[L_BEYOND16]++; if (!st->first_lim[L_BEYOND16]) st->first_lim[L_BEYOND16] = seed; } }
                }
                continue;
            }
            int ctl = P.assign[it->rw];
            if (ctl < 0) { plsess_skip(&S, it->rw); continue; }
            st->asked_inputs++;
            int tries = 0;
            do { rc = plsess_step(&S, it->rw, pg_cap, &P, msg, sizeof msg); } while (rc != PLS_OK && rc != PLS_CANCEL && ++tries < 3);
            if (rc == PLS_OK) { learned[it->rw] = 1; nlearned++; st->learned_inputs++; }
            else {
                if (gp_reachable(&g, ctl)) FAILC(C_STEP, "%s (control %d, role %d) not learned: rc %d \"%s\"", it->label, ctl, g.c[ctl].role, rc, msg);
                else { if (!lim_hit[L_BEYOND16]) { lim_hit[L_BEYOND16] = 1; st->lim[L_BEYOND16]++; if (!st->first_lim[L_BEYOND16]) st->first_lim[L_BEYOND16] = seed; } plsess_skip(&S, it->rw); }
            }
        }
    }
    }
    R.e.rl.delay_hook = NULL;
    plsess_files_t files = cf_table(&F); char sdl[700];
    int fl = plsess_finish(Sp, R.m, 0, &files, sdl, sizeof sdl);
    if (nlearned > 0 && !(fl & PLF_APPLIED)) FAILC(C_FINISH, "plsess_finish flags %d with %d inputs learned", fl, nlearned);
    if (nlearned > 0 && !(fl & PLF_DEC)) FAILC(C_FINISH, ".dec not stored (flags %d)", fl);

    /* ---- 5. the oracle on the table the FPGA holds */
    const pd_table_t *T = &R.fp.p[0].tab;
    if (verbose > 1) { char buf[2400]; pd_dump(T, buf, sizeof buf); printf("  learned table (mode %d, %d entries, min_len %d rid %d/%d): %s\n", R.fp.p[0].mode, T->count, T->min_len, T->rid_en, T->rid_val, buf); }
    if (nlearned > 0 && R.fp.p[0].mode == PDM_TABLE) {
        uint32_t raw; int acc;
        for (int rw = 0; rw < 32; rw++) {
            if (!learned[rw] || P.assign[rw] < 0) continue;
            gp_press_t p1 = { P.assign[rw], 100 };
            acc = pg_press_raw(&g, T, &p1, 1, &raw);
            if (!acc || raw != (1u << rw)) FAILC(C_WRONG, "%s: pressing control %d gives raw %08x (want %08x)%s", RWN[rw], P.assign[rw], raw, 1u << rw, g.c[P.assign[rw]].kind == CT_AXIS ? " [axis]" : "");
            const gp_ctl_t *c = &g.c[P.assign[rw]];
            if (c->kind == CT_AXIS && (c->role == GR_STICK || c->role == GR_TRIG || (c->role == GR_DPAD))) {
                gp_press_t lo = { P.assign[rw], 20 }, hi = { P.assign[rw], 90 };
                uint32_t rl, rh; pg_press_raw(&g, T, &lo, 1, &rl); pg_press_raw(&g, T, &hi, 1, &rh);
                if ((rl >> rw) & 1) FAILC(C_PARTIAL, "%s at 20 %% already triggers (raw %08x)", RWN[rw], rl);
                if (!((rh >> rw) & 1)) FAILC(C_PARTIAL, "%s at 90 %% does not trigger (raw %08x)", RWN[rw], rh);
            }
        }
        gp_rng_t nr = { seed * 7919u + 1u, 0 };
        for (int k = 0; k < 40; k++) {                                     /* rest, with noise */
            uint8_t r[64]; int len = gp_report(&g, NULL, 0, &nr, r); raw = 0; pd_eval(T, r, len, &raw);
            if (raw) { FAILC(C_IDLE, "rest report %d gives raw %08x", k, raw); break; }
        }
        for (int k = 0; k < 40; k++) {                                     /* two controls at once, from different fields */
            int a = -1, b = -1, ra = gp_rn(&nr, 32), rb = gp_rn(&nr, 32);
            if (!learned[ra] || !learned[rb] || ra == rb) continue;
            a = P.assign[ra]; b = P.assign[rb];
            if (g.c[a].fld == g.c[b].fld && g.c[a].kind != CT_BTN) continue;
            gp_press_t p2[2] = { { a, 100 }, { b, 100 } };
            acc = pg_press_raw(&g, T, p2, 2, &raw);
            if (!acc || raw != ((1u << ra) | (1u << rb))) { FAILC(C_COMBO, "%s+%s gives raw %08x (want %08x)", RWN[ra], RWN[rb], raw, (1u << ra) | (1u << rb)); break; }
        }
    }

    /* ---- 6. emit for the RTL bench */
    /* a composite whose FIRST interface is a boot keyboard is classed a keyboard by the host (kbdfirst, a design decision that keeps keyboards working): its pad is not used, so the RTL bench has nothing to learn from */
    int kbd_first = g.n_pre > 0 && g.pre_kind[0] == 1;
    if (emit_dir && !failed && nlearned > 0 && R.fp.p[0].mode == PDM_TABLE && !kbd_first) {
        char path[300]; FILE *fo;
        snprintf(path, sizeof path, "%s/gen_%u.prof", emit_dir, seed);
        if ((fo = fopen(path, "w"))) {
            fprintf(fo, "PROFILE gen_%u\nSPEED %s\nMPS0 %d\nEP %d\nEPMPS %d\nEPIV %d\nHIDIF %d\nNAK %d\nNEEDIDLE %d\nSTALLSTR %d\nOUTEP %d\n", seed, g.ls ? "ls" : "fs", g.mps0, g.ep_addr, g.ep_mps, g.ep_iv, g.hid_if, g.nak_polls, g.needs_idle, g.stall_str, g.has_out);
            fprintf(fo, "DD "); for (int i = 0; i < 18; i++) fprintf(fo, "%02x", g.dd[i]); fprintf(fo, "\nCD "); for (int i = 0; i < g.cdn; i++) fprintf(fo, "%02x", g.cd[i]);
            fprintf(fo, "\nRD "); for (int i = 0; i < g.dn; i++) fprintf(fo, "%02x", g.d[i]);
            fprintf(fo, "\nIDLE "); for (int i = 0; i < rlen; i++) fprintf(fo, "%02x", idle[i]); fprintf(fo, "\n"); fclose(fo);
        }
        snprintf(path, sizeof path, "%s/vec_gen_%u.txt", emit_dir, seed);
        if ((fo = fopen(path, "w"))) {
            uint8_t wire[512]; int wn = pd_wire(T, wire, sizeof wire);
            fprintf(fo, "PROFILE gen_%u\nTABLE %d %d %d %d %d\nWIRE ", seed, T->mode, T->rid_en, T->rid_val, T->min_len, T->count);
            for (int i = 0; i < wn; i++) fprintf(fo, "%02x", wire[i]);
            fprintf(fo, "\n");
            uint32_t cur = 0;
            #define EMIT(buf, len) do { uint32_t rw_ = cur; int a_ = pd_eval(T, (buf), (len) > 16 ? 16 : (len), &rw_); if (a_) cur = rw_; fprintf(fo, "REPORT %d ", (len)); for (int i_ = 0; i_ < (len); i_++) fprintf(fo, "%02x", (buf)[i_]); fprintf(fo, " %d %08x\n", a_, cur); } while (0)
            EMIT(idle, rlen);
            for (int rw = 0; rw < 32; rw++) if (learned[rw] && P.assign[rw] >= 0) { gp_press_t p1 = { P.assign[rw], 100 }; uint8_t r[64]; int len = gp_report(&g, &p1, 1, NULL, r); EMIT(r, len); EMIT(idle, rlen); }
            fprintf(fo, "END\n"); fclose(fo);
        }
    }
    int ok = !failed;
    rig_done(&R);
    if (ok) st->clean++;
    if (g.beyond16) { /* informational */ }
    return ok;
}

static void print_stats(const stats_t *st, const char *title)
{
    printf("== %s: %d pads (%d low speed): %d clean = %.1f %%\n", title, st->pads, st->ls_pads, st->clean, st->pads ? 100.0 * st->clean / st->pads : 0.0);
    printf("   inputs the user was asked for: %d, learned: %d; descriptor-only table: %d of %d reachable real controls give no raw bit (the user is expected to run Define)\n",
           st->asked_inputs, st->learned_inputs, st->auto_dead, st->auto_inputs);
    for (int c = 0; c < C_NCLASS; c++) if (st->cls[c]) printf("   FAILURE CLASS %-60s %4d pads (first seed %u)\n", CNAME[c], st->cls[c], st->first_cls[c]);
    for (int c = 0; c < L_NCLASS; c++) if (st->lim[c]) printf("   known limit  %-60s %4d pads (first seed %u)\n", LNAME[c], st->lim[c], st->first_lim[c]);
}

int main(int argc, char **argv)
{
    int real_mode = 0; int n = 200; uint32_t first = 1; int force_ls = -1, verbose = 0, fail_ok = 0; const char *emit = NULL; int one = 0; uint32_t oneseed = 0;
    int pos = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-ls")) force_ls = 1; else if (!strcmp(argv[i], "-fs")) force_ls = 0; else if (!strcmp(argv[i], "-v")) verbose = 1; else if (!strcmp(argv[i], "-vv")) verbose = 2;
        else if (!strcmp(argv[i], "-fail-ok")) fail_ok = 1; else if (!strcmp(argv[i], "-nodesc")) g_nodesc = 1; else if (!strcmp(argv[i], "-real")) real_mode = 1; else if (!strcmp(argv[i], "-dialog")) g_dialog = 1; else if (!strcmp(argv[i], "-stuck")) g_stuck = 1; else if (!strcmp(argv[i], "-quiet")) g_quiet = 1;
        else if (!strcmp(argv[i], "-seed") && i + 1 < argc) { one = 1; oneseed = (uint32_t)strtoul(argv[++i], NULL, 0); verbose = 2; }
        else if (!strcmp(argv[i], "-emit") && i + 1 < argc) emit = argv[++i];
        else if (pos == 0) { n = atoi(argv[i]); pos++; } else if (pos == 1) { first = (uint32_t)strtoul(argv[i], NULL, 0); pos++; }
    }
    static stats_t st; memset(&st, 0, sizeof st);
    int real_only = -1; for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "-realidx") && i + 1 < argc) real_only = atoi(argv[i + 1]);
    if (real_mode) {
        /* every REAL descriptor in the fixtures: the kernel drivers' game-controller descriptors (real_kernel_pads.h), a low-speed DragonRise pad (0079:0006) (real_pads.h), and the
         * descriptors of other HID devices of a Linux PC (real_descriptors.h: mice, keyboards, a receiver), which must be REFUSED as pads */
        extern int real_run(int variants, int verbose, int *pads_tested, int *refused);
        int total = 0, refused = 0, bad = 0;
        static stats_t rs;
        struct { const char *name; const uint8_t *d; int n; } L2[64]; int nl = 0;
        for (int i = 0; i < REAL_KERNEL_PADS_N && nl < 60; i++) { L2[nl].name = REAL_KERNEL_PADS[i].name; L2[nl].d = REAL_KERNEL_PADS[i].d; L2[nl].n = REAL_KERNEL_PADS[i].n; nl++; }
        L2[nl].name = "DragonRise 0079:0006 (hidraw, this PC)"; L2[nl].d = RP_LS_RPT; L2[nl].n = sizeof RP_LS_RPT; nl++;
        for (unsigned i = 0; i < sizeof REAL_DESCS / sizeof REAL_DESCS[0] && nl < 63; i++) { L2[nl].name = REAL_DESCS[i].name; L2[nl].d = REAL_DESCS[i].d; L2[nl].n = REAL_DESCS[i].n; nl++; }
        for (int i = 0; i < nl; i++) {
            if (real_only >= 0 && i != real_only) continue;
            static hp_layout_t HLx; int ok = hp_parse(L2[i].d, L2[i].n, &HLx);
            if (!ok) { refused++; printf("   %-44s %4d bytes: not a pad (class %d): refused\n", L2[i].name, L2[i].n, HLx.cls); continue; }
            memset(&rs, 0, sizeof rs); g_real_d = L2[i].d; g_real_n = L2[i].n;
            for (int k = 0; k < n; k++) run_pad(first + (uint32_t)k, force_ls, verbose, NULL, &rs);
            g_real_d = NULL; total++;
            printf("   %-44s %4d bytes: %d buttons %d axes %d hat, rid %d: %d of %d runs clean%s\n", L2[i].name, L2[i].n, HLx.nbtn, HLx.naxis, HLx.nhat, HLx.rid, rs.clean, rs.pads, rs.clean == rs.pads ? "" : "  <-- FAILURES");
            if (rs.lim[L_BEYOND16]) printf("        known limit: %d of %d runs had a control past byte 15\n", rs.lim[L_BEYOND16], rs.pads);
            for (int c = 0; c < C_NCLASS; c++) if (rs.cls[c]) printf("        failure class: %s (%d, first seed %u)\n", CNAME[c], rs.cls[c], rs.first_cls[c]);
            if (rs.clean != rs.pads) bad++;
        }
        printf("== real descriptors: %d pads tested with %d runs each (%d descriptors refused as not-a-pad), %d with failures\n", total, n, refused, bad);
        printf("%d checks, %d failed\n", checks, fails);
        return bad && !fail_ok ? 1 : 0;
    }
    if (one) { first = oneseed; n = 1; }
    for (int k = 0; k < n; k++) run_pad(first + (uint32_t)k, force_ls, verbose, emit, &st);
    print_stats(&st, force_ls == 1 ? "low-speed pads" : force_ls == 0 ? "full-speed pads" : "pads");
    printf("%d checks, %d failed\n", checks, fails);
    return (st.pads - st.clean) && !fail_ok ? 1 : 0;
}
