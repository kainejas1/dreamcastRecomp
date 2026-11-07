#ifndef DC_RUNTIME_HOLLY_H
#define DC_RUNTIME_HOLLY_H

#include <stdint.h>

/* Dreamcast system-bus register window and SH-4 P4 register window. */
#define HOLLY_REG_BASE       0x005f6800u
#define HOLLY_REG_END        0x005fa000u
#define HOLLY_SB_ISTNRM      0x005f6900u
#define HOLLY_SB_ISTEXT      0x005f6904u
#define HOLLY_SB_ISTERR      0x005f6908u
#define HOLLY_SB_IML2NRM     0x005f6910u
#define HOLLY_SB_IML2EXT     0x005f6914u
#define HOLLY_SB_IML2ERR     0x005f6918u
#define HOLLY_SB_IML4NRM     0x005f6920u
#define HOLLY_SB_IML4EXT     0x005f6924u
#define HOLLY_SB_IML4ERR     0x005f6928u
#define HOLLY_SB_IML6NRM     0x005f6930u
#define HOLLY_SB_IML6EXT     0x005f6934u
#define HOLLY_SB_IML6ERR     0x005f6938u
#define HOLLY_SPG_VBLANK_INT 0x005f80ccu
#define HOLLY_SPG_LOAD       0x005f80d8u
#define HOLLY_SPG_VBLANK     0x005f80dcu
#define HOLLY_SPG_STATUS     0x005f810cu

void holly_init(void);
int holly_handles(uint32_t address);
uint32_t holly_read32(uint32_t address);
void holly_write32(uint32_t address, uint32_t value);

/* SH-4 internal registers use the same generic register-file semantics. */
int sh4reg_handles(uint32_t address);
uint32_t sh4reg_read32(uint32_t address);
void sh4reg_write32(uint32_t address, uint32_t value);

/* Host-independent hooks useful to tests and future IRQ integration. */
uint32_t holly_scanline(void);
uint32_t holly_vcount(void);
uint32_t holly_status_pending(void);

/* Highest Holly interrupt level (6, 4 or 2) that is raised and enabled by
 * the SB_IML* masks, 0 if none. Also advances the video clock. */
int holly_pending_level(void);

/* Number of vblank-ins so far (frames). */
uint32_t holly_frame_count(void);

/* Microseconds since start, on the video clock. */
uint64_t holly_clock_us(void);

/* Raw register value, no side effects (for the PowerVR code). */
uint32_t holly_reg(uint32_t address);
void holly_set_reg(uint32_t address, uint32_t value);
/* The same for a plain SH-4 register (not the timers), not logged. */
uint32_t sh4_reg(uint32_t address);
/* Set bits in SB_ISTNRM (an interrupt happened: list done, DMA end, ...). */
void holly_raise(uint32_t bits);

#endif