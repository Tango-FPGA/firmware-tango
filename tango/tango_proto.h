// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 the Tango authors
/*
 * tango_proto.h -- the BL616 side of the Tango OSD wire protocol (SPI target 2, commands 0x10-0x17).
 *
 * Tango. Our own code. Byte format: menu-tango docs/spec/tango-spec.html, "Wire format, byte by byte".
 * The link is abstract (tango_link_t) so the same code runs in the firmware (mcu_hw_spi_*) and in the
 * host tests (a recorder that writes every transfer to a stream file for the FPGA testbench).
 */
#ifndef TANGO_PROTO_H
#define TANGO_PROTO_H

#include <stdint.h>
#include "tango_layout.h"

#define TANGO_SPI_TARGET_SYS   0
#define TANGO_SPI_SYS_READ_CFG 8
#define TANGO_SPI_TARGET_OSD   2
#define TANGO_OSD_ENABLE    0x01
#define TANGO_OSD_CAPS      0x10
#define TANGO_OSD_GEOMETRY  0x11
#define TANGO_OSD_PALETTE   0x12
#define TANGO_OSD_ATLAS     0x13
#define TANGO_OSD_GLYPHS    0x14
#define TANGO_OSD_ROWS      0x15
#define TANGO_OSD_SWAP      0x16
#define TANGO_OSD_STATUS    0x17

#define TANGO_CAPS_LEN      13
#define TANGO_PAL_ENTRIES   17      /* 16 palette + entry 16 = 3/4 panel */
#define TANGO_ATLAS_CHUNK   128     /* columns per ATLAS transfer */

typedef struct {
    void    (*begin)(void *ctx);                 /* CSN low */
    uint8_t (*xfer)(void *ctx, uint8_t mosi);    /* one byte out, one byte in */
    void    (*end)(void *ctx);                   /* CSN high */
    void    (*delay_ms)(void *ctx, unsigned ms);
    void     *ctx;
} tango_link_t;

typedef struct {
    const tango_link_t *link;
    uint8_t  caps[TANGO_CAPS_LEN];
    uint8_t  shown;                 /* bank the FPGA shows (or will show after the last SWAP) */
    uint8_t  swapped;               /* a SWAP was sent since the last STATUS wait */
    uint8_t  valid[2];              /* shadow of that bank is known */
    uint16_t nglyphs[2];
    uint32_t glyphs[2][TANGO_GLYPH_SLOTS];
    uint32_t rows[2][TANGO_ROW_WORDS];
    /* statistics of the last tango_osd_show() */
    uint16_t last_glyph_entries, last_row_entries, last_transfers;
} tango_osd_t;

/* CAPS: 1 if a Tango core answered with 'Z' 'O'. */
int  tango_osd_probe(tango_osd_t *o, const tango_link_t *link);

/* SYS READ_CFG: reads the first n-1 bytes of the core's CONF_STR into buf and NUL-terminates it.
 * Returns 1 if the core is the launcher: MiSTer's is_menu() (name "MENU"), or no CONF_STR at all. */
int  tango_core_is_menu(const tango_link_t *link, char *buf, int n);

/* STATUS: returns the status byte (bit0 shown bank, bit1 swap pending). */
uint8_t tango_osd_status(tango_osd_t *o);

/* Core start: STATUS, PALETTE (pal[16] RGB888 + 3/4 panel), GEOMETRY, ATLAS. */
void tango_osd_start(tango_osd_t *o, const uint8_t pal[16][3]);

/* Writes a page into the hidden bank (only what differs from what that bank holds), then SWAP. */
void tango_osd_show(tango_osd_t *o, const tango_tables_t *t);

void tango_osd_enable(tango_osd_t *o, int on);

/* ---- the core interface (menu-tango docs/TANGO-SPEC.md "Cores: CONF_STR, option word, pads and file download") ---- */
#define TANGO_SYS_SET_STATUS   0x10
#define TANGO_SYS_GET_MENUMASK 0x11
#define TANGO_SYS_AUDVOL       0x26          /* UIO_AUDVOL: the global volume {mute 4, att 3:0} */
#define TANGO_SPI_TARGET_HID   1
#define TANGO_HID_REPORT       0x12       /* menu-tango docs/TANGO-SPEC.md "Controllers" */
#define TANGO_HID_SET_PADMAP   0x13
#define TANGO_SPI_TARGET_SDC   3
#define TANGO_SDC_IMAGE        8
#define TANGO_SDC_STATUS       1
#define TANGO_SDC_INS_LARGE    7
#define TANGO_SDC_SECTOR_IN    9
#define TANGO_SDC_SECTOR_OUT   10
#define TANGO_HID_KBD_REPORT   0x15          /* spi.h SPI_SDC_IMAGE; sub-commands 0 STATUS 1 SELECT 2 WRITE */

