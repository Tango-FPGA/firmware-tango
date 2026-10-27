// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 the Tango authors
/*
 * tango_coremenu.c -- see tango_coremenu.h for the MiSTer source lines each rule comes from.
 * Tango.
 *
 * Derived from Main_MiSTer (https://github.com/MiSTer-devel/Main_MiSTer, GPL-3.0; compared at c3dc8ed): these
 * parts follow or copy its code, so this file is GPL-3.0-or-later with the whole firmware
 * (LICENSE-GPL-3.0, NOTICE):
 *   build_dip               menu.cpp MENU_ARCADE_DIP1 (3510-3574; followed step by step)
 *   SMASK_MSG               menu.cpp config_smask_msg (273; copied)
 * Upstream notices: menu.cpp's header reads "Copyright 2005, 2006, 2007 Dennis van Weeren" and "Copyright 2008, 2009
 * Jakub Bednarski" (GPL version 3 or later).
 */
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include "tango_coremenu.h"
#include "tango_about.h"
#include "confstr.h"
#include "tango_mra.h"

enum {   /* System page items: MiSTer's menusub numbers (menu.cpp 2863-2958) */
    SYS_CORE = 0, SYS_DEFINE = 2, SYS_REMAP = 3, SYS_PLAYERS = 4, SYS_VIDEO = 6, SYS_AFILT = 9, SYS_AFILE = 10,
    SYS_RESET = 13, SYS_SAVE = 14, SYS_HELP = 15, SYS_ABOUT = 16, SYS_REBOOT = 17, SYS_EXIT = 18
};
enum { MISC_COREVOL = 0, MISC_GLOBVOL = 1 };   /* Misc. Options items (menu.cpp 4030-4098: menusub 1 and 2) */
enum {   /* Video processing page items: MiSTer's menusub numbers (menu.cpp 3185-3270, 3276-3400) */
    VID_PRESET = 0, VID_HMODE = 1, VID_HFILE = 2, VID_VMODE = 3, VID_VFILE = 4, VID_SMODE = 5, VID_SFILE = 6,
    VID_IMODE = 7, VID_IFILE = 8, VID_GAMMA = 9, VID_GFILE = 10, VID_MASK = 11, VID_MFILE = 12, VID_RESET = 13,
    VID_BACK = 14
};
static const char *const SMASK_MSG[] = { "None", "1x", "2x", "1x Rotated", "2x Rotated" };   /* config_smask_msg (273) */
#define ROW_EXIT (-1)
#define ROW_NONE (-2)

static const tango_legend_t LEG_PAGE0[] = { { "A", "change" }, { "\x7f", "system" }, { "\x83", "misc." }, { "Menu", "close" } };
static const tango_legend_t LEG_SUB[]   = { { "A", "change" }, { "\x83", "back" } };
static const tango_legend_t LEG_SYS[]   = { { "A", "select" }, { "\x83", "core" }, { "Menu", "close" } };
static const tango_legend_t LEG_RESET[] = { { "A", "select" } };
static const tango_legend_t LEG_FILES[] = { { "A", "open" }, { "\x83\x7f", "page" }, { "B", "back" } };
#define N(a) ((int)(sizeof a / sizeof a[0]))

static void copy_text(char *dst, int cap, cs_text_t t)
{
    int n = t.len < cap - 1 ? t.len : cap - 1;
    memcpy(dst, t.p, (size_t)n); dst[n] = 0;
}

void tango_core_send_status(tango_coremenu_t *cm)
{
    tango_sys_set_status(cm->link, 0, cm->status, 16);
}

void tango_core_dip_setup(tango_coremenu_t *cm, const tango_zipfs_t *zipfs, const char *mra_path,
                          int dip_num, uint64_t cur, uint64_t def)
{
    cm->dip_zipfs = zipfs;
    strncpy(cm->dip_mra_path, mra_path ? mra_path : "", sizeof cm->dip_mra_path - 1);
    cm->dip_mra_path[sizeof cm->dip_mra_path - 1] = 0;
    cm->dip_num = (zipfs && cm->dip_mra_path[0] && dip_num > 0) ? dip_num : 0;
    cm->dip_cur = cur;
    cm->dip_def = def;
}

uint64_t tango_core_dip_cur(const tango_coremenu_t *cm) { return cm->dip_cur; }

void tango_core_cfgname(const char *conf, char *out, int cap)
{
    /* user_io_create_config_name(1): NAME + "_v<N>" (from "v,N", user_io.cpp 941-948) + ".CFG" */
    cs_text_t t; char name[32] = "";
    if (cs_get(conf, 0, &t)) copy_text(name, sizeof name, t);
    char ver[12] = "";
    int n = cs_count(conf);
    for (int i = 2; i < n; i++) {
        cs_entry_t e;
        if (!cs_get(conf, i, &t) || !cs_parse(t, &e) || e.kind != CS_CFGVER) continue;
        char num[4] = { 0 }; copy_text(num, 3, e.name);      /* str[2] = 0: two digits */
        int v = 0; for (int k = 0; num[k] >= '0' && num[k] <= '9'; k++) v = v * 10 + (num[k] - '0');
        if (v) snprintf(ver, sizeof ver, "_v%d", v);
    }
    snprintf(out, (size_t)cap, "%s%s.CFG", name, ver);
}

void tango_core_init(tango_coremenu_t *cm, const tango_link_t *link, tango_osd_t *osd, tango_browser_t *br,
                     const tango_core_platform_t *plat, const char *conf, const uint8_t *cfg16)
{
    memset(cm, 0, sizeof *cm);
    cm->link = link; cm->osd = osd; cm->br = br; cm->plat = plat;
    strncpy(cm->conf, conf, sizeof cm->conf - 1);
    cs_text_t t;
    if (cs_get(cm->conf, 0, &t)) copy_text(cm->name, sizeof cm->name, t);
    int i;
    for (i = 1; cs_get(cm->conf, i, &t); i++) {
        cs_entry_t e;
        if (cs_parse(t, &e) && e.kind == CS_VERSION) { copy_text(cm->core_ver, sizeof cm->core_ver, e.name); break; }
    }
    for (i = 0; cm->name[i] && i < (int)sizeof cm->banner - 1; i++) {
        char c = cm->name[i]; cm->banner[i] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
    }
    cm->banner[i] = 0;
    tango_core_cfgname(cm->conf, cm->cfgname, sizeof cm->cfgname);
    if (cfg16) memcpy(cm->status, cfg16, 16);
    /* user_io.cpp 1584-1590: the config is applied with status[0] (reset) set, released when the core is up */
    cm->status[0] |= 1; tango_core_send_status(cm);
    cm->status[0] &= (uint8_t)~1; tango_core_send_status(cm);
    cm->hdmask = tango_sys_get_menumask(link);
    cm->mode = TANGO_CM_CLOSED;
}

/* ---------------------------------------------------------------- page 0 / sub-pages from CONF_STR */
static int add_row(tango_coremenu_t *cm, int kind, int ent, const char *lab, const char *val, int col)
{
    if (cm->nrows >= TANGO_CM_ROWS) return -1;
    int r = cm->nrows++;
    strncpy(cm->lab[r], lab ? lab : "", sizeof cm->lab[r] - 1); cm->lab[r][sizeof cm->lab[r] - 1] = 0;
    strncpy(cm->val[r], val ? val : "", sizeof cm->val[r] - 1); cm->val[r][sizeof cm->val[r] - 1] = 0;
    cm->rows[r].kind = (uint8_t)kind; cm->rows[r].col = (uint8_t)col;
    cm->rows[r].label = cm->lab[r]; cm->rows[r].value = cm->val[r][0] ? cm->val[r] : NULL;
    cm->ent[r] = (int16_t)ent;
    return r;
}
/* an option value MiSTer would offer: present, not empty, and not "[ARC1]"/"[ARC2]" (get_arc, menu.cpp 937-948:
 * those need MiSTer.ini custom_aspect_ratio_1/2, which Tango does not have, so get_arc is always -1 here) */
static int value_ok(const cs_entry_t *e, int x, cs_text_t *v)
{
    if (!cs_value(e, x, v) || !v->len) return 0;
    if (v->len == 6 && (!memcmp(v->p, "[ARC1]", 6) || !memcmp(v->p, "[ARC2]", 6))) return 0;
    return 1;
}
static int selectable(const tango_row_t *r) { return r->kind != TR_BLANK && r->kind != TR_CENTER && r->kind != TR_SEP && r->kind != TR_LOGO && r->kind != TR_SCROLL; }

/* GetExt (menu.cpp 337-351): "BINGEN" -> "BIN,GEN", trailing spaces of a group cut */
static void get_ext(const char *ext, char *out, int cap)
{
    int o = 0;
    while (*ext && o < cap - 5) {
        if (o) out[o++] = ',';
        int k = 0; for (; k < 3 && ext[k]; k++) out[o++] = ext[k];
        while (o && out[o - 1] == ' ') o--;
        if (strlen(ext) <= 3) break;
        ext += 3;
    }
    out[o] = 0;
}

static int entry_visible(tango_coremenu_t *cm, const cs_entry_t *e, int *disabled)
{
    int h, d;
    cs_visibility(e, cm->hdmask, &h, &d);
    *disabled = d;
    if (h) return 0;
    /* inpage: page 0 shows entries without a P# prefix (incl. "P#,Title" links); page n its "P n" ones */
    int inpage = cm->page ? (e->kind != CS_PAGE && e->page == cm->page) : (e->page == 0);
    return inpage;
}

static void build_page(tango_coremenu_t *cm)
{
    cm->nrows = 0;
    int n = cs_count(cm->conf);
    for (int i = 2; i < n; i++) {
        cs_text_t t; cs_entry_t e; int d;
        if (!cs_get(cm->conf, i, &t) || !cs_parse(t, &e)) continue;
        if (!entry_visible(cm, &e, &d)) continue;
        char lab[64], val[48];
        copy_text(lab, sizeof lab, e.name);
        switch (e.kind) {
        case CS_PAGE:    add_row(cm, d ? TR_DIS : TR_SUB, i, lab, NULL, 0); break;
        case CS_OPTION: {
            uint32_t x = cs_status_get(cm->status, e.bit_lo, e.bit_hi);
            cs_text_t v;
            if (!value_ok(&e, (int)x, &v)) cs_value(&e, 0, &v);           /* out of range: show value 0 (1935) */
            copy_text(val, sizeof val, v);
            add_row(cm, d ? TR_DIS : TR_OPT, i, lab, val, 0); break;
        }
        case CS_TRIGGER: case CS_RESET: add_row(cm, d ? TR_DIS : TR_ACT, i, lab, NULL, 0); break;
        case CS_FILE: case CS_MOUNT: {
            char ext[64], extl[64], s[136];
            copy_text(ext, sizeof ext, e.ext); get_ext(ext, extl, sizeof extl);
            if (lab[0]) snprintf(s, sizeof s, "%s *.%s", lab, extl);
            else snprintf(s, sizeof s, "%s *.%s", e.kind == CS_FILE ? "Load" : "Mount", extl);
            add_row(cm, d ? TR_DIS : TR_ACT, i, s, NULL, 0); break;
        }
        case CS_CHEATS:  add_row(cm, TR_DIS, i, lab[0] ? lab : "Cheats", NULL, 0); break;   /* no cheats */
        /* MiSTer's own dual hide rule for this entry (menu.cpp 1954-1965,
         * 2366: "h = page || !arcade_sw()->dip_num") -- the "page" half is already handled for us (a bare
         * "DIP" token parses with e->page left at its zeroed default, so entry_visible()'s existing inpage
         * test already excludes it from every page but 0, the same way a CS_PAGE target does); only the
         * "no dips at all" half needs its own check here. TR_SUB (not a new row kind): the chevron render
         * gets drawn automatically, the same as any other sub-page link (CS_PAGE above). */
        case CS_DIP: if (cm->dip_num > 0) add_row(cm, d ? TR_DIS : TR_SUB, i, "DIP Switches", NULL, 0); break;
        case CS_TEXT: {                  /* MiSTer writes " " + text; Tango centres it, so the spaces go */
            char *s = lab; while (*s == ' ') s++;
            for (int k = (int)strlen(s) - 1; k >= 0 && s[k] == ' '; k--) s[k] = 0;
            if (s[0]) add_row(cm, TR_CENTER, i, s, NULL, 0);
            else add_row(cm, TR_BLANK, i, NULL, NULL, 0);
            break;
        }
        default: break;          /* J, jn, jp, V, v, I, DEFMRA, unknown: not on a page (DIP: handled above) */
        }
    }
    if (!cm->nrows) return;              /* no entries (menu.cpp 2233-2239): the caller goes to page 0 / System */
    /* blank rows up to the last line, then exit / back on it (menu.cpp 2219-2231; OsdGetSize() = Tango's 14 rows) */
    while (cm->nrows < TANGO_VIS - 1) add_row(cm, TR_BLANK, ROW_NONE, NULL, NULL, 0);
    /* the exit row of page 0 carries MiSTer's two cues (MenuWrite(entry, STD_EXIT, .., OSD_ARROW_RIGHT | OSD_ARROW_LEFT), menu.cpp 2219-2222): Left is Misc. Options, Right is System */
    if (cm->page) add_row(cm, TR_ACT, ROW_EXIT, "back", NULL, 0);
    else add_row(cm, TR_ACT, ROW_EXIT, "\x83  exit", "\x7f", 0);
}

