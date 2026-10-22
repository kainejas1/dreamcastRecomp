#ifndef DC_RUNTIME_AICA_H
#define DC_RUNTIME_AICA_H

#include <stdint.h>

/* AICA (sound chip) registers at physical 00700000-00707fff and the
 * real-time clock at 00710000. Addresses here are physical. */

#define AICA_ARM_RESET 0x00702c00u   /* bit 0 set: the sound CPU is held in reset */

void     aica_init(void);
int      aica_handles(uint32_t phys);
uint32_t aica_read32(uint32_t phys);
void     aica_write32(uint32_t phys, uint32_t value);
void     aica_update(void);   /* called every so often: plays the sound driver's side */

#endif
