// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 the Tango authors
/*
 * tango_vproc.c -- see tango_vproc.h. Tango, our own code; every rule cites the Main_MiSTer line it
 * was read from (master). No Main_MiSTer code is copied.
 * No static RAM here (tango_vproc.h MEMORY): scratch is on the caller's stack (the firmware's core task, 16 KB) or in
 * the core's tango_vproc_t, files in buffers of their own while they are parsed.
 */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "tango_vproc.h"

/* every allocation goes through these: the host tests count the bytes and refuse some (test_vproc.c) */
#ifdef VP_MALLOC
void *VP_MALLOC(size_t n);
void  VP_FREE(void *p);
#else
#define VP_MALLOC malloc
#define VP_FREE   free
#endif

#define SYS 0x00
#define C_FLTCOEF 0x2A
#define C_FLTNUM  0x2B
#define C_GAMMA   0x32
#define C_GAMCURV 0x33
#define C_AFILTER 0x39
#define C_SHMASK  0x3E
#define C_VSYNC   0x70      /* Tango's own: VSYNC_ADJUST; MiSTer has no such UIO */

/* ---------------------------------------------------------------- text as Main reads it */
/* FileReadLine (file_io.cpp 2173-2193): a line ends at CR or LF; leading space / tab / CR / LF skipped; lines that
 * then start with '#' or ';', or are empty, skipped; trailing whitespace kept. Returns the next line (in place,
 * NUL-terminated) or NULL. The buffer must have a NUL after len bytes. */
typedef struct { char *p, *end; } vp_rd_t;
static void rd_init(vp_rd_t *r, const uint8_t *buf, int len)
{
    r->p = (char *)buf; r->end = (char *)buf + len;
    char *z = memchr(r->p, 0, (size_t)len);           /* the text stops at a NUL, as Main's C strings */
    if (z) r->end = z;
}
static char *rd_line(vp_rd_t *r)
{
    while (r->p < r->end) {
        char *st = r->p;
        while (r->p < r->end && *r->p != '\r' && *r->p != '\n') r->p++;
        if (r->p < r->end) *r->p++ = 0; else *r->p = 0;
        while (*st == ' ' || *st == '\t' || *st == '\r' || *st == '\n') st++;
        if (*st && *st != '#' && *st != ';') return st;
    }
    return NULL;
}
static int ieq(const char *a, const char *b)
{
    for (;; a++, b++) {
        int x = (unsigned char)*a, y = (unsigned char)*b;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return 0;
        if (!x) return 1;
    }
}
static int ieqn(const char *a, const char *b, int n)
{
    for (int i = 0; i < n; i++) {
        int x = (unsigned char)a[i], y = (unsigned char)b[i];
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return 0;
        if (!x) return 1;
    }
    return 1;
}
/* a bounded string copy (dst always terminated) */
static void cpy(char *dst, int cap, const char *src)
{
    int i = 0;
    for (; i < cap - 1 && src[i]; i++) dst[i] = src[i];
    dst[i] = 0;
}
/* sscanf("%d,%d,...") with n conversions: values converted before the first mismatch */
static int scan_ints(const char *s, int n, long *v)
{
    int k = 0;
    for (; k < n; k++) {
        if (k) { if (*s != ',') break; s++; }
        while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n' || *s == '\v' || *s == '\f') s++;
        int neg = 0;
        if (*s == '+' || *s == '-') { neg = *s == '-'; s++; }
        if (*s < '0' || *s > '9') break;
        long x = 0;
        while (*s >= '0' && *s <= '9') { x = x * 10 + (*s - '0'); s++; }
        v[k] = neg ? -x : x;
    }
    return k;
}
static int hexv(int c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }
/* sscanf("%X,%x,...") */
static int scan_hex(const char *s, int n, unsigned long *v)
{
    int k = 0;
    for (; k < n; k++) {
        if (k) { if (*s != ',') break; s++; }
        while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
        int neg = 0;
        if (*s == '+' || *s == '-') { neg = *s == '-'; s++; }
        if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X') && hexv(s[2]) >= 0) s += 2;
        if (hexv(*s) < 0) break;
        unsigned long x = 0;
        while (hexv(*s) >= 0) { x = (x << 4) | (unsigned long)hexv(*s); s++; }
        v[k] = (neg ? (0UL - x) : x) & 0xFFFFFFFFUL;
    }
    return k;
}

/* ---------------------------------------------------------------- scaler filters (video.cpp 352-466) */
static int is_pow2_divisor(int n) { return n > 0 && n <= 256 && (256 % n) == 0; }   /* scale_phases' test (359-364) */

int vp_parse_filter(const uint8_t *buf, int len, vp_filter_t *f)
{
    memset(f, 0, sizeof *f);
    int count = 0, adaptive = 0, scale = 2;
    if (buf) {
        vp_rd_t r; rd_init(&r, buf, len);
        char *ln;
        while ((ln = rd_line(&r))) {
            if (count == 0 && ieq(ln, "adaptive")) { adaptive = 1; continue; }     /* 397-401 */
            if (count == 0 && ieq(ln, "10bit")) { scale = 1; continue; }           /* 403-407 */
            long v[4];
            if (scan_ints(ln, 4, v) == 4) {
                if (count >= (adaptive ? 512 : 256)) { f->too_many = 1; f->valid = 0; return 0; }   /* 413 */
                for (int t = 0; t < 4; t++) f->ph[count][t] = (int16_t)(v[t] * scale);   /* short (326-329) */
                count++;
            }
        }
    }
    int n, valid;
    if (adaptive) {                                       /* 429-434: first half normal, second half adaptive */
        n = count / 2; valid = is_pow2_divisor(n);
        f->adaptive = 1;
    } else if (count == 32) {                             /* 435-439: legacy H+V file: the first 16 phases */
        n = 16; valid = 1;
    } else {
        n = count; valid = is_pow2_divisor(n);
    }
    if (!valid) {                                         /* 446-456: the 2-phase nearest-neighbour filter */
        f->adaptive = 0; n = 2;
        static const int16_t nn[2][4] = { { 0, 256, 0, 0 }, { 0, 0, 256, 0 } };
        memcpy(f->ph, nn, sizeof nn);
    }
    f->n = n; f->valid = valid;
    return valid;
}

