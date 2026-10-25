// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 the Tango authors
/*
 * tango_proto.c -- see tango_proto.h. Tango, our own code.
 */
#include <string.h>
#include "tango_proto.h"

static void begin(tango_osd_t *o, uint8_t cmd)
{
    o->link->begin(o->link->ctx);
    o->link->xfer(o->link->ctx, TANGO_SPI_TARGET_OSD);
    o->link->xfer(o->link->ctx, cmd);
}
static uint8_t tx(tango_osd_t *o, uint8_t b) { return o->link->xfer(o->link->ctx, b); }
static void end(tango_osd_t *o) { o->link->end(o->link->ctx); o->last_transfers++; }
static void tx16(tango_osd_t *o, unsigned v) { tx(o, (uint8_t)(v >> 8)); tx(o, (uint8_t)v); }
static void tx32(tango_osd_t *o, uint32_t v) { tx(o, (uint8_t)(v >> 24)); tx(o, (uint8_t)(v >> 16)); tx(o, (uint8_t)(v >> 8)); tx(o, (uint8_t)v); }

int tango_osd_probe(tango_osd_t *o, const tango_link_t *link)
{
    memset(o, 0, sizeof *o);
    o->link = link;
    begin(o, TANGO_OSD_CAPS);
    tx(o, 0);                                            /* dummy byte, as SYS STATUS */
    for (int i = 0; i < TANGO_CAPS_LEN; i++) o->caps[i] = tx(o, 0);
    end(o);
    return o->caps[0] == 'Z' && o->caps[1] == 'O';
}

int tango_core_is_menu(const tango_link_t *link, char *buf, int n)
{
    link->begin(link->ctx);
    link->xfer(link->ctx, TANGO_SPI_TARGET_SYS);
    link->xfer(link->ctx, TANGO_SPI_SYS_READ_CFG);
    link->xfer(link->ctx, 0);                            /* sysctrl.c sys_get_config: one dummy byte */
    int i;
    for (i = 0; i < n - 1; i++) buf[i] = (char)link->xfer(link->ctx, 0);
    link->end(link->ctx);
    buf[i] = 0;
    uint8_t c0 = (uint8_t)buf[0];
    if (c0 == 0x00 || c0 == 0xFF) return 1;              /* no CONF_STR: a bare Tango core = launcher */
    /* user_io.cpp 220-222: is_menu() = !strcasecmp(core name, "MENU"); the name is entry 0 */
    static const char menu[] = "MENU";
    for (i = 0; i < 4; i++) {
        char c = buf[i]; if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        if (c != menu[i]) return 0;
    }
    return buf[4] == ';' || buf[4] == 0;
}

uint8_t tango_osd_status(tango_osd_t *o)
{
    begin(o, TANGO_OSD_STATUS);
    tx(o, 0);
    uint8_t s = tx(o, 0);
    end(o);
    return s;
}

void tango_palette_k(const uint8_t pal[16][3], uint8_t k[3])
{
    for (int c = 0; c < 3; c++) k[c] = (uint8_t)(pal[TANGO_C_PANEL][c] * 3 >> 2);
}

void tango_palette_from_accent(const uint8_t base[16][3], const uint8_t accent[3], uint8_t out[16][3])
{
    memcpy(out, base, 16 * 3);
    int r = accent[0], g = accent[1], b = accent[2];
    int light = (r * 299 + g * 587 + b * 114) / 1000 > 110;
    uint8_t on[3];
    if (light) { on[0] = (uint8_t)(r >> 3); on[1] = (uint8_t)(g >> 3); on[2] = (uint8_t)(b >> 3); }
    else { on[0] = 246; on[1] = 247; on[2] = 250; }
    memcpy(out[TANGO_C_BAR], accent, 3); memcpy(out[TANGO_C_ACC], accent, 3);
    memcpy(out[TANGO_C_BARTXT], on, 3); memcpy(out[TANGO_C_ACCTXT], on, 3);
}

