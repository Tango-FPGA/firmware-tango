/*
  gowin.c

  Modified for Tango, 2026: gowin_last_done / gowin_last_status record what
  gowin_writeSRAM_postproc found (its result was ignored, so a load that left the FPGA
  unconfigured still returned true); gowin_status_* keep the status register at each step;
  gowin_reload_first makes a GW5A reload its flash image before the erase (see gowin_reload_flash);
  gowin_diag measures every upload (IDCODE, bytes and CRC-32 of what went out on TDI, duration,
  longest gap between two chunks); gowin_probe_preamble() reads the status after the first 4 KB / 64 KB;
  the .bin path reads into a static, cache-line aligned buffer.
  Apache-2.0 as the original.
*/


#include <string.h>   // for memXXX()

#include "gowin.h"
#include "../debug.h"
#include "../jtag.h"
#include "../spi.h"     // for freetos includes

#ifdef ENABLE_JTAG
#include <ff.h>

// idcode is stored globally so the FPGA routines can address the right FPGA
static uint32_t idcode = 0;

static void gowin_printStatusReg(uint32_t status) {
    if (status & GOWIN_STATUS_CRC_ERROR)            gowin_debugf("Bit 0: CRC ERROR detected");
    if (status & GOWIN_STATUS_BAD_COMMAND)          gowin_debugf("Bit 1: Bad command received");
    if (status & GOWIN_STATUS_ID_VERIFY_FAILED)     gowin_debugf("Bit 2: ID verification failed");
    if (status & GOWIN_STATUS_TIMEOUT)              gowin_debugf("Bit 3: Timeout occurred");
    if (status & GOWIN_STATUS_AUTO_BOOT_2ND_FAIL)   gowin_debugf("Bit 4: Auto boot 2nd failed");
    if (status & GOWIN_STATUS_MEMORY_ERASE)         gowin_debugf("Bit 5: Memory erase in progress");
    if (status & GOWIN_STATUS_PREAMBLE)             gowin_debugf("Bit 6: Preamble detected");
    if (status & GOWIN_STATUS_SYSTEM_EDIT_MODE)     gowin_debugf("Bit 7: System edit mode active");
    if (status & GOWIN_STATUS_PRG_SPIFLASH_DIRECT)  gowin_debugf("Bit 8: Programming SPI flash directly");
    if (status & GOWIN_STATUS_AUTO_BOOT_1ST_FAILED) gowin_debugf("Bit 9: Auto boot 1st failed");
    if (status & GOWIN_STATUS_NON_JTAG_CNF_ACTIVE)  gowin_debugf("Bit 10: Non-JTAG configuration active");
    if (status & GOWIN_STATUS_BYPASS)               gowin_debugf("Bit 11: Bypass mode enabled");
    if (status & GOWIN_STATUS_I2C_SRAM_F)           gowin_debugf("Bit 12: I2C_SRAM_F");
    if (status & GOWIN_STATUS_DONE_FINAL)           gowin_debugf("Bit 13: Done final");
    if (status & GOWIN_STATUS_SECURITY_FINAL)       gowin_debugf("Bit 14: Security final");
    if (status & GOWIN_STATUS_ENCRYPTED_FORMAT)     gowin_debugf("Bit 15: ENCRYPTED_FORMAT");
    if (status & GOWIN_STATUS_KEY_IS_RIGHT)         gowin_debugf("Bit 16: KEY_IS_RIGHT");
    if (status & GOWIN_STATUS_SSPI_MODE)            gowin_debugf("Bit 17: SSPI_MODE");
    if (status & GOWIN_STATUS_SER_CRC_DONE)         gowin_debugf("Bit 18: Serial CRC done");
    if (status & GOWIN_STATUS_SER_CRC_ERR)          gowin_debugf("Bit 19: Serial CRC error");
    if (status & GOWIN_STATUS_SER_ECC_CORR)         gowin_debugf("Bit 20: ECC corrected");
    if (status & GOWIN_STATUS_SER_ECC_UNCORR)       gowin_debugf("Bit 21: ECC uncorrectable");
    if (status & GOWIN_STATUS_SER_RUNNING)          gowin_debugf("Bit 22: Serial running");
    if (status & GOWIN_STATUS_CPU_BUS_WIDTH_0)      gowin_debugf("Bit 23: CPU_BUS_WIDTH_0");
    if (status & GOWIN_STATUS_CPU_BUS_WIDTH_1)      gowin_debugf("Bit 24: CPU_BUS_WIDTH_1");
#ifndef TANG_MEGA138KPRO
    if (status & GOWIN_STATUS_SYNC_DET_TERY_0)      gowin_debugf("Bit 25: SYNC_DET_TERY_0");
    if (status & GOWIN_STATUS_SYNC_DET_TERY_1)      gowin_debugf("Bit 26: SYNC_DET_TERY_1");
    if (status & GOWIN_STATUS_DECOMP_FAIL)          gowin_debugf("Bit 27: Decompression failed");
    if (status & GOWIN_STATUS_MFG_DONE)             gowin_debugf("Bit 28: Manufacturing done");
    if (status & GOWIN_STATUS_INIT)                 gowin_debugf("Bit 29: Initialization complete");
    if (status & GOWIN_STATUS_WAKEUP)               gowin_debugf("Bit 30: Wakeup signal");
    if (status & GOWIN_STATUS_AUTO_ERASE)           gowin_debugf("Bit 31: Auto erase enabled");
#endif
}

