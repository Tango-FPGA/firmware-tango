// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 the Tango authors
/*
 * tango_vproc.h -- MiSTer's video and audio processing on the BL616: the scaler filters, gamma, shadow masks, the
 * audio filter and video presets, from the card's files to the framework's link commands.
 *
 * Tango. Parts of tango_vproc.c are copied or closely paraphrased from Main_MiSTer (GPL-3.0); its
 * header lists them, with the upstream notices. GPL-3.0-or-later with the whole firmware (NOTICE).
 * The rules, from Main_MiSTer master (
 * cited per function in tango_vproc.c): video.cpp 318-1125 (filters, gamma, masks, presets), audio.cpp 23-108,
 * 190-219 (audio filter), cfg.cpp 100-142 + 157-260 + 350-450 (MiSTer.ini keys and parsing), file_io.cpp
 * 2144-2193 (text files). FPGA side and wire format: the SYS commands 0x2A 0x2B 0x32 0x33 0x39 0x3E;
 * menu-tango docs/TANGO-SPEC.md "Video and audio processing".
 * sys-tango build_tools/vproc_preset.py is a second implementation of the same parsing (Python); the host tests
 * compare their transfers byte for byte over every file of a folder of MiSTer's filter files.
 *
 * Saved state as MiSTer's (config/, FileSaveConfig: the raw structs): <core>_scaler.cfg (4 x {mode, name[1023]}),
 * <core>_gamma.cfg ({on, name[1023]}), <core>_shmask.cfg ({mode, name}), <core>_afilter.cfg ({on, name}).
 * Folders (file_io.h 166-170): filters, gamma, filters_audio, shadow_masks, presets (FAT names are case-insensitive:
 * MiSTer's downloader writes Filters, Gamma, ...).
 */
#ifndef TANGO_VPROC_H
#define TANGO_VPROC_H

#include <stdint.h>
#include "tango_proto.h"

#define VP_COEFF_DIR   "filters"
#define VP_GAMMA_DIR   "gamma"
#define VP_AFILTER_DIR "filters_audio"
#define VP_SMASK_DIR   "shadow_masks"
#define VP_PRESET_DIR  "presets"

enum { VP_HORZ = 0, VP_VERT = 1, VP_SCAN = 2, VP_ILACE = 3 };          /* video.h 4-7 */
enum { VP_SM_NONE = 0, VP_SM_1X, VP_SM_2X, VP_SM_1X_ROT, VP_SM_2X_ROT, VP_SM_COUNT };   /* video.cpp 817-825 */

#define VP_CFG_LEN     1024                     /* gamma_cfg / shadow_mask_cfg / filter_cfg: [0] = on/mode, then the name */
#define VP_MAX_PHASES  512                      /* an adaptive file: 256 + 256 */
#define VP_FILE_MAX    (32 * 1024)              /* the most read of one file (the largest of MiSTer's filter files: a 10.4 KB filter) */

/* MEMORY (static buffers starve the heap, and a firmware short of heap never shows the launcher): this module owns no static RAM. A file is read into
 * a buffer of its own size while it is parsed (malloc, then free); the state of a running core (tango_vproc_t, ~16 KB)
 * is allocated by the firmware only after vp_probe found a block, so the launcher and cores without the blocks carry
 * none of it. Every allocation may fail: the file then counts as missing (nomem counts it). */

/* the card, as the firmware (FatFs) or the host tests (stdio) see it */
typedef struct {
    /* path relative to the card root; bytes read or -1 (no file). buf NULL: the file's size (no read), or -1 */
    int  (*read_file)(void *ctx, const char *path, uint8_t *buf, int cap);
    int  (*save_cfg)(void *ctx, const char *name, const void *data, int n); /* config/<name>; 0 = ok */
    int  (*load_cfg)(void *ctx, const char *name, void *data, int n);       /* config/<name>; bytes read, -1 = none */
    void  *ctx;
} vp_io_t;

