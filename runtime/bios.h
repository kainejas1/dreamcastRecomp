#ifndef DC_RUNTIME_BIOS_H
#define DC_RUNTIME_BIOS_H

#include "sh4_ctx.h"

/* High-level BIOS: instead of running the boot ROM, the syscall vectors at
 * 8c0000b0..8c0000e0 point at marker addresses, and calls to those land in
 * bios_call() which does the work in C. */

/* Fill low RAM the way the boot ROM leaves it (vectors, system block). */
void bios_init(void);

/* If addr is one of the syscall markers, run that syscall and return 1. */
int bios_call(sh4_ctx *ctx, uint32_t addr);

#endif
