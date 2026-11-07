#include "pvr.h"
#include "pvr_frame.h"
#include "clock.h"
#ifdef DC_GPU
#include "gpu.h"
#endif
#include "holly.h"
#include "memory.h"
#include "prof.h"

#include <SDL3/SDL.h>

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * TA parameters come in 32-byte units, each starting with a parameter
 * control word (PCW):
 *   31-29  0 end of list, 1 user tile clip, 2 object list set,
 *          4 polygon / modifier volume header, 5 sprite header, 7 vertex
 *   28     end of strip (vertex)
 *   26-24  list: 0 opaque, 1 opaque modifier, 2 translucent,
 *          3 translucent modifier, 4 punch-through
 *   7-0    obj control: 6 two volumes, 5-4 colour type (packed, float,
 *          intensity 1, intensity 2), 3 texture, 2 offset, 1 gouraud, 0 16-bit uv
 * Some headers and vertices take two units (64 bytes).
 * Layouts follow the PowerVR docs and Flycast's ta.cpp.
 */

static int pvrlog;   // DC_PVRLOG=n: describe the first n renders
static long pvrdump = -1;   // DC_PVRDUMP=n: write render n's strips and vertices to pvr_dump.txt
#define PVRLOG(...) do { if (pvrlog > 0) fprintf(stderr, __VA_ARGS__); } while (0)

// ---- VRAM. dc_vram is in 32-bit path (linear) layout; textures and TA
// parameters are addressed through the 64-bit path ----

static uint32_t off64(uint32_t a)
{
    a &= DC_VRAM_SIZE - 1;
    return (a & 3u) | ((a >> 1) & 0x3ffffcu) | (((a >> 2) & 1u) << 22);
}
static uint8_t  v64_r8(uint32_t a)  { return dc_vram[off64(a)]; }
static uint16_t v64_r16(uint32_t a) { uint32_t o = off64(a); return (uint16_t)(dc_vram[o] | dc_vram[o + 1] << 8); }
static uint32_t v64_r32(uint32_t a) { uint32_t v; memcpy(&v, &dc_vram[off64(a)], 4); return v; }

static float as_float(uint32_t w) { float f; memcpy(&f, &w, 4); return f; }

// ---- what the TA collects ----

typedef pvr_vertex vtx;   // in pvr_frame.h, so another renderer can take them as they are
typedef pvr_strip strip;

typedef struct {
    uint32_t key;             // TA_ISP_BASE it was started at
    vtx *v;
    int nv, capv;
    strip *s;
    int ns, caps;
    vtx *mv;                  // modifier volume triangles, 3 vertices each
    int nmv, capmv;
    pvr_modvol *m;
    int nm, capm;
    int open_m;               // modifier volume polygon taking triangles, -1 none
} ta_ctx;

#define MAX_CTX 4
static ta_ctx ctxs[MAX_CTX];
static ta_ctx *cur = &ctxs[0];
static int next_ctx;

static struct {
    uint32_t buf[16];
    int nw, need;             // words collected / words this parameter has
    int list;                 // list being sent, -1 none
    uint32_t pcw, isp, tsp, tcw;
    int vtype;
    float face[4], face_off[4];   // header colours (intensity modes, sprites)
    int open;                 // strip being built, -1 none
} ta;

static uint32_t renders, skipped_units;

uint32_t pvr_render_count(void) { return renders; }

void pvr_init(void)
{
    pvrlog = getenv("DC_PVRLOG") ? atoi(getenv("DC_PVRLOG")) : 0;
    if (getenv("DC_PVRDUMP")) pvrdump = atol(getenv("DC_PVRDUMP"));
    memset(&ta, 0, sizeof ta);
    ta.list = -1;
    ta.open = -1;
    for (int i = 0; i < MAX_CTX; i++) ctxs[i].nv = ctxs[i].ns = 0;
}

static void packed(uint32_t c, float *out)
{
    out[0] = ((c >> 16) & 0xff) / 255.0f;
    out[1] = ((c >> 8) & 0xff) / 255.0f;
    out[2] = (c & 0xff) / 255.0f;
    out[3] = (c >> 24) / 255.0f;
}

// float A, R, G, B at w
static void argb_f(const uint32_t *w, float *out)
{
    out[0] = as_float(w[1]);
    out[1] = as_float(w[2]);
    out[2] = as_float(w[3]);
    out[3] = as_float(w[0]);
}

static void intensity(uint32_t i, const float *face, float *out)
{
    float k = as_float(i);
    out[0] = face[0] * k;
    out[1] = face[1] * k;
    out[2] = face[2] * k;
    out[3] = face[3];
}

static void uv16(uint32_t w, vtx *v)
{
    v->u = as_float(w & 0xffff0000u);
    v->v = as_float(w << 16);
}

static void close_strip(void)
{
    if (ta.open >= 0 && cur->s[ta.open].count < 3)
        cur->ns--;   // nothing to draw
    ta.open = -1;
}

static void add_vertex(const vtx *v)
{
    if (ta.open < 0) {
        if (cur->ns == cur->caps) {
            cur->caps = cur->caps ? cur->caps * 2 : 1024;
            cur->s = realloc(cur->s, (size_t)cur->caps * sizeof *cur->s);
            if (!cur->s) { fprintf(stderr, "[pvr] out of memory\n"); exit(1); }
        }
        strip *s = &cur->s[cur->ns];
        s->pcw = ta.pcw;
        s->isp = ta.isp;
        s->tsp = ta.tsp;
        s->tcw = ta.tcw;
        s->list = ta.list;
        s->first = cur->nv;
        s->count = 0;
        ta.open = cur->ns++;
    }
    if (cur->nv == cur->capv) {
        cur->capv = cur->capv ? cur->capv * 2 : 4096;
        cur->v = realloc(cur->v, (size_t)cur->capv * sizeof *cur->v);
        if (!cur->v) { fprintf(stderr, "[pvr] out of memory\n"); exit(1); }
    }
    cur->v[cur->nv++] = *v;
    cur->s[ta.open].count++;
}

static int is_modvol(int list) { return list == 1 || list == 3; }

// the modifier volume polygon being sent is complete
static void close_modvol(void)
{
    if (cur->open_m >= 0 && cur->m[cur->open_m].count == 0) cur->nm--;
    cur->open_m = -1;
}

static void *grow(void *p, int *cap, int need, size_t size, int first)
{
    if (need <= *cap) return p;
    *cap = *cap ? *cap * 2 : first;
    p = realloc(p, (size_t)*cap * size);
    if (!p) { fprintf(stderr, "[pvr] out of memory\n"); exit(1); }
    return p;
}

// a modifier volume polygon header: its triangles follow
static void open_modvol(uint32_t pcw, uint32_t isp)
{
    close_modvol();
    cur->m = grow(cur->m, &cur->capm, cur->nm + 1, sizeof *cur->m, 256);
    pvr_modvol *m = &cur->m[cur->nm];
    m->isp = isp;
    m->last = (pcw >> 6) & 1;
    m->list = ta.list;
    m->first = cur->nmv / 3;
    m->count = 0;
    cur->open_m = cur->nm++;
}

// a modifier volume vertex parameter: one triangle, x y z three times
static void modvol_triangle(const uint32_t *w)
{
    if (cur->open_m < 0) return;
    cur->mv = grow(cur->mv, &cur->capmv, cur->nmv + 3, sizeof *cur->mv, 3072);
    for (int i = 0; i < 3; i++) {
        vtx *v = &cur->mv[cur->nmv++];
        memset(v, 0, sizeof *v);
        v->x = as_float(w[1 + 3 * i]);
        v->y = as_float(w[2 + 3 * i]);
        v->z = as_float(w[3 + 3 * i]);
    }
    cur->m[cur->open_m].count++;
}

// size of the parameter starting with this PCW, in words
static int param_words(uint32_t pcw)
{
    switch (pcw >> 29) {
    case 4: {
        int list = ta.list >= 0 ? ta.list : (int)((pcw >> 24) & 7);
        if (is_modvol(list)) return 8;
        int volume = (pcw >> 6) & 1, col = (pcw >> 4) & 3, offs = (pcw >> 2) & 1;
        if (!volume) return col == 2 && offs ? 16 : 8;
        return col == 2 ? 16 : 8;
    }
    case 7:
        switch (ta.vtype) {
        case 5: case 6: case 11: case 12: case 13: case 14: case 15: case 16: case 17:
            return 16;
        }
        return 8;
    }
    return 8;
}

