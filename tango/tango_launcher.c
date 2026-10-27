// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 the Tango authors
/*
 * tango_launcher.c -- see tango_launcher.h. Tango, our own code.
 */
#include <stdio.h>
#include <string.h>
#include "tango_launcher.h"
#include "tango_about.h"
#include "tango_coremenu.h"     /* tango_reboot_cold */
#include "confstr.h"

#define BANNER "TANG CONSOLE"
/* the Cores page legend of the mock-up (menu-tango tools/osd-model/launcher_pages.py KEYS_BROWSE) */
static const tango_legend_t KEYS_BROWSE[] = {
    { "A", "open" }, { "Menu", "settings" }, { "\x83\x7f", "page" },     /* no "` recent": there is no Recent Cores page yet */
};

/* ---------------------------------------------------------------- a pad as the OSD sees it */
void tango_padkeys_sync(tango_padkeys_t *k, uint16_t state)
{
    memset(k, 0, sizeof *k);
    k->prev = state;
    if (state & TANGO_JOY_SELECT) { k->sel_down = 1; k->sel_start = 1; }   /* a held button: no key from its release */
    if (state & (TANGO_JOY_A | TANGO_JOY_B)) k->chord = 1;
}

void tango_padkeys_step(tango_padkeys_t *k, uint16_t state, tango_padkeys_out_t *o)
{
    uint16_t dn = (uint16_t)(state & ~k->prev), up = (uint16_t)(k->prev & ~state);
    const uint16_t dirs = TANGO_JOY_UP | TANGO_JOY_DOWN | TANGO_JOY_LEFT | TANGO_JOY_RIGHT;
    memset(o, 0, sizeof *o);
    if (dn & TANGO_JOY_UP)    o->ev[o->n++] = TANGO_EV_UP;
    if (dn & TANGO_JOY_DOWN)  o->ev[o->n++] = TANGO_EV_DOWN;
    if (dn & TANGO_JOY_LEFT)  o->ev[o->n++] = TANGO_EV_LEFT;
    if (dn & TANGO_JOY_RIGHT) o->ev[o->n++] = TANGO_EV_RIGHT;
    if ((k->prev & dirs) && !(state & (k->prev & dirs))) o->dir_released = 1;
    /* A and B: Enter on A's press; Back on B's release; both together = Backspace (joy_digital's osdbtn bookkeeping) */
    if ((dn & (TANGO_JOY_A | TANGO_JOY_B)) && (state & (TANGO_JOY_A | TANGO_JOY_B)) == (TANGO_JOY_A | TANGO_JOY_B)) {
        k->chord = 1;                                   /* both in one report, or the second one going down */
        o->ev[o->n++] = TANGO_EV_BACKSPACE;
    } else if (dn & TANGO_JOY_A) o->ev[o->n++] = TANGO_EV_SELECT;
    else if (dn & TANGO_JOY_B) k->b_down = 1;
    if (up & TANGO_JOY_A) o->a_released = 1;
    if ((up & TANGO_JOY_B) && k->b_down && !k->chord) o->ev[o->n++] = TANGO_EV_BACK;
    if (up & TANGO_JOY_B) k->b_down = 0;
    if (!(state & (TANGO_JOY_A | TANGO_JOY_B))) { k->chord = 0; k->b_down = 0; }
    if (dn & TANGO_JOY_X) o->ev[o->n++] = TANGO_EV_TAB;
    if (dn & TANGO_JOY_L) o->ev[o->n++] = TANGO_EV_MINUS;
    if (dn & TANGO_JOY_R) o->ev[o->n++] = TANGO_EV_PLUS;
    /* Select (the grave key) on its release, but not on the way into or out of the Select+Start combo */
    if (dn & TANGO_JOY_SELECT) { k->sel_down = 1; k->sel_start = (state & TANGO_JOY_START) != 0; }
    if ((dn & TANGO_JOY_START) && k->sel_down) k->sel_start = 1;
    if ((state & (TANGO_JOY_SELECT | TANGO_JOY_START)) == (TANGO_JOY_SELECT | TANGO_JOY_START) &&
        (k->prev & (TANGO_JOY_SELECT | TANGO_JOY_START)) != (TANGO_JOY_SELECT | TANGO_JOY_START)) o->combo = 1;
    if ((k->prev & (TANGO_JOY_SELECT | TANGO_JOY_START)) == (TANGO_JOY_SELECT | TANGO_JOY_START) &&
        (state & (TANGO_JOY_SELECT | TANGO_JOY_START)) != (TANGO_JOY_SELECT | TANGO_JOY_START)) o->combo_up = 1;   /* input.cpp 3211-3214 */
    if (up & TANGO_JOY_SELECT) {
        if (k->sel_down && !k->sel_start) o->ev[o->n++] = TANGO_EV_RECENT;
        k->sel_down = k->sel_start = 0;
    }
    k->prev = state;
}

int tango_ev_repeats(tango_ev_t ev, int in_file_list)
{
    if (ev == TANGO_EV_UP || ev == TANGO_EV_DOWN) return 1;
    return in_file_list && (ev == TANGO_EV_LEFT || ev == TANGO_EV_RIGHT || ev == TANGO_EV_PGUP || ev == TANGO_EV_PGDN ||
                            ev == TANGO_EV_MINUS || ev == TANGO_EV_PLUS);
}

