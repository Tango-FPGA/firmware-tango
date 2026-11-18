// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 the Tango authors
/*
 * tango_mra.h -- arcade .mra loading: find the <rbf> tag's text in a .mra file, and
 * resolve it to a core bitstream under _Arcade/cores/ by MiSTer's matching rule.
 *
 * Tango. Our own code, written from the documented .mra format and from MiSTer's behaviour in
 * Main_MiSTer's support/arcade/mra_loader.cpp (xml_scan_rbf 956-991, get_rbf 1129-1205).
 *
 * The <rbf> lookup reads only the <rbf> tag: a value this narrow (one short tag, usually near the top of the file,
 * but after any header comment of any length) needs no general parser. Never buffers the whole file (the
 * ROM-assembly RAM budget applies here too): tango_mra_find_rbf reads in small chunks through the same
 * byte-addressable file interface tango_zip.h already defines for one file on the card.
 */
#ifndef TANGO_MRA_H
#define TANGO_MRA_H

#include "tango_zip.h"      /* tango_zipfs_t: byte-addressable access to one file on the card */
#include "tango_browser.h"  /* tango_fs_t: a directory-listing source; TANGO_PATH_MAX */
#include "tango_md5.h"       /* the discard-on-mismatch rule */

#define TANGO_MRA_RBF_MAX 64     /* MiSTer's rbf fragment buffer is 1024 (kBigTextSize); core names are short */

/* Reads path (an .mra file) through io and extracts its <rbf>...</rbf> text, trimmed of surrounding
 * whitespace: the first "<rbf>" start tag found (case-insensitive, no attributes -- every real .mra uses
 * the plain form) outside an XML comment, up to the next '<'. The whole file is scanned if need be, in
 * fixed 128-byte reads with the match state carried from one read to the next (a tag split across two
 * reads is still found), and the scan stops at that '<': nothing after the first <rbf> (the <rom>/<patch>
 * data, tens of KB in some files) is read. A "<!--" ... "-->" comment is skipped whole, as MiSTer's XML
 * parser does (sxmlc.c 71 and 1075-1091 deliver a comment as one TAG_COMMENT node whose tag is the whole
 * comment text, so a <rbf> inside one never passes xml_scan_rbf's strcasecmp(node->tag, "rbf"),
 * mra_loader.cpp 1006-1041, local Main_MiSTer copy at c3dc8ed) and as next_tag (tango_mra.c) does for P1:
 * a header comment that names <rbf> in its text, or is long enough to push the tag past any fixed offset,
 * neither hides the real tag nor stands in for it.
 * MiSTer's xml_scan_rbf keeps the LAST <rbf> text it sees; real files carry exactly one, and this keeps the
 * first so the scan can end there. 1 = found (rbf is NUL-terminated), 0 = not found (no <rbf>, an empty
 * one, an I/O error before it, or the file doesn't open). */
int tango_mra_find_rbf(const tango_zipfs_t *io, const char *path, char *rbf, int cap);

/* Matches rbf against the entries of the folder at path (opened via fs, e.g. "_Arcade/cores"), MiSTer's
 * rule for the arcade branch of get_rbf() (mra_loader.cpp 1176-1197): a file named "Arcade-<rbf>" or
 * "<rbf>" followed by '.' or '_', case-insensitive, and recognised as one of ours (tango_is_core_file:
 * .fs or .bin, not MiSTer's .rbf). Both forms are candidates in the same pass; the
 * lexicographically-greatest matching name wins (MiSTer's strcmp-greater-replaces loop, so a later date
 * code wins). 1 = found (out is just the file name, no path, NUL-terminated), 0 = no match or the folder
 * doesn't open. */
int tango_mra_resolve_core(const tango_fs_t *fs, const char *path, const char *rbf, char *out, int cap);

