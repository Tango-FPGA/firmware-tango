// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 the Tango authors
/*
 * tango_fw.h -- the Tango launcher inside the FPGA Companion firmware (BL616 only).
 *
 * Tango. Our own code. The hooks the Companion files call; each is a no-op for a stock
 * FPGA Companion core, so the u8g2 menu keeps working for other people's cores.
 */
#ifndef TANGO_FW_H
#define TANGO_FW_H

#include <stdbool.h>
#include <stdint.h>

/* com_task, before sys_wait4fpga(): at power-on load /menu.fs (else /menu.bin) from the SD card over
 * JTAG; after one of our own MCU resets keep whatever core the FPGA runs. */
void tango_fw_boot(void);

/* com_task, after sys_wait4fpga() succeeded: if the core is a Tango launcher (CAPS 'Z''O' and CONF_STR
 * "MENU" or none) start the launcher and return true; the caller then only runs the IRQ loop. */
bool tango_fw_try_launcher(void);

/* com_task, when tango_fw_try_launcher() said no: a framework core (CAPS 'Z''O', CONF_STR not MENU) gets the
 * in-core OSD (Start+Select / F12); returns true, and the caller runs the IRQ loop with tango_fw_sdc_irq() */
bool tango_fw_try_core(void);

/* com_task, after every HID GET_DB9 in core mode: HID REPORT of both USB-A ports (Define buttons, pad maps) */
void tango_fw_hid_reports(void);

/* com_task: SYS IRQ_CTRL pending bit 3 (SDC) in core mode: one chunk of the file download */
void tango_fw_sdc_irq(void);

/* true while the launcher owns the menu input */
bool tango_fw_active(void);

/* input hooks: a pad state in MiSTer bit order (src 0 = FPGA DB9/SNAC, 1 = BL616 USB), a Companion
 * MENU_EVENT_* (keyboard) */
void tango_fw_joystick(int src, uint8_t state);
void tango_fw_joystick16(int src, uint16_t state);   /* GET_DB9 bytes 3 + 4: joy[15:0] (Start = 11, Select = 10) */
void tango_fw_menu_event(unsigned long msg);

/* mcu_hw_reset(): the next boot is a warm one (keep the FPGA's core) unless a cold one was requested */
void tango_fw_mark_reset(void);

/* companion/src/bl616/mcu_hw.c: the BL616's IPv4 address as text (a USB Ethernet adapter on its own USB host, or Wi-Fi), or
 * 0 = no address: Misc. Options shows an IP line only when there is one */
int mcu_hw_ip_string(char *buf, int cap);

/* controller database: the learn session's platform pieces and the swap bit;
 * nothing calls them yet */
#include "tango_padlearn.h"
struct padmgr *tango_fw_padmgr(void);
const plsess_files_t *tango_fw_plsess_files(void);
int tango_fw_pad_swap_get(void);
int tango_fw_pad_swap_set(int on);
int tango_fw_pad_swap_supported(void);

#endif
