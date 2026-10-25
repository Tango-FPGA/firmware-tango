// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 the Tango authors
/*
 * tango_mra.c -- see tango_mra.h. Tango, our own code.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "tango_mra.h"

static int lc(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
static int ncasecmp(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        int x = lc((unsigned char)a[i]), y = lc((unsigned char)b[i]);
        if (x != y) return x - y;
        if (!x) return 0;
    }
    return 0;
}

/* feeds one byte into a match against the literal (case-insensitive); returns 1 once lit is fully
 * matched (and resets *pos for the next search), 0 otherwise. A short, repeat-free literal like "<rbf>"
 * needs no KMP-style fallback table: on a mismatch, *pos restarts at 1 if c happens to restart the
 * literal (c == lit[0]), else 0. */
static int match_lit(const char *lit, int *pos, char c)
{
    if (lc((unsigned char)c) == lc((unsigned char)lit[*pos])) {
        (*pos)++;
        if (!lit[*pos]) { *pos = 0; return 1; }
        return 0;
    }
    *pos = (lc((unsigned char)c) == lc((unsigned char)lit[0])) ? 1 : 0;
    return 0;
}

int tango_mra_find_rbf(const tango_zipfs_t *io, const char *path, char *rbf, int cap)
{
    rbf[0] = 0;
    uint32_t size = 0;
    if (!io->open(io->ctx, path, &size)) return 0;

    uint32_t want = size < TANGO_MRA_SCAN_MAX ? size : (uint32_t)TANGO_MRA_SCAN_MAX;
    uint8_t chunk[128];
    int open_pos = 0, in_text = 0, out_n = 0, done = 0;
    uint32_t off = 0;

    while (off < want && !done) {
        unsigned room = sizeof chunk;
        if (want - off < room) room = (unsigned)(want - off);
        int n = io->read(io->ctx, off, chunk, room);
        if (n <= 0) break;
        for (int i = 0; i < n && !done; i++) {
            char c = (char)chunk[i];
            if (!in_text) {
                if (match_lit("<rbf>", &open_pos, c)) in_text = 1;
                continue;
            }
            if (c == '<') done = 1;                    /* </rbf> or any following tag: text ends here */
            else if (out_n < cap - 1) rbf[out_n++] = c;
        }
        off += (uint32_t)n;
    }
    io->close(io->ctx);
    rbf[out_n] = 0;

    /* trim: the text node carries the line's own leading/trailing whitespace in a pretty-printed .mra */
    char *p = rbf;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    int len = (int)strlen(p);
    while (len > 0 && (p[len - 1] == ' ' || p[len - 1] == '\t' || p[len - 1] == '\r' || p[len - 1] == '\n')) len--;
    if (p != rbf) memmove(rbf, p, (size_t)len);
    rbf[len] = 0;
    return rbf[0] != 0;
}

int tango_mra_resolve_core(const tango_fs_t *fs, const char *path, const char *rbf, char *out, int cap)
{
    out[0] = 0;
    size_t rbf_len = strlen(rbf);
    if (!rbf_len) return 0;
    if (!fs->open(fs->ctx, path)) return 0;

    char name[TANGO_PATH_MAX], best[TANGO_PATH_MAX] = { 0 };
    int is_dir;
    while (fs->read(fs->ctx, name, sizeof name, &is_dir)) {
        if (is_dir || !tango_is_core_file(name)) continue;

        int match = 0;
        if (!ncasecmp(name, "Arcade-", 7)) {
            size_t pl = 7 + rbf_len;
            if (!ncasecmp(name + 7, rbf, rbf_len) && (name[pl] == '.' || name[pl] == '_')) match = 1;
        }
        if (!match && !ncasecmp(name, rbf, rbf_len) && (name[rbf_len] == '.' || name[rbf_len] == '_'))
            match = 1;

        /* MiSTer's loop: the lexicographically-greatest matching name wins, across both forms */
        if (match && (!best[0] || strcmp(name, best) > 0)) {
            strncpy(best, name, sizeof best - 1); best[sizeof best - 1] = 0;
        }
    }
    fs->close(fs->ctx);

    if (!best[0]) return 0;
    strncpy(out, best, (size_t)cap - 1); out[cap - 1] = 0;
    return 1;
}

/* ================================================================== <rom>/<part> assembly */

/* a small sliding read-ahead window over a byte-addressable file (tango_zipfs_t's read() is
 * offset-based, not a stream cursor, so re-reading an earlier range costs nothing to support --
 * the "never buffer the whole file" rule carried through to a general tag scan, not just the
 * single bounded <rbf> scan). Created fresh wherever a scan resumes: tango_mra_rom_t itself keeps
 * only the plain integer position, never an open window, between tango_mra_rom_read calls. */
#define MRA_CUR_WINDOW 256
typedef struct {
    const tango_zipfs_t *io;
    uint32_t size, buf_off;
    uint8_t  buf[MRA_CUR_WINDOW];
    unsigned buf_len;
} mra_cur_t;

static void cur_init(mra_cur_t *c, const tango_zipfs_t *io, uint32_t size)
{
    c->io = io; c->size = size; c->buf_off = 0; c->buf_len = 0;
}
static int cur_byte(mra_cur_t *c, uint32_t off, uint8_t *out)
{
    if (off >= c->size) return 0;
    if (off < c->buf_off || off >= c->buf_off + c->buf_len) {
        unsigned want = MRA_CUR_WINDOW;
        if (c->size - off < want) want = (unsigned)(c->size - off);
        int n = c->io->read(c->io->ctx, off, c->buf, want);
        if (n <= 0) return 0;
        c->buf_off = off; c->buf_len = (unsigned)n;
    }
    *out = c->buf[off - c->buf_off];
    return 1;
}

/* ------------------------------------------------------------------ a narrow tag/attribute scanner
 * No entities, no CDATA, no PIs. Comments DO appear, between <part> siblings (e.g. "1942 (Revision
 * B).mra") -- handled for real below, not just skipped to the next bare '>'. Both attribute-quote
 * styles and self-closing tags are real, confirmed, and handled. */
typedef struct {
    char     name[24];             /* lowercased tag name */
    int      self_close;
    uint32_t attrs_off, attrs_end; /* the tag's attribute text, excluding '<name' and the '>'/'/>' */
    uint32_t after;                 /* just past '>': where the body (or the next tag) begins */
} mra_tag_t;

