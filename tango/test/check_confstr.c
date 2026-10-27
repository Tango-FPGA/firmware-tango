// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 the Tango authors
/*
 * check_confstr.c -- a core's CONF_STR through the FIRMWARE's own menu code (confstr.c + tango_coremenu.c +
 * tango_browser.c, host-built with fake_io), to prove before any build that the firmware and the core agree:
 *   - every entry, as the firmware parses it (page, kind, status bits, F index / extensions);
 *   - every option: A on it in the real in-core OSD changes exactly its status bits (O[hi:lo] += 1), and
 *     T/R pulse exactly their bit;
 *   - every file you name: the firmware's browser finds it and the SDC IMAGE index it sends (MiSTer's
 *     ioctl_index: F digit or menu position + 1, plus 64 x the extension's position, one byte on the link)
 *     -- compared with what the core accepts when you give it.
 * Tango, our own code.  Driven by menu-tango tools/check_confstr.py (CONF_STR taken from the core's RTL).
 * usage: check_confstr <confstr.txt> [menumask=0xNNNN] [file=NAME[=0xII[/0xMM]]] ...
 *   file=pitfall.a26=0x01/0x3F   the index the firmware sends for pitfall.a26 must have (idx & 0x3F) == 0x01
 * Exit 0 = every check passed.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../tango_launcher.h"
#include "../tango_coremenu.h"
#include "../confstr.h"
#include "fake_io.h"

static int fails, checks;
#define FAIL(...) do { fails++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } while (0)
#define OK(...)   do { printf("ok:   "); printf(__VA_ARGS__); printf("\n"); } while (0)

static const char *card[64]; static int ncard;
static char card_store[64][400];
static int loads; static uint8_t load_idx; static char load_path[600];
static int p_load_core(void *c, const char *p) { (void)c; (void)p; return 0; }
static void p_reboot(void *c, int cold) { (void)c; (void)cold; }
static int p_load_file(void *c, const char *p, uint8_t idx) { (void)c; loads++; load_idx = idx; strncpy(load_path, p, sizeof load_path - 1); return 0; }
static int p_save(void *c, const char *n, const uint8_t st[16]) { (void)c; (void)n; (void)st; return 0; }
static int p_dir_exists(void *c, const char *path)
{
    (void)c; size_t l = strlen(path);
    for (int i = 0; i < ncard; i++) if (!strncmp(card[i], path, l) && card[i][l] == '/') return 1;
    return 0;
}

static tango_coremenu_t cm; static tango_osd_t osd; static tango_browser_t br;
static rec_link_t r; static tango_link_t link; static fake_card_t fc; static tango_fs_t fs;
static tango_core_platform_t plat;
static char conf[TANGO_CONF_MAX];

static void open_page0(void)
{
    if (cm.mode != TANGO_CM_CLOSED) tango_core_toggle(&cm);
    tango_core_toggle(&cm);                            /* Start+Select: page 0 */
}
/* move the cursor to the row showing CONF_STR entry e; returns 0 if it is not on the open page */
static int goto_entry(int e)
{
    int row = -1;
    for (int k = 0; k < cm.nrows; k++) if (cm.ent[k] == e) row = k;
    if (row < 0) return 0;
    for (int n = 0; n < 2 * TANGO_CM_ROWS && cm.sel != row; n++) tango_core_event(&cm, TANGO_EV_DOWN);
    return cm.sel == row;
}
/* open the page an entry lives on (page 0, or P# via its P#,Title link) */
static int open_page_of(const cs_entry_t *e)
{
    open_page0();
    if (!e->page) return 1;
    int n = cs_count(conf);
    for (int i = 2; i < n; i++) {
        cs_text_t t; cs_entry_t pe; cs_get(conf, i, &t);
        if (cs_parse(t, &pe) && pe.kind == CS_PAGE && pe.target_page == e->page && !pe.page) {
            if (!goto_entry(i)) return 0;
            tango_core_event(&cm, TANGO_EV_SELECT);
            return cm.page == e->page;
        }
    }
    return 0;
}
static const char *kind_name(cs_kind_t k)
{
    switch (k) {
    case CS_OPTION: return "option"; case CS_TRIGGER: return "trigger"; case CS_RESET: return "reset";
    case CS_PAGE: return "page"; case CS_FILE: return "file"; case CS_MOUNT: return "mount";
    case CS_TEXT: return "text"; case CS_VERSION: return "version"; default: return "other";
    }
}

