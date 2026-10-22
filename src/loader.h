#ifndef DC_LOADER_H
#define DC_LOADER_H

#include <stddef.h>
#include <stdint.h>

/* Dreamcast main RAM: 16 MB at physical 0x0C000000, normally accessed
 * through the P1 (cached) mirror at 0x8C000000. */
#define DC_RAM_SIZE     0x01000000u
#define DC_RAM_PHYS     0x0C000000u
#define DC_DEFAULT_BASE 0x8C010000u   /* where 1ST_READ.BIN is loaded */

typedef struct {
    uint32_t start, end;   /* [start, end) virtual addresses */
    char name[32];
} dc_region;

typedef struct {
    uint32_t addr, size;
    char *name;
} dc_symbol;

typedef struct {
    uint8_t *ram;          /* DC_RAM_SIZE bytes */
    uint32_t entry;
    dc_region *regions;    /* executable ranges to disassemble */
    int nregions;
    dc_symbol *syms;       /* sorted by addr */
    int nsyms;
    uint32_t *roots;       /* extra function starts (copied code), besides entry */
    int nroots;
} dc_image;

int  dc_load_elf(dc_image *img, const char *path, char *err, size_t errlen);
int  dc_load_bin(dc_image *img, const char *path, uint32_t base, char *err, size_t errlen);
void dc_image_free(dc_image *img);

/* Code the game copies somewhere else at runtime (e.g. exception stubs into
 * the vector area): put `len` bytes from `src` at `dest` too, so they get
 * analysed and recompiled at the address they run from. */
int  dc_image_copy(dc_image *img, uint32_t dest, uint32_t src, uint32_t len, char *err, size_t errlen);

/* Another known function start (a module's entry points). */
int  dc_image_add_root(dc_image *img, uint32_t addr);

/* Translate a virtual address to an offset into img->ram. Returns 0 if
 * the address is not in main RAM. */
int dc_ram_offset(uint32_t addr, uint32_t *off);

int dc_read16(const dc_image *img, uint32_t addr, uint16_t *out);
int dc_read32(const dc_image *img, uint32_t addr, uint32_t *out);

/* Name of a symbol starting exactly at addr, or NULL. */
const char *dc_symbol_at(const dc_image *img, uint32_t addr);

#endif