/* ------------------------------------------------------------------ <rom>/<part> assembly
 *
 * Scope: no interleave (refused -- see below), everything else real files use. A <rom>
 * containing <interleave>, or a <part> with its OWN zip="" override (differing from the <rom>-level
 * one), is refused (TANGO_MRA_REFUSED) rather than silently assembled wrong. <part
 * repeat=""/offset=""/length=""> and <rom>-level zip="a|b|c" fallback ARE supported (Galaga's real
 * romset needs the latter). <patch> and the
 * discard-on-md5-mismatch rule ARE supported -- see tango_mra_rom_read's own doc for
 * both; neither needed the memory tradeoff interleave does (below), since a <patch>'s hex text can be
 * re-read from the .mra itself in place, and an md5 mismatch is reported through the read API rather
 * than retroactively un-sending already-streamed bytes.
 *
 * Why <interleave> specifically stays refused: MiSTer processes one <part> at a time to completion
 * (rom_file reads a whole source file before the next one starts), relying on a full in-RAM
 * assembly buffer to reorder everything afterward. Streaming instead (this module's whole point)
 * would need multiple source zip members open AT ONCE, merged byte-lane by byte-lane as output is
 * produced -- each one needing its own inflate window (32 KB) if compressed, so even a 2-way
 * interleave could cost 64 KB simultaneously, on a platform whose entire proven-free OCRAM heap is
 * ~79 KB (tango_fw.c's RAM check). Not a coding gap: a real hardware-budget tradeoff (cap the
 * supported width? require the members be stored, not deflated? refuse outright, as now?) that needs
 * a design decision, not a guess.
 *
 * Streams a chunk at a time (tango_dl_pump's rd() shape, tango_coremenu.h) with no multi-MB buffer:
 * a file-backed <part> pulls straight from tango_zip_read; an inline-hex <part> (no name attribute --
 * MiSTer's "mod byte" convention) decodes two hex digits per output byte straight out of the .mra as
 * it is scanned, carrying at most one pending nibble across calls. Not a general XML parser (no
 * entities, no CDATA, no PIs -- those really don't appear; comments DO, between <part> siblings,
 * e.g. "1942 (Revision B).mra" -- next_tag handles "<!--...-->" specifically, not just "skip to the
 * next '>'", since a comment's own text could in principle contain one): a narrow tag/attribute
 * scanner, handling both quote styles and
 * self-closing tags because real files use both (confirmed: 7877 self-closing vs. 3134 open/close
 * <part> tags in that same library). */

#define TANGO_MRA_ZIP_NAME_MAX  192   /* a <rom zip="a|b|c"> attribute verbatim: MiSTer's own buffer for
                                       * it is kBigTextSize (1024); real pipe lists (Galaga's own: 4
                                       * entries, 49 characters) are nowhere near that -- generous anyway */
#define TANGO_MRA_PART_NAME_MAX 192   /* a <part name="..."> value: a zip-internal path, generous */
#define TANGO_MRA_MAX_ZIPS 4          /* a <rom zip="a|b|c|d">'s longest real pipe list seen (Galaga's) */
#define TANGO_MRA_PATCH_MAX 160       /* the most <patch> tags seen in one real <rom> across a 679-file
                                       * library ("Frogger (Konami Bugfixed).mra", 146); refusing
                                       * cleanly past this is simpler than a dynamic list, and nothing
                                       * real comes remotely close except that one file */
#define TANGO_MRA_REFUSED (-2)        /* tango_mra_rom_read: this <rom> needs interleave or a part-level
                                       * zip override, or has more <patch> tags than TANGO_MRA_PATCH_MAX --
                                       * cannot be assembled. Distinct from -1 (a real I/O or parse error)
                                       * so a caller can tell "not supported" from "something is broken". */
#define TANGO_MRA_MD5_MISMATCH (-3)   /* tango_mra_rom_read: the assembled bytes (pre-patch, matching
                                       * what MiSTer itself hashes -- rom_patch never re-touches the MD5
                                       * context) didn't match this <rom>'s own md5="" attribute. Returned
                                       * at the point the LAST byte would otherwise have completed the
                                       * stream (0): every byte already produced is correct and already
                                       * handed to the caller -- MiSTer's own "discard, don't send"
                                       * (rom_finish) isn't reachable in a streaming design once sending
                                       * has already started, so this reports the failure instead of
                                       * trying to retroactively un-send what went out already. */

typedef struct {
    uint32_t offset, length;      /* within the assembled (pre-patch) output */
    int      xor_op;              /* 0 = overwrite (the default), 1 = operation="xor" */
    uint32_t text_start, text_end;/* the patch's own hex text's byte range in the .mra -- re-read and
                                    * decoded on demand when the matching output range is produced,
                                    * never buffered (not even for a large patch: one real file has a
                                    * 32 KB one, "Block Gal.mra" -- the RAM-budget rule) */
} tango_mra_patch_t;

