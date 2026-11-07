#ifndef DC_RUNTIME_PVR_H
#define DC_RUNTIME_PVR_H

#include <stdint.h>

/* PowerVR2 (CLX2): the tile accelerator (TA) that takes display lists from
 * the CPU, and a software renderer standing in for the ISP/TSP core.
 *
 * The game sends polygon data to the TA FIFO (physical 10000000-107fffff)
 * with store queues or ch2 DMA. The TA here decodes it straight into vertex
 * and strip lists instead of building object lists in VRAM; STARTRENDER
 * draws those into the framebuffer at FB_W_SOF1. */

/* registers the TA/renderer care about */
#define PVR_SB_C2DSTAT     0x005f6800u   /* ch2 DMA destination (TA FIFO or texture memory) */
#define PVR_SB_C2DLEN      0x005f6804u
#define PVR_SB_C2DST       0x005f6808u   /* write 1: start */
#define PVR_SB_LMMODE0     0x005f6884u   /* texture path 0x11xxxxxx: 0 = 64 bit, 1 = 32 bit */
#define PVR_SB_LMMODE1     0x005f6888u
#define PVR_SOFTRESET      0x005f8008u
#define PVR_STARTRENDER    0x005f8014u
#define PVR_PARAM_BASE     0x005f8020u
#define PVR_REGION_BASE    0x005f802cu
#define PVR_FB_R_CTRL      0x005f8044u
#define PVR_FB_W_CTRL      0x005f8048u
#define PVR_FB_W_LINESTRIDE 0x005f804cu
#define PVR_FB_R_SOF1      0x005f8050u
#define PVR_FB_R_SOF2      0x005f8054u
#define PVR_FB_R_SIZE      0x005f805cu
#define PVR_FB_W_SOF1      0x005f8060u
#define PVR_FB_X_CLIP      0x005f8068u
#define PVR_FB_Y_CLIP      0x005f806cu
#define PVR_FPU_SHAD_SCALE 0x005f8074u   /* bit 8: intensity shadows; 7-0: their scale / 256 */
#define PVR_FOG_COL_RAM    0x005f80b0u   /* table fog colour, RGB */
#define PVR_FOG_COL_VERT   0x005f80b4u   /* per-vertex fog colour */
#define PVR_FOG_DENSITY    0x005f80b8u   /* 15-8 mantissa / 128, 7-0 signed exponent */
#define PVR_FOG_CLAMP_MAX  0x005f80bcu   /* colour clamp (TSP bit 21), ARGB */
#define PVR_FOG_CLAMP_MIN  0x005f80c0u
#define PVR_FOG_TABLE      0x005f8200u   /* 128 entries: two 8-bit fog values each */
#define PVR_FPU_PARAM_CFG  0x005f807cu   /* bit 21: region array entries are 6 words (type 2) */
#define PVR_ISP_FEED_CFG   0x005f8098u   /* bit 0: translucent list in pre-sort (sent) order, else sorted */
#define PVR_ISP_BACKGND_D  0x005f8088u
#define PVR_ISP_BACKGND_T  0x005f808cu
#define PVR_SPG_CONTROL    0x005f80d0u
#define PVR_TEXT_CONTROL   0x005f80e4u
#define PVR_SCALER_CTL     0x005f80f4u
#define PVR_PAL_RAM_CTRL   0x005f8108u
#define PVR_PT_ALPHA_REF   0x005f811cu
#define PVR_TA_ISP_BASE    0x005f8128u
#define PVR_TA_LIST_INIT   0x005f8144u
#define PVR_TA_LIST_CONT   0x005f8160u
#define PVR_PALETTE        0x005f9000u   /* 1024 entries */

void pvr_init(void);

/* area 4, physical 10000000-13ffffff: TA FIFO, YUV converter, texture path */
int  pvr_ta_handles(uint32_t phys);
void pvr_ta_write(uint32_t phys, uint32_t value, int size);

/* Holly calls this after storing a write to 005f8000+ or SB_C2DST */
void pvr_reg_written(uint32_t address, uint32_t value);

/* how many frames were rendered (STARTRENDER) */
uint32_t pvr_render_count(void);

#endif
