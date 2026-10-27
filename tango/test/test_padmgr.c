// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 the Tango authors
/*
 * test_padmgr.c -- host tests for the pad manager (tango_padmgr.c): quarantine on an old FPGA, then the new-FPGA flow.
 * No SDK, no hardware.  make test_padmgr
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include "../tango_padmgr.h"
#include "../tango_paddb_builtin.h"
#include "../tango_padres.h"
#include "fake_io.h"
#include "fake_pad_fpga.h"
#include "pad_fixtures.h"
#include "real_descriptors.h"
#include "learn_gen.h"
#include <time.h>

#define CONTROLLERDB_ATTR
#define CONTROLLERDB_STR_ATTR
#include "../../companion/src/usb_controller_maps.h"

static int fails = 0, checks = 0;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint32_t rng_s = 0x9e3779b9u;
static uint32_t rnd(void) { rng_s ^= rng_s << 13; rng_s ^= rng_s >> 17; rng_s ^= rng_s << 5; return rng_s; }

/* ---------------------------------------------------------------- the platform */
typedef struct {
    rec_link_t rl; tango_link_t link;
    char *rec; size_t recn;                 /* the recorded stream (u16 length + bytes per transfer) */
    int applied[2], apply_calls;
    char logs[16][128]; int nlog;
    long allocs, frees;
    long alloc_n, fail_at;        /* the fail_at-th allocation after alloc_n was reset returns NULL (0 = never) */
} env_t;

static void *e_alloc(void *c, unsigned n) { env_t *e = c; e->alloc_n++; if (e->fail_at && e->alloc_n == e->fail_at) return NULL; e->allocs++; return malloc(n); }
static void e_release(void *c, void *p) { env_t *e = c; if (p) e->frees++; free(p); }
static void e_log(void *c, const char *l) { env_t *e = c; if (e->nlog < 16) snprintf(e->logs[e->nlog], 128, "%s", l); e->nlog++; }
static void e_apply(void *c, int port)
{   /* what pm_apply does with no map file and no J names: the default core map */
    env_t *e = c; e->applied[port]++; e->apply_calls++;
    tango_hid_set_padmap(&e->link, (uint8_t)port, 0xFE, 0, NULL, 0);
}
static int g_no_builtin = 0;            /* a test that wants the no-database, descriptor-only path for a pad the compiled-in table knows */
static int e_builtin(void *c, uint16_t vid, uint16_t pid, uint16_t ver, sdl_map_t *out)
{
    (void)c;
    if (g_no_builtin) return PM_NONE;
    const UsbGamepadMap *first = NULL, *v0 = NULL;
    for (unsigned i = 0; i < kUsbGamepadMapsCount; i++) {
        const UsbGamepadMap *e = &kUsbGamepadMaps[i];
        if (e->vid != vid || e->pid != pid) continue;
        if (!first) first = e;
        if (!v0 && e->version == 0) v0 = e;
        if (e->version == ver) return pdb_from_companion(e, ver, out);
    }
    const UsbGamepadMap *e = v0 ? v0 : first;
    return e ? pdb_from_companion(e, ver, out) : PM_NONE;
}

static FILE *rec_open(env_t *e) { e->rec = NULL; e->recn = 0; return open_memstream(&e->rec, &e->recn); }
static void env_init(env_t *e, padmgr_platform_t *P, int launcher)
{
    memset(e, 0, sizeof *e);
    rec_link_init(&e->rl, &e->link);
    e->rl.out = rec_open(e);
    memset(P, 0, sizeof *P);
    P->ctx = e; P->link = &e->link; P->alloc = e_alloc; P->release = e_release; P->log = e_log;
    P->apply_map = launcher ? NULL : e_apply; P->builtin = e_builtin; P->core = launcher ? NULL : "NES";
}
static void env_stop(env_t *e) { fclose(e->rl.out); e->rl.out = NULL; }

/* ---------------------------------------------------------------- the legacy driver: tango_fw.c raw_in() before this work, verbatim logic */
typedef struct { int conn; uint16_t vid, pid; } legacy_pad_t;
static void legacy_raw_in(env_t *e, legacy_pad_t *pads, int p, uint8_t flags, uint16_t vid, uint16_t pid)
{
    int conn = (flags >> (2 * p)) & 1;
    if (conn != pads[p].conn || (conn && (vid != pads[p].vid || pid != pads[p].pid))) {
        pads[p].conn = conn; pads[p].vid = vid; pads[p].pid = pid;
        if (conn) e_apply(e, p);                      /* pm_apply returns early when the pad is not connected */
    }
}

typedef struct { uint8_t flags; uint16_t vid, pid; } snap_t;
/* a plug-in script for both ports: (port, flags, vid, pid) tuples */
static void run_script(env_t *e, padmgr_t *m, legacy_pad_t *lg, const int (*s)[4], int n)
{
    for (int i = 0; i < n; i++) {
        int p = s[i][0]; uint8_t fl = (uint8_t)s[i][1]; uint16_t vid = (uint16_t)s[i][2], pid = (uint16_t)s[i][3];
        if (m) padmgr_snapshot(m, p, fl, vid, pid, 0);
        if (lg) legacy_raw_in(e, lg, p, fl, vid, pid);
    }
}

static int same_stream(const char *a, size_t an, const char *b, size_t bn) { return an == bn && !memcmp(a, b, an); }

/* the SPI stream of a plug-in script, padmgr against the legacy driver */
static void check_identity(const char *what, const int (*s)[4], int n, int launcher)
{
    env_t a, b; padmgr_platform_t P;
    env_init(&a, &P, launcher);
    padmgr_t *m = padmgr_new(&P);
    env_init(&b, &P, launcher);
    legacy_pad_t lg[2]; memset(lg, 0, sizeof lg);
    padmgr_core_start(m);                           /* the one addition on an old FPGA: DECODE_STATUS 01 19 00 + 7 reads, answered 00 */
    run_script(&a, m, NULL, s, n);
    if (!launcher) run_script(&b, NULL, lg, s, n);
    fflush(a.rl.out); fflush(b.rl.out);
    static const char PROBE[12] = { 10, 0, 1, 0x19, 0, 0, 0, 0, 0, 0, 0, 0 };
    CHECK(a.recn >= 12 && !memcmp(a.rec, PROBE, 12) && padmgr_fpga(m) == PADMGR_FPGA_OLD, "%s: the stream starts with the single DECODE_STATUS probe", what);
    if (a.recn >= 12) { memmove(a.rec, a.rec + 12, a.recn - 12); a.recn -= 12; }
    CHECK(same_stream(a.rec, a.recn, b.rec, b.recn), "%s: SPI stream differs from the legacy driver (%zu vs %zu bytes)", what, a.recn, b.recn);
    CHECK(a.nlog == 0, "%s: unexpected log line: %s", what, a.logs[0]);
    if (launcher) CHECK(a.recn == 0, "%s: the launcher sends nothing for a pad that is left alone (%zu bytes)", what, a.recn);
    else CHECK(a.recn > 0, "%s: the stream is empty, the test proves nothing", what);
    env_stop(&a); env_stop(&b); free(a.rec); free(b.rec);
    padmgr_free(m);
}

/* ---------------------------------------------------------------- tests */
static void test_identity(void)
{
    printf("-- P1: pads that are left alone give the legacy stream byte for byte\n");
    static const int DS4[][4] = { { 0, 0, 0, 0 }, { 1, 0, 0, 0 }, { 0, 1, 0x054c, 0x09cc }, { 0, 1, 0x054c, 0x09cc }, { 0, 1, 0x054c, 0x09cc },
                                  { 1, 4, 0x054c, 0x05c4 }, { 1, 4, 0x054c, 0x05c4 }, { 0, 0, 0x054c, 0x09cc }, { 0, 1, 0x054c, 0x09cc } };
    check_identity("DS4 v2 + DS4 v1", DS4, 9, 0);
    check_identity("DS4 v2 + DS4 v1 (launcher)", DS4, 9, 1);
    static const int UNK[][4] = { { 0, 1, 0x1234, 0x5678 }, { 0, 1, 0x1234, 0x5678 }, { 0, 0, 0x1234, 0x5678 }, { 1, 4, 0x2222, 0x0001 }, { 1, 4, 0x2222, 0x0002 } };
    check_identity("unknown pads (the table does not know them)", UNK, 5, 0);
    static const int DS4PLUS[][4] = { { 0, 1, 0x054c, 0x09cc }, { 1, 4, 0x1234, 0x5678 }, { 0, 1, 0x054c, 0x09cc }, { 0, 0, 0, 0 }, { 1, 0, 0, 0 } };
    check_identity("DS4 and an unknown pad together", DS4PLUS, 5, 0);
}

static int builtin_known(uint16_t vid, uint16_t pid) { sdl_map_t m; return e_builtin(NULL, vid, pid, 0, &m) != PM_NONE; }

static void test_survey(void)
{
    printf("-- P1: the built-in table, which pads are DS4-shaped\n");
    sdl_map_t ref, e;
    CHECK(e_builtin(NULL, 0x054c, 0x09cc, 0, &ref) != PM_NONE, "the DS4 reference line exists");
    int total = 0, like = 0, quar = 0, sony = 0;
    uint16_t lv = 0, lp = 0;
    for (unsigned i = 0; i < kUsbGamepadMapsCount; i++) {
        const UsbGamepadMap *g = &kUsbGamepadMaps[i];
        if (g->vid == lv && g->pid == lp) continue;
        lv = g->vid; lp = g->pid; total++;
        e_builtin(NULL, g->vid, g->pid, 0, &e);
        int ds4 = padmgr_layout_like_ds4(&e, &ref);
        if (pad_is_ds4(g->vid, g->pid)) { sony++; CHECK(ds4, "Sony DS4 %04x:%04x is DS4-shaped", g->vid, g->pid); }
        if (ds4) { like++; if (getenv("PM_SURVEY")) printf("   ds4-shaped %04x:%04x %s\n", g->vid, g->pid, e.name); } else quar++;
    }
    printf("   vendor:product entries seen %d (consecutive runs): DS4-shaped %d, quarantined %d, Sony DS4 ids %d\n", total, like, quar, sony);
    CHECK(quar > like, "most of the table is not DS4-shaped");
    CHECK(padmgr_layout_like_ds4(&ref, &ref), "the reference is DS4-shaped");
    /* named anchors */
    sdl_map_t x360, sn;
    e_builtin(NULL, 0x045e, 0x028e, 0, &x360);
    CHECK(!padmgr_layout_like_ds4(&x360, &ref), "Xbox 360 is not DS4-shaped");
    CHECK(builtin_known(0x0079, 0x0011), "0079:0011 is in the table");
    e_builtin(NULL, 0x0079, 0x0011, 0, &sn);
    CHECK(!padmgr_layout_like_ds4(&sn, &ref), "the Retrolink/DragonRise SNES pad is not DS4-shaped");
}

