#include "analysis.h"
#include "emit.h"
#include "loader.h"
#include "selftest.h"
#include "sh4.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(void)
{
    fprintf(stderr,
        "usage:\n"
        "  dcrecomp disasm <file> [options]   disassemble executable regions\n"
        "  dcrecomp stats  <file> [options]   opcode histogram and unknown words\n"
        "  dcrecomp funcs  <file> [options]   discover functions, print coverage\n"
        "  dcrecomp emit   <file> [options]   translate functions to C\n"
        "  dcrecomp selftest [-v]             check the opcode table\n"
        "\n"
        "options:\n"
        "  --bin            treat file as a raw binary (e.g. 1ST_READ.BIN)\n"
        "  --base ADDR      load address for --bin (default 0x%08x)\n"
        "  --start ADDR     only disassemble from ADDR\n"
        "  --end ADDR       only disassemble up to ADDR (exclusive)\n"
        "  --out DIR        emit: output directory (default out)\n"
        "  --only ADDR      emit: just the function at ADDR, to stdout\n",
        DC_DEFAULT_BASE);
}

typedef struct {
    const char *path;
    int raw_bin;
    uint32_t base, start, end;
    const char *out_dir;   /* emit */
    uint32_t only;         /* emit */
} options;

static int parse_options(int argc, char **argv, options *o)
{
    memset(o, 0, sizeof *o);
    o->base = DC_DEFAULT_BASE;
    o->end = 0xFFFFFFFFu;
    for (int i = 0; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--bin")) o->raw_bin = 1;
        else if (!strcmp(a, "--base") && i + 1 < argc) o->base = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(a, "--start") && i + 1 < argc) o->start = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(a, "--end") && i + 1 < argc) o->end = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(a, "--out") && i + 1 < argc) o->out_dir = argv[++i];
        else if (!strcmp(a, "--only") && i + 1 < argc) o->only = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (a[0] != '-' && !o->path) o->path = a;
        else { fprintf(stderr, "unknown option: %s\n", a); return 0; }
    }
    if (!o->path) { fprintf(stderr, "missing input file\n"); return 0; }
    return 1;
}

static int load(const options *o, dc_image *img)
{
    char err[256];
    int ok = o->raw_bin ? dc_load_bin(img, o->path, o->base, err, sizeof err)
                        : dc_load_elf(img, o->path, err, sizeof err);
    if (!ok) fprintf(stderr, "error: %s\n", err);
    return ok;
}

static int cmd_disasm(const options *o)
{
    dc_image img;
    if (!load(o, &img)) return 1;

    for (int r = 0; r < img.nregions; r++) {
        const dc_region *reg = &img.regions[r];
        uint32_t start = reg->start > o->start ? reg->start : (o->start & ~1u);
        uint32_t end = reg->end < o->end ? reg->end : o->end;
        if (start >= end) continue;

        printf("\nDisassembly of section %s:\n", reg->name);
        for (uint32_t pc = start; pc + 2 <= end; pc += 2) {
            uint16_t raw;
            if (!dc_read16(&img, pc, &raw)) break;

            const char *sym = dc_symbol_at(&img, pc);
            if (sym) printf("\n%08x <%s>:\n", pc, sym);

            sh4_insn in;
            char text[96];
            sh4_decode(pc, raw, &in);
            sh4_format(&in, text, sizeof text);
            printf("%08x:\t%02x %02x \t%s", pc, raw & 0xFF, raw >> 8, text);

            /* Show the literal a PC-relative load picks up. */
            if (in.op == SH4_OP_MOVL_PC) {
                uint32_t v;
                if (dc_read32(&img, in.target, &v)) printf("\t! 0x%08x", v);
            } else if (in.op == SH4_OP_MOVW_PC) {
                uint16_t v;
                if (dc_read16(&img, in.target, &v)) printf("\t! 0x%04x", v);
            }
            putchar('\n');
        }
    }
    dc_image_free(&img);
    return 0;
}

typedef struct { uint16_t raw; uint32_t count; } word_count;

static int by_count_desc(const void *a, const void *b)
{
    const word_count *x = a, *y = b;
    return (y->count > x->count) - (y->count < x->count);
}

