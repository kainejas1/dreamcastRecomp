#ifndef DC_RUNTIME_VIDEO_H
#define DC_RUNTIME_VIDEO_H

/* What the Dreamcast's video output shows (framebuffer at FB_R_SOF1, format
 * from FB_R_CTRL), in an SDL3 window.
 *   DC_NOWINDOW=1   no window (test runs)
 *   DC_SHOT=file    write the screen as a PNG when the run ends */

void video_init(void);
void video_frame(void);      /* call often; presents once per new frame */
void video_shutdown(void);   /* writes DC_SHOT */

#endif
