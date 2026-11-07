/*
 * Unit tests for the emulated clock and the SH-4 timers. Needs no game data.
 *
 * The virtual clock runs with one check per nanosecond here, so clock_tick(n)
 * moves time by exactly n ns. One peripheral clock is 20 ns, so one TCNT tick
 * at the fastest prescaler (4) is 80 ns.
 */

#include "clock.h"
#include "tmu.h"

#include <stdio.h>

#define TSTR  0xffd80004u
#define TCOR(i) (0xffd80008u + 12u * (i))
#define TCNT(i) (0xffd8000cu + 12u * (i))
#define TCR(i)  (0xffd80010u + 12u * (i))
#define UNF   0x0100u
#define UNIE  0x0020u

static int failed;

#define EQ(actual, expected) do { \
    unsigned long long a_ = (actual), e_ = (expected); \
    if (a_ != e_) { \
        printf("%s:%d: %s = %llu (0x%llx), expected %llu (0x%llx)\n", \
               __func__, __LINE__, #actual, a_, a_, e_, e_); \
        failed = 1; \
    } \
} while (0)

static void fresh(void)
{
    clock_init_virtual(DC_FRAME_NS);
    tmu_init();
}

static void ticks(uint32_t n, uint32_t tpsc)
{
    clock_tick(n * (80u << (2 * tpsc)));
}

static void test_clock_scales_checks_to_frames(void)
{
    clock_init_virtual(1000);
    EQ(clock_is_virtual(), 1);
    EQ(clock_ns(), 0);
    clock_tick(500);
    EQ(clock_ns(), DC_FRAME_NS / 2);
    clock_tick(500);
    EQ(clock_ns(), DC_FRAME_NS);
    clock_tick(2000);
    EQ(clock_ns(), 3ull * DC_FRAME_NS);
}

static void test_clock_calendar_is_fixed_and_follows_time(void)
{
    clock_init_virtual(DC_FRAME_NS);
    EQ(clock_unix_seconds(), 992908800u);
    for (int i = 0; i < 3; i++) clock_tick(1000000000u);
    EQ(clock_unix_seconds(), 992908803u);
}

static void test_clock_does_not_overflow_on_long_runs(void)
{
    clock_init_virtual(200000);
    // about 139 hours of frames
    for (uint64_t i = 0; i < 30000000ull; i++) clock_tick(200000);
    EQ(clock_ns(), 30000000ull * DC_FRAME_NS);
}

static void test_reset_state(void)
{
    fresh();
    EQ(tmu_read32(TSTR), 0);
    for (uint32_t i = 0; i < 3; i++) {
        EQ(tmu_read32(TCOR(i)), 0xffffffffu);
        EQ(tmu_read32(TCNT(i)), 0xffffffffu);
        EQ(tmu_read32(TCR(i)), 0);
    }
}

static void test_stopped_timer_does_not_count(void)
{
    fresh();
    tmu_write32(TCNT(0), 1000);
    ticks(10, 0);
    EQ(tmu_read32(TCNT(0)), 1000);
}

static void test_time_while_stopped_is_not_counted_later(void)
{
    fresh();
    tmu_write32(TCNT(0), 1000);
    ticks(10, 0);
    tmu_write32(TSTR, 1);
    EQ(tmu_read32(TCNT(0)), 1000);
    ticks(10, 0);
    EQ(tmu_read32(TCNT(0)), 990);
}

static void test_every_prescaler(void)
{
    for (uint32_t tpsc = 0; tpsc <= 4; tpsc++) {
        fresh();
        tmu_write32(TCR(0), tpsc);
        tmu_write32(TCNT(0), 1000);
        tmu_write32(TSTR, 1);
        ticks(10, tpsc);
        EQ(tmu_read32(TCNT(0)), 990);
    }
}

static void test_clock_sources_a_dreamcast_lacks_do_not_count(void)
{
    fresh();
    tmu_write32(TCR(0), 6);
    tmu_write32(TCNT(0), 1000);
    tmu_write32(TSTR, 1);
    ticks(1000, 0);
    EQ(tmu_read32(TCNT(0)), 1000);
}

static void test_channels_are_independent(void)
{
    fresh();
    tmu_write32(TCR(1), 1);   // 4 times slower than channel 0
    tmu_write32(TCNT(0), 1000);
    tmu_write32(TCNT(1), 1000);
    tmu_write32(TCNT(2), 1000);
    tmu_write32(TSTR, 3);     // channel 2 stays stopped
    ticks(40, 0);
    EQ(tmu_read32(TCNT(0)), 960);
    EQ(tmu_read32(TCNT(1)), 990);
    EQ(tmu_read32(TCNT(2)), 1000);
}

static void test_underflow_takes_tcnt_plus_one_ticks_and_reloads(void)
{
    fresh();
    tmu_write32(TCOR(0), 9);
    tmu_write32(TCNT(0), 2);
    tmu_write32(TSTR, 1);
    ticks(2, 0);
    EQ(tmu_read32(TCNT(0)), 0);
    EQ(tmu_read32(TCR(0)) & UNF, 0);
    ticks(1, 0);
    EQ(tmu_read32(TCNT(0)), 9);
    EQ(tmu_read32(TCR(0)) & UNF, UNF);
    ticks(1, 0);
    EQ(tmu_read32(TCNT(0)), 8);
}

