// Modified for Tango, 2026: sdc_direct_card_idle(); Apache-2.0 as the original.
#ifndef SDC_DIRECT_H
#define SDC_DIRECT_H

bool sdc_direct_init(void);
void sdc_direct_release(void);
#ifdef TANGO
void sdc_direct_card_idle(void);   // Tango: CMD0 before the FPGA gets the card back
#endif
bool sdc_direct_upload_core_bin(const char *name);

#endif // SDC_DIRECT_H
