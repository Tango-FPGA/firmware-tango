// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 the Tango authors
/*
 * pad_fixtures.h -- SYNTHETIC HID report descriptors for the controller-database tests (test_paddb.c, pd_vectors.c).
 *
 * Written from the public USB HID 1.11 item syntax and the HID Usage Tables (Generic Desktop 0x01, Simulation 0x02, Button 0x09,
 * Consumer 0x0C). They are NOT dumps of the named devices. Each one is the STRUCTURE of a family of real pads (where the buttons,
 * hat and axes sit and how wide they are) so the parser, the numbering and the decode are exercised on the shapes that matter:
 * report ids, a hat with and without a null state, signed 16-bit axes, a 10-bit axis that straddles a byte, a 4-way hat, d-pad as
 * Generic Desktop buttons, Start/Select usages out of descriptor order, a second report id, and devices that are not pads.
 * The DualShock 4 one is built from the byte map the FPGA already decodes (sys-tango tango_ds4_joy.v 19-23, calibrated on hardware).
 * Descriptors were assembled by hand; test_paddb.c prints each parsed layout so a reviewer can check the offsets against the
 * comments here.
 */
#ifndef PAD_FIXTURES_H
#define PAD_FIXTURES_H

#include <stdint.h>

#define UP(x)        0x05, (x)
#define UP16(lo, hi) 0x06, (lo), (hi)
#define USAGE(x)     0x09, (x)
#define UMIN(x)      0x19, (x)
#define UMAX(x)      0x29, (x)
#define COLL(x)      0xA1, (x)
#define ENDC         0xC0
#define LMIN(x)      0x15, (x)
#define LMIN16(l, h) 0x16, (l), (h)
#define LMAX(x)      0x25, (x)
#define LMAX16(l, h) 0x26, (l), (h)
#define RSIZE(x)     0x75, (x)
#define RCOUNT(x)    0x95, (x)
#define RID(x)       0x85, (x)
#define INPUT(x)     0x81, (x)
#define OUTPUT(x)    0x91, (x)

/* ---- 1. DualShock 4 shaped: report id 1, X Y Z Rz (8 bit), hat (4 bit, null state), 14 buttons, 6-bit vendor counter, Rx Ry (8 bit),
 *         54 vendor bytes.  bit offsets: id 0-7, X 8, Y 16, Z 24, Rz 32, hat 40-43, buttons 1-14 at 44-57, counter 58-63, Rx 64, Ry 72 */
static const uint8_t D_DS4[] = {
    UP(0x01), USAGE(0x05), COLL(0x01),
      RID(0x01),
      USAGE(0x30), USAGE(0x31), USAGE(0x32), USAGE(0x35), LMIN(0x00), LMAX16(0xFF, 0x00), RSIZE(8), RCOUNT(4), INPUT(0x02),
      USAGE(0x39), LMIN(0x00), LMAX(0x07), 0x35, 0x00, 0x46, 0x3B, 0x01, 0x65, 0x14, RSIZE(4), RCOUNT(1), INPUT(0x42),
      UP(0x09), UMIN(0x01), UMAX(0x0E), LMIN(0x00), LMAX(0x01), RSIZE(1), RCOUNT(14), INPUT(0x02),
      UP16(0x00, 0xFF), USAGE(0x20), RSIZE(6), RCOUNT(1), LMIN(0x00), LMAX(0x7F), INPUT(0x02),
      UP(0x01), USAGE(0x33), USAGE(0x34), LMIN(0x00), LMAX16(0xFF, 0x00), RSIZE(8), RCOUNT(2), INPUT(0x02),
      UP16(0x00, 0xFF), USAGE(0x21), RCOUNT(54), INPUT(0x02),
    ENDC
};

/* ---- 2. SNES-style USB pad: no report id, X Y (8 bit, 0..255, centre 127), 12 buttons, 4 pad bits. 4 bytes: X 0-7, Y 8-15, buttons 16-27 */
static const uint8_t D_SNES12[] = {
    UP(0x01), USAGE(0x04), COLL(0x01), COLL(0x00),
      USAGE(0x30), USAGE(0x31), LMIN(0x00), LMAX16(0xFF, 0x00), RSIZE(8), RCOUNT(2), INPUT(0x02),
      UP(0x09), UMIN(0x01), UMAX(0x0C), LMIN(0x00), LMAX(0x01), RSIZE(1), RCOUNT(12), INPUT(0x02),
      RSIZE(1), RCOUNT(4), INPUT(0x03),
    ENDC, ENDC
};

/* ---- 3. a hat + 12 buttons + 4 axes pad (the PlayStation-adapter / dual-action shape): X Y Z Rz at bytes 0-3, byte 4 = hat (low nibble,
 *         0..7, null 15) + buttons 1-4, byte 5 = buttons 5-12 */
