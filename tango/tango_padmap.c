// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 the Tango authors
/*
 * tango_padmap.c -- see tango_padmap.h for the MiSTer source lines each rule comes from.
 * Tango, our own code.
 */
#include <string.h>
#include <stdio.h>
#include <strings.h>
#include "tango_padmap.h"
#include "confstr.h"

/* def_mmap's role (input.cpp 2924-2933): Tango's system map is the FPGA's fixed decode (spec "Default core map"):
 * directions raw 0-3, A Circle 5, B Cross 4, X Triangle 7, Y Square 6, L L1 8, R R1 9, Select Share 12, Start Options 13 */
const uint32_t tango_pm_mmap[TANGO_PM_BTNS] = {
    TANGO_PM_CODE(0), TANGO_PM_CODE(1), TANGO_PM_CODE(2), TANGO_PM_CODE(3),
    TANGO_PM_CODE(5), TANGO_PM_CODE(4), TANGO_PM_CODE(7), TANGO_PM_CODE(6),
    TANGO_PM_CODE(8), TANGO_PM_CODE(9), TANGO_PM_CODE(12), TANGO_PM_CODE(13),
};

/* joy_button_map (menu.cpp 267) */
static const char *const BUTTON_MAP[] = { "RIGHT", "LEFT", "DOWN", "UP", "BUTTON A", "BUTTON B", "BUTTON X", "BUTTON Y",
                                          "BUTTON L", "BUTTON R", "SELECT", "START" };

/* substrcpy (user_io.cpp 504-524) over one CONF_STR entry of length n */
static int substr(char *d, int cap, const char *s, int n, int idx)
{
    int p = 0, o = 0;
    for (int i = 0; i < n; i++) {
        if (p == idx && s[i] != ',' && o < cap - 1) d[o++] = s[i];
        if (s[i] == ',') { if (p == idx) break; p++; }
    }
    d[o] = 0;
    return o;
}

/* trim (joymapping.cpp 19-29): spaces at both ends */
static void trim(char *s)
{
    int l = (int)strlen(s);
    if (!l) return;
    while (l && s[l - 1] == ' ') s[--l] = 0;
    char *p = s; while (*p == ' ') { p++; l--; }
    memmove(s, p, (size_t)l + 1);
}

/* get_btn (input.cpp 6477-6498): the text after the first comma of the first J (type 0), jn (1) or jp (2) entry */
static int get_btn(const char *conf, int type, const char **out, int *len)
{
    int n = cs_count(conf);
    for (int i = 2; i < n; i++) {
        cs_text_t t;
        if (!cs_get(conf, i, &t) || !t.len) continue;
        const char *p = t.p;
        if ((p[0] == 'J' && !type) || (p[0] == 'j' && t.len > 1 && ((p[1] == 'n' && type == 1) || (p[1] == 'p' && type == 2)))) {
            const char *c = memchr(p, ',', t.len);
            if (!c) return 0;
            c++;
            int l = (int)(t.p + t.len - c);
            if (!l) return 0;
            *out = c; *len = l;
            return 1;
        }
    }
    return 0;
}

/* the idx'th field of the J entry (parse_buttons 6527-6540): its length, 0 = past the end or empty */
int tango_pm_bname(const char *conf, int idx, char out[32])
{
    const char *s; int l;
    out[0] = 0;
    if (idx < 0 || idx >= TANGO_PM_NAMES || !get_btn(conf, 0, &s, &l)) return 0;
    return substr(out, 32, s, l, idx);
}

int tango_pm_bcount(const char *conf)
{
    char b[32]; int n = 0;
    while (n < TANGO_PM_NAMES && tango_pm_bname(conf, n, b)) n++;       /* parse_buttons: up to the first empty field */
    return n;
}

int tango_pm_bnames(const char *conf, char names[TANGO_PM_NAMES][32])
{
    int n = tango_pm_bcount(conf);
    memset(names, 0, TANGO_PM_NAMES * 32);
    for (int i = 0; i < n; i++) tango_pm_bname(conf, i, names[i]);
    return n;
}

static int is_fire(const char *name)   /* joymapping.cpp 104-115 */
{
    if (!strncasecmp(name, "fire", 4) || !strncasecmp(name, "button", 6)) {
        if (!strcasecmp(name, "fire") || strchr(name, '1')) return 1;
        if (strchr(name, '2')) return 2;
        if (strchr(name, '3')) return 3;
        if (strchr(name, '4')) return 4;
    }
    return 0;
}

