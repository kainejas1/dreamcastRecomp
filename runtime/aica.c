#include "aica.h"
#include "arm7.h"
#include "holly.h"
#include "memory.h"
#include "aica_chan.h"
#include "video.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define AICA_REG_BASE 0x00700000u
#define AICA_REG_END  0x00708000u
#define RTC_BASE      0x00710000u
#define RTC_HI        0x00710000u   // seconds since 1950, top 16 bits
#define RTC_LO        0x00710004u   // bottom 16 bits
#define RTC_END       0x0071000cu

// register offsets, from 00700000 (the ARM sees them at 00800000)
#define R_SLOT_INFO 0x2810u   // the monitored channel: loop flag, envelope state and level
#define R_SLOT_POS  0x2814u   // its play position
#define R_TIMA      0x2890u   // timers A B C: 2890 2894 2898
#define R_SCIEB     0x289cu   // interrupts to the ARM: enabled, pending, clear
#define R_SCIPD     0x28a0u
#define R_SCIRE     0x28a4u
#define R_SCILV0    0x28a8u   // level bits, 28a8 28ac 28b0
#define R_MCIEB     0x28b4u   // interrupts to the SH-4: enabled, pending, clear
#define R_MCIPD     0x28b8u
#define R_MCIRE     0x28bcu
#define R_INTREQ    0x2d00u   // the ARM reads the level of its interrupt here
#define R_INTCLEAR  0x2d04u   // and acknowledges it here

#define ARM_PER_SAMPLE 128    // instructions per sample: the ARM7 is 22.6 MHz but waits on sound RAM

static uint32_t regs[(AICA_REG_END - AICA_REG_BASE) / 4];
#define REG(off) regs[(off) / 4]
static uint32_t driver_port;    // fake manatee.drv running: its port area in sound RAM, else 0
static uint32_t driver_frame;   // frame it started at
static int fake_driver;         // DC_FAKEDRIVER=1: fake the driver instead of running it
static int arm_running;
static uint64_t samples_done;   // 44100 a second, on the video clock
static unsigned timer_div[3];

// ---- the ARM's bus: sound RAM at 0, these registers at 800000 ----

static uint32_t arm_bus_r32(uint32_t addr, void *user)
{
    (void)user;
    if (addr < 0x00800000u) {
        uint32_t v;
        memcpy(&v, &dc_aram[addr & (DC_ARAM_SIZE - 4)], 4);
        return v;
    }
    if (addr < 0x00808000u) return aica_read32(AICA_REG_BASE + (addr & 0x7ffcu));
    return 0;
}

static uint8_t arm_bus_r8(uint32_t addr, void *user)
{
    if (addr < 0x00800000u) return dc_aram[addr & (DC_ARAM_SIZE - 1)];
    return (uint8_t)(arm_bus_r32(addr & ~3u, user) >> ((addr & 3) * 8));
}

static void arm_bus_w32(uint32_t addr, uint32_t value, void *user)
{
    (void)user;
    if (addr < 0x00800000u) memcpy(&dc_aram[addr & (DC_ARAM_SIZE - 4)], &value, 4);
    else if (addr < 0x00808000u) aica_write32(AICA_REG_BASE + (addr & 0x7ffcu), value);
}

static void arm_bus_w8(uint32_t addr, uint8_t value, void *user)
{
    if (addr < 0x00800000u) {
        dc_aram[addr & (DC_ARAM_SIZE - 1)] = value;
    } else if (addr < 0x00808000u) {
        // registers are 32 bit: change one byte of what's there
        uint32_t shift = (addr & 3) * 8, old = REG(addr & 0x7ffcu);
        if ((addr & 0x7ffcu) == R_SCIRE || (addr & 0x7ffcu) == R_MCIRE) old = 0;   // write-1-to-clear: only this byte
        arm_bus_w32(addr & ~3u, (old & ~(0xffu << shift)) | (uint32_t)value << shift, user);
    }
}

