// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 the Tango authors
/*
 * tango_padmgr.c -- see tango_padmgr.h. Tango, our own code.
 */
#include <string.h>
#include <stdio.h>
#include "tango_padmgr.h"
#include "tango_padres.h"

enum { PP_IDLE = 0, PP_WAIT, PP_DONE };
#define PM_NEG 8
#define PM_LINE 1024               /* longest database line kept; a longer one is dropped whole */
#define PM_CHUNK 256

struct padmgr_port {
    uint8_t  conn;              /* connected (old FPGA: the REPORT flag; new: connected or live, not a keyboard) */
    uint8_t  quarantined;
    uint8_t  st;                /* PP_ (new FPGA) */
    uint8_t  mode;              /* decode mode this manager put in force (PDM_) */
    uint8_t  src;               /* PRS_ of the last resolution */
    uint8_t  need_check;        /* verify (and re-send) the table at the next snapshot with descriptors ready */
    uint8_t  remap;             /* ... and apply the core map again afterwards (the FPGA was reloaded) */
    uint16_t vid, pid, ver, ver_hid;
    uint16_t img_len;
    uint8_t *img;               /* the pd_wire image while a table is in force (<= 325 bytes) */
    uint32_t raw;               /* the last REPORT raw word (the menu-word fallback) */
    uint32_t t_wait;
};

struct padmgr {
    padmgr_platform_t P;
    uint8_t fpga;               /* PADMGR_FPGA_ */
    uint8_t swap;
    uint8_t nneg;
    long    scans;
    struct { uint16_t vid, pid, ver; uint8_t which, exists; } neg[PM_NEG];     /* "this file has no line for this pad" (exists: the file was there) */
    struct padmgr_port port[PADMGR_PORTS];
};

static void say(const padmgr_t *m, const char *line)
{
    if (m->P.log) m->P.log(m->P.ctx, line);
}

padmgr_t *padmgr_new(const padmgr_platform_t *p)
{
    padmgr_t *m = p->alloc ? p->alloc(p->ctx, (unsigned)sizeof *m) : NULL;
    if (!m) return NULL;
    memset(m, 0, sizeof *m);
    m->P = *p;
    return m;
}

static void free_img(padmgr_t *m, struct padmgr_port *pt)
{
    if (pt->img && m->P.release) m->P.release(m->P.ctx, pt->img);
    pt->img = NULL; pt->img_len = 0;
}

void padmgr_free(padmgr_t *m)
{
    if (!m) return;
    for (int p = 0; p < PADMGR_PORTS; p++) free_img(m, &m->port[p]);
    void (*rel)(void *, void *) = m->P.release; void *c = m->P.ctx;
    if (rel) rel(c, m);
}

/* ------------------------------------------------------------------ old FPGA: which pads are quarantined */
/* The reference layout: Sony's own DualShock 4 line (version 0). The legacy decode (tango_ds4_joy.v) reads fixed bytes of a DualShock 4 report, so the
 * only pads it is right for are those with that report. SDL's button indices alone do not tell (Logitech's F710 in DirectInput mode has the same
 * b0-b11 and hat as the DS4 but a different report), the axis numbers do: the DS4 descriptor gives the sticks a0 a1 a2 / a5 and the triggers
 * a3 a4. A line that binds the face buttons, the d-pad AND the two sticks the same way is taken for a DualShock 4 clone and left alone. */
static const uint8_t DS4_CMP[] = { SN_A, SN_B, SN_X, SN_Y, SN_BACK, SN_START, SN_LSTICK, SN_RSTICK, SN_LSHOULDER, SN_RSHOULDER,
                                   SN_DPUP, SN_DPDOWN, SN_DPLEFT, SN_DPRIGHT, SN_LEFTX, SN_LEFTY, SN_RIGHTX, SN_RIGHTY, SN_LTRIGGER, SN_RTRIGGER };
int padmgr_layout_like_ds4(const sdl_map_t *e, const sdl_map_t *ref)
{
    static const uint8_t MUST[] = { SN_A, SN_B, SN_X, SN_Y, SN_DPUP, SN_DPDOWN, SN_DPLEFT, SN_DPRIGHT, SN_LEFTX, SN_LEFTY, SN_RIGHTX, SN_RIGHTY };
    for (unsigned i = 0; i < sizeof MUST; i++) {
        const sdl_bind_t *a = &e->b[MUST[i]], *b = &ref->b[MUST[i]];
        if (a->type == SB_NONE || a->type != b->type || a->idx != b->idx || a->hatmask != b->hatmask) return 0;
    }
    for (unsigned i = 0; i < sizeof DS4_CMP; i++) {
        const sdl_bind_t *a = &e->b[DS4_CMP[i]], *b = &ref->b[DS4_CMP[i]];
        if (a->type != SB_NONE && b->type != SB_NONE && (a->type != b->type || a->idx != b->idx || a->hatmask != b->hatmask)) return 0;
    }
    return 1;
}

