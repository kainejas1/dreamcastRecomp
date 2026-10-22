/*
 * High-level replacement for the Dreamcast boot ROM's system calls.
 * Calling conventions and behaviour follow Flycast's reios (HLE BIOS).
 *
 *   8c0000b0  SYSINFO   function in r7
 *   8c0000b4  ROMFONT   function in r1
 *   8c0000b8  FLASHROM  function in r7
 *   8c0000bc  GD-ROM    r6 = 0: GD-ROM call, function in r7; else misc
 *   8c0000c0  GD-ROM 2  same
 *   8c0000e0  MISC      function in r4
 */

#include "bios.h"
#include "gdrom.h"
#include "memory.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// where the vectors point. Nothing is ever executed there, dc_call/dc_jump
// check for these first (same addresses reios uses)
#define HOOK_SYSINFO  0x8c001000u
#define HOOK_ROMFONT  0x8c001002u
#define HOOK_FLASHROM 0x8c001004u
#define HOOK_GDROM    0x8c001006u
#define HOOK_MISC     0x8c001008u
#define HOOK_GDROM2   0x8c0010f0u

#define SYSINFO_BLOCK 0x8c000068u   // 24 bytes: console id, region/language/tv

static int verbose_calls = 40;   // log the first N syscalls

static void trace(const char *fmt, ...)
{
    if (verbose_calls <= 0) return;
    verbose_calls--;
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[bios] ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

// ---- flash ROM (128 KiB, 5 partitions) ----

static uint8_t flash[0x20000];

static const struct { uint32_t offset, size; } flash_part[5] = {
    { 0x1a000, 0x02000 },   // 0: factory settings (region, console id)
    { 0x18000, 0x02000 },   // 1: reserved
    { 0x1c000, 0x04000 },   // 2: block allocated (system settings)
    { 0x10000, 0x08000 },   // 3: block allocated (game settings)
    { 0x00000, 0x10000 },   // 4: block allocated
};

static void flash_init(void)
{
    memset(flash, 0xff, sizeof flash);
    // factory partition: '0' '0' region language broadcast
    // region 0 Japan / 1 USA / 2 Europe, language 1 = English, broadcast 0 = NTSC
    static const char factory[5] = { '0', '0', '1', '1', '0' };
    memcpy(&flash[0x1a000], factory, 5);
    memcpy(&flash[0x1a0a0], factory, 5);   // mirror copy

    // console id at 1a058 (6 bytes), preceded by its checksum and inverted checksum
    static const uint8_t id[6] = { 0x44, 0x43, 0x52, 0x45, 0x43, 0x01 };
    uint8_t sum = 0;
    for (int i = 0; i < 6; i++) sum += id[i];
    flash[0x1a056] = (uint8_t)~sum;
    flash[0x1a057] = sum;
    memcpy(&flash[0x1a058], id, 6);
    flash[0x1a05f] = 0xfe;   // machine version VA1
}

static void sys_flashrom(sh4_ctx *ctx)
{
    uint32_t fn = ctx->r[7];
    switch (fn) {
    case 0: {   // INFO(part r4, dest r5): offset, size
        uint32_t part = ctx->r[4];
        trace("FLASHROM_INFO part %u", part);
        if (part < 5) {
            mem_w32(ctx->r[5], flash_part[part].offset);
            mem_w32(ctx->r[5] + 4, flash_part[part].size);
            ctx->r[0] = 0;
        } else {
            ctx->r[0] = (uint32_t)-1;
        }
        break;
    }
    case 1: {   // READ(offset r4, dest r5, size r6)
        uint32_t off = ctx->r[4], n = ctx->r[6];
        trace("FLASHROM_READ %05x, %u bytes -> %08x", off, n, ctx->r[5]);
        for (uint32_t i = 0; i < n; i++)
            mem_w8(ctx->r[5] + i, off + i < sizeof flash ? flash[off + i] : 0xff);
        ctx->r[0] = 0;
        break;
    }
    case 2: {   // WRITE(offset r4, src r5, size r6): flash bits only go 1 -> 0
        uint32_t off = ctx->r[4], n = ctx->r[6];
        trace("FLASHROM_WRITE %05x, %u bytes", off, n);
        for (uint32_t i = 0; i < n && off + i < sizeof flash; i++)
            flash[off + i] &= mem_r8(ctx->r[5] + i);
        ctx->r[0] = n;
        break;
    }
    case 3: {   // DELETE(partition offset r4): erase back to 0xff
        uint32_t off = ctx->r[4];
        trace("FLASHROM_DELETE %05x", off);
        ctx->r[0] = (uint32_t)-1;
        for (int p = 0; p < 5; p++) {
            if (flash_part[p].offset == off) {
                memset(&flash[off], 0xff, flash_part[p].size);
                ctx->r[0] = 0;
            }
        }
        break;
    }
    default:
        trace("FLASHROM function %u?", fn);
        ctx->r[0] = (uint32_t)-1;
        break;
    }
}

// ---- SYSINFO, ROMFONT, MISC ----

static void sys_sysinfo(sh4_ctx *ctx)
{
    switch (ctx->r[7]) {
    case 0:   // INIT: console id + factory settings into RAM
        trace("SYSINFO_INIT");
        for (int i = 0; i < 8; i++) mem_w8(SYSINFO_BLOCK + i, flash[0x1a056 + i]);
        for (int i = 0; i < 5; i++) mem_w8(SYSINFO_BLOCK + 8 + i, flash[0x1a000 + i]);
        for (int i = 13; i < 24; i++) mem_w8(SYSINFO_BLOCK + i, 0);
        ctx->r[0] = 0;
        break;
    case 2:   // ICON: size of the icon (no icon data, like reios)
        trace("SYSINFO_ICON %u", ctx->r[4]);
        ctx->r[0] = ctx->r[4] > 9 ? (uint32_t)-1 : 704;
        break;
    case 3:   // ID: where the block is
        trace("SYSINFO_ID");
        ctx->r[0] = SYSINFO_BLOCK;
        break;
    default:
        trace("SYSINFO function %u?", ctx->r[7]);
        ctx->r[0] = (uint32_t)-1;
        break;
    }
}

static void sys_romfont(sh4_ctx *ctx)
{
    switch (ctx->r[1]) {
    case 0: trace("ROMFONT_ADDRESS"); ctx->r[0] = 0xa0100020u; break;   // no font data yet
    case 1: trace("ROMFONT_LOCK"); ctx->r[0] = 0; break;
    case 2: trace("ROMFONT_UNLOCK"); ctx->r[0] = 0; break;
    default: trace("ROMFONT function %u?", ctx->r[1]); break;
    }
}

static void sys_misc(sh4_ctx *ctx)
{
    switch (ctx->r[4]) {
    case 0: trace("MISC init"); ctx->r[0] = 0; break;
    case 1:
        fprintf(stderr, "[bios] game asked to exit to the BIOS menu (pr=%08x r5=%08x r6=%08x r7=%08x)\n",
                ctx->pr, ctx->r[5], ctx->r[6], ctx->r[7]);
        exit(0);
    case 2: trace("MISC check disc"); ctx->r[0] = gd_is_open() ? 0 : (uint32_t)-1; break;
    case 3:
        fprintf(stderr, "[bios] game asked to exit to the CD player\n");
        exit(0);
    default: trace("MISC function %u?", ctx->r[4]); break;
    }
}

// ---- GD-ROM ----
// Commands go: REQ_CMD (queue, get an id) -> EXEC_SERVER (run it) ->
// GET_CMD_STAT (poll; reports COMPLETE once, then OK). Reads finish
// immediately inside EXEC_SERVER for now.

enum { GDC_ERR = -1, GDC_OK, GDC_BUSY, GDC_COMPLETE, GDC_CONTINUE };
enum { GD_STAT_BUSY, GD_STAT_PAUSE, GD_STAT_STANDBY, GD_STAT_PLAY, GD_STAT_SEEK,
       GD_STAT_SCAN, GD_STAT_OPEN, GD_STAT_NODISC };
enum { CMD_PIOREAD = 16, CMD_DMAREAD, CMD_GETTOC, CMD_GETTOC2, CMD_PLAY, CMD_PLAY2,
       CMD_PAUSE, CMD_RELEASE, CMD_INIT, CMD_READABORT, CMD_OPEN, CMD_SEEK,
       CMD_DMA_READ_REQ, CMD_GETQINFO, CMD_REQ_MODE, CMD_SET_MODE, CMD_SCAN, CMD_STOP,
       CMD_GETSCD, CMD_REQ_SES, CMD_REQ_STAT, CMD_PIOREADREQ, CMD_MULTI_DMAREAD,
       CMD_MULTI_PIOREAD, CMD_GET_VERSION };

static struct {
    int status;
    uint32_t next_id, last_id;
    uint32_t cmd, params[4];
    uint32_t result[4];
    uint32_t drive;
    uint32_t sec_mode[4];
    uint32_t dma_end_cb, dma_end_arg;
} gd;

static void gd_reset(void)
{
    gd.status = GDC_OK;
    gd.last_id = 0;
    memset(gd.result, 0, sizeof gd.result);
    gd.drive = gd_is_open() ? GD_STAT_PAUSE : GD_STAT_NODISC;
}

static void gd_command(void)
{
    uint32_t *p = gd.params;
    memset(gd.result, 0, sizeof gd.result);

    switch (gd.cmd) {
    case CMD_INIT:
        trace("GD INIT");
        gd.drive = GD_STAT_PAUSE;
        break;
    case CMD_GETTOC:
    case CMD_GETTOC2: {   // area p[0], dest p[1]
        uint32_t toc[102];
        trace("GD GETTOC%s area %u -> %08x", gd.cmd == CMD_GETTOC2 ? "2" : "", p[0], p[1]);
        gd_toc(p[0] & 1, toc);
        for (int i = 0; i < 102; i++) mem_w32(p[1] + 4 * i, toc[i]);
        break;
    }
    case CMD_PIOREAD:
    case CMD_DMAREAD: {   // fad p[0], count p[1], dest p[2]
        uint32_t fad = p[0] & 0xffffff, n = p[1];
        trace("GD %s FAD %u x%u -> %08x", gd.cmd == CMD_PIOREAD ? "PIOREAD" : "DMAREAD", fad, n, p[2]);
        uint32_t got = gd_read(fad, n, p[2]);
        gd.result[2] = got * 2048;
        if (got != n) { gd.status = GDC_ERR; return; }
        gd.drive = GD_STAT_PAUSE;
        break;
    }
    case CMD_REQ_MODE:   // dest p[0]: 10 bytes of drive mode, all defaults
        trace("GD REQ_MODE");
        for (int i = 0; i < 10; i++) mem_w8(p[0] + i, 0);
        gd.result[2] = 0xa;
        break;
    case CMD_SET_MODE:
        trace("GD SET_MODE");
        gd.result[2] = 0xa;
        break;
    case CMD_GET_VERSION: {
        static const char ver[] = "GDC Version 1.10 1999-03-31 ";
        trace("GD GET_VERSION");
        for (int i = 0; i < (int)sizeof ver - 1; i++) mem_w8(p[0] + i, (uint8_t)ver[i]);
        mem_w8(p[0] + sizeof ver - 2, 0x02);
        break;
    }
    case CMD_REQ_STAT:   // drive status, track, fad...
        trace("GD REQ_STAT");
        mem_w32(p[0], gd.drive);
        mem_w32(p[1], 0);
        mem_w32(p[2], 0);
        mem_w32(p[3], 0);
        break;
    case CMD_PAUSE: case CMD_RELEASE: case CMD_STOP: case CMD_SEEK:
        trace("GD command %u (drive state only)", gd.cmd);
        gd.drive = GD_STAT_PAUSE;
        break;
    default:
        fprintf(stderr, "[bios] GD command %u not implemented, pretending it worked\n", gd.cmd);
        break;
    }
    gd.status = GDC_COMPLETE;
}

static void sys_gdrom(sh4_ctx *ctx)
{
    if (ctx->r[6] != 0) {   // misc calls through the GD vector
        trace("GD-vector misc function %u", ctx->r[7]);
        ctx->r[0] = 0;
        return;
    }

    switch (ctx->r[7]) {
    case 0:   // REQ_CMD(cmd r4, params r5) -> request id
        if (gd.status != GDC_OK) { ctx->r[0] = 0; break; }
        gd.cmd = ctx->r[4];
        for (int i = 0; i < 4; i++) gd.params[i] = ctx->r[5] ? mem_r32(ctx->r[5] + 4 * i) : 0;
        if (++gd.next_id < 2 || gd.next_id == 0xffffffffu) gd.next_id = 2;   // ids start at 2, skip 0 and ~0
        gd.last_id = gd.next_id;
        gd.status = GDC_BUSY;
        ctx->r[0] = gd.last_id;
        break;
    case 1: {   // GET_CMD_STAT(id r4, result r5)
        for (int i = 0; i < 4; i++) mem_w32(ctx->r[5] + 4 * i, gd.result[i]);
        if (gd.status == GDC_OK || gd.status == GDC_BUSY) { ctx->r[0] = (uint32_t)gd.status; break; }
        if (ctx->r[4] != gd.last_id) { ctx->r[0] = GDC_OK; break; }
        ctx->r[0] = (uint32_t)gd.status;
        gd.status = GDC_OK;   // reported once
        gd.last_id = 0;
        break;
    }
    case 2:   // EXEC_SERVER: run the queued command
        if (gd.status == GDC_BUSY) gd_command();
        break;
    case 3:   // INIT_SYSTEM
        trace("GD INIT_SYSTEM");
        gd_reset();
        break;
    case 4:   // GET_DRV_STAT(dest r4): drive status, disc type
        mem_w32(ctx->r[4], gd.drive);
        mem_w32(ctx->r[4] + 4, gd_is_open() ? 0x80 : 0);   // 0x80 = GD-ROM
        ctx->r[0] = 0;
        break;
    case 5:   // G1_DMA_END(callback r4, arg r5): reads finish at once, keep it
        trace("GD G1_DMA_END callback %08x", ctx->r[4]);
        gd.dma_end_cb = ctx->r[4];
        gd.dma_end_arg = ctx->r[5];
        ctx->r[0] = 0;
        break;
    case 8:   // READ_ABORT
        trace("GD READ_ABORT");
        ctx->r[0] = 0;
        break;
    case 9:   // RESET
        trace("GD RESET");
        gd.status = GDC_OK;
        gd.last_id = 0;
        break;
    case 10:  // CHANGE_DATA_TYPE(4 words at r4)
        for (int i = 0; i < 4; i++) gd.sec_mode[i] = mem_r32(ctx->r[4] + 4 * i);
        trace("GD CHANGE_DATA_TYPE %u %u %u %u", gd.sec_mode[0], gd.sec_mode[1], gd.sec_mode[2], gd.sec_mode[3]);
        ctx->r[0] = 0;
        break;
    default:
        fprintf(stderr, "[bios] GD-ROM syscall %u not implemented\n", ctx->r[7]);
        ctx->r[0] = 0;
        break;
    }
}

// ---- setup / dispatch ----

void bios_init(void)
{
    // the boot ROM leaves 8c000000-8c00ffff as 0xff apart from what it sets
    for (uint32_t a = 0x8c000000u; a < 0x8c010000u; a += 4) mem_w32(a, 0xffffffffu);

    mem_w32(0x8c0000b0u, HOOK_SYSINFO);
    mem_w32(0x8c0000b4u, HOOK_ROMFONT);
    mem_w32(0x8c0000b8u, HOOK_FLASHROM);
    mem_w32(0x8c0000bcu, HOOK_GDROM);
    mem_w32(0x8c0000c0u, HOOK_GDROM2);
    mem_w32(0x8c0000e0u, HOOK_MISC);

    flash_init();
    gd_reset();
}

int bios_call(sh4_ctx *ctx, uint32_t addr)
{
    switch ((addr & 0x1fffffffu) | 0x80000000u) {
    case HOOK_SYSINFO:  sys_sysinfo(ctx); return 1;
    case HOOK_ROMFONT:  sys_romfont(ctx); return 1;
    case HOOK_FLASHROM: sys_flashrom(ctx); return 1;
    case HOOK_GDROM:
    case HOOK_GDROM2:   sys_gdrom(ctx); return 1;
    case HOOK_MISC:     sys_misc(ctx); return 1;
    default: return 0;
    }
}