static int vertex_type(uint32_t pcw)
{
    int volume = (pcw >> 6) & 1, col = (pcw >> 4) & 3, tex = (pcw >> 3) & 1, uv = pcw & 1;
    if (!volume) {
        if (!tex) return col == 0 ? 0 : col == 1 ? 1 : 2;
        if (col == 0) return uv ? 4 : 3;
        if (col == 1) return uv ? 6 : 5;
        return uv ? 8 : 7;
    }
    if (!tex) return col == 0 ? 9 : 10;
    if (col == 0) return uv ? 12 : 11;
    return uv ? 14 : 13;
}

// a sprite vertex parameter: a whole quad, A B C given, D only as x y
static void sprite(const uint32_t *w)
{
    vtx q[4];
    memset(q, 0, sizeof q);
    for (int i = 0; i < 3; i++) {
        q[i].x = as_float(w[1 + 3 * i]);
        q[i].y = as_float(w[2 + 3 * i]);
        q[i].z = as_float(w[3 + 3 * i]);
    }
    q[3].x = as_float(w[10]);
    q[3].y = as_float(w[11]);
    q[3].z = q[0].z + q[2].z - q[1].z;
    if (ta.vtype == 16) {
        uv16(w[13], &q[0]);
        uv16(w[14], &q[1]);
        uv16(w[15], &q[2]);
        q[3].u = q[0].u + q[2].u - q[1].u;
        q[3].v = q[0].v + q[2].v - q[1].v;
    }
    for (int i = 0; i < 4; i++) {
        memcpy(q[i].col, ta.face, sizeof q[i].col);
        memcpy(q[i].off, ta.face_off, sizeof q[i].off);
    }
    close_strip();
    // as a strip: A B D C
    add_vertex(&q[0]);
    add_vertex(&q[1]);
    add_vertex(&q[3]);
    add_vertex(&q[2]);
    close_strip();
}

static const uint32_t list_irq[5] = { 1u << 7, 1u << 8, 1u << 9, 1u << 10, 1u << 21 };

static void ta_param(const uint32_t *w)
{
    uint32_t pcw = w[0];

    switch (pcw >> 29) {
    case 0:   // end of list
        close_strip();
        close_modvol();
        if (ta.list >= 0) holly_raise(list_irq[ta.list]);
        ta.list = -1;
        break;

    case 1:   // user tile clip
    case 2:   // object list set
        break;

    case 4:   // polygon or modifier volume header
    case 5: { // sprite header
        if (ta.list < 0) ta.list = (pcw >> 24) & 7;
        if (ta.list > 4) ta.list = 0;
        close_strip();
        if (is_modvol(ta.list)) { ta.vtype = 17; open_modvol(pcw, w[1]); break; }

        ta.pcw = pcw;
        // the TA copies texture/offset/gouraud/16-bit uv from the PCW
        ta.isp = (w[1] & ~0x03c00000u) | ((pcw & 0xfu) << 22);
        ta.tsp = w[2];
        ta.tcw = w[3];
        if ((pcw >> 29) == 5) {
            ta.vtype = (pcw & 8) ? 16 : 15;
            ta.isp |= 1u << 22;   // sprites always have 16-bit uv
            packed(w[4], ta.face);
            packed(w[5], ta.face_off);
            break;
        }
        ta.vtype = vertex_type(pcw);
        if (((pcw >> 4) & 3) == 2) {   // intensity mode 1: the face colour is here
            int volume = (pcw >> 6) & 1, offs = (pcw >> 2) & 1;
            if (!volume && !offs) argb_f(&w[4], ta.face);
            else { argb_f(&w[8], ta.face); argb_f(&w[12], ta.face_off); }
        }
        break;
    }

    case 7: { // vertex
        if (ta.list < 0) break;
        if (is_modvol(ta.list)) { modvol_triangle(w); break; }
        if (ta.vtype == 15 || ta.vtype == 16) { sprite(w); break; }

        vtx v;
        memset(&v, 0, sizeof v);
        v.x = as_float(w[1]);
        v.y = as_float(w[2]);
        v.z = as_float(w[3]);
        switch (ta.vtype) {
        case 0: packed(w[6], v.col); break;
        case 1: argb_f(&w[4], v.col); break;
        case 2: intensity(w[6], ta.face, v.col); break;
        case 3: v.u = as_float(w[4]); v.v = as_float(w[5]); packed(w[6], v.col); packed(w[7], v.off); break;
        case 4: uv16(w[4], &v); packed(w[6], v.col); packed(w[7], v.off); break;
        case 5: v.u = as_float(w[4]); v.v = as_float(w[5]); argb_f(&w[8], v.col); argb_f(&w[12], v.off); break;
        case 6: uv16(w[4], &v); argb_f(&w[8], v.col); argb_f(&w[12], v.off); break;
        case 7: v.u = as_float(w[4]); v.v = as_float(w[5]);
                intensity(w[6], ta.face, v.col); intensity(w[7], ta.face_off, v.off); break;
        case 8: uv16(w[4], &v); intensity(w[6], ta.face, v.col); intensity(w[7], ta.face_off, v.off); break;
        // two volumes: only the first is used
        case 9: packed(w[4], v.col); break;
        case 10: intensity(w[4], ta.face, v.col); break;
        case 11: v.u = as_float(w[4]); v.v = as_float(w[5]); packed(w[6], v.col); packed(w[7], v.off); break;
        case 12: uv16(w[4], &v); packed(w[6], v.col); packed(w[7], v.off); break;
        case 13: v.u = as_float(w[4]); v.v = as_float(w[5]);
                 intensity(w[6], ta.face, v.col); intensity(w[7], ta.face_off, v.off); break;
        case 14: uv16(w[4], &v); intensity(w[6], ta.face, v.col); intensity(w[7], ta.face_off, v.off); break;
        }
        add_vertex(&v);
        if (pcw & 0x10000000u) close_strip();
        break;
    }

    default:
        skipped_units++;
        break;
    }
}

static void ta_word(uint32_t word)
{
    ta.buf[ta.nw++] = word;
    if (ta.nw == 1) ta.need = param_words(word);
    if (ta.nw == ta.need) {
        ta_param(ta.buf);
        ta.nw = 0;
    }
}

static void list_init(void)
{
    uint32_t key = holly_reg(PVR_TA_ISP_BASE) & 0x00f00000u;
    cur = NULL;
    for (int i = 0; i < MAX_CTX; i++)
        if (ctxs[i].key == key && (ctxs[i].nv || ctxs[i].ns)) cur = &ctxs[i];
    if (!cur) {
        cur = &ctxs[next_ctx];
        next_ctx = (next_ctx + 1) % MAX_CTX;
    }
    cur->key = key;
    cur->nv = cur->ns = 0;
    cur->nmv = cur->nm = 0;
    cur->open_m = -1;
    ta.list = -1;
    ta.open = -1;
    ta.nw = 0;
}

// ---- area 4 ----

int pvr_ta_handles(uint32_t phys)
{
    return phys >= 0x10000000u && phys < 0x14000000u;
}

static void texture_write(uint32_t phys, uint32_t value, int size)
{
    int lm = (int)(holly_reg((phys >> 24) & 2 ? PVR_SB_LMMODE1 : PVR_SB_LMMODE0) & 1);
    uint32_t a = phys & 0x00ffffffu;
    uint32_t o = lm ? (a & (DC_VRAM_SIZE - 1)) : off64(a);
    memcpy(&dc_vram[o], &value, (size_t)size);
}

void pvr_ta_write(uint32_t phys, uint32_t value, int size)
{
    if ((phys >> 24) & 1) { texture_write(phys, value, size); return; }
    if (phys & 0x00800000u) return;   // YUV converter: not yet
    if (size == 4) ta_word(value);
}

// ch2 DMA: system RAM (source in the SH-4 DMAC's SAR2) to the TA or texture memory
static void ch2_dma(void)
{
    uint32_t dst = holly_reg(PVR_SB_C2DSTAT), len = holly_reg(PVR_SB_C2DLEN);
    uint32_t src = sh4reg_read32(0xffa00020u);
    uint32_t phys = dst & 0x1fffffffu;
    int tex = (phys >> 24) & 1;

    for (uint32_t i = 0; i < len; i += 4)
        pvr_ta_write(tex ? phys + i : phys, mem_r32(src + i), 4);

    if (tex) holly_set_reg(PVR_SB_C2DSTAT, dst + len);
    holly_set_reg(PVR_SB_C2DLEN, 0);
    holly_set_reg(PVR_SB_C2DST, 0);
    sh4reg_write32(0xffa00020u, src + len);                             // SAR2
    sh4reg_write32(0xffa00028u, 0);                                     // DMATCR2
    sh4reg_write32(0xffa0002cu, sh4reg_read32(0xffa0002cu) | 2u);       // CHCR2.TE
    holly_raise(1u << 19);
}

// ---- textures ----

static const uint32_t vq_mip[11] = { 0x00000, 0x00001, 0x00002, 0x00006, 0x00016, 0x00056,
                                     0x00156, 0x00556, 0x01556, 0x05556, 0x15556 };
