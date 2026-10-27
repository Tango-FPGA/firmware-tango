// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 the Tango authors
/*
 * test_paddb.c -- host tests for the controller database: HID descriptor parser, SDL lines, lookup ladder, decode tables.
 * No SDK, no hardware.  make test_paddb
 *
 * Expected values are written by hand from the fixtures' bit layouts (pad_fixtures.h comments) BEFORE the code ran; where an independent oracle is used (the RTL decode of tango_ds4_joy.v, a direct-arithmetic SDL evaluator) it is
 * written from the specification of the thing, not from the code under test.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include "../tango_hidparse.h"
#include "../tango_paddb.h"
#include "../tango_paddec.h"
#include "../tango_padres.h"
#include "../tango_paddb_builtin.h"
#include "pad_fixtures.h"

/* the whole built-in table, for the tests (a separate copy; the firmware has one) */
#define CONTROLLERDB_ATTR
#define CONTROLLERDB_STR_ATTR
#include "../../companion/src/usb_controller_maps.h"

static int fails = 0, checks = 0;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint32_t rng_s = 0x1234567u;
static uint32_t rnd(void) { rng_s ^= rng_s << 13; rng_s ^= rng_s >> 17; rng_s ^= rng_s << 5; return rng_s; }

static void setbits(uint8_t *r, int bit, int size, uint32_t v)
{
    for (int i = 0; i < size; i++) {
        int b = bit + i;
        if ((v >> i) & 1) r[b / 8] |= (uint8_t)(1 << (b % 8)); else r[b / 8] &= (uint8_t)~(1 << (b % 8));
    }
}

/* ======================================================================= the RTL decode, as a C oracle (tango_ds4_joy.v 73-95) */
static uint32_t ds4_rw(const uint8_t *r)
{
    uint8_t b5 = r[5], b6 = r[6], b7 = r[7];
    int up = 0, down = 0, left = 0, right = 0;
    switch (b5 & 0xF) {
    case 0: up = 1; break;
    case 1: up = 1; right = 1; break;
    case 2: right = 1; break;
    case 3: down = 1; right = 1; break;
    case 4: down = 1; break;
    case 5: down = 1; left = 1; break;
    case 6: left = 1; break;
    case 7: up = 1; left = 1; break;
    default: break;
    }
    uint32_t rw = 0;
    rw |= (uint32_t)right | (uint32_t)left << 1 | (uint32_t)down << 2 | (uint32_t)up << 3;
    rw |= (uint32_t)((b5 >> 5) & 1) << 4 | (uint32_t)((b5 >> 6) & 1) << 5 | (uint32_t)((b5 >> 4) & 1) << 6 | (uint32_t)((b5 >> 7) & 1) << 7;
    rw |= (uint32_t)b6 << 8;                                   /* L1 R1 L2 R2 share options L3 R3 are b6[0..7] -> 8..15 */
    rw |= (uint32_t)(b7 & 1) << 16 | (uint32_t)((b7 >> 1) & 1) << 17;
    return rw;
}

/* ======================================================================= parser */
static void test_parser(void)
{
    printf("-- parser\n");
    static hp_layout_t L;

    CHECK(hp_parse(D_DS4, sizeof D_DS4, &L) == 1, "ds4 parse");
    CHECK(L.cls == HPC_GAMEPAD && L.app_usage == 5, "ds4 class");
    CHECK(L.rid == 1 && L.bytes == 64, "ds4: report id %d, %d bytes (want 1, 64)", L.rid, L.bytes);
    CHECK(L.nbtn == 14 && L.naxis == 6 && L.nhat == 1, "ds4: %d buttons %d axes %d hats (want 14 6 1)", L.nbtn, L.naxis, L.nhat);
    for (int k = 0; k < 14; k++) CHECK(L.f[L.bidx[k]].bit == 44 + k, "ds4 b%d at bit %d (want %d)", k, L.f[L.bidx[k]].bit, 44 + k);
    /* axes in evdev order: X Y Z Rx Ry Rz = report order X Y Z (Rz) (Rx Ry): bits 8 16 24 64 72 32 */
    static const int AX[6] = { 8, 16, 24, 64, 72, 32 };
    for (int k = 0; k < 6; k++) CHECK(L.f[L.aidx[k]].bit == AX[k], "ds4 a%d at bit %d (want %d)", k, L.f[L.aidx[k]].bit, AX[k]);
    CHECK(L.f[L.hidx[0]].bit == 40 && L.f[L.hidx[0]].size == 4 && L.f[L.hidx[0]].lmax == 7, "ds4 hat");

    CHECK(hp_parse(D_SNES12, sizeof D_SNES12, &L) == 1 && L.rid == 0 && L.bytes == 4 && L.nbtn == 12 && L.naxis == 2 && L.nhat == 0,
          "snes12: rid %d bytes %d btn %d axes %d hat %d", L.rid, L.bytes, L.nbtn, L.naxis, L.nhat);
    CHECK(L.f[L.aidx[0]].bit == 0 && L.f[L.aidx[1]].bit == 8 && L.f[L.bidx[0]].bit == 16 && L.f[L.bidx[11]].bit == 27, "snes12 offsets");
    CHECK(L.app_usage == 4, "snes12 is a Joystick application");

    CHECK(hp_parse(D_HAT12, sizeof D_HAT12, &L) == 1 && L.bytes == 6 && L.nbtn == 12 && L.naxis == 4 && L.nhat == 1, "hat12");
    CHECK(L.f[L.hidx[0]].bit == 32 && L.f[L.bidx[0]].bit == 36 && L.f[L.bidx[11]].bit == 47, "hat12 offsets");
    /* axes X Y Z Rz = evdev 0 1 2 5: a0..a3 are those four */
    CHECK(L.f[L.aidx[3]].usage == 0x35, "hat12: a3 is Rz (usage %x)", L.f[L.aidx[3]].usage);

    CHECK(hp_parse(D_XBOXHID, sizeof D_XBOXHID, &L) == 1 && L.bytes == 14 && L.nbtn == 10 && L.naxis == 6 && L.nhat == 1,
          "xboxhid: bytes %d btn %d axes %d hat %d", L.bytes, L.nbtn, L.naxis, L.nhat);
    /* report order LX LY Rx Ry Z Rz; evdev order X Y Z Rx Ry Rz -> a0 LX(0) a1 LY(16) a2 Z(64) a3 Rx(32) a4 Ry(48) a5 Rz(74) */
    static const int XA[6] = { 0, 16, 64, 32, 48, 74 };
    for (int k = 0; k < 6; k++) CHECK(L.f[L.aidx[k]].bit == XA[k], "xboxhid a%d at bit %d (want %d)", k, L.f[L.aidx[k]].bit, XA[k]);
    CHECK(L.f[L.aidx[0]].sgn && L.f[L.aidx[0]].size == 16 && L.f[L.aidx[0]].lmin == -32768 && L.f[L.aidx[0]].lmax == 32767, "xboxhid LX range");
    CHECK(!L.f[L.aidx[2]].sgn && L.f[L.aidx[2]].size == 10 && L.f[L.aidx[2]].lmax == 1023, "xboxhid Z 10 bit");
    CHECK(L.f[L.hidx[0]].bit == 88 && L.f[L.hidx[0]].lmin == 1 && L.f[L.bidx[0]].bit == 96 && L.f[L.bidx[9]].bit == 105, "xboxhid hat/buttons");

    CHECK(hp_parse(D_TWOID, sizeof D_TWOID, &L) == 1 && L.rid == 3 && L.bytes == 4 && L.nbtn == 8 && L.nrid_other == 1,
          "twoid: rid %d bytes %d btn %d other %d", L.rid, L.bytes, L.nbtn, L.nrid_other);
    CHECK(L.f[L.aidx[0]].bit == 8 && L.f[L.bidx[0]].bit == 24, "twoid offsets (the id is byte 0)");

    CHECK(hp_parse(D_MOUSE, sizeof D_MOUSE, &L) == 0 && L.cls == HPC_MOUSE, "mouse is rejected (cls %d)", L.cls);
    CHECK(hp_parse(D_KBD, sizeof D_KBD, &L) == 0 && L.cls == HPC_KEYBOARD, "keyboard is rejected (cls %d)", L.cls);
    CHECK(hp_parse(D_CONSUMER, sizeof D_CONSUMER, &L) == 0 && L.cls == HPC_OTHER, "consumer control is rejected (cls %d)", L.cls);

    CHECK(hp_parse(D_HAT4, sizeof D_HAT4, &L) == 1 && L.nhat == 1 && L.naxis == 0 && L.nbtn == 8 && L.f[L.bidx[0]].bit == 8, "hat4");

    /* the evdev order is not the descriptor order: select/start sit between buttons 4 and 5, the d-pad buttons come last */
    CHECK(hp_parse(D_DPADBTN, sizeof D_DPADBTN, &L) == 1 && L.nbtn == 12, "dpadbtn: %d buttons", L.nbtn);
    CHECK(L.f[L.bidx[0]].bit == 4 && L.f[L.bidx[3]].bit == 7 && L.f[L.bidx[4]].bit == 10 && L.f[L.bidx[5]].bit == 11, "dpadbtn: b0-b5 = buttons 1-6");
    CHECK(L.f[L.bidx[6]].bit == 8 && L.f[L.bidx[7]].bit == 9, "dpadbtn: b6 = select (bit 8), b7 = start (bit 9): got %d %d", L.f[L.bidx[6]].bit, L.f[L.bidx[7]].bit);
    CHECK(L.f[L.bidx[8]].bit == 0 && L.f[L.bidx[9]].bit == 1 && L.f[L.bidx[10]].bit == 3 && L.f[L.bidx[11]].bit == 2,
          "dpadbtn: b8 up, b9 down, b10 left (bit 3), b11 right (bit 2): got %d %d %d %d", L.f[L.bidx[8]].bit, L.f[L.bidx[9]].bit, L.f[L.bidx[10]].bit, L.f[L.bidx[11]].bit);

    CHECK(hp_parse(D_UNIVERSAL, sizeof D_UNIVERSAL, &L) == 1 && L.nbtn == 32 && L.naxis == 8 && L.nhat == 2, "universal");

    /* evdev order with codes from three ranges: usages 1-16, select/start (0x13a/0x13b) among them, d-pad buttons (0x220..), usages 17+ (0x2c0..) */
    CHECK(hp_parse(D_ORDER, sizeof D_ORDER, &L) == 1 && L.nbtn == 20, "order: %d buttons", L.nbtn);
    {   static const int WANT[20] = { 2, 3, 4, 5, 6, 7, 8, 9, 10, /* b0-b8 = buttons 1-9 */ 0, 1, /* select, start */ 11, 12, 13, /* buttons 14-16 */
                                      16, 17, 19, 18, /* d-pad up down left right (the descriptor has up down right left) */ 14, 15 /* buttons 17, 18 */ };
        for (int k = 0; k < 20; k++) CHECK(L.f[L.bidx[k]].bit == WANT[k], "order: b%d at bit %d (want %d)", k, L.f[L.bidx[k]].bit, WANT[k]);
    }

    /* robustness: truncated and garbage descriptors do not crash and are not pads */
    for (unsigned n = 0; n < sizeof D_DS4; n += 7) hp_parse(D_DS4, (int)n, &L);
    uint8_t junk[200]; for (int i = 0; i < 200; i++) junk[i] = (uint8_t)rnd();
    for (int k = 0; k < 500; k++) { for (int i = 0; i < 200; i++) junk[i] = (uint8_t)rnd(); hp_parse(junk, 1 + (int)(rnd() % 200), &L); }
    CHECK(hp_parse((const uint8_t *)"", 0, &L) == 0, "empty descriptor");
}

