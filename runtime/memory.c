#include "memory.h"
#include "aica.h"
#include "holly.h"

#include <stdio.h>
#include <string.h>

uint8_t dc_ram[DC_RAM_SIZE];
uint8_t dc_vram[DC_VRAM_SIZE];
uint8_t dc_aram[DC_ARAM_SIZE];
uint32_t mem_last_unmapped;

/* Sound RAM: physical 00800000-00ffffff, 2 MiB mirrored. */
static int aram_offset(uint32_t address, size_t length, size_t *offset)
{
    if (address >= 0xe0000000u) return 0;
    uint32_t phys = address & 0x1fffffffu;
    if (phys < 0x00800000u || phys >= 0x01000000u) return 0;
    uint32_t a = phys & (DC_ARAM_SIZE - 1);
    if (length > DC_ARAM_SIZE - a) return 0;
    *offset = a;
    return 1;
}

/* Video RAM is area 1 (physical 04000000-07ffffff). 05/07 is the 32-bit
   path: linear. 04/06 is the 64-bit path: the two 4 MiB banks interleaved
   every 32 bits (same mapping as Flycast). Accesses never cross a word. */
static int vram_offset(uint32_t address, size_t *offset)
{
    if (address >= 0xe0000000u) return 0;
    uint32_t phys = address & 0x1fffffffu;
    if ((phys >> 26) != 1) return 0;

    uint32_t a = phys & (DC_VRAM_SIZE - 1);
    if (!(phys & 0x01000000u))
        a = (a & 3u) | ((a >> 1) & 0x3ffffcu) | (((a >> 2) & 1u) << 22);
    *offset = a;
    return 1;
}

/* SH-4 store queues: two 32-byte buffers written through e0000000-e3ffffff
   (address bit 5 picks the queue). pref sends one out in a single burst, to
   the area in QACR0/QACR1 plus the low address bits. Used for the TA FIFO
   and for fast copies/fills. */
static uint32_t store_queue[2][8];

void mem_sq_flush(uint32_t address)
{
    unsigned q = (address >> 5) & 1u;
    uint32_t qacr = sh4reg_read32(q ? 0xff00003cu : 0xff000038u);
    uint32_t dest = ((qacr & 0x1cu) << 24) | (address & 0x03ffffe0u);

    for (unsigned i = 0; i < 8; i++)
        mem_w32(dest + i * 4u, store_queue[q][i]);
}

void mem_init(void)
{
    memset(dc_ram, 0, sizeof dc_ram);
    holly_init();
    aica_init();
}

/* P0/P1/P2 aliases of the 0x0c000000 main-RAM window map to the
   same store. */
static int ram_offset(uint32_t address, size_t length, size_t *offset)
{
    uint32_t segment = address & 0xfc000000u;

    if (segment != 0x0c000000u
        && segment != 0x8c000000u
        && segment != 0xac000000u)
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

    return ram_offset(address, length, &offset)
        ? &dc_ram[offset]
        : NULL;
}

int mem_load_bin(const char *path, uint32_t address,
                 char *err, size_t err_size)
{
    FILE *fp = fopen(path, "rb");

    if (!fp) {
        if (err && err_size)
            snprintf(err, err_size, "cannot open '%s'", path);
        return 0;
    }

    if (fseek(fp, 0, SEEK_END) != 0) {
        if (err && err_size)
            snprintf(err, err_size, "cannot seek '%s'", path);

        fclose(fp);
        return 0;
    }

    long end = ftell(fp);

    if (end < 0 || fseek(fp, 0, SEEK_SET) != 0) {
        if (err && err_size)
            snprintf(err, err_size,
                     "cannot determine size of '%s'", path);

        fclose(fp);
        return 0;
    }

    uint8_t *dst = mem_ptr(address, (size_t)end);

    if (!dst) {
        if (err && err_size) {
            snprintf(err, err_size,
                     "image (%ld bytes at 0x%08x) "
                     "does not fit in main RAM",
                     end, address);
        }

        fclose(fp);
        return 0;
    }

    size_t got = fread(dst, 1, (size_t)end, fp);
    int failed = ferror(fp) || got != (size_t)end;

    fclose(fp);

    if (failed) {
        if (err && err_size)
            snprintf(err, err_size, "failed reading '%s'", path);

        return 0;
    }

    return 1;
}

/* Holly is reached through P0/P1/P2 (the game uses 0xa05f....), so compare
   its physical address. The SH-4's own registers (P4, 0xff......) aren't
   aliased and stay as they are. */
static uint32_t reg_address(uint32_t address)
{
    return address >= 0xe0000000u ? address : (address & 0x1fffffffu);
}

static int is_register(uint32_t address)
{
    return holly_handles(reg_address(address)) || aica_handles(reg_address(address))
        || sh4reg_handles(address);
}

static uint32_t register_read32(uint32_t address)
{
    if (holly_handles(reg_address(address)))
        return holly_read32(reg_address(address));
    if (aica_handles(reg_address(address)))
        return aica_read32(reg_address(address));

    return sh4reg_read32(address);
}

static void register_write32(uint32_t address, uint32_t value)
{
    if (holly_handles(reg_address(address)))
        holly_write32(reg_address(address), value);
    else if (aica_handles(reg_address(address)))
        aica_write32(reg_address(address), value);
    else
        sh4reg_write32(address, value);
}

