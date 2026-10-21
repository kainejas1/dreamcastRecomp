#include "analysis.h"

#include <stdlib.h>
#include <string.h>

#define MAX_BACK        8         /* resolve_reg look-back, in instructions */
#define MAX_TRACE_STEPS 1000000u  /* per function; guards against runaway traces */
#define MAX_PLAUSIBLE   1024      /* looks_like_code walk limit, in instructions */

typedef struct {
    uint32_t *v;
    size_t n, cap;
} addr_vec;

/* Working state that does not outlive dc_analyze(). */
typedef struct {
    dc_analysis *a;
    addr_vec work;    /* every function start, in discovery order */
    size_t cursor;    /* work.v[cursor..] are not traced yet */
    int *func_at;     /* per halfword: index into a->funcs + 1, or 0 */
} ctx;

/* ---- helpers ---- */

static int in_range(const dc_analysis *a, uint32_t addr, uint32_t bytes)
{
    return !(addr & 1u) && addr >= a->base && (uint64_t)addr + bytes <= (uint64_t)a->base + a->size;
}

static size_t index_of(const dc_analysis *a, uint32_t addr)
{
    return (size_t)((addr - a->base) / 2u);
}

static int read_insn(const dc_analysis *a, uint32_t pc, sh4_insn *in)
{
    uint16_t raw;
    if (!dc_read16(a->img, pc, &raw)) return 0;
    sh4_decode(pc, raw, in);
    return 1;
}

static int push(addr_vec *v, uint32_t x)
{
    if (v->n == v->cap) {
        size_t nc = v->cap ? v->cap * 2 : 32;
        uint32_t *p = realloc(v->v, nc * sizeof *p);
        if (!p) return 0;
        v->v = p;
        v->cap = nc;
    }
    v->v[v->n++] = x;
    return 1;
}

/* Record a function start and queue it for tracing. Out-of-range targets
 * are ignored (not an error). Returns 0 only on allocation failure. */
static int add_func(ctx *c, uint32_t addr, uint32_t flags)
{
    dc_analysis *a = c->a;
    if (!in_range(a, addr, 2)) return 1;
    size_t ix = index_of(a, addr);
    if (c->func_at[ix]) {
        a->funcs[c->func_at[ix] - 1].flags |= flags;
        return 1;
    }
    dc_func *p = realloc(a->funcs, (size_t)(a->nfuncs + 1) * sizeof *p);
    if (!p) return 0;
    a->funcs = p;
    a->funcs[a->nfuncs++] = (dc_func){ addr, addr, flags };
    c->func_at[ix] = a->nfuncs;
    return push(&c->work, addr);
}

static void set_func_flag(ctx *c, uint32_t start, uint32_t flags)
{
    int i = c->func_at[index_of(c->a, start)];
    if (i) c->a->funcs[i - 1].flags |= flags;
}

static void mark_data(dc_analysis *a, uint32_t addr, unsigned bytes)
{
    if (!in_range(a, addr, bytes)) return;
    for (unsigned k = 0; k < bytes; k += 2) {
        uint8_t *kind = &a->kind[index_of(a, addr + k)];
        if (*kind == BYTE_UNKNOWN) *kind = BYTE_DATA;
    }
}

/* The literal a PC-relative load reads is data. (mova only computes an
 * address, so its target is left alone.) */
static void mark_literal(dc_analysis *a, const sh4_insn *in)
{
    if (in->op == SH4_OP_MOVW_PC) mark_data(a, in->target, 2);
    else if (in->op == SH4_OP_MOVL_PC) mark_data(a, in->target, 4);
}

static int is_delay_slot(const dc_analysis *a, uint32_t pc)
{
    sh4_insn prev;
    return in_range(a, pc - 2, 2) && read_insn(a, pc - 2, &prev) && (prev.flags & F_DELAY);
}

/* ---- register constants ---- */

static int writes_reg(const sh4_insn *in, unsigned reg)
{
    return ((in->flags & F_WN) && in->n == reg) ||
           ((in->flags & F_WM) && in->m == reg) ||
           ((in->flags & F_WR0) && reg == 0);
}

/* Find the constant in `reg` just before the instruction at pc by walking
 * back through straight-line code. Follows `mov Rm,Rn` copies. Constants
 * come from mov.l/mov.w PC-relative literals, mov #imm and mova. */
