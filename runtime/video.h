#ifndef DC_RUNTIME_VIDEO_H
#define DC_RUNTIME_VIDEO_H

/* What the Dreamcast's video output shows (framebuffer at FB_R_SOF1, format
 * from FB_R_CTRL), in an SDL3 window.
 *   DC_NOWINDOW=1   no window (test runs)
 *   DC_SHOT=file    write the screen as a PNG when the run ends */

void video_init(void);
void video_frame(void);      /* call often; presents once per new frame */
void video_shutdown(void);   /* writes DC_SHOT */

/* The pad, for maple.c. buttons use the Dreamcast's bits, but 1 = pressed.
 * Keyboard: arrows, Z = A, X = B, A = X, S = Y, Enter = START, Q/W = L/R
 * triggers; or the first gamepad.
 *   DC_PRESS=600:start,700:a   hold buttons for a few frames at those frames
 *                              (works without a window too) */
#define PAD_B     0x0002
#define PAD_A     0x0004
#define PAD_START 0x0008
#define PAD_UP    0x0010
#define PAD_DOWN  0x0020
#define PAD_LEFT  0x0040
#define PAD_RIGHT 0x0080
#define PAD_Y     0x0200
#define PAD_X     0x0400

typedef struct {
    unsigned short buttons;
    unsigned char rt, lt, x, y;   /* triggers 0-255, stick 0x80 = centre */
} dc_pad;

void video_pad(dc_pad *pad);

/* 44.1 kHz stereo samples from the AICA, to the speakers.
 *   DC_WAV=file   also write them to a .wav (works without a window) */
void video_audio(const short *stereo, int frames);

#endif