void aica_init(void)
{
    static const arm7_bus bus = { arm_bus_r8, arm_bus_r32, arm_bus_w8, arm_bus_w32, NULL };
    memset(regs, 0, sizeof regs);
    aica_chan_init(regs);
    driver_port = 0;
    arm_running = 0;
    fake_driver = getenv("DC_FAKEDRIVER") != NULL;
    regs[(AICA_ARM_RESET - AICA_REG_BASE) / 4] = 1;   // sound CPU starts held in reset
    arm7_set_bus(&bus);
}

// The ARM's interrupt line (its FIQ) and the level it reads in INTREQ: the
// lowest pending and enabled source, its level from one bit each of SCILV0-2
// (sources past 7 use bit 7). And the SH-4's: Holly SB_ISTEXT bit 1.
static void update_interrupts(void)
{
    uint32_t p = REG(R_SCIPD) & REG(R_SCIEB) & 0x7ffu, level = 0;
    if (p) {
        unsigned b = 0;
        while (!((p >> b) & 1)) b++;
        if (b > 7) b = 7;
        level = ((REG(R_SCILV0) >> b) & 1) | ((REG(R_SCILV0 + 4) >> b) & 1) << 1 | ((REG(R_SCILV0 + 8) >> b) & 1) << 2;
    }
    REG(R_INTREQ) = level;
    arm7_fiq(p != 0);

    uint32_t ext = holly_reg(HOLLY_SB_ISTEXT);
    holly_set_reg(HOLLY_SB_ISTEXT, (REG(R_MCIPD) & REG(R_MCIEB)) ? ext | 2u : ext & ~2u);
}

// one sample: the timers count every 1 << prescale samples, and going past
// 0xff is an interrupt (bit 6, 7, 8) for both CPUs
static void tick_sample(void)
{
    for (unsigned t = 0; t < 3; t++) {
        uint32_t *tr = &REG(R_TIMA + 4 * t);
        if (++timer_div[t] < (1u << ((*tr >> 8) & 7))) continue;
        timer_div[t] = 0;
        uint32_t c = (*tr + 1) & 0xffu;
        *tr = (*tr & ~0xffu) | c;
        if (!c) {
            REG(R_SCIPD) |= 0x40u << t;
            REG(R_MCIPD) |= 0x40u << t;
        }
    }
}

int aica_handles(uint32_t phys)
{
    return (phys >= AICA_REG_BASE && phys < AICA_REG_END) || (phys >= RTC_BASE && phys < RTC_END);
}

// the Dreamcast clock counts from 1950-01-01; unix time is 20 years (7305 days) later
static uint32_t rtc_now(void)
{
    return (uint32_t)time(NULL) + 631152000u;
}

// Fake sound driver start. Katana games load Sega's manatee.drv into sound RAM
// and release the ARM, then wait for the driver to come up:
//  - header (0x20) +0xa8 points to a port area, filled with "DMPD"; port+0x18
//    has to change and reach 10 (the driver's tick count)
//  - header +0xd8 has to say "EXEC"
// the ARM isn't emulated so write both ourselves.
// Commands get DMA'd to sound RAM, then port byte 0 is set to 1; the driver
// clears it once it has taken them. aica_update() does that part.
// manatee.drv in sound RAM: its port area, else 0
static uint32_t manatee_port(void)
{
    uint32_t port;
    int found = 0;
    for (uint32_t i = 0; i < 0x100; i++)
        if (!memcmp(dc_aram + i, "manatee.drv", 11)) found = 1;
    if (!found) return 0;
    memcpy(&port, dc_aram + 0x20 + 0xa8, 4);
    return port <= DC_ARAM_SIZE - 0x20 ? port : 0;
}

static void fake_driver_start(void)
{
    uint32_t hdr = 0x20, port = manatee_port(), ticks = 10;
    if (!port) return;
    memset(dc_aram + port, 0, 4);   // not busy
    memcpy(dc_aram + port + 0x18, &ticks, 4);
    memcpy(dc_aram + hdr + 0xd8, "EXEC", 4);
    driver_port = port;
    driver_frame = holly_frame_count();
    fprintf(stderr, "[aica] manatee.drv: faked driver start (port %05x)\n", port);
}