static int resolve_reg(const dc_analysis *a, uint32_t pc, unsigned reg, uint32_t *value)
{
    for (unsigned k = 1; k <= MAX_BACK; k++) {
        uint32_t p = pc - 2u * k;
        sh4_insn in;
        uint16_t w;
        if (!in_range(a, p, 2) || !read_insn(a, p, &in) || in.op == SH4_OP_UNKNOWN) return 0;
        /* Stop at the previous block's end: a branch or call, or a delay slot. */
        if ((in.flags & (F_BRANCH | F_CALL | F_RETURN)) || is_delay_slot(a, p)) return 0;
        if (!writes_reg(&in, reg)) continue;
        switch (in.op) {
        case SH4_OP_MOVL_PC:
            return dc_read32(a->img, in.target, value);
        case SH4_OP_MOVW_PC:
            if (!dc_read16(a->img, in.target, &w)) return 0;
            *value = (uint32_t)(int32_t)(int16_t)w;
            return 1;
        case SH4_OP_MOV_I:
            *value = (uint32_t)in.imm;
            return 1;
        case SH4_OP_MOVA:
            *value = in.target;
            return 1;
        case SH4_OP_MOV:
            reg = in.m;   /* a copy: keep looking for Rm */
            continue;
        default:
            return 0;     /* computed value */
        }
    }
    return 0;
}

/* ---- jump tables ---- */

typedef struct {
    uint32_t addr;    /* first entry */
    unsigned size;    /* bytes per entry */
    uint32_t rel;     /* entries are offsets from this (braf), or 0 for absolute */
    unsigned max;     /* entry limit from the range check, or DC_MAX_TABLE */
} jump_table;

/* The range check in front of a switch: `mov #N,Rb ; cmp/hs Rb,Ri ; bf ok`,
 * or cmp/hi, or with the operands swapped. Returns how many indices pass,
 * or DC_MAX_TABLE if there is no recognisable check. */
static unsigned table_bound(const dc_analysis *a, uint32_t pc)
{
    for (unsigned k = 1; k <= 12; k++) {
        uint32_t p = pc - 2u * k, v;
        sh4_insn in;
        if (!in_range(a, p, 2) || !read_insn(a, p, &in) || in.op == SH4_OP_UNKNOWN) break;
        if (in.flags & (F_CALL | F_RETURN)) break;
        if (in.op != SH4_OP_CMPHS && in.op != SH4_OP_CMPHI) continue;
        int hs = in.op == SH4_OP_CMPHS;   /* cmp/hs Rm,Rn: T = Rn >= Rm; cmp/hi: Rn > Rm */
        if (resolve_reg(a, p, in.m, &v) && v < DC_MAX_TABLE) return hs ? v : v + 1;   /* T = out of range */
        if (resolve_reg(a, p, in.n, &v) && v < DC_MAX_TABLE) return hs ? v + 1 : v;   /* T = in range */
        break;
    }
    return DC_MAX_TABLE;
}

/* A jump whose register was just loaded with mov.w/mov.l @(r0,Rm), where
 * r0 or Rm holds a table address: `mova TBL,r0 ; mov.w @(r0,r1),r0 ;
 * braf r0` (offsets from braf+4) or `mov.l LIT,r3 ; mov.l @(r0,r3),r3 ;
 * jmp @r3` (absolute addresses). */
static int find_table(const dc_analysis *a, uint32_t pc, const sh4_insn *jump, jump_table *t)
{
    unsigned reg = jump->n;
    for (unsigned k = 1; k <= MAX_BACK; k++) {
        uint32_t p = pc - 2u * k, base;
        sh4_insn in;
        if (!in_range(a, p, 2) || !read_insn(a, p, &in) || in.op == SH4_OP_UNKNOWN) return 0;
        if ((in.flags & (F_BRANCH | F_CALL | F_RETURN)) || is_delay_slot(a, p)) return 0;
        if (!writes_reg(&in, reg)) continue;
        if (in.op == SH4_OP_MOV) { reg = in.m; continue; }
        if (in.op != SH4_OP_MOVW_L0 && in.op != SH4_OP_MOVL_L0) return 0;
        if (jump->op == SH4_OP_JMP && in.op != SH4_OP_MOVL_L0) return 0;
        if (!(resolve_reg(a, p, 0, &base) && in_range(a, base, 2)) &&
            !(resolve_reg(a, p, in.m, &base) && in_range(a, base, 2))) return 0;
        t->addr = base;
        t->size = in.op == SH4_OP_MOVW_L0 ? 2 : 4;
        t->rel = jump->op == SH4_OP_BRAF ? pc + 4 : 0;
        t->max = table_bound(a, p);
        return 1;
    }
    return 0;
}

