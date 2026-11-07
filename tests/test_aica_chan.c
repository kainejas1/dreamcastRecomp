/*
 * Unit tests for AICA key on/off, against Flycast's rules. Needs no game data.
 *
 * Sound drivers write KYONEX whenever any channel changes, and KYONEX applies
 * every channel's KYONB. So a channel whose one-shot sample has ended must be
 * keyed off with KYONB cleared, or the next KYONEX for some other channel
 * starts it again (SA2's menu beep repeated for 1.5 s that way).
 */

#include "aica_chan.h"
#include "memory.h"

#include <stdio.h>
#include <string.h>

uint8_t dc_aram[DC_ARAM_SIZE];   // aica_chan.c reads samples from here

static uint32_t regs[0x8000 / 4];
static int failed;

#define EQ(actual, expected) do { \
    unsigned long long a_ = (actual), e_ = (expected); \
    if (a_ != e_) { \
        printf("%s:%d: %s = %llu (0x%llx), expected %llu (0x%llx)\n", \
               __func__, __LINE__, #actual, a_, a_, e_, e_); \
        failed = 1; \
    } \
} while (0)

#define CH(n, word) regs[(n) * 0x20 + (word)]
#define KYONB  0x4000u
#define LPCTL  0x0200u
#define SAMPLE 0x1000u   // 64 samples of PCM16 at 10000
#define LP     0x8000u
#define SGC(v) (((v) >> 13) & 3)

// a PCM16 sample at SAMPLE, played at one source sample per output sample
static void setup(unsigned n, int loop, unsigned rr, int audible)
{
    memset(&CH(n, 0), 0, 0x80);
    CH(n, 0) = (loop ? LPCTL : 0) | ((SAMPLE >> 16) & 0x7f);
    CH(n, 1) = SAMPLE & 0xffff;
    CH(n, 2) = 0;                   // LSA
    CH(n, 3) = 64;                  // LEA
    CH(n, 4) = 31;                  // AR: fastest attack
    CH(n, 5) = rr;                  // RR
    CH(n, 6) = 0;                   // OCT 0, FNS 0
    CH(n, 9) = audible ? 0x0f00 : 0;   // DISDL
    CH(n, 10) = 0;                  // TL
}

// set or clear KYONB on channel n, then write KYONEX
static void key(unsigned n, int on)
{
    CH(n, 0) = (CH(n, 0) & ~KYONB) | (on ? KYONB : 0);
    aica_chan_keyon();
}

// output samples that weren't silent, out of `count`
static int loud(int count)
{
    int n = 0;
    for (int i = 0; i < count; i++) {
        int16_t s[2];
        aica_chan_sample(s);
        n += s[0] != 0 || s[1] != 0;
    }
    return n;
}

static void monitor(unsigned n) { regs[0x280c / 4] = n << 8; }

static void reset(void)
{
    memset(regs, 0, sizeof regs);
    aica_chan_init(regs);
    regs[0x2800 / 4] = 15;   // MVOL
}

static void one_shot_plays_once(void)
{
    reset();
    setup(0, 0, 31, 1);
    setup(1, 0, 31, 0);   // silent: only channel 0 can be heard
    key(0, 1);
    int heard = loud(200);
    EQ(heard >= 60 && heard <= 64, 1);      // the 64 samples, less the attack's first
    EQ(CH(0, 0) & KYONB, 0);                // ended: keyed off in the register too
    monitor(0);
    uint32_t info = aica_chan_info(0);
    EQ(info & LP, LP);                      // passed its end
    EQ(SGC(info), 3);                       // release
    EQ(info & 0x1fff, 0x1fff);              // silent

    key(1, 1);                              // KYONEX for another channel
    EQ(loud(200), 0);                       // doesn't bring channel 0 back

    key(0, 1);                              // setting KYONB again does
    EQ(loud(200) > 0, 1);
}

static void key_on_while_fading_restarts(void)
{
    reset();
    setup(0, 1, 1, 1);    // looping, slowest release
    key(0, 1);
    loud(100);
    monitor(0);
    key(0, 0);
    loud(10);
    EQ(SGC(aica_chan_info(0)), 3);          // fading out
    EQ(loud(10), 10);                       // and still heard
    key(0, 1);
    EQ(SGC(aica_chan_info(0)) < 3, 1);      // keyed on again: back to attack/decay
    EQ(aica_chan_pos() < 5, 1);             // from the start of the sample
}

static void key_on_while_on_is_ignored(void)
{
    reset();
    setup(0, 1, 31, 1);
    key(0, 1);
    loud(40);
    monitor(0);
    uint32_t pos = aica_chan_pos();
    key(0, 1);                              // KYONEX again, KYONB still set
    EQ(aica_chan_pos(), pos);               // keeps playing where it was
}

static void lp_clears_on_read(void)
{
    reset();
    setup(0, 1, 31, 1);
    key(0, 1);
    loud(100);                              // past LEA once: looped
    monitor(0);
    EQ(aica_chan_info(0) & LP, LP);         // the low byte's read leaves it
    EQ(aica_chan_info(1) & LP, LP);         // this read sees it and clears it
    EQ(aica_chan_info(1) & LP, 0);
}

int main(void)
{
    for (uint32_t i = 0; i < 64; i++) {
        dc_aram[SAMPLE + 2 * i] = 10000 & 0xff;
        dc_aram[SAMPLE + 2 * i + 1] = 10000 >> 8;
    }
    one_shot_plays_once();
    key_on_while_fading_restarts();
    key_on_while_on_is_ignored();
    lp_clears_on_read();
    puts(failed ? "FAILED" : "ok");
    return failed;
}