typedef struct {
    uint32_t mra_size;
    uint32_t pos;                 /* next byte to resume scanning from, in the .mra (never an implicit
                                    * stream cursor -- tango_zipfs_t's read() is offset-based, so every
                                    * scan here just re-reads whatever range it needs) */
    int      want_index;          /* tango_mra_rom_open's argument; -1 when opened by position (_open_nth) */
    int      index;               /* the opened <rom>'s own index="" (atoi as mra_loader.cpp; absent = 0) */
    int      found;               /* a <rom index="want_index"> tag was actually seen (even if empty) --
                                    * lets a caller tell "declared empty" from "never declared" if it cares;
                                    * both behave the same (zero bytes), matching MiSTer */
    enum {
        TANGO_MRA_ROM_SEEK_PART, TANGO_MRA_ROM_PART_FILE, TANGO_MRA_ROM_PART_INLINE,
        TANGO_MRA_ROM_DONE, TANGO_MRA_ROM_REFUSED_ST, TANGO_MRA_ROM_ERROR_ST
    } phase;
    /* the <part> currently being produced */
    char     part_name[TANGO_MRA_PART_NAME_MAX];   /* "" = inline hex, not a file */
    uint32_t part_crc;
    uint32_t part_offset, part_length;    /* length 0 = "the rest of the member" */
    uint32_t part_close_after;            /* where to resume SEEK_PART once this part is done: just past
                                            * its own "</part>" (computed once, at part-open, since a
                                            * file-based part's body is otherwise empty -- nothing else
                                            * would ever notice where its close tag is) */
    int      repeat_left;                 /* counts down; a repeat re-opens the same member from part_offset */
    /* PART_FILE: which of tango_mra_rom_read's zips[]/zip_ios[] slot actually has this part's member
     * open (tango_mra_rom_read tries them in order per MiSTer's own per-part fallback trial, so this
     * can differ from part to part within the same <rom>); -1 = none open right now */
    int      zip_mem_open_slot;
    uint32_t file_left;                   /* PART_FILE: bytes still owed for the CURRENT repeat */
    uint32_t inline_start, inline_end;    /* PART_INLINE: the hex text's byte range in the .mra (between
                                            * the open tag's '>' and the closing '<'); a repeat rescans
                                            * the same range rather than caching the decoded bytes --
                                            * inline parts are always a handful of bytes */
    int      hex_have_nibble;
    uint8_t  hex_nibble;

    /* md5 and patches, spanning the WHOLE rom (not per-part, unlike everything above) */
    uint32_t out_pos;              /* total pre-patch bytes produced so far for this rom, across every
                                     * part -- the coordinate patch ranges and the md5 context are in */
    tango_md5_t md5_ctx;
    int      md5_checked;          /* the final comparison runs once, the first time a read reports
                                     * 0 (done) -- never again on a later call that also reports 0 */
    char     expected_md5[33];     /* "" or "none" (MiSTer's own no-check sentinels): never checked */
    int      n_patches;
    tango_mra_patch_t patches[TANGO_MRA_PATCH_MAX];
} tango_mra_rom_t;

/* Finds the first <rom index="want_index"> in the .mra at mra_path (opened fresh here and left open --
 * tango_mra_rom_close ends the session) and, if it names one, copies its zip="" attribute verbatim
 * into out_zip (zip_name_cap bytes; "" if the rom has no zip, e.g. an inline-only "mod byte" rom like
 * the sample's index 3) -- a|b|c pipe-separated if the .mra lists more than one, split by the caller
 * (e.g. with strtok on '|') before opening each one; tango_mra_rom_read never sees the names, only the
 * already-opened archives. Also captures the rom's own md5="" (if any -- "" and "none" both mean no
 * check, MiSTer's own convention) and pre-scans its body for every <patch> tag (just the tags: offset,
 * operation, and where its hex text lives in the .mra -- never the patch bytes themselves, even for a
 * large one). More than TANGO_MRA_PATCH_MAX patches makes the rom refused (TANGO_MRA_REFUSED) from the
 * very first tango_mra_rom_read call, not a silent truncation. Leaves *r ready for tango_mra_rom_read
 * to stream that rom's parts. Always returns 1 (even "no such index" and "found but empty" succeed,
 * with zero bytes to read -- see r->found); 0 only if the .mra itself won't open. */
