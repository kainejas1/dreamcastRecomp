#include "emit.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#define MAKE_DIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define MAKE_DIR(p) mkdir(p, 0777)
#endif

/*
 * C emitter: every function the analysis found becomes a C function
 * void f_XXXXXXXX(sh4_ctx *ctx) working on the CPU state in runtime/sh4_ctx.h.
 *
 *   collect_body()   re-trace one function on its own
 *   emit_insn()      one instruction -> C
 *   emit_control()   branches, calls, returns and their delay slots
 *   emit_function()  labels, fall-through gotos, IRQ checks
 *   dc_emit()        output files: funcs.h, funcs_NNN.c, lookup.c
 *
 * One function at a time, to stdout:
 *   dcrecomp emit extracted/sa2/1ST_READ.BIN --bin --only 0x8c1004e0
 */

/* What one function consists of, found by re-tracing it on its own. */
typedef struct {
    uint32_t *insns;    /* addresses of its instructions, sorted, delay slots included */
    int ninsns;
    uint32_t *labels;   /* addresses something branches to, sorted */
    int nlabels;
} func_body;

// code from a --copy region: the game copies it at runtime and may patch its
// literal pool afterwards (Shinobi's interrupt stub does, and keeps r0 in it),
// so literals there are read at runtime instead of inlined
static int in_copied_code(const dc_analysis *a, uint32_t pc)
{
    for (int i = 0; i < a->img->nregions; i++) {
        const dc_region *r = &a->img->regions[i];
        if (pc >= r->start && pc < r->end && !strcmp(r->name, "copy")) return 1;
    }
    return 0;
}

static void free_body(func_body *b)
{
    free(b->insns);
    free(b->labels);
    memset(b, 0, sizeof *b);
}

// worklist for collect_body()
#define MAX_FUNC_BYTES 0x10000
#define WINDOW_BACK    0x8000   // some code sits before the entry point, allow going back this far
#define MAX_WORK       (MAX_FUNC_BYTES / 2)
#define MAX_INSNS      (MAX_FUNC_BYTES / 2)   // biggest SA2 function is ~11k insns
#define MAX_LABELS     4096
static int cmp_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a;
    uint32_t y = *(const uint32_t *)b;
    return (x > y) - (x < y);
}

static int add_unique(uint32_t *items, int *count, int capacity,
                      uint32_t value)
{
    for (int i = 0; i < *count; ++i) {
        if (items[i] == value)
            return 1;
    }

    if (*count >= capacity)
        return 0;

    items[(*count)++] = value;
    return 1;
}

static int queue_pc(uint32_t *work, int *nwork, uint32_t pc)
{
    if (*nwork >= MAX_WORK)
        return 0;

    work[(*nwork)++] = pc;
    return 1;
}

/* follow the function's control flow with its own visited set (not the
 * analysis's global code map), stopping at other functions' starts. Use
 * dc_resolve_reg() and dc_jump_table() for jmp/braf targets. */
static int collect_body(const dc_analysis *a, const dc_func *f,
                        func_body *body)
{
    memset(body, 0, sizeof *body);

    body->insns = malloc(MAX_INSNS * sizeof *body->insns);
    body->labels = malloc(MAX_LABELS * sizeof *body->labels);

    uint8_t *seen = calloc(MAX_FUNC_BYTES / 2, sizeof *seen);
    uint32_t *work = malloc(MAX_WORK * sizeof *work);

    if (!body->insns || !body->labels || !seen || !work)
        goto fail;

    int nwork = 0;
    uint32_t lo = f->start - WINDOW_BACK;   // seen[] covers [lo, lo + MAX_FUNC_BYTES)

    if (!queue_pc(work, &nwork, f->start))
        goto fail;

    while (nwork > 0) {
        uint32_t pc = work[--nwork];

        for (;;) {
            if (pc < lo ||
                pc - lo >= MAX_FUNC_BYTES)
                break;

            if (pc != f->start && dc_func_at(a, pc))
                break;

            uint32_t index = (pc - lo) / 2;

            // seen: 1 = traced, 2 = only seen as someone's delay slot. A branch
            // straight into a slot still has to be traced onward from there.
            if (seen[index] == 1)
                break;

            uint16_t raw;
            sh4_insn in;

            if (!dc_read16(a->img, pc, &raw))
                goto fail;

            sh4_decode(pc, raw, &in);

            seen[index] = 1;

            if (!add_unique(body->insns, &body->ninsns,
                            MAX_INSNS, pc))
                goto fail;

            /*
             * Delay slots are part of the function body, but are emitted
             * by emit_control(). Mark them seen so they aren't traversed
             * as independent instructions.
             */
            if (in.flags & F_DELAY) {
                uint32_t slot = pc + 2;

                if (slot < lo ||
                    slot - lo >= MAX_FUNC_BYTES)
                    goto fail;

                if (!add_unique(body->insns, &body->ninsns,
                                MAX_INSNS, slot))
                    goto fail;

                if (!seen[(slot - lo) / 2])
                    seen[(slot - lo) / 2] = 2;
            }

            switch (in.op) {
            case SH4_OP_BT:
            case SH4_OP_BF:
                if (!add_unique(body->labels, &body->nlabels,
                                MAX_LABELS, in.target))
                    goto fail;

                if (!queue_pc(work, &nwork, in.target))
                    goto fail;

                pc += 2;
                continue;

            case SH4_OP_BTS:
            case SH4_OP_BFS:
                if (!add_unique(body->labels, &body->nlabels,
                                MAX_LABELS, in.target))
                    goto fail;

                if (!queue_pc(work, &nwork, in.target))
                    goto fail;

                pc += 4;
                continue;

            case SH4_OP_BRA:
                if (!add_unique(body->labels, &body->nlabels,
                                MAX_LABELS, in.target))
                    goto fail;

                if (!queue_pc(work, &nwork, in.target))
                    goto fail;

                break;

            case SH4_OP_BSR:
            case SH4_OP_JSR:
            case SH4_OP_BSRF:
                pc += 4;
                continue;

            case SH4_OP_JMP:
            case SH4_OP_BRAF: {
                uint32_t targets[DC_MAX_TABLE];
                int dispatch = 0;

                int n = dc_jump_table(a, f->start, pc, targets,
                                      DC_MAX_TABLE, &dispatch);

                if (n > 0) {
                    for (int j = 0; j < n; ++j) {
                        if (dc_func_at(a, targets[j]))
                            continue;

                        if (!add_unique(body->labels, &body->nlabels,
                                        MAX_LABELS, targets[j]))
                            goto fail;

                        if (!queue_pc(work, &nwork, targets[j]))
                            goto fail;
                    }
                } else {
                    // fixed target: if it's inside this function (even its own
                    // start) it's a goto and needs a label
                    uint32_t t;
                    if (dc_resolve_reg(a, pc, in.n, &t)) {
                        if (in.op == SH4_OP_BRAF)
                            t += pc + 4;
                        if (t == f->start || !dc_func_at(a, t)) {
                            if (!add_unique(body->labels, &body->nlabels, MAX_LABELS, t) ||
                                !queue_pc(work, &nwork, t))
                                goto fail;
                        }
                    }
                }

                (void)dispatch;
                break;
            }

            case SH4_OP_RTS:
            case SH4_OP_RTE:
                break;

            default:
                if (in.flags & F_BRANCH)
                    break;
                if (in.op == SH4_OP_UNKNOWN) // ran into data
                    break;

                pc += 2;
                continue;
            }

            break;
        }
    }

    // a slot that is also a branch target gets emitted twice (once inside its
    // branch, once on its own). If its branch doesn't end the block (call,
    // bt/s), the code inside the branch then needs a label to continue at.
    for (int i = 0; i < body->ninsns; i++) {
        uint16_t raw;
        sh4_insn in;
        if (!dc_read16(a->img, body->insns[i], &raw)) continue;
        sh4_decode(body->insns[i], raw, &in);
        if (!(in.flags & F_DELAY) || !(in.flags & (F_CALL | F_COND))) continue;
        for (int j = 0; j < body->nlabels; j++) {
            if (body->labels[j] == in.pc + 2) {
                if (!add_unique(body->labels, &body->nlabels, MAX_LABELS, in.pc + 4))
                    goto fail;
                break;
            }
        }
    }

    qsort(body->insns, body->ninsns, sizeof *body->insns, cmp_u32);

    // code below the entry point (reached by a backward branch) sorts first,
    // but the function still has to start at f->start: emit_function jumps there
    if (body->ninsns && body->insns[0] != f->start &&
        !add_unique(body->labels, &body->nlabels, MAX_LABELS, f->start))
        goto fail;

    qsort(body->labels, body->nlabels, sizeof *body->labels, cmp_u32);

    free(seen);
    free(work);
    return 1;

fail:
    free(seen);
    free(work);
    free_body(body);
    return 0;
}

static void emit_insn(FILE *out, const dc_analysis *a, const sh4_insn *in);

// --setjmp / --longjmp. Games switch threads with them, which plain C calls
// can't follow; the runtime does it with a fiber per thread (runtime/coro.c)
static uint32_t setjmp_fn, longjmp_fn;

