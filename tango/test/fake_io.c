// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 the Tango authors
/* fake_io.c -- see fake_io.h. Tango, our own code. */
#include <string.h>
#include "fake_io.h"

static void rl_begin(void *ctx) { rec_link_t *r = ctx; r->n = 0; }
static uint8_t rl_xfer(void *ctx, uint8_t b)
{
    rec_link_t *r = ctx;
    int i = r->n;
    if (r->n < (int)sizeof r->buf) r->buf[r->n++] = b;
    uint8_t t = r->buf[0], c = i >= 1 ? r->buf[1] : 0;
    if (i < 3) return 0;
    if (t == 2 && c == TANGO_OSD_CAPS) {
        static const uint8_t caps[TANGO_CAPS_LEN] = { 'Z', 'O', 1, 0x04, 0x00, 32, 0x02, 0x00, 32, 0x05, 0x00, 0x02, 0xD0 };
        return (r->caps_ok && i - 3 < TANGO_CAPS_LEN) ? caps[i - 3] : 0;
    }
    if (t == 2 && c == TANGO_OSD_STATUS) return (uint8_t)(r->shown & 1);    /* swaps complete at once */
    if (t == 0 && c == TANGO_SYS_GET_MENUMASK) return i == 3 ? (uint8_t)(r->menumask >> 8) : i == 4 ? (uint8_t)r->menumask : 0;
    if (t == 1 && r->hid_xfer) { uint8_t rp = 0; if (r->hid_xfer(r->hid_ctx, r->buf, i, &rp)) return rp; }
    if (t == 1 && c == 4) return i == 3 ? (uint8_t)r->joy : i == 4 ? (uint8_t)(r->joy >> 8) : 0;
    if (t == TANGO_SPI_TARGET_SDC && c == TANGO_SDC_IMAGE && r->buf[2] == 0) {   /* IMAGE STATUS, bytes 4-6 */
        if (i == 4) { unsigned s = r->dl_space ? r->dl_space - 1 : 0; r->dl_offer = s > 512 ? 512 : s; return (uint8_t)(r->dl_open << 7); }
        if (i == 5) return (uint8_t)(r->dl_offer >> 8);
        if (i == 6) return (uint8_t)r->dl_offer;
        return 0;
    }
    /* UL_STATUS (sub 5) -- byte 4 bit7 = ready (this fake: always ready the
     * instant ul_open is set, see fake_io.h); bytes 5-6 = the open size, big-endian (menu-tango docs/TANGO-SPEC.md's
     * own UL_STATUS shape, mirrored exactly, not the shrinking "free" count STATUS's own byte 5-6 is). */
    if (t == TANGO_SPI_TARGET_SDC && c == TANGO_SDC_IMAGE && r->buf[2] == 5 && !r->ul_unsupported) {
        if (i == 4) return (uint8_t)((r->ul_open && !r->ul_never_ready) << 7);
        if (i == 5) return (uint8_t)(r->ul_size >> 8);
        if (i == 6) return (uint8_t)r->ul_size;
        return 0;
    }
    /* READ (sub 3): bytes from i == 4 on are the session's own ul_buf, starting at its running cursor
     * (ul_off, advanced at the end of the transfer in rl_end -- never here, same "never from the reply's
     * own lookahead" rule the real RTL follows, menu-tango docs/TANGO-SPEC.md's own READ section). Reading past the
     * open size returns 00, matching the real RTL's own bound (checked against the session's global
     * position, not just this transfer's local byte count). */
    if (t == TANGO_SPI_TARGET_SDC && c == TANGO_SDC_IMAGE && r->buf[2] == 3 && !r->ul_unsupported) {
        if (i < 4) return 0;
        uint32_t pos = r->ul_off + (uint32_t)(i - 4);
        return (r->ul_open && pos < r->ul_size && pos < sizeof r->ul_buf) ? r->ul_buf[pos] : 0;
    }
    if (t == 0 && r->vp_has && i == 3 && (c == 0x2B || c == 0x32 || c == 0x39 || c == 0x3E))
        return c == 0x2B ? r->vp_flags : (c == 0x39 && r->vp_afreply) ? r->vp_afreply : 1;
    if (t == 0 && c == 0x70 && i == 3)                              /* sys-tango TANGO_VSYNC_ADJUST */
        return (r->buf[2] & 0x80) || r->vs_left <= 0 ? r->vs_reply : (uint8_t)(r->vs_reply | 0x04);
    if (t == 0 && c == TANGO_SPI_SYS_READ_CFG) {
        size_t k = (size_t)(i - 3);
        return (r->confstr && k < strlen(r->confstr)) ? (uint8_t)r->confstr[k] : 0;
    }
    return 0;
}
static void rl_end(void *ctx)
{
    rec_link_t *r = ctx;
    if (r->n >= 3 && r->buf[0] == 2 && r->buf[1] == TANGO_OSD_SWAP) r->shown = r->buf[2] & 1;
    if (r->n >= 3 && r->buf[0] == 0 && r->buf[1] == TANGO_SYS_SET_STATUS) {
        for (int k = 3; k < r->n; k++) if (r->buf[2] + k - 3 < 16) r->status[r->buf[2] + k - 3] = r->buf[k];
        r->status_writes++;
    }
    if (r->n >= 4 && r->buf[0] == TANGO_SPI_TARGET_SDC && r->buf[1] == TANGO_SDC_IMAGE) {
        if (r->buf[2] == 1 && r->n >= 8) {                       /* SELECT index size(BE32) */
            uint32_t sz = (uint32_t)r->buf[4] << 24 | (uint32_t)r->buf[5] << 16 | (uint32_t)r->buf[6] << 8 | r->buf[7];
            if (sz && !r->dl_open) { r->dl_open = 1; r->dl_index = r->buf[3]; r->dl_size = sz; r->dl_got = 0; r->dl_crc = 0xFFFFFFFFu; r->dl_starts++; }
            else if (!sz && r->dl_open) { r->dl_open = 0; r->dl_ends++; r->dl_crc ^= 0xFFFFFFFFu; }
        } else if (r->buf[2] == 2) {                             /* WRITE */
            for (int k = 4; k < r->n; k++) {
                if (!r->dl_open || !r->dl_offer) { r->dl_dropped++; continue; }
                r->dl_offer--;
                if (r->dl_got < sizeof r->dl_buf) r->dl_buf[r->dl_got] = r->buf[k];
                r->dl_got++;
                r->dl_crc ^= r->buf[k];
                for (int b = 0; b < 8; b++) r->dl_crc = (r->dl_crc >> 1) ^ (0xEDB88320u & (0u - (r->dl_crc & 1)));
            }
        /* UL_SELECT (sub 4) -- a plain open/close, no deferral behind an
         * open download (fake_io.h's own comment: proving tango_fw.c never relies on that, not re-modelling
         * the RTL's own deferral a second time). A UL_SELECT arriving WHILE a download is open is refused
         * outright (counted, not acted on) -- a real core would instead defer it; this fake flags the
         * attempt so a test can assert tango_fw.c's own arc_dl_idle() guard kept it from ever happening. */
        } else if (r->buf[2] == 4 && r->n >= 8 && !r->ul_unsupported) {   /* UL_SELECT index size(BE32) */
            uint32_t sz = (uint32_t)r->buf[4] << 24 | (uint32_t)r->buf[5] << 16 | (uint32_t)r->buf[6] << 8 | r->buf[7];
            if (r->dl_open) { r->ul_refused_while_dl++; }
            else if (sz && !r->ul_open) { r->ul_open = 1; r->ul_index = r->buf[3]; r->ul_size = sz; r->ul_off = 0; r->ul_opens++; }
            else if (!sz && r->ul_open) { r->ul_open = 0; r->ul_closes++; }
        } else if (r->buf[2] == 3 && r->ul_open) {               /* READ: advance the running cursor by the
                                                                   * real data bytes this transfer carried,
                                                                   * same "never from the reply's own
                                                                   * lookahead" rule the real RTL follows */
            uint32_t got = r->n > 4 ? (uint32_t)(r->n - 4) : 0;
            r->ul_off += got; r->ul_reads++;
        }
    }
    if (r->n >= 3 && r->buf[0] == 0 && r->buf[1] == TANGO_SYS_AUDVOL) { r->audvol = r->buf[2]; r->audvol_writes++; }
    if (r->n >= 3 && r->buf[0] == 0 && r->buf[1] == 0x70) {        /* retune: an apply starts the pending probes */
        if (r->buf[2] & 0x80) r->vs_left = r->vs_pending; else if (r->vs_left > 0) r->vs_left--;
    }
    if (r->n >= 2 && r->buf[0] == 1 && r->hid_end) r->hid_end(r->hid_ctx, r->buf, r->n);
    if (r->out) {
        uint8_t h[2] = { (uint8_t)r->n, (uint8_t)(r->n >> 8) };
        fwrite(h, 1, 2, r->out); fwrite(r->buf, 1, (size_t)r->n, r->out);
    }
    r->transfers++; r->bytes += r->n;
}
static void rl_delay(void *ctx, unsigned ms) { rec_link_t *r = ctx; r->delay_total += ms; if (r->delay_hook) r->delay_hook(r->delay_ctx, ms); }

