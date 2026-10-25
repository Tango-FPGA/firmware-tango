// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 the Tango authors
/*
 * tango_about.c -- see tango_about.h. Tango, our own code.
 */
#include <stdio.h>
#include <string.h>
#include "tango_about.h"

/* The credits are menu-tango's README's "Shipped in the BL616 firmware" / "Shipped as data" list in one breath, with the authors and
 * licences as that list states them (read from each component's own files). */
const char tango_about_ticker[] =
    "Tango FPGA is built on MiSTer's core interface and menu format and on the FPGA Companion firmware "
    "(Apache-2.0, Till Harbaum and contributors). It includes FreeRTOS (MIT), FatFs (ChaN), miniz (MIT), "
    "SDL_GameControllerDB (zlib), hdl-util/hdmi (MIT, Sameer Puri) and the Saira Condensed font "
    "(SIL Open Font License 1.1). The full list of credits and licences is in the Attributions of the Tango FPGA repository.";

/* the wordmark: "TANGO FPGA" in a 5 x 5 pixel font, one cell between letters, three between the words (55 cells = 660 px;
 * a cell is three TANGO_ICON_BRICK = 12 px).  menu-tango tools/osd-model/launcher_pages.py LOGO has the same rows. */
static const char *const LOGO[5] = {
    "#####  ###  #   #  ####  ###    ##### ####   ####  ### ",
    "  #   #   # ##  # #     #   #   #     #   # #     #   #",
    "  #   ##### # # # #  ## #   #   ####  ####  #  ## #####",
    "  #   #   # #  ## #   # #   #   #     #     #   # #   #",
    "  #   #   # #   #  ###   ###    #     #      ###  #   #"
};

void tango_about_version(char *out, int cap, const char *fw_id)
{
    /* g<hash>[-dirty] YYYY-MM-DD HH:MM */
    const char *sp = fw_id ? strchr(fw_id, ' ') : NULL;
    if (!sp || sp - fw_id < 2 || sp - fw_id > 24 || strlen(sp) < 11 || sp[5] != '-' || sp[8] != '-') {
        snprintf(out, (size_t)cap, "Tango FPGA (build unknown)");
        return;
    }
    snprintf(out, (size_t)cap, "Tango FPGA v%c%c%c%c%c%c (%.*s)", sp[3], sp[4], sp[6], sp[7], sp[9], sp[10], (int)(sp - fw_id), fw_id);
}

int tango_about_pos(int step)
{
    int w = TANGO_VX - TANGO_TX;
    int period = tango_text_width(tango_about_ticker) + w;
    return (int)(((long)w + (long)step * TANGO_ABOUT_STEP_PX) % period);
}

static void set(tango_row_t *r, int kind, int col, const char *label)
{
    r->kind = (uint8_t)kind; r->col = (uint8_t)col; r->label = label; r->value = NULL;
}

void tango_about_rows(tango_row_t *rows, char txt[2][64], const char *core_name, const char *core_ver, const char *fw_id)
{
    /* the strip: 33 pairs of the volume page's filled / empty cell = 660 px, as wide as the wordmark */
    static const char STRIP[] = "\x84\x85\x84\x85\x84\x85\x84\x85\x84\x85\x84\x85\x84\x85\x84\x85\x84\x85\x84\x85\x84\x85\x84\x85\x84\x85\x84\x85\x84\x85\x84\x85\x84\x85"
                                "\x84\x85\x84\x85\x84\x85\x84\x85\x84\x85\x84\x85\x84\x85\x84\x85\x84\x85\x84\x85\x84\x85\x84\x85\x84\x85\x84\x85\x84\x85\x84\x85";
    tango_about_version(txt[0], 64, fw_id);
    snprintf(txt[1], 64, "%.31s %.31s", core_name && core_name[0] ? core_name : "Core", core_ver && core_ver[0] ? core_ver : "(version unknown)");
    int i = 0;
    set(&rows[i++], TR_BLANK, 0, NULL);
    for (int k = 0; k < 5; k++) set(&rows[i++], TR_LOGO, 0, LOGO[k]);
    set(&rows[i++], TR_CENTER, TANGO_C_ACC, STRIP);
    set(&rows[i++], TR_BLANK, 0, NULL);
    set(&rows[i++], TR_CENTER, TANGO_C_TEXT, TANGO_ABOUT_WEB);
    set(&rows[i++], TR_BLANK, 0, NULL);
    set(&rows[i++], TR_CENTER, TANGO_C_TEXT, txt[0]);
    set(&rows[i++], TR_CENTER, TANGO_C_TEXT, txt[1]);
    set(&rows[i++], TR_BLANK, 0, NULL);
    set(&rows[i++], TR_SCROLL, 0, tango_about_ticker);
}

/* Misc. Options' HDMI line.  The picture is always 1280x720 (sys-tango tango_cfg.vh: TANGO_HDMI_HTOTAL / _VTOTAL change only the back
 * porches).  The pixel clock and the frame rate are NOT shown: the raster's totals are a build option of each core (defaults 1650 x 750,
 * 74.25 MHz, 60.0 Hz), a native-rate plan moves them at run time (and a retune moves the clock), and SYS 0x70's one reply byte reports
 * neither, so the BL616 cannot say a number without making it up.  What the reply does say:
 * bit 6 the user's choice (0 fixed, 1 native), bit 5 a native plan is in use, bit 2 a retune is pending. */
int tango_info_lines(char lines[2][64], uint8_t vs, const char *ip)
{
    int n = 0;
    if (ip && ip[0]) snprintf(lines[n++], 64, "IP: %.40s", ip);
    if (!(vs & 0x80)) snprintf(lines[n++], 64, "HDMI 1280x720");                                       /* no SYS 0x70 block: nothing more is known */
    else if (vs & 0x04) snprintf(lines[n++], 64, "HDMI 1280x720  retuning ...");
    else if (vs & 0x20) snprintf(lines[n++], 64, "HDMI 1280x720  native rate");
    else if (vs & 0x40) snprintf(lines[n++], 64, "HDMI 1280x720  native rate, no plan yet");
    else snprintf(lines[n++], 64, "HDMI 1280x720  fixed rate");
    return n;
}