/* ======================================================================= SDL lines */
static const char DS4_LINE[] =
    "030000004c050000cc09000000000000,PS4 Controller,a:b1,b:b2,back:b8,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,dpup:h0.1,guide:b12,"
    "leftshoulder:b4,leftstick:b10,lefttrigger:a3,leftx:a0,lefty:a1,rightshoulder:b5,rightstick:b11,righttrigger:a4,rightx:a2,righty:a5,"
    "start:b9,x:b0,y:b3,platform:Linux,";

static void test_sdl(void)
{
    printf("-- SDL lines\n");
    sdl_map_t m;
    CHECK(sdl_parse_line(DS4_LINE, &m) == 1, "ds4 line");
    CHECK(m.bus == 3 && m.vid == 0x054c && m.pid == 0x09cc && m.has_ver && m.ver == 0 && m.plat == PL_LINUX, "ids %04x:%04x v%04x plat %d", m.vid, m.pid, m.ver, m.plat);
    CHECK(m.b[SN_A].type == SB_BTN && m.b[SN_A].idx == 1 && m.b[SN_X].idx == 0 && m.b[SN_START].idx == 9 && m.b[SN_GUIDE].idx == 12, "buttons");
    CHECK(m.b[SN_DPUP].type == SB_HAT && m.b[SN_DPUP].hatmask == 1 && m.b[SN_DPRIGHT].hatmask == 2 && m.b[SN_DPDOWN].hatmask == 4 && m.b[SN_DPLEFT].hatmask == 8, "hat dirs");
    CHECK(m.b[SN_LEFTX].type == SB_AXIS && m.b[SN_RIGHTY].idx == 5 && m.b[SN_LTRIGGER].idx == 3 && m.b[SN_RTRIGGER].idx == 4, "axes");
    CHECK(!strcmp(m.name, "PS4 Controller"), "name '%s'", m.name);

    /* every token form */
    CHECK(sdl_parse_line("03000000010000000200000003000000,T,a:b3,b:+a1,x:-a2,y:a3~,dpup:-a1~,dpleft:h1.8,leftx:a0~,platform:Linux,", &m) == 1, "forms");
    CHECK(m.b[SN_A].type == SB_BTN && m.b[SN_A].idx == 3, "b3");
    CHECK(m.b[SN_B].type == SB_AXIS_POS && m.b[SN_B].idx == 1 && !m.b[SN_B].inv, "+a1");
    CHECK(m.b[SN_X].type == SB_AXIS_NEG && m.b[SN_X].idx == 2, "-a2");
    CHECK(m.b[SN_Y].type == SB_AXIS && m.b[SN_Y].idx == 3 && m.b[SN_Y].inv, "a3~");
    CHECK(m.b[SN_DPUP].type == SB_AXIS_NEG && m.b[SN_DPUP].idx == 1 && m.b[SN_DPUP].inv, "-a1~");
    CHECK(m.b[SN_DPLEFT].type == SB_HAT && m.b[SN_DPLEFT].idx == 1 && m.b[SN_DPLEFT].hatmask == 8, "h1.8");
    CHECK(m.b[SN_LEFTX].inv, "leftx~");
    CHECK(m.vid == 0x0001 && m.pid == 0x0002 && m.ver == 0x0003, "guid 0300 0000 0100 0000 0200 0000 0300 0000 -> %04x:%04x v%04x", m.vid, m.pid, m.ver);

    /* MiSTer's extras and junk are read and ignored; a bad GUID, comments and blank lines are not mapping lines */
    CHECK(sdl_parse_line("030000004c050000cc09000000000000,X,a:b1,guide2:b3,menufunc:b1,menuok:b2,menuesc:b3,asysx:a4,paddle1:b15,crc:1234,platform:MiSTer,", &m) == 1 && m.nignored >= 6 && m.b[SN_A].idx == 1, "extras");
    CHECK(sdl_parse_line("# comment", &m) == 0 && sdl_parse_line("", &m) == 0 && sdl_parse_line("   ", &m) == 0, "comments");
    CHECK(sdl_parse_line("xinput,Xbox,a:b0,platform:Windows,", &m) == 0, "non-hex guid");
    CHECK(sdl_parse_line("0300,short,a:b0", &m) == 0, "guid too short");
    CHECK(sdl_parse_line("030000004c0500006802,no version,a:b0,platform:Linux,", &m) == 1 && !m.has_ver && m.vid == 0x054c && m.pid == 0x0268, "a 20 digit guid has no version");
    CHECK(sdl_parse_line("030000004c050000cc09000000000000,CRLF,a:b2,platform:Linux,\r\n", &m) == 1 && m.b[SN_A].idx == 2 && m.plat == PL_LINUX, "CRLF");
    CHECK(sdl_parse_line("030000004c050000cc09000000000000,N,a:b1,b:b999,x:q3,y:h0,platform:Linux,", &m) == 1 && m.b[SN_B].type == SB_NONE && m.b[SN_X].type == SB_NONE && m.b[SN_Y].type == SB_NONE, "bad tokens");

    /* the platform / core filter */
    sdl_parse_line("030000004c050000cc09000000000000,L,a:b1,platform:Linux,", &m);
    CHECK(sdl_applies(&m, "NES", 0), "Linux applies");
    sdl_parse_line("030000004c050000cc09000000000000,W,a:b1,platform:Windows,", &m);
    CHECK(!sdl_applies(&m, "NES", 1), "Windows never");
    sdl_parse_line("030000004c050000cc09000000000000,N,a:b1,", &m);
    CHECK(!sdl_applies(&m, "NES", 0) && sdl_applies(&m, "NES", 1), "no platform: only in the user file");
    sdl_parse_line("030000004c050000cc09000000000000,M,a:b1,platform:MiSTer,", &m);
    CHECK(sdl_applies(&m, "NES", 0), "MiSTer without a core filter applies everywhere");
    sdl_parse_line("030000004c050000cc09000000000000,M,a:b1,mistercore:SNES*,mistercore:Genesis,platform:MiSTer,", &m);
    CHECK(sdl_applies(&m, "SNES", 0) && sdl_applies(&m, "snes_Accuracy", 0) && sdl_applies(&m, "GENESIS", 0) && !sdl_applies(&m, "NES", 0) && !sdl_applies(&m, "TG16", 0),
          "mistercore patterns: prefix, case-insensitive");
}