/* ---- code recognition ---- */

static int is_prologue(uint16_t raw)
{
    if (raw == 0x4f22 || raw == 0x2fe6) return 1;   /* sts.l pr,@-r15 / mov.l r14,@-r15 */
    /* mov.l Rm,@-r15 (0010 1111 mmmm 0110) saving a callee-saved r8..r13 */
    if ((raw & 0xff0f) == 0x2f06) {
        unsigned m = (raw >> 4) & 15;
        return m >= 8 && m <= 13;
    }
    return 0;
}

/* Cheap check that addr starts real code, not data that happens to decode:
 * follow the fall-through path to an unconditional rts/bra/jmp and its delay
 * slot without hitting data, undecodable words, branches out of the image or
 * a branch in a delay slot. Privileged ops are allowed: Shinobi uses them. */
static int looks_like_code(const dc_analysis *a, uint32_t addr)
{
    for (unsigned i = 0; i < MAX_PLAUSIBLE; i++, addr += 2) {
        sh4_insn in, s;
        if (!in_range(a, addr, 2) || a->kind[index_of(a, addr)] == BYTE_DATA) return 0;
        if (!read_insn(a, addr, &in) || in.op == SH4_OP_UNKNOWN) return 0;
        if ((in.flags & (F_BRANCH | F_CALL)) && !(in.flags & F_INDIRECT) && !in_range(a, in.target, 2)) return 0;
        if (in.flags & F_DELAY) {
            if (!in_range(a, addr + 2, 2) || !read_insn(a, addr + 2, &s) || s.op == SH4_OP_UNKNOWN) return 0;
            if (s.flags & (F_BRANCH | F_CALL | F_RETURN | F_TRAP)) return 0;
            if (!(in.flags & (F_CALL | F_COND))) return 1;
        }
    }
    return 0;
}

/* Read a jump table's targets without changing anything. Stops at the
 * bound, at the first entry that does not point at code, or where the case
 * code begins when the cases follow the table (the usual layout). Sets
 * *dispatch for a table of absolute addresses whose entries start with a
 * prologue: those are separate functions reached by tail call (8c17edb0 in
 * SA2), not case blocks. Returns the number of entries. */
static unsigned read_table(const dc_analysis *a, uint32_t start, const jump_table *t,
                           uint32_t *targets, int *dispatch)
{
    uint32_t lowest = UINT32_MAX;
    unsigned n;

    *dispatch = 0;
    for (n = 0; n < t->max; n++) {
        uint32_t e = t->addr + n * t->size, target, v32;
        uint16_t v16, first;
        sh4_insn in;
        if (lowest > t->addr && e >= lowest) break;
        if (!in_range(a, e, t->size)) break;
        if (a->kind[index_of(a, e)] == BYTE_CODE || a->kind[index_of(a, e + t->size - 2)] == BYTE_CODE) break;
        if (t->size == 2) {
            if (!dc_read16(a->img, e, &v16)) break;
            target = t->rel + (uint32_t)(int32_t)(int16_t)v16;
        } else {
            if (!dc_read32(a->img, e, &v32)) break;
            target = t->rel + v32;
        }
        if (!in_range(a, target, 2) || target < start || a->kind[index_of(a, target)] == BYTE_DATA) break;
        if (target >= t->addr && target < e + t->size) break;   /* points into the table */
        if (!read_insn(a, target, &in) || in.op == SH4_OP_UNKNOWN) break;
        if (target < lowest) lowest = target;
        if (t->rel == 0 && dc_read16(a->img, target, &first) && is_prologue(first)) *dispatch = 1;
        targets[n] = target;
    }
    return n;
}

/* Mark a jump table as data and queue its cases as blocks of the current
 * function, or as new functions for a dispatch table. Returns entries used,
 * or -1 on error. */