static int cmd_stats(const options *o)
{
    dc_image img;
    if (!load(o, &img)) return 1;

    static uint32_t op_count[SH4_OP_COUNT];
    static word_count unknown[65536];
    uint32_t total = 0;
    for (int i = 0; i < 65536; i++) unknown[i].raw = (uint16_t)i;

    for (int r = 0; r < img.nregions; r++) {
        const dc_region *reg = &img.regions[r];
        uint32_t start = reg->start > o->start ? reg->start : (o->start & ~1u);
        uint32_t end = reg->end < o->end ? reg->end : o->end;
        for (uint32_t pc = start; pc + 2 <= end; pc += 2) {
            uint16_t raw;
            sh4_insn in;
            if (!dc_read16(&img, pc, &raw)) break;
            sh4_decode(pc, raw, &in);
            op_count[in.op]++;
            if (in.op == SH4_OP_UNKNOWN) unknown[raw].count++;
            total++;
        }
    }

    printf("entry 0x%08x, %d region(s), %d symbol(s), %u halfwords\n", img.entry, img.nregions, img.nsyms, total);
    for (int r = 0; r < img.nregions; r++)
        printf("  %-16s %08x-%08x\n", img.regions[r].name, img.regions[r].start, img.regions[r].end);

    printf("\nunknown: %u (%.2f%%)\n", op_count[SH4_OP_UNKNOWN], total ? 100.0 * op_count[SH4_OP_UNKNOWN] / total : 0.0);
    qsort(unknown, 65536, sizeof unknown[0], by_count_desc);
    for (int i = 0; i < 20 && unknown[i].count; i++)
        printf("  %04x  x%u\n", unknown[i].raw, unknown[i].count);

    printf("\nmost common:\n");
    for (int shown = 0; shown < 25; shown++) {
        int best = -1;
        for (int op = 1; op < SH4_OP_COUNT; op++)
            if (op_count[op] && (best < 0 || op_count[op] > op_count[best])) best = op;
        if (best < 0) break;
        printf("  %-16s %u\n", sh4_info((sh4_op)best)->name, op_count[best]);
        op_count[best] = 0;
    }

    dc_image_free(&img);
    return 0;
}

/* Load the file and analyse the span covering all executable regions. */
static int load_and_analyze(const options *o, dc_image *img, dc_analysis *a)
{
    if (!load(o, img)) return 0;
    uint32_t lo = 0xFFFFFFFFu, hi = 0;
    for (int r = 0; r < img->nregions; r++) {
        if (img->regions[r].start < lo) lo = img->regions[r].start;
        if (img->regions[r].end > hi) hi = img->regions[r].end;
    }
    if (!dc_analyze(a, img, lo, hi - lo)) {
        fprintf(stderr, "error: analysis failed\n");
        dc_image_free(img);
        return 0;
    }
    return 1;
}

static int cmd_funcs(const options *o)
{
    dc_image img;
    dc_analysis a;
    if (!load_and_analyze(o, &img, &a)) return 1;

    uint32_t n = a.size / 2, code = 0, data = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (a.kind[i] == BYTE_CODE) code++;
        else if (a.kind[i] == BYTE_DATA) data++;
    }
    for (int i = 0; i < a.nfuncs; i++) {
        const dc_func *f = &a.funcs[i];
        const char *sym = dc_symbol_at(&img, f->start);
        printf("%08x %08x %6u %s%s%s\n", f->start, f->end, f->end - f->start,
               f->flags & FUNC_HAS_JUMP_TABLE ? "[switch] " : "",
               f->flags & FUNC_HAS_UNRESOLVED_JUMP ? "[indirect jmp] " : "", sym ? sym : "");
    }
    fprintf(stderr, "%d functions; code %.1f%%, data %.1f%%, unknown %.1f%% of %u halfwords\n",
            a.nfuncs, 100.0 * code / n, 100.0 * data / n, 100.0 * (n - code - data) / n, n);

    dc_analysis_free(&a);
    dc_image_free(&img);
    return 0;
}

static int cmd_emit(const options *o)
{
    dc_image img;
    dc_analysis a;
    if (!load_and_analyze(o, &img, &a)) return 1;

    dc_emit_options eo = { o->out_dir ? o->out_dir : "out", o->only, 200 };
    int ok = dc_emit(&a, &eo);

    dc_analysis_free(&a);
    dc_image_free(&img);
    return ok ? 0 : 1;
}

int main(int argc, char **argv)
{
    if (argc < 2) { usage(); return 2; }
    sh4_init();

    const char *cmd = argv[1];
    if (!strcmp(cmd, "selftest"))
        return sh4_selftest(argc > 2 && !strcmp(argv[2], "-v")) ? 0 : 1;

    options o;
    if (!parse_options(argc - 2, argv + 2, &o)) { usage(); return 2; }
    if (!strcmp(cmd, "disasm")) return cmd_disasm(&o);
    if (!strcmp(cmd, "stats")) return cmd_stats(&o);
    if (!strcmp(cmd, "funcs")) return cmd_funcs(&o);
    if (!strcmp(cmd, "emit")) return cmd_emit(&o);

    usage();
    return 2;
}