/* the built-in table knows this pad and it is not DS4-shaped */
static int known_not_ds4(const padmgr_t *m, uint16_t vid, uint16_t pid)
{
    if (pad_is_ds4(vid, pid) || !m->P.builtin) return 0;
    sdl_map_t e, ref;
    if (m->P.builtin(m->P.ctx, vid, pid, 0, &e) == PM_NONE) return 0;
    if (m->P.builtin(m->P.ctx, 0x054c, 0x09cc, 0, &ref) == PM_NONE) return 0;     /* no reference line: leave the pad as it is */
    return !padmgr_layout_like_ds4(&e, &ref);
}

void padmgr_blank(padmgr_t *m, int port)
{
    uint8_t none[2 * 32];
    memset(none, 0xFF, sizeof none);
    tango_hid_set_padmap(m->P.link, (uint8_t)port, 1, 0, none, 32);       /* every joystick bit none (tb_padmap step 6) */
}

int padmgr_blocked(const padmgr_t *m, int port)
{
    return port >= 0 && port < PADMGR_PORTS && m->port[port].quarantined;
}

static void old_snapshot(padmgr_t *m, int p, uint8_t flags, uint16_t vid, uint16_t pid, uint32_t raw)
{
    struct padmgr_port *pt = &m->port[p];
    int conn = (flags >> (2 * p)) & 1;
    pt->raw = raw;
    if (conn == pt->conn && (!conn || (vid == pt->vid && pid == pt->pid))) return;
    pt->conn = (uint8_t)conn; pt->vid = vid; pt->pid = pid;
    pt->quarantined = 0;
    if (!conn) return;
    if (known_not_ds4(m, vid, pid)) {
        pt->quarantined = 1;
        padmgr_blank(m, p);
        char line[96];
        snprintf(line, sizeof line, "Unsupported pad %04x:%04x on USB-A%d: ignored", vid, pid, p + 1);
        say(m, line);
        return;
    }
    if (m->P.apply_map) m->P.apply_map(m->P.ctx, p);
}

/* ------------------------------------------------------------------ the new-FPGA flow */
static void probe(padmgr_t *m)
{
    tango_decstat_t st;
    tango_hid_decode_status(m->P.link, 0, &st);
    m->fpga = st.sig == TANGO_DECODE_SIG ? PADMGR_FPGA_NEW : PADMGR_FPGA_OLD;
}

static uint8_t ctrl_bits(const padmgr_t *m) { return (uint8_t)(TANGO_PADCTRL_EXT | (m->swap ? TANGO_PADCTRL_SWAP : 0)); }

void padmgr_core_start(padmgr_t *m)
{
    if (!m) return;
    probe(m);
    if (m->fpga != PADMGR_FPGA_NEW) return;
    tango_hid_pad_ctrl(m->P.link, ctrl_bits(m));
    for (int p = 0; p < PADMGR_PORTS; p++)
        if (m->port[p].st == PP_DONE) { m->port[p].need_check = 1; m->port[p].remap = 1; }
}

int padmgr_fpga(const padmgr_t *m) { return m->fpga; }

int padmgr_set_swap(padmgr_t *m, int on)
{
    if (!m) return 0;
    m->swap = on != 0;                           /* before the probe: it goes out with the first PAD_CTRL (padmgr_core_start) */
    if (m->fpga != PADMGR_FPGA_NEW) return 0;
    tango_hid_pad_ctrl(m->P.link, ctrl_bits(m));
    return 1;
}
int padmgr_swap(const padmgr_t *m) { return m->swap; }
int padmgr_swap_supported(const padmgr_t *m) { return m->fpga == PADMGR_FPGA_NEW; }
int padmgr_port_mode(const padmgr_t *m, int port) { return m->port[port].mode; }
int padmgr_port_image(const padmgr_t *m, int port, const uint8_t **img) { if (img) *img = m->port[port].img; return m->port[port].img_len; }
int padmgr_port_src(const padmgr_t *m, int port) { return m->port[port].src; }
long padmgr_scans(const padmgr_t *m) { return m->scans; }