/* ======================================================================= scan / tiers / cache */
static void test_scan(void)
{
    printf("-- lookup tiers\n");
    sdl_map_t m; pdb_scan_t s;
    const char *T =
        "# a comment\n"
        "030000004c0500006802000000000000,ver0,a:b0,platform:Linux,\n"       /* 054c:0268 v0000 */
        "030000004c0500006802000000010000,ver0100,a:b1,platform:Linux,\n"    /* v0100 */
        "030000004c0500006802000000020000,ver0200,a:b2,platform:Linux,\n"    /* v0200 */
        "030000004c0500006802000000010000,ver0100 later,a:b5,platform:Linux,\n"
        "030000004c0500006802000000010000,ver0100 windows,a:b7,platform:Windows,\n"
        "030000004c0500006902000000030000,other pid,a:b6,platform:Linux,\n";

    pdb_scan_begin(&s, 0x054c, 0x0268, 0x0100, "NES", 0); pdb_scan_text(&s, T);
    CHECK(pdb_scan_result(&s, &m) == PM_EXACT && m.b[SN_A].idx == 5, "exact, the LAST line wins (b%d)", m.b[SN_A].idx);
    pdb_scan_begin(&s, 0x054c, 0x0268, 0x0300, "NES", 0); pdb_scan_text(&s, T);
    CHECK(pdb_scan_result(&s, &m) == PM_VER0 && m.b[SN_A].idx == 0, "unknown version falls to the version-0 line (b%d)", m.b[SN_A].idx);
    pdb_scan_begin(&s, 0x054c, 0x0268, 0x0000, "NES", 0); pdb_scan_text(&s, T);
    CHECK(pdb_scan_result(&s, &m) == PM_EXACT && m.b[SN_A].idx == 0, "version 0 is an exact match for the version-0 line");
    pdb_scan_begin(&s, 0x054c, 0x0268, 0x0200, "NES", 0); pdb_scan_text(&s, T);
    CHECK(pdb_scan_result(&s, &m) == PM_EXACT && m.b[SN_A].idx == 2, "exact 0200");
    pdb_scan_begin(&s, 0x054c, 0x0268, 0x0777, "NES", 0); pdb_scan_text(&s, "030000004c0500006802,a guid without a version matches any version,a:b8,platform:Linux,\n");
    CHECK(pdb_scan_result(&s, &m) == PM_VER0 && m.b[SN_A].idx == 8, "a line without a version is a version-0 line");

    const char *T2 =   /* no version-0 line: any line for the vendor:product is better than nothing; the later one wins a tie */
        "030000004c0500006802000000010000,v0100,a:b1,platform:Linux,\n"
        "030000004c0500006802000000020000,v0200,a:b2,platform:Linux,\n";
    pdb_scan_begin(&s, 0x054c, 0x0268, 0x0900, "NES", 0); pdb_scan_text(&s, T2);
    CHECK(pdb_scan_result(&s, &m) == PM_VIDPID && m.b[SN_A].idx == 2, "vendor:product tier, later wins (b%d)", m.b[SN_A].idx);
    pdb_scan_begin(&s, 0x054c, 0x0777, 0x0000, "NES", 0); pdb_scan_text(&s, T2);
    CHECK(pdb_scan_result(&s, &m) == PM_NONE, "no line for the pid");

    /* kernel-driver numbering (version >= 0x8000) is for that exact version only */
    const char *T5 =
        "030000004c0500006802000011810000,driver numbered,a:b0,platform:Linux,\n";       /* v8111 */
    pdb_scan_begin(&s, 0x054c, 0x0268, 0x0300, "NES", 0); pdb_scan_text(&s, T5);
    CHECK(pdb_scan_result(&s, &m) == PM_NONE, "a 0x8111 line is not a fallback for another version");
    pdb_scan_begin(&s, 0x054c, 0x0268, 0x8111, "NES", 0); pdb_scan_text(&s, T5);
    CHECK(pdb_scan_result(&s, &m) == PM_EXACT, "...but matches its own version");
    {   const UsbGamepadMap *g = NULL;
        for (unsigned i = 0; i < kUsbGamepadMapsCount; i++) if (kUsbGamepadMaps[i].vid == 0x057e && kUsbGamepadMaps[i].pid == 0x201e) g = &kUsbGamepadMaps[i];
        CHECK(g && g->version == 0x8111, "057e:201e has only a 0x8111 entry");
        if (g) { CHECK(pdb_from_companion(g, 0x0100, &m) == PM_NONE, "built-in: the driver-numbered entry is not used for bcdDevice 0100");
                 CHECK(pdb_from_companion(g, 0x8111, &m) == PM_EXACT, "built-in: ...but for 8111"); }
    }

    /* a higher rank beats a later lower rank */
    const char *T3 =
        "030000004c0500006802000000010000,exact,a:b1,platform:Linux,\n"
        "030000004c0500006802000000000000,ver0 later,a:b9,platform:Linux,\n";
    pdb_scan_begin(&s, 0x054c, 0x0268, 0x0100, "NES", 0); pdb_scan_text(&s, T3);
    CHECK(pdb_scan_result(&s, &m) == PM_EXACT && m.b[SN_A].idx == 1, "exact beats a later version-0 line");

    /* MiSTer core-specific lines */
    const char *T4 =
        "030000004c0500006802000000000000,generic,a:b0,platform:Linux,\n"
        "030000004c0500006802000000000000,for snes,a:b4,mistercore:SNES*,platform:MiSTer,\n";
    pdb_scan_begin(&s, 0x054c, 0x0268, 0, "SNES", 0); pdb_scan_text(&s, T4); pdb_scan_result(&s, &m);
    CHECK(m.b[SN_A].idx == 4, "core line in the SNES core");
    pdb_scan_begin(&s, 0x054c, 0x0268, 0, "NES", 0); pdb_scan_text(&s, T4); pdb_scan_result(&s, &m);
    CHECK(m.b[SN_A].idx == 0, "core line not used in the NES core");

    /* the cache keeps negative answers */
    pdb_cache_t c; pdb_cache_clear(&c); int rk, src; sdl_map_t cm;
    CHECK(!pdb_cache_get(&c, 1, 2, 3, &rk, &src, &cm), "cold cache");
    pdb_cache_put(&c, 1, 2, 3, PM_NONE, 0, NULL);
    CHECK(pdb_cache_get(&c, 1, 2, 3, &rk, &src, &cm) && rk == PM_NONE, "negative answer cached");
    for (int i = 0; i < PDB_CACHE + 2; i++) pdb_cache_put(&c, 10, (uint16_t)i, 0, PM_VER0, 1, &m);
    CHECK(!pdb_cache_get(&c, 1, 2, 3, &rk, &src, &cm), "round-robin eviction");
}

