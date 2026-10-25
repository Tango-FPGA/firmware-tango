// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 the Tango authors
/*
 * confstr.c -- Tango's CONF_STR parser. See confstr.h and menu-tango docs/CONF_STR.md.
 * Our own code, written from the documented format.
 */
#include "confstr.h"

int cs_count(const char *s)
{
    /* entries = ';' count + 1, minus the empty field after a final ';' */
    int semis = 0, len = 0;
    for (; s[len]; len++) if (s[len] == ';') semis++;
    if (len == 0) return 0;
    return semis + 1 - (s[len - 1] == ';');
}

int cs_get(const char *s, int i, cs_text_t *out)
{
    int idx = 0;
    const char *start = s;
    for (const char *p = s;; p++) {
        if (*p == ';' || *p == 0) {
            if (idx == i) {
                if (*p == 0 && p == start && idx > 0) return 0;   /* nothing after the last ';' */
                out->p = start; out->len = (uint16_t)(p - start);
                return 1;
            }
            if (*p == 0) return 0;
            idx++; start = p + 1;
        }
    }
}

/* ---- small helpers on (pointer, length) text ---------------------------------------------- */

static int t_eq(cs_text_t t, const char *lit)
{
    int i = 0;
    for (; lit[i]; i++) if (i >= t.len || t.p[i] != lit[i]) return 0;
    return i == t.len;
}

static int t_starts(cs_text_t t, const char *lit)
{
    for (int i = 0; lit[i]; i++) if (i >= t.len || t.p[i] != lit[i]) return 0;
    return 1;
}

/* split t at comma number k (0-based): field k. Returns 0 if there is no such field. */
static int t_field(cs_text_t t, int k, cs_text_t *out)
{
    int idx = 0, start = 0;
    for (int i = 0; i <= t.len; i++) {
        if (i == t.len || t.p[i] == ',') {
            if (idx == k) { out->p = t.p + start; out->len = (uint16_t)(i - start); return 1; }
            idx++; start = i + 1;
        }
    }
    return 0;
}

static int t_nfields(cs_text_t t)
{
    int n = 1;
    for (int i = 0; i < t.len; i++) if (t.p[i] == ',') n++;
    return n;
}

static int is_digit(char c) { return c >= '0' && c <= '9'; }

static int idx_char(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'V') return c - 'A' + 10;
    return -1;
}

/* ---- status bits --------------------------------------------------------------------------- */

int cs_bits(const char *p, int n, int ex, uint8_t *lo, uint8_t *hi)
{
    if (n <= 0) return 0;
    if (p[0] == '[') {
        int i = 1, a = 0, b = -1, got = 0;
        while (i < n && is_digit(p[i])) { a = a * 10 + (p[i] - '0'); i++; got = 1; }
        if (!got) return 0;
        if (i < n && p[i] == ':') {
            i++; b = 0; got = 0;
            while (i < n && is_digit(p[i])) { b = b * 10 + (p[i] - '0'); i++; got = 1; }
            if (!got) return 0;
        }
        if (i >= n || p[i] != ']') return 0;
        i++;
        int h = a, l = (b < 0) ? a : b;          /* "[e:s]" is written high:low */
        if (h > 127 || l > 127 || h < l) return 0;
        if (b >= 0 && h == l) return 0;          /* a range must be at least two bits */
        if (h - l > 8) return 0;                  /* at most 9 bits per option */
        *lo = (uint8_t)l; *hi = (uint8_t)h;
        return i;
    }
    int s = idx_char(p[0]);
    if (s < 0) return 0;
    int e = s, used = 1;
    if (n > 1) {
        int t = idx_char(p[1]);
        if (t >= 0 && p[1] != ',') { e = t; used = 2; }
    }
    if (ex) { s += 32; e += 32; }
    if (used == 2 && e <= s) return 0;
    if (e - s > 8 || e > 127) return 0;
    *lo = (uint8_t)s; *hi = (uint8_t)e;
    return used;
}

uint32_t cs_status_get(const uint8_t status[16], uint8_t lo, uint8_t hi)
{
    uint32_t v = 0;
    for (int b = hi; b >= lo; b--) v = (v << 1) | ((status[b >> 3] >> (b & 7)) & 1u);
    return v;
}

void cs_status_set(uint8_t status[16], uint8_t lo, uint8_t hi, uint32_t value)
{
    for (int b = lo; b <= hi; b++) {
        uint8_t m = (uint8_t)(1u << (b & 7));
        if (value & 1u) status[b >> 3] |= m; else status[b >> 3] &= (uint8_t)~m;
        value >>= 1;
    }
}

/* ---- one entry ----------------------------------------------------------------------------- */