static void test_several_underflows_at_once(void)
{
    fresh();
    tmu_write32(TCOR(0), 4);   // a period of 5
    tmu_write32(TCNT(0), 0);
    tmu_write32(TSTR, 1);
    ticks(1 + 12, 0);          // underflow to 4, then 12 more: 2 periods and 2
    EQ(tmu_read32(TCNT(0)), 2);
}

static void test_largest_period_does_not_overflow(void)
{
    fresh();
    tmu_write32(TCOR(0), 0xffffffffu);
    tmu_write32(TCNT(0), 0);
    tmu_write32(TSTR, 1);
    ticks(2, 0);
    EQ(tmu_read32(TCNT(0)), 0xfffffffeu);
    EQ(tmu_read32(TCR(0)) & UNF, UNF);
}

static void test_unf_can_only_be_cleared(void)
{
    fresh();
    tmu_write32(TCR(0), UNF);
    EQ(tmu_read32(TCR(0)) & UNF, 0);

    tmu_write32(TCOR(0), 9);
    tmu_write32(TCNT(0), 0);
    tmu_write32(TSTR, 1);
    ticks(1, 0);
    tmu_write32(TCR(0), UNF);
    EQ(tmu_read32(TCR(0)) & UNF, UNF);
    tmu_write32(TCR(0), 0);
    EQ(tmu_read32(TCR(0)) & UNF, 0);
}

static void test_tcr_masks(void)
{
    fresh();
    tmu_write32(TCR(0), 0xffffu);
    EQ(tmu_read32(TCR(0)), 0x003fu);
    tmu_write32(TCR(2), 0xffffu);
    EQ(tmu_read32(TCR(2)), 0x02ffu);
}

static void test_prescaler_change_keeps_time_already_counted(void)
{
    fresh();
    tmu_write32(TCNT(0), 1000);
    tmu_write32(TSTR, 1);
    ticks(10, 0);
    tmu_write32(TCR(0), 1);   // 10 ticks at /4 happened before this
    EQ(tmu_read32(TCNT(0)), 990);
    ticks(10, 1);
    EQ(tmu_read32(TCNT(0)), 980);
}

static void test_interrupt_needs_unf_and_unie(void)
{
    uint32_t evt = 0;

    fresh();
    tmu_write32(TCOR(0), 9);
    tmu_write32(TCNT(0), 0);
    tmu_write32(TSTR, 1);
    EQ(tmu_pending(0xf000u, &evt), 0);   // UNIE set later, UNF not yet
    tmu_write32(TCR(0), UNIE);
    EQ(tmu_pending(0xf000u, &evt), 0);
    ticks(1, 0);
    EQ(tmu_pending(0xf000u, &evt), 15);
    EQ(evt, 0x400);
    tmu_write32(TCR(0), UNIE);           // clears UNF
    EQ(tmu_pending(0xf000u, &evt), 0);
}

static void test_ipra_level_zero_never_interrupts(void)
{
    uint32_t evt = 0;

    fresh();
    tmu_write32(TCR(0), UNIE);
    tmu_write32(TCNT(0), 0);
    tmu_write32(TSTR, 1);
    ticks(1, 0);
    EQ(tmu_pending(0x0fffu, &evt), 0);
}

static void test_highest_ipra_level_wins(void)
{
    uint32_t evt = 0;

    fresh();
    for (uint32_t i = 0; i < 3; i++) {
        tmu_write32(TCR(i), UNIE);
        tmu_write32(TCNT(i), 0);
    }
    tmu_write32(TSTR, 7);
    ticks(1, 0);
    EQ(tmu_pending(0x2940u, &evt), 9);   // TMU1
    EQ(evt, 0x420);
    EQ(tmu_pending(0x2350u, &evt), 5);   // TMU2
    EQ(evt, 0x440);
    EQ(tmu_pending(0x4440u, &evt), 4);   // a tie goes to TMU0
    EQ(evt, 0x400);
}

int main(void)
{
    test_clock_scales_checks_to_frames();
    test_clock_calendar_is_fixed_and_follows_time();
    test_clock_does_not_overflow_on_long_runs();
    test_reset_state();
    test_stopped_timer_does_not_count();
    test_time_while_stopped_is_not_counted_later();
    test_every_prescaler();
    test_clock_sources_a_dreamcast_lacks_do_not_count();
    test_channels_are_independent();
    test_underflow_takes_tcnt_plus_one_ticks_and_reloads();
    test_several_underflows_at_once();
    test_largest_period_does_not_overflow();
    test_unf_can_only_be_cleared();
    test_tcr_masks();
    test_prescaler_change_keeps_time_already_counted();
    test_interrupt_needs_unf_and_unie();
    test_ipra_level_zero_never_interrupts();
    test_highest_ipra_level_wins();

    printf(failed ? "TIME TESTS FAILED\n" : "TIME TESTS PASSED\n");
    return failed;
}