/* ======================================================================= tables: DS4 anchor */
static void test_ds4_anchor(void)
{
    printf("-- DualShock 4 anchor\n");
    static hp_layout_t L; static pd_table_t t, bt;
    hp_parse(D_DS4, sizeof D_DS4, &L);
    sdl_map_t m;
    /* the real database entry (Companion's generated table, 054C:09CC version 0) and the same line as text must agree */
    const UsbGamepadMap *e = NULL;
    for (unsigned i = 0; i < kUsbGamepadMapsCount; i++) if (kUsbGamepadMaps[i].vid == 0x054C && kUsbGamepadMaps[i].pid == 0x09CC && kUsbGamepadMaps[i].version == 0) e = &kUsbGamepadMaps[i];
    CHECK(e != NULL, "built-in table has 054C:09CC v0");
    if (!e) return;
    CHECK(pdb_from_companion(e, 0, &m) == PM_EXACT, "rank");
    sdl_map_t tm; sdl_parse_line(DS4_LINE, &tm);
    int same = 1; for (int i = 0; i < SN_COUNT; i++) if (m.b[i].type != tm.b[i].type || m.b[i].idx != tm.b[i].idx || m.b[i].hatmask != tm.b[i].hatmask) same = 0;
    CHECK(same, "the built-in entry and the SDL text line (written from the same database) say the same");

    pd_build_info_t info;
    CHECK(pd_from_sdl(&L, &m, &t, &info) == 1, "builds (unresolved %d of %d)", info.nunres, info.nbind);
    CHECK(info.nunres == 0, "every DS4 binding resolves against the descriptor-derived numbering (%d unresolved)", info.nunres);
    CHECK(t.rid_en == 1 && t.rid_val == 1, "report id 1");
    char buf[2000]; pd_dump(&t, buf, sizeof buf); printf("   DS4 table: %s\n", buf);

    pd_builtin_ds4(&bt);
    long n = 0, bad = 0, bad2 = 0;
    for (int k = 0; k < 200000; k++) {
        uint8_t r[64]; for (int i = 0; i < 64; i++) r[i] = (uint8_t)rnd();
        r[0] = 1;
        if (k & 1) r[5] = (uint8_t)((r[5] & 0xF0) | (rnd() % 9));         /* real hat values: 0-7 and 8 = released */
        uint32_t want = ds4_rw(r), got = 0, got_b = 0;
        pd_eval(&t, r, 64, &got);
        pd_eval(&bt, r, 64, &got_b);
        n++;
        /* (1) the built-in DS4 table is bit-identical to the RTL decode */
        if (got_b != want) { if (bad++ < 3) printf("   built-in table differs: report5-7 %02x %02x %02x: %08x vs rtl %08x\n", r[5], r[6], r[7], got_b, want); }
        /* (2) the database-derived table: bits 0-9, 12-16 bit-identical; L2/R2 come from the analog triggers (Rx, Ry) at a quarter, not from the pad's own digital bit;
         *     bits 18-25 are the sticks (new) */
        uint32_t cmp_mask = 0x1F3FF;     /* bits 0-9, 12-16: the SDL line has no touchpad (17), and the triggers (10, 11) are analog */
        if ((got & cmp_mask) != (want & cmp_mask)) { if (bad2++ < 3) printf("   db table differs: %08x vs rtl %08x (mask %x)\n", got, want, cmp_mask); }
        uint32_t ex = 0;
        ex |= (uint32_t)(r[8] >= 63) << 10 | (uint32_t)(r[9] >= 63) << 11;
        ex |= (uint32_t)(r[1] >= 192) << 18 | (uint32_t)(r[1] <= 63) << 19 | (uint32_t)(r[2] >= 192) << 20 | (uint32_t)(r[2] <= 63) << 21;
        ex |= (uint32_t)(r[3] >= 192) << 22 | (uint32_t)(r[3] <= 63) << 23 | (uint32_t)(r[4] >= 192) << 24 | (uint32_t)(r[4] <= 63) << 25;
        uint32_t extra = got & ~cmp_mask;
        if (extra != ex) { if (bad2++ < 3) printf("   db table extras %08x want %08x\n", extra, ex); }
    }
    CHECK(bad == 0, "built-in DS4 table vs the RTL decode: %ld of %ld reports differ", bad, n);
    CHECK(bad2 == 0, "descriptor + database DS4 table vs the RTL decode (buttons, hat) and the analog rules: %ld of %ld differ", bad2, n);
    /* a wrong report id is ignored */
    uint8_t r[64] = { 0 }; uint32_t raw = 0xABCD;
    r[0] = 7; r[5] = 0x28;
    CHECK(pd_eval(&t, r, 64, &raw) == 0 && raw == 0xABCD, "report id 7 leaves the raw word alone");
    CHECK(pd_eval(&t, r, 5, &raw) == 0, "short report rejected");
    /* the built-in table's minimum is 8 bytes: a DS4 always sends 64, and byte 7 must exist */
    CHECK(pd_eval(&bt, r, 7, &raw) == 0 && pd_eval(&bt, r, 8, &raw) == 1, "built-in DS4: min length 8");
}