/* MENU_COMMON1 (menu.cpp 2863-2958) without the rows Tango's System cannot have: "Lock OSD" (no osd_lock),
 * "UART mode" (no UART core mode). The audio filter pair is there when the core has the filter (audio_filter_en()
 * >= 0, 2925). Everything else, blanks included. */
static void build_system(tango_coremenu_t *cm)
{
    char def[64];
    snprintf(def, sizeof def, "Define %s buttons", cm->name);
    cm->nrows = 0;
    add_row(cm, TR_SUB, SYS_CORE, "Core", NULL, 0);
    add_row(cm, TR_BLANK, ROW_NONE, NULL, NULL, 0);
    add_row(cm, TR_SUB, SYS_DEFINE, def, NULL, 0);
    add_row(cm, TR_SUB, SYS_REMAP, "Button/Key remap", NULL, 0);
    add_row(cm, TR_ACT, SYS_PLAYERS, "Reset player assignment", NULL, 0);
    add_row(cm, TR_BLANK, ROW_NONE, NULL, NULL, 0);
    add_row(cm, TR_SUB, SYS_VIDEO, "Video processing", NULL, 0);
    if (cm->vp && cm->vp->has_afilter) {                /* 2925-2941 */
        add_row(cm, TR_BLANK, ROW_NONE, NULL, NULL, 0);
        add_row(cm, TR_OPT, SYS_AFILT, "Audio filter", cm->vp->afilt[0] ? "Custom" : "Internal", 0);
        add_row(cm, cm->vp->afilt[0] ? TR_SUB : TR_DIS, SYS_AFILE, cm->vp->afilt[1] ? vp_short(cm->vp->afilt + 1) : "< none >", NULL, 0);
    }
    add_row(cm, TR_BLANK, ROW_NONE, NULL, NULL, 0);
    add_row(cm, TR_ACT, SYS_RESET, "Reset settings", NULL, 0);
    add_row(cm, TR_ACT, SYS_SAVE, "Save settings", NULL, 0);
    add_row(cm, TR_BLANK, ROW_NONE, NULL, NULL, 0);
    add_row(cm, TR_SUB, SYS_HELP, "Help", NULL, 0);
    add_row(cm, TR_ACT, SYS_ABOUT, "About", NULL, 0);
    add_row(cm, TR_BLANK, ROW_NONE, NULL, NULL, 0);
    add_row(cm, TR_ACT, SYS_REBOOT, "Reboot (hold \x7f cold reboot)", NULL, 0);   /* MiSTer "hold \x16": the arrow glyph */
    while (cm->nrows < TANGO_VIS - 1) add_row(cm, TR_BLANK, ROW_NONE, NULL, NULL, 0);   /* (none: 15 rows already) */
    add_row(cm, TR_ACT, SYS_EXIT, "\x83  exit", NULL, 0);                 /* STD_EXIT with OSD_ARROW_LEFT (menu.cpp 2946): Left is page 0 */
}

/* MENU_VIDEOPROC1 (menu.cpp 3185-3270): file rows show the file's name (after the last '/') or "< none >" and are
 * greyed when their mode is off; Tango has no direct video, so nothing else greys them */
static void build_video(tango_coremenu_t *cm)
{
    tango_vproc_t *v = cm->vp;
    cm->nrows = 0;
    int h = v && v->flt[VP_HORZ].mode;
    add_row(cm, TR_SUB, VID_PRESET, "Load preset", NULL, 0);
    add_row(cm, TR_BLANK, ROW_NONE, NULL, NULL, 0);
    static const char *const MODE_LAB[4] = { "Horz filter", "Vert filter", "Scan filter", "Intl filter" };
    static const char *const OFF_TXT[4] = { "NearNeighbour", "Same as Horz", "Same as Vert", "Same as Horz" };
    static const int MROW[4] = { VID_HMODE, VID_VMODE, VID_SMODE, VID_IMODE };
    for (int t = 0; t < 4; t++) {
        int on = v && v->flt[t].mode;
        int dis = !v || !v->has_scaler || (t && !h);
        if (t) add_row(cm, TR_BLANK, ROW_NONE, NULL, NULL, 0);
        add_row(cm, dis ? TR_DIS : TR_OPT, MROW[t], t ? MODE_LAB[t] : (on ? "Horz filter" : "Video filter"),
                on ? "From file" : OFF_TXT[t], 0);
        const char *nm = v && v->flt[t].name[0] ? vp_short(v->flt[t].name) : "< none >";
        add_row(cm, (on && h && !dis) ? TR_SUB : TR_DIS, MROW[t] + 1, nm, NULL, 0);
    }
    add_row(cm, TR_BLANK, ROW_NONE, NULL, NULL, 0);
    add_row(cm, (v && v->has_gamma) ? TR_OPT : TR_DIS, VID_GAMMA, "Gamma correction", (v && v->gamma[0]) ? "On" : "Off", 0);
    add_row(cm, (v && v->has_gamma && v->gamma[0]) ? TR_SUB : TR_DIS, VID_GFILE,
            v && v->gamma[1] ? vp_short(v->gamma + 1) : "< none >", NULL, 0);
    add_row(cm, TR_BLANK, ROW_NONE, NULL, NULL, 0);
    int mm = v && v->has_mask ? v->mask[0] : 0;
    add_row(cm, (v && v->has_mask) ? TR_OPT : TR_DIS, VID_MASK, "Shadow Mask", SMASK_MSG[mm >= 0 && mm < 5 ? mm : 0], 0);
    add_row(cm, (v && v->has_mask && mm > 0) ? TR_SUB : TR_DIS, VID_MFILE,
            v && v->mask[1] ? vp_short(v->mask + 1) : "< none >", NULL, 0);
    add_row(cm, TR_BLANK, ROW_NONE, NULL, NULL, 0);
    add_row(cm, v ? TR_ACT : TR_DIS, VID_RESET, "Reset to Defaults", NULL, 0);
    add_row(cm, TR_BLANK, ROW_NONE, NULL, NULL, 0);
    add_row(cm, TR_ACT, VID_BACK, "back", NULL, 0);
}

static void rebuild(tango_coremenu_t *cm);
static void move(tango_coremenu_t *cm, int dir);
static int close_osd(tango_coremenu_t *cm);
/* MENU_MISC1 (menu.cpp 4030-4098) as the framework gives it every core: the Information block lists what the firmware knows
 * (the output mode is fixed), then Core Volume and Global Volume, then exit.  Config (alternate inis) is not built. */
static void build_misc(tango_coremenu_t *cm)
{
    char bar[2][16], il[2][64], ip[24] = "";
    cm->nrows = 0;
    add_row(cm, TR_BLANK, ROW_NONE, NULL, NULL, 0);
    add_row(cm, TR_CENTER, ROW_NONE, "Information", NULL, TANGO_C_TEXT);
    if (cm->plat->ip) cm->plat->ip(cm->plat->ctx, ip, sizeof ip);
    int nl = tango_info_lines(il, cm->misc_vs, ip);                  /* only what the BL616 knows (tango_about.h) */
    for (int i = 0; i < nl; i++) add_row(cm, TR_CENTER, ROW_NONE, il[i], NULL, 0);
    while (cm->nrows < 7) add_row(cm, TR_BLANK, ROW_NONE, NULL, NULL, 0);
    if (cm->plat->vol) {
        tango_vol_text(cm->plat->vol, 1, bar[0]); tango_vol_text(cm->plat->vol, 0, bar[1]);
        add_row(cm, TR_VOL, MISC_COREVOL, "Core Volume", bar[0], 0);
        add_row(cm, cm->plat->vol->vol & 0x10 ? TR_OPT : TR_VOL, MISC_GLOBVOL, "Global Volume", bar[1], 0);
    }
    while (cm->nrows < TANGO_VIS - 1) add_row(cm, TR_BLANK, ROW_NONE, NULL, NULL, 0);
    add_row(cm, TR_ACT, ROW_EXIT, "exit", "\x7f", 0);                    /* STD_EXIT with OSD_ARROW_RIGHT (menu.cpp 4079): Right is page 0 */
}

/* Left / Right / minus / plus / Select on Misc. Options (MENU_MISC2, menu.cpp 4100-4165) */
static int misc_event(tango_coremenu_t *cm, tango_ev_t ev)
{
    tango_vol_t *v = cm->plat->vol;
    int e = cm->ent[cm->sel];
    if (v && (e == MISC_COREVOL || e == MISC_GLOBVOL)) {
        int cmd = (ev == TANGO_EV_LEFT || ev == TANGO_EV_MINUS) ? -1 : (ev == TANGO_EV_RIGHT || ev == TANGO_EV_PLUS) ? 1 :
                  (ev == TANGO_EV_SELECT && e == MISC_GLOBVOL) ? 0 : 2;
        if (cmd == 2) return TANGO_CM_NOTHING;
        if (e == MISC_COREVOL) tango_vol_core_step(v, cmd, cm->now_ms); else tango_vol_global_step(v, cmd, cm->now_ms);
        if (cm->plat->vol_apply) cm->plat->vol_apply(cm->plat->ctx, e == MISC_COREVOL);
        rebuild(cm);
        return TANGO_CM_REDRAWN;
    }
    if (ev == TANGO_EV_SELECT && e == ROW_EXIT) return close_osd(cm);
    if (ev == TANGO_EV_RIGHT && e == ROW_EXIT) { cm->mode = TANGO_CM_PAGE; cm->page = 0; cm->sel = 0; rebuild(cm); return TANGO_CM_REDRAWN; }
    return TANGO_CM_NOTHING;
}

/* MENU_ARCADE_DIP1 (menu.cpp 3510-3574): every <dip> as one row, vertically centred when they all fit on
 * one screen (dip_num < TANGO_VIS - 1, matching MiSTer's own "(OsdGetSize()-1-dip_num)/2" -- once there
 * are too many to fit, render_page's own top>=0/auto-scroll logic (tango_layout.c, already used by every
 * other long rows[] page here) takes over exactly the way MiSTer's own firstmenu/adjvisible scroll does,
 * with no new paging code needed), then a closing "Reset to apply" action row (menu.cpp 3567) -- Tango's
 * own CS_OPTION-style rendering (TR_OPT: label left, current value right; dip_select() below cycles it on
 * Enter/A, menu.cpp 3584-3611's own "(m+1)%num" -- no minus/plus row: real MiSTer's own ARCADE_DIP2 has
 * none either).
 * Each row's value comes from tango_mra_switches_dip_at's own per-row, on-demand parse (a RAM ceiling):
 * one tango_mra_dip_t (~1.3 KB) on THIS
 * function's own stack per row, discarded the instant its row is added -- never the whole
 * tango_mra_switches_t (~83.5 KB), and never retained in cm itself either. */
