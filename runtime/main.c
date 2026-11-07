/*
 * Runtime entry point: loads the game into RAM, sets the CPU up the way the
 * boot ROM leaves it, and calls the recompiled entry point.
 *
 *   sa2 <1ST_READ.BIN> [IP.BIN] [disc.cue]
 *
 * No hardware yet. Accesses to it are logged by memory.c, and the run stops
 * with a report when it hits something it can't go past.
 */

#include "aica.h"
#include "bios.h"
#include "clock.h"
#include "gdrom.h"
#include "holly.h"
#include "interp.h"
#include "maple.h"
#include "memory.h"
#include "prof.h"
#include "sh4_rt.h"
#include "tmu.h"
#include "video.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    // DC_DUMP=file: write main RAM out for looking at with a hex editor/disasm.
    // sound RAM follows it (file offset 0x1000000)
    const char *path = getenv("DC_DUMP");
    if (path) {
        FILE *f = fopen(path, "wb");
        if (f) { fwrite(dc_ram, 1, DC_RAM_SIZE, f); fwrite(dc_aram, 1, DC_ARAM_SIZE, f); fclose(f); fprintf(stderr, "  RAM dumped to %s\n", path); }
    }
    exit(1);
}

// DC_HWSTOP=828c: stop at the first unimplemented access whose top 16 bits are
// these, with the registers still showing what made it
void dc_hw_miss(uint32_t address)
{
    static long want = -2;
    if (want == -2) want = getenv("DC_HWSTOP") ? strtol(getenv("DC_HWSTOP"), NULL, 16) : -1;
    if (want >= 0 && (long)(address >> 16) == want)
        stop(&cpu, "access to unimplemented %08x (DC_HWSTOP)", address);
}

// ---- computed calls and jumps ----

static sh4_func find_in(const dc_func_entry *t, int n, uint32_t addr)
{
    int lo = 0, hi = n - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (t[mid].addr < addr) lo = mid + 1;
        else if (t[mid].addr > addr) hi = mid - 1;
        else return t[mid].fn;
    }
    return NULL;
}

// is this module the one in memory right now?
static int module_loaded(const dc_module *m)
{
    const uint8_t *head = mem_ptr(m->base, 8), *code = mem_ptr(m->sig_addr, m->sig_len);
    if (!head || !code || memcmp(head, m->magic, 8)) return 0;
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < m->sig_len; i++)
        h = (h ^ code[i]) * 16777619u;
    return h == m->sig_hash;
}

sh4_func dc_lookup(uint32_t addr)
{
    static const dc_module *current;   // last module used, checked again every time

    sh4_func f = find_in(dc_func_table, dc_func_count, addr);
    if (f) return f;

    if (current && addr - current->base < current->size && module_loaded(current))
        return find_in(current->funcs, current->nfuncs, addr);
    for (int i = 0; i < dc_module_count; i++) {
        const dc_module *m = &dc_modules[i];
        if (addr - m->base < m->size && module_loaded(m)) {
            if (m != current) fprintf(stderr, "[mod] %s is loaded\n", m->name);
            current = m;
            return find_in(m->funcs, m->nfuncs, addr);
        }
    }
    return NULL;
}

// code can be reached through P2 (0xa...) or a physical address too;
// the table uses the P1 (0x8...) addresses
static uint32_t to_p1(uint32_t addr)
{
    return (addr & 0x1fffffffu) | 0x80000000u;
}

// ---- code the analysis missed ----

// A target with no recompiled function isn't fatal: it runs in the interpreter
// instead. DC_MISSLOG=file lists each one once, in the form `dcrecomp emit
// --roots` reads, so the next build has it as a real function. DC_NOINTERP=1
// stops at the first one instead, which is what you want when the target is
// more likely to be a bad pointer.
#define MISS_SLOTS 65536u
#define MISS_PRINT 32u   // how many to mention on stderr

static struct { uint32_t addr; const char *module; } misses[MISS_SLOTS];
static unsigned nmisses;
static FILE *miss_file;
static int no_interp;

static void miss_init(void)
{
    const char *path = getenv("DC_MISSLOG");
    no_interp = getenv("DC_NOINTERP") != NULL;
    if (!path) return;
    miss_file = fopen(path, "w");
    if (!miss_file) fprintf(stderr, "[miss] can't write %s\n", path);
}

static const char *module_at(uint32_t addr)
{
    for (int i = 0; i < dc_module_count; i++) {
        const dc_module *m = &dc_modules[i];
        if (addr - m->base < m->size && module_loaded(m)) return m->name;
    }
    return "main";
}

