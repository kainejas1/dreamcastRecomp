#include "maple.h"
#include "holly.h"
#include "memory.h"
#include "video.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// function codes
#define FN_CONTROLLER 0x01000000u
#define FN_STORAGE    0x02000000u
#define FN_LCD        0x04000000u
#define FN_CLOCK      0x08000000u

// replies
#define R_DEVICE_INFO   0x05u
#define R_ALL_STATUS    0x06u
#define R_DEVICE_REPLY  0x07u
#define R_DATA_TRANSFER 0x08u
#define R_BAD_FUNCTION  0xfeu
#define R_UNKNOWN_CMD   0xfdu

enum { NONE, CONTROLLER, VMU };

#define VMU_SIZE (128 * 1024)
static uint8_t vmu[VMU_SIZE];
static const char *vmu_path;
static int maplelog;

static uint32_t swap32(uint32_t v)
{
    return v >> 24 | (v >> 8 & 0xff00u) | (v << 8 & 0xff0000u) | v << 24;
}

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

// a blank card, as the BIOS formats it
static void vmu_format(void)
{
    memset(vmu, 0, sizeof vmu);
    uint8_t *root = vmu + 255 * 512;
    memset(root, 0x55, 16);
    put16(root + 0x46, 254);   // FAT
    put16(root + 0x48, 1);
    put16(root + 0x4a, 253);   // directory
    put16(root + 0x4c, 13);
    put16(root + 0x4e, 0);     // icon
    put16(root + 0x50, 200);   // user blocks

    uint8_t *fat = vmu + 254 * 512;
    for (int b = 0; b < 256; b++) put16(fat + 2 * b, 0xfffc);   // free
    for (int b = 253; b > 241; b--) put16(fat + 2 * b, (uint16_t)(b - 1));   // directory chain 253 -> 241
    put16(fat + 2 * 241, 0xfffa);
    put16(fat + 2 * 254, 0xfffa);
    put16(fat + 2 * 255, 0xfffa);
}

static void vmu_save(void)
{
    FILE *f = fopen(vmu_path, "wb");
    if (!f) { fprintf(stderr, "[maple] can't write %s\n", vmu_path); return; }
    fwrite(vmu, 1, sizeof vmu, f);
    fclose(f);
}

void maple_init(void)
{
    maplelog = getenv("DC_MAPLELOG") ? atoi(getenv("DC_MAPLELOG")) : 0;
    vmu_path = getenv("DC_VMU") ? getenv("DC_VMU") : "vmu_a1.bin";
    FILE *f = fopen(vmu_path, "rb");
    if (f && fread(vmu, 1, sizeof vmu, f) == sizeof vmu) {
        fprintf(stderr, "[maple] VMU from %s\n", vmu_path);
    } else {
        vmu_format();
        fprintf(stderr, "[maple] blank VMU (saved to %s when the game writes it)\n", vmu_path);
    }
    if (f) fclose(f);
}

static void put_str(uint8_t *p, const char *s, size_t n)
{
    size_t len = strlen(s);
    memset(p, ' ', n);
    memcpy(p, s, len < n ? len : n);
}

// 28 words: functions, their data words, area, direction, name, licence, power
static int device_info(int dev, uint32_t *data)
{
    static const uint32_t ctrl_fn[4] = { FN_CONTROLLER, 0xfe060f00u, 0, 0 };
    static const uint32_t vmu_fn[4] = { FN_STORAGE | FN_LCD | FN_CLOCK, 0x403f7e7eu, 0x00100500u, 0x00410f00u };
    uint8_t b[112];
    memset(b, 0, sizeof b);
    memcpy(b, dev == CONTROLLER ? ctrl_fn : vmu_fn, 16);
    b[16] = 0xff;   // all regions
    b[17] = 0;
    put_str(b + 18, dev == CONTROLLER ? "Dreamcast Controller" : "Visual Memory", 30);
    put_str(b + 48, "Produced By or Under License From SEGA ENTERPRISES,LTD.", 60);
    put16(b + 108, dev == CONTROLLER ? 0x01ae : 0x007c);
    put16(b + 110, dev == CONTROLLER ? 0x01f4 : 0x0082);
    memcpy(data, b, sizeof b);
    return 28;
}