/* ---- the parsers (each as Main reads the file) ---- */
typedef struct {
    int     valid;                    /* the phase count divides 256 (else the 2-phase NN filter is used) */
    int     too_many;                 /* more than 256 (512 adaptive) phases: Main clears the slot */
    int     adaptive;
    int     n;                        /* phases of the (first) set, as sent: a power of two, 1..256 */
    int16_t ph[VP_MAX_PHASES][4];     /* n phases, then n adaptive phases; taps scaled as Main (x2 unless "10bit") */
} vp_filter_t;
int vp_parse_filter(const uint8_t *buf, int len, vp_filter_t *f);              /* returns f->valid */
int vp_parse_gamma(const uint8_t *buf, int len, uint8_t tab[256][3]);         /* returns the entries read */
/* words for SYS 0x3E (MiSTer's command words), the mode's flag first; returns the count, *loaded = a mask was read */
int vp_parse_mask(const uint8_t *buf, int len, int mode, int height, uint16_t *w, int cap, int *loaded);
/* the data bytes of SYS 0x39 after the volume byte: rate, cx, cx0-2, cy0-2 as far as the file gives them */
int vp_parse_afilter(const uint8_t *buf, int len, uint8_t *out, int cap);
/* preset .ini keys -> callback(key, value) for hfilter vfilter sfilter ifilter mask maskmode gamma (+ Tango's afilter) */
void vp_parse_preset(const uint8_t *buf, int len, void (*cb)(void *ctx, const char *key, const char *val), void *ctx);

/* MiSTer.ini's processing keys (cfg.cpp 100-106, 142), from Tango.ini: [MiSTer] (or [Tango]) and the core's section */
typedef struct {
    char afilter_default[1024], vfilter_default[1024], vfilter_vertical_default[1024],
         vfilter_scanlines_default[1024], vfilter_interlace_default[1024], shmask_default[1024], preset_default[1024];
    int  shmask_mode_default;
    /* native-rate output (MiSTer cfg.cpp 48, 70-71): VSYNC_ADJUST 0..2, -1 = not in
     * Tango.ini (nothing is sent: the core's build default stays); REFRESH_MIN / REFRESH_MAX in Hz, 0 = no bound */
    int    vsync_adjust;
    double refresh_min, refresh_max;
} vp_ini_t;
void vp_parse_ini(const uint8_t *buf, int len, const char *core, vp_ini_t *ini);

/* ---- the blocks a core has: the probes (video_init 2701: has_gamma; setShadowMask 834; setFilter 27; set_vfilter
 * 556), each 00 cmd arg + one byte read, 00 = no such block. Nothing is allocated or read from the card for them. */
typedef struct {
    int     has_scaler, has_gamma, has_mask, has_afilter;   /* the core answered (any non-zero reply) */
    int     has_boost;                                      /* SET_AFILTER's reply bit 1 (audio.cpp 33-34) */
    uint8_t flags;                                          /* FLTNUM's reply */
    uint8_t vol;                                            /* the core volume byte of SET_AFILTER: vp_probe's vol, which the
                                                             * caller may change once it knows has_boost (tango_volume.h);
                                                             * vp_core_start_caps hands it to the state */
    int     has_vsync;                                      /* SYS 0x70 VSYNC_ADJUST answered (bit 7 of its reply) */
    uint8_t vsync_status;                                   /* that reply: {1, mode, active, meas_ok, rejected, retune
                                                             * pending, retune capable, 0} */
} vp_caps_t;
/* the four probes, in vp_core_start's order (the SET_AFILTER probe carries the volume byte vol); 1 = any block */
int  vp_probe(const tango_link_t *link, uint8_t vol, vp_caps_t *caps);

/* ---- the state of a running core (heap: only for a core vp_probe found a block in) ---- */
typedef struct {
    const tango_link_t *link;
    const vp_io_t      *io;
    char     core[32];
    struct { char mode; char name[1023]; } flt[4];     /* scaler_flt[4] (video.cpp 318-324) */
    char     gamma[VP_CFG_LEN];                         /* gamma_cfg: [0] on, name */
    char     mask[VP_CFG_LEN];                          /* shadow_mask_cfg: [0] mode, name */
    char     afilt[VP_CFG_LEN];                         /* filter_cfg: [0] on (Custom), name */
    uint8_t  vol;                                       /* core volume byte of SET_AFILTER (0: no volume control) */
    int      has_scaler, has_gamma, has_mask, has_afilter;   /* the core answered the probes (any non-zero reply) */
    int      has_boost;                                  /* SET_AFILTER's reply bit 1 (audio.cpp 33-34); no volume UI yet */
    uint8_t  flags;                                     /* FLTNUM's last reply */
    int      has_vsync;                                 /* the core has native-rate output (SYS 0x70) */
    uint8_t  vsync_status;                              /* its last reply (vp_vsync_status re-reads it) */
    int      retune_timeouts;                           /* HDMI PLL retune: probes after an apply never saw the retune finish */
    int      vset;                                      /* the slot whose coefficients the V set holds */
    int      height;                                    /* output height (shadow-mask resolution= sections): 720 */
    int      adaptive[4];                               /* the slots' files were adaptive, when last read */
    int      nomem;                                     /* reads / sends skipped for want of memory (the firmware logs it) */
    vp_ini_t ini;
    uint8_t  xbuf[4 + 256 * 8 + 16];                    /* one transfer */
} tango_vproc_t;

