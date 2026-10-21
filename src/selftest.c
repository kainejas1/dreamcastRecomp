#include "selftest.h"
#include "sh4.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    uint32_t pc;
    uint16_t raw;
    const char *expect;
} vector;

/* Hand-assembled from the SH-4 manual. Default pc is 0x8c010000. */
static const vector vectors[] = {
    { 0x8c010000, 0x0009, "nop" },
    { 0x8c010000, 0x000B, "rts" },
    { 0x8c010000, 0x002B, "rte" },
    { 0x8c010000, 0x0018, "sett" },
    { 0x8c010000, 0x0019, "div0u" },
    { 0x8c010000, 0xE1FF, "mov\t#-1,r1" },
    { 0x8c010000, 0x6123, "mov\tr2,r1" },
    { 0x8c010000, 0x2F86, "mov.l\tr8,@-r15" },
    { 0x8c010000, 0x4F22, "sts.l\tpr,@-r15" },
    { 0x8c010000, 0x4F26, "lds.l\t@r15+,pr" },
    { 0x8c010000, 0x7F08, "add\t#8,r15" },
    { 0x8c010000, 0x5143, "mov.l\t@(12,r4),r1" },
    { 0x8c010000, 0x1F12, "mov.l\tr1,@(8,r15)" },
    { 0x8c010000, 0x8411, "mov.b\t@(1,r1),r0" },
    { 0x8c010000, 0xC90F, "and\t#15,r0" },
    { 0x8c010000, 0xCAFF, "xor\t#255,r0" },
    { 0x8c010000, 0xCD01, "and.b\t#1,@(r0,gbr)" },
    { 0x8c010000, 0xC601, "mov.l\t@(4,gbr),r0" },
    { 0x8c010000, 0xC3FF, "trapa\t#255" },
    { 0x8c010000, 0x4115, "cmp/pl\tr1" },
    { 0x8c010000, 0x3120, "cmp/eq\tr2,r1" },
    { 0x8c010000, 0xA002, "bra\t8c010008" },
    { 0x8c010000, 0xAFFE, "bra\t8c010000" },
    { 0x8c010000, 0x8BFE, "bf\t8c010000" },
    { 0x8c010000, 0x8D01, "bt.s\t8c010006" },
    { 0x8c010002, 0xD101, "mov.l\t8c010008,r1" },
    { 0x8c010002, 0x9101, "mov.w\t8c010008,r1" },
    { 0x8c010002, 0xC701, "mova\t8c010008,r0" },
    { 0x8c010000, 0x410B, "jsr\t@r1" },
    { 0x8c010000, 0x402B, "jmp\t@r0" },
    { 0x8c010000, 0x0123, "braf\tr1" },
    { 0x8c010000, 0x4E0E, "ldc\tr14,sr" },
    { 0x8c010000, 0x4F8E, "ldc\tr15,r0_bank" },
    { 0x8c010000, 0x0F82, "stc\tr0_bank,r15" },
    { 0x8c010000, 0x4166, "lds.l\t@r1+,fpscr" },
    { 0x8c010000, 0xF00C, "fmov\tfr0,fr0" },
    { 0x8c010000, 0xF12C, "fmov\tfr2,fr1" },
    { 0x8c010000, 0xF018, "fmov.s\t@r1,fr0" },
    { 0x8c010000, 0xF52E, "fmac\tfr0,fr2,fr5" },
    { 0x8c010000, 0xF17D, "fsrra\tfr1" },
    { 0x8c010000, 0xF0FD, "fsca\tfpul,dr0" },
    { 0x8c010000, 0xF2FD, "fsca\tfpul,dr2" },
    { 0x8c010000, 0xF1FD, "ftrv\txmtrx,fv0" },
    { 0x8c010000, 0xFDFD, "ftrv\txmtrx,fv12" },
    { 0x8c010000, 0xF6ED, "fipr\tfv8,fv4" },
    { 0x8c010000, 0xFBFD, "frchg" },
    { 0x8c010000, 0xF3FD, "fschg" },
    { 0x8c010000, 0xF2BD, "fcnvds\tdr2,fpul" },
    { 0x8c010000, 0xF4AD, "fcnvsd\tfpul,dr4" },
    { 0x8c010000, 0xFFFD, ".word\t0xfffd" },
    { 0x8c010000, 0xF1BD, ".word\t0xf1bd" },
};

/* Register writes implied by the operand template, as an independent check of
 * the F_WN / F_WM / F_WR0 flags in the table: @-Rn and @Rn+ update Rn, @Rm+
 * updates Rm, and the last operand is the destination unless the op only
 * compares or tests. */