static const uint32_t other_mip[11] = { 0x00003, 0x00004, 0x00008, 0x00018, 0x00058, 0x00158,
                                        0x00558, 0x01558, 0x05558, 0x15558, 0x55558 };

static uint32_t spread[1024];   // the bits of i moved to the even positions
static uint32_t pal_argb[1024]; // the palette as ARGB8888, read at each render

static void init_spread(void)
{
    for (uint32_t i = 0; i < 1024; i++) {
        uint32_t r = 0;
        for (int b = 0; b < 10; b++) r |= ((i >> b) & 1u) << (2 * b);
        spread[i] = r;
    }
}

static int log2i(uint32_t v)
{
    int l = 0;
    while ((1u << l) < v) l++;
    return l;
}

// texel order: x and y bits interleaved (y in bit 0), in squares of 1 << m
// texels, one after another along the longer side
static uint32_t twiddle(int m, int wide, int tall, uint32_t x, uint32_t y)
{
    uint32_t mask = (1u << m) - 1u;
    uint32_t r = spread[x & mask] << 1 | spread[y & mask];
    if (wide) r += (x >> m) << (2 * m);
    else if (tall) r += (y >> m) << (2 * m);
    return r;
}

static uint32_t expand16(uint16_t t, int fmt)
{
    uint32_t a, r, g, b;
    switch (fmt) {
    case 1:   // 565
        r = (t >> 11) & 31; g = (t >> 5) & 63; b = t & 31;
        return 0xff000000u | (r << 3 | r >> 2) << 16 | (g << 2 | g >> 4) << 8 | (b << 3 | b >> 2);
    case 2:   // 4444
        a = (t >> 12) & 15; r = (t >> 8) & 15; g = (t >> 4) & 15; b = t & 15;
        return (a * 17) << 24 | (r * 17) << 16 | (g * 17) << 8 | b * 17;
    default:  // 1555
        r = (t >> 10) & 31; g = (t >> 5) & 31; b = t & 31;
        return (t & 0x8000 ? 0xff000000u : 0) | (r << 3 | r >> 2) << 16 | (g << 3 | g >> 2) << 8 | (b << 3 | b >> 2);
    }
}

static void load_palette(void)
{
    int mode = (int)(holly_reg(PVR_PAL_RAM_CTRL) & 3);
    for (uint32_t i = 0; i < 1024; i++) {
        uint32_t p = holly_reg(PVR_PALETTE + 4 * i);
        pal_argb[i] = mode == 3 ? p : expand16((uint16_t)p, mode);   // 0 1555, 1 565, 2 4444, 3 8888
    }
}

