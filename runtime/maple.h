#ifndef DC_RUNTIME_MAPLE_H
#define DC_RUNTIME_MAPLE_H

/* Maple bus: a controller on port A with a VMU in its slot 1 (see MAPLE.md).
 *   DC_VMU=file      the VMU's flash image (default vmu_a1.bin, made if missing)
 *   DC_MAPLELOG=n    print the first n frames answered */

#define MAPLE_SB_MDSTAR 0x005f6c04u   /* command list address */
#define MAPLE_SB_MDTSEL 0x005f6c10u   /* bit 0: start every vblank instead of on SB_MDST */
#define MAPLE_SB_MDEN   0x005f6c14u
#define MAPLE_SB_MDST   0x005f6c18u   /* write 1: start; reads 1 while busy */

void maple_init(void);
void maple_dma(void);   /* run the command list now */

#endif
