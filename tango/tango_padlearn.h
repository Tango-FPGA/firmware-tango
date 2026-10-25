// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 the Tango authors
/*
 * tango_padlearn.h -- the learn engine behind the system "Define buttons": a decode table made from what the pad itself reports.
 * Tango, our own code. Pure C, no I/O: the caller (a Define-buttons
 * page) feeds REPORT2 samples (padmgr_report_sample) and asks for the buttons one at a time.
 *
 * The method: an idle BASELINE (the bitwise majority of a few reports; bits that flip among them are NOISE and are never learned), then for each
 * standard button a few reports taken WHILE IT IS HELD. The bits that differ from the baseline, minus the noise, say what the button is:
 *   - exactly one changed bit group of one bit                      -> NZ (went 1) or Z (went 0, an active-low button)
 *   - a group of several bits (an analog trigger, an axis)          -> a field of 8 or 16 bits, GE/LE half-way between idle and pressed
 *   - several groups: one of them a single bit (a pad whose L2 sets a button bit AND moves an analog byte) -> the single bit wins;
 *     otherwise AMBIGUOUS and the user is asked to press that button alone
 *   - the d-pad (pl_learn_dpad, four presses): when all four change the SAME nibble or byte it is a hat: base and 4/8 positions follow from the four
 *     values; otherwise each direction is learned on its own (four buttons, or two axes)
 * A stick direction (pl_learn_stick) decides signed or offset binary from the idle value (near 0 or near the wrap: signed, bias bit set).
 * Limits, on purpose: fields are aligned (a nibble, a byte, two adjacent bytes little-endian); a 10-bit axis that straddles bytes is learned as the
 * 16-bit field around it; an axis that rests at 0 and moves only into the upper byte is read as a byte. The thresholds are half-way, not the
 * quarter points of the database builder: they agree at rest, at full deflection and for any press the user can tell from rest.
 */
#ifndef TANGO_PADLEARN_H
#define TANGO_PADLEARN_H

#include <stdint.h>
#include "tango_paddec.h"
#include "tango_hidparse.h"

#define PLS_MAX_SAMPLES  8                /* the most a HELD capture call is asked for */
#define PLS_IDLE_SAMPLES 12               /* the most the IDLE capture call is asked for */
#define PLS_IDLE_SAMPLES_MAX 16           /* room for it in pl_base_make */

typedef struct { uint8_t b[PD_RPT_BYTES]; uint8_t len; } pl_sample_t;

/* The rest state of a pad, from a few samples taken with nothing pressed: the bitwise majority report, the bits that flipped among them (noise), and for every
 * axis field of the descriptor (when there is one) the MEDIAN value and the SPREAD (max - min) at rest.  An axis is judged by its number, not its bits: a stick
 * that jitters across the middle of its range flips every bit of the field, and a bit mask would call the whole axis noise. */
typedef struct {
    pl_sample_t idle;
    uint8_t     noisy[PD_RPT_BYTES];
    uint16_t    ax_idle[HP_MAX_AXES], ax_spread[HP_MAX_AXES];    /* by axis ordinal (the layout's aidx), offset binary for a signed field */
    uint8_t     have;
} pl_base_t;
void pl_base_make(pl_base_t *b, const hp_layout_t *L, const pl_sample_t *s, int n);

/* What a report shows beyond rest: the changed bits outside the numeric axis fields (noise bits and `dyn` bits left out) and the set of axis ordinals that left
 * their rest range (by more than an eighth of their span and three times their spread at rest).  Returns nonzero when anything differs. */
typedef struct { uint8_t bits[PD_RPT_BYTES]; uint16_t axes; } pl_sig_t;
int pl_sig_of(const pl_base_t *b, const hp_layout_t *L, const uint8_t dyn[PD_RPT_BYTES], const pl_sample_t *s, pl_sig_t *out);
int pl_sig_share(const pl_sig_t *a, const pl_sig_t *c);        /* the two differ from rest in a common bit or a common axis */

enum { PL_OK = 1, PL_NOCHANGE = 0, PL_BAD = -1, PL_AMBIG = -2, PL_FULL = -3 };

typedef struct {
    pl_base_t   b;                       /* rest: majority report, noisy bits, axis medians */
    int         have_idle;
    int         minlen;                  /* shortest payload seen */
    uint8_t     rid_en, rid_val;
    const hp_layout_t *L;                /* the pad's parsed descriptor, when there is a usable one (pl_set_layout): changed bits are then grouped by the descriptor's fields */
    pd_table_t  t;
} pl_t;