/* ======================================================================= tables: families */
static void test_families(void)
{
    printf("-- families (heuristic and database)\n");
    static hp_layout_t L; static pd_table_t t; pd_build_info_t info; uint32_t raw; char buf[1500];

    /* ---- SNES12: descriptor only */
    hp_parse(D_SNES12, sizeof D_SNES12, &L);
    CHECK(pd_from_layout(&L, &t, &info) == 1, "snes12 heuristic");
    pd_dump(&t, buf, sizeof buf); printf("   SNES12 heuristic: %s\n", buf);
    CHECK(t.rid_en == 0 && t.min_len == 4, "no id, min length 4 (%d)", t.min_len);
    { uint8_t r[4] = { 0x7F, 0x7F, 0, 0 };           CHECK(pd_eval(&t, r, 4, &raw) && raw == 0, "snes12 idle %08x", raw); }
    { uint8_t r[4] = { 0xFF, 0x7F, 0x01, 0 };        CHECK(pd_eval(&t, r, 4, &raw) && raw == 0x40011, "snes12 right+A: %08x (want 00040011)", raw); }
    { uint8_t r[4] = { 0x7F, 0x00, 0x00, 0x02 };     CHECK(pd_eval(&t, r, 4, &raw) && raw == 0x202008, "snes12 up+button10: %08x (want 00202008)", raw); }
    /* Y=0xC0 = 192 is >= the high threshold 192: down.  X=0x40 = 64 > 63: not left.  So expected: DOWN (bit 2) and LSD (bit 20) */
    { uint8_t r[4] = { 0x40, 0xC0, 0x00, 0x00 };     pd_eval(&t, r, 4, &raw); CHECK(raw == ((1u << 2) | (1u << 20)), "snes12 thresholds: X=64 not left, Y=192 down: %08x", raw); }
    { uint8_t r[4] = { 0x3F, 0xBF, 0x00, 0x00 };     pd_eval(&t, r, 4, &raw); CHECK(raw == ((1u << 1) | (1u << 19)), "snes12: X=63 left, Y=191 not down: %08x", raw); }

    /* ---- HAT12: descriptor only */
    hp_parse(D_HAT12, sizeof D_HAT12, &L);
    CHECK(pd_from_layout(&L, &t, &info) == 1, "hat12 heuristic");
    CHECK(t.min_len == 6, "hat12 min length %d", t.min_len);
    { uint8_t r[6] = { 0x7F, 0x7F, 0x7F, 0x7F, 0x0F, 0x00 };  CHECK(pd_eval(&t, r, 6, &raw) && raw == 0, "hat12 idle (hat null 15) %08x", raw); }
    { uint8_t r[6] = { 0x7F, 0x7F, 0x7F, 0x7F, 0x12, 0x00 };  pd_eval(&t, r, 6, &raw); CHECK(raw == 0x11, "hat12 E + button1: %08x (want 00000011)", raw); }
    { uint8_t r[6] = { 0x00, 0x7F, 0x7F, 0x7F, 0x05, 0x80 };  pd_eval(&t, r, 6, &raw); CHECK(raw == 0x88006, "hat12 SW + button12 + stick left: %08x (want 00088006)", raw); }
    { uint8_t r[6] = { 0, 0, 0, 0, 0x12 };                    CHECK(pd_eval(&t, r, 5, &raw) == 0, "hat12: a 5-byte payload is too short"); }
    for (int h = 0; h < 8; h++) {      /* each hat position -> exactly the right d-pad bits (N=up, NE=up+right, ...) */
        static const int D[8] = { 8, 8 | 1, 1, 4 | 1, 4, 4 | 2, 2, 8 | 2 };     /* up=8 right=1 down=4 left=2 raw bits 3 0 2 1 */
        uint8_t r[6] = { 0x7F, 0x7F, 0x7F, 0x7F, (uint8_t)h, 0 };
        pd_eval(&t, r, 6, &raw);
        CHECK(raw == (uint32_t)D[h], "hat12 position %d -> %08x (want %x)", h, raw, D[h]);
    }

    /* ---- XBOXHID: descriptor + a Linux-style database line */
    hp_parse(D_XBOXHID, sizeof D_XBOXHID, &L);
    sdl_map_t m;
    sdl_parse_line("030000005e040000ff02000000000000,Test Xbox HID,a:b0,b:b1,x:b2,y:b3,back:b6,start:b7,leftshoulder:b4,rightshoulder:b5,leftstick:b8,rightstick:b9,"
                   "dpup:h0.1,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,leftx:a0,lefty:a1,rightx:a3,righty:a4,lefttrigger:a2,righttrigger:a5,platform:Linux,", &m);
    CHECK(pd_from_sdl(&L, &m, &t, &info) == 1 && info.nunres == 0, "xboxhid builds");
    pd_dump(&t, buf, sizeof buf); printf("   XBOXHID: %s\n", buf);
    CHECK(t.min_len == 14, "xboxhid min length %d (want 14)", t.min_len);
    { uint8_t r[14] = { 0 };   CHECK(pd_eval(&t, r, 14, &raw) && raw == 0, "xboxhid idle %08x", raw); }
    { uint8_t r[14] = { 0 };   r[0] = 0xFF; r[1] = 0x7F;                  pd_eval(&t, r, 14, &raw); CHECK(raw == (1u << 18), "LX=+32767 -> stick right: %08x", raw); }
    { uint8_t r[14] = { 0 };   r[2] = 0x00; r[3] = 0x80;                  pd_eval(&t, r, 14, &raw); CHECK(raw == (1u << 21), "LY=-32768 -> stick up: %08x", raw); }
    /* signed 16 bit range -32768..32767: a quarter is 16383, so "low" is <= -16385 and "high" is >= 16384 */
    { uint8_t r[14] = { 0 };   r[0] = 0x00; r[1] = 0xC0;                  pd_eval(&t, r, 14, &raw); CHECK(raw == 0, "LX=-16384 (0xC000) is just inside the dead zone: %08x", raw); }
    { uint8_t r[14] = { 0 };   r[0] = 0xFF; r[1] = 0xBF;                  pd_eval(&t, r, 14, &raw); CHECK(raw == (1u << 19), "LX=-16385 (0xBFFF) is left: %08x", raw); }
    { uint8_t r[14] = { 0 };   r[0] = 0x00; r[1] = 0x40;                  pd_eval(&t, r, 14, &raw); CHECK(raw == (1u << 18), "LX=+16384 (0x4000) is right: %08x", raw); }
    { uint8_t r[14] = { 0 };   r[0] = 0xFF; r[1] = 0x3F;                  pd_eval(&t, r, 14, &raw); CHECK(raw == 0, "LX=+16383 is in the dead zone: %08x", raw); }
    for (int lt = 250; lt < 260; lt++) {                                  /* LT: 10 bit, bits 64-73; threshold 255 */
        uint8_t r[14] = { 0 }; setbits(r, 64, 10, (uint32_t)lt); pd_eval(&t, r, 14, &raw);
        CHECK(((raw >> 10) & 1) == (lt >= 255), "LT=%d -> L2 %d", lt, (int)((raw >> 10) & 1));
    }
    { uint8_t r[14] = { 0 }; setbits(r, 74, 10, 0x2A5); pd_eval(&t, r, 14, &raw); CHECK(raw == (1u << 11), "RT=677 (straddles bytes 9/10) -> R2: %08x", raw); }
    { uint8_t r[14] = { 0 }; setbits(r, 74, 10, 0x0FE); pd_eval(&t, r, 14, &raw); CHECK(raw == 0, "RT=254 below the quarter: %08x", raw); }
    for (int h = 0; h <= 8; h++) {                                         /* hat 1..8 = N NE E SE S SW W NW, 0 = null */
        static const uint32_t D[9] = { 0, 8, 8 | 1, 1, 4 | 1, 4, 4 | 2, 2, 8 | 2 };
        uint8_t r[14] = { 0 }; setbits(r, 88, 4, (uint32_t)h); pd_eval(&t, r, 14, &raw);
        uint32_t want = D[h];
        CHECK(raw == want, "xboxhid hat %d -> %08x (want %x)", h, raw, want);
    }
    { uint8_t r[14] = { 0 }; setbits(r, 96, 1, 1); setbits(r, 105, 1, 1); pd_eval(&t, r, 14, &raw); CHECK(raw == ((1u << 4) | (1u << 15)), "buttons 1 and 10: %08x", raw); }
    { uint8_t r[14] = { 0 }; r[0] = 0xFF; r[1] = 0x7F; setbits(r, 64, 10, 1023); setbits(r, 74, 10, 1023); setbits(r, 88, 4, 3); setbits(r, 96, 1, 1); setbits(r, 105, 1, 1);
      pd_eval(&t, r, 14, &raw); CHECK(raw == 0x48C11, "combined: %08x (want 00048C11)", raw); }

    /* ---- inverted half axes: "dpup:-a4~" is up when the axis is HIGH */
    hp_parse(D_RETRO5, sizeof D_RETRO5, &L);
    sdl_parse_line("03000000790000001100000000000000,inverted dpad,a:b0,b:b1,x:b2,y:b3,dpup:-a4~,dpdown:+a4~,dpleft:-a3,dpright:+a3,platform:Linux,", &m);
    CHECK(pd_from_sdl(&L, &m, &t, &info) == 1, "inverted dpad builds");
    { uint8_t r[7] = { 0x7F, 0x7F, 0x7F, 0x7F, 0xFF, 0, 0 }; pd_eval(&t, r, 7, &raw); CHECK(raw == 8, "-a4~ with the axis high is up: %08x", raw); }
    { uint8_t r[7] = { 0x7F, 0x7F, 0x7F, 0x7F, 0x00, 0, 0 }; pd_eval(&t, r, 7, &raw); CHECK(raw == 4, "+a4~ with the axis low is down: %08x", raw); }
    { uint8_t r[7] = { 0x7F, 0x7F, 0x7F, 0x00, 0x7F, 0, 0 }; pd_eval(&t, r, 7, &raw); CHECK(raw == 2, "-a3 with the axis low is left: %08x", raw); }

    /* ---- TWOID */
    hp_parse(D_TWOID, sizeof D_TWOID, &L);
    CHECK(pd_from_layout(&L, &t, &info) == 1 && t.rid_en && t.rid_val == 3 && t.min_len == 4, "twoid table header");
    { uint8_t r[4] = { 3, 0xFF, 0x7F, 0x01 }; pd_eval(&t, r, 4, &raw); CHECK(raw == 0x40011, "twoid id 3: %08x", raw); }
    { uint8_t r[5] = { 4, 0xFF, 0xFF, 0xFF, 0xFF }; raw = 0x5A5A; CHECK(pd_eval(&t, r, 5, &raw) == 0 && raw == 0x5A5A, "twoid: report id 4 is ignored, the word is kept"); }

    /* ---- HAT4 (4-way hat) */
    hp_parse(D_HAT4, sizeof D_HAT4, &L);
    CHECK(pd_from_layout(&L, &t, &info) == 1, "hat4");
    { uint8_t r[2] = { 0x0F, 0x00 }; pd_eval(&t, r, 2, &raw); CHECK(raw == 0, "hat4 idle %08x", raw); }
    { uint8_t r[2] = { 0x01, 0x20 }; pd_eval(&t, r, 2, &raw); CHECK(raw == 0x201, "hat4 E + button 6 (R1): %08x (want 00000201)", raw); }
    { uint8_t r[2] = { 0x03, 0x80 }; pd_eval(&t, r, 2, &raw); CHECK(raw == 0x2002, "hat4 W + button 8 (start): %08x (want 00002002)", raw); }
    { uint8_t r[2] = { 0x02, 0x00 }; pd_eval(&t, r, 2, &raw); CHECK(raw == 4, "hat4 S: %08x", raw); }
    { uint8_t r[2] = { 0x00, 0x00 }; pd_eval(&t, r, 2, &raw); CHECK(raw == 8, "hat4 N: %08x", raw); }

    /* ---- the non-pads */
    hp_parse((const uint8_t *)"", 0, &L);
    CHECK(pd_from_layout(&L, &t, &info) == 0, "an empty layout is not a pad");
}

