// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 the Tango authors
/*
 * test_dragonrise.c -- a low-speed pad as a fixture: DragonRise "PC TWIN SHOCK Gamepad" 0079:0006, LOW SPEED, REAL descriptors (real_pads.h) through the REAL pad
 * manager, capture function and Define-buttons session.  No code in the firmware is keyed on this pad: it is here as an acceptance test for low-speed
 * pads, and because it is a shape real pads have: the d-pad is on ABS_X / ABS_Y (0 / 127 / 255) while the descriptor ALSO declares a hat that the
 * hardware never moves, 12 buttons of which the hardware has 8 (Linux buttons 1-6, 9, 10), and a vendor byte.
 *
 *   A  descriptor-only table (no database): what the pad does before the user touches anything
 *   B  Define buttons the way MiSTer's flow asks (RIGHT LEFT DOWN UP, A B X Y, L R, Select Start; the declared-but-absent buttons are skipped):
 *      every held state decodes to the right raw bit, the .dec file reloads to the same table, one SDL line is written and builds the same decode
 *   C  the database shortcut: an SDL line for 0079:0006 from the SDL gamecontrollerdb (the copy in MAME's bgfx examples/runtime/gamecontrollerdb.txt,
 *      line 130, quoted below), found through bcdHID 0x0110 (the evdev version MiSTer compares) although bcdDevice is 0x0107
 *   D  the same sequence with a counter byte and axis wobble the real pad may have (the real raw reports are unreadable without root)
 *
 *   make test_dragonrise && ./test_dragonrise
 */
#define main test_padmgr_main
#include "test_padmgr.c"
#undef main
#include "real_pads.h"

/* the SDL line (platform:Linux) that the SDL database has for a DragonRise pad of this vid:pid and HID version 0x0110 */
#define SDL_DR_LINE "03000000790000000600000010010000,DragonRise Inc.   Generic   USB  Joystick  ,x:b3,a:b2,b:b1,y:b0,back:b8,start:b9,dpleft:h0.8,dpdown:h0.4,dpright:h0.2,dpup:h0.1,leftshoulder:b4,lefttrigger:b6,rightshoulder:b5,righttrigger:b7,leftstick:b10,rightstick:b11,leftx:a0,lefty:a3,rightx:a1,righty:a4,platform:Linux,"

/* the pad's reports: idle, d-pad on X / Y, buttons 1..12 (usage n = bit 44 + n - 1) */
static void dr_report(uint8_t *r, int x, int y, int button /* 1..12, 0 none */)
{
    memcpy(r, RP_LS_IDLE, RP_LS_IDLE_LEN);
    if (x >= 0) r[0] = (uint8_t)x;
    if (y >= 0) r[1] = (uint8_t)y;
    if (button >= 1 && button <= 12) { int bit = 44 + button - 1; r[bit >> 3] |= (uint8_t)(1u << (bit & 7)); }
}

typedef struct { rig_t *R; int t, counter, press_from, press_to; int x, y, button; int noisy; } drclk_t;
static void dr_hook(void *c, unsigned ms)
{
    drclk_t *k = c; k->t += (int)ms; k->counter++;
    uint8_t r[8]; int on = k->t >= k->press_from && k->t < k->press_to;
    dr_report(r, on ? k->x : -1, on ? k->y : -1, on ? k->button : 0);
    if (k->noisy) { r[7] = (uint8_t)(k->counter * 29); r[0] = (uint8_t)(r[0] + ((k->counter >> 1) & 1) * (on ? 0 : 2)); r[1] = (uint8_t)(r[1] - ((k->counter >> 2) & 1) * (on ? 0 : 2)); }
    fpga_send_report(&k->R->fp, 0, r, 8);
}
typedef struct { drclk_t *K; padmgr_cap_t *C; struct { int x, y, b; } held[32]; } drcap_t;
static int dr_cap(void *c, int phase, pl_sample_t *out, int max)
{
    drcap_t *P = c; drclk_t *K = P->K;
    if (PLC_KIND(phase) == PLC_HELD) {
        int rw = PLC_RW(phase);
        K->x = P->held[rw].x; K->y = P->held[rw].y; K->button = P->held[rw].b;
        K->press_from = K->t + 400; K->press_to = K->t + 750;
    }
    return padmgr_capture(P->C, phase, out, max);
}