static const uint8_t D_HAT12[] = {
    UP(0x01), USAGE(0x04), COLL(0x01),
      USAGE(0x30), USAGE(0x31), USAGE(0x32), USAGE(0x35), LMIN(0x00), LMAX16(0xFF, 0x00), RSIZE(8), RCOUNT(4), INPUT(0x02),
      USAGE(0x39), LMIN(0x00), LMAX(0x07), RSIZE(4), RCOUNT(1), INPUT(0x42),
      UP(0x09), UMIN(0x01), UMAX(0x0C), LMIN(0x00), LMAX(0x01), RSIZE(1), RCOUNT(12), INPUT(0x02),
    ENDC
};

/* ---- 4. an Xbox-shaped HID pad: X Y Rx Ry signed 16 bit (bytes 0-7), Z Rz 10 bit triggers (bits 64-73, 74-83; the second straddles
 *         bytes 9/10), 4 pad bits (84-87), hat 4 bit 1..8 with null 0 (bits 88-91) + 4 pad bits, buttons 1-10 at bits 96-105 */
static const uint8_t D_XBOXHID[] = {
    UP(0x01), USAGE(0x05), COLL(0x01),
      USAGE(0x30), USAGE(0x31), USAGE(0x33), USAGE(0x34), LMIN16(0x00, 0x80), LMAX16(0xFF, 0x7F), RSIZE(16), RCOUNT(4), INPUT(0x02),
      USAGE(0x32), USAGE(0x35), LMIN(0x00), LMAX16(0xFF, 0x03), RSIZE(10), RCOUNT(2), INPUT(0x02),
      RSIZE(4), RCOUNT(1), INPUT(0x03),
      USAGE(0x39), LMIN(0x01), LMAX(0x08), RSIZE(4), RCOUNT(1), INPUT(0x42),
      RSIZE(4), RCOUNT(1), INPUT(0x03),
      UP(0x09), UMIN(0x01), UMAX(0x0A), LMIN(0x00), LMAX(0x01), RSIZE(1), RCOUNT(10), INPUT(0x02),
      RSIZE(6), RCOUNT(1), INPUT(0x03),
    ENDC
};

/* ---- 5. two report ids in the pad's collection: id 3 = X Y (8 bit) + 8 buttons (3 bytes with the id); id 4 = four vendor bytes */
static const uint8_t D_TWOID[] = {
    UP(0x01), USAGE(0x05), COLL(0x01),
      RID(0x03),
      USAGE(0x30), USAGE(0x31), LMIN(0x00), LMAX16(0xFF, 0x00), RSIZE(8), RCOUNT(2), INPUT(0x02),
      UP(0x09), UMIN(0x01), UMAX(0x08), LMIN(0x00), LMAX(0x01), RSIZE(1), RCOUNT(8), INPUT(0x02),
      RID(0x04),
      UP16(0x00, 0xFF), USAGE(0x01), RSIZE(8), RCOUNT(4), INPUT(0x02),
    ENDC
};

/* ---- 6. a mouse: 3 buttons, X Y wheel relative */
static const uint8_t D_MOUSE[] = {
    UP(0x01), USAGE(0x02), COLL(0x01), USAGE(0x01), COLL(0x00),
      UP(0x09), UMIN(0x01), UMAX(0x03), LMIN(0x00), LMAX(0x01), RSIZE(1), RCOUNT(3), INPUT(0x02), RSIZE(5), RCOUNT(1), INPUT(0x01),
      UP(0x01), USAGE(0x30), USAGE(0x31), USAGE(0x38), LMIN(0x81), LMAX(0x7F), RSIZE(8), RCOUNT(3), INPUT(0x06),
    ENDC, ENDC
};

/* ---- 7. a boot keyboard: 8 modifier bits, a reserved byte, 6 key-code bytes */
static const uint8_t D_KBD[] = {
    UP(0x01), USAGE(0x06), COLL(0x01),
      UP(0x07), UMIN(0xE0), UMAX(0xE7), LMIN(0x00), LMAX(0x01), RSIZE(1), RCOUNT(8), INPUT(0x02),
      RCOUNT(1), RSIZE(8), INPUT(0x01),
      RCOUNT(6), RSIZE(8), LMIN(0x00), LMAX16(0xFF, 0x00), UP(0x07), UMIN(0x00), UMAX(0xFF), INPUT(0x00),
    ENDC
};

/* ---- 8. a consumer-control remote (not a pad): one 16-bit usage array */
static const uint8_t D_CONSUMER[] = {
    UP(0x0C), USAGE(0x01), COLL(0x01),
      UMIN(0x00), 0x2A, 0x3C, 0x02, LMIN(0x00), LMAX16(0x3C, 0x02), RSIZE(16), RCOUNT(1), INPUT(0x00),
    ENDC
};

/* ---- 9. a 4-way hat (0..3, null 15) + 8 buttons, no axes: byte 0 = hat nibble + 4 pad bits, byte 1 = buttons */
static const uint8_t D_HAT4[] = {
    UP(0x01), USAGE(0x04), COLL(0x01),
      USAGE(0x39), LMIN(0x00), LMAX(0x03), RSIZE(4), RCOUNT(1), INPUT(0x42),
      RSIZE(4), RCOUNT(1), INPUT(0x03),
      UP(0x09), UMIN(0x01), UMAX(0x08), LMIN(0x00), LMAX(0x01), RSIZE(1), RCOUNT(8), INPUT(0x02),
    ENDC
};

