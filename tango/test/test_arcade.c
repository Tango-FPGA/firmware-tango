// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 the Tango authors
/*
 * test_arcade.c -- host tests for the two whole-blob helpers
 * (tango_dl_send/tango_ul_recv, tango_coremenu.c) against fake_io.c's own extension of rec_link_t:
 * a plain C-level stand-in for sys-tango tango_io.v's upload ports (UL_SELECT/UL_STATUS/READ, sub 4/5/3),
 * alongside the download-side stand-in (SELECT/STATUS/WRITE, sub 1/0/2).
 *
 * Scope, stated plainly: this proves the GENERIC send/receive protocol primitives tango_fw.c's own arcade glue
 * (file I/O, .mra switches/nvram parsing, the core-load/Save-settings/reboot hook points) is built on top
 * of -- it does not compile or exercise tango_fw.c itself (not host-testable: FatFs/
 * FreeRTOS, only the real firmware build touches it). "Survives a reboot" is simulated at exactly the
 * layer this test CAN reach: a plain host file (fopen/fwrite/fread, standing in for tango_fw.c's own FatFs
 * config/nvram/<name>.nvm) written by a first "session", then read back and re-sent by a second, fresh
 * rec_link_t (standing in for a freshly booted core) -- proving the save-then-load round trip is correct at
 * the byte level, the same thing the real file I/O would do differently only in which library opens the file.
 *
 * Tango, our own code.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "../tango_coremenu.h"
#include "fake_io.h"

static int fails = 0, checks = 0;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)

int main(void)
{
    /* ---- 1. DIP send (MiSTer's arcade_sw_send: index 254, the raw uint64_t dip_cur, whole) ---- */
    {
        rec_link_t r; tango_link_t link; rec_link_init(&r, &link);
        /* a real-shaped default: Galaxian/Galaga's own <switches default="1F,1F,04"> decodes (tango_mra.c's
         * hex_bytes_decode + little-endian fold, test_mra.c's own coverage of that) to 0x00041F1F, not the
         * byte sequence read in document order -- this value just needs to be some plausible 8-byte dip_cur,
         * not a re-derivation of that decode, which is already covered elsewhere. */
        uint64_t dip_cur = 0x00041F1FULL;
        int rc = tango_dl_send(&link, 254, (const uint8_t *)&dip_cur, sizeof dip_cur);
        CHECK(rc == 0);
        CHECK(r.dl_starts == 1 && r.dl_ends == 1);           /* SELECT 254 size 8, ..., SELECT 254 size 0 */
        CHECK(r.dl_index == 254);
        CHECK(r.dl_got == sizeof dip_cur);
        CHECK(!memcmp(r.dl_buf, &dip_cur, sizeof dip_cur));  /* the exact 8 bytes arrived, not just a checksum */
        CHECK(r.dl_dropped == 0);
    }
    /* ---- 2. cheat send: same mechanism, index 255 ---- */
    {
        rec_link_t r; tango_link_t link; rec_link_init(&r, &link);
        uint64_t cheat_cur = 0xABCDEF0102030405ULL;
        CHECK(tango_dl_send(&link, 255, (const uint8_t *)&cheat_cur, sizeof cheat_cur) == 0);
        CHECK(r.dl_index == 255);
        CHECK(!memcmp(r.dl_buf, &cheat_cur, sizeof cheat_cur));
    }
    /* ---- 3. tango_dl_send(n=0): nothing sent, no SELECT at all (0 = "close", not "open size 0 then close") ---- */
    {
        rec_link_t r; tango_link_t link; rec_link_init(&r, &link);
        CHECK(tango_dl_send(&link, 254, (const uint8_t *)"", 0) == 0);
        CHECK(r.dl_starts == 0 && r.transfers == 0);
    }
    /* ---- 4. NVRAM save: tango_ul_recv pulls a known blob back from "the core" (fake_io.h's ul_buf, the
     * fake's stand-in for the core's own live NVRAM content) at a real .mra-shaped index/size (Galaga's own
     * <nvram index="4" size="51">) ---- */
    {
        rec_link_t r; tango_link_t link; rec_link_init(&r, &link);
        for (int i = 0; i < 51; i++) r.ul_buf[i] = (uint8_t)(0x40 + i);   /* "the core's current NVRAM" */
        uint8_t out[51]; uint32_t got = 0xdeadbeef;
        int rc = tango_ul_recv(&link, 4, 51, out, sizeof out, &got);
        CHECK(rc == 0);
        CHECK(got == 51);
        CHECK(!memcmp(out, r.ul_buf, 51));
        CHECK(r.ul_opens == 1 && r.ul_closes == 1);
        CHECK(r.ul_index == 4);
        CHECK(r.ul_reads >= 1);
    }
    /* ---- 5. tango_ul_recv refuses a session above the caller's own buffer outright (mirrors
     * tango_ul_begin's own TANGO_UL_MAX refusal, one layer up: the CALLER's cap, not the RTL's clamp) --
     * never opens a session it cannot hold whole, so UL_STATUS is never even asked ---- */
    {
        rec_link_t r; tango_link_t link; rec_link_init(&r, &link);
        uint8_t out[8]; uint32_t got = 123;
        CHECK(tango_ul_recv(&link, 4, 51, out, sizeof out, &got) == -1);   /* size 51 > cap 8 */
        CHECK(got == 0);
        CHECK(r.ul_opens == 0);                               /* refused before ever touching the wire */
        CHECK(r.transfers == 0);
    }
    /* ---- 6. a core that never answers (TANGO_IOCTL_UPLOAD not built, or UL_SELECT sent to the wrong
     * index): UL_STATUS reports avail 0 from the first poll (fake_io.c's ul_open stays 0) -- tango_ul_pump's
     * own "avail == 0 means give up" rule fires, a clean failure, never an infinite poll ---- */
    {
        rec_link_t r; tango_link_t link; rec_link_init(&r, &link);
        r.ul_unsupported = 1;        /* models a core built without TANGO_IOCTL_UPLOAD at all */
        uint8_t out[8]; uint32_t got = 0;
        CHECK(tango_ul_recv(&link, 9, 4, out, sizeof out, &got) == -1);
        CHECK(got == 0);
    }
    /* ---- 7. the hazard tango_coremenu.c's own comment on tango_dl_send/tango_ul_recv documents by name:
     * calling tango_ul_recv while a download is still open. fake_io.c's own UL_SELECT branch models the
     * real RTL's *consequence* differently on purpose (refuses outright, ul_refused_while_dl++, rather than
     * re-modelling the RTL's own defer-then-honour mechanism a second time) -- what this proves is that
     * tango_fw.c's own glue must never let this happen (its arc_dl_idle() guard, not built here but
     * required of it), not that the deferral itself works (the RTL's own upload testbench covers that, on
     * the real RTL, exhaustively). Left open while a download is "open": the save fails cleanly (avail
     * never goes nonzero), it does not corrupt the download. ---- */
    {
        rec_link_t r; tango_link_t link; rec_link_init(&r, &link);
        r.dl_open = 1; r.dl_index = 0; r.dl_size = 100;       /* a ROM auto-download still in flight */
        uint8_t out[8]; uint32_t got = 0;
        CHECK(tango_ul_recv(&link, 4, 4, out, sizeof out, &got) == -1);
        CHECK(r.ul_refused_while_dl == 1);
        CHECK(r.ul_open == 0);                                /* never actually opened */
        CHECK(r.dl_open == 1);                                /* and the download itself is untouched */
    }

    /* ---- 7b. a wedged core: UL_STATUS reports the session open and the real size (avail != 0, so
     * tango_ul_pump's own "avail == 0 means give up" rule never fires) but never sets the ready bit --
     * tango_ul_recv's own poll cap (tango_coremenu.c; an unbounded loop would be a real hang risk for
     * c_reboot's own NVRAM-save-after-teardown) must still give up, cleanly,
     * rather than loop forever. Also closes the session on the wire (UL_SELECT size 0) instead of leaving
     * it open, matching this file's own "a session left open stalls every later download" contract.
     * delay_total (rl_delay's own running total, fake_io.c) bounds the GIVE-UP time itself, not just the
     * iteration count: tango_coremenu.c's own TANGO_DL_SEND_MAX_POLLS (2000) * TANGO_DL_SEND_POLL_MS (1)
     * is a real ~2 s wall-clock bound (link->delay_ms between retries), verified here rather than
     * assumed. ---- */
    {
        rec_link_t r; tango_link_t link; rec_link_init(&r, &link);
        r.ul_never_ready = 1;
        uint8_t out[8]; uint32_t got = 123;
        CHECK(tango_ul_recv(&link, 4, 4, out, sizeof out, &got) == -1);
        CHECK(got == 0);
        CHECK(r.ul_opens == 1);
        CHECK(r.ul_closes == 1);     /* tango_ul_end was called on timeout, not left open */
        CHECK(r.ul_open == 0);
        CHECK(r.delay_total >= 1900 && r.delay_total <= 2100);   /* ~2000 ms, not an unbounded/unknown wait */
    }
    /* ---- 7c. the download-side mirror: the core never offers any free space (dl_space stays 0, as if it
     * were holding ioctl_wait forever) -- tango_dl_send's own poll cap must give up the same way, in the
     * same bounded time. ---- */
    {
        rec_link_t r; tango_link_t link; rec_link_init(&r, &link);
        r.dl_space = 0;
        uint64_t dip_cur = 0x1122;
        CHECK(tango_dl_send(&link, 254, (const uint8_t *)&dip_cur, sizeof dip_cur) == -1);
        CHECK(r.dl_starts == 1);
        CHECK(r.dl_ends == 1);       /* tango_dl_end was called on timeout, closing the session on the wire */
        CHECK(r.dl_got == 0);        /* nothing was ever actually written -- the core never offered room */
        CHECK(r.delay_total >= 1900 && r.delay_total <= 2100);
    }

    /* ---- 8. "survives a reboot": a save (tango_ul_recv) written to a plain host file (tango_fw.c's own
     * FatFs config/nvram/<name>.nvm, stood in for here -- see this file's own header comment for why),
     * then a FRESH "boot" (a brand new rec_link_t, modelling a freshly reset core) loads that same file and
     * sends it back to the core via tango_dl_send at the same index -- the real NVRAM-load path,
     * mirroring MiSTer's arcade_nvm_load exactly ("if FileLoadConfig succeeds, send it"). ---- */
    {
        const char *path = "/tmp/test_arcade_nvram_session.bin";
        uint8_t saved[51];
        /* "session 1": the core has been playing, NVRAM has some non-default content; Save settings (or a
         * reboot) triggers the save. */
        {
            rec_link_t r; tango_link_t link; rec_link_init(&r, &link);
            for (int i = 0; i < 51; i++) r.ul_buf[i] = (uint8_t)(i * 7 + 3);
            uint32_t got = 0;
            CHECK(tango_ul_recv(&link, 4, 51, saved, sizeof saved, &got) == 0);
            CHECK(got == 51);
            FILE *f = fopen(path, "wb");
            CHECK(f != NULL);
            if (f) { CHECK(fwrite(saved, 1, 51, f) == 51); fclose(f); }
        }
        /* "reboot": a fresh core session (new rec_link_t -- nothing of session 1's state carries over
         * except the file on disk), the same .mra's <nvram index="4" size="51"> is parsed again, the saved
         * file is found and loaded, then sent to the (new) core exactly as MiSTer's arcade_nvm_load does
         * ("if FileLoadConfig succeeds..."). */
        {
            rec_link_t r; tango_link_t link; rec_link_init(&r, &link);
            uint8_t loaded[51]; memset(loaded, 0, sizeof loaded);
            FILE *f = fopen(path, "rb");
            CHECK(f != NULL);
            size_t n = f ? fread(loaded, 1, sizeof loaded, f) : 0;
            if (f) fclose(f);
            CHECK(n == 51);
            CHECK(!memcmp(loaded, saved, 51));            /* the file itself survived, byte for byte */
            CHECK(tango_dl_send(&link, 4, loaded, 51) == 0);
            CHECK(r.dl_index == 4);
            CHECK(!memcmp(r.dl_buf, saved, 51));          /* ... and the NEW core received exactly that */
        }
        remove(path);
    }

    printf("%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
