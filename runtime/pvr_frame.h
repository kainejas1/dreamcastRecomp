#ifndef DC_RUNTIME_PVR_FRAME_H
#define DC_RUNTIME_PVR_FRAME_H

#include <stdint.h>

/*
 * One render (STARTRENDER) as the TA collected it, for a renderer other than
 * pvr.c's own software one (gpu.c). Strips come in the order they were sent;
 * a renderer draws list 0 (opaque), then 4 (punch-through), then 2
 * (translucent).
 */

typedef struct {
    float x, y, z;            // screen pixels, and 1/w (bigger is nearer)
    float u, v;
    float col[4], off[4];     // r g b a, 0..1
} pvr_vertex;

typedef struct {
    uint32_t pcw, isp, tsp, tcw;   // as the polygon header gave them
    int list, first, count;   // a triangle strip: count vertices from first
} pvr_strip;

#define PVR_PCW_SHADOW 0x80u   // a strip modifier volumes act on

/*
 * Modifier volumes: polygons that don't draw anything themselves but mark
 * where they enclose what's already drawn (SA2: the shadows under
 * characters). A volume is a run of polygons; each polygon's triangles are
 * in pvr_frame.mv, three vertices apiece (only x, y, z used).
 */
typedef struct {
    uint32_t isp;             // 31-29: 0 normal, 1 "inside last", 2 "outside last"; 28-27 culling
    int last;                 // this polygon closes the volume (its header's PCW bit 6)
    int list;                 // 1 opaque, 3 translucent
    int first, count;         // triangles
} pvr_modvol;

typedef struct {
    const pvr_vertex *v;
    int nv;
    const pvr_strip *s;
    int ns;
    int width, height;        // render size in pixels (twice as wide with the horizontal scaler)
    int clip_x0, clip_x1, clip_y0, clip_y1;   // inclusive
    uint32_t bg;              // background plane colour, ARGB8888
    float bgz;                // and depth
    float pt_ref;             // punch-through alpha threshold, 0..1
    uint32_t fb;              // FB_W_SOF1: where the picture belongs in VRAM
    int hscale;               // horizontal scaler on: shown at half the width
    const pvr_vertex *mv;     // modifier volume triangles, 3 vertices each
    int nmv;                  // vertices in mv
    const pvr_modvol *m;
    int nm;
    int autosort;             // ISP_FEED_CFG bit 0 clear: translucent pixels sorted by depth
    int shadow_on;            // FPU_SHAD_SCALE bit 8: inside a volume, colour is scaled
    float shadow;             // by this (else: the polygon's second volume, not drawn yet)
} pvr_frame;

/* Textures, by the strip's TSP and TCW. The palette is the one read at the
 * last render. */
void pvr_texture_size(uint32_t tsp, int *w, int *h);
uint64_t pvr_texture_hash(uint32_t tsp, uint32_t tcw);       /* texels (and palette) it reads */
void pvr_texture_decode(uint32_t tsp, uint32_t tcw, uint32_t *argb);   /* w * h ARGB8888 */

/* video.c: the display is showing the framebuffer at this address. A render
 * to an address that has been on screen can stay on the GPU; one that hasn't
 * (render to texture, say) is drawn into VRAM. */
void pvr_note_display(uint32_t fb_r_sof1);

#endif