static void build_dip(tango_coremenu_t *cm)
{
    cm->nrows = 0;
    int n = cm->dip_num;
    if (n > TANGO_CM_ROWS - 1) n = TANGO_CM_ROWS - 1;   /* add_row's own array cap (TANGO_CM_ROWS); a real
        * <switches> can declare up to TANGO_MRA_DIP_MAX (64) entries, which would silently stop add_row
        * from adding anything past this array's own size -- refuse past it explicitly instead (no real
        * file seen anywhere close: the biggest, Galaga's own, is 11) */
    int pad = (n < TANGO_VIS - 1) ? (TANGO_VIS - 1 - n) / 2 : 0;
    for (int k = 0; k < pad; k++) add_row(cm, TR_BLANK, ROW_NONE, NULL, NULL, 0);
    for (int i = 0; i < n; i++) {
        tango_mra_dip_t d;
        if (!tango_mra_switches_dip_at(cm->dip_zipfs, cm->dip_mra_path, 0, i, &d) || !d.num) {
            /* the .mra vanished/changed under us (shouldn't happen -- arc_setup already opened this same
             * file once), or a malformed <dip> with no ids="" at all (num 0: MiSTer's own "% num" cycle,
             * menu.cpp 3607, would divide by zero on a row like this -- a deliberate safety
             * deviation from a literal port: grey it out instead of crashing) */
            add_row(cm, TR_DIS, i, d.name[0] ? d.name : "?", NULL, 0);
            continue;
        }
        uint64_t status = cm->dip_cur & d.mask;
        int m = 0;
        for (int v = 0; v < d.num; v++) if (status == d.val[v]) { m = v; break; }
        add_row(cm, TR_OPT, i, d.name, d.id[m], 0);
    }
    while (cm->nrows < TANGO_VIS - 1) add_row(cm, TR_BLANK, ROW_NONE, NULL, NULL, 0);
    add_row(cm, TR_ACT, ROW_EXIT, "Reset to apply", NULL, 0);   /* dip_select's own ROW_EXIT case below --
        * reused for "the apply/close action", not page_select's usual "back" meaning; the two modes never
        * share a dispatch function so there is no ambiguity */
}

static void build_reset(tango_coremenu_t *cm)
{
    cm->nrows = 0;
    add_row(cm, TR_CENTER, ROW_NONE, "Reset settings?", NULL, TANGO_C_TEXT);
    add_row(cm, TR_BLANK, ROW_NONE, NULL, NULL, 0);
    add_row(cm, TR_ACT, 1, "yes", NULL, 0);
    add_row(cm, TR_ACT, 0, "no", NULL, 0);
}

/* About (tango_about.h): the wordmark, TangoFPGA.com, "Tango FPGA v<yymmdd> (g<hash>[-dirty])" (the BL616 firmware that is running:
 * the platform's fw_id, build.sh -> tango_fw.c), "<core name> <core version>" (CONF_STR V,), the credits ticker, back.
 * The ticker's position follows the time since the page opened (about_t0), so the firmware on a board can be told from
 * the screen and the page moves; tango_core_tick redraws it. */
static void build_about(tango_coremenu_t *cm)
{
    tango_about_rows(cm->rows, cm->lab, cm->name, cm->core_ver, cm->plat->fw_id);
    cm->nrows = TANGO_ABOUT_ROWS;
    for (int i = 0; i < TANGO_ABOUT_ROWS; i++) cm->ent[i] = ROW_NONE;
    cm->about_step = tango_about_step(cm->about_t0, cm->now_ms);
}

/* About -> System, the cursor back on the About row (MiSTer: "Menu, Select or Left returns with the cursor on About") */
static void about_back(tango_coremenu_t *cm)
{
    cm->mode = TANGO_CM_SYSTEM; cm->sel = 0; rebuild(cm);
    for (int r = 0; r < cm->nrows; r++) if (cm->ent[r] == SYS_ABOUT) { cm->sel = r; break; }
}

static void rebuild(tango_coremenu_t *cm)
{
    if (cm->mode == TANGO_CM_PAGE) {
        cm->hdmask = tango_sys_get_menumask(cm->link);     /* MENU_GENERIC_MAIN2 reads UIO_GET_OSDMASK first (1912) */
        build_page(cm);
        if (!cm->nrows && cm->page) { cm->page = 0; cm->sel = 0; build_page(cm); }
        if (!cm->nrows) { cm->mode = TANGO_CM_SYSTEM; cm->sel = 0; build_system(cm); }
    } else if (cm->mode == TANGO_CM_SYSTEM) build_system(cm);
    else if (cm->mode == TANGO_CM_RESET) build_reset(cm);
    else if (cm->mode == TANGO_CM_ABOUT) build_about(cm);
    else if (cm->mode == TANGO_CM_VIDEO) build_video(cm);
    else if (cm->mode == TANGO_CM_MISC) build_misc(cm);
    else if (cm->mode == TANGO_CM_DIP) build_dip(cm);
    if (cm->mode == TANGO_CM_PAGE || cm->mode == TANGO_CM_SYSTEM || cm->mode == TANGO_CM_RESET || cm->mode == TANGO_CM_ABOUT ||
        cm->mode == TANGO_CM_VIDEO || cm->mode == TANGO_CM_MISC || cm->mode == TANGO_CM_DIP) {
        if (cm->sel >= cm->nrows) cm->sel = cm->nrows - 1;
        if (cm->sel < 0) cm->sel = 0;
        if (cm->nrows && !selectable(&cm->rows[cm->sel])) {      /* first selectable row */
            for (int r = 0; r < cm->nrows; r++) if (selectable(&cm->rows[r])) { cm->sel = r; break; }
        }
    }
}

void tango_core_page(tango_coremenu_t *cm, tango_page_t *p)
{
    static tango_legend_t status[1];
    memset(p, 0, sizeof *p);
    if (cm->mode == TANGO_CM_DEFINE) {
        tango_pmcap_page(&cm->pm, p, cm->banner, cm->now_ms);
        return;
    }
    if (cm->mode == TANGO_CM_SAVING) {                  /* MENU_SAVE_CHECK: rows cleared, row 8 "Saving..." (2271-2276) */
        static const tango_legend_t LEG_SAVE[] = { { "Menu", "close" } };
        memset(cm->rows, 0, sizeof cm->rows);
        cm->rows[8].kind = TR_CENTER; cm->rows[8].col = TANGO_C_TEXT; cm->rows[8].label = "Saving...";
        p->banner = cm->banner; p->title = cm->name; p->rows = cm->rows; p->nrows = 9; p->sel = -1; p->top = 0;
        p->legend = LEG_SAVE; p->nlegend = 1;
        return;
    }
    if (cm->mode == TANGO_CM_FILES || cm->mode == TANGO_CM_CORES) {
        tango_browser_page(cm->br, p, cm->banner, LEG_FILES, N(LEG_FILES));
    } else {
        p->banner = cm->banner; p->rows = cm->rows; p->nrows = cm->nrows; p->sel = cm->mode == TANGO_CM_ABOUT ? -1 : cm->sel; p->top = -1;   /* About: no cursor */
        if (cm->mode == TANGO_CM_ABOUT) p->scroll = tango_about_pos(cm->about_step);
        if (cm->mode == TANGO_CM_PAGE) {
            p->title = cm->page ? cm->title : cm->name;
            p->legend = cm->page ? LEG_SUB : LEG_PAGE0; p->nlegend = cm->page ? N(LEG_SUB) : N(LEG_PAGE0);
        } else if (cm->mode == TANGO_CM_SYSTEM) {
            p->title = "System";                                            /* OsdSetTitle("System", 0), menu.cpp 2880 */
            p->legend = LEG_SYS; p->nlegend = N(LEG_SYS);
        } else if (cm->mode == TANGO_CM_ABOUT) {
            static const tango_legend_t LEG_ABOUT[] = { { "\x83", "back" } };
            p->title = "About";                                            /* menu.cpp 4528 */
            p->legend = LEG_ABOUT; p->nlegend = N(LEG_ABOUT);
        } else if (cm->mode == TANGO_CM_MISC) {
            static const tango_legend_t LEG_MISC[] = { { "\x83\x7f", "change" }, { "A", "select" }, { "Menu", "close" } };
            p->title = "Misc. Options"; p->arrow = TANGO_ARROW_RIGHT; p->legend = LEG_MISC; p->nlegend = N(LEG_MISC);
        } else if (cm->mode == TANGO_CM_VIDEO) {
            static const tango_legend_t LEG_VID[] = { { "A", "change" }, { "\x83", "back" } };
            p->title = "Video Processing";                                  /* menu.cpp 3189 */
            p->legend = LEG_VID; p->nlegend = N(LEG_VID);
        } else if (cm->mode == TANGO_CM_DIP) {
            p->title = "DIP Switches"; p->legend = LEG_SUB; p->nlegend = N(LEG_SUB);   /* same shape as any
                * other CS_PAGE sub-page: "A" cycles the value, back/Menu returns (and saves, dip_select) */
        } else {
            p->title = "Reset"; p->legend = LEG_RESET; p->nlegend = N(LEG_RESET);
        }
    }
    if (cm->status_key[0]) {
        status[0].key = cm->status_key; status[0].label = cm->status_text;
        p->legend = status; p->nlegend = 1;
    }
}

void tango_core_redraw(tango_coremenu_t *cm)
{
    if (cm->mode == TANGO_CM_CLOSED) return;
    tango_page_t p;
    tango_core_page(cm, &p);
    tango_layout_page(&p, &cm->tables);
    tango_osd_show(cm->osd, &cm->tables);
}

static void message(tango_coremenu_t *cm, const char *key, const char *text)
{
    strncpy(cm->status_key, key, sizeof cm->status_key - 1); cm->status_key[sizeof cm->status_key - 1] = 0;
    strncpy(cm->status_text, text, sizeof cm->status_text - 1); cm->status_text[sizeof cm->status_text - 1] = 0;
}

static void open_cores(tango_coremenu_t *cm);
int tango_core_toggle(tango_coremenu_t *cm) { return tango_core_toggle_how(cm, TANGO_OPEN_PAGE0); }

int tango_core_toggle_how(tango_coremenu_t *cm, int how)
{
    if (cm->mode != TANGO_CM_CLOSED) {
        cm->mode = TANGO_CM_CLOSED;
        tango_osd_enable(cm->osd, 0);
        return 0;
    }
    cm->status_key[0] = 0; cm->cores_direct = 0;
    if (how == TANGO_OPEN_CORES) {                                         /* MENU_CORES1: the list, cancel = close (menu.cpp 1621-1626, 2266) */
        open_cores(cm); cm->cores_direct = 1;
    } else if (how == TANGO_OPEN_SYSTEM) {                                 /* MENU_COMMON1, menusub 0 (menu.cpp 1650-1653) */
        cm->mode = TANGO_CM_SYSTEM; cm->page = 0; cm->sel = 0; cm->save_until = 0; rebuild(cm);
    } else {
        cm->mode = TANGO_CM_PAGE; cm->page = 0; cm->sel = 0;
        if (cm->save_until && (int32_t)(cm->save_until - cm->now_ms) > 0) cm->mode = TANGO_CM_SAVING;   /* MENU_SAVE_CHECK */
        else cm->save_until = 0;
        if (cm->mode == TANGO_CM_PAGE) rebuild(cm);
    }
    tango_core_redraw(cm);
    tango_osd_enable(cm->osd, 1);
    return 1;
}

static int close_osd(tango_coremenu_t *cm)
{
    cm->mode = TANGO_CM_CLOSED;
    tango_osd_enable(cm->osd, 0);
    return TANGO_CM_CLOSED_NOW;
}

int tango_core_poll_mask(tango_coremenu_t *cm)
{
    uint16_t m = tango_sys_get_menumask(cm->link);
    if (m == cm->hdmask) return 0;
    cm->hdmask = m;
    if (cm->mode != TANGO_CM_PAGE) return 0;
    rebuild(cm); tango_core_redraw(cm);
    return 1;
}

static void move(tango_coremenu_t *cm, int dir)
{
    if (!cm->nrows) return;
    int r = cm->sel;
    for (int k = 0; k < cm->nrows; k++) {
        r = (r + dir + cm->nrows) % cm->nrows;
        if (selectable(&cm->rows[r])) { cm->sel = r; return; }
    }
}

