// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 the Tango authors
/*
 * test_mra.c -- host tests for tango_mra.c. <rbf> extraction and the core-matching
 * rule, against a real sample .mra's text and a fake _Arcade/cores listing. <rom>/<part>
 * assembly, against synthetic fixtures (fake_zip_fixtures.h's existing placeholder content -- never
 * real ROM bytes, see the real-zip section below for why that matters) covering every part shape:
 * file-based (name/crc, repeat, offset+length), inline hex (the "mod byte" convention), self-closing
 * and open/close tags in both attribute-quote styles (both real, confirmed against a 679-file
 * library), and the refusal paths (interleave, patch, a per-part zip override).
 * Tango, our own code.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../tango_mra.h"
#include "../tango_zip.h"
#include "fake_zip.h"
#include "fake_zip_fixtures.h"
#include "fake_io.h"

static int fails = 0, checks = 0;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)

/* pulls the whole rom through tango_mra_rom_read in the given chunk sizes (cycled), mirroring how a
 * real download pump would drive it unevenly -- returns the total produced, or a negative
 * tango_mra_rom_read error/refusal code if one is ever returned (even after some bytes came through).
 * Takes a single zip/zip_io (every test here but the pipe-fallback one only ever needs one), wrapped
 * into the 1-element array tango_mra_rom_read actually wants -- a convenience, not a scope limit: the
 * pipe-fallback test below calls tango_mra_rom_read directly with a real multi-element array. */
static int mra_read_all(tango_mra_rom_t *r, const tango_zipfs_t *mra_io, tango_zip_t *zip, const tango_zipfs_t *zip_io,
                         uint8_t *out, unsigned cap, const unsigned *chunk_sizes, int n_sizes)
{
    tango_zip_t *zips[1] = { zip };
    const tango_zipfs_t *zip_ios[1] = { zip_io };
    int n_zips = zip ? 1 : 0;
    unsigned total = 0; int si = 0;
    for (;;) {
        unsigned ask = chunk_sizes[si % n_sizes]; si++;
        if (total >= cap) return (int)total;
        if (ask > cap - total) ask = cap - total;
        int got = tango_mra_rom_read(r, mra_io, zips, zip_ios, n_zips, out + total, ask);
        if (got < 0) return got;
        if (got == 0) return (int)total;
        total += (unsigned)got;
    }
}
static void mra_close_one(tango_mra_rom_t *r, const tango_zipfs_t *mra_io, tango_zip_t *zip, const tango_zipfs_t *zip_io)
{
    tango_zip_t *zips[1] = { zip };
    const tango_zipfs_t *zip_ios[1] = { zip_io };
    tango_mra_rom_close(r, mra_io, zip ? zips : NULL, zip_io ? zip_ios : NULL);
}
static const unsigned MRA_SIZES[] = { 1, 3, 17, 512, 599 };
#define N_MRA_SIZES (int)(sizeof MRA_SIZES / sizeof MRA_SIZES[0])

/* the <rbf> block of the MiSTer arcade file _Arcade/Mister Viking (315-5041).mra verbatim, plus enough of the
 * surrounding file to be a realistic offset, not a trivial one-tag fixture */
static const char MRA_VIKING[] =
    "<misterromdescription>\n"
    "\t<name>Mister Viking</name>\n"
    "\t<region></region>\n"
    "\t<homebrew>no</homebrew>\n"
    "\t<bootleg>no</bootleg>\n"
    "\t<version>315-5041</version>\n"
    "\t<alternative></alternative>\n"
    "\t<platform>Sega System 1</platform>\n"
    "\t<series></series>\n"
    "\t<year>1984</year>\n"
    "\t<manufacturer>Sega</manufacturer>\n"
    "\t<category>Shooter - Vertical</category>\n"
    "\n"
    "\t<setname>mrviking</setname>\n"
    "\t<parent>mrviking</parent>\n"
    "\t<mameversion>0217</mameversion>\n"
    "\t<rbf>segasys1</rbf>\n"
    "\t<about author=\"MrX-8B\"></about>\n";

