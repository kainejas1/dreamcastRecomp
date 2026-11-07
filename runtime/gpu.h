#ifndef DC_RUNTIME_GPU_H
#define DC_RUNTIME_GPU_H

#include "pvr_frame.h"

#include <stdint.h>

/*
 * The PowerVR's renders on the host GPU (SDL_gpu, Vulkan), at any multiple of
 * the Dreamcast's resolution. pvr.c still collects the display lists; this
 * draws them. Built when glslc was found (DC_GPU).
 *
 *   DC_RENDERER=gpu|soft   default: gpu with a window, soft without one
 *   DC_SCALE=n             render n times 640x480 (default: fit the window)
 *   DC_SHOTFULL=1          DC_SHOT saves the picture at the scale it was drawn
 *
 * What it draws so far: opaque, punch-through and translucent lists in the
 * order sent, textures (all formats, decoded on the CPU and cached until their
 * VRAM changes), point or bilinear filtering, clamp/flip/repeat, the four
 * shading modes, offset colour, every depth compare and blend factor, the
 * opaque list's modifier volumes (stencil shadows), the translucent list
 * sorted back to front by triangle (pvr.c sorts per pixel), fog and the colour
 * clamp, and mipmaps from the game's own levels.
 * Not yet: the translucent list's volumes, two-volume polygons' second colours.
 */

struct SDL_Window;

/* window may be NULL (offscreen: DC_SHOT still works). 0 if there's no GPU
 * device or the renderer is set to soft. */
int gpu_init(struct SDL_Window *window);
int gpu_active(void);

/* draw a render; the picture stays on the GPU, filed under f->fb */
void gpu_render(const pvr_frame *f);

/* Show the GPU's picture for framebuffer address fb, letterboxed into the
 * window. 0 if it has none for that address (or only a stale one): the
 * caller shows VRAM through gpu_present_pixels instead. */
int gpu_present(uint32_t fb);
void gpu_present_pixels(const uint32_t *argb, int w, int h);

/* The GPU's picture for fb at native resolution, ARGB8888 into *pixels
 * (realloc'd), for DC_SHOT. 0 if there is none. */
int gpu_read(uint32_t fb, uint32_t **pixels, int *w, int *h);

#endif