void rec_link_init(rec_link_t *r, tango_link_t *link)
{
    memset(r, 0, sizeof *r);
    r->caps_ok = 1; r->confstr = "MENU;;"; r->dl_space = 2048;
    link->begin = rl_begin; link->xfer = rl_xfer; link->end = rl_end; link->delay_ms = rl_delay; link->ctx = r;
}

static int fc_open(void *ctx, const char *path)
{
    fake_card_t *c = ctx;
    /* the folder must exist: "" always; else some path starts with path + "/" */
    if (path[0]) {
        size_t l = strlen(path); int found = 0;
        for (int i = 0; i < c->npaths; i++)
            if (!strncmp(c->paths[i], path, l) && c->paths[i][l] == '/') { found = 1; break; }
        if (!found) return 0;
    }
    strncpy(c->dir, path, sizeof c->dir - 1); c->dir[sizeof c->dir - 1] = 0;
    c->pos = 0; c->opened = 1;
    return 1;
}
static int fc_read(void *ctx, char *name, int cap, int *is_dir)
{
    fake_card_t *c = ctx;
    size_t l = strlen(c->dir);
    for (; c->pos < c->npaths; c->pos++) {
        const char *p = c->paths[c->pos], *rest;
        if (l) { if (strncmp(p, c->dir, l) || p[l] != '/') continue; rest = p + l + 1; }
        else rest = p;
        const char *slash = strchr(rest, '/');
        if (!*rest) continue;
        if (slash && slash[1]) continue;              /* deeper than one level */
        size_t n = slash ? (size_t)(slash - rest) : strlen(rest);
        if ((int)n >= cap) n = (size_t)cap - 1;
        memcpy(name, rest, n); name[n] = 0;
        *is_dir = slash != NULL;
        c->pos++;
        return 1;
    }
    return 0;
}
static void fc_close(void *ctx) { ((fake_card_t *)ctx)->opened = 0; }
void fake_card_init(fake_card_t *c, tango_fs_t *fs, const char **paths, int n)
{
    memset(c, 0, sizeof *c);
    c->paths = paths; c->npaths = n;
    fs->open = fc_open; fs->read = fc_read; fs->close = fc_close; fs->ctx = c;
}