static int follow_table(ctx *c, addr_vec *blocks, uint32_t start, const jump_table *t)
{
    dc_analysis *a = c->a;
    uint32_t targets[DC_MAX_TABLE];
    int dispatch;
    unsigned n = read_table(a, start, t, targets, &dispatch);

    for (unsigned i = 0; i < n; i++) {
        mark_data(a, t->addr + i * t->size, t->size);
        if (dispatch ? !add_func(c, targets[i], FUNC_FROM_TAIL) : !push(blocks, targets[i])) return -1;
    }
    return (int)n;
}

/* ---- tracing ---- */

/* Mark the delay slot at `slot` as code, including any literal it loads.
 * Returns 0 if the slot is data or undecodable. */
static int visit_slot(dc_analysis *a, uint32_t slot, uint32_t *end)
{
    sh4_insn d;
    if (!in_range(a, slot, 2)) return 0;
    size_t si = index_of(a, slot);
    if (a->kind[si] == BYTE_DATA || !read_insn(a, slot, &d) || d.op == SH4_OP_UNKNOWN) return 0;
    a->kind[si] = BYTE_CODE;
    if (slot + 2 > *end) *end = slot + 2;
    mark_literal(a, &d);
    return 1;
}

/* Follow control flow from `start`, marking code and literals. Calls and
 * tail calls met on the way are queued as new functions. */
static int trace_func(ctx *c, uint32_t start, uint32_t *end)
{
    dc_analysis *a = c->a;
    addr_vec blocks = {0};
    int ok = push(&blocks, start);
    size_t steps = 0;

    while (ok && blocks.n) {
        uint32_t pc = blocks.v[--blocks.n];
        while (ok && in_range(a, pc, 2) && steps++ < MAX_TRACE_STEPS) {
            size_t ix = index_of(a, pc);
            sh4_insn in;
            uint32_t slot = pc + 2, target;
            if (a->kind[ix] != BYTE_UNKNOWN) break;
            if (!read_insn(a, pc, &in) || in.op == SH4_OP_UNKNOWN) break;
            a->kind[ix] = BYTE_CODE;
            if (pc + 2 > *end) *end = pc + 2;
            mark_literal(a, &in);

            switch (in.op) {
            case SH4_OP_BF:
            case SH4_OP_BT:
                ok = push(&blocks, in.target);
                pc += 2;
                continue;
            case SH4_OP_BFS:
            case SH4_OP_BTS:
                if (visit_slot(a, slot, end))
                    ok = push(&blocks, in.target) && push(&blocks, slot + 2);
                break;
            case SH4_OP_BRA:
                visit_slot(a, slot, end);
                ok = push(&blocks, in.target);
                break;
            case SH4_OP_BSR:
                ok = add_func(c, in.target, FUNC_FROM_BSR);
                visit_slot(a, slot, end);
                pc += 4;
                continue;
            case SH4_OP_JSR:
                if (resolve_reg(a, pc, in.n, &target)) ok = add_func(c, target, FUNC_FROM_JSR);
                visit_slot(a, slot, end);
                pc += 4;
                continue;
            case SH4_OP_BSRF:
                if (resolve_reg(a, pc, in.n, &target)) ok = add_func(c, pc + 4 + target, FUNC_FROM_BSR);
                else set_func_flag(c, start, FUNC_HAS_UNRESOLVED_JUMP);
                visit_slot(a, slot, end);
                pc += 4;
                continue;
            case SH4_OP_JMP:
            case SH4_OP_BRAF: {
                jump_table t;
                int cases = 0;
                visit_slot(a, slot, end);
                if (resolve_reg(a, pc, in.n, &target)) {
                    if (in.op == SH4_OP_BRAF) target += pc + 4;
                    if (in_range(a, target, 2) && target != start) ok = add_func(c, target, FUNC_FROM_TAIL);
                    else if (target != start) set_func_flag(c, start, FUNC_HAS_UNRESOLVED_JUMP);
                } else if (find_table(a, pc, &in, &t) && (cases = follow_table(c, &blocks, start, &t)) != 0) {
                    if (cases < 0) ok = 0;
                    else set_func_flag(c, start, FUNC_HAS_JUMP_TABLE);
                } else {
                    set_func_flag(c, start, FUNC_HAS_UNRESOLVED_JUMP);
                }
                break;
            }
            default:
                if (in.flags & F_RETURN) {
                    if (in.flags & F_DELAY) visit_slot(a, slot, end);
                    break;
                }
                pc += 2;
                continue;
            }
            break;   /* this block ended */
        }
    }
    free(blocks.v);
    return ok;
}

