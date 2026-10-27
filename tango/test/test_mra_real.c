// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 the Tango authors
/*
 * test_mra_real.c -- reproduce known-good MiSTer ROM images
 * byte-for-byte from REAL .mra files and REAL MAME zips, not synthetic fixtures.
 *
 * Opt-in, not part of `make test` (same convention as vproc_stream/test_vproc's $FILTERS/$SYS):
 * the asset is a MAME ROM library (this file NEVER embeds ROM bytes, and the library itself
 * is not part of this repo). Each case
 * skips cleanly (not a failure) when its own files aren't present, rather than failing CI on every
 * other machine.
 *
 *   tango/test/test_mra_real [MISTER_PACK_DIR]   (default: $MISTER_PACK or $HOME/MiSTer-PACK)
 *
 * Verification is against MD5 digests, not embedded ROM bytes -- independently computed (not just
 * trusted from each .mra's own <rom md5="..."> claim, though both happen to agree) by extracting
 * each real zip's matching members BY CRC (not by the .mra's `name=` attribute, which often names a
 * different historical filename than what's actually archived -- confirmed for Galaga: its own
 * "2800l.bin" part is really `gg1_11.4d` inside galaga.zip, found only by CRC) and concatenating in
 * the .mra's own <part> document order. A hash is not the copyrighted content itself
 * (unlike the ROM bytes it fingerprints), so it's fine to commit here.
 *
 * Tango, our own code.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../tango_mra.h"
#include "../tango_zip.h"

static int fails = 0, checks = 0;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)

/* a tango_zipfs_t over a real file on disk (fopen/fseek/fread) -- test-only; the firmware's own
 * equivalent is FatFs-backed (tango_fw.c's zfs_open/zfs_read/zfs_close). open() failing (a listed
 * fallback zip that doesn't exist on disk, e.g. Galaga's own galagamw.zip -- it isn't even
 * present in a real MAME set) is reported as "not open", not treated specially: the
 * archive stays usable but every tango_zip_locate on it correctly reports "not found" and the
 * fallback trial moves on, exactly as it would for a real I/O failure on a present-but-corrupt file. */
typedef struct { FILE *f; } realfile_t;
static int rf_open(void *ctx, const char *path, uint32_t *size)
{
    realfile_t *r = ctx;
    r->f = fopen(path, "rb");
    if (!r->f) return 0;
    if (fseek(r->f, 0, SEEK_END) != 0) { fclose(r->f); r->f = NULL; return 0; }
    long sz = ftell(r->f);
    fseek(r->f, 0, SEEK_SET);
    if (sz < 0) { fclose(r->f); r->f = NULL; return 0; }
    *size = (uint32_t)sz;
    return 1;
}
static int rf_read(void *ctx, uint32_t off, uint8_t *buf, unsigned n)
{
    realfile_t *r = ctx;
    if (!r->f || fseek(r->f, (long)off, SEEK_SET) != 0) return -1;
    size_t got = fread(buf, 1, n, r->f);
    return (int)got;
}
static void rf_close(void *ctx)
{
    realfile_t *r = ctx;
    if (r->f) { fclose(r->f); r->f = NULL; }
}

static int file_exists(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f) fclose(f);
    return f != NULL;
}

/* Verifies one game's rom index 0 end to end: finds it, resolves its zip="" against zip_names[0..n),
 * opens each (a listed zip that doesn't exist is left unopened -- see rf_open's own note, not fatal),
 * streams the assembly, checks the exact byte count, then shells out to md5sum against a temp file
 * (never committed -- see this file's header) and compares to expected_md5. Skips cleanly (prints why,
 * does not fail) if the .mra itself isn't present; a listed zip missing is fine as long as the parts
 * that need it come from one of the others, same as a real card missing an unused fallback archive. */
