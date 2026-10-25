// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 the Tango authors
/*
 * tango_padres.h -- the LOOKUP LADDER: from "a pad was plugged in" to a decode table, with a real fallback at every step.
 * Tango, our own code.
 *
 * Order (first usable step wins; the reason is kept for the on-screen "Controller" line and the debug log):
 *   1 PRS_USER_DEC   a decode the user defined for this very pad (Define buttons, system mode)          saved decode file
 *   2 PRS_USER_DB    gamecontrollerdb_user.txt  (exact bcdDevice, else version 0, else the vendor:product): the user's own line wins
 *                    over EVERYTHING below, a DualShock 4's included
 *   3 PRS_DS4        a DualShock 4 family id: the FPGA's built-in decode, silicon-proven (mode PDM_DS4)   [no table to send]
 *   4 PRS_XINPUT     interface class FF/5D/01 (Xbox 360 wired): the fixed XInput layout, no descriptor needed
 *   5 PRS_DB         gamecontrollerdb.txt on the card, same tiers
 *   6 PRS_BUILTIN    the compile-time table (the same three tiers) -- ONLY when the card file could not be opened
 *                    (the no-card fallback); a card file that opened and has no line for the pad is the answer "unknown"
 *   7 PRS_HEURISTIC  the report descriptor alone (pd_from_layout)
 *   8 PRS_LEGACY     nothing is known and there is no descriptor: PDM_DS4, as the FPGA behaves today
 *     PRS_QUIET      known not to be a gamepad (mouse, keyboard, consumer keys), or known to be a pad whose layout we cannot decode
 *                    yet (a database entry but no descriptor): the raw word stays 0 rather than random bits
 * The DS4 and XInput shortcuts sit ABOVE the card file and the built-in table on purpose: SDL's lines for a DualShock 4 give L2/R2 from
 * the analog triggers and have no touchpad, and the kernel-driver lines number the buttons differently; the silicon-proven decode wins unless the
 * user wrote a line of his own.
 * Steps 4-6 need a descriptor layout (their indices mean "the Nth button of the descriptor"). A database entry whose indices do not
 * fit the descriptor (a button index the pad does not have) is rejected and the ladder goes on: that is the safety net for lines
 * written for a kernel driver's numbering.
 */
#ifndef TANGO_PADRES_H
#define TANGO_PADRES_H

#include "tango_paddec.h"

enum { PRS_NONE = 0, PRS_USER_DEC, PRS_DS4, PRS_XINPUT, PRS_USER_DB, PRS_DB, PRS_BUILTIN, PRS_HEURISTIC, PRS_LEGACY, PRS_QUIET };

typedef struct {
    void *ctx;
    /* a text source: which 0 = gamecontrollerdb_user.txt, 1 = gamecontrollerdb.txt. open returns 0 if the file is absent;
     * line returns the next line (without the terminator needed) or NULL at the end. */
    int         (*open)(void *ctx, int which);
    const char *(*line)(void *ctx, int which);
    void        (*close)(void *ctx, int which);
    /* the saved decode of this pad, if any */
    int         (*load_dec)(void *ctx, uint16_t vid, uint16_t pid, uint16_t ver, pd_table_t *out);
    /* the compile-time table: returns the rank (PM_) and the entry as an sdl_map_t, PM_NONE if absent */
    int         (*builtin)(void *ctx, uint16_t vid, uint16_t pid, uint16_t ver, sdl_map_t *out);
} pad_sources_t;

typedef struct {
    uint16_t vid, pid, ver;                  /* ver: bcdDevice */
    uint16_t ver_hid;                        /* bcdHID of the HID interface (the evdev input version of a Linux-made line), 0 unknown */
    int      if_class, if_sub, if_proto;     /* the first interface's class triplet (config descriptor bytes 14-16), -1 unknown */
    const hp_layout_t *L;                    /* the parsed report descriptor, NULL if not fetched */
    int      L_cls;                          /* hp_parse's cls when L is NULL or not a pad (HPC_) */
    const char *core;                        /* for mistercore: filters */
    const uint8_t *rest; int rest_len;       /* the first report seen (taken to be the pad at rest), NULL if none: the descriptor-only table uses it to tell a stick (rests in the middle) from a trigger (rests at an end) */
} pad_query_t;

typedef struct {
    int      src;                            /* PRS_ */
    int      rank;                           /* PM_ for the database steps */
    pd_table_t table;
    pd_build_info_t info;
    char     name[40];                       /* the database name when there is one */
    char     why[96];
    int      skipped_unusable;               /* database steps that matched but were rejected */
} pad_resolution_t;

int pad_is_ds4(uint16_t vid, uint16_t pid);

/* `cache` is two caches (0: the user file, 1: the card file and the built-in table) or NULL. Returns res->src. */
int pad_resolve(const pad_sources_t *s, pdb_cache_t cache[2], const pad_query_t *q, pad_resolution_t *res);

#endif