/* ---------------------------------------------------------------- gamma (video.cpp 699-743) */
int vp_parse_gamma(const uint8_t *buf, int len, uint8_t tab[256][3])
{
    vp_rd_t r; rd_init(&r, buf, len);
    char *ln; int idx = 0;
    while ((ln = rd_line(&r))) {
        long v[3];
        int n = scan_ints(ln, 3, v);
        if (n == 1) { v[1] = v[0]; v[2] = v[0]; n = 3; }   /* 722-727 */
        if (n == 3) {
            for (int c = 0; c < 3; c++) tab[idx][c] = (uint8_t)(v[c] & 0xFF);   /* 731-733: c & 0xFF */
            if (++idx >= 256) break;
        }
    }
    return idx;
}

/* ---------------------------------------------------------------- shadow masks (video.cpp 808-928) */
#define SM_FLAG(v) ((uint16_t)((0 << 13) | (v)))
#define SM_VMAX(v) ((uint16_t)((1 << 13) | (v)))
#define SM_HMAX(v) ((uint16_t)((2 << 13) | (v)))
#define SM_LUT(v)  ((uint16_t)((3 << 13) | (v)))
static uint16_t mask_flag(int mode)
{
    switch (mode) {                                      /* 841-848 */
    case VP_SM_1X:     return SM_FLAG(1 << 3);
    case VP_SM_2X:     return SM_FLAG((1 << 3) | (1 << 1));
    case VP_SM_1X_ROT: return SM_FLAG((1 << 3) | (1 << 2));
    case VP_SM_2X_ROT: return SM_FLAG((1 << 3) | (1 << 2) | (1 << 1));
    default:           return SM_FLAG(0);
    }
}

int vp_parse_mask(const uint8_t *buf, int len, int mode, int height, uint16_t *w, int cap, int *loaded)
{
    int nw = 0, ok = 0;
    w[nw++] = mask_flag(mode);
    /* the lines, once, so the resolution= pass and the mask pass see the same list (at most 2048, as always; the
     * list is as long as the text has line ends, in memory of its own: none = as without the file) */
    vp_rd_t r = { NULL, NULL }; char **lines = NULL; int maxl = 1;
    if (buf) {
        rd_init(&r, buf, len);
        for (const char *q = r.p; q < r.end; q++) if (*q == '\r' || *q == '\n') maxl++;
        if (maxl > 2048) maxl = 2048;
        lines = VP_MALLOC((size_t)maxl * sizeof *lines);
    }
    if (lines) {
        int nl = 0;
        char *ln;
        while (nl < maxl && (ln = rd_line(&r))) lines[nl++] = ln;
        int start = 0;
        for (int i = 0; i < nl; i++)                      /* 859-871: the last resolution= section with N <= height */
            if (ieqn(lines[i], "resolution=", 11)) {
                long res;
                if (scan_ints(lines[i] + 11, 1, &res) == 1 && height >= res) start = i + 1;
            }
        int wd = -1, ht = -1, y = 0, v2 = 0;
        for (int i = start; i < nl; i++) {
            ln = lines[i];
            if (wd == -1) {
                if (ieq(ln, "v2")) { v2 = 1; continue; }              /* 882-886 */
                if (ieqn(ln, "resolution=", 11)) continue;           /* 888-891 */
                long v[2];
                if (scan_ints(ln, 2, v) != 2 || v[0] <= 0 || v[1] <= 0 || v[0] > 16 || v[1] > 16) break;   /* 893-897 */
                wd = (int)v[0]; ht = (int)v[1];
            } else {
                unsigned long p[16];
                int n = scan_hex(ln, 16, p);
                if (n != wd) break;                                   /* 903-906 */
                for (int x = 0; x < 16; x++) {                        /* 908: 16 per row; x >= w: 0 here */
                    unsigned long e = x < wd ? (v2 ? (p[x] & 0x7FF) : (((p[x] & 7) << 8) | 0x2A)) : 0;
                    if (nw < cap) w[nw++] = SM_LUT(e);
                }
                if (++y == ht) { ok = 1; break; }
            }
        }
        if (ok && nw + 2 <= cap) { w[nw++] = SM_HMAX(wd - 1); w[nw++] = SM_VMAX(ht - 1); }   /* 919-923 */
        VP_FREE(lines);
    }
    if (!ok && nw < cap) w[nw++] = SM_FLAG(0);           /* 926 */
    *loaded = ok;
    return nw;
}

/* ---------------------------------------------------------------- audio filters (audio.cpp 23-108) */
/* %lg as Main's sscanf: a decimal float (sign, digits, '.', exponent) */
static int scan_double(const char *s, double *d)
{
    char *e;
    while (*s == ' ' || *s == '\t' || *s == '\r') s++;
    double x = strtod(s, &e);
    if (e == s) return 0;
    *d = x; return 1;
}

