// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 the Tango authors
/*
 * vproc_stream.c -- the BL616 firmware's video/audio processing transfers (tango_vproc.c) for one set of settings,
 * written as a link stream: the same command line as sys-tango build_tools/vproc_preset.py --stream, so the two
 * implementations can be compared byte for byte (vproc_crosscheck.py). Tango.
 * The shadow-mask mode names (MM below) are Main_MiSTer's menu.cpp config_smask_msg (273, c3dc8ed; GPL-3.0; the header
 * of menu.cpp reads "Copyright 2005, 2006, 2007 Dennis van Weeren", "Copyright 2008, 2009 Jakub Bednarski"), lower case.
 *   vproc_stream --root <filters folder> [--preset <ini>] [--hfilter f] [--vfilter f] [--sfilter f] [--ifilter f]
 *                [--gamma f] [--mask f] [--maskmode m] [--afilter f] [--scanlines n] --stream <out.bin>
 * The card: <root> holds the filter files' subtrees (Filters_MiSTer/Filters, ...), or a make_sd_tree.sh layout.
 * Order as vproc_preset.py transfers(): gamma, FLTNUM + sets (for --scanlines), mask, audio filter; no probes.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <dirent.h>
#include <strings.h>
#include "../tango_vproc.h"
#include "fake_io.h"

static char root[1024];
static const char *map_dir(const char *first, int *len)
{
    static const struct { const char *card, *repo; } M[] = {
        { "filters_audio", "Filters_MiSTer/Filters_Audio" }, { "filters", "Filters_MiSTer/Filters" },
        { "gamma", "Filters_MiSTer/Gamma" }, { "shadow_masks", "ShadowMasks_MiSTer/Shadow_Masks" },
        { "presets", "Presets_MiSTer/Presets" } };
    for (int i = 0; i < 5; i++) {
        int l = (int)strlen(M[i].card);
        if (!strncmp(first, M[i].card, (size_t)l) && first[l] == '/') { *len = l; return M[i].repo; }
    }
    return NULL;
}
/* the card is FAT: names match case-insensitively (MiSTer's presets name "Poly_gamma/..." for Poly_Gamma) */
static int resolve_ci(const char *base, const char *rel, char *out, int cap)
{
    char cur[2300]; snprintf(cur, sizeof cur, "%s", base);
    char part[1024]; const char *p = rel;
    while (*p) {
        int n = 0; while (*p && *p != '/') { if (n < (int)sizeof part - 1) part[n++] = *p; p++; }
        part[n] = 0; if (*p == '/') p++;
        if (!n) continue;
        DIR *d = opendir(cur); if (!d) return -1;
        struct dirent *e; int found = 0;
        while ((e = readdir(d))) if (!strcasecmp(e->d_name, part)) { found = 1; break; }
        if (found) { size_t l = strlen(cur); snprintf(cur + l, sizeof cur - l, "/%s", e->d_name); }
        closedir(d);
        if (!found) return -1;
    }
    snprintf(out, (size_t)cap, "%s", cur);
    return 0;
}
static int st_read(void *ctx, const char *path, uint8_t *buf, int cap)
{
    (void)ctx;
    char full[2300], rel[2300]; struct stat sb;
    int l; const char *repo = map_dir(path, &l);
    char probe[1100]; snprintf(probe, sizeof probe, "%s/Filters_MiSTer", root);
    if (repo && !stat(probe, &sb)) snprintf(rel, sizeof rel, "%s%s", repo, path + l);
    else snprintf(rel, sizeof rel, "%s", path);
    if (resolve_ci(root, rel, full, sizeof full)) return -1;
    if (stat(full, &sb) || S_ISDIR(sb.st_mode)) return -1;
    if (!buf) return (int)sb.st_size;                    /* the size (vp_load) */
    FILE *f = fopen(full, "rb"); if (!f) return -1;
    int n = (int)fread(buf, 1, (size_t)cap, f); fclose(f);
    return n;
}