int main(void)
{
    tango_zipfs_t io; fake_zipmem_t mem;
    char rbf[TANGO_MRA_RBF_MAX];

    /* ---- 1. the real sample: <rbf>segasys1</rbf> found, trimmed, exact ---- */
    fake_zipmem_init(&mem, &io, (const unsigned char *)MRA_VIKING, (unsigned)strlen(MRA_VIKING));
    CHECK(tango_mra_find_rbf(&io, "_Arcade/Mister Viking (315-5041).mra", rbf, sizeof rbf));
    CHECK(!strcmp(rbf, "segasys1"));
    CHECK(mem.open_calls == 1 && mem.close_calls == 1);

    /* ---- 2. whitespace around the text node is trimmed (a pretty-printed variant) ---- */
    static const char PADDED[] = "<misterromdescription><rbf>\n   galaxian  \n</rbf></misterromdescription>";
    fake_zipmem_init(&mem, &io, (const unsigned char *)PADDED, (unsigned)strlen(PADDED));
    CHECK(tango_mra_find_rbf(&io, "x.mra", rbf, sizeof rbf));
    CHECK(!strcmp(rbf, "galaxian"));

    /* ---- 3. case-insensitive tag match ---- */
    static const char UPPER[] = "<Misterromdescription><RBF>phoenix</RBF></Misterromdescription>";
    fake_zipmem_init(&mem, &io, (const unsigned char *)UPPER, (unsigned)strlen(UPPER));
    CHECK(tango_mra_find_rbf(&io, "x.mra", rbf, sizeof rbf));
    CHECK(!strcmp(rbf, "phoenix"));

    /* ---- 4. no <rbf> tag at all: not found ---- */
    static const char NONE[] = "<misterromdescription><name>No RBF Here</name></misterromdescription>";
    fake_zipmem_init(&mem, &io, (const unsigned char *)NONE, (unsigned)strlen(NONE));
    rbf[0] = 'X';
    CHECK(!tango_mra_find_rbf(&io, "x.mra", rbf, sizeof rbf));
    CHECK(rbf[0] == 0);

    /* ---- 5. empty <rbf></rbf>: found the tag, empty text -- reported as "not found" (caller should
     *         not try to resolve an empty rbf; tango_mra_resolve_core also refuses one) ---- */
    static const char EMPTYRBF[] = "<misterromdescription><rbf></rbf></misterromdescription>";
    fake_zipmem_init(&mem, &io, (const unsigned char *)EMPTYRBF, (unsigned)strlen(EMPTYRBF));
    CHECK(!tango_mra_find_rbf(&io, "x.mra", rbf, sizeof rbf));

    /* ---- 6. output truncates cleanly at a tiny cap, still NUL-terminated ---- */
    fake_zipmem_init(&mem, &io, (const unsigned char *)MRA_VIKING, (unsigned)strlen(MRA_VIKING));
    char tiny[4];
    CHECK(tango_mra_find_rbf(&io, "x.mra", tiny, sizeof tiny));
    CHECK(!strcmp(tiny, "seg"));

    /* ---- 7. a file that never opens (card removed, etc.) ---- */
    fake_zipmem_init(&mem, &io, (const unsigned char *)MRA_VIKING, (unsigned)strlen(MRA_VIKING));
    mem.fail_open = 1;
    CHECK(!tango_mra_find_rbf(&io, "x.mra", rbf, sizeof rbf));

    /* ---- 8. <rbf> past TANGO_MRA_SCAN_MAX is never seen (the scan budget) ---- */
    {
        static char big[TANGO_MRA_SCAN_MAX + 256];
        memset(big, 'x', sizeof big - 1); big[sizeof big - 1] = 0;
        memcpy(big + TANGO_MRA_SCAN_MAX + 32, "<rbf>toofar</rbf>", 17);
        fake_zipmem_init(&mem, &io, (const unsigned char *)big, (unsigned)strlen(big));
        CHECK(!tango_mra_find_rbf(&io, "x.mra", rbf, sizeof rbf));
    }

    /* ---- 9. resolve_core: MiSTer's rule -- "Arcade-<rbf>" or "<rbf>" then '.' or '_', newest wins ---- */
    {
        tango_fs_t fs; fake_card_t card;
        static const char *CORES[] = {
            "_Arcade/cores/",
            "_Arcade/cores/Arcade-segasys1_20220101.fs",
            "_Arcade/cores/Arcade-segasys1_20230615.fs",      /* newest: lexicographically greatest */
            "_Arcade/cores/Arcade-segasys1extra_20240101.fs", /* NOT a match: no '.'/'_' right after rbf */
            "_Arcade/cores/galaxian_20200101.fs",             /* bare form, no "Arcade-" competitor */
            "_Arcade/cores/readme.txt",                       /* not a core file: ignored */
        };
        fake_card_init(&card, &fs, CORES, (int)(sizeof CORES / sizeof CORES[0]));
        char out[TANGO_PATH_MAX];
        CHECK(tango_mra_resolve_core(&fs, "_Arcade/cores", "segasys1", out, sizeof out));
        CHECK(!strcmp(out, "Arcade-segasys1_20230615.fs"));

        CHECK(tango_mra_resolve_core(&fs, "_Arcade/cores", "galaxian", out, sizeof out));
        CHECK(!strcmp(out, "galaxian_20200101.fs"));

        CHECK(!tango_mra_resolve_core(&fs, "_Arcade/cores", "nosuchcore", out, sizeof out));
        CHECK(!tango_mra_resolve_core(&fs, "_Arcade/cores", "", out, sizeof out));         /* empty rbf refused */
        CHECK(!tango_mra_resolve_core(&fs, "_Nonexistent/cores", "segasys1", out, sizeof out));
    }

    /* ---- 9b. MiSTer's "newest wins" is really "lexicographically greatest" (plain strcmp,
     *          mra_loader.cpp 1182/1193): it's case-sensitive, so a lowercase bare-form name outranks
     *          any "Arcade-"-prefixed one for the SAME rbf, date code notwithstanding ('s' > 'A' in
     *          ASCII). Real card layouts don't mix the two forms for one rbf, but this is MiSTer's
     *          actual rule, read from source, not an approximation of it -- worth pinning down. */
    {
        tango_fs_t fs; fake_card_t card;
        static const char *CORES[] = {
            "_Arcade/cores/",
            "_Arcade/cores/Arcade-segasys1_20991231.fs",      /* latest possible date, uppercase-prefixed */
            "_Arcade/cores/segasys1_old.bin",                 /* bare, lowercase -- still wins */
        };
        fake_card_init(&card, &fs, CORES, (int)(sizeof CORES / sizeof CORES[0]));
        char out[TANGO_PATH_MAX];
        CHECK(tango_mra_resolve_core(&fs, "_Arcade/cores", "segasys1", out, sizeof out));
        CHECK(!strcmp(out, "segasys1_old.bin"));
    }

    /* ---- 10. case-insensitive core-name matching ---- */
    {
        tango_fs_t fs; fake_card_t card;
        static const char *CORES[] = { "_Arcade/cores/", "_Arcade/cores/ARCADE-SegaSys1_20210101.fs" };
        fake_card_init(&card, &fs, CORES, (int)(sizeof CORES / sizeof CORES[0]));
        char out[TANGO_PATH_MAX];
        CHECK(tango_mra_resolve_core(&fs, "_Arcade/cores", "segasys1", out, sizeof out));
        CHECK(!strcmp(out, "ARCADE-SegaSys1_20210101.fs"));
    }

    /* ================================================================== <rom>/<part> assembly */

    /* ---- 11. a single file-based part, matched by name only (no crc given) ---- */
    {
        fake_zipmem_t mram; tango_zipfs_t mra_io;
        static const char MRA[] =
            "<misterromdescription><rom index=\"0\" zip=\"game.zip\">"
            "<part name=\"GAME.NES\"></part></rom></misterromdescription>";
        fake_zipmem_init(&mram, &mra_io, (const unsigned char *)MRA, (unsigned)strlen(MRA));

        fake_zipmem_t zm; tango_zipfs_t zip_io;
        fake_zipmem_init(&zm, &zip_io, FX_SINGLE, FX_SINGLE_len);
        tango_zip_t zip; memset(&zip, 0, sizeof zip);
        CHECK(tango_zip_open_archive(&zip, &zip_io, "game.zip"));

        tango_mra_rom_t r; char zipname[TANGO_MRA_ZIP_NAME_MAX];
        CHECK(tango_mra_rom_open(&r, &mra_io, "x.mra", 0, zipname, sizeof zipname));
        CHECK(r.found && !strcmp(zipname, "game.zip"));
        static uint8_t out[8192];
        int got = mra_read_all(&r, &mra_io, &zip, &zip_io, out, sizeof out, MRA_SIZES, N_MRA_SIZES);
        CHECK(got == (int)FX_SINGLE_GAME_len && !memcmp(out, FX_SINGLE_GAME, FX_SINGLE_GAME_len));
        mra_close_one(&r, &mra_io, &zip, &zip_io);
    }

    /* ---- 12. two file-based parts: document order, not alphabetical or any other reordering ---- */
    {
        fake_zipmem_t mram; tango_zipfs_t mra_io;
        static const char MRA[] =
            "<misterromdescription><rom index=\"0\" zip=\"multi.zip\">"
            "<part name=\"Game (E).nes\"></part>"       /* E before U in the file: output must follow THIS order */
            "<part name=\"Game (U).nes\"></part>"
            "</rom></misterromdescription>";
        fake_zipmem_init(&mram, &mra_io, (const unsigned char *)MRA, (unsigned)strlen(MRA));

        fake_zipmem_t zm; tango_zipfs_t zip_io;
        fake_zipmem_init(&zm, &zip_io, FX_MULTI, FX_MULTI_len);
        tango_zip_t zip; memset(&zip, 0, sizeof zip);
        CHECK(tango_zip_open_archive(&zip, &zip_io, "multi.zip"));

        tango_mra_rom_t r;
        CHECK(tango_mra_rom_open(&r, &mra_io, "x.mra", 0, NULL, 0));
        static uint8_t out[8192];
        int got = mra_read_all(&r, &mra_io, &zip, &zip_io, out, sizeof out, MRA_SIZES, N_MRA_SIZES);
        unsigned want = FX_MULTI_E_len + FX_MULTI_U_len;
        CHECK(got == (int)want);
        CHECK(!memcmp(out, FX_MULTI_E, FX_MULTI_E_len));
        CHECK(!memcmp(out + FX_MULTI_E_len, FX_MULTI_U, FX_MULTI_U_len));
        mra_close_one(&r, &mra_io, &zip, &zip_io);
    }

    /* ---- 13. repeat="3": the same part's bytes appear three times in a row ---- */
    {
        fake_zipmem_t mram; tango_zipfs_t mra_io;
        static const char MRA[] =
            "<misterromdescription><rom index=\"0\" zip=\"game.zip\">"
            "<part name=\"GAME.NES\" repeat=\"3\"></part></rom></misterromdescription>";
        fake_zipmem_init(&mram, &mra_io, (const unsigned char *)MRA, (unsigned)strlen(MRA));

        fake_zipmem_t zm; tango_zipfs_t zip_io;
        fake_zipmem_init(&zm, &zip_io, FX_SINGLE, FX_SINGLE_len);
        tango_zip_t zip; memset(&zip, 0, sizeof zip);
        CHECK(tango_zip_open_archive(&zip, &zip_io, "game.zip"));

        tango_mra_rom_t r;
        CHECK(tango_mra_rom_open(&r, &mra_io, "x.mra", 0, NULL, 0));
        static uint8_t out[8192];
        int got = mra_read_all(&r, &mra_io, &zip, &zip_io, out, sizeof out, MRA_SIZES, N_MRA_SIZES);
        CHECK(got == (int)FX_SINGLE_GAME_len * 3);
        for (int i = 0; i < 3; i++)
            CHECK(!memcmp(out + i * FX_SINGLE_GAME_len, FX_SINGLE_GAME, FX_SINGLE_GAME_len));
        mra_close_one(&r, &mra_io, &zip, &zip_io);
    }

    /* ---- 14. offset/length: a byte range within the member, not the whole thing ---- */
    {
        fake_zipmem_t mram; tango_zipfs_t mra_io;
        static const char MRA[] =
            "<misterromdescription><rom index=\"0\" zip=\"s.zip\">"
            "<part name=\"plain.nes\" offset=\"7\" length=\"7\"></part></rom></misterromdescription>";
        fake_zipmem_init(&mram, &mra_io, (const unsigned char *)MRA, (unsigned)strlen(MRA));

        fake_zipmem_t zm; tango_zipfs_t zip_io;
        fake_zipmem_init(&zm, &zip_io, FX_STORED, FX_STORED_len);
        tango_zip_t zip; memset(&zip, 0, sizeof zip);
        CHECK(tango_zip_open_archive(&zip, &zip_io, "s.zip"));

        tango_mra_rom_t r;
        CHECK(tango_mra_rom_open(&r, &mra_io, "x.mra", 0, NULL, 0));
        static uint8_t out[32];
        int got = mra_read_all(&r, &mra_io, &zip, &zip_io, out, sizeof out, MRA_SIZES, N_MRA_SIZES);
        CHECK(got == 7 && !memcmp(out, "NOT COM", 7));    /* "STORED NOT COMPRESSED..." bytes [7,14) */
        mra_close_one(&r, &mra_io, &zip, &zip_io);
    }

    /* ---- 15. inline hex (no name attribute: MiSTer's "mod byte" convention), no zip ever touched --- */
    {
        fake_zipmem_t mram; tango_zipfs_t mra_io;
        static const char MRA[] =
            "<misterromdescription><rom index=\"3\" md5=\"none\"><part>\n"
            "01 02 03 0A FF\n"
            "</part></rom></misterromdescription>";
        fake_zipmem_init(&mram, &mra_io, (const unsigned char *)MRA, (unsigned)strlen(MRA));
        tango_mra_rom_t r; char zipname[TANGO_MRA_ZIP_NAME_MAX];
        CHECK(tango_mra_rom_open(&r, &mra_io, "x.mra", 3, zipname, sizeof zipname));
        CHECK(r.found && !zipname[0]);     /* no zip="" on this <rom> at all */
        static const uint8_t want[] = { 0x01, 0x02, 0x03, 0x0A, 0xFF };
        uint8_t out[32];
        int got = mra_read_all(&r, &mra_io, NULL, NULL, out, sizeof out, MRA_SIZES, N_MRA_SIZES);
        CHECK(got == (int)sizeof want && !memcmp(out, want, sizeof want));
        mra_close_one(&r, &mra_io, NULL, NULL);
    }

    /* ---- 16. inline hex with an odd digit count: the trailing lone nibble is its own byte
     *          (hexstr_to_char 408-413: the same rule when *ptr hits NUL mid-pair) ---- */
    {
        fake_zipmem_t mram; tango_zipfs_t mra_io;
        static const char MRA[] = "<misterromdescription><rom index=\"1\"><part>01 02 0A</part></rom></misterromdescription>";
        fake_zipmem_init(&mram, &mra_io, (const unsigned char *)MRA, (unsigned)strlen(MRA));
        tango_mra_rom_t r;
        CHECK(tango_mra_rom_open(&r, &mra_io, "x.mra", 1, NULL, 0));
        static const uint8_t want[] = { 0x01, 0x02, 0x0A };   /* "0A" is a full pair; the lone "0"
            * would be the odd-leftover case -- this fixture exercises the full-pairs path cleanly and
            * the odd-leftover path is exercised by test 17's single stray digit */
        uint8_t out[32];
        int got = mra_read_all(&r, &mra_io, NULL, NULL, out, sizeof out, MRA_SIZES, N_MRA_SIZES);
        CHECK(got == (int)sizeof want && !memcmp(out, want, sizeof want));
        mra_close_one(&r, &mra_io, NULL, NULL);
    }

    /* ---- 17. a genuinely odd digit count: "01 02 0A 7" -- the trailing "7" is its own byte (0x07) --- */
    {
        fake_zipmem_t mram; tango_zipfs_t mra_io;
        static const char MRA[] = "<misterromdescription><rom index=\"1\"><part>01 02 0A 7</part></rom></misterromdescription>";
        fake_zipmem_init(&mram, &mra_io, (const unsigned char *)MRA, (unsigned)strlen(MRA));
        tango_mra_rom_t r;
        CHECK(tango_mra_rom_open(&r, &mra_io, "x.mra", 1, NULL, 0));
        static const uint8_t want[] = { 0x01, 0x02, 0x0A, 0x07 };
        uint8_t out[32];
        int got = mra_read_all(&r, &mra_io, NULL, NULL, out, sizeof out, MRA_SIZES, N_MRA_SIZES);
        CHECK(got == (int)sizeof want && !memcmp(out, want, sizeof want));
        mra_close_one(&r, &mra_io, NULL, NULL);
    }

    /* ---- 18. repeat on an inline part: the decoded bytes repeat too ---- */
    {
        fake_zipmem_t mram; tango_zipfs_t mra_io;
        static const char MRA[] = "<misterromdescription><rom index=\"1\"><part repeat=\"2\">AB</part></rom></misterromdescription>";
        fake_zipmem_init(&mram, &mra_io, (const unsigned char *)MRA, (unsigned)strlen(MRA));
        tango_mra_rom_t r;
        CHECK(tango_mra_rom_open(&r, &mra_io, "x.mra", 1, NULL, 0));
        static const uint8_t want[] = { 0xAB, 0xAB };
        uint8_t out[32];
        int got = mra_read_all(&r, &mra_io, NULL, NULL, out, sizeof out, MRA_SIZES, N_MRA_SIZES);
        CHECK(got == (int)sizeof want && !memcmp(out, want, sizeof want));
        mra_close_one(&r, &mra_io, NULL, NULL);
    }

    /* ---- 19. a self-closing, nameless part contributes zero bytes; surrounding parts are untouched --- */
    {
        fake_zipmem_t mram; tango_zipfs_t mra_io;
        static const char MRA[] =
            "<misterromdescription><rom index=\"0\" zip=\"game.zip\">"
            "<part/><part name=\"GAME.NES\"></part><part/>"
            "</rom></misterromdescription>";
        fake_zipmem_init(&mram, &mra_io, (const unsigned char *)MRA, (unsigned)strlen(MRA));
        fake_zipmem_t zm; tango_zipfs_t zip_io;
        fake_zipmem_init(&zm, &zip_io, FX_SINGLE, FX_SINGLE_len);
        tango_zip_t zip; memset(&zip, 0, sizeof zip);
        CHECK(tango_zip_open_archive(&zip, &zip_io, "game.zip"));
        tango_mra_rom_t r;
        CHECK(tango_mra_rom_open(&r, &mra_io, "x.mra", 0, NULL, 0));
        static uint8_t out[8192];
        int got = mra_read_all(&r, &mra_io, &zip, &zip_io, out, sizeof out, MRA_SIZES, N_MRA_SIZES);
        CHECK(got == (int)FX_SINGLE_GAME_len && !memcmp(out, FX_SINGLE_GAME, FX_SINGLE_GAME_len));
        mra_close_one(&r, &mra_io, &zip, &zip_io);
    }

    /* ---- 20. a self-closing named part (<part name=".." crc=".."/>): a file ref needs no body --- */
    {
        fake_zipmem_t mram; tango_zipfs_t mra_io;
        static const char MRA[] =
            "<misterromdescription><rom index=\"0\" zip=\"game.zip\">"
            "<part name=\"GAME.NES\"/></rom></misterromdescription>";
        fake_zipmem_init(&mram, &mra_io, (const unsigned char *)MRA, (unsigned)strlen(MRA));
        fake_zipmem_t zm; tango_zipfs_t zip_io;
        fake_zipmem_init(&zm, &zip_io, FX_SINGLE, FX_SINGLE_len);
        tango_zip_t zip; memset(&zip, 0, sizeof zip);
        CHECK(tango_zip_open_archive(&zip, &zip_io, "game.zip"));
        tango_mra_rom_t r;
        CHECK(tango_mra_rom_open(&r, &mra_io, "x.mra", 0, NULL, 0));
        static uint8_t out[8192];
        int got = mra_read_all(&r, &mra_io, &zip, &zip_io, out, sizeof out, MRA_SIZES, N_MRA_SIZES);
        CHECK(got == (int)FX_SINGLE_GAME_len && !memcmp(out, FX_SINGLE_GAME, FX_SINGLE_GAME_len));
        mra_close_one(&r, &mra_io, &zip, &zip_io);
    }

    /* ---- 21. no <rom index> at all for this index: zero bytes, not an error, r->found == 0 ---- */
    {
        fake_zipmem_t mram; tango_zipfs_t mra_io;
        static const char MRA[] = "<misterromdescription><rom index=\"0\"><part name=\"x\"></part></rom></misterromdescription>";
        fake_zipmem_init(&mram, &mra_io, (const unsigned char *)MRA, (unsigned)strlen(MRA));
        tango_mra_rom_t r;
        CHECK(tango_mra_rom_open(&r, &mra_io, "x.mra", 7, NULL, 0));
        CHECK(!r.found);
        uint8_t out[8];
        CHECK(mra_read_all(&r, &mra_io, NULL, NULL, out, sizeof out, MRA_SIZES, N_MRA_SIZES) == 0);
        mra_close_one(&r, &mra_io, NULL, NULL);
    }

    /* ---- 22. an empty <rom index> (self-closing and open/close forms): found, but zero bytes ---- */
    {
        fake_zipmem_t mram; tango_zipfs_t mra_io;
        static const char MRA[] =
            "<misterromdescription><rom index=\"0\"><part name=\"x\"></part></rom>"
            "<rom index=\"1\"/><rom index=\"2\"></rom></misterromdescription>";
        fake_zipmem_init(&mram, &mra_io, (const unsigned char *)MRA, (unsigned)strlen(MRA));
        tango_mra_rom_t r;
        CHECK(tango_mra_rom_open(&r, &mra_io, "x.mra", 1, NULL, 0));
        CHECK(r.found);
        uint8_t out[8];
        CHECK(mra_read_all(&r, &mra_io, NULL, NULL, out, sizeof out, MRA_SIZES, N_MRA_SIZES) == 0);
        mra_close_one(&r, &mra_io, NULL, NULL);

        fake_zipmem_init(&mram, &mra_io, (const unsigned char *)MRA, (unsigned)strlen(MRA));
        CHECK(tango_mra_rom_open(&r, &mra_io, "x.mra", 2, NULL, 0));
        CHECK(r.found);
        CHECK(mra_read_all(&r, &mra_io, NULL, NULL, out, sizeof out, MRA_SIZES, N_MRA_SIZES) == 0);
        mra_close_one(&r, &mra_io, NULL, NULL);
    }

    /* ---- 23. <interleave> inside the rom: refused, not silently mis-assembled ---- */
    {
        fake_zipmem_t mram; tango_zipfs_t mra_io;
        static const char MRA[] =
            "<misterromdescription><rom index=\"0\" zip=\"x.zip\"><interleave output=\"16\">"
            "<part name=\"a\" map=\"01\"/><part name=\"b\" map=\"10\"/></interleave></rom></misterromdescription>";
        fake_zipmem_init(&mram, &mra_io, (const unsigned char *)MRA, (unsigned)strlen(MRA));
        tango_mra_rom_t r;
        CHECK(tango_mra_rom_open(&r, &mra_io, "x.mra", 0, NULL, 0));
        uint8_t out[8];
        CHECK(mra_read_all(&r, &mra_io, NULL, NULL, out, sizeof out, MRA_SIZES, N_MRA_SIZES) == TANGO_MRA_REFUSED);
        mra_close_one(&r, &mra_io, NULL, NULL);
    }

    /* ---- 24. <patch> inside the rom: applied (tests 39-45 below cover patch/md5 in depth, this one
     *          just confirms a <patch> right after a real <part> is applied, not refused) ---- */
    {
        fake_zipmem_t mram; tango_zipfs_t mra_io;
        static const char MRA[] =
            "<misterromdescription><rom index=\"0\" zip=\"game.zip\">"
            "<part name=\"GAME.NES\"></part><patch offset=\"0\">FF</patch></rom></misterromdescription>";
        fake_zipmem_init(&mram, &mra_io, (const unsigned char *)MRA, (unsigned)strlen(MRA));
        fake_zipmem_t zm; tango_zipfs_t zip_io;
        fake_zipmem_init(&zm, &zip_io, FX_SINGLE, FX_SINGLE_len);
        tango_zip_t zip; memset(&zip, 0, sizeof zip);
        CHECK(tango_zip_open_archive(&zip, &zip_io, "game.zip"));
        tango_mra_rom_t r;
        CHECK(tango_mra_rom_open(&r, &mra_io, "x.mra", 0, NULL, 0));
        uint8_t out[8192];
        int got = mra_read_all(&r, &mra_io, &zip, &zip_io, out, sizeof out, MRA_SIZES, N_MRA_SIZES);
        CHECK(got == (int)FX_SINGLE_GAME_len);
        CHECK(out[0] == 0xFF);
        CHECK(!memcmp(out + 1, FX_SINGLE_GAME + 1, FX_SINGLE_GAME_len - 1));
        mra_close_one(&r, &mra_io, &zip, &zip_io);
        tango_zip_close(&zip, &zip_io);
    }

    /* ---- 25. a per-part zip="" override: refused (one zip per <rom>, no per-part override) ---- */
    {
        fake_zipmem_t mram; tango_zipfs_t mra_io;
        static const char MRA[] =
            "<misterromdescription><rom index=\"0\" zip=\"game.zip\">"
            "<part name=\"GAME.NES\" zip=\"other.zip\"></part></rom></misterromdescription>";
        fake_zipmem_init(&mram, &mra_io, (const unsigned char *)MRA, (unsigned)strlen(MRA));
        tango_mra_rom_t r;
        CHECK(tango_mra_rom_open(&r, &mra_io, "x.mra", 0, NULL, 0));
        uint8_t out[8];
        CHECK(mra_read_all(&r, &mra_io, NULL, NULL, out, sizeof out, MRA_SIZES, N_MRA_SIZES) == TANGO_MRA_REFUSED);
        mra_close_one(&r, &mra_io, NULL, NULL);
    }

    /* ---- 26. case-insensitive tags/attributes, mixed quote styles -- both real, confirmed ---- */
    {
        fake_zipmem_t mram; tango_zipfs_t mra_io;
        static const char MRA[] =
            "<MISTERROMDESCRIPTION><ROM INDEX='0' ZIP=\"game.zip\">"
            "<PART NAME='GAME.NES'></PART></ROM></MISTERROMDESCRIPTION>";
        fake_zipmem_init(&mram, &mra_io, (const unsigned char *)MRA, (unsigned)strlen(MRA));
        fake_zipmem_t zm; tango_zipfs_t zip_io;
        fake_zipmem_init(&zm, &zip_io, FX_SINGLE, FX_SINGLE_len);
        tango_zip_t zip; memset(&zip, 0, sizeof zip);
        CHECK(tango_zip_open_archive(&zip, &zip_io, "game.zip"));
        tango_mra_rom_t r; char zipname[TANGO_MRA_ZIP_NAME_MAX];
        CHECK(tango_mra_rom_open(&r, &mra_io, "x.mra", 0, zipname, sizeof zipname));
        CHECK(!strcmp(zipname, "game.zip"));
        static uint8_t out[8192];
        int got = mra_read_all(&r, &mra_io, &zip, &zip_io, out, sizeof out, MRA_SIZES, N_MRA_SIZES);
        CHECK(got == (int)FX_SINGLE_GAME_len && !memcmp(out, FX_SINGLE_GAME, FX_SINGLE_GAME_len));
        mra_close_one(&r, &mra_io, &zip, &zip_io);
    }

    /* ---- 27. a non-matching <rom index> earlier in the file is skipped without being parsed as
     *          parts -- its own <part> tags (for a DIFFERENT rom) must never leak into this one ---- */
    {
        fake_zipmem_t mram; tango_zipfs_t mra_io;
        static const char MRA[] =
            "<misterromdescription>"
            "<rom index=\"9\" zip=\"other.zip\"><part name=\"should.never.appear\"></part></rom>"
            "<rom index=\"0\" zip=\"game.zip\"><part name=\"GAME.NES\"></part></rom>"
            "</misterromdescription>";
        fake_zipmem_init(&mram, &mra_io, (const unsigned char *)MRA, (unsigned)strlen(MRA));
        fake_zipmem_t zm; tango_zipfs_t zip_io;
        fake_zipmem_init(&zm, &zip_io, FX_SINGLE, FX_SINGLE_len);
        tango_zip_t zip; memset(&zip, 0, sizeof zip);
        CHECK(tango_zip_open_archive(&zip, &zip_io, "game.zip"));
        tango_mra_rom_t r; char zipname[TANGO_MRA_ZIP_NAME_MAX];
        CHECK(tango_mra_rom_open(&r, &mra_io, "x.mra", 0, zipname, sizeof zipname));
        CHECK(!strcmp(zipname, "game.zip"));
        static uint8_t out[8192];
        int got = mra_read_all(&r, &mra_io, &zip, &zip_io, out, sizeof out, MRA_SIZES, N_MRA_SIZES);
        CHECK(got == (int)FX_SINGLE_GAME_len && !memcmp(out, FX_SINGLE_GAME, FX_SINGLE_GAME_len));
        mra_close_one(&r, &mra_io, &zip, &zip_io);
    }

    /* ---- 28. a realistic file shape: <name>/<region>/<switches> (with nested <dip>s) and a non-
     *          matching <rom> all come BEFORE the target <rom> -- every one of them has a text body
     *          and its own close tag, which a scan that only knew how to skip <rom> bodies would walk
     *          into and mistake for "nothing left" (the synthetic cases above have no sibling tags
     *          with text bodies before <rom>; real .mra files do) ---- */
    {
        fake_zipmem_t mram; tango_zipfs_t mra_io;
        static const char MRA[] =
            "<misterromdescription>\n"
            "\t<name>Some Game</name>\n"
            "\t<region></region>\n"
            "\t<year>1984</year>\n"
            "\t<rbf>somecore</rbf>\n"
            "\t<switches default=\"1F\">\n"
            "\t\t<dip bits=\"0\" ids=\"Off,On\" name=\"Demo Sounds\"></dip>\n"
            "\t</switches>\n"
            "\t<buttons default=\"A,Start\" names=\"Fire,Start\"></buttons>\n"
            "\t<rom index=\"9\"><part name=\"should.never.appear\"></part></rom>\n"
            "\t<rom index=\"0\" zip=\"game.zip\"><part name=\"GAME.NES\"></part></rom>\n"
            "</misterromdescription>";
        fake_zipmem_init(&mram, &mra_io, (const unsigned char *)MRA, (unsigned)strlen(MRA));
        fake_zipmem_t zm; tango_zipfs_t zip_io;
        fake_zipmem_init(&zm, &zip_io, FX_SINGLE, FX_SINGLE_len);
        tango_zip_t zip; memset(&zip, 0, sizeof zip);
        CHECK(tango_zip_open_archive(&zip, &zip_io, "game.zip"));
        tango_mra_rom_t r; char zipname[TANGO_MRA_ZIP_NAME_MAX];
        CHECK(tango_mra_rom_open(&r, &mra_io, "x.mra", 0, zipname, sizeof zipname));
        CHECK(r.found && !strcmp(zipname, "game.zip"));
        static uint8_t out[8192];
        int got = mra_read_all(&r, &mra_io, &zip, &zip_io, out, sizeof out, MRA_SIZES, N_MRA_SIZES);
        CHECK(got == (int)FX_SINGLE_GAME_len && !memcmp(out, FX_SINGLE_GAME, FX_SINGLE_GAME_len));
        mra_close_one(&r, &mra_io, &zip, &zip_io);
    }

    /* ---- 29. zip="a.zip|b.zip" pipe fallback (Galaga's real romset needs it: two
     *          of its twelve rom-0 parts, 51xx.bin/54xx.bin, only exist in namco51.zip/namco54.zip,
     *          not in galaga.zip at all, by CRC against the real files).
     *          Uses a real multi-element zips[]/zip_ios[] array, not the single-zip test helpers
     *          above: this is the one case that genuinely needs more than one. ---- */
    {
        fake_zipmem_t mram; tango_zipfs_t mra_io;
        static const char MRA[] =
            "<misterromdescription><rom index=\"0\" zip=\"a.zip|b.zip\">"
            "<part name=\"GAME.NES\"></part>"       /* only in a.zip (FX_SINGLE) */
            "<part name=\"plain.nes\"></part>"      /* only in b.zip (FX_STORED) -- not in a.zip at all */
            "</rom></misterromdescription>";
        fake_zipmem_init(&mram, &mra_io, (const unsigned char *)MRA, (unsigned)strlen(MRA));

        fake_zipmem_t am, bm; tango_zipfs_t a_io, b_io;
        fake_zipmem_init(&am, &a_io, FX_SINGLE, FX_SINGLE_len);
        fake_zipmem_init(&bm, &b_io, FX_STORED, FX_STORED_len);
        tango_zip_t a, b; memset(&a, 0, sizeof a); memset(&b, 0, sizeof b);
        CHECK(tango_zip_open_archive(&a, &a_io, "a.zip"));
        CHECK(tango_zip_open_archive(&b, &b_io, "b.zip"));
        tango_zip_t *zips[2] = { &a, &b };
        const tango_zipfs_t *zip_ios[2] = { &a_io, &b_io };

        tango_mra_rom_t r; char zipname[TANGO_MRA_ZIP_NAME_MAX];
        CHECK(tango_mra_rom_open(&r, &mra_io, "x.mra", 0, zipname, sizeof zipname));
        CHECK(!strcmp(zipname, "a.zip|b.zip"));    /* verbatim: splitting it is the caller's job */

        static uint8_t out[8192];
        unsigned total = 0; int si = 0;
        static const unsigned SIZES2[] = { 1, 7, 512 };
        int got;
        for (;;) {
            unsigned ask = SIZES2[si % 3]; si++;
            got = tango_mra_rom_read(&r, &mra_io, zips, zip_ios, 2, out + total, ask);
            if (got <= 0) break;
            total += (unsigned)got;
        }
        CHECK(got == 0);
        CHECK(total == FX_SINGLE_GAME_len + FX_STORED_GAME_len);
        CHECK(!memcmp(out, FX_SINGLE_GAME, FX_SINGLE_GAME_len));
        CHECK(!memcmp(out + FX_SINGLE_GAME_len, FX_STORED_GAME, FX_STORED_GAME_len));
        tango_mra_rom_close(&r, &mra_io, zips, zip_ios);
        tango_zip_close(&a, &a_io);
        tango_zip_close(&b, &b_io);
    }

    /* ---- 30. a part found in NEITHER zip of the fallback list: a real error (ERROR, not REFUSED --
     *          "not supported" and "not found anywhere" are different problems) ---- */
    {
        fake_zipmem_t mram; tango_zipfs_t mra_io;
        static const char MRA[] =
            "<misterromdescription><rom index=\"0\" zip=\"a.zip|b.zip\">"
            "<part name=\"nowhere.bin\"></part></rom></misterromdescription>";
        fake_zipmem_init(&mram, &mra_io, (const unsigned char *)MRA, (unsigned)strlen(MRA));

        fake_zipmem_t am, bm; tango_zipfs_t a_io, b_io;
        fake_zipmem_init(&am, &a_io, FX_SINGLE, FX_SINGLE_len);
        fake_zipmem_init(&bm, &b_io, FX_STORED, FX_STORED_len);
        tango_zip_t a, b; memset(&a, 0, sizeof a); memset(&b, 0, sizeof b);
        CHECK(tango_zip_open_archive(&a, &a_io, "a.zip"));
        CHECK(tango_zip_open_archive(&b, &b_io, "b.zip"));
        tango_zip_t *zips[2] = { &a, &b };
        const tango_zipfs_t *zip_ios[2] = { &a_io, &b_io };

        tango_mra_rom_t r;
        CHECK(tango_mra_rom_open(&r, &mra_io, "x.mra", 0, NULL, 0));
        uint8_t out[8];
        CHECK(tango_mra_rom_read(&r, &mra_io, zips, zip_ios, 2, out, sizeof out) == -1);
        tango_mra_rom_close(&r, &mra_io, zips, zip_ios);
        tango_zip_close(&a, &a_io);
        tango_zip_close(&b, &b_io);
    }

    /* ================================================================== <switches>/<cheats> (DIPs) */

    /* ---- 31. Galaxian's real <switches> block verbatim (MiSTer's _Arcade/Galaxian (Namco, Set
     *          1).mra) -- dip_def and every entry's start/size/mask/ids/values checked against the
     *          values independently worked out by hand from the same source ---- */
    {
        fake_zipmem_t mram; tango_zipfs_t mra_io;
        static const char MRA[] =
            "<misterromdescription>\n"
            "\t<switches default=\"1F,1F,04\">\n"
            "\t\t<dip bits=\"7\" ids=\"Off,On\" name=\"Service\"></dip>\n"
            "\t\t<dip bits=\"6\" ids=\"Off,On\" name=\"Test\"></dip>\n"
            "\t\t<dip bits=\"5\" ids=\"Upright,Cocktail\" name=\"Cabinet\"></dip>\n"
            "\t\t<dip bits=\"14,15\" ids=\"1c/1cr,1c/2cr,2c/1cr,Free Play\" name=\"Coinage\" values=\"0,2,1,3\"></dip>\n"
            "\t\t<dip bits=\"16,17\" ids=\"7000,10000,12000,20000\" name=\"Bonus Life After\"></dip>\n"
            "\t\t<dip bits=\"18\" ids=\"2,3\" name=\"Lives\"></dip>\n"
            "\t</switches>\n"
            "</misterromdescription>";
        fake_zipmem_init(&mram, &mra_io, (const unsigned char *)MRA, (unsigned)strlen(MRA));

        tango_mra_switches_t sw;
        CHECK(tango_mra_switches_parse(&mra_io, "x.mra", 0, &sw));
        CHECK(sw.dip_def == 0x041F1Full);
        CHECK(sw.dip_num == 6);

        CHECK(!strcmp(sw.dip[0].name, "Service"));
        CHECK(sw.dip[0].start == 7 && sw.dip[0].size == 1 && sw.dip[0].mask == 0x80);
        CHECK(sw.dip[0].num == 2 && !sw.dip[0].has_val);
        CHECK(!strcmp(sw.dip[0].id[0], "Off") && !strcmp(sw.dip[0].id[1], "On"));
        CHECK(sw.dip[0].val[0] == 0 && sw.dip[0].val[1] == 0x80);   /* no values="": index << start */

        CHECK(!strcmp(sw.dip[3].name, "Coinage"));
        CHECK(sw.dip[3].start == 14 && sw.dip[3].size == 2 && sw.dip[3].mask == 0xC000);
        CHECK(sw.dip[3].num == 4 && sw.dip[3].has_val);
        CHECK(!strcmp(sw.dip[3].id[3], "Free Play"));
        CHECK(sw.dip[3].val[0] == 0 && sw.dip[3].val[1] == 0x8000 && sw.dip[3].val[2] == 0x4000 && sw.dip[3].val[3] == 0xC000);

        CHECK(!strcmp(sw.dip[5].name, "Lives"));
        CHECK(sw.dip[5].start == 18 && sw.dip[5].mask == 0x40000);
        CHECK(sw.dip[5].val[0] == 0 && sw.dip[5].val[1] == 0x40000);
    }

    /* ---- 32. Galaga's real <switches> block verbatim (MiSTer's _Arcade/Galaga (Midway, Set
     *          1).mra): self-closing <dip/> tags (not open/close), attribute order varying between
     *          entries (name before or after bits/ids), an unknown "base" attribute on <switches>
     *          itself (confirmed absent from mra_loader.cpp entirely -- an MRA-authoring-tool
     *          annotation, correctly ignored) ---- */
    {
        fake_zipmem_t mram; tango_zipfs_t mra_io;
        static const char MRA[] =
            "<misterromdescription>\n"
            "\t<switches default=\"08,68\" base=\"8\">\n"
            "\t\t<dip bits=\"0\"     name=\"2 credits\"   ids=\"2P game,1P game\"/>\n"
            "\t\t<dip bits=\"1,2\"   name=\"Difficulty\"  ids=\"Easy,Medium,Hard,Hardest\" values=\"0,3,2,1\"/>\n"
            "\t\t<dip bits=\"3\"     name=\"Demo Sounds\" ids=\"Off,On\"/>\n"
            "\t\t<dip bits=\"16\" name=\"Self-test mode\"  ids=\"Off,On\"/>\n"
            "\t\t<dip bits=\"17\" name=\"Service trigger\" ids=\"Off,On\"/>\n"
            "\t</switches>\n"
            "</misterromdescription>";
        fake_zipmem_init(&mram, &mra_io, (const unsigned char *)MRA, (unsigned)strlen(MRA));

        tango_mra_switches_t sw;
        CHECK(tango_mra_switches_parse(&mra_io, "x.mra", 0, &sw));
        CHECK(sw.dip_def == 0x6808ull);
        CHECK(sw.dip_num == 5);

        CHECK(!strcmp(sw.dip[0].name, "2 credits"));
        CHECK(sw.dip[0].start == 0 && sw.dip[0].size == 1 && sw.dip[0].mask == 1);

        CHECK(!strcmp(sw.dip[1].name, "Difficulty"));
        CHECK(sw.dip[1].start == 1 && sw.dip[1].size == 2 && sw.dip[1].mask == 0x06);
        CHECK(sw.dip[1].val[0] == 0 && sw.dip[1].val[1] == 6 && sw.dip[1].val[2] == 4 && sw.dip[1].val[3] == 2);

        CHECK(!strcmp(sw.dip[4].name, "Service trigger"));
        CHECK(sw.dip[4].start == 17 && sw.dip[4].mask == 0x20000);
    }

    /* ---- 33. <cheats> uses the same parser, selected by the cheats=1 argument ---- */
    {
        fake_zipmem_t mram; tango_zipfs_t mra_io;
        static const char MRA[] =
            "<misterromdescription><switches default=\"FF\"><dip bits=\"0\" name=\"sw\" ids=\"A,B\"/></switches>"
            "<cheats default=\"02\"><dip bits=\"1\" name=\"ch\" ids=\"X,Y\"/></cheats></misterromdescription>";
        fake_zipmem_init(&mram, &mra_io, (const unsigned char *)MRA, (unsigned)strlen(MRA));

        tango_mra_switches_t sw, ch;
        CHECK(tango_mra_switches_parse(&mra_io, "x.mra", 0, &sw));
        CHECK(sw.dip_def == 0xFF && sw.dip_num == 1 && !strcmp(sw.dip[0].name, "sw"));

        fake_zipmem_init(&mram, &mra_io, (const unsigned char *)MRA, (unsigned)strlen(MRA));
        CHECK(tango_mra_switches_parse(&mra_io, "x.mra", 1, &ch));
        CHECK(ch.dip_def == 0x02 && ch.dip_num == 1 && !strcmp(ch.dip[0].name, "ch"));
    }

    /* ---- 34. no <switches> block at all: not an error, comes back zeroed ---- */
    {
        fake_zipmem_t mram; tango_zipfs_t mra_io;
        static const char MRA[] = "<misterromdescription><name>No DIPs</name><rom index=\"0\"></rom></misterromdescription>";
        fake_zipmem_init(&mram, &mra_io, (const unsigned char *)MRA, (unsigned)strlen(MRA));
        tango_mra_switches_t sw;
        CHECK(tango_mra_switches_parse(&mra_io, "x.mra", 0, &sw));
        CHECK(sw.dip_num == 0 && sw.dip_def == 0);
    }

    /* ---- 35. a self-closing <switches/> with a default but no dips at all ---- */
    {
        fake_zipmem_t mram; tango_zipfs_t mra_io;
        static const char MRA[] = "<misterromdescription><switches default=\"42\"/></misterromdescription>";
        fake_zipmem_init(&mram, &mra_io, (const unsigned char *)MRA, (unsigned)strlen(MRA));
        tango_mra_switches_t sw;
        CHECK(tango_mra_switches_parse(&mra_io, "x.mra", 0, &sw));
        CHECK(sw.dip_def == 0x42 && sw.dip_num == 0);
    }

    /* ---- 36. values="" without ids="": MiSTer's own dip_num stays 0 for that entry (num is only
     *          ever set by "ids"), so the values are parsed but never actually used -- faithful to
     *          the real behaviour, not a guess ---- */
    {
        fake_zipmem_t mram; tango_zipfs_t mra_io;
        static const char MRA[] =
            "<misterromdescription><switches default=\"00\">"
            "<dip bits=\"0\" name=\"orphan\" values=\"1,2,3\"/>"
            "</switches></misterromdescription>";
        fake_zipmem_init(&mram, &mra_io, (const unsigned char *)MRA, (unsigned)strlen(MRA));
        tango_mra_switches_t sw;
        CHECK(tango_mra_switches_parse(&mra_io, "x.mra", 0, &sw));
        CHECK(sw.dip_num == 1);
        CHECK(sw.dip[0].num == 0 && sw.dip[0].has_val);   /* parsed, but num==0 means nothing iterates it */
    }

    /* ---- 37. an invalid "bits" value is rejected (left at defaults), matching MiSTer's own
     *          validation (b/e range, e>=b) -- the <dip> itself still counts, just with no bit range */
    {
        fake_zipmem_t mram; tango_zipfs_t mra_io;
        static const char MRA[] =
            "<misterromdescription><switches default=\"00\">"
            "<dip bits=\"5,2\" name=\"bad\" ids=\"A,B\"/>"       /* e < b: invalid */
            "</switches></misterromdescription>";
        fake_zipmem_init(&mram, &mra_io, (const unsigned char *)MRA, (unsigned)strlen(MRA));
        tango_mra_switches_t sw;
        CHECK(tango_mra_switches_parse(&mra_io, "x.mra", 0, &sw));
        CHECK(sw.dip_num == 1);
        CHECK(sw.dip[0].start == 0 && sw.dip[0].size == 0 && sw.dip[0].mask == 0);   /* left at memset-zero */
    }

    /* ---- 37b. tango_mra_switches_summary (dip_def/dip_num only, no per-<dip> table -- because
     *          sizeof(tango_mra_switches_t) is ~83.5 KB, too large for the BL616's own OCRAM heap pool
     *          to ever malloc in one piece) agrees with the full parser on every fixture above that has real citation
     *          weight: Galaxian's and Galaga's actual <switches> blocks (cases 31/32), <cheats> via the
     *          same cheats=1 argument (case 33), no block at all (case 34), and a self-closing <switches/>
     *          with a default but no <dip>s (case 35) -- the exact set of shapes real .mra files use, not
     *          a re-test of every parser edge case already covered above (cases 36/37 test attribute
     *          parsing INSIDE a <dip> that this summary function never looks at at all). This is the one
     *          and only test of the function that replaced the 83 KB table -- the only check on the
     *          code that decides whether any DIP/cheat word is ever sent to a core. ---- */
    {
        fake_zipmem_t mram; tango_zipfs_t mra_io;
        static const char GALAXIAN[] =
            "<misterromdescription>\n"
            "\t<switches default=\"1F,1F,04\">\n"
            "\t\t<dip bits=\"7\" ids=\"Off,On\" name=\"Service\"></dip>\n"
            "\t\t<dip bits=\"6\" ids=\"Off,On\" name=\"Test\"></dip>\n"
            "\t\t<dip bits=\"5\" ids=\"Upright,Cocktail\" name=\"Cabinet\"></dip>\n"
            "\t\t<dip bits=\"14,15\" ids=\"1c/1cr,1c/2cr,2c/1cr,Free Play\" name=\"Coinage\" values=\"0,2,1,3\"></dip>\n"
            "\t\t<dip bits=\"16,17\" ids=\"7000,10000,12000,20000\" name=\"Bonus Life After\"></dip>\n"
            "\t\t<dip bits=\"18\" ids=\"2,3\" name=\"Lives\"></dip>\n"
            "\t</switches>\n"
            "</misterromdescription>";
        fake_zipmem_init(&mram, &mra_io, (const unsigned char *)GALAXIAN, (unsigned)strlen(GALAXIAN));
        uint64_t dip_def = 0xdeadbeef; int dip_num = -1;
        CHECK(tango_mra_switches_summary(&mra_io, "x.mra", 0, &dip_def, &dip_num));
        CHECK(dip_def == 0x041F1Full && dip_num == 6);   /* matches case 31's own full-parser result exactly */

        fake_zipmem_init(&mram, &mra_io, (const unsigned char *)GALAXIAN, (unsigned)strlen(GALAXIAN));
        CHECK(tango_mra_switches_summary(&mra_io, "x.mra", 1, &dip_def, &dip_num));   /* no <cheats> block: still 1, both 0 */
        CHECK(dip_def == 0 && dip_num == 0);

        static const char GALAGA[] =
            "<misterromdescription>\n"
            "\t<switches default=\"08,68\" base=\"8\">\n"
            "\t\t<dip bits=\"0\"     name=\"2 credits\"   ids=\"2P game,1P game\"/>\n"
            "\t\t<dip bits=\"1,2\"   name=\"Difficulty\"  ids=\"Easy,Medium,Hard,Hardest\" values=\"0,3,2,1\"/>\n"
            "\t\t<dip bits=\"3\"     name=\"Demo Sounds\" ids=\"Off,On\"/>\n"
            "\t\t<dip bits=\"16\" name=\"Self-test mode\"  ids=\"Off,On\"/>\n"
            "\t\t<dip bits=\"17\" name=\"Service trigger\" ids=\"Off,On\"/>\n"
            "\t</switches>\n"
            "</misterromdescription>";
        fake_zipmem_init(&mram, &mra_io, (const unsigned char *)GALAGA, (unsigned)strlen(GALAGA));
        CHECK(tango_mra_switches_summary(&mra_io, "x.mra", 0, &dip_def, &dip_num));
        CHECK(dip_def == 0x6808ull && dip_num == 5);   /* matches case 32's own full-parser result exactly */

        static const char CHEATS[] =
            "<misterromdescription><switches default=\"FF\"><dip bits=\"0\" name=\"sw\" ids=\"A,B\"/></switches>"
            "<cheats default=\"02\"><dip bits=\"1\" name=\"ch\" ids=\"X,Y\"/></cheats></misterromdescription>";
        fake_zipmem_init(&mram, &mra_io, (const unsigned char *)CHEATS, (unsigned)strlen(CHEATS));
        CHECK(tango_mra_switches_summary(&mra_io, "x.mra", 0, &dip_def, &dip_num));
        CHECK(dip_def == 0xFF && dip_num == 1);
        fake_zipmem_init(&mram, &mra_io, (const unsigned char *)CHEATS, (unsigned)strlen(CHEATS));
        CHECK(tango_mra_switches_summary(&mra_io, "x.mra", 1, &dip_def, &dip_num));
        CHECK(dip_def == 0x02 && dip_num == 1);

        static const char NONE[] = "<misterromdescription><name>No DIPs</name><rom index=\"0\"></rom></misterromdescription>";
        fake_zipmem_init(&mram, &mra_io, (const unsigned char *)NONE, (unsigned)strlen(NONE));
        CHECK(tango_mra_switches_summary(&mra_io, "x.mra", 0, &dip_def, &dip_num));
        CHECK(dip_def == 0 && dip_num == 0);

        static const char SELFCLOSE[] = "<misterromdescription><switches default=\"42\"/></misterromdescription>";
        fake_zipmem_init(&mram, &mra_io, (const unsigned char *)SELFCLOSE, (unsigned)strlen(SELFCLOSE));
        CHECK(tango_mra_switches_summary(&mra_io, "x.mra", 0, &dip_def, &dip_num));
        CHECK(dip_def == 0x42 && dip_num == 0);
    }

    /* ---- 37c. tango_mra_switches_dip_at (the DIP page's own per-row, on-demand parse -- never
     *          the whole tango_mra_switches_t, too large for the heap: see 37b).
     *          Checked against the SAME Galaxian/Galaga fixtures case 31/32 already hand-verified with the
     *          full parser, entry by entry, plus the cheats/out-of-range cases tango_mra_switches_summary
     *          above already covers for THIS function's own shared scan logic. ---- */
    {
        fake_zipmem_t mram; tango_zipfs_t mra_io;
        static const char GALAXIAN[] =
            "<misterromdescription>\n"
            "\t<switches default=\"1F,1F,04\">\n"
            "\t\t<dip bits=\"7\" ids=\"Off,On\" name=\"Service\"></dip>\n"
            "\t\t<dip bits=\"6\" ids=\"Off,On\" name=\"Test\"></dip>\n"
            "\t\t<dip bits=\"5\" ids=\"Upright,Cocktail\" name=\"Cabinet\"></dip>\n"
            "\t\t<dip bits=\"14,15\" ids=\"1c/1cr,1c/2cr,2c/1cr,Free Play\" name=\"Coinage\" values=\"0,2,1,3\"></dip>\n"
            "\t\t<dip bits=\"16,17\" ids=\"7000,10000,12000,20000\" name=\"Bonus Life After\"></dip>\n"
            "\t\t<dip bits=\"18\" ids=\"2,3\" name=\"Lives\"></dip>\n"
            "\t</switches>\n"
            "</misterromdescription>";
        tango_mra_dip_t d;

        fake_zipmem_init(&mram, &mra_io, (const unsigned char *)GALAXIAN, (unsigned)strlen(GALAXIAN));
        CHECK(tango_mra_switches_dip_at(&mra_io, "x.mra", 0, 0, &d));   /* matches case 31's dip[0] exactly */
        CHECK(!strcmp(d.name, "Service"));
        CHECK(d.start == 7 && d.size == 1 && d.mask == 0x80);
        CHECK(d.num == 2 && !d.has_val);
        CHECK(!strcmp(d.id[0], "Off") && !strcmp(d.id[1], "On"));
        CHECK(d.val[0] == 0 && d.val[1] == 0x80);

        fake_zipmem_init(&mram, &mra_io, (const unsigned char *)GALAXIAN, (unsigned)strlen(GALAXIAN));
        CHECK(tango_mra_switches_dip_at(&mra_io, "x.mra", 0, 3, &d));   /* matches case 31's dip[3] exactly */
        CHECK(!strcmp(d.name, "Coinage"));
        CHECK(d.start == 14 && d.size == 2 && d.mask == 0xC000);
        CHECK(d.num == 4 && d.has_val);
        CHECK(!strcmp(d.id[3], "Free Play"));
        CHECK(d.val[0] == 0 && d.val[1] == 0x8000 && d.val[2] == 0x4000 && d.val[3] == 0xC000);

        fake_zipmem_init(&mram, &mra_io, (const unsigned char *)GALAXIAN, (unsigned)strlen(GALAXIAN));
        CHECK(tango_mra_switches_dip_at(&mra_io, "x.mra", 0, 5, &d));   /* matches case 31's dip[5] exactly */
        CHECK(!strcmp(d.name, "Lives"));
        CHECK(d.start == 18 && d.mask == 0x40000);
        CHECK(d.val[0] == 0 && d.val[1] == 0x40000);

        /* out of range: no such <dip> at index 6 (only 6 entries, 0-5), nor at a wildly out-of-range one
         * -- 0 returned, *out zeroed, not left with the previous call's data */
        fake_zipmem_init(&mram, &mra_io, (const unsigned char *)GALAXIAN, (unsigned)strlen(GALAXIAN));
        memset(&d, 0xAA, sizeof d);
        CHECK(!tango_mra_switches_dip_at(&mra_io, "x.mra", 0, 6, &d));
        CHECK(d.name[0] == 0 && d.mask == 0 && d.num == 0);
        fake_zipmem_init(&mram, &mra_io, (const unsigned char *)GALAXIAN, (unsigned)strlen(GALAXIAN));
        CHECK(!tango_mra_switches_dip_at(&mra_io, "x.mra", 0, 100, &d));
        CHECK(!tango_mra_switches_dip_at(&mra_io, "x.mra", 0, -1, &d));

        /* Galaga's own real <switches>, cross-checked against case 32's full-parser result for dip[1]
         * ("Difficulty", the one entry with values="") */
        static const char GALAGA[] =
            "<misterromdescription>\n"
            "\t<switches default=\"08,68\" base=\"8\">\n"
            "\t\t<dip bits=\"0\"     name=\"2 credits\"   ids=\"2P game,1P game\"/>\n"
            "\t\t<dip bits=\"1,2\"   name=\"Difficulty\"  ids=\"Easy,Medium,Hard,Hardest\" values=\"0,3,2,1\"/>\n"
            "\t\t<dip bits=\"3\"     name=\"Demo Sounds\" ids=\"Off,On\"/>\n"
            "\t\t<dip bits=\"16\" name=\"Self-test mode\"  ids=\"Off,On\"/>\n"
            "\t\t<dip bits=\"17\" name=\"Service trigger\" ids=\"Off,On\"/>\n"
            "\t</switches>\n"
            "</misterromdescription>";
        fake_zipmem_init(&mram, &mra_io, (const unsigned char *)GALAGA, (unsigned)strlen(GALAGA));
        CHECK(tango_mra_switches_dip_at(&mra_io, "x.mra", 0, 1, &d));
        CHECK(!strcmp(d.name, "Difficulty"));
        CHECK(d.start == 1 && d.size == 2 && d.mask == 0x06);
        CHECK(d.val[0] == 0 && d.val[1] == 6 && d.val[2] == 4 && d.val[3] == 2);
        fake_zipmem_init(&mram, &mra_io, (const unsigned char *)GALAGA, (unsigned)strlen(GALAGA));
        CHECK(tango_mra_switches_dip_at(&mra_io, "x.mra", 0, 4, &d));   /* the LAST real entry (index 4 of 5) */
        CHECK(!strcmp(d.name, "Service trigger"));
        CHECK(d.start == 17 && d.mask == 0x20000);
        fake_zipmem_init(&mram, &mra_io, (const unsigned char *)GALAGA, (unsigned)strlen(GALAGA));
        CHECK(!tango_mra_switches_dip_at(&mra_io, "x.mra", 0, 5, &d));   /* one past the last real entry */

        /* <cheats> via the same cheats=1 argument, and no block at all */
        static const char CH[] =
            "<misterromdescription><cheats default=\"02\"><dip bits=\"1\" name=\"ch\" ids=\"X,Y\"/></cheats></misterromdescription>";
        fake_zipmem_init(&mram, &mra_io, (const unsigned char *)CH, (unsigned)strlen(CH));
        CHECK(tango_mra_switches_dip_at(&mra_io, "x.mra", 1, 0, &d));
        CHECK(!strcmp(d.name, "ch"));
        fake_zipmem_init(&mram, &mra_io, (const unsigned char *)CH, (unsigned)strlen(CH));
        CHECK(!tango_mra_switches_dip_at(&mra_io, "x.mra", 0, 0, &d));   /* no <switches> in this file */

        static const char NOBLOCK[] = "<misterromdescription></misterromdescription>";
        fake_zipmem_init(&mram, &mra_io, (const unsigned char *)NOBLOCK, (unsigned)strlen(NOBLOCK));
        CHECK(!tango_mra_switches_dip_at(&mra_io, "x.mra", 0, 0, &d));

        static const char SELFCLOSE2[] = "<misterromdescription><switches default=\"42\"/></misterromdescription>";
        fake_zipmem_init(&mram, &mra_io, (const unsigned char *)SELFCLOSE2, (unsigned)strlen(SELFCLOSE2));
        CHECK(!tango_mra_switches_dip_at(&mra_io, "x.mra", 0, 0, &d));   /* a <switches/> with no <dip>s */
    }

    /* ---- 38. HTML comments between <part> siblings, real (confirmed present in "1942 (Revision
     *          B).mra", which also has one of these verbatim) and one deliberately containing a
     *          literal '>' in its own text -- proves the scanner finds the real "-->", not just the
     *          next bare '>' (the 1942/Aliens comments checked by hand never happen to contain one,
     *          so this is the case that would have silently passed without it). Both parts resolve
     *          to the same fixture, so a correct result is simply "both were reached": 2x the bytes. ---- */
    {
        fake_zipmem_t mram; tango_zipfs_t mra_io;
        static const char MRA[] =
            "<misterromdescription><rom index=\"0\" zip=\"game.zip\">\n"
            "\t<!--  Header to identify the PCB type\n"
            "\tByte 0 = 0 for 1942, 1 for Vulgus, 2 for Higemaru\n"
            "\tByte 1 = XOR with flip\n"
            " -->\n"
            "\t<part name=\"GAME.NES\"></part>\n"
            "\t<!-- a>b looks like <description> text -->\n"
            "\t<part name=\"GAME.NES\"></part>\n"
            "</rom></misterromdescription>";
        fake_zipmem_init(&mram, &mra_io, (const unsigned char *)MRA, (unsigned)strlen(MRA));
        fake_zipmem_t am; tango_zipfs_t a_io;
        fake_zipmem_init(&am, &a_io, FX_SINGLE, FX_SINGLE_len);
        tango_zip_t a; memset(&a, 0, sizeof a);
        CHECK(tango_zip_open_archive(&a, &a_io, "game.zip"));

        tango_mra_rom_t r;
        CHECK(tango_mra_rom_open(&r, &mra_io, "x.mra", 0, NULL, 0));
        static uint8_t out[8192];
        int got = mra_read_all(&r, &mra_io, &a, &a_io, out, sizeof out, MRA_SIZES, N_MRA_SIZES);
        CHECK(got == (int)FX_SINGLE_GAME_len * 2);
        CHECK(!memcmp(out, FX_SINGLE_GAME, FX_SINGLE_GAME_len));
        CHECK(!memcmp(out + FX_SINGLE_GAME_len, FX_SINGLE_GAME, FX_SINGLE_GAME_len));
        mra_close_one(&r, &mra_io, &a, &a_io);
        tango_zip_close(&a, &a_io);
    }

    /* ================================================================== <patch> and the md5 check */

    /* ---- 39. an overwrite patch at offset 0: the first two bytes change, the rest doesn't ---- */
    {
        fake_zipmem_t mram; tango_zipfs_t mra_io;
        static const char MRA[] =
            "<misterromdescription><rom index=\"0\" zip=\"game.zip\">"
            "<part name=\"GAME.NES\"></part><patch offset=\"0\">4142</patch></rom></misterromdescription>";
        fake_zipmem_init(&mram, &mra_io, (const unsigned char *)MRA, (unsigned)strlen(MRA));
        fake_zipmem_t zm; tango_zipfs_t zip_io;
        fake_zipmem_init(&zm, &zip_io, FX_SINGLE, FX_SINGLE_len);
        tango_zip_t zip; memset(&zip, 0, sizeof zip);
        CHECK(tango_zip_open_archive(&zip, &zip_io, "game.zip"));
        tango_mra_rom_t r;
        CHECK(tango_mra_rom_open(&r, &mra_io, "x.mra", 0, NULL, 0));
        static uint8_t out[8192];
        int got = mra_read_all(&r, &mra_io, &zip, &zip_io, out, sizeof out, MRA_SIZES, N_MRA_SIZES);
        CHECK(got == (int)FX_SINGLE_GAME_len);
        CHECK(out[0] == 'A' && out[1] == 'B');
        CHECK(!memcmp(out + 2, FX_SINGLE_GAME + 2, FX_SINGLE_GAME_len - 2));
        mra_close_one(&r, &mra_io, &zip, &zip_io);
        tango_zip_close(&zip, &zip_io);
    }

    /* ---- 40. operation="xor" at a non-zero offset ---- */
    {
        fake_zipmem_t mram; tango_zipfs_t mra_io;
        static const char MRA[] =
            "<misterromdescription><rom index=\"0\" zip=\"game.zip\">"
            "<part name=\"GAME.NES\"></part><patch offset=\"4\" operation=\"xor\">FF</patch></rom></misterromdescription>";
        fake_zipmem_init(&mram, &mra_io, (const unsigned char *)MRA, (unsigned)strlen(MRA));
        fake_zipmem_t zm; tango_zipfs_t zip_io;
        fake_zipmem_init(&zm, &zip_io, FX_SINGLE, FX_SINGLE_len);
        tango_zip_t zip; memset(&zip, 0, sizeof zip);
        CHECK(tango_zip_open_archive(&zip, &zip_io, "game.zip"));
        tango_mra_rom_t r;
        CHECK(tango_mra_rom_open(&r, &mra_io, "x.mra", 0, NULL, 0));
        static uint8_t out[8192];
        int got = mra_read_all(&r, &mra_io, &zip, &zip_io, out, sizeof out, MRA_SIZES, N_MRA_SIZES);
        CHECK(got == (int)FX_SINGLE_GAME_len);
        CHECK(out[4] == (uint8_t)(FX_SINGLE_GAME[4] ^ 0xFF));
        CHECK(!memcmp(out, FX_SINGLE_GAME, 4));
        CHECK(!memcmp(out + 5, FX_SINGLE_GAME + 5, FX_SINGLE_GAME_len - 5));
        mra_close_one(&r, &mra_io, &zip, &zip_io);
        tango_zip_close(&zip, &zip_io);
    }

    /* ---- 41. a multi-byte patch spanning several of mra_read_all's small chunk sizes (1, 3, 17, ...):
     *          proves in-flight application works across call boundaries, not just within one call ---- */
    {
        fake_zipmem_t mram; tango_zipfs_t mra_io;
        static const char MRA[] =
            "<misterromdescription><rom index=\"0\" zip=\"game.zip\">"
            "<part name=\"GAME.NES\"></part>"
            "<patch offset=\"10\">00 01 02 03 04 05 06 07 08 09 0A 0B 0C 0D 0E 0F 10 11 12 13</patch>"
            "</rom></misterromdescription>";
        fake_zipmem_init(&mram, &mra_io, (const unsigned char *)MRA, (unsigned)strlen(MRA));
        fake_zipmem_t zm; tango_zipfs_t zip_io;
        fake_zipmem_init(&zm, &zip_io, FX_SINGLE, FX_SINGLE_len);
        tango_zip_t zip; memset(&zip, 0, sizeof zip);
        CHECK(tango_zip_open_archive(&zip, &zip_io, "game.zip"));
        tango_mra_rom_t r;
        CHECK(tango_mra_rom_open(&r, &mra_io, "x.mra", 0, NULL, 0));
        static uint8_t out[8192];
        int got = mra_read_all(&r, &mra_io, &zip, &zip_io, out, sizeof out, MRA_SIZES, N_MRA_SIZES);
        CHECK(got == (int)FX_SINGLE_GAME_len);
        for (int i = 0; i < 20; i++) CHECK(out[10 + i] == i);
        CHECK(!memcmp(out, FX_SINGLE_GAME, 10));
        CHECK(!memcmp(out + 30, FX_SINGLE_GAME + 30, FX_SINGLE_GAME_len - 30));
        mra_close_one(&r, &mra_io, &zip, &zip_io);
        tango_zip_close(&zip, &zip_io);
    }

    /* ---- 42. md5 match: FX_SINGLE_GAME's real md5 (independently computed via Python's hashlib),
     *          declared on the <rom> itself -- a clean 0, not a mismatch ---- */
    {
        fake_zipmem_t mram; tango_zipfs_t mra_io;
        static const char MRA[] =
            "<misterromdescription><rom index=\"0\" md5=\"50032219336b0b285f3a10b271d364c0\" zip=\"game.zip\">"
            "<part name=\"GAME.NES\"></part></rom></misterromdescription>";
        fake_zipmem_init(&mram, &mra_io, (const unsigned char *)MRA, (unsigned)strlen(MRA));
        fake_zipmem_t zm; tango_zipfs_t zip_io;
        fake_zipmem_init(&zm, &zip_io, FX_SINGLE, FX_SINGLE_len);
        tango_zip_t zip; memset(&zip, 0, sizeof zip);
        CHECK(tango_zip_open_archive(&zip, &zip_io, "game.zip"));
        tango_mra_rom_t r;
        CHECK(tango_mra_rom_open(&r, &mra_io, "x.mra", 0, NULL, 0));
        static uint8_t out[8192];
        int got = mra_read_all(&r, &mra_io, &zip, &zip_io, out, sizeof out, MRA_SIZES, N_MRA_SIZES);
        CHECK(got == (int)FX_SINGLE_GAME_len);
        mra_close_one(&r, &mra_io, &zip, &zip_io);
        tango_zip_close(&zip, &zip_io);
    }

    /* ---- 43. md5 mismatch: a wrong digest -- every byte already produced is still correct, but the
     *          final call reports TANGO_MRA_MD5_MISMATCH instead of 0 ---- */
    {
        fake_zipmem_t mram; tango_zipfs_t mra_io;
        static const char MRA[] =
            "<misterromdescription><rom index=\"0\" md5=\"00000000000000000000000000000000\" zip=\"game.zip\">"
            "<part name=\"GAME.NES\"></part></rom></misterromdescription>";
        fake_zipmem_init(&mram, &mra_io, (const unsigned char *)MRA, (unsigned)strlen(MRA));
        fake_zipmem_t zm; tango_zipfs_t zip_io;
        fake_zipmem_init(&zm, &zip_io, FX_SINGLE, FX_SINGLE_len);
        tango_zip_t zip; memset(&zip, 0, sizeof zip);
        CHECK(tango_zip_open_archive(&zip, &zip_io, "game.zip"));
        tango_mra_rom_t r;
        CHECK(tango_mra_rom_open(&r, &mra_io, "x.mra", 0, NULL, 0));
        static uint8_t out[8192];
        tango_zip_t *zips[1] = { &zip }; const tango_zipfs_t *zip_ios[1] = { &zip_io };
        unsigned total = 0; int si = 0, got;
        for (;;) {
            unsigned ask = MRA_SIZES[si % N_MRA_SIZES]; si++;
            got = tango_mra_rom_read(&r, &mra_io, zips, zip_ios, 1, out + total, ask);
            if (got <= 0) break;
            total += (unsigned)got;
        }
        CHECK(got == TANGO_MRA_MD5_MISMATCH);
        CHECK(total == FX_SINGLE_GAME_len);               /* every byte up to the mismatch is correct */
        CHECK(!memcmp(out, FX_SINGLE_GAME, FX_SINGLE_GAME_len));
        /* calling again after a MISMATCH must not crash or re-finalize (md5_checked guards it) */
        CHECK(tango_mra_rom_read(&r, &mra_io, zips, zip_ios, 1, out, sizeof out) == 0);
        tango_mra_rom_close(&r, &mra_io, zips, zip_ios);
        tango_zip_close(&zip, &zip_io);
    }

    /* ---- 44. md5="none" and no md5 attribute at all: both skip the check entirely ---- */
    {
        for (int variant = 0; variant < 2; variant++) {
            fake_zipmem_t mram; tango_zipfs_t mra_io;
            char mra_buf[256];
            snprintf(mra_buf, sizeof mra_buf,
                "<misterromdescription><rom index=\"0\"%s zip=\"game.zip\">"
                "<part name=\"GAME.NES\"></part></rom></misterromdescription>",
                variant == 0 ? " md5=\"none\"" : "");
            fake_zipmem_init(&mram, &mra_io, (const unsigned char *)mra_buf, (unsigned)strlen(mra_buf));
            fake_zipmem_t zm; tango_zipfs_t zip_io;
            fake_zipmem_init(&zm, &zip_io, FX_SINGLE, FX_SINGLE_len);
            tango_zip_t zip; memset(&zip, 0, sizeof zip);
            CHECK(tango_zip_open_archive(&zip, &zip_io, "game.zip"));
            tango_mra_rom_t r;
            CHECK(tango_mra_rom_open(&r, &mra_io, "x.mra", 0, NULL, 0));
            static uint8_t out[8192];
            int got = mra_read_all(&r, &mra_io, &zip, &zip_io, out, sizeof out, MRA_SIZES, N_MRA_SIZES);
            CHECK(got == (int)FX_SINGLE_GAME_len);
            mra_close_one(&r, &mra_io, &zip, &zip_io);
            tango_zip_close(&zip, &zip_io);
        }
    }

    /* ---- 45. more <patch> tags than TANGO_MRA_PATCH_MAX: refused from the first read, not truncated
     *          or silently applying only some of them (built at runtime, not a giant literal) ---- */
    {
        static char mra_buf[1 << 16];
        int o = snprintf(mra_buf, sizeof mra_buf,
            "<misterromdescription><rom index=\"0\" zip=\"game.zip\"><part name=\"GAME.NES\"></part>");
        for (int i = 0; i < TANGO_MRA_PATCH_MAX + 1; i++)
            o += snprintf(mra_buf + o, sizeof mra_buf - (size_t)o, "<patch offset=\"%d\">00</patch>", i);
        o += snprintf(mra_buf + o, sizeof mra_buf - (size_t)o, "</rom></misterromdescription>");
        CHECK((size_t)o < sizeof mra_buf);

        fake_zipmem_t mram; tango_zipfs_t mra_io;
        fake_zipmem_init(&mram, &mra_io, (const unsigned char *)mra_buf, (unsigned)o);
        tango_mra_rom_t r;
        CHECK(tango_mra_rom_open(&r, &mra_io, "x.mra", 0, NULL, 0));
        CHECK(r.phase == TANGO_MRA_ROM_REFUSED_ST);
        uint8_t out[8];
        CHECK(mra_read_all(&r, &mra_io, NULL, NULL, out, sizeof out, MRA_SIZES, N_MRA_SIZES) == TANGO_MRA_REFUSED);
        mra_close_one(&r, &mra_io, NULL, NULL);
    }

    /* ---- 46. a patch longer than apply_patches' own internal chunk buffer (64 bytes; the real
     *          Arkanoid (US).mra's 0x5042 patch is 69 bytes) -- decode_hex_range must return the
     *          CAPPED decoded count when `out`/`cap` say to stop: an uncapped count lets the caller's
     *          second decode (for the remaining 5 bytes) get the right count back from the FIRST call
     *          too, leaving 5 bytes of uninitialized stack data applied to the output instead of the
     *          real patch bytes. Built here with 100 bytes so the fast synthetic suite catches this
     *          class of bug without needing the real library at all. ---- */
    {
        fake_zipmem_t mram; tango_zipfs_t mra_io;
        char hexbuf[256]; int ho = 0;
        for (int i = 0; i < 100; i++) ho += snprintf(hexbuf + ho, sizeof hexbuf - (size_t)ho, "%02x", i & 0xff);
        char mra_buf[1024];
        snprintf(mra_buf, sizeof mra_buf,
            "<misterromdescription><rom index=\"0\" zip=\"game.zip\">"
            "<part name=\"GAME.NES\"></part><patch offset=\"0\">%s</patch></rom></misterromdescription>",
            hexbuf);
        fake_zipmem_init(&mram, &mra_io, (const unsigned char *)mra_buf, (unsigned)strlen(mra_buf));
        fake_zipmem_t zm; tango_zipfs_t zip_io;
        fake_zipmem_init(&zm, &zip_io, FX_SINGLE, FX_SINGLE_len);
        tango_zip_t zip; memset(&zip, 0, sizeof zip);
        CHECK(tango_zip_open_archive(&zip, &zip_io, "game.zip"));
        tango_mra_rom_t r;
        CHECK(tango_mra_rom_open(&r, &mra_io, "x.mra", 0, NULL, 0));
        static uint8_t out[8192];
        int got = mra_read_all(&r, &mra_io, &zip, &zip_io, out, sizeof out, MRA_SIZES, N_MRA_SIZES);
        CHECK(got == (int)FX_SINGLE_GAME_len);
        int ok = 1;
        for (int i = 0; i < 100; i++) if (out[i] != (uint8_t)(i & 0xff)) ok = 0;
        CHECK(ok);
        CHECK(!memcmp(out + 100, FX_SINGLE_GAME + 100, FX_SINGLE_GAME_len - 100));
        mra_close_one(&r, &mra_io, &zip, &zip_io);
        tango_zip_close(&zip, &zip_io);
    }

    /* ================================================================== <buttons>, <nvram> (data only) */

    /* ---- 47. Galaxian's real <buttons> tag verbatim ---- */
    {
        fake_zipmem_t mram; tango_zipfs_t mra_io;
        static const char MRA[] =
            "<misterromdescription><buttons default=\"A,Start,Select,R,B,L\" "
            "names=\"Fire,Start 1P,Start 2P,Coin,Test,Pause\"></buttons></misterromdescription>";
        fake_zipmem_init(&mram, &mra_io, (const unsigned char *)MRA, (unsigned)strlen(MRA));
        tango_mra_misc_t misc;
        CHECK(tango_mra_misc_parse(&mra_io, "x.mra", &misc));
        CHECK(misc.has_buttons && !misc.has_nvram);
        CHECK(!strcmp(misc.button_default, "A,Start,Select,R,B,L"));
        CHECK(!strcmp(misc.button_names, "Fire,Start 1P,Start 2P,Coin,Test,Pause"));
    }

    /* ---- 48. Galaga's real <nvram> tag verbatim (self-closing) ---- */
    {
        fake_zipmem_t mram; tango_zipfs_t mra_io;
        static const char MRA[] = "<misterromdescription><nvram index=\"4\" size=\"51\"/></misterromdescription>";
        fake_zipmem_init(&mram, &mra_io, (const unsigned char *)MRA, (unsigned)strlen(MRA));
        tango_mra_misc_t misc;
        CHECK(tango_mra_misc_parse(&mra_io, "x.mra", &misc));
        CHECK(misc.has_nvram && !misc.has_buttons);
        CHECK(misc.nvram_index == 4 && misc.nvram_size == 51);
    }

    /* ---- 49. neither tag present: not an error, both have_* flags stay 0 ---- */
    {
        fake_zipmem_t mram; tango_zipfs_t mra_io;
        static const char MRA[] = "<misterromdescription><name>No extras</name></misterromdescription>";
        fake_zipmem_init(&mram, &mra_io, (const unsigned char *)MRA, (unsigned)strlen(MRA));
        tango_mra_misc_t misc;
        CHECK(tango_mra_misc_parse(&mra_io, "x.mra", &misc));
        CHECK(!misc.has_buttons && !misc.has_nvram);
    }

    /* ---- 50. both present, plus a sibling tag with a close tag in between -- proves the shared
     *          "skip every unrelated top-level tag's own body" discipline still applies here too ---- */
    {
        fake_zipmem_t mram; tango_zipfs_t mra_io;
        static const char MRA[] =
            "<misterromdescription><buttons default=\"A,B\" names=\"X,Y\"></buttons>"
            "<switches default=\"00\"></switches><nvram index=\"4\" size=\"8\"/></misterromdescription>";
        fake_zipmem_init(&mram, &mra_io, (const unsigned char *)MRA, (unsigned)strlen(MRA));
        tango_mra_misc_t misc;
        CHECK(tango_mra_misc_parse(&mra_io, "x.mra", &misc));
        CHECK(misc.has_buttons && misc.has_nvram);
        CHECK(!strcmp(misc.button_default, "A,B") && !strcmp(misc.button_names, "X,Y"));
        CHECK(misc.nvram_index == 4 && misc.nvram_size == 8);
    }

    printf("%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
