// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 the Tango authors
/*
 * test_zip.c -- host tests for tango_zip.c: container parsing (real, standards-compliant .zip byte streams,
 * fake_zip_fixtures.h) and the streaming decompressor, including the repeated enter/exit state-cleanup cases:
 * re-opening the same archive, a different archive after closing one, backing out
 * without ever opening a member, and opening a member while another is already open.
 * Tango, our own code.
 */
#include <stdio.h>
#include <string.h>
#include "../tango_zip.h"
#include "fake_zip.h"
#include "fake_zip_fixtures.h"

static int fails = 0, checks = 0;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)

/* reads a whole member via tango_zip_read, in the given chunk sizes (cycled), into out (cap bytes); returns
 * the total produced, or -1 on any read() error -- mirrors how tango_dl_pump actually drives it */
static int read_all(tango_zip_t *z, const tango_zipfs_t *io, uint8_t *out, unsigned cap,
                     const unsigned *chunk_sizes, int n_sizes)
{
    unsigned total = 0; int si = 0;
    for (;;) {
        unsigned ask = chunk_sizes[si % n_sizes]; si++;
        if (total >= cap) break;
        if (ask > cap - total) ask = cap - total;
        int got = tango_zip_read(z, io, out + total, ask);
        if (got < 0) return -1;
        if (got == 0) break;
        total += (unsigned)got;
    }
    return (int)total;
}

static const unsigned SIZES[] = { 1, 3, 17, 512, 599, 255, 2, 500, 31, 128, 32768, 32767, 4, 600 };
#define N_SIZES (int)(sizeof SIZES / sizeof SIZES[0])

