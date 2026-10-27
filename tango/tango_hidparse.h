// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 the Tango authors
/*
 * tango_hidparse.h -- HID report descriptor -> a pad's field layout.
 *
 * Tango. Our own code, written from the public USB HID 1.11 specification (short items, global and
 * local state, Input items, Push/Pop) and the HID Usage Tables 1.x (Generic Desktop, Simulation, Button pages).
 * It does not use companion/src/hidparser.c (a different job: that parser serves the BL616's own USB host).
 *
 * Input:  a report descriptor, as the FPGA host fetched it (GET_DESCRIPTOR type 0x22).
 * Output: for the first joystick / gamepad / multi-axis Application collection, the Input fields that matter for a
 *         pad: buttons, axes and hat switches, each with its bit offset inside the interrupt payload (the report-id
 *         byte counts as byte 0 when the report has an id), size and logical range, plus the ORDINALS a controller
 *         database line uses ("b3", "a2", "h0"): numbered the way Linux's evdev numbers them, which is what MiSTer's
 *         lookup and SDL's Linux lines assume (gamecontroller_db.cpp get_ctrl_index_maps):
 *           - buttons by ascending evdev key code, the joystick range (>= BTN_JOYSTICK) first and the codes below it last;
 *           - axes by ascending evdev ABS code among the codes below ABS_HAT0X (X Y Z Rx Ry Rz, then throttle, rudder, wheel,
 *             gas, brake); a pad with no Y axis simply has no gap: "a1" is the second axis that EXISTS;
 *           - hat switches by order of appearance.
 *         The evdev code of a HID usage follows drivers/hid/hid-input.c (not in this tree: [assumed] from memory, the
 *         constants are the public linux/input-event-codes.h); the claim is tested against the one entry whose layout we know
 *         independently (the DualShock 4, test_paddb.c).
 */
#ifndef TANGO_HIDPARSE_H
#define TANGO_HIDPARSE_H

#include <stdint.h>

#define HP_MAX_FIELDS  96
#define HP_MAX_RIDS    16
#define HP_MAX_BTN     48
#define HP_MAX_AXES    16
#define HP_MAX_HATS    2

enum { HPK_NONE = 0, HPK_BUTTON, HPK_AXIS, HPK_HAT };
enum { HPC_NONE = 0, HPC_GAMEPAD, HPC_MOUSE, HPC_KEYBOARD, HPC_OTHER };   /* what the device is */

typedef struct {
    uint8_t  kind;          /* HPK_ */
    uint8_t  rid;           /* report id (0: the report has none) */
    uint8_t  size;          /* bits */
    uint8_t  sgn;           /* logical minimum < 0: two's complement */
    uint16_t page, usage;
    uint16_t bit;           /* bit offset in the interrupt payload, report-id byte included */
    int32_t  lmin, lmax;    /* logical range */
    int32_t  code;          /* evdev key / ABS code (HPK_BUTTON / HPK_AXIS), the sort key */
} hp_field_t;

typedef struct {
    int        cls;                         /* HPC_ */
    int        app_page, app_usage;         /* of the Application collection used */
    int        nf;
    hp_field_t f[HP_MAX_FIELDS];            /* the chosen report's fields, in descriptor order */
    uint8_t    rid;                         /* the report id used (0: none, and then byte 0 is data) */
    uint16_t   bytes;                       /* payload length of that report, id byte included */
    int        nbtn, naxis, nhat;
    int8_t     bidx[HP_MAX_BTN];            /* "bN" -> index in f[] (-1 none) */
    int8_t     aidx[HP_MAX_AXES];           /* "aN" -> index in f[] */
    int8_t     hidx[HP_MAX_HATS];           /* "hN" -> index in f[] */
    int        nrid_other;                  /* other report ids with Input items in the same collection (ignored) */
} hp_layout_t;

/* Parse a report descriptor. Returns 1 when a pad (joystick / gamepad / multi-axis Application collection with at least
 * one button, hat or two axes) was found, else 0 with out->cls telling what the first Application collection was. */
int hp_parse(const uint8_t *d, int n, hp_layout_t *out);

/* the evdev code of a Button-page usage under an Application collection (joystick 4, gamepad 5, other) */
int hp_button_code(int app_usage, int page, int usage);
/* the evdev ABS code of an axis usage, -1 if it is not an axis Linux counts */
int hp_axis_code(int page, int usage);

#endif
