// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 the Tango authors
/*
 * tango_fw.c -- see tango_fw.h. Tango, our own code; it calls FPGA Companion's
 * (Apache-2.0) SPI, SD and JTAG functions and is compiled into the Companion fork.
 *
 * Boot (menu-tango docs/LAUNCHER.md "Boot"): FPGA Companion never loads a core at boot
 * (its only loader is the Shift+F12 file selector, sdc.c 864 -> mcu_hw_upload_core). Tango adds: at
 * power-on the BL616 loads /menu.fs (else /menu.bin) over JTAG, as MiSTer loads menu.rbf (file_io.cpp 1183).
 * If neither file exists the FPGA keeps the image it booted from its own flash; if an upload breaks off,
 * the FPGA is told to reconfigure from that flash image (mcu_hw_fpga_reconfig, gowin.c GOWIN_COMMAND_RECONFIG).
 *
 * Power-on vs our own reset: FPGA Companion resets the MCU (watchdog, mcu_hw_reset) whenever the FPGA
 * reports a cold boot, e.g. after a core was loaded. That reset must not load /menu.fs again, so
 * mcu_hw_reset leaves a marker in the BL616's HBN RAM (always-on domain) and tango_fw_boot consumes it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <FreeRTOS.h>
#include <task.h>
#include <queue.h>
#include <semphr.h>
#include <ff.h>

#include "mcu_hw.h"
#include "sysctrl.h"
#include "osd.h"
#include "menu.h"
#include "debug.h"
#include "sdc_direct.h"
#ifndef TANGO_COSIM
#include "bflb_mtimer.h"
#endif
#include "bl616_glb.h"

#include "gowin.h"
#include "tango_fw.h"
#include "tango_launcher.h"
#include "tango_coremenu.h"
#include "tango_save.h"
#include "tango_zip.h"
#include "tango_mra.h"
#include "tango_kbd.h"
#include "tango_vproc.h"
#include "tango_volume.h"
#include "tango_padmgr.h"
#include "tango_padlearn.h"
#include "tango_paddb_builtin.h"
#include "tango_padres.h"
#include "ps2helper.h"

/* The firmware's build id, shown on System > About (tango_about.h) and in the launcher's System Settings: build.sh (mk_fwid.sh)
 * writes tango_fwid.h next to this file in its scratch build tree, a single line
 *     #define TANGO_FW_ID "g<git short hash>[-dirty] <date> <time>"
 * Never in the repo: a host build that compiles this file straight from it finds no
 * header, and gets no id -- About then says "Tango FPGA (build unknown)", as it does for a build made without build.sh. */
#if defined(__has_include)
#  if __has_include("tango_fwid.h")
#    include "tango_fwid.h"
#  endif
#endif
#ifndef TANGO_FW_ID
#  define TANGO_FW_ID NULL
#endif

#define tango_debugf(a, ...) debugf("\033[1;33mTANGO: " a "\033[0m", ##__VA_ARGS__)

/* ------------------------------------------------------------------ HBN RAM: state across MCU resets */
/* HBN RAM is 0x20010000..0x20010FFF (bl616_common.ld.in "hbn_memory", 4 KB); the SDK places .hbn_code/.hbn_data
 * at its start (0x80 bytes in this build), and 0xE00 is SYS_CLOCK_CFG_ADDR (bl616_clock.h 160:
 * 0x20010000 + 4*1024 - 512, the boot code's clock configuration, rewritten at every boot: records kept
 * there are lost across the restart, and writing it can corrupt that configuration).
 * Tango uses only the top 128 bytes, which survive a restart on hardware (the note and the marker do):
 *   0xF80 note, or a launch-request flag (X) -- never both   0xFC0 the failed load's numbers   0xFF8 marker
 * Every block starts with a magic, so random power-on contents do not read as valid.
 *
 * The launch request's PATH is not kept in this block: what is left before the numbers block is far
 * too little for a real library (deep MiSTer-style folders, e.g. "_Arcade/_alternatives/<set>/<name>.mra",
 * run past 160 characters; even reclaiming the numbers block's own 56 bytes caps out at ~116, still
 * short). The path goes onto a small SD file (/sd/menu.req) and only the flag stays here -- this adds
 * an SD-mount dependency to a boot-time path (tango_fw_boot runs before sys_wait4fpga, so the card
 * isn't mounted yet; take_request does the same on-demand card_take() dance resolve_mra() needs for
 * .mra resolution a few lines later), in exchange for the same TANGO_PATH_MAX (256) the rest of the
 * browser uses, so every real path fits. */
#ifndef TANGO_HBN_BASE   /* a host co-simulation build points it at host memory */
#define TANGO_HBN_BASE 0x20010000UL
#endif
#define HBN(off) ((volatile uint32_t *)((uintptr_t)(TANGO_HBN_BASE) + (off)))
#define TANGO_MARK_MAGIC 0x54414E47UL   /* 'TANG' */
#define TANGO_NOTE_MAGIC 0x4E4F5445UL   /* 'NOTE' */
#define TANGO_DIAG_MAGIC 0x44494147UL   /* 'DIAG' */
#define TANGO_REQ_MAGIC  0x4C4F4144UL   /* 'LOAD' */
static volatile uint32_t *const mark = HBN(0xFF8);
static volatile uint32_t *const note_mem = HBN(0xF80);
static volatile uint32_t *const nums_mem = HBN(0xFC0);   /* 14 words, up to the marker */
static volatile uint32_t *const req_mem = HBN(0xF80);    /* shares the note block (different magic) */
#define TANGO_NUMS_MAGIC 0x4E554D53UL   /* 'NUMS' */
static bool force_cold = false;

void tango_fw_mark_reset(void)
{
    if (force_cold) { mark[0] = 0; mark[1] = 0; return; }
    mark[0] = TANGO_MARK_MAGIC; mark[1] = ~TANGO_MARK_MAGIC;
}

/* Why the last launch failed, kept across the cold restart that follows it: shown in the launcher's
 * legend row after the restart.
 * [0] magic [1] result [2] status after the write [3] after the flash reload [4] after the erase
 * [5..15] the core's name (43 characters) */
#define NOTE_TEXT_MAX 43
static void save_note(int result, const uint32_t st[3], const char *what)
{
    note_mem[1] = (uint32_t)result;
    for (int k = 0; k < 3; k++) note_mem[2 + k] = st ? st[k] : 0;
    volatile char *t = (volatile char *)&note_mem[5];
    int i = 0; for (; what[i] && i < NOTE_TEXT_MAX; i++) t[i] = what[i]; t[i] = 0;
    note_mem[0] = TANGO_NOTE_MAGIC;
}
static bool take_note(int *result, uint32_t st[3], char *what, int cap)
{
    if (note_mem[0] != TANGO_NOTE_MAGIC) return false;
    note_mem[0] = 0;
    *result = (int)note_mem[1];
    for (int k = 0; k < 3; k++) st[k] = note_mem[2 + k];
    volatile char *t = (volatile char *)&note_mem[5];
    int i = 0; for (; i < cap - 1 && i < NOTE_TEXT_MAX && t[i]; i++) what[i] = t[i]; what[i] = 0;
    return true;
}

/* Load records (on hardware: the power-up load works, a launch from the menu does not).
 * The power-up load of this boot is kept in RAM; the failed launch's numbers survive the restart in
 * nums_mem: [0] magic [1] result [2] IDCODE [3] bytes on TDI [4] crc32 of them [5] ms [6] longest chunk
 * gap us [7] status after the reload [8] after the erase [9] after the write [10] after 4 KB
 * [11] after 64 KB [12] how: 0 power-up, 1 launch from the menu (Tango task), 2 launch through a restart (X) */
enum { HOW_POWERUP = 0, HOW_TASK = 1, HOW_RESTART = 2 };
#define REC_WORDS 13
static uint32_t rec_pwr[REC_WORDS];                  /* this boot's power-up load */
static void make_record(uint32_t *d, int result, int how, uint32_t st4k, uint32_t st64k)
{
    d[1] = (uint32_t)result; d[2] = gowin_diag.idcode; d[3] = gowin_diag.bytes; d[4] = gowin_diag.crc;
    d[5] = gowin_diag.ms; d[6] = gowin_diag.max_gap_us;
    d[7] = gowin_status_reload; d[8] = gowin_status_erase; d[9] = gowin_last_status;
    d[10] = st4k; d[11] = st64k; d[12] = (uint32_t)how;
    d[0] = TANGO_DIAG_MAGIC;
    tango_debugf("load record (%s): result %d id %08lx %lu bytes crc32 %08lx %lu ms gap %lu us R %08lx E %08lx W %08lx 4K %08lx 64K %08lx",
                 how == HOW_POWERUP ? "power-up" : how == HOW_TASK ? "launch" : "launch via restart", result,
                 (unsigned long)d[2], (unsigned long)d[3], (unsigned long)d[4], (unsigned long)d[5], (unsigned long)d[6],
                 (unsigned long)d[7], (unsigned long)d[8], (unsigned long)d[9], (unsigned long)d[10], (unsigned long)d[11]);
}
static void save_nums(const uint32_t *d)
{
    for (int k = 1; k < REC_WORDS; k++) nums_mem[k] = d[k];
    nums_mem[0] = TANGO_NUMS_MAGIC;
}
static bool take_nums(uint32_t *d)
{
    if (nums_mem[0] != TANGO_NUMS_MAGIC) return false;
    nums_mem[0] = 0;
    d[0] = TANGO_DIAG_MAGIC;
    for (int k = 1; k < REC_WORDS; k++) d[k] = nums_mem[k];
    return true;
}
static const char *result_text(int r)
{
    return r == TANGO_LOAD_OK ? "DONE" : r == TANGO_LOAD_NOT_DONE ? "not DONE" : r == TANGO_LOAD_FAILED ? "failed" : "no file";
}
static const char *how_text(int how)
{
    return how == HOW_POWERUP ? "Power-up" : how == HOW_TASK ? "Launch" : "Launch (restart)";
}
/* two lines per record, for the launcher's diagnostic rows */
static int diag_lines(const uint32_t *d, char (*out)[96])
{
    if (d[0] != TANGO_DIAG_MAGIC) return 0;
    int how = (int)d[12];
    snprintf(out[0], 96, "%s: id %08lX  %lu B  crc32 %08lX  %lu ms  gap %lu us", how_text(how),
             (unsigned long)d[2], (unsigned long)d[3], (unsigned long)d[4], (unsigned long)d[5], (unsigned long)d[6]);
    if (how == HOW_POWERUP)
        snprintf(out[1], 96, "    %s  E %08lX  W %08lX", result_text((int)d[1]), (unsigned long)d[8], (unsigned long)d[9]);
    else
        snprintf(out[1], 96, "    %s  R %08lX E %08lX W %08lX 4K %08lX 64K %08lX", result_text((int)d[1]),
                 (unsigned long)d[7], (unsigned long)d[8], (unsigned long)d[9], (unsigned long)d[10], (unsigned long)d[11]);
    return 2;
}
/* one legend line per record (the legend cycles through them) */
static void legend_line(const uint32_t *d, char *key, int kcap, char *text, int tcap)
{
    int how = (int)d[12];
    snprintf(key, (size_t)kcap, "%s", how == HOW_POWERUP ? "Power-up" : how == HOW_TASK ? "Launch" : "Restart");
    if (how == HOW_POWERUP)
        snprintf(text, (size_t)tcap, "%lu B  crc32 %08lX  gap %lu us  W %08lX", (unsigned long)d[3], (unsigned long)d[4],
                 (unsigned long)d[6], (unsigned long)d[9]);
    else
        snprintf(text, (size_t)tcap, "%lu B  crc32 %08lX  gap %lu us  4K %08lX  64K %08lX", (unsigned long)d[3],
                 (unsigned long)d[4], (unsigned long)d[6], (unsigned long)d[10], (unsigned long)d[11]);
}

/* X: the next boot loads this core the way the power-up load does (com_task, before sys_wait4fpga).
 * The path itself lives in /sd/menu.req (see the HBN-layout comment above), not in HBN -- REQ_PATH_MAX
 * is a buffer-size cap, matching TANGO_PATH_MAX everywhere else, not an HBN-constrained size. */
#define REQ_PATH_MAX TANGO_PATH_MAX
#define REQ_MENU "@menu"                  /* the in-core OSD's Reboot: the menu core, as the power-up load */
#define REQ_COLD "@cold"                  /* Reboot held: the FPGA's flash image, then the power-up load */
#define REQ_FILE "/sd/menu.req"
static void card_take(void);              /* forward declarations: defined below, after the card/FatFs plumbing */
static void card_release(void);
static bool card_ok;                      /* tentative declaration; the real one (below) adds "= false" */

/* Set by tango_fw_boot's own have_req success path when the original request
 * was an .mra, consumed once by core_task() -- never acted on here. tango_fw_boot runs well before
 * core_task() exists (com_task calls it before sys_wait4fpga/tango_fw_try_core even run) and has no card
 * mount of its own at this point to open the .mra with; core_task() calls card_take() at its own start and
 * keeps the card mounted for its whole session, which mra_autostart() (tango_fw.c, after resolve_mra())
 * needs and relies on -- see its own header comment for why the open is not done here with the mount
 * handed across. "" = nothing pending (the common case: no .mra, or a plain core). */
static char mra_pending[REQ_PATH_MAX];
static void save_request(const char *path)
{
    /* the card is already mounted here (called only from the launcher task, which holds it) */
    FIL f; UINT bw = 0;
    bool ok = f_open(&f, REQ_FILE, FA_CREATE_ALWAYS | FA_WRITE) == FR_OK;
    if (ok) { FRESULT r = f_write(&f, path, (UINT)strlen(path), &bw); f_close(&f); ok = r == FR_OK && bw == strlen(path); }
    if (ok) req_mem[0] = TANGO_REQ_MAGIC;   /* only flag a request the file write actually backs */
    tango_debugf("save_request(%s): %s", path, ok ? "ok" : "failed to write " REQ_FILE);
}
static bool take_request(char *path, int cap)
{
    if (req_mem[0] != TANGO_REQ_MAGIC) return false;
    req_mem[0] = 0;
    /* this early in boot (before sys_wait4fpga) the card is not mounted yet -- mount it just for this,
     * the same on-demand dance resolve_mra() uses a few lines later for .mra resolution */
    card_take();
    bool ok = false;
    if (card_ok) {
        FIL f; UINT br = 0;
        if (f_open(&f, REQ_FILE, FA_OPEN_EXISTING | FA_READ) == FR_OK) {
            FRESULT r = f_read(&f, path, (UINT)(cap - 1), &br);
            f_close(&f);
            f_unlink(REQ_FILE);
            if (r == FR_OK && br > 0) { path[br] = 0; ok = true; }
        }
    }
    card_release();
    tango_debugf("take_request: %s", ok ? path : "flagged, but " REQ_FILE " could not be read");
    return ok;
}

#define NOTE_IN_PROGRESS (-9)            /* a launch started; the MCU was reset before it returned */
static bool booted_warm = false;
static char running_core[TANGO_PATH_MAX];   /* the launched core's file, relative to the card root (c_running_core) */
static bool launched = false;            /* this boot loaded a core for the launcher (A/X): its menu opens (core_task) */

static bool consume_warm_mark(void)
{
    bool warm = mark[0] == TANGO_MARK_MAGIC && mark[1] == (uint32_t)~TANGO_MARK_MAGIC;
    mark[0] = 0; mark[1] = 0;
    return warm;
}

uint64_t tango_time_us(void)             /* for gowin.c's chunk-gap measurement */
{
#ifdef TANGO_COSIM
    extern uint64_t cosim_time_us(void);
    return cosim_time_us();
#else
    return bflb_mtimer_get_time_us();
#endif
}

/* after a failed load: what the FPGA saw of the start of the file, then the flash image again */
static void probe_after_failure(const char *path, uint32_t *st4k, uint32_t *st64k)
{
    *st4k = *st64k = 0;
    f_mount(NULL, "/sd", 1);
    sdc_direct_init();
    static FATFS pfs;
    TickType_t t0 = xTaskGetTickCount();
    FRESULT res;
    while ((res = f_mount(&pfs, "/sd", 1)) != FR_OK && (xTaskGetTickCount() - t0) < pdMS_TO_TICKS(500))
        vTaskDelay(pdMS_TO_TICKS(100));
    if (res == FR_OK) {
        gowin_probe_preamble(path, st4k, st64k);
        f_mount(NULL, "/sd", 1);
    } else gowin_restore_flash_image();
    sdc_direct_release();
    tango_debugf("probe: status %08lx after 4 KB, %08lx after 64 KB (bit 6 = preamble seen)",
                 (unsigned long)*st4k, (unsigned long)*st64k);
}

/* Resolve an .mra launch request to a bitstream path under _Arcade/cores/. Defined
 * below, after the card/zip plumbing (card_take/card_release, zipfs) it needs; forward-declared here
 * so tango_fw_boot can call it. */
static int resolve_mra(const char *req, char *out, int cap);

