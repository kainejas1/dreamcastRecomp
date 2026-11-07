#include "patches.h"
#include "memory.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint32_t addr, value;
} poke;

// SA2's screen setup block at 8c28def4: projection distance, width 640,
// height 480, centre 320 240, ..., clip 0 0 639 479, then an aspect of 1.0.
// A width of 480 and an aspect of 0.75 make the projection fit 4/3 as much
// view across the same 640 pixels (Flycast's cheats.cpp has the same pair).
static const poke widescreen_sa2_usa[] = {
    { 0x8c28def8u, 0x43f00000u },   // width: 480.0
    { 0x8c28df28u, 0x3f400000u },   // aspect: 0.75
};

static const poke *pokes;
static size_t npokes;
static int widescreen;

void patches_init(void)
{
    const char *w = getenv("DC_WIDESCREEN");
    if (!w || !atoi(w)) return;
    char product[11];
    memcpy(product, dc_ram + 0x8040u, 10);   // IP.BIN at 8c008000, product number at +40
    product[10] = 0;
    if (memcmp(product, "MK-51117", 8) != 0) {
        fprintf(stderr, "[patch] no widescreen patch for %s\n", product);
        return;
    }
    pokes = widescreen_sa2_usa;
    npokes = sizeof widescreen_sa2_usa / sizeof widescreen_sa2_usa[0];
    widescreen = atoi(w) == 2 ? 2 : 1;
    fprintf(stderr, "[patch] widescreen (SA2 USA)%s\n", widescreen == 2 ? ", 2D stretched" : "");
}

void patches_frame(void)
{
    for (size_t i = 0; i < npokes; i++)
        memcpy(dc_ram + (pokes[i].addr & (DC_RAM_SIZE - 1)), &pokes[i].value, 4);
}

int patches_widescreen(void) { return widescreen; }