// whose functions these are: "f" for the main program, "m_stg09" for a
// module (--modules). A module's calls into the main program (main_a) are
// direct calls too
static const char *prefix = "f";
static const dc_analysis *main_a;
// main program functions a module calls, declared in the module's own header
// so modules don't include funcs.h (and don't all rebuild when it changes)
static uint32_t *ext_calls;
static int next_calls, ext_cap;

// name prefix for calling target directly, NULL if it's not a function we know
static const char *callee(const dc_analysis *a, uint32_t target)
{
    if (dc_func_at(a, target)) return prefix;
    if (main_a && main_a != a && dc_func_at(main_a, target)) {
        if (next_calls == ext_cap) {
            ext_cap = ext_cap ? ext_cap * 2 : 256;
            ext_calls = realloc(ext_calls, (size_t)ext_cap * sizeof *ext_calls);
            if (!ext_calls) { fprintf(stderr, "emit: out of memory\n"); exit(1); }
        }
        ext_calls[next_calls++] = target;
        return "f";
    }
    return NULL;
}

// a direct call to a known function
static void emit_call(FILE *out, uint32_t ret, const char *name, uint32_t target)
{
    if (setjmp_fn && target == setjmp_fn)
        fprintf(out, "    ctx->pr = 0x%08xu; DC_SETJMP(ctx, %s_%08x);\n", ret, name, target);
    else if (longjmp_fn && target == longjmp_fn)
        fprintf(out, "    ctx->pr = 0x%08xu; DC_LONGJMP(ctx, %s_%08x);\n", ret, name, target);
    else
        fprintf(out, "    ctx->pr = 0x%08xu; %s_%08x(ctx);\n", ret, name, target);
}

static void emit_slot(FILE *out, const dc_analysis *a, const sh4_insn *branch)
{
    uint16_t raw;
    sh4_insn slot;
    if (dc_read16(a->img, branch->pc + 2, &raw)) {
        sh4_decode(branch->pc + 2, raw, &slot);
        emit_insn(out, a, &slot);
    }
}

// jump to target: a label in this function, or else leave the function
// (tail call into a known function, or let the runtime find it)
static void emit_goto(FILE *out, const dc_analysis *a, const func_body *body,
                      uint32_t from, uint32_t target)
{
    const char *name;
    if (bsearch(&target, body->insns, body->ninsns, sizeof *body->insns, cmp_u32)) {
        if (target <= from) // backward: give interrupts a chance
            fprintf(out, "{ DC_CHECK_IRQ(ctx); goto L_%08x; }\n", target);
        else
            fprintf(out, "goto L_%08x;\n", target);
    } else if ((name = callee(a, target))) {
        if (longjmp_fn && target == longjmp_fn)
            fprintf(out, "{ DC_LONGJMP(ctx, %s_%08x); return; }\n", name, target);
        else
            fprintf(out, "{ %s_%08x(ctx); return; }\n", name, target);
        // setjmp would come back to our caller's caller, no host frame for that
        if (setjmp_fn && target == setjmp_fn)
            fprintf(stderr, "emit: tail jump to setjmp at %08x, not supported\n", from);
    } else {
        fprintf(out, "{ dc_jump(ctx, 0x%08xu); return; }\n", target);
    }
}

/* a branch, call or return at `in`, together with its delay slot. */
static void emit_control(FILE *out, const dc_analysis *a, const dc_func *f,
                         const func_body *body, const sh4_insn *in)
{
    uint32_t target;
    const char *name;

    switch (in->op) {
    case SH4_OP_BSR:
        emit_slot(out, a, in);
        if ((name = callee(a, in->target)))
            emit_call(out, in->pc + 4, name, in->target);
        else
            fprintf(out, "    ctx->pr = 0x%08xu; dc_call(ctx, 0x%08xu);\n", in->pc + 4, in->target);
        break;

    case SH4_OP_BF:
        fprintf(out, "    if (!ctx->t) ");
        emit_goto(out, a, body, in->pc, in->target);
        break;

    case SH4_OP_BT:
        fprintf(out, "    if (ctx->t) ");
        emit_goto(out, a, body, in->pc, in->target);
        break;

    // T has to be read before the slot runs, the slot may change it
    case SH4_OP_BFS:
    case SH4_OP_BTS:
        fprintf(out, "    {\n    uint32_t c = ctx->t;\n");
        emit_slot(out, a, in);
        fprintf(out, in->op == SH4_OP_BFS ? "    if (!c) " : "    if (c) ");
        emit_goto(out, a, body, in->pc, in->target);
        fprintf(out, "    }\n");
        break;

    case SH4_OP_BRA:
        emit_slot(out, a, in);
        fprintf(out, "    ");
        emit_goto(out, a, body, in->pc, in->target);
        break;

    case SH4_OP_JSR:
        if (!in_copied_code(a, in->pc) && dc_resolve_reg(a, in->pc, in->n, &target) && (name = callee(a, target))) {
            emit_slot(out, a, in);
            emit_call(out, in->pc + 4, name, target);
        } else {
            // computed call (callbacks etc): grab Rn before the slot
            fprintf(out, "    {\n    uint32_t t = ctx->r[%u];\n", in->n);
            emit_slot(out, a, in);
            fprintf(out, "    ctx->pr = 0x%08xu; dc_call(ctx, t);\n    }\n", in->pc + 4);
        }
        break;

    case SH4_OP_BSRF:
        // target is pc + 4 + Rn, not Rn
        if (!in_copied_code(a, in->pc) && dc_resolve_reg(a, in->pc, in->n, &target) && (name = callee(a, in->pc + 4 + target))) {
            emit_slot(out, a, in);
            emit_call(out, in->pc + 4, name, in->pc + 4 + target);
        } else {
            fprintf(out, "    {\n    uint32_t t = 0x%08xu + ctx->r[%u];\n", in->pc + 4, in->n);
            emit_slot(out, a, in);
            fprintf(out, "    ctx->pr = 0x%08xu; dc_call(ctx, t);\n    }\n", in->pc + 4);
        }
        break;

    case SH4_OP_JMP:
    case SH4_OP_BRAF: {
        uint32_t targets[DC_MAX_TABLE];
        int dispatch = 0;
        int n = dc_jump_table(a, f->start, in->pc,
                              targets, DC_MAX_TABLE, &dispatch);

        int fixed = n == 0 && !in_copied_code(a, in->pc) && dc_resolve_reg(a, in->pc, in->n, &target);

        // where it goes, worked out before the slot runs (braf is pc+4 relative)
        if (fixed)
            fprintf(out, "    {\n");
        else if (in->op == SH4_OP_BRAF)
            fprintf(out, "    {\n    uint32_t t = 0x%08xu + ctx->r[%u];\n", in->pc + 4, in->n);
        else
            fprintf(out, "    {\n    uint32_t t = ctx->r[%u];\n", in->n);
        emit_slot(out, a, in);

        if (n > 0 && !dispatch) {
            fprintf(out, "    switch (t) {\n");
            for (int i = 0; i < n; ++i) {
                int dup = 0;
                for (int j = 0; j < i; ++j)     // several cases can share code
                    if (targets[j] == targets[i]) dup = 1;
                if (dup) continue;
                fprintf(out, "    case 0x%08xu: ", targets[i]);
                emit_goto(out, a, body, in->pc, targets[i]);
            }
            fprintf(out, "    default: dc_jump(ctx, t); return;\n    }\n");
        } else if (fixed) {
            if (in->op == SH4_OP_BRAF)
                target += in->pc + 4;
            fprintf(out, "    ");
            emit_goto(out, a, body, in->pc, target);
        } else {
            // dispatch table or computed jump: runtime looks it up
            fprintf(out, "    dc_jump(ctx, t); return;\n");
        }
        fprintf(out, "    }\n");
        break;
    }

    case SH4_OP_RTS:
    case SH4_OP_RTE:
        emit_slot(out, a, in);
        if (in->op == SH4_OP_RTE)
            fprintf(out, "    sh4_rte(ctx);\n");
        fprintf(out, "    return;\n");
        break;

    default:
        fprintf(out,
                "    dc_unhandled(ctx, 0x%08xu, 0x%04x);\n",
                in->pc, in->raw);
        break;
    }
}

// fmov with FPSCR.SZ = 1 moves register pairs: an even register number
// means DRn (fr bank), odd means XD(n-1) (the xf bank)
static const char *pair_bank(unsigned r) { return (r & 1) ? "xf" : "fr"; }