/* SYS 0x10: 00 10 <first> <b0> ... : status bytes first.. (byte k = status[8k+7:8k]) */
void     tango_sys_set_status(const tango_link_t *l, uint8_t first, const uint8_t *b, int n);
/* SYS 0x11: 00 11 00 + 2 reads = status_menumask[15:8], [7:0] */
uint16_t tango_sys_get_menumask(const tango_link_t *l);
/* SYS 0x26: 00 26 <vol> (tango_volume.h: tango_vol_audvol) */
void     tango_sys_audvol(const tango_link_t *l, uint8_t vol);
/* SYS READ_CFG: the core's CONF_STR into buf (at most cap-1 bytes), NUL-terminated; returns its length */
int      tango_sys_read_confstr(const tango_link_t *l, char *buf, int cap);
/* file download = FPGA Companion's image commands (sdc.c 500-519, 668-687, 435-476) */
void     tango_ioctl_select(const tango_link_t *l, uint8_t idx, uint32_t size);            /* 03 08 01 idx size(BE32) */
int      tango_ioctl_status(const tango_link_t *l, uint8_t idx, unsigned *free_bytes);     /* 03 08 00 idx + 3 reads; 1 = open */
/* upload = the reverse direction (core -> firmware): sdc_sub values 4/5/3,
 * NOT a reuse of SELECT/STATUS/WRITE's own 1/0/2 -- SELECT's sub 1 already pushes a FIFO start marker that
 * asserts ioctl_download the moment it arrives (sys/tango_link.v's f_sdc_img SELECT branch), so sharing it
 * for an upload-open would wrongly start a download too; a separate sub leaves that path untouched. */
/* MUST match sys/tango_io.v's UL_DEPTH (1 << UL_AW, UL_AW = 11) and sys/tango_link.v's UL_MAX: a session
 * above this is silently truncated by the RTL (UL_STATUS reports the truncated size, not the one asked
 * for), so tango_ul_begin (tango_coremenu.c) refuses a size above this outright rather than mislead a
 * caller into believing more came back than actually did. */
#define TANGO_UL_MAX 2048
void     tango_ioctl_ul_select(const tango_link_t *l, uint8_t idx, uint32_t size);          /* 03 08 04 idx size(BE32); 0 = close */
int      tango_ioctl_ul_status(const tango_link_t *l, uint8_t idx, unsigned *avail);        /* 03 08 05 idx + 3 reads; 1 = ready */
void     tango_ioctl_read(const tango_link_t *l, uint8_t idx, uint8_t *d, unsigned n);       /* 03 08 03 idx + n reads */
/* HID REPORT 01 12 port + 9 reads: byte 3 flags of BOTH ports (bit0 A1 connected, 1 A1 changed, 2 A2 connected,
 * 3 A2 changed), 4-5 VID, 6-7 PID, 8-11 raw[31:0] of `port` (0 = USB-A1 = joystick_1, 1 = USB-A2 = joystick_0) */
uint8_t  tango_hid_report(const tango_link_t *l, uint8_t port, uint16_t *vid, uint16_t *pid, uint32_t *raw);
/* HID SET_PADMAP 01 13 port kind first (p, a)...: kind 1 = core map (n pairs from joystick bit `first`); FE = default */
void     tango_hid_set_padmap(const tango_link_t *l, uint8_t port, uint8_t kind, uint8_t first, const uint8_t *pairs, int n);
/* HID KBD_REPORT 01 15 port + 9 reads: byte 3 flags (REPORT's bits 0-3, bit4 A1 / bit5 A2 is a keyboard),
 * bytes 4-11 the port's 8-byte boot report (menu-tango docs/TANGO-SPEC.md "Keyboards") */
