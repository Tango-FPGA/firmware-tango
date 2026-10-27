// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 the Tango authors
/*
 * fake_pad_fpga.h -- a C model of the new FPGA's HID block (sys-tango) for the pad-manager tests.
 * Tango, our own code. It implements, over fake_io.c's rec_link (hid_xfer / hid_end hooks), exactly the byte forms of
 * the controller-database link: REPORT 0x12, REPORT2 0x17, DESC_SELECT 0x16, DESCRIPTOR 0x11, SET_DECODE 0x18,
 * DECODE_STATUS 0x19, PAD_CTRL 0x1A; the decode is pd_eval (the reference of tango_pad_decode.v). `old` makes every new command answer 00
 * and change nothing, which is what a bitstream without the block does for any unknown HID command.
 * It is a model of the link's byte forms, not of the RTL: the menu repository's co-simulation runs the RTL.
 */
#ifndef FAKE_PAD_FPGA_H
#define FAKE_PAD_FPGA_H

#include "fake_io.h"
#include "../tango_paddec.h"

typedef struct {
    int      present, live, ready, kbd, changed;
    uint16_t vid, pid, bcd;
    uint8_t  rpt[PD_RPT_BYTES]; int rlen;         /* the latest accepted report */
    uint8_t  dev[18]; int devlen;                 /* what the descriptor phase stored */
    uint8_t  cfg[256]; int cfglen, cfg_trunc;
    uint8_t  rep[1024]; int replen, rep_trunc;
    pd_table_t tab; int mode, count;              /* the decode in force */
    uint32_t raw;
    int      xfers;                               /* SET_DECODE transfers completed whole */
} fpad_port_t;

typedef struct {
    int      old;                                 /* an FPGA without the block */
    fpad_port_t p[2];
    int      sel, cursor;                         /* per LINK, as the contract says */
    uint8_t  ctrl;                                /* PAD_CTRL bits */
    int      fail_set;                            /* >0: the next N SET_DECODE are ignored (mode stays 0) */
    int      cut_set;                             /* >0: the next such SET_DECODE ends after this many bytes (an incomplete table: mode 0) */
    long     n_cmd[256];                          /* transfers seen per HID command */
    long     n_set_bytes;                         /* payload bytes of all SET_DECODE */
    int      lastlen[256];                        /* length of the latest transfer of each command */
} fake_pad_fpga_t;

void fpga_init(fake_pad_fpga_t *f, rec_link_t *r, int old);
/* a device appears: its descriptors are stored (ready) unless `ready` is 0; the port starts in decode mode 0 */
void fpga_plug(fake_pad_fpga_t *f, int port, uint16_t vid, uint16_t pid, uint16_t bcd,
               const uint8_t *cfg, int cfglen, const uint8_t *rep, int replen, int ready);
void fpga_unplug(fake_pad_fpga_t *f, int port);
void fpga_set_ready(fake_pad_fpga_t *f, int port, int ready);
void fpga_send_report(fake_pad_fpga_t *f, int port, const uint8_t *r, int len);     /* an accepted report: live, raw recomputed */
void fpga_link_reset(fake_pad_fpga_t *f);         /* a core load: tables and PAD_CTRL cleared; devices re-enumerate (mode 0, descriptors re-read) */
uint8_t fpga_dev_desc(uint16_t vid, uint16_t pid, uint16_t bcd, uint8_t *out18);     /* a plain device descriptor */
/* a configuration descriptor with one interface of this class triplet (9 + 9 bytes) */
int  fpga_cfg(uint8_t cls, uint8_t sub, uint8_t proto, uint8_t *out);

#endif