void pl_begin(pl_t *l);
/* the idle baseline from n >= 1 samples taken with nothing pressed */
int  pl_baseline(pl_t *l, const pl_sample_t *s, int n);
/* a report id the pad's reports carry in byte 0 (so a second report id does not disturb the table) */
void pl_set_report_id(pl_t *l, int rid);
/* the parsed report descriptor of the pad (may be NULL; it must outlive the learner).  With it the learner knows where every field starts and how wide it is:
 * a button is its own bit, an axis is learned at its exact bit offset and width (a 10-bit axis at bit 4 stays a 10-bit axis), signedness comes from the logical
 * range, a hat is the hat field.  Without it (no descriptor, or one that did not parse) the learner works on the report bits alone, with fields aligned to bytes. */
void pl_set_layout(pl_t *l, const hp_layout_t *L);
/* raw bit `out` (RW_*) from n >= 1 samples taken while the button is held; replaces what was learned for that bit */
int  pl_learn_button(pl_t *l, int out, const pl_sample_t *pressed, int n);
/* a stick direction or any "pushed to the end of an axis" input: like pl_learn_button for a multi-bit change but decides signedness from the idle value */
int  pl_learn_stick(pl_t *l, int out, const pl_sample_t *pressed, int n);
/* the d-pad: four presses (RW_UP, RW_RIGHT, RW_DOWN, RW_LEFT in that order of `pressed[]`, each n samples) */
int  pl_learn_dpad(pl_t *l, const pl_sample_t *const pressed[4], int n);
/* the finished table (mode TABLE, min_len and report id set); returns the entry count */
int  pl_table(const pl_t *l, pd_table_t *out);

/* =====================================================================================================================================
 * THE INCREMENTAL CAPTURE: the press detector the Define dialogs drive, one report at a time (no blocking, no clock of its own).
 * The rest state first (PLS_IDLE_SAMPLES reports taken with nothing pressed), then for every button asked: feed reports until a press has come and gone.
 * A press is a difference from rest in a bit the descriptor declares (or, with no descriptor, any bit) that is not known to flicker, or an axis that left
 * its rest range, kept for two reports running; bits that change twice before the press, or three times during it, are noise from then on; a press that
 * lasts under PLCAP_MIN_HOLD_MS is a flicker and is forgotten; it ends when the report has been back at rest for PLCAP_REST_MS.  Reports whose first byte is
 * not the pad's report id are skipped.  padmgr_capture (the blocking reference capture) is this engine on a 20 ms poll.
 * ===================================================================================================================================== */
#define PLCAP_MIN_HOLD_MS 100
#define PLCAP_REST_MS     40
typedef struct plcap {
    pl_base_t   base;
    const hp_layout_t *L;
    uint8_t     rid_en, rid, nidle, have_base, started, have_prev, prev_ch, n;
    pl_sample_t idle[PLS_IDLE_SAMPLES];
    uint8_t     dyn[PD_RPT_BYTES], fl[PD_RPT_BYTES * 8];
    pl_sample_t prev, ring[PLS_MAX_SAMPLES];
    pl_sig_t    sprev;
    uint16_t    total;
    uint32_t    t_start, t_rest;             /* t_rest: when the report went back to rest (0: it has not) */
} plcap_t;
void plcap_begin(plcap_t *c, const hp_layout_t *L);                         /* L may be NULL: bit-diff mode */
/* one rest report; returns 1 when the rest state is complete (call plcap_idle_samples for the samples, plsess_baseline_samples takes them) */
int  plcap_idle(plcap_t *c, const pl_sample_t *s);
int  plcap_idle_samples(const plcap_t *c, const pl_sample_t **s);          /* the PLS_IDLE_SAMPLES reports, count returned */
void plcap_held_begin(plcap_t *c);                                           /* a new button is asked for */
/* one report (NULL: only the clock moved) at time now_ms.  Returns n > 0 when a press has come and gone (out[0..n-1], n <= max), PLCAP_PRESSED while a press is
 * going on (so the page can show the pad is being read), 0 otherwise. */
#define PLCAP_PRESSED (-1)
int  plcap_held(plcap_t *c, const pl_sample_t *s, uint32_t now_ms, pl_sample_t *out, int max);

/* ---- the .dec file: config/inputs/pad_<vid>_<pid>_<ver>_v1.dec ---- */
int  pl_dec_name(uint16_t vid, uint16_t pid, uint16_t ver, char *out, int cap);
int  pl_dec_save(uint16_t vid, uint16_t pid, uint16_t ver, const pd_table_t *t, uint8_t *out, int cap);      /* bytes written, 0 on error */
int  pl_dec_load(const uint8_t *in, int n, uint16_t vid, uint16_t pid, uint16_t ver, pd_table_t *t);          /* 1 if it is a valid file for this pad */