void tango_osd_start(tango_osd_t *o, const uint8_t pal[16][3])
{
    uint8_t st = tango_osd_status(o);
    o->shown = st & 1;
    o->swapped = 0;
    o->valid[0] = o->valid[1] = 0;

    /* PALETTE: first 0, count 17, RGB888 each; entry 16 = 3/4 panel */
    uint8_t k[3]; tango_palette_k(pal, k);
    begin(o, TANGO_OSD_PALETTE);
    tx(o, 0); tx(o, TANGO_PAL_ENTRIES);
    for (int i = 0; i < 16; i++) for (int c = 0; c < 3; c++) tx(o, pal[i][c]);
    for (int c = 0; c < 3; c++) tx(o, k[c]);
    end(o);

    /* GEOMETRY: x0, y0, w, h (2 bytes each), strip width, row pitch, top offset, glyph rows */
    begin(o, TANGO_OSD_GEOMETRY);
    tx16(o, TANGO_X0); tx16(o, TANGO_Y0); tx16(o, TANGO_W); tx16(o, TANGO_H);
    tx(o, TANGO_SW); tx(o, TANGO_RH); tx(o, TANGO_TOP); tx(o, TANGO_FONT_SH);
    end(o);

    /* ATLAS: start column (2), then columns of TANGO_FONT_COL_BYTES, MSB first */
    for (int c0 = 0; c0 < TANGO_FONT_COLS; c0 += TANGO_ATLAS_CHUNK) {
        int n = TANGO_FONT_COLS - c0; if (n > TANGO_ATLAS_CHUNK) n = TANGO_ATLAS_CHUNK;
        begin(o, TANGO_OSD_ATLAS);
        tx16(o, (unsigned)c0);
        const uint8_t *p = &tango_font_atlas[c0 * TANGO_FONT_COL_BYTES];
        for (int i = 0; i < n * TANGO_FONT_COL_BYTES; i++) tx(o, p[i]);
        end(o);
    }
}

/* waits until a SWAP sent earlier has taken effect, so the hidden bank is really hidden */
static void wait_swap(tango_osd_t *o)
{
    if (!o->swapped) return;
    for (int tries = 0; tries < 100; tries++) {
        uint8_t st = tango_osd_status(o);
        if (!(st & 2)) break;
        if (o->link->delay_ms) o->link->delay_ms(o->link->ctx, 1);
    }
    o->swapped = 0;
}

static void send_glyphs(tango_osd_t *o, int bank, int first, const uint32_t *e, int n)
{
    begin(o, TANGO_OSD_GLYPHS);
    tx(o, (uint8_t)bank); tx16(o, (unsigned)first);
    for (int i = 0; i < n; i++) tx32(o, e[i]);
    end(o);
    o->last_glyph_entries += (uint16_t)n;
}

static void send_rows(tango_osd_t *o, int bank, int first_row, const uint32_t *words, int nrows)
{
    begin(o, TANGO_OSD_ROWS);
    tx(o, (uint8_t)bank); tx(o, (uint8_t)first_row);
    for (int r = 0; r < nrows; r++) { tx32(o, words[2 * r + 1]); tx32(o, words[2 * r]); }   /* bits 63:0, MSB first */
    end(o);
    o->last_row_entries += (uint16_t)nrows;
}

/* runs of differing entries; a gap of up to MERGE equal entries is sent too rather than a new header */
#define MERGE_GLYPHS 2
#define MERGE_ROWS   1

void tango_osd_show(tango_osd_t *o, const tango_tables_t *t)
{
    o->last_glyph_entries = o->last_row_entries = o->last_transfers = 0;
    wait_swap(o);
    int h = o->shown ^ 1;
    uint32_t *sg = o->glyphs[h], *sr = o->rows[h];
    int n = t->nglyphs;

    if (!o->valid[h]) {
        if (n) send_glyphs(o, h, 0, t->glyphs, n);
        send_rows(o, h, 0, t->rows, TANGO_ROW_WORDS / 2);
    } else {
        /* glyph list: entries 0..n-1 (entries past n are never referenced) */
        for (int i = 0; i < n;) {
            if (i < o->nglyphs[h] && sg[i] == t->glyphs[i]) { i++; continue; }
            int j = i + 1, last = i;
            while (j < n && j - last <= MERGE_GLYPHS + 1) {
                if (!(j < o->nglyphs[h] && sg[j] == t->glyphs[j])) last = j;
                j++;
            }
            send_glyphs(o, h, i, &t->glyphs[i], last - i + 1);
            i = last + 1;
        }
        for (int r = 0; r < TANGO_ROW_WORDS / 2;) {
            if (sr[2 * r] == t->rows[2 * r] && sr[2 * r + 1] == t->rows[2 * r + 1]) { r++; continue; }
            int j = r + 1, last = r;
            while (j < TANGO_ROW_WORDS / 2 && j - last <= MERGE_ROWS + 1) {
                if (sr[2 * j] != t->rows[2 * j] || sr[2 * j + 1] != t->rows[2 * j + 1]) last = j;
                j++;
            }
            send_rows(o, h, r, &t->rows[2 * r], last - r + 1);
            r = last + 1;
        }
    }
    memcpy(sg, t->glyphs, (size_t)n * 4);
    o->nglyphs[h] = (uint16_t)n;
    memcpy(sr, t->rows, sizeof t->rows);
    o->valid[h] = 1;

    begin(o, TANGO_OSD_SWAP); tx(o, (uint8_t)h); end(o);
    o->shown = (uint8_t)h;
    o->swapped = 1;
}

