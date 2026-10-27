// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 the Tango authors
/* fake_zip.c -- see fake_zip.h. Tango, our own code. */
#include <string.h>
#include "fake_zip.h"

static int fz_open(void *ctx, const char *path, uint32_t *size)
{
    (void)path;
    fake_zipmem_t *m = ctx;
    m->open_calls++;
    if (m->fail_open) return 0;
    m->opened = 1;
    *size = m->size;
    return 1;
}
static int fz_read(void *ctx, uint32_t off, uint8_t *buf, unsigned n)
{
    fake_zipmem_t *m = ctx;
    m->read_calls++;
    if (m->fail_after_calls && m->read_calls >= m->fail_after_calls) return -1;
    if (!m->opened || off > m->size) return -1;
    unsigned avail = m->size - off;
    if (n > avail) n = avail;
    memcpy(buf, m->data + off, n);
    return (int)n;
}
static void fz_close(void *ctx) { fake_zipmem_t *m = ctx; m->close_calls++; m->opened = 0; }

void fake_zipmem_init(fake_zipmem_t *m, tango_zipfs_t *io, const unsigned char *data, unsigned len)
{
    memset(m, 0, sizeof *m);
    m->data = data; m->size = len;
    io->open = fz_open; io->read = fz_read; io->close = fz_close; io->ctx = m;
}
