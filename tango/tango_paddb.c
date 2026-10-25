// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 the Tango authors
/*
 * tango_paddb.c -- see tango_paddb.h. Tango, our own code.
 */
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <ctype.h>
#include "tango_paddb.h"

static const struct { const char *n; int sn; } NAMES[] = {
    { "a", SN_A }, { "b", SN_B }, { "x", SN_X }, { "y", SN_Y }, { "back", SN_BACK }, { "guide", SN_GUIDE },
    { "start", SN_START }, { "leftstick", SN_LSTICK }, { "rightstick", SN_RSTICK },
    { "leftshoulder", SN_LSHOULDER }, { "rightshoulder", SN_RSHOULDER },
    { "dpup", SN_DPUP }, { "dpdown", SN_DPDOWN }, { "dpleft", SN_DPLEFT }, { "dpright", SN_DPRIGHT },
    { "leftx", SN_LEFTX }, { "lefty", SN_LEFTY }, { "rightx", SN_RIGHTX }, { "righty", SN_RIGHTY },
    { "lefttrigger", SN_LTRIGGER }, { "righttrigger", SN_RTRIGGER }, { "misc1", SN_MISC1 }, { "touchpad", SN_MISC1 },
};

const char *sdl_name(int sn)
{
    for (unsigned i = 0; i < sizeof NAMES / sizeof NAMES[0]; i++) if (NAMES[i].sn == sn) return NAMES[i].n;
    return "?";
}