int tango_launcher_repeats(const tango_launcher_t *L, tango_ev_t ev) { return tango_ev_repeats(ev, L->mode == TL_CORES); }

int tango_launcher_detect(tango_launcher_t *L, const tango_link_t *link)
{
    if (!tango_osd_probe(&L->osd, link)) return 0;
    char cfg[33];                              /* 32 bytes of CONF_STR */
    return tango_core_is_menu(link, cfg, sizeof cfg);
}

/* ---------------------------------------------------------------- System Settings / Misc. Options */
enum { SI_NONE = 0, SI_REMAP, SI_DEFINE, SI_REBOOT, SI_EXIT, MI_CONFIG, MI_BOOT, MI_PLAYERS, MI_COREVOL, MI_GLOBVOL, SI_ABOUT };
static const tango_legend_t LEG_SYS[]  = { { "A", "select" }, { "Menu", "cores" }, { "\x83", "misc." } };
static const tango_legend_t LEG_ABOUT[] = { { "\x83", "back" } };
static const tango_legend_t LEG_MISC[] = { { "\x83\x7f", "change" }, { "A", "select" }, { "Menu", "cores" } };
#define N(a) ((int)(sizeof a / sizeof a[0]))

static int srow_selectable(const tango_row_t *r) { return r->kind != TR_BLANK && r->kind != TR_CENTER && r->kind != TR_SEP && r->kind != TR_LOGO && r->kind != TR_SCROLL; }

static void srow_add(tango_launcher_t *L, int kind, int id, const char *label, int col)
{
    if (L->nsrow >= TL_ROWS) return;
    tango_row_t *r = &L->srow[L->nsrow];
    r->kind = (uint8_t)kind; r->col = (uint8_t)col; r->label = label; r->value = NULL;
    L->sid[L->nsrow++] = (uint8_t)id;
}

/* MENU_SYSTEM1 (menu.cpp 7368-7448) without Scripts, Help and Switch to USB.  Storage and the free space are the
 * card's; the two version lines are what this build knows about itself (tango_launcher_set_info). */
static void build_system(tango_launcher_t *L)
{
    L->nsrow = 0;
    snprintf(L->sline[0], sizeof L->sline[0], "Firmware %s", L->fw_id && L->fw_id[0] ? L->fw_id : "unknown");
    if (L->menu_ver[0]) snprintf(L->sline[1], sizeof L->sline[1], "Menu core, version tag %s", L->menu_ver);
    else snprintf(L->sline[1], sizeof L->sline[1], "Menu core, version unknown");
    if (L->free_mb == TL_FREE_UNKNOWN) L->free_mb = L->plat && L->plat->free_mb ? L->plat->free_mb(L->plat->ctx) : -1;
    if (L->free_mb < 0) snprintf(L->sline[2], sizeof L->sline[2], "Available space: unknown");
    else if (L->free_mb < 10 * 1024) snprintf(L->sline[2], sizeof L->sline[2], "Available space: %dmb", L->free_mb);
    else snprintf(L->sline[2], sizeof L->sline[2], "Available space: %dgb", L->free_mb / 1024);
    srow_add(L, TR_BLANK, SI_NONE, NULL, 0);
    srow_add(L, TR_CENTER, SI_NONE, L->sline[0], TANGO_C_TEXT);
    srow_add(L, TR_CENTER, SI_NONE, L->sline[1], 0);
    srow_add(L, TR_BLANK, SI_NONE, NULL, 0);
    srow_add(L, TR_CENTER, SI_NONE, "Storage: SD card", 0);
    srow_add(L, TR_CENTER, SI_NONE, L->sline[2], 0);
    srow_add(L, TR_BLANK, SI_NONE, NULL, 0);
    srow_add(L, TR_DIS, SI_REMAP, "Remap keyboard", 0);              /* placeholder */
    srow_add(L, TR_SUB, SI_DEFINE, "Define joystick buttons", 0);    /* the system map's Menu entry (tango_menubtn.h); its other buttons are the fixed DS4 decode */
    srow_add(L, TR_ACT, SI_ABOUT, "About", 0);                       /* Tango's own: the wordmark page, tango_about.h (MiSTer has none here) */
    srow_add(L, TR_BLANK, SI_NONE, NULL, 0);
    srow_add(L, TR_ACT, SI_REBOOT, "Reboot (hold \x7f cold reboot)", 0);
    while (L->nsrow < TL_ROWS - 1) srow_add(L, TR_BLANK, SI_NONE, NULL, 0);
    srow_add(L, TR_ACT, SI_EXIT, "exit", 0);
}

/* MENU_MISC1 (menu.cpp 4030-4098).  Information lists only what the firmware knows.  Config, Boot core and Players are
 * placeholders (alternate inis, boot core, players).  Core Volume and Global Volume (tango_volume.h) when the
 * platform has the state; the launcher's own Core Volume is, as in MiSTer, not saved or loaded (audio.cpp 249-253). */