// fmov load/store for both SZ modes. addr is a C expression for the address,
// step is how far Rn/Rm moves for @Rm+ (+1) / @-Rn (-1), 0 for none
static void emit_fmov_mem(FILE *out, int load, unsigned fr, const char *addr,
                          unsigned reg, int step)
{
    const char *b = pair_bank(fr);
    unsigned p = fr & 0xE;

    fprintf(out, "    if (ctx->fpscr & FPSCR_SZ) {\n");
    if (step < 0) {
        fprintf(out, "        mem_w32(ctx->r[%u] - 8, ctx->%s.u[%u]); mem_w32(ctx->r[%u] - 4, ctx->%s.u[%u]);\n",
                reg, b, p, reg, b, p + 1);
        fprintf(out, "        ctx->r[%u] -= 8;\n", reg);
    } else if (load) {
        // first word -> even reg, same as flycast (dr_hex is a LE u64 over fr[2n],fr[2n+1]).
        // so a double in memory has its high word first, that's how sh4 does it
        fprintf(out, "        uint32_t ad = %s;\n", addr);
        fprintf(out, "        ctx->%s.u[%u] = mem_r32(ad); ctx->%s.u[%u] = mem_r32(ad + 4);\n", b, p, b, p + 1);
    } else {
        fprintf(out, "        uint32_t ad = %s;\n", addr);
        fprintf(out, "        mem_w32(ad, ctx->%s.u[%u]); mem_w32(ad + 4, ctx->%s.u[%u]);\n", b, p, b, p + 1);
    }
    if (step > 0)
        fprintf(out, "        ctx->r[%u] += 8;\n", reg);
    fprintf(out, "    } else {\n");
    if (step < 0)
        fprintf(out, "        mem_w32(ctx->r[%u] - 4, ctx->fr.u[%u]); ctx->r[%u] -= 4;\n", reg, fr, reg);
    else if (load)
        fprintf(out, "        ctx->fr.u[%u] = mem_r32(%s);\n", fr, addr);
    else
        fprintf(out, "        mem_w32(%s, ctx->fr.u[%u]);\n", addr, fr);
    if (step > 0)
        fprintf(out, "        ctx->r[%u] += 4;\n", reg);
    fprintf(out, "    }\n");
}

// control register field for ldc/stc (and their .l forms), NULL for sr / banks
static const char *ctl_field(sh4_op op)
{
    switch (op) {
    case SH4_OP_LDC_GBR: case SH4_OP_LDCL_GBR: case SH4_OP_STC_GBR: case SH4_OP_STCL_GBR: return "gbr";
    case SH4_OP_LDC_VBR: case SH4_OP_LDCL_VBR: case SH4_OP_STC_VBR: case SH4_OP_STCL_VBR: return "vbr";
    case SH4_OP_LDC_SSR: case SH4_OP_LDCL_SSR: case SH4_OP_STC_SSR: case SH4_OP_STCL_SSR: return "ssr";
    case SH4_OP_LDC_SPC: case SH4_OP_LDCL_SPC: case SH4_OP_STC_SPC: case SH4_OP_STCL_SPC: return "spc";
    case SH4_OP_LDC_DBR: case SH4_OP_LDCL_DBR: case SH4_OP_STC_DBR: case SH4_OP_STCL_DBR: return "dbr";
    case SH4_OP_STC_SGR: case SH4_OP_STCL_SGR: return "sgr";
    case SH4_OP_LDS_MACH: case SH4_OP_LDSL_MACH: case SH4_OP_STS_MACH: case SH4_OP_STSL_MACH: return "mach";
    case SH4_OP_LDS_MACL: case SH4_OP_LDSL_MACL: case SH4_OP_STS_MACL: case SH4_OP_STSL_MACL: return "macl";
    case SH4_OP_LDS_PR: case SH4_OP_STS_PR: return "pr";
    default: return NULL;
    }
}

// fadd/fsub/fmul/fdiv: single or double depending on FPSCR.PR
static void emit_farith(FILE *out, const sh4_insn *in, char op)
{
    fprintf(out, "    if (ctx->fpscr & FPSCR_PR) sh4_set_dr(&ctx->fr, %u, sh4_get_dr(&ctx->fr, %u) %c sh4_get_dr(&ctx->fr, %u));\n",
            in->n & 0xE, in->n & 0xE, op, in->m & 0xE);
    fprintf(out, "    else ctx->fr.f[%u] %c= ctx->fr.f[%u];\n", in->n, op, in->m);
}

/* one non-control-flow instruction. `a` gives access to the image,
 * e.g. to inline the literal a mov.l @(disp,PC) loads. */