/* negative answers: this file has no usable line for this pad (positive answers live in the port's wire image) */
static int neg_find(const padmgr_t *m, int which, uint16_t vid, uint16_t pid, uint16_t ver)
{
    for (int i = 0; i < m->nneg && i < PM_NEG; i++)
        if (m->neg[i].which == which && m->neg[i].vid == vid && m->neg[i].pid == pid && m->neg[i].ver == ver) return i;
    return -1;
}
static void neg_add(padmgr_t *m, int which, uint16_t vid, uint16_t pid, uint16_t ver, int exists)
{
    if (neg_find(m, which, vid, pid, ver) >= 0) return;
    if (m->nneg == PM_NEG) { for (int k = 1; k < PM_NEG; k++) m->neg[k - 1] = m->neg[k]; m->nneg--; }      /* full: drop the oldest */
    int i = m->nneg++;
    m->neg[i].which = (uint8_t)which; m->neg[i].vid = vid; m->neg[i].pid = pid; m->neg[i].ver = ver; m->neg[i].exists = (uint8_t)exists;
}

/* ---- the card files, line by line: one chunk and one line buffer, a line longer than the buffer is dropped WHOLE (a truncated head could parse as a
 * valid line with tokens missing) ---- */
typedef struct {
    padmgr_t *m;
    uint16_t vid, pid, ver;
    int      attempted[2], exists[2];
    int      cur, cn, cp, ll, over, eof, dropped, cached;
    char     chunk[PM_CHUNK];
    char     line[PM_LINE];
} srcctx_t;

static int sx_open(void *c, int which)
{
    srcctx_t *x = c;
    int k = neg_find(x->m, which, x->vid, x->pid, x->ver);
    if (k >= 0) {                                                          /* asked before: not in that file */
        x->cached = 1;                                                     /* a file that was there still counts as opened (the built-in table stays out) */
        return x->m->neg[k].exists;
    }
    x->cached = 0;
    x->attempted[which] = 1;
    if (!x->m->P.file_open) return 0;
    x->m->scans++;
    if (!x->m->P.file_open(x->m->P.ctx, which)) return 0;
    x->exists[which] = 1;
    x->cur = which; x->cn = x->cp = x->ll = x->over = x->eof = 0;
    return 1;
}

static const char *sx_line(void *c, int which)
{
    srcctx_t *x = c;
    (void)which;
    if (x->cached) return NULL;
    for (;;) {
        if (x->cp >= x->cn) {
            int n = x->eof ? 0 : x->m->P.file_read(x->m->P.ctx, x->cur, x->chunk, PM_CHUNK);
            if (n <= 0) {
                x->eof = 1;
                if (x->ll > 0 && !x->over) { x->line[x->ll] = 0; x->ll = 0; return x->line; }       /* a last line without a newline */
                return NULL;
            }
            x->cn = n; x->cp = 0;
        }
        char ch = x->chunk[x->cp++];
        if (ch == '\n') {
            if (x->over) { x->over = 0; x->ll = 0; x->dropped++; continue; }
            if (x->ll > 0 && x->line[x->ll - 1] == '\r') x->ll--;
            x->line[x->ll] = 0; x->ll = 0;
            return x->line;
        }
        if (x->over) continue;
        if (x->ll < PM_LINE - 1) x->line[x->ll++] = ch; else x->over = 1;
    }
}

static void sx_close(void *c, int which)
{
    srcctx_t *x = c;
    (void)which;
    if (x->cached) { x->cached = 0; return; }
    if (x->m->P.file_close) x->m->P.file_close(x->m->P.ctx, x->cur);
}

static int sx_dec(void *c, uint16_t vid, uint16_t pid, uint16_t ver, pd_table_t *out)
{
    srcctx_t *x = c;
    return x->m->P.dec_load ? x->m->P.dec_load(x->m->P.ctx, vid, pid, ver, out) : 0;
}

static int sx_builtin(void *c, uint16_t vid, uint16_t pid, uint16_t ver, sdl_map_t *out)
{
    srcctx_t *x = c;
    return x->m->P.builtin ? x->m->P.builtin(x->m->P.ctx, vid, pid, ver, out) : PM_NONE;
}

/* ---- descriptors ---- */
/* select a descriptor and read its stored length; the cursor is per LINK, so a select is always followed by its reads before anything else is selected */
static int desc_open(padmgr_t *m, int p, int sel, uint8_t *status)
{
    uint16_t len = 0; uint8_t st = 0;
    tango_hid_desc_select(m->P.link, (uint8_t)p, (uint8_t)sel);
    tango_hid_descriptor(m->P.link, (uint8_t)p, &st, &len, NULL, 0);
    if (status) *status = st;
    return len;
}