/* ======================================================================= XInput */
static void test_xinput(void)
{
    printf("-- XInput table\n");
    static pd_table_t t; uint32_t raw;
    pd_builtin_xinput(&t);
    uint8_t r[20] = { 0, 20 };
    CHECK(pd_eval(&t, r, 20, &raw) && raw == 0, "idle %08x", raw);
    r[2] = 0x01; r[3] = 0x10;                                   /* up, A */
    pd_eval(&t, r, 20, &raw); CHECK(raw == ((1u << 3) | (1u << 4)), "d-pad up + A: %08x", raw);
    r[2] = 0x30; r[3] = 0xC3;                                   /* start back; LB RB Y B?? 0xC3 = LB RB X Y */
    pd_eval(&t, r, 20, &raw); CHECK(raw == ((1u << 13) | (1u << 12) | (1u << 8) | (1u << 9) | (1u << 6) | (1u << 7)), "start back LB RB X Y: %08x", raw);
    memset(r, 0, sizeof r); r[1] = 20; r[4] = 0x80; r[5] = 0x3E;
    pd_eval(&t, r, 20, &raw); CHECK(raw == (1u << 10), "LT=128 pressed, RT=62 not: %08x", raw);
    memset(r, 0, sizeof r); r[1] = 20; r[6] = 0xFF; r[7] = 0x7F; r[8] = 0xFF; r[9] = 0x7F;       /* LX +max, LY +max (= up on a pad) */
    pd_eval(&t, r, 20, &raw); CHECK(raw == ((1u << 18) | (1u << 21)), "left stick right+up: %08x", raw);
    memset(r, 0, sizeof r); r[1] = 20; r[10] = 0x00; r[11] = 0x80; r[12] = 0x00; r[13] = 0x80; /* RX min, RY min = left, down */
    pd_eval(&t, r, 20, &raw); CHECK(raw == ((1u << 23) | (1u << 24)), "right stick left+down: %08x", raw);
    memset(r, 0, sizeof r); r[0] = 1; r[2] = 0xFF;
    raw = 77; CHECK(pd_eval(&t, r, 20, &raw) == 0 && raw == 77, "message type 1 (not an input state) ignored");
}

/* ======================================================================= the ladder */
typedef struct {
    const char *user, *card;
    int opened[2], scans;
    int have_dec; pd_table_t dec;
    const char *p; int cur;
} srcs_t;

static int s_open(void *c, int w)   { srcs_t *s = c; const char *t = w ? s->card : s->user; if (!t) return 0; s->scans++; s->opened[w]++; s->p = t; s->cur = w; return 1; }
static const char *s_line(void *c, int w)
{
    srcs_t *s = c; static char buf[1100]; (void)w;
    if (!*s->p) return NULL;
    const char *e = strchr(s->p, '\n'); size_t l = e ? (size_t)(e - s->p) : strlen(s->p);
    memcpy(buf, s->p, l); buf[l] = 0; s->p += e ? l + 1 : l;
    return buf;
}
static void s_close(void *c, int w) { (void)c; (void)w; }
static int s_dec(void *c, uint16_t v, uint16_t p, uint16_t ver, pd_table_t *out) { srcs_t *s = c; (void)v; (void)p; (void)ver; if (!s->have_dec) return 0; *out = s->dec; return 1; }
static int s_builtin(void *c, uint16_t vid, uint16_t pid, uint16_t ver, sdl_map_t *out)
{
    (void)c;
    /* Companion's find_usb_gamepad_map: exact version, else version 0, else the first entry for the vendor:product */
    const UsbGamepadMap *first = NULL, *v0 = NULL;
    for (unsigned i = 0; i < kUsbGamepadMapsCount; i++) {
        const UsbGamepadMap *e = &kUsbGamepadMaps[i];
        if (e->vid != vid || e->pid != pid) continue;
        if (!first) first = e;
        if (!v0 && e->version == 0) v0 = e;
        if (e->version == ver) return pdb_from_companion(e, ver, out);
    }
    const UsbGamepadMap *e = v0 ? v0 : first;
    return e ? pdb_from_companion(e, ver, out) : PM_NONE;
}

static void clr2(pdb_cache_t *c) { pdb_cache_clear(&c[0]); pdb_cache_clear(&c[1]); }

