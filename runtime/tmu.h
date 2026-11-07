#ifndef DC_RUNTIME_TMU_H
#define DC_RUNTIME_TMU_H

#include <stdint.h>

/* The SH-4's three timers (TMU0-2), running off clock.c. */

void tmu_init(void);

int tmu_handles(uint32_t address);
uint32_t tmu_read32(uint32_t address);
void tmu_write32(uint32_t address, uint32_t value);

/* Priority (1-15) of the highest pending timer interrupt, 0 if none, with its
 * INTEVT code in *intevt. ipra is the INTC's IPRA register. */
int tmu_pending(uint32_t ipra, uint32_t *intevt);

#endif
