#ifndef DC_RUNTIME_PROF_H
#define DC_RUNTIME_PROF_H

#include <stdint.h>

/*
 * Where the host's time goes. DC_PROF=n prints a line every n frames:
 *   [prof] frames 120-240: 2.71 s, 44.3 fps | render 41% arm 9% mix 3% present 6% game 41% | audio underruns 3
 * "game" is everything else: the recompiled code and the hardware it talks to.
 * Costs a clock read per measured call when off, so it stays in.
 */

enum { PROF_RENDER, PROF_ARM, PROF_MIX, PROF_PRESENT, PROF_PARTS };

void prof_add(int part, uint64_t start_ns);   /* start_ns from clock_host_ns() */
void prof_underrun(void);                     /* the speakers ran out of samples */
void prof_frame(uint32_t frame);              /* once a frame: prints when due */

#endif
