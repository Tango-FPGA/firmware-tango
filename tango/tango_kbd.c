// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 the Tango authors
/*
 * tango_kbd.c -- see tango_kbd.h. Tango, our own code.
 */
#include <string.h>
#include "tango_kbd.h"

static int has(const uint8_t *keys, uint8_t k) { for (int i = 0; i < 6; i++) if (keys[i] == k) return 1; return 0; }

int tango_kbd_diff(tango_kbd_t *k, const uint8_t rep[8], tango_kbd_cb cb, void *ctx)
{
    for (int i = 2; i < 8; i++) if (rep[i] == 0x01) return 0;     /* ErrorRollOver: a phantom state */
    int n = 0;
    uint8_t om = k->prev[0], nm = rep[0];
    /* releases */
    for (int b = 0; b < 8; b++) if ((om >> b & 1) && !(nm >> b & 1)) { cb(ctx, (uint8_t)b, 0, 1); n++; }
    for (int i = 2; i < 8; i++) if (k->prev[i] > 1 && !has(rep + 2, k->prev[i])) { cb(ctx, k->prev[i], 0, 0); n++; }
    /* presses */
    for (int b = 0; b < 8; b++) if (!(om >> b & 1) && (nm >> b & 1)) { cb(ctx, (uint8_t)b, 1, 1); n++; }
    for (int i = 2; i < 8; i++) if (rep[i] > 1 && !has(k->prev + 2, rep[i])) { cb(ctx, rep[i], 1, 0); n++; }
    memcpy(k->prev, rep, 8);
    return n;
}

int tango_kbd_is_menu_key(uint8_t u) { return u == TANGO_HID_F12 || u == TANGO_HID_APP; }

int tango_kbd_menu_key(uint8_t u, int press, int osd_open)
{
    if (tango_kbd_is_menu_key(u)) return (osd_open ? press : !press) ? TKEY_TOGGLE : TKEY_NONE;   /* 1332-1342 */
    if (!press) {
        if (u == 0x29) return TKEY_MENU;                  /* KEY_ESC | UPSTROKE (1394-1395) */
        if (u == 0x2A) return TKEY_BACK;                  /* KEY_BACKSPACE | UPSTROKE (1401-1403) */
        return TKEY_NONE;
    }
    switch (u) {
    case 0x28: case 0x58: return TKEY_SELECT;             /* Enter, keypad Enter (1404-1407) */
    case 0x2C: return TKEY_SPACE;                         /* Space: Select too (1405), but Define buttons skips with it (input.cpp 2991) */
    case 0x52: return TKEY_UP;
    case 0x51: return TKEY_DOWN;
    case 0x50: return TKEY_LEFT;
    case 0x4F: return TKEY_RIGHT;
    case 0x4A: return TKEY_HOME;                          /* KEY_HOME, KEY_END, KEY_TAB: file lists (5558-5575) */
    case 0x4D: return TKEY_END;
    case 0x2B: return TKEY_TAB;
    case 0x35: return TKEY_RECENT;                        /* KEY_GRAVE (1434) */
    case 0x4B: return TKEY_PGUP;
    case 0x4E: return TKEY_PGDN;
    case 0x2D: case 0x56: return TKEY_MINUS;              /* KEY_MINUS, KEY_KPMINUS (1430-1433) */
    case 0x2E: case 0x57: return TKEY_PLUS;               /* KEY_EQUAL, KEY_KPPLUS (1426-1429) */
    default: return TKEY_NONE;
    }
}

char tango_kbd_ascii(uint8_t u)
{
    if (u >= 0x04 && u <= 0x1D) return (char)('a' + (u - 0x04));
    if (u >= 0x1E && u <= 0x26) return (char)('1' + (u - 0x1E));
    if (u == 0x27) return '0';
    return 0;
}

int tango_kbd_passthru_mod(uint8_t bit) { return bit == 2 || bit == 3 || bit == 6 || bit == 7; }   /* LAlt LGUI RAlt RGUI */
