// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 the Tango authors
/*
 * tango_zip.h -- read one ROM out of a .zip file on the card: the container (End Of Central Directory,
 * central directory, local file header) and a streaming decompressor, so the existing download path
 * (tango_dl_pump, tango_coremenu.h) can pull decompressed bytes exactly as it pulls raw file bytes today.
 *
 * Tango. Our own code, written from the public .ZIP format spec -- PKWARE's "APPNOTE.TXT .ZIP
 * File Format Specification", version 6.3.9 (pkware.cachefly.net/webdocs/APPNOTE/APPNOTE-6.3.9.TXT), sections
 * 4.3.7 (local file header), 4.3.12 (central directory structure) and 4.3.16 (end of central directory record) --
 * not from MiSTer's file_io.cpp, which uses a different library (miniz's mz_zip_reader_*) and a different design
 * (MiSTer's user-facing zip behaviour is matched feature for feature; the implementation is
 * our own, shaped for this platform, not a port of theirs).
 *
 * Scope (console zips only; arcade/MAME-style multi-file romsets are a separate, larger feature):
 * exactly one archive open at a time (menu-tango docs/TANGO-SPEC.md's SDC IMAGE download is one-at-a-time
 * too), entries matched by extension only (tango_browser.c's tango_ext_match, the same list a core's F entry
 * already uses),
 * ignoring any folder path inside the zip (a flat match -- console ROM zips are the common single-file case;
 * MiSTer's internal-folder navigation, file_io.cpp's GetRelativeFileName/IsInSameFolder, is not reproduced).
 * No ZIP64 (console ROM zips are always small); no writing (a zip is always opened read-only, matching
 * MiSTer's own rule, file_io.cpp's FileOpenEx: "writing to zipped files is not supported").
 *
 * RAM (no big static buffers: they starve the heap): the 32 KiB inflate window DEFLATE's maximum
 * back-reference distance requires (RFC 1951 section 3.2.1) is malloc'd only while a member is open for
 * streaming (tango_zip_open_member) and freed by tango_zip_close_member/tango_zip_close -- never kept when idle.
 * The archive's own state (the matching-entry index) is a small fixed-cap table, the same budgeted shape as
 * tango_browser_t (tango_browser.h): a cap on matching entries plus a name pool, not an unbounded list.
 *
 * Heap margin: the matching-entry table (TANGO_ZIP_MAX_ENTRIES=64 entries + a 2 KB name pool, ~3.3 KB)
 * is not embedded in tango_zip_t by value. A real .mra ROM load (tango_fw.c's mra_session_t) needs up to
 * TANGO_MRA_MAX_ZIPS=4 of these open AT ONCE (one per fallback zip), even though tango_mra.c's own
 * open_file_part only ever uses ONE of them (via tango_zip_locate, which needs exactly one entry) at any
 * instant -- the other 3 are idle at that moment, just not idle for LONG (the next <part> might need any of
 * them). So the table is owned through an explicit `cache` pointer (below): mra_session_t gives its own 4
 * zip instances ONE shared tango_zip_cache_t of their own (4 embedded copies collapse to 1, saving
 * ~9.9 KB net once that one shared copy's own ~3.3 KB is added back in), while every OTHER caller
 * (tango_coremenu.h's CM.zip, every host test) leaves `cache` NULL and falls back to tango_zip.c's own
 * single global default -- self-contained, and NOT the same object mra_session_t's own zips use, on
 * purpose: CM.zip's own scan results survive across many later redraws/scroll events while the user
 * browses Files, and a core's own `.mra` ROM-0 auto-download can genuinely still be streaming in the
 * background at the same moment (the OSD is open at core start, and the download spans many later SDC
 * interrupts). ONE table shared between the two would let Files-browsing silently corrupt (or be
 * corrupted by) an in-flight .mra download's own locate calls, exactly the class of bug tango_fw.c's own
 * arc_fil comment already names and avoids elsewhere, for a different shared resource. Explicit, separate
 * ownership removes that risk outright rather than detecting it after the fact. */