/* nnames[k] of MiSTer's read_buttons: the k'th "jn" field, trimmed, else the k'th J name with its "(...)" cut, trimmed.
 * Computed one name at a time: the arrays MiSTer keeps (2 x 28 x 32 bytes) would be static RAM here (build.sh heap floor) */
static void nname(const char *conf, int k, char out[32])
{
    const char *s; int l;
    out[0] = 0;
    if (get_btn(conf, 1, &s, &l)) { substr(out, 32, s, l, k); trim(out); return; }
    if (get_btn(conf, 0, &s, &l)) { substr(out, 32, s, l, k); char *p = strchr(out, '('); if (p) *p = 0; trim(out); }
}

void tango_pm_guess_mm(const char *conf, const uint32_t *mmap, uint32_t map[TANGO_PM_BTNS])
{
    char nm[32], b[32];
    int count = 0;
    memset(map, 0, TANGO_PM_BTNS * sizeof map[0]);
    /* read_buttons (joymapping.cpp 38-102): count = the J names up to the first empty one (empty also after the "(...)" cut) */
    while (count < TANGO_PM_NAMES && tango_pm_bname(conf, count, nm)) {
        char t[32]; strcpy(t, nm); char *p = strchr(t, '('); if (p) *p = 0; trim(t);
        if (!t[0]) break;
        count++;
    }
    /* jp (positional) is used only with MiSTer.ini gamepad_defaults=1; its default is 0 and Tango has no ini */
    /* map_joystick (joymapping.cpp 117-218); mmap has no analog axes on Tango (the FPGA gives digital directions) */
    map[PM_RIGHT] = mmap[PM_RIGHT] & 0xFFFF; map[PM_LEFT] = mmap[PM_LEFT] & 0xFFFF;
    map[PM_DOWN]  = mmap[PM_DOWN]  & 0xFFFF; map[PM_UP]   = mmap[PM_UP]   & 0xFFFF;
    for (int i = 0, n = 0; i < count; i++) {
        tango_pm_bname(conf, i, nm);
        if (!strcmp(nm, "-")) continue;
        int idx = i + 4;
        nname(conf, n, b);
        char *p = strchr(b, '|'); if (p) *p = 0;
        if (!strcasecmp(b, "A") || !strcasecmp(b, "Jump") || is_fire(b) == 1) map[idx] = mmap[PM_A];
        else if (!strcasecmp(b, "B") || is_fire(b) == 2) map[idx] = mmap[PM_B];
        else if (!strcasecmp(b, "X") || !strcasecmp(b, "C") || is_fire(b) == 3) map[idx] = mmap[PM_X];
        else if (!strcasecmp(b, "Y") || !strcasecmp(b, "D") || is_fire(b) == 4) map[idx] = mmap[PM_Y];
        else if (!strcasecmp(b, "R") || !strcasecmp(b, "RT") || !strcasecmp(b, "Coin")) map[idx] = mmap[PM_R];
        else if (!strcasecmp(b, "L") || !strcasecmp(b, "LT")) map[idx] = mmap[PM_L];
        else if (!strcasecmp(b, "Select") || !strcasecmp(b, "Mode") || !strcasecmp(b, "Game Select") || !strcasecmp(b, "Start 2P"))
            map[idx] = mmap[PM_SELECT];
        else if (!strcasecmp(b, "Start") || !strcasecmp(b, "Run") || !strcasecmp(b, "Pause") || !strcasecmp(b, "Start 1P"))
            map[idx] = mmap[PM_START];
        n++;
    }
}

void tango_pm_mapname(const char *core, uint16_t vid, uint16_t pid, char *out, int cap)
{
    snprintf(out, (size_t)cap, "%s_input_%04x_%04x_v3.map", core, vid, pid);   /* get_map_name 1852, idstr 5519 */
}

int tango_pm_pairs(const uint32_t map[TANGO_PM_BTNS], uint8_t pairs[2 * TANGO_PM_BTNS])
{
    for (int i = 0; i < TANGO_PM_BTNS; i++) {
        uint32_t lo = map[i] & 0xFFFF, hi = map[i] >> 16;
        pairs[2 * i]     = (lo >= 0x100 && lo < 0x120) ? (uint8_t)(lo - 0x100) : 0xFF;
        pairs[2 * i + 1] = (hi >= 0x100 && hi < 0x120) ? (uint8_t)(hi - 0x100) : 0xFF;
    }
    return TANGO_PM_BTNS;
}

