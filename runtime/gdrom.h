#ifndef DC_RUNTIME_GDROM_H
#define DC_RUNTIME_GDROM_H

#include <stdint.h>

/* A GD-ROM image (Redump-style .cue with one .bin per track).
 * Sectors are addressed by FAD: LBA + 150. The high-density area, where
 * the game's files are, starts at LBA 45000. */

int gd_open(const char *cue_path, char *err, int err_size);
int gd_is_open(void);

/* Read `count` 2048-byte data sectors starting at `fad` into emulated
 * memory at `dest`. Returns sectors read. */
uint32_t gd_read(uint32_t fad, uint32_t count, uint32_t dest);

/* The 102-word TOC the BIOS hands out: 99 track entries
 * (ctrl << 28 | adr << 24 | fad, 0xffffffff if unused), first track,
 * last track (ctrl/adr << 24 | track << 16) and the lead-out FAD.
 * area 0 = single density, 1 = high density. */
void gd_toc(int area, uint32_t toc[102]);

#endif