static int blank_ok(const env_t *e, int port)
{   /* the stream ends with 01 13 port 01 00 <64 x FF> */
    const unsigned char *r = (const unsigned char *)e->rec;
    size_t n = e->recn, pos = 0;
    int found = 0;
    while (pos + 2 <= n) {
        size_t l = r[pos] | (size_t)r[pos + 1] << 8; const unsigned char *b = r + pos + 2;
        if (l == 69 && b[0] == 1 && b[1] == 0x13 && b[2] == port && b[3] == 1 && b[4] == 0) {
            int ff = 1; for (int i = 5; i < 69; i++) if (b[i] != 0xFF) ff = 0;
            if (ff) found++;
        }
        pos += 2 + l;
    }
    return found;
}

static void test_quarantine(void)
{
    printf("-- P1: quarantine\n");
    env_t e; padmgr_platform_t P; env_init(&e, &P, 0);
    padmgr_t *m = padmgr_new(&P);
    /* an Xbox 360 pad on A2 (port 1) */
    padmgr_snapshot(m, 1, 0x04, 0x045e, 0x028e, 0);
    fflush(e.rl.out);
    CHECK(padmgr_blocked(m, 1) && !padmgr_blocked(m, 0), "port 1 quarantined, port 0 not");
    CHECK(e.apply_calls == 0, "no core map was applied to the quarantined pad");
    CHECK(blank_ok(&e, 1) == 1, "exactly one SET_PADMAP none for port 1 (found %d)", blank_ok(&e, 1));
    CHECK(e.nlog == 1 && strstr(e.logs[0], "Unsupported pad 045e:028e on USB-A2"), "one message: %s", e.logs[0]);
    /* the same snapshot again: nothing new */
    size_t before = e.recn;
    padmgr_snapshot(m, 1, 0x04, 0x045e, 0x028e, 0); fflush(e.rl.out);
    CHECK(e.recn == before && e.nlog == 1, "a repeated snapshot sends and logs nothing");
    /* the menu word is rebuilt from the other port's raw word */
    uint16_t w = 0xABCD;
    CHECK(padmgr_menu_word(m, &w) == 1 && w == 0, "quarantined, nothing else plugged: the menu word is empty (0x%04x)", w);
    padmgr_snapshot(m, 0, 0x01, 0x054c, 0x09cc, 0);
    CHECK(e.applied[0] == 1, "a DS4 on port 0 gets its map");
    padmgr_note_raw(m, 0, (1u << 5) | (1u << 3) | (1u << 13));            /* circle, up, options */
    padmgr_note_raw(m, 1, 0xFFFFFFFFu);                                   /* the quarantined pad's garbage must not count */
    CHECK(padmgr_menu_word(m, &w) == 1 && w == (0x10 | 0x08 | 0x800), "menu word from the DS4 only: 0x%04x", w);
    /* the pad is unplugged: the quarantine ends */
    padmgr_snapshot(m, 1, 0x00, 0, 0, 0);
    CHECK(!padmgr_blocked(m, 1) && padmgr_menu_word(m, &w) == 0, "unplugged: no quarantine, GET_DB9 value used as it is");
    /* a DS4 plugged into the same port afterwards */
    padmgr_snapshot(m, 1, 0x04, 0x054c, 0x05c4, 0);
    CHECK(!padmgr_blocked(m, 1) && e.applied[1] == 1, "DS4 after the Xbox pad on the same port is mapped");
    /* a different pad swapped without an unplug is judged again */
    padmgr_snapshot(m, 1, 0x04, 0x0079, 0x0011, 0);
    CHECK(padmgr_blocked(m, 1), "SNES-style pad replaces the DS4: quarantined");
    padmgr_snapshot(m, 1, 0x04, 0x054c, 0x05c4, 0);
    CHECK(!padmgr_blocked(m, 1), "and back");
    env_stop(&e); free(e.rec); padmgr_free(m);

    /* the launcher: the same, with no map callback */
    env_init(&e, &P, 1); m = padmgr_new(&P);
    padmgr_snapshot(m, 0, 0x01, 0x045e, 0x028e, 0); fflush(e.rl.out);
    CHECK(padmgr_blocked(m, 0) && blank_ok(&e, 0) == 1, "launcher: quarantine works without apply_map");
    env_stop(&e); free(e.rec); padmgr_free(m);
}

/* the menu word from a raw word equals the FPGA's fixed decode (tango_ds4_joy.v dflt) from the report bytes it came from */
static void test_menu_word(void)
{
    printf("-- the menu word rebuilt from the raw word\n");
    int bad = 0;
    for (int i = 0; i < 20000; i++) {
        uint8_t b5 = (uint8_t)rnd(), b6 = (uint8_t)rnd(), b7 = (uint8_t)rnd();
        int up = 0, down = 0, left = 0, right = 0;
        switch (b5 & 0xF) { case 0: up = 1; break; case 1: up = right = 1; break; case 2: right = 1; break; case 3: down = right = 1; break;
                            case 4: down = 1; break; case 5: down = left = 1; break; case 6: left = 1; break; case 7: up = left = 1; break; default: break; }
        /* rw as tango_ds4_joy.v builds it */
        uint32_t rw = (uint32_t)right | (uint32_t)left << 1 | (uint32_t)down << 2 | (uint32_t)up << 3 |
                      (uint32_t)((b5 >> 5) & 1) << 4 | (uint32_t)((b5 >> 6) & 1) << 5 | (uint32_t)((b5 >> 4) & 1) << 6 | (uint32_t)((b5 >> 7) & 1) << 7 |
                      (uint32_t)b6 << 8 | (uint32_t)(b7 & 3) << 16;
        /* dflt as the RTL writes it: {4'd0, b6[5], b6[4], b6[1], b6[0], b5[4], b5[7], b5[5], b5[6], up, down, left, right} */
        uint16_t dflt = (uint16_t)((((b6 >> 5) & 1) << 11) | (((b6 >> 4) & 1) << 10) | (((b6 >> 1) & 1) << 9) | (((b6 >> 0) & 1) << 8) |
                                   (((b5 >> 4) & 1) << 7) | (((b5 >> 7) & 1) << 6) | (((b5 >> 5) & 1) << 5) | (((b5 >> 6) & 1) << 4) |
                                   (up << 3) | (down << 2) | (left << 1) | right);
        if (padmgr_menu_from_raw(rw) != dflt) bad++;
    }
    CHECK(bad == 0, "%d of 20000 random DS4 reports differ from the RTL's dflt", bad);
}


/* ======================================================================= the new FPGA and the descriptor-driven ladder */
typedef struct {
    env_t e; padmgr_platform_t P; fake_pad_fpga_t fp; padmgr_t *m;
    const char *txt[2];          /* the card files' contents; NULL = absent */
    int nolinux;                 /* the whole /linux directory is missing */
    int pos[2], cur, chunk;      /* read chunk size (0 = as asked) */
    int open_calls[2], reads, open_now;
    uint32_t t;
    int dec_have; pd_table_t dec;
} rig_t;

static int r_open(void *c, int w) { rig_t *r = c; if (r->nolinux || !r->txt[w]) return 0; r->open_calls[w]++; r->pos[w] = 0; r->cur = w; r->open_now++; return 1; }
static int r_read(void *c, int w, char *buf, int cap)
{
    rig_t *r = c; (void)w;
    int left = (int)strlen(r->txt[r->cur]) - r->pos[r->cur];
    int n = cap < left ? cap : left;
    if (r->chunk && n > r->chunk) n = r->chunk;
    memcpy(buf, r->txt[r->cur] + r->pos[r->cur], (size_t)n); r->pos[r->cur] += n; r->reads++;
    return n;
}
static void r_close(void *c, int w) { rig_t *r = c; (void)w; r->open_now--; }
static uint32_t r_now(void *c) { return ((rig_t *)c)->t; }
static int r_dec(void *c, uint16_t v, uint16_t p, uint16_t ver, pd_table_t *o) { rig_t *r = c; (void)v; (void)p; (void)ver; if (!r->dec_have) return 0; *o = r->dec; return 1; }

/* the rig's own platform: env_t's callbacks with the card files and the clock */
static env_t *RIG_ENV;
static void rig_init(rig_t *r, int old, int launcher)
{
    memset(r, 0, sizeof *r);
    env_init(&r->e, &r->P, launcher);
    fpga_init(&r->fp, &r->e.rl, old);
    r->P.file_open = r_open; r->P.file_read = r_read; r->P.file_close = r_close; r->P.now_ms = r_now; r->P.dec_load = r_dec;
    /* the callbacks that need the rig get it as ctx; log/alloc/apply go through env_t, which is the first member */
    r->P.ctx = r;
    r->m = padmgr_new(&r->P);
    RIG_ENV = &r->e;
}
static void rig_done(rig_t *r) { padmgr_free(r->m); env_stop(&r->e); free(r->e.rec); }
/* ctx is the rig: wrappers forwarding to env_t's callbacks */
static void *rg_alloc(void *c, unsigned n) { return e_alloc(&((rig_t *)c)->e, n); }
static void rg_release(void *c, void *p) { e_release(&((rig_t *)c)->e, p); }
static void rg_log(void *c, const char *l) { e_log(&((rig_t *)c)->e, l); }
static void rg_apply(void *c, int port) { e_apply(&((rig_t *)c)->e, port); }
static void rig_start(rig_t *r, int old, int launcher)
{
    rig_init(r, old, launcher);
    r->P.alloc = rg_alloc; r->P.release = rg_release; r->P.log = rg_log; r->P.apply_map = launcher ? NULL : rg_apply;
    padmgr_free(r->m);
    r->m = padmgr_new(&r->P);
}
static void plug(rig_t *r, int port, uint16_t vid, uint16_t pid, uint16_t bcd, int cls, int sub, int proto, const uint8_t *rep, int rn, int ready)
{
    uint8_t cfg[18]; fpga_cfg((uint8_t)cls, (uint8_t)sub, (uint8_t)proto, cfg);
    fpga_plug(&r->fp, port, vid, pid, bcd, cfg, 18, rep, rn, ready);
}
/* the order the firmware has: com_task's legacy REPORT (tango_fw_hid_reports) first, which clears the port's changed bit, then the Tango task's snapshot */
static void snap(rig_t *r, int port)
{
    uint16_t v, p; uint32_t raw;
    uint8_t fl = tango_hid_report(&r->e.link, (uint8_t)port, &v, &p, &raw);
    padmgr_snapshot(r->m, port, fl, v, p, raw);
}
static long ncmd(rig_t *r, int c) { return r->fp.n_cmd[c]; }

/* the same SDL line the card/user files carry */
static void guid(char *o, uint16_t vid, uint16_t pid, uint16_t ver)
{
    sprintf(o, "03000000%02x%02x0000%02x%02x0000%02x%02x0000", vid & 255, vid >> 8, pid & 255, pid >> 8, ver & 255, ver >> 8);
}