/* a card file, whole and NUL-terminated, in a buffer of its own (at most cap bytes); free() it. NULL = no file, or
 * no memory. For Tango.ini at core start and the files below. */
uint8_t *vp_load(const vp_io_t *io, const char *path, int cap, int *len);

/* core start (video_cfg_init + load*Cfg + set*, audio's load_volume + setFilter): Tango.ini (ini_buf may be NULL),
 * the saved state or the ini defaults, the probes, then everything to the core. vp_core_start_caps: the same with
 * the probes already made (vp_probe): the link sees exactly what vp_core_start sends. */
void vp_core_start(tango_vproc_t *v, const tango_link_t *link, const vp_io_t *io, const char *core,
                   const uint8_t *ini_buf, int ini_len);
void vp_core_start_caps(tango_vproc_t *v, const tango_link_t *link, const vp_io_t *io, const char *core,
                        const uint8_t *ini_buf, int ini_len, const vp_caps_t *caps);
/* SYS 0x70 VSYNC_ADJUST: Tango.ini's VSYNC_ADJUST / REFRESH_MIN / REFRESH_MAX to
 * the core (vp_core_start_caps sends it when the key is there; mode 1 for VSYNC_ADJUST 1 or 2) */
void vp_send_vsync(tango_vproc_t *v);
/* the core's native-rate status now (a probe: changes nothing): {1 present, mode, active, meas_ok, rejected, 000}, 0 = none */
uint8_t vp_vsync_status(tango_vproc_t *v);
/* one SYS 0x70 probe without a vproc state (the launcher frees its own at once): the reply byte, bit 7 clear / 0 = the core has no
 * VSYNC_ADJUST block.  Misc. Options' Information block reads it once, when the page opens (never while a retune may be running) */
uint8_t vp_vsync_read(const tango_link_t *link);
/* set_vfilter(0): FLTNUM again; if the core's scanline level now selects another V set, send it. Returns 1 if sent */
int  vp_poll(tango_vproc_t *v);
/* the menu's actions (each saves the core's state, as video_set_* / audio_set_*) */
void vp_set_scaler_flt(tango_vproc_t *v, int type, int on);
void vp_set_scaler_coeff(tango_vproc_t *v, int type, const char *name);
void vp_set_gamma_en(tango_vproc_t *v, int on);
void vp_set_gamma_curve(tango_vproc_t *v, const char *name);
void vp_set_mask_mode(tango_vproc_t *v, int mode);
void vp_set_mask(tango_vproc_t *v, const char *name);
void vp_set_afilter_en(tango_vproc_t *v, int on);
void vp_set_afilter(tango_vproc_t *v, const char *name);
int  vp_load_preset(tango_vproc_t *v, const char *path, int save);   /* path relative to the card root */
void vp_reset_defaults(tango_vproc_t *v);                             /* video_cfg_reset: delete, ini defaults, send */
/* the name shown on a file row: the part after the last '/' (video_get_*(1)) */
const char *vp_short(const char *path);

/* ---- the transfers (exported for the host tests: same bytes as vproc_preset.py) ---- */
void vp_send_gamma(tango_vproc_t *v);
void vp_send_scaler(tango_vproc_t *v);                 /* FLTNUM probe (the level in use), then vp_send_scaler_level */
void vp_send_scaler_level(tango_vproc_t *v, int sl);   /* FLTNUM mode + the H, V (+ adaptive) sets for scanline level sl */
int  vp_apply_preset(tango_vproc_t *v, const char *path);   /* a preset into the state, nothing sent; bit0 scaler,
                                                               bit1 mask, bit2 gamma, bit3 audio filter changed; -1 = no file */
void vp_send_mask(tango_vproc_t *v);
void vp_send_afilter(tango_vproc_t *v);

#endif
