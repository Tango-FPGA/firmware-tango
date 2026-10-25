// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 the Tango authors
/*
 * tango_paddb.h -- the controller database: SDL / MiSTer "gamecontrollerdb" lines, and a lookup that has a fallback.
 *
 * Tango. Our own code. It reads the SAME text format as MiSTer's linux/gamecontrollerdb/gamecontrollerdb.txt
 * (MiSTer's source was read, none of it is copied):
 *   GUID,name,sdlname:token,...,platform:X,
 *   tokens: bN button N | aN axis N | +aN / -aN half axis | hN.D hat N, direction mask D (1 up, 2 right, 4 down, 8 left)
 *           a trailing '~' inverts an axis.  MiSTer's extras (guide2, menufunc, menuok, menuesc, asysx, asysy, platform:MiSTer,
 *           mistercore:) are read and ignored or honoured as MiSTer does, so a MiSTer user's files work unchanged.
 * Differences from MiSTer, on purpose:
 *   - the lookup has TIERS: exact vendor:product:bcdDevice, else the same vendor:product with version 0 (SDL's own second try),
 *     else any line for the vendor:product (MiSTer has none of this: its GUID must match exactly);
 *   - a file is scanned once per (vendor, product, version) and the answer, including "nothing", is cached;
 *   - lines without "platform:" are accepted in the USER file (MiSTer ignores them);
 *   - a line whose version is 0x8000 or more is a Linux KERNEL-DRIVER numbering (hid-sony, hid-nintendo set the input version to 0x8111 and
 *     number the buttons their own way): it applies to that exact version only, never as a fallback for a pad with another bcdDevice.
 */
#ifndef TANGO_PADDB_H
#define TANGO_PADDB_H

#include <stdint.h>

enum { SN_A, SN_B, SN_X, SN_Y, SN_BACK, SN_GUIDE, SN_START, SN_LSTICK, SN_RSTICK, SN_LSHOULDER, SN_RSHOULDER,
       SN_DPUP, SN_DPDOWN, SN_DPLEFT, SN_DPRIGHT, SN_LEFTX, SN_LEFTY, SN_RIGHTX, SN_RIGHTY, SN_LTRIGGER, SN_RTRIGGER,
       SN_MISC1, SN_COUNT };
enum { SB_NONE = 0, SB_BTN, SB_AXIS, SB_AXIS_POS, SB_AXIS_NEG, SB_HAT };
enum { PL_NONE = 0, PL_LINUX, PL_MISTER, PL_TANGO, PL_OTHER };

typedef struct { uint8_t type, idx, hatmask, inv; } sdl_bind_t;

typedef struct {
    sdl_bind_t b[SN_COUNT];
    uint16_t   bus, vid, pid, ver;       /* the GUID's: bus 3 = USB */
    uint8_t    has_ver;                /* the GUID carried a version (28 or more hex digits) */
    uint8_t    plat;                   /* PL_ */
    uint8_t    nignored;               /* tokens read and ignored (paddles, MiSTer extras, unknown names) */
    char       name[40];
    char       cores[96];              /* mistercore: / tangocore: patterns, ',' separated, "" = any core */
} sdl_map_t;

/* the canonical sdl names (for tests and the on-screen list) */
const char *sdl_name(int sn);

/* Parse one line. Returns 1 for a mapping line (even one with no usable token), 0 for a comment, a blank line or a bad GUID. */
int sdl_parse_line(const char *line, sdl_map_t *m);

/* The line's platform / core filter: 1 if it applies to Tango running `core`.
 * Linux lines always apply; MiSTer and Tango lines apply when they carry no core filter or one matches (a pattern matches when
 * it is a case-insensitive prefix of the core name; a '*' ends the pattern); other platforms never; no platform only if lenient. */
int sdl_applies(const sdl_map_t *m, const char *core, int lenient);

/* ---- the scan of one text source (a file read line by line, or a memory block) ---- */
enum { PM_NONE = 0, PM_VIDPID = 1, PM_VER0 = 2, PM_EXACT = 3 };    /* match quality, higher wins */
typedef struct {
    uint16_t vid, pid, ver;
    const char *core; int lenient;
    int      rank;                     /* best match so far (PM_) */
    int      have;
    sdl_map_t best;
    int      lines;                    /* mapping lines seen (for tests) */
    uint16_t ver2;                         /* an alternative exact version (see pdb_scan_set_ver2), 0 none */
} pdb_scan_t;

void pdb_scan_begin(pdb_scan_t *s, uint16_t vid, uint16_t pid, uint16_t ver, const char *core, int lenient);
/* a second version that also counts as an exact match (0: none). The GUID version of a Linux-made line is the evdev input version, which for a USB HID pad is the
 * HID class version (bcdHID) of the interface; lines made on other systems carry bcdDevice.  MiSTer compares the evdev one only (input.cpp 5206, with a bcdDevice
 * exception for two adapters, 5560); Tango compares both. */
void pdb_scan_set_ver2(pdb_scan_t *s, uint16_t ver2);
void pdb_scan_line(pdb_scan_t *s, const char *line);               /* consider one line: the LAST line of the best rank wins */
int  pdb_scan_result(const pdb_scan_t *s, sdl_map_t *out);         /* returns the rank (PM_NONE: nothing) */

/* a text block in memory, line by line (tests; the firmware feeds f_gets lines) */
void pdb_scan_text(pdb_scan_t *s, const char *text);

/* ---- the answer cache: positive AND negative answers, so an unknown pad is looked up once ---- */
#define PDB_CACHE 8
typedef struct {
    struct { uint16_t vid, pid, ver; uint8_t used, rank, src; sdl_map_t map; } e[PDB_CACHE];
    int next;
} pdb_cache_t;
void pdb_cache_clear(pdb_cache_t *c);
/* returns 1 if cached (then *rank, *src, and *map when rank != PM_NONE are set) */
int  pdb_cache_get(const pdb_cache_t *c, uint16_t vid, uint16_t pid, uint16_t ver, int *rank, int *src, sdl_map_t *map);
void pdb_cache_put(pdb_cache_t *c, uint16_t vid, uint16_t pid, uint16_t ver, int rank, int src, const sdl_map_t *map);

#endif