int cs_parse(cs_text_t t, cs_entry_t *e)
{
    *e = (cs_entry_t){0};
    if (t.len == 0) { e->kind = CS_NONE; return 1; }

    /* whole-entry keywords first */
    if (t_eq(t, "DIP")) { e->kind = CS_DIP; return 1; }
    if (t_starts(t, "DEFMRA,")) { e->kind = CS_DEFMRA; e->rest = t; return 1; }
    if (t_starts(t, "jn,") || t_eq(t, "jn")) { e->kind = CS_JN; e->rest = t; return 1; }
    if (t_starts(t, "jp,") || t_eq(t, "jp")) { e->kind = CS_JP; e->rest = t; return 1; }
    if (t_starts(t, "V,")) { e->kind = CS_VERSION; e->rest = t; t_field(t, 1, &e->name); return 1; }
    if (t_starts(t, "v,")) { e->kind = CS_CFGVER; e->rest = t; t_field(t, 1, &e->name); return 1; }
    if (t_starts(t, "I,")) { e->kind = CS_INFO; e->rest = t; return 1; }
    if (t.p[0] == 'J' && (t.len == 1 || t.p[1] == ',' || t.p[1] == '1')) { e->kind = CS_BUTTONS; e->rest = t; return 1; }

    /* prefixes: any number of H/h/D/d + bit (a digit, or the old index form), then an optional P# */
    const char *p = t.p; int n = t.len;
    while (n > 2 && (p[0] == 'H' || p[0] == 'h' || p[0] == 'D' || p[0] == 'd')) {
        int b = idx_char(p[1]);
        if (b < 0 || b > 15) break;
        if (e->n_hd < 4) { e->hd[e->n_hd].op = p[0]; e->hd[e->n_hd].bit = (uint8_t)b; e->n_hd++; }
        p += 2; n -= 2;
    }
    if (n > 1 && p[0] == 'P' && is_digit(p[1])) {
        if (n > 2 && p[2] == ',') {                      /* "P#,Title": a link to a sub-page */
            e->kind = CS_PAGE; e->target_page = (uint8_t)(p[1] - '0');
            cs_text_t r = { p + 3, (uint16_t)(n - 3) };
            t_field(r, 0, &e->name);
            return 1;
        }
        e->page = (uint8_t)(p[1] - '0');                 /* "P#..." prefix */
        p += 2; n -= 2;
    }
    if (n == 0) return 0;
    cs_text_t body = { p, (uint16_t)n };

    switch (p[0]) {
    case 'O': case 'o': case 'T': case 't': case 'R': case 'r': {
        int ex = (p[0] == 'o' || p[0] == 't' || p[0] == 'r');
        int used = cs_bits(p + 1, n - 1, ex, &e->bit_lo, &e->bit_hi);
        if (!used) return 0;
        cs_text_t r = { p + 1 + used, (uint16_t)(n - 1 - used) };
        if (r.len == 0 || r.p[0] != ',') return 0;
        r.p++; r.len--;
        t_field(r, 0, &e->name);
        if (p[0] == 'O' || p[0] == 'o') {
            e->kind = CS_OPTION;
            int nf = t_nfields(r);
            e->n_values = (uint8_t)(nf - 1);
            if (nf > 1) { e->values_all.p = e->name.p + e->name.len + 1; e->values_all.len = (uint16_t)(r.p + r.len - e->values_all.p); }
        } else {
            e->kind = (p[0] == 'T' || p[0] == 't') ? CS_TRIGGER : CS_RESET;
        }
        return 1;
    }
    case 'F': case 'S': {
        int i = 1;
        e->kind = (p[0] == 'F') ? CS_FILE : CS_MOUNT;
        if (p[0] == 'F' && i < n && p[i] == 'S') { e->f_save = 1; i++; }
        if (i < n && p[i] == 'C') { e->f_remember = 1; i++; }
        if (i < n && is_digit(p[i])) { e->index = (uint8_t)(p[i] - '0'); i++; }
        if (i >= n || p[i] != ',') return 0;
        cs_text_t r = { p + i + 1, (uint16_t)(n - i - 1) };
        t_field(r, 0, &e->ext);
        t_field(r, 1, &e->name);                       /* optional text */
        return 1;
    }
    case 'C':
        e->kind = CS_CHEATS;
        if (n > 1 && p[1] == ',') { cs_text_t r = { p + 2, (uint16_t)(n - 2) }; e->name = r; }
        return 1;
    case '-':
        e->kind = CS_TEXT;
        if (n > 1 && p[1] == ',') { cs_text_t r = { p + 2, (uint16_t)(n - 2) }; e->name = r; }
        return 1;
    default:
        (void)body;
        e->kind = CS_NONE;          /* unknown entries are ignored, as MiSTer does */
        return 1;
    }
}

int cs_value(const cs_entry_t *e, int v, cs_text_t *out)
{
    if (e->kind != CS_OPTION || v < 0 || v >= e->n_values) return 0;
    return t_field(e->values_all, v, out);
}

void cs_visibility(const cs_entry_t *e, uint16_t menumask, int *hidden, int *disabled)
{
    int h = 0, d = 0;
    for (int i = 0; i < e->n_hd; i++) {
        int set = (menumask >> e->hd[i].bit) & 1;
        switch (e->hd[i].op) {
        case 'H': h |= set; break;
        case 'h': h |= !set; break;
        case 'D': d |= set; break;
        case 'd': d |= !set; break;
        }
    }
    *hidden = h; *disabled = d;
}