int vp_parse_afilter(const uint8_t *buf, int len, uint8_t *out, int cap)
{
    int n = 0, line = 0;
    const char *p = (const char *)buf, *end = (const char *)buf + len;
    char tmp[512];
    while (p < end && line < 9) {                         /* 61-104: lines end at LF; leading ' ' '\t' CR skipped */
        const char *st = p;
        while (p < end && *p && *p != '\n') p++;
        int l = (int)(p - st); if (l > (int)sizeof tmp - 1) l = sizeof tmp - 1;
        memcpy(tmp, st, (size_t)l); tmp[l] = 0;
        if (p < end && !*p) { p = end; } else p++;
        char *s = tmp;
        while (*s == ' ' || *s == '\t' || *s == 13) s++;
        if (*s == '#' || *s == ';' || !*s) continue;
        if (line == 0) {
            if (!ieqn(s, "v1", 2)) break;                 /* 77 */
            line++;
        } else if (line == 1 || line == 3 || line == 4 || line == 5) {
            long v;
            if (scan_ints(s, 1, &v) == 1) {
                if (line == 1) {                          /* rate: 2 words on the HPS bus, 4 bytes here */
                    if (n + 4 <= cap) { uint32_t u = (uint32_t)v; out[n++] = (uint8_t)(u >> 24); out[n++] = (uint8_t)(u >> 16);
                                        out[n++] = (uint8_t)(u >> 8); out[n++] = (uint8_t)u; }
                } else if (n < cap) out[n++] = (uint8_t)v;   /* cx0..2: the low byte of (uint16_t)val */
                line++;
            }
        } else if (line == 2) {
            double d;
            if (scan_double(s, &d)) {                     /* 92-99: (int64_t)(0x8000000000 * val), 40 bits used */
                uint64_t c = (uint64_t)(int64_t)(549755813888.0 * d);
                if (n + 5 <= cap) for (int k = 4; k >= 0; k--) out[n++] = (uint8_t)(c >> (8 * k));
                line++;
            }
        } else {
            double d;
            if (scan_double(s, &d)) {                     /* 101-107: (int32_t)(0x200000 * val), 24 bits used */
                uint32_t c = (uint32_t)(int32_t)(2097152.0 * d);
                if (n + 3 <= cap) { out[n++] = (uint8_t)(c >> 16); out[n++] = (uint8_t)(c >> 8); out[n++] = (uint8_t)c; }
                line++;
            }
        }
    }
    return n;
}

/* ---------------------------------------------------------------- presets (video.cpp 1007-1125) */
void vp_parse_preset(const uint8_t *buf, int len, void (*cb)(void *ctx, const char *key, const char *val), void *ctx)
{
    static const char *const KEYS[] = { "hfilter=", "vfilter=", "sfilter=", "ifilter=", "mask=", "maskmode=", "gamma=", "afilter=" };
    vp_rd_t r; rd_init(&r, buf, len);
    char *ln;
    while ((ln = rd_line(&r))) {
        for (int k = 0; k < (int)(sizeof KEYS / sizeof KEYS[0]); k++) {
            int kl = (int)strlen(KEYS[k]);
            if (!ieqn(ln, KEYS[k], kl)) continue;
            char *a = ln + kl;                            /* get_preset_arg (1010-1026): trailing whitespace off */
            int l = (int)strlen(a);
            while (l > 0 && (a[l - 1] == ' ' || a[l - 1] == '\t' || a[l - 1] == '\r' || a[l - 1] == '\n')) a[--l] = 0;
            if (a[0]) { char key[16]; memcpy(key, KEYS[k], (size_t)kl - 1); key[kl - 1] = 0; cb(ctx, key, a); }
            break;
        }
    }
}

