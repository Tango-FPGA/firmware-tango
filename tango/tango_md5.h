// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 the Tango authors
/*
 * tango_md5.h -- MD5 (RFC 1321), needed for the .mra discard-on-mismatch rule: MiSTer hashes
 * each <rom>'s assembled bytes (as read from its parts, before any <patch>) and compares against the
 * <rom md5="..."> attribute, discarding the whole rom rather than sending it on a mismatch
 * (mra_loader.cpp's rom_finish(checksumsame, ...)).
 *
 * Tango. Our own code, written directly from RFC 1321's specification (the algorithm
 * is a fixed public specification, not MiSTer's expression of it -- this is not lib/md5/md5.h ported,
 * it's the same standard both independently implement). Verified against RFC 1321's own published
 * test vectors (the empty string, "abc", and the alphabet/digit strings) in test_md5.c.
 */
#ifndef TANGO_MD5_H
#define TANGO_MD5_H

#include <stdint.h>

typedef struct {
    uint32_t a, b, c, d;
    uint64_t total_len;          /* bytes fed so far, for the final bit-length field */
    uint8_t  buf[64];
    unsigned buf_len;             /* 0..63 bytes buffered, waiting for a full 64-byte block */
} tango_md5_t;

void tango_md5_init(tango_md5_t *m);
void tango_md5_update(tango_md5_t *m, const void *data, unsigned len);
void tango_md5_final(tango_md5_t *m, uint8_t digest[16]);   /* digest is the raw 16 bytes, not hex text */

/* digest (16 raw bytes) vs. a 32-character lowercase-or-uppercase hex string (a <rom md5="..."> or
 * <nvram>/cheat save attribute) -- "none" and "" are never a match here (the caller checks those
 * itself; MiSTer's own no_checksum test, mra_loader.cpp 782). 1 = equal. */
int tango_md5_eq_hex(const uint8_t digest[16], const char *hex32);

#endif