static uint32_t yuv(int y, int u, int v)
{
    float fy = (float)y, fu = (float)(u - 128), fv = (float)(v - 128);
    int r = (int)(fy + 1.375f * fv), g = (int)(fy - 0.34375f * fu - 0.6875f * fv), b = (int)(fy + 1.71875f * fu);
    r = r < 0 ? 0 : r > 255 ? 255 : r;
    g = g < 0 ? 0 : g > 255 ? 255 : g;
    b = b < 0 ? 0 : b > 255 ? 255 : b;
    return 0xff000000u | (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b;
}

enum { TEX_16, TEX_VQ, TEX_PAL4, TEX_PAL8, TEX_YUV, TEX_BUMP };

// a texture as a strip uses it, worked out once per strip
typedef struct {
    int kind, fmt;
    uint32_t base, book;      // the texels (for VQ, the indices) and the VQ codebook
    int w, h;                 // powers of two
    int twiddled, stride;     // stride in texels, when not twiddled
    int m, wide, tall;        // twiddle squares (for VQ, of the index map)
    uint32_t pal;             // first palette entry
    int clamp_u, clamp_v, flip_u, flip_v;
} sampler;

// level: for a mipmapped texture, the level 2^level square to read (-1: the top one)
static void sampler_level(sampler *t, uint32_t tsp, uint32_t tcw, int level)
{
    uint32_t addr = (tcw & 0x1fffffu) << 3;
    int fmt = (tcw >> 27) & 7, vq = (tcw >> 30) & 1, mip = (tcw >> 31) & 1;
    t->fmt = fmt;
    t->w = 8 << ((tsp >> 3) & 7);
    t->h = 8 << (tsp & 7);
    if (mip && level >= 0) t->w = t->h = 1 << level;
    t->clamp_u = (tsp >> 16) & 1;
    t->clamp_v = (tsp >> 15) & 1;
    t->flip_u = (tsp >> 18) & 1;
    t->flip_v = (tsp >> 17) & 1;
    t->twiddled = 1;
    t->stride = t->w;
    t->book = 0;
    t->pal = 0;
    t->m = log2i((uint32_t)(t->w < t->h ? t->w : t->h));
    t->wide = t->w > t->h;
    t->tall = t->h > t->w;
    int lw = log2i((uint32_t)t->w);

    if (vq) {   // 2048-byte codebook of 2x2 blocks, then a byte per block
        t->kind = TEX_VQ;
        t->book = addr;
        t->base = addr + 2048 + (mip ? vq_mip[lw] : 0);
        if (t->m > 0) t->m -= 1;   // the index map is (w/2) x (h/2); the 1x1 level is a 2x2 block's corner
        return;
    }
    if (fmt == 5) {   // 4 bpp palette
        t->kind = TEX_PAL4;
        t->base = addr + (mip ? other_mip[lw] / 2 : 0);
        t->pal = ((tcw >> 21) & 0x3fu) << 4;
        return;
    }
    if (fmt == 6) {   // 8 bpp palette
        t->kind = TEX_PAL8;
        t->base = addr + (mip ? other_mip[lw] : 0);
        t->pal = ((tcw >> 25) & 3u) << 8;
        return;
    }
    t->twiddled = !((tcw >> 26) & 1);
    if (!t->twiddled && ((tcw >> 25) & 1)) t->stride = (int)(holly_reg(PVR_TEXT_CONTROL) & 31) * 32;
    t->base = addr + (mip ? other_mip[lw] * 2 : 0);
    t->kind = fmt == 3 ? TEX_YUV : fmt == 4 ? TEX_BUMP : TEX_16;
}

static void sampler_init(sampler *t, uint32_t tsp, uint32_t tcw) { sampler_level(t, tsp, tcw, -1); }

// ARGB8888 of texel (x, y), already wrapped to the texture
static uint32_t sample(const sampler *t, uint32_t x, uint32_t y)
{
    switch (t->kind) {
    case TEX_VQ: {
        uint8_t code = v64_r8(t->base + twiddle(t->m, t->wide, t->tall, x >> 1, y >> 1));
        return expand16(v64_r16(t->book + code * 8u + ((x & 1) << 1 | (y & 1)) * 2), t->fmt);
    }
    case TEX_PAL4: {
        uint32_t i = twiddle(t->m, t->wide, t->tall, x, y);
        uint8_t b = v64_r8(t->base + i / 2);
        return pal_argb[(t->pal + ((i & 1) ? b >> 4 : b & 15u)) & 1023];
    }
    case TEX_PAL8:
        return pal_argb[(t->pal + v64_r8(t->base + twiddle(t->m, t->wide, t->tall, x, y))) & 1023];
    case TEX_YUV: {   // U Y0 V Y1 for each pair of texels
        uint32_t pair = t->twiddled ? twiddle(t->m, t->wide, t->tall, x & ~1u, y) : y * (uint32_t)t->stride + (x & ~1u);
        uint32_t d = v64_r32(t->base + pair * 2);
        int u = d & 0xff, y0 = (d >> 8) & 0xff, v = (d >> 16) & 0xff, y1 = d >> 24;
        return yuv(x & 1 ? y1 : y0, u, v);
    }
    case TEX_BUMP:
        return 0xff808080u;   // not shaded
    default: {
        uint32_t i = t->twiddled ? twiddle(t->m, t->wide, t->tall, x, y) : y * (uint32_t)t->stride + x;
        return expand16(v64_r16(t->base + i * 2), t->fmt);
    }
    }
}

// ---- textures for another renderer (pvr_frame.h) ----

void pvr_texture_size(uint32_t tsp, int *w, int *h)
{
    *w = 8 << ((tsp >> 3) & 7);
    *h = 8 << (tsp & 7);
}

static uint64_t hash_words(uint64_t h, uint32_t addr, uint32_t len)
{
    for (uint32_t i = 0; i < len; i += 4) h = (h ^ v64_r32(addr + i)) * 0x100000001b3ull;
    return h;
}

// everything a texture's texels come from: its bytes in VRAM, and the
// palette entries or row stride it uses
uint64_t pvr_texture_hash(uint32_t tsp, uint32_t tcw)
{
    sampler t;
    sampler_init(&t, tsp, tcw);
    uint32_t w = (uint32_t)t.w, h = (uint32_t)t.h, addr = (tcw & 0x1fffffu) << 3, top;
    uint64_t r = 0xcbf29ce484222325ull ^ (uint64_t)t.kind << 40 ^ (uint64_t)t.stride << 48;
    switch (t.kind) {   // bytes of the top level; smaller mip levels (and a VQ codebook) come before it
    case TEX_VQ: top = w / 2 * h / 2; break;
    case TEX_PAL4: top = w * h / 2; break;
    case TEX_PAL8: top = w * h; break;
    case TEX_BUMP: return r;
    default: top = (t.twiddled ? w : (uint32_t)t.stride) * h * 2; break;
    }
    r = hash_words(r, addr, t.base + top - addr);
    if (t.kind == TEX_PAL4 || t.kind == TEX_PAL8)
        for (uint32_t i = 0, n = t.kind == TEX_PAL4 ? 16 : 256; i < n; i++)
            r = (r ^ pal_argb[(t.pal + i) & 1023]) * 0x100000001b3ull;
    return r;
}

int pvr_texture_levels(uint32_t tsp, uint32_t tcw)
{
    int w, h;
    pvr_texture_size(tsp, &w, &h);
    if (!((tcw >> 31) & 1) || w != h) return 1;
    return log2i((uint32_t)w) + 1;
}

void pvr_texture_decode_level(uint32_t tsp, uint32_t tcw, int l, uint32_t *argb)
{
    sampler t;
    sampler_level(&t, tsp, tcw, l);
    for (int y = 0; y < t.h; y++)
        for (int x = 0; x < t.w; x++)
            argb[y * t.w + x] = sample(&t, (uint32_t)x, (uint32_t)y);
}

void pvr_texture_decode(uint32_t tsp, uint32_t tcw, uint32_t *argb)
{
    sampler t;
    sampler_init(&t, tsp, tcw);
    for (int y = 0; y < t.h; y++)
        for (int x = 0; x < t.w; x++)
            argb[y * t.w + x] = sample(&t, (uint32_t)x, (uint32_t)y);
}

// framebuffer addresses the display has shown, most recent first
static uint32_t displayed[4];
static int ndisplayed;

void pvr_note_display(uint32_t fb)
{
    fb &= 0x00fffffcu;
    int i = 0;
    while (i < ndisplayed && displayed[i] != fb) i++;
    if (i == ndisplayed && ndisplayed < 4) ndisplayed++;
    if (i == 4) i = 3;
    for (; i > 0; i--) displayed[i] = displayed[i - 1];
    displayed[0] = fb;
}

static int was_displayed(uint32_t fb)
{
    for (int i = 0; i < ndisplayed; i++)
        if (displayed[i] == fb) return 1;
    return 0;
}

static uint32_t wrap(float t, int size, int clamp, int flip)
{
    float f = t * (float)size;
    if (!(f > -1e9f && f < 1e9f)) f = 0.0f;   // NaN, or too far out to matter
    int i = (int)f;
    if ((float)i > f) i--;   // floor
    if (clamp) return i < 0 ? 0 : i >= size ? (uint32_t)size - 1 : (uint32_t)i;
    if (flip) {
        uint32_t p = (uint32_t)i & (2u * (uint32_t)size - 1);
        return p < (uint32_t)size ? p : 2u * (uint32_t)size - 1 - p;
    }
    return (uint32_t)i & (uint32_t)(size - 1);
}

// ---- rendering ----
//
// The frame is cut into bands of 16 rows, dealt out to the threads in turn.
// Each thread draws every triangle, in order, into its own rows only, so a
// pixel always sees the same triangles in the same order and with the same
// arithmetic: the picture doesn't depend on how many threads there are.

#define BAND_SHIFT 4
#define MAX_THREADS 32

static uint32_t *cbuf;   // ARGB8888
static float *zbuf;
// per pixel, as Flycast's stencil: bit 7 the opaque polygon there takes
// shadows, bits 1-0 modifier volume state (1 = inside a volume)
static uint8_t *sbuf;

// The translucent list in auto-sort mode: every translucent pixel not behind
// the opaque ones is kept, then each pixel's are blended back to front (ties
// in the order sent), as the PVR sorts them. Each thread keeps the fragments
// of its own rows.
typedef struct {
    float z;
    float col[4];             // the polygon's colour there, before blending
    uint8_t src_f, dst_f, shadow, stencil;   // blend factors; modifier volume bits as sbuf's
    int next;                 // the pixel's previous fragment, -1 none
} frag;
static struct {
    frag *f;
    int n, cap;
    int *order, order_cap;    // scratch for sorting one pixel's fragments
} pool[32];
static int *fhead;            // per pixel: its latest fragment, -1 none
static int rw, rh, cap_pixels;
static int clip_x0, clip_x1, clip_y0, clip_y1;
static float pt_ref;     // punch-through alpha threshold
static int nthreads;

static struct {
    const ta_ctx *c;
    uint32_t bg;
    float bgz;
    int hscale;
    // the framebuffer write
    uint32_t base, stride, threshold;
    int mode, k, bpp, ow, rows_apart;
    // intensity shadows (FPU_SHAD_SCALE)
    int shadow_on;
    float shadow;
    int autosort;             // ISP_FEED_CFG bit 0 clear
    // fog (FOG_* registers) and the colour clamp
    float fog_density, fog_ram[3], fog_vert[3], clamp_min[4], clamp_max[4];
    uint8_t fog_hi[128], fog_lo[128];
} job;

// Table fog (Flycast's fog_mode2): density x 1/w, as a float with a 4-bit
// mantissa, picks one of 128 entries; the fraction blends its two values
static float fog_coef(float z)
{
    float v = job.fog_density * z;
    if (!(v >= 1.0f)) v = 1.0f;   // also NaN
    if (v > 255.9999f) v = 255.9999f;
    float e = floorf(log2f(v));
    float m = v * 16.0f / exp2f(e) - 16.0f;
    int idx = (int)floorf(m) + (int)e * 16;
    idx = idx < 0 ? 0 : idx > 127 ? 127 : idx;
    float f = m - floorf(m);
    return ((float)job.fog_hi[idx] * (1.0f - f) + (float)job.fog_lo[idx] * f) / 255.0f;
}

static void argb_to_float(uint32_t c, float *out)
{
    out[0] = ((c >> 16) & 0xff) / 255.0f;
    out[1] = ((c >> 8) & 0xff) / 255.0f;
    out[2] = (c & 0xff) / 255.0f;
    out[3] = (c >> 24) / 255.0f;
}

static int band_of(int t, int y) { return ((y >> BAND_SHIFT) % nthreads) == t; }

static float blend_factor(int f, int is_src, const float *src, const float *dst, int ch)
{
    switch (f) {
    case 0: return 0.0f;
    case 1: return 1.0f;
    case 2: return is_src ? dst[ch] : src[ch];
    case 3: return 1.0f - (is_src ? dst[ch] : src[ch]);
    case 4: return src[3];
    case 5: return 1.0f - src[3];
    case 6: return dst[3];
    default: return 1.0f - dst[3];
    }
}

static int depth_pass(int mode, float z, float old)
{
    switch (mode) {
    case 0: return 0;
    case 1: return z < old;
    case 2: return z == old;
    case 3: return z <= old;
    case 4: return z > old;
    case 5: return z != old;
    case 6: return z >= old;
    default: return 1;
    }
}

static float clamp01(float f) { return f < 0.0f ? 0.0f : f > 1.0f ? 1.0f : f; }

// what a strip's ISP/TSP words say, decoded once per strip
typedef struct {
    int list, textured, offset, gouraud, zmode, zwrite;
    int fog, clamp;           // TSP: fog control (0 table, 1 vertex, 2 none, 3 table mode 2), colour clamp
    int use_alpha, ignore_tex_alpha, shade, src_f, dst_f;
    uint8_t shadow;           // 0x80 if modifier volumes act on it
    sampler tex;
} strip_mode;

static void strip_mode_init(strip_mode *m, const strip *s)
{
    uint32_t isp = s->isp, tsp = s->tsp;
    m->list = s->list;
    m->textured = (isp >> 25) & 1;
    m->offset = (isp >> 24) & 1;
    m->gouraud = (isp >> 23) & 1;
    m->zmode = (int)(isp >> 29);
    m->zwrite = !((isp >> 26) & 1);
    // the hardware's own rules (Flycast): punch-through always tests "greater
    // or equal" and writes depth; sorted translucent tests "greater or equal"
    // against the opaque depth and writes none
    if (s->list == 4) { m->zmode = 6; m->zwrite = 1; }
    if (s->list == 2 && job.autosort) { m->zmode = 6; m->zwrite = 0; }
    m->use_alpha = (tsp >> 20) & 1;
    m->ignore_tex_alpha = (tsp >> 19) & 1;
    m->shade = (tsp >> 6) & 3;
    m->src_f = (int)(tsp >> 29);
    m->dst_f = (int)((tsp >> 26) & 7);
    m->shadow = s->pcw & PVR_PCW_SHADOW ? 0x80 : 0;
    m->fog = (int)((tsp >> 22) & 3);
    m->clamp = (int)((tsp >> 21) & 1);
    if (m->textured) sampler_init(&m->tex, tsp, s->tcw);
}

// px where alpha + beta * px >= 0 narrows [*lo, *hi]; 0 if none does
static int edge_span(double alpha, double beta, double *lo, double *hi)
{
    if (beta > 0.0) { double b = -alpha / beta; if (b > *lo) *lo = b; }
    else if (beta < 0.0) { double b = -alpha / beta; if (b < *hi) *hi = b; }
    else if (alpha < 0.0) return 0;
    return *lo <= *hi;
}

static void triangle(int t, const strip_mode *m, const vtx *a, const vtx *b, const vtx *c)
{
    float area = (b->x - a->x) * (c->y - a->y) - (b->y - a->y) * (c->x - a->x);
    if (!(fabsf(area) > 1e-6f)) return;   // also catches NaN
    // A corner at infinity (projected from behind the camera) makes every
    // weight below NaN, so z is NaN and only the "always" and "not equal"
    // depth tests let a pixel through: anything else would test every pixel
    // in its box for nothing.
    if (!(isfinite(a->x) && isfinite(a->y) && isfinite(b->x) && isfinite(b->y) && isfinite(c->x) && isfinite(c->y)) &&
        m->zmode != 5 && m->zmode != 7)
        return;

    float fminx = fminf(a->x, fminf(b->x, c->x)), fmaxx = fmaxf(a->x, fmaxf(b->x, c->x));
    float fminy = fminf(a->y, fminf(b->y, c->y)), fmaxy = fmaxf(a->y, fmaxf(b->y, c->y));
    if (fmaxx < (float)clip_x0 || fminx > (float)clip_x1 + 1 || fmaxy < (float)clip_y0 || fminy > (float)clip_y1 + 1) return;
    int x0 = fminx < (float)clip_x0 ? clip_x0 : (int)fminx;
    int x1 = fmaxx > (float)clip_x1 ? clip_x1 : (int)fmaxx;
    int y0 = fminy < (float)clip_y0 ? clip_y0 : (int)fminy;
    int y1 = fmaxy > (float)clip_y1 ? clip_y1 : (int)fmaxy;

    // any of our rows in it?
    int b0 = y0 >> BAND_SHIFT, b1 = y1 >> BAND_SHIFT;
    if (b1 - b0 + 1 < nthreads) {
        int mine = 0;
        for (int k = b0; k <= b1 && !mine; k++) mine = k % nthreads == t;
        if (!mine) return;
    }

    float inv_area = 1.0f / area;
    // Which pixels of a row are inside is decided per pixel, below. To get
    // there quickly, the same three weights in double precision say where in
    // the row they can be positive, give or take two pixels. Slivers (under a
    // pixel of area) and overflowing areas skip that and test the whole row.
    int use_span = fabsf(area) >= 1.0f && fabsf(area) < 1e30f;
    double ax = a->x, ay = a->y, bx = b->x, by = b->y, cx = c->x, cy = c->y, da = area;
    double be0 = (by - cy) / da, be1 = (cy - ay) / da;

    for (int y = y0; y <= y1; y++) {
        if (!band_of(t, y)) { y |= (1 << BAND_SHIFT) - 1; continue; }
        float py = (float)y + 0.5f;
        int xs = x0, xe = x1;
        if (use_span) {
            double dpy = py, lo = x0, hi = x1 + 1.0;
            double al0 = (bx * (cy - dpy) - (by - dpy) * cx) / da;
            double al1 = (cx * (ay - dpy) - (cy - dpy) * ax) / da;
            if (!edge_span(al0, be0, &lo, &hi) || !edge_span(al1, be1, &lo, &hi) ||
                !edge_span(1.0 - al0 - al1, -(be0 + be1), &lo, &hi))
                continue;
            if (lo - 2.0 > (double)xs) xs = (int)(lo - 2.0);
            if (hi + 2.0 < (double)xe) xe = (int)(hi + 2.0);
        }

        for (int x = xs; x <= xe; x++) {
            float px = (float)x + 0.5f;
            float w0 = ((b->x - px) * (c->y - py) - (b->y - py) * (c->x - px)) * inv_area;
            float w1 = ((c->x - px) * (a->y - py) - (c->y - py) * (a->x - px)) * inv_area;
            float w2 = 1.0f - w0 - w1;
            if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f) continue;

            float z = w0 * a->z + w1 * b->z + w2 * c->z;   // 1/w, bigger is nearer
            int p = y * rw + x;
            if (!depth_pass(m->zmode, z, zbuf[p])) continue;

            // perspective correct weights
            float q0 = w0 * a->z, q1 = w1 * b->z, q2 = w2 * c->z, qs = q0 + q1 + q2;
            if (qs != 0.0f) { q0 /= qs; q1 /= qs; q2 /= qs; }

            float col[4], off[4];
            for (int k = 0; k < 4; k++) {
                col[k] = m->gouraud ? q0 * a->col[k] + q1 * b->col[k] + q2 * c->col[k] : c->col[k];
                off[k] = m->gouraud ? q0 * a->off[k] + q1 * b->off[k] + q2 * c->off[k] : c->off[k];
            }
            if (!m->use_alpha) col[3] = 1.0f;
            if (m->fog == 3) {   // fog look-up mode 2: the fog colour, the table as alpha
                for (int k = 0; k < 3; k++) col[k] = job.fog_ram[k];
                col[3] = fog_coef(z);
            }

            float out[4] = { col[0], col[1], col[2], col[3] };
            if (m->textured) {
                float u = q0 * a->u + q1 * b->u + q2 * c->u, v = q0 * a->v + q1 * b->v + q2 * c->v;
                const sampler *tx = &m->tex;
                uint32_t tc32 = sample(tx, wrap(u, tx->w, tx->clamp_u, tx->flip_u), wrap(v, tx->h, tx->clamp_v, tx->flip_v));
                float tc[4] = { ((tc32 >> 16) & 0xff) / 255.0f, ((tc32 >> 8) & 0xff) / 255.0f, (tc32 & 0xff) / 255.0f,
                                (tc32 >> 24) / 255.0f };
                if (m->ignore_tex_alpha) tc[3] = 1.0f;
                switch (m->shade) {
                case 0:   // decal
                    out[0] = tc[0]; out[1] = tc[1]; out[2] = tc[2]; out[3] = tc[3];
                    break;
                case 1:   // modulate
                    out[0] = tc[0] * col[0]; out[1] = tc[1] * col[1]; out[2] = tc[2] * col[2]; out[3] = tc[3];
                    break;
                case 2:   // decal alpha
                    for (int k = 0; k < 3; k++) out[k] = tc[k] * tc[3] + col[k] * (1.0f - tc[3]);
                    out[3] = col[3];
                    break;
                default:  // modulate alpha
                    for (int k = 0; k < 4; k++) out[k] = tc[k] * col[k];
                    break;
                }
                if (m->offset)   // offset colour goes on textured polygons only
                    for (int k = 0; k < 3; k++) out[k] += off[k];
            }
            if (m->clamp)
                for (int k = 0; k < 4; k++)
                    out[k] = out[k] < job.clamp_min[k] ? job.clamp_min[k] : out[k] > job.clamp_max[k] ? job.clamp_max[k] : out[k];
            if (m->fog == 0) {   // table fog
                float f = fog_coef(z);
                for (int k = 0; k < 3; k++) out[k] = out[k] * (1.0f - f) + job.fog_ram[k] * f;
            } else if (m->fog == 1 && m->offset) {   // per-vertex fog: the offset colour's alpha
                for (int k = 0; k < 3; k++) out[k] = out[k] * (1.0f - off[3]) + job.fog_vert[k] * off[3];
            }

            if (m->list == 4) {   // punch-through: alpha test on 8-bit alpha, then opaque
                float a8 = roundf(out[3] * 255.0f) / 255.0f;
                if (pt_ref > a8) continue;
                out[3] = 1.0f;
            }
            if (m->list == 2 && job.autosort) {   // sorted: blended later, with the pixel's others
                if (pool[t].n == pool[t].cap) {
                    pool[t].cap = pool[t].cap ? pool[t].cap * 2 : 65536;
                    pool[t].f = realloc(pool[t].f, (size_t)pool[t].cap * sizeof *pool[t].f);
                    if (!pool[t].f) { fprintf(stderr, "[pvr] out of memory\n"); exit(1); }
                }
                frag *f = &pool[t].f[pool[t].n];
                f->z = z;
                for (int k = 0; k < 4; k++) f->col[k] = clamp01(out[k]);
                f->src_f = (uint8_t)m->src_f;
                f->dst_f = (uint8_t)m->dst_f;
                f->shadow = m->shadow;
                f->stencil = 0;
                f->next = fhead[p];
                fhead[p] = pool[t].n++;
                continue;
            }
            if (m->list == 2) {   // translucent: blend with what's there
                uint32_t d = cbuf[p];
                float dst[4] = { ((d >> 16) & 0xff) / 255.0f, ((d >> 8) & 0xff) / 255.0f, (d & 0xff) / 255.0f, (d >> 24) / 255.0f };
                float src[4] = { clamp01(out[0]), clamp01(out[1]), clamp01(out[2]), clamp01(out[3]) };
                for (int k = 0; k < 4; k++)
                    out[k] = src[k] * blend_factor(m->src_f, 1, src, dst, k) + dst[k] * blend_factor(m->dst_f, 0, src, dst, k);
            }
            cbuf[p] = (uint32_t)(clamp01(out[3]) * 255.0f + 0.5f) << 24 | (uint32_t)(clamp01(out[0]) * 255.0f + 0.5f) << 16 |
                      (uint32_t)(clamp01(out[1]) * 255.0f + 0.5f) << 8 | (uint32_t)(clamp01(out[2]) * 255.0f + 0.5f);
            if (m->zwrite) zbuf[p] = z;
            if (m->list != 2) sbuf[p] = (uint8_t)((sbuf[p] & 0x7f) | m->shadow);
        }
    }
}

