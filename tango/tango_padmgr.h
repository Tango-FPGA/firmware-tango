// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 the Tango authors
/*
 * tango_padmgr.h -- the per-port pad manager: from "a pad was plugged in" to "the FPGA decodes it right".
 * Tango, our own code.
 *
 * Pure C: the platform (SPI link, card files, malloc, log, the core's map) is a table of callbacks, so the same code runs in the firmware
 * (tango_fw.c) and in the host tests (test/test_padmgr.c with test/fake_pad_fpga.c). It runs in the Tango task in BOTH the launcher and a
 * core; the link reads of the HID interrupt stay in com_task (tango_fw_hid_reports) and only feed it snapshots.
 *
 * Old FPGA (a sys-tango without the generic pad decode: unknown HID commands answer 00): the quarantine.
 *   A pad that the built-in table knows and whose layout is not the DualShock 4's is blanked (SET_PADMAP "none") and logged; a DualShock 4,
 *   a clone with the DS4 layout and every pad the table does not know are left exactly as before (apply_map is called when the connect flag
 *   or the ids change, as raw_in always did). No file is touched and no command is sent that an old FPGA could misread.
 * New FPGA (DECODE_STATUS answers the signature 0xD1): see tango_padmgr.c, "the new-FPGA flow".
 *
 * Memory: the manager is one small malloc'd block (about 220 bytes); everything big (descriptor, layout, ladder result, line buffer) is
 * allocated for one resolve and freed; a port keeps only its wire image (at most 325 bytes) while a table is in force.
 */
#ifndef TANGO_PADMGR_H
#define TANGO_PADMGR_H

#include <stddef.h>
#include <stdint.h>
#include "tango_proto.h"
#include "tango_paddb.h"
#include "tango_paddec.h"
#include "tango_padlearn.h"

#define PADMGR_PORTS 2

typedef struct {
    void *ctx;
    const tango_link_t *link;                         /* the SPI link (HID target 1) */
    void *(*alloc)(void *ctx, unsigned n);            /* NULL result = out of memory (the step is skipped with a log line) */
    void  (*release)(void *ctx, void *p);
    void  (*log)(void *ctx, const char *line);        /* one line, no newline */
    void  (*apply_map)(void *ctx, int port);          /* the core's pad map for a connected pad (pm_apply); NULL in the launcher */
    /* the compile-time pad table: rank (PM_) and the entry; PM_NONE if the pad is not in it */
    int   (*builtin)(void *ctx, uint16_t vid, uint16_t pid, uint16_t ver, sdl_map_t *out);
    /* the card files, read in chunks (which 0 = gamecontrollerdb_user.txt, 1 = gamecontrollerdb.txt). open returns 1 if the file exists. */
    int   (*file_open)(void *ctx, int which);
    int   (*file_read)(void *ctx, int which, char *buf, int cap);     /* bytes read, 0 at the end, < 0 on an error */
    void  (*file_close)(void *ctx, int which);
    /* a decode the user saved for this very pad (Define buttons, config/inputs/pad_<vid>_<pid>_<ver>_v1.dec); NULL = none */
    int   (*dec_load)(void *ctx, uint16_t vid, uint16_t pid, uint16_t ver, pd_table_t *out);
    uint32_t (*now_ms)(void *ctx);                    /* NULL = no timeouts */
    const char *core;                                 /* the running core's name, for mistercore: lines; NULL in the launcher */
} padmgr_platform_t;

typedef struct padmgr padmgr_t;

/* One manager per boot. NULL if there is no memory. */
padmgr_t *padmgr_new(const padmgr_platform_t *p);
void      padmgr_free(padmgr_t *m);

/* A REPORT snapshot of one port (HID REPORT, 01 12): connect flag (bit 0 A1, bit 2 A2 of `flags`), ids, raw word. Call it for every snapshot;
 * it decides by itself when something has to be done. The old-FPGA path calls platform.apply_map exactly when the connect flag or the ids change. */
void padmgr_snapshot(padmgr_t *m, int port, uint8_t flags, uint16_t vid, uint16_t pid, uint32_t raw);

/* A core (or the launcher) has started: its FPGA was just loaded, so every table in it is gone. Probes DECODE_STATUS once (0xD1 = the new
 * FPGA, anything else = old), switches PAD_CTRL on (ext events + the swap bit) and marks the ports that already hold a resolved table so the next
 * snapshot re-sends it (no file is read again). Call it before the first snapshot of the core; the snapshot probes by itself otherwise. */
void padmgr_core_start(padmgr_t *m);
enum { PADMGR_FPGA_UNKNOWN = 0, PADMGR_FPGA_OLD, PADMGR_FPGA_NEW };
int  padmgr_fpga(const padmgr_t *m);

