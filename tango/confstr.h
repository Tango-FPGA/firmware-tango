// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 the Tango authors
/*
 * confstr.h -- Tango's parser for MiSTer-format core menus (CONF_STR).
 *
 * Written from menu-tango docs/CONF_STR.md (a description of the format and behaviour, read from the MiSTer
 * documentation and source). Our own code.
 *
 * The core returns its CONF_STR over SPI_SYS_READ_CFG as ';'-separated entries ending in a NUL.
 * Entry 0 is the core name, entry 1 is normally empty, and page entries start at entry 2.
 * This module turns one entry into a tagged struct, and builds the visible item list for a page.
 * It allocates nothing; all text fields point into the caller's string.
 */
#ifndef TANGO_CONFSTR_H
#define TANGO_CONFSTR_H

#include <stdint.h>
#include <stddef.h>

typedef enum {
    CS_NONE = 0,      /* empty or unknown entry: ignored */
    CS_OPTION,        /* O / o : option, value list */
    CS_TRIGGER,       /* T / t : pulse a bit */
    CS_RESET,         /* R / r : pulse a bit, then close the menu */
    CS_PAGE,          /* P#,Title : link to sub-page # */
    CS_FILE,          /* F[S][C][#],EXT[,Text][,Addr] : load a file */
    CS_MOUNT,         /* S#,EXT[,Text] : mount an image */
    CS_CHEATS,        /* C[,Text] */
    CS_DIP,           /* DIP */
    CS_TEXT,          /* -[,Text] : blank line or text line */
    /* entries that are never shown on a page */
    CS_BUTTONS,       /* J / J1 */
    CS_JN,            /* jn */
    CS_JP,            /* jp */
    CS_VERSION,       /* V,Version string */
    CS_CFGVER,        /* v,N : config version */
    CS_INFO,          /* I,... */
    CS_DEFMRA         /* DEFMRA,... */
} cs_kind_t;

typedef struct {
    const char *p;    /* start of a field inside the CONF_STR */
    uint16_t    len;  /* its length */
} cs_text_t;

typedef struct {
    cs_kind_t kind;
    /* prefixes: H/h hide, D/d disable, each against a bit of the core's 16-bit menumask */
    uint8_t   n_hd;
    struct { char op; uint8_t bit; } hd[4];   /* op is 'H','h','D','d' */
    uint8_t   page;        /* P# prefix: 0 = main page */
    /* option / trigger / reset bits */
    uint8_t   bit_lo, bit_hi; /* status bits, inclusive (0..127) */
    /* file / mount */
    uint8_t   index;       /* F#, S# (0 if none given) */
    uint8_t   f_save, f_remember;
    uint8_t   target_page; /* CS_PAGE: the page it opens */
    cs_text_t name;        /* label (option name, trigger name, page title, text line, F/S text) */
    cs_text_t ext;         /* F/S extensions, e.g. "BINGEN" */
    uint8_t   n_values;    /* CS_OPTION: number of values */
    cs_text_t values_all;  /* CS_OPTION: "V0,V1,..." (use cs_value() to pick one) */
    cs_text_t rest;        /* the unparsed remainder, for J/jn/jp/V/v/I */
} cs_entry_t;

/* Returns the number of ';'-separated entries in s (the trailing empty one after a final ';' is not counted). */
int cs_count(const char *s);

/* Returns entry i of s as a pointer + length (without the ';'). Returns 0 if i is out of range. */
int cs_get(const char *s, int i, cs_text_t *out);

/* Parses one entry. Returns 1 on success (kind may be CS_NONE for unknown entries), 0 on a malformed one. */
int cs_parse(cs_text_t entry, cs_entry_t *e);

/* Parses a status-bit index: "[e:s]" / "[b]", or the old one-/two-character form (0-9, A-V; +32 if ex).
 * Returns the number of characters consumed, or 0 on error. At most 9 bits (hi - lo <= 8). */
int cs_bits(const char *p, int n, int ex, uint8_t *lo, uint8_t *hi);

/* Picks value v (0-based) of an option. Returns 0 if v is out of range. */
int cs_value(const cs_entry_t *e, int v, cs_text_t *out);

/* Reads an option's current value from the 128-bit status word (16 bytes, bit 0 = byte 0 bit 0). */
uint32_t cs_status_get(const uint8_t status[16], uint8_t lo, uint8_t hi);
/* Writes a value into the status word (masked to the option's width). */
void cs_status_set(uint8_t status[16], uint8_t lo, uint8_t hi, uint32_t value);

/* Visibility against the core's menumask: sets *hidden and *disabled from the H/h/D/d prefixes. */
void cs_visibility(const cs_entry_t *e, uint16_t menumask, int *hidden, int *disabled);

#endif
