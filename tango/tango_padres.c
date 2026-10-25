// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 the Tango authors
/*
 * tango_padres.c -- see tango_padres.h. Tango, our own code.
 */
#include <string.h>
#include <stdio.h>
#include "tango_padres.h"

/* DualShock 4 family: Sony's own ids (v1, v2, USB dongle). Other makers' DS4-compatible pads come with a database line. */
static const uint16_t DS4_IDS[][2] = { { 0x054c, 0x05c4 }, { 0x054c, 0x09cc }, { 0x054c, 0x0ba0 } };

int pad_is_ds4(uint16_t vid, uint16_t pid)
{
    for (unsigned i = 0; i < sizeof DS4_IDS / sizeof DS4_IDS[0]; i++) if (DS4_IDS[i][0] == vid && DS4_IDS[i][1] == pid) return 1;
    return 0;
}

static void why(pad_resolution_t *r, const char *fmt, int a, int b)
{
    snprintf(r->why, sizeof r->why, fmt, a, b);
}

/* the database steps share one body */
static int try_map(const pad_query_t *q, pad_resolution_t *r, const sdl_map_t *m, int rank, int src)
{
    if (!q->L) return 0;
    pd_table_t t; pd_build_info_t info;
    int ok = pd_from_sdl(q->L, m, &t, &info);
    if (!ok) { r->skipped_unusable++; r->info = info; why(r, "entry rejected: %d bindings unresolved of %d", info.nunres, info.nbind); return 0; }
    r->table = t; r->info = info; r->src = src; r->rank = rank;
    snprintf(r->name, sizeof r->name, "%s", m->name);
    return 1;
}

/* one text file (which 0 user, 1 card), through the answer cache; PM_NONE if absent or no line */
static int scan_file(const pad_sources_t *s, pdb_cache_t *cache, int which, const pad_query_t *q, sdl_map_t *m, int *opened)
{
    int rank = PM_NONE, src = 0;
    if (opened) *opened = 0;
    if (cache && pdb_cache_get(cache, q->vid, q->pid, q->ver, &rank, &src, m)) { if (opened) *opened = 1; return rank; }
    if (s && s->open && s->line && s->open(s->ctx, which)) {
        if (opened) *opened = 1;
        pdb_scan_t sc; pdb_scan_begin(&sc, q->vid, q->pid, q->ver, q->core, which == 0); pdb_scan_set_ver2(&sc, q->ver_hid);
        const char *ln;
        while ((ln = s->line(s->ctx, which)) != NULL) pdb_scan_line(&sc, ln);
        if (s->close) s->close(s->ctx, which);
        rank = pdb_scan_result(&sc, m);
    }
    if (cache) pdb_cache_put(cache, q->vid, q->pid, q->ver, rank, which, m);
    return rank;
}

int pad_resolve(const pad_sources_t *s, pdb_cache_t cache[2], const pad_query_t *q, pad_resolution_t *r)
{
    memset(r, 0, sizeof *r);
    r->src = PRS_NONE;
    sdl_map_t m; int rank;
    int not_pad = !q->L && (q->L_cls == HPC_MOUSE || q->L_cls == HPC_KEYBOARD || q->L_cls == HPC_OTHER);

    /* 1 the user's own decode */
    if (s && s->load_dec && s->load_dec(s->ctx, q->vid, q->pid, q->ver, &r->table)) {
        r->src = PRS_USER_DEC; snprintf(r->why, sizeof r->why, "saved decode"); return r->src;
    }
    /* not a gamepad at all: nothing below applies */
    if (not_pad) {
        pd_quiet(&r->table); r->src = PRS_QUIET; snprintf(r->why, sizeof r->why, "not a gamepad (class %d)", q->L_cls); return r->src;
    }
    /* 2 the user's database file beats everything that follows */
    rank = scan_file(s, cache ? &cache[0] : NULL, 0, q, &m, NULL);
    int have_entry = rank != PM_NONE;
    if (rank != PM_NONE && try_map(q, r, &m, rank, PRS_USER_DB)) {
        snprintf(r->why, sizeof r->why, "user line, %s", rank == PM_EXACT ? "exact bcdDevice" : rank == PM_VER0 ? "version 0" : "vendor:product only");
        return r->src;
    }
    if (rank != PM_NONE) snprintf(r->name, sizeof r->name, "%s", m.name);
    /* 3 DualShock 4 */
    if (pad_is_ds4(q->vid, q->pid)) {
        memset(&r->table, 0, sizeof r->table); r->table.mode = PDM_DS4; r->src = PRS_DS4; snprintf(r->why, sizeof r->why, "DualShock 4 (built in)");
        return r->src;
    }
    /* 4 XInput class */
    if (q->if_class == 0xFF && q->if_sub == 0x5D && q->if_proto == 0x01) {
        pd_builtin_xinput(&r->table); r->src = PRS_XINPUT; snprintf(r->why, sizeof r->why, "XInput class FF/5D/01"); return r->src;
    }
    /* 5-6 the card file, then the built-in table */
    int src = 0;
    int cached = cache && pdb_cache_get(&cache[1], q->vid, q->pid, q->ver, &rank, &src, &m);
    if (!cached) {
        int card_opened = 0;
        rank = scan_file(s, NULL, 1, q, &m, &card_opened); src = PRS_DB;
        /* the compiled-in table is the NO-CARD fallback only. A card file that opened is the database, whatever it lacks. */
        if (rank == PM_NONE && !card_opened && s && s->builtin) { rank = s->builtin(s->ctx, q->vid, q->pid, q->ver, &m); src = PRS_BUILTIN; }
        if (cache) pdb_cache_put(&cache[1], q->vid, q->pid, q->ver, rank, src, &m);
    }
    if (rank != PM_NONE) {
        have_entry = 1;
        if (try_map(q, r, &m, rank, src)) {
            snprintf(r->why, sizeof r->why, "%s line, %s", src == PRS_DB ? "card" : "built-in",
                     rank == PM_EXACT ? "exact bcdDevice" : rank == PM_VER0 ? "version 0" : "vendor:product only");
            return r->src;
        }
        snprintf(r->name, sizeof r->name, "%s", m.name);      /* a rejected entry: keep its name for the log, go on to the descriptor */
    }

    /* 7 the descriptor alone */
    if (q->L) {
        if (pd_from_layout_rest(q->L, q->rest, q->rest_len, &r->table, &r->info)) {
            r->src = PRS_HEURISTIC; r->rank = PM_NONE;
            snprintf(r->why, sizeof r->why, "descriptor only: %d buttons, %d axes, %d hat", q->L->nbtn, q->L->naxis, q->L->nhat);
            return r->src;
        }
        pd_quiet(&r->table); r->src = PRS_QUIET;
        snprintf(r->why, sizeof r->why, "descriptor has no usable d-pad and buttons");
        return r->src;
    }
    /* no descriptor */
    if (have_entry) { pd_quiet(&r->table); r->src = PRS_QUIET; snprintf(r->why, sizeof r->why, "known pad, layout needs the report descriptor"); return r->src; }
    memset(&r->table, 0, sizeof r->table); r->table.mode = PDM_DS4; r->src = PRS_LEGACY;
    snprintf(r->why, sizeof r->why, "unknown pad, no descriptor: DualShock 4 decode as before");
    return r->src;
}