static void desc_read(padmgr_t *m, int p, uint8_t *buf, int n)
{
    int off = 0;
    while (off < n) {
        int k = n - off > 64 ? 64 : n - off;
        tango_hid_descriptor(m->P.link, (uint8_t)p, NULL, NULL, buf + off, (unsigned)k);
        off += k;
    }
}

/* the first interface descriptor of a configuration descriptor: class, subclass, protocol (-1 when there is none) */
static void cfg_first_interface(const uint8_t *c, int n, int *cls, int *sub, int *proto)
{
    *cls = *sub = *proto = -1;
    for (int off = n > 0 ? c[0] : 0; off + 1 < n && c[off] >= 2; off += c[off])
        if (c[off + 1] == 4 && off + 8 <= n) { *cls = c[off + 5]; *sub = c[off + 6]; *proto = c[off + 7]; return; }
}

/* bcdHID of the first HID descriptor (type 0x21) in a configuration descriptor, 0 when there is none */
static uint16_t cfg_hid_bcd(const uint8_t *c, int n)
{
    for (int off = n > 0 ? c[0] : 0; off + 1 < n && c[off] >= 2; off += c[off])
        if (c[off + 1] == 0x21 && off + 4 <= n) return (uint16_t)(c[off + 2] | c[off + 3] << 8);
    return 0;
}

typedef struct {
    pad_query_t q;
    hp_layout_t L;
    pad_resolution_t res;
    srcctx_t sx;
} work_t;

static const char *const PRS_NAME[] = { "none", "saved decode", "DualShock 4", "XInput", "user database", "card database", "built-in table", "descriptor only",
                                  "legacy DS4 decode", "quiet" };

static void quarantine(padmgr_t *m, int p, const char *why)
{
    char line[112];
    m->port[p].quarantined = 1;
    padmgr_blank(m, p);
    snprintf(line, sizeof line, "Unsupported pad %04x:%04x on USB-A%d: ignored (%s)", m->port[p].vid, m->port[p].pid, p + 1, why);
    say(m, line);
}

/* SET_DECODE and check it with DECODE_STATUS; one retry (the contract: an incomplete table leaves mode 0) */
static int send_table(padmgr_t *m, int p, const uint8_t *img, int n, int mode, int count)
{
    for (int attempt = 0; attempt < 2; attempt++) {
        tango_decstat_t st;
        tango_hid_set_decode(m->P.link, (uint8_t)p, img, (unsigned)n);
        tango_hid_decode_status(m->P.link, (uint8_t)p, &st);
        if (st.sig == TANGO_DECODE_SIG && st.mode == mode && st.count == count) return 1;
    }
    return 0;
}

