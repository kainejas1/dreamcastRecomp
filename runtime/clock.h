#ifndef DC_RUNTIME_CLOCK_H
#define DC_RUNTIME_CLOCK_H

#include <stdint.h>

/*
 * The one source of emulated time: video, sound, the SH-4 timers and the
 * battery clock all read it from here.
 *
 * Host mode (the default) follows real time, so a run plays at the right
 * speed but no two runs are the same.
 *
 * Virtual mode (DC_VCLOCK=n) defines a frame as n interrupt checks. Time then
 * depends only on what the game executes, so interrupts land at the same
 * points in every run and two runs can be compared. It runs as fast as the
 * host allows, which is meant for headless runs (DC_NOWINDOW=1, input from
 * DC_PRESS). If n is too low the game gets too little done per frame and
 * slows down, which is still reproducible.
 */

#define DC_FRAME_NS 16666666u   /* one 60 Hz frame */

void clock_init(void);                       /* picks the mode from DC_VCLOCK */
void clock_init_virtual(uint32_t checks_per_frame);

int clock_is_virtual(void);

uint64_t clock_ns(void);                     /* emulated time since start */
void clock_tick(uint32_t checks);            /* interrupt checks ran; moves virtual time */

/* Wall clock for the game's calendar. Fixed in virtual mode: it starts at
 * SA2's release date and follows emulated time. */
uint64_t clock_unix_seconds(void);

/* Real time since the first call, in either mode. For measuring the host
 * (DC_PROF), never for anything the game sees. */
uint64_t clock_host_ns(void);

#endif
