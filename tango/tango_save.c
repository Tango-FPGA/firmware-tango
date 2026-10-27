// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 the Tango authors
/*
 * tango_save.c -- see tango_save.h for the MiSTer source lines each rule comes from.
 * Tango, our own code.
 */
#include <string.h>
#include <stdio.h>
#include "tango_save.h"
#include "confstr.h"

void tango_save_path(const char *core, const char *rom, char *out, int cap)
{
    const char *p = strrchr(rom, '/');
    const char *base = p ? p + 1 : rom;
    char name[TANGO_SAVE_PATH];
    snprintf(name, sizeof name, "%s", base);
    char *e = strrchr(name, '.');
    if (e) *e = 0;                                  /* ext_replace = 1 (file_io.h 107) */
    snprintf(out, (size_t)cap, "saves/%s/%s.sav", core, name);
}

int tango_save_wanted(const char *conf, int ent)
{
    cs_text_t t; cs_entry_t e;
    if (!cs_get(conf, ent, &t) || !cs_parse(t, &e)) return 0;
    return e.kind == CS_FILE && e.f_save;
}

int64_t tango_save_mount(tango_slot_t *s, const tango_savefs_t *fs, const char *path, int slot, int pre, uint8_t *flags)
{
    tango_save_unmount(s, fs);
    snprintf(s->path, sizeof s->path, "%s", path);
    /* create_path(SAVE_DIR, CoreName2) (file_io.cpp 972): every folder of the path */
    char dir[TANGO_SAVE_PATH];
    for (const char *q = strchr(path, '/'); q; q = strchr(q + 1, '/')) {
        int n = (int)(q - path); if (n >= (int)sizeof dir) break;
        memcpy(dir, path, (size_t)n); dir[n] = 0;
        fs->mkdir(fs->ctx, dir);
    }
    int writable = 0; uint32_t size = 0;
    s->h = fs->open(fs->ctx, path, 0, &writable, &size);
    s->cangrow = pre != 0;
    if (s->h >= 0) { s->missing = 0; s->writable = writable; s->size = size; }
    else if (pre) { s->missing = 1; s->writable = 1; s->size = 0; }     /* "Will be created upon write" */
    else { s->mounted = 0; if (flags) *flags = (uint8_t)(1u << slot | 0x80); return -1; }
    s->mounted = 1;
    if (flags) *flags = (uint8_t)((1u << slot) | (s->writable ? 0 : 0x80));
    return s->size;                                 /* the missing file: pre_size = 0 */
}

void tango_save_unmount(tango_slot_t *s, const tango_savefs_t *fs)
{
    if (s->h >= 0 && s->mounted) fs->close(fs->ctx, s->h);
    memset(s, 0, sizeof *s);
    s->h = -1;
}

void tango_save_read(tango_slot_t *s, const tango_savefs_t *fs, uint32_t lba, uint8_t buf[512])
{
    if (!s->mounted || s->missing) { memset(buf, 0xFF, 512); return; }   /* type 2, the generic core: 0xFF */
    int n = fs->read(fs->ctx, s->h, lba * 512u, buf, 512);
    if (n <= 0) { memset(buf, 0, 512); return; }                           /* read nothing: 0x00 (3621-3626) */
    if (n < 512) memset(buf + n, 0, (size_t)(512 - n));
}

void tango_save_write(tango_slot_t *s, const tango_savefs_t *fs, uint32_t lba, const uint8_t buf[512])
{
    if (!s->mounted || !s->writable) return;
    if (s->missing) {
        if (lba) return;                            /* lba <= size/512 = 0: only sector 0 creates the file */
        int w = 0; uint32_t sz = 0;
        s->h = fs->open(fs->ctx, s->path, 1, &w, &sz);
        if (s->h < 0) return;                       /* "Error in creating file" */
        s->missing = 0;
        if (fs->write(fs->ctx, s->h, 0, buf, 512) == 512) s->size = 512;
        return;
    }
    if (lba > s->size / 512) return;
    unsigned n = 512;
    if (!s->cangrow) {
        uint32_t off = lba * 512u, rem = s->size > off ? s->size - off : 0;
        if (rem < n) n = rem;
    }
    if (!n) return;
    int w = fs->write(fs->ctx, s->h, lba * 512u, buf, n);
    if (w > 0 && lba * 512u + (uint32_t)w > s->size) s->size = lba * 512u + (uint32_t)w;
}