static void emit_insn(FILE *out, const dc_analysis *a, const sh4_insn *in)
{
    (void)a;
    switch (in->op) {
        case SH4_OP_STSL_PR: 
            fprintf(out, "  ctx->r[%u] -=4; mem_w32(ctx->r[%u], ctx->pr);\n", in->n, in->n);
            break;
        case SH4_OP_MOVL_S:
            fprintf(out, "  mem_w32(ctx->r[%u], ctx->r[%u]);\n", in->n, in->m);
            break;
        case SH4_OP_CMPHS:
            fprintf(out, "  ctx->t = ctx->r[%u] >= ctx->r[%u];\n", in->n, in->m);
            break;
        case SH4_OP_LDSL_PR:
            fprintf(out, "  ctx->pr = mem_r32(ctx->r[%u]); ctx->r[%u] += 4;\n", in->n, in->n);
            break;
        case SH4_OP_ADD_I:
            fprintf(out, "    ctx->r[%u] += 0x%08xu;\n", in->n, (uint32_t)in->imm);
            break;
        case SH4_OP_MOVL_PC: {                   /* mov.l @(disp,PC),Rn: inline the literal */
            uint32_t v = 0;
            if (in_copied_code(a, in->pc) || dc_literal_written(a, in->target)) {   // a variable, not a constant
                fprintf(out, "    ctx->r[%u] = mem_r32(0x%08xu);\n", in->n, in->target);
                break;
            }
            dc_read32(a->img, in->target, &v);
            fprintf(out, "    ctx->r[%u] = 0x%08xu;\n", in->n, v);
            break;
        }
        case SH4_OP_NOP: 
            break;
        case SH4_OP_MOV_I:
            fprintf(out, "    ctx->r[%u] = (uint32_t)(int32_t)%d;\n",
                    in->n, in->imm);
            break;

        case SH4_OP_MOV:
            fprintf(out, "    ctx->r[%u] = ctx->r[%u];\n",
                    in->n, in->m);
            break;

        case SH4_OP_MOVL_L:
            fprintf(out, "    ctx->r[%u] = mem_r32(ctx->r[%u]);\n",
                    in->n, in->m);
            break;

        case SH4_OP_MOVL_L4:
            fprintf(out, "    ctx->r[%u] = mem_r32(ctx->r[%u] + %d);\n",
                    in->n, in->m, in->disp);
            break;

        case SH4_OP_MOVL_S4:
            fprintf(out, "    mem_w32(ctx->r[%u] + %d, ctx->r[%u]);\n",
                    in->n, in->disp, in->m);
            break;

        case SH4_OP_MOVL_LINC:
            // mov.l @Rm+,Rm keeps the loaded value, no increment
            if (in->n == in->m)
                fprintf(out, "    ctx->r[%u] = mem_r32(ctx->r[%u]);\n", in->n, in->m);
            else
            fprintf(out,
                    "    ctx->r[%u] = mem_r32(ctx->r[%u]); ctx->r[%u] += 4;\n",
                    in->n, in->m, in->m);
            break;

        case SH4_OP_MOVL_SDEC:
            // store first: with n == m the old value is what gets written
            fprintf(out,
                    "    mem_w32(ctx->r[%u] - 4, ctx->r[%u]); ctx->r[%u] -= 4;\n",
                    in->n, in->m, in->n);
            break;

        case SH4_OP_ADD:
            fprintf(out, "    ctx->r[%u] += ctx->r[%u];\n",
                    in->n, in->m);
            break;
        case SH4_OP_FLDI0:
            fprintf(out, "    ctx->fr.u[%u] = 0x00000000u;\n",
                    in->n);
            break;

        case SH4_OP_FLDI1:
            fprintf(out, "    ctx->fr.u[%u] = 0x3f800000u;\n",
                    in->n);
            break;

        case SH4_OP_FLDS:
            fprintf(out, "    ctx->fpul = ctx->fr.u[%u];\n",
                    in->n);
            break;

        case SH4_OP_FSTS:
            fprintf(out, "    ctx->fr.u[%u] = ctx->fpul;\n",
                    in->n);
            break;

        case SH4_OP_FABS:
            fprintf(out, "    ctx->fr.u[%u] &= 0x7fffffffu;\n",
                    in->n);
            break;

        case SH4_OP_FNEG:
            fprintf(out, "    ctx->fr.u[%u] ^= 0x80000000u;\n",
                    in->n);
            break;

        case SH4_OP_FSCHG:
            fprintf(out, "    ctx->fpscr ^= FPSCR_SZ;\n");
            break;

        case SH4_OP_FRCHG:
            fprintf(out,
                    "    sh4_set_fpscr(ctx, ctx->fpscr ^ FPSCR_FR);\n");
            break;

        // ---- compares / tests ----
        case SH4_OP_TST:
            fprintf(out, "    ctx->t = (ctx->r[%u] & ctx->r[%u]) == 0;\n", in->n, in->m);
            break;
        case SH4_OP_TST_I:
            fprintf(out, "    ctx->t = (ctx->r[0] & 0x%xu) == 0;\n", (uint32_t)in->imm);
            break;
        case SH4_OP_CMPEQ_I:   // imm is sign extended
            fprintf(out, "    ctx->t = ctx->r[0] == 0x%08xu;\n", (uint32_t)in->imm);
            break;
        case SH4_OP_CMPEQ:
            fprintf(out, "    ctx->t = ctx->r[%u] == ctx->r[%u];\n", in->n, in->m);
            break;
        case SH4_OP_CMPHI:
            fprintf(out, "    ctx->t = ctx->r[%u] > ctx->r[%u];\n", in->n, in->m);
            break;
        case SH4_OP_CMPGE:
            fprintf(out, "    ctx->t = (int32_t)ctx->r[%u] >= (int32_t)ctx->r[%u];\n", in->n, in->m);
            break;
        case SH4_OP_CMPGT:
            fprintf(out, "    ctx->t = (int32_t)ctx->r[%u] > (int32_t)ctx->r[%u];\n", in->n, in->m);
            break;
        case SH4_OP_CMPPL:
            fprintf(out, "    ctx->t = (int32_t)ctx->r[%u] > 0;\n", in->n);
            break;
        case SH4_OP_CMPPZ:
            fprintf(out, "    ctx->t = (int32_t)ctx->r[%u] >= 0;\n", in->n);
            break;

        // ---- shifts without T ----
        case SH4_OP_SHLL2:  fprintf(out, "    ctx->r[%u] <<= 2;\n", in->n); break;
        case SH4_OP_SHLL8:  fprintf(out, "    ctx->r[%u] <<= 8;\n", in->n); break;
        case SH4_OP_SHLL16: fprintf(out, "    ctx->r[%u] <<= 16;\n", in->n); break;
        case SH4_OP_SHLR2:  fprintf(out, "    ctx->r[%u] >>= 2;\n", in->n); break;
        case SH4_OP_SHLR8:  fprintf(out, "    ctx->r[%u] >>= 8;\n", in->n); break;
        case SH4_OP_SHLR16: fprintf(out, "    ctx->r[%u] >>= 16;\n", in->n); break;

        // ---- extends ----
        case SH4_OP_EXTUB:
            fprintf(out, "    ctx->r[%u] = ctx->r[%u] & 0xffu;\n", in->n, in->m);
            break;
        case SH4_OP_EXTUW:
            fprintf(out, "    ctx->r[%u] = ctx->r[%u] & 0xffffu;\n", in->n, in->m);
            break;
        case SH4_OP_EXTSB:
            fprintf(out, "    ctx->r[%u] = (uint32_t)(int32_t)(int8_t)ctx->r[%u];\n", in->n, in->m);
            break;
        case SH4_OP_EXTSW:
            fprintf(out, "    ctx->r[%u] = (uint32_t)(int32_t)(int16_t)ctx->r[%u];\n", in->n, in->m);
            break;

        // ---- @(r0,Rm) loads/stores, b and w loads sign extend ----
        case SH4_OP_MOVL_L0:
            fprintf(out, "    ctx->r[%u] = mem_r32(ctx->r[0] + ctx->r[%u]);\n", in->n, in->m);
            break;
        case SH4_OP_MOVW_L0:
            fprintf(out, "    ctx->r[%u] = (uint32_t)(int32_t)(int16_t)mem_r16(ctx->r[0] + ctx->r[%u]);\n", in->n, in->m);
            break;
        case SH4_OP_MOVB_L0:
            fprintf(out, "    ctx->r[%u] = (uint32_t)(int32_t)(int8_t)mem_r8(ctx->r[0] + ctx->r[%u]);\n", in->n, in->m);
            break;
        case SH4_OP_MOVL_S0:
            fprintf(out, "    mem_w32(ctx->r[0] + ctx->r[%u], ctx->r[%u]);\n", in->n, in->m);
            break;
        case SH4_OP_MOVW_S0:
            fprintf(out, "    mem_w16(ctx->r[0] + ctx->r[%u], (uint16_t)ctx->r[%u]);\n", in->n, in->m);
            break;
        case SH4_OP_MOVB_S0:
            fprintf(out, "    mem_w8(ctx->r[0] + ctx->r[%u], (uint8_t)ctx->r[%u]);\n", in->n, in->m);
            break;

        case SH4_OP_MOVW_PC: {  // 16-bit literal, sign extended at emit time
            uint16_t v = 0;
            if (in_copied_code(a, in->pc) || dc_literal_written(a, in->target)) {
                fprintf(out, "    ctx->r[%u] = (uint32_t)(int32_t)(int16_t)mem_r16(0x%08xu);\n", in->n, in->target);
                break;
            }
            dc_read16(a->img, in->target, &v);
            fprintf(out, "    ctx->r[%u] = 0x%08xu;\n", in->n, (uint32_t)(int32_t)(int16_t)v);
            break;
        }

        // ---- fmov, all forms (SZ picks 32-bit or register pairs) ----
        case SH4_OP_FMOV: {
            const char *bn = pair_bank(in->n), *bm = pair_bank(in->m);
            unsigned pn = in->n & 0xE, pm = in->m & 0xE;
            fprintf(out, "    if (ctx->fpscr & FPSCR_SZ) { ctx->%s.u[%u] = ctx->%s.u[%u]; ctx->%s.u[%u] = ctx->%s.u[%u]; }\n",
                    bn, pn, bm, pm, bn, pn + 1, bm, pm + 1);
            fprintf(out, "    else ctx->fr.u[%u] = ctx->fr.u[%u];\n", in->n, in->m);
            break;
        }
        case SH4_OP_FMOV_LOAD: {         // fmov.s @Rm,FRn
            char ad[48];
            snprintf(ad, sizeof ad, "ctx->r[%u]", in->m);
            emit_fmov_mem(out, 1, in->n, ad, in->m, 0);
            break;
        }
        case SH4_OP_FMOV_STORE: {        // fmov.s FRm,@Rn
            char ad[48];
            snprintf(ad, sizeof ad, "ctx->r[%u]", in->n);
            emit_fmov_mem(out, 0, in->m, ad, in->n, 0);
            break;
        }
        case SH4_OP_FMOV_RESTORE: {      // fmov.s @Rm+,FRn
            char ad[48];
            snprintf(ad, sizeof ad, "ctx->r[%u]", in->m);
            emit_fmov_mem(out, 1, in->n, ad, in->m, +1);
            break;
        }
        case SH4_OP_FMOV_SAVE:           // fmov.s FRm,@-Rn
            emit_fmov_mem(out, 0, in->m, "", in->n, -1);
            break;
        case SH4_OP_FMOV_INDEX_LOAD: {   // fmov.s @(r0,Rm),FRn
            char ad[48];
            snprintf(ad, sizeof ad, "ctx->r[0] + ctx->r[%u]", in->m);
            emit_fmov_mem(out, 1, in->n, ad, in->m, 0);
            break;
        }
        case SH4_OP_FMOV_INDEX_STORE: {  // fmov.s FRm,@(r0,Rn)
            char ad[48];
            snprintf(ad, sizeof ad, "ctx->r[0] + ctx->r[%u]", in->n);
            emit_fmov_mem(out, 0, in->m, ad, in->n, 0);
            break;
        }

        // ---- FPU arithmetic (PR picks single or double) ----
        case SH4_OP_FADD: emit_farith(out, in, '+'); break;
        case SH4_OP_FSUB: emit_farith(out, in, '-'); break;
        case SH4_OP_FMUL: emit_farith(out, in, '*'); break;
        case SH4_OP_FDIV: emit_farith(out, in, '/'); break;
        case SH4_OP_FCMPEQ:
        case SH4_OP_FCMPGT: {
            const char *op = in->op == SH4_OP_FCMPEQ ? "==" : ">";
            fprintf(out, "    ctx->t = (ctx->fpscr & FPSCR_PR) ? sh4_get_dr(&ctx->fr, %u) %s sh4_get_dr(&ctx->fr, %u)\n"
                         "                                     : ctx->fr.f[%u] %s ctx->fr.f[%u];\n",
                    in->n & 0xE, op, in->m & 0xE, in->n, op, in->m);
            break;
        }
        case SH4_OP_FSQRT:
            fprintf(out, "    if (ctx->fpscr & FPSCR_PR) sh4_set_dr(&ctx->fr, %u, sqrt(sh4_get_dr(&ctx->fr, %u)));\n",
                    in->n & 0xE, in->n & 0xE);
            fprintf(out, "    else ctx->fr.f[%u] = sqrtf(ctx->fr.f[%u]);\n", in->n, in->n);
            break;
        case SH4_OP_FMAC:   // single only
            fprintf(out, "    ctx->fr.f[%u] = ctx->fr.f[0] * ctx->fr.f[%u] + ctx->fr.f[%u];\n", in->n, in->m, in->n);
            break;
        case SH4_OP_FSRRA:
            fprintf(out, "    ctx->fr.f[%u] = 1.0f / sqrtf(ctx->fr.f[%u]);\n", in->n, in->n);
            break;
        case SH4_OP_FLOAT:  // float fpul,FRn
            fprintf(out, "    if (ctx->fpscr & FPSCR_PR) sh4_set_dr(&ctx->fr, %u, (double)(int32_t)ctx->fpul);\n", in->n & 0xE);
            fprintf(out, "    else ctx->fr.f[%u] = (float)(int32_t)ctx->fpul;\n", in->n);
            break;
        case SH4_OP_FTRC:   // ftrc FRm,fpul (reg is in the n field), saturates
            fprintf(out, "    if (ctx->fpscr & FPSCR_PR) { double v = sh4_get_dr(&ctx->fr, %u);\n"
                         "        ctx->fpul = v != v ? 0x80000000u : v >= 2147483648.0 ? 0x7fffffffu : v < -2147483648.0 ? 0x80000000u : (uint32_t)(int32_t)v; }\n",
                    in->n & 0xE);
            fprintf(out, "    else { float v = ctx->fr.f[%u];\n"
                         "        ctx->fpul = v != v ? 0x80000000u : v >= 2147483648.0f ? 0x7fffffffu : v < -2147483648.0f ? 0x80000000u : (uint32_t)(int32_t)v; }\n",
                    in->n);
            break;
        case SH4_OP_FCNVDS: // fcnvds DRm,fpul (reg in n field)
            fprintf(out, "    { float f = (float)sh4_get_dr(&ctx->fr, %u); memcpy(&ctx->fpul, &f, 4); }\n", in->n & 0xE);
            break;
        case SH4_OP_FCNVSD:
            fprintf(out, "    { float f; memcpy(&f, &ctx->fpul, 4); sh4_set_dr(&ctx->fr, %u, f); }\n", in->n & 0xE);
            break;
        case SH4_OP_FIPR: { // fipr FVm,FVn: FVm index is in bits 9-8, FVn in 11-10
            unsigned vm = (in->n & 3) * 4, vn = in->n & 0xC;
            fprintf(out, "    ctx->fr.f[%u] = ctx->fr.f[%u] * ctx->fr.f[%u] + ctx->fr.f[%u] * ctx->fr.f[%u]"
                         " + ctx->fr.f[%u] * ctx->fr.f[%u] + ctx->fr.f[%u] * ctx->fr.f[%u];\n",
                    vn + 3, vm, vn, vm + 1, vn + 1, vm + 2, vn + 2, vm + 3, vn + 3);
            break;
        }
        case SH4_OP_FTRV:
            fprintf(out, "    sh4_ftrv(ctx, %u);\n", in->n & 0xC);
            break;
        case SH4_OP_FSCA:
            fprintf(out, "    sh4_fsca(ctx, %u);\n", in->n & 0xE);
            break;

        // ---- fpul / fpscr ----
        case SH4_OP_LDS_FPUL:  fprintf(out, "    ctx->fpul = ctx->r[%u];\n", in->n); break;
        case SH4_OP_STS_FPUL:  fprintf(out, "    ctx->r[%u] = ctx->fpul;\n", in->n); break;
        case SH4_OP_STS_FPSCR: fprintf(out, "    ctx->r[%u] = ctx->fpscr;\n", in->n); break;
        case SH4_OP_LDS_FPSCR: fprintf(out, "    sh4_set_fpscr(ctx, ctx->r[%u]);\n", in->n); break;
        case SH4_OP_LDSL_FPUL:
            fprintf(out, "    ctx->fpul = mem_r32(ctx->r[%u]); ctx->r[%u] += 4;\n", in->n, in->n);
            break;
        case SH4_OP_LDSL_FPSCR:
            fprintf(out, "    sh4_set_fpscr(ctx, mem_r32(ctx->r[%u])); ctx->r[%u] += 4;\n", in->n, in->n);
            break;
        case SH4_OP_STSL_FPUL:
            fprintf(out, "    mem_w32(ctx->r[%u] - 4, ctx->fpul); ctx->r[%u] -= 4;\n", in->n, in->n);
            break;
        case SH4_OP_STSL_FPSCR:
            fprintf(out, "    mem_w32(ctx->r[%u] - 4, ctx->fpscr); ctx->r[%u] -= 4;\n", in->n, in->n);
            break;

        // ---- byte / word moves. loads sign extend! ----
        case SH4_OP_MOVB_L:
            fprintf(out, "    ctx->r[%u] = (uint32_t)(int32_t)(int8_t)mem_r8(ctx->r[%u]);\n", in->n, in->m);
            break;
        case SH4_OP_MOVW_L:
            fprintf(out, "    ctx->r[%u] = (uint32_t)(int32_t)(int16_t)mem_r16(ctx->r[%u]);\n", in->n, in->m);
            break;
        case SH4_OP_MOVB_S:
            fprintf(out, "    mem_w8(ctx->r[%u], (uint8_t)ctx->r[%u]);\n", in->n, in->m);
            break;
        case SH4_OP_MOVW_S:
            fprintf(out, "    mem_w16(ctx->r[%u], (uint16_t)ctx->r[%u]);\n", in->n, in->m);
            break;
        case SH4_OP_MOVB_LINC:
        case SH4_OP_MOVW_LINC: {
            int b = in->op == SH4_OP_MOVB_LINC;
            fprintf(out, b ? "    ctx->r[%u] = (uint32_t)(int32_t)(int8_t)mem_r8(ctx->r[%u]);\n"
                           : "    ctx->r[%u] = (uint32_t)(int32_t)(int16_t)mem_r16(ctx->r[%u]);\n", in->n, in->m);
            if (in->n != in->m)   // @Rm+ into Rm keeps the loaded value
                fprintf(out, "    ctx->r[%u] += %d;\n", in->m, b ? 1 : 2);
            break;
        }
        case SH4_OP_MOVB_SDEC:
            fprintf(out, "    mem_w8(ctx->r[%u] - 1, (uint8_t)ctx->r[%u]); ctx->r[%u] -= 1;\n", in->n, in->m, in->n);
            break;
        case SH4_OP_MOVW_SDEC:
            fprintf(out, "    mem_w16(ctx->r[%u] - 2, (uint16_t)ctx->r[%u]); ctx->r[%u] -= 2;\n", in->n, in->m, in->n);
            break;
        // @(disp,Rn) forms: the base register is in the m field for b/w, value is r0
        case SH4_OP_MOVB_L4:
            fprintf(out, "    ctx->r[0] = (uint32_t)(int32_t)(int8_t)mem_r8(ctx->r[%u] + %d);\n", in->m, in->disp);
            break;
        case SH4_OP_MOVW_L4:
            fprintf(out, "    ctx->r[0] = (uint32_t)(int32_t)(int16_t)mem_r16(ctx->r[%u] + %d);\n", in->m, in->disp);
            break;
        case SH4_OP_MOVB_S4:
            fprintf(out, "    mem_w8(ctx->r[%u] + %d, (uint8_t)ctx->r[0]);\n", in->m, in->disp);
            break;
        case SH4_OP_MOVW_S4:
            fprintf(out, "    mem_w16(ctx->r[%u] + %d, (uint16_t)ctx->r[0]);\n", in->m, in->disp);
            break;
        // @(disp,GBR)
        case SH4_OP_MOVB_LG:
            fprintf(out, "    ctx->r[0] = (uint32_t)(int32_t)(int8_t)mem_r8(ctx->gbr + %d);\n", in->disp);
            break;
        case SH4_OP_MOVW_LG:
            fprintf(out, "    ctx->r[0] = (uint32_t)(int32_t)(int16_t)mem_r16(ctx->gbr + %d);\n", in->disp);
            break;
        case SH4_OP_MOVL_LG:
            fprintf(out, "    ctx->r[0] = mem_r32(ctx->gbr + %d);\n", in->disp);
            break;
        case SH4_OP_MOVB_SG:
            fprintf(out, "    mem_w8(ctx->gbr + %d, (uint8_t)ctx->r[0]);\n", in->disp);
            break;
        case SH4_OP_MOVW_SG:
            fprintf(out, "    mem_w16(ctx->gbr + %d, (uint16_t)ctx->r[0]);\n", in->disp);
            break;
        case SH4_OP_MOVL_SG:
            fprintf(out, "    mem_w32(ctx->gbr + %d, ctx->r[0]);\n", in->disp);
            break;
        case SH4_OP_MOVA:   // an address, not a load
            fprintf(out, "    ctx->r[0] = 0x%08xu;\n", in->target);
            break;
        case SH4_OP_MOVCAL:
            fprintf(out, "    mem_w32(ctx->r[%u], ctx->r[0]);\n", in->n);
            break;
        case SH4_OP_MOVT:
            fprintf(out, "    ctx->r[%u] = ctx->t;\n", in->n);
            break;
        case SH4_OP_SWAPB:
            fprintf(out, "    ctx->r[%u] = (ctx->r[%u] & 0xffff0000u) | ((ctx->r[%u] & 0xffu) << 8) | ((ctx->r[%u] >> 8) & 0xffu);\n",
                    in->n, in->m, in->m, in->m);
            break;
        case SH4_OP_SWAPW:
            fprintf(out, "    ctx->r[%u] = (ctx->r[%u] >> 16) | (ctx->r[%u] << 16);\n", in->n, in->m, in->m);
            break;
        case SH4_OP_XTRCT:
            fprintf(out, "    ctx->r[%u] = (ctx->r[%u] << 16) | (ctx->r[%u] >> 16);\n", in->n, in->m, in->n);
            break;

        // ---- arithmetic ----
        case SH4_OP_SUB:
            fprintf(out, "    ctx->r[%u] -= ctx->r[%u];\n", in->n, in->m);
            break;
        case SH4_OP_NEG:
            fprintf(out, "    ctx->r[%u] = 0 - ctx->r[%u];\n", in->n, in->m);
            break;
        case SH4_OP_DT:
            fprintf(out, "    ctx->r[%u] -= 1; ctx->t = ctx->r[%u] == 0;\n", in->n, in->n);
            break;
        // carry / overflow versions, straight from the manual's pseudocode
        case SH4_OP_ADDC:
            fprintf(out, "    { uint32_t a = ctx->r[%u], s = a + ctx->r[%u]; ctx->r[%u] = s + ctx->t; ctx->t = (a > s) | (s > ctx->r[%u]); }\n",
                    in->n, in->m, in->n, in->n);
            break;
        case SH4_OP_SUBC:
            fprintf(out, "    { uint32_t a = ctx->r[%u], d = a - ctx->r[%u]; ctx->r[%u] = d - ctx->t; ctx->t = (a < d) | (d < ctx->r[%u]); }\n",
                    in->n, in->m, in->n, in->n);
            break;
        case SH4_OP_NEGC:
            fprintf(out, "    { uint32_t d = 0 - ctx->r[%u]; ctx->r[%u] = d - ctx->t; ctx->t = (0 < d) | (d < ctx->r[%u]); }\n",
                    in->m, in->n, in->n);
            break;
        case SH4_OP_ADDV:
            fprintf(out, "    { uint32_t s = ctx->r[%u] + ctx->r[%u]; ctx->t = (~(ctx->r[%u] ^ ctx->r[%u]) & (ctx->r[%u] ^ s)) >> 31; ctx->r[%u] = s; }\n",
                    in->n, in->m, in->n, in->m, in->n, in->n);
            break;
        case SH4_OP_SUBV:
            fprintf(out, "    { uint32_t d = ctx->r[%u] - ctx->r[%u]; ctx->t = ((ctx->r[%u] ^ ctx->r[%u]) & (ctx->r[%u] ^ d)) >> 31; ctx->r[%u] = d; }\n",
                    in->n, in->m, in->n, in->m, in->n, in->n);
            break;
        case SH4_OP_CMPSTR:
            fprintf(out, "    { uint32_t x = ctx->r[%u] ^ ctx->r[%u]; ctx->t = !(x >> 24) | !((x >> 16) & 0xff) | !((x >> 8) & 0xff) | !(x & 0xff); }\n",
                    in->n, in->m);
            break;
        // division steps (div1 is a runtime helper, see runtime/sh4_helpers.c)
        case SH4_OP_DIV0U:
            fprintf(out, "    ctx->q = ctx->m = ctx->t = 0;\n");
            break;
        case SH4_OP_DIV0S:
            fprintf(out, "    ctx->q = ctx->r[%u] >> 31; ctx->m = ctx->r[%u] >> 31; ctx->t = ctx->q ^ ctx->m;\n", in->n, in->m);
            break;
        case SH4_OP_DIV1:
            fprintf(out, "    sh4_div1(ctx, %u, %u);\n", in->n, in->m);
            break;
        // multiplies go to mach/macl, never Rn
        case SH4_OP_MULL:
            fprintf(out, "    ctx->macl = ctx->r[%u] * ctx->r[%u];\n", in->n, in->m);
            break;
        case SH4_OP_MULSW:
            fprintf(out, "    ctx->macl = (uint32_t)((int32_t)(int16_t)ctx->r[%u] * (int16_t)ctx->r[%u]);\n", in->n, in->m);
            break;
        case SH4_OP_MULUW:
            fprintf(out, "    ctx->macl = (uint32_t)(uint16_t)ctx->r[%u] * (uint16_t)ctx->r[%u];\n", in->n, in->m);
            break;
        case SH4_OP_DMULS:
            fprintf(out, "    { int64_t p = (int64_t)(int32_t)ctx->r[%u] * (int32_t)ctx->r[%u]; ctx->mach = (uint32_t)((uint64_t)p >> 32); ctx->macl = (uint32_t)p; }\n",
                    in->n, in->m);
            break;
        case SH4_OP_DMULU:
            fprintf(out, "    { uint64_t p = (uint64_t)ctx->r[%u] * ctx->r[%u]; ctx->mach = (uint32_t)(p >> 32); ctx->macl = (uint32_t)p; }\n",
                    in->n, in->m);
            break;
        case SH4_OP_MACL:
            fprintf(out, "    sh4_mac_l(ctx, %u, %u);\n", in->n, in->m);
            break;
        case SH4_OP_MACW:
            fprintf(out, "    sh4_mac_w(ctx, %u, %u);\n", in->n, in->m);
            break;

        // ---- logic ----
        case SH4_OP_AND: fprintf(out, "    ctx->r[%u] &= ctx->r[%u];\n", in->n, in->m); break;
        case SH4_OP_OR:  fprintf(out, "    ctx->r[%u] |= ctx->r[%u];\n", in->n, in->m); break;
        case SH4_OP_XOR: fprintf(out, "    ctx->r[%u] ^= ctx->r[%u];\n", in->n, in->m); break;
        case SH4_OP_NOT: fprintf(out, "    ctx->r[%u] = ~ctx->r[%u];\n", in->n, in->m); break;
        // #imm,r0 forms: imm is zero extended here
        case SH4_OP_AND_I: fprintf(out, "    ctx->r[0] &= 0x%xu;\n", (uint32_t)in->imm); break;
        case SH4_OP_OR_I:  fprintf(out, "    ctx->r[0] |= 0x%xu;\n", (uint32_t)in->imm); break;
        case SH4_OP_XOR_I: fprintf(out, "    ctx->r[0] ^= 0x%xu;\n", (uint32_t)in->imm); break;
        // byte at gbr + r0, read-modify-write
        case SH4_OP_ANDB:
            fprintf(out, "    { uint32_t ad = ctx->gbr + ctx->r[0]; mem_w8(ad, mem_r8(ad) & 0x%xu); }\n", (uint32_t)in->imm);
            break;
        case SH4_OP_ORB:
            fprintf(out, "    { uint32_t ad = ctx->gbr + ctx->r[0]; mem_w8(ad, mem_r8(ad) | 0x%xu); }\n", (uint32_t)in->imm);
            break;
        case SH4_OP_XORB:
            fprintf(out, "    { uint32_t ad = ctx->gbr + ctx->r[0]; mem_w8(ad, mem_r8(ad) ^ 0x%xu); }\n", (uint32_t)in->imm);
            break;
        case SH4_OP_TSTB:
            fprintf(out, "    ctx->t = (mem_r8(ctx->gbr + ctx->r[0]) & 0x%xu) == 0;\n", (uint32_t)in->imm);
            break;
        case SH4_OP_TASB:
            fprintf(out, "    { uint8_t v = mem_r8(ctx->r[%u]); ctx->t = v == 0; mem_w8(ctx->r[%u], v | 0x80); }\n", in->n, in->n);
            break;

        // ---- shifts / rotates through T ----
        case SH4_OP_SHLL:
        case SH4_OP_SHAL:
            fprintf(out, "    ctx->t = ctx->r[%u] >> 31; ctx->r[%u] <<= 1;\n", in->n, in->n);
            break;
        case SH4_OP_SHLR:
            fprintf(out, "    ctx->t = ctx->r[%u] & 1; ctx->r[%u] >>= 1;\n", in->n, in->n);
            break;
        case SH4_OP_SHAR:
            fprintf(out, "    ctx->t = ctx->r[%u] & 1; ctx->r[%u] = (uint32_t)((int32_t)ctx->r[%u] >> 1);\n", in->n, in->n, in->n);
            break;
        case SH4_OP_ROTL:
            fprintf(out, "    ctx->t = ctx->r[%u] >> 31; ctx->r[%u] = (ctx->r[%u] << 1) | ctx->t;\n", in->n, in->n, in->n);
            break;
        case SH4_OP_ROTR:
            fprintf(out, "    ctx->t = ctx->r[%u] & 1; ctx->r[%u] = (ctx->r[%u] >> 1) | (ctx->t << 31);\n", in->n, in->n, in->n);
            break;
        case SH4_OP_ROTCL:
            fprintf(out, "    { uint32_t c = ctx->r[%u] >> 31; ctx->r[%u] = (ctx->r[%u] << 1) | ctx->t; ctx->t = c; }\n", in->n, in->n, in->n);
            break;
        case SH4_OP_ROTCR:
            fprintf(out, "    { uint32_t c = ctx->r[%u] & 1; ctx->r[%u] = (ctx->r[%u] >> 1) | (ctx->t << 31); ctx->t = c; }\n", in->n, in->n, in->n);
            break;
        // dynamic shifts: Rm >= 0 shifts left, negative shifts right by 32 - (Rm & 31)
        case SH4_OP_SHAD:
            fprintf(out, "    { int32_t s = (int32_t)ctx->r[%u];\n"
                         "      if (s >= 0) ctx->r[%u] <<= (s & 31);\n"
                         "      else if ((s & 31) == 0) ctx->r[%u] = (int32_t)ctx->r[%u] < 0 ? 0xffffffffu : 0;\n"
                         "      else ctx->r[%u] = (uint32_t)((int32_t)ctx->r[%u] >> ((~s & 31) + 1)); }\n",
                    in->m, in->n, in->n, in->n, in->n, in->n);
            break;
        case SH4_OP_SHLD:
            fprintf(out, "    { int32_t s = (int32_t)ctx->r[%u];\n"
                         "      if (s >= 0) ctx->r[%u] <<= (s & 31);\n"
                         "      else if ((s & 31) == 0) ctx->r[%u] = 0;\n"
                         "      else ctx->r[%u] >>= ((~s & 31) + 1); }\n",
                    in->m, in->n, in->n, in->n);
            break;

        // ---- status bits / system ----
        case SH4_OP_CLRT: fprintf(out, "    ctx->t = 0;\n"); break;
        case SH4_OP_SETT: fprintf(out, "    ctx->t = 1;\n"); break;
        case SH4_OP_CLRS: fprintf(out, "    ctx->s = 0;\n"); break;
        case SH4_OP_SETS: fprintf(out, "    ctx->s = 1;\n"); break;
        case SH4_OP_CLRMAC: fprintf(out, "    ctx->mach = ctx->macl = 0;\n"); break;

        case SH4_OP_LDC_SR:  fprintf(out, "    sh4_set_sr(ctx, ctx->r[%u]);\n", in->n); break;
        case SH4_OP_STC_SR:  fprintf(out, "    ctx->r[%u] = sh4_get_sr(ctx);\n", in->n); break;
        case SH4_OP_LDCL_SR: // bump Rn before set_sr, which may swap banks
            fprintf(out, "    { uint32_t v = mem_r32(ctx->r[%u]); ctx->r[%u] += 4; sh4_set_sr(ctx, v); }\n", in->n, in->n);
            break;
        case SH4_OP_STCL_SR:
            fprintf(out, "    mem_w32(ctx->r[%u] - 4, sh4_get_sr(ctx)); ctx->r[%u] -= 4;\n", in->n, in->n);
            break;
        // banked r0-r7 (the inactive bank lives in r_bank)
        case SH4_OP_LDC_BANK:  fprintf(out, "    ctx->r_bank[%u] = ctx->r[%u];\n", in->m & 7, in->n); break;
        case SH4_OP_STC_BANK:  fprintf(out, "    ctx->r[%u] = ctx->r_bank[%u];\n", in->n, in->m & 7); break;
        case SH4_OP_LDCL_BANK:
            fprintf(out, "    ctx->r_bank[%u] = mem_r32(ctx->r[%u]); ctx->r[%u] += 4;\n", in->m & 7, in->n, in->n);
            break;
        case SH4_OP_STCL_BANK:
            fprintf(out, "    mem_w32(ctx->r[%u] - 4, ctx->r_bank[%u]); ctx->r[%u] -= 4;\n", in->n, in->m & 7, in->n);
            break;
        // plain control / mac / pr registers
        case SH4_OP_LDC_GBR: case SH4_OP_LDC_VBR: case SH4_OP_LDC_SSR: case SH4_OP_LDC_SPC: case SH4_OP_LDC_DBR:
        case SH4_OP_LDS_MACH: case SH4_OP_LDS_MACL: case SH4_OP_LDS_PR:
            fprintf(out, "    ctx->%s = ctx->r[%u];\n", ctl_field(in->op), in->n);
            break;
        case SH4_OP_STC_GBR: case SH4_OP_STC_VBR: case SH4_OP_STC_SSR: case SH4_OP_STC_SPC: case SH4_OP_STC_DBR:
        case SH4_OP_STC_SGR: case SH4_OP_STS_MACH: case SH4_OP_STS_MACL: case SH4_OP_STS_PR:
            fprintf(out, "    ctx->r[%u] = ctx->%s;\n", in->n, ctl_field(in->op));
            break;
        case SH4_OP_LDCL_GBR: case SH4_OP_LDCL_VBR: case SH4_OP_LDCL_SSR: case SH4_OP_LDCL_SPC: case SH4_OP_LDCL_DBR:
        case SH4_OP_LDSL_MACH: case SH4_OP_LDSL_MACL:
            fprintf(out, "    ctx->%s = mem_r32(ctx->r[%u]); ctx->r[%u] += 4;\n", ctl_field(in->op), in->n, in->n);
            break;
        case SH4_OP_STCL_GBR: case SH4_OP_STCL_VBR: case SH4_OP_STCL_SSR: case SH4_OP_STCL_SPC: case SH4_OP_STCL_DBR:
        case SH4_OP_STCL_SGR: case SH4_OP_STSL_MACH: case SH4_OP_STSL_MACL:
            fprintf(out, "    mem_w32(ctx->r[%u] - 4, ctx->%s); ctx->r[%u] -= 4;\n", in->n, ctl_field(in->op), in->n);
            break;

        // cache ops: nothing to do on a PC. pref matters for the store queues
        case SH4_OP_OCBI: case SH4_OP_OCBP: case SH4_OP_OCBWB:
            break;
        case SH4_OP_PREF:
            fprintf(out, "    sh4_pref(ctx, ctx->r[%u]);\n", in->n);
            break;
        case SH4_OP_TRAPA:
            fprintf(out, "    sh4_trapa(ctx, %u, 0x%08xu);\n", (uint32_t)in->imm, in->pc);
            break;
        case SH4_OP_SLEEP: fprintf(out, "    sh4_sleep(ctx);\n"); break;
        case SH4_OP_LDTLB: fprintf(out, "    sh4_ldtlb(ctx);\n"); break;

        default:
            fprintf(out, "    dc_unhandled(ctx, 0x%08xu, 0x%04x);\n", in->pc, in->raw);
            break;
        }
}