/* the expected raw bit of every physical control, and a probe: press it alone, read the FPGA model's decode */
typedef struct { const char *name; int x, y, b; int rw; } ctl_t;
static const ctl_t PHYS[] = {
    { "d-pad RIGHT (X=255)", 255, -1, 0, RW_RIGHT }, { "d-pad LEFT (X=0)", 0, -1, 0, RW_LEFT }, { "d-pad DOWN (Y=255)", -1, 255, 0, RW_DOWN }, { "d-pad UP (Y=0)", -1, 0, 0, RW_UP },
    { "button 1 (BTN_TRIGGER)", -1, -1, 1, RW_EAST }, { "button 2", -1, -1, 2, RW_SOUTH }, { "button 3", -1, -1, 3, RW_NORTH }, { "button 4", -1, -1, 4, RW_WEST },
    { "button 5", -1, -1, 5, RW_L1 }, { "button 6", -1, -1, 6, RW_R1 }, { "button 9 (BTN_BASE3)", -1, -1, 9, RW_BACK }, { "button 10 (BTN_BASE4)", -1, -1, 10, RW_START },
};

static int dr_probe(const pd_table_t *T, const ctl_t *c, uint32_t *raw)
{
    uint8_t r[8]; dr_report(r, c->x, c->y, c->b); *raw = 0; return pd_eval(T, r, 8, raw);
}