static const uint8_t *ds4_idle(void)
{
    static uint8_t r[64]; memset(r, 0, sizeof r); r[0] = 1; r[1] = r[2] = r[3] = r[4] = 0x80; r[5] = 0x08; return r;
}
static const uint8_t *sr_idle(void) { static const uint8_t r[4] = { 0x7F, 0x7F, 0, 0 }; return r; }
static const char *PRS_NAME_FOR(int s)
{
    static const char *n[] = { "none", "saved decode", "DS4", "XInput", "user db", "card db", "built-in", "descriptor only", "legacy", "quiet" };
    return s >= 0 && s < 10 ? n[s] : "?";
}

static void test_new_fpga(void)
{
    printf("-- P2: the new FPGA, DS4 / XInput / descriptor-only pads\n");
    static rig_t R;
    /* ---- a DualShock 4: nothing is sent to the decode block, the FPGA's own decode stays */
    rig_start(&R, 0, 0);
    padmgr_core_start(R.m);
    CHECK(padmgr_fpga(R.m) == PADMGR_FPGA_NEW && R.fp.ctrl == TANGO_PADCTRL_EXT, "probe says new, PAD_CTRL ext events on (ctrl %02x)", R.fp.ctrl);
    plug(&R, 0, 0x054c, 0x09cc, 0x0100, 3, 0, 0, D_DS4, sizeof D_DS4, 1);
    uint8_t ds4r[64] = { 1, 0x80, 0x80, 0x80, 0x80, 0x28 /* hat 8, circle (bit 6?) */, 0, 0 };
    ds4r[5] = 0x08 | 0x40; ds4r[6] = 0x01;              /* released hat, circle, L1 */
    fpga_send_report(&R.fp, 0, ds4r, 64);
    snap(&R, 0);
    CHECK(padmgr_port_src(R.m, 0) == PRS_DS4 && padmgr_port_mode(R.m, 0) == PDM_DS4, "DS4 resolved as the built-in decode (src %d)", padmgr_port_src(R.m, 0));
    CHECK(ncmd(&R, TANGO_HID_SET_DECODE) == 0, "no SET_DECODE for a DS4 (the FPGA's power-up decode)");
    CHECK(R.e.apply_calls == 1 && R.e.applied[0] == 1, "the core map is applied once");
    CHECK(R.fp.p[0].raw == ((1u << 5) | (1u << 8)), "FPGA raw word %08x (circle + L1)", R.fp.p[0].raw);
    CHECK(R.e.nlog == 1 && strstr(R.e.logs[0], "054c:09cc") && strstr(R.e.logs[0], "DualShock 4"), "one log line: %s", R.e.logs[0]);
    CHECK(R.open_calls[0] + R.open_calls[1] == 0 || R.open_calls[0] == 0, "card files only when present");
    snap(&R, 0); snap(&R, 0);
    CHECK(R.e.apply_calls == 1 && R.e.nlog == 1, "further snapshots of the same pad do nothing");
    fpga_unplug(&R.fp, 0); snap(&R, 0);
    CHECK(padmgr_port_src(R.m, 0) == 0 && padmgr_port_mode(R.m, 0) == PDM_DS4, "unplugged: state cleared");
    rig_done(&R);
    CHECK(R.e.allocs == R.e.frees, "every allocation is freed (%ld/%ld)", R.e.allocs, R.e.frees);

    /* ---- Xbox 360 wired: class triplet, no report descriptor, a table is sent and verified */
    rig_start(&R, 0, 0);
    padmgr_core_start(R.m);
    plug(&R, 1, 0x045e, 0x028e, 0x0114, 0xFF, 0x5D, 0x01, NULL, 0, 1);
    uint8_t xr[20] = { 0, 20, 0x00, 0x10 /* A (xinput 0x1000) */, 0, 0 };
    fpga_send_report(&R.fp, 1, xr, 20);
    snap(&R, 1);
    CHECK(padmgr_port_src(R.m, 1) == PRS_XINPUT && padmgr_port_mode(R.m, 1) == PDM_TABLE, "XInput table (src %d)", padmgr_port_src(R.m, 1));
    pd_table_t xt; pd_builtin_xinput(&xt); uint8_t xi[400]; int xn = pd_wire(&xt, xi, sizeof xi);
    const uint8_t *img; int il = padmgr_port_image(R.m, 1, &img);
    CHECK(il == xn && !memcmp(img, xi, (size_t)xn), "the cached image is pd_wire of the XInput table (%d bytes)", il);
    CHECK(ncmd(&R, TANGO_HID_SET_DECODE) == 1 && R.fp.p[1].mode == PDM_TABLE && R.fp.p[1].count == xt.count, "the FPGA holds it: mode %d count %d", R.fp.p[1].mode, R.fp.p[1].count);
    uint32_t want; pd_eval(&xt, xr, 20, &want);
    CHECK(R.fp.p[1].raw == want && (want & (1u << RW_SOUTH)), "raw word %08x = the table's %08x, A pressed", R.fp.p[1].raw, want);
    CHECK(R.e.applied[1] == 1, "the core map follows the table");
    /* an incomplete table leaves mode 0; the retry succeeds */
    rig_done(&R);

    rig_start(&R, 0, 0); padmgr_core_start(R.m);
    R.fp.cut_set = 12;
    plug(&R, 1, 0x045e, 0x028e, 0x0114, 0xFF, 0x5D, 0x01, NULL, 0, 1); fpga_send_report(&R.fp, 1, xr, 20);
    snap(&R, 1);
    CHECK(ncmd(&R, TANGO_HID_SET_DECODE) == 2 && R.fp.p[1].mode == PDM_TABLE && !padmgr_blocked(R.m, 1), "an incomplete table is noticed (DECODE_STATUS) and sent again: %ld transfers", ncmd(&R, TANGO_HID_SET_DECODE));
    rig_done(&R);
    rig_start(&R, 0, 0); padmgr_core_start(R.m);
    R.fp.fail_set = 2;
    plug(&R, 1, 0x045e, 0x028e, 0x0114, 0xFF, 0x5D, 0x01, NULL, 0, 1); fpga_send_report(&R.fp, 1, xr, 20);
    snap(&R, 1);
    fflush(R.e.rl.out);
    CHECK(ncmd(&R, TANGO_HID_SET_DECODE) == 2 && padmgr_blocked(R.m, 1) && blank_ok(&R.e, 1) == 1 && R.e.apply_calls == 0,
          "two failures: quarantined and blanked, no map applied");
    rig_done(&R);
    CHECK(R.e.allocs == R.e.frees, "failure path frees everything (%ld/%ld)", R.e.allocs, R.e.frees);

    /* ---- descriptor only, an unknown 4-byte SNES-shaped pad */
    rig_start(&R, 0, 0); padmgr_core_start(R.m);
    plug(&R, 0, 0x1234, 0x5678, 0x0001, 3, 0, 0, D_SNES12, sizeof D_SNES12, 1);
    uint8_t sr[4] = { 0x00, 0x7F, 0x01, 0x00 };           /* X hard left, button 1 */
    fpga_send_report(&R.fp, 0, sr, 4);
    snap(&R, 0);
    CHECK(padmgr_port_src(R.m, 0) == PRS_HEURISTIC, "unknown pad + descriptor -> descriptor only (src %d)", padmgr_port_src(R.m, 0));
    CHECK((R.fp.p[0].raw & 0x3FFFF) == ((1u << RW_LEFT) | (1u << RW_SOUTH)), "left + south: %08x", R.fp.p[0].raw);
    uint8_t sr2[4] = { 0xFF, 0x00, 0x00, 0x02 };           /* X right, Y up, button 10 */
    fpga_send_report(&R.fp, 0, sr2, 4);
    CHECK(R.fp.p[0].raw & (1u << RW_RIGHT) && R.fp.p[0].raw & (1u << RW_UP), "right + up: %08x", R.fp.p[0].raw);
    rig_done(&R);

    /* ---- descriptors not ready: nothing is read until the ready bit, then one resolve */
    rig_start(&R, 0, 0); padmgr_core_start(R.m);
    plug(&R, 0, 0x1234, 0x5678, 0x0001, 3, 0, 0, D_SNES12, sizeof D_SNES12, 0);
    fpga_send_report(&R.fp, 0, sr, 4);
    snap(&R, 0); snap(&R, 0);
    CHECK(ncmd(&R, TANGO_HID_DESCRIPTOR) == 0 && R.e.apply_calls == 0 && padmgr_waiting(R.m), "waiting for the descriptors: nothing read, no map, tick wanted");
    R.t = 1000; padmgr_tick(R.m);
    CHECK(R.e.apply_calls == 0, "not yet (1 s)");
    fpga_set_ready(&R.fp, 0, 1); snap(&R, 0);
    CHECK(padmgr_port_src(R.m, 0) == PRS_HEURISTIC && R.e.apply_calls == 1 && !padmgr_waiting(R.m), "ready: resolved once");
    rig_done(&R);
    rig_start(&R, 0, 0); padmgr_core_start(R.m);
    plug(&R, 0, 0x1234, 0x5678, 0x0001, 3, 0, 0, D_SNES12, sizeof D_SNES12, 0);
    fpga_send_report(&R.fp, 0, sr, 4); snap(&R, 0);
    R.t = 3000; padmgr_tick(R.m);
    CHECK(padmgr_port_src(R.m, 0) == PRS_LEGACY && R.e.apply_calls == 1, "descriptors never came: resolved without them (legacy DS4 decode for an unknown pad), src %d", padmgr_port_src(R.m, 0));
    rig_done(&R);
    rig_start(&R, 0, 0); padmgr_core_start(R.m);
    plug(&R, 0, 0x045e, 0x028e, 0x0114, 0xFF, 0x5D, 0x01, NULL, 0, 0);
    fpga_send_report(&R.fp, 0, xr, 20); snap(&R, 0);
    R.t = 3000; padmgr_tick(R.m);
    CHECK(padmgr_port_src(R.m, 0) == PRS_QUIET && padmgr_port_mode(R.m, 0) == PDM_QUIET && R.fp.p[0].mode == PDM_QUIET,
          "a known pad without descriptors is quiet, not random bits (src %d)", padmgr_port_src(R.m, 0));
    rig_done(&R);

    /* ---- core start: the FPGA was reloaded; the cached image is sent again, no file or descriptor is read */
    rig_start(&R, 0, 0); padmgr_core_start(R.m);
    R.txt[0] = ""; R.txt[1] = "";
    plug(&R, 1, 0x045e, 0x028e, 0x0114, 0xFF, 0x5D, 0x01, NULL, 0, 1); fpga_send_report(&R.fp, 1, xr, 20);
    snap(&R, 1);
    long scans = padmgr_scans(R.m), d11 = ncmd(&R, TANGO_HID_DESCRIPTOR), s18 = ncmd(&R, TANGO_HID_SET_DECODE);
    CHECK(scans >= 1, "the files were scanned once (%ld)", scans);
    fpga_link_reset(&R.fp);
    padmgr_core_start(R.m);
    CHECK(R.fp.ctrl == TANGO_PADCTRL_EXT && R.fp.p[1].mode == 0, "after the reload: PAD_CTRL back on, table gone");
    snap(&R, 1);
    CHECK(R.fp.p[1].mode == 0, "device not re-enumerated yet: nothing sent into the void");
    fpga_set_ready(&R.fp, 1, 1); fpga_send_report(&R.fp, 1, xr, 20); snap(&R, 1);
    CHECK(R.fp.p[1].mode == PDM_TABLE && ncmd(&R, TANGO_HID_SET_DECODE) == s18 + 1, "the cached image is sent again");
    CHECK(padmgr_scans(R.m) == scans && ncmd(&R, TANGO_HID_DESCRIPTOR) == d11, "no file scan and no descriptor read for it");
    CHECK(R.e.applied[1] == 2, "and the core map is applied again (%d)", R.e.applied[1]);
    rig_done(&R);

    /* ---- an old FPGA (the fake answers 00 to the new commands): only the probe; the Xbox pad is quarantined; no file, no descriptor */
    rig_start(&R, 1, 0);
    R.txt[0] = ""; R.txt[1] = "";
    padmgr_core_start(R.m);
    plug(&R, 1, 0x045e, 0x028e, 0x0114, 0xFF, 0x5D, 0x01, NULL, 0, 1); fpga_send_report(&R.fp, 1, xr, 20);
    padmgr_snapshot(R.m, 1, 0x04, 0x045e, 0x028e, 0);
    CHECK(padmgr_fpga(R.m) == PADMGR_FPGA_OLD && padmgr_blocked(R.m, 1), "old FPGA: quarantine");
    CHECK(R.fp.n_cmd[0x19] == 0 && padmgr_scans(R.m) == 0 && R.open_calls[0] + R.open_calls[1] == 0, "old FPGA: no card file touched");
    rig_done(&R);
}

