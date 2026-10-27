// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 the Tango authors
/* Host unit tests for confstr.c. Strings are the real ones read from source:
 * MiSTer-devel/Template_MiSTer Template.sv 58-86 and MiSTer-devel/Menu_MiSTer menu.sv 60-64 (at fa83dc1).
 * Test data copied as text: TEMPLATE is Template_MiSTer's CONF_STR (GPL-2.0-or-later, as Template.sv's header states;
 * the header names no author), MENU is Menu_MiSTer's (see NOTICE). This file is GPL-3.0-or-later with the whole
 * firmware (LICENSE-GPL-3.0). */
#include <stdio.h>
#include <string.h>
#include "../confstr.h"

static int fails = 0, checks = 0;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)
static int teq(cs_text_t t, const char *s) { return t.len == strlen(s) && !memcmp(t.p, s, t.len); }

static const char *TEMPLATE =
    "Template;;"
    "-;"
    "O[122:121],Aspect ratio,Original,Full Screen,[ARC1],[ARC2];"
    "O[2],TV Mode,NTSC,PAL;"
    "O[4:3],Noise,White,Red,Green,Blue;"
    "-;"
    "P1,Test Page 1;"
    "P1-;"
    "P1-, -= Options in page 1 =-;"
    "P1-;"
    "P1O[5],Option 1-1,Off,On;"
    "d0P1F1,BIN;"
    "H0P1O[10],Option 1-2,Off,On;"
    "-;"
    "P2,Test Page 2;"
    "P2-;"
    "P2-, -= Options in page 2 =-;"
    "P2-;"
    "P2S0,DSK;"
    "P2O[7:6],Option 2,1,2,3,4;"
    "-;"
    "-;"
    "T[0],Reset;"
    "R[0],Reset and close OSD;"
    "v,0;";

static const char *MENU = "MENU;UART31250,MIDI;-  ;V,v250928";

static cs_entry_t entry(const char *s, int i)
{
    cs_text_t t; cs_entry_t e;
    CHECK(cs_get(s, i, &t));
    CHECK(cs_parse(t, &e));
    return e;
}