/* Trace every queued function that has not been traced yet. */
static int drain(ctx *c)
{
    while (c->cursor < c->work.n) {
        uint32_t start = c->work.v[c->cursor++], end = start;
        if (!trace_func(c, start, &end)) return 0;
        dc_func *f = &c->a->funcs[c->func_at[index_of(c->a, start)] - 1];
        if (end > f->end) f->end = end;
    }
    return 1;
}

/* ---- discovery passes; each returns functions added, or -1 on error ---- */

/* Aligned words that point at untraced code: literal-pool words (usually
 * callbacks) or any word whose target starts with a prologue. */
static int scan_pointers(ctx *c)
{
    dc_analysis *a = c->a;
    int added = 0;
    for (uint32_t p = (a->base + 3u) & ~3u; in_range(a, p, 4); p += 4) {
        size_t ix = index_of(a, p);
        uint32_t value;
        uint16_t raw;
        if (a->kind[ix] == BYTE_CODE) continue;
        if (!dc_read32(a->img, p, &value) || !in_range(a, value, 2)) continue;
        if (a->kind[index_of(a, value)] != BYTE_UNKNOWN || !dc_read16(a->img, value, &raw)) continue;
        int is_literal = a->kind[ix] == BYTE_DATA && a->kind[ix + 1] == BYTE_DATA;
        if (!is_literal && !is_prologue(raw)) continue;
        if (!looks_like_code(a, value)) continue;
        int before = a->nfuncs;
        if (!add_func(c, value, FUNC_FROM_PTR)) return -1;
        added += a->nfuncs != before;
    }
    return drain(c) ? added : -1;
}

/* Compilers sometimes schedule an instruction or two ahead of the prologue
 * (8c011896 starts with `mov.l LIT,r3`). Step back over up to three, but
 * never into the previous function's rts/bra/jmp or its delay slot. */
static uint32_t step_back(const dc_analysis *a, uint32_t pc)
{
    for (unsigned k = 0; k < 3; k++) {
        uint32_t cand = pc - 2;
        sh4_insn in;
        if (!in_range(a, cand, 2) || a->kind[index_of(a, cand)] != BYTE_UNKNOWN) break;
        if (!read_insn(a, cand, &in) || in.op == SH4_OP_UNKNOWN) break;
        if ((in.flags & (F_DELAY | F_BRANCH | F_CALL | F_RETURN)) || is_delay_slot(a, cand)) break;
        pc = cand;
    }
    return pc;
}

/* Prologues in untraced space: functions nothing references directly
 * (dead code, or reached through computed addresses). */
static int scan_gaps(ctx *c)
{
    dc_analysis *a = c->a;
    int added = 0;
    for (uint32_t pc = a->base; in_range(a, pc, 2); pc += 2) {
        uint16_t raw;
        if (a->kind[index_of(a, pc)] != BYTE_UNKNOWN) continue;
        if (!dc_read16(a->img, pc, &raw) || !is_prologue(raw)) continue;
        uint32_t entry = step_back(a, pc);
        if (!looks_like_code(a, entry)) continue;
        int before = a->nfuncs;
        /* Trace right away so the rest of this prologue is already code. */
        if (!add_func(c, entry, FUNC_FROM_GAP) || !drain(c)) return -1;
        added += a->nfuncs != before;
    }
    return added;
}

/* pc directly follows an rts/rte and its delay slot. */
static int follows_return(const dc_analysis *a, uint32_t pc)
{
    sh4_insn in;
    return in_range(a, pc - 4, 2) && read_insn(a, pc - 4, &in) &&
           (in.flags & F_RETURN) && (in.flags & F_DELAY);
}

/* Untraced space inside a function with an unresolved jump is most likely
 * its jump table or unreached case blocks, not a new function. */
static int in_unresolved_func(const dc_analysis *a, uint32_t pc)
{
    for (int i = 0; i < a->nfuncs; i++) {
        const dc_func *f = &a->funcs[i];
        if ((f->flags & FUNC_HAS_UNRESOLVED_JUMP) && f->start < pc && pc < f->end) return 1;
    }
    return 0;
}

