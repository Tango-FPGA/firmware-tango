// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 the Tango authors
/*
 * tango_paddb_builtin.h -- the compile-time pad table (companion/src/usb_controller_maps.h, generated from SDL's
 * database, USB devices only, 1,681 entries) as sdl_map_t. Tango, our own code.
 *
 * The table is NOT included here: the firmware already carries it once (mcu_hw.c find_usb_gamepad_map) and a second copy
 * would cost 200 KB of flash. The caller passes the entry it found; a host test includes the table itself.
 */
#ifndef TANGO_PADDB_BUILTIN_H
#define TANGO_PADDB_BUILTIN_H

#include "tango_paddb.h"

/* Convert one entry. `ver` is the pad's bcdDevice: the rank (PM_EXACT / PM_VER0 / PM_VIDPID) is the one Companion's
 * find_usb_gamepad_map implies (exact version, else version 0, else the first entry for the vendor:product). */
int pdb_from_companion(const void *e /* a UsbGamepadMap */, uint16_t ver, sdl_map_t *out);

#endif