static void test_learn_flow(void)
{
    printf("-- P4: samples out, a user decode back in\n");
    static rig_t R; rig_start(&R, 0, 0); padmgr_core_start(R.m);
    plug(&R, 0, 0x1234, 0x5678, 0x0001, 3, 0, 0, NULL, 0, 1);          /* no descriptor: nothing to look the pad up with */
    uint8_t rp[4] = { 0x7F, 0x7F, 0, 0 };
    fpga_send_report(&R.fp, 0, rp, 4); snap(&R, 0);
    CHECK(padmgr_port_src(R.m, 0) == PRS_LEGACY, "unknown pad, no descriptor: legacy decode (src %d)", padmgr_port_src(R.m, 0));
    pl_sample_t idle[3], press[3]; pl_t L;
    for (int i = 0; i < 3; i++) { CHECK(padmgr_report_sample(R.m, 0, &idle[i]) == 1 && idle[i].len == 4, "idle sample"); }
    pl_begin(&L); pl_baseline(&L, idle, 3);
    uint8_t pr[4] = { 0x7F, 0x7F, 0x04, 0 };                              /* a button at byte 2 bit 2 */
    fpga_send_report(&R.fp, 0, pr, 4);
    for (int i = 0; i < 3; i++) padmgr_report_sample(R.m, 0, &press[i]);
    CHECK(pl_learn_button(&L, RW_SOUTH, press, 3) == PL_OK, "learned");
    pd_table_t t; pl_table(&L, &t);
    CHECK(padmgr_set_table(R.m, 0, &t) == 1 && R.fp.p[0].mode == PDM_TABLE && padmgr_port_src(R.m, 0) == PRS_USER_DEC, "the user's table is in force");
    CHECK(R.fp.p[0].raw == (1u << RW_SOUTH), "the FPGA decodes the learned button: %08x", R.fp.p[0].raw);
    const uint8_t *img; int il = padmgr_port_image(R.m, 0, &img);
    CHECK(il == 10, "cached for the next core start (%d bytes)", il);
    uint16_t v, p, ver;
    CHECK(padmgr_port_ids(R.m, 0, &v, &p, &ver) && v == 0x1234 && p == 0x5678, "ids for the .dec file name");
    rig_done(&R);
    /* the saved decode is ladder step 1: a replug (or the next boot) finds it by the pad's ids, before any file or descriptor */
    rig_start(&R, 0, 0); padmgr_core_start(R.m); R.dec_have = 1; R.dec = t; R.txt[0] = ""; R.txt[1] = "";
    plug(&R, 1, 0x1234, 0x5678, 0x0001, 3, 0, 0, D_SNES12, sizeof D_SNES12, 1);
    fpga_send_report(&R.fp, 1, pr, 4); snap(&R, 1);
    CHECK(padmgr_port_src(R.m, 1) == PRS_USER_DEC && R.fp.p[1].mode == PDM_TABLE && padmgr_scans(R.m) == 0 && R.fp.p[1].raw == (1u << RW_SOUTH), "saved decode wins before any file is read (src %d)", padmgr_port_src(R.m, 1));
    rig_done(&R);
    rig_start(&R, 1, 0);
    pl_sample_t s;
    CHECK(padmgr_report_sample(R.m, 0, &s) == 0 && padmgr_set_table(R.m, 0, &t) == 0, "an old FPGA: no samples, no table");
    rig_done(&R);
}

/* every allocation of a resolve fails in turn: no crash, nothing leaks, a known non-DS4 pad never ends on the DualShock 4 decode */
static void test_oom(void)
{
    printf("-- allocation failures\n");
    static rig_t R;
    const struct { const char *n; uint16_t v, p; int cls, sub, proto; const uint8_t *d; int dn; } PADS[] = {
        { "xbox", 0x045e, 0x028e, 0xFF, 0x5D, 1, NULL, 0 }, { "snes-unknown", 0x1234, 0x5678, 3, 0, 0, D_SNES12, sizeof D_SNES12 },
        { "known-hat12", 0x046d, 0xc216, 3, 0, 0, D_HAT12, sizeof D_HAT12 }, { "ds4", 0x054c, 0x09cc, 3, 0, 0, D_DS4, sizeof D_DS4 } };
    for (unsigned k = 0; k < sizeof PADS / sizeof PADS[0]; k++) {
        int survived = 0;
        for (int fail = 1; fail <= 6; fail++) {
            rig_start(&R, 0, 0); padmgr_core_start(R.m);
            R.e.alloc_n = 0; R.e.fail_at = fail;
            plug(&R, 0, PADS[k].v, PADS[k].p, 0x0100, PADS[k].cls, PADS[k].sub, PADS[k].proto, PADS[k].d, PADS[k].dn, 1);
            fpga_send_report(&R.fp, 0, ds4_idle(), 64); snap(&R, 0);
            int st = padmgr_port_src(R.m, 0);
            if (R.e.alloc_n < fail) { rig_done(&R); CHECK(R.e.allocs == R.e.frees, "%s: no failure was reached at %d, leak check %ld/%ld", PADS[k].n, fail, R.e.allocs, R.e.frees); break; }
            (void)st;
            if (!strcmp(PADS[k].n, "xbox")) CHECK(R.fp.p[0].mode != PDM_DS4 || padmgr_blocked(R.m, 0), "xbox, allocation %d fails: either a table is in force or the port is blanked (never the DS4 decode alone)", fail);
            rig_done(&R);
            CHECK(R.e.allocs == R.e.frees, "%s: allocation %d fails: no leak (%ld/%ld)", PADS[k].n, fail, R.e.allocs, R.e.frees);
            survived++;
        }
        CHECK(survived >= 1, "%s: at least one failure point was exercised", PADS[k].n);
    }
}

static void test_swap(void)
{
    printf("-- P5: the swap bit\n");
    static rig_t R; rig_start(&R, 0, 0);
    padmgr_set_swap(R.m, 1);                                    /* the saved setting, read before the probe */
    padmgr_core_start(R.m);
    CHECK(R.fp.ctrl == (TANGO_PADCTRL_EXT | TANGO_PADCTRL_SWAP), "first PAD_CTRL carries ext events + swap (%02x)", R.fp.ctrl);
    CHECK(padmgr_swap(R.m) && padmgr_swap_supported(R.m), "getter, supported");
    CHECK(padmgr_set_swap(R.m, 0) == 1 && R.fp.ctrl == TANGO_PADCTRL_EXT, "swap off keeps the ext-events bit (%02x)", R.fp.ctrl);
    padmgr_set_swap(R.m, 1);
    fpga_link_reset(&R.fp);
    CHECK(R.fp.ctrl == 0, "a core load clears both bits in the FPGA");
    padmgr_core_start(R.m);
    CHECK(R.fp.ctrl == 3, "and the next core start sends both again (%02x)", R.fp.ctrl);
    rig_done(&R);
    rig_start(&R, 1, 0); padmgr_core_start(R.m);
    CHECK(padmgr_set_swap(R.m, 1) == 0 && padmgr_swap(R.m) == 1 && !padmgr_swap_supported(R.m) && R.fp.n_cmd[TANGO_HID_PAD_CTRL] == 0, "old FPGA: not supported, nothing sent, the request is remembered");
    rig_done(&R);
}

static void test_cursor(void)
{
    printf("-- the descriptor cursor is per link\n");
    static rig_t R; rig_start(&R, 0, 0); padmgr_core_start(R.m);
    plug(&R, 0, 0x054c, 0x09cc, 0x0100, 3, 0, 0, D_DS4, sizeof D_DS4, 1);
    plug(&R, 1, 0x1234, 0x5678, 0x0001, 3, 0, 0, D_SNES12, sizeof D_SNES12, 1);
    const tango_link_t *l = &R.e.link;
    uint8_t a[8], b[8], st; uint16_t n;
    tango_hid_desc_select(l, 0, TANGO_DESC_REPORT);
    tango_hid_descriptor(l, 0, &st, &n, a, 8);
    CHECK(n == sizeof D_DS4 && !memcmp(a, D_DS4, 8), "port 0 report descriptor, first 8 bytes (stored length %u)", n);
    tango_hid_desc_select(l, 1, TANGO_DESC_REPORT);                  /* resets the one cursor */
    tango_hid_descriptor(l, 0, &st, &n, b, 8);                       /* port 0 again, but the cursor is at 0 and the selector is global */
    CHECK(!memcmp(b, D_DS4, 8), "a new select rewinds the cursor for every port");
    tango_hid_descriptor(l, 1, NULL, NULL, b, 8);
    CHECK(!memcmp(b, D_SNES12 + 8, 8), "and the cursor advanced by the 8 bytes clocked (port 1 continues at 8)");
    /* two ports resolved back to back, each with its own select */
    fpga_send_report(&R.fp, 0, ds4_idle(), 64); fpga_send_report(&R.fp, 1, sr_idle(), 4);
    snap(&R, 0); snap(&R, 1);
    CHECK(padmgr_port_src(R.m, 0) == PRS_DS4 && padmgr_port_src(R.m, 1) == PRS_HEURISTIC, "both ports resolved from their own descriptors (%d, %d)", padmgr_port_src(R.m, 0), padmgr_port_src(R.m, 1));
    rig_done(&R);
}

