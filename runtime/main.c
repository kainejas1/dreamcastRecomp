/*
 * Runtime entry point: loads the game into RAM, sets the CPU up the way the
 * boot ROM leaves it, and calls the recompiled entry point.
 *
 *   sa2 <1ST_READ.BIN> [IP.BIN] [disc.cue]
 *
 * No hardware yet. Accesses to it are logged by memory.c, and the run stops
 * with a report when it hits something it can't go past.
 */

#include "bios.h"
#include "gdrom.h"
#include "holly.h"
#include "memory.h"
#include "sh4_rt.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

void f_8c010000(sh4_ctx *ctx);   // every game starts here

static sh4_ctx cpu;

static void dump(const sh4_ctx *ctx)
{
    for (int i = 0; i < 16; i++)
        fprintf(stderr, "  r%-2d %08x%s", i, ctx->r[i], i % 4 == 3 ? "\n" : "");
    fprintf(stderr, "  pr  %08x  sr  %08x  gbr %08x  t %u\n", ctx->pr, sh4_get_sr(ctx), ctx->gbr, ctx->t);
    fprintf(stderr, "  fpscr %08x  fpul %08x  mach %08x  macl %08x\n", ctx->fpscr, ctx->fpul, ctx->mach, ctx->macl);
    if (mem_last_unmapped)
        fprintf(stderr, "  last unimplemented hardware access: %08x\n", mem_last_unmapped);
}

static void stop(const sh4_ctx *ctx, const char *fmt, ...)
{
    va_list ap;
    fprintf(stderr, "\n*** stopped: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
    dump(ctx);
    // DC_DUMP=file: write main RAM out for looking at with a hex editor/disasm
    const char *path = getenv("DC_DUMP");
    if (path) {
        FILE *f = fopen(path, "wb");
        if (f) { fwrite(dc_ram, 1, DC_RAM_SIZE, f); fclose(f); fprintf(stderr, "  RAM dumped to %s\n", path); }
    }
    exit(1);
}

// ---- computed calls and jumps ----

sh4_func dc_lookup(uint32_t addr)
{
    int lo = 0, hi = dc_func_count - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (dc_func_table[mid].addr < addr) lo = mid + 1;
        else if (dc_func_table[mid].addr > addr) hi = mid - 1;
        else return dc_func_table[mid].fn;
    }
    return NULL;
}

// code can be reached through P2 (0xa...) or a physical address too;
// the table uses the P1 (0x8...) addresses
static uint32_t to_p1(uint32_t addr)
{
    return (addr & 0x1fffffffu) | 0x80000000u;
}

void dc_call(sh4_ctx *ctx, uint32_t addr)
{
    if (bios_call(ctx, addr)) return;   // syscall vectors point at BIOS hooks
    sh4_func f = dc_lookup(to_p1(addr));
    if (!f)
        stop(ctx, "call to %08x, no recompiled function there (called from around pr=%08x)", addr, ctx->pr);
    f(ctx);
}

void dc_jump(sh4_ctx *ctx, uint32_t addr)
{
    if (bios_call(ctx, addr)) return;   // syscall stubs jmp through the vectors
    sh4_func f = dc_lookup(to_p1(addr));
    if (!f)
        stop(ctx, "jump to %08x, no recompiled function there", addr);
    f(ctx);
}

void dc_unhandled(sh4_ctx *ctx, uint32_t pc, uint16_t raw)
{
    stop(ctx, "reached %08x, which the recompiler couldn't translate (%04x)", pc, raw);
}

// ---- interrupts ----

// Kept at 1 so every DC_CHECK_IRQ (function entry, backward branch) lands in
// dc_service_irq, which looks at the video clock every so often and takes
// pending interrupts there. That's the only place interrupts can happen, a
// little later than on hardware, which games don't mind.
volatile int dc_irq_pending = 1;

#define CHECK_EVERY 4096          // DC_CHECK_IRQs between looks at the clock
#define WATCHDOG    300000000ull  // checks without a single interrupt: stuck

static unsigned long long interrupts;
static uint32_t max_frames;       // DC_FRAMES=n: stop after n frames (for test runs)