int main(void)
{
    cs_text_t t; cs_entry_t e; uint8_t lo, hi;

    /* ---- splitting ---- */
    CHECK(cs_get(TEMPLATE, 0, &t) && teq(t, "Template"));
    CHECK(cs_get(TEMPLATE, 1, &t) && t.len == 0);
    CHECK(cs_count(TEMPLATE) == 26);
    CHECK(cs_get(TEMPLATE, 25, &t) && teq(t, "v,0"));
    CHECK(!cs_get(TEMPLATE, 26, &t));

    /* ---- status bits ---- */
    CHECK(cs_bits("[122:121]", 9, 0, &lo, &hi) == 9 && lo == 121 && hi == 122);
    CHECK(cs_bits("[2]", 3, 0, &lo, &hi) == 3 && lo == 2 && hi == 2);
    CHECK(cs_bits("89,", 3, 0, &lo, &hi) == 2 && lo == 8 && hi == 9);      /* old form, two chars */
    CHECK(cs_bits("A,", 2, 0, &lo, &hi) == 1 && lo == 10 && hi == 10);     /* old form, one char */
    CHECK(cs_bits("2", 1, 1, &lo, &hi) == 1 && lo == 34 && hi == 34);      /* lowercase: +32 */
    CHECK(cs_bits("[9:0]", 5, 0, &lo, &hi) == 0);                           /* 10 bits: too wide */
    CHECK(cs_bits("[8:0]", 5, 0, &lo, &hi) == 5);                           /* 9 bits: allowed */
    CHECK(cs_bits("[128]", 5, 0, &lo, &hi) == 0);
    CHECK(cs_bits("[3:3]", 5, 0, &lo, &hi) == 0);

    /* ---- the Template's entries ---- */
    e = entry(TEMPLATE, 2);  CHECK(e.kind == CS_TEXT && e.name.len == 0);
    e = entry(TEMPLATE, 3);  CHECK(e.kind == CS_OPTION && e.bit_lo == 121 && e.bit_hi == 122 && teq(e.name, "Aspect ratio") && e.n_values == 4);
    CHECK(cs_value(&e, 1, &t) && teq(t, "Full Screen"));
    CHECK(cs_value(&e, 3, &t) && teq(t, "[ARC2]"));
    CHECK(!cs_value(&e, 4, &t));
    e = entry(TEMPLATE, 4);  CHECK(e.kind == CS_OPTION && e.bit_lo == 2 && e.bit_hi == 2 && e.n_values == 2);
    e = entry(TEMPLATE, 5);  CHECK(e.kind == CS_OPTION && e.bit_lo == 3 && e.bit_hi == 4 && e.n_values == 4);
    e = entry(TEMPLATE, 7);  CHECK(e.kind == CS_PAGE && e.target_page == 1 && teq(e.name, "Test Page 1") && e.page == 0);
    e = entry(TEMPLATE, 9);  CHECK(e.kind == CS_TEXT && e.page == 1 && teq(e.name, " -= Options in page 1 =-"));
    e = entry(TEMPLATE, 11); CHECK(e.kind == CS_OPTION && e.page == 1 && e.bit_lo == 5 && teq(e.name, "Option 1-1"));
    e = entry(TEMPLATE, 12); CHECK(e.kind == CS_FILE && e.page == 1 && e.index == 1 && teq(e.ext, "BIN") && e.name.len == 0);
    CHECK(e.n_hd == 1 && e.hd[0].op == 'd' && e.hd[0].bit == 0);
    e = entry(TEMPLATE, 13); CHECK(e.kind == CS_OPTION && e.page == 1 && e.bit_lo == 10 && e.n_hd == 1 && e.hd[0].op == 'H');
    e = entry(TEMPLATE, 19); CHECK(e.kind == CS_MOUNT && e.page == 2 && e.index == 0 && teq(e.ext, "DSK"));
    e = entry(TEMPLATE, 20); CHECK(e.kind == CS_OPTION && e.page == 2 && e.bit_lo == 6 && e.bit_hi == 7 && e.n_values == 4);
    e = entry(TEMPLATE, 23); CHECK(e.kind == CS_TRIGGER && e.bit_lo == 0 && teq(e.name, "Reset"));
    e = entry(TEMPLATE, 24); CHECK(e.kind == CS_RESET && e.bit_lo == 0 && teq(e.name, "Reset and close OSD"));

    /* v,0 */
    {
        int found = 0;
        for (int i = 2; i < cs_count(TEMPLATE); i++) { e = entry(TEMPLATE, i);   /* page entries start at 2 (menu.cpp 1936) */ if (e.kind == CS_CFGVER) { found = 1; CHECK(teq(e.name, "0")); } }
        CHECK(found);
    }

    /* ---- hide / disable against the menumask (Template wires menumask = {status[5]}) ---- */
    {
        int h, d;
        e = entry(TEMPLATE, 12);                   /* d0: greyed when bit 0 is NOT set */
        cs_visibility(&e, 0x0000, &h, &d); CHECK(!h && d);
        cs_visibility(&e, 0x0001, &h, &d); CHECK(!h && !d);
        e = entry(TEMPLATE, 13);                   /* H0: hidden when bit 0 IS set */
        cs_visibility(&e, 0x0000, &h, &d); CHECK(!h && !d);
        cs_visibility(&e, 0x0001, &h, &d); CHECK(h && !d);
    }

    /* ---- the status word ---- */
    {
        uint8_t st[16] = {0};
        cs_status_set(st, 121, 122, 2);  CHECK(cs_status_get(st, 121, 122) == 2);
        cs_status_set(st, 3, 4, 3);      CHECK(cs_status_get(st, 3, 4) == 3 && st[0] == 0x18);
        cs_status_set(st, 6, 9, 0xF);    CHECK(cs_status_get(st, 6, 9) == 0xF && st[0] == 0xD8 && st[1] == 0x03);
        cs_status_set(st, 3, 4, 7);      CHECK(cs_status_get(st, 3, 4) == 3);   /* masked to 2 bits */
    }

    /* ---- the menu core's own string ---- */
    CHECK(cs_get(MENU, 0, &t) && teq(t, "MENU"));
    CHECK(cs_count(MENU) == 4);
    e = entry(MENU, 1); CHECK(e.kind == CS_NONE);          /* "UART31250,MIDI": not a page entry */
    e = entry(MENU, 2); CHECK(e.kind == CS_TEXT);          /* "-  " */
    e = entry(MENU, 3); CHECK(e.kind == CS_VERSION && teq(e.name, "v250928"));

    /* ---- non-page entries ---- */
    { cs_text_t x = { "J1,A,B,Select,Start", 19 }; CHECK(cs_parse(x, &e) && e.kind == CS_BUTTONS); }
    { cs_text_t x = { "jn,A,B,Select,Start", 19 }; CHECK(cs_parse(x, &e) && e.kind == CS_JN); }
    { cs_text_t x = { "DIP", 3 };                  CHECK(cs_parse(x, &e) && e.kind == CS_DIP); }
    { cs_text_t x = { "C,Cheats", 8 };             CHECK(cs_parse(x, &e) && e.kind == CS_CHEATS && teq(e.name, "Cheats")); }
    { cs_text_t x = { "FS1,SAV,Save RAM", 16 };    CHECK(cs_parse(x, &e) && e.kind == CS_FILE && e.f_save && e.index == 1 && teq(e.name, "Save RAM")); }
    { cs_text_t x = { "d5P1o2,Vertical Crop,Disabled,216p(5x)", 39 };
      CHECK(cs_parse(x, &e) && e.kind == CS_OPTION && e.page == 1 && e.bit_lo == 34 && e.n_hd == 1 && e.hd[0].bit == 5); }

    printf("%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
