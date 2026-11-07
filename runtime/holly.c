#include "holly.h"
#include "clock.h"
#include "maple.h"
#include "memory.h"
#include "pvr.h"
#include "tmu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HOLLY_WORDS ((HOLLY_REG_END - HOLLY_REG_BASE) / 4u)
#define SH4REG_BASE 0xff000000u
#define SH4REG_END  0x100000000ull
#define SH4REG_WORDS (0x01000000u / 4u)
#define STATUS_NORMAL_VBLANK_IN  (1u << 3)
#define STATUS_NORMAL_VBLANK_OUT (1u << 4)

static uint32_t holly_regs[HOLLY_WORDS];
static uint32_t sh4_regs[SH4REG_WORDS];
static uint32_t configured_vcount = 524u;
static uint32_t last_scanline;
static uint32_t pending_status;
static uint32_t frame_count;   // vblank-ins seen

uint32_t holly_frame_count(void) { return frame_count; }

uint32_t holly_reg(uint32_t address) { return holly_regs[(address - HOLLY_REG_BASE) / 4u]; }
uint32_t sh4_reg(uint32_t address) { return sh4_regs[(address - SH4REG_BASE) / 4u]; }
void holly_set_reg(uint32_t address, uint32_t value) { holly_regs[(address - HOLLY_REG_BASE) / 4u] = value; }
void holly_raise(uint32_t bits) { holly_regs[(HOLLY_SB_ISTNRM - HOLLY_REG_BASE) / 4u] |= bits; }

// DC_HOLLYLOG=1: log register accesses (not SPG_STATUS polling), first 3000
static int reglog;
#define REGLOG(...) do { if (reglog > 0) { reglog--; fprintf(stderr, __VA_ARGS__); } } while (0)

void holly_init(void)
{
    memset(holly_regs, 0, sizeof holly_regs);
    memset(sh4_regs, 0, sizeof sh4_regs);

    configured_vcount = 524u;
    pending_status = 0;
    last_scanline = 0;

    reglog = getenv("DC_HOLLYLOG") ? 3000 : 0;
    clock_init();
    tmu_init();

    holly_regs[(HOLLY_SPG_LOAD - HOLLY_REG_BASE) / 4u]
        = (524u << 16) | 857u;

    holly_regs[(HOLLY_SPG_VBLANK_INT - HOLLY_REG_BASE) / 4u]
        = (480u << 16) | 240u;
}

// microseconds of the video clock (the AICA runs its samples off it)
uint64_t holly_clock_us(void)
{
    return clock_ns() / 1000u;
}

int holly_handles(uint32_t address)
{
    return address >= HOLLY_REG_BASE
        && address < HOLLY_REG_END;
}

int sh4reg_handles(uint32_t address)
{
    return address >= SH4REG_BASE;
}

uint32_t holly_vcount(void)
{
    return configured_vcount;
}

uint32_t holly_scanline(void)
{
    uint64_t in_frame = clock_ns() % DC_FRAME_NS;

    return (uint32_t)(
        (in_frame * ((uint64_t)configured_vcount + 1u))
        / DC_FRAME_NS
    );
}

static void update_video_events(void)
{
    uint32_t line = holly_scanline();

    uint32_t reg =
        holly_regs[(HOLLY_SPG_VBLANK_INT - HOLLY_REG_BASE) / 4u];

    uint32_t vblank_in = reg & 0x3ffu;
    uint32_t vblank_out = (reg >> 16) & 0x3ffu;

    if (line != last_scanline) {
        /* Handle skipped scanlines and frame wrap without requiring
           millisecond timing. */
        uint32_t step =
            (line + configured_vcount + 1u - last_scanline)
            % (configured_vcount + 1u);

        for (uint32_t i = 1; i <= step; ++i) {
            uint32_t crossed =
                (last_scanline + i) % (configured_vcount + 1u);

            if (crossed == vblank_in) {
                frame_count++;
                // Maple DMA started by vblank (SB_MDTSEL bit 0)
                if ((holly_regs[(MAPLE_SB_MDTSEL - HOLLY_REG_BASE) / 4u] & 1) &&
                    (holly_regs[(MAPLE_SB_MDEN - HOLLY_REG_BASE) / 4u] & 1))
                    maple_dma();
                pending_status |= STATUS_NORMAL_VBLANK_IN;

                holly_regs[
                    (HOLLY_SB_ISTNRM - HOLLY_REG_BASE) / 4u
                ] |= STATUS_NORMAL_VBLANK_IN;
            }

            if (crossed == vblank_out) {
                pending_status |= STATUS_NORMAL_VBLANK_OUT;

                holly_regs[
                    (HOLLY_SB_ISTNRM - HOLLY_REG_BASE) / 4u
                ] |= STATUS_NORMAL_VBLANK_OUT;
            }
        }

        last_scanline = line;
    }
}

