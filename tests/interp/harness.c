/*
 * Runs the program from prog.py twice, recompiled and then interpreted, against
 * plain memory, and checks that:
 *  - both left the results prog.py expects
 *  - both left exactly the same CPU state and memory
 *  - the recompiled run missed the targets it should have
 *
 *   harness prog.bin expected.txt --expect-misses N [--misses-out FILE]
 *
 * Built twice, from code emitted without and with --roots, which shows a miss
 * log going back through the recompiler makes the misses go away.
 */

#include "interp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RAM_MASK 0x00ffffffu
#define RAM_SIZE (RAM_MASK + 1u)
#define ENTRY    0x8c010000u
#define RESULTS  0x8c080000u

void f_8c010000(sh4_ctx *ctx);

static uint8_t ram[RAM_SIZE], ram_native[RAM_SIZE];
static sh4_ctx cpu, cpu_native;
static int interpret_everything;
static FILE *miss_file;
static int nmisses;

// ---- what the generated code and the interpreter need from a runtime ----

volatile int dc_irq_pending = 1;

void dc_service_irq(sh4_ctx *ctx) { (void)ctx; }

uint8_t mem_r8(uint32_t a) { return ram[a & RAM_MASK]; }
uint16_t mem_r16(uint32_t a) { uint16_t v; memcpy(&v, &ram[a & RAM_MASK], 2); return v; }
uint32_t mem_r32(uint32_t a) { uint32_t v; memcpy(&v, &ram[a & RAM_MASK], 4); return v; }
uint64_t mem_r64(uint32_t a) { uint64_t v; memcpy(&v, &ram[a & RAM_MASK], 8); return v; }
void mem_w8(uint32_t a, uint8_t v) { ram[a & RAM_MASK] = v; }
void mem_w16(uint32_t a, uint16_t v) { memcpy(&ram[a & RAM_MASK], &v, 2); }
void mem_w32(uint32_t a, uint32_t v) { memcpy(&ram[a & RAM_MASK], &v, 4); }
void mem_w64(uint32_t a, uint64_t v) { memcpy(&ram[a & RAM_MASK], &v, 8); }

static void die(const char *what, uint32_t a, uint32_t b)
{
    printf("FAILED: %s %08x %08x\n", what, a, b);
    exit(1);
}

sh4_func dc_lookup(uint32_t addr)
{
    if (interpret_everything) return NULL;
    int lo = 0, hi = dc_func_count - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (dc_func_table[mid].addr < addr) lo = mid + 1;
        else if (dc_func_table[mid].addr > addr) hi = mid - 1;
        else return dc_func_table[mid].fn;
    }
    return NULL;
}

static int run_known(sh4_ctx *ctx, uint32_t addr)
{
    sh4_func f = dc_lookup(addr);
    if (f) {
        f(ctx);
        return 1;
    }
    if (!interpret_everything) {
        nmisses++;
        if (miss_file) fprintf(miss_file, "%08x main\n", addr);
    }
    return 0;
}

void dc_call(sh4_ctx *ctx, uint32_t addr) { if (!run_known(ctx, addr)) dc_interp(ctx, addr); }
void dc_jump(sh4_ctx *ctx, uint32_t addr) { if (!run_known(ctx, addr)) dc_interp(ctx, addr); }
int dc_try_jump(sh4_ctx *ctx, uint32_t addr) { return run_known(ctx, addr); }

void dc_unhandled(sh4_ctx *ctx, uint32_t pc, uint16_t raw) { (void)ctx; die("unhandled", pc, raw); }
dc_jmpbuf *dc_setjmp_here(sh4_ctx *ctx, uint32_t buf) { (void)ctx; die("setjmp", buf, 0); return NULL; }
void dc_longjmp(sh4_ctx *ctx, uint32_t buf) { (void)ctx; die("longjmp", buf, 0); }