/* ---------------------------------------------------------------- Tango.ini (MiSTer.ini's rules, cfg.cpp 157-260, 350-510) */
static int ini_valid(int c)                               /* CHAR_IS_VALID + CHAR_IS_SPACE (160-171) */
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ' || c == '\t' ||
           strchr("[]()-+/=#$@_,.!*:~", c) != NULL;
}
void vp_parse_ini(const uint8_t *buf, int len, const char *core, vp_ini_t *ini)
{
    memset(ini, 0, sizeof *ini);
    ini->vsync_adjust = -1;                               /* not in the file: nothing is sent (the core's default) */
    if (!buf) return;
    int i = 0, section = 0;
    char line[1024];
    while (i < len) {
        int n = 0, ignore = 0, skip = 1;                  /* ini_getline (190-207) */
        while (i < len && buf[i]) {
            int c = buf[i++];
            if (c != ' ' && c != '\t') skip = 0;
            if (n >= (int)sizeof line - 1 || c == ';') ignore = 1;
            if (c == '\n') break;
            if (ini_valid(c) && !ignore && !skip) line[n++] = (char)c;
        }
        line[n] = 0;
        while (n > 0 && (line[n - 1] == ' ' || line[n - 1] == '\t')) line[--n] = 0;
        if (line[0] == '[') {                              /* ini_get_section (210-260): MiSTer, or the core's name */
            char *e = strchr(line, ']'); if (e) *e = 0;
            const char *nm = line + 1;
            char *wc = strchr(line + 1, '*');
            section = ieq(nm, "MiSTer") || ieq(nm, "Tango") ||
                      (wc ? ieqn(nm, core, (int)(wc - nm)) : ieq(nm, core));
        } else if (section && line[0]) {                   /* ini_parse_var (350-450) */
            char *v = line;
            while (*v && *v != '=' && *v != ' ' && *v != '\t') v++;
            if (!*v) continue;
            *v++ = 0;
            while (*v == '=' || *v == ' ' || *v == '\t') v++;
            struct { const char *k; char *dst; } S[] = {
                { "AFILTER_DEFAULT", ini->afilter_default }, { "VFILTER_DEFAULT", ini->vfilter_default },
                { "VFILTER_VERTICAL_DEFAULT", ini->vfilter_vertical_default },
                { "VFILTER_SCANLINES_DEFAULT", ini->vfilter_scanlines_default },
                { "VFILTER_INTERLACE_DEFAULT", ini->vfilter_interlace_default },
                { "SHMASK_DEFAULT", ini->shmask_default }, { "PRESET_DEFAULT", ini->preset_default } };
            for (int k = 0; k < (int)(sizeof S / sizeof S[0]); k++)
                if (ieq(line, S[k].k)) { cpy(S[k].dst, (int)(1024), v); }
            if (ieq(line, "SHMASK_MODE_DEFAULT")) ini->shmask_mode_default = atoi(v);
            /* cfg.cpp 48 VSYNC_ADJUST UINT8 0..2, 70-71 REFRESH_MIN / REFRESH_MAX FLOAT (later sections win, as there) */
            if (ieq(line, "VSYNC_ADJUST")) { int a = atoi(v); ini->vsync_adjust = a < 0 ? 0 : a > 2 ? 2 : a; }
            if (ieq(line, "REFRESH_MIN")) { double d; if (scan_double(v, &d) && d >= 0) ini->refresh_min = d; }
            if (ieq(line, "REFRESH_MAX")) { double d; if (scan_double(v, &d) && d >= 0) ini->refresh_max = d; }
        }
    }
}

/* ---------------------------------------------------------------- the link */
static void xfer(tango_vproc_t *v, const uint8_t *b, int n)
{
    const tango_link_t *l = v->link;
    l->begin(l->ctx);
    for (int i = 0; i < n; i++) l->xfer(l->ctx, b[i]);
    l->end(l->ctx);
}
/* a probe: 00 cmd arg, then byte 3 read (the core's answer; 00 = no such block) */
static uint8_t probe_link(const tango_link_t *l, uint8_t cmd, uint8_t arg)
{
    l->begin(l->ctx);
    l->xfer(l->ctx, SYS); l->xfer(l->ctx, cmd); l->xfer(l->ctx, arg);
    uint8_t r = l->xfer(l->ctx, 0);
    l->end(l->ctx);
    return r;
}
static uint8_t probe(tango_vproc_t *v, uint8_t cmd, uint8_t arg) { return probe_link(v->link, cmd, arg); }

int vp_probe(const tango_link_t *link, uint8_t vol, vp_caps_t *c)
{
    memset(c, 0, sizeof *c);
    c->vol = vol;
    c->has_gamma = probe_link(link, C_GAMMA, 0) != 0;
    c->has_mask  = probe_link(link, C_SHMASK, 0) != 0;      /* (sends word 0000: mask off until vp_send_mask) */
    {   uint8_t a = probe_link(link, C_AFILTER, vol);        /* (restores the defaults, as Main's probe does) */
        c->has_afilter = a != 0;
        c->has_boost   = (a & 2) != 0;                       /* audio.cpp 33-34: the volume byte's boost bits work */
    }
    c->flags      = probe_link(link, C_FLTNUM, 0);
    c->has_scaler = c->flags != 0;
    c->vsync_status = probe_link(link, C_VSYNC, 0);          /* n0 = 0: bit 7 (apply) clear, a probe changes nothing */
    c->has_vsync    = (c->vsync_status & 0x80) != 0;
    return c->has_scaler || c->has_gamma || c->has_mask || c->has_afilter || c->has_vsync;
}

uint8_t *vp_load(const vp_io_t *io, const char *path, int cap, int *len)
{
    *len = 0;
    if (!io || !io->read_file || !path || !path[0]) return NULL;
    int sz = io->read_file(io->ctx, path, NULL, 0);        /* the size first: the buffer is the file's, not a maximum */
    if (sz < 0) return NULL;
    if (sz > cap) sz = cap;
    uint8_t *b = VP_MALLOC((size_t)sz + 1);
    if (!b) return NULL;
    int n = io->read_file(io->ctx, path, b, sz);
    if (n < 0 || n > sz) { VP_FREE(b); return NULL; }
    b[n] = 0;
    *len = n;
    return b;
}
/* dir/name of the card, as vp_load (VP_FILE_MAX); a file that exists but finds no memory counts in nomem */
static uint8_t *load_rel(tango_vproc_t *v, const char *dir, const char *name, int *len)
{
    *len = 0;
    if (!name || !name[0] || !v->io || !v->io->read_file) return NULL;
    char path[1200];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    uint8_t *b = vp_load(v->io, path, VP_FILE_MAX, len);
    if (!b && v->io->read_file(v->io->ctx, path, NULL, 0) >= 0) v->nomem++;
    return b;
}

const char *vp_short(const char *path)
{
    const char *p = strrchr(path, '/');
    return p ? p + 1 : path;
}