static void draw_list(int t, const ta_ctx *c, int list)
{
    for (int i = 0; i < c->ns; i++) {
        const strip *s = &c->s[i];
        if (s->list != list) continue;
        strip_mode m;
        strip_mode_init(&m, s);
        for (int k = 0; k + 2 < s->count; k++)
            triangle(t, &m, &c->v[s->first + k], &c->v[s->first + k + 1], &c->v[s->first + k + 2]);
    }
}

// the background plane: ISP_BACKGND_T points at an ISP/TSP/TCW and three
// vertices in the parameter memory. Only its colour is used, as the clear colour
static uint32_t background_colour(void)
{
    uint32_t bgt = holly_reg(PVR_ISP_BACKGND_T);
    uint32_t addr = (holly_reg(PVR_PARAM_BASE) & 0x00f00000u) + ((bgt >> 3) & 0x1fffffu) * 4u;
    uint32_t isp = v64_r32(addr);
    int col = 3 + ((isp >> 25) & 1 ? ((isp >> 22) & 1 ? 1 : 2) : 0);
    return v64_r32(addr + 12u + (uint32_t)col * 4u);
}

static void write_fb_row(int y)
{
    for (int x = 0; x < job.ow; x++) {
        uint32_t c = cbuf[y * rw + x];
        if (job.hscale) {   // average two pixels
            uint32_t d = cbuf[y * rw + 2 * x + 1];
            c = cbuf[y * rw + 2 * x];
            c = ((c >> 1) & 0x7f7f7f7fu) + ((d >> 1) & 0x7f7f7f7fu);
        }
        uint32_t a = c >> 24, r = (c >> 16) & 0xff, g = (c >> 8) & 0xff, b = c & 0xff, px;
        switch (job.mode) {
        case 0: px = (uint32_t)job.k << 15 | (r >> 3) << 10 | (g >> 3) << 5 | b >> 3; break;
        case 1: px = (r >> 3) << 11 | (g >> 2) << 5 | b >> 3; break;
        case 2: px = (a >> 4) << 12 | (r >> 4) << 8 | (g >> 4) << 4 | b >> 4; break;
        case 3: px = (a >= job.threshold ? 0x8000u : 0) | (r >> 3) << 10 | (g >> 3) << 5 | b >> 3; break;
        case 5: px = (job.k ? 0xff000000u : 0) | (c & 0xffffffu); break;
        default: px = c; break;
        }
        uint32_t o = (job.base + (uint32_t)y * job.stride + (uint32_t)(x * job.bpp)) & (DC_VRAM_SIZE - 1);
        if (o + 4 > DC_VRAM_SIZE) continue;
        memcpy(&dc_vram[o], &px, (size_t)job.bpp);
    }
}

