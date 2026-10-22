#ifndef DC_SH4_RT_H
#define DC_SH4_RT_H

/*
 * Everything the generated code calls. The generated files include only this
 * header. The runtime implements these; a stub file that defines them is
 * enough to compile and link the generated code.
 * The C emitter (src/emit.c) decides which of these each instruction uses.
 */

#include "sh4_ctx.h"

#include <math.h>   /* fsqrt, fsrra */

typedef void (*sh4_func)(sh4_ctx *ctx);

/* Every generated function by address, sorted (the emitter writes it to
 * lookup.c). dc_lookup searches this. */
typedef struct {
    uint32_t addr;
    sh4_func fn;
} dc_func_entry;

extern const dc_func_entry dc_func_table[];
extern const int dc_func_count;

/* ---- memory (virtual addresses; the runtime handles mirrors and MMIO) ---- */

uint8_t  mem_r8(uint32_t addr);
uint16_t mem_r16(uint32_t addr);
uint32_t mem_r32(uint32_t addr);
uint64_t mem_r64(uint32_t addr);
void     mem_w8(uint32_t addr, uint8_t v);
void     mem_w16(uint32_t addr, uint16_t v);
void     mem_w32(uint32_t addr, uint32_t v);
void     mem_w64(uint32_t addr, uint64_t v);

/* ---- control flow ---- */

/* Address -> generated function, from the table the emitter writes in
 * lookup.c. NULL if addr is not a known function start. */
sh4_func dc_lookup(uint32_t addr);

/* Call through a computed address (jsr @Rn, bsrf, unknown table targets).
 * The caller has already set ctx->pr. Must not return quietly if addr is
 * unknown: report it, because that is a function the analysis missed. */
void dc_call(sh4_ctx *ctx, uint32_t addr);

/* Same for a computed tail jump (jmp @Rn, braf); the generated code returns
 * right after it. */
void dc_jump(sh4_ctx *ctx, uint32_t addr);

/* An instruction the emitter has no translation for. */
void dc_unhandled(sh4_ctx *ctx, uint32_t pc, uint16_t raw);

/* ---- interrupts ---- */

extern volatile int dc_irq_pending;
void dc_service_irq(sh4_ctx *ctx);

/* Emitted at function entry and on every backward branch. */
#define DC_CHECK_IRQ(ctx) do { if (dc_irq_pending) dc_service_irq(ctx); } while (0)

/* Emitted at function entry. Build with -DDC_TRACE to record the most recently
 * entered functions (the runtime prints them when the run ends). */
#ifdef DC_TRACE
void dc_trace_enter(uint32_t addr);
#define DC_ENTER(ctx, addr) dc_trace_enter(addr)
#else
#define DC_ENTER(ctx, addr) ((void)0)
#endif

/* ---- instructions implemented as helpers ---- */

uint32_t sh4_get_sr(const sh4_ctx *ctx);
void     sh4_set_sr(sh4_ctx *ctx, uint32_t sr);         /* swaps r0-r7 if RB changes */
void     sh4_set_fpscr(sh4_ctx *ctx, uint32_t fpscr);   /* swaps fr/xf if FR changes */

void sh4_div1(sh4_ctx *ctx, unsigned n, unsigned m);    /* div1 Rm,Rn (uses Q, M, T) */
void sh4_mac_l(sh4_ctx *ctx, unsigned n, unsigned m);   /* mac.l @Rm+,@Rn+ */
void sh4_mac_w(sh4_ctx *ctx, unsigned n, unsigned m);   /* mac.w @Rm+,@Rn+ */
void sh4_pref(sh4_ctx *ctx, uint32_t addr);             /* store-queue flush for 0xE0000000-0xE3FFFFFF */
void sh4_trapa(sh4_ctx *ctx, uint32_t imm, uint32_t pc);
void sh4_rte(sh4_ctx *ctx);
void sh4_sleep(sh4_ctx *ctx);
void sh4_ldtlb(sh4_ctx *ctx);
void sh4_fsca(sh4_ctx *ctx, unsigned n);                /* fsca fpul,DRn */
void sh4_ftrv(sh4_ctx *ctx, unsigned n);                /* ftrv xmtrx,FVn */

#endif