static uint32_t gowin_readStatusReg(void) {
  uint32_t status = jtag_command_u08_read32(GOWIN_COMMAND_STATUS);
  // gowin_printStatusReg(status);
  return status;
}

static void gowin_gw2a_force_state(void) {
  /* undocumented sequence but required when
   * flash failure
   */
  uint32_t state =  gowin_readStatusReg();
  if ((state & GOWIN_STATUS_CRC_ERROR) == 0)
    return;
  jtag_command_u08(GOWIN_COMMAND_CONFIG_DISABLE);
  jtag_command_u08(0);
  jtag_command_u08_read32(GOWIN_COMMAND_IDCODE);
  state = gowin_readStatusReg();
  jtag_command_u08(GOWIN_COMMAND_CONFIG_DISABLE);
  jtag_command_u08(0);
  state = gowin_readStatusReg();
  jtag_command_u08_read32(GOWIN_COMMAND_IDCODE);
  jtag_command_u08(GOWIN_COMMAND_CONFIG_ENABLE);
  jtag_command_u08(GOWIN_COMMAND_CONFIG_DISABLE);
  jtag_command_u08(GOWIN_COMMAND_NOOP);
  jtag_command_u08_read32(GOWIN_COMMAND_IDCODE);
  jtag_command_u08(GOWIN_COMMAND_NOOP);
  jtag_command_u08_read32(GOWIN_COMMAND_IDCODE);
}

static bool gowin_pollFlag(uint32_t mask, uint32_t value) {
  uint32_t status;
  int timeout = 0;
  do {
    status = gowin_readStatusReg();
    if (timeout == 1000){  // TODO: was 100000000
      gowin_debugf("pollFlag(): timeout");
      gowin_printStatusReg(status);
      return false;
    }
    timeout++;
  } while ((status & mask) != value);
  
  return true;
}

static bool gowin_enableCfg(void) {
  jtag_command_u08(GOWIN_COMMAND_CONFIG_ENABLE);
  return gowin_pollFlag(GOWIN_STATUS_SYSTEM_EDIT_MODE, GOWIN_STATUS_SYSTEM_EDIT_MODE);
}

static bool gowin_disableCfg(void) {
  jtag_command_u08(GOWIN_COMMAND_CONFIG_DISABLE);
  jtag_command_u08(GOWIN_COMMAND_NOOP);
  return gowin_pollFlag(GOWIN_STATUS_SYSTEM_EDIT_MODE, 0);
}