/* ================================================================ the dialog */
static void start_map(tango_pmcap_t *c, int set)
{
    /* start_map_setting(cnt, set) (input.cpp 1740-1768): with set, the device stays */
    c->cur_key = 0; c->cur_dev = -1;
    c->button = 0; c->set = set;
    if (!set) c->dev = -1;
    c->clear = 0;
    c->osd_timer_on = 0;
    c->state = PMC_MAP;
}

void tango_pmcap_start(tango_pmcap_t *c, const char *conf)
{
    /* the raw words as last seen stay: the key that opened the dialog is held now, and its release must not
     * count (in MiSTer it was pressed before mapping began: with no device yet a release is ignored, 3275) */
    uint32_t prev[2] = { c->prev[0], c->prev[1] };
    uint8_t mb[2][2]; memcpy(mb, c->mb, sizeof mb);
    tango_pmpad_t pads[2]; memcpy(pads, c->pads, sizeof pads);
    memset(c, 0, sizeof *c);
    c->prev[0] = prev[0]; c->prev[1] = prev[1];
    memcpy(c->pads, pads, sizeof pads);
    memcpy(c->mb, mb, sizeof mb);
    for (int p = 0; p < 2; p++) if (!c->mb[p][0] && !c->mb[p][1]) tango_pmcap_set_menu(c, p, TANGO_RAW_SHARE, TANGO_RAW_OPTIONS);
    c->conf = conf; c->bcount = tango_pm_bcount(conf);          /* parse_buttons (menu.cpp 3013) */
    c->count = c->bcount ? c->bcount + 4 : 8;              /* start_map_setting(joy_bcount ? joy_bcount+4 : 8) */
    c->first = 1;                                          /* joymap_first */
    c->dev = -1;
    start_map(c, 0);
}

static const char SYS_CONF[] = "SYS;;J,A,B,X,Y,L,R,Select,Start;";
void tango_pm_guess(const char *conf, uint32_t map[TANGO_PM_BTNS]) { tango_pm_guess_mm(conf, tango_pm_mmap, map); }

int tango_pm_sysmap_valid(const uint32_t m[TANGO_PM_BTNS])
{
    for (int i = 0; i < 12; i++) if ((m[i] & 0xFFFF) < 0x100 || (m[i] & 0xFFFF) >= 0x120) return 0;
    return 1;
}

void tango_pmcap_known(tango_pmcap_t *c, int port, int known) { if (port >= 0 && port < 2) c->pads[port].known = (uint8_t)known; }

void tango_pmcap_start_sys(tango_pmcap_t *c)
{
    tango_pmcap_start(c, SYS_CONF);
    c->sys = 1;
    for (int p = 0; p < 2; p++) c->mb[p][0] = c->mb[p][1] = 0xFF;      /* is_menu(): osd_event = 0 while mapping (input.cpp 3290): no combo skips or cancels */
}

/* the end-of-dialog checks MENU_JOYDIGMAP1 makes after every event (menu.cpp 4366-4385) */
static int check_end(tango_pmcap_t *c, int select, int menu)
{
    if ((c->button >= c->count || (select & c->vid & c->pid & 1)) && c->first && c->dev >= 0) {
        if (!c->sys) { c->state = PMC_ASK; c->ask_sel = 0; }   /* finish_map_setting(0): saved, then the question (the menu core has none, 4366-4385) */
        return PMC_SAVED;
    }
    if (select || menu || c->button >= c->count)
        return menu ? PMC_DISMISSED : PMC_SAVED;           /* finish_map_setting(menu) */
    return PMC_REDRAW;
}