static void note_miss(const sh4_ctx *ctx, uint32_t addr, const char *kind)
{
    if (no_interp)
        stop(ctx, "%s to %08x, no recompiled function there (called from around pr=%08x)", kind, addr, ctx->pr);

    const char *module = module_at(addr);
    uint32_t i = ((addr >> 1) * 2654435761u) >> 16;
    while (misses[i].addr) {
        if (misses[i].addr == addr && !strcmp(misses[i].module, module)) return;   // seen it
        i = (i + 1) % MISS_SLOTS;
    }
    if (nmisses + 1 >= MISS_SLOTS) return;
    misses[i].addr = addr;
    misses[i].module = module;
    if (nmisses++ < MISS_PRINT)
        fprintf(stderr, "[miss] %s to %08x in %s, from around pr=%08x: interpreting it\n", kind, addr, module, ctx->pr);
    if (miss_file) {
        fprintf(miss_file, "%08x %s %s %08x\n", addr, module, kind, ctx->pr);
        fflush(miss_file);
    }
}

// runs what's at addr if there's a function for it, otherwise notes the miss
static int run_known(sh4_ctx *ctx, uint32_t addr, const char *kind)
{
    if (bios_call(ctx, addr)) return 1;   // syscall vectors point at BIOS hooks

    uint32_t p1 = to_p1(addr);
    sh4_func f = dc_lookup(p1);
    if (!f) {
        note_miss(ctx, p1, kind);
        return 0;
    }
    if (p1 == dc_longjmp_addr) DC_LONGJMP(ctx, f);   // doesn't return
    f(ctx);
    return 1;
}

void dc_call(sh4_ctx *ctx, uint32_t addr)
{
    if (!run_known(ctx, addr, "call")) dc_interp(ctx, addr);
}

void dc_jump(sh4_ctx *ctx, uint32_t addr)
{
    if (!run_known(ctx, addr, "jump")) dc_interp(ctx, addr);
}