// prepare SRAM upload
static bool gowin_eraseSRAM(void) {
  uint32_t status;
  jtag_command_u08_read32(GOWIN_COMMAND_USERCODE);

  // Clearing if failed loading
  status = gowin_readStatusReg();

  if ((idcode == IDCODE_GW5AST138)||(idcode == IDCODE_GW5A25)) {
    if ((status & GOWIN_STATUS_DONE_FINAL) == 0) {
      gowin_debugf("FPGA REINIT");
      jtag_command_u08(GOWIN_COMMAND_REINIT);
      jtag_clk_us(10000);
    }
  }
  // mandatory for GW5A to avoid SRAM clear errors
  if ((idcode == IDCODE_GW5AT60) || (idcode == IDCODE_GW5AST138) || (idcode == IDCODE_GW5A25)) {
      gowin_debugf("FPGA REINIT");
      jtag_command_u08(GOWIN_COMMAND_REINIT);
      jtag_clk_us(10000);
    }

  // Clearing Status Code Errors
  if (idcode != IDCODE_GW2AR18) {
    status = gowin_readStatusReg();
    bool auto_boot_2nd_fail = (status & GOWIN_STATUS_AUTO_BOOT_2ND_FAIL) == GOWIN_STATUS_AUTO_BOOT_2ND_FAIL;
    bool is_timeout = (status & GOWIN_STATUS_TIMEOUT) == GOWIN_STATUS_TIMEOUT;
    bool bad_cmd = (status & GOWIN_STATUS_BAD_COMMAND) == GOWIN_STATUS_BAD_COMMAND;
    bool id_verify_failed = (status & GOWIN_STATUS_ID_VERIFY_FAILED) ==  GOWIN_STATUS_ID_VERIFY_FAILED;
    if (is_timeout || auto_boot_2nd_fail || bad_cmd || id_verify_failed) {
    gowin_debugf("Clearing status errors by FPGA RELOAD");
    gowin_printStatusReg(status);
    jtag_command_u08(GOWIN_COMMAND_NOOP);
    jtag_command_u08(GOWIN_COMMAND_CONFIG_ENABLE);
    jtag_command_u08(GOWIN_COMMAND_RECONFIG);
    jtag_command_u08(GOWIN_COMMAND_NOOP);
    jtag_clk_us(100000);
    jtag_command_u08(GOWIN_COMMAND_CONFIG_DISABLE);
    jtag_command_u08(GOWIN_COMMAND_NOOP);
    jtag_clk_us(100000);
    }
  }

  if(idcode == IDCODE_GW2AR18) {
    gowin_gw2a_force_state();

    if(!gowin_enableCfg()) {
      status = gowin_readStatusReg();
      gowin_printStatusReg(status);
      gowin_debugf("Failed to enable config");
      return false;
     }
  } else {
      jtag_command_u08(GOWIN_COMMAND_CONFIG_ENABLE);
      // no status polling !
   }

  jtag_command_u08(GOWIN_COMMAND_ERASE_SRAM);
  jtag_command_u08(GOWIN_COMMAND_NOOP);
  jtag_clk_us(10000); // wait for erase to complete
#ifdef TANG_MEGA138KPRO
  jtag_clk_us(40000);
#endif

  if(!gowin_pollFlag(GOWIN_STATUS_MEMORY_ERASE, GOWIN_STATUS_MEMORY_ERASE)) {
    status = gowin_readStatusReg();
    gowin_printStatusReg(status);
    gowin_debugf("Failed to trigger SRAM erase");
    return false;
  }

  jtag_command_u08(GOWIN_COMMAND_XFER_DONE);
  jtag_command_u08(GOWIN_COMMAND_NOOP);
  if(!gowin_disableCfg()) {
    status = gowin_readStatusReg();
    gowin_printStatusReg(status);
    gowin_debugf("Failed to disable config");
    return false;
  }

  return true;
}

static void gowin_writeSRAM_prepare(void) {
  jtag_command_u08(GOWIN_COMMAND_CONFIG_ENABLE); // config enable

  /* UG704 3.4.3 */
  jtag_command_u08(GOWIN_COMMAND_INIT_ADDR); // address initialize

  /* 2.2.6.4 */
  jtag_command_u08(GOWIN_COMMAND_XFER_WRITE); // transfer configuration data

  /* finally enter shift DR state */
  jtag_enter_shiftDR();
}

#ifndef MCU_HW_JTAG_GPIO_OUT_MODE
static void gowin_writeSRAM_transfer(uint8_t *data, uint16_t len, bool last) {
  jtag_shiftDR_part(data, NULL, 8*len, last);
}
#endif

bool gowin_last_done = false;          // Tango: DONE_FINAL after the last SRAM write
uint32_t gowin_last_status = 0;       // Tango: the status register it was read from

// Tango: the status register at each step of the last upload, for the launcher's failure note
uint32_t gowin_status_before = 0, gowin_status_reload = 0, gowin_status_erase = 0;
bool gowin_reload_first = false;      // Tango: set by the launcher around a launch (not at power-up)

// Tango: which step inside gowin_upload_core_bin/_fs failed last -- "" on success or before any upload
// has run. gowin_upload_core() returns a single
// bool (TANGO_LOAD_FAILED, -2, at every caller above this file), which otherwise collapses every one
// of these into one undifferentiated code: a card/file-open problem, "FPGA not detected" (an IDCODE
// mismatch), either SRAM erase failing, or an actual mid-transfer SD-read/JTAG failure all look
// identical from tango_fw.c's own summary line without this. Set right before each such return,
// cleared only by gowin_upload_core() at the top of a fresh attempt -- never read as meaningful except
// immediately after gowin_upload_core() returns false.
char gowin_fail_step[80] = "";