/* one key event (a raw bit of `port` pressed or released): input_cb's mapping part (input.cpp 3199-3585) */
static int event(tango_pmcap_t *c, int port, int bit, int value, uint32_t now_ms, int *cancel)
{
    int code = (int)TANGO_PM_CODE(bit);
    int map_skip = 0, osd_event = 0;
    /* the OSD button: Select + Start of the system map (input.cpp 3180-3216) */
    int old = c->combo[port];
    if (bit == c->mb[port][0]) c->combo[port] = value ? (c->combo[port] | 1) : (c->combo[port] & ~1);
    if (bit == c->mb[port][1]) c->combo[port] = value ? (c->combo[port] | 2) : (c->combo[port] & ~2);
    if (old != 3 && c->combo[port] == 3) {
        osd_event = 1; c->osd_timer_on = 1; c->osd_t0 = now_ms;
        /* Tango's menu button is two keys (Share + Options); MiSTer's is normally one, whose press is the OSD event
         * itself and is never taken as a mapping. So the first combo key, taken on its press, is given back: the
         * combo then skips like MiSTer's one-key button and the next button is taken normally */
        if ((c->key_mapped == (int)TANGO_PM_CODE(c->mb[port][0]) || c->key_mapped == (int)TANGO_PM_CODE(c->mb[port][1]))
            && c->dev == port && c->button < c->count) {
            c->map[c->button] &= c->set ? 0x0000FFFFu : 0xFFFF0000u;
            c->key_mapped = 0;
        }
        c->cur_key = 0;
    }
    else if (old == 3 && c->combo[port] != 3) {
        osd_event = 2;
        if (c->osd_timer_on && now_ms - c->osd_t0 >= 1000) { *cancel = 1; c->osd_timer_on = 0; return PMC_DISMISSED; }
        map_skip = 1; value = 1;                           /* "if (mapping_type != 3) ev->value = 1" */
        c->osd_timer_on = 0;
    }
    if (!(c->dev >= 0 || value)) return PMC_NONE;          /* mapping && (mapping_dev >= 0 || ev->value) */
    if (!c->sys && !c->set && value == 1 && c->dev < 0 && !c->pads[port].known && port >= 0) {   /* MENU_JOYDIGMAP1 (menu.cpp 4366-4373): a pad with no map and no default */
        c->state = PMC_NEED; c->vid = c->pads[port].vid; c->pid = c->pads[port].pid;
        return PMC_REDRAW;
    }
    /* one key at a time (3292-3312) */
    if (!map_skip) {
        if (!c->cur_key) {
            if (value == 1) { c->cur_key = code; c->cur_dev = port; }
            else return PMC_NONE;
        } else {
            if (value == 0 && c->cur_key == code && c->cur_dev == port) c->cur_key = 0;
            else return PMC_NONE;
        }
    }
    if (map_skip) c->cur_key = 0;
    if (c->button >= 0 && !osd_event) {
        if (value == 1 && c->dev < 0) {                    /* the first press picks the device (3353-3358) */
            c->dev = port; c->key_mapped = 0;
            memset(c->map, 0, sizeof c->map);
        }
        c->clear = 0;
        if (c->dev >= 0 && c->dev == port && c->button < c->count) {
            if (value == 1 && !c->key_mapped) {           /* not the menu core (3389-3408) */
                if (!c->button) for (int i = 0; i < TANGO_PM_BTNS; i++) c->map[i] &= c->set ? 0x0000FFFFu : 0xFFFF0000u;
                int found = 0;
                for (int i = 0; i < c->button; i++) {
                    if (c->set && (int)(c->map[i] >> 16) == code) found = 1;
                    if (!c->set && (int)(c->map[i] & 0xFFFF) == code) found = 1;
                }
                if (!found) {
                    if (c->set) c->map[c->button] = (c->map[c->button] & 0xFFFF) | ((uint32_t)code << 16);
                    else c->map[c->button] = (c->map[c->button] & 0xFFFF0000u) | (uint32_t)code;
                    c->key_mapped = code;
                }
            }
            if (c->dev == port && value == 0 && c->key_mapped == code) { c->button++; c->key_mapped = 0; }
        }
    }
    /* skips (3562-3585) */
    while (c->button < c->count) {
        if (map_skip) {
            if (map_skip == 2 || value == 1) {
                if (c->dev >= 0 && c->button > 0) c->map[c->button] &= c->set ? 0x0000FFFFu : 0xFFFF0000u;
                c->button++;
            }
        }
        map_skip = 0;
        if (c->button >= 4 && c->button - 4 < TANGO_PM_NAMES) {
            char bn[32]; tango_pm_bname(c->conf, c->button - 4, bn);
            if (!strcmp(bn, "-")) map_skip = 2;
        }
        if (!map_skip) break;
    }
    return PMC_REDRAW;
}

