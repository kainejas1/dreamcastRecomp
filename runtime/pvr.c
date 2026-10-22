#include "pvr.h"
#include "holly.h"
#include "memory.h"

#include <math.h>
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

typedef struct {
    float x, y, z, u, v;
    float col[4], off[4];   // r g b a, 0..1
} vtx;

typedef struct {
    uint32_t isp, tsp, tcw;
    int list, first, count;   // a triangle strip
} strip;

typedef struct {
    uint32_t key;             // TA_ISP_BASE it was started at
    vtx *v;
    int nv, capv;
    strip *s;
    int ns, caps;
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
        if (is_modvol(ta.list)) { ta.vtype = 17; break; }

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
        if (ta.list < 0 || is_modvol(ta.list)) break;   // modifier volumes: not drawn yet
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

// texel order: x and y bits interleaved (y in bit 0), in min(w, h) squares
static uint32_t twiddle(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    uint32_t m = w < h ? w : h, r = 0;
    for (uint32_t b = 0; (1u << b) < m; b++)
        r |= ((y >> b) & 1u) << (2 * b) | ((x >> b) & 1u) << (2 * b + 1);
    if (w > h) r += (x / m) * m * m;
    else if (h > w) r += (y / m) * m * m;
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

static uint32_t palette(uint32_t i)
{
    uint32_t p = holly_reg(PVR_PALETTE + 4 * (i & 1023));
    switch (holly_reg(PVR_PAL_RAM_CTRL) & 3) {
    case 0: return expand16((uint16_t)p, 0);
    case 1: return expand16((uint16_t)p, 1);
    case 2: return expand16((uint16_t)p, 2);
    default: return p;   // 8888
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

// ARGB8888 of texel (x, y), already wrapped to the texture
static uint32_t texel(uint32_t tcw, int x, int y, int w, int h)
{
    uint32_t addr = (tcw & 0x1fffffu) << 3;
    int fmt = (tcw >> 27) & 7, vq = (tcw >> 30) & 1, mip = (tcw >> 31) & 1;
    int lw = 0;
    while ((1 << lw) < w) lw++;

    if (vq) {   // 2048-byte codebook of 2x2 blocks, then a byte per block
        uint32_t idx = addr + 2048 + (mip ? vq_mip[lw] : 0);
        uint8_t code = v64_r8(idx + twiddle((uint32_t)x >> 1, (uint32_t)y >> 1, (uint32_t)w >> 1, (uint32_t)h >> 1));
        uint16_t t = v64_r16(addr + code * 8u + (uint32_t)(((x & 1) << 1 | (y & 1)) * 2));
        return expand16(t, fmt);
    }
    if (fmt == 5) {   // 4 bpp palette
        uint32_t i = twiddle((uint32_t)x, (uint32_t)y, (uint32_t)w, (uint32_t)h);
        uint8_t b = v64_r8(addr + (mip ? other_mip[lw] / 2 : 0) + i / 2);
        return palette((((tcw >> 21) & 0x3fu) << 4) + ((i & 1) ? b >> 4 : b & 15u));
    }
    if (fmt == 6) {   // 8 bpp palette
        uint32_t i = twiddle((uint32_t)x, (uint32_t)y, (uint32_t)w, (uint32_t)h);
        return palette((((tcw >> 25) & 3u) << 8) + v64_r8(addr + (mip ? other_mip[lw] : 0) + i));
    }

    int twiddled = !((tcw >> 26) & 1);
    int stride = w;
    if (!twiddled && ((tcw >> 25) & 1)) stride = (int)(holly_reg(PVR_TEXT_CONTROL) & 31) * 32;
    uint32_t base = addr + (mip ? other_mip[lw] * 2 : 0);
    if (fmt == 3) {   // YUV422: U Y0 V Y1 for each pair of texels
        uint32_t pair = twiddled ? twiddle((uint32_t)x & ~1u, (uint32_t)y, (uint32_t)w, (uint32_t)h)
                                 : (uint32_t)(y * stride + (x & ~1));
        uint32_t d = v64_r32(base + pair * 2);
        int u = d & 0xff, y0 = (d >> 8) & 0xff, v = (d >> 16) & 0xff, y1 = d >> 24;
        return yuv(x & 1 ? y1 : y0, u, v);
    }
    if (fmt == 4) return 0xff808080u;   // bump map: not shaded
    uint32_t i = twiddled ? twiddle((uint32_t)x, (uint32_t)y, (uint32_t)w, (uint32_t)h) : (uint32_t)(y * stride + x);
    return expand16(v64_r16(base + i * 2), fmt);
}

static int wrap(float t, int size, int clamp, int flip)
{
    int i = (int)floorf(t * (float)size);
    if (clamp) return i < 0 ? 0 : i >= size ? size - 1 : i;
    if (flip) {
        int p = ((i % (2 * size)) + 2 * size) % (2 * size);
        return p < size ? p : 2 * size - 1 - p;
    }
    return ((i % size) + size) % size;
}

// ---- rendering ----

static uint32_t *cbuf;   // ARGB8888
static float *zbuf;
static int rw, rh, cap_pixels;
static int clip_x0, clip_x1, clip_y0, clip_y1;

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

static void triangle(const strip *s, const vtx *a, const vtx *b, const vtx *c)
{
    float area = (b->x - a->x) * (c->y - a->y) - (b->y - a->y) * (c->x - a->x);
    if (!(fabsf(area) > 1e-6f)) return;   // also catches NaN

    float fminx = fminf(a->x, fminf(b->x, c->x)), fmaxx = fmaxf(a->x, fmaxf(b->x, c->x));
    float fminy = fminf(a->y, fminf(b->y, c->y)), fmaxy = fmaxf(a->y, fmaxf(b->y, c->y));
    if (fmaxx < (float)clip_x0 || fminx > (float)clip_x1 + 1 || fmaxy < (float)clip_y0 || fminy > (float)clip_y1 + 1) return;
    int x0 = fminx < (float)clip_x0 ? clip_x0 : (int)fminx;
    int x1 = fmaxx > (float)clip_x1 ? clip_x1 : (int)fmaxx;
    int y0 = fminy < (float)clip_y0 ? clip_y0 : (int)fminy;
    int y1 = fmaxy > (float)clip_y1 ? clip_y1 : (int)fmaxy;

    uint32_t isp = s->isp, tsp = s->tsp, tcw = s->tcw;
    int textured = (isp >> 25) & 1, offset = (isp >> 24) & 1, gouraud = (isp >> 23) & 1;
    int zmode = (int)(isp >> 29), zwrite = !((isp >> 26) & 1);
    int tw = 8 << ((tsp >> 3) & 7), th = 8 << (tsp & 7);
    int use_alpha = (tsp >> 20) & 1, ignore_tex_alpha = (tsp >> 19) & 1;
    int shade = (tsp >> 6) & 3;
    int src_f = (int)(tsp >> 29), dst_f = (int)((tsp >> 26) & 7);
    float pt_ref = (float)(holly_reg(PVR_PT_ALPHA_REF) & 0xff) / 255.0f;
    float inv_area = 1.0f / area;

    for (int y = y0; y <= y1; y++) {
        float py = (float)y + 0.5f;
        for (int x = x0; x <= x1; x++) {
            float px = (float)x + 0.5f;
            float w0 = ((b->x - px) * (c->y - py) - (b->y - py) * (c->x - px)) * inv_area;
            float w1 = ((c->x - px) * (a->y - py) - (c->y - py) * (a->x - px)) * inv_area;
            float w2 = 1.0f - w0 - w1;
            if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f) continue;

            float z = w0 * a->z + w1 * b->z + w2 * c->z;   // 1/w, bigger is nearer
            int p = y * rw + x;
            if (!depth_pass(zmode, z, zbuf[p])) continue;

            // perspective correct weights
            float q0 = w0 * a->z, q1 = w1 * b->z, q2 = w2 * c->z, qs = q0 + q1 + q2;
            if (qs != 0.0f) { q0 /= qs; q1 /= qs; q2 /= qs; }

            float col[4], off[4];
            for (int k = 0; k < 4; k++) {
                col[k] = gouraud ? q0 * a->col[k] + q1 * b->col[k] + q2 * c->col[k] : c->col[k];
                off[k] = gouraud ? q0 * a->off[k] + q1 * b->off[k] + q2 * c->off[k] : c->off[k];
            }
            if (!use_alpha) col[3] = 1.0f;

            float out[4] = { col[0], col[1], col[2], col[3] };
            if (textured) {
                float u = q0 * a->u + q1 * b->u + q2 * c->u, v = q0 * a->v + q1 * b->v + q2 * c->v;
                int tx = wrap(u, tw, (tsp >> 16) & 1, (tsp >> 18) & 1);
                int ty = wrap(v, th, (tsp >> 15) & 1, (tsp >> 17) & 1);
                uint32_t t = texel(tcw, tx, ty, tw, th);
                float tc[4] = { ((t >> 16) & 0xff) / 255.0f, ((t >> 8) & 0xff) / 255.0f, (t & 0xff) / 255.0f, (t >> 24) / 255.0f };
                if (ignore_tex_alpha) tc[3] = 1.0f;
                switch (shade) {
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
            }
            if (offset)
                for (int k = 0; k < 3; k++) out[k] += off[k];

            if (s->list == 4 && out[3] < pt_ref) continue;   // punch-through: alpha test
            if (s->list == 2) {   // translucent: blend with what's there
                uint32_t d = cbuf[p];
                float dst[4] = { ((d >> 16) & 0xff) / 255.0f, ((d >> 8) & 0xff) / 255.0f, (d & 0xff) / 255.0f, (d >> 24) / 255.0f };
                float src[4] = { clamp01(out[0]), clamp01(out[1]), clamp01(out[2]), clamp01(out[3]) };
                for (int k = 0; k < 4; k++)
                    out[k] = src[k] * blend_factor(src_f, 1, src, dst, k) + dst[k] * blend_factor(dst_f, 0, src, dst, k);
            }
            cbuf[p] = (uint32_t)(clamp01(out[3]) * 255.0f + 0.5f) << 24 | (uint32_t)(clamp01(out[0]) * 255.0f + 0.5f) << 16 |
                      (uint32_t)(clamp01(out[1]) * 255.0f + 0.5f) << 8 | (uint32_t)(clamp01(out[2]) * 255.0f + 0.5f);
            if (zwrite) zbuf[p] = z;
        }
    }
}

static void draw_list(const ta_ctx *c, int list)
{
    for (int i = 0; i < c->ns; i++) {
        const strip *s = &c->s[i];
        if (s->list != list) continue;
        for (int k = 0; k + 2 < s->count; k++)
            triangle(s, &c->v[s->first + k], &c->v[s->first + k + 1], &c->v[s->first + k + 2]);
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

static void write_fb(int hscale)
{
    uint32_t ctrl = holly_reg(PVR_FB_W_CTRL), base = holly_reg(PVR_FB_W_SOF1) & 0x00fffffcu;
    uint32_t stride = (holly_reg(PVR_FB_W_LINESTRIDE) & 0x1ffu) * 8u;
    int mode = ctrl & 7, k = (ctrl >> 16) & 0x80 ? 1 : 0;
    uint32_t threshold = (ctrl >> 8) & 0xff;
    int bpp = mode < 4 ? 2 : mode == 4 ? 3 : 4;
    int ow = hscale ? rw / 2 : rw;

    for (int y = 0; y < rh; y++) {
        for (int x = 0; x < ow; x++) {
            uint32_t c = cbuf[y * rw + x];
            if (hscale) {   // average two pixels
                uint32_t d = cbuf[y * rw + 2 * x + 1];
                c = cbuf[y * rw + 2 * x];
                c = ((c >> 1) & 0x7f7f7f7fu) + ((d >> 1) & 0x7f7f7f7fu);
            }
            uint32_t a = c >> 24, r = (c >> 16) & 0xff, g = (c >> 8) & 0xff, b = c & 0xff, px;
            switch (mode) {
            case 0: px = (uint32_t)k << 15 | (r >> 3) << 10 | (g >> 3) << 5 | b >> 3; break;
            case 1: px = (r >> 3) << 11 | (g >> 2) << 5 | b >> 3; break;
            case 2: px = (a >> 4) << 12 | (r >> 4) << 8 | (g >> 4) << 4 | b >> 4; break;
            case 3: px = (a >= threshold ? 0x8000u : 0) | (r >> 3) << 10 | (g >> 3) << 5 | b >> 3; break;
            case 5: px = (k ? 0xff000000u : 0) | (c & 0xffffffu); break;
            default: px = c; break;
            }
            uint32_t o = (base + (uint32_t)y * stride + (uint32_t)(x * bpp)) & (DC_VRAM_SIZE - 1);
            if (o + 4 > DC_VRAM_SIZE) continue;
            memcpy(&dc_vram[o], &px, (size_t)bpp);
        }
    }
}

static void render(void)
{
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
        if (!cbuf || !zbuf) { fprintf(stderr, "[pvr] out of memory\n"); exit(1); }
    }

    uint32_t bg = background_colour();
    float bgz = as_float(holly_reg(PVR_ISP_BACKGND_D));
    for (int i = 0; i < rw * rh; i++) { cbuf[i] = bg; zbuf[i] = bgz; }

    draw_list(c, 0);   // opaque
    draw_list(c, 4);   // punch-through
    draw_list(c, 2);   // translucent, in the order sent (no sorting yet)
    write_fb(hscale);

    if (pvrlog > 0) {
        int n[5] = { 0 };
        for (int i = 0; i < c->ns; i++) n[c->s[i].list]++;
        fprintf(stderr, "[pvr] render %u: %d strips (op %d pt %d tr %d), %d verts, fb %08x %dx%d mode %u stride %u, "
                "bg %08x, scaler %08x%s\n", renders, c->ns, n[0], n[4], n[2], c->nv,
                holly_reg(PVR_FB_W_SOF1), hscale ? rw / 2 : rw, rh, holly_reg(PVR_FB_W_CTRL) & 7,
                (holly_reg(PVR_FB_W_LINESTRIDE) & 0x1ff) * 8, bg, holly_reg(PVR_SCALER_CTL),
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
    case PVR_STARTRENDER:
        render();
        holly_raise(7u);   // render done (video, ISP, TSP)
        break;
    case PVR_SOFTRESET:
        if (value & 1) { ta.list = -1; ta.open = -1; ta.nw = 0; }
        break;
    }
}
