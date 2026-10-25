// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 the Tango authors
/*
 * fake_zip.h -- a tango_zipfs_t over an in-memory buffer (a real .zip file's bytes), for host tests.
 * Tango, our own code.
 */
#ifndef FAKE_ZIP_H
#define FAKE_ZIP_H
#include "../tango_zip.h"

typedef struct {
    const unsigned char *data;
    uint32_t size;
    int opened;
    int open_calls, read_calls, close_calls;   /* so tests can check close/open discipline across cycles */
    int fail_open;                             /* simulate open() failing (card removed, etc.) */
    int fail_after_calls;                      /* simulate an I/O error from this read() call on (0 = never); a
                                                 * torn SD read partway through, not a bad file -- callers read
                                                 * `read_calls` after a known-good phase (e.g. open+scan) and set
                                                 * this just past it, so only later reads (the member's own data)
                                                 * are the ones that start failing */
} fake_zipmem_t;

void fake_zipmem_init(fake_zipmem_t *m, tango_zipfs_t *io, const unsigned char *data, unsigned len);

#endif
