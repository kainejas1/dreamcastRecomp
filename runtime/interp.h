#ifndef DC_RUNTIME_INTERP_H
#define DC_RUNTIME_INTERP_H

#include "sh4_rt.h"

/* Runs the code at pc the way a recompiled function would: until it returns
 * with rts or rte. Calls go to recompiled functions wherever there are some. */
void dc_interp(sh4_ctx *ctx, uint32_t pc);

/* Instructions interpreted so far. */
uint64_t dc_interp_count(void);

#endif