#ifndef TANGO_ZIP_H
#define TANGO_ZIP_H

#include <stdint.h>

#define TANGO_ZIP_MAX_ENTRIES 64     /* matching entries kept per archive */
#define TANGO_ZIP_POOL        2048   /* their names, pooled once each (tango_browser.h's pattern) */

/* Byte-addressable access to one file on the card. The firmware wraps FatFs (its own FIL, separate from the
 * save slots and the plain-file download path); host tests use a fake backed by an in-memory buffer.
 * `path` is relative to the card root (tango_savefs_t's convention, not tango_fs_t's directory-listing one). */
typedef struct {
    int  (*open)(void *ctx, const char *path, uint32_t *size);        /* 1 = ok (*size set), 0 = not found/error */
    int  (*read)(void *ctx, uint32_t off, uint8_t *buf, unsigned n);  /* bytes read (n unless at EOF), -1 = error */
    void (*close)(void *ctx);
    void  *ctx;
} tango_zipfs_t;

typedef struct {
    uint32_t local_header_off;  /* the central directory's claim; tango_zip_open_member re-reads the local
                                  * header there to find the true compressed-data start (its name/extra field
                                  * lengths are not required to match the central directory's, APPNOTE.TXT 4.3.7) */
    uint32_t comp_size, uncomp_size, crc32;
    uint16_t method;            /* 0 = stored, 8 = deflate; anything else makes the entry unusable (never matched) */
    uint16_t name;               /* pool offset, NUL-terminated there (tango_browser.c's pool_add pattern) */
} tango_zip_entry_t;

/* The matching-entry table a scan/locate fills in (see this file's own header comment for why this is
 * not embedded in tango_zip_t by value). Callers that need their own, e.g. mra_session_t's own
 * shared instance for its TANGO_MRA_MAX_ZIPS zip slots, declare one of these and point a tango_zip_t's
 * own `cache` field at it; a caller that never sets `cache` (leaves it NULL, e.g. CM.zip, every host
 * test) falls back to tango_zip.c's own single default instance instead -- this struct's own layout
 * need not be public for that default path, only for a caller that embeds its own. */
typedef struct {
    tango_zip_entry_t e[TANGO_ZIP_MAX_ENTRIES];
    char     pool[TANGO_ZIP_POOL];
    unsigned pool_used;
} tango_zip_cache_t;

typedef struct {
    int      open;              /* an archive is open (io->open succeeded); 0 = everything below is unused */
    uint32_t file_size;
    int      n;                 /* matching entries found by the last scan */
    int      truncated;         /* more matches existed than TANGO_ZIP_MAX_ENTRIES or the pool could hold */
    tango_zip_cache_t *cache;    /* NULL = use tango_zip.c's own single default (this file's own header
                                   * comment has the full reasoning); every tango_zip_t left at its own
                                   * memset-to-0 state already starts this way, the self-contained
                                   * default for any caller that never
                                   * sets this -- tango_zip_close() preserves it across a reset/reopen,
                                   * so a caller that DOES set it only ever needs to once, not per use. */
    /* the member currently open for streaming */
    int      mem_open;
    int      mem_index;
    uint32_t mem_data_off;      /* verified compressed-data start, absolute within the archive */
    uint32_t mem_comp_size, mem_uncomp_size, mem_comp_pos, mem_uncomp_pos, mem_crc, mem_crc_want;
    uint16_t mem_method;
    int      mem_done;          /* deflate: tinfl reported DONE (mem_pending may still hold its last bytes) */
    /* streaming inflate state (method 8 only): malloc'd by tango_zip_open_member, freed by close_member/close,
     * never static. window_pos is tinfl's own write cursor (dict_ofs); pending_pos/len is already-produced
     * output not yet handed to a tango_zip_read caller (tinfl's windowed mode always fills to the dictionary's
     * end in one call -- it has no notion of "give me only N bytes" -- so tango_zip_read buffers the remainder
     * here and must fully drain it before calling tinfl again, or the next call's write could lap it). */
    void    *inflator;          /* tinfl_decompressor*, opaque here so FatFs/FreeRTOS headers need not be seen */
    uint8_t *window;            /* TINFL_LZ_DICT_SIZE (32768) bytes */
    unsigned window_pos, pending_pos, pending_len;
    uint8_t  inbuf[512];
    unsigned in_len, in_pos;
} tango_zip_t;