uint8_t  tango_hid_kbd_report(const tango_link_t *l, uint8_t port, uint8_t rep[8]);
/* ---- the controller-database link (sys-tango tango_link.v, generic pads; tango_pad_decode.v). An FPGA without it answers 00 to every one. ---- */
#define TANGO_HID_DESCRIPTOR   0x11
#define TANGO_HID_DESC_SELECT  0x16
#define TANGO_HID_REPORT2      0x17
#define TANGO_HID_SET_DECODE   0x18
#define TANGO_HID_DECODE_STATUS 0x19
#define TANGO_HID_PAD_CTRL     0x1A
#define TANGO_DECODE_SIG       0xD1       /* DECODE_STATUS byte 3 on an FPGA that has the decode block */
#define TANGO_REPORT2_BYTES    16
enum { TANGO_DESC_DEVICE = 0, TANGO_DESC_CONFIG = 1, TANGO_DESC_REPORT = 2 };
#define TANGO_R2_LIVE(p)       (0x40 << (p))   /* REPORT2 flags (b3, port independent): bit6 USB-A1 live, bit7 USB-A2 live (a CRC-good report stream) */
#define TANGO_R2_DESC_READY    0x01       /* REPORT2 b31 (selected port): descriptors ready */
#define TANGO_R2_CFG_TRUNC     0x02       /*   configuration descriptor longer than the store */
#define TANGO_R2_REP_TRUNC     0x04       /*   report descriptor longer than the store */
#define TANGO_PADCTRL_EXT      0x01       /* PAD_CTRL bit 0: HID interrupt also on live / descriptors-ready changes */
#define TANGO_PADCTRL_SWAP     0x02       /* bit 1: joystick_0 and joystick_1 exchanged (MiSTer joyswap) */
typedef struct {
    uint8_t  flags;                 /* b3: REPORT's flags (bit0 A1 connected, 1 A1 changed, 2 A2 connected, 3 A2 changed, 4/5 keyboard) + bit6 A1 live, bit7 A2 live */
    uint16_t vid, pid, bcd;         /* bcdDevice is 0 until the device descriptor was fetched */
    uint32_t raw;                   /* the raw word the port publishes now */
    uint8_t  len;                   /* payload length of the latest accepted report (0: none), saturating at 255 */
    uint8_t  rpt[TANGO_REPORT2_BYTES];
    uint8_t  pstat;                 /* b31: port status, TANGO_R2_DESC_READY ... */
} tango_report2_t;
typedef struct {
    uint8_t sig;                    /* TANGO_DECODE_SIG, or 00 (an old FPGA) */
    uint8_t mode, count, flags;     /* mode in force, entries in force; flags bit4 busy, 5 table write (hold), 6 the table owns the pad, 7 descriptors ready */
    uint8_t xfers, max_entries;     /* SET_DECODE transfers completed (wraps), 64 */
} tango_decstat_t;
/* 01 17 port + 29 reads */
void     tango_hid_report2(const tango_link_t *l, uint8_t port, tango_report2_t *r);
/* 01 16 port sel: resets the (per link) descriptor read cursor; sel TANGO_DESC_* */
void     tango_hid_desc_select(const tango_link_t *l, uint8_t port, uint8_t sel);
/* 01 11 port + (4 + n) reads: b3 status {A2 ready, A1 ready}, b4 reserved, b5-6 stored length of the selected descriptor of `port`, then the n bytes from the cursor
 * (which advances by n). n may be 0. */
void     tango_hid_descriptor(const tango_link_t *l, uint8_t port, uint8_t *status, uint16_t *stored_len, uint8_t *d, unsigned n);
/* 01 18 port + the pd_wire() image (mode, rid_en, rid_val, min_len, count, count x 5 bytes) */
void     tango_hid_set_decode(const tango_link_t *l, uint8_t port, const uint8_t *img, unsigned n);
/* 01 19 port + 7 reads: sig, reserved, mode, count, flags, transfers, 64 */
void     tango_hid_decode_status(const tango_link_t *l, uint8_t port, tango_decstat_t *s);
/* 01 1A bits */
void     tango_hid_pad_ctrl(const tango_link_t *l, uint8_t bits);

/* SDC (menu-tango docs/TANGO-SPEC.md "Block device"): STATUS 03 01 00 + 8 reads (bytes 3-10): request bitmap, LBA BE32,
 * 0x80, bit0 write, drive. Returns the request bitmap. */
uint8_t  tango_sdc_status(const tango_link_t *l, uint32_t *lba, int *write, uint8_t *drive);
/* INS_LARGE 03 07 drive(bit7 = read-only) size BE64: img_mounted / img_size / img_readonly; size 0 = ejected */
void     tango_sdc_insert(const tango_link_t *l, uint8_t drive, int readonly, uint64_t size);
void     tango_sdc_sector_in(const tango_link_t *l, uint8_t drive, const uint8_t buf[512]);   /* 03 09 drive <512> */
void     tango_sdc_sector_out(const tango_link_t *l, uint8_t drive, uint8_t buf[512]);        /* 03 0A drive + 512 reads */
void     tango_ioctl_write(const tango_link_t *l, uint8_t idx, const uint8_t *d, unsigned n); /* 03 08 02 idx data */

/* the 3/4 panel colour entry 16 (export_hw: K = palette[panel]*3 >> 2) */
void tango_palette_k(const uint8_t pal[16][3], uint8_t k[3]);

/* Menu colour presets: pages.palette(accent) -- bar/strip = accent, text on them = a near-black tint
 * of the accent, or near-white if the accent is dark. */
void tango_palette_from_accent(const uint8_t base[16][3], const uint8_t accent[3], uint8_t out[16][3]);

#endif