// ---- modifier volumes ----
//
// As Flycast draws them with the stencil buffer (gles/gldraw.cpp). Each
// volume's triangles that are in front of what's drawn flip bit 1 (XOR: a
// closed volume, so an odd count means the surface is inside), or set it (OR:
// an open one). The polygon that closes a volume says how it combines with
// the volumes before it: "inside last" folds bit 1 into bit 0 (inclusion),
// "outside last" keeps bit 0 only where bit 1 is clear (exclusion). Then a
// pixel with bits 7 and 0 set is in shadow.

enum { MV_XOR, MV_OR, MV_INCL, MV_EXCL };

// edges belong to one side only: a pixel centre exactly on an edge two
// triangles share is counted once, which XOR needs
static int edge_in(float e, float dx, float dy)
{
    return e > 0.0f || (e == 0.0f && (dy > 0.0f || (dy == 0.0f && dx < 0.0f)));
}

static uint8_t mv_op(uint8_t s, int op, int in_front)
{
    switch (op) {
    case MV_XOR: return in_front ? (uint8_t)(s ^ 2) : s;
    case MV_OR: return in_front ? (uint8_t)(s | 2) : s;
    case MV_INCL: return (uint8_t)((s & ~3) | ((s & 3) >= 1 ? 1 : 0));
    default: return (s & 3) == 1 ? s : (uint8_t)(s & ~3);
    }
}

// on_frags: act on the pixel's translucent fragments (the translucent list's
// volumes) instead of the opaque pixel
static void mv_triangle(int t, const vtx *a, const vtx *b, const vtx *c, int op, int on_frags)
{
    float area = (b->x - a->x) * (c->y - a->y) - (b->y - a->y) * (c->x - a->x);
    if (!(fabsf(area) > 1e-6f) || !isfinite(area)) return;
    if (area < 0.0f) { const vtx *tmp = b; b = c; c = tmp; area = -area; }

    float fminx = fminf(a->x, fminf(b->x, c->x)), fmaxx = fmaxf(a->x, fmaxf(b->x, c->x));
    float fminy = fminf(a->y, fminf(b->y, c->y)), fmaxy = fmaxf(a->y, fmaxf(b->y, c->y));
    if (fmaxx < (float)clip_x0 || fminx > (float)clip_x1 + 1 || fmaxy < (float)clip_y0 || fminy > (float)clip_y1 + 1) return;
    int x0 = fminx < (float)clip_x0 ? clip_x0 : (int)fminx;
    int x1 = fmaxx > (float)clip_x1 ? clip_x1 : (int)fmaxx;
    int y0 = fminy < (float)clip_y0 ? clip_y0 : (int)fminy;
    int y1 = fmaxy > (float)clip_y1 ? clip_y1 : (int)fmaxy;
    float inv_area = 1.0f / area;
    const vtx *v[3] = { a, b, c };

    for (int y = y0; y <= y1; y++) {
        if (!band_of(t, y)) { y |= (1 << BAND_SHIFT) - 1; continue; }
        float py = (float)y + 0.5f;
        for (int x = x0; x <= x1; x++) {
            float px = (float)x + 0.5f, e[3];
            int in = 1;
            for (int i = 0; i < 3 && in; i++) {   // e[i]: edge from v[i] to the next, 0 there, area at the third
                const vtx *p0 = v[i], *p1 = v[(i + 1) % 3];
                float dx = p1->x - p0->x, dy = p1->y - p0->y;
                e[i] = dx * (py - p0->y) - dy * (px - p0->x);
                in = edge_in(e[i], dx, dy);
            }
            if (!in) continue;
            int p = y * rw + x;
            float z = (e[1] * a->z + e[2] * b->z + e[0] * c->z) * inv_area;
            if (on_frags) {   // Flycast's OIT: a fragment at or behind the volume's face
                for (int i = fhead[p]; i >= 0; i = pool[t].f[i].next)
                    pool[t].f[i].stencil = mv_op(pool[t].f[i].stencil, op, z >= pool[t].f[i].z);
            } else {          // in front of what's there (the PVR's "greater" on 1/w)
                sbuf[p] = mv_op(sbuf[p], op, z > zbuf[p]);
            }
        }
    }
}

