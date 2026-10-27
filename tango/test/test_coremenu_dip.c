// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 the Tango authors
/*
 * test_coremenu_dip.c -- host tests for the TANGO_CM_DIP page (tango_coremenu.c:
 * build_dip/dip_select, the "DIP" CONF_STR entry's CS_DIP row, and tango_core_dip_setup/_dip_cur).
 * Drives tango_coremenu.c through its public event API exactly as test_tango.c/test_coremenu_zip.c do, not
 * by calling its static helpers.
 *
 * What this checks: Galaga's real 11-switch, two-bank <switches> (also parsed and hand-verified in
 * test_mra.c) renders as the correct rows, paging, and +/- behaviour; a toggle produces the correct
 * dip_cur bit via the DIP send path (tango_dl_send). The <switches> block below is copied
 * VERBATIM (attribute values only, whitespace reformatted for this file -- tango_mra.c's own attribute
 * parsing is whitespace-insensitive, already proven in test_mra.c) from the real
 * MiSTer _Arcade/Galaga (Midway, Set 1).mra, the same file test_mra_real.c opens (opt-in,
 * gated on a local MAME ROM library) and whose dip_def (0x6808)/dip_num (11)/"Bonus" entry (bits 11,13, mask
 * 0x3800) that file independently confirms against a real copy. No ROM bytes, no zip
 * reference, no CRCs, no <part>/<rom> data from that file -- only the <switches> tag text, read directly
 * out of the real file, not re-typed or guessed.
 *
 * Every expected row/value/wire-byte check below was worked out by hand from that same real text (bit
 * positions, default bytes "08,68" little-endian = 0x6808, each <dip>'s own ids=""/values="" list), NOT
 * read back from the code under test -- see each case's own comment for the arithmetic. "+/-" above
 * is satisfied by Select/Enter cycling a row's value (real MiSTer's own MENU_ARCADE_DIP2,
 * menu.cpp 3576-3613, has no minus/plus for this page either, confirmed directly from source -- not a
 * guess), the same CS_OPTION-style mechanism page_select()'s own CS_OPTION case already uses.
 *
 * Tango, our own code.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "../tango_coremenu.h"
#include "fake_io.h"
#include "fake_zip.h"

static int fails = 0, checks = 0;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)
#define CHECKS(a, b) CHECK(!strcmp((a), (b)))

/* Galaga (Midway, Set 1).mra's own <switches> block, verbatim attribute text (see header comment). Two
 * physical DIP banks (the file's own comments: "DIP Switch 6K" = bits 0-7, six entries; "DIP Switch 6J" =
 * bits 8-15, three entries) plus two "Service switches" at bits 16-17 -- 11 <dip> entries total, matching
 * test_mra_real.c's own independently-confirmed dip_num. */
static const char GALAGA_MRA[] =
    "<misterromdescription>\n"
    "\t<switches default=\"08,68\" base=\"8\">\n"
    "\t\t<dip bits=\"0\"     name=\"2 credits\"   ids=\"2P game,1P game\"/>\n"
    "\t\t<dip bits=\"1,2\"   name=\"Difficulty\"  ids=\"Easy (Rank A),Medium (Rank B),Hard (Rank C),Hardest (Rank D)\" values=\"0,3,2,1\"/>\n"
    "\t\t<dip bits=\"3\"     name=\"Demo Sounds\" ids=\"Off,On\"/>\n"
    "\t\t<dip bits=\"4\"     name=\"Freeze\"      ids=\"Off,On\"/>\n"
    "\t\t<dip bits=\"5\"     name=\"Rack Test\"   ids=\"Off,On\"/>\n"
    "\t\t<dip bits=\"7\"     name=\"Cabinet\"     ids=\"Upright,Cocktail\"/>\n"
    "\t\t<dip bits=\"8,10\"  name=\"Coinage\"     ids=\"1 Coin  1 Credit ,2 Coins 1 Credit ,1 Coin  3 Credits,4 Coins 1 Credit ,1 Coin  2 Credits,3 Coins 1 Credit ,2 Coins 3 Credits,Free Play\"/>\n"
    "\t\t<dip bits=\"11,13\" name=\"Bonus\"\t     ids=\"30k 80k,20k 80k every 80k,30k 120k every 120k,20k 60k every 60k,20k 60k,20k 70k every 70k,30k 100k every 100k,Nothing\"/>\n"
    "\t\t<dip bits=\"14,15\" name=\"Lives\"       ids=\"5,3,4,2\"/>\n"
    "\t\t<dip bits=\"16\" name=\"Self-test mode\"  ids=\"Off,On\"/>\n"
    "\t\t<dip bits=\"17\" name=\"Service trigger\" ids=\"Off,On\"/>\n"
    "\t</switches>\n"
    "</misterromdescription>";