/* ---- the ladder through the file callbacks */
static const char *BODY_HAT12 = "a:b0,b:b1,x:b2,y:b3,back:b8,start:b9,leftshoulder:b4,rightshoulder:b5,dpup:h0.1,dpright:h0.2,dpdown:h0.4,dpleft:h0.8,leftx:a0,lefty:a1,platform:Linux,";

static char *mkfile(const char *eol, const char *fmt_lines[], int n)
{
    size_t cap = 1; for (int i = 0; i < n; i++) cap += strlen(fmt_lines[i]) + 4;
    char *s = malloc(cap); s[0] = 0;
    for (int i = 0; i < n; i++) { strcat(s, fmt_lines[i]); strcat(s, eol); }
    return s;
}

static void expect_table(rig_t *r, int port, const sdl_map_t *line, const uint8_t *desc, int dn, const char *what)
{
    hp_layout_t *L = malloc(sizeof *L); pd_table_t t; pd_build_info_t info;
    int ok = hp_parse(desc, dn, L) && pd_from_sdl(L, line, &t, &info);
    CHECK(ok, "%s: the oracle builds a table", what);
    uint8_t w[400]; int wn = ok ? pd_wire(&t, w, sizeof w) : 0;
    const uint8_t *img; int il = padmgr_port_image(r->m, port, &img);
    CHECK(ok && il == wn && !memcmp(img, w, (size_t)wn), "%s: the image sent is pd_wire of the table built from that very line (%d vs %d bytes)", what, il, wn);
    free(L);
}

static void test_ladder_files(void)
{
    printf("-- P3: every tier through the file callbacks\n");
    static rig_t R; char g[64], l1[700], l2[700], l3[700]; sdl_map_t m;

    /* user line beats the DS4 shortcut */
    guid(g, 0x054c, 0x09cc, 0x0100);
    snprintf(l1, sizeof l1, "%s,Mine,a:b1,b:b2,x:b0,y:b3,back:b8,start:b9,leftshoulder:b4,rightshoulder:b5,dpup:h0.1,dpright:h0.2,dpdown:h0.4,dpleft:h0.8,leftx:a0,lefty:a1,platform:Linux,", g);
    const char *ul[] = { "# my pads", l1 };
    char *user = mkfile("\n", ul, 2);
    rig_start(&R, 0, 0); padmgr_core_start(R.m); R.txt[0] = user; R.txt[1] = "";
    plug(&R, 0, 0x054c, 0x09cc, 0x0100, 3, 0, 0, D_DS4, sizeof D_DS4, 1); fpga_send_report(&R.fp, 0, ds4_idle(), 64);
    snap(&R, 0);
    CHECK(padmgr_port_src(R.m, 0) == PRS_USER_DB && R.open_calls[1] == 0, "user line beats the DS4 shortcut, the card file is not even opened (src %d)", padmgr_port_src(R.m, 0));
    sdl_parse_line(l1, &m); expect_table(&R, 0, &m, D_DS4, sizeof D_DS4, "user/DS4");
    CHECK(R.fp.p[0].mode == PDM_TABLE, "and the FPGA runs it");
    rig_done(&R);

    /* the user file has no line for the DS4: the DS4 shortcut applies, and the answer is remembered */
    rig_start(&R, 0, 0); padmgr_core_start(R.m); R.txt[0] = user;
    guid(g, 0x1111, 0x2222, 0x0100); snprintf(l2, sizeof l2, "%s,Other,a:b0,platform:Linux,", g);
    const char *ul2[] = { l2 }; char *user2 = mkfile("\n", ul2, 1); R.txt[0] = user2;
    plug(&R, 0, 0x054c, 0x09cc, 0x0100, 3, 0, 0, D_DS4, sizeof D_DS4, 1); fpga_send_report(&R.fp, 0, ds4_idle(), 64); snap(&R, 0);
    CHECK(padmgr_port_src(R.m, 0) == PRS_DS4 && R.open_calls[0] == 1, "DS4 with a user file that does not know it -> built-in decode, user file read once");
    fpga_unplug(&R.fp, 0); snap(&R, 0);
    plug(&R, 0, 0x054c, 0x09cc, 0x0100, 3, 0, 0, D_DS4, sizeof D_DS4, 1); fpga_send_report(&R.fp, 0, ds4_idle(), 64); snap(&R, 0);
    CHECK(R.open_calls[0] == 1 && padmgr_scans(R.m) == 1, "second plug-in: the negative answer is remembered, no rescan (opens %d, scans %ld)", R.open_calls[0], padmgr_scans(R.m));
    rig_done(&R);
    free(user2);

    /* card file beats the built-in table; the built-in table is the no-card fallback */
    guid(g, 0x046d, 0xc216, 0x0000);
    snprintf(l3, sizeof l3, "%s,Card,%s", g, BODY_HAT12);
    const char *cl[] = { l3 }; char *card = mkfile("\n", cl, 1);
    rig_start(&R, 0, 0); padmgr_core_start(R.m); R.txt[0] = NULL; R.txt[1] = card;
    plug(&R, 0, 0x046d, 0xc216, 0x0000, 3, 0, 0, D_HAT12, sizeof D_HAT12, 1); fpga_send_report(&R.fp, 0, (const uint8_t *)"\x80\x80\x80\x80\x0f\x00", 6); snap(&R, 0);
    CHECK(padmgr_port_src(R.m, 0) == PRS_DB, "card file beats the built-in table (src %d)", padmgr_port_src(R.m, 0));
    sdl_parse_line(l3, &m); expect_table(&R, 0, &m, D_HAT12, sizeof D_HAT12, "card/hat12");
    rig_done(&R);
    rig_start(&R, 0, 0); padmgr_core_start(R.m); R.nolinux = 1;       /* no card, no /linux directory */
    plug(&R, 0, 0x046d, 0xc216, 0x0000, 3, 0, 0, D_HAT12, sizeof D_HAT12, 1); fpga_send_report(&R.fp, 0, (const uint8_t *)"\x80\x80\x80\x80\x0f\x00", 6); snap(&R, 0);
    int src = padmgr_port_src(R.m, 0);
    CHECK(src == PRS_BUILTIN || src == PRS_HEURISTIC, "no card: the built-in table (or the descriptor) answers, nothing crashes (src %d)", src);
    CHECK(R.open_calls[0] + R.open_calls[1] == 0 && R.e.nlog >= 1, "the missing /linux directory is not an error");
    rig_done(&R);
    printf("   046d:c216 without a card resolved as: %s\n", PRS_NAME_FOR(src));
    CHECK(src == PRS_BUILTIN, "no card at all: the built-in table answers");
    /* the built-in table is the NO-CARD fallback only. A card file that opens and lacks the pad is the database: no built-in line. */
    const char *oth[] = { l1 }; char *other = mkfile("\n", oth, 1);
    rig_start(&R, 0, 0); padmgr_core_start(R.m); R.txt[1] = other;
    plug(&R, 0, 0x046d, 0xc216, 0x0000, 3, 0, 0, D_HAT12, sizeof D_HAT12, 1); fpga_send_report(&R.fp, 0, (const uint8_t *)"\x80\x80\x80\x80\x0f\x00", 6); snap(&R, 0);
    CHECK(padmgr_port_src(R.m, 0) == PRS_HEURISTIC, "card file present, pad not in it: not the built-in table (src %d)", padmgr_port_src(R.m, 0));
    fpga_unplug(&R.fp, 0); snap(&R, 0);
    plug(&R, 0, 0x046d, 0xc216, 0x0000, 3, 0, 0, D_HAT12, sizeof D_HAT12, 1); fpga_send_report(&R.fp, 0, (const uint8_t *)"\x80\x80\x80\x80\x0f\x00", 6); snap(&R, 0);
    CHECK(padmgr_port_src(R.m, 0) == PRS_HEURISTIC && R.open_calls[1] == 1, "same answer from the negative cache, file read once (src %d, opens %d)", padmgr_port_src(R.m, 0), R.open_calls[1]);
    rig_done(&R); free(other);

    /* unknown pad: scanned once per file, a second plug-in does not rescan */
    rig_start(&R, 0, 0); padmgr_core_start(R.m); R.txt[0] = user2 = mkfile("\n", ul2, 1); R.txt[1] = card;
    plug(&R, 0, 0x1234, 0x5678, 0x0001, 3, 0, 0, D_SNES12, sizeof D_SNES12, 1); fpga_send_report(&R.fp, 0, sr_idle(), 4); snap(&R, 0);
    CHECK(padmgr_port_src(R.m, 0) == PRS_HEURISTIC && padmgr_scans(R.m) == 2, "unknown pad: both files scanned once (%ld)", padmgr_scans(R.m));
    fpga_unplug(&R.fp, 0); snap(&R, 0);
    plug(&R, 1, 0x1234, 0x5678, 0x0001, 3, 0, 0, D_SNES12, sizeof D_SNES12, 1); fpga_send_report(&R.fp, 1, sr_idle(), 4); snap(&R, 1);
    CHECK(padmgr_scans(R.m) == 2 && padmgr_port_src(R.m, 1) == PRS_HEURISTIC, "same pad again, on the other port: no rescan (%ld)", padmgr_scans(R.m));
    rig_done(&R);
    free(user2);

    /* CRLF, chunked reads (1 byte at a time), a last line with no newline, comments and blank lines */
    const char *cr[] = { "# card", "", "garbage line with no guid", l3 };
    char *crlf = mkfile("\r\n", cr, 4); size_t cl_ = strlen(crlf); crlf[cl_ - 2] = 0;      /* strip the final CRLF: no terminator on the last line */
    rig_start(&R, 0, 0); padmgr_core_start(R.m); R.txt[1] = crlf; R.chunk = 1;
    plug(&R, 0, 0x046d, 0xc216, 0x0000, 3, 0, 0, D_HAT12, sizeof D_HAT12, 1); fpga_send_report(&R.fp, 0, (const uint8_t *)"\x80\x80\x80\x80\x0f\x00", 6); snap(&R, 0);
    CHECK(padmgr_port_src(R.m, 0) == PRS_DB, "CRLF file, one byte per read, unterminated last line: found (src %d)", padmgr_port_src(R.m, 0));
    sdl_parse_line(l3, &m); expect_table(&R, 0, &m, D_HAT12, sizeof D_HAT12, "crlf");
    rig_done(&R);
    free(crlf);

    /* an over-long line is dropped whole: its head alone would be a valid line for this pad with tokens missing */
    static char big[4096];
    snprintf(big, sizeof big, "%s,Head,%spad:", g, BODY_HAT12);     /* the head is a complete, valid, usable line; the padding after it is what makes it too long */
    size_t k = strlen(big); memset(big + k, 'x', 1500); big[k + 1500] = 0;
    char *tail = big + k + 1500; *tail++ = '\n'; strcpy(tail, l3); strcat(tail, "\n");
    rig_start(&R, 0, 0); padmgr_core_start(R.m); R.txt[1] = big;
    plug(&R, 0, 0x046d, 0xc216, 0x0000, 3, 0, 0, D_HAT12, sizeof D_HAT12, 1); fpga_send_report(&R.fp, 0, (const uint8_t *)"\x80\x80\x80\x80\x0f\x00", 6); snap(&R, 0);
    CHECK(padmgr_port_src(R.m, 0) == PRS_DB, "the over-long line is skipped, the next line is read (src %d)", padmgr_port_src(R.m, 0));
    sdl_parse_line(l3, &m); expect_table(&R, 0, &m, D_HAT12, sizeof D_HAT12, "overlong");
    int dropped_logged = 0; for (int i = 0; i < R.e.nlog && i < 16; i++) if (strstr(R.e.logs[i], "over-long")) dropped_logged = 1;
    CHECK(dropped_logged, "and the drop is logged");
    rig_done(&R);
    /* the same file with ONLY the over-long line: it must not match */
    big[k + 1500] = '\n'; big[k + 1501] = 0;
    rig_start(&R, 0, 0); padmgr_core_start(R.m); R.txt[1] = big;
    plug(&R, 0, 0x046d, 0xc216, 0x0000, 3, 0, 0, D_HAT12, sizeof D_HAT12, 1); fpga_send_report(&R.fp, 0, (const uint8_t *)"\x80\x80\x80\x80\x0f\x00", 6); snap(&R, 0);
    CHECK(padmgr_port_src(R.m, 0) != PRS_DB, "a file whose only line is over-long gives no card match (src %d)", padmgr_port_src(R.m, 0));
    rig_done(&R);
    free(card);
}