/* MiSTer's menusub of a row: its position among the selectable rows (menumask bits) */
static int menusub_of(tango_coremenu_t *cm, int row)
{
    int m = 0;
    for (int r = 0; r < row; r++) if (selectable(&cm->rows[r])) m++;
    return m;
}

/* a .zip is never left open across a fresh browse: every entry into the Files/Cores list (including a second
 * visit after backing all the way out) starts from the real folder, not a stale archive from an earlier visit */
static void leave_zip(tango_coremenu_t *cm)
{
    if (!cm->in_zip) return;
    tango_zip_close(&cm->zip, cm->plat->zipfs);
    cm->in_zip = 0;
}

static void open_browser(tango_coremenu_t *cm, int files, const char *exts, const char *path)
{
    leave_zip(cm);
    tango_browser_set_mode(cm->br, files, exts);
    const char *p = path;
    if (p[0] && cm->plat->dir_exists && !cm->plat->dir_exists(cm->plat->ctx, p)) p = "";
    tango_browser_scan(cm->br, cm->plat->fs, p);
    cm->mode = files ? TANGO_CM_FILES : TANGO_CM_CORES;
}

/* The Cores list (System > Core, Alt+Menu): SelectFile(.., SCANO_CORES) starts at the running core's folder with that core selected
 * (menu.cpp 450-459: get_rbf_dir / get_rbf_name, ResolveExistingCorePath cuts the path back to what exists).  A core Tango did not
 * start through the launcher (a power-up into a core, a warm MCU reset) has no path: the card root. */
static void open_cores(tango_coremenu_t *cm)
{
    char path[TANGO_PATH_MAX] = "";
    int known = cm->plat->running_core && cm->plat->running_core(cm->plat->ctx, path, sizeof path);
    char *sl = known ? strrchr(path, '/') : NULL, file[TANGO_PATH_MAX] = "";
    if (sl) { strncpy(file, sl + 1, sizeof file - 1); *sl = 0; } else if (known) { strncpy(file, path, sizeof file - 1); path[0] = 0; }
    int trimmed = 0;
    while (path[0] && cm->plat->dir_exists && !cm->plat->dir_exists(cm->plat->ctx, path)) {
        char *p = strrchr(path, '/');
        if (p) *p = 0; else path[0] = 0;
        trimmed = 1;
    }
    open_browser(cm, 0, "", path);
    if (file[0] && !trimmed && !cm->br->error) tango_browser_select_file(cm->br, file);
}

/* ---------------------------------------------------------------- browsing inside a .zip (console ROMs only;
 * MiSTer's zip support, matched feature for feature -- a zip is always a one-level
 * pseudo-folder, never auto-loaded even on a single match; auto-loading is NOT MiSTer's behaviour (file_io.cpp
 * ScanDirectory 1781-1787 fakes a matching .zip as a directory entry unconditionally; 1843-1848 adds its own
 * ".." row); the implementation (tango_zip.c/.h) is ours, shaped for this platform, not a port of theirs. */
static int is_zip_name(const char *name)
{
    size_t n = strlen(name);
    return n > 4 && !strcasecmp(name + n - 4, ".zip");
}

/* feeds tango_browser_scan the zip's own pre-scanned matching entries as if they were a folder's contents --
 * tango_browser.c's sort/display/paging code needs no change at all for this (tango_browser.h's tango_fs_t is
 * already filesystem-agnostic); the only new code is this adapter and the decision of when to use it. */
typedef struct { tango_zip_t *z; int pos; } zipbrowse_ctx_t;
static int zipbrowse_open(void *ctx, const char *path) { (void)path; ((zipbrowse_ctx_t *)ctx)->pos = 0; return 1; }
static int zipbrowse_read(void *ctx, char *name, int cap, int *is_dir)
{
    zipbrowse_ctx_t *c = ctx;
    if (c->pos >= c->z->n) return 0;
    strncpy(name, tango_zip_name(c->z, c->pos), (size_t)cap - 1); name[cap - 1] = 0;
    *is_dir = 0;
    c->pos++;
    return 1;
}
static void zipbrowse_close(void *ctx) { (void)ctx; }

/* a .zip selected in a core's F-entry file browser: open it, scan for this F's extensions, and on at least one
 * match show its contents in place of the real folder (0 matches: a message, the real folder stays shown).
 *
 * KNOWN, LOW-SEVERITY, NARROW EDGE CASE (not fixed, documented instead): cm->zip is the one archive shared
 * between "being browsed now" and "being streamed by an in-flight download" (tango_fw.c's dl_is_zip owns it
 * for the second role once the OSD closes for a load, cm->in_zip is cleared at that same moment precisely so a
 * LATER open_browser()/leave_zip() will not touch it -- see browser_event's TANGO_EV_SELECT). But this
 * function's own tango_zip_open_archive() call always resets cm->zip, with no way to tell "a download is still
 * reading this" from "nothing is using it". If the user reopens the OSD and enters a *second* zip before a
 * *first* zip's download has finished -- which needs sub-second reaction time during an active SPI transfer,
 * practically unreachable by a human -- that first download's tango_zip_read() next call sees a reset struct
 * (mem_open 0) and ends cleanly with an error (tango_dl_pump's existing got<=0 path); it does not corrupt or
 * crash, it just aborts early. Fixing this fully would need a second tango_zip_t (more fixed RAM, permanently,
 * for a window that is realistically never open); it is left as a known limitation instead. */
static int enter_zip(tango_coremenu_t *cm, const char *name)
{
    tango_browser_t *b = cm->br;
    char path[2 * TANGO_PATH_MAX + 8];
    snprintf(path, sizeof path, "%s%s%s", b->path, b->path[0] ? "/" : "", name);
    if (!cm->plat->zipfs || !tango_zip_open_archive(&cm->zip, cm->plat->zipfs, path)) {
        message(cm, "Could not open", name);
        return TANGO_CM_REDRAWN;
    }
    if (!tango_zip_scan(&cm->zip, cm->plat->zipfs, cm->f_exts)) {
        tango_zip_close(&cm->zip, cm->plat->zipfs);
        message(cm, "No matching file", name);
        return TANGO_CM_REDRAWN;
    }
    strncpy(cm->zip_relpath, path, sizeof cm->zip_relpath - 1); cm->zip_relpath[sizeof cm->zip_relpath - 1] = 0;
    strncpy(cm->real_folder, b->path, sizeof cm->real_folder - 1); cm->real_folder[sizeof cm->real_folder - 1] = 0;
    cm->in_zip = 1;
    zipbrowse_ctx_t zctx = { &cm->zip, 0 };
    tango_fs_t zipbrowse = { .open = zipbrowse_open, .read = zipbrowse_read, .close = zipbrowse_close, .ctx = &zctx };
    tango_browser_scan(b, &zipbrowse, name[0] ? name : "zip");   /* non-empty: tango_browser_scan adds ".." itself */
    return TANGO_CM_REDRAWN;
}

/* an option row stepped (menu.cpp 2535-2570): plus / Select to the next value (no next value: value 0), minus to the previous one,
 * skipping values the CONF_STR leaves empty (and wrapping through the status field's mask) */
static int option_step(tango_coremenu_t *cm, const cs_entry_t *e, int minus)
{
    uint32_t mask = e->bit_hi - e->bit_lo + 1 >= 32 ? 0xffffffffu : ((1u << (e->bit_hi - e->bit_lo + 1)) - 1);
    uint32_t x = (cs_status_get(cm->status, e->bit_lo, e->bit_hi) + (minus ? 0xffffffffu : 1u)) & mask;
    cs_text_t v;
    if (minus) { for (uint32_t n = 0; n <= mask && !value_ok(e, (int)x, &v); n++) x = (x - 1) & mask; }
    else if (!value_ok(e, (int)x, &v)) x = 0;                 /* no next value: back to value 0 (2553) */
    cs_status_set(cm->status, e->bit_lo, e->bit_hi, x);
    tango_core_send_status(cm);
    rebuild(cm);
    return TANGO_CM_REDRAWN;
}

/* minus / plus on a page row: only an enabled option takes them (the other rows act on Select alone, menu.cpp 2514-2630) */
static int page_option(tango_coremenu_t *cm, int minus)
{
    int row = cm->sel, ent = cm->ent[row];
    cs_text_t t; cs_entry_t e;
    if (ent < 0 || cm->rows[row].kind == TR_DIS || !cs_get(cm->conf, ent, &t) || !cs_parse(t, &e) || e.kind != CS_OPTION)
        return TANGO_CM_NOTHING;
    return option_step(cm, &e, minus);
}

static int page_select(tango_coremenu_t *cm)
{
    int row = cm->sel, ent = cm->ent[row];
    if (ent == ROW_EXIT) {                                   /* last row: close, or back to page 0 */
        if (!cm->page) return close_osd(cm);
        cm->page = 0; cm->sel = cm->parent_sel; rebuild(cm); return TANGO_CM_REDRAWN;
    }
    if (ent < 0 || cm->rows[row].kind == TR_DIS) return TANGO_CM_NOTHING;   /* greyed: nothing (menu.cpp 2394 "!d") */
    cs_text_t t; cs_entry_t e;
    if (!cs_get(cm->conf, ent, &t) || !cs_parse(t, &e)) return TANGO_CM_NOTHING;
    switch (e.kind) {
    case CS_PAGE:
        cm->parent_sel = row; cm->page = e.target_page; copy_text(cm->title, sizeof cm->title, e.name);
        cm->sel = 0; rebuild(cm); return TANGO_CM_REDRAWN;
    case CS_OPTION: return option_step(cm, &e, 0);
    case CS_TRIGGER: case CS_RESET:
        cs_status_set(cm->status, e.bit_lo, e.bit_hi, 1); tango_core_send_status(cm);
        cs_status_set(cm->status, e.bit_lo, e.bit_hi, 0); tango_core_send_status(cm);
        if (e.kind == CS_RESET) return close_osd(cm);
        return TANGO_CM_REDRAWN;
    case CS_DIP:                                      /* menu.cpp 1959: dip_submenu = selentry */
        cm->parent_sel = row; cm->mode = TANGO_CM_DIP; cm->sel = 0; rebuild(cm); return TANGO_CM_REDRAWN;
    case CS_FILE: {
        cm->vp_pick = VPICK_NONE;
        cm->f_ent = (int16_t)ent;
        cm->f_index = (uint8_t)(e.index ? e.index : menusub_of(cm, row) + 1);   /* menu.cpp 2413-2428 */
        char ext[64]; copy_text(ext, sizeof ext, e.ext);
        while (strlen(ext) % 3 && strlen(ext) < sizeof ext - 1) strcat(ext, " ");
        strncpy(cm->f_exts, ext, sizeof cm->f_exts - 1); cm->f_exts[sizeof cm->f_exts - 1] = 0;
        /* a platform with zip support (tango_zip.h) also lists a .zip as a selectable entry here, the same way
         * a real ROM is -- tango_ext_match (tango_browser.c) has no idea what a zip is, so it is simply given
         * one more 3-character group to match; this never changes a REAL file's own extension position (ZIP is
         * only ever appended, never inserted), so tango_ext_idx's ioctl_index bits are unaffected for them, and
         * a .zip entry never reaches tango_ext_idx at all (browser_event intercepts it first, is_zip_name). */
        if (cm->plat->zipfs && strlen(cm->f_exts) + 3 < sizeof cm->f_exts) strcat(cm->f_exts, "ZIP");
        char home[64]; snprintf(home, sizeof home, "games/%s", cm->name);   /* HomeDir: prefixGameDir */
        cm->parent_sel = row;
        open_browser(cm, 1, cm->f_exts, home);
        return TANGO_CM_REDRAWN;
    }
    default:
        message(cm, "Not available yet", cm->lab[cm->sel]);
        return TANGO_CM_REDRAWN;
    }
}