void sh4_rte(sh4_ctx *ctx) { sh4_set_sr(ctx, ctx->ssr); }
void sh4_pref(sh4_ctx *ctx, uint32_t addr) { (void)ctx; (void)addr; }
void sh4_trapa(sh4_ctx *ctx, uint32_t imm, uint32_t pc) { (void)ctx; die("trapa", imm, pc); }
void sh4_sleep(sh4_ctx *ctx) { (void)ctx; die("sleep", 0, 0); }
void sh4_ldtlb(sh4_ctx *ctx) { (void)ctx; }

// ---- the test ----

static void reset(const char *path)
{
    memset(ram, 0, sizeof ram);
    memset(&cpu, 0, sizeof cpu);
    cpu.sr = SR_MD;
    cpu.r[15] = 0x8c0f0000u;

    FILE *f = fopen(path, "rb");
    if (!f) die("can't read program", 0, 0);
    size_t n = fread(&ram[ENTRY & RAM_MASK], 1, RAM_SIZE - (ENTRY & RAM_MASK), f);
    fclose(f);
    if (!n) die("empty program", 0, 0);
}

static int check_results(const char *path, const char *run)
{
    FILE *f = fopen(path, "r");
    if (!f) die("can't read expected results", 0, 0);

    int bad = 0, n = 0;
    unsigned v;
    for (; fscanf(f, "%x", &v) == 1; n++) {
        uint32_t got = mem_r32(RESULTS + 4u * (uint32_t)n);
        if (got != v) {
            printf("%s: result %d is %08x, expected %08x\n", run, n, got, v);
            bad = 1;
        }
    }
    fclose(f);
    if (!n) die("no expected results", 0, 0);
    printf("%s: %d results checked\n", run, n);
    return !bad;
}

static int check_same(void)
{
    int ok = 1;

    if (memcmp(&cpu, &cpu_native, sizeof cpu)) {
        const uint32_t *x = (const uint32_t *)&cpu_native, *y = (const uint32_t *)&cpu;
        for (size_t i = 0; i < sizeof cpu / 4; i++)
            if (x[i] != y[i]) printf("CPU word %u: recompiled %08x, interpreted %08x\n", (unsigned)i, x[i], y[i]);
        ok = 0;
    }
    for (size_t i = 0; i < RAM_SIZE; i++)
        if (ram[i] != ram_native[i]) {
            printf("RAM %06x: recompiled %02x, interpreted %02x\n", (unsigned)i, ram_native[i], ram[i]);
            ok = 0;
            break;
        }
    return ok;
}

int main(int argc, char **argv)
{
    int want_misses = -1;
    const char *miss_path = NULL;

    if (argc < 3) {
        printf("usage: %s prog.bin expected.txt --expect-misses N [--misses-out FILE]\n", argv[0]);
        return 2;
    }
    for (int i = 3; i + 1 < argc; i += 2) {
        if (!strcmp(argv[i], "--expect-misses")) want_misses = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "--misses-out")) miss_path = argv[i + 1];
    }
    if (miss_path) {
        miss_file = fopen(miss_path, "w");
        if (!miss_file) die("can't write the miss log", 0, 0);
    }

    reset(argv[1]);
    f_8c010000(&cpu);
    if (miss_file) fclose(miss_file);
    int ok = check_results(argv[2], "recompiled");
    memcpy(ram_native, ram, sizeof ram);
    cpu_native = cpu;
    uint64_t before = dc_interp_count();

    interpret_everything = 1;
    reset(argv[1]);
    dc_interp(&cpu, ENTRY);
    ok = check_results(argv[2], "interpreted") && ok;
    ok = check_same() && ok;

    printf("%d calls to missed targets; %llu instructions interpreted in the recompiled run, %llu in the interpreted one\n",
           nmisses, (unsigned long long)before, (unsigned long long)(dc_interp_count() - before));
    if (want_misses == 0 && nmisses) { printf("expected no misses\n"); ok = 0; }
    if (want_misses > 0 && nmisses != want_misses) { printf("expected %d misses\n", want_misses); ok = 0; }

    printf(ok ? "INTERPRETER TEST PASSED\n" : "INTERPRETER TEST FAILED\n");
    return ok ? 0 : 1;
}