void tango_osd_enable(tango_osd_t *o, int on)
{
    begin(o, TANGO_OSD_ENABLE); tx(o, on ? 1 : 0); end(o);
}

/* ---- the core interface --------------------------------------------------------------------- */
static void lb(const tango_link_t *l, uint8_t t, uint8_t c) { l->begin(l->ctx); l->xfer(l->ctx, t); l->xfer(l->ctx, c); }

void tango_sys_set_status(const tango_link_t *l, uint8_t first, const uint8_t *b, int n)
{
    lb(l, TANGO_SPI_TARGET_SYS, TANGO_SYS_SET_STATUS);
    l->xfer(l->ctx, first);
    for (int i = 0; i < n; i++) l->xfer(l->ctx, b[i]);
    l->end(l->ctx);
}

void tango_sys_audvol(const tango_link_t *l, uint8_t vol)
{
    lb(l, TANGO_SPI_TARGET_SYS, TANGO_SYS_AUDVOL);
    l->xfer(l->ctx, vol);
    l->end(l->ctx);
}

uint16_t tango_sys_get_menumask(const tango_link_t *l)
{
    lb(l, TANGO_SPI_TARGET_SYS, TANGO_SYS_GET_MENUMASK);
    l->xfer(l->ctx, 0);
    uint16_t hi = l->xfer(l->ctx, 0), lo = l->xfer(l->ctx, 0);
    l->end(l->ctx);
    return (uint16_t)(hi << 8 | lo);
}

int tango_sys_read_confstr(const tango_link_t *l, char *buf, int cap)
{
    lb(l, TANGO_SPI_TARGET_SYS, TANGO_SPI_SYS_READ_CFG);
    l->xfer(l->ctx, 0);                             /* sysctrl.c sys_get_config: one dummy byte */
    int n = 0;
    while (n < cap - 1) {
        uint8_t c = l->xfer(l->ctx, 0);
        if (!c || c == 0xFF) break;
        buf[n++] = (char)c;
    }
    l->end(l->ctx);
    buf[n] = 0;
    return n;
}

void tango_ioctl_select(const tango_link_t *l, uint8_t idx, uint32_t size)
{
    lb(l, TANGO_SPI_TARGET_SDC, TANGO_SDC_IMAGE);
    l->xfer(l->ctx, 1); l->xfer(l->ctx, idx);
    for (int k = 3; k >= 0; k--) l->xfer(l->ctx, (uint8_t)(size >> (8 * k)));
    l->end(l->ctx);
}

int tango_ioctl_status(const tango_link_t *l, uint8_t idx, unsigned *free_bytes)
{
    lb(l, TANGO_SPI_TARGET_SDC, TANGO_SDC_IMAGE);
    l->xfer(l->ctx, 0); l->xfer(l->ctx, idx);
    uint8_t st = l->xfer(l->ctx, 0);
    unsigned f = l->xfer(l->ctx, 0); f = f << 8 | l->xfer(l->ctx, 0);
    l->end(l->ctx);
    if (free_bytes) *free_bytes = f > 512 ? 512 : f;
    return (st & 0x80) != 0;
}

void tango_ioctl_write(const tango_link_t *l, uint8_t idx, const uint8_t *d, unsigned n)
{
    lb(l, TANGO_SPI_TARGET_SDC, TANGO_SDC_IMAGE);
    l->xfer(l->ctx, 2); l->xfer(l->ctx, idx);
    for (unsigned i = 0; i < n; i++) l->xfer(l->ctx, d[i]);
    l->end(l->ctx);
}