static void test_quiet_devices(void)
{
    printf("-- P3: mice, keyboards and remotes resolve quiet\n");
    static rig_t R;
    const struct { const char *n; const uint8_t *d; int l; } DEV[] = { { "mouse", D_MOUSE, sizeof D_MOUSE }, { "keyboard", D_KBD, sizeof D_KBD }, { "consumer", D_CONSUMER, sizeof D_CONSUMER } };
    for (unsigned i = 0; i < 3; i++) {
        rig_start(&R, 0, 0); padmgr_core_start(R.m);
        plug(&R, 0, 0x1c4f, 0x0034, 0x0100, 3, 1, 2, DEV[i].d, DEV[i].l, 1);
        uint8_t rp[8] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
        fpga_send_report(&R.fp, 0, rp, 8);
        snap(&R, 0);
        CHECK(padmgr_port_src(R.m, 0) == PRS_QUIET && R.fp.p[0].mode == PDM_QUIET, "%s: quiet (src %d)", DEV[i].n, padmgr_port_src(R.m, 0));
        fpga_send_report(&R.fp, 0, rp, 8);
        CHECK(R.fp.p[0].raw == 0, "%s: the raw word stays 0 (%08x)", DEV[i].n, R.fp.p[0].raw);
        rig_done(&R);
    }
}

static char *slurp(const char *path, long *n)
{
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); *n = ftell(f); fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)*n + 1); if (fread(b, 1, (size_t)*n, f) != (size_t)*n) { fclose(f); free(b); return NULL; }
    b[*n] = 0; fclose(f); return b;
}