static void test_ladder(void)
{
    printf("-- the ladder\n");
    static srcs_t S; static pad_resolution_t R; static hp_layout_t L, LU;
    static const pad_sources_t SRC = { &S, s_open, s_line, s_close, s_dec, s_builtin };
    pdb_cache_t cache[2]; pdb_cache_clear(&cache[0]); pdb_cache_clear(&cache[1]);
    pad_query_t q; memset(&q, 0, sizeof q); q.if_class = q.if_sub = q.if_proto = -1; q.core = "NES";

    /* DS4 */
    hp_parse(D_DS4, sizeof D_DS4, &L);
    q.vid = 0x054c; q.pid = 0x09cc; q.ver = 0x0100; q.L = &L;
    memset(&S, 0, sizeof S);
    CHECK(pad_resolve(&SRC, cache, &q, &R) == PRS_DS4 && R.table.mode == PDM_DS4, "DS4 id -> built in (%s)", R.why);
    /* a saved decode beats everything, even the DS4 id */
    S.have_dec = 1; pd_builtin_xinput(&S.dec);
    CHECK(pad_resolve(&SRC, cache, &q, &R) == PRS_USER_DEC, "saved decode wins");
    S.have_dec = 0;
    /* ...the user's own database line beats the DS4 shortcut (and the card file does not: a DS4 keeps the decode proven on hardware) */
    static char card_ds4[700]; snprintf(card_ds4, sizeof card_ds4, "%s\n", DS4_LINE); S.card = card_ds4;
    clr2(cache);
    CHECK(pad_resolve(&SRC, cache, &q, &R) == PRS_DS4, "a card line does not displace the built-in DS4 decode (%s)", R.why);
    S.user = "030000004c050000cc09000000000000,my ps4,a:b0,b:b1,x:b2,y:b3,dpup:h0.1,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,platform:Linux,\n";
    clr2(cache);
    CHECK(pad_resolve(&SRC, cache, &q, &R) == PRS_USER_DB && !strcmp(R.name, "my ps4"), "a USER line for 054c:09cc wins over the DS4 shortcut (src %d '%s')", R.src, R.name);
    {   uint8_t r[64] = { 0 }; r[0] = 1; r[5] = 0x18; uint32_t raw;                 /* hat released (8), button 1 = bit 4 of byte 5 = A in the user's line */
        pd_eval(&R.table, r, 64, &raw); CHECK(raw == (1u << 4), "the user's line decodes: a = b0 -> south: %08x", raw); }
    S.user = NULL; S.card = NULL; clr2(cache);

    /* XInput class: no descriptor needed */
    q.vid = 0x045e; q.pid = 0x028e; q.ver = 0x0114; q.L = NULL; q.if_class = 0xFF; q.if_sub = 0x5D; q.if_proto = 0x01;
    CHECK(pad_resolve(&SRC, cache, &q, &R) == PRS_XINPUT && R.table.mode == PDM_TABLE && R.table.rid_en, "xinput class (%s)", R.why);
    q.if_class = q.if_sub = q.if_proto = -1;

    /* user file beats the card file at ANY tier */
    hp_parse(D_SNES12, sizeof D_SNES12, &L);
    q.vid = 0x1234; q.pid = 0x5678; q.ver = 0x0200; q.L = &L;
    S.user = "03000000341200007856000000000000,user ver0,a:b4,b:b5,x:b6,y:b7,dpup:-a1,dpdown:+a1,dpleft:-a0,dpright:+a0,start:b9,back:b8,platform:Linux,\n";
    S.card = "03000000341200007856000000020000,card exact,a:b0,b:b1,x:b2,y:b3,dpup:-a1,dpdown:+a1,dpleft:-a0,dpright:+a0,start:b9,back:b8,platform:Linux,\n";
    clr2(cache);
    CHECK(pad_resolve(&SRC, cache, &q, &R) == PRS_USER_DB && R.rank == PM_VER0 && !strcmp(R.name, "user ver0"), "user line (version 0) beats a card line with the exact version: src %d '%s'", R.src, R.name);
    {   /* A -> raw 4 is button 4 of the descriptor = bit 16+4 = byte 2 bit 4 */
        uint8_t r[4] = { 0x7F, 0x7F, 0x10, 0 }; uint32_t raw; pd_eval(&R.table, r, 4, &raw);
        CHECK(raw == (1u << 4), "user line: A = b4 (bit 20) -> raw SOUTH: %08x", raw);
    }
    S.user = NULL;
    clr2(cache);
    CHECK(pad_resolve(&SRC, cache, &q, &R) == PRS_DB && R.rank == PM_EXACT, "card line exact (src %d rank %d)", R.src, R.rank);
    /* the same lookup again is answered from the cache: no file is opened */
    int before = S.scans;
    CHECK(pad_resolve(&SRC, cache, &q, &R) == PRS_DB && S.scans == before, "cached: no file opened (%d)", S.scans - before);

    /* an unknown pad: descriptor only; negative result cached */
    S.user = ""; S.card = "# empty\n"; clr2(cache);
    q.vid = 0x9999; q.pid = 0x0001; q.ver = 0;
    S.scans = 0;
    CHECK(pad_resolve(&SRC, cache, &q, &R) == PRS_HEURISTIC && R.table.mode == PDM_TABLE, "unknown pad + descriptor -> heuristic (%s)", R.why);
    int sc1 = S.scans;
    CHECK(pad_resolve(&SRC, cache, &q, &R) == PRS_HEURISTIC && S.scans == sc1, "an unknown pad is looked up once (negative cache): %d scans then %d", sc1, S.scans);
    CHECK(sc1 == 2, "...which read both files once each (%d)", sc1);

    /* a database line whose indices do not fit this descriptor is rejected: the descriptor decides */
    S.user = NULL; S.card = "03000000999900000100000000000000,kernel numbered,a:b20,b:b21,x:b22,y:b23,dpup:h0.1,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,platform:Linux,\n";
    clr2(cache);
    CHECK(pad_resolve(&SRC, cache, &q, &R) == PRS_HEURISTIC && R.skipped_unusable == 1, "bad indices -> rejected, heuristic (skipped %d, %s)", R.skipped_unusable, R.why);

    /* not a pad / no descriptor */
    clr2(cache);
    q.L = NULL; q.L_cls = HPC_MOUSE;
    CHECK(pad_resolve(&SRC, cache, &q, &R) == PRS_QUIET && R.table.mode == PDM_QUIET, "a mouse is quiet (%s)", R.why);
    q.L_cls = HPC_NONE; S.card = ""; clr2(cache);
    CHECK(pad_resolve(&SRC, cache, &q, &R) == PRS_LEGACY && R.table.mode == PDM_DS4, "unknown pad, no descriptor: the FPGA's old behaviour (%s)", R.why);
    q.vid = 0x0079; q.pid = 0x0011; q.ver = 0x0106; S.card = NULL; clr2(cache);
    CHECK(pad_resolve(&SRC, cache, &q, &R) == PRS_QUIET && R.table.mode == PDM_QUIET, "known (built-in table) non-DS4 pad but no descriptor: quiet, not garbage (%s)", R.why);

    /* Retrolink SNES (0079:0011 v0106) from the real built-in table: its line puts the d-pad on axes a3/a4, so on the 2-axis SNES12 shape the
     * entry cannot apply (the descriptor decides: heuristic), and on a descriptor that really has 5 axes it does */
    clr2(cache);
    q.vid = 0x0079; q.pid = 0x0011; q.ver = 0x0106; q.L = &L;
    int r = pad_resolve(&SRC, cache, &q, &R);
    CHECK(r == PRS_HEURISTIC && R.skipped_unusable == 1, "0079:0011 v0106 on a 2-axis descriptor: entry rejected, heuristic (src %d, skipped %d)", r, R.skipped_unusable);
    hp_parse(D_RETRO5, sizeof D_RETRO5, &LU);
    q.L = &LU; clr2(cache);
    r = pad_resolve(&SRC, cache, &q, &R);
    CHECK(r == PRS_BUILTIN && R.rank == PM_EXACT, "0079:0011 v0106 on a 5-axis descriptor: built-in exact: src %d rank %d (%s)", r, R.rank, R.why);
    char buf[1500]; pd_dump(&R.table, buf, sizeof buf); printf("   %s (%s): %s\n", R.name, R.why, buf);
    {   uint32_t raw; uint8_t a[7] = { 0x7F, 0x7F, 0x7F, 0xFF, 0x7F, 0x04, 0x00 };         /* Rx = 255 (right), button b2 = a */
        pd_eval(&R.table, a, 7, &raw); CHECK(raw == 0x11, "v0106: d-pad right (+a3) and A (b2): %08x (want 00000011)", raw);
        uint8_t b[7] = { 0x7F, 0x7F, 0x7F, 0x7F, 0x00, 0x01, 0x02 };                        /* Ry = 0 (up), b0 = y, b9 = start */
        pd_eval(&R.table, b, 7, &raw); CHECK(raw == 0x2088, "v0106: up (-a4), Y (b0), start (b9): %08x (want 00002088)", raw);
        uint8_t c[7] = { 0x00, 0xFF, 0x7F, 0x7F, 0x7F, 0x00, 0x00 };                        /* X, Y are not the d-pad on this pad */
        pd_eval(&R.table, c, 7, &raw); CHECK(raw == 0, "v0106: X/Y are not bound: %08x", raw);
    }
    q.ver = 0x0777; clr2(cache);
    r = pad_resolve(&SRC, cache, &q, &R);
    CHECK(r == PRS_BUILTIN && R.rank == PM_VER0 && !strcmp(R.name, "Retro Controller"), "unknown bcdDevice falls to the version-0 entry (rank %d, name '%s')", R.rank, R.name);
    {   uint32_t raw; uint8_t a[7] = { 0x7F, 0x7F, 0x7F, 0xFF, 0x7F, 0x02, 0x00 };         /* version-0 entry: a = b1 */
        pd_eval(&R.table, a, 7, &raw); CHECK(raw == 0x11, "version-0 entry: right + A (b1): %08x", raw);
    }
    (void)LU;
}

/* ======================================================================= pack / wire */
/* the compiled-in table answers only when the card file cannot be opened */
static void test_builtin_is_nocard_fallback(void)
{
    printf("-- the built-in table is the no-card fallback only\n");
    static srcs_t S; static pad_resolution_t R; static hp_layout_t L;
    static const pad_sources_t SRC = { &S, s_open, s_line, s_close, s_dec, s_builtin };
    pad_query_t q; memset(&q, 0, sizeof q); q.if_class = q.if_sub = q.if_proto = -1; q.core = "NES";
    hp_parse(D_HAT12, sizeof D_HAT12, &L);
    q.vid = 0x046d; q.pid = 0xc216; q.ver = 0; q.L = &L;
    memset(&S, 0, sizeof S); S.user = NULL; S.card = NULL;
    CHECK(pad_resolve(&SRC, NULL, &q, &R) == PRS_BUILTIN, "no card file: the built-in table (%s)", R.why);
    memset(&S, 0, sizeof S); S.card = "";
    int r = pad_resolve(&SRC, NULL, &q, &R);
    CHECK(r != PRS_BUILTIN && r == PRS_HEURISTIC, "an empty card file that opened is the database: no built-in line (src %d)", r);
    memset(&S, 0, sizeof S); S.card = "# nothing for this pad\n";
    CHECK(pad_resolve(&SRC, NULL, &q, &R) == PRS_HEURISTIC, "a card file without the pad: descriptor only");
}