/* ---- gamma: setGamma (699-743): the file's lines as GAMCURV, then SET_GAMMA with the on/off; no file: nothing */
void vp_send_gamma(tango_vproc_t *v)
{
    int n;
    uint8_t *f = load_rel(v, VP_GAMMA_DIR, v->gamma + 1, &n);
    if (!f) return;
    uint8_t *b = v->xbuf;                                 /* the curve is parsed into its transfer: 00 33 00, then R G B */
    int cnt = vp_parse_gamma(f, n, (uint8_t (*)[3])(b + 3));
    VP_FREE(f);
    b[0] = SYS; b[1] = C_GAMCURV; b[2] = 0;
    xfer(v, b, 3 + 3 * cnt);
    b[0] = SYS; b[1] = C_GAMMA; b[2] = (uint8_t)v->gamma[0];
    xfer(v, b, 3);
}

/* ---- scaler: setScaler -> set_vfilter (549-576) + send_video_filters (505-547) */
/* one slot's file into f (the NN filter when off or missing); a file with too many phases clears the slot (413 +
 * 689-692) */
static void load_slot(tango_vproc_t *v, int type, vp_filter_t *f)
{
    int n = 0;
    uint8_t *b = v->flt[type].mode ? load_rel(v, VP_COEFF_DIR, v->flt[type].name, &n) : NULL;
    vp_parse_filter(b, n, f);
    VP_FREE(b);
    if (f->too_many) {
        memset(&v->flt[type], 0, sizeof v->flt[type]);
        vp_parse_filter(NULL, 0, f);
    }
    v->adaptive[type] = f->adaptive;
}
static void send_set(tango_vproc_t *v, int set, const vp_filter_t *f, int adaptive)
{
    uint8_t *b = v->xbuf; int k = 0;
    int n = f->n, lg = 0;
    while ((1 << lg) < n) lg++;
    b[k++] = SYS; b[k++] = C_FLTCOEF; b[k++] = (uint8_t)set; b[k++] = (uint8_t)lg;
    for (int p = 0; p < n; p++)
        for (int t = 0; t < 4; t++) {
            uint16_t c = (uint16_t)(f->ph[adaptive ? n + p : p][t] & 0x3FF);   /* & 0x3FF (send_phases 495-498) */
            b[k++] = (uint8_t)(c >> 8); b[k++] = (uint8_t)c;
        }
    xfer(v, b, k);
}
/* the V set for the scanline level sl (set_vfilter 569-573; Tango has no interlaced input) */
static int pick_vset(tango_vproc_t *v, int sl)
{
    if (sl && v->flt[VP_SCAN].mode) return VP_SCAN;
    if (v->flt[VP_VERT].mode) return VP_VERT;
    return VP_HORZ;
}
void vp_send_scaler_level(tango_vproc_t *v, int sl)
{
    /* every slot's file is read (each may clear its slot), but only two sets are held: H, and the V set in use (the
     * slot last read, or read again: one more card read instead of 8 KB more memory) */
    vp_filter_t *fs = VP_MALLOC(2 * sizeof *fs);
    if (!fs) { v->nomem++; return; }                      /* the core keeps the coefficients it has */
    load_slot(v, VP_ILACE, &fs[1]);                       /* (Tango has no interlaced input: its slot's rule only) */
    load_slot(v, VP_SCAN, &fs[1]);
    load_slot(v, VP_VERT, &fs[1]);
    load_slot(v, VP_HORZ, &fs[0]);
    int vs = pick_vset(v, sl);
    if (vs == VP_SCAN) load_slot(v, VP_SCAN, &fs[1]);
    int hmode = v->flt[VP_HORZ].mode ? 1 : 0;
    int scan = hmode && vs == VP_SCAN;
    uint8_t b[3] = { SYS, C_FLTNUM, (uint8_t)(hmode | scan << 1) };
    xfer(v, b, 3);
    const vp_filter_t *H = &fs[0], *V = vs == VP_HORZ ? &fs[0] : &fs[1];
    send_set(v, 0, H, 0);
    send_set(v, 1, V, 0);
    if (H->adaptive) send_set(v, 2, H, 1);
    else if (V->adaptive) send_set(v, 3, V, 1);
    v->vset = vs;
    VP_FREE(fs);
}
void vp_send_scaler(tango_vproc_t *v)
{
    if (!v->has_scaler) return;
    int vs = pick_vset(v, 0);
    int hmode = v->flt[VP_HORZ].mode ? 1 : 0;
    v->flags = probe(v, C_FLTNUM, (uint8_t)(hmode | ((hmode && vs == VP_SCAN) << 1)));   /* flags: the level in use */
    vp_send_scaler_level(v, (v->flags >> 4) & 3);
}

/* ---- shadow mask: setShadowMask (827-928) */
void vp_send_mask(tango_vproc_t *v)
{
    if (!v->has_mask) return;
    uint16_t w[2 + 256 + 3];                              /* (the transfer, 2 + 2 x 261 bytes, fits xbuf) */
    int n;
    uint8_t *f = load_rel(v, VP_SMASK_DIR, v->mask + 1, &n);
    int loaded;
    int nw = vp_parse_mask(f, n, v->mask[0], v->height, w, (int)(sizeof w / sizeof w[0]), &loaded);
    VP_FREE(f);
    uint8_t *b = v->xbuf;
    int k = 0;
    b[k++] = SYS; b[k++] = C_SHMASK;
    for (int i = 0; i < nw; i++) { b[k++] = (uint8_t)(w[i] >> 8); b[k++] = (uint8_t)w[i]; }
    xfer(v, b, k);
}

