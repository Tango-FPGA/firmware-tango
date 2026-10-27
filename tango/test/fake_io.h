// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 the Tango authors
/*
 * fake_io.h -- host-test stand-ins for the BL616's SPI link and SD card. Tango, our own code.
 *
 * rec_link: a tango_link_t that writes every CSN-low transfer to a stream file
 *           (u16 little-endian length, then the bytes the BL616 sent) and answers like a Tango launcher
 *           core would: CAPS 'Z' 'O' ..., STATUS (shown bank, swap done), SYS READ_CFG "MENU;;".
 * fake_card: a tango_fs_t over a list of paths ("dir/" for folders), listed in the given order.
 */
#ifndef FAKE_IO_H
#define FAKE_IO_H
#include <stdio.h>
#include "../tango_proto.h"
#include "../tango_browser.h"

typedef struct {
    FILE    *out;            /* NULL = don't record */
    uint8_t  buf[16384];
    int      n;
    uint8_t  shown;          /* the FPGA's shown bank */
    const char *confstr;     /* READ_CFG answer */
    int      caps_ok;        /* answer CAPS with the magic */
    long     transfers, bytes;
    /* a framework core (sys-tango tango_link.v): SET_STATUS, GET_MENUMASK, SDC IMAGE download */
    uint8_t  status[16];     /* the word the core got (applied at the CSN rise) */
    int      status_writes;
    uint16_t menumask;
    unsigned dl_space;       /* free download FIFO entries the core offers (capped at 512 per STATUS) */
    unsigned dl_offer;       /* offered by the last STATUS: WRITE bytes beyond it are dropped */
    int      dl_open, dl_starts, dl_ends, dl_dropped;
    uint8_t  dl_index;
    uint32_t dl_size, dl_got, dl_crc;
    uint8_t  dl_buf[4096];   /* every WRITE byte, in order (dl_crc alone can't
                              * tell a test "which 8 bytes" -- a DIP/NVRAM round-trip test needs the exact
                              * content, not just a checksum). Bytes beyond sizeof dl_buf are still counted
                              * (dl_got) but not stored. */
    /* the upload direction (sub 4 UL_SELECT, 5 UL_STATUS, 3 READ), a plain
     * C-level stand-in for sys-tango tango_io.v's own fill engine -- no fill-engine timing is modelled (ul_ready
     * goes true the instant UL_SELECT opens, since this fake has no "pull bytes from the core" latency to
     * simulate at all), only the wire shape tango_ul_begin/_pump/_end actually drive: open/size, poll ready,
     * read back ul_buf (which the TEST pre-fills, standing in for "the core's own current NVRAM content"),
     * close. A UL_SELECT while a download is open is refused outright here (ul_refused_while_dl++), not
     * deferred the way the real RTL defers it -- this fake exists to prove tango_fw.c's own glue never
     * actually does that (arc_dl_idle()'s own guard), not to re-model the RTL's own deferral mechanism again. */
    int      ul_open, ul_opens, ul_closes, ul_reads, ul_refused_while_dl;
    uint8_t  ul_index;
    uint32_t ul_size, ul_off;
    uint8_t  ul_buf[4096];
    int      ul_unsupported; /* 1 = model a core built without TANGO_IOCTL_UPLOAD: sub 4/5/3 hit no decode
                              * branch at all (menu-tango docs/TANGO-SPEC.md), so UL_STATUS always reports not-open/
                              * avail 0 -- tango_ul_pump's own "avail == 0 means give up" rule, not a stuck
                              * open/stuck poll */
    int      ul_never_ready; /* models a session that opens (avail reports the
                              * real size, never 0) but whose fill engine never finishes -- a wedged core,
                              * or an upload deferred forever behind a download that never closes. Unlike
                              * ul_unsupported (avail 0 from the first poll), tango_ul_pump's own "avail==0
                              * means give up" rule never fires here: this is exactly the case
                              * tango_ul_recv's own poll cap (tango_coremenu.c) exists for. */
    uint16_t joy;            /* HID GET_DB9 bytes 3, 4 */
    /* MiSTer's video / audio processing (sys TANGO_VIDEO_PROC): byte 3 of SYS 0x2B = vp_flags, of 0x32 0x39 0x3E = 1 */
    int      vp_has;
    uint8_t  vp_flags;
    uint8_t  vp_afreply;    /* byte 3 of SYS 0x39 (0: 1, a core without the mix stage; 3: with boost, a sys-tango with the mix stage) */
    uint8_t  vs_reply;      /* byte 3 of SYS 0x70 VSYNC_ADJUST (0: no such block, as an older core) */
    int      vs_pending;    /* a retune-capable core: after an apply (n0 bit 7), this many probes answer with bit 2 (retune pending) set */
    int      vs_left;       /*   (counts down) */
    uint8_t  audvol;        /* the last SYS 0x26 byte the core got */
    int      audvol_writes;
    /* HID target 1 beyond the canned GET_DB9: test/fake_pad_fpga.c. xfer returns 1 and sets *reply to answer byte i (i >= 3) of the transfer
     * in buf[0..i-1]; end sees the whole transfer at CSN high. NULL = the old canned behaviour (everything but GET_DB9 answers 00). */
    void    *hid_ctx;
    int    (*hid_xfer)(void *ctx, const uint8_t *buf, int i, uint8_t *reply);
    void   (*hid_end)(void *ctx, const uint8_t *buf, int n);
    void   (*delay_hook)(void *ctx, unsigned ms);    /* test clock: called by every delay_ms (fake_pad_fpga timelines) */
    void    *delay_ctx;
    unsigned delay_total;   /* ms the code under test waited through link->delay_ms */
} rec_link_t;

void rec_link_init(rec_link_t *r, tango_link_t *link);

typedef struct {
    const char **paths;
    int          npaths;
    char         dir[256];
    int          pos;
    int          opened;
} fake_card_t;

void fake_card_init(fake_card_t *c, tango_fs_t *fs, const char **paths, int n);
#endif