static void mv_triangles(int t, const ta_ctx *c, int first, int count, int op, int on_frags)
{
    for (int i = first; i < first + count; i++)
        mv_triangle(t, &c->mv[3 * i], &c->mv[3 * i + 1], &c->mv[3 * i + 2], op, on_frags);
}

// list 1 (opaque) acts on the opaque pixels, list 3 on the translucent fragments
static void run_modvols(int t, const ta_ctx *c, int list)
{
    int base = -1;
    for (int i = 0; i < c->nm; i++) {
        const pvr_modvol *m = &c->m[i];
        if (m->list != list || !m->count) continue;
        int mode = (int)(m->isp >> 29);
        if (base < 0) base = m->first;
        mv_triangles(t, c, m->first, m->count, !m->last && mode > 0 ? MV_OR : MV_XOR, list == 3);
        if (mode == 1 || mode == 2) {
            mv_triangles(t, c, base, m->first + m->count - base, mode == 1 ? MV_INCL : MV_EXCL, list == 3);
            base = -1;
        }
    }
}

// each pixel's translucent fragments, back to front (ties in the order sent)
static void resolve_fragments(int t)
{
    frag *f = pool[t].f;
    for (int y = 0; y < rh; y++) {
        if (!band_of(t, y)) { y |= (1 << BAND_SHIFT) - 1; continue; }
        for (int x = 0; x < rw; x++) {
            int p = y * rw + x, n = 0;
            if (fhead[p] < 0) continue;
            for (int i = fhead[p]; i >= 0; i = f[i].next) n++;
            if (n > pool[t].order_cap) {
                pool[t].order_cap = n * 2;
                pool[t].order = realloc(pool[t].order, (size_t)pool[t].order_cap * sizeof *pool[t].order);
                if (!pool[t].order) { fprintf(stderr, "[pvr] out of memory\n"); exit(1); }
            }
            int *o = pool[t].order, k = n;
            for (int i = fhead[p]; i >= 0; i = f[i].next) o[--k] = i;   // in the order sent
            for (int i = 1; i < n; i++) {   // stable: farthest (smallest 1/w) first
                int key = o[i], j = i - 1;
                while (j >= 0 && f[o[j]].z > f[key].z) { o[j + 1] = o[j]; j--; }
                o[j + 1] = key;
            }
            uint32_t d = cbuf[p];
            for (int i = 0; i < n; i++) {
                const frag *g = &f[o[i]];
                float dst[4] = { ((d >> 16) & 0xff) / 255.0f, ((d >> 8) & 0xff) / 255.0f, (d & 0xff) / 255.0f, (d >> 24) / 255.0f };
                float src[4] = { g->col[0], g->col[1], g->col[2], g->col[3] }, out[4];
                if (job.shadow_on && g->shadow && (g->stencil & 1))
                    for (int c = 0; c < 3; c++) src[c] *= job.shadow;
                for (int c = 0; c < 4; c++)
                    out[c] = src[c] * blend_factor(g->src_f, 1, src, dst, c) + dst[c] * blend_factor(g->dst_f, 0, src, dst, c);
                d = (uint32_t)(clamp01(out[3]) * 255.0f + 0.5f) << 24 | (uint32_t)(clamp01(out[0]) * 255.0f + 0.5f) << 16 |
                    (uint32_t)(clamp01(out[1]) * 255.0f + 0.5f) << 8 | (uint32_t)(clamp01(out[2]) * 255.0f + 0.5f);
            }
            cbuf[p] = d;
        }
    }
}

static void draw_modvols(int t, const ta_ctx *c)
{
    run_modvols(t, c, 1);
    if (!job.shadow_on) return;   // two-volume mode: the polygons' second colours, not drawn yet
    for (int y = 0; y < rh; y++) {
        if (!band_of(t, y)) { y |= (1 << BAND_SHIFT) - 1; continue; }
        for (int x = 0; x < rw; x++) {
            int p = y * rw + x;
            if ((sbuf[p] & 0x81) != 0x81) continue;
            uint32_t col = cbuf[p];
            uint32_t r = (uint32_t)(((col >> 16) & 0xff) * job.shadow + 0.5f);
            uint32_t g = (uint32_t)(((col >> 8) & 0xff) * job.shadow + 0.5f);
            uint32_t b = (uint32_t)((col & 0xff) * job.shadow + 0.5f);
            cbuf[p] = (col & 0xff000000u) | r << 16 | g << 8 | b;
        }
    }
}

// thread t's share of a render: clear, draw, and (when the rows don't
// overlap in VRAM) write out its rows
static void draw_band(int t)
{
    for (int y = 0; y < rh; y++) {
        if (!band_of(t, y)) { y |= (1 << BAND_SHIFT) - 1; continue; }
        uint32_t *cr = cbuf + (size_t)y * rw;
        float *zr = zbuf + (size_t)y * rw;
        for (int x = 0; x < rw; x++) { cr[x] = job.bg; zr[x] = job.bgz; fhead[(size_t)y * rw + x] = -1; }
        memset(sbuf + (size_t)y * rw, 0, (size_t)rw);
    }
    pool[t].n = 0;
    draw_list(t, job.c, 0);   // opaque
    draw_list(t, job.c, 4);   // punch-through
    draw_modvols(t, job.c);   // the opaque list's modifier volumes, and the shadows they cast
    draw_list(t, job.c, 2);   // translucent: sorted per pixel (auto-sort), or blended in the order sent
    if (job.autosort) {
        run_modvols(t, job.c, 3);   // the translucent list's volumes, on each fragment
        resolve_fragments(t);
    }
    if (job.rows_apart)
        for (int y = 0; y < rh; y++) {
            if (!band_of(t, y)) { y |= (1 << BAND_SHIFT) - 1; continue; }
            write_fb_row(y);
        }
}

static SDL_Semaphore *go[MAX_THREADS], *done;

static int worker(void *arg)
{
    int t = (int)(intptr_t)arg;
    for (;;) {
        SDL_WaitSemaphore(go[t]);
        draw_band(t);
        SDL_SignalSemaphore(done);
    }
    return 0;
}

// DC_RENDER_THREADS=n: draw on n threads (default: one per logical core)
static void start_threads(void)
{
    init_spread();
    const char *e = getenv("DC_RENDER_THREADS");
    int n = e ? atoi(e) : SDL_GetNumLogicalCPUCores();
    if (n < 1) n = 1;
    if (n > MAX_THREADS) n = MAX_THREADS;
    nthreads = 1;
    done = SDL_CreateSemaphore(0);
    for (int t = 1; t < n && done; t++) {
        go[t] = SDL_CreateSemaphore(0);
        SDL_Thread *th = go[t] ? SDL_CreateThread(worker, "pvr", (void *)(intptr_t)t) : NULL;
        if (!th) break;
        SDL_DetachThread(th);
        nthreads++;
    }
    fprintf(stderr, "[pvr] drawing on %d thread%s\n", nthreads, nthreads == 1 ? "" : "s");
}

static void dump_render(const ta_ctx *c)
{
    FILE *f = fopen("pvr_dump.txt", "w");
    if (!f) return;
    fprintf(f, "fpu_shad_scale %08x\n", holly_reg(PVR_FPU_SHAD_SCALE));
    fprintf(f, "fog col_ram %08x col_vert %08x density %08x clamp_max %08x clamp_min %08x\n", holly_reg(PVR_FOG_COL_RAM),
            holly_reg(PVR_FOG_COL_VERT), holly_reg(PVR_FOG_DENSITY), holly_reg(PVR_FOG_CLAMP_MAX), holly_reg(PVR_FOG_CLAMP_MIN));
    fprintf(f, "fog table");
    for (uint32_t i = 0; i < 128; i++) fprintf(f, " %04x", holly_reg(PVR_FOG_TABLE + 4 * i) & 0xffff);
    fprintf(f, "\n");
    fprintf(f, "isp_feed_cfg %08x fpu_param_cfg %08x region_base %08x\n", holly_reg(PVR_ISP_FEED_CFG),
            holly_reg(PVR_FPU_PARAM_CFG), holly_reg(PVR_REGION_BASE));
    for (int i = 0; i < 12; i++) fprintf(f, "region word %d: %08x\n", i, v64_r32(holly_reg(PVR_REGION_BASE) + 4u * (uint32_t)i));
    for (int i = 0; i < c->nm; i++) {
        const pvr_modvol *m = &c->m[i];
        fprintf(f, "modvol %d list %d isp %08x last %d count %d\n", i, m->list, m->isp, m->last, m->count);
        for (int k = 0; k < m->count * 3; k++) {
            const vtx *v = &c->mv[m->first * 3 + k];
            fprintf(f, "  %12g %12g %12g\n", v->x, v->y, v->z);
        }
    }
    for (int i = 0; i < c->ns; i++) {
        const strip *s = &c->s[i];
        fprintf(f, "strip %d list %d pcw %08x isp %08x tsp %08x tcw %08x count %d\n", i, s->list, s->pcw, s->isp, s->tsp, s->tcw, s->count);
        for (int k = 0; k < s->count; k++) {
            const vtx *v = &c->v[s->first + k];
            fprintf(f, "  %12g %12g %12g  uv %9g %9g  col %.2f %.2f %.2f %.2f\n", v->x, v->y, v->z, v->u, v->v,
                    v->col[0], v->col[1], v->col[2], v->col[3]);
        }
    }
    fclose(f);
}