static void build_misc(tango_launcher_t *L)
{
    L->nsrow = 0;
    srow_add(L, TR_BLANK, SI_NONE, NULL, 0);
    srow_add(L, TR_CENTER, SI_NONE, "Information", TANGO_C_TEXT);
    {   char ip[24] = "";                                     /* the lines the BL616 can really fill (tango_info_lines) */
        if (L->plat && L->plat->ip) L->plat->ip(L->plat->ctx, ip, sizeof ip);
        int n = tango_info_lines(L->sline, L->vs, ip);
        for (int i = 0; i < n; i++) srow_add(L, TR_CENTER, SI_NONE, L->sline[i], 0);
        while (L->nsrow < 5) srow_add(L, TR_BLANK, SI_NONE, NULL, 0);
    }
    srow_add(L, TR_DIS, MI_CONFIG, "Config", 0);
    srow_add(L, TR_BLANK, SI_NONE, NULL, 0);
    if (L->plat && L->plat->vol) {
        const tango_vol_t *v = L->plat->vol;
        tango_vol_text(v, 1, L->vtxt[0]); tango_vol_text(v, 0, L->vtxt[1]);
        srow_add(L, TR_VOL, MI_COREVOL, "Core Volume", 0); L->srow[L->nsrow - 1].value = L->vtxt[0];
        srow_add(L, v->vol & 0x10 ? TR_OPT : TR_VOL, MI_GLOBVOL, "Global Volume", 0); L->srow[L->nsrow - 1].value = L->vtxt[1];
    }
    while (L->nsrow < 10) srow_add(L, TR_BLANK, SI_NONE, NULL, 0);
    srow_add(L, TR_DIS, MI_BOOT, "Boot core", 0);
    srow_add(L, TR_DIS, MI_PLAYERS, "Players", 0);
    while (L->nsrow < TL_ROWS - 1) srow_add(L, TR_BLANK, SI_NONE, NULL, 0);
    srow_add(L, TR_ACT, SI_EXIT, "exit", 0);
    L->srow[L->nsrow - 1].value = "\x7f";                              /* STD_EXIT with OSD_ARROW_RIGHT (menu.cpp 4079) */
}

/* About (tango_about.h): the menu core is "Menu core v<its V tag>"; the last row goes back to System Settings */
static void build_about(tango_launcher_t *L)
{
    L->about_step = tango_about_step(L->about_t0, L->now_ms);
    tango_about_rows(L->srow, L->sline, "Menu core", L->menu_ver, L->fw_id);
    L->nsrow = TANGO_ABOUT_ROWS;
    for (int i = 0; i < TANGO_ABOUT_ROWS; i++) L->sid[i] = SI_NONE;
}

static void build_settings(tango_launcher_t *L)
{
    if (L->mode == TL_SYSTEM) build_system(L); else if (L->mode == TL_ABOUT) build_about(L); else build_misc(L);
    uint8_t *sel = &L->sel[L->mode];
    if (*sel >= L->nsrow) *sel = (uint8_t)(L->nsrow - 1);
    if (!srow_selectable(&L->srow[*sel])) for (int r = 0; r < L->nsrow; r++) if (srow_selectable(&L->srow[r])) { *sel = (uint8_t)r; break; }
}

#define PM(L) ((L)->plat->pmcap((L)->plat->ctx))    /* the dialog's state is the platform's: the firmware shares the core menu's, only one of them runs per boot (heap, build.sh) */
static void set_status(tango_launcher_t *L, const char *key, const char *text);
static void open_page(tango_launcher_t *L, int mode, int id);
/* ---------------------------------------------------------------- Define joystick buttons
 * MENU_JOYSYSMAP (menu.cpp 7527-7552) -> MENU_JOYDIGMAP (4195-4400), the menu core's walk: "Press: RIGHT" ... one button at a time (tango_pmcap_start_sys:
 * directions, then A B X Y L R Select Start; a button is taken when pressed and the prompt moves on when it is let go), "Joystick ID: vvvv:pppp" once a pad has
 * pressed, "User/Space > Undefine" past the first, "Esc > Cancel", "Enter > Finish", the on-screen gamepad flashing the button asked for; then the Menu
 * entry (phase 1: "Press: Menu", "(can use 2-button combo)", 4258-4262, tango_menubtn.h).  It ends in System Settings with the cursor on Define (4376-4383:
 * menustate = MENU_SYSTEM1, menusub 2).  The map is written once, at the end, to config/inputs/input_<vid>_<pid>_v3.map (tango_fw.c l_save_sysmap). */
static void define_rows(tango_launcher_t *L)
{
    memset(L->srow, 0, sizeof L->srow);
    L->nsrow = 13;
    L->srow[3].kind = TR_CENTER; L->srow[3].col = TANGO_C_TEXT; L->srow[3].label = "Press: Menu";
    L->srow[8].kind = TR_CENTER; L->srow[8].label = "Esc \x7f Cancel";
    L->srow[9].kind = TR_CENTER; L->srow[9].label = "Enter \x7f Finish";
    L->srow[12].kind = TR_CENTER; L->srow[12].label = "(can use 2-button combo)";
    if (L->dhave) {
        snprintf(L->sline[0], sizeof L->sline[0], "Joystick ID: %04x:%04x", L->dvid, L->dpid);
        L->srow[5].kind = TR_CENTER; L->srow[5].label = L->sline[0];
    }
    if (L->dtext[0]) { L->srow[7].kind = TR_CENTER; L->srow[7].col = TANGO_C_TEXT; L->srow[7].label = L->dtext; }
    if (L->dnote) { L->srow[10].kind = TR_CENTER; L->srow[10].label = "The d-pad cannot be the menu button"; }
}

