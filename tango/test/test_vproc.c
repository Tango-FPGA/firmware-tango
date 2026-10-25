// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 the Tango authors
/*
 * test_vproc.c -- host tests of MiSTer's video / audio processing in the firmware (tango_vproc.c and the core menu's
 * Video processing page / Audio filter rows), on the real files of a folder of MiSTer's filter files. Tango, our own code.
 *   ./test_vproc [<filters folder>]
 * The parsers' transfers are compared with sys-tango's vproc_preset.py by vproc_crosscheck.py (make vproc); here:
 * the core-start sequence (probes, Tango.ini defaults, saved state), the menu's actions and what they send and save,
 * the scanline poll (set_vfilter(0)), presets, and a core without the blocks.
 * Memory: every allocation of tango_vproc.c is counted here (VP_MALLOC / VP_FREE):
 * nothing leaks, the peak stays small, a refused allocation is survived, and a core without the blocks allocates
 * nothing.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>
#include "../tango_coremenu.h"
#include "../tango_vproc.h"
#include "fake_io.h"

static int fails = 0, checks = 0;

/* tango_vproc.c's allocations (built with -DVP_MALLOC=t_vp_malloc -DVP_FREE=t_vp_free): live bytes, the peak, the
 * count, and a budget beyond which they are refused (-1: none) */
static long vp_live = 0, vp_peak = 0, vp_budget = -1; static int vp_nalloc = 0, vp_refused = 0;
void *t_vp_malloc(size_t n)
{
    if (vp_budget >= 0 && vp_live + (long)n > vp_budget) { vp_refused++; return NULL; }
    size_t *p = malloc(n + 2 * sizeof(size_t));
    if (!p) return NULL;
    p[0] = n; vp_live += (long)n; vp_nalloc++;
    if (vp_live > vp_peak) vp_peak = vp_live;
    return p + 2;
}
void t_vp_free(void *q)
{
    if (!q) return;
    size_t *p = (size_t *)q - 2;
    vp_live -= (long)p[0];
    free(p);
}
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)

static char root[1024];
/* the card: filters/ gamma/ shadow_masks/ filters_audio/ presets/ map to the filter folder's subtrees */
static int map_path(const char *path, char *out, int cap)
{
    static const struct { const char *card, *repo; } M[] = {
        { "filters_audio/", "Filters_MiSTer/Filters_Audio/" }, { "filters/", "Filters_MiSTer/Filters/" },
        { "gamma/", "Filters_MiSTer/Gamma/" }, { "shadow_masks/", "ShadowMasks_MiSTer/Shadow_Masks/" },
        { "presets/", "Presets_MiSTer/Presets/" } };
    for (int i = 0; i < 5; i++) {
        size_t l = strlen(M[i].card);
        if (!strncmp(path, M[i].card, l)) { snprintf(out, (size_t)cap, "%s/%s%s", root, M[i].repo, path + l); return 1; }
    }
    snprintf(out, (size_t)cap, "%s/%s", root, path);
    return 0;
}
static const char *ini_text = "";
static int t_read(void *c, const char *path, uint8_t *buf, int cap)
{
    (void)c;
    if (!strcmp(path, "Tango.ini")) {
        int n = (int)strlen(ini_text); if (!buf) return n;
        if (n > cap) n = cap;
        memcpy(buf, ini_text, (size_t)n); return n;
    }
    char full[2200]; struct stat sb; map_path(path, full, sizeof full);
    if (stat(full, &sb) || S_ISDIR(sb.st_mode)) return -1;
    if (!buf) return (int)sb.st_size;                     /* the size (vp_load) */
    FILE *f = fopen(full, "rb"); if (!f) return -1;
    int n = (int)fread(buf, 1, (size_t)cap, f); fclose(f); return n;
}
/* config/ in memory */
static struct { char name[64]; char data[4096]; int n; } cfgs[8]; static int ncfg = 0, saves = 0;
static int t_save(void *c, const char *name, const void *d, int n)
{
    (void)c; saves++;
    for (int i = 0; i < ncfg; i++) if (!strcmp(cfgs[i].name, name)) { memcpy(cfgs[i].data, d, (size_t)n); cfgs[i].n = n; return 0; }
    if (ncfg < 8) { snprintf(cfgs[ncfg].name, 64, "%s", name); memcpy(cfgs[ncfg].data, d, (size_t)n); cfgs[ncfg].n = n; ncfg++; }
    return 0;
}
static int t_load(void *c, const char *name, void *d, int n)
{
    (void)c;
    for (int i = 0; i < ncfg; i++) if (!strcmp(cfgs[i].name, name) && cfgs[i].n) { int k = cfgs[i].n < n ? cfgs[i].n : n; memcpy(d, cfgs[i].data, (size_t)k); return k; }
    return -1;
}
static const vp_io_t IO = { t_read, t_save, t_load, NULL };

