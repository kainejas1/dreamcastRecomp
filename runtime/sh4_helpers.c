/*
 * SH-4 instructions the generated code calls as functions because they are
 * awkward to inline. Pure CPU semantics, no hardware. The rest of
 * sh4_rt.h (memory, dc_call, pref, trapa, ...) is the runtime's job.
 */

#include "sh4_rt.h"

uint32_t sh4_get_sr(const sh4_ctx *ctx)
{
    return ctx->sr | ctx->t | (ctx->s << 1) | (ctx->q << 8) | (ctx->m << 9);
}

void sh4_set_sr(sh4_ctx *ctx, uint32_t sr)
{
    // switching register banks: r0-r7 trade places with r_bank
    if ((sr ^ ctx->sr) & SR_RB) {
        for (int i = 0; i < 8; i++) {
            uint32_t tmp = ctx->r[i];
            ctx->r[i] = ctx->r_bank[i];
            ctx->r_bank[i] = tmp;
        }
    }
    ctx->t = sr & 1;
    ctx->s = (sr >> 1) & 1;
    ctx->q = (sr >> 8) & 1;
    ctx->m = (sr >> 9) & 1;
    ctx->sr = sr & ~(SR_T | SR_S | SR_Q | SR_M);
}

void sh4_set_fpscr(sh4_ctx *ctx, uint32_t fpscr)
{
    if ((fpscr ^ ctx->fpscr) & FPSCR_FR) {
        sh4_fbank tmp = ctx->fr;
        ctx->fr = ctx->xf;
        ctx->xf = tmp;
    }
    ctx->fpscr = fpscr;
}

/* One step of non-restoring division, from the DIV1 pseudocode in the SH-4
 * software manual. Run 32 times (with rotcl) by every software divide. */
void sh4_div1(sh4_ctx *ctx, unsigned n, unsigned m)
{
    uint32_t old_q = ctx->q, rm = ctx->r[m], tmp0, tmp1;
    uint32_t *rn = &ctx->r[n];

    ctx->q = *rn >> 31;
    *rn = (*rn << 1) | ctx->t;
    tmp0 = *rn;
    if (old_q == ctx->m) {      // old_q 0 / M 0, or old_q 1 / M 1: subtract
        *rn -= rm;
        tmp1 = *rn > tmp0;
    } else {                    // add
        *rn += rm;
        tmp1 = *rn < tmp0;
    }
    // new Q, the manual's four (old_q, M) cases written out
    if (old_q == 0 && ctx->m == 0)      ctx->q = ctx->q ? !tmp1 : tmp1;
    else if (old_q == 0)                ctx->q = ctx->q ? tmp1 : !tmp1;
    else if (ctx->m == 0)               ctx->q = ctx->q ? !tmp1 : tmp1;
    else                                ctx->q = ctx->q ? tmp1 : !tmp1;
    ctx->t = ctx->q == ctx->m;
}

/* mac.l @Rm+,@Rn+: MAC += (signed) @Rn * @Rm. With S set the result
 * saturates to 48 bits. Rn is read first, matching the manual. */
void sh4_mac_l(sh4_ctx *ctx, unsigned n, unsigned m)
{
    int64_t a = (int32_t)mem_r32(ctx->r[n]);
    ctx->r[n] += 4;
    int64_t b = (int32_t)mem_r32(ctx->r[m]);
    ctx->r[m] += 4;

    int64_t mac = (int64_t)(((uint64_t)ctx->mach << 32) | ctx->macl);
    mac += a * b;
    if (ctx->s) {
        if (mac > 0x00007fffffffffffLL) mac = 0x00007fffffffffffLL;
        if (mac < -0x0000800000000000LL) mac = -0x0000800000000000LL;
    }
    ctx->mach = (uint32_t)((uint64_t)mac >> 32);
    ctx->macl = (uint32_t)mac;
}

/* mac.w @Rm+,@Rn+: 16x16. With S set only MACL accumulates, saturating at
 * 32 bits, and MACH bit 0 flags the overflow. */
void sh4_mac_w(sh4_ctx *ctx, unsigned n, unsigned m)
{
    int32_t a = (int16_t)mem_r16(ctx->r[n]);
    ctx->r[n] += 2;
    int32_t b = (int16_t)mem_r16(ctx->r[m]);
    ctx->r[m] += 2;

    if (ctx->s) {
        int64_t sum = (int64_t)(int32_t)ctx->macl + (int64_t)a * b;
        if (sum > 0x7fffffffLL)       { sum = 0x7fffffffLL; ctx->mach |= 1; }
        else if (sum < -0x80000000LL) { sum = -0x80000000LL; ctx->mach |= 1; }
        ctx->macl = (uint32_t)sum;
    } else {
        int64_t mac = (int64_t)(((uint64_t)ctx->mach << 32) | ctx->macl);
        mac += (int64_t)a * b;
        ctx->mach = (uint32_t)((uint64_t)mac >> 32);
        ctx->macl = (uint32_t)mac;
    }
}

/* fsca fpul,DRn: sine and cosine of a 16-bit angle (0x10000 = 2 pi).
 * The real chip uses a table, so the last bits can differ from sinf/cosf. */
void sh4_fsca(sh4_ctx *ctx, unsigned n)
{
    float rad = (float)(ctx->fpul & 0xffff) * (6.28318530717958647692f / 65536.0f);
    ctx->fr.f[n] = sinf(rad);
    ctx->fr.f[n + 1] = cosf(rad);
}

/* ftrv xmtrx,FVn: FVn = XMTRX * FVn, XMTRX being the other bank, column major. */
void sh4_ftrv(sh4_ctx *ctx, unsigned n)
{
    float v[4];
    for (int i = 0; i < 4; i++)
        v[i] = ctx->xf.f[i] * ctx->fr.f[n] + ctx->xf.f[i + 4] * ctx->fr.f[n + 1] +
               ctx->xf.f[i + 8] * ctx->fr.f[n + 2] + ctx->xf.f[i + 12] * ctx->fr.f[n + 3];
    for (int i = 0; i < 4; i++)
        ctx->fr.f[n + i] = v[i];
}
