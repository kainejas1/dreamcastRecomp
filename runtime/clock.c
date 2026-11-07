#include "clock.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#define VIRTUAL_EPOCH 992908800ull   /* 2001-06-19 */

static int virtual_mode;
static uint32_t checks_per_frame;
static uint64_t checks;

static uint64_t host_frequency, host_start;

static uint64_t host_ticks(void)
{
#ifdef _WIN32
    LARGE_INTEGER now;
    if (!QueryPerformanceCounter(&now)) return 0;
    return (uint64_t)now.QuadPart;
#else
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}

static uint64_t ticks_per_second(void)
{
#ifdef _WIN32
    LARGE_INTEGER f;
    if (!QueryPerformanceFrequency(&f) || f.QuadPart <= 0) return 0;
    return (uint64_t)f.QuadPart;
#else
    return 1000000000ull;
#endif
}

void clock_init_virtual(uint32_t n)
{
    virtual_mode = 1;
    checks_per_frame = n ? n : 1;
    checks = 0;
}

void clock_init(void)
{
    const char *v = getenv("DC_VCLOCK");
    uint32_t n = v ? (uint32_t)strtoul(v, NULL, 0) : 0;

    if (n) {
        clock_init_virtual(n);
        fprintf(stderr, "[clock] virtual, %u checks per frame\n", n);
        return;
    }
    virtual_mode = 0;
    host_frequency = ticks_per_second();
    host_start = host_ticks();
}

int clock_is_virtual(void) { return virtual_mode; }

uint64_t clock_ns(void)
{
    if (virtual_mode) {
        uint64_t frames = checks / checks_per_frame, rest = checks % checks_per_frame;
        return frames * DC_FRAME_NS + rest * DC_FRAME_NS / checks_per_frame;
    }
    if (!host_frequency) return 0;
    uint64_t t = host_ticks() - host_start;
    return t / host_frequency * 1000000000u + t % host_frequency * 1000000000u / host_frequency;
}

void clock_tick(uint32_t n)
{
    checks += n;
}

uint64_t clock_host_ns(void)
{
    static uint64_t frequency, start;
    if (!frequency) {
        frequency = ticks_per_second();
        start = host_ticks();
        if (!frequency) return 0;
    }
    uint64_t t = host_ticks() - start;
    return t / frequency * 1000000000u + t % frequency * 1000000000u / frequency;
}

uint64_t clock_unix_seconds(void)
{
    if (virtual_mode) return VIRTUAL_EPOCH + clock_ns() / 1000000000u;
    return (uint64_t)time(NULL);
}
