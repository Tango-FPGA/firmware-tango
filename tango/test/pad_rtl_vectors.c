// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 the Tango authors
/*
 * pad_rtl_vectors.c -- the vectors for the full-chain pad bench (menu-tango fpga/sim/tb_pads.inc, run_tb_pads.sh): for one device profile of
 * menu-tango fpga/sim/tb_devs.inc it writes the decode table the firmware would send (SET_DECODE payload) and reports the device will send, each with
 * the raw word the C reference (pd_eval) says the FPGA must publish.  Deterministic.
 *
 *   ./pad_rtl_vectors <profile> > menu-tango fpga/sim/pads/vec_<profile>.txt
 *
 * Format:  PROFILE <name> / TABLE <mode> <rid_en> <rid_val> <min_len> <count> / WIRE <hex of the SET_DECODE payload> /
 *          REPORT <len> <hex bytes> <accepted 0/1> <expected raw, 8 hex>   (raw of a rejected report = the previous raw)
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include "../tango_hidparse.h"
#include "../tango_paddb.h"
#include "../tango_paddec.h"
#include "pad_fixtures.h"
#include "real_pads.h"

static uint32_t rng_s = 0xBEEF1234u;
static uint32_t rnd(void) { rng_s ^= rng_s << 13; rng_s ^= rng_s >> 17; rng_s ^= rng_s << 5; return rng_s; }

static pd_table_t T;
static uint32_t cur_raw = 0;
static int minlen_seen = 1000;

static void emit(const uint8_t *r, int len)
{
    uint32_t raw = cur_raw;
    int acc = pd_eval(&T, r, len, &raw);
    if (acc) cur_raw = raw;
    printf("REPORT %d ", len);
    for (int i = 0; i < len; i++) printf("%02x", r[i]);
    printf(" %d %08x\n", acc, cur_raw);
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s <profile>\n", argv[0]); return 2; }
    const char *name = argv[1];
    int len = 0; uint8_t idle[64]; memset(idle, 0, sizeof idle);
    const uint8_t *desc = NULL; int dn = 0; int kind = 0;       /* kind 0: layout table, 1 ds4 builtin, 2 xinput, 3 quiet */
    if (!strcmp(name, "ds4"))        { kind = 1; len = 64; idle[0] = 1; memset(idle + 1, 0x80, 4); idle[5] = 0x08; }
    else if (!strcmp(name, "retro5")) { desc = D_RETRO5; dn = sizeof D_RETRO5; len = 7; memset(idle, 0x80, 5); }
    else if (!strcmp(name, "snes4"))  { desc = D_SNES12; dn = sizeof D_SNES12; len = 4; idle[0] = idle[1] = 0x7F; }
    else if (!strcmp(name, "hat12") || !strcmp(name, "composite") || !strcmp(name, "kbdfirst")) {
        desc = D_HAT12; dn = sizeof D_HAT12; len = 6; memset(idle, 0x80, 4); idle[4] = 0x0F; }
    else if (!strcmp(name, "dragonrise")) { desc = RP_LS_RPT; dn = sizeof RP_LS_RPT; len = RP_LS_IDLE_LEN; memcpy(idle, RP_LS_IDLE, len); }
    else if (!strcmp(name, "xin360")) { kind = 2; len = 20; idle[1] = 0x14; }
    else if (!strcmp(name, "mouse"))  { kind = 3; len = 4; }
    else { fprintf(stderr, "unknown profile %s\n", name); return 2; }

    memset(&T, 0, sizeof T);
    if (kind == 1) pd_builtin_ds4(&T);
    else if (kind == 2) pd_builtin_xinput(&T);
    else if (kind == 3) pd_quiet(&T);
    else {
        hp_layout_t L; pd_build_info_t info;
        if (!hp_parse(desc, dn, &L) || !pd_from_layout(&L, &T, &info)) { fprintf(stderr, "no table for %s\n", name); return 1; }
    }
    uint8_t wire[512]; int wn = pd_wire(&T, wire, sizeof wire);
    printf("PROFILE %s\nTABLE %d %d %d %d %d\nWIRE ", name, T.mode, T.rid_en, T.rid_val, T.min_len, T.count);
    for (int i = 0; i < wn; i++) printf("%02x", wire[i]);
    printf("\n");

    uint8_t r[64], r0[64];
    emit(idle, len);                                                  /* the idle report first: the pad arms on it */
    if (!strcmp(name, "dragonrise")) {
        /* the pad's real held states: d-pad on X / Y, buttons 1-6, 9, 10 (hardware) and 7, 8, 11, 12 (declared, never sent by the real pad), hat null 0x8 as well as 0xF */
        for (int k = 0; k < 4; k++) { memcpy(r0, idle, 8); r0[k >> 1] = (k & 1) ? 0xFF : 0x00; emit(r0, 8); }
        for (int b = 0; b < 12; b++) { memcpy(r0, idle, 8); r0[b < 4 ? 5 : 6] |= (uint8_t)(1u << (b < 4 ? 4 + b : b - 4)); emit(r0, 8); }
        memcpy(r0, idle, 8); r0[5] = 0x08; emit(r0, 8);
    }
    for (int n = 0; n < 60; n++) {
        memcpy(r, idle, sizeof r);
        int mode = n % 4;
        if (mode == 0) { for (int i = 0; i < len; i++) r[i] = (uint8_t)rnd(); }
        else if (mode == 1) { for (int i = 0; i < len; i++) if (rnd() & 1) r[i] = (uint8_t)rnd(); }      /* some bytes changed */
        else if (mode == 2) { r[rnd() % len] ^= (uint8_t)(1u << (rnd() % 8)); }                          /* one bit */
        else { for (int i = 0; i < len; i++) r[i] = (uint8_t)(idle[i] ^ ((rnd() % 3 == 0) ? (rnd() & 0xFF) : 0)); }
        if (kind == 2) { r[0] = (n % 7 == 0) ? 1 : 0; r[1] = 0x14; }                                      /* some with the wrong message type: rejected */
        if (kind == 1) { r[0] = 1; if ((r[5] & 0x0F) > 8) r[5] = (uint8_t)((r[5] & 0xF0) | 8); }
        emit(r, len);
        if (n % 9 == 5 && T.min_len > 1) { emit(r, T.min_len - 1); minlen_seen = 0; }                      /* too short: rejected */
        if (n % 10 == 3) emit(idle, len);                                                                  /* back to rest */
    }
    emit(idle, len);
    printf("END\n");
    (void)minlen_seen;
    return 0;
}