static void verify_rom0(const char *game, const char *mra_path, const char *const zip_paths[], int n_zips,
                        const char *expected_md5, uint32_t expected_size)
{
    if (!file_exists(mra_path)) {
        printf("test_mra_real: %s not found -- skipping %s (not a failure)\n", mra_path, game);
        return;
    }

    realfile_t mra_ctx = { 0 }; tango_zipfs_t mra_io = { rf_open, rf_read, rf_close, &mra_ctx };

    realfile_t zip_ctx[TANGO_MRA_MAX_ZIPS] = { { 0 } };
    tango_zipfs_t zip_io[TANGO_MRA_MAX_ZIPS];
    tango_zip_t zip[TANGO_MRA_MAX_ZIPS];
    tango_zip_t *zips[TANGO_MRA_MAX_ZIPS];
    const tango_zipfs_t *zip_ios[TANGO_MRA_MAX_ZIPS];
    for (int i = 0; i < n_zips; i++) {
        zip_io[i] = (tango_zipfs_t){ rf_open, rf_read, rf_close, &zip_ctx[i] };
        memset(&zip[i], 0, sizeof zip[i]);
        if (!tango_zip_open_archive(&zip[i], &zip_io[i], zip_paths[i]))
            printf("test_mra_real: %s: %s did not open (fine if unused by this romset)\n", game, zip_paths[i]);
        zips[i] = &zip[i]; zip_ios[i] = &zip_io[i];
    }

    tango_mra_rom_t r; char zipname[TANGO_MRA_ZIP_NAME_MAX];
    CHECK(tango_mra_rom_open(&r, &mra_io, mra_path, 0, zipname, sizeof zipname));
    CHECK(r.found);

    static uint8_t out[1u << 20];   /* 1 MB: every real rom-0 checked here is under 64 KB */
    static const unsigned SIZES[] = { 1, 3, 97, 2048, 4096 };
    unsigned total = 0; int si = 0;
    int got = -1;
    for (;;) {
        unsigned ask = SIZES[si % (int)(sizeof SIZES / sizeof SIZES[0])]; si++;
        if (total >= sizeof out) break;
        if (ask > sizeof out - total) ask = (unsigned)(sizeof out - total);
        got = tango_mra_rom_read(&r, &mra_io, zips, zip_ios, n_zips, out + total, ask);
        if (got <= 0) break;
        total += (unsigned)got;
    }
    CHECK(got == 0);                        /* ended cleanly (0 = done), not an error or a refusal */
    CHECK(total == expected_size);

    tango_mra_rom_close(&r, &mra_io, zips, zip_ios);
    for (int i = 0; i < n_zips; i++) tango_zip_close(&zip[i], &zip_io[i]);

    /* MD5 oracle: write the assembled bytes to a temp file (never committed), shell out to md5sum
     * (already a host-test convention here -- tango_stream_check.py etc. are Python subprocesses
     * too), compare against the independently-verified digest. No ROM bytes ever touch git. */
    char tmp_path[] = "/tmp/tango_mra_real_test_XXXXXX";
    int fd = mkstemp(tmp_path);
    CHECK(fd >= 0);
    if (fd >= 0) {
        FILE *tf = fdopen(fd, "wb");
        fwrite(out, 1, total, tf);
        fclose(tf);

        char cmd[1200];
        snprintf(cmd, sizeof cmd, "md5sum %s", tmp_path);
        FILE *p = popen(cmd, "r");
        CHECK(p != NULL);
        if (p) {
            char line[256] = { 0 };
            CHECK(fgets(line, sizeof line, p) != NULL);
            pclose(p);
            CHECK(!strncmp(line, expected_md5, 32));
        }
        remove(tmp_path);
    }
}

/* Verifies one game's real <switches> block against values worked out by hand from the same source
 * file -- dip_def, dip_num, and one spot-checked
 * entry's start/mask/ids. Skips cleanly if the .mra isn't present. */
static void verify_switches(const char *game, const char *mra_path, uint64_t expect_def, int expect_num,
                            int spot_i, const char *spot_name, int spot_start, uint64_t spot_mask)
{
    if (!file_exists(mra_path)) {
        printf("test_mra_real: %s not found -- skipping %s switches (not a failure)\n", mra_path, game);
        return;
    }
    realfile_t mra_ctx = { 0 }; tango_zipfs_t mra_io = { rf_open, rf_read, rf_close, &mra_ctx };
    tango_mra_switches_t sw;
    CHECK(tango_mra_switches_parse(&mra_io, mra_path, 0, &sw));
    CHECK(sw.dip_def == expect_def);
    CHECK(sw.dip_num == expect_num);
    CHECK(spot_i < sw.dip_num);
    if (spot_i < sw.dip_num) {
        CHECK(!strcmp(sw.dip[spot_i].name, spot_name));
        CHECK(sw.dip[spot_i].start == spot_start);
        CHECK(sw.dip[spot_i].mask == spot_mask);
    }
}

