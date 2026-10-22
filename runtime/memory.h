#ifndef DC_RUNTIME_MEMORY_H
#define DC_RUNTIME_MEMORY_H

#include <stdint.h>
#include <stddef.h>

#define DC_RAM_SIZE (16u * 1024u * 1024u)
#define DC_RAM_LOAD_BASE 0x8c010000u

/* Backing store for the Dreamcast's 16 MiB main RAM. */
extern uint8_t dc_ram[DC_RAM_SIZE];

/* 8 MiB video RAM, in its linear (32-bit path) layout. */
#define DC_VRAM_SIZE (8u * 1024u * 1024u)
extern uint8_t dc_vram[DC_VRAM_SIZE];

/* 2 MiB sound RAM (the AICA's, where its ARM7 runs the sound driver). */
#define DC_ARAM_SIZE (2u * 1024u * 1024u)
extern uint8_t dc_aram[DC_ARAM_SIZE];

/* Address of the most recent access that hit unimplemented hardware. */
extern uint32_t mem_last_unmapped;

void mem_init(void);
int mem_load_bin(const char *path, uint32_t address,
                 char *err, size_t err_size);

uint8_t  mem_r8(uint32_t address);
uint16_t mem_r16(uint32_t address);
uint32_t mem_r32(uint32_t address);
uint64_t mem_r64(uint32_t address);

void mem_w8(uint32_t address, uint8_t value);
void mem_w16(uint32_t address, uint16_t value);
void mem_w32(uint32_t address, uint32_t value);
void mem_w64(uint32_t address, uint64_t value);

/* pref on a store queue address (e0000000-e3ffffff): send that queue out. */
void mem_sq_flush(uint32_t address);

/* Returns a host pointer only for addresses backed by main RAM. */
uint8_t *mem_ptr(uint32_t address, size_t length);

#endif