/* the whole C function. */
static int emit_function(FILE *out, const dc_analysis *a, const dc_func *f)
{
    func_body body;
    if (!collect_body(a, f, &body)) return 0;

    fprintf(out, "void %s_%08x(sh4_ctx *ctx)\n{\n", prefix, f->start);
    fprintf(out, "    DC_CHECK_IRQ(ctx);\n");
    fprintf(out, "    DC_ENTER(ctx, 0x%08xu);\n", f->start);
    if (body.ninsns && body.insns[0] != f->start)   // body starts below the entry point
        fprintf(out, "    goto L_%08x;\n", f->start);

    for (int i = 0; i < body.ninsns; i++) {
        sh4_insn in;
        uint16_t raw;
        if (!dc_read16(a->img, body.insns[i], &raw)) break;
        sh4_decode(body.insns[i], raw, &in);

        // print a label if this instrctuion's address
        // is a branch target.

        for (int j = 0; j < body.nlabels; j++) {
            if (body.labels[j] == body.insns[i]) {
                fprintf(out, "L_%08x:\n", body.insns[i]);
                break;
            }
        }
        
        if (in.flags & (F_BRANCH | F_CALL | F_RETURN)) {
            emit_control(out, a, f, &body, &in);
            // skip the delay-slot instruction if this
            // control instruction has one already.

            // (unless something also jumps straight to the slot, then it
            // gets emitted again on its own with its label)
            uint32_t slot = in.pc + 2;
            if ((in.flags & F_DELAY) &&
                !bsearch(&slot, body.labels, body.nlabels, sizeof *body.labels, cmp_u32))
                i++;
        } else {
            emit_insn(out, a, &in);
        }

        // falls through to something that isn't emitted next (usually the
        // start of another function): jump there explicitly
        uint32_t next = in.pc + ((in.flags & F_DELAY) ? 4 : 2);
        int ends = in.op == SH4_OP_BRA || in.op == SH4_OP_JMP || in.op == SH4_OP_BRAF ||
                   in.op == SH4_OP_RTS || in.op == SH4_OP_RTE || in.op == SH4_OP_UNKNOWN;
        if (!ends && (i + 1 >= body.ninsns || body.insns[i + 1] != next)) {
            fprintf(out, "    ");
            emit_goto(out, a, &body, in.pc, next);
        }
    }
    fprintf(out, "}\n\n");
    free_body(&body);
    return 1;
}

