#ifndef DC_SH4_H
#define DC_SH4_H

#include <stddef.h>
#include <stdint.h>

/* How the low bits of an instruction are interpreted. */
typedef enum {
    E_NONE,
    E_IMMS,   /* imm8, sign-extended */
    E_IMMU,   /* imm8, zero-extended */
    E_D4B,    /* disp4 * 1 */
    E_D4W,    /* disp4 * 2 */
    E_D4L,    /* disp4 * 4 */
    E_D8B,    /* disp8 * 1 (GBR) */
    E_D8W,    /* disp8 * 2 (GBR) */
    E_D8L,    /* disp8 * 4 (GBR) */
    E_PCW,    /* PC + 4 + disp8 * 2 */
    E_PCL,    /* (PC & ~3) + 4 + disp8 * 4 */
    E_BR8,    /* PC + 4 + sext(disp8) * 2 */
    E_BR12,   /* PC + 4 + sext(disp12) * 2 */
} sh4_enc;

enum {
    F_LOAD     = 1u << 0,
    F_STORE    = 1u << 1,
    F_BRANCH   = 1u << 2,   /* transfers control (jump, not call) */
    F_COND     = 1u << 3,   /* conditional on T */
    F_CALL     = 1u << 4,   /* sets PR */
    F_RETURN   = 1u << 5,
    F_DELAY    = 1u << 6,   /* has a delay slot */
    F_INDIRECT = 1u << 7,   /* target comes from a register */
    F_PCREL    = 1u << 8,   /* reads a PC-relative literal / address */
    F_FPU      = 1u << 9,
    F_FPMODE   = 1u << 10,  /* meaning depends on FPSCR.SZ / FPSCR.PR */
    F_PRIV     = 1u << 11,
    F_TRAP     = 1u << 12,
    /* General registers written (FPU and control registers are not tracked). */
    F_WN       = 1u << 13,  /* writes r{n} (bits 11-8), incl. @-Rn / @Rn+ updates */
    F_WM       = 1u << 14,  /* writes r{m} (bits 7-4) via @Rm+ */
    F_WR0      = 1u << 15,  /* writes r0 implicitly */
};

typedef enum {
    SH4_OP_UNKNOWN = 0,
#define X(id, pat, mn, ops, enc, fl) SH4_OP_##id,
#include "sh4_ops.def"
#undef X
    SH4_OP_COUNT
} sh4_op;

typedef struct {
    const char *name;       /* enum name, e.g. "MOVL_PC" */
    const char *pattern;
    const char *mnemonic;
    const char *operands;
    sh4_enc enc;
    uint32_t flags;
    uint16_t mask, match;
} sh4_opinfo;

typedef struct {
    uint32_t pc;
    uint16_t raw;
    sh4_op op;
    uint8_t n, m;       /* bits 11-8, bits 7-4 */
    int32_t imm;        /* E_IMMS / E_IMMU */
    int32_t disp;       /* scaled displacement in bytes */
    uint32_t target;    /* absolute address for PC-relative forms */
    uint32_t flags;
} sh4_insn;

void sh4_init(void);
const sh4_opinfo *sh4_info(sh4_op op);
void sh4_decode(uint32_t pc, uint16_t raw, sh4_insn *out);

/* Format as "mnemonic\toperands" in GNU objdump syntax. */
void sh4_format(const sh4_insn *in, char *buf, size_t len);

#endif
