#ifndef DC_RUNTIME_AICA_CHAN_H
#define DC_RUNTIME_AICA_CHAN_H

#include <stdint.h>

void aica_chan_init(uint32_t *regs);

/* Process a KYONEX write: start or release channels according to KYONB. */
void aica_chan_keyon(void);

/* Generate one 44.1 kHz stereo sample and advance channel playback. */
void aica_chan_sample(int16_t out[2]);

/* Read status and position for the channel selected by MSLC. */
uint32_t aica_chan_info(void);
uint32_t aica_chan_pos(void);

#endif