// mkdir -p, good enough for out/sa2 style paths
static void make_dirs(const char *path)
{
    char buf[1024];
    size_t len = strlen(path);
    if (len >= sizeof buf) return;
    memcpy(buf, path, len + 1);
    for (char *p = buf + 1; ; p++) {
        if (*p == '/' || *p == '\\' || *p == '\0') {
            char c = *p;
            *p = '\0';
            MAKE_DIR(buf);   // already exists is fine
            if (c == '\0') break;
            *p = c;
        }
    }
}

// files are written as NAME.tmp and only replace NAME if they changed, so a
// re-emit doesn't make the build recompile everything
static FILE *open_out(const char *dir, const char *name)
{
    char path[1100];
    snprintf(path, sizeof path, "%s/%s.tmp", dir, name);
    FILE *f = fopen(path, "w");
    if (!f) fprintf(stderr, "emit: can't write %s\n", path);
    return f;
}

static int same_file(const char *a, const char *b)
{
    FILE *fa = fopen(a, "rb"), *fb = fopen(b, "rb");
    int same = fa && fb;
    while (same) {
        int ca = getc(fa), cb = getc(fb);
        if (ca != cb) same = 0;
        if (ca == EOF) break;
    }
    if (fa) fclose(fa);
    if (fb) fclose(fb);
    return same;
}