/* ---- audio filter: setFilter (23-108): Custom + a file = its values, else the core volume only (= the defaults) */
void vp_send_afilter(tango_vproc_t *v)
{
    if (!v->has_afilter) return;
    uint8_t *b = v->xbuf; int k = 0;
    b[k++] = SYS; b[k++] = C_AFILTER; b[k++] = v->vol;
    int n = 0;
    uint8_t *f = v->afilt[0] ? load_rel(v, VP_AFILTER_DIR, v->afilt + 1, &n) : NULL;
    if (f) k += vp_parse_afilter(f, n, b + k, (int)sizeof v->xbuf - k);
    VP_FREE(f);
    xfer(v, b, k);
}

/* ---------------------------------------------------------------- saved state */
static void cfgname(tango_vproc_t *v, const char *what, char *out, int cap) { snprintf(out, (size_t)cap, "%s_%s.cfg", v->core, what); }
static void save(tango_vproc_t *v, const char *what, const void *d, int n)
{
    if (!v->io || !v->io->save_cfg) return;
    char nm[64]; cfgname(v, what, nm, sizeof nm);
    v->io->save_cfg(v->io->ctx, nm, d, n);
}
static int load(tango_vproc_t *v, const char *what, void *d, int n)
{
    if (!v->io || !v->io->load_cfg) return -1;
    char nm[64]; cfgname(v, what, nm, sizeof nm);
    return v->io->load_cfg(v->io->ctx, nm, d, n);
}
static void save_scaler(tango_vproc_t *v) { save(v, "scaler", v->flt, (int)sizeof v->flt); }
static void save_gamma(tango_vproc_t *v)  { save(v, "gamma", v->gamma, (int)sizeof v->gamma); }
static void save_mask(tango_vproc_t *v)   { save(v, "shmask", v->mask, (int)sizeof v->mask); }
static void save_af(tango_vproc_t *v)     { save(v, "afilter", v->afilt, (int)sizeof v->afilt); }

/* ---------------------------------------------------------------- presets (video_loadPreset 1045-1125) */
typedef struct { tango_vproc_t *v; int sc, mk, gm, af; } preset_ctx_t;
static void preset_key(void *ctx, const char *key, const char *a)
{
    preset_ctx_t *c = ctx; tango_vproc_t *v = c->v;
    int t = !strcmp(key, "hfilter") ? VP_HORZ : !strcmp(key, "vfilter") ? VP_VERT : !strcmp(key, "sfilter") ? VP_SCAN :
            !strcmp(key, "ifilter") ? VP_ILACE : -1;
    if (t >= 0) {                                        /* load_flt_pres (1028-1043) */
        if (ieq(a, "same") || ieq(a, "off")) v->flt[t].mode = 0;
        else { cpy(v->flt[t].name, (int)(sizeof v->flt[t].name), a); v->flt[t].mode = 1; }
        c->sc = 1;
    } else if (!strcmp(key, "mask")) {
        if (ieq(a, "off") || ieq(a, "none")) v->mask[0] = 0;
        else cpy(v->mask + 1, (int)(VP_CFG_LEN - 1), a);
        c->mk = 1;
    } else if (!strcmp(key, "maskmode")) {
        if (ieq(a, "off") || ieq(a, "none")) v->mask[0] = 0;
        else if (ieq(a, "1x")) v->mask[0] = VP_SM_1X;
        else if (ieq(a, "2x")) v->mask[0] = VP_SM_2X;
        else if (ieq(a, "1x rotated")) v->mask[0] = VP_SM_1X_ROT;
        else if (ieq(a, "2x rotated")) v->mask[0] = VP_SM_2X_ROT;
        c->mk = 1;
    } else if (!strcmp(key, "gamma")) {
        if (ieq(a, "off") || ieq(a, "none")) v->gamma[0] = 0;
        else { cpy(v->gamma + 1, (int)(VP_CFG_LEN - 1), a); v->gamma[0] = 1; }
        c->gm = 1;
    } else if (!strcmp(key, "afilter")) {                /* Tango's addition: the audio filter in a preset */
        if (ieq(a, "off") || ieq(a, "none")) v->afilt[0] = 0;
        else { cpy(v->afilt + 1, (int)(VP_CFG_LEN - 1), a); v->afilt[0] = 1; }
        c->af = 1;
    }
}
static int apply_preset_c(tango_vproc_t *v, const char *path, preset_ctx_t *c)
{
    memset(c, 0, sizeof *c); c->v = v;
    if (!v->io || !v->io->read_file) return -1;
    int n;
    uint8_t *b = vp_load(v->io, path, 4095, &n);          /* a preset's first 4095 bytes (as always) */
    if (!b) { if (v->io->read_file(v->io->ctx, path, NULL, 0) >= 0) v->nomem++; return -1; }
    vp_parse_preset(b, n, preset_key, c);
    VP_FREE(b);
    return 0;
}

int vp_apply_preset(tango_vproc_t *v, const char *path)
{
    preset_ctx_t c;
    if (apply_preset_c(v, path, &c)) return -1;
    return c.sc | c.mk << 1 | c.gm << 2 | c.af << 3;
}

int vp_load_preset(tango_vproc_t *v, const char *path, int do_save)
{
    preset_ctx_t c;
    if (apply_preset_c(v, path, &c)) return -1;
    if (c.gm) vp_send_gamma(v);
    if (c.sc) vp_send_scaler(v);
    if (c.mk) vp_send_mask(v);
    if (c.af) vp_send_afilter(v);
    if (do_save) {
        if (c.sc) save_scaler(v);
        if (c.mk) save_mask(v);
        if (c.gm) save_gamma(v);
        if (c.af) save_af(v);
    }
    return 0;
}