/* A pad has been seen but its descriptors are not ready yet: the manager wants padmgr_tick() every ~250 ms and resolves without descriptors
 * after PADMGR_DESC_WAIT_MS (needs platform.now_ms). */
#define PADMGR_DESC_WAIT_MS 2500
int  padmgr_waiting(const padmgr_t *m);
void padmgr_tick(padmgr_t *m);

/* Players: the swap bit (PAD_CTRL bit 1). Returns 0 on an FPGA without it (old bitstream). */
int  padmgr_set_swap(padmgr_t *m, int on);
int  padmgr_swap(const padmgr_t *m);                  /* the requested state */
int  padmgr_swap_supported(const padmgr_t *m);

/* Define buttons (system): the engine of tango_padlearn.c needs the pad's raw report bytes, and a finished decode goes back through the manager.
 * padmgr_report_sample: one REPORT2 of the port as a learn sample (new FPGA only; returns 0 otherwise or when no report was accepted yet).
 * padmgr_set_table: the user's own decode for this pad: sent, verified and cached like a resolved one (src PRS_USER_DEC); 1 on success. */
int  padmgr_report_sample(padmgr_t *m, int port, pl_sample_t *s);
int  padmgr_set_table(padmgr_t *m, int port, const pd_table_t *t);
int  padmgr_port_ids(const padmgr_t *m, int port, uint16_t *vid, uint16_t *pid, uint16_t *ver);    /* 1 if a pad is there */
int  padmgr_layout(padmgr_t *m, int port, hp_layout_t *out);       /* the pad's parsed report descriptor into *out; 1 if it is a pad */
uint16_t padmgr_port_ver_hid(const padmgr_t *m, int port);         /* bcdHID of its HID interface, 0 unknown */

/* The reference capture function for the session layer (tango_padlearn.h, pl_capture_fn), built on padmgr_report_sample and the link's delay: IDLE
 * takes 12 reports 50 ms apart; HELD polls every 20 ms, starts when two consecutive reports differ from the idle ones in the same bit that did not flicker (during
 * IDLE, or twice since), collects while the difference stays (the last `max` samples are kept), and returns when it has gone for two polls. Gives up (0) after press_timeout_ms without a press. `cancelled`
 * (NULL ok) is polled every cycle and returns nonzero when the user asked to cancel (-1). Runs in the Tango task. */
typedef struct {
    padmgr_t *m; int port;
    int (*cancelled)(void *ctx); void *cctx;
    unsigned press_timeout_ms;                 /* 0 = 10000 */
    int rid_en, rid;                           /* the pad's report id (the parsed descriptor's), when it has one: reports with another id in byte 0 are not the pad's input and are skipped */
    const hp_layout_t *L;                      /* the pad's parsed descriptor (NULL: none): axis fields are then judged by value, not by bit */
    pl_sample_t idle; uint8_t noisy[PD_RPT_BYTES]; int have_idle;     /* filled by the IDLE phase */
    pl_base_t base;                            /* the same rest state with the axis medians (what pl_baseline will compute from the same samples) */
    plcap_t pc;                                /* the incremental press detector this capture drives */
} padmgr_cap_t;
int  padmgr_capture(void *ctx, int phase, pl_sample_t *out, int max);

/* diagnostics for the tests and the debug log */
int  padmgr_port_mode(const padmgr_t *m, int port);   /* decode mode in force on the port as the manager believes (PDM_) */
int  padmgr_port_image(const padmgr_t *m, int port, const uint8_t **img);   /* the cached wire image (length), 0 = none */
int  padmgr_port_src(const padmgr_t *m, int port);    /* PRS_ of the last resolution, 0 = none */
long padmgr_scans(const padmgr_t *m);                 /* card files opened so far */

/* the pad on `port` is quarantined (its core map must stay blank: pm_apply must call padmgr_blank instead of sending a map) */
int  padmgr_blocked(const padmgr_t *m, int port);
void padmgr_blank(padmgr_t *m, int port);             /* SET_PADMAP "none" */

/* A quarantined pad still reaches GET_DB9's menu word (the FPGA ORs both ports; the blank map only quiets joystick_0/1). When a port is
 * quarantined, the menu word is rebuilt from the REPORT raw words of the other ports instead. `raw` is fed by padmgr_note_raw (com_task).
 * Returns 1 and sets *word when the menu word must be replaced, 0 when the GET_DB9 value is right as it is. */
void padmgr_note_raw(padmgr_t *m, int port, uint32_t raw);
int  padmgr_menu_word(const padmgr_t *m, uint16_t *word);
uint16_t padmgr_menu_from_raw(uint32_t raw);          /* the fixed menu decode, written in raw bits */

/* is this built-in entry laid out like a DualShock 4 (the face buttons and the d-pad bound to the same report bits)? exposed for the tests */
int  padmgr_layout_like_ds4(const sdl_map_t *e, const sdl_map_t *ds4);

#endif