/* ---- the equivalent SDL line (gamecontrollerdb syntax) for a learned table, when the pad's descriptor layout is known and every learned input sits on a
 * descriptor field (bN / aN / hN). Inputs with no SDL form (the digital stick bits 18-25) are left out. Returns the length, 0 if nothing could be written. */
int  pl_export_sdl(const pd_table_t *t, const hp_layout_t *L, uint16_t vid, uint16_t pid, uint16_t ver, const char *name, char *out, int cap);

/* ---- how many separate changed groups a press shows (0: nothing changed; the d-pad session uses it to ask again at once) ---- */
int  pl_changed_groups(const pl_t *l, const pl_sample_t *pressed, int n);

/* =====================================================================================================================================
 * THE SESSION LAYER: what a Define-buttons page calls.
 * The page owns the screen and the user; this layer owns the learning. No malloc, no I/O except through the two callback tables below.
 * ===================================================================================================================================== */
#include "tango_hidparse.h"

#define PLSESS_BYTES 1100                 /* room for a plsess_t (checked by a static assertion in tango_padlearn.c) */
#define PLS_DPAD_SAMPLES 4                /* samples kept per d-pad direction */

/* the capture function: the page's "wait for a press" (pluggable). phase = PLC_KIND(phase) | (rw_bit << 8):
 *   PLC_IDLE: nothing is pressed (the user was told to let go); return 3..8 reports taken over about 300 ms. rw bit field = 0xFF.
 *   PLC_HELD: wait for the user to press the button that is asked for (PLC_RW(phase) says which: the page can show the prompt, also for each
 *             d-pad direction), return up to `max` reports taken WHILE IT IS HELD, and return only after it was released again.
 * Returns the number of samples (> 0), 0 = nothing came (no report from the pad, or the wait timed out), -1 = the user cancelled (menu key). */
enum { PLC_IDLE = 1, PLC_HELD = 2 };
#define PLC_KIND(p) ((p) & 0xFF)
#define PLC_RW(p)   (((p) >> 8) & 0xFF)
typedef int (*pl_capture_fn)(void *ctx, int phase, pl_sample_t *out, int max);

enum { PLS_OK = 1, PLS_NOCHANGE = 0, PLS_AMBIG = -2, PLS_FULL = -3, PLS_CANCEL = -4, PLS_NOREPORT = -5, PLS_BAD = -1 };

typedef struct plsess {
    pl_t        pl;
    pl_sample_t dp[4][PLS_DPAD_SAMPLES];      /* d-pad presses collected so far: RIGHT LEFT DOWN UP */
    uint8_t     dpn[4];                       /* samples per direction, 0 = not yet */
    uint8_t     state;                        /* 0 new, 1 baseline taken, 2 finished/aborted */
    uint8_t     nlearned, nskipped;
    uint16_t    vid, pid, ver;
    uint16_t    sdl_ver;                      /* the version of the SDL line's GUID: MiSTer compares the evdev version (bcdHID for USB HID), see plsess_set_sdl_version */
    const hp_layout_t *L;
    char        name[24];
} plsess_t;

int  plsess_begin(plsess_t *s, uint16_t vid, uint16_t pid, uint16_t ver, const hp_layout_t *L, const char *name);
int  plsess_baseline(plsess_t *s, pl_capture_fn cap, void *ctx);                                              /* PLS_OK, PLS_CANCEL, PLS_NOREPORT */
/* rw_bit: the RW_* bit asked for (RW_SOUTH.., the shoulder / trigger / stick-click bits, the stick directions RW_LSR..RW_RSU). `msg` gets a short
 * on-screen string (at most about 24 characters) for every return value. */
int  plsess_step(plsess_t *s, int rw_bit, pl_capture_fn cap, void *ctx, char *msg, int cap_msg);
/* the d-pad: four presses, asked in MiSTer's order RIGHT, LEFT, DOWN, UP (the capture function sees each in PLC_RW). After PLS_NOCHANGE / PLS_NOREPORT
 * the directions already taken are kept: call it again and it asks only for the missing ones. */
int  plsess_dpad(plsess_t *s, pl_capture_fn cap, void *ctx, char *msg, int cap_msg);
/* the version field of the SDL line written to the user's database: bcdHID of the pad's HID interface, which is what a Linux-made line (and MiSTer's lookup, input.cpp 5206)
 * carries; default = the bcdDevice given to plsess_begin.  The .dec file keeps bcdDevice. */