int main(int argc, char **argv)
{
    if (argc < 2) { printf("usage: %s confstr.txt [menumask=0xNNNN] [file=NAME[=0xII[/0xMM]]]...\n", argv[0]); return 2; }
    FILE *f = fopen(argv[1], "rb"); if (!f) { printf("cannot open %s\n", argv[1]); return 2; }
    size_t n = fread(conf, 1, sizeof conf - 1, f); fclose(f); conf[n] = 0;
    while (n && (conf[n - 1] == '\n' || conf[n - 1] == '\r')) conf[--n] = 0;
    uint16_t mask = 0;
    for (int i = 2; i < argc; i++) if (!strncmp(argv[i], "menumask=", 9)) mask = (uint16_t)strtoul(argv[i] + 9, NULL, 0);

    /* the card: games/<core>/ with the named files */
    char name[64]; { cs_text_t t; cs_get(conf, 0, &t); int l = t.len < 63 ? t.len : 63; memcpy(name, t.p, (size_t)l); name[l] = 0; }
    card[ncard++] = "games/";
    snprintf(card_store[0], sizeof card_store[0], "games/%s/", name); card[ncard++] = card_store[0];
    for (int i = 2; i < argc && ncard < 60; i++) if (!strncmp(argv[i], "file=", 5)) {
        char fn[256]; strncpy(fn, argv[i] + 5, sizeof fn - 1); fn[sizeof fn - 1] = 0; char *eq = strchr(fn, '='); if (eq) *eq = 0;
        snprintf(card_store[ncard], sizeof card_store[0], "games/%s/%s", name, fn); card[ncard] = card_store[ncard]; ncard++;
    }
    rec_link_init(&r, &link); r.confstr = conf; r.menumask = mask; r.dl_space = 2048;
    fake_card_init(&fc, &fs, card, ncard);
    memset(&plat, 0, sizeof plat);      /* designated fields: the struct grows with the firmware */
    plat.fs = &fs; plat.load_core = p_load_core; plat.reboot = p_reboot; plat.load_file = p_load_file;
    plat.save_cfg = p_save; plat.dir_exists = p_dir_exists; plat.ctx = NULL;
    static const uint8_t PAL[16][3] = TANGO_PAL_INIT;
    static char got[TANGO_CONF_MAX];
    int gl = tango_sys_read_confstr(&link, got, sizeof got);
    if (gl != (int)strlen(conf) || strcmp(got, conf)) FAIL("READ_CFG through the firmware's proto call differs");
    if (!tango_osd_probe(&osd, &link)) { FAIL("OSD probe"); return 1; }
    tango_osd_start(&osd, PAL);
    tango_core_init(&cm, &link, &osd, &br, &plat, got, NULL);
    printf("core \"%s\", CONF_STR %d bytes, %d entries, menumask %04X, config file %s\n", cm.name, (int)strlen(conf),
           cs_count(conf), mask, cm.cfgname);

    /* 1. every entry, parsed; every option / trigger through the real OSD */
    int ne = cs_count(conf);
    for (int i = 2; i < ne; i++) {
        cs_text_t t; cs_entry_t e; cs_get(conf, i, &t);
        if (!cs_parse(t, &e)) { FAIL("entry %d \"%.*s\" is malformed for the firmware", i, t.len, t.p); continue; }
        char lab[64]; int l = e.name.len < 63 ? e.name.len : 63; memcpy(lab, e.name.p, (size_t)l); lab[l] = 0;
        if (e.kind == CS_OPTION || e.kind == CS_TRIGGER || e.kind == CS_RESET) {
            printf("      %2d P%d %-7s O[%d:%d] \"%s\"", i, e.page, kind_name(e.kind), e.bit_hi, e.bit_lo, lab);
            if (e.kind == CS_OPTION) printf(" (%d values)", e.n_values);
            printf("\n");
            int hid, dis; cs_visibility(&e, mask, &hid, &dis);
            if (hid || dis) { printf("        (hidden or greyed with menumask %04X: not exercised)\n", mask); continue; }
            if (!open_page_of(&e) || !goto_entry(i)) { FAIL("entry %d \"%s\": not reachable in the OSD", i, lab); continue; }
            uint8_t before[16]; memcpy(before, r.status, 16);
            int w0 = r.status_writes;
            tango_core_event(&cm, TANGO_EV_SELECT);
            if (e.kind == CS_OPTION) {
                uint8_t want[16]; memcpy(want, before, 16);
                uint32_t v = cs_status_get(before, e.bit_lo, e.bit_hi) + 1; if ((int)v >= e.n_values) v = 0;
                cs_status_set(want, e.bit_lo, e.bit_hi, v);
                if (r.status_writes == w0 || memcmp(want, r.status, 16)) FAIL("option \"%s\": the status word the core got is not O[%d:%d] = %u", lab, e.bit_hi, e.bit_lo, v);
                else OK("option \"%s\": A -> the core gets O[%d:%d] = %u, no other bit changes", lab, e.bit_hi, e.bit_lo, v);
                tango_core_event(&cm, TANGO_EV_SELECT);       /* cycle it back to 0 */
                while (cs_status_get(r.status, e.bit_lo, e.bit_hi) != cs_status_get(before, e.bit_lo, e.bit_hi) && e.n_values > 1)
                    { if (!goto_entry(i)) break; tango_core_event(&cm, TANGO_EV_SELECT); }
            } else {
                if (r.status_writes < w0 + 2 || memcmp(before, r.status, 16)) FAIL("%s \"%s\": no pulse of O[%d:%d]", kind_name(e.kind), lab, e.bit_hi, e.bit_lo);
                else OK("%s \"%s\": pulses O[%d:%d] (1 then 0)", kind_name(e.kind), lab, e.bit_hi, e.bit_lo);
            }
        } else if (e.kind == CS_FILE) {
            char ext[64]; int el = e.ext.len < 63 ? e.ext.len : 63; memcpy(ext, e.ext.p, (size_t)el); ext[el] = 0;
            printf("      %2d P%d file    F%d exts \"%s\" \"%s\"\n", i, e.page, e.index, ext, lab);
        } else if (e.kind == CS_PAGE) printf("      %2d P%d page -> %d \"%s\"\n", i, e.page, e.target_page, lab);
        else if (e.kind == CS_NONE) printf("      %2d (ignored by the firmware) \"%.*s\"\n", i, t.len, t.p);
    }

    /* 2. files: the index the firmware sends, through its own browser */
    for (int a = 2; a < argc; a++) {
        if (strncmp(argv[a], "file=", 5)) continue;
        char fn[256]; strncpy(fn, argv[a] + 5, sizeof fn - 1); fn[sizeof fn - 1] = 0;
        unsigned want = 0, wmask = 0; int have_want = 0;
        char *eq = strchr(fn, '='); if (eq) { *eq = 0; have_want = 1; char *sl = strchr(eq + 1, '/'); want = (unsigned)strtoul(eq + 1, NULL, 0); wmask = sl ? (unsigned)strtoul(sl + 1, NULL, 0) : 0xFF; }
        int found = 0;
        for (int i = 2; i < ne && !found; i++) {
            cs_text_t t; cs_entry_t e; cs_get(conf, i, &t);
            if (!cs_parse(t, &e) || e.kind != CS_FILE) continue;
            char ext[64]; int el = e.ext.len < 63 ? e.ext.len : 63; memcpy(ext, e.ext.p, (size_t)el); ext[el] = 0;
            while (strlen(ext) % 3) strcat(ext, " ");
            if (!tango_ext_match(fn, ext)) continue;
            found = 1;
            if (!open_page_of(&e) || !goto_entry(i)) { FAIL("file entry %d not reachable in the OSD", i); break; }
            tango_core_event(&cm, TANGO_EV_SELECT);
            if (cm.mode != TANGO_CM_FILES) { FAIL("file entry %d: A did not open the browser", i); break; }
            tango_page_t p; int hit = -1;
            char base[256]; strncpy(base, fn, sizeof base - 1); base[sizeof base - 1] = 0; char *dot = strrchr(base, '.'); if (dot) *dot = 0;
            for (int k = 0; k < 64 && hit < 0; k++) {
                tango_core_page(&cm, &p);
                if (p.sel < p.nrows && p.rows[p.sel].label && (!strcmp(p.rows[p.sel].label, base) || !strcmp(p.rows[p.sel].label, fn))) hit = p.sel;
                else tango_core_event(&cm, TANGO_EV_DOWN);
            }
            if (hit < 0) { FAIL("the browser in games/%s does not list %s", name, fn); break; }
            loads = 0; tango_core_event(&cm, TANGO_EV_SELECT);
            if (loads != 1) { FAIL("A on %s did not start a download", fn); break; }
            printf("      %s: F entry %d -> SDC IMAGE index 0x%02X (ioctl_index[5:0] = %u, extension #%u) from %s\n",
                   fn, i, load_idx, load_idx & 0x3F, load_idx >> 6, load_path);
            if (have_want) {
                if ((load_idx & wmask) == (want & wmask)) OK("%s: index 0x%02X & 0x%02X = 0x%02X, what the core accepts", fn, load_idx, wmask, want & wmask);
                else FAIL("%s: the firmware sends index 0x%02X, the core accepts (idx & 0x%02X) == 0x%02X", fn, load_idx, wmask, want & wmask);
            }
        }
        if (!found) FAIL("no F entry of the CONF_STR takes %s", fn);
    }
    printf("%s: %d failure(s)\n", fails ? "CHECK_CONFSTR FAIL" : "CHECK_CONFSTR PASS", fails);
    (void)checks;
    return fails ? 1 : 0;
}
