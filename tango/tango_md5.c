// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 the Tango authors
/*
 * tango_md5.c -- see tango_md5.h. Tango, our own code, from RFC 1321.
 */
#include <string.h>
#include "tango_md5.h"

#define F(x, y, z) (((x) & (y)) | (~(x) & (z)))
#define G(x, y, z) (((x) & (z)) | ((y) & ~(z)))
#define H(x, y, z) ((x) ^ (y) ^ (z))
#define I(x, y, z) ((y) ^ ((x) | ~(z)))
#define ROTL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

/* RFC 1321 section 3.4's T[1..64] (floor(2^32 * abs(sin(i)))) and per-round shift amounts, and the
 * message-word order each round reads (K[i] in most expositions) -- all fixed by the specification,
 * not a design choice. */
static const uint32_t T[64] = {
    0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,
    0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,
    0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
    0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
    0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
    0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
    0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,
    0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391,
};
static const int S[64] = {
    7,12,17,22, 7,12,17,22, 7,12,17,22, 7,12,17,22,
    5, 9,14,20, 5, 9,14,20, 5, 9,14,20, 5, 9,14,20,
    4,11,16,23, 4,11,16,23, 4,11,16,23, 4,11,16,23,
    6,10,15,21, 6,10,15,21, 6,10,15,21, 6,10,15,21,
};
static const int K[64] = {
     0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,15,
     1, 6,11, 0, 5,10,15, 4, 9,14, 3, 8,13, 2, 7,12,
     5, 8,11,14, 1, 4, 7,10,13, 0, 3, 6, 9,12,15, 2,
     0, 7,14, 5,12, 3,10, 1, 8,15, 6,13, 4,11, 2, 9,
};

static void process_block(tango_md5_t *m, const uint8_t block[64])
{
    uint32_t X[16];
    for (int i = 0; i < 16; i++)
        X[i] = (uint32_t)block[i*4] | (uint32_t)block[i*4+1] << 8 |
               (uint32_t)block[i*4+2] << 16 | (uint32_t)block[i*4+3] << 24;

    uint32_t a = m->a, b = m->b, c = m->c, d = m->d;
    for (int i = 0; i < 64; i++) {
        uint32_t f;
        if (i < 16) f = F(b, c, d);
        else if (i < 32) f = G(b, c, d);
        else if (i < 48) f = H(b, c, d);
        else f = I(b, c, d);
        uint32_t tmp = d;
        d = c; c = b;
        b = b + ROTL(a + f + T[i] + X[K[i]], S[i]);
        a = tmp;
    }
    m->a += a; m->b += b; m->c += c; m->d += d;
}

void tango_md5_init(tango_md5_t *m)
{
    m->a = 0x67452301; m->b = 0xefcdab89; m->c = 0x98badcfe; m->d = 0x10325476;
    m->total_len = 0; m->buf_len = 0;
}

void tango_md5_update(tango_md5_t *m, const void *data, unsigned len)
{
    const uint8_t *p = data;
    m->total_len += len;
    if (m->buf_len) {
        unsigned take = 64 - m->buf_len;
        if (take > len) take = len;
        memcpy(m->buf + m->buf_len, p, take);
        m->buf_len += take; p += take; len -= take;
        if (m->buf_len == 64) { process_block(m, m->buf); m->buf_len = 0; }
    }
    while (len >= 64) { process_block(m, p); p += 64; len -= 64; }
    if (len) { memcpy(m->buf, p, len); m->buf_len = len; }
}

void tango_md5_final(tango_md5_t *m, uint8_t digest[16])
{
    uint64_t bit_len = m->total_len * 8;
    uint8_t pad = 0x80;
    tango_md5_update(m, &pad, 1);
    uint8_t zero = 0;
    while (m->buf_len != 56) tango_md5_update(m, &zero, 1);
    uint8_t lenbuf[8];
    for (int i = 0; i < 8; i++) lenbuf[i] = (uint8_t)(bit_len >> (8 * i));
    /* append the length directly into the final block ourselves (not via tango_md5_update, which
     * would recompute total_len and could re-trigger the pad/zero loop above) */
    memcpy(m->buf + 56, lenbuf, 8);
    process_block(m, m->buf);

    uint32_t words[4] = { m->a, m->b, m->c, m->d };
    for (int i = 0; i < 4; i++) {
        digest[i*4]   = (uint8_t)(words[i]);
        digest[i*4+1] = (uint8_t)(words[i] >> 8);
        digest[i*4+2] = (uint8_t)(words[i] >> 16);
        digest[i*4+3] = (uint8_t)(words[i] >> 24);
    }
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int tango_md5_eq_hex(const uint8_t digest[16], const char *hex32)
{
    for (int i = 0; i < 16; i++) {
        int hi = hexval(hex32[i*2]), lo = hexval(hex32[i*2 + 1]);
        if (hi < 0 || lo < 0) return 0;
        if ((uint8_t)((hi << 4) | lo) != digest[i]) return 0;
    }
    return hex32[32] == 0;
}