// Tango: make a launch look like the power-up load, which works on hardware.
// At power-up the FPGA runs its flash image (JTAG pins dedicated) and the status register is clean
// (0x00026020 on the Console 138K: DONE, SECURITY, MEMORY_ERASE, SSPI). At a launch it runs the menu
// core, loaded over JTAG, with the JTAG pins as regular IO and Auto_Boot_1st/2nd_Fail set
// (0x00026230); gowin_eraseSRAM then takes its "clearing status errors" branch (a RECONFIG inside
// CONFIG_ENABLE, 2 x 100 ms without TCK) and the bitstream never reached DONE (on hardware:
// 0x00020230 after the write, no CRC error). So, first:
//   Reload 0x3C + Noop 0x02 = read the bitstream from flash into SRAM (UG704-1.0.4E p.20 "Reload 0x3C");
//   openFPGALoader does the same for every GW5A SRAM load (src/gowin.cpp 384-394: reset() = RELOAD,
//   NOOP [267-271], then 1,000,000 TCK in Run-Test-Idle); UG290-2.7.6E p.40: with the JTAG pins or
//   JTAGSEL_N multiplexed as GPIO, "a reprogram instruction is required if you want to reconfigure".
// then wait until the flash image reports DONE with no Auto_Boot failure (or 3 s pass).
static void gowin_reload_flash(void) {
  uint32_t st = gowin_readStatusReg();
  gowin_debugf("Tango: RELOAD from the FPGA's flash first (status %08lx)", st);
  jtag_command_u08(GOWIN_COMMAND_RECONFIG);
  jtag_command_u08(GOWIN_COMMAND_NOOP);
  TickType_t t0 = xTaskGetTickCount();
  uint32_t last = 0xdeadbeef;
  bool left_done = false;
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(10));
    st = gowin_readStatusReg();
    if (st != last) { gowin_debugf("Tango: reload status %08lx after %lu ms", st, (unsigned long)(xTaskGetTickCount() - t0)); last = st; }
    bool valid = st != 0 && st != 0xffffffff;
    if (valid && !(st & GOWIN_STATUS_DONE_FINAL)) left_done = true;       // the old design is gone
    bool clean = valid && (st & GOWIN_STATUS_DONE_FINAL) &&
                 !(st & (GOWIN_STATUS_AUTO_BOOT_1ST_FAILED | GOWIN_STATUS_AUTO_BOOT_2ND_FAIL));
    // the flash image runs: DONE again with no auto-boot failure (DONE only counts once it has
    // dropped, or after 100 ms if the reload was too quick to see it drop)
    if (clean && (left_done || xTaskGetTickCount() - t0 > pdMS_TO_TICKS(100))) break;
    if (xTaskGetTickCount() - t0 > pdMS_TO_TICKS(3000)) { gowin_debugf("Tango: the flash image never reported DONE"); break; }
  }
  gowin_status_reload = st;
}

// ---------------------------------------------------------------- Tango: upload instrumentation
gowin_diag_t gowin_diag;
extern uint64_t tango_time_us(void);          // tango/tango_fw.c (BL616 mtimer)
static uint32_t crc_table[256];
static void crc_init(void) {
  if (crc_table[1]) return;
  for (uint32_t i = 0; i < 256; i++) {
    uint32_t c = i;
    for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
    crc_table[i] = c;
  }
}
static uint64_t diag_last_us;
static void diag_begin(void) {
  crc_init();
  gowin_diag.bytes = 0; gowin_diag.crc = 0xFFFFFFFFu; gowin_diag.max_gap_us = 0; gowin_diag.ms = 0;
  diag_last_us = 0;
}
// every byte that goes out on TDI passes here (same bit order as the file: crc32(file) on a PC matches)
static void diag_bytes(const uint8_t *d, unsigned n) {
  uint32_t c = gowin_diag.crc;
  for (unsigned i = 0; i < n; i++) c = crc_table[(c ^ d[i]) & 0xff] ^ (c >> 8);
  gowin_diag.crc = c; gowin_diag.bytes += n;
  uint64_t t = tango_time_us();
  if (diag_last_us && t - diag_last_us > gowin_diag.max_gap_us) gowin_diag.max_gap_us = (uint32_t)(t - diag_last_us);
  diag_last_us = t;
}
static void diag_end(TickType_t ticks) { gowin_diag.crc ^= 0xFFFFFFFFu; gowin_diag.ms = ticks; }