int dc_try_jump(sh4_ctx *ctx, uint32_t addr)
{
    return run_known(ctx, addr, "jump");
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

#define INTC_IPRA   0xffd00004u

static unsigned long long interrupts, checks;
static uint32_t max_frames;       // DC_FRAMES=n: stop after n frames (for test runs)

// ---- machine state hashes ----

// DC_STATEHASH=file: every DC_STATEEVERY frames (default 60) write a line of
// hashes of RAM and the CPU. Two DC_VCLOCK runs should give the same file, and
// the first line that differs brackets where they went apart.
static FILE *hash_file;
static uint32_t hash_every = 60, hash_next;

static uint64_t hash_bytes(const void *data, size_t n)
{
    const uint8_t *p = data;
    uint64_t h = 0xcbf29ce484222325ull;   // FNV-1a, a word at a time

    for (; n >= 8; n -= 8, p += 8) {
        uint64_t w;
        memcpy(&w, p, 8);
        h = (h ^ w) * 0x100000001b3ull;
    }
    for (; n; n--, p++) h = (h ^ *p) * 0x100000001b3ull;
    return h;
}

static void state_hash_init(void)
{
    const char *path = getenv("DC_STATEHASH");
    if (!path) return;
    hash_file = fopen(path, "w");
    if (!hash_file) fprintf(stderr, "[hash] can't write %s\n", path);
    if (getenv("DC_STATEEVERY")) hash_every = (uint32_t)strtoul(getenv("DC_STATEEVERY"), NULL, 0);
    if (!hash_every) hash_every = 1;
}

static void state_hash(const sh4_ctx *ctx)
{
    uint32_t frame = holly_frame_count();

    if (!hash_file || frame < hash_next) return;
    hash_next = frame + hash_every;
    fprintf(hash_file, "frame %u time %llu ram %016llx vram %016llx aram %016llx cpu %016llx\n", frame,
            (unsigned long long)clock_ns(),
            (unsigned long long)hash_bytes(dc_ram, DC_RAM_SIZE),
            (unsigned long long)hash_bytes(dc_vram, DC_VRAM_SIZE),
            (unsigned long long)hash_bytes(dc_aram, DC_ARAM_SIZE),
            (unsigned long long)hash_bytes(ctx, sizeof *ctx));
    fflush(hash_file);
}

// ---- DC_PROF ----

static uint64_t prof_ns[PROF_PARTS];
static unsigned prof_underruns;

void prof_add(int part, uint64_t start_ns)
{
    prof_ns[part] += clock_host_ns() - start_ns;
}

void prof_underrun(void)
{
    prof_underruns++;
}

void prof_frame(uint32_t frame)
{
    static long every = -1;
    static uint32_t from;
    static uint64_t from_ns;
    if (every < 0) {
        every = getenv("DC_PROF") ? atol(getenv("DC_PROF")) : 0;
        from = frame;
        from_ns = clock_host_ns();
    }
    if (every <= 0 || frame < from + (uint32_t)every) return;

    static const char *names[PROF_PARTS] = { "render", "arm", "mix", "present" };
    uint64_t now = clock_host_ns(), total = now - from_ns, rest = total;
    double secs = (double)total / 1e9;
    fprintf(stderr, "[prof] frames %u-%u: %.2f s, %.1f fps |", from, frame, secs, (frame - from) / secs);
    for (int i = 0; i < PROF_PARTS; i++) {
        fprintf(stderr, " %s %.0f%%", names[i], 100.0 * (double)prof_ns[i] / (double)total);
        rest -= prof_ns[i] < rest ? prof_ns[i] : rest;
        prof_ns[i] = 0;
    }
    fprintf(stderr, " game %.0f%% | audio underruns %u\n", 100.0 * (double)rest / (double)total, prof_underruns);
    prof_underruns = 0;
    from = frame;
    from_ns = now;
}

void dc_service_irq(sh4_ctx *ctx)
{
    static unsigned long long since_irq;

    if (++since_irq == WATCHDOG)
        stop(ctx, "no interrupt taken for %llu checks, probably waiting on hardware", since_irq);
    if (++checks % CHECK_EVERY) return;

    clock_tick(CHECK_EVERY);
    if (max_frames && holly_frame_count() >= max_frames)
        stop(ctx, "ran %u frames (DC_FRAMES), %llu interrupts taken", holly_frame_count(), interrupts);
    aica_update();
    video_frame();
    prof_frame(holly_frame_count());
    state_hash(ctx);

    int level = holly_pending_level();
    uint32_t intevt = level == 6 ? 0x320u : level == 4 ? 0x360u : 0x3a0u, tmu_evt = 0;

    // on a tie the external (Holly) interrupt goes first
    int tmu_level = tmu_pending(sh4_reg(INTC_IPRA) & 0xffffu, &tmu_evt);
    if (tmu_level > level) {
        level = tmu_level;
        intevt = tmu_evt;
    }

    uint32_t sr = sh4_get_sr(ctx);
    if (!level || level <= (int)((sr >> 4) & 15) || (sr & SR_BL))
        return;

    // what the SH-4 does when it takes an interrupt. There's no PC to save:
    // returning from here is "continue where we were"
    ctx->ssr = sr;
    ctx->sgr = ctx->r[15];
    sh4_set_sr(ctx, sr | SR_MD | SR_RB | SR_BL);   // switches to register bank 1
    sh4reg_write32(0xff000028u, intevt);   // INTEVT

    interrupts++;
    since_irq = 0;
    static int irqlog = -1;   // DC_IRQLOG=n: print the first n interrupts taken
    if (irqlog < 0) irqlog = getenv("DC_IRQLOG") ? atoi(getenv("DC_IRQLOG")) : 0;
    if (irqlog > 0) {
        irqlog--;
        fprintf(stderr, "[irq] level %d, INTEVT %03x, frame %u, ISTNRM %08x ISTEXT %08x\n", level, intevt,
                holly_frame_count(), holly_read32(HOLLY_SB_ISTNRM), holly_read32(HOLLY_SB_ISTEXT));
    }
    dc_call(ctx, ctx->vbr + 0x600);   // the handler ends with rte
}

#ifdef DC_TRACE
// last functions entered, oldest first when printed
// with the stack pointer at entry, for finding stack mixups
static uint32_t trace_ring[64], trace_sp[64];
static unsigned trace_pos;

void dc_trace_enter(uint32_t addr, uint32_t sp)
{
    static int live = -1;   // DC_TRACELOG=n: also print the first n entries as they happen
    if (live < 0) live = getenv("DC_TRACELOG") ? atoi(getenv("DC_TRACELOG")) : 0;
    if (live > 0) { live--; fprintf(stderr, "[fn] %08x  sp %08x\n", addr, sp); }
    trace_sp[trace_pos % 64] = sp;
    trace_ring[trace_pos++ % 64] = addr;
}
#endif

// printed however the run ends (stop, BIOS exit, entry point returning)
static void summary(void)
{
    uint32_t frames = holly_frame_count();
    // checks per frame: a starting point for DC_VCLOCK
    fprintf(stderr, "[run] %u frames, %llu interrupts taken, %llu checks per frame\n", frames, interrupts,
            frames ? checks / frames : 0);
    if (nmisses)
        fprintf(stderr, "[run] %u missed targets, %llu instructions interpreted\n", nmisses,
                (unsigned long long)dc_interp_count());
    video_shutdown();
#ifdef DC_TRACE
    fprintf(stderr, "[run] last functions entered (function/sp):");
    for (unsigned i = 0; i < 64; i++) {
        unsigned k = trace_pos + i;
        if (k >= 64 && trace_ring[k % 64])
            fprintf(stderr, "%s%08x/%08x", i % 4 ? "  " : "\n   ", trace_ring[k % 64], trace_sp[k % 64]);
    }
    fprintf(stderr, "\n");
#endif
}

// ---- CPU helpers that need hardware ----

void sh4_pref(sh4_ctx *ctx, uint32_t addr)
{
    (void)ctx;
    if ((addr & 0xfc000000u) == 0xe0000000u)
        mem_sq_flush(addr);
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
    maple_init();
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
    video_init();
    state_hash_init();
    miss_init();

    fprintf(stderr, "starting at 8c010000 (%d recompiled functions)\n", dc_func_count);
    f_8c010000(&cpu);
    fprintf(stderr, "entry point returned\n");
    dump(&cpu);
    return 0;
}