void tango_ioctl_ul_select(const tango_link_t *l, uint8_t idx, uint32_t size)
{
    lb(l, TANGO_SPI_TARGET_SDC, TANGO_SDC_IMAGE);
    l->xfer(l->ctx, 4); l->xfer(l->ctx, idx);
    for (int k = 3; k >= 0; k--) l->xfer(l->ctx, (uint8_t)(size >> (8 * k)));
    l->end(l->ctx);
}

int tango_ioctl_ul_status(const tango_link_t *l, uint8_t idx, unsigned *avail)
{
    lb(l, TANGO_SPI_TARGET_SDC, TANGO_SDC_IMAGE);
    l->xfer(l->ctx, 5); l->xfer(l->ctx, idx);
    uint8_t st = l->xfer(l->ctx, 0);
    unsigned a = l->xfer(l->ctx, 0); a = a << 8 | l->xfer(l->ctx, 0);
    l->end(l->ctx);
    if (avail) *avail = a;
    return (st & 0x80) != 0;
}

void tango_ioctl_read(const tango_link_t *l, uint8_t idx, uint8_t *d, unsigned n)
{
    lb(l, TANGO_SPI_TARGET_SDC, TANGO_SDC_IMAGE);
    l->xfer(l->ctx, 3); l->xfer(l->ctx, idx);
    for (unsigned i = 0; i < n; i++) d[i] = l->xfer(l->ctx, 0);
    l->end(l->ctx);
}

uint8_t tango_hid_report(const tango_link_t *l, uint8_t port, uint16_t *vid, uint16_t *pid, uint32_t *raw)
{
    lb(l, TANGO_SPI_TARGET_HID, TANGO_HID_REPORT);
    l->xfer(l->ctx, port);
    uint8_t b[9];
    for (int i = 0; i < 9; i++) b[i] = l->xfer(l->ctx, 0);
    l->end(l->ctx);
    if (vid) *vid = (uint16_t)(b[1] << 8 | b[2]);
    if (pid) *pid = (uint16_t)(b[3] << 8 | b[4]);
    if (raw) *raw = (uint32_t)b[5] << 24 | (uint32_t)b[6] << 16 | (uint32_t)b[7] << 8 | b[8];
    return b[0];
}

void tango_hid_set_padmap(const tango_link_t *l, uint8_t port, uint8_t kind, uint8_t first, const uint8_t *pairs, int n)
{
    lb(l, TANGO_SPI_TARGET_HID, TANGO_HID_SET_PADMAP);
    l->xfer(l->ctx, port); l->xfer(l->ctx, kind);
    if (kind != 0xFE) {
        l->xfer(l->ctx, first);
        for (int i = 0; i < 2 * n; i++) l->xfer(l->ctx, pairs[i]);
    }
    l->end(l->ctx);
}

void tango_hid_report2(const tango_link_t *l, uint8_t port, tango_report2_t *r)
{
    lb(l, TANGO_SPI_TARGET_HID, TANGO_HID_REPORT2);
    l->xfer(l->ctx, port);
    uint8_t b[29];
    for (int i = 0; i < 29; i++) b[i] = l->xfer(l->ctx, 0);
    l->end(l->ctx);
    r->flags = b[0];
    r->vid = (uint16_t)(b[1] << 8 | b[2]); r->pid = (uint16_t)(b[3] << 8 | b[4]); r->bcd = (uint16_t)(b[5] << 8 | b[6]);
    r->raw = (uint32_t)b[7] << 24 | (uint32_t)b[8] << 16 | (uint32_t)b[9] << 8 | b[10];
    r->len = b[11];
    for (int i = 0; i < TANGO_REPORT2_BYTES; i++) r->rpt[i] = b[12 + i];
    r->pstat = b[28];
}

void tango_hid_desc_select(const tango_link_t *l, uint8_t port, uint8_t sel)
{
    lb(l, TANGO_SPI_TARGET_HID, TANGO_HID_DESC_SELECT);
    l->xfer(l->ctx, port); l->xfer(l->ctx, sel);
    l->end(l->ctx);
}

