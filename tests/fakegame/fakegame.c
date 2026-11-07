/*
 * A stand-in for generated game code, written by hand so the runtime can run
 * end to end with no disc data. It sets up what a real game sets up (an
 * interrupt handler, the vblank interrupt, a periodic TMU interrupt) and then
 * loops, stirring guest RAM with values that depend on exactly when each
 * interrupt landed. If interrupt timing changes between runs, the state hashes
 * change too.
 *
 * It also writes a little SH-4 code into guest RAM and calls it. Nothing was
 * recompiled for those addresses, so they stand for code the analysis missed
 * and go through the runtime's miss handling into the interpreter.
 *
 * Exits 0 if everything came out as expected, 1 if not.
 */

#include "sh4_rt.h"

#include <stdio.h>
#include <stdlib.h>

#define INTEVT     0xff000028u
#define INTC_IPRA  0xffd00004u
#define TSTR       0xffd80004u
#define TCOR0      0xffd80008u
#define TCNT0      0xffd8000cu
#define TCR0       0xffd80010u
#define SB_ISTNRM  0xa05f6900u
#define SB_IML6NRM 0xa05f6930u

#define VBLANK_IN  (1u << 3)
#define TCR_UNIE   0x0020u
#define TCR_UNF    0x0100u
#define TPSC_64    2u

#define VBLANKS    0x8c0f0000u
#define TIMERS     0x8c0f0004u
#define SCRATCH    0x8c100000u

#define FRAMES         120u
#define TICKS_PER_FRAME 13020u   /* 16.67 ms at 50 MHz / 64 */
#define PERIOD         1302u     /* 10 timer interrupts a frame */

#define WAIT_CODE    0x8c200000u
#define SETJMP_CODE  0x8c200100u

/* Spins until a timer interrupt has counted, so it only ends if interrupts
 * reach interpreted code. Returns 42. */
static const uint16_t wait_code[] = {
    0xd103,          /*      mov.l  timers,r1 */
    0x6212,          /*      mov.l  @r1,r2 */
    0x6312,          /* 1:   mov.l  @r1,r3 */
    0x3320,          /*      cmp/eq r2,r3 */
    0x89fc,          /*      bt     1b */
    0xe02a,          /*      mov    #42,r0 */
    0x000b,          /*      rts */
    0x0009,          /*      nop */
    0x0004, 0x8c0f,  /* timers: .long TIMERS */
};

/* setjmp(buf) from interpreted code, then longjmp(buf, 7), which has to come
 * back to the interpreter's frame. Returns 8. */
static const uint16_t setjmp_code[] = {
    0x4f22,          /*      sts.l  pr,@-r15 */
    0xd408,          /*      mov.l  buf,r4 */
    0xd108,          /*      mov.l  setjmp,r1 */
    0x410b,          /*      jsr    @r1 */
    0x0009,          /*      nop */
    0x8800,          /*      cmp/eq #0,r0 */
    0x8b05,          /*      bf     1f */
    0xd405,          /*      mov.l  buf,r4 */
    0xe507,          /*      mov    #7,r5 */
    0xd106,          /*      mov.l  longjmp,r1 */
    0x410b,          /*      jsr    @r1 */
    0x0009,          /*      nop */
    0xe0ff,          /*      mov    #-1,r0: longjmp doesn't return */
    0x7001,          /* 1:   add    #1,r0 */
    0x4f26,          /*      lds.l  @r15+,pr */
    0x000b,          /*      rts */
    0x0009,          /*      nop */
    0x0009,          /*      nop */
    0x0100, 0x8c0f,  /* buf:     .long 0x8c0f0100 */
    0x1000, 0x8c00,  /* setjmp:  .long 0x8c001000 */
    0x1100, 0x8c00,  /* longjmp: .long 0x8c001100 */
};