static void vproc_browse(tango_coremenu_t *cm, int pick, const char *dir, const char *cur, const char *ext);
/* the System page with the cursor on the row of item `id` (the rows move when the page has the Audio filter or the menu button rows) */
static void sys_goto(tango_coremenu_t *cm, int id)
{
    cm->mode = TANGO_CM_SYSTEM; cm->sel = 0; rebuild(cm);
    for (int r = 0; r < cm->nrows; r++) if (cm->ent[r] == id) { cm->sel = r; break; }
}
static int system_select(tango_coremenu_t *cm)
{
    switch (cm->ent[cm->sel]) {
    case SYS_CORE: cm->cores_direct = 0; open_cores(cm); return TANGO_CM_REDRAWN;
    case SYS_RESET: cm->mode = TANGO_CM_RESET; cm->sel = 3; rebuild(cm); return TANGO_CM_REDRAWN;   /* menusub = 1: "no" */
    case SYS_DEFINE:                                  /* parse_buttons, start_map_setting (menu.cpp 3011-3019): MiSTer's guided walk, no list */
        tango_pmcap_start(&cm->pm, cm->conf);
        cm->mode = TANGO_CM_DEFINE;
        return TANGO_CM_REDRAWN;
    case SYS_SAVE: {
        int r = cm->plat->save_cfg ? cm->plat->save_cfg(cm->plat->ctx, cm->cfgname, cm->status) : -1;
        cm->mode = TANGO_CM_PAGE; cm->page = 0; cm->sel = 0; rebuild(cm);      /* then page 0, menusub 0 */
        message(cm, r == 0 ? "Saved" : "Could not save", cm->cfgname);
        return TANGO_CM_REDRAWN;
    }
    case SYS_REBOOT: return TANGO_CM_REBOOT;
    case SYS_EXIT: return close_osd(cm);
    case SYS_ABOUT: cm->mode = TANGO_CM_ABOUT; cm->about_t0 = cm->now_ms; rebuild(cm); return TANGO_CM_REDRAWN;
    case SYS_VIDEO: cm->mode = TANGO_CM_VIDEO; cm->sel = 0; rebuild(cm); return TANGO_CM_REDRAWN;   /* 3048-3052 */
    case SYS_AFILT:                                       /* 3054-3057: Internal <-> Custom */
        if (cm->vp) vp_set_afilter_en(cm->vp, !cm->vp->afilt[0]);
        rebuild(cm); return TANGO_CM_REDRAWN;
    case SYS_AFILE:                                       /* 3059-3066: the browser in filters_audio */
        if (cm->vp && cm->vp->afilt[0]) vproc_browse(cm, VPICK_AFILT, VP_AFILTER_DIR, cm->vp->afilt + 1, "TXT");
        return TANGO_CM_REDRAWN;
    default: message(cm, "Not available yet", cm->lab[cm->sel]); return TANGO_CM_REDRAWN;
    }
}

/* the browser in a processing folder, at the folder of the current file (SelectFile with SCANO_DIR | SCANO_TXT,
 * Selected_tmp = dir/file or else dir: menu.cpp 3352-3356) */
static void vproc_browse(tango_coremenu_t *cm, int pick, const char *dir, const char *cur, const char *ext)
{
    char path[TANGO_PATH_MAX];
    snprintf(path, sizeof path, "%s", dir);
    const char *sl = strrchr(cur, '/');
    if (cur[0] && sl && (size_t)(sl - cur) + strlen(dir) + 2 < sizeof path) {
        size_t l = strlen(path); path[l++] = '/'; memcpy(path + l, cur, (size_t)(sl - cur)); path[l + (size_t)(sl - cur)] = 0;
    }
    cm->vp_pick = pick;
    cm->parent_sel = cm->sel;
    open_browser(cm, 1, ext, path);
    if (cm->br->error || !cm->br->n) open_browser(cm, 1, ext, dir);
}

/* MENU_VIDEOPROC2 select (menu.cpp 3336-3400) */
static int video_select(tango_coremenu_t *cm)
{
    tango_vproc_t *v = cm->vp;
    int e = cm->ent[cm->sel];
    if (e == VID_BACK || e == ROW_EXIT) { sys_goto(cm, SYS_VIDEO); return TANGO_CM_REDRAWN; }   /* 3394-3397 */
    if (!v || cm->rows[cm->sel].kind == TR_DIS) return TANGO_CM_NOTHING;
    switch (e) {
    case VID_PRESET: vproc_browse(cm, VPICK_PRESET, VP_PRESET_DIR, "", "INI"); return TANGO_CM_REDRAWN;
    case VID_HMODE: vp_set_scaler_flt(v, VP_HORZ, !v->flt[VP_HORZ].mode); break;
    case VID_VMODE: case VID_SMODE: case VID_IMODE: {
        int t = e == VID_VMODE ? VP_VERT : e == VID_SMODE ? VP_SCAN : VP_ILACE;
        if (v->flt[VP_HORZ].mode) vp_set_scaler_flt(v, t, !v->flt[t].mode);
        break;
    }
    case VID_HFILE: case VID_VFILE: case VID_SFILE: case VID_IFILE: {
        int t = e == VID_HFILE ? VP_HORZ : e == VID_VFILE ? VP_VERT : e == VID_SFILE ? VP_SCAN : VP_ILACE;
        if (v->flt[VP_HORZ].mode) vproc_browse(cm, VPICK_FLT + t, VP_COEFF_DIR, v->flt[t].name, "TXT");
        return TANGO_CM_REDRAWN;
    }
    case VID_GAMMA: vp_set_gamma_en(v, !v->gamma[0]); break;
    case VID_GFILE: if (v->gamma[0]) vproc_browse(cm, VPICK_GAMMA, VP_GAMMA_DIR, v->gamma + 1, "TXT"); return TANGO_CM_REDRAWN;
    case VID_MASK: vp_set_mask_mode(v, v->mask[0] + 1); break;
    case VID_MFILE: if (v->mask[0] > 0) vproc_browse(cm, VPICK_MASK, VP_SMASK_DIR, v->mask + 1, "TXT"); return TANGO_CM_REDRAWN;
    case VID_RESET: vp_reset_defaults(v); break;
    default: return TANGO_CM_NOTHING;
    }
    rebuild(cm);
    return TANGO_CM_REDRAWN;
}

/* MENU_ARCADE_DIP2 (menu.cpp 3576-3613): Enter on a dip row cycles its value and sends dip_cur right away
 * (arcade_sw_send -- tango_dl_send, index 254, the SAME wire call tango_fw.c's arc_setup_one
 * already uses); Enter on "Reset to apply" pulses a status[0] reset (so a core that only latches switches
 * at reset actually picks up the new value) and closes the OSD outright (MENU_NONE1, not back to page 0).
 * Both leave with something to persist -- TANGO_CM_DIP_SAVE tells the caller so (this module has no FatFs
 * access of its own, see that constant's own doc comment). The toggle's own value cycle is computed from
 * the SAME per-row, on-demand parse build_dip() uses (tango_mra_switches_dip_at) -- a second, independent
 * parse of just this one row, not anything cached from the build that drew it. */
static int dip_select(tango_coremenu_t *cm)
{
    int row = cm->sel, ent = cm->ent[row];
    if (ent == ROW_EXIT) {                             /* "Reset to apply" (menu.cpp 3584-3592) */
        cm->status[0] |= 1; tango_core_send_status(cm);
        cm->status[0] &= (uint8_t)~1; tango_core_send_status(cm);
        close_osd(cm);
        return TANGO_CM_DIP_SAVE;
    }
    if (ent < 0 || cm->rows[row].kind == TR_DIS) return TANGO_CM_NOTHING;   /* a disabled/malformed row */
    if (cm->plat->dl_busy && cm->plat->dl_busy(cm->plat->ctx)) return TANGO_CM_NOTHING;   /* refuse rather
        * than corrupt an in-flight download/upload this module has no visibility into (tango_core_platform_t's
        * own doc comment on dl_busy) */
    tango_mra_dip_t d;
    if (!tango_mra_switches_dip_at(cm->dip_zipfs, cm->dip_mra_path, 0, ent, &d) || !d.num) return TANGO_CM_NOTHING;
    uint64_t status = cm->dip_cur & d.mask;
    int m = 0;
    for (int v = 0; v < d.num; v++) if (status == d.val[v]) { m = v; break; }
    m = (m + 1) % d.num;                                /* menu.cpp 3607 */
    cm->dip_cur = (cm->dip_cur & ~d.mask) | d.val[m];    /* menu.cpp 3608 */
    tango_dl_send(cm->link, 254, (const uint8_t *)&cm->dip_cur, sizeof cm->dip_cur);   /* arcade_sw_send */
    rebuild(cm);
    return TANGO_CM_REDRAWN;
}

/* a file picked for the processing (MENU_COEFF/GAMMA/SMASK/AFILTER/PRESET_FILE_SELECTED, menu.cpp 3966-4025): the name
 * after the folder ("filters/Foo/x.txt" -> "Foo/x.txt"), or the whole path if it is elsewhere */