static bool gowin_writeSRAM_postproc(__attribute__((unused)) uint32_t checksum) {
  jtag_command_u08(GOWIN_COMMAND_CONFIG_DISABLE);
  jtag_command_u08(GOWIN_COMMAND_NOOP);

  uint32_t status_reg = gowin_readStatusReg();
  status_reg = gowin_readStatusReg(); // read twice to get updated status
  gowin_last_status = status_reg;
  gowin_last_done = (status_reg & GOWIN_STATUS_DONE_FINAL) != 0;
  if(!(status_reg & GOWIN_STATUS_DONE_FINAL)) {
    gowin_printStatusReg(status_reg);
    fatal_debugf("Failed to write SRAM");
    return false;
  }
  uint32_t usercode = jtag_command_u08_read32(GOWIN_COMMAND_USERCODE);
  gowin_debugf("SRAM successfully written, Usercode=0x%08lx", usercode);
  return true;
}

void gowin_fpgaReset(void) {
    gowin_debugf("FPGA RECONFIG");

    jtag_command_u08(GOWIN_COMMAND_RECONFIG);
    jtag_command_u08(GOWIN_COMMAND_NOOP);
    jtag_clk_us(100000);
}

static inline uint8_t reverse_byte(uint8_t byte) {
  byte = ((byte & 0x55) << 1) | ((byte & 0xaa) >> 1);
  byte = ((byte & 0x33) << 2) | ((byte & 0xcc) >> 2);
  return ((byte & 0x0f) << 4) | ((byte & 0xf0) >> 4);
}

static bool gowin_open(void) {
  idcode = jtag_open();
  gowin_diag.idcode = idcode;                   // Tango
  gowin_debugf("FPGA detected: %08lx", idcode);

  return(idcode == IDCODE_GW2AR18  || 
         idcode == IDCODE_GW5AT60  || 
         idcode == IDCODE_GW5A25  ||
         idcode == IDCODE_GW5AST138);
}

