// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 the Tango authors
/*
 * afilter_seq.c -- the link transfers of an in-core OSD Audio filter sequence, made by the firmware's own code
 * (tango_vproc.c: vp_core_start, vp_set_afilter_en, vp_set_afilter, as tango_coremenu.c calls them for System ->
 * Audio filter), with waits between the steps, for console-template-tango sim/tb_vproc.v (+stream=). Tango,
 * our own code. It reproduces a fault seen on hardware: Audio filter Custom -> Internal turned the
 * Template's 440 Hz test tone into a loud 9.6 kHz (48 kHz / 5) oscillation.
 *   ./afilter_seq <filters folder> <out stream> <step> ...
 *   steps: start                (core start: the four probes, then everything to the core, as vp_start)
 *          custom | internal    (the Audio filter row: Internal <-> Custom; vp_set_afilter_en)
 *          file=<name>          (a Filters_Audio file picked in the browser; vp_set_afilter)
 *          wait=<ms>            (a wait record: u16 0xFFFF, then u32 little-endian microseconds)
 * Stream records otherwise as rec_link writes them: u16 little-endian length, then the bytes the BL616 sends.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include "../tango_vproc.h"
#include "fake_io.h"

static char root[1024];
/* the card: filters_audio/ etc. map to the filter folder's subtrees (as test_vproc.c) */
static int t_read(void *c, const char *path, uint8_t *buf, int cap)
{
    (void)c;
    static const struct { const char *card, *repo; } M[] = {
        { "filters_audio/", "Filters_MiSTer/Filters_Audio/" }, { "filters/", "Filters_MiSTer/Filters/" },
        { "gamma/", "Filters_MiSTer/Gamma/" }, { "shadow_masks/", "ShadowMasks_MiSTer/Shadow_Masks/" },
        { "presets/", "Presets_MiSTer/Presets/" } };
    char full[2200];
    snprintf(full, sizeof full, "%s/%s", root, path);
    for (int i = 0; i < 5; i++) {
        size_t l = strlen(M[i].card);
        if (!strncmp(path, M[i].card, l)) { snprintf(full, sizeof full, "%s/%s%s", root, M[i].repo, path + l); break; }
    }
    struct stat sb;
    if (stat(full, &sb) || S_ISDIR(sb.st_mode)) return -1;
    if (!buf) return (int)sb.st_size;
    FILE *f = fopen(full, "rb"); if (!f) return -1;
    int n = (int)fread(buf, 1, (size_t)cap, f); fclose(f);
    return n;
}
static int t_save(void *c, const char *name, const void *d, int n) { (void)c; (void)name; (void)d; (void)n; return 0; }
static int t_load(void *c, const char *name, void *d, int n) { (void)c; (void)name; (void)d; (void)n; return -1; }
static const vp_io_t IO = { t_read, t_save, t_load, NULL };

int main(int argc, char **argv)
{
    if (argc < 4) { fprintf(stderr, "usage: afilter_seq <filters folder> <out stream> <step> ...\n"); return 2; }
    snprintf(root, sizeof root, "%s", argv[1]);
    static rec_link_t r; tango_link_t link; rec_link_init(&r, &link);
    r.vp_has = 1; r.vp_flags = 0x07; r.vp_afreply = 3;     /* as the Template's FRAC 8 framework answers (tango_link.v) */
    r.out = fopen(argv[2], "wb");
    if (!r.out) { perror(argv[2]); return 1; }
    static tango_vproc_t V; int started = 0;
    for (int i = 3; i < argc; i++) {
        const char *a = argv[i];
        long before = r.transfers;
        if (!strcmp(a, "start")) { vp_core_start(&V, &link, &IO, "Template", NULL, 0); started = 1; }
        else if (!started) { fprintf(stderr, "afilter_seq: 'start' first\n"); return 2; }
        else if (!strcmp(a, "custom")) vp_set_afilter_en(&V, 1);
        else if (!strcmp(a, "internal")) vp_set_afilter_en(&V, 0);
        else if (!strncmp(a, "file=", 5)) vp_set_afilter(&V, a + 5);
        else if (!strncmp(a, "wait=", 5)) {
            uint32_t us = (uint32_t)(atof(a + 5) * 1000.0);
            uint8_t w[6] = { 0xFF, 0xFF, (uint8_t)us, (uint8_t)(us >> 8), (uint8_t)(us >> 16), (uint8_t)(us >> 24) };
            fwrite(w, 1, sizeof w, r.out);
        } else { fprintf(stderr, "afilter_seq: unknown step %s\n", a); return 2; }
        if (r.transfers != before) printf("%-40s %ld transfer(s); the last %d bytes:", a, r.transfers - before, r.n);
        if (r.transfers != before) { for (int k = 0; k < r.n && k < 24; k++) printf(" %02X", r.buf[k]); printf("\n"); }
    }
    fclose(r.out);
    printf("afilter_seq: %ld transfers, %ld bytes -> %s\n", r.transfers, r.bytes, argv[2]);
    return 0;
}
