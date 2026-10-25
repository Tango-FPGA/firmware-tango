/*
  sdc_direct.c - access the sd card directly from the  BL616

  SD card access for Console 60K and 138k where the
  card is connected to the FPGA _and_ the BL616  

  uses the BL616 SDH SD Host controller (SDH) to access the card or read from an USB Mass Storage (MSC).

  Modified for Tango, 2026: sdc_direct_card_idle(). Apache-2.0 as the original.
*/

#include "bflb_gpio.h"
#include "board.h"
#include "sdc_direct.h"
#include "../mcu_hw.h"

static struct bflb_device_s *gpio;
static bool sdc_direct_active = false;

bool sdc_direct_init(void) {  
  gpio = bflb_device_get_by_name("gpio");

#ifdef TANG_CONSOLE
  /* Console 60k/138k SD Card bus to BL616 TF_SDIO_SEL*/
  bflb_gpio_set(gpio, PIN_TF_SDIO_SEL);
#endif
  bflb_mtimer_delay_ms(250);

  board_sdh_gpio_init();

  fatfs_sdh_driver_register();
  sdc_direct_active = true;

  return true;
}

#ifdef TANGO
#include "ff.h"
#include "sdh_sd.h"
extern struct sd_card_s sd_card;              // components/fs/fatfs/port/fatfs_sdh_lhal_sdcard.c 28
int sdh_sd_go_idle(struct sd_card_s *sd_card, uint32_t arg);   // bsp/common/sdh/sd/sdh_sd.c 29 (not in its header)

// Tango: leave the card in the idle state before the FPGA gets it back, so a core that drives it in
// SPI mode (e.g. a core's own SD loader) can enter SPI mode with CMD0 + CS low, as after power-on. The BL616 ran
// it in SD 4-bit mode at SDR25 (sdh_sd.c 929-936); the SDH block was reset by mcu_hw_fpga_resume_spi,
// so the card is initialised again first (a mount = MMC_disk_initialize: sdh_sd_deinit + sdh_sd_card_init),
// then CMD0 GO_IDLE_STATE. The Console has no card power switch (only GPIO16 TF_SDIO_SEL, mcu_hw.c pin table).
void sdc_direct_card_idle(void) {
  static FATFS fs;
  bool mounted = f_mount(&fs, "/sd", 1) == FR_OK;
  if (mounted) f_mount(NULL, "/sd", 1);
  int r = mounted ? sdh_sd_go_idle(&sd_card, 0) : -1;
  printf("Tango: card %s before it goes back to the FPGA (CMD0 %d)\r\n", r == 0 ? "left idle" : "NOT re-initialised", r);
}
#endif

void sdc_direct_release(void) {
  gpio = bflb_device_get_by_name("gpio");
  sdc_direct_active = false;
#ifdef TANG_CONSOLE
  /* Console 60k/138k SD Card bus back to FPGA TF_SDIO_SEL*/
  bflb_gpio_reset(gpio, PIN_TF_SDIO_SEL);
#endif
}