// try to open "core.bin" on the sd card and try to transfer it via JTAG to
// the FPGA
static bool gowin_upload_core_bin(const char *name) {
  // get file size
  FILINFO fno;
  if (f_stat(name, &fno) != FR_OK) {
    gowin_debugf("Unable to determine length of %s", name);
    snprintf(gowin_fail_step, sizeof gowin_fail_step, "f_stat(%s) failed -- card or file problem", name);
    return false;
  }

  // try to open core.bin
  FIL fil;
  if(f_open(&fil, name, FA_OPEN_EXISTING | FA_READ) != FR_OK) {
    gowin_debugf("Opening %s failed", name);
    snprintf(gowin_fail_step, sizeof gowin_fail_step, "f_open(%s) failed -- card or file problem", name);
    return false;
  }

  if(!gowin_open()) {
    gowin_debugf("FPGA not detected");
    snprintf(gowin_fail_step, sizeof gowin_fail_step, "FPGA not detected (IDCODE %08lx did not match)", (unsigned long)idcode);
    jtag_close();
    f_close(&fil);
    return false;
  }
  
  // measure total download time
  TickType_t ticks = xTaskGetTickCount();
  
  gowin_status_before = gowin_readStatusReg();        // Tango
  if (gowin_reload_first && (idcode == IDCODE_GW5AST138 || idcode == IDCODE_GW5AT60 || idcode == IDCODE_GW5A25))
    gowin_reload_flash();                               // Tango: a launch starts from the flash image
  gowin_debugf("=== Erase SRAM ===");
  if(!gowin_eraseSRAM()) {
    // Tango: gowin_eraseSRAM() already logged which inner step failed (enable/disable config, trigger
    // erase) with the status register; this says WHICH of the two calls on GW5AST138 it was (every
    // return false in this function collapses to the SAME TANGO_LOAD_FAILED/-2 at every caller above
    // this file, so each step needs to be unambiguous on its own -- see gowin_fail_step, set right
    // below, which carries this detail up to there).
    gowin_debugf("Failed to erase SRAM (1st erase)");
    snprintf(gowin_fail_step, sizeof gowin_fail_step, "1st SRAM erase failed");
    jtag_close();
    f_close(&fil);
    return false;
  }

  if (idcode == IDCODE_GW5AST138) {
    gowin_status_erase = gowin_readStatusReg();       // Tango
    gowin_debugf("=== 2nd Erase SRAM ===");
    if(!gowin_eraseSRAM()) {
      gowin_debugf("Failed to erase SRAM (2nd erase, GW5AST138)");
      snprintf(gowin_fail_step, sizeof gowin_fail_step, "2nd SRAM erase failed (GW5AST138)");
      jtag_close();
      f_close(&fil);
      return false;
    }
  }
 
  gowin_status_erase = gowin_readStatusReg();         // Tango: after the (last) erase
  gowin_debugf("=== Load SRAM ===");
  gowin_writeSRAM_prepare();
    
  FRESULT fr;
  // Tango: a static, cache-line aligned buffer (SD DMA target) instead of 512 bytes of task stack
  static uint8_t buffer[512] __attribute__((aligned(64)));
  UINT bytesRead;
  uint32_t total = 0;
  diag_begin();

#ifdef MCU_HW_JTAG_GPIO_OUT_MODE
  mcu_hw_jtag_enter_gpio_out_mode();
#endif
  
  do {
    if((fr = f_read(&fil, buffer, sizeof(buffer), &bytesRead)) == FR_OK) {
      total += bytesRead;
      diag_bytes(buffer, bytesRead);

#ifndef MCU_HW_JTAG_GPIO_OUT_MODE
      for(UINT i=0;i<bytesRead;i++) buffer[i] = reverse_byte(buffer[i]);
      gowin_writeSRAM_transfer(buffer, bytesRead, total >= fno.fsize);
#else
      mcu_hw_jtag_writeTDI_msb_first_gpio_out_mode(buffer, bytesRead, total >= fno.fsize);
#endif
    }
  } while(fr == FR_OK && bytesRead && total < fno.fsize);

#ifdef MCU_HW_JTAG_GPIO_OUT_MODE
  mcu_hw_jtag_exit_gpio_out_mode();

  // send TMS 1/0 to return into RUN-TEST/IDLE
  mcu_hw_jtag_tms(1, 0b01, 2);    // normally this happens in gowin_writeSRAM_transfer/jtag_shiftDR_part
#endif  

  if(fr != FR_OK) {
    fatal_debugf("Binary download failed after %lu of %lu bytes", total, (unsigned long)fno.fsize);
    snprintf(gowin_fail_step, sizeof gowin_fail_step, "mid-transfer SD-read/JTAG failure after %lu of %lu bytes",
             (unsigned long)total, (unsigned long)fno.fsize);
    jtag_close();
    f_close(&fil);
    return false;
  }

  // don't set checksum
  gowin_writeSRAM_postproc(0xffffffff);

  gowin_debugf("Read %lu of %lu bytes", total, (unsigned long)fno.fsize);
  
  ticks = xTaskGetTickCount() - ticks;
  diag_end(ticks);
  gowin_debugf("Download time: %lu.%03lu seconds", ticks/1000, ticks%1000);
  gowin_debugf("Tango: %lu bytes on TDI, crc32 %08lx, longest gap between chunks %lu us",
               (unsigned long)gowin_diag.bytes, (unsigned long)gowin_diag.crc, (unsigned long)gowin_diag.max_gap_us);
  
  jtag_close();
  f_close(&fil);

  return true;
}