// the fog registers into job, and the scale the density stands for
static void load_fog(void)
{
    uint32_t d = holly_reg(PVR_FOG_DENSITY);
    job.fog_density = (float)((d >> 8) & 0xff) / 128.0f * ldexpf(1.0f, (int8_t)(d & 0xff));
    float ram[4], vert[4];
    argb_to_float(holly_reg(PVR_FOG_COL_RAM), ram);
    argb_to_float(holly_reg(PVR_FOG_COL_VERT), vert);
    for (int k = 0; k < 3; k++) { job.fog_ram[k] = ram[k]; job.fog_vert[k] = vert[k]; }
    argb_to_float(holly_reg(PVR_FOG_CLAMP_MIN), job.clamp_min);
    argb_to_float(holly_reg(PVR_FOG_CLAMP_MAX), job.clamp_max);
    for (uint32_t i = 0; i < 128; i++) {
        uint32_t e = holly_reg(PVR_FOG_TABLE + 4 * i);
        job.fog_hi[i] = (uint8_t)(e >> 8);
        job.fog_lo[i] = (uint8_t)e;
    }
}

static void render(void)
{
    if (!nthreads) start_threads();

    uint32_t key = holly_reg(PVR_PARAM_BASE) & 0x00f00000u;
    ta_ctx *c = cur;
    for (int i = 0; i < MAX_CTX; i++)
        if (ctxs[i].key == key && (ctxs[i].nv || ctxs[i].ns)) c = &ctxs[i];

    uint32_t xc = holly_reg(PVR_FB_X_CLIP), yc = holly_reg(PVR_FB_Y_CLIP);
    int hscale = (holly_reg(PVR_SCALER_CTL) >> 16) & 1;
    clip_x0 = (int)(xc & 0x7ff);
    clip_x1 = (int)((xc >> 16) & 0x7ff);
    clip_y0 = (int)(yc & 0x3ff);
    clip_y1 = (int)((yc >> 16) & 0x3ff);
    if (hscale) { clip_x0 *= 2; clip_x1 = clip_x1 * 2 + 1; }
    rw = clip_x1 + 1;
    rh = clip_y1 + 1;
    if (rw > 2048) rw = 2048;
    if (rh > 1024) rh = 1024;
    if (clip_x1 >= rw) clip_x1 = rw - 1;
    if (clip_y1 >= rh) clip_y1 = rh - 1;
    if (rw * rh > cap_pixels) {
        cap_pixels = rw * rh;
        cbuf = realloc(cbuf, (size_t)cap_pixels * sizeof *cbuf);
        zbuf = realloc(zbuf, (size_t)cap_pixels * sizeof *zbuf);
        sbuf = realloc(sbuf, (size_t)cap_pixels);
        fhead = realloc(fhead, (size_t)cap_pixels * sizeof *fhead);
        if (!cbuf || !zbuf || !sbuf || !fhead) { fprintf(stderr, "[pvr] out of memory\n"); exit(1); }
    }

    if ((long)renders == pvrdump) dump_render(c);

    load_palette();
    load_fog();
    pt_ref = (float)(holly_reg(PVR_PT_ALPHA_REF) & 0xff) / 255.0f;
    uint32_t ctrl = holly_reg(PVR_FB_W_CTRL);
    job.c = c;
    job.bg = background_colour();
    job.bgz = as_float(holly_reg(PVR_ISP_BACKGND_D));

    // On the GPU when what's drawn goes to the screen. A render to anywhere
    // else (a texture, say) is drawn here, into VRAM, where the game looks.
    int on_gpu = 0;
#ifdef DC_GPU
    uint32_t fbw = holly_reg(PVR_FB_W_SOF1) & 0x00fffffcu;
    if (gpu_active() && was_displayed(fbw)) {
        pvr_frame f = {
            .v = c->v, .nv = c->nv, .s = c->s, .ns = c->ns, .width = rw, .height = rh,
            .clip_x0 = clip_x0, .clip_x1 = clip_x1, .clip_y0 = clip_y0, .clip_y1 = clip_y1,
            .bg = job.bg, .bgz = job.bgz, .pt_ref = pt_ref, .fb = fbw, .hscale = hscale,
            .mv = c->mv, .nmv = c->nmv, .m = c->m, .nm = c->nm,
            .autosort = !(holly_reg(PVR_ISP_FEED_CFG) & 1),
            .fog_density = job.fog_density,
            .fog_col_ram = holly_reg(PVR_FOG_COL_RAM), .fog_col_vert = holly_reg(PVR_FOG_COL_VERT),
            .clamp_max = holly_reg(PVR_FOG_CLAMP_MAX), .clamp_min = holly_reg(PVR_FOG_CLAMP_MIN),
            .shadow_on = (holly_reg(PVR_FPU_SHAD_SCALE) >> 8) & 1,
            .shadow = (float)(holly_reg(PVR_FPU_SHAD_SCALE) & 0xff) / 256.0f,
        };
        for (uint32_t i = 0; i < 128; i++) f.fog_table[i] = (uint16_t)holly_reg(PVR_FOG_TABLE + 4 * i);
        gpu_render(&f);
        on_gpu = 1;
    }
#endif
    job.hscale = hscale;
    job.base = holly_reg(PVR_FB_W_SOF1) & 0x00fffffcu;
    job.stride = (holly_reg(PVR_FB_W_LINESTRIDE) & 0x1ffu) * 8u;
    job.mode = ctrl & 7;
    job.k = (ctrl >> 16) & 0x80 ? 1 : 0;
    job.threshold = (ctrl >> 8) & 0xff;
    job.bpp = job.mode < 4 ? 2 : job.mode == 4 ? 3 : 4;
    job.ow = hscale ? rw / 2 : rw;
    // rows that overlap in VRAM have to go out in order, so on this thread
    job.rows_apart = job.stride >= (uint32_t)(job.ow * job.bpp);
    job.shadow_on = (holly_reg(PVR_FPU_SHAD_SCALE) >> 8) & 1;
    job.shadow = (float)(holly_reg(PVR_FPU_SHAD_SCALE) & 0xff) / 256.0f;
    job.autosort = !(holly_reg(PVR_ISP_FEED_CFG) & 1);

    if (!on_gpu) {
        for (int t = 1; t < nthreads; t++) SDL_SignalSemaphore(go[t]);
        draw_band(0);
        for (int t = 1; t < nthreads; t++) SDL_WaitSemaphore(done);
        if (!job.rows_apart)
            for (int y = 0; y < rh; y++) write_fb_row(y);
    }

    if (pvrlog > 0) {
        int n[5] = { 0 };
        for (int i = 0; i < c->ns; i++) n[c->s[i].list]++;
        fprintf(stderr, "[pvr] render %u%s: %d strips (op %d pt %d tr %d), %d verts, fb %08x %dx%d mode %u stride %u, "
                "bg %08x, scaler %08x%s\n", renders, on_gpu ? " (gpu)" : "", c->ns, n[0], n[4], n[2], c->nv,
                holly_reg(PVR_FB_W_SOF1), hscale ? rw / 2 : rw, rh, holly_reg(PVR_FB_W_CTRL) & 7,
                (holly_reg(PVR_FB_W_LINESTRIDE) & 0x1ff) * 8, job.bg, holly_reg(PVR_SCALER_CTL),
                skipped_units ? " (skipped some TA params)" : "");
        pvrlog--;
    }
    renders++;
}

void pvr_reg_written(uint32_t address, uint32_t value)
{
    switch (address) {
    case PVR_SB_C2DST:
        if (value & 1) ch2_dma();
        break;
    case PVR_TA_LIST_INIT:
        if (value & 0x80000000u) list_init();
        break;
    case PVR_TA_LIST_CONT:
        if (value & 0x80000000u) { ta.list = -1; ta.open = -1; ta.nw = 0; }
        break;
    case PVR_STARTRENDER: {
        uint64_t t0 = clock_host_ns();
        render();
        prof_add(PROF_RENDER, t0);
        holly_raise(7u);   // render done (video, ISP, TSP)
        break;
    }
    case PVR_SOFTRESET:
        if (value & 1) { ta.list = -1; ta.open = -1; ta.nw = 0; }
        break;
    }
}