static void vproc_picked(tango_coremenu_t *cm, const char *rel)
{
    tango_vproc_t *v = cm->vp;
    static const char *const DIRS[] = { VP_COEFF_DIR, VP_GAMMA_DIR, VP_SMASK_DIR, VP_AFILTER_DIR, VP_PRESET_DIR };
    int pick = cm->vp_pick;
    const char *dir = pick < VPICK_GAMMA ? DIRS[0] : DIRS[pick - VPICK_GAMMA + 1];
    size_t dl = strlen(dir);
    const char *name = rel;
    int same = 1;
    for (size_t i = 0; i < dl; i++) {
        char a = rel[i], b = dir[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
        if (a != b) { same = 0; break; }
    }
    if (same && rel[dl] == '/') name = rel + dl + 1;
    if (pick >= VPICK_FLT && pick < VPICK_FLT + 4) vp_set_scaler_coeff(v, pick - VPICK_FLT, name);
    else if (pick == VPICK_GAMMA) vp_set_gamma_curve(v, name);
    else if (pick == VPICK_MASK) vp_set_mask(v, name);
    else if (pick == VPICK_AFILT) vp_set_afilter(v, name);
    else if (pick == VPICK_PRESET) vp_load_preset(v, rel, 1);
    /* back to the page the browser came from: MENU_COMMON1 for the audio filter (3066), Video processing for the rest */
    cm->mode = pick == VPICK_AFILT ? TANGO_CM_SYSTEM : TANGO_CM_VIDEO;
    cm->sel = cm->parent_sel; cm->vp_pick = VPICK_NONE;
    rebuild(cm);
}

/* fs_MenuCancel (menu.cpp 5570-5586: Menu, and B at the top of a list): the page the browser came from.  A Cores list opened by Alt+Menu closes the OSD (2266) */
static int browser_cancel(tango_coremenu_t *cm)
{
    leave_zip(cm);                                /* a way out of the whole Files list drops any open zip too */
    if (cm->mode == TANGO_CM_FILES && cm->vp_pick) {
        cm->mode = cm->vp_pick == VPICK_AFILT ? TANGO_CM_SYSTEM : TANGO_CM_VIDEO; cm->sel = cm->parent_sel; cm->vp_pick = VPICK_NONE;
    }
    else if (cm->mode == TANGO_CM_FILES) { cm->mode = TANGO_CM_PAGE; cm->sel = cm->parent_sel; }
    else if (cm->cores_direct) { cm->cores_direct = 0; return close_osd(cm); }
    else { cm->mode = TANGO_CM_SYSTEM; cm->sel = 0; }
    rebuild(cm); return TANGO_CM_REDRAWN;
}

static int browser_event(tango_coremenu_t *cm, tango_ev_t ev)
{
    tango_browser_t *b = cm->br;
    const tango_dirent_t *e;
    switch (ev) {
    case TANGO_EV_UP: tango_browser_prev(b); return TANGO_CM_REDRAWN;
    case TANGO_EV_DOWN: tango_browser_next(b); return TANGO_CM_REDRAWN;
    case TANGO_EV_LEFT: case TANGO_EV_PGUP: tango_browser_prev_page(b); return TANGO_CM_REDRAWN;
    case TANGO_EV_RIGHT: case TANGO_EV_PGDN: tango_browser_next_page(b); return TANGO_CM_REDRAWN;
    /* Home / Backspace (A + B) / Tab rescan the folder from the top.  A zip's listing comes from an adapter that lives only while
     * the archive is entered (enter_zip), so inside one they only move to its first entry, and Tab takes ".." out of the zip */
    case TANGO_EV_BACKSPACE: case TANGO_EV_HOME:
        if (cm->in_zip) tango_browser_select(b, 0);
        else if (ev == TANGO_EV_HOME) tango_browser_home(b, cm->plat->fs); else tango_browser_clear_filter(b, cm->plat->fs);
        return TANGO_CM_REDRAWN;
    case TANGO_EV_END:   tango_browser_end(b); return TANGO_CM_REDRAWN;
    case TANGO_EV_MINUS: tango_browser_prev_char(b); return TANGO_CM_REDRAWN;
    case TANGO_EV_PLUS:  tango_browser_next_char(b); return TANGO_CM_REDRAWN;
    case TANGO_EV_TAB:                           /* rescan from the top; on ".." that is up */
        if (cm->in_zip) {
            tango_browser_select(b, 0);
            if ((e = tango_browser_cur(b)) && e->is_dir) { leave_zip(cm); tango_browser_scan(b, cm->plat->fs, cm->real_folder); }
        } else tango_browser_tab(b, cm->plat->fs);
        return TANGO_CM_REDRAWN;
    case TANGO_EV_BACK:                          /* a typed filter goes first; in the Cores list B is up a folder, else fs_MenuCancel */
        if (b->filtered && !cm->in_zip) { tango_browser_clear_filter(b, cm->plat->fs); return TANGO_CM_REDRAWN; }
        if (cm->mode == TANGO_CM_CORES && b->path[0]) { tango_browser_chdir(b, cm->plat->fs, ".."); return TANGO_CM_REDRAWN; }
        return browser_cancel(cm);
    case TANGO_EV_SELECT: case TANGO_EV_ALT_SELECT: {
        if (!(e = tango_browser_cur(b))) return TANGO_CM_NOTHING;
        char name[TANGO_PATH_MAX];
        strncpy(name, tango_browser_name(b, e), sizeof name - 1); name[sizeof name - 1] = 0;
        if (cm->in_zip && e->is_dir) {             /* the zip's own ".." row: back out to the real folder */
            leave_zip(cm);
            tango_browser_scan(b, cm->plat->fs, cm->real_folder);
            return TANGO_CM_REDRAWN;
        }
        if (!cm->in_zip && cm->mode == TANGO_CM_FILES && !cm->vp_pick && !e->is_dir && is_zip_name(name))
            return enter_zip(cm, name);
        if (e->is_dir) { tango_browser_chdir(b, cm->plat->fs, name); return TANGO_CM_REDRAWN; }
        char path[2 * TANGO_PATH_MAX + 8];
        snprintf(path, sizeof path, "/sd/%s%s%s", b->path, b->path[0] ? "/" : "", name);
        if (cm->mode == TANGO_CM_CORES) {
            message(cm, "Loading", tango_browser_alt(b, e));
            tango_core_redraw(cm);
            if (cm->plat->load_core) cm->plat->load_core(cm->plat->ctx, path);   /* restarts the BL616 */
            return TANGO_CM_REDRAWN;
        }
        if (cm->vp_pick) {                                                     /* a processing file */
            char rel[2 * TANGO_PATH_MAX + 8];
            snprintf(rel, sizeof rel, "%s%s%s", b->path, b->path[0] ? "/" : "", name);
            vproc_picked(cm, rel);
            return TANGO_CM_REDRAWN;
        }
        uint8_t idx = (uint8_t)(tango_ext_idx(name, cm->f_exts) << 6 | cm->f_index);   /* menu.cpp 2680 */
        close_osd(cm);                                                           /* MenuHide (menu.cpp 2660) */
        int was_zip = cm->in_zip;
        char zip_path[TANGO_PATH_MAX]; strncpy(zip_path, cm->zip_relpath, sizeof zip_path - 1); zip_path[sizeof zip_path - 1] = 0;
        /* the Files list is no longer showing this zip's listing the instant the OSD closes for a load -- a
         * LATER open_browser() (the OSD reopened before this download finishes) must not tear down cm->zip out
         * from under it via leave_zip(); the platform's own load_zip_file/the firmware owns cm->zip from here,
         * until its download actually ends (tango_fw.c's dl_close, keyed off its own, separate dl_is_zip flag) */
        cm->in_zip = 0;
        if (was_zip) {
            if (cm->plat->load_zip_file) cm->plat->load_zip_file(cm->plat->ctx, zip_path, name, idx);
        } else if (cm->plat->load_file) cm->plat->load_file(cm->plat->ctx, path, idx);
        return TANGO_CM_CLOSED_NOW;
    }
    default: return TANGO_CM_NOTHING;
    }
}

/* flist_GetPrevNext (file_io.cpp 2098-2116): the file after (next) or before `cur` in its own folder under `dir`
 * (ScanDirectory with options 0: files with the extension only, no folders, 1736-1757; MiSTer's order, DirentComp
 * 1348-1384; wrapping both ways, 1891-1917).  If `cur` is empty or not there (!FileExists), the first file of `dir`
 * itself.  The name comes back relative to `dir` (static), or NULL if there is no such file.  FAT names: `cur` is
 * found without regard to case.  Uses the core menu's browser, as MiSTer's shares its DirItem list. */
static const char *flist_prev_next(tango_coremenu_t *cm, const char *dir, const char *cur, const char *ext, int next)
{
    static char out[TANGO_PATH_MAX];
    tango_browser_t *b = cm->br;
    const tango_fs_t *fs = cm->plat ? cm->plat->fs : NULL;
    if (!b || !fs) return NULL;
    tango_browser_set_mode(b, 2, ext);
    const char *slash = cur ? strrchr(cur, '/') : NULL;
    int sublen = slash ? (int)(slash - cur) : 0;
    int found = -1;
    if (cur && cur[0]) {
        char sub[TANGO_PATH_MAX];
        snprintf(sub, sizeof sub, "%s%s%.*s", dir, sublen ? "/" : "", sublen, cur);
        const char *bn = slash ? slash + 1 : cur;
        if (tango_browser_scan(b, fs, sub) > 0)
            for (int i = 0; i < b->n && found < 0; i++)
                if (!strcasecmp(tango_browser_name(b, &b->e[b->order[i]]), bn)) found = i;
    }
    if (found < 0) {                                    /* p = 0: the base folder, no step */
        sublen = 0;
        if (tango_browser_scan(b, fs, dir) <= 0) return NULL;
        tango_browser_select(b, 0);
    } else {
        tango_browser_select(b, found);
        if (next) tango_browser_next(b); else tango_browser_prev(b);
    }
    const tango_dirent_t *e = tango_browser_cur(b);
    if (!e) return NULL;
    snprintf(out, sizeof out, "%.*s%s%s", sublen, cur ? cur : "", sublen ? "/" : "", tango_browser_name(b, e));
    return out;
}

/* minus / plus (MiSTer: keys - =, a pad's L R) on the Video processing page (MENU_VIDEOPROC2, menu.cpp 3290-3335): the
 * Shadow Mask mode steps; a file row of an enabled filter, gamma or mask steps to the folder's previous / next file.
 * On the System page (MENU_COMMON2, 3172-3179): the Audio filter's file when Custom. */
static int minus_plus(tango_coremenu_t *cm, int plus)
{
    tango_vproc_t *v = cm->vp;
    if (!v) return TANGO_CM_NOTHING;
    const char *nf;
    int e = cm->ent[cm->sel];
    if (cm->mode == TANGO_CM_SYSTEM) {
        if (e != SYS_AFILE || !v->has_afilter || !v->afilt[0]) return TANGO_CM_NOTHING;
        nf = flist_prev_next(cm, VP_AFILTER_DIR, v->afilt + 1, "TXT", plus);
        vp_set_afilter(v, nf ? nf : "");
    } else if (cm->mode == TANGO_CM_VIDEO) {
        switch (e) {
        case VID_MASK:                                    /* 3292-3295 tests menusub 9, which is the Gamma row since
                                                           * the rows moved (3244-3264): Tango steps the mode on the
                                                           * Shadow Mask row (11), as the code means */
            if (!v->has_mask) return TANGO_CM_NOTHING;
            vp_set_mask_mode(v, v->mask[0] + (plus ? 1 : -1));
            break;
        case VID_HFILE: case VID_VFILE: case VID_SFILE: case VID_IFILE: {   /* 3299-3311 */
            int t = e == VID_HFILE ? VP_HORZ : e == VID_VFILE ? VP_VERT : e == VID_SFILE ? VP_SCAN : VP_ILACE;
            if (!v->has_scaler || !v->flt[VP_HORZ].mode || !v->flt[t].mode) return TANGO_CM_NOTHING;
            nf = flist_prev_next(cm, VP_COEFF_DIR, v->flt[t].name, "TXT", plus);
            vp_set_scaler_coeff(v, t, nf ? nf : "");
            break;
        }
        case VID_GFILE:                                   /* 3313-3319 */
            if (!v->has_gamma || !v->gamma[0]) return TANGO_CM_NOTHING;
            nf = flist_prev_next(cm, VP_GAMMA_DIR, v->gamma + 1, "TXT", plus);
            vp_set_gamma_curve(v, nf ? nf : "");
            break;
        case VID_MFILE:                                   /* 3321-3327 */
            if (!v->has_mask || v->mask[0] <= 0) return TANGO_CM_NOTHING;
            nf = flist_prev_next(cm, VP_SMASK_DIR, v->mask + 1, "TXT", plus);
            vp_set_mask(v, nf ? nf : "");
            break;
        default: return TANGO_CM_NOTHING;
        }
    } else return TANGO_CM_NOTHING;
    rebuild(cm);
    return TANGO_CM_REDRAWN;
}

int tango_core_repeats(const tango_coremenu_t *cm, tango_ev_t ev)
{
    return tango_ev_repeats(ev, cm->mode == TANGO_CM_FILES || cm->mode == TANGO_CM_CORES);
}

int tango_core_event(tango_coremenu_t *cm, tango_ev_t ev)
{
    if (cm->mode == TANGO_CM_CLOSED) return TANGO_CM_NOTHING;
    if (ev == TANGO_EV_SPACE) ev = TANGO_EV_SELECT;                /* Space is Select (menu.cpp 1405); Define buttons' capture reads it as a skip itself (tango_fw.c) */
    int had_msg = cm->status_key[0] != 0;
    cm->status_key[0] = 0;
    int r = TANGO_CM_NOTHING;
    if (cm->mode == TANGO_CM_SAVING) {                /* MENU_SAVE_WAIT: Menu closes, nothing else (2282-2293) */
        if (ev == TANGO_EV_MENU) { cm->save_until = 0; return close_osd(cm); }
        return TANGO_CM_NOTHING;
    }
    if (cm->mode == TANGO_CM_DEFINE) {                /* the pad drives the prompts; only the question takes keys */
        if (cm->pm.state != PMC_ASK) return TANGO_CM_NOTHING;   /* (the keyboard's keys: tango_core_define_key) */
        int k = ev == TANGO_EV_UP ? PMC_KEY_UP : ev == TANGO_EV_DOWN ? PMC_KEY_DOWN : ev == TANGO_EV_SELECT ? PMC_KEY_ENTER :
                (ev == TANGO_EV_MENU || ev == TANGO_EV_BACK) ? PMC_KEY_ESC : 0;
        return k ? tango_core_define_key(cm, k) : TANGO_CM_NOTHING;
    }
    if (ev == TANGO_EV_MENU && cm->mode == TANGO_CM_RESET) {   /* menu.cpp 5966-5969: Menu = "no" */
        sys_goto(cm, SYS_RESET); tango_core_redraw(cm); return TANGO_CM_REDRAWN;
    }
    if (ev == TANGO_EV_MENU && cm->mode == TANGO_CM_DIP) {     /* menu.cpp 3576-3582: Menu also returns to
        * page 0 here (not a close, unlike every other mode below) -- and ALSO saves, same as Back/Left */
        cm->mode = TANGO_CM_PAGE; cm->page = 0; cm->sel = cm->parent_sel; rebuild(cm); tango_core_redraw(cm);
        return TANGO_CM_DIP_SAVE;
    }
    /* Menu / Esc / F12 (the menu key), page by page as MiSTer's handlers: Video processing and About go back to System (menu.cpp
     * 3274-3279, 4560-4562), a file or core list cancels to the page that opened it (5570-5586), everything else closes (NONE1) */
    if (ev == TANGO_EV_MENU && cm->mode != TANGO_CM_VIDEO && cm->mode != TANGO_CM_ABOUT && cm->mode != TANGO_CM_FILES && cm->mode != TANGO_CM_CORES)
        return close_osd(cm);
    if (ev == TANGO_EV_MENU) {
        if (cm->mode == TANGO_CM_VIDEO) sys_goto(cm, SYS_VIDEO);
        else if (cm->mode == TANGO_CM_ABOUT) about_back(cm);
        else { r = browser_cancel(cm); if (r == TANGO_CM_CLOSED_NOW) return r; }
        r = TANGO_CM_REDRAWN;
    }
    else if (cm->mode == TANGO_CM_FILES || cm->mode == TANGO_CM_CORES) r = browser_event(cm, ev);
    else switch (ev) {
    case TANGO_EV_UP:   move(cm, -1); r = TANGO_CM_REDRAWN; break;
    case TANGO_EV_DOWN: move(cm, +1); r = TANGO_CM_REDRAWN; break;
    case TANGO_EV_SELECT:
        if (cm->mode == TANGO_CM_PAGE) r = page_select(cm);
        else if (cm->mode == TANGO_CM_SYSTEM) r = system_select(cm);
        else if (cm->mode == TANGO_CM_ABOUT) { about_back(cm); r = TANGO_CM_REDRAWN; }
        else if (cm->mode == TANGO_CM_VIDEO) r = video_select(cm);
        else if (cm->mode == TANGO_CM_MISC) r = misc_event(cm, ev);
        else if (cm->mode == TANGO_CM_DIP) r = dip_select(cm);
        else {                                                  /* Reset: yes = reset + save, no = System */
            if (cm->ent[cm->sel] == 1) {                            /* MENU_RESET2 menu.cpp 5932-5962 */
                memset(cm->status, 0, 16); tango_core_send_status(cm);            /* user_io_status_reset */
                if (cm->plat->save_cfg) cm->plat->save_cfg(cm->plat->ctx, cm->cfgname, cm->status);
                cm->page = 0; cm->sel = 0;
                r = close_osd(cm);                                              /* menustate = MENU_NONE1 */
                /* menu.cpp 5964-5970: real MiSTer ALSO resets the arcade DIPs to their default here; this
                 * struct tracks dip_cur/dip_def for its own page, so Tango does the same.
                 * TANGO_CM_DIP_SAVE (not CLOSED_NOW): the caller still owes the .dip
                 * file save, same as every other path that changes dip_cur. */
                if (cm->dip_num > 0) {
                    cm->dip_cur = cm->dip_def;
                    if (!cm->plat->dl_busy || !cm->plat->dl_busy(cm->plat->ctx))   /* see dip_select's own comment */
                        tango_dl_send(cm->link, 254, (const uint8_t *)&cm->dip_cur, sizeof cm->dip_cur);
                    cm->status[0] |= 1; tango_core_send_status(cm);
                    cm->status[0] &= (uint8_t)~1; tango_core_send_status(cm);
                    r = TANGO_CM_DIP_SAVE;
                }
            } else { sys_goto(cm, SYS_RESET); r = TANGO_CM_REDRAWN; }
        }
        break;
    case TANGO_EV_MINUS:
    case TANGO_EV_PLUS:
        r = cm->mode == TANGO_CM_MISC ? misc_event(cm, ev) : cm->mode == TANGO_CM_PAGE ? page_option(cm, ev == TANGO_EV_MINUS) :
            minus_plus(cm, ev == TANGO_EV_PLUS);
        break;
    case TANGO_EV_RIGHT:
        if (cm->mode == TANGO_CM_MISC) r = misc_event(cm, ev);
        else if (cm->mode == TANGO_CM_PAGE && !cm->page) { cm->mode = TANGO_CM_SYSTEM; cm->sel = 0; rebuild(cm); r = TANGO_CM_REDRAWN; }
        break;
    case TANGO_EV_LEFT:
    case TANGO_EV_BACK:
        if (cm->mode == TANGO_CM_MISC) {                      /* Left: the row's own (volume); B: the OSD closes, as MiSTer's (menu.cpp 4100-4160: B is Menu there) */
            if (ev == TANGO_EV_LEFT) r = misc_event(cm, ev);
            if (ev == TANGO_EV_BACK) return close_osd(cm);
        }
        else if (cm->mode == TANGO_CM_SYSTEM && ev == TANGO_EV_BACK) return close_osd(cm);   /* menu.cpp 2971-2983 */
        else if (cm->mode == TANGO_CM_VIDEO) { sys_goto(cm, SYS_VIDEO); r = TANGO_CM_REDRAWN; }   /* menu or left (3274-3279) */
        else if (cm->mode == TANGO_CM_ABOUT) { about_back(cm); r = TANGO_CM_REDRAWN; }
        else if (cm->mode == TANGO_CM_SYSTEM) { cm->mode = TANGO_CM_PAGE; cm->page = 0; cm->sel = 0; rebuild(cm); r = TANGO_CM_REDRAWN; }
        else if (cm->mode == TANGO_CM_RESET) { sys_goto(cm, SYS_RESET); r = TANGO_CM_REDRAWN; }
        else if (cm->mode == TANGO_CM_DIP) {   /* menu.cpp 3576-3582: same as Menu on this page -- back to
            * page 0, saving first (TANGO_CM_DIP_SAVE, not REDRAWN -- the caller still owes the save) */
            cm->mode = TANGO_CM_PAGE; cm->page = 0; cm->sel = cm->parent_sel; rebuild(cm); tango_core_redraw(cm);
            r = TANGO_CM_DIP_SAVE;
        }
        else if (cm->page) { cm->page = 0; cm->sel = cm->parent_sel; rebuild(cm); r = TANGO_CM_REDRAWN; }
        else if (ev == TANGO_EV_BACK) r = close_osd(cm);       /* back on page 0: close */
        else {                                                  /* Left on page 0: MENU_MISC1, menusub 3 = exit (menu.cpp 2640-2646) */
            cm->mode = TANGO_CM_MISC; cm->misc_vs = cm->plat->vsync ? cm->plat->vsync(cm->plat->ctx) : 0;   /* once, as the page opens */
            build_misc(cm); cm->sel = cm->nrows - 1; r = TANGO_CM_REDRAWN;
        }
        break;
    default: break;
    }
    if (r == TANGO_CM_REDRAWN || (r == TANGO_CM_NOTHING && had_msg && cm->mode != TANGO_CM_CLOSED)) {
        tango_core_redraw(cm);
        if (r == TANGO_CM_NOTHING) r = TANGO_CM_REDRAWN;
    }
    return r;
}

/* ---------------------------------------------------------------- file download */
int tango_dl_begin(tango_dl_t *d, const tango_link_t *link, uint8_t index, uint32_t size)
{
    memset(d, 0, sizeof *d);
    d->link = link; d->index = index; d->size = size;
    if (!size) return -1;                        /* SELECT with size 0 means "close": nothing to send */
    tango_ioctl_select(link, index, size);
    d->open = 1;
    return 0;
}

void tango_dl_end(tango_dl_t *d)
{
    if (!d->open) return;
    tango_ioctl_select(d->link, d->index, 0);
    d->open = 0;
}

int tango_dl_pump(tango_dl_t *d, int (*rd)(void *ctx, uint8_t *buf, unsigned n), void *ctx)
{
    static uint8_t buf[512];
    if (!d->open) return 0;
    unsigned space = 0;
    if (!tango_ioctl_status(d->link, d->index, &space)) { d->open = 0; return -1; }   /* the core closed it */
    unsigned n = d->size - d->sent < space ? d->size - d->sent : space;
    if (n) {
        int got = rd(ctx, buf, n);
        if (got <= 0) { tango_dl_end(d); return -1; }
        tango_ioctl_write(d->link, d->index, buf, (unsigned)got);
        d->sent += (unsigned)got;
    }
    if (d->sent >= d->size) { if (d->before_end) d->before_end(d->end_ctx); tango_dl_end(d); return 0; }
    return 1;
}

/* ---------------------------------------------------------------- upload from the core */
int tango_ul_begin(tango_ul_t *u, const tango_link_t *link, uint8_t index, uint32_t size)
{
    memset(u, 0, sizeof *u);
    u->link = link; u->index = index; u->size = size;
    if (!size) return -1;                        /* UL_SELECT with size 0 means "close": nothing to receive */
    /* Refused, not silently truncated: the RTL's own buffer (sys/tango_io.v's UL_DEPTH) clamps a larger
     * session on its own, and UL_STATUS would then report the clamped size -- but a caller that read THIS
     * size back from the file it is saving and got fewer bytes than that would wrongly believe the save
     * came back whole. Failing here instead gives the caller an unambiguous signal to chunk the save
     * itself (several UL_SELECT/_pump sessions) rather than trust a number this layer cannot honour. */
    if (size > TANGO_UL_MAX) return -1;
    tango_ioctl_ul_select(link, index, size);
    u->open = 1;
    return 0;
}

void tango_ul_end(tango_ul_t *u)
{
    if (!u->open) return;
    tango_ioctl_ul_select(u->link, u->index, 0);
    u->open = 0;
}

int tango_ul_pump(tango_ul_t *u, int (*wr)(void *ctx, const uint8_t *buf, unsigned n), void *ctx)
{
    static uint8_t buf[512];
    if (!u->open) return 0;
    unsigned avail = 0;
    if (!tango_ioctl_ul_status(u->link, u->index, &avail)) {
        /* UL_STATUS reports the open size in bytes 5-6 from the moment UL_SELECT opens it -- well before
         * "ready" (byte 4 bit 7) ever goes high -- so avail == 0 here can only mean nothing is actually
         * open: no core answered this SDC_IMAGE sub-command at all (TANGO_IOCTL_UPLOAD not built), or the
         * session was closed from elsewhere. Either way, polling again would never succeed; give up, as
         * tango_dl_pump does when the core itself closes a download out from under it. A real "still
         * filling" state always reports avail == u->size, never 0. */
        if (!avail) { u->open = 0; return -1; }
        return 1;                                                      /* still filling: try again later */
    }
    unsigned n = u->size - u->got < sizeof buf ? u->size - u->got : sizeof buf;
    if (n > avail) n = avail;
    if (n) {
        tango_ioctl_read(u->link, u->index, buf, n);
        /* tango_ioctl_read has already pulled n bytes off the RTL's own cursor (ul_off): a SHORT put, not
         * just put <= 0, would desync u->got from it (unlike tango_dl_pump's own WRITE side, which only
         * ever sends the bytes it was given, so a short count there has no such cursor to fall out of step
         * with). Treated as a hard failure, same as a short/garbled download read is (tango_mra_rom_read's
         * own extra return codes): the caller finds out its save is incomplete instead of
         * silently keeping a byte count that no longer matches what the RTL will hand back on the next
         * READ (the unwritten bytes would simply be skipped over, not retried). */
        int put = wr(ctx, buf, n);
        if (put != (int)n) { tango_ul_end(u); return -1; }
        u->got += n;
    }
    if (u->got >= u->size) { tango_ul_end(u); return 0; }
    return 1;
}

/* ---------------------------------------------------------------- whole-blob
 * helpers, synchronous. DIP/cheat send and NVRAM load reuse the SAME tango_dl_t download mechanism already
 * used for ROM/file loads, just at a new fixed index (MiSTer's own arcade_sw_send/arcade_nvm_load,
 * mra_loader.cpp 116-126, 90-109: "nothing but user_io_set_index(idx) + a plain indexed blob transfer").
 * NVRAM save is the mirror, over the upload direction (tango_ul_t). Both loop their own pump to completion
 * locally rather than returning control to a caller's own per-interrupt pump loop (tango_dl_pump's/
 * tango_ul_pump's usual shape): every real caller's blob here is already whole, in RAM, and small (a
 * dip_cur word: 8 bytes; a real arcade NVRAM image: a few hundred bytes, never more than TANGO_UL_MAX on
 * the save side, tango_ul_begin's own refusal) -- there is no reason to spread it across multiple SDC
 * interrupts the way a multi-KB ROM or Files download needs to. Each uses its OWN local tango_dl_t/
 * tango_ul_t (never a caller's shared struct), so calling one of these right after another (DIP, then
 * cheats, then NVRAM, as tango_fw.c's glue does once per .mra core load) is simply three sequential,
 * independent wire sessions -- exactly as safe as loading two files back to back already is.
 * The ONE invariant neither of these checks for itself, left to the caller: do not call either while a
 * DIFFERENT download/upload session is genuinely open (tango_fw.c's own dl_file_open/dl_is_zip/dl_is_mra).
 * Opening a new download on top of one already open would corrupt it (the FPGA's download FIFO just sees a
 * second SELECT mid-stream); opening an upload while a download is open is not corrupted -- the RTL
 * defers it -- but polling that deferral to completion from inside this same,
 * single-threaded loop, when the only thing that can ever advance the download blocking it is a message
 * this same loop has not yet processed, is exactly the self-deadlock menu-tango docs/TANGO-SPEC.md's
 * "File download" section warns the firmware against ("must never itself block, waiting on a download to finish, while
 * holding an upload open, or the reverse"). */
/* Both pumps below are bounded, not "loop until done": a core holding ioctl_wait forever (download) or an
 * upload left genuinely wedged behind one (a scenario this file's own comment above already asks the
 * CALLER to avoid, but a stuck/misbehaving core's own ioctl_wait is not something any caller here
 * controls) would otherwise spin tango_dl_pump/tango_ul_pump's own "try again later" return (1) forever --
 * a real reliability risk: tango_fw.c's own c_reboot calls
 * tango_ul_recv (via arc_save_nvram) AFTER tearing down any in-flight download but BEFORE the MCU resets,
 * so an unbounded loop there would hang Reboot itself, the user's one escape hatch from a wedged core.
 * Bounded with an explicit delay between retries (link->delay_ms, already part of every tango_link_t --
 * tango_osd_show's own "." poll at tango_proto.c:115 uses the same 1 ms idiom), not just an iteration
 * count: an iteration count alone has no known wall-clock length (each poll is a handful of real SPI
 * transfers, whose own timing this file has no visibility into), while TANGO_DL_SEND_MAX_POLLS * TANGO_DL_SEND_POLL_MS is a real, stated bound:
 * about 2 seconds, generous for any legitimate transfer this file's own callers ever make (a dip_cur word:
 * 1 poll; a real NVRAM image, worst case TANGO_UL_MAX/512 ~= 4 polls under normal backpressure) while
 * still giving up, cleanly (ending the session so a later open isn't left stuck behind it), in a bounded
 * time instead of an unknown one. */
#define TANGO_DL_SEND_MAX_POLLS 2000
#define TANGO_DL_SEND_POLL_MS   1u
static void dl_send_wait(const tango_link_t *link) { if (link->delay_ms) link->delay_ms(link->ctx, TANGO_DL_SEND_POLL_MS); }
typedef struct { const uint8_t *p; uint32_t n, off; } tango_dl_blob_src_t;
static int tango_dl_blob_rd(void *ctx, uint8_t *buf, unsigned n)
{
    tango_dl_blob_src_t *s = ctx;
    uint32_t left = s->n - s->off;
    if (n > left) n = (unsigned)left;
    memcpy(buf, s->p + s->off, n);
    s->off += n;
    return (int)n;
}
int tango_dl_send(const tango_link_t *link, uint8_t index, const uint8_t *buf, uint32_t n)
{
    if (!n) return 0;
    tango_dl_t d;
    if (tango_dl_begin(&d, link, index, n) != 0) return -1;
    tango_dl_blob_src_t s = { buf, n, 0 };
    int r; int polls = 0;
    do { if (polls) dl_send_wait(link); r = tango_dl_pump(&d, tango_dl_blob_rd, &s); } while (r == 1 && ++polls < TANGO_DL_SEND_MAX_POLLS);
    if (r == 1) { tango_dl_end(&d); return -1; }   /* gave up: the core never freed enough space */
    return r == 0 ? 0 : -1;
}

typedef struct { uint8_t *p; uint32_t cap, off; } tango_ul_blob_dst_t;
static int tango_ul_blob_wr(void *ctx, const uint8_t *buf, unsigned n)
{
    tango_ul_blob_dst_t *s = ctx;
    if (s->off + n > s->cap) return -1;          /* should never happen: size <= cap is checked at open */
    memcpy(s->p + s->off, buf, n);
    s->off += n;
    return (int)n;
}
int tango_ul_recv(const tango_link_t *link, uint8_t index, uint32_t size, uint8_t *buf, uint32_t cap, uint32_t *got)
{
    if (got) *got = 0;
    if (!size || size > cap) return -1;           /* the caller's own buffer, not the RTL's TANGO_UL_MAX clamp */
    tango_ul_t u;
    if (tango_ul_begin(&u, link, index, size) != 0) return -1;
    tango_ul_blob_dst_t s = { buf, cap, 0 };
    int r; int polls = 0;
    do { if (polls) dl_send_wait(link); r = tango_ul_pump(&u, tango_ul_blob_wr, &s); } while (r == 1 && ++polls < TANGO_DL_SEND_MAX_POLLS);
    if (r == 1) { tango_ul_end(&u); return -1; }   /* gave up: never became ready (deferred forever, or a wedged core) */
    if (r == 0 && got) *got = s.off;
    return r == 0 ? 0 : -1;
}

int tango_reboot_cold(unsigned held_ms)
{
    int hold_cnt = held_ms < 500 ? 1 : 2 + (int)((held_ms - 500) / 50);
    return hold_cnt / 3 > 5;
}

/* ---------------------------------------------------------------- Define <core> buttons */
static int define_result(tango_coremenu_t *cm, int r)
{
    if (r == PMC_NONE) return TANGO_CM_NOTHING;
    if (r == PMC_SAVED) {                               /* finish_map_setting(0): save_map (input.cpp 1905) */
        char name[80];
        tango_pm_mapname(cm->name, cm->pm.vid, cm->pm.pid, name, sizeof name);
        if (cm->plat->save_map) cm->plat->save_map(cm->plat->ctx, name, cm->pm.map);
        if (cm->plat->maps_changed) cm->plat->maps_changed(cm->plat->ctx);
        if (cm->pm.state == PMC_ASK) { tango_core_redraw(cm); return TANGO_CM_REDRAWN; }
    } else if (r == PMC_DISMISSED) {                    /* nothing saved: the pads get their maps back */
        if (cm->plat->maps_changed) cm->plat->maps_changed(cm->plat->ctx);
    }
    if (r == PMC_SAVED || r == PMC_EXIT || r == PMC_DISMISSED) {
        cm->mode = TANGO_CM_SYSTEM; cm->sel = 2;    /* MENU_COMMON1, menusub 2 (menu.cpp 4380-4384, 4413-4424) */
        rebuild(cm);
    }
    tango_core_redraw(cm);
    return TANGO_CM_REDRAWN;
}

int tango_core_capturing(const tango_coremenu_t *cm)
{
    return cm->mode == TANGO_CM_DEFINE && cm->pm.state == PMC_MAP;
}

void tango_core_set_menu_bits(tango_coremenu_t *cm, int port, int a, int b) { tango_pmcap_set_menu(&cm->pm, port, a, b); }
void tango_core_set_known(tango_coremenu_t *cm, int port, int known) { tango_pmcap_known(&cm->pm, port, known); }

void tango_core_pad_gone(tango_coremenu_t *cm, int port)
{
    if (port < 0 || port > 1) return;
    cm->pm.pads[port].conn = 0; cm->pm.prev[port] = 0;
}

int tango_core_raw(tango_coremenu_t *cm, int port, uint16_t vid, uint16_t pid, uint32_t raw, uint32_t now_ms)
{
    cm->now_ms = now_ms;
    if (port < 0 || port > 1) return TANGO_CM_NOTHING;
    tango_pmcap_pad(&cm->pm, port, 1, vid, pid);
    if (cm->mode != TANGO_CM_DEFINE) { cm->pm.prev[port] = raw; return TANGO_CM_NOTHING; }
    return define_result(cm, tango_pmcap_raw(&cm->pm, port, vid, pid, raw, now_ms));
}

int tango_core_define_key(tango_coremenu_t *cm, int key)
{
    if (cm->mode != TANGO_CM_DEFINE) return TANGO_CM_NOTHING;
    return define_result(cm, tango_pmcap_key(&cm->pm, key));
}

int tango_core_menu_key(tango_coremenu_t *cm, int from_kbd)
{
    if (cm->mode != TANGO_CM_DEFINE) return TANGO_CM_NOTHING;
    if (from_kbd) return tango_core_define_key(cm, tango_core_capturing(cm) ? PMC_KEY_F12 : PMC_KEY_ESC);   /* F12 = Clear all */
    return tango_core_capturing(cm) ? TANGO_CM_NOTHING : tango_core_define_key(cm, PMC_KEY_ESC);
}

void tango_core_saved_sector(tango_coremenu_t *cm, uint32_t now_ms)
{
    cm->save_until = now_ms + 500; if (!cm->save_until) cm->save_until = 1;
}

int tango_core_type(tango_coremenu_t *cm, char ch, uint32_t now_ms)
{
    cm->now_ms = now_ms;
    if (cm->mode != TANGO_CM_FILES && cm->mode != TANGO_CM_CORES) return TANGO_CM_NOTHING;
    if (!tango_browser_type(cm->br, cm->plat->fs, ch, now_ms)) return TANGO_CM_NOTHING;
    tango_core_redraw(cm);
    return TANGO_CM_REDRAWN;
}

int tango_core_wait_ms(const tango_coremenu_t *cm, uint32_t now_ms)
{
    if (cm->mode == TANGO_CM_ABOUT) return tango_about_wait(cm->about_t0, now_ms);        /* the ticker's next step */
    return (cm->mode == TANGO_CM_FILES || cm->mode == TANGO_CM_CORES) && cm->br->n ? tango_browser_scroll_wait(cm->br, now_ms) : -1;
}

int tango_core_tick(tango_coremenu_t *cm, uint32_t now_ms)
{
    if (cm->mode == TANGO_CM_ABOUT) {                 /* the ticker moves */
        cm->now_ms = now_ms;
        if (tango_about_step(cm->about_t0, now_ms) == cm->about_step) return TANGO_CM_NOTHING;
        build_about(cm); tango_core_redraw(cm);
        return TANGO_CM_REDRAWN;
    }
    if ((cm->mode == TANGO_CM_FILES || cm->mode == TANGO_CM_CORES) && cm->br->n) {   /* a long name moves */
        cm->now_ms = now_ms;
        if (tango_browser_scroll_tick(cm->br, now_ms)) { tango_core_redraw(cm); return TANGO_CM_REDRAWN; }
        return TANGO_CM_NOTHING;
    }
    if (cm->mode == TANGO_CM_SAVING) {                /* the 500 ms are over: page 0 (parentstate + 1) */
        cm->now_ms = now_ms;
        if ((int32_t)(cm->save_until - now_ms) > 0) return TANGO_CM_NOTHING;
        cm->save_until = 0; cm->mode = TANGO_CM_PAGE; cm->page = 0; cm->sel = 0;
        rebuild(cm); tango_core_redraw(cm);
        return TANGO_CM_REDRAWN;
    }
    int was = tango_pmcap_canceling(&cm->pm, cm->now_ms);
    cm->now_ms = now_ms;
    if (cm->mode == TANGO_CM_DEFINE && tango_pmcap_canceling(&cm->pm, now_ms) != was) { tango_core_redraw(cm); return TANGO_CM_REDRAWN; }
    return TANGO_CM_NOTHING;
}