/* the learner's state lives from the start of the walk to its end; ending it twice is harmless */
static void define_learn_end(tango_launcher_t *L, int save)
{
    if (!L->dlearn) return;
    L->dlearn = 0;
    if (save) tango_pmcap_learn_finish(PM(L));                      /* a d-pad group left half done is learned direction by direction first */
    if (L->plat->learn_end) L->plat->learn_end(L->plat->ctx, save, PM(L)->dev);
}

static void define_start(tango_launcher_t *L)
{
    uint32_t raw[2] = { 0, 0 };
    L->dhave = L->dnote = 0; L->dtext[0] = 0; L->dph = 0; L->dflash = 0; L->dt0 = L->now_ms; L->dlearn = 0;
    for (int p = 0; p < 2; p++) { uint16_t v, d; if (L->plat->pad_raw) L->plat->pad_raw(L->plat->ctx, p, &v, &d, &raw[p]); }
    tango_pmcap_start_sys(PM(L));
    if (L->plat->learn_begin && L->plat->pad_sample) {
        tango_pmlearn_t *lm = L->plat->learn_begin(L->plat->ctx);
        if (lm) { tango_pmcap_learn(PM(L), lm); L->dlearn = 1; }
    }
    for (int p = 0; p < 2; p++) PM(L)->prev[p] = raw[p];                 /* the A that opened this page is held: its release is not a button */
    L->mode = TL_DEFINE;
}

static void define_end(tango_launcher_t *L, const char *key, const char *text)
{
    define_learn_end(L, 0);                                         /* a cancel (or a timeout): nothing is applied, nothing saved */
    tango_mbl_cancel(&L->ml);
    open_page(L, TL_SYSTEM, SI_DEFINE);
    set_status(L, key, text);
    tango_launcher_redraw(L);
}

/* finish_map_setting(0): the buttons the walk took, and the Menu entry if it was learned (menu_a < 0 = not: the pad keeps Share + Options) */
static void define_save(tango_launcher_t *L, int menu_a, int menu_b)
{
    uint32_t map[TANGO_PM_BTNS]; tango_mb_t m = { 0, 0 }; char nm[56];
    define_learn_end(L, 1);                                         /* the walk's table goes to the FPGA, the .dec file and the SDL line are written; the map below follows what was learned */
    memset(map, 0, sizeof map);
    for (int i = 0; i < 12; i++) map[i] = PM(L)->map[i] & 0xFFFF;
    if (menu_a >= 0) { m.a = (uint8_t)menu_a; m.b = (uint8_t)menu_b; map[TANGO_SYS_BTN_OSD] = TANGO_PM_CODE(menu_a); map[TANGO_SYS_BTN_OSD + 1] = TANGO_PM_CODE(menu_b); }
    int ok = L->plat->save_sysmap && PM(L)->dev >= 0 && L->plat->save_sysmap(L->plat->ctx, PM(L)->vid, PM(L)->pid, map) == 0;
    if (menu_a >= 0) tango_mb_name(&m, nm); else snprintf(nm, sizeof nm, "buttons saved, menu button unchanged");
    define_end(L, ok ? "Joystick map" : "Could not save", ok ? nm : "the system map");
}

static void define_menu_phase(tango_launcher_t *L, const uint32_t raw[2])
{
    L->dph = 1; L->dhave = 1; L->dvid = PM(L)->vid; L->dpid = PM(L)->pid; L->dtext[0] = 0; L->dt0 = L->now_ms;
    tango_mbl_start(&L->ml, raw, L->now_ms);
}

int tango_launcher_capturing(const tango_launcher_t *L) { return L->mode == TL_DEFINE; }