/* ---------------------------------------------------------------- core start */
static void cfg_defaults(tango_vproc_t *v)
{
    memset(v->flt, 0, sizeof v->flt); memset(v->gamma, 0, sizeof v->gamma);
    memset(v->mask, 0, sizeof v->mask); memset(v->afilt, 0, sizeof v->afilt);
    if (v->ini.preset_default[0]) {                       /* video_cfg_init (2659-2675): PRESET_DEFAULT first */
        char p[1200]; preset_ctx_t c;
        int l = snprintf(p, sizeof p, "%s/%s", VP_PRESET_DIR, v->ini.preset_default);
        if (l < 4 || !ieq(p + l - 4, ".ini")) strncat(p, ".ini", sizeof p - strlen(p) - 1);
        apply_preset_c(v, p, &c);
    }
}
static void load_saved(tango_vproc_t *v)
{
    /* loadGammaCfg (793-803) */
    if (load(v, "gamma", v->gamma, VP_CFG_LEN - 1) > 0 && v->gamma[0] > 1) memset(v->gamma, 0, sizeof v->gamma);
    /* loadScalerCfg (651-693) */
    if (load(v, "scaler", v->flt, (int)sizeof v->flt) > 0) {
        if (v->flt[0].mode > 1) memset(v->flt, 0, sizeof v->flt);
    } else {
        struct { int t; const char *d; } D[] = { { VP_HORZ, v->ini.vfilter_default }, { VP_VERT, v->ini.vfilter_vertical_default },
                                                 { VP_SCAN, v->ini.vfilter_scanlines_default }, { VP_ILACE, v->ini.vfilter_interlace_default } };
        for (int i = 0; i < 4; i++)
            if (D[i].d[0]) { cpy(v->flt[D[i].t].name, (int)(sizeof v->flt[0].name), D[i].d); v->flt[D[i].t].mode = 1; }
    }
    /* loadShadowMaskCfg (987-1004) */
    if (load(v, "shmask", v->mask, VP_CFG_LEN - 1) <= 0 && v->ini.shmask_default[0]) {
        cpy(v->mask + 1, (int)(VP_CFG_LEN - 1), v->ini.shmask_default);
        v->mask[0] = (char)v->ini.shmask_mode_default;
    }
    if (v->mask[0] >= VP_SM_COUNT || v->mask[0] < 0) v->mask[0] = 0;
    /* load_volume (190-219): <core>_afilter.cfg, else AFILTER_DEFAULT as Custom */
    if (load(v, "afilter", v->afilt, VP_CFG_LEN - 1) <= 0 || v->afilt[0] > 1) {
        memset(v->afilt, 0, sizeof v->afilt);
        if (v->ini.afilter_default[0]) { cpy(v->afilt + 1, (int)(VP_CFG_LEN - 1), v->ini.afilter_default); v->afilt[0] = 1; }
    }
}

void vp_core_start_caps(tango_vproc_t *v, const tango_link_t *link, const vp_io_t *io, const char *core,
                        const uint8_t *ini_buf, int ini_len, const vp_caps_t *caps)
{
    memset(v, 0, sizeof *v);                              /* a new core: nothing of the one before */
    v->link = link; v->io = io; v->height = 720; v->vol = caps->vol;
    cpy(v->core, (int)(sizeof v->core), core);
    vp_parse_ini(ini_buf, ini_len, core, &v->ini);        /* (the card only: the link sees the probes, then the sends) */
    cfg_defaults(v);
    load_saved(v);
    v->has_gamma   = caps->has_gamma;
    v->has_mask    = caps->has_mask;
    v->has_afilter = caps->has_afilter;
    v->has_boost   = caps->has_boost;
    v->flags       = caps->flags;
    v->has_scaler  = caps->has_scaler;
    v->has_vsync   = caps->has_vsync;
    v->vsync_status = caps->vsync_status;
    if (v->has_gamma) vp_send_gamma(v);
    vp_send_scaler(v);
    vp_send_mask(v);
    vp_send_afilter(v);
    /* native-rate output: only a core (MiSTer acts !is_menu(), video.cpp 2647) that has it, only when Tango.ini says so */
    if (v->has_vsync && v->ini.vsync_adjust >= 0 && !ieq(v->core, "MENU")) vp_send_vsync(v);
}

/* ---------------------------------------------------------------- SYS 0x70 VSYNC_ADJUST */
/* the bounds travel as OUTPUT frame lengths in clk_pix of the framework's fixed 74.25 MHz pixel clock: P_min =
 * 74.25e6 / REFRESH_MAX (shortest frame), P_max = 74.25e6 / REFRESH_MIN; 0 = no bound (MiSTer: 0 = unlimited) */