/* transfers recorded by the fake link: count the SYS commands */
static FILE *rec; static char recpath[256];
static void rec_start(rec_link_t *r) { if (r->out) fclose(r->out); r->out = fopen(recpath, "wb"); }
static int rec_count(rec_link_t *r, int cmd, int *bytes)
{
    fflush(r->out);
    FILE *f = fopen(recpath, "rb"); int n = 0, b = 0, c0, c1;
    while ((c0 = fgetc(f)) >= 0 && (c1 = fgetc(f)) >= 0) {
        int len = c0 | c1 << 8; uint8_t x[4096]; if (len > 4096) len = 4096;
        if ((int)fread(x, 1, (size_t)len, f) != len) break;
        if (len >= 2 && x[0] == 0 && x[1] == cmd) { n++; b += len; }
    }
    fclose(f);
    if (bytes) *bytes = b;
    return n;
}
/* the first data byte (x[2]) of the n-th recorded transfer of a SYS command, -1 if there is none */
static int rec_arg(rec_link_t *r, int cmd, int nth)
{
    fflush(r->out);
    FILE *f = fopen(recpath, "rb"); int c0, c1, res = -1;
    while ((c0 = fgetc(f)) >= 0 && (c1 = fgetc(f)) >= 0) {
        int len = c0 | c1 << 8; uint8_t x[4096]; if (len > 4096) len = 4096;
        if ((int)fread(x, 1, (size_t)len, f) != len) break;
        if (len >= 3 && x[0] == 0 && x[1] == cmd && nth-- == 0) { res = x[2]; break; }
    }
    fclose(f);
    return res;
}
static int cfg_has(const char *name, const char *needle)
{
    for (int i = 0; i < ncfg; i++) if (!strcmp(cfgs[i].name, name)) {
        for (int k = 0; k + (int)strlen(needle) <= cfgs[i].n; k++) if (!memcmp(cfgs[i].data + k, needle, strlen(needle))) return 1;
    }
    return 0;
}

static const char *CARD[] = { "filters/", "filters/Interpolation (Sharp).txt", "filters/Scanlines - Default.txt",
                              "gamma/", "gamma/Pure_Gamma/", "gamma/Pure_Gamma/gamma_150.txt", "shadow_masks/",
                              "shadow_masks/JVC D-Series (~2000).txt", "presets/", "presets/Scanlines - Soft.ini",
                              "filters_audio/", "filters_audio/General LPF/", "filters_audio/General LPF/LPF 20khz 3rd BW.txt" };
static int t_dir_exists(void *c, const char *p)
{
    (void)c; size_t l = strlen(p);
    for (unsigned i = 0; i < sizeof CARD / sizeof CARD[0]; i++) if (!strncmp(CARD[i], p, l) && CARD[i][l] == '/') return 1;
    return 0;
}
/* the real card for the minus / plus tests: the folders of the filter files through map_path (readdir order, as FatFs) */
static DIR *rd_dir; static char rd_base[2200];
static int rd_open(void *c, const char *path)
{
    (void)c; char p[2200]; snprintf(p, sizeof p, "%s/", path); map_path(p, rd_base, sizeof rd_base);
    rd_dir = opendir(rd_base); return rd_dir != NULL;
}
static int rd_read(void *c, char *name, int cap, int *is_dir)
{
    (void)c; struct dirent *d;
    while ((d = readdir(rd_dir))) {
        if (!strcmp(d->d_name, ".")) continue;
        char full[4400]; struct stat sb; snprintf(full, sizeof full, "%s%s", rd_base, d->d_name);
        if (stat(full, &sb)) continue;
        snprintf(name, (size_t)cap, "%s", d->d_name); *is_dir = S_ISDIR(sb.st_mode); return 1;
    }
    return 0;
}
static void rd_close(void *c) { (void)c; if (rd_dir) closedir(rd_dir); rd_dir = NULL; }

static int row_with(tango_coremenu_t *cm, const char *label)
{
    for (int i = 0; i < cm->nrows; i++) if (cm->rows[i].label && !strcmp(cm->rows[i].label, label)) return i;
    return -1;
}
static void go_row(tango_coremenu_t *cm, const char *label) { int r = row_with(cm, label); CHECK(r >= 0); if (r >= 0) cm->sel = r; }
/* in the file browser: select the entry with this name and press A */
static int pick(tango_coremenu_t *cm, const char *name)
{
    tango_browser_t *b = cm->br;
    for (int i = 0; i < b->n; i++) {
        tango_browser_select(b, i);
        const tango_dirent_t *e = tango_browser_cur(b);
        if (e && !strcmp(tango_browser_name(b, e), name)) { tango_core_event(cm, TANGO_EV_SELECT); return 1; }
    }
    return 0;
}