void tango_launcher_page(tango_launcher_t *L, tango_page_t *p)
{
    static tango_legend_t status[1];
    if (L->mode == TL_DEFINE) {
        static const tango_legend_t LEG_DEF[] = { { "Esc", "cancel" }, { "Enter", "finish" } };
        if (L->dph == 0) { tango_pmcap_page(PM(L), p, BANNER, L->now_ms); return; }
        define_rows(L);
        memset(p, 0, sizeof *p);
        p->banner = BANNER; p->title = "Define buttons"; p->rows = L->srow; p->nrows = L->nsrow; p->sel = -1; p->top = 0;
        p->legend = LEG_DEF; p->nlegend = N(LEG_DEF);
        return;
    }
    if (L->mode != TL_CORES) {
        build_settings(L);
        memset(p, 0, sizeof *p);
        p->banner = BANNER; p->rows = L->srow; p->nrows = L->nsrow; p->sel = L->mode == TL_ABOUT ? -1 : L->sel[L->mode]; p->top = -1;   /* About: no cursor */
        if (L->mode == TL_ABOUT) p->scroll = tango_about_pos(L->about_step);
        if (L->mode == TL_SYSTEM) { p->title = "System Settings"; p->arrow = TANGO_ARROW_LEFT; p->legend = LEG_SYS; p->nlegend = N(LEG_SYS); }
        else if (L->mode == TL_ABOUT) { p->title = "About"; p->legend = LEG_ABOUT; p->nlegend = N(LEG_ABOUT); }
        else { p->title = "Misc. Options"; p->arrow = TANGO_ARROW_RIGHT; p->legend = LEG_MISC; p->nlegend = N(LEG_MISC); }
        if (L->status_key[0]) { status[0].key = L->status_key; status[0].label = L->status_text; p->legend = status; p->nlegend = 1; }
        return;
    }
    tango_browser_page(&L->br, p, BANNER, KEYS_BROWSE, (int)(sizeof KEYS_BROWSE / sizeof KEYS_BROWSE[0]));
    if (L->status_key[0]) {                 /* "Loading" <name>... in the legend row */
        status[0].key = L->status_key; status[0].label = L->status_text;
        p->legend = status; p->nlegend = 1;
    }
    /* diagnostic lines after the list: a blank row and one dim row each when all of it fits, else as
     * many rows as fit (the legend cycles through the same numbers, see tango_fw.c) */
    int n = p->nrows, avail = TANGO_VIS - n;
    if (L->ndiag && L->br.n == n && avail > 0) {
        int blank = avail >= L->ndiag + 1, k = L->ndiag < avail - blank ? L->ndiag : avail - blank;
        tango_row_t *r = L->br.rows;
        if (blank) { r[n].kind = TR_BLANK; r[n].col = 0; r[n].label = NULL; r[n].value = NULL; }
        for (int i = 0; i < k; i++) {
            tango_row_t *d = &r[n + blank + i];
            d->kind = TR_INFO; d->col = 0; d->label = L->diag[i]; d->value = NULL;
        }
        p->nrows = n + blank + k;
    }
}

void tango_launcher_diag(tango_launcher_t *L, const char *const *lines, int n)
{
    if (n > 4) n = 4;
    for (int i = 0; i < n; i++) { strncpy(L->diag[i], lines[i], sizeof L->diag[i] - 1); L->diag[i][sizeof L->diag[i] - 1] = 0; }
    L->ndiag = n;
}

void tango_launcher_redraw(tango_launcher_t *L)
{
    tango_page_t p;
    tango_launcher_page(L, &p);
    tango_layout_page(&p, &L->tables);
    tango_osd_show(&L->osd, &L->tables);
}

void tango_launcher_start_at(tango_launcher_t *L, const tango_platform_t *plat, const char *path, int sel)
{
    static const uint8_t PAL[16][3] = TANGO_PAL_INIT;
    L->plat = plat;
    memcpy(L->pal, PAL, sizeof PAL);
    L->status_key[0] = L->status_text[0] = 0;
    L->mode = TL_CORES; L->sel[TL_SYSTEM] = L->sel[TL_MISC] = L->sel[TL_ABOUT] = 0; L->ml.on = 0; L->reboot_held = 0; L->free_mb = TL_FREE_UNKNOWN;
    tango_osd_start(&L->osd, (const uint8_t (*)[3])L->pal);
    tango_browser_scan(&L->br, plat->fs, path);
    if (sel >= 0) tango_browser_select(&L->br, sel);
    tango_launcher_redraw(L);
    tango_osd_enable(&L->osd, 1);
}

void tango_launcher_start(tango_launcher_t *L, const tango_platform_t *plat)
{
    tango_launcher_start_at(L, plat, "", -1);   /* SelectFile("", 0, SCANO_CORES, ...) in the menu core: the root */
}

void tango_launcher_notice(tango_launcher_t *L, const char *key, const char *text)
{
    if (L->mode == TL_DEFINE) return;                                    /* the walk has its own page */
    set_status(L, key, text);
    tango_launcher_redraw(L);
}

static void set_status(tango_launcher_t *L, const char *key, const char *text)
{
    strncpy(L->status_key, key, sizeof L->status_key - 1); L->status_key[sizeof L->status_key - 1] = 0;
    strncpy(L->status_text, text, sizeof L->status_text - 1); L->status_text[sizeof L->status_text - 1] = 0;
}

void tango_launcher_message(tango_launcher_t *L, const char *key, const char *text)
{
    set_status(L, key, text);
    tango_launcher_redraw(L);
}

static int load(tango_launcher_t *L, const tango_dirent_t *e, int via_restart)
{
    char msg[TANGO_PATH_MAX];
    const char *alt = tango_browser_alt(&L->br, e);
    strncpy(msg, alt, sizeof msg - 4); msg[sizeof msg - 4] = 0; strcat(msg, "...");
    set_status(L, via_restart ? "Restarting to load" : "Loading", msg);
    tango_launcher_redraw(L);

    strcpy(L->load_path, "/sd/");
    if (L->br.path[0]) { strncat(L->load_path, L->br.path, TANGO_PATH_MAX - 1); strcat(L->load_path, "/"); }
    strncat(L->load_path, tango_browser_name(&L->br, e), sizeof L->load_path - strlen(L->load_path) - 1);
    int (*fn)(void *, const char *) = via_restart && L->plat->load_core_alt ? L->plat->load_core_alt : L->plat->load_core;
    L->last_load = fn ? fn(L->plat->ctx, L->load_path) : TANGO_LOAD_NOT_FOUND;
    if (L->last_load == TANGO_LOAD_NOT_FOUND) {         /* the launcher is still in the FPGA: say so */
        set_status(L, "Could not load", alt);
        tango_browser_scan(&L->br, L->plat->fs, L->br.path);   /* the card came back from the loader */
        tango_launcher_redraw(L);
    }
    return 1;
}