int tango_pmcap_raw(tango_pmcap_t *c, int port, uint16_t vid, uint16_t pid, uint32_t raw, uint32_t now_ms)
{
    if (c->lm && c->sys && port >= 0 && port <= 1) { c->prev[port] = raw; return PMC_NONE; }     /* the learner takes the presses (tango_pmcap_sample) */
    if (port < 0 || port > 1 || c->state != PMC_MAP) { if (port >= 0 && port <= 1) c->prev[port] = raw; return PMC_NONE; }
    uint32_t ch = raw ^ c->prev[port];
    int r = PMC_NONE;
    /* the evdev order: releases, then presses, each by bit number */
    for (int pass = 0; pass < 2 && r != PMC_DISMISSED; pass++)
        for (int b = 0; b < 32 && r != PMC_DISMISSED; b++) {
            if (!((ch >> b) & 1)) continue;
            int v = (raw >> b) & 1;
            if (v != pass) continue;
            int cancel = 0;
            if (c->dev < 0 && v) { c->vid = vid; c->pid = pid; }
            int e = event(c, port, b, v, now_ms, &cancel);
            if (cancel) { r = PMC_DISMISSED; break; }
            if (e == PMC_REDRAW) {
                int end = check_end(c, 0, 0);
                if (end != PMC_REDRAW) { c->prev[port] = raw; return end; }
                r = PMC_REDRAW;
            }
        }
    c->prev[port] = raw;
    return r;
}

/* ---- the learner behind the walk (tango_padmap.h) ---- */
void tango_pmlearn_zero(tango_pmlearn_t *lm) { memset(lm, 0, sizeof *lm); }

void tango_pmlearn_port(tango_pmlearn_t *lm, int port, uint16_t vid, uint16_t pid, uint16_t ver, uint16_t ver_hid, const hp_layout_t *layout)
{
    if (port < 0 || port > 1) return;
    lm->vid[port] = vid; lm->pid[port] = pid; lm->ver[port] = ver; lm->ver_hid[port] = ver_hid;
    lm->L[port] = layout; lm->use[port] = 1; lm->ready[port] = 0; lm->settled[port] = 0; lm->t_quiet[port] = lm->t_first[port] = 0;
    plcap_begin(&lm->C[port], layout);
}

void tango_pmcap_learn(tango_pmcap_t *c, tango_pmlearn_t *lm) { c->lm = c->sys ? lm : NULL; if (c->lm) c->lm->msg[0] = 0; }

void tango_pmcap_learn_finish(tango_pmcap_t *c)
{
    tango_pmlearn_t *lm = c->lm;
    if (!lm || !lm->began || lm->dpad_done) return;
    int mask = plsess_dpad_flush(&lm->S);
    for (int d = 0; d < 4; d++) if (c->map[d] && !((mask >> d) & 1)) c->map[d] = 0;
}

plsess_t *tango_pmlearn_session(tango_pmlearn_t *lm) { return lm && lm->began ? &lm->S : NULL; }

/* the session for the pad that made the first press, from the rest samples its port gathered */
static void learn_begin_session(tango_pmcap_t *c, int port)
{
    tango_pmlearn_t *lm = c->lm; const pl_sample_t *idle; int n = plcap_idle_samples(&lm->C[port], &idle);
    char nm[24]; snprintf(nm, sizeof nm, "%04x:%04x", lm->vid[port], lm->pid[port]);      /* no product string is read: the ids name the SDL line */
    plsess_begin(&lm->S, lm->vid[port], lm->pid[port], lm->ver[port], lm->L[port], nm);
    plsess_set_sdl_version(&lm->S, lm->ver_hid[port] ? lm->ver_hid[port] : lm->ver[port]);
    plsess_baseline_samples(&lm->S, idle, n);
    lm->began = 1; lm->dpad_done = 0;
}

/* F12 (clear all): the session starts over from the same rest state; what was learned is dropped */
static void learn_restart(tango_pmcap_t *c)
{
    if (c->dev >= 0 && c->lm->began) learn_begin_session(c, c->dev);
}

static void learn_say(tango_pmlearn_t *lm, const char *m) { snprintf(lm->msg, sizeof lm->msg, "%s", m ? m : ""); }