static void log_unmapped(const char *op, uint32_t address,
                         uint32_t value, int writing)
{
    // games poll hardware in tight loops, don't drown the console
    static unsigned logged;

    // SH-4 cache / TLB address arrays (f0000000-feffffff): only used to flush
    // caches, which we don't have. Ignore without logging
    if (address >= 0xf0000000u && address < 0xff000000u)
        return;

    mem_last_unmapped = address;
    if (logged >= 500)
        return;
    if (++logged == 500) {
        fprintf(stderr, "[hw] ... more unimplemented accesses, not logging them\n");
        return;
    }

    if (writing) {
        fprintf(stderr,
                "[hw] %s 0x%08x <- 0x%x (unimplemented)\n",
                op, address, value);
    } else {
        fprintf(stderr,
                "[hw] %s 0x%08x "
                "(unimplemented; returning all-ones)\n",
                op, address);
    }
}

uint8_t mem_r8(uint32_t address)
{
    size_t o;

    if (ram_offset(address, 1, &o))
        return dc_ram[o];

    if (vram_offset(address, &o))
        return dc_vram[o];

    if (aram_offset(address, 1, &o))
        return dc_aram[o];

    if (is_register(address)) {
        return (uint8_t)(
            register_read32(address & ~3u)
            >> ((address & 3u) * 8u)
        );
    }

    log_unmapped("read8", address, 0, 0);
    return 0xff;
}

uint16_t mem_r16(uint32_t address)
{
    size_t o;

    if (ram_offset(address, 2, &o)) {
        return (uint16_t)(
            dc_ram[o] | ((uint16_t)dc_ram[o + 1] << 8)
        );
    }

    if (vram_offset(address, &o))
        return (uint16_t)(dc_vram[o] | ((uint16_t)dc_vram[o + 1] << 8));

    if (aram_offset(address, 2, &o))
        return (uint16_t)(dc_aram[o] | ((uint16_t)dc_aram[o + 1] << 8));

    if (is_register(address)) {
        return (uint16_t)(
            register_read32(address & ~3u)
            >> ((address & 2u) * 8u)
        );
    }

    log_unmapped("read16", address, 0, 0);
    return 0xffff;
}

uint32_t mem_r32(uint32_t address)
{
    size_t o;

    if (ram_offset(address, 4, &o)) {
        return (uint32_t)dc_ram[o]
             | ((uint32_t)dc_ram[o + 1] << 8)
             | ((uint32_t)dc_ram[o + 2] << 16)
             | ((uint32_t)dc_ram[o + 3] << 24);
    }

    if (vram_offset(address, &o)) {
        uint32_t v;
        memcpy(&v, &dc_vram[o], 4);
        return v;
    }

    if (aram_offset(address, 4, &o)) {
        uint32_t v;
        memcpy(&v, &dc_aram[o], 4);
        return v;
    }

    if (is_register(address))
        return register_read32(address & ~3u);

    log_unmapped("read32", address, 0, 0);
    return 0xffffffffu;
}

void mem_w8(uint32_t address, uint8_t value)
{
    size_t o;

    if (ram_offset(address, 1, &o)) {
        dc_ram[o] = value;
        return;
    }

    if (vram_offset(address, &o)) {
        dc_vram[o] = value;
        return;
    }

    if (aram_offset(address, 1, &o)) {
        dc_aram[o] = value;
        return;
    }

    if (is_register(address)) {
        uint32_t a = address & ~3u;
        uint32_t shift = (address & 3u) * 8u;

        if (reg_address(a) == HOLLY_SB_ISTNRM || reg_address(a) == HOLLY_SB_ISTERR) {
            register_write32(a, (uint32_t)value << shift);
        } else {
            uint32_t old = register_read32(a);

            register_write32(
                a,
                (old & ~(0xffu << shift))
                | ((uint32_t)value << shift)
            );
        }

        return;
    }

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

    if (vram_offset(address, &o)) {
        memcpy(&dc_vram[o], &value, 2);
        return;
    }

    if (aram_offset(address, 2, &o)) {
        memcpy(&dc_aram[o], &value, 2);
        return;
    }

    if (is_register(address)) {
        uint32_t a = address & ~3u;
        uint32_t shift = (address & 2u) * 8u;

        if (reg_address(a) == HOLLY_SB_ISTNRM || reg_address(a) == HOLLY_SB_ISTERR) {
            register_write32(a, (uint32_t)value << shift);
        } else {
            uint32_t old = register_read32(a);

            register_write32(
                a,
                (old & ~(0xffffu << shift))
                | ((uint32_t)value << shift)
            );
        }

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

    if (vram_offset(address, &o)) {
        memcpy(&dc_vram[o], &value, 4);
        return;
    }

    if (aram_offset(address, 4, &o)) {
        memcpy(&dc_aram[o], &value, 4);
        return;
    }

    if ((address & 0xfc000000u) == 0xe0000000u) {
        store_queue[(address >> 5) & 1u][(address >> 2) & 7u] = value;
        return;
    }

    if (is_register(address)) {
        register_write32(address & ~3u, value);
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
