// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 the Tango authors
/*
 * tango_save.h -- MiSTer's backup-RAM saves: saves/<CORE>/<rom>.sav mounted for the core's sd_rd / sd_wr.
 *
 * Tango. Our own code, written from MiSTer's behaviour (Main_MiSTer master 5a3a086):
 *   user_io.cpp 955-966        an "FS..." CONF_STR entry = opensave: after its file is loaded, mount its save
 *   user_io.cpp 3017-3022      FileGenerateSavePath(rom) + user_io_file_mount(save, 0, pre = 1)
 *   file_io.cpp 970-998, file_io.h 106-107  saves/<core>/<rom name, extension replaced by ".sav">; the folder made
 *   user_io.cpp 2181-2357      user_io_file_mount: open read-write if the file can be written; a missing file with
 *                              pre = "created upon write" (size reported 0, writable, may grow); then
 *                              SET_SDINFO (size) and SET_SDSTAT ((1 << slot) | read-only 0x80)
 *   user_io.cpp 3494-3538      a sector write: a missing file is created by a write to lba 0 (other lbas are lost
 *                              until then); an existing one takes writes at lba <= size / 512 (so it grows one
 *                              sector at a time), clipped to its size unless it may grow
 *   user_io.cpp 3585-3627      a sector read: the file's sector; a missing file reads 0xFF (the generic case), a
 *                              sector past the end of an existing file reads 0x00
 *   user_io.cpp 3493, menu.cpp 8654, 2263-2293   every write starts a 500 ms "Saving..." window for the OSD
 * The file I/O goes through a small interface so that the host tests can use a fake card.
 */
#ifndef TANGO_SAVE_H
#define TANGO_SAVE_H

#include <stdint.h>

#define TANGO_SAVE_SLOTS 4
#define TANGO_SAVE_PATH  192

typedef struct {
    int  (*open)(void *ctx, const char *path, int create, int *writable, uint32_t *size);   /* handle >= 0, or -1 */
    int  (*read)(void *ctx, int h, uint32_t off, uint8_t *buf, unsigned n);                  /* bytes read, -1 */
    int  (*write)(void *ctx, int h, uint32_t off, const uint8_t *buf, unsigned n);           /* bytes written, -1 */
    void (*close)(void *ctx, int h);
    int  (*mkdir)(void *ctx, const char *path);                                              /* 0, or exists */
    void  *ctx;
} tango_savefs_t;

typedef struct {
    int      mounted;          /* an image is in this slot (possibly still to be created) */
    int      h;                /* open handle, -1 while the file does not exist yet */
    int      missing;          /* MiSTer's sd_image type 2: "created upon write" */
    int      writable, cangrow;
    uint32_t size;
    char     path[TANGO_SAVE_PATH];
} tango_slot_t;

/* FileGenerateSavePath: "saves/<core>/<rom file name, extension replaced>.sav" (relative to the card root) */
void tango_save_path(const char *core, const char *rom, char *out, int cap);
/* 1 if CONF_STR entry `ent` is an F entry with the S flag ("FS..."): its file gets a save */
int  tango_save_wanted(const char *conf, int ent);

/* user_io_file_mount(path, slot, pre): returns the size to report to the core (SET_SDINFO) and sets *flags to
 * MiSTer's SET_SDSTAT byte: (1 << slot) | 0x80 if read-only. Returns -1 if nothing could be mounted. */
int64_t tango_save_mount(tango_slot_t *s, const tango_savefs_t *fs, const char *path, int slot, int pre, uint8_t *flags);
void    tango_save_unmount(tango_slot_t *s, const tango_savefs_t *fs);
/* the core's 512-byte sector requests */
void tango_save_read(tango_slot_t *s, const tango_savefs_t *fs, uint32_t lba, uint8_t buf[512]);
void tango_save_write(tango_slot_t *s, const tango_savefs_t *fs, uint32_t lba, const uint8_t buf[512]);

#endif