static void resolve_port(padmgr_t *m, int p, const tango_report2_t *r2, int ready)
{
    struct padmgr_port *pt = &m->port[p];
    char line[224];
    free_img(m, pt);
    pt->mode = PDM_DS4; pt->src = 0; pt->quarantined = 0; pt->need_check = pt->remap = 0;
    work_t *w = m->P.alloc ? m->P.alloc(m->P.ctx, (unsigned)sizeof *w) : NULL;
    if (!w) {
        pt->st = PP_DONE; pt->ver = r2->bcd;
        if (known_not_ds4(m, r2->vid, r2->pid)) { quarantine(m, p, "no memory to look it up"); return; }     /* needs no heap */
        say(m, "pad: no memory to look the pad up; it keeps the DualShock 4 decode");
        if (m->P.apply_map) m->P.apply_map(m->P.ctx, p);
        return;
    }
    memset(w, 0, sizeof *w);
    uint16_t ver = r2->bcd;
    uint8_t *rd = NULL; int rn = 0, parsed = 0;
    w->q.if_class = w->q.if_sub = w->q.if_proto = -1;
    if (ready) {
        uint8_t d[64];
        int n = desc_open(m, p, TANGO_DESC_DEVICE, NULL);
        if (n >= 14) { if (n > 18) n = 18; desc_read(m, p, d, n); ver = (uint16_t)(d[12] | d[13] << 8); }
        n = desc_open(m, p, TANGO_DESC_CONFIG, NULL);
        if (n > 0) {
            if (n > (int)sizeof d) n = (int)sizeof d;
            desc_read(m, p, d, n);
            cfg_first_interface(d, n, &w->q.if_class, &w->q.if_sub, &w->q.if_proto);
            w->q.ver_hid = cfg_hid_bcd(d, n);
        }
        n = desc_open(m, p, TANGO_DESC_REPORT, NULL);
        if (n > 0) {
            rn = n > 1024 ? 1024 : n;
            rd = m->P.alloc(m->P.ctx, (unsigned)rn);
            if (rd) { desc_read(m, p, rd, rn); parsed = hp_parse(rd, rn, &w->L); }
            else say(m, "pad: no memory for the report descriptor; looked up without it");
        }
    }
    w->q.vid = r2->vid; w->q.pid = r2->pid; w->q.ver = ver; w->q.core = m->P.core;
    w->q.L = parsed ? &w->L : NULL;
    w->q.rest = r2->rpt; w->q.rest_len = r2->len > PD_RPT_BYTES ? PD_RPT_BYTES : r2->len;
    w->q.L_cls = (rd && !parsed) ? w->L.cls : HPC_NONE;
    w->sx.m = m; w->sx.vid = r2->vid; w->sx.pid = r2->pid; w->sx.ver = ver;
    pad_sources_t S = { &w->sx, sx_open, sx_line, sx_close, sx_dec, sx_builtin };
    int src = pad_resolve(&S, NULL, &w->q, &w->res);
    if (w->sx.dropped) { snprintf(line, sizeof line, "pad database: %d over-long line(s) dropped", w->sx.dropped); say(m, line); }
    if (w->sx.attempted[0] && src != PRS_USER_DB && !w->res.skipped_unusable) neg_add(m, 0, r2->vid, r2->pid, ver, w->sx.exists[0]);
    if (w->sx.attempted[1] && src != PRS_DB && !w->res.skipped_unusable) neg_add(m, 1, r2->vid, r2->pid, ver, w->sx.exists[1]);
    if (rd) m->P.release(m->P.ctx, rd);
    pt->st = PP_DONE; pt->ver = ver; pt->ver_hid = w->q.ver_hid; pt->src = (uint8_t)src;
    snprintf(line, sizeof line, "USB-A%d %04x:%04x v%04x: %s (%s)", p + 1, r2->vid, r2->pid, ver,
             src >= 0 && src < (int)(sizeof PRS_NAME / sizeof PRS_NAME[0]) ? PRS_NAME[src] : "?", w->res.why);
    say(m, line);
    if (src != PRS_DS4 && src != PRS_LEGACY && src != PRS_NONE) {         /* a table (or quiet) goes to the FPGA; DS4 is its power-up decode */
        int n = 5 + 5 * w->res.table.count;
        uint8_t *img = m->P.alloc(m->P.ctx, (unsigned)n);
        if (img && pd_wire(&w->res.table, img, n) == n) {
            pt->img = img; pt->img_len = (uint16_t)n; pt->mode = w->res.table.mode;
            if (!send_table(m, p, img, n, w->res.table.mode, w->res.table.count)) {
                free_img(m, pt); pt->mode = PDM_DS4;
                quarantine(m, p, "the FPGA did not take the decode table");
                m->P.release(m->P.ctx, w);
                return;
            }
        } else {
            if (img) m->P.release(m->P.ctx, img);
            quarantine(m, p, "no memory for the decode table");
            m->P.release(m->P.ctx, w);
            return;
        }
    }
    m->P.release(m->P.ctx, w);
    if (m->P.apply_map) m->P.apply_map(m->P.ctx, p);
}

static void drop_port(padmgr_t *m, int p)
{
    struct padmgr_port *pt = &m->port[p];
    pt->conn = 0; pt->quarantined = 0; pt->st = PP_IDLE; pt->need_check = 0;
    if (pt->img && pt->remap) return;        /* the FPGA was just reloaded and the pad is still enumerating: keep its table for when it is back */
    free_img(m, pt);
    pt->mode = PDM_DS4; pt->remap = 0; pt->src = 0;
}

static uint32_t now(const padmgr_t *m) { return m->P.now_ms ? m->P.now_ms(m->P.ctx) : 0; }

