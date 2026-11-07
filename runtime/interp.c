#include "interp.h"
#include "sh4.h"

/*
 * Runs SH-4 code straight from guest memory, for targets the analysis missed.
 *
 * All CPU state lives in sh4_ctx, so it can start anywhere, including the
 * middle of a recompiled function, and hand over to recompiled code on any
 * call. It behaves like a generated function: rts returns from dc_interp, a
 * call nests, and interrupts are checked at the same kinds of places.
 *
 * Most instructions run from dc_interp_ops, which the recompiler generates
 * (dcrecomp interp-ops). What's here is what depends on where the code is:
 * control flow and PC-relative loads. Those read the code as it is in memory
 * now, which is the point: it may not be what the recompiler saw.
 */

extern const sh4_func dc_interp_ops[65536];

static uint64_t executed;

uint64_t dc_interp_count(void) { return executed; }

static uint32_t to_p1(uint32_t addr)
{
    return (addr & 0x1fffffffu) | 0x80000000u;
}

/* One instruction that isn't control flow, on its own or in a delay slot.
 * A branch in a delay slot is illegal, so it ends up at dc_unhandled too. */
static void step(sh4_ctx *ctx, uint32_t pc)
{
    uint16_t raw = mem_r16(pc);
    sh4_func op = dc_interp_ops[raw];
    sh4_insn in;

    executed++;
    if (op) {
        op(ctx);
        return;
    }

    sh4_decode(pc, raw, &in);
    switch (in.op) {
    case SH4_OP_MOVL_PC:
        ctx->r[in.n] = mem_r32(in.target);
        break;
    case SH4_OP_MOVW_PC:
        ctx->r[in.n] = (uint32_t)(int32_t)(int16_t)mem_r16(in.target);
        break;
    case SH4_OP_MOVA:
        ctx->r[0] = in.target;
        break;
    case SH4_OP_TRAPA:
        sh4_trapa(ctx, (uint32_t)in.imm, pc);
        break;
    default:
        dc_unhandled(ctx, pc, raw);
        break;
    }
}

void dc_interp(sh4_ctx *ctx, uint32_t pc)
{
    static int ready;
    if (!ready) {
        sh4_init();
        ready = 1;
    }

    DC_CHECK_IRQ(ctx);

    for (;;) {
        uint16_t raw = mem_r16(pc);
        sh4_insn in;
        uint32_t next, target;

        sh4_decode(pc, raw, &in);
        if (!(in.flags & (F_BRANCH | F_CALL | F_RETURN))) {
            step(ctx, pc);
            pc += 2;
            continue;
        }
        executed++;

        switch (in.op) {
        case SH4_OP_BF:
            next = ctx->t ? pc + 2 : in.target;
            break;
        case SH4_OP_BT:
            next = ctx->t ? in.target : pc + 2;
            break;

        // T is read before the slot runs, the slot may change it
        case SH4_OP_BFS:
        case SH4_OP_BTS: {
            uint32_t taken = in.op == SH4_OP_BTS ? ctx->t : !ctx->t;
            step(ctx, pc + 2);
            next = taken ? in.target : pc + 4;
            break;
        }

        case SH4_OP_BRA:
            step(ctx, pc + 2);
            next = in.target;
            break;

        // where a jump or call goes is read before the slot, which may change it
        case SH4_OP_JMP:
        case SH4_OP_BRAF:
            target = ctx->r[in.n] + (in.op == SH4_OP_BRAF ? pc + 4 : 0);
            step(ctx, pc + 2);
            if (dc_try_jump(ctx, target)) return;
            next = target;
            break;

        case SH4_OP_BSR:
        case SH4_OP_BSRF:
        case SH4_OP_JSR: {
            target = in.op == SH4_OP_BSR ? in.target : ctx->r[in.n] + (in.op == SH4_OP_BSRF ? pc + 4 : 0);
            step(ctx, pc + 2);
            ctx->pr = pc + 4;
            if (dc_setjmp_addr && to_p1(target) == dc_setjmp_addr) {
                // longjmp comes back to this frame, where only a volatile keeps its value
                volatile uint32_t resume = pc + 4;
                sh4_func fn = dc_lookup(dc_setjmp_addr);
                DC_SETJMP(ctx, fn);
                pc = resume;
                continue;
            }
            dc_call(ctx, target);
            pc += 4;
            continue;
        }

        case SH4_OP_RTS:
            step(ctx, pc + 2);
            return;
        case SH4_OP_RTE:
            step(ctx, pc + 2);
            sh4_rte(ctx);
            return;

        default:
            dc_unhandled(ctx, pc, raw);
            return;
        }

        // longjmp has to go through the runtime however it's reached
        if (dc_longjmp_addr && to_p1(next) == dc_longjmp_addr) {
            dc_jump(ctx, next);
            return;
        }
        if (next <= pc) DC_CHECK_IRQ(ctx);
        pc = next;
    }
}
