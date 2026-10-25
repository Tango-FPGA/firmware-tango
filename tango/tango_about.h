// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 the Tango authors
/*
 * tango_about.h -- the About page (MiSTer's About page, in Tango's own look and words).
 *
 * Tango. Our own code.  MiSTer's About shows a logo, the project's web address, "MiSTer v<yymmdd>", the
 * running core's name and version, and a licence text that scrolls along the bottom line (menu.cpp MENU_ABOUT1): the page's
 * structure and information are the parity target, nothing of MiSTer's artwork or wording is used.  Tango's page:
 *
 *    row  0   blank
 *    1-5      the wordmark "TANGO FPGA", a 5 x 5 pixel font: each pixel is three TANGO_ICON_BRICK glyphs (12 px) (TR_LOGO rows)
 *    6        a decorative strip of the volume page's segment glyphs
 *    7        blank
 *    8        TangoFPGA.com                      (the product's web address)
 *    9        blank
 *    10       Tango FPGA v<yymmdd> (g<hash>[-dirty])   the BL616 firmware build (mk_fwid.sh's id: the date is the
 *                                                 build's, the hash tells two builds of one day apart)
 *    11       <core name> <core version>         the core's CONF_STR name and V,<version> ("v<yymmdd>" = BUILD_DATE)
 *    12       blank
 *    13       the ticker: credits and licences   (TR_SCROLL, on the last line as MiSTer's; one step every TANGO_ABOUT_STEP_MS, wrapping)
 * No row is selectable: Select, Left, B and Menu all go back (the legend says so).
 *
 * Shared by the in-core System > About (tango_coremenu.c) and the launcher's System Settings > About (tango_launcher.c).
 * The ticker's position is a function of the time since the page opened, so a page rebuilt at any moment shows the
 * position it should; the caller redraws when tango_about_step changes.
 */
#ifndef TANGO_ABOUT_H
#define TANGO_ABOUT_H

#include <stdint.h>
#include "tango_layout.h"

#define TANGO_ABOUT_ROWS     14
#define TANGO_ABOUT_STEP_MS  60      /* the ticker moves one step every 60 ms ... */
#define TANGO_ABOUT_STEP_PX  3       /* ... by 3 px (50 px/s, about five characters a second) */
#define TANGO_ABOUT_WEB      "TangoFPGA.com"

/* the text of the ticker (ASCII only: the font has no other glyphs) */
extern const char tango_about_ticker[];

/* "Tango FPGA v<yymmdd> (g<hash>[-dirty])" from the firmware id ("g<hash>[-dirty] YYYY-MM-DD HH:MM"); NULL, "" or an id
 * that does not parse: "Tango FPGA (build unknown)" */
void tango_about_version(char *out, int cap, const char *fw_id);

/* ticker position (pixels, tango_page_t.scroll) after `step` steps: the text starts at the left edge when the page opens
 * (step 0), moves out to the left, then enters again from the right.  The page's `scroll` is this, for the step the rows were built for */
int tango_about_pos(int step);
/* steps since the page opened at t0, and ms to the next one */
static inline int tango_about_step(uint32_t t0, uint32_t now) { return (int)((now - t0) / TANGO_ABOUT_STEP_MS); }
static inline int tango_about_wait(uint32_t t0, uint32_t now) { return TANGO_ABOUT_STEP_MS - (int)((now - t0) % TANGO_ABOUT_STEP_MS); }

/* the 14 rows.  txt[0] and txt[1] (64 bytes each) hold the two text lines the rows point to.
 * core_ver NULL or "" = "(version unknown)". */
void tango_about_rows(tango_row_t *rows, char txt[2][64], const char *core_name, const char *core_ver, const char *fw_id);


/* ---- Misc. Options' Information block (as MiSTer's), shared by the launcher's and a core's page ----
 * What the BL616 really knows (nothing is invented: a line it cannot fill is left out):
 *  - IP: only when the BL616 has an IPv4 address (mcu_hw_ip_string: a USB Ethernet adapter or Wi-Fi; the product starts no Wi-Fi).
 *  - HDMI mode: "HDMI 1280x720" (the picture is always that), plus what SYS 0x70's reply says: "fixed rate", "native rate" (a plan is in
 *    use), "native rate, no plan yet", "retuning ...".  NO pixel clock or Hz: the raster's totals are a per-core build option and a
 *    native-rate plan moves them, and the link does not report them back (see tango_about.c).
 *  - The core's own mode (width x height, line kHz, frame Hz) is not on the link at all: no line.
 * vs = the SYS 0x70 reply (vp_vsync_read; 0 = the core has no such block).  Returns the number of lines written (0 to 2). */
int tango_info_lines(char lines[2][64], uint8_t vs, const char *ip);

#endif