void tango_fw_boot(void)
{
    GLB_RESET_RECORD_Type rr; memset(&rr, 0, sizeof rr);
    GLB_Get_Reset_Reason(&rr);
    bool warm = consume_warm_mark();
    booted_warm = warm;
    tango_debugf("reset record: por %d ext %d wdt %d sys %d cpu_req %d; marker %s",
                 rr.reset_recorder_ana_powb, rr.reset_recorder_ext_rst_n, rr.reset_recorder_wdt_rst_n,
                 rr.reset_recorder_sys_reset_n, rr.reset_recorder_cpu_sys_rstreq_n, warm ? "warm" : "none");
    char req[REQ_PATH_MAX] = "";
    bool reload = false;
    bool have_req = take_request(req, sizeof req);
    if (have_req && !strcmp(req, REQ_MENU)) {         /* in-core OSD, System > Reboot (short): the menu core */
        tango_debugf("reboot request: loading the menu core (MiSTer fpga_load_rbf(\"menu.rbf\"), menu.cpp 3172)");
        have_req = false; reload = true;              /* the FPGA runs a core: start from its flash image */
    } else if (have_req && !strcmp(req, REQ_COLD)) {  /* System > Reboot held: MiSTer reboot(1), menu.cpp 3116 */
        tango_debugf("cold reboot request: the FPGA reloads its flash image, then the power-on load");
        gowin_restore_flash_image();
        have_req = false;
    }
    if (have_req) {                                   /* A/X: load this core exactly as the power-up load does */
        tango_debugf("launch request from the menu: %s (loaded before sys_wait4fpga, in com_task)", req);
        const char *base = strrchr(req, '/');
        char what[NOTE_TEXT_MAX + 1], date[16];
        tango_display_name(base ? base + 1 : req, what, sizeof what, date);

        /* An .mra request names no bitstream directly -- resolve its <rbf> to a core
         * under _Arcade/cores/ first (MiSTer's xml_load()/get_rbf(), mra_loader.cpp 1207-1230), then
         * load that instead. A resolution failure is treated exactly like an upload that never found
         * its file (TANGO_LOAD_NOT_FOUND): no probe, straight to the existing failure reporting below. */
        char resolved[REQ_PATH_MAX];
        const char *ext = strrchr(req, '.'), *upload_path = req;
        int r = -1; bool have_r = false;
        if (ext && !strcasecmp(ext, ".mra")) {
            if (resolve_mra(req, resolved, sizeof resolved)) upload_path = resolved;
            else { r = TANGO_LOAD_NOT_FOUND; have_r = true; tango_debugf("mra: could not resolve a core for %s", req); }
        }
        uint32_t st4k = 0, st64k = 0, rec[REC_WORDS];
        if (!have_r) {
            gowin_reload_first = true;                /* the FPGA runs the menu core: start from its flash image */
            r = mcu_hw_upload_core_result(upload_path);
            gowin_reload_first = false;
            if (r == TANGO_LOAD_NOT_DONE) probe_after_failure(upload_path, &st4k, &st64k);
        }
        make_record(rec, r, HOW_RESTART, st4k, st64k);
        if (r == TANGO_LOAD_OK) {                    /* the core runs */
            /* a core with the link answers SYS STATUS at once (the Atari 2600 core: first poll); one without
             * it (a core that uses the BL616 UART pin) never does: then GPIO28/V14 goes to the UART idle
             * level (mcu_hw_jtagsel_idle_high).
             *
             * A large core can take longer than 1 s to answer: on hardware, a Galaga (Midway, Set 1) .mra
             * launch with a 1 s window came up in Galaga's own OSD with the game screen black and
             * "Load ROM *.BIN" still offered -- this loop had fallen through to the SYS-STATUS-timeout
             * branch below, so mra_pending was never set (and launched stayed false). sysctrl.c's own
             * sys_wait4fpga() polls the exact same sys_status_is_valid() check after this function
             * returns, with FPGA_BOOT_TIMEOUT = 10000 ms (commented "prolonged for GW5AST138K" in FPGA
             * Companion): the time this board's FPGA can take to answer SYS STATUS after a load. This
             * window uses the same number (SYS_STATUS_TIMEOUT_MS below), so this launch path does not
             * give up sooner than sys_wait4fpga() does for the identical check. */
#define SYS_STATUS_TIMEOUT_MS 10000  /* sysctrl.c's FPGA_BOOT_TIMEOUT: the same board, the same check */
#define SYS_STATUS_POLL_MS    20
            for (int t = 0; t < SYS_STATUS_TIMEOUT_MS / SYS_STATUS_POLL_MS; t++) {
                if (sys_status_is_valid()) {
                    tango_debugf("the new core answers SYS STATUS");
                    /* MiSTer's xml_load()/arcade_send_rom() equivalent. Only
                     * RECORD the original .mra's own path here (req, not upload_path -- resolve_mra's own
                     * output is the bitstream path, already consumed above): mra_autostart() itself needs
                     * a mounted card, and tango_fw_boot has none to offer at this point (it runs well
                     * before core_task() exists -- see mra_autostart's own header comment, and mra_pending's,
                     * for why the actual open+tango_dl_begin work happens there instead, after core_task()'s
                     * own card_take()). A plain restart-launch (ext not ".mra") leaves mra_pending empty. */
                    if (ext && !strcasecmp(ext, ".mra")) {
                        strncpy(mra_pending, req, sizeof mra_pending - 1);
                        mra_pending[sizeof mra_pending - 1] = 0;
                    }
                    launched = true;
                    strncpy(running_core, strncmp(req, "/sd/", 4) ? req : req + 4, sizeof running_core - 1);   /* System > Core opens in its folder */
                    return;
                }
                vTaskDelay(pdMS_TO_TICKS(SYS_STATUS_POLL_MS));
            }
            tango_debugf("no SYS STATUS answer in %d s: a core without the link; V14 (GPIO28) set to the UART idle level (high)",
                         SYS_STATUS_TIMEOUT_MS / 1000);
            mcu_hw_jtagsel_idle_high();
            return;
        }
#undef SYS_STATUS_TIMEOUT_MS
#undef SYS_STATUS_POLL_MS
        uint32_t st[3] = { rec[9], rec[7], rec[8] };
        save_note(r, st, what);
        save_nums(rec);
        /* TANGO_LOAD_FAILED (-2)
         * and TANGO_LOAD_NOT_DONE (-3) drive the IDENTICAL visible symptom here (fall back to
         * /menu.fs); a bare "(%d)" in the log tells the two apart only to
         * someone willing to decode the numeric result by hand against tango_launcher.h. This is also
         * the one summary line most likely to be read first after a failed launch (and the actual live
         * path -- load_core_restart() is what A/X use, not the TANGO_LAUNCH_IN_TASK-gated load_core()
         * above, which has a two-way version of this same distinction). It spells out
         * which of the two happened:
         *   -2 is NOT always "mid-transfer" -- gowin_upload_core_bin() returns false for several
         *   different reasons (an f_stat/f_open/FPGA-not-detected problem before any JTAG write, either
         *   SRAM erase failing, or an actual mid-transfer SD-read/JTAG failure), all collapsed to the
         *   same bool/-2 by gowin_upload_core(). gowin_fail_step (gowin.c) is set at
         *   every one of those return points specifically so this line does not have to guess -- it is
         *   only meaningful immediately after a -2, which is exactly this branch.
         *   -3 means the download itself DID complete in full (gowin_upload_core_bin() only reaches
         *   gowin_writeSRAM_postproc()/returns true past the point where an incomplete transfer would
         *   already have returned false/-2) -- DONE_FINAL simply never came back set afterward, read
         *   from the status words already captured just above (rec[7] after the flash reload, rec[8]
         *   after the erase, rec[9]/gowin_last_status after the write; st4k/st64k from
         *   probe_after_failure a few lines above, the only probe that ever runs for this result).
         * Observability only -- reload/control flow unchanged. */
        if (r == TANGO_LOAD_FAILED)
            tango_debugf("launch through a restart failed: %s (status R %08lx E %08lx) -- loading the "
                         "launcher again", gowin_fail_step, (unsigned long)rec[7], (unsigned long)rec[8]);
        else if (r == TANGO_LOAD_NOT_DONE)
            tango_debugf("launch through a restart failed: uploaded in full, but DONE_FINAL never set "
                         "-- a genuine FPGA config failure (status %08lx after write, %08lx after 4 KB, "
                         "%08lx after 64 KB) -- loading the launcher again",
                         (unsigned long)rec[9], (unsigned long)st4k, (unsigned long)st64k);
        else if (r == TANGO_LOAD_NOT_FOUND)
            tango_debugf("launch through a restart failed: nothing was sent to the FPGA (%s, or the SD "
                         "card itself -- see the line just above) -- loading the launcher again", upload_path);
        else
            tango_debugf("launch through a restart failed (%d): loading the launcher again", r);
        reload = true;                               /* whatever the FPGA holds now: start from the flash image */
    } else if (warm && !reload && strcmp(req, REQ_COLD)) {
        tango_debugf("our own MCU reset: keeping the core the FPGA runs");
        return;
    }
    tango_debugf("power-on: loading /menu.fs from the SD card");
    gowin_reload_first = reload;
    int r = mcu_hw_upload_core_result("/sd/menu.fs");
    if (r == TANGO_LOAD_NOT_FOUND) {
        tango_debugf("no /menu.fs, trying /menu.bin");
        r = mcu_hw_upload_core_result("/sd/menu.bin");
    }
    gowin_reload_first = false;
    if (r != TANGO_LOAD_NOT_FOUND) make_record(rec_pwr, r, HOW_POWERUP, 0, 0);
    if (r == TANGO_LOAD_OK) tango_debugf("launcher core loaded");
    else if (r == TANGO_LOAD_NOT_DONE)      /* the proven path: keep going, but say so on the log */
        tango_debugf("launcher core uploaded, but DONE_FINAL is not set (status %08lx)", (unsigned long)gowin_last_status);
    else if (r == TANGO_LOAD_NOT_FOUND) tango_debugf("no /menu.fs or /menu.bin: the FPGA keeps its flash image");
    else {
        tango_debugf("launcher upload failed: the FPGA reloads its flash image");
        mcu_hw_fpga_reconfig(false);
    }
}

/* ------------------------------------------------------------------ SPI link */
static void l_begin(void *c) { (void)c; mcu_hw_spi_begin(); }
static uint8_t l_xfer(void *c, uint8_t b) { (void)c; return mcu_hw_spi_tx_u08(b); }
static void l_end(void *c) { (void)c; mcu_hw_spi_end(); }
static void l_delay(void *c, unsigned ms) { (void)c; vTaskDelay(pdMS_TO_TICKS(ms ? ms : 1)); }
static const tango_link_t link = { l_begin, l_xfer, l_end, l_delay, NULL };

/* ------------------------------------------------------------------ the SD card, taken from the FPGA */
static FATFS card_fs;
static bool card_ok = false;
static DIR card_dir;

static void card_take(void)
{
    /* as mcu_hw_upload_core: drop the FPGA-bridged mount, switch the TF mux (GPIO16) to the BL616 SDH */
    f_mount(NULL, "/sd", 1);
    sdc_direct_init();
    TickType_t t0 = xTaskGetTickCount();
    FRESULT res;
    while ((res = f_mount(&card_fs, "/sd", 1)) != FR_OK && (xTaskGetTickCount() - t0) < pdMS_TO_TICKS(500))
        vTaskDelay(pdMS_TO_TICKS(100));
    card_ok = res == FR_OK;
    tango_debugf("SD card %s", card_ok ? "mounted (BL616)" : "not found");
}

static void card_release(void)
{
    f_mount(NULL, "/sd", 1);
    sdc_direct_release();
    card_ok = false;
}

static int fs_open(void *c, const char *path)
{
    (void)c;
    if (!card_ok) return 0;
    char p[TANGO_PATH_MAX + 8];
    strcpy(p, "/sd");
    if (path[0]) { strcat(p, "/"); strncat(p, path, TANGO_PATH_MAX - 1); }
    return f_opendir(&card_dir, p) == FR_OK;
}
static int fs_read(void *c, char *name, int cap, int *is_dir)
{
    (void)c;
    FILINFO fno;
    if (f_readdir(&card_dir, &fno) != FR_OK || !fno.fname[0]) return 0;
    strncpy(name, fno.fname, (size_t)cap - 1); name[cap - 1] = 0;
    *is_dir = (fno.fattrib & AM_DIR) != 0;
    return 1;
}
static void fs_close(void *c) { (void)c; f_closedir(&card_dir); }
static const tango_fs_t card = { .open = fs_open, .read = fs_read, .close = fs_close, .ctx = NULL };

/* ------------------------------------------------------------------ launcher task */
static tango_launcher_t L;
static char core_conf[TANGO_CONF_MAX];                  /* the framework core's CONF_STR (the launcher reads its own into it for System Settings) */
static QueueHandle_t queue = NULL;
static volatile bool active = false;

#define Q_PRESS    0x100   /* | tango_ev_t */
#define Q_RELEASE  0x200
#define Q_TOGGLE   0x400
#define Q_DL       0x800
#define Q_REL_SEL  0x1000  /* A (pad) released */
#define Q_KBD      0x2000  /* from the keyboard (Companion's menu events), not a pad */
#define Q_RAW      0x4000  /* HID REPORT snapshots waiting in raw_q */
typedef struct { uint8_t port, flags; uint16_t vid, pid; uint32_t raw; } rawsnap_t;   /* launcher and cores both feed the pad manager */
static QueueHandle_t raw_q = NULL;
static padmgr_t *PM;                         /* the pad manager (tango_padmgr.c); NULL = out of memory: the pads behave as without it */
#define Q_CHAR     0x8000  /* | a typed character (keyboard): the jump / filter in a list */
#define Q_MKDN     0x10000 /* the menu key went down (a pad's combo or button, F12 / the Menu key): MiSTer's F12 press -- closes an open menu */
#define Q_MKUP     0x20000 /* ... and up: MiSTer's F12|UPSTROKE -- opens a closed menu */
#define Q_HOW_CORES 0x40000 /* | on Q_MKUP: Alt was held (MiSTer: Alt+Menu = the Cores list, menu.cpp 1621-1626) */
#define Q_HOW_SYS  0x80000 /* | on Q_MKUP: Win was held (MiSTer: Win+Menu = the System page, menu.cpp 1650-1653) */
static void post(uint32_t m);

#ifdef TANGO_LAUNCH_IN_TASK
/* after an upload: the new core either reports a cold boot (com_task then resets the MCU, sysctrl.c
 * sys_handle_event) or answers SYS STATUS; one that never answers (a core without the link) leaves us in
 * FPGA Companion's own poll loop, as main.c does when no FPGA answers at boot */