/* Finds the next START tag at or after `from` (a close tag, "</...", ends the search -- see
 * next_rom_event, which is the only caller that can encounter one; tango_mra_rom_open's own top-level
 * scan for <rom index> never needs to stop at a close tag, since <rom> never nests). Also skips
 * anything that looks like "<!" or "<?" (never appears in a real .mra, handled only so a corrupt file
 * can't wedge the scan). Returns 0 at EOF or on a close tag (out *is_close distinguishes the two: EOF
 * leaves it 0, matching "nothing left to find" either way for next_tag's own callers, which never
 * reach a close tag at the top level). */
static int next_tag(mra_cur_t *c, uint32_t from, mra_tag_t *t, int *is_close)
{
    if (is_close) *is_close = 0;
    uint32_t p = from;
    for (;;) {
        uint8_t ch;
        for (;;) { if (!cur_byte(c, p, &ch)) return 0; if (ch == '<') break; p++; }
        p++;
        if (!cur_byte(c, p, &ch)) return 0;
        if (ch == '/') { if (is_close) *is_close = 1; return 0; }
        if (ch == '!') {
            /* a real XML comment, confirmed present in real .mra files between <part> siblings
             * (e.g. "1942 (Revision B).mra") -- its true end is "-->", which can
             * be arbitrarily far past the next bare '>' if the comment text itself contains one (never
             * seen in practice, but worth getting right since comments are a known real case) */
            uint8_t c1 = 0, c2 = 0;
            cur_byte(c, p + 1, &c1); cur_byte(c, p + 2, &c2);
            if (c1 == '-' && c2 == '-') {
                p += 3;
                int dashes = 0;
                for (;;) {
                    if (!cur_byte(c, p, &ch)) return 0;
                    p++;
                    if (ch == '-') { dashes++; continue; }
                    if (ch == '>' && dashes >= 2) break;
                    dashes = 0;
                }
                continue;
            }
            /* some other "<!..." form (DOCTYPE, CDATA): never real here either, but still skip past
             * it rather than choke on it */
            while (cur_byte(c, p, &ch) && ch != '>') p++;
            p++;
            continue;
        }
        if (ch == '?') {   /* a processing instruction: not real in practice; skip to '>' */
            while (cur_byte(c, p, &ch) && ch != '>') p++;
            p++;
            continue;
        }
        int nl = 0;
        while (cur_byte(c, p, &ch) && ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n' && ch != '>' && ch != '/') {
            if (nl < (int)sizeof t->name - 1) t->name[nl++] = (char)lc(ch);
            p++;
        }
        t->name[nl] = 0;
        if (!nl) continue;             /* "<>" or similar: not a real tag; keep scanning past it */
        t->attrs_off = p;
        int in_quote = 0, last_slash = 0; char quote = 0;
        for (;;) {
            if (!cur_byte(c, p, &ch)) return 0;
            if (in_quote) { if (ch == (uint8_t)quote) in_quote = 0; p++; continue; }
            if (ch == '\'' || ch == '"') { in_quote = 1; quote = (char)ch; last_slash = 0; p++; continue; }
            if (ch == '>') break;
            last_slash = (ch == '/');
            p++;
        }
        t->attrs_end = p; t->self_close = last_slash; t->after = p + 1;
        return 1;
    }
}

