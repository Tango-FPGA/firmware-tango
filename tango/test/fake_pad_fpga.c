// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 the Tango authors
/* fake_pad_fpga.c -- see fake_pad_fpga.h. Tango, our own code. */
#include <string.h>
#include "fake_pad_fpga.h"

static void recompute(fpad_port_t *p)
{
    uint32_t r;
    if (!p->live) return;
    if (p->mode == PDM_QUIET) { p->raw = 0; return; }
    if (p->mode == PDM_TABLE) { if (pd_eval(&p->tab, p->rpt, p->rlen, &r)) p->raw = r; return; }
    pd_table_t d; pd_builtin_ds4(&d);
    if (pd_eval(&d, p->rpt, p->rlen, &r)) p->raw = r;
}

uint8_t fpga_dev_desc(uint16_t vid, uint16_t pid, uint16_t bcd, uint8_t *d)
{
    static const uint8_t base[18] = { 18, 1, 0x00, 0x02, 0, 0, 0, 64, 0, 0, 0, 0, 0, 0, 1, 2, 0, 1 };
    memcpy(d, base, 18);
    d[8] = (uint8_t)vid; d[9] = (uint8_t)(vid >> 8); d[10] = (uint8_t)pid; d[11] = (uint8_t)(pid >> 8); d[12] = (uint8_t)bcd; d[13] = (uint8_t)(bcd >> 8);
    return 18;
}

int fpga_cfg(uint8_t cls, uint8_t sub, uint8_t proto, uint8_t *o)
{
    static const uint8_t c[18] = { 9, 2, 18, 0, 1, 1, 0, 0x80, 50, 9, 4, 0, 0, 1, 0, 0, 0, 0 };
    memcpy(o, c, 18);
    o[14] = cls; o[15] = sub; o[16] = proto;
    return 18;
}

void fpga_init(fake_pad_fpga_t *f, rec_link_t *r, int old)
{
    memset(f, 0, sizeof *f);
    f->old = old;
    r->hid_ctx = f;
    extern int fpga_xfer(void *, const uint8_t *, int, uint8_t *);
    extern void fpga_end(void *, const uint8_t *, int);
    r->hid_xfer = fpga_xfer; r->hid_end = fpga_end;
}

void fpga_plug(fake_pad_fpga_t *f, int port, uint16_t vid, uint16_t pid, uint16_t bcd, const uint8_t *cfg, int cfglen, const uint8_t *rep, int replen, int ready)
{
    fpad_port_t *p = &f->p[port];
    memset(p, 0, sizeof *p);
    p->present = 1; p->changed = 1; p->vid = vid; p->pid = pid; p->bcd = bcd;
    p->devlen = 18; fpga_dev_desc(vid, pid, bcd, p->dev);
    if (cfg) { p->cfglen = cfglen > 256 ? 256 : cfglen; memcpy(p->cfg, cfg, (size_t)p->cfglen); p->cfg_trunc = cfglen > 256; }
    if (rep) { p->replen = replen > 1024 ? 1024 : replen; memcpy(p->rep, rep, (size_t)p->replen); p->rep_trunc = replen > 1024; }
    p->ready = ready;
}

void fpga_unplug(fake_pad_fpga_t *f, int port) { memset(&f->p[port], 0, sizeof f->p[port]); f->p[port].changed = 1; }
void fpga_set_ready(fake_pad_fpga_t *f, int port, int ready) { f->p[port].ready = ready; f->p[port].changed = 1; }

void fpga_send_report(fake_pad_fpga_t *f, int port, const uint8_t *r, int len)
{
    fpad_port_t *p = &f->p[port];
    if (!p->present || len < 1) return;
    p->rlen = len > 255 ? 255 : len;
    memset(p->rpt, 0, sizeof p->rpt);
    memcpy(p->rpt, r, (size_t)(len > PD_RPT_BYTES ? PD_RPT_BYTES : len));
    if (!p->live) p->changed = 1;
    p->live = 1;
    recompute(p);
}

void fpga_link_reset(fake_pad_fpga_t *f)
{
    f->ctrl = 0; f->sel = 0; f->cursor = 0;
    for (int i = 0; i < 2; i++) {
        fpad_port_t *p = &f->p[i];
        p->mode = 0; p->count = 0; memset(&p->tab, 0, sizeof p->tab); p->xfers = 0;
        p->live = 0; p->ready = 0; p->raw = 0; p->changed = p->present;       /* the device enumerates again */
    }
}

static int is_new_cmd(int c) { return c == TANGO_HID_DESCRIPTOR || (c >= TANGO_HID_DESC_SELECT && c <= TANGO_HID_PAD_CTRL); }

static const uint8_t *store(const fpad_port_t *p, int sel, int *len)
{
    if (!p->ready) { *len = 0; return NULL; }
    if (sel == TANGO_DESC_DEVICE) { *len = p->devlen; return p->dev; }
    if (sel == TANGO_DESC_CONFIG) { *len = p->cfglen; return p->cfg; }
    *len = p->replen; return p->rep;
}