uint32_t holly_status_pending(void)
{
    update_video_events(); return pending_status;
}

#define REG(a) holly_regs[((a) - HOLLY_REG_BASE) / 4u]

int holly_pending_level(void)
{
    update_video_events();

    uint32_t nrm = REG(HOLLY_SB_ISTNRM), ext = REG(HOLLY_SB_ISTEXT), err = REG(HOLLY_SB_ISTERR);

    if ((nrm & REG(HOLLY_SB_IML6NRM)) | (ext & REG(HOLLY_SB_IML6EXT)) | (err & REG(HOLLY_SB_IML6ERR))) return 6;
    if ((nrm & REG(HOLLY_SB_IML4NRM)) | (ext & REG(HOLLY_SB_IML4EXT)) | (err & REG(HOLLY_SB_IML4ERR))) return 4;
    if ((nrm & REG(HOLLY_SB_IML2NRM)) | (ext & REG(HOLLY_SB_IML2EXT)) | (err & REG(HOLLY_SB_IML2ERR))) return 2;
    return 0;
}

uint32_t holly_read32(uint32_t address)
{
    if (!holly_handles(address) || (address & 3u)) return 0xffffffffu;

    update_video_events();

    // read-only chip ids. SA2 halts if the PowerVR doesn't say CLX2 rev 0x11+
    if (address == 0x005f8000u) return 0x17fd11dbu;   // PVR ID
    if (address == 0x005f8004u) return 0x00000011u;   // PVR REVISION
    if (address == 0x005f689cu) return 0x0000000bu;   // SB_SBREV (Holly)

    if (address == HOLLY_SPG_STATUS) {
        uint32_t line = holly_scanline() & 0x3ffu;
        uint32_t vb = holly_regs[(HOLLY_SPG_VBLANK - HOLLY_REG_BASE) / 4u];
        uint32_t vstart = vb & 0x3ffu, vend = (vb >> 16) & 0x3ffu;
        // bit 13: in vertical blank (from SPG_VBLANK start .. end, which wraps)
        int in_vblank = vstart > vend ? (line >= vstart || line < vend)
                                      : (line >= vstart && line < vend);
        return line | (in_vblank ? 1u << 13 : 0);
    }

    uint32_t v = holly_regs[(address - HOLLY_REG_BASE) / 4u];

    // SB_ISTNRM bit 30/31 = something pending in SB_ISTEXT / SB_ISTERR
    if (address == HOLLY_SB_ISTNRM) {
        v &= 0x3fffffffu;
        if (holly_regs[(HOLLY_SB_ISTEXT - HOLLY_REG_BASE) / 4u]) v |= 1u << 30;
        if (holly_regs[(HOLLY_SB_ISTERR - HOLLY_REG_BASE) / 4u]) v |= 1u << 31;
    }

    REGLOG("[reg] r %08x -> %08x\n", address, v);
    return v;
}

// G2 DMA: main RAM <-> G2 bus (sound RAM, expansion). 4 channels 0x20 apart:
// +0 STAG (G2 address), +4 STAR (system RAM), +8 LEN, +c DIR, +10 TSEL,
// +14 EN, +18 ST (start / busy), +1c SUSP. Done at once when started.
#define G2_DMA_BASE 0x005f7800u

