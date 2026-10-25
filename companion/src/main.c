/*
  main.c - MiSTeryNano FPGA Companion Pi Pico variant

  Modified for Tango, 2026: see the TANGO blocks; Apache-2.0 as the original.
*/

#include "../mcu_hw.h"

#include "../config.h"
#include "../sysctrl.h"
#include "../sdc.h"
#include "../osd.h"
#include "../menu.h"
#include "../inifile.h"
#include "../debug.h"
#include "../ftpd.h"
#include "../telnetd.h"
#include "../xml.h"
#include "../at_wifi.h"
#ifdef TANGO
#include "tango_fw.h"
#endif

/*-----------------------------------------------------------*/
/*---            main FPGA communication task            ----*/
/*-----------------------------------------------------------*/

TaskHandle_t com_task_handle = NULL;

static void com_task(__attribute__((unused)) void *p ) {
  debugf("Starting main communication task");

#ifdef TANGO
  // Tango: at power-on load the launcher core /menu.fs from the SD card (MiSTer's menu.rbf)
  tango_fw_boot();
#endif

  // startup FPGA, this will also put the core into reset
  if(sys_wait4fpga()) {
    // FPGA is ready and can be talked to

#ifdef TANGO
    // Tango: a launcher core answers CAPS; the launcher then owns the OSD, the card and the input,
    // and this task only services the FPGA's SYS (cold boot, buttons) and HID (DB9) interrupts. SDC
    // events are masked: in the launcher the card belongs to the BL616 and sdc_init() never ran.
    if(tango_fw_try_launcher()) {
      sys_handle_interrupts(sys_irq_ctrl(0xff) & 0x03, true);
      hid_handle_event();    // reading the DB9 state arms its interrupt
      debugf("Entering main loop (Tango launcher)");
      for(;;) {
        mcu_hw_irq_ack();
        ulTaskNotifyTake( pdTRUE, portMAX_DELAY);
        sys_handle_interrupts(sys_irq_ctrl(0xff) & 0x03, false);
      }
    }

    // Tango: any other Tango core (CAPS 'Z''O', CONF_STR not MENU) is a framework core: the in-core OSD
    // (Start+Select / F12). The BL616 keeps the card; pending bit 3 (SDC) is the core asking for file
    // bytes (menu-tango docs/TANGO-SPEC.md "Cores": a level while a download is open and >= 256 bytes are free),
    // served by the Tango task instead of FPGA Companion's sdc_handle_event.
    if(tango_fw_try_core()) {
      unsigned char pending = sys_irq_ctrl(0xff);
      sys_handle_interrupts(pending & 0x03, true);
      hid_handle_event();    // reading the DB9 state arms its interrupt
      debugf("Entering main loop (Tango core)");
      for(;;) {
        mcu_hw_irq_ack();
        ulTaskNotifyTake( pdTRUE, portMAX_DELAY);
        pending = sys_irq_ctrl(0xff);
        sys_handle_interrupts(pending & 0x03, false);
        if(pending & 0x08) tango_fw_sdc_irq();
      }
    }
#endif

    // initialitze SD card
    sdc_init();

    // try to load the global config
    inifile_config_read();
    
    // try to load a config .xml from sd card. If the core has identified itself,
    // then e.g. atarist.xml will be read. otherwise config.xml
    FIL fil;
    if(f_open(&fil, sys_get_config_name(), FA_OPEN_EXISTING | FA_READ) == FR_OK) {
      config_init();

      UINT br; char c;
      debugf("Loading XML config from file");

      // read byte by byte. Slow but that doesn't hurt ...
      FRESULT r = f_read(&fil, &c, 1, &br);
      while(r == FR_OK && br) {
	xml_parse(c);      
	r = f_read(&fil, &c, 1, &br);
      }    
      f_close(&fil);

      config_dump();
    } else {
      // no XML on SD card, try to load from core itself
      char *cfg_str = sys_get_config();
      if(cfg_str) {
	debugf("Loading XML config from core");
	config_init();
	char *c = cfg_str;
	while(*c) xml_parse(*c++); 
	config_dump();
      } else
	debugf("No valid config found, neither on sd card nor in core");
    }

    // process any pending interrupt. Filter out irq 1 which is the
    // FPGA cold boot event which we ignore since we just booted outselves
    sys_handle_interrupts(sys_irq_ctrl(0xff), true);
    
    // by default, DB9 interrupts are disabled. Reading
    // the DB9 state enables them. This is what hid_handle_event
    // does.
    hid_handle_event();

    // initialize on-screen-display and menu system
    osd_init();    
    menu_init();

    // open disk images, either defaults set in sdc_init or
    // user configure ones from the ini file. This will also
    // start rom image transfers if specified in the ini file
    sdc_mount_defaults();

    // finally run the ready action. This will usually get the core out of reset
    // On setups not using core configs, just release FPGA from reset
    // But this should actually never be the case nowadays.
    // TODO: An image upload may still be in progress ...
    if(!sdc_image_upload_in_progress()) {
      if(!cfg) sys_set_val('R', 0);
      else     sys_run_action_by_name("ready");
    } else
      debugf("Image upload in progress, delaying ready action");

#ifndef TANGO_NO_NET
    ftpd_init();
    
    // finally prepare for wifi communication
    at_wifi_init();
#endif

    debugf("Entering main loop");
  
    for(;;) {
      mcu_hw_irq_ack();  // (re-)enable interrupt
      ulTaskNotifyTake( pdTRUE, portMAX_DELAY);    
      sys_handle_interrupts(sys_irq_ctrl(0xff), false);
    }
  }

  /* This will only be reached if the FPGA is not ready */
  /* So loop foreever while e.g. USB is still being handled */
  /* e.g. for debugging */
  for(;;) {
    // frequently check for an FPGA to show up and reboot to
    // startup normally if one is detected
    if(sys_status_is_valid()) {
      debugf("FPGA detected!");
      // This may be due to a USB download. So give USB
      // some time to finish. The same happens in sysconfig.c
      vTaskDelay(pdMS_TO_TICKS(2000));
      mcu_hw_reset();
    }
      
    vTaskDelay(pdMS_TO_TICKS(250));
  }
}

#ifdef ESP_PLATFORM
void app_main( void )
#else
int main( void )
#endif
{
  mcu_hw_init();
#ifndef TANGO_NO_NET
  telnetd_init();
#endif
  
  // run FPGA com thread
  xTaskCreate( com_task, "FPGA Com", 4096, NULL, CONFIG_MAX_PRIORITY-1, &com_task_handle );

  mcu_hw_main_loop();

#ifndef ESP_PLATFORM
  return 0;
#endif
}
/*-----------------------------------------------------------*/

