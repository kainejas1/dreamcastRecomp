#ifndef DC_RUNTIME_PATCHES_H
#define DC_RUNTIME_PATCHES_H

/*
 * Changes to the game itself, for one release each (checked against IP.BIN's
 * product number). So far they're values written into RAM once a frame, the
 * way Flycast applies its widescreen cheats.
 *
 *   DC_WIDESCREEN=1   SA2 (USA, MK-51117): the 3D view is widened to 16:9
 *                     inside the 640x480 frame, and the frame is shown
 *                     stretched to 16:9; 2D (HUD, menus) is pulled back in
 *                     so it keeps its shape (pvr.c), laid out as on 4:3
 *   DC_WIDESCREEN=2   the same, with 2D left stretched
 */

void patches_init(void);      /* after IP.BIN is in RAM */
void patches_frame(void);     /* once a frame */
int patches_widescreen(void); /* show frames at 16:9: 0 no, 1 with 2D kept 4:3, 2 all stretched */

#endif