static void g2_dma(unsigned ch)
{
    uint32_t *r = &holly_regs[(G2_DMA_BASE + ch * 0x20u - HOLLY_REG_BASE) / 4u];
    uint32_t g2 = r[0] & 0x1fffffe0u, sys = r[1] & 0x1fffffe0u, len = r[2] & 0x7fffffe0u;
    int to_ram = r[3] & 1;

    for (uint32_t i = 0; i < len; i += 4) {
        if (to_ram) mem_w32(sys + i, mem_r32(g2 + i));
        else        mem_w32(g2 + i, mem_r32(sys + i));
    }
    REGLOG("[reg] G2 DMA ch%u: %u bytes %08x %s %08x\n", ch, len, sys, to_ram ? "<-" : "->", g2);

    r[0] = g2 + len;
    r[1] = sys + len;
    if (r[2] & 0x80000000u) r[5] = 0;   // LEN bit 31: clear EN when done
    r[6] = 0;                            // not busy any more
    holly_regs[(HOLLY_SB_ISTNRM - HOLLY_REG_BASE) / 4u] |= 1u << (15 + ch);   // DMA end interrupt
}

void holly_write32(uint32_t address, uint32_t value)
{
    if (!holly_handles(address) || (address & 3u))
        return;

    if (address == HOLLY_SB_ISTNRM || address == HOLLY_SB_ISTERR) {
        /* Interrupt status is write-one-to-clear. */
        size_t i = (address - HOLLY_REG_BASE) / 4u;

        holly_regs[i] &= ~value;
        if (address == HOLLY_SB_ISTNRM) pending_status &= ~value;
        REGLOG("[reg] w %08x <- %08x (ack)\n", address, value);
        return;
    }
    if (address == HOLLY_SB_ISTEXT)   // read only: follows the external lines (GD-ROM, AICA, ...)
        return;

    holly_regs[(address - HOLLY_REG_BASE) / 4u] = value;
    REGLOG("[reg] w %08x <- %08x\n", address, value);

    // G2 DMA start: channel enabled and ST written with 1
    if (address >= G2_DMA_BASE && address < G2_DMA_BASE + 0x80u && (address & 0x1fu) == 0x18u && (value & 1)) {
        unsigned ch = (address - G2_DMA_BASE) / 0x20u;
        if (holly_regs[(G2_DMA_BASE + ch * 0x20u + 0x14u - HOLLY_REG_BASE) / 4u] & 1)
            g2_dma(ch);
    }

    // Maple DMA started by software (SA2 does it this way)
    if (address == MAPLE_SB_MDST && (value & 1) && (holly_regs[(MAPLE_SB_MDEN - HOLLY_REG_BASE) / 4u] & 1))
        maple_dma();

    // PowerVR registers (TA list init, start render, ...) and the ch2 DMA
    // that feeds the TA
    if (address >= 0x005f8000u || address == PVR_SB_C2DST)
        pvr_reg_written(address, value);

    if (address == HOLLY_SPG_LOAD) {
        uint32_t lines = (value >> 16) & 0x3ffu;

        configured_vcount = lines ? lines : 524u;

        if (configured_vcount > 1023u)
            configured_vcount = 1023u;

        last_scanline = holly_scanline();
    }
}

uint32_t sh4reg_read32(uint32_t address)
{
    if (!sh4reg_handles(address) || (address & 3u))
        return 0xffffffffu;

    if (tmu_handles(address)) return tmu_read32(address);

    REGLOG("[reg] r %08x -> %08x (sh4)\n", address, sh4_regs[(address - SH4REG_BASE) / 4u]);
    return sh4_regs[(address - SH4REG_BASE) / 4u];
}

void sh4reg_write32(uint32_t address, uint32_t value)
{
    if (!sh4reg_handles(address) || (address & 3u))
        return;

    if (tmu_handles(address)) { tmu_write32(address, value); return; }

    sh4_regs[(address - SH4REG_BASE) / 4u] = value;
    REGLOG("[reg] w %08x <- %08x (sh4)\n", address, value);
}