void plsess_set_sdl_version(plsess_t *s, uint16_t ver);
int  plsess_skip(plsess_t *s, int rw_bit);
/* the same steps for a caller that does its own capturing (plcap_t): the baseline from reports already taken, a button from the reports of its press, the d-pad
 * one direction at a time (d = 0 RIGHT, 1 LEFT, 2 DOWN, 3 UP, MiSTer's order): PLS_PARTIAL while directions are missing, the learning's result with the fourth */
#define PLS_PARTIAL 2
int  plsess_baseline_samples(plsess_t *s, const pl_sample_t *b, int n);
int  plsess_learn(plsess_t *s, int rw_bit, const pl_sample_t *p, int n, char *msg, int cap_msg);
int  plsess_dpad_part(plsess_t *s, int d, const pl_sample_t *p, int n, char *msg, int cap_msg);
/* the walk ended with some d-pad directions taken and the group not complete (a direction skipped, Enter pressed early): learn each stored direction on its own; returns
 * the bit mask (bit d, d as plsess_dpad_part) of the directions now in the table, and forgets the stored presses */
int  plsess_dpad_flush(plsess_t *s);                                                                   /* the pad has no such input */
void plsess_abort(plsess_t *s);                                                                               /* nothing sent, nothing stored */

/* the order of MiSTer's Define joystick buttons (menu.cpp joy_button_map 267, MENU_JOYDIGMAP1 4220-4260) as raw bits. A / B / X / Y are MiSTer's
 * joystick bits 4-7, which the default core map takes from raw 5, 4, 7, 6 (Circle, Cross, Triangle, Square: menu-tango docs/TANGO-SPEC.md "Default core map"),
 * L / R from raw 8, 9, Select / Start from raw 12, 13. `kind`: PLI_DPAD = part of the four-direction group (call plsess_dpad once at the first one),
 * PLI_BUTTON = plsess_step, PLI_STICK = plsess_step with an analog direction (optional). `optional` entries may be skipped without a question; MiSTer's
 * own flow lets the user skip any button with the menu-combo tap and has the analog tests optional. "KBD TOGGLE" and "MENU" of MiSTer's list are not pad
 * bits here (the in-core OSD opens with Start+Select) and are not in the table. L2 R2 L3 R3 Guide and the sticks are additions MiSTer's list does not have. */
enum { PLI_DPAD = 0, PLI_BUTTON, PLI_STICK };
typedef struct { uint8_t rw, kind, optional; const char *label; } plsess_item_t;
extern const plsess_item_t plsess_default_order[];
extern const int plsess_default_order_n;

/* the card side of plsess_finish; every callback returns 1 on success */
typedef struct {
    void *ctx;
    int (*save_dec)(void *ctx, const char *name, const uint8_t *data, int n);     /* config/inputs/<name>, creating the folders */
    /* gamecontrollerdb_user.txt is rewritten through a temporary copy: open_old (0 = there is no file yet), read, close_old; then open_new (creates
     * linux/gamecontrollerdb), write, commit (the new content replaces the file) or discard */
    int (*db_open_old)(void *ctx);
    int (*db_read)(void *ctx, char *buf, int cap);                                /* bytes, 0 at the end */
    void (*db_close_old)(void *ctx);
    int (*db_open_new)(void *ctx);
    int (*db_write)(void *ctx, const char *buf, int n);
    int (*db_commit)(void *ctx);
    void (*db_discard)(void *ctx);
} plsess_files_t;

/* replace-or-append one line in the user database through the callbacks (used by plsess_finish; exposed for the tests). `guid` = the 32 hex digits the line
 * starts with; an existing line starting with the same GUID whose name field starts with PLS_NAME_TAG is dropped, every other line is copied unchanged. */
#define PLS_NAME_TAG "Tango Define"
int  pl_sdl_db_update(const plsess_files_t *f, const char *guid, const char *line);

struct padmgr;
enum { PLF_APPLIED = 1, PLF_DEC = 2, PLF_SDL = 4 };                               /* what plsess_finish stored */
/* (a) build the table, (b) padmgr_set_table: sent, verified, cached (a new FPGA only), (c) the .dec file, (d) the SDL line in the user file when the
 * descriptor layout is known (a line this layer wrote for the same GUID before is replaced; others are kept). Returns the PLF_ flags, 0 if the table
 * is empty (nothing is stored), PLS_BAD on misuse. `sdl_out` (may be NULL) receives the line. The session is finished afterwards. */
int  plsess_finish(plsess_t *s, struct padmgr *m, int port, const plsess_files_t *f, char *sdl_out, int sdl_cap);

#endif
