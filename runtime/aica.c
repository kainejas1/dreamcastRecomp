#include "aica.h"
#include "holly.h"
#include "memory.h"

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

static uint32_t regs[(AICA_REG_END - AICA_REG_BASE) / 4];
static uint32_t driver_port;    // fake manatee.drv running: its port area in sound RAM, else 0
static uint32_t driver_frame;   // frame it started at

void aica_init(void)
{
    memset(regs, 0, sizeof regs);
    driver_port = 0;
    regs[(AICA_ARM_RESET - AICA_REG_BASE) / 4] = 1;   // sound CPU starts held in reset
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
static void fake_driver_start(void)
{
    uint32_t hdr = 0x20, port, ticks = 10;
    int found = 0;
    for (uint32_t i = 0; i < 0x100; i++)
        if (!memcmp(dc_aram + i, "manatee.drv", 11)) found = 1;
    if (!found) return;

    memcpy(&port, dc_aram + hdr + 0xa8, 4);
    if (port > DC_ARAM_SIZE - 0x20) return;
    memset(dc_aram + port, 0, 4);   // not busy
    memcpy(dc_aram + port + 0x18, &ticks, 4);
    memcpy(dc_aram + hdr + 0xd8, "EXEC", 4);
    driver_port = port;
    driver_frame = holly_frame_count();
    fprintf(stderr, "[aica] manatee.drv: faked driver start (port %05x)\n", port);
}

void aica_update(void)
{
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
    return regs[(phys - AICA_REG_BASE) / 4];
}

void aica_write32(uint32_t phys, uint32_t value)
{
    if (phys >= RTC_BASE) return;   // setting the clock: we always use the host's

    uint32_t old = regs[(phys - AICA_REG_BASE) / 4];
    regs[(phys - AICA_REG_BASE) / 4] = value;

    if (phys == AICA_ARM_RESET && ((old ^ value) & 1)) {
        fprintf(stderr, "[aica] sound CPU %s\n", value & 1 ? "held in reset" : "released (not emulated)");
        if (!(value & 1)) fake_driver_start();
    }
}