/* Leaf functions have no prologue, so look for untraced code where known
 * code ends: right after an rts and its delay slot, or after a literal pool,
 * skipping nop padding. Not after bra/jmp, and not inside a function with an
 * unresolved jump (see in_unresolved_func). */
static int scan_leaves(ctx *c)
{
    dc_analysis *a = c->a;
    int added = 0;
    for (uint32_t pc = a->base + 4; in_range(a, pc, 2); pc += 2) {
        size_t ix = index_of(a, pc);
        uint16_t raw;
        if (a->kind[ix] != BYTE_UNKNOWN || a->kind[ix - 1] == BYTE_UNKNOWN) continue;
        if (a->kind[ix - 1] == BYTE_CODE && !follows_return(a, pc)) continue;

        uint32_t entry = pc;
        while (in_range(a, entry, 2) && a->kind[index_of(a, entry)] == BYTE_UNKNOWN &&
               dc_read16(a->img, entry, &raw) && raw == 0x0009)   /* nop padding */
            entry += 2;
        if (!in_range(a, entry, 2) || a->kind[index_of(a, entry)] != BYTE_UNKNOWN) continue;
        if (in_unresolved_func(a, entry) || !looks_like_code(a, entry)) continue;

        int before = a->nfuncs;
        if (!add_func(c, entry, FUNC_FROM_GAP) || !drain(c)) return -1;
        added += a->nfuncs != before;
    }
    return added;
}

/* ---- driver ---- */

static int func_cmp(const void *x, const void *y)
{
    const dc_func *f = x, *g = y;
    return (f->start > g->start) - (f->start < g->start);
}

int dc_analyze(dc_analysis *a, const dc_image *img, uint32_t base, uint32_t size)
{
    if (!a || !img || !img->ram || (base & 1u) || (size & 1u) || size < 2 ||
        (uint64_t)base + size > UINT32_MAX) return 0;
    memset(a, 0, sizeof *a);
    a->img = img;
    a->base = base;
    a->size = size;
    a->kind = calloc((size_t)size / 2 + 1, 1);

    ctx c = { .a = a, .func_at = calloc((size_t)size / 2 + 1, sizeof(int)) };
    int ok = a->kind && c.func_at;
    if (ok) {
        sh4_init();
        ok = add_func(&c, img->entry, FUNC_FROM_ENTRY) && drain(&c);
    }
    /* Each pass can uncover code the others feed on (a gap function loads a
     * new callback pointer, a leaf follows a newly traced rts), so repeat
     * until none of them finds anything. */
    while (ok) {
        int p = scan_pointers(&c);
        int g = p < 0 ? -1 : scan_gaps(&c);
        int l = g < 0 ? -1 : scan_leaves(&c);
        if (l < 0) ok = 0;
        else if (p + g + l == 0) break;
    }

    free(c.work.v);
    free(c.func_at);
    if (!ok) {
        dc_analysis_free(a);
        return 0;
    }
    qsort(a->funcs, (size_t)a->nfuncs, sizeof *a->funcs, func_cmp);
    return 1;
}

void dc_analysis_free(dc_analysis *a)
{
    if (!a) return;
    free(a->kind);
    free(a->funcs);
    memset(a, 0, sizeof *a);
}

/* ---- queries ---- */

const dc_func *dc_func_at(const dc_analysis *a, uint32_t addr)
{
    int lo = 0, hi = a->nfuncs - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (a->funcs[mid].start < addr) lo = mid + 1;
        else if (a->funcs[mid].start > addr) hi = mid - 1;
        else return &a->funcs[mid];
    }
    return NULL;
}

int dc_resolve_reg(const dc_analysis *a, uint32_t pc, unsigned reg, uint32_t *value)
{
    return resolve_reg(a, pc, reg, value);
}

int dc_jump_table(const dc_analysis *a, uint32_t func_start, uint32_t pc,
                  uint32_t *targets, int max, int *dispatch)
{
    sh4_insn in;
    jump_table t;
    *dispatch = 0;
    if (max <= 0 || !in_range(a, pc, 2) || !read_insn(a, pc, &in)) return 0;
    if (in.op != SH4_OP_JMP && in.op != SH4_OP_BRAF) return 0;
    if (!find_table(a, pc, &in, &t)) return 0;
    if (t.max > (unsigned)max) t.max = (unsigned)max;
    return (int)read_table(a, func_start, &t, targets, dispatch);
}
