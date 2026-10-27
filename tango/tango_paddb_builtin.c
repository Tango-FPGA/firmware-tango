// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 the Tango authors
/*
 * tango_paddb_builtin.c -- see tango_paddb_builtin.h. Tango, our own code.
 */
#include <string.h>
#include <stdint.h>
/* only the struct is wanted: no section attributes, so an unreferenced table is dropped by the compiler */
#define CONTROLLERDB_ATTR
#define CONTROLLERDB_STR_ATTR
#if defined(__has_include)
#if __has_include("../companion/src/usb_controller_maps.h")
#include "../companion/src/usb_controller_maps.h"      /* the source tree and the host tests */
#else
#include "usb_controller_maps.h"                         /* the firmware build tree: companion/ is copied to its root (build.sh), src/ is on the include path */
#endif
#else
#include "../companion/src/usb_controller_maps.h"
#endif
#include "tango_paddb_builtin.h"

static void bind(sdl_map_t *m, int sn, int type, int idx, int hatmask, int inv)
{
    if (idx < 0) return;
    m->b[sn].type = (uint8_t)type; m->b[sn].idx = (uint8_t)idx; m->b[sn].hatmask = (uint8_t)hatmask; m->b[sn].inv = (uint8_t)(inv != 0);
}

static void bind_axis(sdl_map_t *m, int sn, int idx, int mode, int inv)
{
    if (idx < 0) return;
    int t = mode == CONTROLLERDB_AXIS_POSITIVE ? SB_AXIS_POS : mode == CONTROLLERDB_AXIS_NEGATIVE ? SB_AXIS_NEG : SB_AXIS;
    bind(m, sn, t, idx, 0, inv);
}

int pdb_from_companion(const void *ep, uint16_t ver, sdl_map_t *m)
{
    const UsbGamepadMap *e = (const UsbGamepadMap *)ep;
    memset(m, 0, sizeof *m);
    m->bus = 3; m->vid = e->vid; m->pid = e->pid; m->ver = e->version; m->has_ver = 1; m->plat = PL_LINUX;
    strncpy(m->name, e->name ? e->name : "", sizeof m->name - 1);
    bind(m, SN_A, SB_BTN, e->btn_a, 0, 0);   bind(m, SN_B, SB_BTN, e->btn_b, 0, 0);
    bind(m, SN_X, SB_BTN, e->btn_x, 0, 0);   bind(m, SN_Y, SB_BTN, e->btn_y, 0, 0);
    bind(m, SN_BACK, SB_BTN, e->btn_back, 0, 0); bind(m, SN_GUIDE, SB_BTN, e->btn_guide, 0, 0); bind(m, SN_START, SB_BTN, e->btn_start, 0, 0);
    bind(m, SN_LSHOULDER, SB_BTN, e->btn_leftshoulder, 0, 0); bind(m, SN_RSHOULDER, SB_BTN, e->btn_rightshoulder, 0, 0);
    bind(m, SN_LSTICK, SB_BTN, e->btn_leftstick, 0, 0); bind(m, SN_RSTICK, SB_BTN, e->btn_rightstick, 0, 0);
    bind(m, SN_LTRIGGER, SB_BTN, e->btn_lt, 0, 0); bind(m, SN_RTRIGGER, SB_BTN, e->btn_rt, 0, 0);
    bind(m, SN_DPUP, SB_BTN, e->btn_dpad_up, 0, 0); bind(m, SN_DPDOWN, SB_BTN, e->btn_dpad_down, 0, 0);
    bind(m, SN_DPLEFT, SB_BTN, e->btn_dpad_left, 0, 0); bind(m, SN_DPRIGHT, SB_BTN, e->btn_dpad_right, 0, 0);
    if (e->dpad_hat >= 0) {                                  /* hat directions: a button-free d-pad */
        if (e->dpad_hat_up)    bind(m, SN_DPUP,    SB_HAT, e->dpad_hat, e->dpad_hat_up, 0);
        if (e->dpad_hat_right) bind(m, SN_DPRIGHT, SB_HAT, e->dpad_hat, e->dpad_hat_right, 0);
        if (e->dpad_hat_down)  bind(m, SN_DPDOWN,  SB_HAT, e->dpad_hat, e->dpad_hat_down, 0);
        if (e->dpad_hat_left)  bind(m, SN_DPLEFT,  SB_HAT, e->dpad_hat, e->dpad_hat_left, 0);
    }
    bind_axis(m, SN_DPUP, e->dpad_axis_up, e->dpad_axis_up_mode, e->dpad_axis_up_invert);
    bind_axis(m, SN_DPDOWN, e->dpad_axis_down, e->dpad_axis_down_mode, e->dpad_axis_down_invert);
    bind_axis(m, SN_DPLEFT, e->dpad_axis_left, e->dpad_axis_left_mode, e->dpad_axis_left_invert);
    bind_axis(m, SN_DPRIGHT, e->dpad_axis_right, e->dpad_axis_right_mode, e->dpad_axis_right_invert);
    bind_axis(m, SN_LEFTX, e->axis_lx, e->axis_lx_mode, e->axis_lx_invert); bind_axis(m, SN_LEFTY, e->axis_ly, e->axis_ly_mode, e->axis_ly_invert);
    bind_axis(m, SN_RIGHTX, e->axis_rx, e->axis_rx_mode, e->axis_rx_invert); bind_axis(m, SN_RIGHTY, e->axis_ry, e->axis_ry_mode, e->axis_ry_invert);
    if (e->btn_lt < 0) bind_axis(m, SN_LTRIGGER, e->axis_lt, e->axis_lt_mode, e->axis_lt_invert);
    if (e->btn_rt < 0) bind_axis(m, SN_RTRIGGER, e->axis_rt, e->axis_rt_mode, e->axis_rt_invert);
    if (e->version >= 0x8000 && e->version != ver) return PM_NONE;       /* kernel-driver numbering: exact version only (as tango_paddb.c) */
    return e->version == ver ? PM_EXACT : e->version == 0 ? PM_VER0 : PM_VIDPID;
}