/* ---- 10. the d-pad as four Generic Desktop buttons (Up Down Right Left = 0x90 0x91 0x92 0x93), declared FIRST, then buttons 1-6 and
 *          Start (0x3D) / Select (0x3E) declared before button 5, 6: the evdev order is not the descriptor order.
 *          bits: dpad up/down/right/left 0-3, button 1-4 at 4-7, select 8, start 9, button 5, 6 at 10, 11, 4 pad bits */
static const uint8_t D_DPADBTN[] = {
    UP(0x01), USAGE(0x05), COLL(0x01),
      USAGE(0x90), USAGE(0x91), USAGE(0x92), USAGE(0x93), LMIN(0x00), LMAX(0x01), RSIZE(1), RCOUNT(4), INPUT(0x02),
      UP(0x09), UMIN(0x01), UMAX(0x04), RSIZE(1), RCOUNT(4), INPUT(0x02),
      UP(0x01), USAGE(0x3E), USAGE(0x3D), RSIZE(1), RCOUNT(2), INPUT(0x02),
      UP(0x09), UMIN(0x05), UMAX(0x06), RSIZE(1), RCOUNT(2), INPUT(0x02),
      RSIZE(1), RCOUNT(4), INPUT(0x03),
    ENDC
};

/* ---- 11. UNIVERSAL: every index a database line can name resolves. 32 buttons (bits 0-31), 8 axes X Y Z Rx Ry Rz slider dial 8 bit
 *          (bits 32-95), two hats 4 bit (96-99, 100-103), 0..7 with null 15. Used to run every entry of the built-in table. */
static const uint8_t D_UNIVERSAL[] = {
    UP(0x01), USAGE(0x05), COLL(0x01),
      UP(0x09), UMIN(0x01), UMAX(0x20), LMIN(0x00), LMAX(0x01), RSIZE(1), RCOUNT(32), INPUT(0x02),
      UP(0x01), USAGE(0x30), USAGE(0x31), USAGE(0x32), USAGE(0x33), USAGE(0x34), USAGE(0x35), USAGE(0x36), USAGE(0x37),
      LMIN(0x00), LMAX16(0xFF, 0x00), RSIZE(8), RCOUNT(8), INPUT(0x02),
      USAGE(0x39), USAGE(0x39), LMIN(0x00), LMAX(0x07), RSIZE(4), RCOUNT(2), INPUT(0x42),
    ENDC
};

/* ---- 12. a "Retro Controller" shape (the 0079:0011 family of the built-in table: its database line puts the d-pad on axes a3 and a4):
 *          X Y Z Rx Ry 8 bit at bytes 0-4 (a0..a4 in evdev order), buttons 1-10 at bits 40-49, 6 pad bits */
static const uint8_t D_RETRO5[] = {
    UP(0x01), USAGE(0x04), COLL(0x01), COLL(0x00),
      USAGE(0x30), USAGE(0x31), USAGE(0x32), USAGE(0x33), USAGE(0x34), LMIN(0x00), LMAX16(0xFF, 0x00), RSIZE(8), RCOUNT(5), INPUT(0x02),
      UP(0x09), UMIN(0x01), UMAX(0x0A), LMIN(0x00), LMAX(0x01), RSIZE(1), RCOUNT(10), INPUT(0x02),
      RSIZE(1), RCOUNT(6), INPUT(0x03),
    ENDC, ENDC
};

/* ---- 13. evdev ordering stress: Gamepad application; bits: Select (GD 0x3E) 0, Start (GD 0x3D) 1, buttons 1-9 at 2-10, buttons 14-18 at 11-15,
 *          d-pad Up/Down/Right/Left (GD 0x90-0x93) at 16-19, 4 pad bits.  evdev codes: buttons 1-16 = 0x130-0x13f (select 0x13a and start 0x13b
 *          fall between buttons 9 and 14), d-pad buttons 0x220-0x223 (up down left right), buttons 17, 18 = 0x2c0, 0x2c1 */
static const uint8_t D_ORDER[] = {
    UP(0x01), USAGE(0x05), COLL(0x01),
      USAGE(0x3E), USAGE(0x3D), LMIN(0x00), LMAX(0x01), RSIZE(1), RCOUNT(2), INPUT(0x02),
      UP(0x09), UMIN(0x01), UMAX(0x09), RSIZE(1), RCOUNT(9), INPUT(0x02),
      UMIN(0x0E), UMAX(0x12), RSIZE(1), RCOUNT(5), INPUT(0x02),
      UP(0x01), USAGE(0x90), USAGE(0x91), USAGE(0x92), USAGE(0x93), RSIZE(1), RCOUNT(4), INPUT(0x02),
      RSIZE(1), RCOUNT(4), INPUT(0x03),
    ENDC
};

#endif