static int hexv(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* the GUID's hex digits -> bytes; returns the number of digits (0 if the field is not a GUID) */
static int guid_bytes(const char *p, int len, uint8_t b[16])
{
    memset(b, 0, 16);
    if (len < 20 || len > 32 || (len & 1)) return 0;      /* up to the product id; 28 digits or more also carry the version */
    for (int i = 0; i < len; i++) {
        int v = hexv(p[i]);
        if (v < 0) return 0;
        if (i & 1) b[i / 2] |= (uint8_t)v; else b[i / 2] = (uint8_t)(v << 4);
    }
    return len;
}

static int plat_of(const char *v, int len)
{
    if (len == 5 && !strncasecmp(v, "Linux", 5)) return PL_LINUX;
    if (len == 6 && !strncasecmp(v, "MiSTer", 6)) return PL_MISTER;
    if (len == 5 && !strncasecmp(v, "Tango", 5)) return PL_TANGO;
    return PL_OTHER;
}

/* one binding value: bN aN +aN -aN hN.D, optional '~' */
static int bind_parse(const char *v, int len, sdl_bind_t *out)
{
    memset(out, 0, sizeof *out);
    int i = 0, half = 0;
    if (len <= 0) return 0;
    if (v[i] == '+') { half = 1; i++; } else if (v[i] == '-') { half = 2; i++; }
    if (i >= len) return 0;
    char k = v[i++];
    int n = 0, dig = 0;
    while (i < len && isdigit((unsigned char)v[i])) { n = n * 10 + (v[i] - '0'); i++; dig++; if (n > 255) return 0; }
    if (!dig) return 0;
    if (k == 'b' && !half) { out->type = SB_BTN; out->idx = (uint8_t)n; }
    else if (k == 'a') { out->type = half == 1 ? SB_AXIS_POS : half == 2 ? SB_AXIS_NEG : SB_AXIS; out->idx = (uint8_t)n; }
    else if (k == 'h' && !half) {
        if (i >= len || v[i] != '.') return 0;
        i++;
        int d = 0, dd = 0;
        while (i < len && isdigit((unsigned char)v[i])) { d = d * 10 + (v[i] - '0'); i++; dd++; }
        if (!dd || d < 1 || d > 15) return 0;
        out->type = SB_HAT; out->idx = (uint8_t)n; out->hatmask = (uint8_t)d;
    } else return 0;
    if (i < len && v[i] == '~') { out->inv = 1; i++; }
    return i == len;
}

int sdl_parse_line(const char *line, sdl_map_t *m)
{
    memset(m, 0, sizeof *m);
    const char *p = line;
    while (*p == ' ' || *p == '\t') p++;
    if (!*p || *p == '#' || *p == '\r' || *p == '\n') return 0;
    const char *c = strchr(p, ',');
    if (!c) return 0;
    uint8_t g[16];
    int nd = guid_bytes(p, (int)(c - p), g);
    if (!nd) return 0;
    m->bus = (uint16_t)(g[0] | g[1] << 8);
    m->vid = (uint16_t)(g[4] | g[5] << 8);
    m->pid = (uint16_t)(g[8] | g[9] << 8);
    m->has_ver = nd >= 28;
    m->ver = m->has_ver ? (uint16_t)(g[12] | g[13] << 8) : 0;
    p = c + 1;
    c = strchr(p, ',');
    int nl = c ? (int)(c - p) : (int)strlen(p);
    if (nl > (int)sizeof m->name - 1) nl = (int)sizeof m->name - 1;
    memcpy(m->name, p, (size_t)nl); m->name[nl] = 0;
    if (!c) return 1;
    p = c + 1;
    while (*p) {
        const char *e = p;
        while (*e && *e != ',' && *e != '\r' && *e != '\n') e++;
        int tl = (int)(e - p);
        const char *col = memchr(p, ':', (size_t)tl);
        if (col) {
            int kl = (int)(col - p), vl = tl - kl - 1;
            const char *val = col + 1;
            int found = 0;
            if (kl == 8 && !strncasecmp(p, "platform", 8)) { m->plat = (uint8_t)plat_of(val, vl); found = 1; }
            else if ((kl == 10 && !strncasecmp(p, "mistercore", 10)) || (kl == 9 && !strncasecmp(p, "tangocore", 9))) {
                size_t cur = strlen(m->cores);
                if (cur + (size_t)vl + 2 < sizeof m->cores) {
                    if (cur) m->cores[cur++] = ',';
                    memcpy(m->cores + cur, val, (size_t)vl); m->cores[cur + (size_t)vl] = 0;
                }
                found = 1;
            } else {
                for (unsigned i = 0; i < sizeof NAMES / sizeof NAMES[0]; i++) {
                    if ((int)strlen(NAMES[i].n) == kl && !strncmp(NAMES[i].n, p, (size_t)kl)) {
                        sdl_bind_t bd;
                        if (bind_parse(val, vl, &bd)) { m->b[NAMES[i].sn] = bd; } else m->nignored++;
                        found = 1; break;
                    }
                }
            }
            if (!found && m->nignored < 255) m->nignored++;    /* guide2, menufunc, menuok, menuesc, asysx, paddles, crc, hint, ... */
        }
        p = e;
        while (*p == ',' || *p == '\r' || *p == '\n' || *p == ' ') p++;
    }
    return 1;
}

/* a MiSTer core pattern: a case-insensitive prefix of the core name, cut at the first '*' */
static int core_match(const char *pats, const char *core)
{
    const char *p = pats;
    while (*p) {
        const char *e = strchr(p, ',');
        int l = e ? (int)(e - p) : (int)strlen(p);
        const char *star = memchr(p, '*', (size_t)l);
        if (star) l = (int)(star - p);
        if (!strncasecmp(p, core, (size_t)l)) return 1;
        if (!e) break;
        p = e + 1;
    }
    return 0;
}

int sdl_applies(const sdl_map_t *m, const char *core, int lenient)
{
    switch (m->plat) {
    case PL_LINUX: return 1;
    case PL_MISTER: case PL_TANGO: return !m->cores[0] || (core && core_match(m->cores, core));
    case PL_NONE: return lenient;
    default: return 0;
    }
}

void pdb_scan_begin(pdb_scan_t *s, uint16_t vid, uint16_t pid, uint16_t ver, const char *core, int lenient)
{
    memset(s, 0, sizeof *s);
    s->vid = vid; s->pid = pid; s->ver = ver; s->core = core; s->lenient = lenient;
}

void pdb_scan_set_ver2(pdb_scan_t *s, uint16_t ver2) { s->ver2 = ver2; }

void pdb_scan_line(pdb_scan_t *s, const char *line)
{
    sdl_map_t m;
    if (!sdl_parse_line(line, &m)) return;
    s->lines++;
    if (m.bus != 3 || m.vid != s->vid || m.pid != s->pid) return;
    if (!sdl_applies(&m, s->core, s->lenient)) return;
    int rank;
    if (m.has_ver && (m.ver == s->ver || (s->ver2 && m.ver == s->ver2))) rank = PM_EXACT;
    else if (!m.has_ver || m.ver == 0) rank = PM_VER0;
    else if (m.ver < 0x8000) rank = PM_VIDPID;
    else return;          /* a Linux kernel-driver numbering (hid-sony, hid-nintendo set the input version to 0x8xxx): only for that exact version */
    if (rank >= s->rank) { s->rank = rank; s->best = m; s->have = 1; }
}

int pdb_scan_result(const pdb_scan_t *s, sdl_map_t *out)
{
    if (!s->have) return PM_NONE;
    *out = s->best;
    return s->rank;
}

void pdb_scan_text(pdb_scan_t *s, const char *text)
{
    char buf[1100];
    const char *p = text;
    while (*p) {
        const char *e = strchr(p, '\n');
        size_t l = e ? (size_t)(e - p) : strlen(p);
        if (l >= sizeof buf) l = sizeof buf - 1;
        memcpy(buf, p, l); buf[l] = 0;
        pdb_scan_line(s, buf);
        if (!e) break;
        p = e + 1;
    }
}

void pdb_cache_clear(pdb_cache_t *c) { memset(c, 0, sizeof *c); }

int pdb_cache_get(const pdb_cache_t *c, uint16_t vid, uint16_t pid, uint16_t ver, int *rank, int *src, sdl_map_t *map)
{
    for (int i = 0; i < PDB_CACHE; i++)
        if (c->e[i].used && c->e[i].vid == vid && c->e[i].pid == pid && c->e[i].ver == ver) {
            *rank = c->e[i].rank; *src = c->e[i].src;
            if (c->e[i].rank != PM_NONE && map) *map = c->e[i].map;
            return 1;
        }
    return 0;
}

void pdb_cache_put(pdb_cache_t *c, uint16_t vid, uint16_t pid, uint16_t ver, int rank, int src, const sdl_map_t *map)
{
    int i = c->next;
    c->next = (c->next + 1) % PDB_CACHE;
    c->e[i].used = 1; c->e[i].vid = vid; c->e[i].pid = pid; c->e[i].ver = ver; c->e[i].rank = (uint8_t)rank; c->e[i].src = (uint8_t)src;
    if (map && rank != PM_NONE) c->e[i].map = *map; else memset(&c->e[i].map, 0, sizeof c->e[i].map);
}