/* a press of the control asked for has come and gone: learn it as the standard raw bit of button c->button */
static int learn_press(tango_pmcap_t *c, const pl_sample_t *p, int n)
{
    tango_pmlearn_t *lm = c->lm; char msg[40]; int k = c->button, r, ok = 0;
    if (k < 0 || k >= 12) return PMC_NONE;
    int rw = (int)(tango_pm_mmap[k] - 0x100u);
    if (k < 4) {                                                 /* RIGHT LEFT DOWN UP: the session's d-pad group, one direction at a time */
        r = plsess_dpad_part(&lm->S, k, p, n, msg, sizeof msg);
        ok = r == PLS_OK || r == PLS_PARTIAL;
        if (!ok && r != PLS_NOCHANGE) {                           /* the four together made no sense: ask for the d-pad again from RIGHT */
            learn_say(lm, "Not clear: from RIGHT"); for (int i = 0; i < 4; i++) c->map[i] = 0;
            c->button = 0; return PMC_REDRAW;
        }
    } else { r = plsess_learn(&lm->S, rw, p, n, msg, sizeof msg); ok = r == PLS_OK; }
    if (!ok) { learn_say(lm, msg); return PMC_REDRAW; }          /* no change / press only this one: the same button again */
    learn_say(lm, "");
    if (k == 3) lm->dpad_done = 1;
    c->map[k] = tango_pm_mmap[k];
    c->button++; c->key_mapped = 0; c->cur_key = 0; c->clear = 0;
    return PMC_REDRAW;
}

int tango_pmcap_sample(tango_pmcap_t *c, int port, const pl_sample_t *s, uint32_t now_ms)
{
    tango_pmlearn_t *lm = c->lm;
    if (!lm || !c->sys || port < 0 || port > 1 || !lm->use[port] || c->state != PMC_MAP) return PMC_NONE;
    if (c->dev >= 0 && port != c->dev) return PMC_NONE;
    if (!lm->settled[port]) {                                     /* the key that opened the page is still down: its release must not become the pad's rest state (MiSTer ignores a release before the first press) */
        if (!lm->t_first[port]) lm->t_first[port] = now_ms ? now_ms : 1;
        if (c->prev[port] != 0) lm->t_quiet[port] = 0;
        else if (!lm->t_quiet[port]) lm->t_quiet[port] = now_ms ? now_ms : 1;
        if ((lm->t_quiet[port] && now_ms - lm->t_quiet[port] >= 150) || now_ms - lm->t_first[port] >= 3000) lm->settled[port] = 1;
        else return PMC_NONE;
    }
    if (!lm->ready[port]) {                                       /* the rest state, 12 reports */
        if (s && plcap_idle(&lm->C[port], s)) { lm->ready[port] = 1; }
        return PMC_NONE;
    }
    pl_sample_t held[PLS_MAX_SAMPLES];
    int n = plcap_held(&lm->C[port], s, now_ms, held, PLS_MAX_SAMPLES);
    if (n <= 0) return PMC_NONE;
    if (c->dev < 0) {                                             /* the first press picks the device (input.cpp 3353-3358) */
        c->dev = port; c->key_mapped = 0; c->vid = lm->vid[port]; c->pid = lm->pid[port];
        memset(c->map, 0, sizeof c->map);
        learn_begin_session(c, port);
    }
    int r = learn_press(c, held, n);
    if (c->button >= c->count) return PMC_SAVED;
    (void)now_ms;
    return r;
}

