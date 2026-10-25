// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 the Tango authors
/*
 * tango_paddec.h -- the pad DECODE TABLE: the contract between the BL616 (which knows pads) and the FPGA (which applies the
 * table to every report with no added lag).
 *
 * Tango. Our own code.
 *
 * pd_eval() is the REFERENCE for sys-tango's tango_pad_decode.v: the RTL unit bench replays vectors generated from it
 * (sys-tango sim/pads/). Change one, change the other, regenerate the vectors.
 *
 * The RAW WORD (32 bits per pad; bits 0-17 are the DualShock 4 layout of menu-tango docs/TANGO-SPEC.md "Raw word", frozen: every saved
 * .map file and SET_PADMAP pair counts in these bits):
 *    0 right  1 left  2 down  3 up                     (d-pad: hat, d-pad buttons or d-pad axes)
 *    4 south (Cross, Xbox A)  5 east (Circle, B)  6 west (Square, X)  7 north (Triangle, Y)
 *    8 L1  9 R1  10 L2  11 R2  12 Share/Back/Select  13 Options/Start  14 L3  15 R3  16 Guide/PS  17 touchpad/misc
 *   18 left stick right  19 left stick left  20 left stick down  21 left stick up      (sticks as digital directions)
 *   22 right stick right 23 right stick left 24 right stick down 25 right stick up
 *   26-31 reserved, read 0
 *
 * A TABLE is a header and up to PD_MAXENT entries. An entry says: "raw bit OUT is set when this FIELD of the report satisfies this
 * TEST". Several entries may set the same raw bit (they are OR-ed): a d-pad that is a hat AND four buttons, a stick that is two
 * directions. The report is the interrupt payload as the pad sent it, byte 0 first (the report id when the pad has ids), only the
 * first 16 bytes are kept, and bytes at or after the payload length read as 0.
 *   field = ((byte[b+2] << 16 | byte[b+1] << 8 | byte[b]) >> shift) & (2^(widthm1+1) - 1)      (a window of 3 bytes: a 16-bit
 *           axis at any bit offset fits;  shift 0..7, width 1..16)
 *   bias:   field ^= 2^widthm1  (two's complement -> offset binary: signed axes compare as unsigned)
 *   tests:  NZ   field != 0            (a button; thr unused)
 *           Z    field == 0            (an active-low button)
 *           GE   field >= thr          (an axis pushed high, or a trigger)
 *           LE   field <= thr          (an axis pushed low)
 *           EQ   field == thr
 *           HAT  n = field - thr[11:8]; true when 0 <= n <= 7 and bit n of thr[7:0] is set (a hat's null state is out of range)
 * HEADER:  rid_en/rid_val: accept only reports whose byte 0 equals rid_val (a pad that sends several report ids);
 *          min_len: accept only payloads of at least this many bytes.  A report that fails either check leaves the raw word as it was.
 */
#ifndef TANGO_PADDEC_H
#define TANGO_PADDEC_H

#include <stdint.h>
#include "tango_hidparse.h"
#include "tango_paddb.h"

#define PD_MAXENT   64
#define PD_RPT_BYTES 16

enum { PDC_NZ = 0, PDC_Z, PDC_GE, PDC_LE, PDC_EQ, PDC_HAT };

/* raw bit numbers */
enum { RW_RIGHT = 0, RW_LEFT, RW_DOWN, RW_UP, RW_SOUTH, RW_EAST, RW_WEST, RW_NORTH, RW_L1, RW_R1, RW_L2, RW_R2, RW_BACK, RW_START,
       RW_L3, RW_R3, RW_GUIDE, RW_MISC, RW_LSR, RW_LSL, RW_LSD, RW_LSU, RW_RSR, RW_RSL, RW_RSD, RW_RSU };

typedef struct {
    uint8_t  out;            /* raw bit 0..31 */
    uint8_t  cmp;            /* PDC_ */
    uint8_t  byte;           /* 0..15 */
    uint8_t  shift;          /* 0..7 */
    uint8_t  widthm1;        /* width - 1, 0..15 */
    uint8_t  bias;           /* 0/1 */
    uint16_t thr;
} pd_ent_t;