static int close_out(FILE *f, const char *dir, const char *name)
{
    char path[1100], tmp[1100];
    int ok = !ferror(f);
    fclose(f);
    snprintf(path, sizeof path, "%s/%s", dir, name);
    snprintf(tmp, sizeof tmp, "%s/%s.tmp", dir, name);
    if (ok && same_file(tmp, path)) { remove(tmp); return 1; }
    remove(path);   // rename won't replace a file on Windows
    if (!ok || rename(tmp, path)) { fprintf(stderr, "emit: can't write %s\n", path); return 0; }
    return 1;
}

// "STG09" -> "stg09", for C names and file names
static void c_name(char *dst, size_t size, const char *name)
{
    size_t n = 0;
    for (const char *p = name; *p && n + 1 < size; p++)
        dst[n++] = isalnum((unsigned char)*p) ? (char)tolower((unsigned char)*p) : '_';
    dst[n] = '\0';
}

/* One set of functions, the main program's or a module's, into dir:
 *   HEADER        prototypes
 *   STEM_NNN.c    the functions, `per` to a file, starting with `includes`
 *   LOOKUP        address -> function table (sorted) for the runtime, named
 *                 TABLE; COUNT (if not NULL) holds its size */
static int emit_set(const dc_analysis *a, const char *dir, const char *header, const char *stem,
                    const char *includes, const char *lookup, const char *table, const char *count,
                    int per, int *nfiles_out)
{
    char guard[64], name[96];
    size_t g = 0;
    for (const char *p = header; *p && g + 1 < sizeof guard; p++)
        guard[g++] = *p == '.' ? '_' : (char)toupper((unsigned char)*p);
    guard[g] = '\0';

    int nfiles = 0;
    next_calls = 0;
    FILE *out = NULL;
    for (int i = 0; i < a->nfuncs; i++) {
        if (i % per == 0) {
            if (out && !close_out(out, dir, name)) return 0;
            snprintf(name, sizeof name, "%s_%03d.c", stem, nfiles++);
            out = open_out(dir, name);
            if (!out) return 0;
            fprintf(out, "/* generated by dcrecomp, don't edit */\n%s\n", includes);
        }
        if (!emit_function(out, a, &a->funcs[i])) {
            fprintf(stderr, "emit: %s_%08x: collect_body() failed\n", prefix, a->funcs[i].start);
            fclose(out);
            return 0;
        }
    }
    if (out && !close_out(out, dir, name)) return 0;

    // after the functions, which collect what a module calls in the main program
    FILE *h = open_out(dir, header);
    if (!h) return 0;
    fprintf(h, "/* generated by dcrecomp, don't edit */\n#ifndef %s\n#define %s\n\n#include \"sh4_rt.h\"\n\n", guard, guard);
    for (int i = 0; i < a->nfuncs; i++)
        fprintf(h, "void %s_%08x(sh4_ctx *ctx);\n", prefix, a->funcs[i].start);
    if (next_calls) {
        qsort(ext_calls, (size_t)next_calls, sizeof *ext_calls, cmp_u32);
        fprintf(h, "\n/* in the main program */\n");
        for (int i = 0; i < next_calls; i++)
            if (!i || ext_calls[i] != ext_calls[i - 1])
                fprintf(h, "void f_%08x(sh4_ctx *ctx);\n", ext_calls[i]);
    }
    fprintf(h, "\n#endif\n");
    if (!close_out(h, dir, header)) return 0;

    // a->funcs is sorted by address already, so the runtime can binary search
    FILE *l = open_out(dir, lookup);
    if (!l) return 0;
    fprintf(l, "/* generated by dcrecomp, don't edit */\n#include \"%s\"\n\n", header);
    fprintf(l, "const dc_func_entry %s[] = {\n", table);
    for (int i = 0; i < a->nfuncs; i++)
        fprintf(l, "    { 0x%08xu, %s_%08x },\n", a->funcs[i].start, prefix, a->funcs[i].start);
    fprintf(l, "};\n");
    if (count) fprintf(l, "\nconst int %s = %d;\n", count, a->nfuncs);
    if (!close_out(l, dir, lookup)) return 0;

    *nfiles_out = nfiles;
    return 1;
}