#define VP_FPIX 74250000.0
#define VP_RETUNE_WAIT_MS  60     /* <= 18.3 ms to line 730 + 7 x 2.62 ms of PLL_INIT + 1.3 ms settle = ~38 ms, and margin */
#define VP_RETUNE_POLL_MS  30
#define VP_RETUNE_TRIES    30
static uint32_t vp_frame_len(double hz, int up)
{
    if (hz <= 0) return 0;
    double p = VP_FPIX / hz;
    uint32_t n = (uint32_t)p;
    if (up && (double)n < p) n++;
    return n > 0xFFFFFF ? 0xFFFFFF : n;
}
void vp_send_vsync(tango_vproc_t *v)
{
    if (!v->has_vsync || v->ini.vsync_adjust < 0) return;
    uint32_t pmin = vp_frame_len(v->ini.refresh_max, 1), pmax = vp_frame_len(v->ini.refresh_min, 0);
    uint8_t *b = v->xbuf;
    b[0] = SYS; b[1] = C_VSYNC;
    b[2] = (uint8_t)(0x80 | (v->ini.vsync_adjust ? 1 : 0));     /* MiSTer's 2 (single buffer) has no analogue: as 1 */
    b[3] = (uint8_t)(pmin >> 16); b[4] = (uint8_t)(pmin >> 8); b[5] = (uint8_t)pmin;
    b[6] = (uint8_t)(pmax >> 16); b[7] = (uint8_t)(pmax >> 8); b[8] = (uint8_t)pmax;
    xfer(v, b, 9);
    /* A core that can retune its HDMI PLL (the probe's bit 1, TANGO_HDMI_RETUNE)
     * may now pause its pixel clock -- the link with it -- for the PLL's re-lock: up to a frame to reach the blanking line,
     * then ~18 ms of PLL_INIT's sweep and 1.3 ms of settling. Nothing may reach the FPGA meanwhile (bytes sent into a
     * paused link are lost): wait VP_RETUNE_WAIT_MS, then probe until the reply is valid (bit 7) with bit 2 (a retune
     * pending or running) clear, VP_RETUNE_POLL_MS apart, VP_RETUNE_TRIES times at most. */
    if ((v->vsync_status & 0x02) && v->link->delay_ms) {
        v->link->delay_ms(v->link->ctx, VP_RETUNE_WAIT_MS);
        for (int i = 0; i < VP_RETUNE_TRIES; i++) {
            uint8_t st = probe(v, C_VSYNC, 0);
            if ((st & 0x80) && !(st & 0x04)) { v->vsync_status = st; return; }
            v->link->delay_ms(v->link->ctx, VP_RETUNE_POLL_MS);
        }
        v->retune_timeouts++;
    }
}
uint8_t vp_vsync_read(const tango_link_t *link) { return probe_link(link, C_VSYNC, 0); }
uint8_t vp_vsync_status(tango_vproc_t *v)
{
    if (!v->has_vsync) return 0;
    v->vsync_status = probe(v, C_VSYNC, 0);
    return v->vsync_status;
}
void vp_core_start(tango_vproc_t *v, const tango_link_t *link, const vp_io_t *io, const char *core,
                   const uint8_t *ini_buf, int ini_len)
{
    vp_caps_t caps;
    vp_probe(link, 0, &caps);                             /* the volume byte: 0 (no volume control yet) */
    vp_core_start_caps(v, link, io, core, ini_buf, ini_len, &caps);
}

int vp_poll(tango_vproc_t *v)
{
    if (!v->has_scaler) return 0;
    int hmode = v->flt[VP_HORZ].mode ? 1 : 0;
    uint8_t f = probe(v, C_FLTNUM, (uint8_t)(hmode | ((hmode && v->vset == VP_SCAN) << 1)));
    int sl = (f >> 4) & 3;
    v->flags = f;
    if (pick_vset(v, sl) == v->vset) return 0;
    vp_send_scaler_level(v, sl);
    return 1;
}

/* ---------------------------------------------------------------- the menu's actions */
void vp_set_scaler_flt(tango_vproc_t *v, int type, int on) { v->flt[type].mode = (char)(on ? 1 : 0); vp_send_scaler(v); save_scaler(v); }
void vp_set_scaler_coeff(tango_vproc_t *v, int type, const char *name)
{
    cpy(v->flt[type].name, (int)(sizeof v->flt[type].name), name); vp_send_scaler(v); save_scaler(v);
}
void vp_set_gamma_en(tango_vproc_t *v, int on) { v->gamma[0] = (char)(on ? 1 : 0); vp_send_gamma(v); save_gamma(v); }
void vp_set_gamma_curve(tango_vproc_t *v, const char *name) { cpy(v->gamma + 1, (int)(VP_CFG_LEN - 1), name); vp_send_gamma(v); save_gamma(v); }
void vp_set_mask_mode(tango_vproc_t *v, int mode)
{
    if (mode >= VP_SM_COUNT) mode = 0; else if (mode < 0) mode = VP_SM_COUNT - 1;   /* 955-962 */
    v->mask[0] = (char)mode; vp_send_mask(v); save_mask(v);
}
void vp_set_mask(tango_vproc_t *v, const char *name) { cpy(v->mask + 1, (int)(VP_CFG_LEN - 1), name); vp_send_mask(v); save_mask(v); }
void vp_set_afilter_en(tango_vproc_t *v, int on) { v->afilt[0] = (char)(on ? 1 : 0); save_af(v); vp_send_afilter(v); }
void vp_set_afilter(tango_vproc_t *v, const char *name) { cpy(v->afilt + 1, (int)(VP_CFG_LEN - 1), name); save_af(v); vp_send_afilter(v); }

void vp_reset_defaults(tango_vproc_t *v)
{
    /* video_cfg_reset (2678-2688): the files go, the ini defaults come back, everything is sent */
    char af[VP_CFG_LEN];
    memcpy(af, v->afilt, sizeof af);                     /* the audio filter is not part of Video processing */
    if (v->io && v->io->save_cfg) { save(v, "gamma", "", 0); save(v, "scaler", "", 0); save(v, "shmask", "", 0); }
    cfg_defaults(v);
    load_saved(v);
    memcpy(v->afilt, af, sizeof af);
    if (v->has_gamma) vp_send_gamma(v);
    vp_send_scaler(v);
    vp_send_mask(v);
}