static void test_real(void)
{
    printf("-- P3: real descriptors and real SDL files\n");
    static rig_t R;
    for (unsigned i = 0; i < sizeof REAL_DESCS / sizeof REAL_DESCS[0]; i++) {
        static hp_layout_t L;
        int pad = hp_parse(REAL_DESCS[i].d, REAL_DESCS[i].n, &L);
        rig_start(&R, 0, 0); padmgr_core_start(R.m);
        plug(&R, 0, 0x046d, 0xc534, 0x2900, 3, 1, 2, REAL_DESCS[i].d, REAL_DESCS[i].n, 1);
        uint8_t rp[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
        fpga_send_report(&R.fp, 0, rp, 8); snap(&R, 0);
        printf("   %-24s %3d bytes: parser class %d pad %d -> src %s\n", REAL_DESCS[i].name, REAL_DESCS[i].n, L.cls, pad, PRS_NAME_FOR(padmgr_port_src(R.m, 0)));
        CHECK(!pad, "%s: a keyboard / mouse / vendor interface is not a pad", REAL_DESCS[i].name);
        CHECK(padmgr_port_src(R.m, 0) != PRS_LEGACY && padmgr_port_src(R.m, 0) != PRS_HEURISTIC, "%s: not decoded as a pad (src %d)", REAL_DESCS[i].name, padmgr_port_src(R.m, 0));
        if (L.cls == HPC_MOUSE || L.cls == HPC_KEYBOARD || L.cls == HPC_OTHER) CHECK(R.fp.p[0].mode == PDM_QUIET && R.fp.p[0].raw == 0, "%s: quiet", REAL_DESCS[i].name);
        rig_done(&R);
    }
    /* real gamecontrollerdb.txt files when the machine has them: BlastEm's, and any other named by TANGO_GCDB */
    const char *paths[] = { "/usr/share/games/blastem/gamecontrollerdb.txt", getenv("TANGO_GCDB") };
    for (int k = 0; k < 2; k++) {
        if (!paths[k]) { printf("   TANGO_GCDB unset: no second file\n"); continue; }
        long n; char *txt = slurp(paths[k], &n);
        if (!txt) { printf("   %s: absent, skipped\n", paths[k]); continue; }
        int lines = 0; for (long i = 0; i < n; i++) if (txt[i] == '\n') lines++;
        /* a pad the file does not know: both files are read to the end, with the card chunk size of 256 */
        rig_start(&R, 0, 0); padmgr_core_start(R.m); R.txt[1] = txt; R.txt[0] = "";
        plug(&R, 0, 0xdead, 0xbeef, 0x0001, 3, 0, 0, D_SNES12, sizeof D_SNES12, 1); fpga_send_report(&R.fp, 0, sr_idle(), 4);
        struct timespec a, b; clock_gettime(CLOCK_MONOTONIC, &a);
        int reps = 20; for (int r = 0; r < reps; r++) { fpga_unplug(&R.fp, 0); padmgr_free(R.m); R.m = padmgr_new(&R.P); padmgr_core_start(R.m); plug(&R, 0, 0xdead, 0xbeef, 0x0001, 3, 0, 0, D_SNES12, sizeof D_SNES12, 1); fpga_send_report(&R.fp, 0, sr_idle(), 4); snap(&R, 0); }
        clock_gettime(CLOCK_MONOTONIC, &b);
        double s = (double)(b.tv_sec - a.tv_sec) + (double)(b.tv_nsec - a.tv_nsec) / 1e9;
        printf("   %s: %d lines, %ld bytes; unknown-pad full scan %.2f ms (%.0f lines/s, %.1f MB/s) on this PC (the BL616 figure is unmeasured)\n",
               paths[k], lines, n, s / reps * 1e3, lines * reps / s, (double)n * reps / s / 1e6);
        CHECK(padmgr_port_src(R.m, 0) == PRS_HEURISTIC, "unknown pad in a real file -> descriptor only");
        rig_done(&R);
        /* a pad the file knows: the DualShock 4 family is answered before the card file; an 8BitDo / Retrolink pad through it */
        rig_start(&R, 0, 0); padmgr_core_start(R.m); R.txt[1] = txt;
        plug(&R, 0, 0x2dc8, 0x6001, 0x0000, 3, 0, 0, D_XBOXHID, sizeof D_XBOXHID, 1); fpga_send_report(&R.fp, 0, (const uint8_t *)"\0\0\0\0\0\0\0\0\0\0\0\0\0\0", 14); snap(&R, 0);
        int s2 = padmgr_port_src(R.m, 0);
        printf("   2dc8:6001 (8BitDo) with D_XBOXHID against this file: %s\n", PRS_NAME_FOR(s2));
        CHECK(s2 != PRS_LEGACY && s2 != PRS_QUIET, "a known pad gets a table (src %d)", s2);
        rig_done(&R);
        free(txt);
    }
}

/* ======================================================================= the session layer (Define buttons: what the menu calls) */
typedef struct {
    char dec_name[64]; uint8_t dec[400]; int dec_n, dec_saves;
    char db[8192]; int db_exists; char tmp[8192]; int tn, rpos, commits, discards;
    int fail_commit;
} cardf_t;
static int cf_save_dec(void *c, const char *n, const uint8_t *d, int len) { cardf_t *f = c; snprintf(f->dec_name, sizeof f->dec_name, "%s", n); memcpy(f->dec, d, (size_t)len); f->dec_n = len; f->dec_saves++; return 1; }
static int cf_open_old(void *c) { cardf_t *f = c; f->rpos = 0; return f->db_exists; }
static int cf_read(void *c, char *b, int cap) { cardf_t *f = c; int left = (int)strlen(f->db) - f->rpos; int n = left < cap ? left : (cap > 7 ? 7 : cap); if (n < 0) n = 0; memcpy(b, f->db + f->rpos, (size_t)n); f->rpos += n; return n; }
static void cf_close_old(void *c) { (void)c; }
static int cf_open_new(void *c) { cardf_t *f = c; f->tn = 0; f->tmp[0] = 0; return 1; }
static int cf_write(void *c, const char *b, int n) { cardf_t *f = c; if (f->tn + n >= (int)sizeof f->tmp) return 0; memcpy(f->tmp + f->tn, b, (size_t)n); f->tn += n; f->tmp[f->tn] = 0; return 1; }
static int cf_commit(void *c) { cardf_t *f = c; if (f->fail_commit) return 0; memcpy(f->db, f->tmp, (size_t)f->tn + 1); f->db_exists = 1; f->commits++; return 1; }
static void cf_discard(void *c) { cardf_t *f = c; f->discards++; }
static plsess_files_t cf_table(cardf_t *f) { plsess_files_t t = { f, cf_save_dec, cf_open_old, cf_read, cf_close_old, cf_open_new, cf_write, cf_commit, cf_discard }; return t; }

typedef struct {
    const pd_table_t *T; int rid, len; int noisy;
    int cancel_at, held_calls;                 /* cancel on the Nth HELD call */
    int nochange_rw, ambig_rw;                 /* the first HELD for this bit shows no change / two separate analog groups */
    int noreport_rw;
    int order[64], norder, idles;
} script_t;

static int script_cap(void *c, int phase, pl_sample_t *out, int max)
{
    script_t *s = c; rep_t idle; build_idle(s->T, s->rid, s->len, &idle);
    if (PLC_KIND(phase) == PLC_IDLE) {
        s->idles++;
        int n = max < 6 ? max : 6;
        for (int k = 0; k < n; k++) { rep_t r = idle; if (s->noisy) add_noise(s->T, &r, k); out[k] = to_sample(&r); }
        return n;
    }
    int rw = PLC_RW(phase);
    s->held_calls++;
    if (s->norder < 64) s->order[s->norder++] = rw;
    if (s->cancel_at && s->held_calls == s->cancel_at) return -1;
    if (s->noreport_rw == rw + 1) { s->noreport_rw = 0; return 0; }
    int n = max < 4 ? max : 4;
    for (int k = 0; k < n; k++) {
        rep_t r = idle;
        if (s->nochange_rw == rw + 1) r = idle;                           /* nothing moves */
        else if (s->ambig_rw == rw + 1 && s->len >= 6) { r.r[1] ^= 0x3C; r.r[s->len - 1] ^= 0x3C; }
        else press_out(s->T, rw, &r);
        if (s->noisy) add_noise(s->T, &r, k + 5);
        out[k] = to_sample(&r);
    }
    if (s->nochange_rw == rw + 1) s->nochange_rw = 0;
    if (s->ambig_rw == rw + 1) s->ambig_rw = 0;
    return n;
}

static int dpad_all(const pd_table_t *T) { return has_out(T, RW_UP) && has_out(T, RW_RIGHT) && has_out(T, RW_DOWN) && has_out(T, RW_LEFT); }

/* the walk a page makes over plsess_default_order; returns the first non-OK code, PLS_OK when everything went through */
static int walk(plsess_t *S, script_t *sc, int stop_on_bad)
{
    char msg[40]; int bad = PLS_OK;
    for (int i = 0; i < plsess_default_order_n; i++) {
        const plsess_item_t *it = &plsess_default_order[i];
        int rc = PLS_OK;
        if (it->kind == PLI_DPAD) {
            if (it->rw != RW_RIGHT) continue;                           /* the group is one call */
            if (dpad_all(sc->T)) rc = plsess_dpad(S, script_cap, sc, msg, sizeof msg);
        } else if (has_out(sc->T, it->rw)) rc = plsess_step(S, it->rw, script_cap, sc, msg, sizeof msg);
        else plsess_skip(S, it->rw);
        if (rc != PLS_OK) { bad = rc; if (stop_on_bad) return rc; }
    }
    return bad;
}

static void db_lines(const char *db, const char *guid, int *ours, int *foreign, int *total)
{
    *ours = *foreign = *total = 0;
    for (const char *p = db; *p; ) {
        const char *e = strchr(p, '\n'); size_t l = e ? (size_t)(e - p) : strlen(p);
        if (l) { (*total)++; if (!strncasecmp(p, guid, 32)) { if (memmem(p, l, PLS_NAME_TAG, strlen(PLS_NAME_TAG))) (*ours)++; else (*foreign)++; } }
        if (!e) break;
        p = e + 1;
    }
}

static void test_session(void)
{
    printf("-- Define buttons: the session layer\n");
    static rig_t R; static hp_layout_t HL; static pd_table_t T; static plsess_t S; static cardf_t F; pd_build_info_t info; char msg[40], sdl[600];
    const struct { const char *n; const uint8_t *d; int l; } FX[] = { { "ds4", D_DS4, sizeof D_DS4 }, { "snes12", D_SNES12, sizeof D_SNES12 }, { "hat12", D_HAT12, sizeof D_HAT12 },
        { "xboxhid", D_XBOXHID, sizeof D_XBOXHID }, { "retro5", D_RETRO5, sizeof D_RETRO5 }, { "hat4", D_HAT4, sizeof D_HAT4 }, { "twoid", D_TWOID, sizeof D_TWOID } };
    CHECK(sizeof(plsess_t) <= PLSESS_BYTES, "plsess_t is %zu bytes (limit %d)", sizeof(plsess_t), PLSESS_BYTES);
    printf("   sizeof(plsess_t) = %zu (PLSESS_BYTES %d)\n", sizeof(plsess_t), PLSESS_BYTES);
    for (unsigned k = 0; k < sizeof FX / sizeof FX[0]; k++) {
        for (int noisy = 0; noisy < 2; noisy++) {
            if (!hp_parse(FX[k].d, FX[k].l, &HL) || !pd_from_layout(&HL, &T, &info)) continue;
            int len = HL.bytes > PD_RPT_BYTES ? PD_RPT_BYTES : HL.bytes;
            rig_start(&R, 0, 0); padmgr_core_start(R.m); memset(&F, 0, sizeof F);
            plug(&R, 0, 0x1234, 0x5678, 0x0100, 3, 0, 0, FX[k].d, FX[k].l, 1);
            rep_t id; build_idle(&T, HL.rid, len, &id); fpga_send_report(&R.fp, 0, id.r, len); snap(&R, 0);
            int before = (int)R.fp.n_cmd[TANGO_HID_SET_DECODE];
            script_t sc; memset(&sc, 0, sizeof sc); sc.T = &T; sc.rid = HL.rid; sc.len = len; sc.noisy = noisy;
            plsess_begin(&S, 0x1234, 0x5678, 0x0100, &HL, FX[k].n);
            CHECK(plsess_baseline(&S, script_cap, &sc) == PLS_OK, "%s: baseline", FX[k].n);
            CHECK(walk(&S, &sc, 0) == PLS_OK, "%s%s: every input learned", FX[k].n, noisy ? " (noisy)" : "");
            plsess_files_t files = cf_table(&F);
            int fl = plsess_finish(&S, R.m, 0, &files, sdl, sizeof sdl);
            CHECK((fl & (PLF_APPLIED | PLF_DEC | PLF_SDL)) == (PLF_APPLIED | PLF_DEC | PLF_SDL), "%s: applied, .dec and SDL stored (flags %d)", FX[k].n, fl);
            CHECK(R.fp.n_cmd[TANGO_HID_SET_DECODE] == before + 1 && R.fp.p[0].mode == PDM_TABLE && padmgr_port_src(R.m, 0) == PRS_USER_DEC, "%s: the FPGA holds the user's table", FX[k].n);
            /* the FPGA's table gives the reference answer for every input alone and for rest */
            int bad = 0;
            rep_t idl; build_idle(&T, HL.rid, len, &idl);
            uint32_t S_ = 0; for (int i = 0; i < T.count; i++) S_ |= 1u << T.e[i].out;
            uint32_t got = 0; pd_eval(&R.fp.p[0].tab, idl.r, len, &got);
            if (got & S_) { bad++; if (getenv("PM_DB")) printf("rest raw %08x S %08x\n", got, S_); }
            for (int o = 0; o < 26; o++) {
                int inord = 0; for (int q = 0; q < plsess_default_order_n; q++) if (plsess_default_order[q].rw == o) inord = 1;
                if (!has_out(&T, o) || !inord) continue;
                rep_t r = idl; press_out(&T, o, &r); pd_eval(&R.fp.p[0].tab, r.r, len, &got);
                if (!((got >> o) & 1)) { bad++; if (getenv("PM_DB")) printf("%s: out %d missing, raw %08x\n", FX[k].n, o, got); }
            }
            CHECK(bad == 0, "%s%s: the stored table answers rest and every input (%d wrong)", FX[k].n, noisy ? " (noisy)" : "", bad);
            char nm[40]; pl_dec_name(0x1234, 0x5678, 0x0100, nm, sizeof nm);
            pd_table_t back; CHECK(!strcmp(F.dec_name, nm) && pl_dec_load(F.dec, F.dec_n, 0x1234, 0x5678, 0x0100, &back) && back.count == R.fp.p[0].tab.count, "%s: .dec file %s round-trips", FX[k].n, F.dec_name);
            int ours, foreign, total; db_lines(F.db, "03000000" "3412" "0000" "7856" "0000" "0001" "0000", &ours, &foreign, &total);
            CHECK(ours == 1 && total == 1 && F.db[strlen(F.db) - 1] == '\n' && !strcmp(sdl, strtok(strcpy((char[600]){0}, F.db), "\n")), "%s: one SDL line in the user file, the same text as returned", FX[k].n);
            rig_done(&R);
        }
    }

    /* cancel in the middle: nothing is sent, nothing stored, the pad keeps its table */
    hp_parse(D_HAT12, sizeof D_HAT12, &HL); pd_from_layout(&HL, &T, &info);
    rig_start(&R, 0, 0); padmgr_core_start(R.m); memset(&F, 0, sizeof F);
    plug(&R, 0, 0x1234, 0x5678, 0x0100, 3, 0, 0, D_HAT12, sizeof D_HAT12, 1);
    { rep_t id; build_idle(&T, 0, 6, &id); fpga_send_report(&R.fp, 0, id.r, 6); snap(&R, 0); }
    long cmds = R.fp.n_cmd[TANGO_HID_SET_DECODE]; int srcb = padmgr_port_src(R.m, 0);
    script_t sc; memset(&sc, 0, sizeof sc); sc.T = &T; sc.len = 6; sc.cancel_at = 6;
    plsess_begin(&S, 0x1234, 0x5678, 0x0100, &HL, "x"); plsess_baseline(&S, script_cap, &sc);
    int rc = walk(&S, &sc, 1);
    CHECK(rc == PLS_CANCEL, "cancel mid-way gives PLS_CANCEL (%d)", rc);
    plsess_abort(&S);
    plsess_files_t files = cf_table(&F);
    CHECK(plsess_finish(&S, R.m, 0, &files, NULL, 0) == PLS_BAD, "a finished / aborted session cannot be finished");
    CHECK(R.fp.n_cmd[TANGO_HID_SET_DECODE] == cmds && padmgr_port_src(R.m, 0) == srcb && F.dec_saves == 0 && F.commits == 0 && F.db[0] == 0, "nothing was sent or written; the pad keeps its table");
    rig_done(&R);

    /* retries: no change -> again; ambiguous -> again; no report -> again; d-pad resumes where it stopped */
    rig_start(&R, 0, 0); padmgr_core_start(R.m); memset(&F, 0, sizeof F);
    plug(&R, 0, 0x1234, 0x5678, 0x0100, 3, 0, 0, D_HAT12, sizeof D_HAT12, 1);
    { rep_t id; build_idle(&T, 0, 6, &id); fpga_send_report(&R.fp, 0, id.r, 6); snap(&R, 0); }
    memset(&sc, 0, sizeof sc); sc.T = &T; sc.len = 6; sc.nochange_rw = RW_SOUTH + 1; sc.ambig_rw = RW_EAST + 1; sc.noreport_rw = RW_L1 + 1;
    plsess_begin(&S, 0x1234, 0x5678, 0x0100, &HL, "retry"); plsess_baseline(&S, script_cap, &sc);
    rc = plsess_step(&S, RW_SOUTH, script_cap, &sc, msg, sizeof msg);
    CHECK(rc == PLS_NOCHANGE && strstr(msg, "again"), "no change: '%s'", msg);
    rc = plsess_step(&S, RW_SOUTH, script_cap, &sc, msg, sizeof msg); CHECK(rc == PLS_OK, "pressed again: ok");
    rc = plsess_step(&S, RW_EAST, script_cap, &sc, msg, sizeof msg);
    CHECK(rc == PLS_AMBIG && strstr(msg, "only"), "two separate moves: '%s'", msg);
    rc = plsess_step(&S, RW_EAST, script_cap, &sc, msg, sizeof msg); CHECK(rc == PLS_OK, "retry alone: ok");
    rc = plsess_step(&S, RW_L1, script_cap, &sc, msg, sizeof msg); CHECK(rc == PLS_NOREPORT, "no report from the pad (%d)", rc);
    rc = plsess_step(&S, RW_L1, script_cap, &sc, msg, sizeof msg); CHECK(rc == PLS_OK, "and again");
    /* d-pad: the second direction shows no change; the call resumes at LEFT */
    sc.norder = 0; sc.nochange_rw = RW_LEFT + 1;
    rc = plsess_dpad(&S, script_cap, &sc, msg, sizeof msg);
    CHECK(rc == PLS_NOCHANGE && sc.norder == 2 && sc.order[0] == RW_RIGHT && sc.order[1] == RW_LEFT, "d-pad: stopped at LEFT after RIGHT (%d presses asked)", sc.norder);
    sc.norder = 0;
    rc = plsess_dpad(&S, script_cap, &sc, msg, sizeof msg);
    CHECK(rc == PLS_OK && sc.norder == 3 && sc.order[0] == RW_LEFT && sc.order[1] == RW_DOWN && sc.order[2] == RW_UP, "d-pad resumed: LEFT DOWN UP only (%d asked)", sc.norder);
    CHECK(plsess_skip(&S, RW_R2) == PLS_OK, "skip");
    int fl = plsess_finish(&S, R.m, 0, &files, NULL, 0);
    CHECK(fl == (PLF_APPLIED | PLF_DEC | PLF_SDL), "finished with %d inputs (flags %d)", S.nlearned, fl);
    pd_table_t learned = R.fp.p[0].tab;
    int has_dpad = 0; for (int i = 0; i < learned.count; i++) if (learned.e[i].cmp == PDC_HAT) has_dpad++;
    CHECK(has_dpad == 4, "the hat was learned from the four presses (%d entries)", has_dpad);
    /* a second Define of the same pad replaces the first: one line of ours, the user's own lines kept, the table swapped */
    { char old[8192]; snprintf(old, sizeof old, "%.4000s", F.db);      /* our line from the first run, the user's own lines (CRLF) in front of it */
      snprintf(F.db, sizeof F.db, "# my pads\r\n03000000341200007856000000010000,My own line,a:b0,platform:Linux,\r\n%.4000s", old); }
    memset(&sc, 0, sizeof sc); sc.T = &T; sc.len = 6;
    plsess_begin(&S, 0x1234, 0x5678, 0x0100, &HL, "second"); plsess_baseline(&S, script_cap, &sc);
    rc = plsess_step(&S, RW_SOUTH, script_cap, &sc, msg, sizeof msg); CHECK(rc == PLS_OK, "second define: one button");
    plsess_skip(&S, RW_EAST);
    fl = plsess_finish(&S, R.m, 0, &files, sdl, sizeof sdl);
    int ours, foreign, total; db_lines(F.db, "03000000" "3412" "0000" "7856" "0000" "0001" "0000", &ours, &foreign, &total);
    if (getenv("PM_DB")) printf("<<<%s>>>\n", F.db);
    CHECK(fl == (PLF_APPLIED | PLF_DEC | PLF_SDL) && ours == 1 && foreign == 1 && total == 3, "second Define: one Tango line (%d), the user's own line kept (%d), the comment kept (total %d)", ours, foreign, total);
    CHECK(strstr(F.db, "# my pads\r\n") == F.db, "the user's CRLF lines are untouched");
    CHECK(R.fp.p[0].tab.count == 1 && R.fp.p[0].mode == PDM_TABLE, "the FPGA table was replaced (%d entries)", R.fp.p[0].tab.count);
    CHECK(strstr(F.db, "second") && !strstr(F.db, "retry"), "the line carries the new name");
    /* a failing commit leaves the old file and does not claim the SDL flag */
    F.fail_commit = 1; plsess_begin(&S, 0x1234, 0x5678, 0x0100, &HL, "third"); plsess_baseline(&S, script_cap, &sc); plsess_step(&S, RW_SOUTH, script_cap, &sc, msg, sizeof msg);
    char keep[8192]; memcpy(keep, F.db, sizeof keep);
    fl = plsess_finish(&S, R.m, 0, &files, NULL, 0);
    CHECK(!(fl & PLF_SDL) && (fl & PLF_APPLIED) && !strcmp(keep, F.db) && F.discards >= 1, "failed commit: file unchanged, no SDL flag, the table still applied");
    rig_done(&R);

    /* the file edit alone */
    {
        static cardf_t G; plsess_files_t g = cf_table(&G); memset(&G, 0, sizeof G);
        const char *guid = "0300000034120000785600000001" "0000";
        char line[200]; snprintf(line, sizeof line, "%s,%s one,a:b0,platform:Linux,", guid, PLS_NAME_TAG);
        CHECK(pl_sdl_db_update(&g, guid, line) && !strcmp(G.db, "0300000034120000785600000001000" "0,Tango Define one,a:b0,platform:Linux,\n") , "no file: created with the line");
        snprintf(G.db, sizeof G.db, "first\nsecond"); G.db_exists = 1;
        CHECK(pl_sdl_db_update(&g, guid, line) && strstr(G.db, "second\n03000000") != NULL, "no trailing newline in the old file: one is added");
        snprintf(line, sizeof line, "%s,%s two,a:b1,platform:Linux,", guid, PLS_NAME_TAG);
        pl_sdl_db_update(&g, guid, line);
        int o, f_, t_; db_lines(G.db, guid, &o, &f_, &t_);
        CHECK(o == 1 && t_ == 3 && strstr(G.db, "two") && !strstr(G.db, "one"), "replaced, not duplicated (%d ours, %d lines)", o, t_);
        snprintf(G.db, sizeof G.db, "%s,Mine,a:b9,platform:Linux,\n\n%s,%s old,a:b2,\nx\n", guid, guid, PLS_NAME_TAG); G.db_exists = 1;
        pl_sdl_db_update(&g, guid, line);
        db_lines(G.db, guid, &o, &f_, &t_);
        CHECK(o == 1 && f_ == 1 && strstr(G.db, "\n\n") && strstr(G.db, "\nx\n"), "a hand-written line for the same GUID, a blank line and an unrelated short line survive");
    }
}

/* the reference capture on the fake FPGA's clock */
typedef struct { rig_t *R; int t, press_from, press_to; int cancel_at; uint8_t idle[8], pressed[8]; int counter; } clk_t;
static void clk_hook(void *c, unsigned ms)
{
    clk_t *k = c; k->t += (int)ms; k->counter++;
    uint8_t r[8]; memcpy(r, (k->t >= k->press_from && k->t < k->press_to) ? k->pressed : k->idle, 8);
    r[7] = (uint8_t)(k->counter * 29);                                   /* a free-running counter byte: noise */
    fpga_send_report(&k->R->fp, 0, r, 8);
}
static int clk_cancel(void *c) { clk_t *k = c; return k->cancel_at && k->t >= k->cancel_at; }

static void test_ref_capture(void)
{
    printf("-- the reference capture function\\n");
    static rig_t R; static clk_t K; pl_sample_t out[12];
    rig_start(&R, 0, 0); padmgr_core_start(R.m);
    plug(&R, 0, 0x1234, 0x5678, 0x0100, 3, 0, 0, D_SNES12, sizeof D_SNES12, 1);
    memset(&K, 0, sizeof K); K.R = &R; K.press_from = 1000; K.press_to = 1300; K.pressed[2] = 0x04;
    R.e.rl.delay_hook = clk_hook; R.e.rl.delay_ctx = &K;
    fpga_send_report(&R.fp, 0, K.idle, 8); snap(&R, 0);
    padmgr_cap_t C; memset(&C, 0, sizeof C); C.m = R.m; C.port = 0; C.cancelled = clk_cancel; C.cctx = &K;
    int n = padmgr_capture(&C, PLC_IDLE | (0xFF << 8), out, 12);
    CHECK(n == 12 && C.have_idle && C.noisy[7] != 0 && C.noisy[2] == 0, "IDLE: 12 samples, the counter byte flagged as noise (n %d, noisy[7] %02x)", n, C.noisy[7]);
    CHECK(K.t >= 500 && K.t <= 700, "IDLE took about 600 ms (%d)", K.t);
    n = padmgr_capture(&C, PLC_HELD | (RW_SOUTH << 8), out, 8);
    CHECK(n >= 3 && n <= 8 && (out[0].b[2] & 4) && K.t >= 1300 && K.t < 1400, "HELD: %d samples while held, returned just after the release (t=%d)", n, K.t);
    /* never pressed: the wait times out */
    K.press_from = 100000; K.press_to = 100001; C.press_timeout_ms = 500; int t0 = K.t;
    n = padmgr_capture(&C, PLC_HELD | (RW_SOUTH << 8), out, 8);
    CHECK(n == 0 && K.t - t0 >= 500 && K.t - t0 < 700, "no press: 0 after the timeout (%d ms)", K.t - t0);
    /* cancel while waiting */
    K.cancel_at = K.t + 200; C.press_timeout_ms = 5000;
    n = padmgr_capture(&C, PLC_HELD | (RW_SOUTH << 8), out, 8);
    CHECK(n == -1, "cancel while waiting: -1");
    /* a pad that never lets go does not hang the capture */
    K.cancel_at = 0; K.press_from = K.t; K.press_to = K.t + 1000000; C.press_timeout_ms = 300;
    n = padmgr_capture(&C, PLC_HELD | (RW_SOUTH << 8), out, 8);
    CHECK(n > 0 && n <= 8, "a stuck button returns after a bounded time (%d samples)", n);
    /* the session on top of it: learn one button end to end */
    K.t = 0; fpga_send_report(&R.fp, 0, K.idle, 8); K.press_from = 1000; K.press_to = 1300; K.cancel_at = 0; C.press_timeout_ms = 0; C.have_idle = 0;
    static plsess_t S; hp_layout_t *HL = malloc(sizeof *HL); hp_parse(D_SNES12, sizeof D_SNES12, HL);
    plsess_begin(&S, 0x1234, 0x5678, 0x0100, HL, "ref");
    CHECK(plsess_baseline(&S, padmgr_capture, &C) == PLS_OK, "session baseline through the reference capture");
    char msg[40];
    int rc = plsess_step(&S, RW_SOUTH, padmgr_capture, &C, msg, sizeof msg);
    pd_table_t t; pl_table(&S.pl, &t);
    CHECK(rc == PLS_OK && t.count == 1 && t.e[0].cmp == PDC_NZ && t.e[0].byte == 2 && t.e[0].shift == 2, "learned 'south' = byte 2 bit 2 from a real REPORT2 poll (rc %d, %d entries)", rc, t.count);
    free(HL);
    R.e.rl.delay_hook = NULL;
    rig_done(&R);
}

int main(void)
{
    test_identity();
    test_survey();
    test_quarantine();
    test_menu_word();
    test_new_fpga();
    test_cursor();
    test_swap();
    test_oom();
    test_session();
    test_ref_capture();
    test_learn_flow();
    test_ladder_files();
    test_quiet_devices();
    test_real();
    printf("%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