int tango_mra_rom_open(tango_mra_rom_t *r, const tango_zipfs_t *mra_io, const char *mra_path,
                       int want_index, char *out_zip, int zip_name_cap);

/* tango_mra_rom_open, but for the nth top-level <rom> of the file (0-based document order, whatever its index;
 * r->index is that rom's index=""). r->found == 0 = the file has fewer than nth+1 <rom> tags. Same return and
 * same open-file contract as tango_mra_rom_open. */
int tango_mra_rom_open_nth(tango_mra_rom_t *r, const tango_zipfs_t *mra_io, const char *mra_path,
                           int nth, char *out_zip, int zip_name_cap);

/* tango_dl_pump's rd() shape: produces up to n assembled bytes for the <rom> tango_mra_rom_open found,
 * in <part> document order, with every <patch> already applied in place before a byte is ever handed
 * back (the patch pre-scan's own hex text is re-read and decoded from the .mra on demand -- it is
 * never buffered, even for the one real patch that runs 32 KB). zips[0..n_zips) / zip_ios[0..n_zips)
 * must already be opened, in the same order as out_zip's pipe-split names (the caller resolves each
 * name to an actual card path -- menu-tango docs/LAUNCHER.md's "/games/mame/<set>.zip, else /_Arcade/mame/<set>.zip"
 * -- this function only calls tango_zip_locate/tango_zip_open_member/tango_zip_read on them); for each
 * file-based <part>, every zip is tried in order until one has it (MiSTer's own per-part fallback
 * trial, mra_loader.cpp 853-879) -- n_zips may be 0 (pass NULL for both arrays) if out_zip was "" (an
 * inline-only rom never touches them). Returns 0 once every part has been produced and, if the rom
 * declared an md5="", it matched (not an error -- same as MiSTer's own rolling romlen[], the total
 * length is never known up front); TANGO_MRA_MD5_MISMATCH at that same point if it didn't (every byte
 * already produced in earlier calls is still correct -- see its own definition above for why this
 * can't be "discard and send nothing", MiSTer's own rule, once streaming has already started);
 * TANGO_MRA_REFUSED if this <rom> needs something not supported (interleave, a part-level zip
 * override, or more patches than the cap -- may be returned after some bytes were already produced
 * for earlier parts: those are correct, the rom is only refused once the scan actually reaches the
 * part that needs it); or -1 on a real I/O/parse error (including a file-based part found in none of
 * the given zips). */
int tango_mra_rom_read(tango_mra_rom_t *r, const tango_zipfs_t *mra_io,
                       tango_zip_t *const zips[], const tango_zipfs_t *const zip_ios[], int n_zips,
                       uint8_t *buf, unsigned n);

/* Closes the .mra file tango_mra_rom_open opened, and (if one is open) the zip member tango_mra_rom_read
 * left open on whichever of its zips[]/zip_ios[] slots last served a part -- pass the SAME arrays (by
 * slot position) tango_mra_rom_read was last called with, so the right one gets closed; NULL/0 if no
 * file-based part was ever read (nothing to close there). The archives themselves (tango_zip_open_archive)
 * are the caller's to close, since the caller opened them. Safe to call more than once. */
void tango_mra_rom_close(tango_mra_rom_t *r, const tango_zipfs_t *mra_io,
                         tango_zip_t *const zips[], const tango_zipfs_t *const zip_ios[]);