void tango_hid_descriptor(const tango_link_t *l, uint8_t port, uint8_t *status, uint16_t *stored_len, uint8_t *d, unsigned n)
{
    lb(l, TANGO_SPI_TARGET_HID, TANGO_HID_DESCRIPTOR);
    l->xfer(l->ctx, port);
    uint8_t st = l->xfer(l->ctx, 0);
    l->xfer(l->ctx, 0);                              /* b4 reserved */
    uint8_t hi = l->xfer(l->ctx, 0), lo = l->xfer(l->ctx, 0);
    for (unsigned i = 0; i < n; i++) d[i] = l->xfer(l->ctx, 0);
    l->end(l->ctx);
    if (status) *status = st;
    if (stored_len) *stored_len = (uint16_t)(hi << 8 | lo);
}

void tango_hid_set_decode(const tango_link_t *l, uint8_t port, const uint8_t *img, unsigned n)
{
    lb(l, TANGO_SPI_TARGET_HID, TANGO_HID_SET_DECODE);
    l->xfer(l->ctx, port);
    for (unsigned i = 0; i < n; i++) l->xfer(l->ctx, img[i]);
    l->end(l->ctx);
}

void tango_hid_decode_status(const tango_link_t *l, uint8_t port, tango_decstat_t *s)
{
    lb(l, TANGO_SPI_TARGET_HID, TANGO_HID_DECODE_STATUS);
    l->xfer(l->ctx, port);
    uint8_t b[7];
    for (int i = 0; i < 7; i++) b[i] = l->xfer(l->ctx, 0);
    l->end(l->ctx);
    s->sig = b[0]; s->mode = b[2]; s->count = b[3]; s->flags = b[4]; s->xfers = b[5]; s->max_entries = b[6];
}

void tango_hid_pad_ctrl(const tango_link_t *l, uint8_t bits)
{
    lb(l, TANGO_SPI_TARGET_HID, TANGO_HID_PAD_CTRL);
    l->xfer(l->ctx, bits);
    l->end(l->ctx);
}

uint8_t tango_hid_kbd_report(const tango_link_t *l, uint8_t port, uint8_t rep[8])
{
    lb(l, TANGO_SPI_TARGET_HID, TANGO_HID_KBD_REPORT);
    l->xfer(l->ctx, port);
    uint8_t f = l->xfer(l->ctx, 0);
    for (int i = 0; i < 8; i++) rep[i] = l->xfer(l->ctx, 0);
    l->end(l->ctx);
    return f;
}

uint8_t tango_sdc_status(const tango_link_t *l, uint32_t *lba, int *write, uint8_t *drive)
{
    lb(l, TANGO_SPI_TARGET_SDC, TANGO_SDC_STATUS);
    l->xfer(l->ctx, 0);                                     /* byte 2: 00 = card not busy */
    uint8_t b[8];
    for (int i = 0; i < 8; i++) b[i] = l->xfer(l->ctx, 0);  /* bytes 3-10 */
    l->end(l->ctx);
    if (lba) *lba = (uint32_t)b[1] << 24 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 8 | b[4];
    if (write) *write = b[6] & 1;
    if (drive) *drive = b[7];
    return b[0];
}

void tango_sdc_insert(const tango_link_t *l, uint8_t drive, int readonly, uint64_t size)
{
    lb(l, TANGO_SPI_TARGET_SDC, TANGO_SDC_INS_LARGE);
    l->xfer(l->ctx, (uint8_t)(drive | (readonly ? 0x80 : 0)));
    for (int k = 7; k >= 0; k--) l->xfer(l->ctx, (uint8_t)(size >> (8 * k)));
    l->end(l->ctx);
}

void tango_sdc_sector_in(const tango_link_t *l, uint8_t drive, const uint8_t buf[512])
{
    lb(l, TANGO_SPI_TARGET_SDC, TANGO_SDC_SECTOR_IN);
    l->xfer(l->ctx, drive);
    for (int i = 0; i < 512; i++) l->xfer(l->ctx, buf[i]);
    l->end(l->ctx);
}

void tango_sdc_sector_out(const tango_link_t *l, uint8_t drive, uint8_t buf[512])
{
    lb(l, TANGO_SPI_TARGET_SDC, TANGO_SDC_SECTOR_OUT);
    l->xfer(l->ctx, drive);
    for (int i = 0; i < 512; i++) buf[i] = l->xfer(l->ctx, 0);   /* bytes 3-514 */
    l->end(l->ctx);
}