static tango_vproc_t V;
int main(int argc, char **argv)
{
    const char *out = NULL, *preset = NULL; int sl = 0;
    rec_link_t rl; tango_link_t link; rec_link_init(&rl, &link);
    vp_io_t io = { st_read, NULL, NULL, NULL };
    memset(&V, 0, sizeof V);
    V.link = &link; V.io = &io; V.height = 720; V.has_scaler = V.has_gamma = V.has_mask = V.has_afilter = 1;
    snprintf(root, sizeof root, "%s", getenv("FILTERS") ? getenv("FILTERS") : ".");   /* --root overrides */
    int gamma_set = 0;
    for (int i = 1; i + 1 < argc; i += 2) {
        const char *k = argv[i], *a = argv[i + 1];
        if (!strcmp(k, "--root")) snprintf(root, sizeof root, "%s", a);
        else if (!strcmp(k, "--stream")) out = a;
        else if (!strcmp(k, "--preset")) preset = a;
        else if (!strcmp(k, "--scanlines")) sl = atoi(a);
        else if (!strcmp(k, "--hfilter") || !strcmp(k, "--vfilter") || !strcmp(k, "--sfilter") || !strcmp(k, "--ifilter")) {
            int t = k[2] == 'h' ? VP_HORZ : k[2] == 'v' ? VP_VERT : k[2] == 's' ? VP_SCAN : VP_ILACE;
            if (!strcmp(a, "off") || !strcmp(a, "same")) V.flt[t].mode = 0;
            else { snprintf(V.flt[t].name, sizeof V.flt[t].name, "%s", a); V.flt[t].mode = 1; }
        } else if (!strcmp(k, "--gamma")) {
            gamma_set = 1;
            if (!strcmp(a, "off") || !strcmp(a, "none")) V.gamma[0] = 0;
            else { snprintf(V.gamma + 1, VP_CFG_LEN - 1, "%s", a); V.gamma[0] = 1; }
        } else if (!strcmp(k, "--mask")) {
            if (!strcmp(a, "off") || !strcmp(a, "none")) V.mask[0] = 0, V.mask[1] = 0;
            else snprintf(V.mask + 1, VP_CFG_LEN - 1, "%s", a);
        } else if (!strcmp(k, "--maskmode")) {
            static const char *const MM[] = { "none", "1x", "2x", "1x rotated", "2x rotated" };
            for (int m = 0; m < 5; m++) if (!strcmp(a, MM[m])) V.mask[0] = (char)m;
        } else if (!strcmp(k, "--afilter")) {
            if (!strcmp(a, "off") || !strcmp(a, "none")) V.afilt[0] = 0;
            else { snprintf(V.afilt + 1, VP_CFG_LEN - 1, "%s", a); V.afilt[0] = 1; }
        } else { fprintf(stderr, "vproc_stream: unknown %s\n", k); return 2; }
    }
    if (preset) {                                        /* the preset first, as vproc_preset.py (then the options) */
        char p[1200]; snprintf(p, sizeof p, "presets/%s", preset);
        tango_vproc_t keep = V;                          /* the options given on the command line win */
        memset(V.flt, 0, sizeof V.flt); memset(V.gamma, 0, sizeof V.gamma); memset(V.mask, 0, sizeof V.mask); memset(V.afilt, 0, sizeof V.afilt);
        if (vp_apply_preset(&V, p) < 0) { fprintf(stderr, "vproc_stream: no preset %s\n", p); return 1; }
        for (int t = 0; t < 4; t++) if (keep.flt[t].mode || keep.flt[t].name[0]) V.flt[t] = keep.flt[t];
        if (keep.gamma[1] || gamma_set) memcpy(V.gamma, keep.gamma, sizeof V.gamma);
        if (keep.mask[1] || keep.mask[0]) { if (keep.mask[1]) memcpy(V.mask + 1, keep.mask + 1, VP_CFG_LEN - 1); if (keep.mask[0]) V.mask[0] = keep.mask[0]; }
        if (keep.afilt[1]) memcpy(V.afilt, keep.afilt, sizeof V.afilt);
    }
    if (!out) { fprintf(stderr, "vproc_stream: --stream <file>\n"); return 2; }
    rl.out = fopen(out, "wb");
    if (!rl.out) return 1;
    if (V.gamma[0] && V.gamma[1]) vp_send_gamma(&V);    /* vproc_preset.py sends gamma only when it is on */
    vp_send_scaler_level(&V, sl);
    vp_send_mask(&V);
    vp_send_afilter(&V);
    fclose(rl.out);
    return 0;
}