static void test_wire(void)
{
    printf("-- wire format\n");
    for (int k = 0; k < 20000; k++) {
        pd_ent_t e; e.out = (uint8_t)(rnd() & 31); e.cmp = (uint8_t)(rnd() % 6); e.byte = (uint8_t)(rnd() & 15); e.shift = (uint8_t)(rnd() & 7);
        e.widthm1 = (uint8_t)(rnd() & 15); e.bias = (uint8_t)(rnd() & 1); e.thr = (uint16_t)rnd();
        pd_ent_t f; uint64_t w = pd_ent_pack(&e); pd_ent_unpack(w, &f);
        CHECK(w < (1ull << 36) && !memcmp(&e, &f, sizeof e), "entry round trip");
        if (fails > 10) return;
    }
    static pd_table_t a, b; uint8_t buf[400];
    pd_builtin_xinput(&a);
    int n = pd_wire(&a, buf, sizeof buf);
    CHECK(n == 5 + 5 * a.count && pd_unwire(buf, n, &b) && !memcmp(&a, &b, sizeof a), "xinput table round trip (%d bytes)", n);
    pd_builtin_ds4(&a); n = pd_wire(&a, buf, sizeof buf);
    CHECK(pd_unwire(buf, n, &b) && b.count == a.count && !memcmp(a.e, b.e, sizeof(pd_ent_t) * a.count), "ds4 table round trip (%d bytes)", n);
    CHECK(pd_wire(&a, buf, 6) < 0, "buffer too small");
    CHECK(!pd_unwire(buf, 4, &b), "short");
}

/* ======================================================================= the whole built-in table */
/* an independent oracle: evaluate a database entry on a report of the UNIVERSAL layout with plain arithmetic */
static uint32_t oracle(const sdl_map_t *m, const uint8_t *r)
{
    /* universal: button k = bit k (bytes 0-3); axis k = byte 4+k (8 bit unsigned, quarter points 63 and 192); hat0 = low nibble of byte 12,
     * hat1 = high nibble (0-7 = N NE E SE S SW W NW, anything else = released) */
    static const uint8_t RAW[SN_COUNT] = { 4, 5, 6, 7, 12, 16, 13, 14, 15, 8, 9, 3, 2, 1, 0, 18, 20, 22, 24, 10, 11, 17 };
    static const uint8_t NEG[4] = { 19, 21, 23, 25 };
    uint32_t out = 0;
    int dp_any = 0;
    for (int sn = SN_DPUP; sn <= SN_DPRIGHT; sn++) if (m->b[sn].type) dp_any = 1;
    for (int sn = 0; sn < SN_COUNT; sn++) {
        const sdl_bind_t *b = &m->b[sn];
        if (!b->type) continue;
        int stick = sn >= SN_LEFTX && sn <= SN_RIGHTY, trig = sn == SN_LTRIGGER || sn == SN_RTRIGGER;
        if (b->type == SB_BTN) {
            if (b->idx < 32 && ((r[b->idx / 8] >> (b->idx % 8)) & 1)) out |= 1u << RAW[sn];
        } else if (b->type == SB_HAT) {
            if (b->idx > 1) continue;
            int nib = (r[12] >> (b->idx ? 4 : 0)) & 0xF;
            static const int S[4] = { 0x83, 0x0E, 0x38, 0xE0 };
            if (nib <= 7) for (int d = 0; d < 4; d++) if (((b->hatmask >> d) & 1) && ((S[d] >> nib) & 1)) { out |= 1u << RAW[sn]; break; }
        } else {
            if (b->idx > 7) continue;
            int v = r[4 + b->idx];
            if (stick) {
                int p = v >= 192, n = v <= 63;
                if (b->inv) { int t = p; p = n; n = t; }
                if (p) out |= 1u << RAW[sn];
                if (n) out |= 1u << NEG[sn - SN_LEFTX];
            } else if (trig || b->type == SB_AXIS) {
                if (b->inv ? (v <= 192) : (v >= 63)) out |= 1u << RAW[sn];
            } else {
                int high = (b->type == SB_AXIS_POS) != (b->inv != 0);
                if (high ? (v >= 192) : (v <= 63)) out |= 1u << RAW[sn];
            }
        }
    }
    if (!dp_any) {                       /* the left stick drives the directions when there is no d-pad */
        const sdl_bind_t *x = &m->b[SN_LEFTX], *y = &m->b[SN_LEFTY];
        if (x->type >= SB_AXIS && x->type <= SB_AXIS_NEG && x->idx <= 7) {
            int v = r[4 + x->idx]; int p = v >= 192, n = v <= 63; if (x->inv) { int t = p; p = n; n = t; }
            if (p) out |= 1u << 0;
            if (n) out |= 1u << 1;
        }
        if (y->type >= SB_AXIS && y->type <= SB_AXIS_NEG && y->idx <= 7) {
            int v = r[4 + y->idx]; int p = v >= 192, n = v <= 63; if (y->inv) { int t = p; p = n; n = t; }
            if (p) out |= 1u << 2;
            if (n) out |= 1u << 3;
        }
    }
    return out;
}

static void test_builtin_sweep(void)
{
    printf("-- every entry of the built-in table (%u entries) on the universal layout\n", kUsbGamepadMapsCount);
    static hp_layout_t L; static pd_table_t t;
    hp_parse(D_UNIVERSAL, sizeof D_UNIVERSAL, &L);
    long usable = 0, unusable = 0, differ = 0, evals = 0, maxcount = 0, sumcount = 0, nores_dir = 0, nores_face = 0;
    long reasons[4] = { 0 };
    for (unsigned i = 0; i < kUsbGamepadMapsCount; i++) {
        sdl_map_t m; pdb_from_companion(&kUsbGamepadMaps[i], kUsbGamepadMaps[i].version, &m);
        pd_build_info_t info;
        int ok = pd_from_sdl(&L, &m, &t, &info);
        if (!ok) {
            unusable++;
            if (!info.has_dpad) reasons[0]++; else if (info.nface < 2) reasons[1]++; else reasons[2]++;
            continue;
        }
        usable++;
        if (t.count > maxcount) maxcount = t.count;
        sumcount += t.count;
        (void)nores_dir; (void)nores_face;
        for (int k = 0; k < 60; k++) {
            uint8_t r[16]; for (int j = 0; j < 16; j++) r[j] = (uint8_t)rnd();
            if (k < 20) {      /* mostly idle reports with a few things pressed: buttons mostly 0, axes mostly centred, hats mostly null */
                memset(r, 0, 4); for (int j = 0; j < 8; j++) r[4 + j] = (uint8_t)(100 + rnd() % 56); r[12] = 0xFF;
                for (int j = 0; j < 3; j++) { int b = (int)(rnd() % 32); r[b / 8] |= (uint8_t)(1 << (b % 8)); }
                if (k & 1) r[4 + rnd() % 8] = (rnd() & 1) ? 255 : 0;
                if (k & 2) r[12] = (uint8_t)(rnd() % 8) | (uint8_t)((rnd() % 8) << 4);
            }
            uint32_t got = 0; pd_eval(&t, r, 13, &got);
            uint32_t want = oracle(&m, r);
            evals++;
            if (got != want) { if (differ++ < 6) printf("   entry %u '%s' %04x:%04x v%04x: got %08x want %08x\n", i, m.name, m.vid, m.pid, m.ver, got, want); }
        }
    }
    printf("   usable %ld, not usable %ld (no d-pad %ld, fewer than 2 face buttons %ld, other %ld); table entries per pad: max %ld, mean %.1f; %ld evaluations\n",
           usable, unusable, reasons[0], reasons[1], reasons[2], maxcount, usable ? (double)sumcount / (double)usable : 0.0, evals);
    CHECK(differ == 0, "%ld evaluations differ from the arithmetic oracle", differ);
    CHECK(usable > 1500, "most entries build (%ld)", usable);
    CHECK(maxcount <= PD_MAXENT, "table size");
}

int main(void)
{
    test_parser();
    test_sdl();
    test_scan();
    test_ds4_anchor();
    test_families();
    test_xinput();
    test_ladder();
    test_builtin_is_nocard_fallback();
    test_wire();
    test_builtin_sweep();
    printf("%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