static uint32_t expected_writes(const sh4_opinfo *info)
{
    static const char *const readonly[] = { "cmp/", "tst", "div0s", "dmul", "mul", "braf", "bsrf" };
    const char *ops = info->operands, *last = ops;
    uint32_t w = 0;
    int depth = 0;

    for (const char *p = ops; *p; p++) {
        if (*p == '(') depth++;
        else if (*p == ')') depth--;
        else if (*p == ',' && depth == 0) last = p + 1;
    }
    if (strstr(ops, "@-%n") || strstr(ops, "@%n+")) w |= F_WN;
    if (strstr(ops, "@%m+")) w |= F_WM;
    for (size_t i = 0; i < sizeof readonly / sizeof readonly[0]; i++)
        if (!strncmp(info->mnemonic, readonly[i], strlen(readonly[i]))) return w;
    if (!strcmp(last, "%n")) w |= F_WN;
    else if (!strcmp(last, "r0")) w |= F_WR0;
    return w;
}

int sh4_selftest(int verbose)
{
    int errors = 0;
    sh4_init();

    /* 1. Patterns are well-formed. */
    for (int op = 1; op < SH4_OP_COUNT; op++) {
        const sh4_opinfo *info = sh4_info((sh4_op)op);
        size_t len = strlen(info->pattern);
        if (len != 16) {
            printf("BAD PATTERN  %-14s \"%s\" has %zu chars\n", info->name, info->pattern, len);
            errors++;
        }
        uint32_t want = expected_writes(info), have = info->flags & (F_WN | F_WM | F_WR0);
        if (want != have) {
            printf("WRITE FLAGS  %-14s \"%s %s\": table has %s%s%s, operands imply %s%s%s\n",
                   info->name, info->mnemonic, info->operands,
                   have & F_WN ? "WN " : "", have & F_WM ? "WM " : "", have & F_WR0 ? "WR0 " : "",
                   want & F_WN ? "WN " : "", want & F_WM ? "WM " : "", want & F_WR0 ? "WR0 " : "");
            errors++;
        }
    }

    /* 2. No encoding is claimed by two entries with the same specificity,
     *    and every entry is reachable. */
    static int reachable[SH4_OP_COUNT];
    memset(reachable, 0, sizeof reachable);
    int defined = 0, ambiguous = 0;
    for (uint32_t raw = 0; raw < 65536; raw++) {
        int best_bits = -1, best = 0, ties = 0;
        for (int op = 1; op < SH4_OP_COUNT; op++) {
            const sh4_opinfo *info = sh4_info((sh4_op)op);
            if ((raw & info->mask) != info->match) continue;
            int bits = 0;
            for (uint16_t v = info->mask; v; v &= v - 1) bits++;
            if (bits > best_bits) { best_bits = bits; best = op; ties = 0; }
            else if (bits == best_bits) {
                if (ambiguous < 20)
                    printf("AMBIGUOUS    %04x matches %s and %s\n", raw,
                           sh4_info((sh4_op)best)->name, info->name);
                ties++;
            }
        }
        if (ties) ambiguous++;
        if (best) { defined++; reachable[best] = 1; }
    }
    for (int op = 1; op < SH4_OP_COUNT; op++) {
        if (!reachable[op]) {
            printf("UNREACHABLE  %s\n", sh4_info((sh4_op)op)->name);
            errors++;
        }
    }
    errors += ambiguous;

    /* 3. Known encodings disassemble as expected. */
    int vfail = 0;
    for (size_t i = 0; i < sizeof vectors / sizeof vectors[0]; i++) {
        const vector *v = &vectors[i];
        sh4_insn in;
        char buf[96];
        sh4_decode(v->pc, v->raw, &in);
        sh4_format(&in, buf, sizeof buf);
        if (strcmp(buf, v->expect) != 0) {
            printf("MISMATCH     %08x %04x: got \"%s\" expected \"%s\"\n", v->pc, v->raw, buf, v->expect);
            vfail++;
        } else if (verbose) {
            printf("ok           %04x  %s\n", v->raw, buf);
        }
    }
    errors += vfail;

    printf("%d opcode entries, %d/65536 encodings defined, %d ambiguous, %zu vectors (%d failed)\n",
           SH4_OP_COUNT - 1, defined, ambiguous, sizeof vectors / sizeof vectors[0], vfail);
    printf(errors ? "SELFTEST FAILED\n" : "SELFTEST PASSED\n");
    return errors == 0;
}