static void wait_for_new_core(void)
{
    active = false;
    tango_debugf("core loaded; waiting for it to answer");
    for (;;) {
        if (sys_status_is_valid()) {
            tango_debugf("new core answers; restarting the MCU for it");
            vTaskDelay(pdMS_TO_TICKS(2000));       /* main.c: give a cold-boot IRQ / USB time first */
            mcu_hw_reset();
        }
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

/* The launch from inside the launcher (Tango task). On hardware: never DONE for any core
 * (R 00026020 E/W 00000220, correct bytes and CRC), while the same load in the next boot works. Kept for
 * experiments only; A and X both use load_core_restart. */
static int load_core(void *c, const char *path)
{
    (void)c;
    tango_debugf("loading %s", path);
    const char *base = strrchr(path, '/');
    char what[NOTE_TEXT_MAX + 1], date[16];
    tango_display_name(base ? base + 1 : path, what, sizeof what, date);   /* the name the launcher shows */
    save_note(NOTE_IN_PROGRESS, NULL, what);      /* if an MCU reset cuts the load short, say so afterwards */
    card_release();                               /* mcu_hw_upload_core takes the card itself */
    gowin_reload_first = true;                    /* start from the flash image, as the power-up load does */
    /* the power-up load runs in com_task at the top priority: so does this one (fix candidate) */
    UBaseType_t prio = uxTaskPriorityGet(NULL);
    vTaskPrioritySet(NULL, configMAX_PRIORITIES - 1);
    int r = mcu_hw_upload_core_result(path);
    vTaskPrioritySet(NULL, prio);
    gowin_reload_first = false;
    if (r == TANGO_LOAD_OK || r == TANGO_LOAD_NOT_FOUND) note_mem[0] = 0;
    uint32_t rec[REC_WORDS] = { 0 };
    if (r != TANGO_LOAD_NOT_FOUND) {
        uint32_t st4k = 0, st64k = 0;
        if (r == TANGO_LOAD_NOT_DONE) probe_after_failure(path, &st4k, &st64k);   /* also reloads the flash image */
        make_record(rec, r, HOW_TASK, st4k, st64k);
    }
    if (r == TANGO_LOAD_OK) wait_for_new_core();  /* does not return */
    if (r == TANGO_LOAD_FAILED || r == TANGO_LOAD_NOT_DONE) {
        /* the FPGA holds no usable core: a cold restart loads /menu.fs again, and the launcher then
         * says what happened (the note survives the restart in HBN RAM) */
        tango_debugf("%s: restarting to load /menu.fs again",
                     r == TANGO_LOAD_FAILED ? "upload broke off" : "uploaded, but the FPGA is not DONE");
        uint32_t st[3] = { rec[9], rec[7], rec[8] };
        save_note(r, st, what);
        save_nums(rec);
        force_cold = true;
        mcu_hw_reset();                           /* does not return */
    }
    card_take();                                  /* nothing reached the FPGA: the launcher still runs */
    return r;
}

#endif

/* A and X: restart the MCU and load the core in the next boot, before sys_wait4fpga (tango_fw_boot):
 * the power-up conditions, which load on hardware (the Atari 2600 core). MiSTer, too, restarts its Main
 * app after loading a core. */
static int load_core_restart(void *c, const char *path)
{
    (void)c;
    tango_debugf("loading %s through an MCU restart", path);
    save_request(path);                           /* replaces any note: the request uses the note block */
    card_release();
    mcu_hw_reset();                               /* warm: the next boot sees the request */
    return TANGO_LOAD_FAILED;                     /* not reached */
}

/* System Settings > Available space (menu.cpp 7388-7396): the card's free MB; asked once per launcher start (tango_launcher.c)
 * -- f_getfree is instant when the volume's free count is valid (FAT32 FSINFO) and scans the FAT or bitmap otherwise */
static int l_free_mb(void *c)
{
    (void)c;
    if (!card_ok) return -1;
    DWORD nclst = 0; FATFS *fs = NULL;
    if (f_getfree("/sd/", &nclst, &fs) != FR_OK || !fs) return -1;
    return (int)(((uint64_t)nclst * fs->csize) / 2048);          /* 512-byte sectors (FF_MIN_SS = FF_MAX_SS = 512) */
}
static void c_reboot(void *c, int cold);                          /* System > Reboot: defined with the in-core OSD below */
static tango_mb_t MBP[2];                                         /* the menu button of the pad on each USB port: its system map (tango_menubtn.h), else Share + Options */
static uint8_t MBC[2];                                            /* 1 = that pad's system map defines it (so the GET_DB9 combo is not the menu button) */
static uint8_t SYSV[2]; static uint32_t SYSM[2][12];               /* the pad's system map when its Define joystick buttons wrote one (tango_pm_sysmap_valid): its twelve buttons */
static tango_mkey_t MK;                                           /* the menu key's edges (core_task only) */
static tango_mbdet_t MBD[2], ESCD[2];                             /* the raw-word detectors per port: the menu button, and the 3 s Share + Options escape (core_task only) */
static tango_vol_t VOL;                                           /* Core Volume / Global Volume (tango_volume.h): one per boot */
static void vol_apply(void *c, int core);                         /* defined with the video processing below */
/* Misc. Options' Information block: SYS 0x70's reply (once, as the page opens) and the BL616's IP address (mcu_hw.c; none unless a USB
 * Ethernet adapter or Wi-Fi gave it one) */
static uint8_t l_vsync(void *c) { (void)c; return vp_vsync_read(&link); }
static int l_ip(void *c, char *buf, int cap) { (void)c; return mcu_hw_ip_string(buf, cap); }
/* System Settings > Define joystick buttons: the pads' raw buttons (a HID REPORT read from the launcher's task, only while that page is open) and the
 * pad's system map file */
static int c_save_map(void *c, const char *name, const uint32_t map[TANGO_PM_BTNS]);
static int l_pad_raw(void *c, int port, uint16_t *vid, uint16_t *pid, uint32_t *raw)
{
    (void)c;
    uint8_t fl = tango_hid_report(&link, (uint8_t)port, vid, pid, raw);
    return (fl >> (2 * port)) & 1;
}
static int l_save_sysmap(void *c, uint16_t vid, uint16_t pid, const uint32_t map[32])
{
    char name[40];
    tango_mb_mapname(vid, pid, name, sizeof name);
    return c_save_map(c, name, map);
}
/* the learner behind the walk (tango_padmap.h): its state is heap, allocated when the walk starts and freed when it ends (the launcher's heap is what it was before) */
static tango_pmlearn_t *LM; static hp_layout_t *LML[2];
extern const plsess_files_t *tango_fw_plsess_files(void);
static tango_pmlearn_t *l_learn_begin(void *c)
{
    (void)c;
    if (!PM || !padmgr_swap_supported(PM) || LM) return NULL;                     /* an FPGA without the decode block: the walk reads the raw word */
    LM = malloc(sizeof *LM);
    if (!LM) { tango_debugf("Define: no memory for the learner (%u bytes); the walk reads the raw word", (unsigned)sizeof *LM); return NULL; }
    tango_pmlearn_zero(LM);
    int n = 0;
    for (int p = 0; p < 2; p++) {
        uint16_t vid, pid, ver;
        if (!padmgr_port_ids(PM, p, &vid, &pid, &ver)) continue;                   /* a pad whose descriptors the manager has resolved */
        int src = padmgr_port_src(PM, p);
        if (src == PRS_DS4 || src == PRS_XINPUT) continue;                            /* a hard-wired decode (the DualShock 4's, the Xbox 360 class): its raw word is right, the walk reads it */
        hp_layout_t *lay = malloc(sizeof *lay);                                     /* 2.4 KB, only for a pad that is there */
        if (lay && !padmgr_layout(PM, p, lay)) { free(lay); lay = NULL; }          /* no usable descriptor: learned bit by bit */
        LML[p] = lay;
        tango_pmlearn_port(LM, p, vid, pid, ver, padmgr_port_ver_hid(PM, p), lay);
        n++;
    }
    if (!n) { free(LM); LM = NULL; return NULL; }
    return LM;
}
static int l_pad_sample(void *c, int port, pl_sample_t *s) { (void)c; return PM && padmgr_report_sample(PM, port, s); }
static void l_learn_end(void *c, int save, int port)
{
    (void)c;
    if (!LM) return;
    if (save && port >= 0) {
        plsess_t *S = tango_pmlearn_session(LM);
        if (S) {
            char sdl[512];
            int fl = plsess_finish(S, PM, port, tango_fw_plsess_files(), sdl, sizeof sdl);
            tango_debugf("Define: %s%s%s", fl & PLF_APPLIED ? "decode applied " : "", fl & PLF_DEC ? ".dec saved " : "", fl & PLF_SDL ? "SDL line added" : "");
            if (fl & PLF_SDL) tango_debugf("Define: %s", sdl);
        }
    }
    for (int p = 0; p < 2; p++) { free(LML[p]); LML[p] = NULL; }
    free(LM); LM = NULL;
}
static tango_pmcap_t *l_pmcap(void *c);
static const tango_platform_t plat = { .fs = &card, .load_core = load_core_restart, .load_core_alt = load_core_restart,
                                       .free_mb = l_free_mb, .reboot = c_reboot, .vol = &VOL, .vol_apply = vol_apply, .vsync = l_vsync, .ip = l_ip,
                                       .pad_raw = l_pad_raw, .pmcap = l_pmcap, .save_sysmap = l_save_sysmap,
                                       .learn_begin = l_learn_begin, .pad_sample = l_pad_sample, .learn_end = l_learn_end };

static char cyc_key[3][24], cyc_text[3][96];
static int ncyc = 0;

static tango_vproc_t *vp_begin(const char *name);
static void vp_end(tango_vproc_t *v);
static void vp_start(void);   /* forward-declared: dl_close() (its vp_pending branch) calls it, defined
                                * only much later in this file, same as vp_begin/vp_end just above */
static void vol_tick(const char *core);
static TickType_t vol_wait(TickType_t wait);
static void pm_start(int launcher);               /* the pad manager (tango_padmgr.c), below */
static void pm_drain_raw(void);
static void task(void *p)
{
    (void)p;
    card_take();
    tango_launcher_start(&L, &plat);
    osd_enable(OSD_VISIBLE);                      /* osd_is_visible(): Companion then routes input to the menu */
    pm_start(1);                                  /* pads in the launcher too: a pad that cannot be decoded must not drive the menu */
    {   /* the launcher's video processing, after its panel (vp_begin below).  It has no Video processing page and nothing
         * changes it later (menu.rbf: menu.cpp 7367-7441), so its state is freed once everything is sent: the launcher's
         * heap is then what it was before */
        vp_end(vp_begin("MENU"));
    }
    {
        int res; uint32_t st[3]; char what[NOTE_TEXT_MAX + 1], msg[112];
        if (take_note(&res, st, what, sizeof what) && (res != NOTE_IN_PROGRESS || booted_warm)) {   /* the last launch failed */
            if (res == NOTE_IN_PROGRESS) snprintf(msg, sizeof msg, "%s (cut short by an MCU reset)", what);
            else if (res == TANGO_LOAD_NOT_DONE)    /* the Gowin status register: after the reload, erase, write */
                snprintf(msg, sizeof msg, "%s: not DONE  R %08lx E %08lx W %08lx", what,
                         (unsigned long)st[1], (unsigned long)st[2], (unsigned long)st[0]);
            else snprintf(msg, sizeof msg, "%s: upload failed  R %08lx E %08lx", what, (unsigned long)st[1], (unsigned long)st[2]);
            tango_debugf("last launch: %s", msg);
            /* the load records: this boot's power-up load and the failed launch */
            uint32_t rec_lau[REC_WORDS] = { 0 };
            bool have_lau = take_nums(rec_lau);
            static char lines[4][96];
            int n = diag_lines(rec_pwr, &lines[0]);
            if (have_lau) n += diag_lines(rec_lau, &lines[n]);
            const char *lp[4] = { lines[0], lines[1], lines[2], lines[3] };
            for (int i = 0; i < n; i++) tango_debugf("diag: %s", lines[i]);
            tango_launcher_diag(&L, lp, n);
            tango_launcher_message(&L, "Could not load", msg);
            /* the legend also cycles through them (the rows may not fit under a long list) */
            ncyc = 0;
            snprintf(cyc_key[ncyc], sizeof cyc_key[0], "Could not load"); snprintf(cyc_text[ncyc], sizeof cyc_text[0], "%s", msg); ncyc++;
            if (have_lau) { legend_line(rec_lau, cyc_key[ncyc], sizeof cyc_key[0], cyc_text[ncyc], sizeof cyc_text[0]); ncyc++; }
            if (rec_pwr[0] == TANGO_DIAG_MAGIC) { legend_line(rec_pwr, cyc_key[ncyc], sizeof cyc_key[0], cyc_text[ncyc], sizeof cyc_text[0]); ncyc++; }
        }
    }
    tango_ev_t held = TANGO_EV_NONE;
    TickType_t wait = ncyc > 1 ? pdMS_TO_TICKS(3000) : portMAX_DELAY;
    int cyc = 0;
    for (;;) {
        uint32_t msg;
        vol_tick("MENU");                             /* a volume change is saved a second after the last one */
        /* a Reboot press waits for its release or for the 1.3 s of a cold reboot (tango_launcher_tick; MiSTer's REPEATRATE 50 ms) */
        if (tango_launcher_reboot_pending(&L) && wait > pdMS_TO_TICKS(50)) wait = pdMS_TO_TICKS(50);
        if (PM && padmgr_waiting(PM)) { padmgr_tick(PM); if (wait > pdMS_TO_TICKS(250)) wait = pdMS_TO_TICKS(250); }   /* a pad whose descriptors are late: a look every 250 ms */
        TickType_t w = vol_wait(wait);                /* ... and the task wakes for it */
        {   /* ... and for the next step of a long name that scrolls in its row */
            int ms = tango_launcher_wait_ms(&L, xTaskGetTickCount() * portTICK_PERIOD_MS);
            if (ms >= 0 && pdMS_TO_TICKS(ms ? ms : 1) < w) w = pdMS_TO_TICKS(ms ? ms : 1);
        }
        if (xQueueReceive(queue, &msg, w) == pdTRUE) {
            tango_launcher_tick(&L, xTaskGetTickCount() * portTICK_PERIOD_MS);   /* the clock the Select press is stamped with */
            if (msg & Q_REL_SEL) { tango_launcher_select_released(&L); continue; }   /* A went up */
            if (msg & Q_RELEASE) {
                if (msg & Q_KBD) tango_launcher_select_released(&L);               /* Enter / Space went up (any key, as core_task) */
                held = TANGO_EV_NONE; wait = portMAX_DELAY; continue;
            }
            if (msg & Q_CHAR) { tango_launcher_type(&L, (char)(msg & 0xff), xTaskGetTickCount() * portTICK_PERIOD_MS); ncyc = 0; continue; }
            if (msg & Q_RAW) { pm_drain_raw(); continue; }                 /* HID REPORT snapshots: the pad manager (pads in the launcher too) */
            if (msg & (Q_TOGGLE | Q_MKDN | Q_MKUP | Q_DL)) continue;
            if (tango_launcher_capturing(&L) && !(msg & Q_KBD)) continue;   /* Define joystick buttons reads the pad raw (tango_launcher_tick) */
            ncyc = 0;                                 /* a key ends the legend cycle */
            tango_ev_t ev = (tango_ev_t)(msg & 0xff);
            tango_launcher_event(&L, ev);
            /* key repeat as MiSTer's osd.h: 500 ms, then every 50 ms (tango_launcher_repeats: file lists take every list key) */
            if (tango_launcher_repeats(&L, ev)) { held = ev; wait = pdMS_TO_TICKS(TANGO_REPEAT_DELAY_MS); }
            else { held = TANGO_EV_NONE; wait = portMAX_DELAY; }
        } else if (tango_launcher_tick(&L, xTaskGetTickCount() * portTICK_PERIOD_MS)) {
            /* the cold reboot was asked for (the MCU resets), or a long name moved */
        } else if (w < wait) {
            /* only the volume's save timer ran out (vol_tick, above) */
        } else if (held != TANGO_EV_NONE) {
            tango_launcher_event(&L, held);
            wait = pdMS_TO_TICKS(TANGO_REPEAT_RATE_MS);
        } else if (ncyc > 1 && !tango_launcher_reboot_pending(&L)) {   /* next line of the failed launch's evidence */
            cyc = (cyc + 1) % ncyc;
            tango_launcher_message(&L, cyc_key[cyc], cyc_text[cyc]);
            wait = pdMS_TO_TICKS(3000);
        }
    }
}

bool tango_fw_try_launcher(void)
{
    if (!tango_launcher_detect(&L, &link)) {
        tango_debugf("CAPS: %s", (L.osd.caps[0] == 'Z' && L.osd.caps[1] == 'O') ?
                     "a Tango core, not the launcher: the in-core OSD" : "not a Tango core");
        return false;
    }
    tango_debugf("Tango launcher core (CAPS version %d)", L.osd.caps[2]);
    /* System Settings shows this firmware's id and the menu core's CONF_STR version tag.  core_conf is the in-core OSD's
     * buffer: a boot runs the launcher or a core, never both, so reading the launcher's string into it costs no RAM. */
    tango_sys_read_confstr(&link, core_conf, sizeof core_conf);
    tango_launcher_set_info(&L, TANGO_FW_ID, core_conf);
    queue = xQueueCreate(16, sizeof(uint32_t));
    raw_q = xQueueCreate(16, sizeof(rawsnap_t));
    active = true;
    xTaskCreate(task, "Tango", 4096, NULL, configMAX_PRIORITIES - 3, NULL);
    return true;
}

bool tango_fw_active(void) { return active; }

/* ------------------------------------------------------------------ the in-core OSD (a framework core) */
/* A core that answers CAPS 'Z''O' and whose CONF_STR is not the launcher's: MiSTer's menu for a running core
 * (tango_coremenu.c), opened with Start+Select on a pad (GET_DB9 bits 11+10) or F12. The BL616 owns the card
 * (the framework has no SD bridge: SDC STATUS reads 00), loads config/<NAME>.CFG at core start, and feeds F
 * file loads to the core through SDC IMAGE (menu-tango docs/TANGO-SPEC.md "Cores"). All FatFs work runs in this task:
 * com_task hands every SDC interrupt over (tango_fw_sdc_irq) and waits for the chunk to be written. */
static struct { int conn; uint16_t vid, pid; } pads[2];
static bool core_mode = false;
static tango_coremenu_t CM;
static tango_pmcap_t *l_pmcap(void *c) { (void)c; return &CM.pm; }   /* the launcher's Define walk uses the core menu's dialog state: one of them runs per boot */
static SemaphoreHandle_t dl_sem = NULL;
static tango_dl_t DL;
static FIL dl_fil;
static bool dl_file_open = false;
static bool dl_is_zip = false;             /* the current download's source is CM.zip (tango_zip_read), not dl_fil */
static bool dl_is_mra = false;             /* the source is mra_rom (tango_mra_rom_read),
                                             * an auto-started .mra rom index 0, not dl_fil or CM.zip */

static int c_dir_exists(void *c, const char *path)
{
    (void)c;
    char p[TANGO_PATH_MAX + 8]; FILINFO f;
    snprintf(p, sizeof p, "/sd/%s", path);
    return card_ok && f_stat(p, &f) == FR_OK && (f.fattrib & AM_DIR);
}

/* Forward-declared here (defined much further down, after dl_close(), which
 * its own arc_save_dip_cheat()/arc_save_nvram() need in scope first) so c_save_cfg below -- defined long
 * before that point in the file -- can call it. Without it the call is an implicit declaration, and the
 * firmware build (build.sh) fails with "static declaration follows non-static declaration": the implicit
 * int rule gives the call site a non-static, int-returning prototype that the real (void, static)
 * definition conflicts with. tango_fw.c isn't part of the host test suite, so only the firmware build
 * sees this. */
static void arc_save_all(void);

/* FileSaveConfig (file_io.cpp 854-869): config/<name>, the raw status bytes (user_io.cpp 624-627) */
static int c_save_cfg(void *c, const char *name, const uint8_t st[16])
{
    (void)c;
    /* DIP/cheat/NVRAM save mirrors this exact call's own two real trigger
     * points (SYS_SAVE "Save settings" and the Reset-settings confirm, tango_coremenu.c) -- see arc_save_all's
     * own header comment for the full reasoning, including the one deliberate deviation from MiSTer (also
     * firing on Reset-settings, which real MiSTer does not, since both share this one callback here). A
     * no-op for any core that isn't arc_active (every console core, every plain-restart-launched core). */
    arc_save_all();
    if (!card_ok) return -1;
    FRESULT r = f_mkdir("/sd/config");
    if (r != FR_OK && r != FR_EXIST) return -1;
    char p[80]; FIL f; UINT bw = 0;
    snprintf(p, sizeof p, "/sd/config/%s", name);
    if (f_open(&f, p, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) return -1;
    r = f_write(&f, st, 16, &bw);
    f_close(&f);
    tango_debugf("saved %s (%s)", p, r == FR_OK && bw == 16 ? "ok" : "failed");
    return r == FR_OK && bw == 16 ? 0 : -1;
}

/* FileLoadConfig (file_io.cpp 847-852): up to 16 bytes; a missing file leaves the word 0 (user_io.cpp 1584-1592) */
static bool c_load_cfg(const char *name, uint8_t st[16])
{
    memset(st, 0, 16);
    if (!card_ok) return false;
    char p[80]; FIL f; UINT br = 0;
    snprintf(p, sizeof p, "/sd/config/%s", name);
    if (f_open(&f, p, FA_OPEN_EXISTING | FA_READ) != FR_OK) return false;
    FRESULT r = f_read(&f, st, 16, &br);
    f_close(&f);
    if (r != FR_OK) memset(st, 0, 16);
    tango_debugf("config %s: %u bytes", p, (unsigned)br);
    return r == FR_OK;
}

static int dl_read(void *c, uint8_t *b, unsigned n)
{
    (void)c; UINT br = 0;
    if (f_read(&dl_fil, b, n, &br) != FR_OK) return -1;
    return (int)br;
}

/* ---- zip archives (tango_zip.c): a dedicated FatFs handle, separate from dl_fil and the save slots, since a
 * zip browse session (CM.zip) stays open across the whole "list its matching files, then stream one of them"
 * lifecycle, including the time after the OSD has closed and the download is already under way. */
static FIL zip_fil;
static bool zip_fil_open = false;
static int zfs_open(void *c, const char *path, uint32_t *size)
{
    (void)c;
    if (zip_fil_open) { f_close(&zip_fil); zip_fil_open = false; }
    if (!card_ok) return 0;
    char p[TANGO_PATH_MAX + 8]; snprintf(p, sizeof p, "/sd/%s", path);
    if (f_open(&zip_fil, p, FA_OPEN_EXISTING | FA_READ) != FR_OK) return 0;
    zip_fil_open = true;
    *size = (uint32_t)f_size(&zip_fil);
    return 1;
}
static int zfs_read(void *c, uint32_t off, uint8_t *buf, unsigned n)
{
    (void)c; UINT br = 0;
    if (!zip_fil_open || f_lseek(&zip_fil, off) != FR_OK || f_read(&zip_fil, buf, n, &br) != FR_OK) return -1;
    return (int)br;
}
static void zfs_close(void *c) { (void)c; if (zip_fil_open) { f_close(&zip_fil); zip_fil_open = false; } }
static const tango_zipfs_t zipfs = { zfs_open, zfs_read, zfs_close, NULL };
static int dl_read_zip(void *c, uint8_t *b, unsigned n) { (void)c; return tango_zip_read(&CM.zip, &zipfs, b, n); }

/* The third dl_step() dispatch leaf, defined up here with dl_read/dl_read_zip
 * even though mra_s (below, with the rest of the auto-trigger) isn't declared yet -- forward-declared
 * immediately below so dl_step() (also later in this file) can use this name; the real body is defined
 * with the rest of the auto-trigger, after resolve_mra(). Surfaces tango_mra_rom_read's own extra error
 * codes (TANGO_MRA_REFUSED, TANGO_MRA_MD5_MISMATCH) as a clearly logged failure instead of a silently
 * truncated/garbled stream -- tango_dl_pump's own rd() contract already treats ANY non-positive return as
 * a hard failure (tango_coremenu.c's tango_dl_pump: "got <= 0" ends the download), so no translation is
 * needed for correctness, only for the debug line's own clarity. */
static int dl_read_mra(void *c, uint8_t *b, unsigned n);

/* See the forward declaration in tango_fw_boot's neighbourhood for why this lives here.
 * req is the saved request's full "/sd/..." path; out gets the resolved core's full "/sd/_Arcade/cores/..."
 * path. Needs the BL616's own FatFs mount (card_take), which mcu_hw_upload_core_result does not share
 * (card_release's own comment: "mcu_hw_upload_core takes the card itself") -- released again before
 * returning, same as load_core/c_load_core already do around that call. */
static int resolve_mra(const char *req, char *out, int cap)
{
    const char *rel = !strncmp(req, "/sd/", 4) ? req + 4 : req;   /* zfs_open/fs_open each add "/sd/" back */
    char rbf[TANGO_MRA_RBF_MAX] = "", core_name[TANGO_PATH_MAX] = "";
    card_take();
    int ok = card_ok && tango_mra_find_rbf(&zipfs, rel, rbf, sizeof rbf) &&
             tango_mra_resolve_core(&card, "_Arcade/cores", rbf, core_name, sizeof core_name);
    card_release();
    if (ok) {
        snprintf(out, (size_t)cap, "/sd/_Arcade/cores/%s", core_name);
        tango_debugf("mra: %s -> rbf \"%s\" -> %s", rel, rbf, out);
    }
    return ok;
}

/* ------------------------------------------------------------------ The .mra
 * ROM auto-trigger. tango_mra_rom_read needs up to 1 (the .mra itself) + TANGO_MRA_MAX_ZIPS FatFs files
 * open AT ONCE (its own doc: "zips[]/zip_ios[] must already be opened" by the caller, tried per-part in
 * MiSTer's own fallback order) -- separate from zip_fil/zipfs (CM's own, console-zip Files browsing, one
 * archive at a time) and dl_fil (plain F downloads), since those are each scoped to one file/archive.
 *
 * All of it (the .mra's own FIL, up to TANGO_MRA_MAX_ZIPS zip FILs/tango_zip_t's, and the tango_mra_rom_t
 * state) lives in ONE malloc'd block, allocated only while an auto-started .mra download is actually in
 * flight and freed the moment it ends (dl_close(), or any failure path below) -- NOT static .bss, which
 * would sit in RAM permanently whether or not an arcade core is ever loaded (see the comment on VP:
 * "MEMORY: no static buffers").
 *
 * Heap margin: with a full scan table in each tango_zip_t this struct measures ~22 KB (tango_zip_t at
 * 3944 bytes * 4, tango_mra_rom_t 3608, five FILs at ~550 each) -- with a core built with
 * TANGO_VIDEO_PROC=1 (e.g. Galaga) that is tight enough to fail the very first deflated part's 32 KB
 * inflate window outright. Sizing for the real pipe count rather than the worst case
 * (TANGO_MRA_MAX_ZIPS zips) does not help Galaga (its zip="" list names all TANGO_MRA_MAX_ZIPS=4 zips,
 * so n_zips IS 4, the worst case, regardless). Each tango_zip_t carries a TANGO_ZIP_MAX_ENTRIES=64-
 * entry scan table (+2 KB name pool) purely for tango_zip_scan's own multi-match listing (the console
 * Files-browse feature) -- a capability tango_mra.c's own open_file_part NEVER uses (it only ever calls
 * tango_zip_locate, which needs exactly one entry). That table is reached through each tango_zip_t's own
 * `cache` pointer (tango_zip.h) -- this struct's 4 zip[] instances share ONE of their own
 * (`mra_zip_cache`, below), not tango_zip.c's own single global default, which tango_coremenu.h's CM.zip
 * uses: this struct's own ROM-0 download can still be streaming in the background while the OSD lets
 * the user browse Files through CM.zip at the same time, and sharing one table between the two would
 * let either corrupt the other's scan/locate result (tango_zip.h's own header comment has the full case).
 * Net effect: ~9.9 KB off this struct (4 copies of tango_zip_t's own 3320-byte table collapse to 1,
 * minus the ~3.3 KB the one shared copy costs back here) -- no behaviour change for any caller of
 * tango_zip_scan/_locate (every one uses at most one archive's table at a time). */
typedef struct {
    tango_zipfs_t mra_io;
    FIL           mra_fil;
    bool          mra_fil_open;
    tango_mra_rom_t rom;                      /* reused as-is for both the sizing pass and the real pass
                                                * (tango_mra_rom_open's own memset resets it fully) */
    int           n_zips;
    tango_zip_cache_t mra_zip_cache;          /* shared by every zip[] slot below (mra_open_zips wires
                                                * each one's own .cache at it) -- open_file_part only
                                                * ever has ONE of them actively scanning/locating at a
                                                * time, so one table does for all 4; deliberately NOT
                                                * tango_zip.c's own global default (CM.zip's own table),
                                                * since a core's .mra ROM-0 download can still be
                                                * streaming in the background while the OSD lets the
                                                * user browse Files through CM.zip -- sharing the SAME
                                                * table with that would let one corrupt the other
                                                * (tango_zip.h's own header comment has the full case) */
    tango_zip_t   zip[TANGO_MRA_MAX_ZIPS];
    tango_zipfs_t zip_io[TANGO_MRA_MAX_ZIPS];
    FIL           zip_fil[TANGO_MRA_MAX_ZIPS];
    bool          zip_fil_open[TANGO_MRA_MAX_ZIPS];
    tango_zip_t  *zips_p[TANGO_MRA_MAX_ZIPS];
    const tango_zipfs_t *zip_ios_p[TANGO_MRA_MAX_ZIPS];
} mra_session_t;
static mra_session_t *mra_s = NULL;           /* NULL = no .mra auto-download in flight (also dl_is_mra's
                                                * own backing: the two are always set/cleared together) */

static int mra_mfs_open(void *c, const char *path, uint32_t *size)
{
    (void)c;
    if (mra_s->mra_fil_open) { f_close(&mra_s->mra_fil); mra_s->mra_fil_open = false; }
    if (!card_ok) return 0;
    char p[TANGO_PATH_MAX + 8]; snprintf(p, sizeof p, "/sd/%s", path);
    if (f_open(&mra_s->mra_fil, p, FA_OPEN_EXISTING | FA_READ) != FR_OK) return 0;
    mra_s->mra_fil_open = true;
    *size = (uint32_t)f_size(&mra_s->mra_fil);
    return 1;
}
static int mra_mfs_read(void *c, uint32_t off, uint8_t *buf, unsigned n)
{
    (void)c; UINT br = 0;
    if (!mra_s->mra_fil_open || f_lseek(&mra_s->mra_fil, off) != FR_OK || f_read(&mra_s->mra_fil, buf, n, &br) != FR_OK) return -1;
    return (int)br;
}
static void mra_mfs_close(void *c) { (void)c; if (mra_s->mra_fil_open) { f_close(&mra_s->mra_fil); mra_s->mra_fil_open = false; } }

static int mra_zfs_open(void *c, const char *path, uint32_t *size)
{
    int i = (int)(intptr_t)c;
    if (mra_s->zip_fil_open[i]) { f_close(&mra_s->zip_fil[i]); mra_s->zip_fil_open[i] = false; }
    if (!card_ok) return 0;
    char p[TANGO_PATH_MAX + 8]; snprintf(p, sizeof p, "/sd/%s", path);
    if (f_open(&mra_s->zip_fil[i], p, FA_OPEN_EXISTING | FA_READ) != FR_OK) return 0;
    mra_s->zip_fil_open[i] = true;
    *size = (uint32_t)f_size(&mra_s->zip_fil[i]);
    return 1;
}
static int mra_zfs_read(void *c, uint32_t off, uint8_t *buf, unsigned n)
{
    int i = (int)(intptr_t)c; UINT br = 0;
    if (!mra_s->zip_fil_open[i] || f_lseek(&mra_s->zip_fil[i], off) != FR_OK || f_read(&mra_s->zip_fil[i], buf, n, &br) != FR_OK) return -1;
    return (int)br;
}
static void mra_zfs_close(void *c)
{
    int i = (int)(intptr_t)c;
    if (mra_s->zip_fil_open[i]) { f_close(&mra_s->zip_fil[i]); mra_s->zip_fil_open[i] = false; }
}

/* menu-tango docs/LAUNCHER.md: a <rom zip="..."> name starting with '/' is root-relative (strip the slash);
 * otherwise try games/mame/<name> first, then _Arcade/mame/<name> (mra_loader.cpp 157-180, 893-895, as
 * menu-tango docs/LAUNCHER.md cites it) -- both are plain candidates in the SAME pipe-fallback trial
 * tango_mra_rom_read already does per <part>, so a name neither path has simply never matches any part,
 * exactly like a listed zip that doesn't exist on disk at all (e.g. Galaga's galagamw.zip). */
static int mra_open_one_zip(const char *name, int i)
{
    mra_s->zip_io[i] = (tango_zipfs_t){ mra_zfs_open, mra_zfs_read, mra_zfs_close, (void *)(intptr_t)i };
    memset(&mra_s->zip[i], 0, sizeof mra_s->zip[i]);
    char p[TANGO_PATH_MAX];
    if (name[0] == '/') { snprintf(p, sizeof p, "%s", name + 1); return tango_zip_open_archive(&mra_s->zip[i], &mra_s->zip_io[i], p); }
    snprintf(p, sizeof p, "games/mame/%s", name);
    if (tango_zip_open_archive(&mra_s->zip[i], &mra_s->zip_io[i], p)) return 1;
    snprintf(p, sizeof p, "_Arcade/mame/%s", name);
    return tango_zip_open_archive(&mra_s->zip[i], &mra_s->zip_io[i], p);
}

/* Splits out_zip ("a|b|c", up to TANGO_MRA_MAX_ZIPS names -- tango_mra_rom_open already capped it to
 * TANGO_MRA_ZIP_NAME_MAX) and opens each candidate archive, same shape test_mra_real.c's own helper
 * already proved against the real Galaga romset: a name that fails at both paths is left "not open"
 * (tango_zip_t.open stays 0) but still counted and passed to tango_mra_rom_read, which treats that exactly
 * like a present-but-corrupt or absent fallback -- not fatal. "" (no zip at all, e.g. an inline-only rom)
 * opens nothing: n_zips stays 0, zips[]/zip_ios[] are never dereferenced for an inline part. */
static void mra_open_zips(const char *out_zip)
{
    for (int i = 0; i < TANGO_MRA_MAX_ZIPS; i++) {
        mra_s->zips_p[i] = &mra_s->zip[i]; mra_s->zip_ios_p[i] = &mra_s->zip_io[i];
        mra_s->zip[i].cache = &mra_s->mra_zip_cache;   /* before any open_archive call touches it below --
            * tango_zip_close() (open_archive's own first step) preserves whatever .cache already is, it
            * does not establish it the first time */
    }
    mra_s->n_zips = 0;
    char list[TANGO_MRA_ZIP_NAME_MAX]; strncpy(list, out_zip, sizeof list - 1); list[sizeof list - 1] = 0;
    char *cur = list;
    while (*cur && mra_s->n_zips < TANGO_MRA_MAX_ZIPS) {
        char *bar = strchr(cur, '|');
        if (bar) *bar = 0;
        int ok = mra_open_one_zip(cur, mra_s->n_zips);
        tango_debugf("mra: zip \"%s\" %s", cur, ok ? "opened" : "did not open (fine if unused by this romset)");
        mra_s->n_zips++;
        if (!bar) break;
        cur = bar + 1;
    }
}

static void mra_close_zips(void)
{
    for (int i = 0; i < mra_s->n_zips; i++) tango_zip_close(&mra_s->zip[i], &mra_s->zip_io[i]);
    mra_s->n_zips = 0;
}

static void dl_close(void);   /* defined below mra_autostart; mra_autostart's own stale-state guard needs it */

/* The .mra ROM auto-trigger -- MiSTer's xml_load()/arcade_send_rom() equivalent
 * (mra_loader.cpp 1207-1233, 1048-1094), carried across app_restart(). The ORIGINAL .mra's path (already
 * resolved to a bitstream path earlier by resolve_mra -- that resolution's own card_take()/card_release()
 * pair is long done) is parsed a second time, this time for its <rom index="0"> (MiSTer's own convention:
 * each <rom index="N"> sends ioctl_index = N as one byte, mra_loader.cpp:326's user_io_set_index(romindex);
 * the first/primary ROM is always index 0), and its bytes start flowing to the just-loaded core over the
 * SAME ioctl_download/SDC_IMAGE mechanism every plain F download already uses -- dl_step's dl_read_mra
 * leaf (below) keeps pumping it on every later SDC interrupt, exactly like any other in-progress download.
 * Only rom index 0 is sent: a real multi-rom core (e.g. Galaga's own mod-byte roms at index 3 and 4) needs
 * more auto-trigger calls than this, which are not built here.
 *
 * Two passes, not one, because tango_dl_begin's own SELECT needs the exact total size up front (the real
 * wire protocol, not a Tango choice) and tango_mra_rom_read has no way to report that without actually
 * producing every byte (it streams rather than buffers: the total length is never known up front, like
 * MiSTer's own rolling romlen[]). Pass 1 streams the whole assembly into a throwaway
 * buffer just to count it -- and, for free, runs the real md5 check BEFORE any byte ever reaches the FPGA:
 * since tango_dl_begin/SELECT hasn't happened yet at that point, a REFUSED or MD5_MISMATCH result here
 * means nothing was ever sent, exactly MiSTer's own "discard, don't send" rule (rom_finish), not a
 * deviation from it. Pass 2 re-opens the same .mra fresh (tango_mra_rom_open resets all per-rom state,
 * reusing the SAME tango_mra_rom_t -- its own memset makes that safe) and feeds the real tango_dl_begin/
 * dl_step pump. Because pass 1 already validated the exact byte count and the md5, pass 2's own read
 * calls should never actually reach "done" (tango_dl_pump's own d->sent>=d->size check ends the download
 * one call earlier) or hit a fresh md5 mismatch -- dl_read_mra's own handling of those two codes is a
 * defensive backstop for a source that changed between the two passes, not a path this normally exercises.
 * The zip archives pass 1 opened are reused as-is for pass 2 (re-opening them would just re-read the same
 * central directory for no benefit).
 *
 * Card-mount lifecycle: called from core_task() (below), AFTER its own card_take()/tango_core_init()/
 * vp_start() -- not from tango_fw_boot, where the .mra path is first noticed. tango_fw_boot runs well
 * before core_task() exists (com_task calls it before sys_wait4fpga/tango_fw_try_core even run), with no
 * card mount of its own to hand off. Doing its own card_take() there and skipping card_release() on
 * success, so the mount and its open FatFs handles carry over into core_task()'s own session, does not
 * work: core_task() always calls card_take() at its
 * own start regardless, which unmounts and remounts (f_mount(NULL,...) then a fresh f_mount) and would
 * invalidate every FIL opened before it (FatFs bumps the FATFS object's internal id on every (re)mount;
 * an open FIL's own now-stale id fails validate() with FR_INVALID_OBJECT on its next read, ff.c) --
 * making card_take() skip its own remount when already mounted would be a global
 * behaviour change to a function every restart-launch and every core start goes through, for the sake of
 * one caller. Calling this from core_task() instead, after ITS card_take() has already run, needs no
 * special-casing anywhere: this function never touches card_take()/card_release() at all, the same as
 * every other core_task()-side file access (c_load_file, pm_load, ...). tango_fw_boot only ever records
 * the path (mra_pending, near REQ_PATH_MAX) for core_task() to pick up once its own mount is live. */
static void mra_autostart(const char *req)
{
    const char *rel = !strncmp(req, "/sd/", 4) ? req + 4 : req;   /* mra_mfs_open/fs_open each add "/sd/" back */

    mra_session_t *s = malloc(sizeof *s);
    if (!s) { tango_debugf("mra autostart: out of memory (%u bytes)", (unsigned)sizeof *s); return; }
    memset(s, 0, sizeof *s);
    mra_s = s;
    s->mra_io = (tango_zipfs_t){ mra_mfs_open, mra_mfs_read, mra_mfs_close, NULL };

    char zipname[TANGO_MRA_ZIP_NAME_MAX];
    if (!tango_mra_rom_open(&s->rom, &s->mra_io, rel, 0, zipname, sizeof zipname)) {
        tango_debugf("mra autostart: %s would not open", rel);
        free(s); mra_s = NULL;
        return;
    }
    if (!s->rom.found) {
        tango_debugf("mra autostart: %s declares no <rom index=\"0\">: nothing to send", rel);
        s->mra_io.close(s->mra_io.ctx);
        free(s); mra_s = NULL;
        return;
    }
    mra_open_zips(zipname);

    static uint8_t scratch[512];
    uint32_t total = 0; int got = -1;
    for (;;) {
        got = tango_mra_rom_read(&s->rom, &s->mra_io, s->zips_p, s->zip_ios_p, s->n_zips, scratch, sizeof scratch);
        if (got <= 0) break;
        total += (uint32_t)got;
    }
    tango_mra_rom_close(&s->rom, &s->mra_io, s->zips_p, s->zip_ios_p);
    if (got < 0) {
        /* part_name/out_pos survive tango_mra_rom_close() just above (it does no memset) -- on a
         * hardware-only failure (OOM in tango_zip_open_member's own malloc for a deflated member, a FatFs
         * open-file-limit refusal, a zip missing from the card) this sizing pass's own "I/O or parse
         * error" catch-all would otherwise say nothing about WHERE it happened; open_file_part's own
         * per-zip fallback loop (tango_mra.c) treats any such failure identically to "not in this zip",
         * so the part name/offset here is the only way to tell "OOM, FatFs limit, or genuinely absent"
         * apart after the fact, from the board's debug console. */
        tango_debugf("mra autostart: %s: %s (stopped at part \"%s\", %lu bytes assembled) -- not sending", rel,
                     got == TANGO_MRA_REFUSED ? "refused (interleave, a part-level zip override, or too many patches)" :
                     got == TANGO_MRA_MD5_MISMATCH ? "md5 mismatch (discarded before sending, same as MiSTer)" :
                     "I/O or parse error", s->rom.part_name, (unsigned long)s->rom.out_pos);
        mra_close_zips(); free(s); mra_s = NULL;
        return;
    }
    if (total == 0) {
        tango_debugf("mra autostart: %s rom 0 is empty: nothing to send", rel);
        mra_close_zips(); free(s); mra_s = NULL;
        return;
    }

    if (!tango_mra_rom_open(&s->rom, &s->mra_io, rel, 0, NULL, 0) || !s->rom.found) {
        tango_debugf("mra autostart: %s: rom 0 vanished between passes", rel);
        mra_close_zips(); free(s); mra_s = NULL;
        return;
    }
    if (dl_file_open || dl_is_zip || dl_is_mra) { tango_dl_end(&DL); dl_close(); }   /* stale state from an earlier core */
    if (tango_dl_begin(&DL, &link, 0, total) != 0) {
        tango_debugf("mra autostart: tango_dl_begin failed (%lu bytes)", (unsigned long)total);
        tango_mra_rom_close(&s->rom, &s->mra_io, s->zips_p, s->zip_ios_p);
        mra_close_zips(); free(s); mra_s = NULL;
        return;
    }
    dl_is_mra = true;
    DL.before_end = NULL; DL.end_ctx = NULL;
    tango_debugf("mra autostart: %s rom 0: %lu bytes, index 0", rel, (unsigned long)total);
}

static int dl_read_mra(void *c, uint8_t *b, unsigned n)
{
    (void)c;
    int got = tango_mra_rom_read(&mra_s->rom, &mra_s->mra_io, mra_s->zips_p, mra_s->zip_ios_p, mra_s->n_zips, b, n);
    if (got < 0)
        /* same reasoning as mra_autostart's own sizing-pass failure log above: the real streaming pass
         * (this function) is what a mid-transfer board failure -- OOM, a FatFs limit, the card going
         * away -- would hit, after the sizing pass already succeeded; part_name/out_pos (mra_s->rom's,
         * not yet closed here) say where this pass itself stopped, which the generic "I/O or parse
         * error" text alone cannot distinguish from the sizing pass's own report. */
        tango_debugf("mra rom read: %s (stopped at part \"%s\", %lu bytes already sent)",
                      got == TANGO_MRA_REFUSED ? "refused (interleave/zip override/too many patches)" :
                      got == TANGO_MRA_MD5_MISMATCH ? "md5 mismatch" : "I/O or parse error",
                      mra_s->rom.part_name, (unsigned long)mra_s->rom.out_pos);
    return got;
}

/* ------------------------------------------------------------------ DIP/
 * cheat/NVRAM send and NVRAM save, for a core launched from an .mra (arc_active only ever becomes true for
 * one of those -- a plain restart-launch, ext != ".mra", never touches any of this, exactly MiSTer's own
 * is_arcade() gate).
 *
 * Index assignment: DIP word at 254, cheat word at 255.
 * DIP is MiSTer's own fixed-index convention (mra_loader.cpp's arcade_sw_send:
 * `user_io_set_index(254)`, sizeof(dip_cur) == 8, sent whole, only if
 * dip_num). NVRAM uses the .mra's own <nvram index="N">, MiSTer's own arcade_nvm_load/save convention
 * (support/arcade/mra_loader.cpp:60-107). The cheat index 255 is not found in Main_MiSTer's source:
 * its arcade_sw_send only ever sends index 254 (<switches>) -- there is no literal "255" anywhere in it, and
 * <cheats> there is handled entirely by the newer cheats.cpp Pro-cheat engine (a different, more
 * general mechanism, not a second 64-bit word at a fixed index). A fixed cheat index 255 is
 * plausibly from an older MiSTer revision that used a second sw_struct-shaped word for <cheats> before
 * cheats.cpp replaced it. tango_mra.h parses <cheats> as a second tango_mra_switches_t, the same shape
 * as <switches>, and this code follows that (index 255, same send/save mechanism). Open: whether 255 is
 * still right, or a modern cheat belongs on the Pro-cheat path instead (a materially different
 * mechanism).
 *
 * File I/O, mirroring MiSTer's own naming
 * (mra_loader.cpp:1133-1143, 66-77): the .mra's own basename with ".mra" replaced by ".dip" / ".nvm"
 * (cheat: ".cheat" -- the citation gap noted above is about the INDEX (255) and the send/save
 * MECHANISM for cheats, not this filename), in
 * config/dips/, config/cheats/, config/nvram/ respectively -- the same config/ root c_save_cfg/c_load_cfg
 * (status word) and
 * c_save_map/pm_load (pad maps) already use, just their own subfolders, not <NAME>.CFG's own flat layout
 * (MiSTer's own convention uses subfolders here, so this follows that). */
#define TANGO_ARC_NAME_MAX   48          /* the .mra's own basename, generous past any real filename seen */
#define TANGO_ARC_NVRAM_MAX  TANGO_UL_MAX /* the upload mechanism's own clamp (tango_proto.h): never ask for
                                           * more than a save can ever actually come back as */
static bool     arc_active = false;          /* true once arc_setup() has parsed a real .mra for this core */
/* True unless the core's own NVRAM state is known to NOT reflect a real saved
 * file on disk -- gates arc_save_nvram so a later save can never destroy a good .nvm. Without it, a
 * data loss: arc_setup() loads a valid .nvm from
 * disk, but the send (tango_dl_send) to the core fails or is skipped (!arc_dl_idle()); arc_active still
 * becomes true; the next Save-settings or reboot then calls arc_save_nvram(), which pulls the CORE's own
 * current NVRAM (its own reset-default, since the load never actually reached it) and overwrites the good
 * file on disk with it. Set in arc_setup() to true only when there is nothing to protect (no <nvram>, or
 * no saved file existed yet) or the load+send both genuinely succeeded; false the moment a load succeeded
 * but the send did not, or never ran. */
static bool     arc_nvram_ok = true;
static bool     arc_pending_setup = false;    /* an .mra launched this core; arc_setup() has not run yet --
                                                * set where mra_pending is captured, cleared by whichever of
                                                * dl_close()/core_task() actually runs it (see their own
                                                * comments: it must wait for the ROM auto-download, if any,
                                                * to fully close first -- reusing the SAME download
                                                * mechanism at a new index would otherwise collide with it) */
static bool     vp_pending = false;           /* heap margin: the SAME
                                                * shape as arc_pending_setup, just for vp_start() instead
                                                * of arc_setup() -- set in core_task() only when the ROM-0
                                                * auto-download genuinely started (dl_is_mra true), cleared
                                                * and acted on by dl_close()'s own mra branch once that
                                                * download's real streaming pass has fully closed, win or
                                                * lose; also cleared (never acted on) by c_reboot/
                                                * c_load_core if the core is torn down before that happens. */
static char     arc_mra_path[REQ_PATH_MAX];   /* the .mra's own path, relative (no "/sd/"), for &zipfs */
static char     arc_name[TANGO_ARC_NAME_MAX]; /* its basename, ".mra" stripped (MiSTer's switches.name/nvram_name, minus the extension) */
/* NOT a static tango_mra_switches_t (x2): sizeof(tango_mra_switches_t) is ~83.5 KB (TANGO_MRA_DIP_MAX
 * 64 entries, each carrying
 * TANGO_MRA_DIP_IDS_MAX 32 id strings of TANGO_MRA_DIP_ID_LEN_MAX 32 bytes plus a 32-entry val[] -- a
 * worst-case shape, not a typical one, but sized that way regardless), so two of them as permanent statics
 * is ~167 KB of RAM every build pays for whether or not an arcade core is ever loaded, let alone one with
 * DIPs -- the firmware link fails ("region `ram' overflowed by 92480 bytes", ld). Only the two small
 * facts arc_save_dip_cheat() and
 * arc_save_all() actually need for the whole rest of the core's session are kept (whether a DIP/cheat
 * table was non-empty, and the current/saved word) -- the full parsed table itself is malloc'd, used, and
 * freed again within arc_setup() alone, never held longer than that one function call. */
static bool     arc_dip_has = false, arc_cheat_has = false;
static int      arc_dip_num = 0;       /* the <switches> dip_num tango_mra_switches_summary
                                         * already computed in arc_setup_one -- a single int, not re-parsed; wires
                                         * tango_coremenu_t's own DIP page (tango_core_dip_setup, arc_setup below).
                                         * NOT the <cheats> count (there is a switches page only, no cheats page) */
static uint64_t arc_dip_def = 0;       /* the <switches default=""> word itself,
                                         * captured in arc_setup_one BEFORE arc_load_blob may overwrite arc_dip_cur
                                         * with a saved .dip override -- lets "Reset settings" put dip_cur back to
                                         * the real default (menu.cpp 5964-5970), not whatever was last saved */
static tango_mra_misc_t     arc_misc;         /* small (~270 bytes: two char[128]s + four ints) -- fine as a permanent static */
static uint64_t arc_dip_cur, arc_dip_saved, arc_cheat_cur, arc_cheat_saved;

/* true only while neither direction (download or upload) is already busy with something else -- guards
 * every call site below that actually touches the link (tango_dl_send inside arc_setup_one/arc_setup;
 * tango_ul_recv inside arc_save_nvram). Calling either while a real download is open would corrupt it (a
 * second SELECT mid-stream); calling tango_ul_recv while one is open is not a hang risk (its own
 * poll cap, tango_coremenu.c, bounds the wait the RTL's own upload deferral
 * could otherwise impose), but skipping cleanly here is still better than paying that wait every time.
 * A real case: the OSD can be reopened (Start+Select/F12,
 * tango_core_toggle does not care whether a download is active) while an F-browser download is still
 * streaming in the background, so a user CAN reach System > Save settings while dl_file_open is true.
 * arc_save_dip_cheat() (below) deliberately does NOT use this guard: it only reads/writes already-known,
 * in-RAM uint64_t values to/from FatFs, never touches the link, so there is nothing for it to protect
 * against -- gating it here would only cost real DIP/cheat saves for no safety benefit. */
static bool arc_dl_idle(void) { return !dl_file_open && !dl_is_zip && !dl_is_mra; }

/* A DEDICATED FIL/tango_zipfs_t for re-reading the .mra itself (switches/cheats/nvram metadata), separate
 * from zip_fil/zipfs (CM's own console-zip Files browsing) on purpose: arc_setup() can run from inside
 * dl_close() mid-session, while the OSD (open at core start) could in principle
 * already have the user inside Files browsing a DIFFERENT zip archive via zip_fil -- reusing that shared
 * handle here would silently steal it out from under CM.zip's own session (zfs_open's own "close whatever
 * was open first" rule). A second, independent FIL avoids that entirely; it is only ever open for the
 * length of one tango_mra_switches_parse/tango_mra_misc_parse call (each opens, reads, and closes it
 * internally -- tango_mra.h's own contract), never held across calls. */
static FIL arc_fil;
static bool arc_fil_open = false;
static int arc_fs_open(void *c, const char *path, uint32_t *size)
{
    (void)c;
    if (arc_fil_open) { f_close(&arc_fil); arc_fil_open = false; }
    if (!card_ok) return 0;
    char p[TANGO_PATH_MAX + 8]; snprintf(p, sizeof p, "/sd/%s", path);
    if (f_open(&arc_fil, p, FA_OPEN_EXISTING | FA_READ) != FR_OK) return 0;
    arc_fil_open = true;
    *size = (uint32_t)f_size(&arc_fil);
    return 1;
}
static int arc_fs_read(void *c, uint32_t off, uint8_t *buf, unsigned n)
{
    (void)c; UINT br = 0;
    if (!arc_fil_open || f_lseek(&arc_fil, off) != FR_OK || f_read(&arc_fil, buf, n, &br) != FR_OK) return -1;
    return (int)br;
}
static void arc_fs_close(void *c) { (void)c; if (arc_fil_open) { f_close(&arc_fil); arc_fil_open = false; } }
static const tango_zipfs_t arc_zipfs = { arc_fs_open, arc_fs_read, arc_fs_close, NULL };

/* config/<dir>/<name>: a plain, whole-file read/write, the same FatFs calls c_save_cfg/c_load_cfg already
 * use for config/<NAME>.CFG, just with an extra path segment (MiSTer's own dips/, nvram/ subfolders,
 * confirmed from source, see this section's own header comment). Unlike c_load_cfg (which always memsets a
 * full 16 bytes and reports whether the file existed), a short read here is treated as "not found" too --
 * a partial DIP/NVRAM file is exactly as useless as a missing one, and MiSTer's own FileLoadConfig applies
 * the same "did the whole thing arrive" rule.
 * Reads into a scratch buffer FIRST, copying to the caller's buf only on a full, successful read:
 * f_read fills buf from byte 0 up to whatever it actually got before a
 * short read or error, so writing straight into the caller's buf would leave a
 * truncated real file's bytes mixed with whatever buf held before the call -- for arc_dip_cur/
 * arc_cheat_cur specifically, already set to dip_def right before this call, a half-corrupted hybrid of
 * "some real file bytes, some stale default bytes" instead of cleanly falling back to dip_def whole.
 * malloc'd, NOT a static (a static TANGO_ARC_NVRAM_MAX-byte scratch costs exactly 2048 bytes of
 * permanent OCRAM, enough to fail the firmware build's RAM check: it passes at 78592 bytes without it
 * and fails at 76544 with it). Bounded to n (never more than TANGO_ARC_NVRAM_MAX in practice), freed immediately. */
static bool arc_load_blob(const char *dir, const char *name, uint8_t *buf, uint32_t n)
{
    if (!card_ok) return false;
    uint8_t *scratch = malloc(n);
    if (!scratch) { tango_debugf("arcade: %s/%s: out of memory (%u bytes)", dir, name, (unsigned)n); return false; }
    char p[96]; FIL f; UINT br = 0;
    snprintf(p, sizeof p, "/sd/config/%s/%s", dir, name);
    if (f_open(&f, p, FA_OPEN_EXISTING | FA_READ) != FR_OK) { free(scratch); return false; }
    FRESULT r = f_read(&f, scratch, n, &br);
    f_close(&f);
    tango_debugf("arcade: %s: %u/%u bytes", p, (unsigned)br, (unsigned)n);
    bool ok = r == FR_OK && br == n;
    if (ok) memcpy(buf, scratch, n);
    free(scratch);
    return ok;
}
static bool arc_save_blob(const char *dir, const char *name, const uint8_t *buf, uint32_t n)
{
    if (!card_ok) return false;
    FRESULT r = f_mkdir("/sd/config");
    if (r != FR_OK && r != FR_EXIST) return false;
    char d[32]; snprintf(d, sizeof d, "/sd/config/%s", dir);
    r = f_mkdir(d);
    if (r != FR_OK && r != FR_EXIST) return false;
    char p[96]; FIL f; UINT bw = 0;
    snprintf(p, sizeof p, "%s/%s", d, name);
    if (f_open(&f, p, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) return false;
    r = f_write(&f, buf, n, &bw);
    f_close(&f);
    tango_debugf("arcade: saved %s (%s)", p, r == FR_OK && bw == n ? "ok" : "failed");
    return r == FR_OK && bw == n;
}

/* arc_name + the given extension (".dip"/".cheat"/".nvm"), e.g. "galaga.dip" */
static void arc_filename(char *out, int cap, const char *suffix)
{
    snprintf(out, (size_t)cap, "%s%s", arc_name, suffix);
}

/* MiSTer's own DIP/cheat sequence, mirrored directly (mra_loader.cpp:1178-1181, 60-77, 116-126): dip_cur =
 * dip_def; load the saved override if one exists (leaves dip_cur unchanged if not, matching
 * "arcade_sw_load... FileLoadConfig into dip_cur"); dip_saved = dip_cur; send dip_cur IF dip_num (a core
 * with no <switches> at all gets no send, matching "if (sw->dip_num)"). Cheats: identical, index 255 (see
 * this section's own header comment on the citation gap for 255 specifically). Uses
 * tango_mra_switches_summary, NOT tango_mra_switches_parse -- see that function's own comment
 * (tango_mra.c): the full per-<dip> table is far too large to malloc on the BL616 at all. Returns 0
 * only if the .mra itself would not open (a real I/O error, not "no such block" -- that case still returns
 * 1, *has left false). */
static bool arc_setup_one(int cheats, const char *dir, const char *suffix, uint8_t idx, bool *has, uint64_t *cur, uint64_t *saved)
{
    int dip_num = 0;
    int ok = tango_mra_switches_summary(&arc_zipfs, arc_mra_path, cheats, cur, &dip_num);
    if (ok) {
        *has = dip_num != 0;
        if (!cheats) { arc_dip_num = dip_num; arc_dip_def = *cur; }   /* tango_core_dip_setup's
            * own count/default, below -- *cur is still dip_def itself here, before arc_load_blob (next line) may
            * overwrite it with a saved override */
        char nm[TANGO_ARC_NAME_MAX + 8]; arc_filename(nm, sizeof nm, suffix);
        arc_load_blob(dir, nm, (uint8_t *)cur, sizeof *cur);
        *saved = *cur;
        if (*has && arc_dl_idle())
            tango_debugf("arcade: send idx %d: %s", idx, tango_dl_send(&link, idx, (const uint8_t *)cur, sizeof *cur) == 0 ? "ok" : "FAILED");
    }
    return ok != 0;
}

/* Called once the .mra ROM auto-download (if one started: mra_autostart() succeeded) has fully closed --
 * its own SELECT(0) already on the wire, DL/dl_is_mra idle again (see arc_pending_setup's own comment for
 * why this must wait) -- or once it's clear none was ever started at all (refused/empty/not found/no
 * <rom index="0">: still fine, DIP/cheat/NVRAM are independent of ROM assembly succeeding, exactly as
 * MiSTer's own arcade_send_rom runs the DIP send unconditionally at its own end regardless of how the ROM
 * itself went). DIP/cheat: arc_setup_one's own doc comment above. NVRAM: load-and-send ONLY if a saved file
 * exists (arcade_nvm_load's own `if (FileLoadConfig(...))` guard -- MiSTer sends nothing at all, leaving
 * the core's own reset-default state, when there is no saved file yet) and only up to TANGO_ARC_NVRAM_MAX
 * (the upload mechanism's own clamp; a declared size above that is refused outright and logged, not
 * truncated silently -- the same "refuse, don't mislead" discipline tango_ul_begin already applies). */
static void arc_setup(void)
{
    memset(&arc_misc, 0, sizeof arc_misc);
    if (!arc_setup_one(0, "dips", ".dip", 254, &arc_dip_has, &arc_dip_cur, &arc_dip_saved) ||
        !arc_setup_one(1, "cheats", ".cheat", 255, &arc_cheat_has, &arc_cheat_cur, &arc_cheat_saved) ||
        !tango_mra_misc_parse(&arc_zipfs, arc_mra_path, &arc_misc)) {
        tango_debugf("arcade: %s would not open for DIP/cheat/NVRAM metadata", arc_mra_path);
        return;
    }
    arc_active = true;
    arc_nvram_ok = true;    /* the default: nothing to protect yet (no <nvram>, or no file on disk) */
    /* Wire the OSD's own DIP page to what was just parsed -- arc_dip_has false
     * (no <switches> at all) passes dip_num 0, which tango_core_dip_setup treats as "disabled" regardless
     * of arc_dip_num's own stale value from a PREVIOUS core (it's never reset between loads on its own). */
    tango_core_dip_setup(&CM, &arc_zipfs, arc_mra_path, arc_dip_has ? arc_dip_num : 0, arc_dip_cur, arc_dip_def);

    if (arc_misc.has_nvram && arc_misc.nvram_size > 0) {
        if ((uint32_t)arc_misc.nvram_size > TANGO_ARC_NVRAM_MAX) {
            tango_debugf("arcade: <nvram size=\"%d\"> exceeds the upload mechanism's own %u-byte clamp -- not loaded",
                         arc_misc.nvram_size, (unsigned)TANGO_ARC_NVRAM_MAX);
        } else {
            /* malloc'd, not static (as mra_session_t): static RAM starves the heap -- a console core,
             * or an arcade core with no <nvram> at all, never pays for this. Sized to the real declared size, not the worst-case
             * TANGO_ARC_NVRAM_MAX, and freed right after this one use. */
            uint8_t *nvbuf = malloc((size_t)arc_misc.nvram_size);
            char nm[TANGO_ARC_NAME_MAX + 8];
            if (!nvbuf) { tango_debugf("arcade: NVRAM: out of memory (%d bytes)", arc_misc.nvram_size); return; }
            arc_filename(nm, sizeof nm, ".nvm");
            if (arc_load_blob("nvram", nm, nvbuf, (uint32_t)arc_misc.nvram_size)) {
                /* a real saved file exists: the core must actually receive it before arc_save_nvram() is
                 * ever allowed to read the core back and overwrite that same file -- arc_nvram_ok stays
                 * true ONLY if the send genuinely happens and succeeds (never just because a send was
                 * attempted). */
                bool sent_ok = arc_dl_idle() &&
                    tango_dl_send(&link, (uint8_t)arc_misc.nvram_index, nvbuf, (uint32_t)arc_misc.nvram_size) == 0;
                tango_debugf("arcade: NVRAM send (idx %d, %d bytes): %s", arc_misc.nvram_index, arc_misc.nvram_size,
                             arc_dl_idle() ? (sent_ok ? "ok" : "FAILED") : "skipped (a download is open)");
                arc_nvram_ok = sent_ok;
            }
            /* else: no saved file (or it was unreadable) -- nothing of value on disk to protect; a fresh
             * save later is fine (arc_nvram_ok stays true, its default above), and nothing was sent, so
             * the core's own reset-default NVRAM state stands, matching MiSTer's own arcade_nvm_load. */
            free(nvbuf);
        }
    }
}

/* Save-timing policy: DIP/cheat are saved (only
 * if changed, mirroring arcade_sw_save's own "dip_saved != dip_cur" guard) and NVRAM is pulled back from the
 * core and saved, at exactly the SAME two points tango_fw.c's own status-word save already uses --
 * c_save_cfg's two real call sites, tango_coremenu.c's SYS_SAVE ("Save settings") and its Reset-settings
 * confirm. This is also MiSTer's own precedent for the explicit half
 * (menu.cpp:3103 arcade_nvm_save fires from the SAME "Save settings" case that calls
 * user_io_status_save). One deliberate deviation from a
 * literal MiSTer mirror: MiSTer's Reset-settings path does NOT call arcade_nvm_save (only case 14, "Save
 * settings", does) -- this also flushes NVRAM on Reset-settings, since c_save_cfg is the one shared callback
 * behind both and distinguishing them would need a new tango_core_platform_t field for one extra, harmless
 * save (writing the CURRENT nvram content to disk can never corrupt anything; it is simply an extra save at
 * a point MiSTer itself does not bother to take one).
 * ALSO saved at core teardown (c_reboot, c_load_core, below) -- NOT a MiSTer trigger (MiSTer's source
 * has only the two above; real MiSTer arcade cores mostly rely on the core's own
 * ioctl_upload_req-driven autosave for this, which is not honoured here). Without the teardown save,
 * NVRAM would survive a reboot only for
 * a user who happens to have pressed "Save settings" first, and a real NVRAM save (high scores, EEPROM
 * state) is reasonably expected to survive an ordinary reboot the way a cartridge battery save already does
 * for every console core here, not just an explicit settings save. */
static void arc_save_dip_cheat(void)
{
    if (!arc_active) return;   /* pure FatFs I/O, no link use -- no arc_dl_idle() gate needed (see its own comment) */
    /* tango_coremenu_t's own DIP page (TANGO_CM_DIP) is the only thing that
     * ever mutates dip_cur (dip_select's toggle, and the Reset-settings dip-reset) -- resync it from
     * there HERE, in the one place every save path (Save settings, Reset-settings' own second save call,
     * core teardown, AND the OSD-close-via-combo/F12 path below) already funnels through, rather than at
     * each call site individually. Closing the OSD with
     * the combo/F12 while still on the DIP page calls tango_core_toggle() directly (core_task's own
     * Q_TOGGLE branch), which never runs tango_core_event() and so never returns TANGO_CM_DIP_SAVE at all
     * -- without this resync, a toggle would reach the core live (tango_dl_send already happened, inside
     * dip_select) but never get this far, and the OLD value would be what every later save/reboot writes. */
    if (CM.dip_num > 0) arc_dip_cur = tango_core_dip_cur(&CM);
    char nm[TANGO_ARC_NAME_MAX + 8];
    if (arc_dip_has && arc_dip_cur != arc_dip_saved) {
        arc_filename(nm, sizeof nm, ".dip");
        if (arc_save_blob("dips", nm, (const uint8_t *)&arc_dip_cur, sizeof arc_dip_cur)) arc_dip_saved = arc_dip_cur;
    }
    if (arc_cheat_has && arc_cheat_cur != arc_cheat_saved) {
        arc_filename(nm, sizeof nm, ".cheat");
        if (arc_save_blob("cheats", nm, (const uint8_t *)&arc_cheat_cur, sizeof arc_cheat_cur)) arc_cheat_saved = arc_cheat_cur;
    }
}
static void arc_save_nvram(void)
{
    if (!arc_active || !arc_misc.has_nvram || arc_misc.nvram_size <= 0) return;
    if ((uint32_t)arc_misc.nvram_size > TANGO_ARC_NVRAM_MAX) return;   /* arc_setup() already logged this once */
    if (!arc_nvram_ok) {
        /* arc_setup()'s own load+send didn't both succeed: the core's current NVRAM is NOT known to match
         * (or safely supersede) whatever is already on disk -- saving now could destroy a good file with
         * the core's own un-synced/reset-default state (the real data-loss bug this flag exists to close,
         * see its own declaration). Refuse until the next core load gets a clean run. */
        tango_debugf("arcade: NVRAM save skipped (the load/send at core start did not both succeed)");
        return;
    }
    if (!arc_dl_idle()) { tango_debugf("arcade: NVRAM save skipped (a download is open)"); return; }
    uint8_t *nvbuf = malloc((size_t)arc_misc.nvram_size);     /* malloc'd, not static -- see arc_setup's own comment */
    if (!nvbuf) { tango_debugf("arcade: NVRAM save: out of memory (%d bytes)", arc_misc.nvram_size); return; }
    uint32_t got = 0;
    int ok = tango_ul_recv(&link, (uint8_t)arc_misc.nvram_index, (uint32_t)arc_misc.nvram_size, nvbuf, (uint32_t)arc_misc.nvram_size, &got) == 0 &&
             got == (uint32_t)arc_misc.nvram_size;
    if (!ok) {
        tango_debugf("arcade: NVRAM save: upload from the core failed or came back short (%u of %d bytes) -- not saved",
                     (unsigned)got, arc_misc.nvram_size);
    } else {
        char nm[TANGO_ARC_NAME_MAX + 8]; arc_filename(nm, sizeof nm, ".nvm");
        arc_save_blob("nvram", nm, nvbuf, (uint32_t)arc_misc.nvram_size);
    }
    free(nvbuf);
}
static void arc_save_all(void) { arc_save_dip_cheat(); arc_save_nvram(); }

static void dl_close(void)
{
    /* a zip-sourced download: the OSD that showed its listing is already closed (TANGO_CM_CLOSED_NOW), so the
     * whole archive -- not just the member -- can come down now, not just whenever the user next opens Files */
    if (dl_is_zip) { tango_zip_close(&CM.zip, &zipfs); CM.in_zip = 0; dl_is_zip = false; }
    else if (dl_file_open) { f_close(&dl_fil); dl_file_open = false; }
    else if (dl_is_mra) {                       /* the auto-started .mra session */
        tango_mra_rom_close(&mra_s->rom, &mra_s->mra_io, mra_s->zips_p, mra_s->zip_ios_p);
        mra_close_zips();
        free(mra_s); mra_s = NULL;
        dl_is_mra = false;
        /* heap margin: the ROM-0 download's own real streaming pass
         * (not just the sizing dry-run inside mra_autostart()) has now fully closed, win or lose --
         * dl_step() calls this on both "done" and "FAILED" -- so this is the first safe moment to give
         * vp_start() its own ~16.5 KB tango_vproc_t back, the same reasoning core_task()'s own comment
         * on vp_pending has in full. Runs before arc_setup() below, the same relative order as for a
         * core whose vp_start() is not deferred. */
        if (vp_pending) { vp_pending = false; vp_start(); }
        /* The ROM auto-download (if it ever actually started -- dl_is_mra was
         * only ever true if it did) has now fully closed; this is the one safe moment to send DIP/cheat/
         * NVRAM at their own new indices, reusing the SAME now-idle download mechanism (arc_pending_setup's
         * own comment explains why it cannot happen any earlier). */
        if (arc_pending_setup) { arc_pending_setup = false; arc_setup(); }
    }
}

/* ---- saves (tango_save.c): FatFs files on the card, a few open at once ---- */
static FIL sv_fil[TANGO_SAVE_SLOTS];
static bool sv_used[TANGO_SAVE_SLOTS];
static tango_slot_t slots[TANGO_SAVE_SLOTS] = { { .h = -1 }, { .h = -1 }, { .h = -1 }, { .h = -1 } };
static char sv_pending[TANGO_SAVE_PATH];         /* the save of the file being downloaded ("" = none) */
static int sv_open(void *c, const char *path, int create, int *writable, uint32_t *size)
{
    (void)c;
    int h = 0; while (h < TANGO_SAVE_SLOTS && sv_used[h]) h++;
    if (h == TANGO_SAVE_SLOTS || !card_ok) return -1;
    char p[TANGO_SAVE_PATH + 8]; snprintf(p, sizeof p, "/sd/%s", path);
    FRESULT r = f_open(&sv_fil[h], p, FA_READ | FA_WRITE | (create ? FA_CREATE_ALWAYS : FA_OPEN_EXISTING));
    *writable = 1;
    if (r != FR_OK && !create) { r = f_open(&sv_fil[h], p, FA_READ | FA_OPEN_EXISTING); *writable = 0; }   /* FileCanWrite */
    if (r != FR_OK) return -1;
    *size = (uint32_t)f_size(&sv_fil[h]);
    sv_used[h] = true;
    return h;
}
static int sv_read(void *c, int h, uint32_t off, uint8_t *buf, unsigned n)
{
    (void)c; UINT br = 0;
    if (f_lseek(&sv_fil[h], off) != FR_OK || f_read(&sv_fil[h], buf, n, &br) != FR_OK) return -1;
    return (int)br;
}
static int sv_write(void *c, int h, uint32_t off, const uint8_t *buf, unsigned n)
{
    (void)c; UINT bw = 0;
    if (f_lseek(&sv_fil[h], off) != FR_OK || f_write(&sv_fil[h], buf, n, &bw) != FR_OK) return -1;
    f_sync(&sv_fil[h]);                               /* O_SYNC (user_io.cpp 2220) */
    return (int)bw;
}
static void sv_close(void *c, int h) { (void)c; f_close(&sv_fil[h]); sv_used[h] = false; }
static int sv_mkdir(void *c, const char *path)
{
    (void)c; char p[TANGO_SAVE_PATH + 8]; snprintf(p, sizeof p, "/sd/%s", path);
    FRESULT r = f_mkdir(p); return (r == FR_OK || r == FR_EXIST) ? 0 : -1;
}
static const tango_savefs_t savefs = { sv_open, sv_read, sv_write, sv_close, sv_mkdir, NULL };

/* the download of an "FS" entry's file is complete: mount its save on slot 0 (user_io_file_mount(save, 0, 1)) */
static void sv_mount_pending(void *c)
{
    (void)c;
    if (!sv_pending[0]) return;
    uint8_t flags = 0;
    int64_t size = tango_save_mount(&slots[0], &savefs, sv_pending, 0, 1, &flags);
    if (size >= 0) tango_sdc_insert(&link, 0, (flags & 0x80) != 0, (uint64_t)size);   /* SET_SDINFO + SET_SDSTAT */
    tango_debugf("save %s: %s, %ld bytes", sv_pending, slots[0].missing ? "created upon write" : "mounted", (long)size);
    sv_pending[0] = 0;
}

/* F: the file goes to the core with ioctl index idx (menu.cpp 2680: user_io_ext_idx << 6 | ioctl_index) */
static int c_load_file(void *c, const char *path, uint8_t idx)
{
    (void)c;
    if (dl_file_open || dl_is_zip || dl_is_mra) { tango_dl_end(&DL); dl_close(); }
    if (f_open(&dl_fil, path, FA_OPEN_EXISTING | FA_READ) != FR_OK) { tango_debugf("cannot open %s", path); return -1; }
    dl_file_open = true;
    uint32_t size = (uint32_t)f_size(&dl_fil);
    tango_debugf("download %s: %lu bytes, index %02x", path, (unsigned long)size, idx);
    sv_pending[0] = 0;
    if (tango_save_wanted(core_conf, CM.f_ent))            /* "FS": opensave (user_io.cpp 955-966) */
        tango_save_path(CM.name, path, sv_pending, sizeof sv_pending);
    if (tango_dl_begin(&DL, &link, idx, size) != 0) { dl_close(); return -1; }
    DL.before_end = sv_mount_pending; DL.end_ctx = NULL;
    return 0;
}

/* F, the selected file is inside a zip (tango_coremenu.h's load_zip_file; console
 * zips only): CM.zip is already open and scanned from browsing it (tango_coremenu.c's enter_zip) -- this only
 * opens the chosen member for streaming and starts the download exactly as c_load_file does, with
 * tango_zip_read (via dl_read_zip) as tango_dl_pump's byte source instead of dl_fil. */
static int c_load_zip_file(void *c, const char *zip_path, const char *member, uint8_t idx)
{
    (void)c; (void)zip_path;             /* CM.zip is already the right archive; zip_path is for logging only */
    if (dl_file_open || dl_is_zip || dl_is_mra) { tango_dl_end(&DL); dl_close(); }
    int mi = tango_zip_find(&CM.zip, member);
    if (mi < 0) { tango_debugf("zip member not found: %s", member); return -1; }
    uint32_t size = 0;
    if (tango_zip_open_member(&CM.zip, &zipfs, mi, &size) != 0) {
        tango_debugf("cannot open zip member %s", member);
        return -1;
    }
    dl_is_zip = true;
    tango_debugf("download (zip) %s: %lu bytes, index %02x", member, (unsigned long)size, idx);
    sv_pending[0] = 0;
    if (tango_save_wanted(core_conf, CM.f_ent))            /* keyed on the inner member's name, not the zip's */
        tango_save_path(CM.name, member, sv_pending, sizeof sv_pending);
    if (tango_dl_begin(&DL, &link, idx, size) != 0) { dl_close(); return -1; }
    DL.before_end = sv_mount_pending; DL.end_ctx = NULL;
    return 0;
}

/* one SDC interrupt: at most one chunk (the core offers <= 512 bytes per STATUS) */
static void dl_step(void)
{
    /* a block request of the core (sd_rd / sd_wr), MiSTer user_io.cpp 3383-3660 */
    uint32_t lba; int wr; uint8_t drv;
    if (tango_sdc_status(&link, &lba, &wr, &drv)) {
        static uint8_t sec[512];
        tango_slot_t *s = &slots[drv < TANGO_SAVE_SLOTS ? drv : 0];
        if (wr) {
            tango_sdc_sector_out(&link, drv, sec);
            if (drv < TANGO_SAVE_SLOTS) tango_save_write(s, &savefs, lba, sec);
            if (drv == 0) tango_core_saved_sector(&CM, xTaskGetTickCount() * portTICK_PERIOD_MS);   /* use_save */
        } else {
            if (drv < TANGO_SAVE_SLOTS) tango_save_read(s, &savefs, lba, sec); else memset(sec, 0xFF, sizeof sec);
            tango_sdc_sector_in(&link, drv, sec);
        }
        return;
    }
    if (!dl_file_open && !dl_is_zip && !dl_is_mra) {   /* not ours (e.g. left open before an MCU reset): close it */
        tango_ioctl_select(&link, 0, 0);
        return;
    }
    int r = tango_dl_pump(&DL, dl_is_mra ? dl_read_mra : dl_is_zip ? dl_read_zip : dl_read, NULL);
    if (r != 1) {
        tango_debugf("download %s: %lu of %lu bytes", r == 0 ? "done" : "FAILED", (unsigned long)DL.sent, (unsigned long)DL.size);
        dl_close();
    }
}

/* System > Reboot: short = the menu core through the restart path (MiSTer fpga_load_rbf("menu.rbf"));
 * held = cold: the FPGA's flash image, then the power-on load (MiSTer reboot(1)) */
static void c_reboot(void *c, int cold)
{
    (void)c;
    tango_debugf("%s", cold ? "cold reboot" : "reboot to the menu core");
    /* Abandon a still-pending arc_setup() rather than let the teardown below
     * run it. The .mra ROM auto-download can still be
     * in flight when the user reboots (the OSD is open at core start, so Reboot is reachable immediately);
     * dl_close() just below would otherwise see dl_is_mra fall and its own `if (arc_pending_setup)` hook
     * fire arc_setup() -- sending DIP/cheat/NVRAM-load to a core that is about to be torn down anyway, then
     * arc_save_all() (below) immediately uploading and saving THAT freshly-(re)loaded, never-actually-played
     * NVRAM state over whatever good file was already on disk. Clearing the flag here first means dl_close()
     * simply skips it; arc_active (never set in this path) already keeps arc_save_all() a no-op too. */
    arc_pending_setup = false;
    vp_pending = false;   /* same reasoning as arc_pending_setup just above --
                            * a core about to be torn down never needs its VP state (re)started */
    if (dl_file_open || dl_is_zip || dl_is_mra) { tango_dl_end(&DL); dl_close(); }
    /* Flush NVRAM (and any changed DIP/cheat) before the MCU resets -- a save
     * beyond c_save_cfg's two MiSTer-cited trigger points, so NVRAM survives a reboot
     * even for a user who never explicitly pressed Save settings first. See arc_save_all's
     * own header comment for the full reasoning. Must run AFTER the download/zip teardown just above, not
     * before it: arc_dl_idle()'s own guard reads
     * dl_file_open/dl_is_zip/dl_is_mra, which are still true until the teardown above actually clears them
     * -- calling arc_save_all() first would see any in-flight session as "busy" and skip the save in
     * exactly the moment it is about to be force-closed anyway. Calling it here, after, is also safe on the
     * wire even if the RTL's own FIFO hasn't fully drained the just-closed download's last few bytes yet:
     * the RTL's deferred-upload mechanism ("download closed, then
     * the deferred upload becomes ready") exists for exactly this ordering, and tango_ul_recv's own
     * poll cap (tango_coremenu.c) bounds how long this can ever wait. */
    arc_save_all();
    save_request(cold ? REQ_COLD : REQ_MENU);
    force_cold = cold != 0;
    card_release();
    mcu_hw_reset();
}

static int c_load_core(void *c, const char *path)
{
    arc_pending_setup = false;   /* same reasoning as c_reboot's own, above */
    vp_pending = false;          /* same reasoning as c_reboot's own, above */
    if (dl_file_open || dl_is_zip || dl_is_mra) { tango_dl_end(&DL); dl_close(); }
    arc_save_all();   /* same reasoning as c_reboot's own -- switching away, AFTER teardown */
    return load_core_restart(c, path);
}

/* ---- the core pad maps (menu-tango docs/TANGO-SPEC.md "Controllers"; MiSTer input.cpp 1692-1717, 3059-3080) ---- */
/* load_map: config/inputs/<name>, else config/<name>; up to the 128 bytes of uint32 map[32] */
static bool pm_load(const char *name, uint32_t map[TANGO_PM_BTNS])
{
    static const char *const dirs[] = { "/sd/config/inputs/", "/sd/config/" };
    memset(map, 0, TANGO_PM_BTNS * sizeof map[0]);
    if (!card_ok) return false;
    for (int i = 0; i < 2; i++) {
        char p[128]; FIL fl; UINT br = 0;
        snprintf(p, sizeof p, "%s%s", dirs[i], name);
        if (f_open(&fl, p, FA_OPEN_EXISTING | FA_READ) != FR_OK) continue;
        FRESULT r = f_read(&fl, map, TANGO_PM_BTNS * sizeof map[0], &br);
        f_close(&fl);
        if (r == FR_OK && br) { tango_debugf("pad map %s: %u bytes", p, (unsigned)br); return true; }
    }
    return false;
}

/* save_map: FileDeleteConfig(name) (the old place), FileSaveConfig("inputs/" name) */
static int c_save_map(void *c, const char *name, const uint32_t map[TANGO_PM_BTNS])
{
    (void)c;
    if (!card_ok) return -1;
    char p[128]; FIL fl; UINT bw = 0;
    snprintf(p, sizeof p, "/sd/config/%s", name); f_unlink(p);
    FRESULT r = f_mkdir("/sd/config");
    if (r != FR_OK && r != FR_EXIST) return -1;
    r = f_mkdir("/sd/config/inputs");
    if (r != FR_OK && r != FR_EXIST) return -1;
    snprintf(p, sizeof p, "/sd/config/inputs/%s", name);
    if (f_open(&fl, p, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) return -1;
    r = f_write(&fl, map, TANGO_PM_BTNS * sizeof map[0], &bw);
    f_close(&fl);
    tango_debugf("saved %s (%s)", p, r == FR_OK && bw == 128 ? "ok" : "failed");
    return r == FR_OK && bw == 128 ? 0 : -1;
}

/* Define buttons editor: the pad's file into map[] (1 = read), and Reset to default (both places MiSTer's old and new layout use) */
static int c_load_map(void *c, const char *name, uint32_t map[TANGO_PM_BTNS]) { (void)c; return pm_load(name, map) ? 1 : 0; }
static int c_delete_map(void *c, const char *name)
{
    (void)c;
    if (!card_ok) return -1;
    char p[128];
    snprintf(p, sizeof p, "/sd/config/inputs/%s", name); FRESULT a = f_unlink(p);
    snprintf(p, sizeof p, "/sd/config/%s", name); FRESULT b = f_unlink(p);
    tango_debugf("deleted pad map %s (%s)", name, (a == FR_OK || b == FR_OK) ? "ok" : "none there");
    return 0;
}

/* a connected pad gets its core map: the file, else map_joystick's guess (a core with J names), else the FPGA's
 * default (SET_PADMAP FE: MiSTer's guess would leave a core without J names with the d-pad only) */
static void pm_apply(int port)
{
    if (!pads[port].conn) return;
    if (PM && padmgr_blocked(PM, port)) { padmgr_blank(PM, port); return; }     /* a quarantined pad stays blank (Define buttons' save etc.) */
    char name[96]; uint32_t map[TANGO_PM_BTNS]; uint8_t pairs[2 * TANGO_PM_BTNS];
    tango_pm_mapname(CM.name, pads[port].vid, pads[port].pid, name, sizeof name);
    const char *how;
    if (pm_load(name, map)) how = "file";
    else if (tango_pm_bcount(core_conf)) { tango_pm_guess_mm(core_conf, SYSV[port] ? SYSM[port] : tango_pm_mmap, map); how = SYSV[port] ? "guess (map_joystick over its system map)" : "guess (map_joystick)"; }
    else if (SYSV[port]) { memset(map, 0, sizeof map); for (int i = 0; i < 12; i++) map[i] = SYSM[port][i] & 0xFFFF; how = "system map"; }
    else { tango_hid_set_padmap(&link, (uint8_t)port, 0xFE, 0, NULL, 0); tango_debugf("port %d: default core map (no J names, no %s)", port, name); return; }
    tango_pm_pairs(map, pairs);
    tango_hid_set_padmap(&link, (uint8_t)port, 1, 0, pairs, TANGO_PM_BTNS);
    tango_debugf("port %d (%04x:%04x): core map from the %s", port, pads[port].vid, pads[port].pid, how);
}

static void c_maps_changed(void *c) { (void)c; pm_apply(0); pm_apply(1); }
/* tango_core_platform_t's own dl_busy -- arc_dl_idle() is declared well above
 * this point (it's used by arc_setup/arc_save_nvram already), so this is just a wrapper with the shape the
 * platform struct needs, not new logic. */
static int c_dl_busy(void *c) { (void)c; return !arc_dl_idle(); }

/* the core file the launcher started in this boot (MiSTer's core_path): relative to the card root, "" = not known (a power-up straight into a
 * core, a warm MCU reset: no request was taken).  Held in RAM: the launch request is consumed in the same boot that runs core_task. */
static int c_running_core(void *c, char *path, int cap)
{
    (void)c;
    if (!running_core[0]) return 0;
    strncpy(path, running_core, (size_t)cap - 1); path[cap - 1] = 0;
    return 1;
}
static const tango_core_platform_t cplat = { .fs = &card, .zipfs = &zipfs, .load_core = c_load_core,
                                             .reboot = c_reboot, .load_file = c_load_file,
                                             .load_zip_file = c_load_zip_file, .save_cfg = c_save_cfg,
                                             .dir_exists = c_dir_exists, .save_map = c_save_map,
                                             .maps_changed = c_maps_changed, .load_map = c_load_map, .delete_map = c_delete_map, .fw_id = TANGO_FW_ID,
                                             .vol = &VOL, .vol_apply = vol_apply, .vsync = l_vsync, .ip = l_ip, .running_core = c_running_core, .dl_busy = c_dl_busy };

/* ---- MiSTer's video / audio processing (tango_vproc.c): the card's files and config/ for it ----
 * MEMORY: no static buffers (91 KB of them -- a static tango_vproc_t of 65.7 KB, an 8 KB Tango.ini
 * buffer and 17 KB of parser scratch -- leave the OCRAM heap 3 KB, too small even for TLSF's own 3.2 KB control
 * block, so every FreeRTOS stack comes from the 59 KB WRAM heap and the launcher's 16 KB task stack is refused
 * (pvPortMalloc -> vApplicationMallocFailedHook -> abort): no launcher panel). VP is allocated in core_task, only for a
 * core whose probes answer, with malloc (NULL = the core runs without the blocks; no malloc-failed hook). */
static tango_vproc_t *VP;
static int vp_read_file(void *c, const char *path, uint8_t *buf, int cap)
{
    (void)c;
    if (!card_ok) return -1;
    char p[1300]; FIL f; UINT br = 0;
    snprintf(p, sizeof p, "/sd/%s", path);
    if (f_open(&f, p, FA_OPEN_EXISTING | FA_READ) != FR_OK) return -1;
    if (!buf) {                                       /* the size (vp_load sizes its buffer to the file) */
        FSIZE_t sz = f_size(&f);
        f_close(&f);
        return sz > 0x7FFFFFFF ? 0x7FFFFFFF : (int)sz;
    }
    FRESULT r = f_read(&f, buf, (UINT)cap, &br);
    f_close(&f);
    return r == FR_OK ? (int)br : -1;
}
/* FileSaveConfig (file_io.cpp 854-869): config/<name>; n = 0 deletes it (FileDeleteConfig, video_cfg_reset) */
static int vp_save_cfg(void *c, const char *name, const void *data, int n)
{
    (void)c;
    if (!card_ok) return -1;
    char p[96]; FIL f; UINT bw = 0;
    snprintf(p, sizeof p, "/sd/config/%s", name);
    if (!n) { f_unlink(p); return 0; }
    FRESULT r = f_mkdir("/sd/config");
    if (r != FR_OK && r != FR_EXIST) return -1;
    if (f_open(&f, p, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) return -1;
    r = f_write(&f, data, (UINT)n, &bw);
    f_close(&f);
    tango_debugf("saved %s (%s)", p, r == FR_OK && (int)bw == n ? "ok" : "failed");
    return r == FR_OK && (int)bw == n ? 0 : -1;
}
static int vp_load_cfg(void *c, const char *name, void *data, int n)
{
    (void)c;
    if (!card_ok) return -1;
    char p[96]; FIL f; UINT br = 0;
    snprintf(p, sizeof p, "/sd/config/%s", name);
    if (f_open(&f, p, FA_OPEN_EXISTING | FA_READ) != FR_OK) return -1;
    FRESULT r = f_read(&f, data, (UINT)n, &br);
    f_close(&f);
    return r == FR_OK ? (int)br : -1;
}
static const vp_io_t vpio = { vp_read_file, vp_save_cfg, vp_load_cfg, NULL };
/* MiSTer's video_init / load_volume at a core's start (user_io.cpp 1568-1570; video.cpp 2653-2675 video_cfg_init): the
 * processing files saved for the core's name (<name>_scaler.cfg, _gamma.cfg, _shmask.cfg, _afilter.cfg), else
 * Tango.ini's defaults (MiSTer.ini's keys, its [MiSTer] section or the core's own).  For every core: an in-core OSD
 * core (core_task, its CONF_STR name) and the launcher (task, "MENU": MiSTer runs video_init for menu.rbf too, by its
 * name "MENU"; menu.rbf has no Video processing page, menu.cpp 7367-7441, so only the files and Tango.ini set it). */
/* ---- Core Volume / Global Volume (tango_volume.c; MiSTer audio.cpp load_volume / save_volume / send_volume) ----
 * Volume.dat (the global byte) and <core>_volume.cfg (the core byte; never for the menu core) in config/, one byte each, saved a
 * second after the last change by the task that owns the card (vol_tick) */
static void volume_start(const char *name)
{
    uint8_t b = 0; int g = -1, c = -1;
    bool menu = !strcasecmp(name, "MENU");
    tango_vol_init(&VOL, menu);
    if (vp_load_cfg(NULL, "Volume.dat", &b, 1) == 1) g = b;
    if (!menu) {
        char nm[64]; snprintf(nm, sizeof nm, "%s_volume.cfg", name);
        if (vp_load_cfg(NULL, nm, &b, 1) == 1) c = b;
    }
    tango_vol_load(&VOL, g, c);
}
static void vol_send(void) { tango_sys_audvol(&link, tango_vol_audvol(&VOL)); }
/* a change from Misc. Options: SYS 0x26 always; with the audio filter the core volume is SET_AFILTER's first byte */
static void vol_apply(void *c, int core)
{
    (void)c;
    vol_send();
    if (core && VOL.has_filter && VP && VP->has_afilter) { VP->vol = tango_vol_corebyte(&VOL); vp_send_afilter(VP); }
}
/* the save timer: Tango's two tasks call it when they wake; vol_wait = the longest they may sleep */
static void vol_tick(const char *core)
{
    if (!tango_vol_save_due(&VOL, xTaskGetTickCount() * portTICK_PERIOD_MS)) return;
    uint8_t g = tango_vol_file_global(&VOL), c = tango_vol_file_core(&VOL);
    vp_save_cfg(NULL, "Volume.dat", &g, 1);
    if (!VOL.is_menu) { char nm[64]; snprintf(nm, sizeof nm, "%s_volume.cfg", core); vp_save_cfg(NULL, nm, &c, 1); }
}
static TickType_t vol_wait(TickType_t wait)
{
    int ms = tango_vol_wait_ms(&VOL, xTaskGetTickCount() * portTICK_PERIOD_MS);
    if (ms < 0) return wait;
    TickType_t t = pdMS_TO_TICKS(ms ? ms : 1);
    return t < wait ? t : wait;
}

static tango_vproc_t *vp_begin(const char *name)
{
    vp_caps_t caps;
    volume_start(name);                               /* load_volume (user_io.cpp 1577): Volume.dat, <core>_volume.cfg */
    int any = vp_probe(&link, tango_vol_corebyte(&VOL), &caps);   /* the SET_AFILTER probe carries the core volume */
    tango_vol_caps(&VOL, caps.has_afilter, caps.has_boost);       /* setFilter: no boost for the menu core or a core without it */
    caps.vol = tango_vol_corebyte(&VOL);
    vol_send();                                       /* send_volume: the FPGA powers up at full volume */
    if (!any) {                                       /* nothing filter-related for a core without the blocks */
        tango_debugf("video processing: the core has none of the blocks");
        return NULL;
    }
    tango_vproc_t *v = malloc(sizeof *v);
    if (!v) {
        tango_debugf("video processing: no memory for its state (%u bytes): the core runs without it", (unsigned)sizeof *v);
        return NULL;
    }
    int n = 0;
    uint8_t *ini = vp_load(&vpio, "Tango.ini", 8191, &n);   /* MiSTer.ini's place (cfg.cpp 567); its first 8 KB */
    vp_core_start_caps(v, &link, &vpio, name, ini, n, &caps);
    free(ini);
    VP = v;
    tango_debugf("video processing: scaler %d (flags %02x) gamma %d mask %d audio filter %d; H %s%s V %s%s gamma %d %s mask %d %s audio %d %s; %d skipped (memory)",
                 v->has_scaler, v->flags, v->has_gamma, v->has_mask, v->has_afilter,
                 v->flt[VP_HORZ].mode ? "" : "(off) ", v->flt[VP_HORZ].name, v->flt[VP_VERT].mode ? "" : "(off) ", v->flt[VP_VERT].name,
                 v->gamma[0], v->gamma + 1, v->mask[0], v->mask + 1, v->afilt[0], v->afilt + 1, v->nomem);
    return v;
}
static void vp_start(void) { CM.vp = vp_begin(CM.name); }
static void vp_end(tango_vproc_t *v) { if (v) { if (VP == v) VP = NULL; free(v); } }   /* the launcher: no page uses it */

static uint32_t pad_quiet_until;          /* ms: the pad's menu keys are ignored until then (raw_in) */
/* a REPORT snapshot in the Tango task: a pad plugged in or changed model gets its map; the rest drives the dialog */
/* The menu button of the pad on `port`: its system map config/inputs/input_<vid>_<pid>_v3.map (tango_menubtn.h), written by the launcher's
 * System Settings > Define joystick buttons; no file = Share + Options through the GET_DB9 combo, as ever.  While any connected pad has a
 * definition, the GET_DB9 combo (the FPGA's fixed Select + Start) opens nothing and every pad is read from its raw word. */
static void mb_load(int port)
{
    tango_mb_t d; uint32_t m[TANGO_PM_BTNS]; char name[40];
    MBC[port] = 0; SYSV[port] = 0; tango_mb_default(&d);
    if (pads[port].conn) {
        tango_mb_mapname(pads[port].vid, pads[port].pid, name, sizeof name);
        if (pm_load(name, m)) {
            if (tango_mb_from_map(&d, m)) MBC[port] = 1;
            if (tango_pm_sysmap_valid(m)) { SYSV[port] = 1; for (int i = 0; i < 12; i++) SYSM[port][i] = m[i]; }
        }
    }
    tango_core_set_known(&CM, port, SYSV[port] || (pads[port].conn && pad_is_ds4(pads[port].vid, pads[port].pid)));   /* a DualShock has the FPGA's decode: MiSTer's has_default_map; every other pad is judged by what the pad manager resolved (raw_in) */
    MBP[port] = d; memset(&MBD[port], 0, sizeof MBD[port]);
    tango_core_set_menu_bits(&CM, port, d.a, d.b);
    if (MBC[port]) { char nm[56]; tango_mb_name(&d, nm); tango_debugf("port %d: menu button %s (system map %s)", port, nm, name); }
}
static bool mb_custom(void) { return (pads[0].conn && MBC[0]) || (pads[1].conn && MBC[1]); }

/* ---- the pad manager's platform (tango_padmgr.c): malloc, the log, the core's map, Companion's pad table, the card files ---- */
extern const void *find_usb_gamepad_map(uint16_t vid, uint16_t pid, int version_optional);   /* companion/src/bl616/mcu_hw.c: the 1,681-entry table, in flash once */
static void *pmg_alloc(void *c, unsigned n) { (void)c; return malloc(n); }
static void pmg_release(void *c, void *p) { (void)c; free(p); }
static void pmg_log(void *c, const char *line) { (void)c; tango_debugf("%s", line); }
static void pmg_apply(void *c, int port) { (void)c; pm_apply(port); }
static int pmg_builtin(void *c, uint16_t vid, uint16_t pid, uint16_t ver, sdl_map_t *out)
{
    (void)c;
#ifdef TANGO_COSIM
    (void)vid; (void)pid; (void)ver; (void)out;       /* a TANGO_COSIM host build links no mcu_hw.c */
    return PM_NONE;
#else
    const void *e = find_usb_gamepad_map(vid, pid, (int)ver);
    return e ? pdb_from_companion(e, ver, out) : PM_NONE;
#endif
}
/* the MiSTer paths on the card: /linux/gamecontrollerdb/gamecontrollerdb_user.txt, then gamecontrollerdb.txt; one FIL, allocated only while a scan runs */
static FIL *pmg_fp;
static int pmg_open(void *c, int which)
{
    (void)c;
    if (!card_ok || pmg_fp) return 0;
    pmg_fp = malloc(sizeof *pmg_fp);
    if (!pmg_fp) return 0;
    if (f_open(pmg_fp, which ? "/sd/linux/gamecontrollerdb/gamecontrollerdb.txt" : "/sd/linux/gamecontrollerdb/gamecontrollerdb_user.txt",
               FA_OPEN_EXISTING | FA_READ) != FR_OK) { free(pmg_fp); pmg_fp = NULL; return 0; }
    return 1;
}
static int pmg_read(void *c, int which, char *buf, int cap)
{
    (void)c; (void)which;
    UINT br = 0;
    if (!pmg_fp || f_read(pmg_fp, buf, (UINT)cap, &br) != FR_OK) return -1;
    return (int)br;
}
/* ladder step 1: the decode the user defined for this very pad, config/inputs/pad_<vid>_<pid>_<ver>_v1.dec (tango_padlearn.c) */
static int pmg_dec_load(void *c, uint16_t vid, uint16_t pid, uint16_t ver, pd_table_t *out)
{
    (void)c;
    if (!card_ok) return 0;
    char name[40], p[96]; uint8_t buf[10 + 5 + 5 * PD_MAXENT]; FIL fl; UINT br = 0;
    pl_dec_name(vid, pid, ver, name, sizeof name);
    snprintf(p, sizeof p, "/sd/config/inputs/%s", name);
    if (f_open(&fl, p, FA_OPEN_EXISTING | FA_READ) != FR_OK) return 0;
    FRESULT r = f_read(&fl, buf, sizeof buf, &br);
    f_close(&fl);
    return r == FR_OK && pl_dec_load(buf, (int)br, vid, pid, ver, out);
}
static uint32_t pmg_now(void *c) { (void)c; return xTaskGetTickCount() * portTICK_PERIOD_MS; }
static void pmg_close(void *c, int which) { (void)c; (void)which; if (pmg_fp) { f_close(pmg_fp); free(pmg_fp); pmg_fp = NULL; } }

/* the pad manager, once per boot: the launcher's or the core's task starts it; the pads already plugged in get their turn at once */
static void raw_in(const rawsnap_t *s, uint32_t now_ms);
static void pm_start(int launcher)
{
    padmgr_platform_t P;
    memset(&P, 0, sizeof P);
    P.link = &link; P.alloc = pmg_alloc; P.release = pmg_release; P.log = pmg_log; P.builtin = pmg_builtin;
    P.dec_load = pmg_dec_load; P.file_open = pmg_open; P.file_read = pmg_read; P.file_close = pmg_close; P.now_ms = pmg_now;
    P.apply_map = launcher ? NULL : pmg_apply;
    P.core = launcher ? NULL : CM.name;
    PM = padmgr_new(&P);
    if (!PM) tango_debugf("pad manager: no memory; pads are handled as before it existed");
    if (PM) {   /* Swap Joysticks, persisted like the other settings (MiSTer keeps it in memory only: user_io.cpp 1838); sent with the first PAD_CTRL */
        uint8_t st[16];
        padmgr_set_swap(PM, c_load_cfg("padswap.cfg", st) && st[0] == 1);
    }
    padmgr_core_start(PM);                            /* DECODE_STATUS probe (new or old FPGA), PAD_CTRL */
    for (int p = 0; p < 2; p++) {
        rawsnap_t s; s.port = (uint8_t)p;
        s.flags = tango_hid_report(&link, (uint8_t)p, &s.vid, &s.pid, &s.raw);
        raw_in(&s, xTaskGetTickCount() * portTICK_PERIOD_MS);
    }
}

/* ---- the firmware entry points for the controller database ("Define buttons": the interface the menu calls).
 * The Define page builds a plsess_t, a capture function (padmgr_capture on tango_fw_padmgr() is the reference one) and calls
 * plsess_baseline / plsess_step / plsess_dpad / plsess_finish with tango_fw_plsess_files(). All of it runs in the Tango task. ---- */
struct padmgr *tango_fw_padmgr(void) { return PM; }

static FIL *dbo_fp, *dbn_fp;
static const char DB_DIR[] = "/sd/linux/gamecontrollerdb", DB_USER[] = "/sd/linux/gamecontrollerdb/gamecontrollerdb_user.txt",
                  DB_TMP[] = "/sd/linux/gamecontrollerdb/gamecontrollerdb_user.tmp";
static int pf_save_dec(void *c, const char *name, const uint8_t *data, int n)
{
    (void)c;
    if (!card_ok) return 0;
    char p[96]; FIL fl; UINT bw = 0;
    f_mkdir("/sd/config"); f_mkdir("/sd/config/inputs");
    snprintf(p, sizeof p, "/sd/config/inputs/%s", name);
    if (f_open(&fl, p, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) { tango_debugf("could not write %s", p); return 0; }
    FRESULT r = f_write(&fl, data, (UINT)n, &bw);
    f_close(&fl);
    tango_debugf("saved %s (%s)", p, r == FR_OK && (int)bw == n ? "ok" : "failed");
    return r == FR_OK && (int)bw == n;
}
static int pf_open_old(void *c)
{
    (void)c;
    if (!card_ok || dbo_fp) return 0;
    dbo_fp = malloc(sizeof *dbo_fp);
    if (dbo_fp && f_open(dbo_fp, DB_USER, FA_OPEN_EXISTING | FA_READ) == FR_OK) return 1;
    free(dbo_fp); dbo_fp = NULL; return 0;
}
static int pf_read(void *c, char *buf, int cap) { (void)c; UINT br = 0; return dbo_fp && f_read(dbo_fp, buf, (UINT)cap, &br) == FR_OK ? (int)br : 0; }
static void pf_close_old(void *c) { (void)c; if (dbo_fp) { f_close(dbo_fp); free(dbo_fp); dbo_fp = NULL; } }
static int pf_open_new(void *c)
{
    (void)c;
    if (!card_ok || dbn_fp) return 0;
    f_mkdir("/sd/linux"); f_mkdir(DB_DIR);
    dbn_fp = malloc(sizeof *dbn_fp);
    if (dbn_fp && f_open(dbn_fp, DB_TMP, FA_CREATE_ALWAYS | FA_WRITE) == FR_OK) return 1;
    free(dbn_fp); dbn_fp = NULL; return 0;
}
static int pf_write(void *c, const char *b, int n) { (void)c; UINT bw = 0; return dbn_fp && f_write(dbn_fp, b, (UINT)n, &bw) == FR_OK && (int)bw == n; }
static void pf_discard(void *c)
{
    (void)c;
    if (dbn_fp) { f_close(dbn_fp); free(dbn_fp); dbn_fp = NULL; }
    f_unlink(DB_TMP);
}
/* the new content replaces the file by copying the temporary file over it (no rename: the same f_ calls the rest of the firmware uses) */
static int pf_commit(void *c)
{
    if (!dbn_fp) return 0;
    f_close(dbn_fp); free(dbn_fp); dbn_fp = NULL;
    FIL *src = malloc(sizeof *src), *dst = malloc(sizeof *dst);
    int ok = 0;
    if (src && dst && f_open(src, DB_TMP, FA_OPEN_EXISTING | FA_READ) == FR_OK) {
        if (f_open(dst, DB_USER, FA_CREATE_ALWAYS | FA_WRITE) == FR_OK) {
            char buf[256]; UINT br = 0, bw = 0; ok = 1;
            while (ok && f_read(src, buf, sizeof buf, &br) == FR_OK && br) ok = f_write(dst, buf, br, &bw) == FR_OK && bw == br;
            f_close(dst);
        }
        f_close(src);
    }
    free(src); free(dst);
    f_unlink(DB_TMP);
    (void)c;
    return ok;
}
const plsess_files_t *tango_fw_plsess_files(void)
{
    static const plsess_files_t F = { NULL, pf_save_dec, pf_open_old, pf_read, pf_close_old, pf_open_new, pf_write, pf_commit, pf_discard };
    return &F;
}

/* Swap Joysticks: the core's joystick_0 / joystick_1 exchanged (PAD_CTRL bit 1, MiSTer joyswap). 0 from the setter = the FPGA has no such bit (old bitstream). */
int tango_fw_pad_swap_get(void) { return PM ? padmgr_swap(PM) : 0; }
int tango_fw_pad_swap_supported(void) { return PM && padmgr_swap_supported(PM); }
int tango_fw_pad_swap_set(int on)
{
    if (!PM) return 0;
    int r = padmgr_set_swap(PM, on);
    uint8_t st[16] = { 0 }; st[0] = on ? 1 : 0;
    if (card_ok) c_save_cfg(NULL, "padswap.cfg", st);
    return r;
}

/* a REPORT snapshot in the Tango task (launcher or core): the pad manager decides what the pad gets (a core map, or a quarantine blank);
 * the rest drives the Define-buttons dialog */
static void raw_in(const rawsnap_t *s, uint32_t now_ms)
{
    int p = s->port, conn = (s->flags >> (2 * p)) & 1;
    int changed = conn != pads[p].conn || (conn && (s->vid != pads[p].vid || s->pid != pads[p].pid));
    if (changed) {
        pads[p].conn = conn; pads[p].vid = s->vid; pads[p].pid = s->pid;
        tango_debugf("USB-A%d: %s %04x:%04x", p + 1, conn ? "pad" : "no pad", s->vid, s->pid);
        if (core_mode) mb_load(p);                    /* first: the pad manager's pm_apply reads the system map it finds */
    }
    if (PM) padmgr_snapshot(PM, p, s->flags, s->vid, s->pid, s->raw);
    if (PM && core_mode && conn) {                /* MiSTer's has_default_map for a pad that is not a DualShock: a decode that came from a definition, a database line or a fixed layout is a default map; the descriptor alone, nothing, or a pad we do not decode is not (input.cpp 3754) */
        int src = padmgr_port_src(PM, p);
        if (src) tango_core_set_known(&CM, p, SYSV[p] || (src != PRS_HEURISTIC && src != PRS_LEGACY && src != PRS_QUIET));
    }
    else if (changed && core_mode) pm_apply(p);
    if (!core_mode && PM) {   /* MiSTer: "This joystick is not defined" (input.cpp 3754, an Info() at the first mapped press of a pad with no map): a pad that only the descriptor (or nothing) decodes */
        static uint8_t told[2];
        int src = conn ? padmgr_port_src(PM, p) : 0;
        if (!conn) told[p] = 0;
        else if (src && !told[p]) {
            told[p] = 1;
            if (src == PRS_HEURISTIC || src == PRS_QUIET || src == PRS_LEGACY) tango_launcher_notice(&L, "Pad", "This joystick is not defined: Define joystick buttons");
        }
    }
    if (!core_mode) return;                       /* the launcher feeds the pad manager only; the core menu below is the core's */
    if (!conn) tango_core_pad_gone(&CM, p);
    else if (!(PM && padmgr_blocked(PM, p))) {
        int was = tango_core_capturing(&CM);
        tango_core_raw(&CM, p, s->vid, s->pid, s->raw, now_ms);
        if (mb_custom()) {                                          /* press = F12's press, release = its UPSTROKE (the OSD opens on the release) */
            int e = tango_mbd_edge(&MBD[p], MBP[p].a, MBP[p].b, s->raw, now_ms);
            if (e) { uint32_t m = e > 0 ? Q_MKDN : Q_MKUP; xQueueSendToBack(queue, &m, 0); }
        }
        tango_mbd_step(&ESCD[p], TANGO_MB_DEF_A, TANGO_MB_DEF_B, TANGO_MB_ESCAPE_MS, s->raw, now_ms);   /* armed; core_task's clock fires it (below) */
        /* A capture that ends on a button's release (Cross: the press that was just bound) must not also act in the menu: the GET_DB9
         * path posts B's Back on that same release, after this report.  The pad's own menu keys are ignored for a moment. */
        if (was && !tango_core_capturing(&CM)) pad_quiet_until = now_ms + 150;
    }
}

static void pm_drain_raw(void)
{
    rawsnap_t s;
    while (raw_q && xQueueReceive(raw_q, &s, 0) == pdTRUE) raw_in(&s, xTaskGetTickCount() * portTICK_PERIOD_MS);
}

/* keep FPGA Companion's OSD state in step: hid.c sends keys and USB pads to the menu only while it is visible */
static void sync_osd(void)
{
    static int shown = 0;
    int on = CM.mode != TANGO_CM_CLOSED;
    if (on != shown) { osd_enable(on ? OSD_VISIBLE : OSD_INVISIBLE); shown = on; }
}

static void core_task(void *p)
{
    (void)p;
    static const uint8_t PAL[16][3] = TANGO_PAL_INIT;
    card_take();
    char cfgname[48]; static uint8_t cfg[16];
    tango_core_cfgname(core_conf, cfgname, sizeof cfgname);
    bool have_cfg = c_load_cfg(cfgname, cfg);
    tango_osd_start(&L.osd, PAL);
    tango_core_init(&CM, &link, &L.osd, &L.br, &cplat, core_conf, have_cfg ? cfg : NULL);
    /* Heap margin (together with the mra_session_t zip-cache sharing elsewhere in this file):
     * vp_start()'s own ~16.5 KB tango_vproc_t (for a core with
     * TANGO_VIDEO_PROC=1, e.g. Galaga) must not be resident THROUGH the whole .mra ROM-0
     * auto-download below -- with it, the first deflated part's own 32 KB inflate window fails.
     * Calling it after the mra_pending block below is not enough: mra_autostart() only runs the
     * SIZING pass synchronously; the real streaming pass runs later, via dl_step() on each SDC
     * interrupt, well after this function returns. So this mirrors arc_pending_setup's own
     * pattern: if the ROM-0
     * download actually started (dl_is_mra true after mra_autostart()), defer vp_start() all the way to
     * dl_close()'s own mra branch (below, after the session is fully torn down, win or lose) via
     * vp_pending; otherwise (no .mra, or nothing to send) call it immediately. */
    if (!mra_pending[0]) vp_start();                    /* video_init / load_volume: filters, gamma, mask,
                                                          * audio filter -- the plain-core path */
    tango_osd_enable(&L.osd, 0);
    tango_debugf("core %s ready: config %s%s, menumask %04x", CM.name, cfgname, have_cfg ? "" : " (none)", CM.hdmask);
    /* The .mra auto-trigger, deferred here from tango_fw_boot (which only
     * recorded the path in mra_pending -- see its own comment there and mra_autostart's own header for
     * why) specifically so it runs AFTER card_take() just above: mra_autostart() never mounts the card
     * itself, it relies entirely on this task's own session-long mount, the same as c_load_file/pm_load/
     * every other core_task()-side file access already does. */
    /* An .mra (arcade) launch loads and starts directly, without the menu opening first (arcade games
     * only) -- captured here, BEFORE mra_pending[0] is cleared a few lines down, so the `if (launched)`
     * block further below can tell an .mra launch apart from a plain console one and skip the automatic
     * OSD-open for it specifically (see that block's own comment for the full reasoning). */
    bool mra_launch = mra_pending[0] != 0;
    if (mra_pending[0]) {
        /* Stash the .mra's own path/basename for DIP/cheat/NVRAM (mra_pending
         * itself is about to be cleared, and mra_autostart()'s own mra_s is freed again once the ROM
         * auto-download ends -- neither survives long enough on its own). Mirrors resolve_mra's/
         * mra_autostart's own "rel" convention (strip a leading "/sd/": zipfs's own open() adds it back). */
        const char *rel = !strncmp(mra_pending, "/sd/", 4) ? mra_pending + 4 : mra_pending;
        strncpy(arc_mra_path, rel, sizeof arc_mra_path - 1); arc_mra_path[sizeof arc_mra_path - 1] = 0;
        const char *base = strrchr(arc_mra_path, '/'); base = base ? base + 1 : arc_mra_path;
        char *dst = arc_name; int cap = (int)sizeof arc_name;
        const char *dot = strrchr(base, '.');
        size_t blen = (dot && !strcasecmp(dot, ".mra")) ? (size_t)(dot - base) : strlen(base);
        if ((int)blen >= cap) blen = (size_t)cap - 1;
        memcpy(dst, base, blen); dst[blen] = 0;
        arc_pending_setup = true;
        mra_autostart(mra_pending);
        mra_pending[0] = 0;
        /* no ROM auto-download ever started (refused/empty/not found/no <rom index="0">): dl_is_mra never
         * became true, so dl_close()'s own arc_setup hook will never run for this core -- do it here instead,
         * synchronously, right now (DL is guaranteed idle: nothing above ever opened it). If one DID
         * start, dl_is_mra is true here and this is deliberately skipped -- dl_close() picks it up once
         * that session's own SELECT(0) is actually on the wire (see arc_pending_setup's own comment). */
        if (arc_pending_setup && !dl_is_mra) { arc_pending_setup = false; arc_setup(); }
        /* vp_pending's own counterpart to the vp_start() skipped above: if the ROM-0 download genuinely
         * started, defer to dl_close(); otherwise (refused/empty/not found/no <rom index="0">, or
         * mra_autostart() itself never even opened), there is nothing to wait for -- call it now, the
         * same place a plain console core already gets it. */
        if (dl_is_mra) vp_pending = true; else vp_start();
    }
    pm_start(0);                                   /* the pad manager, and the pads already plugged in get their maps */
    /* A core the launcher started opens with its menu shown, so a game can be loaded at once. This is NOT MiSTer's behaviour. MiSTer's fpga_load_rbf closes
     * the OSD (fpga_io.cpp 428) and restarts Main (506, app_restart 620-641; main.cpp 73 user_io_init). Main then starts
     * at MENU_NONE1 (menu.cpp 239, 1600-1607), and MENU_NONE2 (1613-1619) opens the OSD only for the menu key, the menu
     * core (is_menu) or an MGL's file step. user_io_init (user_io.cpp 1452-) never opens it.
     * The closest MiSTer mechanism is used: one menu-key press, as menu_key_set(KEY_F12) does after the OSD unlock
     * code (menu.cpp 573-577, 4627). MENU_NONE2 then shows MENU_GENERIC_MAIN1 with menusub 0 (1622, 1655-1656), which
     * is tango_core_toggle: page 0 with its first row.
     * The menu stays closed after a power-up boot (that loads the launcher, which is always shown), after our own MCU
     * reset with a core running (no request: its game keeps playing), and
     * after System > Reboot (@menu: the launcher). MiSTer has no case like ours to copy for a launch with a game (an
     * MGL opens the OSD only for its file step: 1619, 1661, 2869); Tango has no MGL or recents yet.
     * Loading a file closes the menu, as MenuHide does (menu.cpp 2670 F, 2800 S): tango_coremenu.c.
     *
     * The arcade exception: an .mra-launched
     * arcade core does NOT get this treatment -- it loads and starts directly, without the menu
     * opening first. The general rule above (every
     * OTHER launched core: NES, TG16, Genesis, Atari2600, ...) still applies. mra_launch was captured
     * above, before mra_pending[0] was cleared, specifically so this one `if` can tell the two cases
     * apart -- skipping tango_core_toggle()/sync_osd() entirely here means `launched` never turns into
     * an open OSD for this core at all (tango_core_toggle_how()'s own body does nothing but flip
     * cm->mode and redraw, so skipping it has no side effect beyond the OSD staying closed).
     * Applies regardless of whether the ROM-0 auto-download itself
     * succeeds, fails, or was empty/refused: the rule is about arcade games,
     * not the download's own outcome -- a failure still leaves the user with the standing menu-key
     * combo (Start+Select/F12) to open the OSD by hand. */
    if (launched && !mra_launch) {
        CM.now_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
        tango_core_toggle(&CM);
        sync_osd();
        tango_debugf("launched from the launcher: %s's menu is open (page 0)", CM.name);
    } else if (launched) {
        tango_debugf("launched from the launcher: %s (arcade .mra): starting directly, menu stays closed", CM.name);
    }

    tango_ev_t held = TANGO_EV_NONE;
    TickType_t rep_due = 0, last_poll = xTaskGetTickCount();   /* rep_due: when the held key repeats next */
    const TickType_t POLL = pdMS_TO_TICKS(250);
    bool reboot_held = false; TickType_t reboot_t0 = 0;
    for (;;) {
        vol_tick(CM.name);                             /* a volume change is saved a second after the last one */
        /* the core's menumask while a page is open (menu.cpp 2640: MAIN3 compares it on every pass), keys or not */
        TickType_t tnow = xTaskGetTickCount();
        if (CM.mode == TANGO_CM_CLOSED) last_poll = tnow;
        else if (tnow - last_poll >= POLL) { tango_core_poll_mask(&CM); last_poll = tnow; }
        TickType_t wait = portMAX_DELAY;
        if (CM.mode != TANGO_CM_CLOSED) wait = POLL - (tnow - last_poll);
        if ((CM.mode == TANGO_CM_DEFINE || CM.mode == TANGO_CM_SAVING) && wait > pdMS_TO_TICKS(100))
            wait = pdMS_TO_TICKS(100);                 /* "Canceling" / the editor's capture timers / the end of "Saving..." */
        CM.now_ms = tnow * portTICK_PERIOD_MS;         /* MENU_SAVE_CHECK compares against it at the toggle */
        if (PM && padmgr_waiting(PM)) { padmgr_tick(PM); if (wait > pdMS_TO_TICKS(250)) wait = pdMS_TO_TICKS(250); }
        if (reboot_held) wait = pdMS_TO_TICKS(50);                     /* MiSTer's REPEATRATE (osd.h 12) */
        else if (held != TANGO_EV_NONE) {                             /* key repeat as the launcher */
            TickType_t r = (int32_t)(rep_due - tnow) > 0 ? rep_due - tnow : 0;
            if (r < wait) wait = r;
        }
        uint32_t msg;
        wait = vol_wait(wait);
        {   /* the escape's 3 s (tango_menubtn.h) */
            uint32_t tm = xTaskGetTickCount() * portTICK_PERIOD_MS;
            for (int p = 0; p < 2; p++) { int ms = tango_mbd_wait_ms(&ESCD[p], TANGO_MB_ESCAPE_MS, tm); if (ms >= 0 && pdMS_TO_TICKS(ms ? ms : 1) < wait) wait = pdMS_TO_TICKS(ms ? ms : 1); }
        }
        {   int ms = tango_core_wait_ms(&CM, xTaskGetTickCount() * portTICK_PERIOD_MS);      /* a long name scrolls in its row */
            if (ms >= 0 && pdMS_TO_TICKS(ms ? ms : 1) < wait) wait = pdMS_TO_TICKS(ms ? ms : 1); }
        if (xQueueReceive(queue, &msg, wait) == pdTRUE) {
            if (msg & Q_DL) { dl_step(); xSemaphoreGive(dl_sem); continue; }
            if (msg & Q_RAW) {
                rawsnap_t s;
                while (xQueueReceive(raw_q, &s, 0) == pdTRUE) raw_in(&s, xTaskGetTickCount() * portTICK_PERIOD_MS);
                sync_osd();
                continue;
            }
            if (msg & Q_CHAR) {
                if (tango_core_type(&CM, (char)(msg & 0xff), xTaskGetTickCount() * portTICK_PERIOD_MS)) sync_osd();
                continue;
            }
            if (msg & (Q_MKDN | Q_MKUP)) {                 /* the menu key's two edges (tango_menubtn.h tango_mkey_*): one rule for every source */
                int act = (msg & Q_MKDN) ? tango_mkey_press(&MK, CM.mode != TANGO_CM_CLOSED) : tango_mkey_release(&MK);
                if (!act) continue;                        /* closed + press, or the release of a press that closed the menu */
                msg = Q_TOGGLE | (msg & (Q_KBD | Q_HOW_CORES | Q_HOW_SYS));   /* from here as the toggle always was */
            }
            if ((msg & Q_PRESS) && !(msg & Q_KBD) && (int32_t)(pad_quiet_until - xTaskGetTickCount() * portTICK_PERIOD_MS) > 0) continue;   /* the release that ended a capture */
            if (CM.mode == TANGO_CM_DEFINE || tango_core_capturing(&CM)) {   /* Define buttons, an editor capture or Test: the pad is read raw (raw_in) */
                if (msg & Q_TOGGLE) {                      /* F12 = Clear all; the pad's combo is the dialog's own */
                    tango_core_menu_key(&CM, (msg & Q_KBD) != 0);
                    sync_osd();
                    continue;
                }
                if (msg & (Q_RELEASE | Q_REL_SEL)) { held = TANGO_EV_NONE; continue; }
                tango_ev_t kev = (tango_ev_t)(msg & 0xff);
                if (tango_core_capturing(&CM)) {
                    if (msg & Q_KBD) tango_core_define_key(&CM, kev == TANGO_EV_SELECT ? PMC_KEY_ENTER : kev == TANGO_EV_SPACE ? PMC_KEY_SPACE :
                                                                  (kev == TANGO_EV_BACK || kev == TANGO_EV_MENU) ? PMC_KEY_ESC : 0);
                } else tango_core_event(&CM, kev);        /* the alternative-buttons question: menu navigation */
                sync_osd();
                continue;
            }
            if (msg & Q_TOGGLE) {
                reboot_held = false; held = TANGO_EV_NONE;
                CM.now_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;   /* MENU_SAVE_CHECK looks at the time now */
                /* tango_core_toggle() closes the OSD directly, with no
                 * tango_core_event() call and so no TANGO_CM_DIP_SAVE -- the combo/F12 path arc_save_dip_cheat's
                 * own doc comment names. Save explicitly here instead if the DIP page was open. */
                                bool was_dip = CM.mode == TANGO_CM_DIP;
                tango_core_toggle_how(&CM, (msg & Q_HOW_CORES) ? TANGO_OPEN_CORES : (msg & Q_HOW_SYS) ? TANGO_OPEN_SYSTEM : TANGO_OPEN_PAGE0);
                sync_osd();
                if (was_dip) arc_save_dip_cheat();
                continue;
            }
            if (msg & (Q_RELEASE | Q_REL_SEL)) {
                held = TANGO_EV_NONE;
                if (reboot_held && (msg & (Q_REL_SEL | Q_KBD))) c_reboot(NULL, 0);   /* released: the menu core */
                continue;
            }
            if (CM.mode == TANGO_CM_CLOSED) continue;                    /* pad moves while the core plays */
            tango_ev_t ev = (tango_ev_t)(msg & 0xff);
            CM.now_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;   /* About's ticker starts counting at the press, not at the last wake-up */
            int r = tango_core_event(&CM, ev);
            if (VP) vp_poll(VP);                        /* an option may have changed the scanline level (set_vfilter(0)) */
            sync_osd();
            if (r == TANGO_CM_REBOOT) { reboot_held = true; reboot_t0 = xTaskGetTickCount(); held = TANGO_EV_NONE; continue; }
            /* The DIP page was left with dip_cur possibly changed -- save right
             * away (menu.cpp 3576-3611's own unconditional arcade_sw_save on every exit from this page);
             * arc_save_dip_cheat() itself resyncs arc_dip_cur from the OSD module's own copy first (its own
             * doc comment has the full reasoning, including the combo/F12 case below that also needs it) */
            if (r == TANGO_CM_DIP_SAVE) arc_save_dip_cheat();
            if (CM.mode != TANGO_CM_CLOSED && tango_core_repeats(&CM, ev)) { held = ev; rep_due = xTaskGetTickCount() + pdMS_TO_TICKS(TANGO_REPEAT_DELAY_MS); }
            else held = TANGO_EV_NONE;
        } else {
            {   /* Share + Options held 3 s opens a closed menu whatever the pads' menu buttons are: a console has no menu button on the box */
                uint32_t tm = xTaskGetTickCount() * portTICK_PERIOD_MS;
                for (int p = 0; p < 2; p++)
                    if (tango_mbd_poll(&ESCD[p], TANGO_MB_ESCAPE_MS, tm) && CM.mode == TANGO_CM_CLOSED) {
                        tango_mkey_swallow(&MK);           /* Share + Options is usually the menu key, still held: its release must not close what this opens */
                        uint32_t m = Q_TOGGLE; xQueueSendToBack(queue, &m, 0);
                    }
            }
            /* a file list's long name scrolls in its row (an early wake-up may be the only thing due) */
            if ((CM.mode == TANGO_CM_FILES || CM.mode == TANGO_CM_CORES || CM.mode == TANGO_CM_ABOUT) &&
                tango_core_tick(&CM, xTaskGetTickCount() * portTICK_PERIOD_MS) == TANGO_CM_REDRAWN) sync_osd();
            if (CM.mode == TANGO_CM_DEFINE || CM.mode == TANGO_CM_SAVING) {
                tango_core_tick(&CM, xTaskGetTickCount() * portTICK_PERIOD_MS);
            } else if (reboot_held) {
                unsigned ms = (unsigned)((xTaskGetTickCount() - reboot_t0) * portTICK_PERIOD_MS);
                if (tango_reboot_cold(ms)) c_reboot(NULL, 1);
            } else if (held != TANGO_EV_NONE && (int32_t)(xTaskGetTickCount() - rep_due) >= 0) {
                tango_core_event(&CM, held);
                rep_due = xTaskGetTickCount() + pdMS_TO_TICKS(TANGO_REPEAT_RATE_MS);
            }
        }
    }
}

bool tango_fw_try_core(void)
{
    if (!(L.osd.caps[0] == 'Z' && L.osd.caps[1] == 'O')) return false;   /* tango_fw_try_launcher probed CAPS */
    int n = tango_sys_read_confstr(&link, core_conf, sizeof core_conf);
    if (n <= 0 || !strncmp(core_conf, "MENU", 4)) return false;
    tango_debugf("Tango framework core: CONF_STR %d bytes", n);
    queue = xQueueCreate(16, sizeof(uint32_t));
    dl_sem = xSemaphoreCreateBinary();
    raw_q = xQueueCreate(16, sizeof(rawsnap_t));
    core_mode = true;
    active = true;
    xTaskCreate(core_task, "TangoCore", 4096, NULL, configMAX_PRIORITIES - 3, NULL);
    return true;
}

static void post(uint32_t m);
/* ---- keyboards (tango_kbd.c): KBD_REPORT in com_task ---- */
static tango_kbd_t kbds[2];
static uint8_t kbd_mods;                              /* the modifier bits held on any keyboard (HID bit 0-7): Alt / Win make Alt+Menu / Win+Menu */
static void kbd_change(void *c, uint8_t code, int press, int mod)
{
    (void)c;
    int open = osd_is_visible() || !core_mode;       /* the launcher is always shown */
    if (mod && code < 8) { if (press) kbd_mods |= (uint8_t)(1u << code); else kbd_mods &= (uint8_t)~(1u << code); }
    if (!mod && tango_kbd_is_menu_key(code)) {       /* the OSD key (F12, or the Application key): never to the core (user_io.cpp 4427-4441) */
        if (!core_mode) { if (press) post(Q_KBD | Q_PRESS | TANGO_EV_MENU); }   /* the launcher is always shown: it acts on the press */
        else if (press) post(Q_KBD | Q_MKDN);
        else {                                       /* a core: opens on the release (tango_mkey_*); the modifiers held then pick the page (menu.cpp 1621, 1650) */
            uint32_t how = (kbd_mods & 0x44) ? Q_HOW_CORES : (kbd_mods & 0x88) ? Q_HOW_SYS : 0;   /* LAlt RAlt / LGUI RGUI */
            post(Q_KBD | Q_MKUP | how);
        }
        return;
    }
    if (!open) {                                      /* send_keycode: PS/2 set 2 through FPGA Companion's path */
        if (press) kbd_tx_hid_ps2_make(code, (uint8_t)mod); else kbd_tx_hid_ps2_break(code, (uint8_t)mod);
        return;
    }
    if (mod) {                                        /* Alt / Win releases still reach the core (4436-4437) */
        if (!press && core_mode && tango_kbd_passthru_mod(code)) kbd_tx_hid_ps2_break(code, 1);
        return;
    }
    static const uint32_t EV[] = { 0, TANGO_EV_UP, TANGO_EV_DOWN, TANGO_EV_LEFT, TANGO_EV_RIGHT, TANGO_EV_SELECT,
                                   TANGO_EV_BACK, TANGO_EV_MENU, TANGO_EV_PGUP, TANGO_EV_PGDN, TANGO_EV_MINUS,
                                   TANGO_EV_PLUS, 0 /* TKEY_TOGGLE: handled above */, TANGO_EV_HOME, TANGO_EV_END, TANGO_EV_TAB,
                                   TANGO_EV_RECENT, TANGO_EV_SPACE };
    int k = tango_kbd_menu_key(code, press, 1);
    if (k > TKEY_NONE && k <= TKEY_SPACE && EV[k]) post(Q_KBD | Q_PRESS | EV[k]);
    if (!press) post(Q_KBD | Q_RELEASE);
    char ch = press ? tango_kbd_ascii(code) : 0;
    if (ch) post(Q_KBD | Q_CHAR | (uint8_t)ch);
}
static void kbd_reports(void)
{
    uint8_t rep[8];
    uint8_t fl = tango_hid_kbd_report(&link, 0, rep);
    if (fl & 0x10) tango_kbd_diff(&kbds[0], rep, kbd_change, NULL);
    if (fl & 0x20) { tango_hid_kbd_report(&link, 1, rep); tango_kbd_diff(&kbds[1], rep, kbd_change, NULL); }
}

/* com_task, every HID interrupt in core mode: both ports' HID REPORT (raw word, VID/PID, connect) to the Tango task */
void tango_fw_hid_reports(void)
{
    if (!active) return;
    kbd_reports();                                    /* a framework core or the launcher: keyboards on USB-A */
    if (!raw_q) return;
    for (int p = 0; p < 2; p++) {
        rawsnap_t s; s.port = (uint8_t)p;
        s.flags = tango_hid_report(&link, (uint8_t)p, &s.vid, &s.pid, &s.raw);
        padmgr_note_raw(PM, p, s.raw);                /* the menu word of a quarantined pad's neighbour (tango_fw_joystick16) */
        xQueueSendToBack(raw_q, &s, 0);
    }
    post(Q_RAW);
}

/* com_task: SDC interrupt (pending bit 3) = the core wants file bytes; the Tango task writes one chunk */
void tango_fw_sdc_irq(void)
{
    if (!core_mode || !queue) return;
    uint32_t m = Q_DL;
    xQueueSendToBack(queue, &m, pdMS_TO_TICKS(100));
    xSemaphoreTake(dl_sem, pdMS_TO_TICKS(1000));
}

static void post(uint32_t m)
{
    /* the FromISR form, as Companion's menu_notify: its callers include timer and USB callbacks */
    if (queue) xQueueSendToBackFromISR(queue, &m, NULL);
}

void tango_fw_joystick(int src, uint8_t state) { tango_fw_joystick16(src, state); }

void tango_fw_joystick16(int src, uint16_t state16)
{
    static tango_padkeys_t pk[2];
    {   /* GET_DB9 ORs both ports: with a quarantined pad plugged in, the menu word comes from the other pad's REPORT raw word instead */
        uint16_t w;
        if (PM && state16 != 0xFFFF && padmgr_menu_word(PM, &w)) state16 = w;
    }
    static bool armed[2] = { false, true };   /* USB pads report changes only: armed at once */
    if (!active || src < 0 || src > 1) return;
    /* an FPGA that doesn't serve HID reads 0xFF; a real pad can't press up and down together */
    if (state16 == 0xFFFF) { tango_padkeys_sync(&pk[src], 0); return; }
    if ((state16 & (TANGO_JOY_UP | TANGO_JOY_DOWN)) == (TANGO_JOY_UP | TANGO_JOY_DOWN)) return;
    /* the pad as MiSTer's OSD sees it (tango_padkeys_step: A, B on release, A+B = Backspace, X = Tab, L / R = minus / plus,
     * Select = recents on release; Select+Start is the menu key: the in-core OSD's toggle (menu-tango
     * docs/CORE-CONTRACT.md) and, in the launcher, MiSTer's Menu key (Cores <-> System Settings)) */
    tango_padkeys_out_t o;
    tango_padkeys_step(&pk[src], state16, &o);
    /* A game's buttons held when the OSD opens, or pressed while it is hidden, must not act when they go up in the menu (a B held
     * for "run" would close it at once): MiSTer clears osdbtn while the OSD is hidden (input.cpp 2412-2413) and drops a B release whose
     * press it did not see (2470-2471).  The combo's own edge is detected first; the buttons held then make no key. */
    if (core_mode && (o.combo || CM.mode == TANGO_CM_CLOSED)) tango_padkeys_sync(&pk[src], state16);
    /* the combo is the menu key: its press closes an open menu, its release opens a closed one (MiSTer's osd_event 1 and 2, input.cpp 3206-3215);
     * the launcher is always shown and acts on the press.  A core's other menu buttons come
     * from the raw word (core_task). */
    if (!core_mode || !mb_custom()) {
        if (o.combo) post(core_mode ? Q_MKDN : (Q_PRESS | TANGO_EV_MENU));
        if (o.combo_up && core_mode) post(Q_MKUP);
    }
    if (o.a_released) post(Q_REL_SEL);
    /* SNAC pads can read "pressed" before they settle: act only after one all-released state (L / R and the combo, which
     * a settling pad cannot fake, are not held back) */
    bool was_armed = armed[src];
    if (!armed[src] && (state16 & 0xFF) == 0) armed[src] = true;
    for (int i = 0; i < o.n; i++)
        if (was_armed || o.ev[i] == TANGO_EV_MINUS || o.ev[i] == TANGO_EV_PLUS) post(Q_PRESS | o.ev[i]);
    if (o.dir_released) post(Q_RELEASE);
}

void tango_fw_menu_event(unsigned long msg)
{
    if (!active) return;
    switch (msg) {
    case MENU_EVENT_UP:          post(Q_KBD | Q_PRESS | TANGO_EV_UP); break;
    case MENU_EVENT_DOWN:        post(Q_KBD | Q_PRESS | TANGO_EV_DOWN); break;
    case MENU_EVENT_LEFT:        post(Q_KBD | Q_PRESS | TANGO_EV_LEFT); break;
    case MENU_EVENT_RIGHT:       post(Q_KBD | Q_PRESS | TANGO_EV_RIGHT); break;
    case MENU_EVENT_PGUP:        post(Q_KBD | Q_PRESS | TANGO_EV_PGUP); break;
    case MENU_EVENT_PGDOWN:      post(Q_KBD | Q_PRESS | TANGO_EV_PGDN); break;
    case MENU_EVENT_SELECT:      post(Q_KBD | Q_PRESS | TANGO_EV_SELECT); break;
    case MENU_EVENT_BACK:        post(Q_KBD | Q_PRESS | TANGO_EV_BACK); break;
    case MENU_EVENT_KEY_RELEASE: post(Q_RELEASE | Q_KBD); break;
    case MENU_EVENT_TOGGLE:      /* F12 (hid.c INIFILE_OPTION_HOTKEY): Companion's own USB keyboards, never the FPGA ones above */
        post(core_mode ? (Q_TOGGLE | Q_KBD) : (Q_KBD | Q_PRESS | TANGO_EV_MENU)); break;
    default: break;              /* SYSTEM, USB/network notices; the launcher is always shown */
    }
}