int main(void)
{
    tango_zip_t z; tango_zipfs_t io; fake_zipmem_t mem;
    static uint8_t out[64 * 1024];

    /* ---- 1. a single matching entry: scan, open, stream-read, exact content ---- */
    fake_zipmem_init(&mem, &io, FX_SINGLE, FX_SINGLE_len);
    memset(&z, 0, sizeof z);
    CHECK(tango_zip_open_archive(&z, &io, "games/NES/game.zip"));
    CHECK(tango_zip_scan(&z, &io, "NES") == 1);
    CHECK(!strcmp(tango_zip_name(&z, 0), "GAME.NES"));
    CHECK(tango_zip_find(&z, "game.nes") == 0);         /* case-insensitive */
    CHECK(tango_zip_find(&z, "nope.nes") == -1);
    uint32_t usize = 0;
    CHECK(tango_zip_open_member(&z, &io, 0, &usize) == 0);
    CHECK(usize == FX_SINGLE_GAME_len);
    int got = read_all(&z, &io, out, sizeof out, SIZES, N_SIZES);
    CHECK(got == (int)FX_SINGLE_GAME_len);
    CHECK(got > 0 && !memcmp(out, FX_SINGLE_GAME, FX_SINGLE_GAME_len));
    tango_zip_close_member(&z);
    tango_zip_close(&z, &io);
    CHECK(mem.open_calls == 1 && mem.close_calls == 1);  /* symmetric: no leak, no double-close */

    /* ---- 2. multiple matching entries + one non-matching; both readable by name ---- */
    fake_zipmem_init(&mem, &io, FX_MULTI, FX_MULTI_len);
    memset(&z, 0, sizeof z);
    CHECK(tango_zip_open_archive(&z, &io, "games/NES/multi.zip"));
    CHECK(tango_zip_scan(&z, &io, "NES") == 2);          /* readme.txt excluded */
    int iu = tango_zip_find(&z, "Game (U).nes");
    int ie = tango_zip_find(&z, "Game (E).nes");
    CHECK(iu >= 0 && ie >= 0 && iu != ie);
    CHECK(tango_zip_open_member(&z, &io, iu, &usize) == 0 && usize == FX_MULTI_U_len);
    got = read_all(&z, &io, out, sizeof out, SIZES, N_SIZES);
    CHECK(got == (int)FX_MULTI_U_len && !memcmp(out, FX_MULTI_U, FX_MULTI_U_len));
    /* opening a second member while the first is still "open" must close the first cleanly first */
    CHECK(tango_zip_open_member(&z, &io, ie, &usize) == 0 && usize == FX_MULTI_E_len);
    got = read_all(&z, &io, out, sizeof out, SIZES, N_SIZES);
    CHECK(got == (int)FX_MULTI_E_len && !memcmp(out, FX_MULTI_E, FX_MULTI_E_len));
    tango_zip_close(&z, &io);

    /* ---- 3. no match at all: the archive stays open, the caller decides what to do ---- */
    fake_zipmem_init(&mem, &io, FX_MULTI, FX_MULTI_len);
    memset(&z, 0, sizeof z);
    CHECK(tango_zip_open_archive(&z, &io, "x.zip"));
    CHECK(tango_zip_scan(&z, &io, "TXT") == 1);           /* only readme.txt matches a TXT-only core (hypothetically) */
    CHECK(tango_zip_scan(&z, &io, "BIN") == 0);            /* re-scanning the SAME open archive with a different
                                                            * extension list must not accumulate stale entries */
    tango_zip_close(&z, &io);

    /* ---- 4. a stored (uncompressed) entry ---- */
    fake_zipmem_init(&mem, &io, FX_STORED, FX_STORED_len);
    memset(&z, 0, sizeof z);
    CHECK(tango_zip_open_archive(&z, &io, "s.zip"));
    CHECK(tango_zip_scan(&z, &io, "NES") == 1);
    CHECK(tango_zip_open_member(&z, &io, 0, &usize) == 0 && usize == FX_STORED_GAME_len);
    got = read_all(&z, &io, out, sizeof out, SIZES, N_SIZES);
    CHECK(got == (int)FX_STORED_GAME_len && !memcmp(out, FX_STORED_GAME, FX_STORED_GAME_len));
    tango_zip_close(&z, &io);

    /* ---- 5. a .ZIP file comment containing a decoy EOCD signature: the real one must still be found ---- */
    fake_zipmem_init(&mem, &io, FX_COMMENT, FX_COMMENT_len);
    memset(&z, 0, sizeof z);
    CHECK(tango_zip_open_archive(&z, &io, "c.zip"));
    CHECK(tango_zip_scan(&z, &io, "NES") == 1);
    CHECK(!strcmp(tango_zip_name(&z, 0), "commented.nes"));
    CHECK(tango_zip_open_member(&z, &io, 0, &usize) == 0 && usize == FX_COMMENT_GAME_len);
    got = read_all(&z, &io, out, sizeof out, SIZES, N_SIZES);
    CHECK(got == (int)FX_COMMENT_GAME_len && !memcmp(out, FX_COMMENT_GAME, FX_COMMENT_GAME_len));
    tango_zip_close(&z, &io);

    /* ---- 6. a >32 KB entry: the inflate window must wrap correctly end to end, via the real struct ---- */
    fake_zipmem_init(&mem, &io, FX_BIG, FX_BIG_len);
    memset(&z, 0, sizeof z);
    CHECK(FX_BIG_GAME_len > 32768);
    CHECK(tango_zip_open_archive(&z, &io, "b.zip"));
    CHECK(tango_zip_scan(&z, &io, "NES") == 1);
    CHECK(tango_zip_open_member(&z, &io, 0, &usize) == 0 && usize == FX_BIG_GAME_len);
    got = read_all(&z, &io, out, sizeof out, SIZES, N_SIZES);
    CHECK(got == (int)FX_BIG_GAME_len);
    CHECK(got > 0 && !memcmp(out, FX_BIG_GAME, FX_BIG_GAME_len));
    tango_zip_close(&z, &io);

    /* ---- 7. not a zip at all: a clean failure, not a crash or a bogus "success" ---- */
    {
        static const unsigned char garbage[64] = { 0 };
        fake_zipmem_t m2; tango_zipfs_t io2;
        fake_zipmem_init(&m2, &io2, garbage, sizeof garbage);
        tango_zip_t z2; memset(&z2, 0, sizeof z2);
        CHECK(!tango_zip_open_archive(&z2, &io2, "junk.zip"));
        tango_zip_close(&z2, &io2);          /* must be safe even though open_archive already failed/closed */
    }

    /* ---- 8. a card that refuses to open (removed, I/O error) ---- */
    {
        fake_zipmem_t m2; tango_zipfs_t io2;
        fake_zipmem_init(&m2, &io2, FX_SINGLE, FX_SINGLE_len);
        m2.fail_open = 1;
        tango_zip_t z2; memset(&z2, 0, sizeof z2);
        CHECK(!tango_zip_open_archive(&z2, &io2, "nope.zip"));
        tango_zip_close(&z2, &io2);
    }

    /* ---- 9. repeated enter/exit state-cleanup cases ---- */
    {
        fake_zipmem_t mA, mB; tango_zipfs_t ioA, ioB;
        tango_zip_t zz; memset(&zz, 0, sizeof zz);

        /* 9a. backing out without ever opening a member: open, scan, close -- then the exact same archive again */
        fake_zipmem_init(&mA, &ioA, FX_SINGLE, FX_SINGLE_len);
        CHECK(tango_zip_open_archive(&zz, &ioA, "a.zip"));
        CHECK(tango_zip_scan(&zz, &ioA, "NES") == 1);
        tango_zip_close(&zz, &ioA);                         /* no member was ever opened */
        CHECK(mA.open_calls == 1 && mA.close_calls == 1);

        /* 9b. re-entering the SAME zip a second time must give the identical listing, not an empty or doubled one */
        CHECK(tango_zip_open_archive(&zz, &ioA, "a.zip"));
        CHECK(tango_zip_scan(&zz, &ioA, "NES") == 1);
        CHECK(!strcmp(tango_zip_name(&zz, 0), "GAME.NES"));
        uint32_t us2 = 0;
        CHECK(tango_zip_open_member(&zz, &ioA, 0, &us2) == 0 && us2 == FX_SINGLE_GAME_len);
        /* read only PART of it this time, then close mid-stream (as "back out before it finishes" would) */
        unsigned partial[] = { 37 };
        got = read_all(&zz, &ioA, out, 37, partial, 1);
        CHECK(got == 37 && !memcmp(out, FX_SINGLE_GAME, 37));
        tango_zip_close(&zz, &ioA);                          /* mid-stream close: must not leak the window/inflator */
        CHECK(mA.open_calls == 2 && mA.close_calls == 2);

        /* re-opening after a mid-stream close must start completely fresh (full, correct content again) */
        CHECK(tango_zip_open_archive(&zz, &ioA, "a.zip"));
        CHECK(tango_zip_scan(&zz, &ioA, "NES") == 1);
        CHECK(tango_zip_open_member(&zz, &ioA, 0, &us2) == 0 && us2 == FX_SINGLE_GAME_len);
        got = read_all(&zz, &ioA, out, sizeof out, SIZES, N_SIZES);
        CHECK(got == (int)FX_SINGLE_GAME_len && !memcmp(out, FX_SINGLE_GAME, FX_SINGLE_GAME_len));
        tango_zip_close(&zz, &ioA);
        CHECK(mA.open_calls == 3 && mA.close_calls == 3);

        /* 9c. a DIFFERENT zip after closing one: no entries or state from A leak into B's scan */
        fake_zipmem_init(&mB, &ioB, FX_MULTI, FX_MULTI_len);
        CHECK(tango_zip_open_archive(&zz, &ioB, "b.zip"));
        CHECK(tango_zip_scan(&zz, &ioB, "NES") == 2);                 /* B's count, not A's 1 */
        CHECK(tango_zip_find(&zz, "GAME.NES") == -1);                 /* A's name must not still be findable */
        CHECK(tango_zip_find(&zz, "Game (U).nes") >= 0);
        tango_zip_close(&zz, &ioB);
        CHECK(mB.open_calls == 1 && mB.close_calls == 1);
        CHECK(mA.open_calls == 3 && mA.close_calls == 3);             /* A's own fake untouched by B's session */

        /* 9d. tango_zip_close / close_member with NOTHING open: must be harmless, repeatable no-ops */
        tango_zip_close_member(&zz);
        tango_zip_close_member(&zz);
        tango_zip_close(&zz, &ioB);
        tango_zip_close(&zz, &ioB);
        CHECK(mB.open_calls == 1 && mB.close_calls == 1);    /* the extra closes did not call io->close again */

        /* a fresh open after all those idle no-ops still works correctly */
        CHECK(tango_zip_open_archive(&zz, &ioA, "a.zip"));
        CHECK(tango_zip_scan(&zz, &ioA, "NES") == 1);
        tango_zip_close(&zz, &ioA);
    }

    /* ---- 10. a cut-off archive (the EOCD/central directory are simply missing): a clean refusal ---- */
    {
        fake_zipmem_t m2; tango_zipfs_t io2;
        fake_zipmem_init(&m2, &io2, FX_SINGLE, FX_SINGLE_len - 5);
        tango_zip_t z2; memset(&z2, 0, sizeof z2);
        CHECK(!tango_zip_open_archive(&z2, &io2, "cut.zip"));
        tango_zip_close(&z2, &io2);
    }

    /* ---- 11. a torn SD read partway through streaming (the container itself was read fine -- open_archive and
     * scan already succeeded before the card starts failing): tango_zip_read must return a clean -1, never
     * hang, never report success with short or garbage data -- for both a stored and a deflate entry. The
     * failure is keyed off the read() call count, set just past the known-good open+scan+open_member phase, so
     * it always lands inside the member's own data reads regardless of the container's byte layout. ---- */
    {
        fake_zipmem_t m2; tango_zipfs_t io2;
        fake_zipmem_init(&m2, &io2, FX_STORED, FX_STORED_len);
        tango_zip_t z2; memset(&z2, 0, sizeof z2);
        CHECK(tango_zip_open_archive(&z2, &io2, "t.zip"));
        CHECK(tango_zip_scan(&z2, &io2, "NES") == 1);
        uint32_t us3 = 0;
        CHECK(tango_zip_open_member(&z2, &io2, 0, &us3) == 0);
        m2.fail_after_calls = m2.read_calls + 1;     /* the next read (the member's own data) starts failing */
        int r = read_all(&z2, &io2, out, sizeof out, SIZES, N_SIZES);
        CHECK(r == -1);
        tango_zip_close(&z2, &io2);

        fake_zipmem_init(&m2, &io2, FX_BIG, FX_BIG_len);
        memset(&z2, 0, sizeof z2);
        CHECK(tango_zip_open_archive(&z2, &io2, "t2.zip"));
        CHECK(tango_zip_scan(&z2, &io2, "NES") == 1);
        CHECK(tango_zip_open_member(&z2, &io2, 0, &us3) == 0);
        m2.fail_after_calls = m2.read_calls + 1;
        r = read_all(&z2, &io2, out, sizeof out, SIZES, N_SIZES);
        CHECK(r == -1);
        tango_zip_close(&z2, &io2);
    }

    /* ---- 12. tango_zip_locate: crc first, name as fallback, no caching of the rest ---- */
    {
        fake_zipmem_t m2; tango_zipfs_t io2;
        fake_zipmem_init(&m2, &io2, FX_MULTI, FX_MULTI_len);
        tango_zip_t z2; memset(&z2, 0, sizeof z2);
        CHECK(tango_zip_open_archive(&z2, &io2, "m.zip"));
        CHECK(tango_zip_scan(&z2, &io2, "NES") == 2);
        int iu = tango_zip_find(&z2, "Game (U).nes"), ie = tango_zip_find(&z2, "Game (E).nes");
        uint32_t crc_u = tango_zip_crc32(&z2, iu), crc_e = tango_zip_crc32(&z2, ie);
        CHECK(crc_u != crc_e);
        tango_zip_close(&z2, &io2);

        /* by crc alone (no name given) */
        fake_zipmem_init(&m2, &io2, FX_MULTI, FX_MULTI_len);
        memset(&z2, 0, sizeof z2);
        CHECK(tango_zip_open_archive(&z2, &io2, "m.zip"));
        CHECK(tango_zip_locate(&z2, &io2, NULL, crc_e));
        CHECK(z2.n == 1 && !strcmp(tango_zip_name(&z2, 0), "Game (E).nes"));
        uint32_t usize = 0;
        CHECK(tango_zip_open_member(&z2, &io2, 0, &usize) == 0 && usize == FX_MULTI_E_len);
        int got2 = read_all(&z2, &io2, out, sizeof out, SIZES, N_SIZES);
        CHECK(got2 == (int)FX_MULTI_E_len && !memcmp(out, FX_MULTI_E, FX_MULTI_E_len));
        tango_zip_close(&z2, &io2);

        /* by name alone (crc = 0, as MiSTer's own "if (crc32)" convention treats "no crc given") */
        fake_zipmem_init(&m2, &io2, FX_MULTI, FX_MULTI_len);
        memset(&z2, 0, sizeof z2);
        CHECK(tango_zip_open_archive(&z2, &io2, "m.zip"));
        CHECK(tango_zip_locate(&z2, &io2, "game (u).nes", 0));        /* case-insensitive */
        CHECK(z2.n == 1 && !strcmp(tango_zip_name(&z2, 0), "Game (U).nes"));
        tango_zip_close(&z2, &io2);

        /* crc wins over a name that would otherwise match a DIFFERENT entry -- proves priority, not
         * just "either one works when they agree" */
        fake_zipmem_init(&m2, &io2, FX_MULTI, FX_MULTI_len);
        memset(&z2, 0, sizeof z2);
        CHECK(tango_zip_open_archive(&z2, &io2, "m.zip"));
        CHECK(tango_zip_locate(&z2, &io2, "Game (U).nes", crc_e));    /* name says U, crc says E: crc wins */
        CHECK(!strcmp(tango_zip_name(&z2, 0), "Game (E).nes"));
        tango_zip_close(&z2, &io2);

        /* a crc that matches nothing falls back to the name */
        fake_zipmem_init(&m2, &io2, FX_MULTI, FX_MULTI_len);
        memset(&z2, 0, sizeof z2);
        CHECK(tango_zip_open_archive(&z2, &io2, "m.zip"));
        CHECK(tango_zip_locate(&z2, &io2, "Game (U).nes", 0xdeadbeefu));
        CHECK(!strcmp(tango_zip_name(&z2, 0), "Game (U).nes"));
        tango_zip_close(&z2, &io2);

        /* neither matches: a clean 0, not a crash or a stale leftover entry */
        fake_zipmem_init(&m2, &io2, FX_MULTI, FX_MULTI_len);
        memset(&z2, 0, sizeof z2);
        CHECK(tango_zip_open_archive(&z2, &io2, "m.zip"));
        CHECK(!tango_zip_locate(&z2, &io2, "nope.nes", 0xdeadbeefu));
        CHECK(z2.n == 0);
        tango_zip_close(&z2, &io2);
    }

    printf("%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