/* Reads one attribute's value out of t (case-insensitive name match, either quote style). 0 = absent. */
static int tag_attr(mra_cur_t *c, const mra_tag_t *t, const char *want, char *out, int cap)
{
    out[0] = 0;
    uint32_t p = t->attrs_off;
    size_t want_len = strlen(want);
    while (p < t->attrs_end) {
        uint8_t ch;
        while (p < t->attrs_end && cur_byte(c, p, &ch) &&
               (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n' || ch == '/')) p++;
        if (p >= t->attrs_end) break;
        uint32_t name_start = p;
        while (p < t->attrs_end && cur_byte(c, p, &ch) && ch != '=' &&
               ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n') p++;
        uint32_t name_end = p;
        while (p < t->attrs_end && cur_byte(c, p, &ch) && ch != '=') p++;
        if (p >= t->attrs_end) break;
        p++;
        while (p < t->attrs_end && cur_byte(c, p, &ch) && (ch == ' ' || ch == '\t')) p++;
        if (p >= t->attrs_end || !cur_byte(c, p, &ch) || (ch != '\'' && ch != '"')) break;
        char quote = (char)ch; p++;
        uint32_t val_start = p;
        while (p < t->attrs_end && cur_byte(c, p, &ch) && ch != (uint8_t)quote) p++;
        uint32_t val_end = p;
        if (p < t->attrs_end) p++;

        unsigned nlen = name_end - name_start;
        int match = (nlen == want_len);
        for (unsigned i = 0; match && i < nlen; i++) {
            uint8_t nc; cur_byte(c, name_start + i, &nc);
            match = (lc(nc) == lc((unsigned char)want[i]));
        }
        if (match) {
            unsigned vlen = val_end - val_start;
            if ((int)vlen > cap - 1) vlen = (unsigned)(cap - 1);
            for (unsigned i = 0; i < vlen; i++) { uint8_t vc; cur_byte(c, val_start + i, &vc); out[i] = (char)vc; }
            out[vlen] = 0;
            return 1;
        }
    }
    return 0;
}

/* Scans forward from `from` for the literal "</tagname>" (case-insensitive), returning the offset
 * just past it, or `size` if never found (a malformed/truncated file -- treated as "goes to EOF", not
 * an error: the caller's own bounds then naturally stop it there). If start is non-NULL, also reports
 * the offset of the '<' that begins the closing tag (same as the return value minus the literal's own
 * length -- kept as an explicit out-param rather than a length callers would have to know, since the
 * literal's length depends on tagname). */
static uint32_t skip_to_close_at(mra_cur_t *c, uint32_t from, uint32_t size, const char *tagname, uint32_t *start)
{
    char lit[32]; unsigned cl = 0;
    lit[cl++] = '<'; lit[cl++] = '/';
    for (const char *p = tagname; *p && cl < sizeof lit - 2; p++) lit[cl++] = *p;
    lit[cl++] = '>'; lit[cl] = 0;
    int pos_in_lit = 0;
    for (uint32_t p = from; p < size; p++) {
        uint8_t ch; if (!cur_byte(c, p, &ch)) break;
        if (lc(ch) == lc((unsigned char)lit[pos_in_lit])) {
            pos_in_lit++;
            if (!lit[pos_in_lit]) { if (start) *start = p + 1 - cl; return p + 1; }
        } else {
            pos_in_lit = (lc(ch) == lc((unsigned char)lit[0])) ? 1 : 0;
        }
    }
    if (start) *start = size;
    return size;
}
static uint32_t skip_to_close(mra_cur_t *c, uint32_t from, uint32_t size, const char *tagname)
{
    return skip_to_close_at(c, from, size, tagname, NULL);
}

/* Inside a <rom>'s body: the next thing that matters is a <part>/<interleave> start tag, or this
 * rom's own close tag ("</rom>", trusted to be the only close tag reachable here -- <rom> never
 * nests, confirmed against a real library). <patch> is handled entirely by its own pre-scan
 * (scan_patches, called once from tango_mra_rom_open) and applied in flight (apply_patches) -- by
 * the time this scanner runs, a <patch> tag is just more sibling content to skip past, same as
 * anything else nested in there that was never seen in practice. */
typedef enum { ROM_EVENT_END, ROM_EVENT_PART, ROM_EVENT_INTERLEAVE } rom_event_t;
static rom_event_t next_rom_event(mra_cur_t *c, uint32_t from, uint32_t size, mra_tag_t *t, uint32_t *next_pos)
{
    uint32_t p = from;
    for (;;) {
        int is_close = 0;
        if (!next_tag(c, p, t, &is_close) ) { *next_pos = size; return ROM_EVENT_END; }
        /* next_tag returning 0 with is_close set means it stopped AT a close tag without consuming it;
         * either way (EOF or close), this rom's part search ends here */
        if (is_close) { *next_pos = size; return ROM_EVENT_END; }
        *next_pos = t->after;
        if (!strcmp(t->name, "part")) return ROM_EVENT_PART;
        if (!strcmp(t->name, "interleave")) return ROM_EVENT_INTERLEAVE;
        if (!t->self_close) *next_pos = skip_to_close(c, t->after, size, t->name);
        p = *next_pos;
    }
}

/* ------------------------------------------------------------------ top-level tag lookup (shared) */
/* Scans forward from *pos for the next top-level tag named `tag_name` (next_tag already lowercases,
 * so this is case-insensitive), skipping the whole body of every sibling tag that doesn't match --
 * and descending into, never skipping, the document root <misterromdescription> (skipping that root
 * tag's own body like any other sibling's would take every <rom> with it). On a match, *t holds the tag and *pos is already
 * advanced to t->after; the caller decides whether to accept it (e.g. check an attribute) or reject
 * and call again from the now-updated *pos, having first skipped the rejected tag's own body itself
 * (skip_to_close; this function only ever skips tags that never matched tag_name to begin with).
 * Returns 0 at EOF or a stray close tag -- no (more) occurrences. */
static int find_next_top_level(mra_cur_t *cur, uint32_t *pos, uint32_t size, const char *tag_name, mra_tag_t *t)
{
    for (;;) {
        int is_close = 0;
        if (!next_tag(cur, *pos, t, &is_close)) return 0;
        *pos = t->after;
        if (!strcmp(t->name, "misterromdescription")) continue;
        if (!strcmp(t->name, tag_name)) return 1;
        if (!t->self_close) *pos = skip_to_close(cur, t->after, size, t->name);
    }
}

/* ------------------------------------------------------------------ rom_open / rom_read / rom_close */
static void scan_patches(mra_cur_t *cur, uint32_t from, uint32_t rom_end, tango_mra_rom_t *r);

int tango_mra_rom_open(tango_mra_rom_t *r, const tango_zipfs_t *mra_io, const char *mra_path,
                       int want_index, char *out_zip, int zip_name_cap)
{
    memset(r, 0, sizeof *r);
    r->want_index = want_index;
    r->zip_mem_open_slot = -1;
    if (out_zip && zip_name_cap > 0) out_zip[0] = 0;

    uint32_t size = 0;
    if (!mra_io->open(mra_io->ctx, mra_path, &size)) { r->phase = TANGO_MRA_ROM_ERROR_ST; return 0; }
    r->mra_size = size;

    mra_cur_t cur; cur_init(&cur, mra_io, size);
    mra_tag_t t;
    uint32_t pos = 0;
    while (find_next_top_level(&cur, &pos, size, "rom", &t)) {
        char idxbuf[16];
        if (!tag_attr(&cur, &t, "index", idxbuf, sizeof idxbuf) || atoi(idxbuf) != want_index) {
            if (!t.self_close) pos = skip_to_close(&cur, t.after, size, "rom");   /* never look inside a
                                                                                   * non-matching <rom> */
            continue;
        }
        r->found = 1;
        if (out_zip) tag_attr(&cur, &t, "zip", out_zip, zip_name_cap);
        tag_attr(&cur, &t, "md5", r->expected_md5, sizeof r->expected_md5);
        tango_md5_init(&r->md5_ctx);
        if (t.self_close) { r->phase = TANGO_MRA_ROM_DONE; return 1; }   /* an empty <rom index="N"/> */
        scan_patches(&cur, t.after, size, r);          /* sets REFUSED_ST itself if over the cap */
        if (r->phase != TANGO_MRA_ROM_REFUSED_ST) r->phase = TANGO_MRA_ROM_SEEK_PART;
        r->pos = t.after;
        return 1;
    }
    r->phase = TANGO_MRA_ROM_DONE;   /* no <rom index="want_index"> at all: the same "nothing to send"
                                      * as an empty one (MiSTer: romlen[0] stays 0 either way) */
    return 1;
}

/* Tries each zips[0..n_zips) in order (MiSTer's own per-part fallback trial, mra_loader.cpp 853-879)
 * until one has this part's member; sets r->zip_mem_open_slot to whichever one worked. -1 = found in
 * none of them (a real error -- see tango_mra_rom_read's doc comment), -2 = an I/O error partway
 * through skipping to part_offset on the one that did have it (distinct from "not found" only for the
 * caller's own diagnostics; both are TANGO_MRA_ROM_ERROR_ST to tango_mra_rom_read either way). */
static int open_file_part(tango_mra_rom_t *r, tango_zip_t *const zips[], const tango_zipfs_t *const zip_ios[],
                          int n_zips)
{
    for (int i = 0; i < n_zips; i++) {
        if (!zips[i] || !zip_ios[i]) continue;
        if (!tango_zip_locate(zips[i], zip_ios[i], r->part_name, r->part_crc)) continue;
        uint32_t usize = 0;
        if (tango_zip_open_member(zips[i], zip_ios[i], 0, &usize) != 0) continue;
        r->zip_mem_open_slot = i;

        uint32_t skip = r->part_offset;
        uint8_t scratch[64];
        while (skip) {
            unsigned want = skip < sizeof scratch ? skip : (unsigned)sizeof scratch;
            int got = tango_zip_read(zips[i], zip_ios[i], scratch, want);
            if (got <= 0) {
                tango_zip_close_member(zips[i]); r->zip_mem_open_slot = -1;
                return -2;
            }
            skip -= (uint32_t)got;
        }
        uint32_t avail = usize > r->part_offset ? usize - r->part_offset : 0;
        r->file_left = (r->part_length && r->part_length < avail) ? r->part_length : avail;
        return 1;
    }
    return -1;   /* not in any of them */
}

/* ------------------------------------------------------------------ <patch> and the md5 check */

/* Decodes hex-digit pairs from the .mra range [start,end) (MiSTer's own adjacent-pairs convention,
 * same as tango_mra_rom_read's inline-part decoder), skipping the first `skip` DECODED bytes and
 * writing up to `cap` further ones into `out` (out may be NULL, to just count -- cap is then
 * ignored). Returns how many bytes were produced after the skip. Used both to size a patch at
 * pre-scan time (skip=0, out=NULL) and to decode just the slice overlapping one output chunk at
 * apply time -- never the whole patch at once, however large it is (the RAM-budget rule:
 * one real patch runs 32 KB, and this still only ever touches a few dozen bytes per call). */
static uint32_t decode_hex_range(mra_cur_t *cur, uint32_t start, uint32_t end, uint32_t skip,
                                 uint8_t *out, uint32_t cap)
{
    uint32_t decoded = 0, produced = 0;
    int have_nibble = 0; uint8_t nibble = 0;
    for (uint32_t p = start; p < end; p++) {
        if (out && produced >= cap) break;   /* got everything the caller asked for -- stop, don't
            * keep counting past it: the return value IS the written count when out is given, never
            * more (a caller writing exactly `produced` bytes into a `cap`-sized buffer must never see
            * a bigger number back, or it reads past what was actually written) */
        uint8_t ch; if (!cur_byte(cur, p, &ch)) break;
        int v;
        if (ch >= '0' && ch <= '9') v = ch - '0';
        else if (ch >= 'a' && ch <= 'f') v = ch - 'a' + 10;
        else if (ch >= 'A' && ch <= 'F') v = ch - 'A' + 10;
        else continue;
        if (!have_nibble) { nibble = (uint8_t)v; have_nibble = 1; continue; }
        uint8_t byte_val = (uint8_t)((nibble << 4) | v); have_nibble = 0;
        if (decoded >= skip) { if (out) out[produced] = byte_val; produced++; }
        decoded++;
    }
    if ((!out || produced < cap) && have_nibble && decoded >= skip) {
        if (out) out[produced] = nibble;
        produced++;
    }
    return produced;
}

/* Pre-scans a <rom>'s body (from just after its own '>' to its "</rom>") for every <patch> tag,
 * recording each (offset, operation, and its hex text's byte range -- never the bytes themselves)
 * into r->patches[]. Sets r->phase to TANGO_MRA_ROM_REFUSED_ST, not silently truncating, if there are
 * more than TANGO_MRA_PATCH_MAX (one real file needs 146; the cap is 160). Anything that isn't
 * <patch> is skipped whole (its body included), same discipline as every other scan in this file. */
static void scan_patches(mra_cur_t *cur, uint32_t from, uint32_t rom_end, tango_mra_rom_t *r)
{
    uint32_t pos = from;
    for (;;) {
        int is_close = 0;
        mra_tag_t t;
        if (!next_tag(cur, pos, &t, &is_close)) return;   /* EOF or this rom's own "</rom>" */
        pos = t.after;
        if (strcmp(t.name, "patch")) {
            if (!t.self_close) pos = skip_to_close(cur, t.after, rom_end, t.name);
            continue;
        }
        if (r->n_patches >= TANGO_MRA_PATCH_MAX) { r->phase = TANGO_MRA_ROM_REFUSED_ST; return; }
        tango_mra_patch_t *p = &r->patches[r->n_patches];
        char valbuf[32];
        p->offset = tag_attr(cur, &t, "offset", valbuf, sizeof valbuf) ? (uint32_t)strtoul(valbuf, NULL, 0) : 0;
        p->xor_op = tag_attr(cur, &t, "operation", valbuf, sizeof valbuf) && !ncasecmp(valbuf, "xor", 4);
        if (t.self_close) {
            p->text_start = p->text_end = t.after;   /* an empty <patch/>: a harmless zero-length no-op */
        } else {
            p->text_start = t.after;
            pos = skip_to_close_at(cur, t.after, rom_end, "patch", &p->text_end);
        }
        p->length = decode_hex_range(cur, p->text_start, p->text_end, 0, NULL, 0);
        r->n_patches++;
    }
}

/* Applies every patch overlapping the absolute output range [base, base+len) to buf in place --
 * decoding only the overlapping slice of each patch's hex text (never the whole patch), one small
 * fixed-size piece at a time regardless of how big the patch itself is. Called on the PRE-patch bytes
 * tango_mra_rom_read just assembled, after they've already been fed to the md5 context (MiSTer's own
 * order: rom_patch never updates the hash, so a patched rom's md5 still describes its unpatched
 * parts). */
static void apply_patches(tango_mra_rom_t *r, const tango_zipfs_t *mra_io, uint32_t base,
                          uint8_t *buf, unsigned len)
{
    if (!r->n_patches) return;
    mra_cur_t cur; cur_init(&cur, mra_io, r->mra_size);
    uint32_t chunk_end = base + len;
    for (int i = 0; i < r->n_patches; i++) {
        tango_mra_patch_t *p = &r->patches[i];
        uint32_t p_end = p->offset + p->length;
        uint32_t ov_start = p->offset > base ? p->offset : base;
        uint32_t ov_end = p_end < chunk_end ? p_end : chunk_end;
        if (ov_start >= ov_end) continue;
        uint32_t take = ov_end - ov_start, done = 0;
        uint8_t tmp[64];
        while (done < take) {
            uint32_t want = take - done; if (want > sizeof tmp) want = (uint32_t)sizeof tmp;
            uint32_t got = decode_hex_range(&cur, p->text_start, p->text_end,
                                            (ov_start - p->offset) + done, tmp, want);
            if (!got) break;
            for (uint32_t k = 0; k < got; k++) {
                uint8_t *dst = &buf[(ov_start - base) + done + k];
                if (p->xor_op) *dst ^= tmp[k]; else *dst = tmp[k];
            }
            done += got;
        }
    }
}

static int tango_mra_rom_read_inner(tango_mra_rom_t *r, const tango_zipfs_t *mra_io,
                       tango_zip_t *const zips[], const tango_zipfs_t *const zip_ios[], int n_zips,
                       uint8_t *buf, unsigned n)
{
    unsigned produced = 0;
    while (produced < n) {
        switch (r->phase) {
        case TANGO_MRA_ROM_DONE:
            return (int)produced;
        case TANGO_MRA_ROM_REFUSED_ST:
            return produced ? (int)produced : TANGO_MRA_REFUSED;
        case TANGO_MRA_ROM_ERROR_ST:
            return produced ? (int)produced : -1;

        case TANGO_MRA_ROM_SEEK_PART: {
            mra_cur_t cur; cur_init(&cur, mra_io, r->mra_size);
            mra_tag_t t; uint32_t next_pos;
            rom_event_t ev = next_rom_event(&cur, r->pos, r->mra_size, &t, &next_pos);
            r->pos = next_pos;
            if (ev == ROM_EVENT_END) { r->phase = TANGO_MRA_ROM_DONE; break; }
            if (ev == ROM_EVENT_INTERLEAVE) { r->phase = TANGO_MRA_ROM_REFUSED_ST; break; }

            char zip_override[TANGO_MRA_ZIP_NAME_MAX];
            if (tag_attr(&cur, &t, "zip", zip_override, sizeof zip_override) && zip_override[0]) {
                r->phase = TANGO_MRA_ROM_REFUSED_ST;   /* a per-part zip override: not supported yet */
                break;
            }
            tag_attr(&cur, &t, "name", r->part_name, sizeof r->part_name);
            char numbuf[16];
            r->part_crc = tag_attr(&cur, &t, "crc", numbuf, sizeof numbuf) ? (uint32_t)strtoul(numbuf, NULL, 16) : 0;
            r->part_offset = tag_attr(&cur, &t, "offset", numbuf, sizeof numbuf) ? (uint32_t)strtoul(numbuf, NULL, 0) : 0;
            r->part_length = tag_attr(&cur, &t, "length", numbuf, sizeof numbuf) ? (uint32_t)strtoul(numbuf, NULL, 0) : 0;
            r->repeat_left = tag_attr(&cur, &t, "repeat", numbuf, sizeof numbuf) ? atoi(numbuf) : 1;
            if (r->repeat_left < 1) r->repeat_left = 1;

            if (r->part_name[0]) {
                r->phase = TANGO_MRA_ROM_PART_FILE;
                /* a file-based part's body is otherwise empty (no inline text matters once name is
                 * given) -- but a non-self-closing one still has its own "</part>" to skip past before
                 * the next SEEK_PART scan resumes, or it would mistake that close tag for the rom's own */
                r->part_close_after = t.self_close ? t.after : skip_to_close(&cur, t.after, r->mra_size, "part");
            } else if (t.self_close) {
                continue;   /* a self-closing, nameless part: zero bytes, nothing to do -- next part */
            } else {
                r->phase = TANGO_MRA_ROM_PART_INLINE;
                r->inline_start = t.after;      /* the hex text starts right after '>' */
                r->part_close_after = skip_to_close_at(&cur, t.after, r->mra_size, "part", &r->inline_end);
                r->pos = r->inline_start;
                r->hex_have_nibble = 0;
            }
            break;
        }

        case TANGO_MRA_ROM_PART_FILE: {
            if (r->zip_mem_open_slot < 0) {
                if (open_file_part(r, zips, zip_ios, n_zips) < 0) {
                    r->phase = TANGO_MRA_ROM_ERROR_ST;   /* not in any of the given zips, or a real I/O
                                                            * error partway through: either way, a real
                                                            * error, not silently skippable */
                    break;
                }
            }
            tango_zip_t *zip = zips[r->zip_mem_open_slot];
            const tango_zipfs_t *zip_io = zip_ios[r->zip_mem_open_slot];
            if (r->file_left == 0) {
                tango_zip_close_member(zip); r->zip_mem_open_slot = -1;
                if (--r->repeat_left > 0) continue;        /* re-open the same part for the next repeat
                    * (re-tries the fallback list from the top -- cheap, and the part could in principle
                    * be found via a different zip on a retry if the card changed underfoot; it never
                    * does in practice, this is just not worth special-casing) */
                r->pos = r->part_close_after;
                r->phase = TANGO_MRA_ROM_SEEK_PART;
                continue;
            }
            unsigned want = n - produced;
            if (want > r->file_left) want = r->file_left;
            int got = tango_zip_read(zip, zip_io, buf + produced, want);
            if (got < 0) { r->phase = TANGO_MRA_ROM_ERROR_ST; break; }
            if (got == 0) { r->phase = TANGO_MRA_ROM_ERROR_ST; break; }   /* file_left said more was owed */
            produced += (unsigned)got; r->file_left -= (uint32_t)got;
            continue;
        }

        case TANGO_MRA_ROM_PART_INLINE: {
            mra_cur_t cur; cur_init(&cur, mra_io, r->mra_size);
            while (produced < n && r->pos < r->inline_end) {
                uint8_t ch;
                if (!cur_byte(&cur, r->pos, &ch)) { r->phase = TANGO_MRA_ROM_ERROR_ST; break; }
                r->pos++;
                int v;
                if (ch >= '0' && ch <= '9') v = ch - '0';
                else if (ch >= 'a' && ch <= 'f') v = ch - 'a' + 10;
                else if (ch >= 'A' && ch <= 'F') v = ch - 'A' + 10;
                else continue;             /* whitespace/a separator: skipped, matching hexstr_to_char
                    * for every real file (mra_loader.cpp 391-421: adjacent digit pairs, a separator only
                    * ever between pairs, never inside one). Deliberately NOT a literal port of that
                    * function's exact quirk: it does not validate position, so a separator landing
                    * BETWEEN a pair's two digits feeds the separator itself into its hex formula as a
                    * bogus nibble instead of skipping it -- never observed in a real .mra, and skipping
                    * unconditionally here is strictly safer, not a divergence that matters in practice */
                if (!r->hex_have_nibble) { r->hex_nibble = (uint8_t)v; r->hex_have_nibble = 1; }
                else { buf[produced++] = (uint8_t)((r->hex_nibble << 4) | v); r->hex_have_nibble = 0; }
            }
            if (r->phase == TANGO_MRA_ROM_ERROR_ST) break;
            if (r->pos >= r->inline_end) {
                /* produced < n is guaranteed here (the "stash a lone nibble" branch above never
                 * increments produced, so entering this block with hex_have_nibble set means produced
                 * is still whatever let the loop above start its last iteration) -- guarded anyway. */
                if (r->hex_have_nibble && produced < n) { buf[produced++] = r->hex_nibble; r->hex_have_nibble = 0; }
                if (--r->repeat_left > 0) r->pos = r->inline_start;   /* rescans the same hex text --
                    * decoding it again costs nothing extra: these parts are a handful of bytes */
                else { r->pos = r->part_close_after; r->phase = TANGO_MRA_ROM_SEEK_PART; }
            }
            continue;
        }
        default:
            return -1;
        }
    }
    return (int)produced;
}

int tango_mra_rom_read(tango_mra_rom_t *r, const tango_zipfs_t *mra_io,
                       tango_zip_t *const zips[], const tango_zipfs_t *const zip_ios[], int n_zips,
                       uint8_t *buf, unsigned n)
{
    uint32_t base = r->out_pos;
    int ret = tango_mra_rom_read_inner(r, mra_io, zips, zip_ios, n_zips, buf, n);
    if (ret > 0) {
        /* MiSTer's own order: the md5 context sees the PRE-patch bytes (rom_patch never updates it),
         * so feed it first, then patch buf in place before the caller ever sees it */
        tango_md5_update(&r->md5_ctx, buf, (unsigned)ret);
        apply_patches(r, mra_io, base, buf, (unsigned)ret);
        r->out_pos += (uint32_t)ret;
    } else if (ret == 0 && !r->md5_checked) {
        r->md5_checked = 1;
        if (r->expected_md5[0] && ncasecmp(r->expected_md5, "none", 5) != 0) {
            uint8_t digest[16];
            tango_md5_final(&r->md5_ctx, digest);
            if (!tango_md5_eq_hex(digest, r->expected_md5)) return TANGO_MRA_MD5_MISMATCH;
        }
    }
    return ret;
}

void tango_mra_rom_close(tango_mra_rom_t *r, const tango_zipfs_t *mra_io,
                         tango_zip_t *const zips[], const tango_zipfs_t *const zip_ios[])
{
    if (r->zip_mem_open_slot >= 0 && zips && zip_ios) {
        tango_zip_close_member(zips[r->zip_mem_open_slot]);
        r->zip_mem_open_slot = -1;
    }
    mra_io->close(mra_io->ctx);
}

/* ================================================================== <switches>/<cheats> (DIPs) */

/* Decodes a short, already-extracted attribute VALUE string (never a file range -- "default"'s value
 * is a handful of bytes, tag_attr already capped it) the same way MiSTer's hexstr_to_char does for
 * every real .mra: adjacent hex digit pairs, separators (',', ' ', '\t', '\r', '\n') only ever
 * between pairs, an odd trailing digit flushed as its own byte. Returns the byte count (capped at
 * `cap`). Deliberately not a literal port of hexstr_to_char's own quirk (see tango_mra_rom_read's
 * inline-hex comment) for the same reason: it doesn't validate position either, here. */
static int hex_bytes_decode(const char *s, uint8_t *out, int cap)
{
    int n = 0, have_nibble = 0; uint8_t nibble = 0;
    for (; *s && n < cap; s++) {
        int v;
        if (*s >= '0' && *s <= '9') v = *s - '0';
        else if (*s >= 'a' && *s <= 'f') v = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'F') v = *s - 'A' + 10;
        else continue;
        if (!have_nibble) { nibble = (uint8_t)v; have_nibble = 1; }
        else { out[n++] = (uint8_t)((nibble << 4) | v); have_nibble = 0; }
    }
    if (have_nibble && n < cap) out[n++] = nibble;
    return n;
}

/* Splits an already-extracted "ids" or "values"-shaped attribute string on ',' into up to
 * TANGO_MRA_DIP_IDS_MAX pieces, writing each trimmed-of-nothing piece (MiSTer's own id-split keeps
 * leading/trailing spaces, xml_send_rom 644-660) into id[i] (cap TANGO_MRA_DIP_ID_LEN_MAX each).
 * Returns the piece count. */
static int split_ids(const char *s, char id[][TANGO_MRA_DIP_ID_LEN_MAX])
{
    int n = 0;
    while (*s && n < TANGO_MRA_DIP_IDS_MAX) {
        const char *comma = strchr(s, ',');
        size_t len = comma ? (size_t)(comma - s) : strlen(s);
        if (len > TANGO_MRA_DIP_ID_LEN_MAX - 1) len = TANGO_MRA_DIP_ID_LEN_MAX - 1;
        memcpy(id[n], s, len); id[n][len] = 0;
        n++;
        s += comma ? (size_t)(comma - s) + 1 : strlen(s);
    }
    return n;
}

/* Parses an already-extracted "values" attribute string into up to TANGO_MRA_DIP_IDS_MAX uint64_t's
 * (MiSTer's own strtoul base 0, so "0x.." or plain decimal both work, xml_send_rom 662-682). Returns
 * the count; a value that fails to parse (strtoul makes no progress) stops the scan there, same as
 * MiSTer's own early `break`. */
static int split_values(const char *s, uint64_t val[])
{
    int n = 0;
    while (*s && n < TANGO_MRA_DIP_IDS_MAX) {
        char *endp = NULL;
        uint64_t v = strtoul(s, &endp, 0);
        if (!endp || endp == s) break;
        val[n++] = v;
        s = endp;
        while (*s == ' ' || *s == ',') s++;
    }
    return n;
}

int tango_mra_switches_parse(const tango_zipfs_t *mra_io, const char *mra_path, int cheats,
                             tango_mra_switches_t *out)
{
    memset(out, 0, sizeof *out);
    uint32_t size = 0;
    if (!mra_io->open(mra_io->ctx, mra_path, &size)) return 0;

    mra_cur_t cur; cur_init(&cur, mra_io, size);
    mra_tag_t t; uint32_t pos = 0;
    const char *want = cheats ? "cheats" : "switches";
    if (!find_next_top_level(&cur, &pos, size, want, &t)) { mra_io->close(mra_io->ctx); return 1; }

    char valbuf[256];
    if (tag_attr(&cur, &t, "default", valbuf, sizeof valbuf)) {
        uint8_t bytes[8];
        int nb = hex_bytes_decode(valbuf, bytes, (int)sizeof bytes);
        for (int i = 0; i < nb; i++) out->dip_def |= (uint64_t)bytes[i] << (i * 8);
    }
    if (t.self_close) { mra_io->close(mra_io->ctx); return 1; }

    uint32_t dip_pos = t.after;
    for (;;) {
        int is_close = 0;
        mra_tag_t dt;
        if (!next_tag(&cur, dip_pos, &dt, &is_close)) break;   /* EOF or the enclosing </switches>/</cheats> */
        dip_pos = dt.after;
        if (strcmp(dt.name, "dip")) {
            if (!dt.self_close) dip_pos = skip_to_close(&cur, dt.after, size, dt.name);
            continue;
        }
        if (out->dip_num >= TANGO_MRA_DIP_MAX) {
            if (!dt.self_close) dip_pos = skip_to_close(&cur, dt.after, size, "dip");
            continue;   /* the array's own size (mra_loader.h dip[64]); MiSTer's literal check
                * ("if (sw->dip_num < 63) sw->dip_num++", xml_send_rom 924) stops COUNTING past 63
                * entries but keeps re-writing index 63 for every <dip> after that forever -- a
                * never-exercised quirk no real board comes remotely close to (the biggest seen here
                * is Galaga's 11); refusing cleanly at the array's real size is simpler and no real
                * file can tell the difference */
        }
        tango_mra_dip_t *d = &out->dip[out->dip_num];

        tag_attr(&cur, &dt, "name", d->name, sizeof d->name);
        if (tag_attr(&cur, &dt, "bits", valbuf, sizeof valbuf)) {
            int b = 0, e = 0;
            int n = sscanf(valbuf, "%d,%d", &b, &e);
            if (n >= 1 && b >= 0 && b <= 63 && e >= 0 && e <= 63 && !(n == 2 && e < b)) {
                if (n == 1) e = b;
                d->start = b; d->size = e - b + 1;
                uint64_t m = 1; for (int i = 0; i < e - b; i++) m = (m << 1) | 1;
                d->mask = m << b;
            }
        }
        if (tag_attr(&cur, &dt, "ids", valbuf, sizeof valbuf)) d->num = split_ids(valbuf, d->id);
        if (tag_attr(&cur, &dt, "values", valbuf, sizeof valbuf)) {
            d->has_val = 1;
            split_values(valbuf, d->val);   /* count already came from "ids" (MiSTer trusts them to
                                              * agree, xml_send_rom 918: loops dip[n].num, not val's own count) */
        }
        for (int i = 0; i < d->num; i++) d->val[i] = (d->has_val ? d->val[i] : (uint64_t)i) << d->start;
        out->dip_num++;
        if (!dt.self_close) dip_pos = skip_to_close(&cur, dt.after, size, "dip");   /* skip past its own
            * "</dip>" before the next iteration's next_tag call, or that close tag reads as "nothing
            * left" (is_close) and the scan stops after just one entry -- the same bug class as a
            * non-self-closing file-based <part>'s own "</part>" (SEEK_PART); same fix, same reason */
    }

    mra_io->close(mra_io->ctx);
    return 1;
}

/* The DIP page's own per-row, on-demand parse -- see tango_mra.h's own doc comment for the full
 * reasoning (the RAM ceiling of a whole tango_mra_switches_t).
 * Shares every helper tango_mra_switches_parse above uses (cur_init/find_next_top_level/tag_attr/
 * next_tag/skip_to_close/split_ids/split_values), just stops at the wanted <dip> instead of filling an
 * array -- no new scanning logic, only a different stopping point. */
int tango_mra_switches_dip_at(const tango_zipfs_t *mra_io, const char *mra_path, int cheats,
                              int index, tango_mra_dip_t *out)
{
    memset(out, 0, sizeof *out);
    if (index < 0 || index >= TANGO_MRA_DIP_MAX) return 0;
    uint32_t size = 0;
    if (!mra_io->open(mra_io->ctx, mra_path, &size)) return 0;

    mra_cur_t cur; cur_init(&cur, mra_io, size);
    mra_tag_t t; uint32_t pos = 0;
    const char *want = cheats ? "cheats" : "switches";
    if (!find_next_top_level(&cur, &pos, size, want, &t) || t.self_close) {
        mra_io->close(mra_io->ctx); return 0;   /* no block, or one with no <dip>s at all: no such index */
    }

    char valbuf[256];
    int found = 0, n = 0;
    uint32_t dip_pos = t.after;
    for (;;) {
        int is_close = 0;
        mra_tag_t dt;
        if (!next_tag(&cur, dip_pos, &dt, &is_close)) break;   /* EOF or the enclosing close tag */
        dip_pos = dt.after;
        if (strcmp(dt.name, "dip")) {
            if (!dt.self_close) dip_pos = skip_to_close(&cur, dt.after, size, dt.name);
            continue;
        }
        if (n == index) {
            tag_attr(&cur, &dt, "name", out->name, sizeof out->name);
            if (tag_attr(&cur, &dt, "bits", valbuf, sizeof valbuf)) {
                int b = 0, e = 0;
                int ns = sscanf(valbuf, "%d,%d", &b, &e);
                if (ns >= 1 && b >= 0 && b <= 63 && e >= 0 && e <= 63 && !(ns == 2 && e < b)) {
                    if (ns == 1) e = b;
                    out->start = b; out->size = e - b + 1;
                    uint64_t m = 1; for (int i = 0; i < e - b; i++) m = (m << 1) | 1;
                    out->mask = m << b;
                }
            }
            if (tag_attr(&cur, &dt, "ids", valbuf, sizeof valbuf)) out->num = split_ids(valbuf, out->id);
            if (tag_attr(&cur, &dt, "values", valbuf, sizeof valbuf)) {
                out->has_val = 1;
                split_values(valbuf, out->val);
            }
            for (int i = 0; i < out->num; i++) out->val[i] = (out->has_val ? out->val[i] : (uint64_t)i) << out->start;
            found = 1;
            break;      /* the file is about to be closed anyway -- no need to skip to "</dip>" */
        }
        n++;
        if (!dt.self_close) dip_pos = skip_to_close(&cur, dt.after, size, "dip");
    }

    mra_io->close(mra_io->ctx);
    if (!found) memset(out, 0, sizeof *out);
    return found;
}

/* The two fields tango_fw.c's own DIP/cheat send actually needs for the whole rest of a core's session
 * (whether to send at all, and the default/current word) -- NOT the full per-<dip> table
 * tango_mra_switches_parse above builds (name/bits/ids/values, the DIP page's own OSD material).
 * sizeof(tango_mra_switches_t) is ~83.5 KB (TANGO_MRA_DIP_MAX 64 * a ~1.3 KB tango_mra_dip_t each,
 * mostly the 32x32-byte id[] table) -- a single malloc of one, on the BL616, cannot succeed: a real build
 * (build.sh) reports a 78592-byte OCRAM heap pool in total, smaller than ONE such struct, before any of a
 * core's own task stacks are even carved out of it. Even a transient malloc of a full tango_mra_switches_t
 * per core-load (freed right after use) would fail silently on real hardware every single time -- and no
 * host test would show it (host tests link against glibc malloc, which has gigabytes to give; nothing
 * host-testable or RTL-testable here would ever see the real heap's own size). Reuses the exact same
 * tag-scanning helpers (cur_init/find_next_top_level/tag_attr/next_tag/skip_to_close/hex_bytes_decode) as
 * tango_mra_switches_parse, just without ever touching a tango_mra_dip_t: each <dip> found only increments
 * *dip_num (capped at TANGO_MRA_DIP_MAX, matching the full parser's own array-size cap) and is skipped
 * past, never populated. Needs no allocation beyond its own small, stack-sized mra_cur_t/mra_tag_t locals
 * (the same ones every other function in this file already uses on the stack). The DIP page, which DOES
 * need the per-<dip> fields to render rows, parses one <dip> at a time on demand (tango_mra_switches_dip_at
 * above), never the whole table at once. */
int tango_mra_switches_summary(const tango_zipfs_t *mra_io, const char *mra_path, int cheats,
                               uint64_t *dip_def, int *dip_num)
{
    *dip_def = 0; *dip_num = 0;
    uint32_t size = 0;
    if (!mra_io->open(mra_io->ctx, mra_path, &size)) return 0;

    mra_cur_t cur; cur_init(&cur, mra_io, size);
    mra_tag_t t; uint32_t pos = 0;
    const char *want = cheats ? "cheats" : "switches";
    if (!find_next_top_level(&cur, &pos, size, want, &t)) { mra_io->close(mra_io->ctx); return 1; }

    char valbuf[256];
    if (tag_attr(&cur, &t, "default", valbuf, sizeof valbuf)) {
        uint8_t bytes[8];
        int nb = hex_bytes_decode(valbuf, bytes, (int)sizeof bytes);
        for (int i = 0; i < nb; i++) *dip_def |= (uint64_t)bytes[i] << (i * 8);
    }
    if (t.self_close) { mra_io->close(mra_io->ctx); return 1; }

    uint32_t dip_pos = t.after;
    for (;;) {
        int is_close = 0;
        mra_tag_t dt;
        if (!next_tag(&cur, dip_pos, &dt, &is_close)) break;
        dip_pos = dt.after;
        if (strcmp(dt.name, "dip")) {
            if (!dt.self_close) dip_pos = skip_to_close(&cur, dt.after, size, dt.name);
            continue;
        }
        if (*dip_num < TANGO_MRA_DIP_MAX) (*dip_num)++;   /* same cap as the full parser's own dip[] array */
        if (!dt.self_close) dip_pos = skip_to_close(&cur, dt.after, size, "dip");
    }

    mra_io->close(mra_io->ctx);
    return 1;
}

/* ================================================================== <buttons>, <nvram> (data only) */

int tango_mra_misc_parse(const tango_zipfs_t *mra_io, const char *mra_path, tango_mra_misc_t *out)
{
    memset(out, 0, sizeof *out);
    uint32_t size = 0;
    if (!mra_io->open(mra_io->ctx, mra_path, &size)) return 0;

    mra_cur_t cur; cur_init(&cur, mra_io, size);
    mra_tag_t t; uint32_t pos = 0;
    for (;;) {
        int is_close = 0;
        if (!next_tag(&cur, pos, &t, &is_close)) break;
        pos = t.after;
        if (!strcmp(t.name, "misterromdescription")) continue;
        if (!strcmp(t.name, "buttons")) {
            out->has_buttons = 1;
            tag_attr(&cur, &t, "default", out->button_default, sizeof out->button_default);
            tag_attr(&cur, &t, "names", out->button_names, sizeof out->button_names);
        } else if (!strcmp(t.name, "nvram")) {
            out->has_nvram = 1;
            char valbuf[16];
            out->nvram_index = tag_attr(&cur, &t, "index", valbuf, sizeof valbuf) ? atoi(valbuf) : 0;
            out->nvram_size = tag_attr(&cur, &t, "size", valbuf, sizeof valbuf) ? atoi(valbuf) : 0;
        }
        if (!t.self_close) pos = skip_to_close(&cur, t.after, size, t.name);   /* every top-level tag's
            * own close -- including <buttons>/<nvram> themselves once their attributes are already
            * read, same discipline as every other top-level scan in this file (find_next_top_level's
            * own doc comment has the full reasoning for why this matters) */
    }

    mra_io->close(mra_io->ctx);
    return 1;
}