void tango_launcher_set_info(tango_launcher_t *L, const char *fw_id, const char *conf)
{
    L->fw_id = fw_id;
    L->menu_ver[0] = 0;
    cs_text_t t; cs_entry_t e;
    for (int i = 1; conf && cs_get(conf, i, &t); i++)
        if (cs_parse(t, &e) && e.kind == CS_VERSION) {
            int n = e.name.len < (int)sizeof L->menu_ver - 1 ? e.name.len : (int)sizeof L->menu_ver - 1;
            memcpy(L->menu_ver, e.name.p, (size_t)n); L->menu_ver[n] = 0;
            break;
        }
}

static int find_row(const tango_launcher_t *L, int id)
{
    for (int r = 0; r < L->nsrow; r++) if (L->sid[r] == id) return r;
    return 0;
}

/* Menu in the Cores page: System Settings with the cursor on exit (menu.cpp 1633-1642: menusub 6, cancel = MENU_SYSTEM1) */
static void open_page(tango_launcher_t *L, int mode, int id)
{
    if (mode == TL_MISC && L->mode != TL_MISC) L->vs = L->plat && L->plat->vsync ? L->plat->vsync(L->plat->ctx) : 0;   /* once, as the page opens */
    if (mode == TL_ABOUT) L->about_t0 = L->now_ms;
    L->mode = (uint8_t)mode;
    build_settings(L);
    L->sel[mode] = (uint8_t)find_row(L, id);
}

static void do_reboot(tango_launcher_t *L, int cold)
{
    L->reboot_held = 0;
    set_status(L, cold ? "Cold reboot" : "Reboot", cold ? "from the flash image" : "reloading the menu core");
    tango_launcher_redraw(L);
    L->plat->reboot(L->plat->ctx, cold);
}

int tango_launcher_tick(tango_launcher_t *L, uint32_t now_ms)
{
    L->now_ms = now_ms;
    if (L->mode == TL_DEFINE && L->plat->pad_raw) {                 /* the pads' raw buttons, polled (the launcher reads no REPORT otherwise) */
        int redraw = 0;
        uint32_t rawall[2] = { 0, 0 };
        for (int p = 0; p < 2; p++) {
            uint16_t v = 0, d = 0; uint32_t raw = 0;
            if (!L->plat->pad_raw(L->plat->ctx, p, &v, &d, &raw)) raw = 0;
            rawall[p] = raw;
            if (L->dph == 0) {
                int r = tango_pmcap_raw(PM(L), p, v, d, raw, now_ms);
                if (r == PMC_REDRAW) redraw = 1;
                if (r == PMC_SAVED) { define_learn_end(L, 1); define_menu_phase(L, rawall); tango_launcher_redraw(L); return 1; }   /* the last button: the table is applied, now the Menu entry */
                if (L->dlearn) {                                          /* the learner reads the payload (REPORT2), not the decoded word */
                    pl_sample_t sm; int got = L->plat->pad_sample(L->plat->ctx, p, &sm);
                    int r2 = tango_pmcap_sample(PM(L), p, got ? &sm : NULL, now_ms);
                    if (r2 == PMC_REDRAW) redraw = 1;
                    if (r2 == PMC_SAVED) { define_learn_end(L, 1); define_menu_phase(L, rawall); tango_launcher_redraw(L); return 1; }
                }
                continue;
            }
            if (p != PM(L)->dev) continue;                           /* the Menu entry belongs to the pad the walk defined */
            int r = tango_mbl_raw(&L->ml, p, raw, now_ms);
            if (r == TANGO_MBL_REFUSED) { if (!L->dnote) redraw = 1; L->dnote = 1; }
            if (r == TANGO_MBL_REDRAW) { L->dnote = 0; tango_mbl_text(&L->ml, L->dtext); redraw = 1; }
            if (r == TANGO_MBL_DONE) { define_save(L, L->ml.a, L->ml.b); return 1; }
        }
        if (L->dph == 0) {
            int flash = (int)((now_ms / 100) & 1);                    /* the gamepad flashes every 100 ms (menu.cpp 4300-4325) */
            if (PM(L)->button >= PM_RIGHT && PM(L)->button <= PM_START && flash != L->dflash) redraw = 1;
            L->dflash = (uint8_t)flash;
            if (PM(L)->dev < 0 && now_ms - L->dt0 >= TANGO_MB_LEARN_MS) { define_end(L, "Define buttons", "nothing pressed: unchanged"); return 1; }
        } else if (tango_mbl_tick(&L->ml, now_ms) == TANGO_MBL_GAVE_UP) { define_save(L, -1, -1); return 1; }   /* the walk is kept, the menu button is not */
        if (redraw) { tango_launcher_redraw(L); return 1; }
        return 0;
    }
    if (L->mode == TL_ABOUT && tango_about_step(L->about_t0, now_ms) != L->about_step) { tango_launcher_redraw(L); return 1; }   /* the ticker moves */
    if (L->mode == TL_CORES && L->br.n && tango_browser_scroll_tick(&L->br, now_ms)) { tango_launcher_redraw(L); return 1; }   /* a long name moves */
    if (!L->reboot_held || !tango_reboot_cold(now_ms - L->reboot_t0)) return 0;
    do_reboot(L, 1);
    return 1;
}