/* Opens path (relative to the card root) via io and reads its End Of Central Directory record. 1 = ok, 0 = not
 * a zip / an I/O error / a ZIP64 archive (not supported; see above) -- io is closed again on failure. Always
 * resets z first, so it is safe to call on a tango_zip_t left open or mid-use from an earlier archive. */
int tango_zip_open_archive(tango_zip_t *z, const tango_zipfs_t *io, const char *path);

/* Walks the central directory once, keeping the entries whose name matches `exts` (tango_browser.c's
 * tango_ext_match -- CONF_STR's 3-character groups, case-insensitive), ignoring any folder path inside the zip.
 * Returns the match count (0 = none; the archive stays open either way, the caller decides what to do). */
int tango_zip_scan(tango_zip_t *z, const tango_zipfs_t *io, const char *exts);

/* Finds ONE entry by crc (tried first, matching MiSTer's FileOpenZip/zip_search_by_crc
 * priority -- file_io.cpp 331-358) or by exact internal path, case-insensitive (fallback,
 * matching mz_zip_reader_locate_file) -- unlike tango_zip_scan, this walks the central directory once
 * without caching every entry, because an MRA <part> always names the one member it wants (the browsing
 * cap stays as it is; this direct-lookup mode serves that case instead).
 * crc == 0 skips crc matching (MiSTer's own convention, `if (crc32)`); name == NULL skips name matching.
 * On success, the archive's entry list becomes just the one match at index 0 (z->n = 1), so the ordinary
 * tango_zip_open_member(z, io, 0, ...) opens it. 1 = found, 0 = neither matched. */
int tango_zip_locate(tango_zip_t *z, const tango_zipfs_t *io, const char *name, uint32_t crc);

/* a matching entry's name exactly as it is inside the zip (NUL-terminated) */
const char *tango_zip_name(const tango_zip_t *z, int i);
/* find a matching entry by that name, case-insensitive; -1 if there is none */
int tango_zip_find(const tango_zip_t *z, const char *name);
/* a matching entry's own crc32 (the zip's central-directory value, not recomputed), so a caller (test_zip.c's own locate test, distinguishing two
 * same-named-pattern matches by content) never needs direct access to the cache's own layout to read
 * back what a scan/locate already found -- the same spirit as tango_zip_name, just the other field. */
uint32_t tango_zip_crc32(const tango_zip_t *z, int i);

/* Opens matching entry i for streaming: reads its local file header to find the true compressed-data offset,
 * and -- for method 8 -- allocates the inflate window and decompressor (freed by close_member/close; never
 * kept otherwise). *uncomp_size is the exact total byte count tango_zip_read will eventually produce
 * (tango_dl_begin's size). Closes any member already open first. 0 = ok, -1 = i out of range, an I/O error, or
 * an unsupported method (only stored and deflate are read). */
int tango_zip_open_member(tango_zip_t *z, const tango_zipfs_t *io, int i, uint32_t *uncomp_size);

/* tango_dl_pump's rd() shape exactly (tango_coremenu.h): up to n bytes, possibly fewer, <= 0 only on a real
 * error (corrupt data, an I/O error, or a CRC-32 mismatch once the full member has been produced) -- reaching
 * the declared size is tango_dl_pump's job, not signalled here, same as the existing FatFs-backed dl_read(). */
int tango_zip_read(tango_zip_t *z, const tango_zipfs_t *io, uint8_t *buf, unsigned n);

void tango_zip_close_member(tango_zip_t *z);              /* safe when nothing is open */
void tango_zip_close(tango_zip_t *z, const tango_zipfs_t *io);   /* closes any open member too; safe to call twice */

#endif