// try to open on the sd card and try to transfer it via JTAG into the FPGA
static bool gowin_upload_core_fs(const char *name) {  
  // try to open core.fs
  FIL fil;
  if(f_open(&fil, name, FA_OPEN_EXISTING | FA_READ) != FR_OK) {
    gowin_debugf("%s not found", name);
    return false;
  }
    
  // transmit core via JTAG
  if(!gowin_open()) {
    gowin_debugf("FPGA not detected");    
    jtag_close();
    f_close(&fil);
    return false;
  }
  
  // measure total download time
  TickType_t ticks = xTaskGetTickCount();
  
  gowin_status_before = gowin_readStatusReg();        // Tango
  if (gowin_reload_first && (idcode == IDCODE_GW5AST138 || idcode == IDCODE_GW5AT60 || idcode == IDCODE_GW5A25))
    gowin_reload_flash();                               // Tango: a launch starts from the flash image
  gowin_debugf("=== Erase SRAM ===");
  if(!gowin_eraseSRAM()) {
    gowin_debugf("Failed to erase SRAM");
    jtag_close();
    f_close(&fil);
    return false;
  }
  
  gowin_status_erase = gowin_readStatusReg();         // Tango: after the (last) erase
  gowin_debugf("=== Load SRAM ===");
  gowin_writeSRAM_prepare();
  
  // =================== parse the fs file =================
  // these are rather huge (>7MB for the GW2AR-18) and mainly contain
  // ASCII encoded binary data with some text header
  uint8_t buffer[128];
  UINT bytesRead;
  FRESULT fr;
  uint32_t total = 0;
  bool header_done = false;
  bool parse_error = false;
  uint16_t used = 0;
  
  // --------- parse the .fs header -----------
  do {
    if((fr = f_read(&fil, buffer+used, sizeof(buffer)-used, &bytesRead)) == FR_OK) {
      total += bytesRead;
      
      // check if the line starts with a binary digit
      if(buffer[0] == '0' || buffer[0] == '1') {
	gowin_debugf("header done");
	used += bytesRead;
	header_done = true;
      } else {
	// the buffer should start with "//" and there should be a newline in this buffer
	if(buffer[0] != '/' || buffer[1] != '/')
	  parse_error = true;
	else {	    
	  uint8_t *nl = memchr(buffer, '\n', sizeof(buffer));
	  if(!nl) parse_error = true;  // that would not happen with a valid fs file
	  else {
	    // there's a complete header line in the buffer. Terminate it
	    *nl = '\0';
	    // line may have DOS line endings. In that case terminate earlier
	    if(*(nl-1) == '\r') *(nl-1) = '\0';
	    
	    gowin_debugf("header: %s", buffer);
	    
	    // shift unused part of buffer down
	    memmove(buffer, nl+1, sizeof(buffer)-(nl-buffer+1));
	    used = sizeof(buffer)-(nl-buffer+1);
	  }
	}	    
      }
    }
  } while(fr == FR_OK && bytesRead && !header_done && !parse_error);
  
  // TODO: check header values to e.g. make sure the bitstream is for the
  // correct FPGA type
    
  if(fr != FR_OK || !header_done || parse_error) {
    gowin_debugf("Header parsing failed");
    jtag_close();
    f_close(&fil);
    return false;
  }
  
  // --------- parse the .fs binary data -----------
  
  gowin_debugf("Parsing and uploading binary bitstream");
    
  uint8_t data[512];  // one line holds up to 430 bytes
  uint8_t byte = 0, bit = 0;
  diag_begin();                                       // Tango
  uint32_t total_data = 0, line_data = 0;
  uint16_t line = 0;
  
#ifdef MCU_HW_JTAG_GPIO_OUT_MODE
  mcu_hw_jtag_enter_gpio_out_mode();
#endif
  
  do {
    // collect all 0/1 bits
    for(int i=0;i<used && !parse_error;i++) {
      if(buffer[i] == '0' || buffer[i] == '1') {
#ifdef MCU_HW_JTAG_GPIO_OUT_MODE
	// in gpio out mode the bit order is different
	byte = (byte << 1)|((buffer[i] == '1')?1:0);
#else
	byte = (byte >> 1)|((buffer[i] == '1')?0x80:0);
#endif
	
	bit = (bit+1)&7;
	if(!bit) {
	  // got a complete byte
	  data[line_data++] = byte;
	  total_data++;
	  byte = 0;
	}
      } else {
	// the digits should always come in multiple of 8
	if(bit) {
	  fatal_debugf("incomplete byte");
	  parse_error = true;
	} else if(buffer[i] == '\n') {
	  
	  // gowin_debugf("line %d: %ld (total %ld, used %d)", line, line_data, total_data, used);
	  // hexdump(data, line_data);
	  diag_bytes(data, line_data);                  // Tango
#ifndef MCU_HW_JTAG_GPIO_OUT_MODE  
	  gowin_writeSRAM_transfer(data, line_data, line>100 && line_data == 2);
#else
	  mcu_hw_jtag_writeTDI_msb_first_gpio_out_mode(data, line_data, line>100 && line_data == 2);
#endif
	  
	  line_data = 0;
	  line++;
	}
      }
    }
    
    if(!parse_error) {
      // refill the buffer
      if((fr = f_read(&fil, buffer, sizeof(buffer), &bytesRead)) == FR_OK) {
	total += bytesRead;
	used = bytesRead;
      }
    }
  } while(fr == FR_OK && bytesRead && !parse_error);
  
#ifdef MCU_HW_JTAG_GPIO_OUT_MODE
  mcu_hw_jtag_exit_gpio_out_mode();

  // send TMS 1/0 to return into RUN-TEST/IDLE
  mcu_hw_jtag_tms(1, 0b01, 2);    // normally this happens in gowin_writeSRAM_transfer/jtag_shiftDR_part
#endif
  
  if((fr != FR_OK) || parse_error) {
    fatal_debugf("Binary download failed after %lu bytes", total);
    jtag_close();
    f_close(&fil);
    return false;	
  }
  
  gowin_debugf("Total data transferred: %ld bytes", total_data);

  // TODO: Figure out where the checksum is supposed to come from. The checksum openFPGAloader
  // downloads is not the one mentioned in the .fs header.
  gowin_writeSRAM_postproc(0xffffffff);
  
  gowin_debugf("Read %lu bytes", total);
  
  ticks = xTaskGetTickCount() - ticks;
  diag_end(ticks);                                     // Tango
  gowin_debugf("Download time: %lu.%03lu seconds", ticks/1000, ticks%1000);
  
  jtag_close();
  f_close(&fil);
  return true;
}

