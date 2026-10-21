#include "memory.h"

#include <stdio.h>
#include <string.h>

uint8_t dc_ram[DC_RAM_SIZE];
uint32_t mem_last_unmapped;

void mem_init(void)
{
    memset(dc_ram, 0, sizeof dc_ram);
}

static int ram_offset(uint32_t address, size_t length, size_t *offset)
{
    uint32_t segment = address & 0xfc000000u;
    if (segment != 0x0c000000u && segment != 0x8c000000u &&
        segment != 0xac000000u)
        return 0;

    // area 3 is 64 MiB but RAM is 16 MiB, mirrored 4 times (0x8c/8d/8e/8f...)
    uint32_t physical = address & (DC_RAM_SIZE - 1);
    if (length > DC_RAM_SIZE - (size_t)physical)
        return 0;
    *offset = (size_t)physical;
    return 1;
}

uint8_t *mem_ptr(uint32_t address, size_t length)
{
    size_t offset;
    return ram_offset(address, length, &offset) ? &dc_ram[offset] : NULL;
}

int mem_load_bin(const char *path, uint32_t address, char *err, size_t err_size)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        if (err && err_size) snprintf(err, err_size, "cannot open '%s'", path);
        return 0;
    }

    if (fseek(fp, 0, SEEK_END) != 0) {
        if (err && err_size) snprintf(err, err_size, "cannot seek '%s'", path);
        fclose(fp);
        return 0;
    }
    long end = ftell(fp);
    if (end < 0 || fseek(fp, 0, SEEK_SET) != 0) {
        if (err && err_size) snprintf(err, err_size, "cannot determine size of '%s'", path);
        fclose(fp);
        return 0;
    }

    uint8_t *dst = mem_ptr(address, (size_t)end);
    if (!dst) {
        if (err && err_size) snprintf(err, err_size,
            "image (%ld bytes at 0x%08x) does not fit in main RAM", end, address);
        fclose(fp);
        return 0;
    }
    size_t got = fread(dst, 1, (size_t)end, fp);
    int failed = ferror(fp) || got != (size_t)end;
    fclose(fp);
    if (failed) {
        if (err && err_size) snprintf(err, err_size, "failed reading '%s'", path);
        return 0;
    }
    return 1;
}

static void log_unmapped(const char *op, uint32_t address, uint32_t value, int writing)
{
    // games poll hardware in tight loops, don't drown the console
    static unsigned logged;
    mem_last_unmapped = address;
    if (logged >= 500) return;
    if (++logged == 500) {
        fprintf(stderr, "[hw] ... more unimplemented accesses, not logging them\n");
        return;
    }

    if (writing)
            fprintf(stderr, "[hw] %s 0x%08x <- 0x%x (unimplemented)\n", op, address, value);
        else
            fprintf(stderr, "[hw] %s 0x%08x (unimplemented; returning all-ones)\n", op, address);
}

uint8_t mem_r8(uint32_t address)
{
    size_t o;
    if (ram_offset(address, 1, &o)) return dc_ram[o];
        log_unmapped("read8", address, 0, 0);
        return 0xff;
} 

uint16_t mem_r16(uint32_t address) 
{
    size_t o;
    if (ram_offset(address, 2, &o))
        return (uint16_t)(dc_ram[o] | ((uint16_t)dc_ram[o + 1] << 8));
    log_unmapped("read16", address, 0, 0);
    return 0xffff;
}   

uint32_t mem_r32(uint32_t address)
{
    size_t o;
    if (ram_offset(address, 4, &o))
            return (uint32_t)dc_ram[o] | ((uint32_t)dc_ram[o + 1] << 8) |
               ((uint32_t)dc_ram[o + 2] << 16) | ((uint32_t)dc_ram[o + 3] << 24);
    log_unmapped("read32", address, 0, 0);
    return 0xffffffffu;
}

void mem_w8(uint32_t address, uint8_t value)
{
    size_t o;
    if (ram_offset(address, 1, &o)) {dc_ram[o] = value; return;}
    log_unmapped("write8", address, value, 1);
}

void mem_w16(uint32_t address, uint16_t value)
{
    size_t o;
    if (ram_offset(address, 2, &o)) {
        dc_ram[o] = (uint8_t)value;
        dc_ram[o + 1] = (uint8_t)(value >> 8);
        return;
    }

    log_unmapped("write16", address, value, 1);
}

void mem_w32(uint32_t address, uint32_t value)
{
    size_t o;
    if (ram_offset(address, 4, &o)) {
        dc_ram[o] = (uint8_t)value;
        dc_ram[o + 1] = (uint8_t)(value >> 8);
        dc_ram[o + 2] = (uint8_t)(value >> 16);
        dc_ram[o + 3] = (uint8_t)(value >> 24);
        return;
    }
    log_unmapped("write32", address, value, 1);
}

// 64-bit: two 32-bit halves, low word first (little endian)
uint64_t mem_r64(uint32_t address)
{
    return (uint64_t)mem_r32(address) | ((uint64_t)mem_r32(address + 4) << 32);
}

void mem_w64(uint32_t address, uint64_t value)
{
    mem_w32(address, (uint32_t)value);
    mem_w32(address + 4, (uint32_t)(value >> 32));
}