/* ------------------------------------------------------------------ every <rom>, in document order
 *
 * MiSTer's loader (mra_loader.cpp: xml_send_rom, rom_start, rom_finish) assembles each <rom> of the file in
 * document order and, at its </rom>, sends it as one download of its own: ioctl_index = the rom's index=""
 * (user_io_set_index(romindex)), the bytes, then the close. Per <rom>:
 *   - no data (an empty <rom>, or parts that add up to nothing): nothing is sent;
 *   - md5="" given (not "" or "none") and the rom has a zip="": a mismatch discards the rom, nothing is sent;
 *     a rom without zip="" is sent whatever its md5 (checksumsame = !strlen(zipname) || match);
 *   - once a rom with index 0 has matched a declared md5 (validrom0), every later index-0 rom adds no data
 *     (merged/non-merged alternatives of the same set) and is therefore not sent;
 *   - a rom that cannot be assembled here (a part in none of its zips, an I/O error, interleave or a part-level
 *     zip override -- TANGO_MRA_REFUSED) is not sent; the following roms still are.
 * The download protocol needs each size before the first byte (tango_dl_begin's SELECT), so
 * tango_mra_seq_plan assembles every rom once into a scratch buffer to count it and check its md5, and
 * records the outcome; tango_mra_seq_next then re-opens the next sendable rom for the real stream
 * (tango_mra_seq_read, tango_dl_pump's rd() shape). The archives a rom names are opened through the caller's
 * tango_mra_zips_t, only while that rom is assembled or streamed. */

#define TANGO_MRA_SEQ_MAX 16          /* <rom> entries planned per file; a 2093-file library has at most 8 */
#define TANGO_MRA_SEQ_SEND        1   /* tango_mra_seq_rom_t.status: this rom is sent */
#define TANGO_MRA_SEQ_EMPTY       0   /*   no data: not sent */
#define TANGO_MRA_SEQ_SUPPRESSED (-4) /*   an index-0 rom after one that matched its md5: not sent */
#define TANGO_MRA_SEQ_OVER_CAP   (-5) /*   past TANGO_MRA_SEQ_MAX: this and every later <rom> are not planned */
                                      /*   -1, TANGO_MRA_REFUSED, TANGO_MRA_MD5_MISMATCH: as tango_mra_rom_read */

typedef struct {
    uint8_t  ordinal;          /* position among the file's top-level <rom> tags */
    uint8_t  index;            /* the ioctl index: index="" as an unsigned char (rom_start's parameter) */
    int8_t   status;           /* TANGO_MRA_SEQ_* or a tango_mra_rom_read error code */
    uint8_t  no_md5_check;     /* a rom without zip="" whose md5 did not match: streamed without the check */
    uint32_t size;             /* bytes to send (status TANGO_MRA_SEQ_SEND) */
} tango_mra_seq_rom_t;

typedef struct {
    /* opens the archives of a <rom zip="a|b|c"> list, one slot per name in list order (an archive that is
     * not on the card still takes its slot, left not open -- tango_mra_rom_read skips it); returns the slot
     * count (at most TANGO_MRA_MAX_ZIPS) */
    int  (*open)(void *ctx, const char *zip_list, tango_zip_t *zips[], const tango_zipfs_t *zip_ios[]);
    void (*close)(void *ctx, tango_zip_t *zips[], const tango_zipfs_t *zip_ios[], int n);
    void *ctx;
} tango_mra_zips_t;

typedef struct {
    const tango_zipfs_t *mra_io;
    const char *mra_path;      /* the caller's string: must outlive the sequence */
    tango_mra_zips_t zcb;
    tango_mra_rom_t rom;       /* the rom being planned or streamed */
    tango_zip_t *zips[TANGO_MRA_MAX_ZIPS];
    const tango_zipfs_t *zip_ios[TANGO_MRA_MAX_ZIPS];
    int      n_zips;
    int      rom_open;         /* rom (and its archives) open for streaming */
    int      n;                /* plan[] entries */
    int      cur;              /* the plan[] entry streamed now (or last), -1 = none yet */
    tango_mra_seq_rom_t plan[TANGO_MRA_SEQ_MAX];
} tango_mra_seq_t;

/* called once per planned <rom> (and once for the first one past TANGO_MRA_SEQ_MAX), after its assembly:
 * r->part_name / r->out_pos still say where an assembly stopped */
typedef void (*tango_mra_seq_note_fn)(void *ctx, const tango_mra_seq_rom_t *e, const tango_mra_rom_t *r);

/* Plans every top-level <rom> of the .mra at mra_path in document order (rules above), assembling each one
 * through scratch (any size; 512 is plenty). Every file is closed again on return. Returns the number of roms
 * to send, or -1 if the .mra does not open. */
int tango_mra_seq_plan(tango_mra_seq_t *q, const tango_zipfs_t *mra_io, const char *mra_path,
                       const tango_mra_zips_t *zcb, uint8_t *scratch, unsigned scratch_len,
                       tango_mra_seq_note_fn note, void *note_ctx);

