#include "tmu.h"
#include "clock.h"

#include <stdio.h>

/*
 * Each channel counts TCNT down at the peripheral clock (50 MHz) divided by
 * 4, 16, 64, 256 or 1024. Going past 0 reloads TCOR and sets TCR.UNF, which
 * stays set until the game clears it, and interrupts while TCR.UNIE is set.
 *
 * Counters are brought up to date when something looks at them, not on every
 * tick. Input capture on channel 2 is not implemented.
 */

#define TMU_BASE 0xffd80000u
#define TMU_END  0xffd80030u
#define TOCR     0xffd80000u
#define TSTR     0xffd80004u
#define TCPR2    0xffd8002cu

#define TCR_TPSC 0x0007u
#define TCR_UNIE 0x0020u
#define TCR_UNF  0x0100u

#define NS_PER_PCLK 20u

typedef struct {
    uint32_t tcor, tcnt, tcr;
    uint64_t last;   /* peripheral clock when this channel was last updated */
} tmu_chan;

static tmu_chan chan[3];
static uint32_t tocr, tstr, tcpr2;

void tmu_init(void)
{
    for (int i = 0; i < 3; i++) {
        chan[i].tcor = chan[i].tcnt = 0xffffffffu;
        chan[i].tcr = 0;
        chan[i].last = clock_ns() / NS_PER_PCLK;
    }
    tocr = tstr = tcpr2 = 0;
}

/* log2 of the prescaler, -1 for the clock sources a Dreamcast doesn't have */
static int prescale(uint32_t tcr)
{
    uint32_t tpsc = tcr & TCR_TPSC;
    return tpsc <= 4 ? 2 + 2 * (int)tpsc : -1;
}

static void update(int i)
{
    tmu_chan *t = &chan[i];
    uint64_t now = clock_ns() / NS_PER_PCLK, prev = t->last;
    int shift = prescale(t->tcr);

    t->last = now;
    if (!(tstr & (1u << i)) || shift < 0) return;

    uint64_t ticks = (now >> shift) - (prev >> shift);
    if (ticks <= t->tcnt) {
        t->tcnt -= (uint32_t)ticks;
        return;
    }
    // past zero, possibly several times over
    ticks -= (uint64_t)t->tcnt + 1;
    t->tcnt = t->tcor - (uint32_t)(ticks % ((uint64_t)t->tcor + 1));
    t->tcr |= TCR_UNF;
}

static void update_all(void)
{
    for (int i = 0; i < 3; i++) update(i);
}

int tmu_handles(uint32_t address)
{
    return address >= TMU_BASE && address < TMU_END;
}

uint32_t tmu_read32(uint32_t address)
{
    update_all();

    if (address == TOCR) return tocr;
    if (address == TSTR) return tstr;
    if (address == TCPR2) return tcpr2;

    uint32_t off = address - 0xffd80008u;
    const tmu_chan *t = &chan[off / 12u];
    switch (off % 12u) {
    case 0:  return t->tcor;
    case 4:  return t->tcnt;
    default: return t->tcr;
    }
}

void tmu_write32(uint32_t address, uint32_t value)
{
    update_all();   // at the old settings, before they change

    if (address == TOCR) { tocr = value & 1u; return; }
    if (address == TSTR) { tstr = value & 7u; return; }
    if (address == TCPR2) return;   // read-only

    uint32_t off = address - 0xffd80008u;
    int i = (int)(off / 12u);
    tmu_chan *t = &chan[i];
    switch (off % 12u) {
    case 0:
        t->tcor = value;
        break;
    case 4:
        t->tcnt = value;
        break;
    default: {
        uint32_t mask = i == 2 ? 0x03ffu : 0x013fu;
        // UNF can only be cleared: writing 0 clears it, writing 1 leaves it
        uint32_t unf = t->tcr & value & TCR_UNF;
        t->tcr = (value & mask & ~TCR_UNF) | unf;
        break;
    }
    }
}

int tmu_pending(uint32_t ipra, uint32_t *intevt)
{
    int best = 0;

    update_all();
    for (int i = 0; i < 3; i++) {
        if ((chan[i].tcr & (TCR_UNF | TCR_UNIE)) != (TCR_UNF | TCR_UNIE)) continue;
        int level = (int)((ipra >> (12 - 4 * i)) & 15u);
        if (level > best) {
            best = level;
            *intevt = 0x400u + 0x20u * (uint32_t)i;
        }
    }
    return best;
}