static void new_snapshot(padmgr_t *m, int p, int force)
{
    struct padmgr_port *pt = &m->port[p];
    tango_report2_t r;
    tango_hid_report2(m->P.link, (uint8_t)p, &r);
    pt->raw = r.raw;
    int conn = (r.flags >> (2 * p)) & 1, live = (r.flags >> (6 + p)) & 1, kbd = (r.flags >> (4 + p)) & 1, chg = (r.flags >> (2 * p + 1)) & 1;
    int ready = r.pstat & TANGO_R2_DESC_READY;
    if (!(conn || live) || kbd) { if (pt->st != PP_IDLE || pt->conn) drop_port(m, p); return; }
    pt->conn = 1;
    int newdev = pt->st == PP_IDLE || r.vid != pt->vid || r.pid != pt->pid || (ready && pt->st == PP_DONE && r.bcd && pt->ver && r.bcd != pt->ver);
    if (newdev) {
        if (!(pt->img && pt->remap && r.vid == pt->vid && r.pid == pt->pid)) {           /* not the pad whose table we kept across a reload */
            free_img(m, pt);
            pt->mode = PDM_DS4; pt->remap = 0;
        }
        pt->st = PP_WAIT; pt->vid = r.vid; pt->pid = r.pid; pt->quarantined = 0; pt->t_wait = now(m);
        pt->need_check = 0;
    }
    if (pt->st == PP_WAIT && ready && pt->img && pt->remap && (!r.bcd || r.bcd == pt->ver)) {
        /* the same pad after a core start: its table is sent again from the cache, no descriptor and no file is read */
        pt->st = PP_DONE;
        if (!send_table(m, p, pt->img, pt->img_len, pt->mode, pt->img[4])) quarantine(m, p, "the FPGA did not take the decode table");
        else if (m->P.apply_map) m->P.apply_map(m->P.ctx, p);
        pt->remap = 0;
        return;
    }
    if (pt->st == PP_WAIT) {
        /* the descriptor phase may still be running: wait for the ready bit (a ready-change interrupt brings us back), but not for ever */
        int timed = force || (m->P.now_ms && now(m) - pt->t_wait >= PADMGR_DESC_WAIT_MS);
        if (ready || timed) resolve_port(m, p, &r, ready != 0);
        return;
    }
    if (!ready) { if (pt->mode != PDM_DS4) pt->need_check = 1; return; }            /* re-enumerating: look again when it is back */
    /* chg is the port's "changed" bit, which com_task's own REPORT read has usually cleared already (one bit per port, shared by REPORT and REPORT2
     * [assumed from the contract]); the dependable triggers are the descriptors-ready edge (above) and padmgr_core_start's need_check */
    if ((pt->need_check || chg) && pt->img) {
        tango_decstat_t st;
        tango_hid_decode_status(m->P.link, (uint8_t)p, &st);
        int cnt = pt->img[4];
        if (!(st.sig == TANGO_DECODE_SIG && st.mode == pt->mode && st.count == cnt)) {       /* the FPGA lost it: send the cached image, no file is read */
            char line[96];
            if (send_table(m, p, pt->img, pt->img_len, pt->mode, cnt)) {
                snprintf(line, sizeof line, "USB-A%d %04x:%04x: decode table sent again", p + 1, pt->vid, pt->pid);
                say(m, line);
            } else quarantine(m, p, "the FPGA did not take the decode table");
        }
    }
    if (pt->remap) { if (!pt->quarantined && m->P.apply_map) m->P.apply_map(m->P.ctx, p); }
    pt->need_check = pt->remap = 0;
}

int padmgr_report_sample(padmgr_t *m, int port, pl_sample_t *s)
{
    if (!m || port < 0 || port >= PADMGR_PORTS || m->fpga != PADMGR_FPGA_NEW) return 0;
    tango_report2_t r;
    tango_hid_report2(m->P.link, (uint8_t)port, &r);
    if (r.len == 0) return 0;
    memcpy(s->b, r.rpt, PD_RPT_BYTES);
    s->len = r.len > PD_RPT_BYTES ? PD_RPT_BYTES : r.len;
    return 1;
}

/* the pad's report descriptor, parsed into the caller's *out (hp_layout_t is large: the caller owns it); 1 if it parsed as a pad, 0 otherwise (no descriptor, not a pad, no memory) */
int padmgr_layout(padmgr_t *m, int port, hp_layout_t *out)
{
    if (!m || port < 0 || port >= PADMGR_PORTS || m->fpga != PADMGR_FPGA_NEW || m->port[port].st != PP_DONE) return 0;
    int n = desc_open(m, port, TANGO_DESC_REPORT, NULL);
    if (n <= 0) return 0;
    int rn = n > 1024 ? 1024 : n;
    uint8_t *rd = m->P.alloc(m->P.ctx, (unsigned)rn);
    if (!rd) return 0;
    desc_read(m, port, rd, rn);
    int ok = hp_parse(rd, rn, out);
    m->P.release(m->P.ctx, rd);
    return ok;
}

