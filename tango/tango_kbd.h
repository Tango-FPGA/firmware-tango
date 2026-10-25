// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 the Tango authors
/*
 * tango_kbd.h -- a USB boot keyboard (HID KBD_REPORT) as MiSTer uses one: the menu's keys, and PS/2 for the core.
 *
 * Tango. Our own code, from MiSTer's behaviour (Main_MiSTer master 5a3a086):
 *   user_io.cpp 4424-4480  user_io_kbd: F12 (the OSD key) never reaches the core; while the OSD is open every key
 *                          goes to the menu only, except the release of Alt / Win (so they cannot stick); with the
 *                          OSD closed the core gets the key as PS/2 set 2 (send_keycode, 1944)
 *   menu.cpp 1332-1440     the menu's keys: F12 opens on its release (closed) and closes on its press (open); Esc
 *                          release = menu; Backspace release = back; Enter / Space / keypad Enter = select; the
 *                          arrows; Page Up / Page Down
 *   menu.cpp 5642-5668     letters and digits: jump / filter in a file list (GetASCIIKey)
 *   menu.cpp 1424-1434, 5558-5575   Tab, Home, End (file lists), the grave key (recents)
 * The PS/2 bytes for the core are FPGA Companion's (ps2helper.c, HID KEYBOARD), sent by the firmware.
 */
#ifndef TANGO_KBD_H
#define TANGO_KBD_H

#include <stdint.h>

typedef struct { uint8_t prev[8]; } tango_kbd_t;

/* one key change: `code` is the HID usage, or with mod = 1 the modifier bit 0-7 (ps2helper.c's convention) */
typedef void (*tango_kbd_cb)(void *ctx, uint8_t code, int press, int mod);
/* the changes between the last boot report and this one: releases first, then presses (modifiers, then keys);
 * a report with ErrorRollOver (usage 01) in a key slot is ignored. Returns the number of changes. */
int  tango_kbd_diff(tango_kbd_t *k, const uint8_t rep[8], tango_kbd_cb cb, void *ctx);

enum { TKEY_NONE = 0, TKEY_UP, TKEY_DOWN, TKEY_LEFT, TKEY_RIGHT, TKEY_SELECT, TKEY_BACK, TKEY_MENU, TKEY_PGUP,
       TKEY_PGDN, TKEY_MINUS, TKEY_PLUS, TKEY_TOGGLE, TKEY_HOME, TKEY_END, TKEY_TAB, TKEY_RECENT, TKEY_SPACE };
/* the menu key of a change (menu.cpp 1332-1440); osd_open picks F12's edge */
int  tango_kbd_menu_key(uint8_t usage, int press, int osd_open);
/* GetASCIIKey for the typing jump: '0'-'9', 'a'-'z', else 0 */
char tango_kbd_ascii(uint8_t usage);
/* 1 = the modifier bit is Alt or Win (released to the core even while the OSD is open) */
int  tango_kbd_passthru_mod(uint8_t bit);

#define TANGO_HID_F12 0x45
#define TANGO_HID_APP 0x65      /* the Application (context-menu) key: Tango's "Menu key" -- see tango_kbd_is_menu_key */
/* 1 = the usage opens and closes the OSD: F12, and the Application key (MiSTer's KEY_MENU comes from
 * the consumer page, which a boot keyboard report does not carry, and MiSTer passes the Application key to the core: Tango's cores are consoles) */
int  tango_kbd_is_menu_key(uint8_t usage);

#endif