int fpga_xfer(void *ctx, const uint8_t *b, int i, uint8_t *rp)
{
    fake_pad_fpga_t *f = ctx;
    int c = b[1];
    if (f->old && is_new_cmd(c)) { *rp = 0; return 1; }          /* an unknown HID command answers 00 in every byte */
    int port = (b[2] & 1);
    fpad_port_t *p = &f->p[port];
    if (c == 0x12) {
        if (i == 3) { *rp = (uint8_t)((f->p[0].live ? 1 : 0) | (f->p[0].changed ? 2 : 0) | (f->p[1].live ? 4 : 0) | (f->p[1].changed ? 8 : 0)); return 1; }
        if (i == 4) p->changed = 0;                                   /* reading clears that port's changed bit, as REPORT does */
        uint8_t v[8] = { (uint8_t)(p->vid >> 8), (uint8_t)p->vid, (uint8_t)(p->pid >> 8), (uint8_t)p->pid,
                         (uint8_t)(p->raw >> 24), (uint8_t)(p->raw >> 16), (uint8_t)(p->raw >> 8), (uint8_t)p->raw };
        *rp = i - 4 < 8 ? v[i - 4] : 0; return 1;
    }
    if (c == TANGO_HID_REPORT2) {
        if (i == 3) {
            *rp = (uint8_t)((f->p[0].live ? 1 : 0) | (f->p[0].changed ? 2 : 0) | (f->p[1].live ? 4 : 0) | (f->p[1].changed ? 8 : 0) |
                            (f->p[0].kbd ? 16 : 0) | (f->p[1].kbd ? 32 : 0) | (f->p[0].live ? 0x40 : 0) | (f->p[1].live ? 0x80 : 0));
            return 1;
        }
        if (i == 4) p->changed = 0;
        uint8_t v[28] = { (uint8_t)(p->vid >> 8), (uint8_t)p->vid, (uint8_t)(p->pid >> 8), (uint8_t)p->pid, (uint8_t)(p->bcd >> 8), (uint8_t)p->bcd,
                          (uint8_t)(p->raw >> 24), (uint8_t)(p->raw >> 16), (uint8_t)(p->raw >> 8), (uint8_t)p->raw, (uint8_t)p->rlen };
        memcpy(v + 11, p->rpt, 16);
        v[27] = (uint8_t)((p->ready ? 1 : 0) | (p->cfg_trunc ? 2 : 0) | (p->rep_trunc ? 4 : 0));
        *rp = i - 4 < 28 ? v[i - 4] : 0;
        return 1;
    }
    if (c == TANGO_HID_DESCRIPTOR) {
        int len; const uint8_t *s = store(p, f->sel, &len);
        if (i == 3) { *rp = (uint8_t)((f->p[0].ready ? 1 : 0) | (f->p[1].ready ? 2 : 0)); return 1; }
        if (i == 4) { *rp = 0; return 1; }
        if (i == 5) { *rp = (uint8_t)(len >> 8); return 1; }
        if (i == 6) { *rp = (uint8_t)len; return 1; }
        int k = f->cursor + (i - 7);
        *rp = (s && k < len) ? s[k] : 0;
        return 1;
    }
    if (c == TANGO_HID_DECODE_STATUS) {
        uint8_t v[7] = { TANGO_DECODE_SIG, 0, (uint8_t)p->mode, (uint8_t)p->count,
                         (uint8_t)((p->ready ? 0x80 : 0) | ((p->mode != 0 && p->live) ? 0x40 : 0)), (uint8_t)p->xfers, 64 };
        *rp = (i >= 3 && i - 3 < 7) ? v[i - 3] : 0;
        return 1;
    }
    return 0;
}

void fpga_end(void *ctx, const uint8_t *b, int n)
{
    fake_pad_fpga_t *f = ctx;
    int c = b[1];
    if (c >= 0 && c < 256 && n >= 2) { if (!(f->old && is_new_cmd(c))) f->n_cmd[c]++; f->lastlen[c] = n; }
    if (f->old && is_new_cmd(c)) return;
    if (c == TANGO_HID_DESC_SELECT && n >= 4) { f->sel = b[3]; f->cursor = 0; }
    if (c == TANGO_HID_DESCRIPTOR && n > 7) f->cursor += n - 7;
    if (c == TANGO_HID_PAD_CTRL && n >= 3) f->ctrl = b[2];
    if (c == TANGO_HID_SET_DECODE && n >= 3) {
        fpad_port_t *p = &f->p[b[2] & 1];
        int len = n - 3;
        f->n_set_bytes += len;
        if (f->fail_set > 0) { f->fail_set--; return; }
        if (f->cut_set > 0 && len > f->cut_set) { len = f->cut_set; f->cut_set = 0; }
        pd_table_t t;
        if (!pd_unwire(b + 3, len, &t)) { p->mode = 0; p->count = 0; return; }       /* ended before count entries: mode 0 */
        p->tab = t; p->mode = t.mode; p->count = t.count; p->xfers++;
        recompute(p);                                                                /* the held report is evaluated again */
    }
}