// Tango: diagnosis after a failed load. Two short loads of the file's first 4 KB and 64 KB (each after
// the normal erase) with the status register read right after the data, still in edit mode: does
// the FPGA see the bitstream's preamble (bit 6)? Then the flash image is reloaded, so the FPGA is left
// as at power-on. Only the .bin (raw) format.
static bool probe_one(FIL *fil, uint32_t n, uint32_t *st) {
  if (f_lseek(fil, 0) != FR_OK) return false;
  if (!gowin_eraseSRAM()) return false;
  if (idcode == IDCODE_GW5AST138 && !gowin_eraseSRAM()) return false;
  gowin_writeSRAM_prepare();
  static uint8_t pbuf[512] __attribute__((aligned(64)));
  uint32_t sent = 0; UINT br;
#ifdef MCU_HW_JTAG_GPIO_OUT_MODE
  mcu_hw_jtag_enter_gpio_out_mode();
#endif
  while (sent < n) {
    UINT want = (n - sent) < sizeof pbuf ? (n - sent) : sizeof pbuf;
    if (f_read(fil, pbuf, want, &br) != FR_OK || !br) break;
    sent += br;
#ifndef MCU_HW_JTAG_GPIO_OUT_MODE
    for (UINT i = 0; i < br; i++) pbuf[i] = reverse_byte(pbuf[i]);
    gowin_writeSRAM_transfer(pbuf, br, sent >= n);
#else
    mcu_hw_jtag_writeTDI_msb_first_gpio_out_mode(pbuf, br, sent >= n);
#endif
  }
#ifdef MCU_HW_JTAG_GPIO_OUT_MODE
  mcu_hw_jtag_exit_gpio_out_mode();
  mcu_hw_jtag_tms(1, 0b01, 2);
#endif
  *st = gowin_readStatusReg();                    // still in edit mode, before CONFIG_DISABLE
  jtag_command_u08(GOWIN_COMMAND_CONFIG_DISABLE);
  jtag_command_u08(GOWIN_COMMAND_NOOP);
  gowin_debugf("Tango probe: status %08lx after the first %lu bytes", (unsigned long)*st, (unsigned long)sent);
  return sent == n;
}

bool gowin_probe_preamble(const char *name, uint32_t *st4k, uint32_t *st64k) {
  *st4k = *st64k = 0;
  if (strcasecmp(name + strlen(name) - 4, ".bin") != 0) return false;
  FIL fil;
  if (f_open(&fil, name, FA_OPEN_EXISTING | FA_READ) != FR_OK) return false;
  bool ok = false;
  if (gowin_open()) {
    ok = probe_one(&fil, 4096, st4k) && probe_one(&fil, 65536, st64k);
    gowin_reload_flash();                         // leave the FPGA as at power-on
  }
  jtag_close();
  f_close(&fil);
  return ok;
}

// Tango: reload the flash image (e.g. before a cold restart after a failed load)
void gowin_restore_flash_image(void) {
  if (gowin_open()) gowin_reload_flash();
  jtag_close();
}

bool gowin_upload_core(const char *name) {
  gowin_last_done = false; gowin_last_status = 0;
  gowin_status_before = gowin_status_reload = gowin_status_erase = 0;
  gowin_fail_step[0] = 0;        // Tango: cleared at the top of every fresh attempt, set below on failure
  if(strcasecmp(name + strlen(name) - 4, ".bin") == 0)
    return gowin_upload_core_bin(name);

  if(strcasecmp(name + strlen(name) - 3, ".fs") == 0)
    return gowin_upload_core_fs(name);

  gowin_debugf("Unknown file type");
  snprintf(gowin_fail_step, sizeof gowin_fail_step, "unknown file type (%s)", name);
  return false;
}

#endif // ENABLE_JTAG