uint16_t padmgr_port_ver_hid(const padmgr_t *m, int port) { return (m && port >= 0 && port < PADMGR_PORTS) ? m->port[port].ver_hid : 0; }

int padmgr_port_ids(const padmgr_t *m, int port, uint16_t *vid, uint16_t *pid, uint16_t *ver)
{
    const struct padmgr_port *pt = &m->port[port];
    if (pt->st != PP_DONE) return 0;
    *vid = pt->vid; *pid = pt->pid; *ver = pt->ver;
    return 1;
}

int padmgr_set_table(padmgr_t *m, int port, const pd_table_t *t)
{
    if (!m || port < 0 || port >= PADMGR_PORTS || m->fpga != PADMGR_FPGA_NEW || m->port[port].st != PP_DONE || t->mode == PDM_DS4) return 0;
    struct padmgr_port *pt = &m->port[port];
    int n = 5 + 5 * t->count;
    uint8_t *img = m->P.alloc ? m->P.alloc(m->P.ctx, (unsigned)n) : NULL;
    if (!img || pd_wire(t, img, n) != n) { if (img) m->P.release(m->P.ctx, img); return 0; }
    if (!send_table(m, port, img, n, t->mode, t->count)) { m->P.release(m->P.ctx, img); return 0; }
    free_img(m, pt);
    pt->img = img; pt->img_len = (uint16_t)n; pt->mode = t->mode; pt->src = PRS_USER_DEC; pt->quarantined = 0;
    char line[96];
    snprintf(line, sizeof line, "USB-A%d %04x:%04x: your own decode table (%d entries)", port + 1, pt->vid, pt->pid, t->count);
    say(m, line);
    return 1;
}

int padmgr_waiting(const padmgr_t *m)
{
    if (!m || m->fpga != PADMGR_FPGA_NEW || !m->P.now_ms) return 0;
    for (int p = 0; p < PADMGR_PORTS; p++) if (m->port[p].st == PP_WAIT) return 1;
    return 0;
}

void padmgr_tick(padmgr_t *m)
{
    if (!padmgr_waiting(m)) return;
    for (int p = 0; p < PADMGR_PORTS; p++)
        if (m->port[p].st == PP_WAIT && now(m) - m->port[p].t_wait >= PADMGR_DESC_WAIT_MS) new_snapshot(m, p, 1);
}

void padmgr_snapshot(padmgr_t *m, int p, uint8_t flags, uint16_t vid, uint16_t pid, uint32_t raw)
{
    if (!m || p < 0 || p >= PADMGR_PORTS) return;
    if (m->fpga == PADMGR_FPGA_UNKNOWN) probe(m);
    if (m->fpga == PADMGR_FPGA_NEW) new_snapshot(m, p, 0);
    else old_snapshot(m, p, flags, vid, pid, raw);
}
/* ------------------------------------------------------------------ the menu word */
uint16_t padmgr_menu_from_raw(uint32_t r)
{
    uint16_t w = (uint16_t)((r | (r >> 18)) & 0x0F);                /* right left down up = joystick bits 0..3: the d-pad or the left stick (raw 18..21), as tango_ds4_joy.v */
    w |= (uint16_t)(((r >> 5) & 1) << 4);                           /* A = Circle (raw 5) */
    w |= (uint16_t)(((r >> 4) & 1) << 5);                           /* B = Cross (raw 4) */
    w |= (uint16_t)(((r >> 7) & 1) << 6);                           /* X = Triangle (raw 7) */
    w |= (uint16_t)(((r >> 6) & 1) << 7);                           /* Y = Square (raw 6) */
    w |= (uint16_t)(((r >> 8) & 1) << 8);                           /* L = L1 */
    w |= (uint16_t)(((r >> 9) & 1) << 9);                           /* R = R1 */
    w |= (uint16_t)(((r >> 12) & 1) << 10);                         /* Select = Share */
    w |= (uint16_t)(((r >> 13) & 1) << 11);                         /* Start = Options */
    return w;
}

void padmgr_note_raw(padmgr_t *m, int port, uint32_t raw)
{
    if (m && port >= 0 && port < PADMGR_PORTS) m->port[port].raw = raw;
}

int padmgr_menu_word(const padmgr_t *m, uint16_t *word)
{
    int any = 0;
    uint16_t w = 0;
    for (int p = 0; p < PADMGR_PORTS; p++) if (m->port[p].quarantined) any = 1;
    if (!any) return 0;
    for (int p = 0; p < PADMGR_PORTS; p++)
        if (m->port[p].conn && !m->port[p].quarantined) w |= padmgr_menu_from_raw(m->port[p].raw);
    *word = w;
    return 1;
}