/* Closes the rom streamed so far (if any) and opens the next plan[] entry with status TANGO_MRA_SEQ_SEND
 * after q->cur for streaming: 1 = q->plan[q->cur] is open (its index and size are the download's), 0 = none
 * left. An entry that no longer opens is marked -1 and skipped. */
int tango_mra_seq_next(tango_mra_seq_t *q);

/* tango_dl_pump's rd(): the open rom's next bytes (ctx = the tango_mra_seq_t); < 0 as tango_mra_rom_read */
int tango_mra_seq_read(void *ctx, uint8_t *buf, unsigned n);

/* Closes the rom being streamed and its archives (safe when none is open, and more than once). */
void tango_mra_seq_close(tango_mra_seq_t *q);

/* 1 if the plan has a <rom> with this index="" that is sent (status TANGO_MRA_SEQ_SEND), else 0. */
int tango_mra_seq_has_data(const tango_mra_seq_t *q, uint8_t index);

/* ------------------------------------------------------------------ a blank hi-score .nvm
 *
 * <rom index="3"> is the configuration of MiSTer's hiscore.v (the addresses of the game's score table): it must
 * reach the core before the table can be read back or written. Saved through a firmware that never sent it, the
 * <nvram> upload never followed ioctl_addr, and the saved .nvm is <nvram size=""> copies of one byte. Sending such
 * a file writes that byte over the game's own table, and the next save keeps it. With a sent <rom index="3">, a
 * saved .nvm whose bytes are all identical is therefore not sent and is removed (tango_fw.c's arc_setup). */
#define TANGO_MRA_HISCORE_INDEX 3

/* 1 if n > 0 and every one of the n bytes at p equals p[0], else 0 */
int tango_mra_bytes_uniform(const uint8_t *p, uint32_t n);

/* ------------------------------------------------------------------ <switches>/<cheats> (DIPs)
 *
 * MiSTer's arcade DIPs (and cheats, the same mechanism) live entirely in the .mra, never in a core's
 * own CONF_STR -- a completely separate path from the option word every other menu page uses. One
 * 64-bit word (dip_cur) is sent whole to a fixed ioctl index (254 for switches, 255 for cheats,
 * arcade_sw_send, mra_loader.cpp 116-126); mra_loader.h's own caps (dip[64], id[32][32]) are kept
 * here unchanged, since nothing real has ever needed more. */

#define TANGO_MRA_DIP_MAX      64   /* mra_loader.h sw_struct.dip[64] */
#define TANGO_MRA_DIP_IDS_MAX   32  /* mra_loader.h dip_struct.id[32][...] */
#define TANGO_MRA_DIP_NAME_MAX  32  /* mra_loader.h dip_struct.name[32] */
#define TANGO_MRA_DIP_ID_LEN_MAX 32 /* mra_loader.h dip_struct.id[...][32] */

typedef struct {
    char     name[TANGO_MRA_DIP_NAME_MAX];       /* <dip name="..."> */
    int      start, size;                        /* <dip bits="b[,e]">: start=b, size=e-b+1 */
    uint64_t mask;                                /* size-bit mask, shifted into position: ((1<<size)-1) << start */
    int      num;                                 /* how many ids/values (0 if the <dip> gave neither) */
    int      has_val;                              /* values="" was given; else val[i] defaults to i */
    char     id[TANGO_MRA_DIP_IDS_MAX][TANGO_MRA_DIP_ID_LEN_MAX];   /* <dip ids="a,b,..">, comma-split */
    uint64_t val[TANGO_MRA_DIP_IDS_MAX];           /* already shifted by `start`, MiSTer's own convention
                                                     * (xml_send_rom's </dip> handler) -- ready to OR into
                                                     * dip_cur directly, never needs re-shifting by a reader */
} tango_mra_dip_t;

typedef struct {
    uint64_t dip_def;              /* <switches default="1F,1F,04">: hex bytes, little-endian into the word */
    int      dip_num;              /* how many <dip> entries were found, in document order */
    tango_mra_dip_t dip[TANGO_MRA_DIP_MAX];
} tango_mra_switches_t;