int tango_pmcap_key(tango_pmcap_t *c, int key)
{
    if (c->state == PMC_NEED) return (key == PMC_KEY_ENTER || key == PMC_KEY_ESC || key == PMC_KEY_SPACE) ? PMC_DISMISSED : PMC_NONE;   /* MENU_JOYDIGMAP2: Select / Menu: back to System */
    if (c->state == PMC_ASK) {                             /* MENU_JOYDIGMAP4 (menu.cpp 4409-4431) */
        if (key == PMC_KEY_ESC) return PMC_EXIT;           /* menu: back to System (the map is already saved) */
        if (key == PMC_KEY_UP || key == PMC_KEY_DOWN) { c->ask_sel ^= 1; return PMC_REDRAW; }
        if (key == PMC_KEY_ENTER) {
            if (!c->ask_sel) return PMC_EXIT;
            start_map(c, 1); c->first = 0;                  /* start_map_setting(cnt, 1); joymap_first = 0 */
            return PMC_REDRAW;
        }
        return PMC_NONE;
    }
    if (key == PMC_KEY_F12) {                              /* clear all (3373-3378) */
        if (c->dev < 0) return PMC_NONE;
        if (c->lm) learn_restart(c);
        memset(c->map, 0, sizeof c->map); c->button = 0; c->clear = 1; c->first = 1;
        return PMC_REDRAW;
    }
    if (key == PMC_KEY_SPACE) {                            /* map_skip (input.cpp 2991, 3571-3580): the keyboard's Space skips the button asked for */
        if (c->dev < 0 && c->button >= 0) return PMC_NONE;
        if (c->dev >= 0 && c->button > 0) c->map[c->button] &= c->set ? 0x0000FFFFu : 0xFFFF0000u;
        if (c->lm && c->dev >= 0 && c->button < 12) { c->map[c->button] = 0; plsess_skip(&c->lm->S, (int)(tango_pm_mmap[c->button] - 0x100u)); }
        c->button++; c->key_mapped = 0; c->cur_key = 0;
        return check_end(c, 0, 0);
    }
    if (key == PMC_KEY_ENTER) return check_end(c, 1, 0);
    if (key == PMC_KEY_ESC) return check_end(c, 0, 1);
    return PMC_NONE;
}

int tango_pmcap_canceling(const tango_pmcap_t *c, uint32_t now_ms)
{
    return c->state == PMC_MAP && c->osd_timer_on && now_ms - c->osd_t0 >= 1000;   /* get_map_cancel (1817-1820) */
}

static void row(tango_pmcap_t *c, int r, int kind, const char *label, int col)
{
    c->rows[r].kind = (uint8_t)kind; c->rows[r].col = (uint8_t)col; c->rows[r].label = label; c->rows[r].value = NULL;
}