void aica_update(void)
{
    if (!fake_driver) {
        // run the ARM for the samples since last time (at most 0.1 s worth)
        uint64_t target = holly_clock_us() * 44100u / 1000000u;
        if (!arm_running) { samples_done = target; return; }
        if (target - samples_done > 4410) samples_done = target - 4410;
        while (samples_done < target) {
            unsigned batch = target - samples_done > 16 ? 16 : (unsigned)(target - samples_done);
            int16_t pcm[16 * 2];
            for (unsigned i = 0; i < batch; i++) {
                tick_sample();
                aica_chan_sample(&pcm[i * 2]);
            }
            video_audio(pcm, (int)batch);
            update_interrupts();
            arm7_run((int)batch * ARM_PER_SAMPLE);
            samples_done += batch;
        }
        // DC_AICALOG=1: say when the driver takes the game's commands (port byte back to 0)
        static int log = -1;
        static uint8_t last;
        if (log < 0) log = getenv("DC_AICALOG") ? 30 : 0;
        if (log > 0 && driver_port) {
            uint8_t b = dc_aram[driver_port];
            if (last && !b) { log--; fprintf(stderr, "[aica] driver took commands (frame %u)\n", holly_frame_count()); }
            last = b;
        }
        return;
    }
    if (!driver_port) return;
    uint32_t ticks = 10 + holly_frame_count() - driver_frame;
    memcpy(dc_aram + driver_port + 0x18, &ticks, 4);
    if (dc_aram[driver_port]) {
        if (getenv("DC_AICALOG")) fprintf(stderr, "[aica] driver: took commands (port byte 0 = %02x)\n", dc_aram[driver_port]);
        dc_aram[driver_port] = 0;
    }
}

uint32_t aica_read32(uint32_t phys)
{
    if (phys == RTC_HI) return rtc_now() >> 16;
    if (phys == RTC_LO) return rtc_now() & 0xffffu;
    if (phys >= RTC_BASE) return 0;
    uint32_t off = phys - AICA_REG_BASE;
    if (off == R_SLOT_INFO) return aica_chan_info();
    if (off == R_SLOT_POS) return aica_chan_pos();
    return regs[off / 4];
}

void aica_write32(uint32_t phys, uint32_t value)
{
    if (phys >= RTC_BASE) return;   // setting the clock: we always use the host's

    uint32_t off = phys - AICA_REG_BASE;
    uint32_t old = regs[off / 4];

    switch (off) {
    case R_SCIRE: REG(R_SCIPD) &= ~value; update_interrupts(); return;
    case R_MCIRE: REG(R_MCIPD) &= ~value; update_interrupts(); return;
    // only bit 5 can be set from outside: one CPU interrupting the other
    case R_SCIPD: REG(R_SCIPD) |= value & 0x20u; update_interrupts(); return;
    case R_MCIPD: REG(R_MCIPD) |= value & 0x20u; update_interrupts(); return;
    case R_INTREQ: return;
    case R_INTCLEAR: update_interrupts(); return;
    }
    if (off < 0x2000u && (off % 0x80u) == 0 &&
        (value & 0x8000u)) {
        /* KYONEX triggers key-on/key-off for all channels. */
        regs[off / 4] = value & ~0x8000u;
        aica_chan_keyon();
        return;
    }
    regs[off / 4] = value;
    if (off >= R_TIMA && off <= R_TIMA + 8) timer_div[(off - R_TIMA) / 4] = 0;
    if (off == R_SCIEB || off == R_MCIEB || (off >= R_SCILV0 && off <= R_SCILV0 + 8)) update_interrupts();

    if (phys == AICA_ARM_RESET && ((old ^ value) & 1)) {
        fprintf(stderr, "[aica] sound CPU %s\n", value & 1 ? "held in reset" : fake_driver ? "released (faked)" : "released");
        if (value & 1) {
            arm_running = 0;
        } else if (fake_driver) {
            fake_driver_start();
        } else {
            arm7_reset();
            arm_running = 1;
            driver_port = manatee_port();   // only for DC_AICALOG
            samples_done = holly_clock_us() * 44100u / 1000000u;
        }
    }
}