/* the runtime's interrupt entry, at VBR + 0x600 */
static void f_8c000600(sh4_ctx *ctx)
{
    uint32_t evt = mem_r32(INTEVT);

    if (evt == 0x400u) {
        mem_w16(TCR0, (uint16_t)(mem_r16(TCR0) & ~TCR_UNF));
        mem_w32(TIMERS, mem_r32(TIMERS) + 1);
    } else if (evt == 0x320u) {
        mem_w32(SB_ISTNRM, VBLANK_IN);
        mem_w32(VBLANKS, mem_r32(VBLANKS) + 1);
    } else {
        fprintf(stderr, "fakegame: unexpected INTEVT %03x\n", evt);
        exit(1);
    }
    sh4_rte(ctx);
}

/* the game's setjmp and longjmp, enough of them for the runtime's records */
static void f_8c001000(sh4_ctx *ctx)
{
    mem_w32(ctx->r[4], ctx->r[15]);
    mem_w32(ctx->r[4] + 4, ctx->pr);
    ctx->r[0] = 0;
}

static void f_8c001100(sh4_ctx *ctx)
{
    ctx->r[15] = mem_r32(ctx->r[4]);
    ctx->pr = mem_r32(ctx->r[4] + 4);
    ctx->r[0] = ctx->r[5];
}

const dc_func_entry dc_func_table[] = {
    { 0x8c000600u, f_8c000600 },
    { 0x8c001000u, f_8c001000 },
    { 0x8c001100u, f_8c001100 },
};
const int dc_func_count = 3;
const uint32_t dc_setjmp_addr = 0x8c001000u, dc_longjmp_addr = 0x8c001100u;

const dc_module dc_modules[1] = { { 0 } };
const int dc_module_count = 0;

static int near(uint32_t actual, uint32_t expected)
{
    uint32_t margin = expected / 20u;   // 5%
    return actual + margin >= expected && actual <= expected + margin;
}

static uint32_t run_code(sh4_ctx *ctx, uint32_t addr, const uint16_t *code, size_t words)
{
    for (size_t i = 0; i < words; i++)
        mem_w16(addr + 2u * (uint32_t)i, code[i]);
    ctx->pr = 0;
    dc_call(ctx, addr);
    return ctx->r[0];
}

void f_8c010000(sh4_ctx *ctx)
{
    uint32_t i = 0, last_tcnt = 0, tcnt_moved = 0;

    mem_w32(SB_IML6NRM, VBLANK_IN);
    mem_w16(INTC_IPRA, 0x8000u);   // TMU0 at level 8
    mem_w32(TCOR0, PERIOD - 1);
    mem_w32(TCNT0, PERIOD - 1);
    mem_w16(TCR0, (uint16_t)(TCR_UNIE | TPSC_64));
    mem_w8(TSTR, 1);
    sh4_set_sr(ctx, sh4_get_sr(ctx) & ~(SR_BL | SR_IMASK));

    uint32_t waited = run_code(ctx, WAIT_CODE, wait_code, sizeof wait_code / sizeof *wait_code);
    uint32_t jumped = run_code(ctx, SETJMP_CODE, setjmp_code, sizeof setjmp_code / sizeof *setjmp_code);

    while (mem_r32(VBLANKS) < FRAMES) {
        DC_CHECK_IRQ(ctx);

        uint32_t tcnt = mem_r32(TCNT0);
        if (tcnt != last_tcnt) tcnt_moved++;
        last_tcnt = tcnt;

        uint32_t a = SCRATCH + (i++ & 0xfffcu);
        mem_w32(a, mem_r32(a) * 31u + tcnt + mem_r32(TIMERS) + mem_r32(VBLANKS));
        ctx->r[0] = mem_r32(a);
    }

    uint32_t vblanks = mem_r32(VBLANKS), timers = mem_r32(TIMERS);
    uint32_t expected = FRAMES * TICKS_PER_FRAME / PERIOD;
    int ok = vblanks == FRAMES && near(timers, expected) && tcnt_moved > 0 && waited == 42 && jumped == 8;

    fprintf(stderr, "fakegame: %u vblanks, %u timer interrupts (expected about %u), TCNT moved %u times, "
            "interpreted code returned %u and %u: %s\n",
            vblanks, timers, expected, tcnt_moved, waited, jumped, ok ? "ok" : "FAILED");
    exit(ok ? 0 : 1);
}