enum { PDM_DS4 = 0, PDM_TABLE = 1, PDM_QUIET = 2 };      /* what the FPGA does with a port's reports */

typedef struct {
    uint8_t  mode;           /* PDM_ (PDM_DS4: the FPGA's built-in DualShock 4 decode, no table) */
    uint8_t  rid_en, rid_val;
    uint8_t  min_len;
    uint8_t  count;
    pd_ent_t e[PD_MAXENT];
} pd_table_t;

/* entry <-> its 36-bit word (the RTL table memory's width): out[35:31] cmp[30:28] byte[27:24] shift[23:21] widthm1[20:17] bias[16] thr[15:0] */
uint64_t pd_ent_pack(const pd_ent_t *e);
void     pd_ent_unpack(uint64_t w, pd_ent_t *e);

/* THE REFERENCE EVALUATOR. rpt = the payload (at most PD_RPT_BYTES kept), len its length. Returns 1 when the report is accepted
 * (header checks pass) and then *raw is the new raw word; 0 when it is rejected (*raw untouched). */
int      pd_eval(const pd_table_t *t, const uint8_t *rpt, int len, uint32_t *raw);
/* the FPGA engine's latency model: clock cycles (clk48) from the report to the raw word, for a table of this size */
int      pd_cycles(const pd_table_t *t);

/* ---- the link write: SET_DECODE payload after "01 18 port": kind, rid flags, rid val, min_len, count, count x 5 bytes. Returns the length. */
int      pd_wire(const pd_table_t *t, uint8_t *out, int cap);
int      pd_unwire(const uint8_t *in, int n, pd_table_t *t);          /* 1 ok (tests; the RTL does the same parse) */

/* ---- built-in tables ---- */
void     pd_builtin_ds4(pd_table_t *t);            /* exactly sys-tango tango_ds4_joy.v 73-95, as a table */
void     pd_builtin_xinput(pd_table_t *t);         /* Xbox 360 wired (interface class FF/5D/01), mcu_hw.c xbox_parse layout */
void     pd_quiet(pd_table_t *t);                  /* PDM_QUIET: the raw word stays 0 */

/* ---- builders ---- */
typedef struct {
    int nbind;               /* bindings in the line (buttons, axes, hats) */
    int nres;                /* resolved against the descriptor */
    int nunres;              /* index beyond what the descriptor has, or a field the engine cannot reach (outside bytes 0-15) */
    int has_dpad;            /* some direction resolved */
    int nface;               /* a b x y resolved */
    int too_many;            /* the table would exceed PD_MAXENT */
} pd_build_info_t;

/* database line + descriptor layout -> table. Returns 1 if the result is usable (a d-pad and two face buttons, nothing unresolved
 * of the d-pad / faces), else 0 (the caller falls to the descriptor heuristic). */
int      pd_from_sdl(const hp_layout_t *L, const sdl_map_t *m, pd_table_t *t, pd_build_info_t *info);

/* descriptor only (no database line): the SDL-style default. Returns 1 if the layout has a d-pad
 * (hat or two axes) and at least two buttons. */
int      pd_from_layout(const hp_layout_t *L, pd_table_t *t, pd_build_info_t *info);
/* the same, told where the pad rests (a report taken with nothing pressed, or NULL): an axis that rests at the low or high end of its range is a trigger, not a
 * stick, so it does not hold a stick direction (or a d-pad direction) on at rest, and it becomes L2 / R2 */
int      pd_from_layout_rest(const hp_layout_t *L, const uint8_t *rest, int rest_len, pd_table_t *t, pd_build_info_t *info);

/* a plain axis / button entry appended by hand (xinput, the Define-buttons learner) */
int      pd_add(pd_table_t *t, const pd_ent_t *e);
int      pd_add_bit(pd_table_t *t, int out, int bit_offset, int active_low);
int      pd_add_axis(pd_table_t *t, int out, int bit_offset, int size, int lmin, int lmax, int dir /* +1 high, -1 low, 0 trigger (high, quarter) */);
int      pd_add_hat(pd_table_t *t, int out, int bit_offset, int size, int lmin, int lmax, int dirmask);

/* a one-line dump for tests and the debug log */
void     pd_dump(const pd_table_t *t, char *buf, int cap);

#endif
