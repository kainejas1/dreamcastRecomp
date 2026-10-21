#ifndef DC_SH4_CTX_H
#define DC_SH4_CTX_H

/*
 * SH-4 CPU state shared by the generated code and the runtime.
 * The generated code reads and writes these fields directly.
 */

#include <stdint.h>
#include <string.h>

/* SR bits. T, S, Q and M live in their own fields (0 or 1) because the
 * generated code touches them constantly; `sr` holds the rest. Use
 * sh4_get_sr() / sh4_set_sr() (sh4_rt.h) to read or write the whole thing. */
#define SR_T     0x00000001u
#define SR_S     0x00000002u
#define SR_IMASK 0x000000f0u
#define SR_Q     0x00000100u
#define SR_M     0x00000200u
#define SR_FD    0x00008000u
#define SR_BL    0x10000000u
#define SR_RB    0x20000000u   /* register bank: changing it swaps r0-r7 with r_bank */
#define SR_MD    0x40000000u

/* FPSCR bits. */
#define FPSCR_RM 0x00000003u   /* rounding mode */
#define FPSCR_DN 0x00040000u   /* denormals are zero */
#define FPSCR_PR 0x00080000u   /* 1: double precision arithmetic */
#define FPSCR_SZ 0x00100000u   /* 1: fmov moves 64-bit register pairs */
#define FPSCR_FR 0x00200000u   /* FP bank: changing it swaps fr with xf */

typedef union {
    float f[16];
    uint32_t u[16];
} sh4_fbank;

typedef struct sh4_ctx {
    uint32_t r[16];
    uint32_t r_bank[8];        /* the inactive bank of r0-r7 */
    uint32_t t, s, q, m;       /* SR bits, each 0 or 1 */
    uint32_t sr;               /* SR without T, S, Q, M */
    uint32_t gbr, vbr, ssr, spc, sgr, dbr;
    uint32_t mach, macl, pr;
    uint32_t fpscr, fpul;
    sh4_fbank fr;              /* current bank: FR0-FR15 */
    sh4_fbank xf;              /* other bank: XF0-XF15 (XMTRX for ftrv) */
    uint64_t cycles;           /* advanced by the generated code, drives timers */
} sh4_ctx;

/* Double registers. DRn (n even) is FRn:FRn+1 with FRn holding the upper
 * 32 bits. XDn is the same pair in the other bank. */
static inline double sh4_get_dr(const sh4_fbank *b, unsigned n)
{
    uint64_t bits = ((uint64_t)b->u[n] << 32) | b->u[n + 1];
    double d;
    memcpy(&d, &bits, sizeof d);
    return d;
}

static inline void sh4_set_dr(sh4_fbank *b, unsigned n, double d)
{
    uint64_t bits;
    memcpy(&bits, &d, sizeof bits);
    b->u[n] = (uint32_t)(bits >> 32);
    b->u[n + 1] = (uint32_t)bits;
}

#endif