/* Writes into opt->out_dir:
 *   funcs.h       prototypes for every function
 *   funcs_NNN.c   the functions, opt->funcs_per_file per file
 *   lookup.c      address -> function table (sorted) for dc_lookup
 * With opt->only set, just that one function goes to stdout instead. */
int dc_emit(const dc_analysis *a, const dc_emit_options *opt)
{
    setjmp_fn = opt->setjmp_fn;
    longjmp_fn = opt->longjmp_fn;
    prefix = "f";
    main_a = NULL;
    if (setjmp_fn && !dc_func_at(a, setjmp_fn)) fprintf(stderr, "emit: warning: no function at --setjmp %08x\n", setjmp_fn);
    if (longjmp_fn && !dc_func_at(a, longjmp_fn)) fprintf(stderr, "emit: warning: no function at --longjmp %08x\n", longjmp_fn);

    if (opt->only) {
        const dc_func *f = dc_func_at(a, opt->only);
        if (!f) {
            fprintf(stderr, "emit: no function at %08x\n", opt->only);
            return 0;
        }
        if (!emit_function(stdout, a, f)) {
            fprintf(stderr, "emit: f_%08x: collect_body() failed\n", f->start);
            return 0;
        }
        return 1;
    }

    make_dirs(opt->out_dir);
    int nfiles;
    if (!emit_set(a, opt->out_dir, "funcs.h", "funcs", "#include \"funcs.h\"\n", "lookup.c",
                  "dc_func_table", "dc_func_count", opt->funcs_per_file > 0 ? opt->funcs_per_file : 200, &nfiles))
        return 0;

    fprintf(stderr, "emit: %d functions in %d files, written to %s/\n", a->nfuncs, nfiles, opt->out_dir);
    return 1;
}

// how the runtime tells which module is in memory: FNV-1a of the start of the
// first function (code doesn't change, the header does)
static uint32_t fnv1a(const dc_analysis *a, uint32_t addr, uint32_t len)
{
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < len; i += 2) {
        uint16_t w = 0;
        dc_read16(a->img, addr + i, &w);
        h = (h ^ (w & 0xffu)) * 16777619u;
        h = (h ^ (uint32_t)(w >> 8)) * 16777619u;
    }
    return h;
}

int dc_emit_module(const dc_analysis *a, const dc_analysis *main, const dc_emit_options *opt,
                   const char *name, dc_module_info *info)
{
    char low[32], pre[40], header[48], includes[128], lookup[56], table[48];
    c_name(low, sizeof low, name);
    snprintf(pre, sizeof pre, "m_%s", low);
    snprintf(header, sizeof header, "m_%s.h", low);
    snprintf(includes, sizeof includes, "#include \"m_%s.h\"\n", low);
    snprintf(lookup, sizeof lookup, "m_%s_lookup.c", low);
    snprintf(table, sizeof table, "m_%s_table", low);

    setjmp_fn = opt->setjmp_fn;
    longjmp_fn = opt->longjmp_fn;
    prefix = pre;
    main_a = main;

    memset(info, 0, sizeof *info);
    snprintf(info->name, sizeof info->name, "%s", name);
    info->base = a->base;
    info->size = a->size;
    for (int i = 0; i < 8; i++) {
        uint16_t w = 0;
        dc_read16(a->img, a->base + (uint32_t)(i & ~1), &w);
        info->magic[i] = (uint8_t)(i & 1 ? w >> 8 : w);
    }
    info->nfuncs = a->nfuncs;
    if (a->nfuncs) {
        uint32_t s = a->funcs[0].start, len = 0;
        while (len < 0x100 && s + len - a->base < a->size && a->kind[(s + len - a->base) / 2] == BYTE_CODE)
            len += 2;
        info->sig_addr = s;
        info->sig_len = len;
        info->sig_hash = fnv1a(a, s, len);
    }

    make_dirs(opt->out_dir);
    int nfiles = 0, ok = emit_set(a, opt->out_dir, header, pre, includes, lookup, table, NULL,
                                  opt->funcs_per_file > 0 ? opt->funcs_per_file : 200, &nfiles);
    prefix = "f";
    main_a = NULL;
    if (ok) fprintf(stderr, "emit: module %s: %d functions in %d files\n", name, a->nfuncs, nfiles);
    return ok;
}

int dc_emit_registry(const dc_emit_options *opt, const dc_module_info *mods, int n)
{
    char low[32];
    make_dirs(opt->out_dir);
    FILE *f = open_out(opt->out_dir, "modules.c");
    if (!f) return 0;
    fprintf(f, "/* generated by dcrecomp, don't edit */\n#include \"sh4_rt.h\"\n\n");
    for (int i = 0; i < n; i++) {
        c_name(low, sizeof low, mods[i].name);
        fprintf(f, "extern const dc_func_entry m_%s_table[];\n", low);
    }
    fprintf(f, "\nconst dc_module dc_modules[] = {\n");
    for (int i = 0; i < n; i++) {
        const dc_module_info *m = &mods[i];
        c_name(low, sizeof low, m->name);
        fprintf(f, "    { \"%s\", 0x%08xu, 0x%08xu, {", m->name, m->base, m->size);
        for (int j = 0; j < 8; j++) fprintf(f, "%s0x%02x", j ? "," : "", m->magic[j]);
        fprintf(f, "}, 0x%08xu, 0x%xu, 0x%08xu, m_%s_table, %d },\n",
                m->sig_addr, m->sig_len, m->sig_hash, low, m->nfuncs);
    }
    if (!n) fprintf(f, "    { 0 }\n");
    fprintf(f, "};\n\nconst int dc_module_count = %d;\n", n);
    return close_out(f, opt->out_dir, "modules.c");
}
