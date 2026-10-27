// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 the Tango authors
/*
 * tango_zip.c -- see tango_zip.h for the format spec sections each piece of this comes from, and for why this
 * is our own code and not MiSTer's. Tango, our own code.
 */
#include <string.h>
#include <stdlib.h>
#include "tango_zip.h"
#include "tango_browser.h"     /* tango_ext_match: the same CONF_STR extension-list matcher the real browser uses */
#include "third_party/miniz/miniz_tinfl.h"

#define EOCD_SIG    0x06054b50u
#define CDIR_SIG    0x02014b50u
#define LOCAL_SIG   0x04034b50u
#define EOCD_FIXED  22
#define CDIR_FIXED  46
#define LOCAL_FIXED 30
#define WINDOW_SIZE TINFL_LZ_DICT_SIZE   /* 32768, DEFLATE's maximum back-reference distance, RFC 1951 3.2.1 */

static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static int lc(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
static int casecmp(const char *a, const char *b)
{
    for (;; a++, b++) { int x = lc((unsigned char)*a), y = lc((unsigned char)*b); if (x != y) return x - y; if (!x) return 0; }
}

/* ------------------------------------------------------------------ member (streaming) state */
void tango_zip_close_member(tango_zip_t *z)
{
    free(z->inflator); z->inflator = NULL;
    free(z->window); z->window = NULL;
    z->mem_open = 0; z->mem_index = -1;
    z->mem_data_off = z->mem_comp_size = z->mem_uncomp_size = z->mem_comp_pos = z->mem_uncomp_pos = 0;
    z->mem_crc = z->mem_crc_want = 0;
    z->mem_method = 0; z->mem_done = 0;
    z->window_pos = z->pending_pos = z->pending_len = 0;
    z->in_len = z->in_pos = 0;
}

void tango_zip_close(tango_zip_t *z, const tango_zipfs_t *io)
{
    tango_zip_close_member(z);
    if (z->open) io->close(io->ctx);
    tango_zip_cache_t *cache = z->cache;   /* survives the reset below: a caller that wired its own
        * cache (mra_session_t's shared one) only ever does so once, not before every reopen -- this
        * function is also tango_zip_open_archive's own first step, so losing it there would silently
        * fall back to the default on the very next use, re-opening the CM.zip-vs-mra race this field
        * exists to close (see tango_zip.h's own header comment). */
    memset(z, 0, sizeof *z);
    z->cache = cache;
}

/* ------------------------------------------------------------------ End Of Central Directory (APPNOTE.TXT 4.3.16) */
/* The common case (no .ZIP file comment, true of every ROM zip a packaging tool makes without being asked
 * otherwise) is the EOCD's fixed 22 bytes are the file's last 22 bytes: one small read, no backward search.
 * The rare case (a comment is present, up to 65535 bytes, 4.4.1.4) is handled by scanning backward in
 * overlapping windows small enough not to trouble a FreeRTOS task's stack.
 *
 * A candidate signature is only accepted once its own comment-length field is self-consistent with the file
 * size (off + 22 + comment_len == file_size): a signature that merely appears inside an earlier comment (or
 * inside compressed data) will not satisfy this, so it is skipped and the search continues further back.
 * This is stricter than the "assumed" convention most implementations use -- including Python's own zipfile,
 * whose _EndRecData() takes the rightmost signature match unconditionally and documents the same assumption
 * this avoids ("it is assumed that the ... magic number does not appear in the comment", cpython zipfile/
 * __init__.py _EndRecData) -- tested against a zip whose comment deliberately contains a decoy
 * signature, test_zip.c's FX_COMMENT case. */
#define EOCD_WIN 1024
static int find_eocd(const tango_zipfs_t *io, uint32_t file_size, uint8_t eocd[EOCD_FIXED], uint32_t *eocd_off)
{
    if (file_size < EOCD_FIXED) return 0;
    if (io->read(io->ctx, file_size - EOCD_FIXED, eocd, EOCD_FIXED) == EOCD_FIXED && le32(eocd) == EOCD_SIG &&
        le16(eocd + 20) == 0) {
        *eocd_off = file_size - EOCD_FIXED;
        return 1;
    }
    uint32_t max_back = EOCD_FIXED + 65535u;
    if (max_back > file_size) max_back = file_size;
    uint8_t buf[EOCD_WIN];
    uint32_t end = file_size;                 /* [start, end) is this window; shrinks each pass */
    uint32_t lowest = file_size - max_back;
    while (end > lowest) {
        uint32_t w = end - lowest < EOCD_WIN ? end - lowest : EOCD_WIN;
        uint32_t start = end - w;
        if (io->read(io->ctx, start, buf, w) != (int)w) return 0;
        for (int32_t i = (int32_t)w - 4; i >= 0; i--) {    /* latest-starting candidate first */
            if (le32(buf + (uint32_t)i) != EOCD_SIG) continue;
            uint32_t off = start + (uint32_t)i;
            if (off + EOCD_FIXED > file_size || io->read(io->ctx, off, eocd, EOCD_FIXED) != EOCD_FIXED) continue;
            if (off + EOCD_FIXED + le16(eocd + 20) != file_size) continue;   /* a false positive: keep looking */
            *eocd_off = off;
            return 1;
        }
        if (w <= 3) break;             /* no further progress possible */
        end = start + 3;               /* 3-byte overlap: a signature split across the boundary is still found */
    }
    return 0;
}

int tango_zip_open_archive(tango_zip_t *z, const tango_zipfs_t *io, const char *path)
{
    tango_zip_close(z, io);
    uint32_t size;
    if (!io->open(io->ctx, path, &size)) return 0;
    z->open = 1; z->file_size = size;
    uint8_t eocd[EOCD_FIXED]; uint32_t eocd_off;
    if (!find_eocd(io, size, eocd, &eocd_off)) { tango_zip_close(z, io); return 0; }
    uint16_t total_entries = le16(eocd + 10);
    uint32_t cd_size = le32(eocd + 12), cd_off = le32(eocd + 16);
    /* ZIP64 (4.3.14/4.4.1.4): any of these fields pinned at -1 means "see the ZIP64 record instead" -- not
     * supported (console ROM zips never need it; a clean refusal here, not a silent miscompute) */
    if (total_entries == 0xFFFFu || cd_size == 0xFFFFFFFFu || cd_off == 0xFFFFFFFFu || cd_off + cd_size > size) {
        tango_zip_close(z, io);
        return 0;
    }
    return 1;   /* the archive is open; tango_zip_scan walks the central directory it just located */
}

/* ------------------------------------------------------------------ central directory scan (4.3.12) */
/* Heap margin: tango_zip.h's own header comment has the full
 * reasoning for why the matching-entry table is reached through z->cache (NULL = this one default
 * instance) instead of being embedded in every tango_zip_t by value. */
static tango_zip_cache_t g_default_zip_cache;
static tango_zip_cache_t *zc(const tango_zip_t *z) { return z->cache ? z->cache : &g_default_zip_cache; }

static int pool_add(tango_zip_t *z, const char *name, unsigned len)
{
    tango_zip_cache_t *c = zc(z);
    if (c->pool_used + len + 1 > sizeof c->pool) return 0;
    memcpy(c->pool + c->pool_used, name, len);
    c->pool[c->pool_used + len] = 0;
    return 1;
}

/* Shared by tango_zip_scan (collects every matching entry) and tango_zip_locate (keeps the best of at
 * most two candidates): walks the central directory once, calling visit() for each real file entry --
 * directory entries (4.4.17.1) and entries whose method isn't stored/deflate never reach it, since
 * neither caller can ever use one. Low-level record parsing (and its corrupt-record bounds checks) has
 * one definition this way instead of two that could drift apart. */
typedef void (*zip_visit_fn)(void *ctx, const char *name, unsigned nl, uint32_t crc, uint32_t local_off,
                              uint32_t comp_size, uint32_t uncomp_size, uint16_t method);
static void walk_central_dir(const tango_zip_t *z, const tango_zipfs_t *io, zip_visit_fn visit, void *ctx)
{
    uint8_t eocd[EOCD_FIXED]; uint32_t eocd_off;
    if (!find_eocd(io, z->file_size, eocd, &eocd_off)) return;   /* re-locate: open_archive did not keep it */
    uint32_t cd_size = le32(eocd + 12), cd_off = le32(eocd + 16);
    uint32_t pos = cd_off, end = cd_off + cd_size;
    char name[300];
    while (pos + CDIR_FIXED <= end) {
        uint8_t h[CDIR_FIXED];
        if ((uint32_t)io->read(io->ctx, pos, h, CDIR_FIXED) != CDIR_FIXED || le32(h) != CDIR_SIG) break;
        uint16_t method = le16(h + 10);
        uint32_t comp_size = le32(h + 20), uncomp_size = le32(h + 24);
        uint16_t name_len = le16(h + 28), extra_len = le16(h + 30), comment_len = le16(h + 32);
        uint32_t crc = le32(h + 16), local_off = le32(h + 42);
        uint32_t next = pos + CDIR_FIXED + name_len + extra_len + comment_len;
        if (next > end || next <= pos) break;              /* a corrupt record: stop, keep whatever matched so far */
        unsigned nl = name_len < sizeof name - 1 ? name_len : (unsigned)sizeof name - 1;
        if ((uint32_t)io->read(io->ctx, pos + CDIR_FIXED, (uint8_t *)name, nl) != nl) break;
        name[nl] = 0;
        pos = next;
        if (!nl || name[nl - 1] == '/') continue;           /* a directory entry (4.4.17.1): never matched */
        if (method != 0 && method != 8) continue;           /* only stored and deflate are read */
        visit(ctx, name, nl, crc, local_off, comp_size, uncomp_size, method);
    }
}

typedef struct { tango_zip_t *z; const char *exts; } scan_ctx_t;
static void scan_visit(void *ctx, const char *name, unsigned nl, uint32_t crc, uint32_t local_off,
                        uint32_t comp_size, uint32_t uncomp_size, uint16_t method)
{
    scan_ctx_t *sc = ctx; tango_zip_t *z = sc->z;
    if (!tango_ext_match(name, sc->exts)) return;
    if (z->n >= TANGO_ZIP_MAX_ENTRIES || !pool_add(z, name, nl)) { z->truncated = 1; return; }
    tango_zip_cache_t *c = zc(z);
    tango_zip_entry_t *e = &c->e[z->n++];
    e->local_header_off = local_off; e->comp_size = comp_size; e->uncomp_size = uncomp_size;
    e->crc32 = crc; e->method = method; e->name = (uint16_t)(c->pool_used);
    c->pool_used += nl + 1;
}

int tango_zip_scan(tango_zip_t *z, const tango_zipfs_t *io, const char *exts)
{
    if (!z->open) return 0;
    z->n = 0; z->truncated = 0; zc(z)->pool_used = 0;
    scan_ctx_t sc = { z, exts };
    walk_central_dir(z, io, scan_visit, &sc);
    return z->n;
}

typedef struct {
    const char *want_name; uint32_t want_crc;
    int found_crc, found_name;
    char crc_name[300], name_name[300];
    tango_zip_entry_t crc_e, name_e;
} locate_ctx_t;
static void locate_visit(void *ctx, const char *name, unsigned nl, uint32_t crc, uint32_t local_off,
                          uint32_t comp_size, uint32_t uncomp_size, uint16_t method)
{
    locate_ctx_t *L = ctx;
    if (L->want_crc && !L->found_crc && crc == L->want_crc) {
        L->found_crc = 1;
        L->crc_e = (tango_zip_entry_t){ .local_header_off = local_off, .comp_size = comp_size,
            .uncomp_size = uncomp_size, .crc32 = crc, .method = method, .name = 0 };
        memcpy(L->crc_name, name, nl + 1);
    }
    if (!L->found_name && L->want_name && !casecmp(name, L->want_name)) {
        L->found_name = 1;
        L->name_e = (tango_zip_entry_t){ .local_header_off = local_off, .comp_size = comp_size,
            .uncomp_size = uncomp_size, .crc32 = crc, .method = method, .name = 0 };
        memcpy(L->name_name, name, nl + 1);
    }
}

int tango_zip_locate(tango_zip_t *z, const tango_zipfs_t *io, const char *name, uint32_t crc)
{
    if (!z->open) return 0;
    z->n = 0; z->truncated = 0; zc(z)->pool_used = 0;
    locate_ctx_t L; memset(&L, 0, sizeof L); L.want_name = name; L.want_crc = crc;
    walk_central_dir(z, io, locate_visit, &L);

    const tango_zip_entry_t *pick = NULL; const char *pick_name = NULL;
    if (crc && L.found_crc) { pick = &L.crc_e; pick_name = L.crc_name; }
    else if (L.found_name) { pick = &L.name_e; pick_name = L.name_name; }
    if (!pick) return 0;

    unsigned nl = (unsigned)strlen(pick_name);
    if (!pool_add(z, pick_name, nl)) return 0;   /* can't happen: the pool is 2 KB, one name always fits */
    zc(z)->e[0] = *pick; zc(z)->e[0].name = 0; z->n = 1;
    return 1;
}

const char *tango_zip_name(const tango_zip_t *z, int i) { return zc(z)->pool + zc(z)->e[i].name; }
uint32_t tango_zip_crc32(const tango_zip_t *z, int i) { return zc(z)->e[i].crc32; }

int tango_zip_find(const tango_zip_t *z, const char *name)
{
    for (int i = 0; i < z->n; i++) if (!casecmp(tango_zip_name(z, i), name)) return i;
    return -1;
}

/* ------------------------------------------------------------------ opening one member (4.3.7) */
int tango_zip_open_member(tango_zip_t *z, const tango_zipfs_t *io, int i, uint32_t *uncomp_size)
{
    tango_zip_close_member(z);
    if (i < 0 || i >= z->n) return -1;
    const tango_zip_entry_t *e = &zc(z)->e[i];
    uint8_t h[LOCAL_FIXED];
    if ((uint32_t)io->read(io->ctx, e->local_header_off, h, LOCAL_FIXED) != LOCAL_FIXED || le32(h) != LOCAL_SIG) return -1;
    uint16_t name_len = le16(h + 26), extra_len = le16(h + 28);
    uint32_t data_off = e->local_header_off + LOCAL_FIXED + name_len + extra_len;
    if (data_off + e->comp_size > z->file_size) return -1;
    z->mem_index = i; z->mem_data_off = data_off;
    z->mem_comp_size = e->comp_size; z->mem_uncomp_size = e->uncomp_size;
    z->mem_crc_want = e->crc32; z->mem_crc = 0xFFFFFFFFu;
    z->mem_method = e->method;
    if (e->method == 8) {
        z->inflator = malloc(sizeof(tinfl_decompressor));
        z->window = malloc(WINDOW_SIZE);
        if (!z->inflator || !z->window) { tango_zip_close_member(z); return -1; }
        tinfl_init((tinfl_decompressor *)z->inflator);
    }
    z->mem_open = 1;
    if (uncomp_size) *uncomp_size = e->uncomp_size;
    return 0;
}

/* ------------------------------------------------------------------ CRC-32 (the zip format's, = Ethernet/gzip's) */
static uint32_t crc32_add(uint32_t crc, const uint8_t *buf, unsigned n)
{
    for (unsigned i = 0; i < n; i++) {
        crc ^= buf[i];
        for (int b = 0; b < 8; b++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
    }
    return crc;
}

/* ------------------------------------------------------------------ streaming read (tango_dl_pump's rd() shape) */
static int read_stored(tango_zip_t *z, const tango_zipfs_t *io, uint8_t *buf, unsigned n)
{
    uint32_t remain = z->mem_comp_size - z->mem_comp_pos;      /* comp_size == uncomp_size for method 0 */
    if (n > remain) n = remain;
    if (!n) return 0;
    int got = io->read(io->ctx, z->mem_data_off + z->mem_comp_pos, buf, n);
    if (got <= 0) return -1;
    z->mem_comp_pos += (uint32_t)got; z->mem_uncomp_pos += (uint32_t)got;
    z->mem_crc = crc32_add(z->mem_crc, buf, (unsigned)got);
    if (z->mem_uncomp_pos >= z->mem_uncomp_size && (z->mem_crc ^ 0xFFFFFFFFu) != z->mem_crc_want) return -1;
    return got;
}

/* tinfl_decompress's windowed mode requires the output span passed on every call to reach exactly the
 * dictionary's end (miniz_tinfl.c's power-of-two check) -- it has no notion of "produce at most N bytes", so
 * whatever it produces beyond what the caller asked for is kept in z->pending_* and delivered first on later
 * calls, before tinfl is invoked again (never while bytes it already produced are still undelivered: that
 * would let a later call's write lap them before they are read out). Checked against real deflate streams,
 * including window-wrapping ones and deliberately uneven, non-power-of-two request sizes. */
static int read_deflate(tango_zip_t *z, const tango_zipfs_t *io, uint8_t *buf, unsigned n)
{
    unsigned produced = 0;
    while (produced < n) {
        if (z->pending_len == 0) {
            if (z->mem_done) break;
            if (z->in_pos >= z->in_len) {
                int got = io->read(io->ctx, z->mem_data_off + z->mem_comp_pos, z->inbuf, sizeof z->inbuf);
                if (got < 0) return -1;
                uint32_t remain = z->mem_comp_size - z->mem_comp_pos;
                if ((uint32_t)got > remain) got = (int)remain;        /* never feed tinfl past this member's data */
                z->in_len = (unsigned)got; z->in_pos = 0;
                if (!got && z->mem_comp_pos < z->mem_comp_size) return -1;   /* truncated archive */
            }
            size_t in_avail = z->in_len - z->in_pos;
            size_t out_avail = WINDOW_SIZE - z->window_pos;
            tinfl_status st = tinfl_decompress((tinfl_decompressor *)z->inflator, z->inbuf + z->in_pos, &in_avail,
                                                z->window, z->window + z->window_pos, &out_avail,
                                                TINFL_FLAG_HAS_MORE_INPUT);
            z->in_pos += (unsigned)in_avail; z->mem_comp_pos += (uint32_t)in_avail;
            z->pending_pos = z->window_pos; z->pending_len = (unsigned)out_avail;
            z->mem_crc = crc32_add(z->mem_crc, z->window + z->window_pos, (unsigned)out_avail);
            z->window_pos += (unsigned)out_avail;
            if (z->window_pos >= WINDOW_SIZE) z->window_pos = 0;
            if (st < TINFL_STATUS_DONE) return -1;                 /* corrupt deflate stream */
            if (st == TINFL_STATUS_DONE) {
                z->mem_done = 1;
                if ((z->mem_crc ^ 0xFFFFFFFFu) != z->mem_crc_want) return -1;
            }
            if (z->pending_len == 0 && !z->mem_done) continue;     /* no output yet: refill input and retry */
        }
        unsigned take = z->pending_len < n - produced ? z->pending_len : n - produced;
        memcpy(buf + produced, z->window + z->pending_pos, take);
        z->pending_pos += take; z->pending_len -= take; produced += take;
        z->mem_uncomp_pos += take;
    }
    return (int)produced;
}

int tango_zip_read(tango_zip_t *z, const tango_zipfs_t *io, uint8_t *buf, unsigned n)
{
    if (!z->mem_open) return -1;
    if (z->mem_uncomp_pos >= z->mem_uncomp_size) return 0;
    if (n > z->mem_uncomp_size - z->mem_uncomp_pos) n = z->mem_uncomp_size - z->mem_uncomp_pos;
    if (!n) return 0;
    return z->mem_method == 0 ? read_stored(z, io, buf, n) : read_deflate(z, io, buf, n);
}