// answer one command frame; returns the reply's length in words, header included
static int answer(const uint32_t *in, int nin, uint32_t *out)
{
    uint32_t h = in[0];
    uint32_t cmd = h & 0xff, to = (h >> 8) & 0xff, from = (h >> 16) & 0xff;
    int dev = NONE;
    if (to == 0x20) dev = CONTROLLER;      // port A main device
    else if (to == 0x01) dev = VMU;        // port A, slot 1
    if (dev == NONE) { out[0] = 0xffffffffu; return 1; }

    // a main device's address also says what's plugged into it
    uint32_t sender = dev == CONTROLLER ? 0x21u : to;
    uint32_t *data = out + 1, fn = nin > 1 ? in[1] : 0, code;
    int n = 0;

    switch (cmd) {
    case 0x01:   // device request
    case 0x02:   // all status
        n = device_info(dev, data);
        code = cmd == 0x01 ? R_DEVICE_INFO : R_ALL_STATUS;
        break;

    case 0x09:   // get condition
        if (dev == CONTROLLER && fn == FN_CONTROLLER) {
            dc_pad p;
            video_pad(&p);
            uint8_t b[8];
            put16(b, (uint16_t)(~p.buttons | 0xf901u));   // 0 = pressed; no C, Z, D, 2nd d-pad
            b[2] = p.rt;
            b[3] = p.lt;
            b[4] = p.x;
            b[5] = p.y;
            b[6] = b[7] = 0x80;
            data[0] = FN_CONTROLLER;
            memcpy(&data[1], b, 8);
            n = 3;
            code = R_DATA_TRANSFER;
        } else if (dev == VMU && fn == FN_CLOCK) {
            time_t t = time(NULL);
            struct tm *tm = localtime(&t);
            uint8_t b[8];
            put16(b, (uint16_t)(tm->tm_year + 1900));
            b[2] = (uint8_t)(tm->tm_mon + 1);
            b[3] = (uint8_t)tm->tm_mday;
            b[4] = (uint8_t)tm->tm_hour;
            b[5] = (uint8_t)tm->tm_min;
            b[6] = (uint8_t)tm->tm_sec;
            b[7] = (uint8_t)tm->tm_wday;
            data[0] = FN_CLOCK;
            memcpy(&data[1], b, 8);
            n = 3;
            code = R_DATA_TRANSFER;
        } else {
            code = R_BAD_FUNCTION;
        }
        break;

    case 0x0a:   // get media info
        if (dev == VMU && fn == FN_STORAGE) {
            uint8_t b[24];
            memset(b, 0, sizeof b);
            put16(b + 0, 255);    // total blocks
            put16(b + 2, 0);      // partition
            put16(b + 4, 255);    // root block
            put16(b + 6, 254);    // FAT
            put16(b + 8, 1);
            put16(b + 10, 253);   // directory
            put16(b + 12, 13);
            b[14] = 0;            // icon
            b[15] = 0;
            put16(b + 16, 200);   // user blocks
            put16(b + 18, 31);
            data[0] = FN_STORAGE;
            memcpy(&data[1], b, sizeof b);
            n = 7;
            code = R_DATA_TRANSFER;
        } else {
            code = R_BAD_FUNCTION;
        }
        break;

    case 0x0b:   // block read
        if (dev == VMU && fn == FN_STORAGE && nin > 2) {
            uint32_t block = swap32(in[2]) & 0xffff;
            if (block > 255) block = 255;
            data[0] = FN_STORAGE;
            data[1] = in[2];
            memcpy(&data[2], vmu + block * 512, 512);
            n = 130;
            code = R_DATA_TRANSFER;
        } else {
            code = R_BAD_FUNCTION;
        }
        break;

    case 0x0c:   // block write: 128 bytes, `phase` says which quarter of the block
        if (dev == VMU && fn == FN_STORAGE && nin > 3) {
            uint32_t loc = swap32(in[2]), block = loc & 0xffff, phase = (loc >> 16) & 0xff;
            uint32_t off = block * 512 + phase * 128, len = (uint32_t)(nin - 3) * 4;
            if (block <= 255 && off + len <= VMU_SIZE) {
                memcpy(vmu + off, &in[3], len);
                vmu_save();
            }
            code = R_DEVICE_REPLY;
        } else {
            code = R_DEVICE_REPLY;   // LCD writes: nothing to show them on yet
        }
        break;

    case 0x03:   // reset
    case 0x0d:   // get last error (end of a write)
    case 0x0e:   // set condition (LCD, clock, rumble)
        code = R_DEVICE_REPLY;
        break;

    default:
        code = R_UNKNOWN_CMD;
        break;
    }

    out[0] = code | from << 8 | sender << 16 | (uint32_t)n << 24;
    if (maplelog > 0) {
        maplelog--;
        fprintf(stderr, "[maple] %02x to %02x -> %02x, %d words\n", cmd, to, code, n);
    }
    return n + 1;
}

void maple_dma(void)
{
    static uint32_t in[256], out[256];
    uint32_t addr = holly_reg(MAPLE_SB_MDSTAR) & 0x1fffffe0u;

    for (int entries = 0; entries < 64; entries++) {
        uint32_t h = mem_r32(addr), reply = mem_r32(addr + 4) & 0x1fffffe0u;
        int pattern = (h >> 8) & 7, len = (int)(h & 0xff) + 1;
        if (pattern == 0) {
            for (int i = 0; i < len; i++) in[i] = mem_r32(addr + 8 + 4u * (uint32_t)i);
            int n = answer(in, len, out);
            for (int i = 0; i < n; i++) mem_w32(reply + 4u * (uint32_t)i, out[i]);
            addr += 8 + 4u * (uint32_t)len;
        } else {
            addr += 4;   // NOP, reset, light gun: header only
        }
        if (h >> 31) break;
    }
    holly_set_reg(MAPLE_SB_MDST, 0);
    holly_raise(1u << 12);   // Maple DMA end
}
