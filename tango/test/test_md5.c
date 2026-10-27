// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 the Tango authors
/*
 * test_md5.c -- tango_md5.c against RFC 1321 Appendix A.5's own published test suite, verbatim, plus
 * a chunked (byte-at-a-time) update to exercise the block-boundary carry logic separately from one
 * big call. Needed before trusting this on any real .mra <rom md5="..."> check.
 * Tango, our own code.
 */
#include <stdio.h>
#include <string.h>
#include "../tango_md5.h"

static int fails = 0, checks = 0;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)

static void check_str(const char *msg, const char *want_hex)
{
    tango_md5_t m; tango_md5_init(&m);
    tango_md5_update(&m, msg, (unsigned)strlen(msg));
    uint8_t d[16]; tango_md5_final(&m, d);
    char hex[33]; for (int i = 0; i < 16; i++) sprintf(hex + i * 2, "%02x", d[i]);
    CHECK(!strcmp(hex, want_hex));
    CHECK(tango_md5_eq_hex(d, want_hex));
}

int main(void)
{
    check_str("", "d41d8cd98f00b204e9800998ecf8427e");
    check_str("a", "0cc175b9c0f1b6a831c399e269772661");
    check_str("abc", "900150983cd24fb0d6963f7d28e17f72");
    check_str("message digest", "f96b697d7cb7938d525a2f31aaf161d0");
    check_str("abcdefghijklmnopqrstuvwxyz", "c3fcd3d76192e4007dfb496cca67e13b");
    check_str("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789",
              "d174ab98d277d9f5a5611c2c9f419d9f");
    check_str("12345678901234567890123456789012345678901234567890123456789012345678901234567890",
              "57edf4a22be3c955ac49da2e2107b67a");

    /* chunked, one byte at a time, to exercise the block-boundary carry separately from one big call */
    {
        tango_md5_t m; tango_md5_init(&m);
        const char *s = "The quick brown fox jumps over the lazy dog";
        for (size_t i = 0; i < strlen(s); i++) tango_md5_update(&m, s + i, 1);
        uint8_t d[16]; tango_md5_final(&m, d);
        CHECK(tango_md5_eq_hex(d, "9e107d9d372bb6826bd81d3542a419d6"));
    }

    /* a message that lands exactly on a 64-byte block boundary (56 mod 64, where padding alone
     * would overflow into a second block) -- the trickiest case in the RFC's own padding rule */
    {
        char msg[56]; memset(msg, 'x', sizeof msg);
        tango_md5_t m; tango_md5_init(&m);
        tango_md5_update(&m, msg, sizeof msg);
        uint8_t d[16]; tango_md5_final(&m, d);
        /* cross-checked against Python's hashlib (an independent implementation), not asserted from
         * this file's own output */
        CHECK(tango_md5_eq_hex(d, "668a72d5ba17f08e62dabcafad6db14b"));
    }

    /* eq_hex: case-insensitive, and a malformed/short hex string never matches */
    {
        tango_md5_t m; tango_md5_init(&m);
        tango_md5_update(&m, "abc", 3);
        uint8_t d[16]; tango_md5_final(&m, d);
        CHECK(tango_md5_eq_hex(d, "900150983CD24FB0D6963F7D28E17F72"));   /* uppercase hex */
        CHECK(!tango_md5_eq_hex(d, "900150983cd24fb0d6963f7d28e17f73"));  /* one bit off */
        CHECK(!tango_md5_eq_hex(d, "900150983cd24fb0d6963f7d28e17f7"));   /* truncated */
        CHECK(!tango_md5_eq_hex(d, "none"));                              /* MiSTer's own sentinel string */
    }

    printf("%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