int tango_launcher_wait_ms(const tango_launcher_t *L, uint32_t now_ms)
{
    if (L->mode == TL_DEFINE) return L->dph == 0 && PM(L)->button >= PM_RIGHT && PM(L)->button <= PM_START ? 50 : 30;   /* the raw buttons are polled; the gamepad flashes */
    if (L->mode == TL_ABOUT) return tango_about_wait(L->about_t0, now_ms);
    return L->mode == TL_CORES && L->br.n ? tango_browser_scroll_wait(&L->br, now_ms) : -1;
}

int tango_launcher_select_released(tango_launcher_t *L)
{
    if (!L->reboot_held) return 0;
    do_reboot(L, 0);                                    /* released before the cold reboot's 1.3 s: the menu core */
    return 1;
}

int tango_launcher_reboot_pending(const tango_launcher_t *L) { return L->reboot_held; }

/* Back to the Cores list from System Settings / Misc. Options.  MiSTer's menu core reopens it with SelectFile("", 0, SCANO_CORES, ...) (menu.cpp
 * 1641-1642): a fresh scan from the menu core's own folder, the card root (get_rbf_dir() is "" for menu.rbf), nothing typed, the cursor on the first
 * entry -- the place in a subfolder is NOT kept, as in MiSTer. */
static void to_cores(tango_launcher_t *L)
{
    L->mode = TL_CORES;
    L->br.filter_on = 0;
    tango_browser_scan(&L->br, L->plat->fs, "");
}

/* System Settings / Misc. Options (MENU_SYSTEM2 7448-7510, MENU_MISC2 4100-4165).  Returns 1 if something changed. */
static int settings_event(tango_launcher_t *L, tango_ev_t ev)
{
    build_settings(L);
    uint8_t *sel = &L->sel[L->mode];
    int id = L->sid[*sel], placeholder = id == SI_REMAP || id == MI_CONFIG || id == MI_BOOT || id == MI_PLAYERS;
    int dir = ev == TANGO_EV_UP ? -1 : ev == TANGO_EV_DOWN ? 1 : 0;
    if (L->mode == TL_MISC && (id == MI_COREVOL || id == MI_GLOBVOL) && L->plat->vol) {   /* menu.cpp 4116-4126 */
        int cmd = (ev == TANGO_EV_LEFT || ev == TANGO_EV_MINUS) ? -1 : (ev == TANGO_EV_RIGHT || ev == TANGO_EV_PLUS) ? 1 :
                  (ev == TANGO_EV_SELECT && id == MI_GLOBVOL) ? 0 : 2;        /* Select mutes the global volume only */
        if (cmd != 2) {
            if (id == MI_COREVOL) tango_vol_core_step(L->plat->vol, cmd, L->now_ms);
            else tango_vol_global_step(L->plat->vol, cmd, L->now_ms);
            if (L->plat->vol_apply) L->plat->vol_apply(L->plat->ctx, id == MI_COREVOL);
            return 1;
        }
    }
    if (dir) {
        for (int k = 0; k < L->nsrow; k++) {
            int r = (*sel + dir * (k + 1) % L->nsrow + L->nsrow) % L->nsrow;
            if (srow_selectable(&L->srow[r])) { *sel = (uint8_t)r; break; }
        }
        return 1;
    }
    switch (ev) {
    case TANGO_EV_MENU: case TANGO_EV_BACK:           /* System: the Cores list; Misc: close = NONE1, which reopens as Cores */
        if (L->mode == TL_ABOUT) { open_page(L, TL_SYSTEM, SI_ABOUT); return 1; }     /* About: back to System Settings, cursor on About */
        to_cores(L);
        return 1;
    case TANGO_EV_LEFT:
        if (L->mode == TL_ABOUT) { open_page(L, TL_SYSTEM, SI_ABOUT); return 1; }
        if (L->mode == TL_SYSTEM) { open_page(L, TL_MISC, SI_EXIT); return 1; }   /* menusub = 3: exit (menu.cpp 4040-4046) */
        break;
    case TANGO_EV_RIGHT:
        if (L->mode == TL_MISC && !placeholder) { open_page(L, TL_SYSTEM, SI_DEFINE); return 1; }   /* "go back to the core requesting this menu" */
        break;
    case TANGO_EV_SELECT:
        if (id == SI_EXIT) { to_cores(L); return 1; }
        if (id == SI_ABOUT) { open_page(L, TL_ABOUT, SI_NONE); return 1; }
        if (id == SI_DEFINE && !L->plat->pmcap) { set_status(L, "Not available yet", L->srow[*sel].label); return 1; }
        if (id == SI_DEFINE) { define_start(L); return 1; }
        if (L->mode == TL_ABOUT) { open_page(L, TL_SYSTEM, SI_ABOUT); return 1; }               /* About has no cursor: Select goes back too */
        if (id == SI_REBOOT && L->plat->reboot) { L->reboot_held = 1; L->reboot_t0 = L->now_ms; return 0; }
        if (id == SI_REBOOT || placeholder) { set_status(L, "Not available yet", id == SI_REBOOT ? "Reboot" : L->srow[*sel].label); return 1; }
        return 0;
    default:
        break;
    }
    if (L->mode == TL_MISC && placeholder && (ev == TANGO_EV_LEFT || ev == TANGO_EV_RIGHT || ev == TANGO_EV_MINUS || ev == TANGO_EV_PLUS)) {
        set_status(L, "Not available yet", L->srow[*sel].label);    /* a row that will take Left / Right says so, too */
        return 1;
    }
    return 0;
}