void tango_pmcap_page(tango_pmcap_t *c, tango_page_t *p, const char *banner, uint32_t now_ms)
{
    static const tango_legend_t LEG_MAP[] = { { "Menu-hold", "cancel" }, { "Enter", "finish" } };
    static const tango_legend_t LEG_ASK[] = { { "A", "select" } };
    memset(p, 0, sizeof *p);
    memset(c->rows, 0, sizeof c->rows);
    p->banner = banner; p->title = "Define buttons"; p->rows = c->rows; p->top = 0;
    if (c->state == PMC_NEED) {                            /* MENU_JOYDIGMAP1 (4366-4373): the pad is not known to the system */
        static const tango_legend_t LEG_NEED[] = { { "Esc", "back" }, { "Enter", "back" } };
        row(c, 6, TR_CENTER, "You need to define this", 0);
        row(c, 7, TR_CENTER, "joystick in Menu core first", 0);
        row(c, 9, TR_CENTER, "Press ESC/Enter", 0);
        p->nrows = 10; p->sel = -1; p->legend = LEG_NEED; p->nlegend = 2;
        return;
    }
    if (c->state == PMC_ASK) {                             /* MENU_JOYDIGMAP3 (4397-4407), rows 6-9 */
        row(c, 6, TR_CENTER, "Do you want to setup", TANGO_C_TEXT);
        row(c, 7, TR_CENTER, "alternative buttons?", TANGO_C_TEXT);
        row(c, 8, TR_ACT, "No", 0);
        row(c, 9, TR_ACT, "Yes", 0);
        p->nrows = 10; p->sel = 8 + c->ask_sel; p->legend = LEG_ASK; p->nlegend = 1;
        return;
    }
    static const tango_legend_t LEG_SYSMAP[] = { { "Esc", "cancel" }, { "Enter", "finish" } };
    p->legend = c->sys ? LEG_SYSMAP : LEG_MAP; p->nlegend = 2; p->sel = -1;
    /* rows 8/9 (MENU_JOYDIGMAP, 4202-4210): the menu core says Esc / Enter, a core Menu-hold / Enter */
    row(c, 8, TR_CENTER, c->sys ? "Esc \x7f Cancel" : "Menu-hold \x7f Cancel", 0);
    row(c, 9, TR_CENTER, "Enter \x7f Finish", 0);
    p->nrows = 10;
    if (c->clear || tango_pmcap_canceling(c, now_ms)) {    /* "Clearing" / "Canceling" on row 4 (4211-4226) */
        row(c, 4, TR_CENTER, c->clear ? "Clearing" : "Canceling", TANGO_C_TEXT);
    } else {
        const char *name = ""; char bn[32];
        if (c->button < 4) name = BUTTON_MAP[c->button];
        else if (c->bcount) { tango_pm_bname(c->conf, c->button - 4 < TANGO_PM_NAMES ? c->button - 4 : 0, bn); name = bn; }
        else name = c->button < 12 ? BUTTON_MAP[c->button] : "";
        snprintf(c->txt[0], sizeof c->txt[0], "Press: %s", name);
        row(c, 3, TR_CENTER, c->txt[0], TANGO_C_TEXT);
    }
    if (c->lm && c->lm->msg[0]) row(c, 6, TR_CENTER, c->lm->msg, TANGO_C_TEXT);        /* the learner's last answer: "No change: press it again" */
    if (c->dev >= 0) {                                     /* rows 5, 7, 10 once the pad is known (4351-4363) */
        snprintf(c->txt[1], sizeof c->txt[1], "Joystick ID: %04x:%04x", c->vid, c->pid);
        row(c, 5, TR_CENTER, c->txt[1], 0);
        if (c->button > 0 || !c->first) row(c, 7, TR_CENTER, c->sys ? "User/Space \x7f Undefine" : "User/Menu \x7f Undefine", 0);
        if (!c->sys) row(c, 10, TR_CENTER, "F12 \x7f Clear all", 0);
        p->nrows = c->sys ? 10 : 11;
    }
    if (c->sys && c->button >= PM_RIGHT && c->button <= PM_START) {
        /* the on-screen gamepad of the menu core's flow (4298-4345), which flashes the button asked for every 100 ms.  MiSTer draws one picture with
         * font glyphs; the proportional font cannot keep that picture's columns, so it is four centred lines (rows 10-13), and the flash blanks the line that
         * holds the button (MiSTer blanks the one name) */
        static const char *const GP[4] = { "L                                  R", "Up                                 X", "Left   Right   Select   Start   Y   A",
                                           "Down                               B" };
        static const uint8_t LN[12] = { 2, 2, 3, 1, 2, 3, 1, 2, 0, 0, 2, 2 };   /* PM_RIGHT.. : the line that holds it */
        int cur = LN[c->button], flash = (int)((now_ms / 100) & 1);
        for (int i = 0; i < 4; i++) {
            if (flash && i == cur) continue;
            row(c, 10 + i, TR_CENTER, GP[i], 0);
        }
        p->nrows = 14;
    }
}

/* ================================================================ raw code names (the menu button's, tango_menubtn.c) */
static const char *const RAWN[18] = { "D-pad Right", "D-pad Left", "D-pad Down", "D-pad Up", "Cross", "Circle", "Square", "Triangle",
                                      "L1", "R1", "L2", "R2", "Share", "Options", "L3", "R3", "PS", "Touchpad" };
void tango_pmcap_set_menu(tango_pmcap_t *c, int port, int a, int b)
{
    if (port < 0 || port > 1) return;
    c->mb[port][0] = (uint8_t)a; c->mb[port][1] = (uint8_t)b;
}

void tango_pmcap_pad(tango_pmcap_t *c, int port, int connected, uint16_t vid, uint16_t pid)
{
    if (port < 0 || port > 1) return;
    c->pads[port].conn = (uint8_t)connected; c->pads[port].vid = vid; c->pads[port].pid = pid;
}

void tango_pm_default(const char *conf, uint32_t map[TANGO_PM_BTNS])
{
    if (tango_pm_bcount(conf)) { tango_pm_guess(conf, map); return; }
    memset(map, 0, TANGO_PM_BTNS * sizeof map[0]);
    for (int i = 0; i < 12; i++) map[i] = tango_pm_mmap[i];
}

void tango_pmed_codename(uint32_t code, char out[24])
{
    if (!code) strcpy(out, "-");
    else if (code >= 0x100 && code < 0x100 + 18) snprintf(out, 24, "%s", RAWN[code - 0x100]);
    else if (code >= 0x100 && code < 0x120) snprintf(out, 24, "Button %u", (unsigned)(code - 0x100));
    else snprintf(out, 24, "Code %x", (unsigned)code);       /* a map written by another program */
}