/* Parses the first <switches> (cheats=0) or <cheats> (cheats=1) block in the .mra at mra_path --
 * MiSTer's own rule, read from mra_loader.cpp's xml_send_rom (472-488 for the block itself, 603-684
 * for each nested <dip>'s attributes, 914-925 for what happens at </dip>: val[i] = (has_val ? the
 * given value : i) << start, dip_num capped at 63, then incremented). No such block in the file is
 * not an error -- *out comes back zeroed (dip_num 0, dip_def 0), matching a core with no DIPs at all.
 * 1 = parsed (whether or not a block was actually found -- check out->dip_num / out->dip_def if the
 * difference matters); 0 only if the .mra itself won't open. */
int tango_mra_switches_parse(const tango_zipfs_t *mra_io, const char *mra_path, int cheats,
                             tango_mra_switches_t *out);

/* dip_def/dip_num only, never the full per-<dip> table (tango_mra.c's own
 * comment at the definition: sizeof(tango_mra_switches_t) is ~83.5 KB, too large for the BL616's own OCRAM
 * heap pool to ever hand back in one malloc -- this is the version the DIP/cheat send actually needs,
 * which only ever reads dip_def/dip_num, never a single tango_mra_dip_t entry). Same return convention as
 * tango_mra_switches_parse (1 = parsed, whether or not a block was found; 0 only if the .mra won't open). */
int tango_mra_switches_summary(const tango_zipfs_t *mra_io, const char *mra_path, int cheats,
                               uint64_t *dip_def, int *dip_num);

/* The DIP page's own per-row parse, RAM-bounded: a page's own rows are parsed on demand, never the whole
 * table. Parses ONLY the `index`-th <dip> (0-based, document order) of the first
 * <switches> (cheats=0) or <cheats> (cheats=1) block into *out, on the CALLER's own stack -- never the
 * whole tango_mra_switches_t (~83.5 KB). Internally the exact same scan as
 * tango_mra_switches_parse, just stopped the instant the wanted <dip> is found instead of continuing to
 * fill an array -- a page's own K rows cost K independent, bounded re-scans from the top of <switches>
 * (always small in every real file seen: the biggest, Galaga's own, is 11 entries in well under 2 KB of
 * XML), not one scan whose own cost grows with K. Returns 1 if the .mra opened AND a <dip> at that index
 * exists (out populated, same fields/shift convention as tango_mra_switches_parse's own dip[] entries); 0
 * if the .mra would not open, or there is no such index (including index < 0 or >= TANGO_MRA_DIP_MAX, the
 * same cap every other function here enforces) -- out is zeroed in that case too, never left stale. */
int tango_mra_switches_dip_at(const tango_zipfs_t *mra_io, const char *mra_path, int cheats,
                              int index, tango_mra_dip_t *out);

/* ------------------------------------------------------------------ <buttons>, <nvram> (data only)
 *
 * Both are plain top-level tags, siblings of <rom>/<switches>, parsed here the same narrow way: the
 * data only, not the mechanism that would consume it. <buttons default="" names=""> just stores the
 * two raw comma-separated strings verbatim -- MiSTer's own set_ovr_buttons (input.cpp) does exactly
 * that (snprintf into a 1024-byte buffer; splitting them into individual button names is
 * parse_buttons()'s job, a pad-mapping concern, not this file's). <nvram index="" size=""> is the
 * battery-save index/size MiSTer's arcade_nvm_load/save key off of (mra_loader.cpp 60-107) -- the
 * actual load/save is firmware file I/O (tango_save.c's save-RAM path is close but not identical: a
 * fixed-size NVRAM image isn't the same shape as a growing sector-addressed one), not built here. */
typedef struct {
    int  has_buttons;
    char button_default[128];   /* <buttons default="..."> verbatim */
    char button_names[128];     /* <buttons names="..."> verbatim */
    int  has_nvram;
    int  nvram_index;
    int  nvram_size;
} tango_mra_misc_t;

/* Parses the .mra at mra_path's top-level <buttons> and <nvram> tags (each optional; a real file may
 * have either, both, or neither -- out's corresponding has_* flags say which). 1 = parsed cleanly
 * (regardless of whether either tag was actually present); 0 only if the .mra itself won't open. */
int tango_mra_misc_parse(const tango_zipfs_t *mra_io, const char *mra_path, tango_mra_misc_t *out);

#endif