int tango_launcher_event(tango_launcher_t *L, tango_ev_t ev)
{
    tango_browser_t *b = &L->br;
    const tango_dirent_t *e;
    int had_status = L->status_key[0] != 0, changed = 1;
    if (ev == TANGO_EV_NONE) return 0;
    if (ev == TANGO_EV_SPACE && L->mode != TL_DEFINE) ev = TANGO_EV_SELECT;      /* Space is Select (menu.cpp 1405) */
    L->reboot_held = 0;                                 /* any other key ends a Reboot press (its own SELECT sets it below) */
    if (had_status) set_status(L, "", "");              /* any key clears a message */
    if (L->mode == TL_DEFINE) {                         /* the pad is read raw; the keyboard drives Esc / Enter / Space (input.cpp 2991, menu.cpp 4366-4385) */
        if (ev == TANGO_EV_BACK || ev == TANGO_EV_MENU) { define_end(L, "Define buttons", "cancelled: nothing saved"); return 1; }
        if (L->dph == 0) {
            int r = ev == TANGO_EV_SELECT ? tango_pmcap_key(PM(L), PMC_KEY_ENTER) : ev == TANGO_EV_SPACE ? tango_pmcap_key(PM(L), PMC_KEY_SPACE) : PMC_NONE;
            if (r == PMC_SAVED) {
                if (ev == TANGO_EV_SELECT) { define_save(L, -1, -1); return 1; }        /* Enter: finish_map_setting(0) with what was taken */
                uint32_t raw[2] = { PM(L)->prev[0], PM(L)->prev[1] };
                define_menu_phase(L, raw);                                                /* Space past the last button */
            }
            if (r != PMC_NONE || ev == TANGO_EV_NONE) tango_launcher_redraw(L);
        } else if (ev == TANGO_EV_SELECT || ev == TANGO_EV_SPACE) define_save(L, -1, -1);
        return 1;
    }
    if (L->mode != TL_CORES) {
        changed = settings_event(L, ev);
        if (!changed && !had_status) return 0;
        tango_launcher_redraw(L);
        return 1;
    }
    switch (ev) {
    case TANGO_EV_MENU: open_page(L, TL_SYSTEM, SI_EXIT); break;
    case TANGO_EV_UP:    tango_browser_prev(b); break;
    case TANGO_EV_DOWN:  tango_browser_next(b); break;
    case TANGO_EV_LEFT:
    case TANGO_EV_PGUP:  tango_browser_prev_page(b); break;
    case TANGO_EV_RIGHT:
    case TANGO_EV_PGDN:  tango_browser_next_page(b); break;
    case TANGO_EV_BACK:                                 /* a typed filter first; else up one folder; at the root MiSTer's B (= its Menu key)
                                                         * opens System Settings */
        if (b->filtered) tango_browser_clear_filter(b, L->plat->fs);
        else if (!b->path[0]) open_page(L, TL_SYSTEM, SI_EXIT);
        else changed = tango_browser_chdir(b, L->plat->fs, "..");
        break;
    case TANGO_EV_BACKSPACE: tango_browser_clear_filter(b, L->plat->fs); break;    /* A + B: the filter goes, the list starts at the top */
    case TANGO_EV_HOME:  tango_browser_home(b, L->plat->fs); break;
    case TANGO_EV_END:   tango_browser_end(b); break;
    case TANGO_EV_TAB:   tango_browser_tab(b, L->plat->fs); break;                  /* up a folder; at the root: the top */
    case TANGO_EV_MINUS: tango_browser_prev_char(b); break;
    case TANGO_EV_PLUS:  tango_browser_next_char(b); break;
    case TANGO_EV_RECENT: changed = 0; break;                                       /* Recent Cores: no page yet (the legend does not offer it) */
    case TANGO_EV_ALT_SELECT:
        if (!(e = tango_browser_cur(b)) || e->is_dir || !L->plat->load_core_alt) { changed = 0; break; }
        return load(L, e, 1);
    case TANGO_EV_SELECT:
        if (!(e = tango_browser_cur(b))) { changed = 0; break; }
        if (e->is_dir) {
            char name[TANGO_PATH_MAX];
            strncpy(name, tango_browser_name(b, e), sizeof name - 1); name[sizeof name - 1] = 0;
            tango_browser_chdir(b, L->plat->fs, name);
            break;
        }
        return load(L, e, 0);
    default:
        changed = 0;
        break;
    }
    if (!changed && !had_status) return 0;
    tango_launcher_redraw(L);
    return 1;
}

int tango_launcher_type(tango_launcher_t *L, char ch, uint32_t now_ms)
{
    if (!tango_browser_type(&L->br, L->plat->fs, ch, now_ms)) return 0;
    if (L->status_key[0]) set_status(L, "", "");
    tango_launcher_redraw(L);
    return 1;
}