void dc_service_irq(sh4_ctx *ctx)
{
    static unsigned long long checks, since_irq;

    if (++since_irq == WATCHDOG)
        stop(ctx, "no interrupt taken for %llu checks, probably waiting on hardware", since_irq);
    if (++checks % CHECK_EVERY) return;

    if (max_frames && holly_frame_count() >= max_frames)
        stop(ctx, "ran %u frames (DC_FRAMES), %llu interrupts taken", holly_frame_count(), interrupts);

    int level = holly_pending_level();
    uint32_t sr = sh4_get_sr(ctx);
    if (!level || level <= (int)((sr >> 4) & 15) || (sr & SR_BL))
        return;

    // what the SH-4 does when it takes an interrupt. There's no PC to save:
    // returning from here is "continue where we were"
    ctx->ssr = sr;
    ctx->sgr = ctx->r[15];
    sh4_set_sr(ctx, sr | SR_MD | SR_RB | SR_BL);   // switches to register bank 1
    sh4reg_write32(0xff000028u, level == 6 ? 0x320u : level == 4 ? 0x360u : 0x3a0u);   // INTEVT

    interrupts++;
    since_irq = 0;
    dc_call(ctx, ctx->vbr + 0x600);   // the handler ends with rte
}

#ifdef DC_TRACE
// last functions entered, oldest first when printed
static uint32_t trace_ring[64];
static unsigned trace_pos;

void dc_trace_enter(uint32_t addr)
{
    static int live = -1;   // DC_TRACELOG=n: also print the first n entries as they happen
    if (live < 0) live = getenv("DC_TRACELOG") ? atoi(getenv("DC_TRACELOG")) : 0;
    if (live > 0) { live--; fprintf(stderr, "[fn] %08x\n", addr); }
    trace_ring[trace_pos++ % 64] = addr;
}
#endif

// printed however the run ends (stop, BIOS exit, entry point returning)
static void summary(void)
{
    fprintf(stderr, "[run] %u frames, %llu interrupts taken\n", holly_frame_count(), interrupts);
#ifdef DC_TRACE
    fprintf(stderr, "[run] last functions entered:");
    for (unsigned i = 0; i < 64; i++) {
        unsigned k = trace_pos + i;
        if (k >= 64 && trace_ring[k % 64]) fprintf(stderr, "%s%08x", i % 8 ? " " : "\n   ", trace_ring[k % 64]);
    }
    fprintf(stderr, "\n");
#endif
}

// ---- CPU helpers that need hardware ----

void sh4_pref(sh4_ctx *ctx, uint32_t addr)
{
    (void)ctx;
    static int warned;
    if ((addr & 0xfc000000u) == 0xe0000000u && !warned++)
        fprintf(stderr, "[hw] store queue flush (pref %08x) not implemented yet\n", addr);
}

void sh4_trapa(sh4_ctx *ctx, uint32_t imm, uint32_t pc)
{
    stop(ctx, "trapa #%u at %08x, exceptions not implemented yet", imm, pc);
}

void sh4_rte(sh4_ctx *ctx)
{
    sh4_set_sr(ctx, ctx->ssr);   // back to the interrupted code's SR (and bank); returning resumes it
}

void sh4_sleep(sh4_ctx *ctx)
{
    stop(ctx, "sleep (waits for an interrupt, none implemented yet)");
}

void sh4_ldtlb(sh4_ctx *ctx)
{
    (void)ctx;   // MMU is off on the Dreamcast, nothing to do
}

int main(int argc, char **argv)
{
    char err[256];

    if (argc < 2) {
        fprintf(stderr, "usage: %s 1ST_READ.BIN [IP.BIN] [disc.cue]\n", argv[0]);
        return 2;
    }
    mem_init();
    bios_init();   // before loading: it fills low RAM, IP.BIN goes on top
    if (argc > 3 && !gd_open(argv[3], err, sizeof err)) {
        fprintf(stderr, "error: %s\n", err);
        return 1;
    }
    if (!mem_load_bin(argv[1], DC_RAM_LOAD_BASE, err, sizeof err)) {
        fprintf(stderr, "error: %s\n", err);
        return 1;
    }
    // IP.BIN sits at 8c008000; the game may look at it
    if (argc > 2 && !mem_load_bin(argv[2], 0x8c008000u, err, sizeof err)) {
        fprintf(stderr, "error: %s\n", err);
        return 1;
    }

    // what the boot ROM hands over (values from Flycast's HLE BIOS): privileged
    // mode, interrupts masked, round-to-zero FPU, stack at the top of RAM
    cpu.sr = 0x400000f0u;
    cpu.fpscr = 0x00040001u;
    cpu.r[15] = 0x8d000000u;
    cpu.gbr = 0x8c000000u;
    cpu.vbr = 0x8c000000u;
    cpu.dbr = 0x8c000010u;
    cpu.sgr = 0x8d000000u;

    if (getenv("DC_FRAMES")) max_frames = (uint32_t)strtoul(getenv("DC_FRAMES"), NULL, 0);
    atexit(summary);

    fprintf(stderr, "starting at 8c010000 (%d recompiled functions)\n", dc_func_count);
    f_8c010000(&cpu);
    fprintf(stderr, "entry point returned\n");
    dump(&cpu);
    return 0;
}