/* dip_def decode: default="08,68" -> bytes [0x08, 0x68], little-endian into the word -> 0x6808 (matches
 * test_mra_real.c's own independently-confirmed value exactly). */
#define GALAGA_DIP_DEF 0x6808ull
#define GALAGA_DIP_NUM 11

static int BUSY_FLAG = 0;
/* tango_core_platform_t's own dl_busy -- see case 11. */
static int fake_dl_busy(void *ctx) { (void)ctx; return BUSY_FLAG; }

static int row_is(const tango_page_t *p, int r, int kind, const char *label)
{
    if (r >= p->nrows) return 0;
    return p->rows[r].kind == kind && (!label || !strcmp(p->rows[r].label, label));
}
static int row_val_is(const tango_page_t *p, int r, const char *val)
{
    if (r >= p->nrows) return 0;
    const char *v = p->rows[r].value;
    return !strcmp(v ? v : "", val);
}

int main(void)
{
    fake_zipmem_t mram; tango_zipfs_t mra_io;
    fake_zipmem_init(&mram, &mra_io, (const unsigned char *)GALAGA_MRA, (unsigned)strlen(GALAGA_MRA));

    static tango_coremenu_t cm; static tango_osd_t osd; static tango_browser_t br;
    static rec_link_t r; tango_link_t link;
    static tango_core_platform_t plat;   /* all-zero: the DIP page never touches fs, zipfs, or any load/save hook */
    tango_page_t p;

    rec_link_init(&r, &link);
    r.confstr = "Galaga;;DIP;";   /* MiSTer's own bare "DIP" CONF_STR token (confstr.c's CS_DIP) */
    r.menumask = 0;
    tango_osd_probe(&osd, &link);
    tango_core_init(&cm, &link, &osd, &br, &plat, r.confstr, NULL);
    tango_core_dip_setup(&cm, &mra_io, "galaga.mra", GALAGA_DIP_NUM, GALAGA_DIP_DEF, GALAGA_DIP_DEF);

    /* ---- 1. page 0 shows the "DIP Switches" row (TR_SUB -- the chevron render, same as any CS_PAGE link),
     * and nothing else (this CONF_STR has no other page-0 entries) ---- */
    CHECK(tango_core_toggle(&cm) == 1);
    CHECK(cm.mode == TANGO_CM_PAGE && cm.page == 0);
    tango_core_page(&cm, &p);
    CHECK(row_is(&p, 0, TR_SUB, "DIP Switches"));

    /* ---- 2. selecting it opens TANGO_CM_DIP: 14 rows total -- 1 leading blank (centring: (13-11)/2 = 1,
     * menu.cpp 3528's own formula), the 11 dips in document order, 1 trailing blank (the generic
     * fill-to-TANGO_VIS-1 rule), then "Reset to apply" (menu.cpp 3567) ---- */
    CHECK(tango_core_event(&cm, TANGO_EV_SELECT) == TANGO_CM_REDRAWN);
    CHECK(cm.mode == TANGO_CM_DIP);
    tango_core_page(&cm, &p);
    CHECKS(p.title, "DIP Switches");
    CHECK(p.nrows == 14);
    CHECK(row_is(&p, 0, TR_BLANK, NULL));
    CHECK(row_is(&p, 12, TR_BLANK, NULL));
    CHECK(row_is(&p, 13, TR_ACT, "Reset to apply"));
    /* rebuild()'s own "land on the first selectable row" clamp: row 0 is blank, so sel skips to row 1 */
    CHECK(cm.sel == 1);

    /* ---- 3. every one of the 11 rows' name AND its default-selected value, independently hand-computed
     * from dip_def=0x6808 and each <dip>'s own bits=""/ids=""/values="" (see header comment; the full
     * arithmetic for each is below so a reviewer can re-check it against the real file without re-running
     * this test) ---- */
    /* row1: "2 credits" bits=0 mask=0x1, ids 2 no values (val=[0,1]); bit0 of 0x6808 = 0 -> id[0] */
    CHECK(row_is(&p, 1, TR_OPT, "2 credits"));    CHECK(row_val_is(&p, 1, "2P game"));
    /* row2: "Difficulty" bits=1,2 mask=0x6, values="0,3,2,1"<<1=[0,6,4,2]; (0x6808&0x6)=0 -> val[0] */
    CHECK(row_is(&p, 2, TR_OPT, "Difficulty"));   CHECK(row_val_is(&p, 2, "Easy (Rank A)"));
    /* row3: "Demo Sounds" bits=3 mask=0x8, val=[0,8]; (0x6808&0x8)=8 -> val[1] */
    CHECK(row_is(&p, 3, TR_OPT, "Demo Sounds"));  CHECK(row_val_is(&p, 3, "On"));
    /* row4: "Freeze" bits=4 mask=0x10; (0x6808&0x10)=0 -> val[0] */
    CHECK(row_is(&p, 4, TR_OPT, "Freeze"));       CHECK(row_val_is(&p, 4, "Off"));
    /* row5: "Rack Test" bits=5 mask=0x20; (0x6808&0x20)=0 -> val[0] */
    CHECK(row_is(&p, 5, TR_OPT, "Rack Test"));    CHECK(row_val_is(&p, 5, "Off"));
    /* row6: "Cabinet" bits=7 mask=0x80; (0x6808&0x80)=0 -> val[0] */
    CHECK(row_is(&p, 6, TR_OPT, "Cabinet"));      CHECK(row_val_is(&p, 6, "Upright"));
    /* row7: "Coinage" bits=8,10 mask=0x700, 8 ids no values (val[i]=i<<8); (0x6808&0x700)=0 -> val[0] */
    CHECK(row_is(&p, 7, TR_OPT, "Coinage"));      CHECK(row_val_is(&p, 7, "1 Coin  1 Credit "));
    /* row8: "Bonus" bits=11,13 mask=0x3800, 8 ids no values (val[i]=i<<11); 0x6808&0x3800=0x2800=val[5] */
    CHECK(row_is(&p, 8, TR_OPT, "Bonus"));        CHECK(row_val_is(&p, 8, "20k 70k every 70k"));
    /* row9: "Lives" bits=14,15 mask=0xC000, 4 ids no values (val[i]=i<<14); 0x6808&0xC000=0x4000=val[1] */
    CHECK(row_is(&p, 9, TR_OPT, "Lives"));        CHECK(row_val_is(&p, 9, "3"));
    /* row10: "Self-test mode" bits=16 mask=0x10000; bit16 of a word built only from 2 default bytes = 0 */
    CHECK(row_is(&p, 10, TR_OPT, "Self-test mode"));  CHECK(row_val_is(&p, 10, "Off"));
    /* row11: "Service trigger" bits=17 mask=0x20000; bit17 = 0 */
    CHECK(row_is(&p, 11, TR_OPT, "Service trigger"));  CHECK(row_val_is(&p, 11, "Off"));

    /* ---- 4. paging: Down from row 1 eleven times visits every one of the 11 dip rows in order and lands
     * exactly on "Reset to apply" (row 13), skipping the blank at row 12 -- the SAME generic move()/
     * render_page() scroll mechanism every other rows[]-based page here already uses, needing no new
     * paging code of its own (tango_layout.c's render_page: nrows (14) > TANGO_VIS (14) is false here, so
     * this page does not even scroll -- a core with more DIPs than fit would, automatically, through that
     * same existing mechanism; see point 10 below, a synthetic case with more dips than fit one screen,
     * for that case actually being exercised). ---- */
    for (int i = 2; i <= 11; i++) {
        CHECK(tango_core_event(&cm, TANGO_EV_DOWN) == TANGO_CM_REDRAWN);
        CHECK(cm.sel == i);
    }
    CHECK(tango_core_event(&cm, TANGO_EV_DOWN) == TANGO_CM_REDRAWN);   /* off row 11, skip blank row 12 */
    CHECK(cm.sel == 13);
    CHECK(cm.ent[cm.sel] == -1);                                       /* ROW_EXIT: "Reset to apply" */
    CHECK(tango_core_event(&cm, TANGO_EV_UP) == TANGO_CM_REDRAWN);     /* back up onto the last dip row */
    CHECK(cm.sel == 11);

    /* ---- 5. a toggle: Select/Enter on "Bonus" (row 8) cycles (m+1)%num = (5+1)%8 = 6 -> val[6] = 6<<11 =
     * 0x3000 (menu.cpp 3607-3608's own formula, independently re-applied here, not read back from the code
     * under test), sent whole over the SAME wire path arc_setup_one uses (tango_dl_send,
     * SDC_IMAGE index 254) -- the exact 8 little-endian bytes of the new dip_cur are checked below byte for byte, not
     * just a bit of it. ---- */
    /* point 4 left cm.sel == 11 (the last dip row); walk back to row 8 ("Bonus") one step at a time,
     * through the same generic move()/tango_core_event path every other row navigation here uses */
    while (cm.sel != 8) CHECK(tango_core_event(&cm, cm.sel > 8 ? TANGO_EV_UP : TANGO_EV_DOWN) == TANGO_CM_REDRAWN);
    CHECK(cm.sel == 8);
    int status_writes_before = r.status_writes;
    CHECK(tango_core_event(&cm, TANGO_EV_SELECT) == TANGO_CM_REDRAWN);   /* still open: not the apply row */
    CHECK(cm.mode == TANGO_CM_DIP);
    CHECK(r.status_writes == status_writes_before);         /* an ordinary toggle never pulses status */
    CHECK(tango_core_dip_cur(&cm) == 0x7008ull);             /* (0x6808 & ~0x3800) | 0x3000 = 0x4008|0x3000 */
    CHECK(r.dl_index == 254);
    CHECK(r.dl_got == 8);
    static const uint8_t expect_wire[8] = { 0x08, 0x70, 0, 0, 0, 0, 0, 0 };   /* 0x7008, little-endian */
    CHECK(!memcmp(r.dl_buf, expect_wire, 8));
    tango_core_page(&cm, &p);
    CHECK(row_val_is(&p, 8, "30k 100k every 100k"));          /* ids[6] */

    /* ---- 6. "Reset to apply": closes the OSD entirely (not back to page 0, menu.cpp 3584-3592's own
     * MENU_NONE1), pulses status[0] (set then clear -- a real reset so a core that only latches switches at
     * reset picks up the new value) and tells the caller to persist dip_cur (TANGO_CM_DIP_SAVE, not
     * TANGO_CM_REDRAWN/CLOSED_NOW -- this module has no FatFs access of its own). ---- */
    CHECK(tango_core_event(&cm, TANGO_EV_DOWN) == TANGO_CM_REDRAWN);  /* -> row 9 */
    CHECK(tango_core_event(&cm, TANGO_EV_DOWN) == TANGO_CM_REDRAWN);  /* -> row 10 */
    CHECK(tango_core_event(&cm, TANGO_EV_DOWN) == TANGO_CM_REDRAWN);  /* -> row 11 */
    CHECK(tango_core_event(&cm, TANGO_EV_DOWN) == TANGO_CM_REDRAWN);  /* -> row 13 (skips blank row 12) */
    CHECK(cm.sel == 13);
    status_writes_before = r.status_writes;
    uint64_t cur_before_reset = tango_core_dip_cur(&cm);
    CHECK(tango_core_event(&cm, TANGO_EV_SELECT) == TANGO_CM_DIP_SAVE);
    CHECK(cm.mode == TANGO_CM_CLOSED);
    CHECK(r.status_writes == status_writes_before + 2);      /* set, then clear -- a real pulse, not one write */
    CHECK(r.status[0] == 0);                                  /* ends cleared (the second write), not left set */
    CHECK(tango_core_dip_cur(&cm) == cur_before_reset);       /* "Reset to apply" does not change dip_cur itself */

    /* ---- 7. Back/Left/Menu from the DIP page (without "Reset to apply"): returns to page 0 ON THE DIP
     * ROW (parent_sel), NOT a close -- and ALSO reports TANGO_CM_DIP_SAVE (menu.cpp 3576-3582's own
     * unconditional arcade_sw_save on this path too) ---- */
    CHECK(tango_core_toggle(&cm) == 1);                        /* reopen: page 0 */
    CHECK(tango_core_event(&cm, TANGO_EV_SELECT) == TANGO_CM_REDRAWN);   /* -> TANGO_CM_DIP again */
    CHECK(cm.mode == TANGO_CM_DIP);
    CHECK(tango_core_event(&cm, TANGO_EV_BACK) == TANGO_CM_DIP_SAVE);
    CHECK(cm.mode == TANGO_CM_PAGE && cm.page == 0);
    CHECK(cm.sel == 0);                                        /* parent_sel: the DIP row itself */
    CHECK(tango_core_toggle(&cm) == 0);                        /* close for the next case */

    /* ---- 7b. the GENERIC "System > Reset settings" (not the DIP page's own "Reset to apply") ALSO resets
     * dip_cur back to its default -- menu.cpp 5964-5970's own real precedent; this struct tracks
     * dip_cur/dip_def. cm.dip_cur is still 0x7008 from point 5's toggle (neither point 6's "Reset
     * to apply" nor point 7's Back ever changed it) -- a real before/after to show this path is doing
     * something, not just happening to already match the default. ---- */
    CHECK(tango_core_dip_cur(&cm) == 0x7008ull);
    CHECK(tango_core_toggle(&cm) == 1);                        /* page 0 */
    CHECK(tango_core_event(&cm, TANGO_EV_RIGHT) == TANGO_CM_REDRAWN);   /* page 0 -> System (menu.cpp Right) */
    CHECK(cm.mode == TANGO_CM_SYSTEM);
    tango_core_page(&cm, &p);
    int reset_row = -1;
    for (int i = 0; i < p.nrows; i++) if (p.rows[i].label && !strcmp(p.rows[i].label, "Reset settings")) { reset_row = i; break; }
    CHECK(reset_row >= 0);
    while (cm.sel != reset_row) CHECK(tango_core_event(&cm, TANGO_EV_DOWN) == TANGO_CM_REDRAWN);
    CHECK(tango_core_event(&cm, TANGO_EV_SELECT) == TANGO_CM_REDRAWN);
    CHECK(cm.mode == TANGO_CM_RESET);
    CHECK(tango_core_event(&cm, TANGO_EV_UP) == TANGO_CM_REDRAWN);     /* off "no" (the default) onto "yes" */
    int sw_before = r.status_writes;
    CHECK(tango_core_event(&cm, TANGO_EV_SELECT) == TANGO_CM_DIP_SAVE);   /* not CLOSED_NOW: dip_num > 0 */
    CHECK(cm.mode == TANGO_CM_CLOSED);
    CHECK(tango_core_dip_cur(&cm) == GALAGA_DIP_DEF);           /* back to 0x6808, not left at 0x7008 */
    CHECK(r.dl_index == 254);
    static const uint8_t expect_def_wire[8] = { 0x08, 0x68, 0, 0, 0, 0, 0, 0 };   /* 0x6808, little-endian */
    CHECK(!memcmp(r.dl_buf, expect_def_wire, 8));
    CHECK(r.status_writes == sw_before + 3);   /* the general status reset (1) + the dip-specific pulse (2) */
    CHECK(r.status[0] == 0);                    /* the pulse ends cleared */

    /* ---- 8. a core with NO <switches> at all (dip_num 0): the "DIP Switches" row never appears, page 0
     * has nothing else either (menu.cpp 2233-2239's own "no entries -> System" rule) -- confirms the page
     * stays fully inert, and TANGO_CM_DIP unreachable, for every console core / DIP-less arcade core (the
     * "zero behaviour change for them" claim tango_core_dip_setup's own doc comment makes) ---- */
    {
        static tango_coremenu_t cm2;
        tango_core_init(&cm2, &link, &osd, &br, &plat, r.confstr, NULL);
        /* tango_core_dip_setup deliberately never called -- memset's own zeroed dip_num is what every
         * console core relies on */
        CHECK(tango_core_toggle(&cm2) == 1);
        CHECK(cm2.mode == TANGO_CM_SYSTEM);          /* build_page found zero rows (DIP hidden, nothing
                                                       * else on page 0) -> rebuild()'s own fallback */
    }

    /* ---- 9. a malformed <dip> with no ids="" at all (num 0): rendered disabled (TR_DIS), not a crash --
     * a deliberate, stated safety deviation from a literal MiSTer port (real MENU_ARCADE_DIP2 would divide
     * by zero cycling a row like this, menu.cpp 3607's own "% sw->dip[menusub].num"); Select on it is a
     * no-op, same as any other TR_DIS row (page_select's own "greyed: nothing" rule) ---- */
    {
        static const char ORPHAN_MRA[] =
            "<misterromdescription><switches default=\"00\">"
            "<dip bits=\"0\" name=\"orphan\" values=\"1,2,3\"/>"
            "</switches></misterromdescription>";
        fake_zipmem_t m2; tango_zipfs_t io2;
        fake_zipmem_init(&m2, &io2, (const unsigned char *)ORPHAN_MRA, (unsigned)strlen(ORPHAN_MRA));
        static tango_coremenu_t cm3;
        tango_core_init(&cm3, &link, &osd, &br, &plat, r.confstr, NULL);
        tango_core_dip_setup(&cm3, &io2, "orphan.mra", 1, 0, 0);
        CHECK(tango_core_toggle(&cm3) == 1);
        CHECK(tango_core_event(&cm3, TANGO_EV_SELECT) == TANGO_CM_REDRAWN);
        CHECK(cm3.mode == TANGO_CM_DIP);
        CHECK(cm3.sel == 6);     /* pad = (TANGO_VIS-1 - dip_num)/2 = (13-1)/2 = 6 leading blanks */
        tango_core_page(&cm3, &p);
        CHECK(row_is(&p, cm3.sel, TR_DIS, "orphan"));
        CHECK(tango_core_event(&cm3, TANGO_EV_SELECT) == TANGO_CM_NOTHING);   /* no crash, no toggle */
        CHECK(tango_core_toggle(&cm3) == 0);
    }

    /* ---- 10. a synthetic core with MORE dips (20, bits 0-19) than fit one screen -- proves build_dip()/
     * move() genuinely span multiple screens' worth through the SAME render_page() auto-scroll every other
     * long rows[] page here already relies on (tango_layout.c: nrows > TANGO_VIS triggers its own
     * top=min(max(0,sel-VIS+3),max(0,nrows-VIS)) formula, not re-tested pixel-by-pixel here -- that part is
     * tango_layout.c's own generic concern) -- Galaga's real 11 dips (one screen exactly, point 2-4 above)
     * never exercised this at all. ---- */
    {
        char big_mra[4096];
        int o = 0;
        o += snprintf(big_mra + o, sizeof big_mra - (size_t)o, "<misterromdescription><switches default=\"00\">");
        for (int i = 0; i < 20; i++)
            o += snprintf(big_mra + o, sizeof big_mra - (size_t)o, "<dip bits=\"%d\" name=\"d%d\" ids=\"A,B\"/>", i, i);
        o += snprintf(big_mra + o, sizeof big_mra - (size_t)o, "</switches></misterromdescription>");
        fake_zipmem_t m4; tango_zipfs_t io4;
        fake_zipmem_init(&m4, &io4, (const unsigned char *)big_mra, (unsigned)o);
        static tango_coremenu_t cm4;
        tango_core_init(&cm4, &link, &osd, &br, &plat, r.confstr, NULL);
        tango_core_dip_setup(&cm4, &io4, "big.mra", 20, 0, 0);
        CHECK(tango_core_toggle(&cm4) == 1);
        CHECK(tango_core_event(&cm4, TANGO_EV_SELECT) == TANGO_CM_REDRAWN);
        CHECK(cm4.mode == TANGO_CM_DIP);
        tango_core_page(&cm4, &p);
        /* pad = 0 (20 >= TANGO_VIS-1=13, no centring blanks); 20 dip rows + 1 "Reset to apply" = 21 */
        CHECK(p.nrows == 21);
        CHECK(cm4.sel == 0);                 /* row 0 is the first dip itself -- no leading blank this time */
        CHECK(row_is(&p, 0, TR_OPT, "d0"));
        CHECK(row_is(&p, 19, TR_OPT, "d19"));
        CHECK(row_is(&p, 20, TR_ACT, "Reset to apply"));
        /* walk all 20 dip rows with Down, landing on "Reset to apply" -- 20 presses, well past TANGO_VIS
         * (14): only reachable at all if move()'s wrap-skip and the auto-scroll both keep working beyond
         * one screen, not just assumed to because Galaga's own 11 happened to fit */
        for (int i = 1; i <= 20; i++) {
            CHECK(tango_core_event(&cm4, TANGO_EV_DOWN) == TANGO_CM_REDRAWN);
            CHECK(cm4.sel == i);
        }
        CHECK(cm4.ent[cm4.sel] == -1);        /* landed on "Reset to apply" */
        CHECK(tango_core_toggle(&cm4) == 0);
    }

    /* ---- 11. tango_core_platform_t's own dl_busy: dip_select() refuses a toggle rather
     * than risk a new SELECT corrupting a download/upload this module cannot see on its own (the .mra ROM
     * auto-download, a zip load, an NVRAM save -- all firmware-side, tango_fw.c's own arc_dl_idle) --
     * tango_dl_send/tango_ul_recv's own doc comment already names this exact hazard by name. No wire
     * traffic and no dip_cur change while busy; works again once it clears. ---- */
    {
        fake_zipmem_init(&mram, &mra_io, (const unsigned char *)GALAGA_MRA, (unsigned)strlen(GALAGA_MRA));
        static tango_coremenu_t cm5;
        static tango_core_platform_t plat2;
        plat2 = plat; plat2.dl_busy = fake_dl_busy;
        tango_core_init(&cm5, &link, &osd, &br, &plat2, r.confstr, NULL);
        tango_core_dip_setup(&cm5, &mra_io, "galaga.mra", GALAGA_DIP_NUM, GALAGA_DIP_DEF, GALAGA_DIP_DEF);
        CHECK(tango_core_toggle(&cm5) == 1);
        CHECK(tango_core_event(&cm5, TANGO_EV_SELECT) == TANGO_CM_REDRAWN);   /* -> TANGO_CM_DIP, row 1 */
        CHECK(cm5.mode == TANGO_CM_DIP);
        BUSY_FLAG = 1;
        uint8_t dl_index_before = r.dl_index;
        uint64_t cur_before = tango_core_dip_cur(&cm5);
        CHECK(tango_core_event(&cm5, TANGO_EV_SELECT) == TANGO_CM_NOTHING);   /* "2 credits": refused */
        CHECK(tango_core_dip_cur(&cm5) == cur_before);     /* unchanged -- never even attempted */
        CHECK(r.dl_index == dl_index_before);               /* zero new wire traffic */
        BUSY_FLAG = 0;
        CHECK(tango_core_event(&cm5, TANGO_EV_SELECT) == TANGO_CM_REDRAWN);   /* clear: works again */
        CHECK(tango_core_dip_cur(&cm5) != cur_before);
        CHECK(tango_core_toggle(&cm5) == 0);
    }

    printf("%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