int main(int argc, char **argv)
{
    const char *root = argc > 1 ? argv[1] : getenv("MISTER_PACK");
    char root_buf[1024];
    if (!root) {
        const char *home = getenv("HOME");
        if (!home) { printf("test_mra_real: no MISTER_PACK and no $HOME -- skipping\n"); return 0; }
        snprintf(root_buf, sizeof root_buf, "%s/MiSTer-PACK", home);
        root = root_buf;
    }

    char galaxian_mra[1024], galaxian_zip[1024];
    snprintf(galaxian_mra, sizeof galaxian_mra, "%s/_Arcade/Galaxian (Namco, Set 1).mra", root);
    snprintf(galaxian_zip, sizeof galaxian_zip, "%s/MAME/galaxian.zip", root);
    const char *galaxian_zips[] = { galaxian_zip };
    verify_rom0("Galaxian", galaxian_mra, galaxian_zips, 1,
                "434c546b09dbb7559e7fde9354c27d19", 24608);
    /* Galaxian's real <switches default="1F,1F,04">, 6 dips -- "Coinage" (bits="14,15") spot-checked */
    verify_switches("Galaxian", galaxian_mra, 0x041F1Full, 6, 3, "Coinage", 14, 0xC000);

    /* Galaga: zip="galaga.zip|galagamw.zip|
     * namco51.zip|namco54.zip" -- a real pipe fallback, not a formality. 10 of its 12 rom-0 parts
     * live in galaga.zip (several under a different internal name than the .mra's own name="", found
     * only by CRC); 51xx.bin/54xx.bin live only in namco51.zip/namco54.zip. galagamw.zip is listed but
     * isn't present in the library at all -- exercises the "a fallback entry that fails to open is
     * not fatal" path for real, not just in a synthetic test. */
    char galaga_mra[1024], galaga_zip[1024], galagamw_zip[1024], namco51_zip[1024], namco54_zip[1024];
    snprintf(galaga_mra, sizeof galaga_mra, "%s/_Arcade/Galaga (Midway, Set 1).mra", root);
    snprintf(galaga_zip, sizeof galaga_zip, "%s/MAME/galaga.zip", root);
    snprintf(galagamw_zip, sizeof galagamw_zip, "%s/MAME/galagamw.zip", root);
    snprintf(namco51_zip, sizeof namco51_zip, "%s/MAME/namco51.zip", root);
    snprintf(namco54_zip, sizeof namco54_zip, "%s/MAME/namco54.zip", root);
    const char *galaga_zips[] = { galaga_zip, galagamw_zip, namco51_zip, namco54_zip };
    verify_rom0("Galaga", galaga_mra, galaga_zips, 4,
                "3b0a45f542babfaa58e4298cab488fb6", 43008);
    /* Galaga's real <switches default="08,68" base="8">, 11 dips -- "Bonus" (bits="11,13") spot-checked */
    verify_switches("Galaga", galaga_mra, 0x6808ull, 11, 7, "Bonus", 11, 0x3800);

    /* Arkanoid (US): real <patch> tags (11 of them, including a 65-byte one), md5="None" on the <rom>
     * itself (so the INTERNAL pre-patch check is skipped -- confirms "None", capitalised, matches
     * MiSTer's own case-insensitive convention for real, not just a lowercase synthetic fixture) --
     * verify_rom0's own md5 here is the POST-patch result instead, independently assembled+patched
     * byte-for-byte in Python from the real zip, to prove the patch pipeline end to end
     * against real data (Galaxian/Galaga have no <patch> tags at all, so neither covers this). The
     * real zip="arkanoid.zip|arkatayt.zip" list's second entry isn't present in the library either --
     * another real "a fallback zip that doesn't exist" case, like Galaga's galagamw.zip. */
    char arkanoid_mra[1024], arkanoid_zip[1024], arkatayt_zip[1024];
    snprintf(arkanoid_mra, sizeof arkanoid_mra, "%s/_Arcade/Arkanoid (US).mra", root);
    snprintf(arkanoid_zip, sizeof arkanoid_zip, "%s/MAME/arkanoid.zip", root);
    snprintf(arkatayt_zip, sizeof arkatayt_zip, "%s/MAME/arkatayt.zip", root);
    const char *arkanoid_zips[] = { arkanoid_zip, arkatayt_zip };
    verify_rom0("Arkanoid", arkanoid_mra, arkanoid_zips, 2,
                "6f6cf39ed26ce2591e1ea68654b01c68", 165376);

    /* data only: Galaxian's real <buttons>, Galaga's real <nvram> */
    if (file_exists(galaxian_mra)) {
        realfile_t mra_ctx = { 0 }; tango_zipfs_t mra_io = { rf_open, rf_read, rf_close, &mra_ctx };
        tango_mra_misc_t misc;
        CHECK(tango_mra_misc_parse(&mra_io, galaxian_mra, &misc));
        CHECK(misc.has_buttons);
        CHECK(!strcmp(misc.button_default, "A,Start,Select,R,B,L"));
        CHECK(!strcmp(misc.button_names, "Fire,Start 1P,Start 2P,Coin,Test,Pause"));
    } else {
        printf("test_mra_real: %s not found -- skipping Galaxian <buttons> (not a failure)\n", galaxian_mra);
    }
    if (file_exists(galaga_mra)) {
        realfile_t mra_ctx = { 0 }; tango_zipfs_t mra_io = { rf_open, rf_read, rf_close, &mra_ctx };
        tango_mra_misc_t misc;
        CHECK(tango_mra_misc_parse(&mra_io, galaga_mra, &misc));
        CHECK(misc.has_nvram);
        CHECK(misc.nvram_index == 4 && misc.nvram_size == 51);
    } else {
        printf("test_mra_real: %s not found -- skipping Galaga <nvram> (not a failure)\n", galaga_mra);
    }

    printf("%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