static void test_all(int noisy)
{
    printf("-- DragonRise 0079:0006 (low speed, real descriptors)%s\n", noisy ? ", with a counter byte and rest wobble" : "");
    static rig_t R; static hp_layout_t HL; static plsess_t S; static cardf_t F; static drclk_t K; static padmgr_cap_t C; static drcap_t P; char msg[40], sdl[700];

    /* ---- parse */
    CHECK(hp_parse(RP_LS_RPT, sizeof RP_LS_RPT, &HL) == 1 && HL.nbtn == 12 && HL.naxis == 4 && HL.nhat == 1 && HL.rid == 0 && HL.bytes == 8,
          "the real descriptor parses: %d buttons %d axes %d hat, report id %d, %d bytes", HL.nbtn, HL.naxis, HL.nhat, HL.rid, HL.bytes);
    { const hp_field_t *fx = &HL.f[HL.aidx[0]], *fy = &HL.f[HL.aidx[1]], *fz = &HL.f[HL.aidx[2]], *frz = &HL.f[HL.aidx[3]], *fh = &HL.f[HL.hidx[0]], *fb1 = &HL.f[HL.bidx[0]], *fb12 = &HL.f[HL.bidx[11]];
      CHECK(fx->bit == 0 && fy->bit == 8 && fz->bit == 24 && frz->bit == 32 && fx->size == 8 && fx->lmax == 255, "axes a0..a3 = X Y Z Rz at bits 0 8 24 32, 8 bits");
      CHECK(fh->bit == 40 && fh->size == 4 && fh->lmin == 0 && fh->lmax == 7, "the hat: bits 40-43, 0..7 (null outside)");
      CHECK(fb1->bit == 44 && fb12->bit == 55, "buttons 1..12 at bits 44..55 (the high nibble of byte 5, then byte 6)"); }

    /* ---- A0: the compiled-in table (no card file): it has a line for exactly this vid:pid:bcdDevice (Companion's "USB Gamepad", version 0x0107) */
    { rig_start(&R, 0, 0); padmgr_core_start(R.m);
      plug(&R, 0, 0x0079, 0x0006, 0x0107, 3, 0, 0, RP_LS_RPT, sizeof RP_LS_RPT, 1);
      uint8_t id[8]; dr_report(id, -1, -1, 0); fpga_send_report(&R.fp, 0, id, 8); snap(&R, 0);
      int src = padmgr_port_src(R.m, 0); uint32_t raw; int dp = 0, ok = 0;
      for (unsigned k = 0; k < sizeof PHYS / sizeof PHYS[0]; k++) { dr_probe(&R.fp.p[0].tab, &PHYS[k], &raw); if (PHYS[k].rw <= RW_UP) { if (raw & 0xF) dp++; } else if (raw & (1u << PHYS[k].rw)) ok++; }
      printf("   A0: built-in line (src %s): the d-pad of this pad gives %d of 4 directions (its line has the d-pad on a hat); %d of 8 buttons on their MiSTer-order bit\n", PRS_NAME_FOR(src), dp, ok);
      CHECK(src == PRS_BUILTIN && dp == 0, "A0: the compiled-in line for 0079:0006 v0107 is used and leaves this pad's d-pad dead (src %d, %d directions)", src, dp);
      rig_done(&R); }
    /* ---- A: no database: the descriptor alone */
    g_no_builtin = 1;
    rig_start(&R, 0, 0); padmgr_core_start(R.m); memset(&F, 0, sizeof F);
    plug(&R, 0, 0x0079, 0x0006, 0x0107, 3, 0, 0, RP_LS_RPT, sizeof RP_LS_RPT, 1);
    { uint8_t id[8]; dr_report(id, -1, -1, 0); fpga_send_report(&R.fp, 0, id, 8); snap(&R, 0); }
    CHECK(padmgr_port_src(R.m, 0) == PRS_HEURISTIC, "A: no database line: descriptor only (src %d)", padmgr_port_src(R.m, 0));
    { uint32_t raw; uint8_t id[8]; dr_report(id, -1, -1, 0); raw = 0; pd_eval(&R.fp.p[0].tab, id, 8, &raw);
      CHECK(raw == 0, "A: nothing pressed: raw %08x", raw);
      int dpad_dead = 0, stick = 0, btn_ok = 0;
      for (unsigned k = 0; k < sizeof PHYS / sizeof PHYS[0]; k++) {
          dr_probe(&R.fp.p[0].tab, &PHYS[k], &raw);
          if (PHYS[k].rw <= RW_UP && !(raw & 0xF)) dpad_dead++;
          if (PHYS[k].rw <= RW_UP && (raw & 0x3C0000)) stick++;
          if (PHYS[k].rw > RW_UP && (raw & (1u << PHYS[k].rw))) btn_ok++;
      }
      printf("   A: with the descriptor only the real d-pad (X / Y) reads as the LEFT STICK (%d of 4 directions are stick bits), the declared hat is dead (%d of 4 d-pad bits missing); %d of 8 physical buttons already land on their MiSTer-order bit\n", stick, dpad_dead, btn_ok);
      CHECK(stick == 4 && dpad_dead == 4, "A: the guess puts X / Y on the stick (expected 4/4, got %d/%d): Define buttons is what makes this d-pad a d-pad", stick, dpad_dead); }

    g_no_builtin = 0;
    /* ---- B: Define, the way MiSTer asks */
    memset(&K, 0, sizeof K); K.R = &R; K.noisy = noisy; K.x = K.y = -1;
    R.e.rl.delay_hook = dr_hook; R.e.rl.delay_ctx = &K;
    memset(&C, 0, sizeof C); C.m = R.m; C.port = 0; C.L = &HL; C.press_timeout_ms = 900;
    memset(&P, 0, sizeof P); P.K = &K; P.C = &C;
    for (int i = 0; i < 32; i++) { P.held[i].x = P.held[i].y = -1; }
    for (unsigned k = 0; k < sizeof PHYS / sizeof PHYS[0]; k++) { P.held[PHYS[k].rw].x = PHYS[k].x; P.held[PHYS[k].rw].y = PHYS[k].y; P.held[PHYS[k].rw].b = PHYS[k].b; }
    { uint8_t id[8]; dr_report(id, -1, -1, 0); fpga_send_report(&R.fp, 0, id, 8); }
    plsess_begin(&S, 0x0079, 0x0006, 0x0107, &HL, "PC TWIN SHOCK Gamepad");
    CHECK(plsess_baseline(&S, dr_cap, &P) == PLS_OK, "B: baseline");
    int nok = 0, nskip = 0, nfail = 0;
    for (int i = 0; i < plsess_default_order_n; i++) {
        const plsess_item_t *it = &plsess_default_order[i];
        int has = 0; for (unsigned k = 0; k < sizeof PHYS / sizeof PHYS[0]; k++) if (PHYS[k].rw == it->rw) has = 1;
        if (it->kind == PLI_DPAD) {
            if (it->rw != RW_RIGHT) continue;
            int rc = plsess_dpad(&S, dr_cap, &P, msg, sizeof msg);
            CHECK(rc == PLS_OK, "B: the four d-pad directions (X / Y axes): rc %d \"%s\"", rc, msg);
            if (rc == PLS_OK) nok += 4; else nfail += 4;
            continue;
        }
        if (!has) { plsess_skip(&S, it->rw); nskip++; continue; }                 /* L2 R2 L3 R3 Guide, the sticks: the pad has no such control */
        int rc = plsess_step(&S, it->rw, dr_cap, &P, msg, sizeof msg);
        CHECK(rc == PLS_OK, "B: %s: rc %d \"%s\"", it->label, rc, msg);
        if (rc == PLS_OK) nok++; else nfail++;
    }
    R.e.rl.delay_hook = NULL;
    printf("   B: %d inputs learned, %d skipped (no such control), %d failed\n", nok, nskip, nfail);
    plsess_files_t files = cf_table(&F);
    int fl = plsess_finish(&S, R.m, 0, &files, sdl, sizeof sdl);
    CHECK((fl & (PLF_APPLIED | PLF_DEC | PLF_SDL)) == (PLF_APPLIED | PLF_DEC | PLF_SDL), "B: applied, .dec and SDL stored (flags %d)", fl);
    CHECK(R.fp.p[0].mode == PDM_TABLE && padmgr_port_src(R.m, 0) == PRS_USER_DEC, "B: the FPGA model holds the user's table");
    { int bad = 0; uint32_t raw;
      uint8_t id[8]; dr_report(id, -1, -1, 0); raw = 0; pd_eval(&R.fp.p[0].tab, id, 8, &raw); if (raw) { bad++; printf("   rest: raw %08x\n", raw); }
      for (unsigned k = 0; k < sizeof PHYS / sizeof PHYS[0]; k++) {
          dr_probe(&R.fp.p[0].tab, &PHYS[k], &raw);
          if (raw != (1u << PHYS[k].rw)) { bad++; printf("   %s: raw %08x (want %08x)\n", PHYS[k].name, raw, 1u << PHYS[k].rw); }
      }
      /* the buttons the hardware does not have but the descriptor declares (7, 8, 11, 12), and the hat nibble value 8 (null), 0xF: nothing */
      for (int b = 7; b <= 12; b++) if (b == 7 || b == 8 || b == 11 || b == 12) { uint8_t r[8]; dr_report(r, -1, -1, b); raw = 0; pd_eval(&R.fp.p[0].tab, r, 8, &raw); if (raw) { bad++; printf("   declared-but-absent button %d reads raw %08x\n", b, raw); } }
      { uint8_t r[8]; dr_report(r, -1, -1, 0); r[5] = (uint8_t)((r[5] & 0xF0) | 0x08); raw = 0; pd_eval(&R.fp.p[0].tab, r, 8, &raw); if (raw) { bad++; printf("   hat null 8 reads raw %08x\n", raw); } }
      { uint8_t r[8]; dr_report(r, 100, 140, 0); raw = 0; pd_eval(&R.fp.p[0].tab, r, 8, &raw); if (raw) { bad++; printf("   X=100 Y=140 (inside the dead zone) reads raw %08x\n", raw); } }
      { uint8_t r[8]; dr_report(r, 255, 0, 5); raw = 0; pd_eval(&R.fp.p[0].tab, r, 8, &raw); if (raw != ((1u << RW_RIGHT) | (1u << RW_UP) | (1u << RW_L1))) { bad++; printf("   right+up+button5 reads raw %08x\n", raw); } }
      CHECK(bad == 0, "B: every held state of the real pad decodes to its bit, absent buttons and the dead zone read 0 (%d wrong)", bad); }
    { char nm[64]; pd_table_t back; pl_dec_name(0x0079, 0x0006, 0x0107, nm, sizeof nm);
      CHECK(!strcmp(F.dec_name, nm) && pl_dec_load(F.dec, F.dec_n, 0x0079, 0x0006, 0x0107, &back) && back.count == R.fp.p[0].tab.count && !memcmp(back.e, R.fp.p[0].tab.e, (size_t)back.count * sizeof back.e[0]),
            "B: the .dec file (%s, %d bytes) reloads to the identical table", F.dec_name, F.dec_n);
      printf("   B: SDL line written to the user file: %s\n", sdl);
      sdl_map_t m; CHECK(sdl_parse_line(sdl, &m), "B: the line parses");
      pd_table_t T2; hp_layout_t L2 = HL; pd_build_info_t info;
      int rc = pd_from_sdl(&L2, &m, &T2, &info);
      CHECK(rc, "B: the line builds a table against the descriptor");
      if (rc) { int bad = 0; for (unsigned k = 0; k < sizeof PHYS / sizeof PHYS[0]; k++) { uint32_t raw; dr_probe(&T2, &PHYS[k], &raw); if (!(raw & (1u << PHYS[k].rw))) { bad++; printf("   the line's table misses %s\n", PHYS[k].name); } }
                CHECK(bad == 0, "B: the SDL line alone gives the same decode for every control (%d missing)", bad); } }
    rig_done(&R);

    /* ---- C: the database shortcut, Linux line found through bcdHID */
    static rig_t R2; rig_start(&R2, 0, 0); padmgr_core_start(R2.m);
    R2.txt[1] = SDL_DR_LINE "\n";                                               /* the card's gamecontrollerdb.txt */
    plug(&R2, 0, 0x0079, 0x0006, 0x0107, 3, 0, 0, RP_LS_RPT, sizeof RP_LS_RPT, 1);
    { uint8_t id[8]; dr_report(id, -1, -1, 0); fpga_send_report(&R2.fp, 0, id, 8);
      /* the configuration descriptor the FPGA stored must carry the real HID descriptor so the manager can read bcdHID: use the real 41 bytes */
      memcpy(R2.fp.p[0].cfg, RP_LS_CFG, sizeof RP_LS_CFG); R2.fp.p[0].cfglen = sizeof RP_LS_CFG; snap(&R2, 0); }
    printf("   C: card line for 0079:0006 version 0x0110, pad bcdDevice 0x0107, bcdHID 0x%04x: src %s (%s)\n", RP_LS_CFG[20] | RP_LS_CFG[21] << 8,
           PRS_NAME_FOR(padmgr_port_src(R2.m, 0)), padmgr_port_src(R2.m, 0) == PRS_DB ? "found" : "not used");
    CHECK(padmgr_port_src(R2.m, 0) == PRS_DB, "C: the card's Linux line (version 0x0110 = bcdHID) is the exact match although bcdDevice is 0x0107 (src %d)", padmgr_port_src(R2.m, 0));
    if (padmgr_port_src(R2.m, 0) == PRS_DB) {
        int w = 0; uint32_t raw;
        for (unsigned k = 0; k < sizeof PHYS / sizeof PHYS[0]; k++) { dr_probe(&R2.fp.p[0].tab, &PHYS[k], &raw); if (!(raw & (1u << PHYS[k].rw))) w++; }
        printf("   C: with that line %d of the pad's 12 physical controls land on the bit MiSTer-order Define would give them; the line is for a DragonRise whose d-pad is a hat, so this pad's X / Y d-pad does not follow it (honest limit of a database shortcut: Define buttons fixes it)\n", w);
    }
    rig_done(&R2);
}

int main(void)
{
    test_all(0);
    test_all(1);
    printf("%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
