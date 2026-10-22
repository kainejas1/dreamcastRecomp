#include "holly.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <time.h>
#endif

#define HOLLY_WORDS ((HOLLY_REG_END - HOLLY_REG_BASE) / 4u)
#define SH4REG_BASE 0xff000000u
#define SH4REG_END  0x100000000ull
#define SH4REG_WORDS (0x01000000u / 4u)
#define STATUS_NORMAL_VBLANK_IN  (1u << 3)
#define STATUS_NORMAL_VBLANK_OUT (1u << 4)

static uint32_t holly_regs[HOLLY_WORDS];
static uint32_t sh4_regs[SH4REG_WORDS];
static uint64_t host_frequency;
static uint64_t start_ticks;
static uint32_t configured_vcount = 524u;
static uint32_t last_scanline;
static uint32_t pending_status;
static uint32_t frame_count;   // vblank-ins seen
static int clock_ready;

uint32_t holly_frame_count(void) { return frame_count; }

// DC_HOLLYLOG=1: log register accesses (not SPG_STATUS polling), first 3000
static int reglog;
#define REGLOG(...) do { if (reglog > 0) { reglog--; fprintf(stderr, __VA_ARGS__); } } while (0)

static uint64_t host_ticks(void)
{
#ifdef _WIN32
    LARGE_INTEGER now;
    if (!QueryPerformanceCounter(&now)) return 0;
    return (uint64_t)now.QuadPart;
#else
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) !=0 ) return 0;
    return (uint64_t)ts.tv_sec * 1000000000ull
        +  (uint64_t)ts.tv_nsec;
#endif
}

static uint64_t ticks_per_second(void)
{
#ifdef _WIN32
    LARGE_INTEGER f;
    if (!QueryPerformanceFrequency(&f) || f.QuadPart <= 0)
        return 0;
    return (uint64_t)f.QuadPart;
#else
    return 1000000000ull;
#endif
}

void holly_init(void)
{
    memset(holly_regs, 0, sizeof holly_regs);
    memset(sh4_regs, 0, sizeof sh4_regs);

    configured_vcount = 524u;
    pending_status = 0;
    last_scanline = 0;

    reglog = getenv("DC_HOLLYLOG") ? 3000 : 0;
    host_frequency = ticks_per_second();
    start_ticks = host_ticks();
    clock_ready = host_frequency != 0;

    holly_regs[(HOLLY_SPG_LOAD - HOLLY_REG_BASE) / 4u]
        = (524u << 16) | 857u;

    holly_regs[(HOLLY_SPG_VBLANK_INT - HOLLY_REG_BASE) / 4u]
        = (480u << 16) | 240u;
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
    if (!clock_ready || !host_frequency) return 0;

    uint64_t now = host_ticks();
    if (now < start_ticks) return 0;

    uint64_t elapsed = now - start_ticks;

    uint64_t frame_ticks = host_frequency / 60u;

    if (!frame_ticks) return 0;

    uint64_t in_frame = elapsed % frame_ticks;

    return (uint32_t)(
        (in_frame * ((uint64_t)configured_vcount + 1u))
        / frame_ticks
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

void holly_write32(uint32_t address, uint32_t value)
{
    if (!holly_handles(address) || (address & 3u))
        return;

    if (address == HOLLY_SB_ISTNRM) {
        /* Interrupt status is write-one-to-clear. */
        size_t i = (address - HOLLY_REG_BASE) / 4u;

        holly_regs[i] &= ~value;
        pending_status &= ~value;
        REGLOG("[reg] w %08x <- %08x (ack)\n", address, value);
        return;
    }

    holly_regs[(address - HOLLY_REG_BASE) / 4u] = value;
    REGLOG("[reg] w %08x <- %08x\n", address, value);

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

    REGLOG("[reg] r %08x -> %08x (sh4)\n", address, sh4_regs[(address - SH4REG_BASE) / 4u]);
    return sh4_regs[(address - SH4REG_BASE) / 4u];
}

void sh4reg_write32(uint32_t address, uint32_t value)
{
    if (!sh4reg_handles(address) || (address & 3u))
        return;

    sh4_regs[(address - SH4REG_BASE) / 4u] = value;
    REGLOG("[reg] w %08x <- %08x (sh4)\n", address, value);
}