static tango_vproc_t V;
int main(int argc, char **argv)
{
    const char *fenv = getenv("FILTERS");                    /* the folder of MiSTer's filter files: argv[1], else FILTERS */
    if (argc > 1) snprintf(root, sizeof root, "%s", argv[1]);
    else if (fenv && *fenv) snprintf(root, sizeof root, "%s", fenv);
    else { fprintf(stderr, "usage: test_vproc <folder> (or FILTERS=<folder>): MiSTer's Filters_MiSTer, Gamma, Shadow_Masks, "
                           "Filters_Audio and Presets folders\n"); return 2; }
    snprintf(recpath, sizeof recpath, "/tmp/test_vproc_%d.bin", (int)getpid());
    (void)rec;
    static rec_link_t r; tango_link_t link; rec_link_init(&r, &link);
    r.vp_has = 1; r.vp_flags = 0x03;                        /* the blocks are there; scanline level 0 */

    /* ---- core start with Tango.ini defaults: [MiSTer] keys, then the core's section wins */
    ini_text = "[MiSTer]\nVFILTER_DEFAULT=Interpolation (Sharp).txt\nVFILTER_SCANLINES_DEFAULT=Scanlines - Default.txt ; comment\n"
               "SHMASK_DEFAULT=JVC D-Series (~2000).txt\nSHMASK_MODE_DEFAULT=1\n[Template]\nAFILTER_DEFAULT=General LPF/LPF 20khz 3rd BW.txt\n"
               "[NES]\nVFILTER_DEFAULT=nope.txt\n";
    rec_start(&r);
    vp_core_start(&V, &link, &IO, "Template", (const uint8_t *)ini_text, (int)strlen(ini_text));
    CHECK(V.has_scaler && V.has_gamma && V.has_mask && V.has_afilter && !V.has_boost);
    CHECK(V.flt[VP_HORZ].mode == 1 && !strcmp(V.flt[VP_HORZ].name, "Interpolation (Sharp).txt"));
    CHECK(V.flt[VP_SCAN].mode == 1 && !strcmp(V.flt[VP_SCAN].name, "Scanlines - Default.txt"));
    CHECK(V.mask[0] == 1 && !strcmp(V.mask + 1, "JVC D-Series (~2000).txt"));
    CHECK(V.afilt[0] == 1 && !strcmp(V.afilt + 1, "General LPF/LPF 20khz 3rd BW.txt"));
    int b;
    CHECK(rec_count(&r, 0x2B, NULL) == 3);                  /* probe + probe in vp_send_scaler + the mode */
    CHECK(rec_count(&r, 0x2A, &b) == 2 && b == 2 * (4 + 64 * 8));   /* H and V sets (64 phases, V = H at level 0) */
    CHECK(rec_count(&r, 0x3E, &b) == 2);                    /* probe + the mask */
    CHECK(rec_count(&r, 0x39, &b) == 2 && b == 4 + 3 + 21);  /* probe (00 39 vol 00) + vol and the file's 21 bytes */
    CHECK(rec_count(&r, 0x33, NULL) == 0);                  /* no gamma default in MiSTer.ini */
    CHECK(V.vset == VP_HORZ);

    /* ---- the poll: the core's scanlines go on -> the Scan filter is sent into the V set */
    rec_start(&r); r.vp_flags = 0x23;                      /* level 2 */
    CHECK(vp_poll(&V) == 1); CHECK(V.vset == VP_SCAN);
    CHECK(rec_count(&r, 0x2A, NULL) == 2);
    rec_start(&r); CHECK(vp_poll(&V) == 0); CHECK(rec_count(&r, 0x2A, NULL) == 0);   /* no change: nothing more */
    r.vp_flags = 0x03; CHECK(vp_poll(&V) == 1 && V.vset == VP_HORZ);

    /* ---- the menu */
    static tango_coremenu_t cm; static tango_osd_t osd; static tango_browser_t br;
    fake_card_t card; tango_fs_t fs; fake_card_init(&card, &fs, CARD, (int)(sizeof CARD / sizeof CARD[0]));
    tango_core_platform_t plat = { .fs = &fs, .dir_exists = t_dir_exists };   /* (designated: the struct also has
                                                                                 * zipfs / load_zip_file) */
    r.confstr = "Template;;O[2],TV Mode,NTSC,PAL;V,v1;";
    tango_osd_probe(&osd, &link);
    tango_core_init(&cm, &link, &osd, &br, &plat, r.confstr, NULL);
    cm.vp = &V;
    tango_core_toggle(&cm);
    tango_core_event(&cm, TANGO_EV_RIGHT); CHECK(cm.mode == TANGO_CM_SYSTEM);
    CHECK(row_with(&cm, "Audio filter") >= 0); CHECK(row_with(&cm, "LPF 20khz 3rd BW.txt") >= 0);
    go_row(&cm, "Video processing"); tango_core_event(&cm, TANGO_EV_SELECT);
    CHECK(cm.mode == TANGO_CM_VIDEO);
    CHECK(row_with(&cm, "Horz filter") >= 0 && row_with(&cm, "Interpolation (Sharp).txt") >= 0);
    CHECK(row_with(&cm, "Scan filter") >= 0 && row_with(&cm, "Scanlines - Default.txt") >= 0);
    { int k = row_with(&cm, "Vert filter"); CHECK(k >= 0 && !strcmp(cm.rows[k].value, "Same as Horz")); }
    { int k = row_with(&cm, "Shadow Mask"); CHECK(k >= 0 && !strcmp(cm.rows[k].value, "1x")); }
    { int k = row_with(&cm, "Gamma correction"); CHECK(k >= 0 && !strcmp(cm.rows[k].value, "Off")); }
    /* Horz filter off: NearNeighbour, FLTNUM mode 0 sent, the state saved */
    rec_start(&r); saves = 0;
    go_row(&cm, "Horz filter"); tango_core_event(&cm, TANGO_EV_SELECT);
    CHECK(V.flt[VP_HORZ].mode == 0); CHECK(row_with(&cm, "Video filter") >= 0); CHECK(saves == 1);
    CHECK(rec_count(&r, 0x2B, NULL) == 2);
    go_row(&cm, "Video filter"); tango_core_event(&cm, TANGO_EV_SELECT); CHECK(V.flt[VP_HORZ].mode == 1);
    /* gamma on, then a curve picked in the browser: GAMCURV + SET_GAMMA, saved as <core>_gamma.cfg */
    go_row(&cm, "Gamma correction"); tango_core_event(&cm, TANGO_EV_SELECT); CHECK(V.gamma[0] == 1);
    go_row(&cm, "< none >");                                   /* the gamma file row (the first "< none >": gamma's) */
    { int k = row_with(&cm, "Gamma correction"); cm.sel = k + 1; }
    tango_core_event(&cm, TANGO_EV_SELECT);
    CHECK(cm.mode == TANGO_CM_FILES && cm.vp_pick == VPICK_GAMMA);
    /* the browser shows gamma/: Pure_Gamma (folder) -> gamma_150.txt */
    rec_start(&r);
    CHECK(pick(&cm, "Pure_Gamma"));                          /* the folder, then the file */
    CHECK(pick(&cm, "gamma_150.txt"));
    CHECK(cm.mode == TANGO_CM_VIDEO);
    CHECK(!strcmp(V.gamma + 1, "Pure_Gamma/gamma_150.txt"));
    CHECK(rec_count(&r, 0x33, &b) == 1 && b == 3 + 768); CHECK(rec_count(&r, 0x32, NULL) == 1);
    CHECK(cfg_has("Template_gamma.cfg", "Pure_Gamma/gamma_150.txt"));
    /* mask mode: A cycles, minus / plus too (menu.cpp 3292-3295); 5 values wrap (video.cpp 955-962) */
    go_row(&cm, "Shadow Mask");
    tango_core_event(&cm, TANGO_EV_SELECT); CHECK(V.mask[0] == 2);
    tango_core_event(&cm, TANGO_EV_PLUS);   CHECK(V.mask[0] == 3);
    tango_core_event(&cm, TANGO_EV_MINUS);  CHECK(V.mask[0] == 2);
    tango_core_event(&cm, TANGO_EV_RIGHT);  CHECK(V.mask[0] == 2 && cm.mode == TANGO_CM_VIDEO);   /* Right: nothing */
    tango_core_event(&cm, TANGO_EV_LEFT);   CHECK(cm.mode == TANGO_CM_SYSTEM);   /* Left: back to System (3274-3279) */
    go_row(&cm, "Video processing"); tango_core_event(&cm, TANGO_EV_SELECT); CHECK(cm.mode == TANGO_CM_VIDEO);
    go_row(&cm, "Shadow Mask");
    tango_core_event(&cm, TANGO_EV_SELECT); tango_core_event(&cm, TANGO_EV_SELECT); tango_core_event(&cm, TANGO_EV_SELECT);
    CHECK(V.mask[0] == 0);
    tango_core_event(&cm, TANGO_EV_MINUS);  CHECK(V.mask[0] == 4);   /* minus from None wraps to the last mode */
    tango_core_event(&cm, TANGO_EV_PLUS);   CHECK(V.mask[0] == 0);

    /* ---- minus / plus on the file rows: the folder's previous / next file (flist_GetPrevNext, file_io.cpp 2098-2116),
     * MiSTer's order (DirentComp), wrapping; the expected neighbours worked out separately from the real folders */
    {
        tango_fs_t rfs = { .open = rd_open, .read = rd_read, .close = rd_close };
        plat.fs = &rfs;
        rec_start(&r);
        { int k = row_with(&cm, "Gamma correction"); cm.sel = k + 1; }               /* gamma: Pure_Gamma/gamma_150 */
        tango_core_event(&cm, TANGO_EV_PLUS);  CHECK(!strcmp(V.gamma + 1, "Pure_Gamma/gamma_155.txt"));
        tango_core_event(&cm, TANGO_EV_MINUS); tango_core_event(&cm, TANGO_EV_MINUS);
        CHECK(!strcmp(V.gamma + 1, "Pure_Gamma/gamma_145.txt"));
        CHECK(rec_count(&r, 0x33, NULL) == 3); CHECK(cfg_has("Template_gamma.cfg", "Pure_Gamma/gamma_145.txt"));
        vp_set_gamma_curve(&V, "Pure_Gamma/gamma_200.txt");
        tango_core_event(&cm, TANGO_EV_PLUS);  CHECK(!strcmp(V.gamma + 1, "Pure_Gamma/gamma_110.txt"));   /* wraps */
        tango_core_event(&cm, TANGO_EV_MINUS); CHECK(!strcmp(V.gamma + 1, "Pure_Gamma/gamma_200.txt"));
        vp_set_gamma_curve(&V, "");                                                  /* no file: gamma/'s first file */
        tango_core_event(&cm, TANGO_EV_PLUS);  CHECK(!strcmp(V.gamma + 1, "CRT Simulation.txt"));
        vp_set_gamma_curve(&V, "Pure_Gamma/GAMMA_150.TXT");                          /* FAT: any case */
        tango_core_event(&cm, TANGO_EV_PLUS);  CHECK(!strcmp(V.gamma + 1, "Pure_Gamma/gamma_155.txt"));
        /* the Horz filter's file (filters/ root, 15 files): Sharp -> Soft / Medium */
        { int k = row_with(&cm, "Horz filter"); CHECK(k >= 0); cm.sel = k + 1; }
        CHECK(!strcmp(V.flt[VP_HORZ].name, "Interpolation (Sharp).txt"));
        rec_start(&r);
        tango_core_event(&cm, TANGO_EV_PLUS);  CHECK(!strcmp(V.flt[VP_HORZ].name, "Interpolation (Soft).txt"));
        tango_core_event(&cm, TANGO_EV_MINUS); tango_core_event(&cm, TANGO_EV_MINUS);
        CHECK(!strcmp(V.flt[VP_HORZ].name, "Interpolation (Medium).txt"));
        CHECK(rec_count(&r, 0x2A, NULL) >= 3); CHECK(cfg_has("Template_scaler.cfg", "Interpolation (Medium).txt"));
        /* the Vert filter's file row with Vert "Same as Horz" (mode 0): nothing (3305: both filters must be on) */
        { int k = row_with(&cm, "Vert filter"); cm.sel = k + 1; }
        { static char was[sizeof V.flt[0].name]; memcpy(was, V.flt[VP_VERT].name, sizeof was);
          CHECK(tango_core_event(&cm, TANGO_EV_PLUS) == TANGO_CM_NOTHING); CHECK(!strcmp(V.flt[VP_VERT].name, was)); }
        /* the mask file (mode 1x): JVC D-Series -> its neighbours; with mode None: nothing */
        go_row(&cm, "Shadow Mask"); tango_core_event(&cm, TANGO_EV_SELECT); CHECK(V.mask[0] == 1);
        vp_set_mask(&V, "JVC D-Series (~2000).txt");
        { int k = row_with(&cm, "Shadow Mask"); cm.sel = k + 1; }
        tango_core_event(&cm, TANGO_EV_PLUS);
        CHECK(!strcmp(V.mask + 1, "Lukac & Plataniotis CFA GRGB (Green) [Mirrored] (2004).txt"));
        tango_core_event(&cm, TANGO_EV_MINUS); tango_core_event(&cm, TANGO_EV_MINUS);
        CHECK(!strcmp(V.mask + 1, "Imaging Science & Technology CFA MYCK (Dark) [RGB] (2016).txt"));
        vp_set_mask_mode(&V, 0); tango_core_event(&cm, TANGO_EV_UP); tango_core_event(&cm, TANGO_EV_DOWN);
        { int k = row_with(&cm, "Shadow Mask"); cm.sel = k + 1; }
        CHECK(tango_core_event(&cm, TANGO_EV_PLUS) == TANGO_CM_NOTHING);
        /* the Audio filter's file on the System page (MENU_COMMON2 3172-3179), Custom only */
        tango_core_event(&cm, TANGO_EV_LEFT); CHECK(cm.mode == TANGO_CM_SYSTEM);
        vp_set_afilter_en(&V, 1); vp_set_afilter(&V, "General LPF/LPF 20khz 3rd BW.txt");
        tango_core_event(&cm, TANGO_EV_UP); tango_core_event(&cm, TANGO_EV_DOWN);   /* rebuild the rows */
        { int k = row_with(&cm, "Audio filter"); CHECK(k >= 0); cm.sel = k + 1; }
        rec_start(&r);
        tango_core_event(&cm, TANGO_EV_PLUS);  CHECK(!strcmp(V.afilt + 1, "General LPF/LPF 20khz 3rd CH 1db.txt"));
        tango_core_event(&cm, TANGO_EV_MINUS); tango_core_event(&cm, TANGO_EV_MINUS);
        CHECK(!strcmp(V.afilt + 1, "General LPF/LPF 20khz 2nd BW.txt"));
        CHECK(rec_count(&r, 0x39, NULL) == 3); CHECK(cfg_has("Template_afilter.cfg", "LPF 20khz 2nd BW.txt"));
        vp_set_afilter_en(&V, 0);
        { int k = row_with(&cm, "Audio filter"); cm.sel = k + 1; }
        CHECK(tango_core_event(&cm, TANGO_EV_PLUS) == TANGO_CM_NOTHING);
        vp_set_afilter_en(&V, 1); vp_set_afilter(&V, "General LPF/LPF 20khz 3rd BW.txt");   /* as before the block */
        plat.fs = &fs;
        go_row(&cm, "Video processing"); tango_core_event(&cm, TANGO_EV_SELECT); CHECK(cm.mode == TANGO_CM_VIDEO);
    }
    /* a preset: Scanlines - Soft (H GS_Sharpness_055, V adaptive, gamma off, mask off), saved */
    rec_start(&r);
    CHECK(vp_load_preset(&V, "presets/Scanlines - Soft.ini", 1) == 0);
    CHECK(!strcmp(V.flt[VP_HORZ].name, "Upscaling - Recommended/GS_Sharpness_055.txt"));
    CHECK(V.flt[VP_VERT].mode == 1 && V.adaptive[VP_VERT]);
    CHECK(rec_count(&r, 0x2A, NULL) == 3);                  /* H, V, V adaptive (set 3) */
    CHECK(cfg_has("Template_scaler.cfg", "GS_Sharpness_055.txt"));
    /* Reset to Defaults: the ini's defaults again */
    go_row(&cm, "Reset to Defaults"); tango_core_event(&cm, TANGO_EV_SELECT);
    CHECK(!strcmp(V.flt[VP_HORZ].name, "Interpolation (Sharp).txt") && V.flt[VP_VERT].mode == 0);
    /* back to System; Audio filter Custom -> Internal: SET_AFILTER with the volume byte alone */
    go_row(&cm, "back"); tango_core_event(&cm, TANGO_EV_SELECT); CHECK(cm.mode == TANGO_CM_SYSTEM);
    rec_start(&r);
    go_row(&cm, "Audio filter"); tango_core_event(&cm, TANGO_EV_SELECT);
    CHECK(V.afilt[0] == 0); CHECK(rec_count(&r, 0x39, &b) == 1 && b == 3);
    { int k = row_with(&cm, "Audio filter"); CHECK(k >= 0 && !strcmp(cm.rows[k].value, "Internal")); }

    /* ---- a core whose SET_AFILTER reply is 3 (sys-tango with the mix stage: boost works, sys_top 396): still the filter */
    { static tango_vproc_t W; r.vp_afreply = 3; rec_start(&r);
      vp_core_start(&W, &link, &IO, "Template", NULL, 0);
      CHECK(W.has_afilter && W.has_boost); r.vp_afreply = 0; }

    /* ---- memory: all of the above freed what it took; the peak is one file and two filter sets */
    CHECK(vp_live == 0);
    CHECK(vp_peak > 0 && vp_peak <= 24 * 1024);
    printf("test_vproc: tango_vproc.c took %d allocations, peak %ld bytes, all freed; a core's state is %u bytes\n",
           vp_nalloc, vp_peak, (unsigned)sizeof(tango_vproc_t));
    CHECK(sizeof(tango_vproc_t) <= 20 * 1024);
    /* ---- no memory at all: the core start survives and counts it; the sends that need none still go */
    { static tango_vproc_t W; ncfg = 0; vp_budget = 0; vp_refused = 0; rec_start(&r);   /* (no saved state: the ini's) */
      vp_core_start(&W, &link, &IO, "Template", (const uint8_t *)ini_text, (int)strlen(ini_text));
      CHECK(W.has_scaler && W.nomem == 3 && vp_refused == 3 && vp_live == 0);   /* the scaler's sets, mask, audio file */
      if (W.nomem != 3 || vp_refused != 3) printf("  nomem %d refused %d live %ld\n", W.nomem, vp_refused, vp_live);
      CHECK(rec_count(&r, 0x2B, NULL) == 2 && rec_count(&r, 0x2A, NULL) == 0);  /* probes only: the core keeps its sets */
      CHECK(rec_count(&r, 0x3E, NULL) == 2);                /* probe + the mask words as without its file */
      CHECK(rec_count(&r, 0x39, &b) == 2 && b == 4 + 3);    /* probe + the volume alone */
      vp_budget = -1; }

    /* ---- the core volume byte (tango_volume.h): the probe carries it, and so does the start's SET_AFILTER, as the caller set it
     * once it knew the core's boost (vp_caps_t.vol); a plain vp_core_start still sends 0 */
    { static tango_vproc_t W; vp_caps_t caps; r.vp_has = 1; r.vp_afreply = 3; rec_start(&r);
      CHECK(vp_probe(&link, 0x42, &caps) == 1 && caps.vol == 0x42 && caps.has_boost);
      caps.vol = 0x22;                                       /* (the caller: boost allowed or not) */
      vp_core_start_caps(&W, &link, &IO, "Template", (const uint8_t *)ini_text, (int)strlen(ini_text), &caps);
      CHECK(W.vol == 0x22 && rec_arg(&r, 0x39, 0) == 0x42 && rec_arg(&r, 0x39, 1) == 0x22);
      rec_start(&r); vp_core_start(&W, &link, &IO, "Template", (const uint8_t *)ini_text, (int)strlen(ini_text));
      CHECK(W.vol == 0 && rec_arg(&r, 0x39, 1) == 0);
      W.vol = 0x31; rec_start(&r); vp_send_afilter(&W); CHECK(rec_arg(&r, 0x39, 0) == 0x31);   /* a volume change re-sends it */
      r.vp_afreply = 0; }

    /* ---- native-rate output, SYS 0x70 VSYNC_ADJUST: Tango.ini's keys go to a core
     * that answers the probe, in one 9-byte transfer {00 70 apply|mode, P_min(3), P_max(3)}; nothing without the key */
    {
        struct { const char *ini; const char *core; int sends; uint8_t n0; uint32_t pmin, pmax; } T[] = {
            { "", "TG16", 0, 0, 0, 0 },                                                /* no key: the core's default */
            { "[TG16]\nVSYNC_ADJUST=1\n", "TG16", 1, 0x81, 0, 0 },
            { "[Tango]\nVSYNC_ADJUST=0\n", "TG16", 1, 0x80, 0, 0 },                  /* the global section */
            { "[MiSTer]\nVSYNC_ADJUST=2\n", "TG16", 1, 0x81, 0, 0 },                 /* 2 (single buffer) = native */
            { "[Tango]\nVSYNC_ADJUST=1\n[TG16]\nVSYNC_ADJUST=0\n", "TG16", 1, 0x80, 0, 0 },   /* later wins */
            { "[TG16]\nVSYNC_ADJUST=1\n", "NES", 0, 0, 0, 0 },                        /* another core's section */
            { "[TG16]\nvsync_adjust=1\nrefresh_min=50\nREFRESH_MAX=65.0\n", "TG16", 1, 0x81, 1142308, 1485000 },
            { "[Tango]\nVSYNC_ADJUST=1\n", "MENU", 0, 0, 0, 0 },                      /* never the launcher */
        };
        for (unsigned k = 0; k < sizeof T / sizeof T[0]; k++) {
            static tango_vproc_t W; ncfg = 0; r.vs_reply = 0xD0; ini_text = T[k].ini; rec_start(&r);
            vp_core_start(&W, &link, &IO, T[k].core, (const uint8_t *)ini_text, (int)strlen(ini_text));
            int bytes = 0, n = rec_count(&r, 0x70, &bytes);
            int ok = W.has_vsync && n == 1 + T[k].sends && bytes == 4 + 9 * T[k].sends;
            if (ok && T[k].sends) {                                  /* the send is the last 0x70 record */
                fflush(r.out); FILE *f = fopen(recpath, "rb"); int c0, c1; uint8_t x[16] = {0}, last[16] = {0};
                while ((c0 = fgetc(f)) >= 0 && (c1 = fgetc(f)) >= 0) {
                    int len = c0 | c1 << 8; uint8_t y[4096]; if (len > 4096) len = 4096;
                    if ((int)fread(y, 1, (size_t)len, f) != len) break;
                    if (len == 9 && y[0] == 0 && y[1] == 0x70) memcpy(last, y, 9);
                }
                fclose(f); memcpy(x, last, 9);
                uint32_t pmin = (uint32_t)x[3] << 16 | (uint32_t)x[4] << 8 | x[5], pmax = (uint32_t)x[6] << 16 | (uint32_t)x[7] << 8 | x[8];
                ok = x[2] == T[k].n0 && pmin == T[k].pmin && pmax == T[k].pmax;
                if (!ok) printf("  case %u: n0 %02x pmin %u pmax %u\n", k, x[2], pmin, pmax);
            }
            if (!ok) printf("  case %u: has_vsync %d, %d records of 0x70 (%d bytes)\n", k, W.has_vsync, n, bytes);
            CHECK(ok);
        }
        { static tango_vproc_t W; ini_text = "[TG16]\nVSYNC_ADJUST=1\n"; r.vs_reply = 0; rec_start(&r);   /* an older core */
          vp_core_start(&W, &link, &IO, "TG16", (const uint8_t *)ini_text, (int)strlen(ini_text));
          CHECK(!W.has_vsync && rec_count(&r, 0x70, NULL) == 1); }                   /* the probe, nothing else */
        { static tango_vproc_t W; r.vs_reply = 0xF0; rec_start(&r);
          vp_core_start(&W, &link, &IO, "TG16", NULL, 0);
          CHECK(vp_vsync_status(&W) == 0xF0 && rec_count(&r, 0x70, NULL) == 2); }   /* the status re-read is a probe */
        /* a retune-capable core (reply bit 1): after the apply the firmware waits 60 ms,
         * then probes 30 ms apart until bit 2 (retune pending) is clear -- here 2 pending probes, then the clear one */
        { static tango_vproc_t W; ini_text = "[TG16]\nVSYNC_ADJUST=0\n"; r.vs_reply = 0xD2; r.vs_pending = 2; rec_start(&r);
          r.delay_total = 0;
          vp_core_start(&W, &link, &IO, "TG16", (const uint8_t *)ini_text, (int)strlen(ini_text));
          int n = rec_count(&r, 0x70, NULL);
          if (n != 5 || r.delay_total != 60 + 2 * 30 || W.retune_timeouts)
              printf("  retune: %d records of 0x70 (want 5: probe, apply, 3 probes), delay %u ms (want 120), timeouts %d\n",
                     n, r.delay_total, W.retune_timeouts);
          CHECK(n == 5 && r.delay_total == 120 && W.retune_timeouts == 0 && W.vsync_status == 0xD2); }
        /* a retune that never finishes: 30 probes, then the firmware goes on (counted) */
        { static tango_vproc_t W; ini_text = "[TG16]\nVSYNC_ADJUST=0\n"; r.vs_reply = 0xD2; r.vs_pending = 1000; rec_start(&r);
          r.delay_total = 0;
          vp_core_start(&W, &link, &IO, "TG16", (const uint8_t *)ini_text, (int)strlen(ini_text));
          CHECK(rec_count(&r, 0x70, NULL) == 2 + 30 && W.retune_timeouts == 1 && r.delay_total == 60 + 30 * 30); }
        /* a core without the capability (bit 1 clear): no wait, no extra probe */
        { static tango_vproc_t W; ini_text = "[TG16]\nVSYNC_ADJUST=0\n"; r.vs_reply = 0xD0; r.vs_pending = 2; rec_start(&r);
          r.delay_total = 0;
          vp_core_start(&W, &link, &IO, "TG16", (const uint8_t *)ini_text, (int)strlen(ini_text));
          CHECK(rec_count(&r, 0x70, NULL) == 2 && r.delay_total == 0); }
        r.vs_pending = 0;
        ini_text = ""; r.vs_reply = 0;
    }

    /* ---- a core without the blocks: the four probes and nothing else -- no memory taken (the firmware's gate) */
    r.vp_has = 0; ncfg = 0;
    { int na = vp_nalloc; vp_caps_t caps; rec_start(&r);
      CHECK(vp_probe(&link, 0, &caps) == 0 && vp_nalloc == na);
      CHECK(rec_count(&r, 0x32, NULL) == 1 && rec_count(&r, 0x3E, NULL) == 1 && rec_count(&r, 0x39, NULL) == 1 &&
            rec_count(&r, 0x2B, NULL) == 1); }
    /* the module itself, should it be started anyway: nothing but the probes; no Audio filter rows; greyed rows */
    rec_start(&r);
    vp_core_start(&V, &link, &IO, "Bare", NULL, 0);
    CHECK(!V.has_scaler && !V.has_gamma && !V.has_mask && !V.has_afilter);
    CHECK(rec_count(&r, 0x2A, NULL) == 0 && rec_count(&r, 0x3E, NULL) == 1 && rec_count(&r, 0x39, NULL) == 1);
    tango_core_init(&cm, &link, &osd, &br, &plat, "Bare;;O[2],X,A,B;", NULL); cm.vp = &V;
    tango_core_toggle(&cm); tango_core_event(&cm, TANGO_EV_RIGHT);
    CHECK(row_with(&cm, "Audio filter") < 0);
    go_row(&cm, "Video processing"); tango_core_event(&cm, TANGO_EV_SELECT);
    { int k = row_with(&cm, "Video filter"); CHECK(k >= 0 && cm.rows[k].kind == TR_DIS); }

    if (r.out) fclose(r.out);
    remove(recpath);
    printf("test_vproc: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