/* ---- the session's finish (declared in tango_padlearn.h) ---- */
int plsess_finish(plsess_t *s, padmgr_t *m, int port, const plsess_files_t *f, char *sdl_out, int sdl_cap)
{
    pd_table_t t;
    if (s->state != 1) return PLS_BAD;
    pl_table(&s->pl, &t);
    s->state = 2;
    if (t.count == 0) return 0;
    int flags = 0;
    if (m && padmgr_set_table(m, port, &t)) flags |= PLF_APPLIED;
    if (f && f->save_dec) {
        uint8_t buf[10 + 5 + 5 * PD_MAXENT]; char nm[40];
        int n = pl_dec_save(s->vid, s->pid, s->ver, &t, buf, sizeof buf);
        pl_dec_name(s->vid, s->pid, s->ver, nm, sizeof nm);
        if (n > 0 && f->save_dec(f->ctx, nm, buf, n)) flags |= PLF_DEC;
    }
    if (s->L) {
        char line[512], nm[48];
        snprintf(nm, sizeof nm, "%s %s", PLS_NAME_TAG, s->name);
        int n = pl_export_sdl(&t, s->L, s->vid, s->pid, s->sdl_ver, nm, line, sizeof line);
        if (n > 0) {
            if (sdl_out && sdl_cap > 0) snprintf(sdl_out, (size_t)sdl_cap, "%s", line);
            char guid[33]; memcpy(guid, line, 32); guid[32] = 0;
            if (f && f->db_open_new && pl_sdl_db_update(f, guid, line)) flags |= PLF_SDL;
        }
    }
    return flags;
}

/* ---- the reference capture ---- */
#define CAP_IDLE_SAMPLES PLS_IDLE_SAMPLES   /* 12 reports 50 ms apart: 600 ms of rest, enough for a counter or a sensor byte to show every bit it will ever flicker */

/* one report of the pad (not another report id's) */
static int cap_sample(padmgr_cap_t *c, pl_sample_t *s)
{
    if (!padmgr_report_sample(c->m, c->port, s)) return 0;
    if (c->rid_en && (s->len < 1 || s->b[0] != (uint8_t)c->rid)) return 0;
    return 1;
}

/* The blocking reference capture: the incremental engine (plcap_t, tango_padlearn.h) on a 20 ms poll of REPORT2 */
int padmgr_capture(void *ctx, int phase, pl_sample_t *out, int max)
{
    padmgr_cap_t *c = ctx;
    const tango_link_t *l = c->m->P.link;
    unsigned waited = 0, limit = c->press_timeout_ms ? c->press_timeout_ms : 10000;
    if (PLC_KIND(phase) == PLC_IDLE) {
        int n = 0, tries = 0;
        if (max > CAP_IDLE_SAMPLES) max = CAP_IDLE_SAMPLES;
        plcap_begin(&c->pc, c->L);
        if (c->rid_en) { c->pc.rid_en = 1; c->pc.rid = (uint8_t)c->rid; }
        while (n < max && tries < 80) {
            if (c->cancelled && c->cancelled(c->cctx)) return -1;
            tries++;
            if (cap_sample(c, &out[n])) n++;
            if (n < max) l->delay_ms(l->ctx, 50);
        }
        if (n == 0) return 0;
        pl_base_make(&c->base, c->L, out, n);
        c->pc.base = c->base; c->pc.have_base = 1;
        c->idle = c->base.idle; memcpy(c->noisy, c->base.noisy, sizeof c->noisy);
        c->have_idle = 1;
        return n;
    }
    if (!c->have_idle || max < 1) return 0;
    plcap_held_begin(&c->pc);
    unsigned t = 0;                                              /* the capture's own clock: 20 ms per poll */
    for (;;) {
        if (c->cancelled && c->cancelled(c->cctx)) return -1;
        pl_sample_t cur;
        int got = cap_sample(c, &cur);
        int r = plcap_held(&c->pc, got ? &cur : NULL, t + 1, out, max);
        if (r > 0) return r;
        if (r == 0) { if (waited >= limit) return 0; }                          /* nothing pressed yet */
        else if (waited > 3 * limit) { r = c->pc.n; return r; }                 /* a button that never lets go */
        l->delay_ms(l->ctx, 20); waited += 20; t += 20;
    }
}
