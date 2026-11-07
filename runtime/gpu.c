#include "gpu.h"
#include "holly.h"

#include <SDL3/SDL.h>

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pvr_vert.h"   // SPIR-V, made from shaders/ by glslc at build time
#include "pvr_frag.h"

#define FORMAT SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM   // ARGB8888 words, as everything here keeps colour
#define DEPTH_SCALE (1.0f / 256.0f)   // 1/w up to 256 fits the depth range; a power of two loses no precision
#define MAX_TARGETS 4
#define STALE_FRAMES 8                // a GPU picture older than this isn't shown: VRAM may have moved on

static SDL_GPUDevice *dev;
static SDL_Window *win;
static int active, fixed_scale;
static SDL_GPUShader *vs, *fs;
static SDL_GPUTexture *white;         // bound when a strip has no texture
static SDL_GPUSampler *samplers[2][3][3];   // filter, u, v address modes
static SDL_GPUGraphicsPipeline *pipes[4096];
static SDL_GPUTextureFormat depth_format;   // with 8 stencil bits, for modifier volumes
static uint32_t renders;

// ---- setup ----

static SDL_GPUShader *shader(const unsigned char *code, size_t size, SDL_GPUShaderStage stage, int samplers_n, int uniforms)
{
    SDL_GPUShaderCreateInfo ci = {
        .code_size = size, .code = code, .entrypoint = "main", .format = SDL_GPU_SHADERFORMAT_SPIRV,
        .stage = stage, .num_samplers = (Uint32)samplers_n, .num_uniform_buffers = (Uint32)uniforms,
    };
    return SDL_CreateGPUShader(dev, &ci);
}

static SDL_GPUTexture *texture(int w, int h, SDL_GPUTextureFormat format, SDL_GPUTextureUsageFlags usage)
{
    SDL_GPUTextureCreateInfo ci = {
        .type = SDL_GPU_TEXTURETYPE_2D, .format = format, .usage = usage,
        .width = (Uint32)w, .height = (Uint32)h, .layer_count_or_depth = 1, .num_levels = 1,
    };
    return SDL_CreateGPUTexture(dev, &ci);
}

// copy w x h ARGB8888 into tex (in a copy pass of cmd)
static void upload(SDL_GPUCommandBuffer *cmd, SDL_GPUTexture *tex, const uint32_t *argb, int w, int h)
{
    SDL_GPUTransferBufferCreateInfo ti = { .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, .size = (Uint32)(w * h * 4) };
    SDL_GPUTransferBuffer *tb = SDL_CreateGPUTransferBuffer(dev, &ti);
    if (!tb) return;
    void *p = SDL_MapGPUTransferBuffer(dev, tb, false);
    if (p) {
        memcpy(p, argb, (size_t)w * h * 4);
        SDL_UnmapGPUTransferBuffer(dev, tb);
        SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cmd);
        SDL_GPUTextureTransferInfo src = { .transfer_buffer = tb, .pixels_per_row = (Uint32)w, .rows_per_layer = (Uint32)h };
        SDL_GPUTextureRegion dst = { .texture = tex, .w = (Uint32)w, .h = (Uint32)h, .d = 1 };
        SDL_UploadToGPUTexture(cp, &src, &dst, false);
        SDL_EndGPUCopyPass(cp);
    }
    SDL_ReleaseGPUTransferBuffer(dev, tb);   // freed once the GPU is done with it
}

int gpu_init(SDL_Window *window)
{
    const char *r = getenv("DC_RENDERER");
    if (r && strcmp(r, "gpu") != 0) return 0;
    if (!window && !r) return 0;   // headless runs stay on the software renderer unless asked
    if (getenv("DC_SCALE")) fixed_scale = atoi(getenv("DC_SCALE"));

    dev = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV, getenv("DC_GPUDEBUG") != NULL, NULL);
    if (!dev) {
        fprintf(stderr, "[gpu] no Vulkan device (%s), drawing in software\n", SDL_GetError());
        return 0;
    }
    if (!SDL_GPUTextureSupportsFormat(dev, FORMAT, SDL_GPU_TEXTURETYPE_2D,
                                      SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET)) {
        fprintf(stderr, "[gpu] BGRA8 textures not supported, drawing in software\n");
        SDL_DestroyGPUDevice(dev);
        dev = NULL;
        return 0;
    }
    depth_format = SDL_GPUTextureSupportsFormat(dev, SDL_GPU_TEXTUREFORMAT_D32_FLOAT_S8_UINT, SDL_GPU_TEXTURETYPE_2D,
                                                SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET)
                 ? SDL_GPU_TEXTUREFORMAT_D32_FLOAT_S8_UINT : SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT;
    if (window) {
        if (!SDL_ClaimWindowForGPUDevice(dev, window)) {
            fprintf(stderr, "[gpu] can't draw to the window (%s), drawing in software\n", SDL_GetError());
            SDL_DestroyGPUDevice(dev);
            dev = NULL;
            return 0;
        }
        SDL_GPUPresentMode mode = SDL_WindowSupportsGPUPresentMode(dev, window, SDL_GPU_PRESENTMODE_MAILBOX)
                                ? SDL_GPU_PRESENTMODE_MAILBOX : SDL_GPU_PRESENTMODE_VSYNC;
        SDL_SetGPUSwapchainParameters(dev, window, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, mode);
        win = window;
    }

    vs = shader(pvr_vert_spv, sizeof pvr_vert_spv, SDL_GPU_SHADERSTAGE_VERTEX, 0, 1);
    fs = shader(pvr_frag_spv, sizeof pvr_frag_spv, SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 1);
    white = texture(1, 1, FORMAT, SDL_GPU_TEXTUREUSAGE_SAMPLER);
    if (!vs || !fs || !white) {
        fprintf(stderr, "[gpu] setup failed (%s), drawing in software\n", SDL_GetError());
        return 0;
    }
    static const SDL_GPUSamplerAddressMode modes[3] = {
        SDL_GPU_SAMPLERADDRESSMODE_REPEAT, SDL_GPU_SAMPLERADDRESSMODE_MIRRORED_REPEAT, SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
    };
    for (int f = 0; f < 2; f++)
        for (int u = 0; u < 3; u++)
            for (int v = 0; v < 3; v++) {
                SDL_GPUSamplerCreateInfo si = {
                    .min_filter = f ? SDL_GPU_FILTER_LINEAR : SDL_GPU_FILTER_NEAREST,
                    .mag_filter = f ? SDL_GPU_FILTER_LINEAR : SDL_GPU_FILTER_NEAREST,
                    .mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST,
                    .address_mode_u = modes[u], .address_mode_v = modes[v], .address_mode_w = modes[0],
                };
                samplers[f][u][v] = SDL_CreateGPUSampler(dev, &si);
            }
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(dev);
    uint32_t px = 0xffffffffu;
    upload(cmd, white, &px, 1, 1);
    SDL_SubmitGPUCommandBuffer(cmd);

    active = 1;
    fprintf(stderr, "[gpu] drawing on %s (%s)\n", SDL_GetGPUDeviceDriver(dev), window ? "window" : "offscreen");
    return 1;
}

int gpu_active(void) { return active; }

// ---- pipelines: one per list kind, depth mode and blend ----

static SDL_GPUBlendFactor blend(int f, int is_src)
{
    switch (f) {
    case 0: return SDL_GPU_BLENDFACTOR_ZERO;
    case 1: return SDL_GPU_BLENDFACTOR_ONE;
    case 2: return is_src ? SDL_GPU_BLENDFACTOR_DST_COLOR : SDL_GPU_BLENDFACTOR_SRC_COLOR;   // "the other colour"
    case 3: return is_src ? SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_COLOR : SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_COLOR;
    case 4: return SDL_GPU_BLENDFACTOR_SRC_ALPHA;
    case 5: return SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    case 6: return SDL_GPU_BLENDFACTOR_DST_ALPHA;
    default: return SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_ALPHA;
    }
}

static SDL_GPUGraphicsPipeline *pipeline(int translucent, int zmode, int zwrite, int src, int dst)
{
    if (!translucent) src = dst = 0;
    int key = translucent | zmode << 1 | zwrite << 4 | src << 5 | dst << 8;
    if (pipes[key]) return pipes[key];

    SDL_GPUVertexBufferDescription vb = { .slot = 0, .pitch = sizeof(pvr_vertex), .input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX };
    SDL_GPUVertexAttribute at[4] = {
        { 0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, offsetof(pvr_vertex, x) },
        { 1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(pvr_vertex, u) },
        { 2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, offsetof(pvr_vertex, col) },
        { 3, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, offsetof(pvr_vertex, off) },
    };
    SDL_GPUColorTargetDescription ct = { .format = FORMAT };
    if (translucent) {
        ct.blend_state.enable_blend = true;
        ct.blend_state.src_color_blendfactor = ct.blend_state.src_alpha_blendfactor = blend(src, 1);
        ct.blend_state.dst_color_blendfactor = ct.blend_state.dst_alpha_blendfactor = blend(dst, 0);
        ct.blend_state.color_blend_op = ct.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
    }
    SDL_GPUGraphicsPipelineCreateInfo ci = {
        .vertex_shader = vs,
        .fragment_shader = fs,
        .vertex_input_state = { .vertex_buffer_descriptions = &vb, .num_vertex_buffers = 1,
                                .vertex_attributes = at, .num_vertex_attributes = 4 },
        .primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST,
        .rasterizer_state = { .fill_mode = SDL_GPU_FILLMODE_FILL, .cull_mode = SDL_GPU_CULLMODE_NONE,
                              .front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE, .enable_depth_clip = false },
        .depth_stencil_state = { .compare_op = (SDL_GPUCompareOp)(SDL_GPU_COMPAREOP_NEVER + zmode),
                                 .enable_depth_test = true, .enable_depth_write = zwrite != 0 },
        .target_info = { .color_target_descriptions = &ct, .num_color_targets = 1,
                         .depth_stencil_format = depth_format, .has_depth_stencil_target = true },
    };
    if (!translucent) {
        // stencil bit 7 says whether shadows land on the pixel: the reference
        // (0x80 or 0) goes in wherever a pixel is drawn
        SDL_GPUStencilOpState so = { .fail_op = SDL_GPU_STENCILOP_KEEP, .pass_op = SDL_GPU_STENCILOP_REPLACE,
                                     .depth_fail_op = SDL_GPU_STENCILOP_KEEP, .compare_op = SDL_GPU_COMPAREOP_ALWAYS };
        ci.depth_stencil_state.enable_stencil_test = true;
        ci.depth_stencil_state.front_stencil_state = ci.depth_stencil_state.back_stencil_state = so;
        ci.depth_stencil_state.compare_mask = 0xff;
        ci.depth_stencil_state.write_mask = 0x80;
    }
    pipes[key] = SDL_CreateGPUGraphicsPipeline(dev, &ci);
    if (!pipes[key]) fprintf(stderr, "[gpu] pipeline %03x: %s\n", key, SDL_GetError());
    return pipes[key];
}

// ---- modifier volumes: Flycast's stencil passes (see pvr.c's draw_modvols) ----

enum { MV_XOR, MV_OR, MV_INCL, MV_EXCL, MV_SHADE };

static SDL_GPUGraphicsPipeline *mv_pipeline(int op)
{
    static SDL_GPUGraphicsPipeline *mv[5];
    if (mv[op]) return mv[op];

    SDL_GPUVertexBufferDescription vb = { .slot = 0, .pitch = sizeof(pvr_vertex), .input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX };
    SDL_GPUVertexAttribute at[4] = {
        { 0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, offsetof(pvr_vertex, x) },
        { 1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(pvr_vertex, u) },
        { 2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, offsetof(pvr_vertex, col) },
        { 3, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, offsetof(pvr_vertex, off) },
    };
    SDL_GPUColorTargetDescription ct = { .format = FORMAT };
    ct.blend_state.enable_color_write_mask = true;
    ct.blend_state.color_write_mask = 0;   // volumes only touch the stencil
    if (op == MV_SHADE) {
        // black at 1 - scale over the shadowed pixels: colour times scale
        ct.blend_state.color_write_mask = SDL_GPU_COLORCOMPONENT_R | SDL_GPU_COLORCOMPONENT_G | SDL_GPU_COLORCOMPONENT_B;
        ct.blend_state.enable_blend = true;
        ct.blend_state.src_color_blendfactor = ct.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
        ct.blend_state.dst_color_blendfactor = ct.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        ct.blend_state.color_blend_op = ct.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
    }
    SDL_GPUStencilOpState so = { .fail_op = SDL_GPU_STENCILOP_KEEP, .pass_op = SDL_GPU_STENCILOP_KEEP,
                                 .depth_fail_op = SDL_GPU_STENCILOP_KEEP, .compare_op = SDL_GPU_COMPAREOP_ALWAYS };
    Uint8 compare_mask = 0xff, write_mask = 0;
    switch (op) {
    case MV_XOR:   // faces in front of the surface flip bit 1
        so.pass_op = SDL_GPU_STENCILOP_INVERT;
        write_mask = 2;
        break;
    case MV_OR:    // or set it (reference 2)
        so.pass_op = SDL_GPU_STENCILOP_REPLACE;
        write_mask = 2;
        break;
    case MV_INCL:  // bits 1-0 = 1 if either was set, else 0 (reference 1)
        so.compare_op = SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
        so.pass_op = SDL_GPU_STENCILOP_REPLACE;
        so.fail_op = so.depth_fail_op = SDL_GPU_STENCILOP_ZERO;
        compare_mask = write_mask = 3;
        break;
    case MV_EXCL:  // bits 1-0 stay 1 only if they were exactly 1 (reference 1)
        so.compare_op = SDL_GPU_COMPAREOP_EQUAL;
        so.fail_op = so.depth_fail_op = SDL_GPU_STENCILOP_ZERO;
        compare_mask = write_mask = 3;
        break;
    default:       // shade where bits 7 and 0 are set (reference 0x81)
        so.compare_op = SDL_GPU_COMPAREOP_EQUAL;
        compare_mask = 0x81;
        break;
    }
    SDL_GPUGraphicsPipelineCreateInfo ci = {
        .vertex_shader = vs,
        .fragment_shader = fs,
        .vertex_input_state = { .vertex_buffer_descriptions = &vb, .num_vertex_buffers = 1,
                                .vertex_attributes = at, .num_vertex_attributes = 4 },
        .primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST,
        .rasterizer_state = { .fill_mode = SDL_GPU_FILLMODE_FILL, .cull_mode = SDL_GPU_CULLMODE_NONE,
                              .front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE, .enable_depth_clip = false },
        .depth_stencil_state = {
            .compare_op = SDL_GPU_COMPAREOP_GREATER,   // the PVR's "greater" on 1/w: in front of the surface
            .front_stencil_state = so, .back_stencil_state = so,
            .compare_mask = compare_mask, .write_mask = write_mask,
            .enable_depth_test = op == MV_XOR || op == MV_OR, .enable_depth_write = false, .enable_stencil_test = true,
        },
        .target_info = { .color_target_descriptions = &ct, .num_color_targets = 1,
                         .depth_stencil_format = depth_format, .has_depth_stencil_target = true },
    };
    mv[op] = SDL_CreateGPUGraphicsPipeline(dev, &ci);
    if (!mv[op]) fprintf(stderr, "[gpu] modifier volume pipeline %d: %s\n", op, SDL_GetError());
    return mv[op];
}

// ---- textures, cached until the VRAM they come from changes ----

#define TCACHE 4096
static struct tex {
    uint64_t key, hash;
    uint32_t checked, used;   // render it was last hashed in / drawn in
    SDL_GPUTexture *t;
} tcache[TCACHE];
static uint32_t *decoded;
static int decoded_cap;

static SDL_GPUTexture *get_texture(SDL_GPUCommandBuffer *cmd, uint32_t tsp, uint32_t tcw)
{
    uint64_t key = (uint64_t)tcw << 32 | (tsp & 0x3fu);
    uint64_t h = key * 0x9e3779b97f4a7c15ull;
    struct tex *slot = NULL, *oldest = NULL;
    for (int i = 0; i < 16; i++) {
        struct tex *e = &tcache[(h >> 52 ^ (uint64_t)i * 0x9e3779b9u) % TCACHE];
        if (e->t && e->key == key) { slot = e; break; }
        if (!oldest || !e->t || (oldest->t && e->used < oldest->used)) oldest = e;
    }
    if (slot && slot->checked == renders) { slot->used = renders; return slot->t; }

    uint64_t content = pvr_texture_hash(tsp, tcw);
    if (slot && slot->hash == content) { slot->checked = slot->used = renders; return slot->t; }

    int w, hgt;
    pvr_texture_size(tsp, &w, &hgt);
    if (!slot) {
        // never one this render already uses: its draws are recorded but not bound yet
        if (oldest->t && oldest->used == renders) return white;
        slot = oldest;
        if (slot->t) SDL_ReleaseGPUTexture(dev, slot->t);
        slot->t = NULL;
    }
    if (!slot->t) slot->t = texture(w, hgt, FORMAT, SDL_GPU_TEXTUREUSAGE_SAMPLER);
    if (!slot->t) return white;
    if (w * hgt > decoded_cap) {
        decoded_cap = w * hgt;
        decoded = realloc(decoded, (size_t)decoded_cap * 4);
        if (!decoded) { decoded_cap = 0; return white; }
    }
    pvr_texture_decode(tsp, tcw, decoded);
    upload(cmd, slot->t, decoded, w, hgt);
    slot->key = key;
    slot->hash = content;
    slot->checked = slot->used = renders;
    return slot->t;
}

// ---- render targets, by framebuffer address ----

static struct target {
    uint32_t fb, frame;       // address, and the frame it was last drawn in
    int lw, lh, out_w;        // render size, and the width it is shown at
    int scale;
    SDL_GPUTexture *color, *depth;
} targets[MAX_TARGETS];

static int pick_scale(int lh)
{
    if (fixed_scale > 0) return fixed_scale > 8 ? 8 : fixed_scale;
    int pw = 0, ph = 0;
    if (!win || !SDL_GetWindowSizeInPixels(win, &pw, &ph) || ph <= 0) return 1;
    int s = (ph + lh - 1) / lh;   // at least the window's height: the blit to it only ever shrinks
    return s < 1 ? 1 : s > 8 ? 8 : s;
}

static struct target *get_target(uint32_t fb, int lw, int lh, int out_w)
{
    struct target *t = NULL, *oldest = &targets[0];
    for (int i = 0; i < MAX_TARGETS; i++) {
        if (targets[i].color && targets[i].fb == fb) { t = &targets[i]; break; }
        if (targets[i].frame < oldest->frame || !targets[i].color) oldest = &targets[i];
    }
    if (!t) { t = oldest; t->fb = fb; t->lw = 0; }
    int scale = pick_scale(lh);
    if (!t->color || t->lw != lw || t->lh != lh || t->scale != scale) {
        if (t->color) SDL_ReleaseGPUTexture(dev, t->color);
        if (t->depth) SDL_ReleaseGPUTexture(dev, t->depth);
        t->color = texture(lw * scale, lh * scale, FORMAT, SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER);
        t->depth = texture(lw * scale, lh * scale, depth_format, SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET);
        t->lw = lw;
        t->lh = lh;
        t->scale = scale;
    }
    t->out_w = out_w;
    t->frame = holly_frame_count();
    return t->color && t->depth ? t : NULL;
}

// ---- drawing ----

static SDL_GPUBuffer *vbuf, *ibuf;
static Uint32 vcap, icap;
static uint32_t *indices;
static int index_cap;

typedef struct {
    int32_t mode[4];   // flags, shading
    float ref[4];      // punch-through threshold
} strip_uniforms;

static int finite3(const pvr_vertex *v) { return isfinite(v->x) && isfinite(v->y) && isfinite(v->z); }

// a translucent triangle, for sorting: its farthest point (smallest 1/w), and
// where it was sent
typedef struct { float key; int strip, k, ord; } sort_tri;

static int cmp_tri(const void *pa, const void *pb)
{
    const sort_tri *a = pa, *b = pb;
    if (a->key != b->key) return a->key < b->key ? -1 : 1;
    return a->ord - b->ord;
}

static pvr_vertex *staged;   // the frame's vertices, then its modifier volumes', then a full-screen quad
static int staged_cap;

static void draw_range(SDL_GPURenderPass *rp, SDL_GPUGraphicsPipeline *p, Uint8 ref, int first_vertex, int count)
{
    if (!p || count <= 0) return;
    SDL_BindGPUGraphicsPipeline(rp, p);
    SDL_SetGPUStencilReference(rp, ref);
    SDL_DrawGPUPrimitives(rp, (Uint32)count, 1, (Uint32)first_vertex, 0);
}

// the opaque list's modifier volumes, then the shadows they cast; in the
// order pvr.c's draw_modvols takes them
static void draw_modvols(SDL_GPUCommandBuffer *cmd, SDL_GPURenderPass *rp, const pvr_frame *f, int mv_base)
{
    static const int32_t none[8] = { 0 };
    SDL_PushGPUFragmentUniformData(cmd, 0, none, sizeof none);
    int base = -1;
    for (int i = 0; i < f->nm; i++) {
        const pvr_modvol *m = &f->m[i];
        if (m->list != 1 || !m->count) continue;
        int mode = (int)(m->isp >> 29);
        if (base < 0) base = m->first;
        int is_or = !m->last && mode > 0;
        draw_range(rp, mv_pipeline(is_or ? MV_OR : MV_XOR), is_or ? 2 : 0, mv_base + m->first * 3, m->count * 3);
        if (mode == 1 || mode == 2) {
            draw_range(rp, mv_pipeline(mode == 1 ? MV_INCL : MV_EXCL), 1, mv_base + base * 3, (m->first + m->count - base) * 3);
            base = -1;
        }
    }
    if (f->shadow_on) {
        strip_uniforms u = { { 2 | 8, 0, 0, 0 }, { 0, 0, 0, 0 } };   // gouraud colour, with its alpha
        SDL_PushGPUFragmentUniformData(cmd, 0, &u, sizeof u);
        draw_range(rp, mv_pipeline(MV_SHADE), 0x81, mv_base + f->nmv, 6);
    }
}

// grow a GPU buffer to hold size bytes
static SDL_GPUBuffer *buffer(SDL_GPUBuffer *b, Uint32 *cap, Uint32 size, SDL_GPUBufferUsageFlags usage)
{
    if (b && size <= *cap) return b;
    if (b) SDL_ReleaseGPUBuffer(dev, b);
    Uint32 n = *cap ? *cap : 1u << 20;
    while (n < size) n *= 2;
    SDL_GPUBufferCreateInfo ci = { .usage = usage, .size = n };
    *cap = n;
    return SDL_CreateGPUBuffer(dev, &ci);
}

static void put_buffer(SDL_GPUCopyPass *cp, SDL_GPUBuffer *dst, const void *data, Uint32 size)
{
    SDL_GPUTransferBufferCreateInfo ti = { .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, .size = size };
    SDL_GPUTransferBuffer *tb = SDL_CreateGPUTransferBuffer(dev, &ti);
    if (!tb) return;
    void *p = SDL_MapGPUTransferBuffer(dev, tb, false);
    if (p) {
        memcpy(p, data, size);
        SDL_UnmapGPUTransferBuffer(dev, tb);
        SDL_GPUTransferBufferLocation src = { .transfer_buffer = tb };
        SDL_GPUBufferRegion region = { .buffer = dst, .size = size };
        SDL_UploadToGPUBuffer(cp, &src, &region, true);   // last frame may still be drawing from it
    }
    SDL_ReleaseGPUTransferBuffer(dev, tb);
}

void gpu_render(const pvr_frame *f)
{
    if (!active) return;
    renders++;
    struct target *t = get_target(f->fb, f->width, f->height, f->hscale ? f->width / 2 : f->width);
    if (!t) return;
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(dev);
    if (!cmd) return;

    // Every strip becomes a triangle list. Triangle k of a strip is drawn as
    // (k+2, k, k+1): the same triangle, but with its last vertex first, where
    // the GPU takes flat-shaded values from. Triangles with a corner at
    // infinity (behind the camera) are left out, as the software renderer
    // leaves them out.
    int max_idx = 0;
    for (int i = 0; i < f->ns; i++) max_idx += f->s[i].count > 2 ? (f->s[i].count - 2) * 3 : 0;
    if (max_idx > index_cap) {
        index_cap = max_idx;
        indices = realloc(indices, (size_t)index_cap * sizeof *indices);
        if (!indices) { index_cap = 0; SDL_CancelGPUCommandBuffer(cmd); return; }
    }

    static struct draw { int strip; Uint32 first, count; SDL_GPUTexture *tex; } *draws;
    static int draw_cap;
    if (f->ns + max_idx / 3 > draw_cap) {   // sorted translucent triangles can each be a draw
        draw_cap = f->ns + max_idx / 3;
        draws = realloc(draws, (size_t)draw_cap * sizeof *draws);
        if (!draws) { draw_cap = 0; SDL_CancelGPUCommandBuffer(cmd); return; }
    }
    int nd = 0, ni = 0;
    static const int order[3] = { 0, 4, 2 };   // opaque, punch-through, translucent
    for (int pass = 0; pass < 3; pass++) {
        if (order[pass] == 2 && f->autosort) {
            // The PVR sorts translucent pixels by depth. A fragment shader
            // here can't keep per-pixel lists (SDL_gpu gives them no writable
            // storage), so this sorts whole triangles back to front instead,
            // as Flycast's OpenGL renderer does; pvr.c's software renderer
            // sorts per pixel and shows where the two differ.
            static sort_tri *tris;
            static int tri_cap;
            int nt = 0;
            for (int i = 0; i < f->ns; i++) {
                const pvr_strip *s = &f->s[i];
                if (s->list != 2) continue;
                for (int k = 0; k + 2 < s->count; k++) {
                    const pvr_vertex *v = &f->v[s->first + k];
                    if (!finite3(&v[0]) || !finite3(&v[1]) || !finite3(&v[2])) continue;
                    if (nt == tri_cap) {
                        tri_cap = tri_cap ? tri_cap * 2 : 4096;
                        tris = realloc(tris, (size_t)tri_cap * sizeof *tris);
                        if (!tris) { tri_cap = 0; SDL_CancelGPUCommandBuffer(cmd); return; }
                    }
                    tris[nt] = (sort_tri){ fminf(v[0].z, fminf(v[1].z, v[2].z)), i, k, nt };
                    nt++;
                }
            }
            qsort(tris, (size_t)nt, sizeof *tris, cmp_tri);
            for (int j = 0; j < nt; j++) {
                const pvr_strip *s = &f->s[tris[j].strip];
                uint32_t v0 = (uint32_t)(s->first + tris[j].k);
                indices[ni++] = v0 + 2;
                indices[ni++] = v0;
                indices[ni++] = v0 + 1;
                if (nd && draws[nd - 1].strip == tris[j].strip && draws[nd - 1].first + draws[nd - 1].count == (Uint32)(ni - 3)) {
                    draws[nd - 1].count += 3;
                    continue;
                }
                draws[nd].strip = tris[j].strip;
                draws[nd].first = (Uint32)(ni - 3);
                draws[nd].count = 3;
                draws[nd].tex = NULL;
                nd++;
            }
            continue;
        }
        for (int i = 0; i < f->ns; i++) {
            const pvr_strip *s = &f->s[i];
            if (s->list != order[pass] || s->count < 3) continue;
            int first = ni;
            for (int k = 0; k + 2 < s->count; k++) {
                const pvr_vertex *v = &f->v[s->first + k];
                if (!finite3(&v[0]) || !finite3(&v[1]) || !finite3(&v[2])) continue;
                indices[ni++] = (uint32_t)(s->first + k + 2);
                indices[ni++] = (uint32_t)(s->first + k);
                indices[ni++] = (uint32_t)(s->first + k + 1);
            }
            if (ni == first) continue;
            draws[nd].strip = i;
            draws[nd].first = (Uint32)first;
            draws[nd].count = (Uint32)(ni - first);
            draws[nd].tex = NULL;
            nd++;
        }
    }

    // textures first: their uploads go in before the render pass
    for (int d = 0; d < nd; d++) {
        const pvr_strip *s = &f->s[draws[d].strip];
        draws[d].tex = (s->isp >> 25) & 1 ? get_texture(cmd, s->tsp, s->tcw) : white;
    }

    int has_mv = 0;
    for (int i = 0; i < f->nm; i++) has_mv |= f->m[i].list == 1 && f->m[i].count;
    int nstaged = f->nv + (has_mv ? f->nmv + 6 : 0);
    if (nstaged > staged_cap) {
        staged_cap = nstaged;
        staged = realloc(staged, (size_t)staged_cap * sizeof *staged);
        if (!staged) { staged_cap = 0; SDL_CancelGPUCommandBuffer(cmd); return; }
    }
    if (f->nv) memcpy(staged, f->v, (size_t)f->nv * sizeof *staged);
    if (has_mv) {
        memcpy(staged + f->nv, f->mv, (size_t)f->nmv * sizeof *staged);
        pvr_vertex *q = staged + f->nv + f->nmv;
        float qw = (float)f->width, qh = (float)f->height;
        const float corner[6][2] = { { 0, 0 }, { qw, 0 }, { 0, qh }, { qw, 0 }, { qw, qh }, { 0, qh } };
        for (int k = 0; k < 6; k++) {
            memset(&q[k], 0, sizeof q[k]);
            q[k].x = corner[k][0];
            q[k].y = corner[k][1];
            q[k].z = 1.0f;
            q[k].col[3] = 1.0f - f->shadow;
        }
    }
    Uint32 vsize = (Uint32)(nstaged * (int)sizeof(pvr_vertex)), isize = (Uint32)(ni * 4);
    if (vsize && (isize || has_mv)) {
        vbuf = buffer(vbuf, &vcap, vsize, SDL_GPU_BUFFERUSAGE_VERTEX);
        ibuf = buffer(ibuf, &icap, isize ? isize : 4, SDL_GPU_BUFFERUSAGE_INDEX);
        if (!vbuf || !ibuf) { SDL_CancelGPUCommandBuffer(cmd); return; }
        SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cmd);
        put_buffer(cp, vbuf, staged, vsize);
        if (isize) put_buffer(cp, ibuf, indices, isize);
        SDL_EndGPUCopyPass(cp);
    }

    uint32_t bg = f->bg;
    float bgz = f->bgz * DEPTH_SCALE;
    SDL_GPUColorTargetInfo ct = {
        .texture = t->color,
        .clear_color = { ((bg >> 16) & 0xff) / 255.0f, ((bg >> 8) & 0xff) / 255.0f, (bg & 0xff) / 255.0f, (bg >> 24) / 255.0f },
        .load_op = SDL_GPU_LOADOP_CLEAR, .store_op = SDL_GPU_STOREOP_STORE,
    };
    SDL_GPUDepthStencilTargetInfo dt = {
        .texture = t->depth, .clear_depth = bgz < 0 ? 0 : bgz > 1 ? 1 : bgz,
        .load_op = SDL_GPU_LOADOP_CLEAR, .store_op = SDL_GPU_STOREOP_DONT_CARE,
        .stencil_load_op = SDL_GPU_LOADOP_CLEAR, .stencil_store_op = SDL_GPU_STOREOP_DONT_CARE, .clear_stencil = 0,
    };
    SDL_GPURenderPass *rp = SDL_BeginGPURenderPass(cmd, &ct, 1, &dt);
    if (nd || has_mv) {
        int sc = t->scale;
        SDL_GPUViewport vp = { 0, 0, (float)(f->width * sc), (float)(f->height * sc), 0, 1 };
        SDL_SetGPUViewport(rp, &vp);
        SDL_Rect clip = { f->clip_x0 * sc, f->clip_y0 * sc, (f->clip_x1 - f->clip_x0 + 1) * sc, (f->clip_y1 - f->clip_y0 + 1) * sc };
        SDL_SetGPUScissor(rp, &clip);
        float screen[4] = { 2.0f / (float)f->width, 2.0f / (float)f->height, DEPTH_SCALE, 0 };
        SDL_PushGPUVertexUniformData(cmd, 0, screen, sizeof screen);
        SDL_GPUBufferBinding vb = { vbuf, 0 }, ib = { ibuf, 0 };
        SDL_BindGPUVertexBuffers(rp, 0, &vb, 1);
        SDL_BindGPUIndexBuffer(rp, &ib, SDL_GPU_INDEXELEMENTSIZE_32BIT);

        SDL_GPUGraphicsPipeline *bound = NULL;
        SDL_GPUTexture *bound_tex = NULL;
        SDL_GPUSampler *bound_smp = NULL;
        strip_uniforms last = { { -1, -1, -1, -1 }, { -1, 0, 0, 0 } };
        int ref = -1, volumes_done = !has_mv;
        SDL_GPUTextureSamplerBinding plain = { white, samplers[0][0][0] };
        SDL_BindGPUFragmentSamplers(rp, 0, &plain, 1);
        for (int d = 0; d <= nd; d++) {
            // the opaque and punch-through lists are down: the volumes go on them
            if (!volumes_done && (d == nd || f->s[draws[d].strip].list == 2)) {
                draw_modvols(cmd, rp, f, f->nv);
                volumes_done = 1;
                bound = NULL;
                ref = -1;
                memset(&last, 0xff, sizeof last);
            }
            if (d == nd) break;
            const pvr_strip *s = &f->s[draws[d].strip];
            uint32_t isp = s->isp, tsp = s->tsp;
            int textured = (isp >> 25) & 1;
            // the hardware's rules (as pvr.c): punch-through tests "greater or
            // equal" and writes depth; sorted translucent tests "greater or
            // equal" and writes none
            int zmode = (int)(isp >> 29), zwrite = !((isp >> 26) & 1);
            if (s->list == 4) { zmode = 6; zwrite = 1; }
            if (s->list == 2 && f->autosort) { zmode = 6; zwrite = 0; }
            SDL_GPUGraphicsPipeline *p = pipeline(s->list == 2, zmode, zwrite, (int)(tsp >> 29), (int)((tsp >> 26) & 7));
            if (!p) continue;
            if (p != bound) { SDL_BindGPUGraphicsPipeline(rp, p); bound = p; }
            int r = s->list != 2 && (s->pcw & PVR_PCW_SHADOW) ? 0x80 : 0;
            if (r != ref) { SDL_SetGPUStencilReference(rp, (Uint8)r); ref = r; }

            int filter = textured && ((tsp >> 13) & 3) != 0;
            int um = (tsp >> 16) & 1 ? 2 : (tsp >> 18) & 1 ? 1 : 0;
            int vm = (tsp >> 15) & 1 ? 2 : (tsp >> 17) & 1 ? 1 : 0;
            SDL_GPUSampler *smp = samplers[filter][um][vm];
            if (draws[d].tex != bound_tex || smp != bound_smp) {
                SDL_GPUTextureSamplerBinding tb = { draws[d].tex, smp };
                SDL_BindGPUFragmentSamplers(rp, 0, &tb, 1);
                bound_tex = draws[d].tex;
                bound_smp = smp;
            }

            strip_uniforms u = { { 0, (int32_t)((tsp >> 6) & 3), 0, 0 }, { f->pt_ref, 0, 0, 0 } };
            u.mode[0] = textured | ((isp >> 23) & 1) << 1 | ((isp >> 24) & 1) << 2 | ((tsp >> 20) & 1) << 3 |
                        ((tsp >> 19) & 1) << 4 | (s->list == 4) << 5;
            if (memcmp(&u, &last, sizeof u)) {
                SDL_PushGPUFragmentUniformData(cmd, 0, &u, sizeof u);
                last = u;
            }
            SDL_DrawGPUIndexedPrimitives(rp, draws[d].count, 1, draws[d].first, 0, 0);
        }
    }
    SDL_EndGPURenderPass(rp);
    SDL_SubmitGPUCommandBuffer(cmd);
}

// ---- showing it ----

static struct target *shown(uint32_t fb)
{
    for (int i = 0; i < MAX_TARGETS; i++)
        if (targets[i].color && targets[i].fb == fb && holly_frame_count() - targets[i].frame <= STALE_FRAMES)
            return &targets[i];
    return NULL;
}

// blit w x h of src into the window, as large as fits at 4:3
static void to_window(SDL_GPUTexture *src, int w, int h)
{
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(dev);
    if (!cmd) return;
    SDL_GPUTexture *sw = NULL;
    Uint32 sw_w = 0, sw_h = 0;
    if (!SDL_AcquireGPUSwapchainTexture(cmd, win, &sw, &sw_w, &sw_h) || !sw) {
        SDL_SubmitGPUCommandBuffer(cmd);   // minimised, or too many frames in flight: skip this one
        return;
    }
    Uint32 dw = sw_w, dh = sw_w * 3 / 4;
    if (dh > sw_h) { dh = sw_h; dw = sw_h * 4 / 3; }
    SDL_GPUBlitInfo bi = {
        .source = { .texture = src, .w = (Uint32)w, .h = (Uint32)h },
        .destination = { .texture = sw, .x = (sw_w - dw) / 2, .y = (sw_h - dh) / 2, .w = dw, .h = dh },
        .load_op = SDL_GPU_LOADOP_CLEAR,
        .clear_color = { 0, 0, 0, 1 },
        .filter = SDL_GPU_FILTER_LINEAR,
    };
    SDL_BlitGPUTexture(cmd, &bi);
    SDL_SubmitGPUCommandBuffer(cmd);
}

int gpu_present(uint32_t fb)
{
    if (!active || !win) return active;
    struct target *t = shown(fb);
    if (!t) return 0;
    to_window(t->color, t->lw * t->scale, t->lh * t->scale);
    return 1;
}

void gpu_present_pixels(const uint32_t *argb, int w, int h)
{
    static SDL_GPUTexture *screen;
    static int sw, sh;
    if (!active || !win) return;
    if (!screen || sw != w || sh != h) {
        if (screen) SDL_ReleaseGPUTexture(dev, screen);
        screen = texture(w, h, FORMAT, SDL_GPU_TEXTUREUSAGE_SAMPLER);
        sw = w;
        sh = h;
        if (!screen) return;
    }
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(dev);
    if (!cmd) return;
    upload(cmd, screen, argb, w, h);
    SDL_SubmitGPUCommandBuffer(cmd);
    to_window(screen, w, h);
}

int gpu_read(uint32_t fb, uint32_t **pixels, int *w, int *h)
{
    if (!active) return 0;
    struct target *t = shown(fb);
    if (!t) return 0;
    // native size (what VRAM would hold), or with DC_SHOTFULL=1 everything rendered
    int full = getenv("DC_SHOTFULL") != NULL;
    int ow = full ? t->lw * t->scale : t->out_w, oh = full ? t->lh * t->scale : t->lh;
    SDL_GPUTexture *small = texture(ow, oh, FORMAT, SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER);
    SDL_GPUTransferBufferCreateInfo ti = { .usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD, .size = (Uint32)(ow * oh * 4) };
    SDL_GPUTransferBuffer *tb = SDL_CreateGPUTransferBuffer(dev, &ti);
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(dev);
    int ok = 0;
    if (small && tb && cmd) {
        SDL_GPUBlitInfo bi = {
            .source = { .texture = t->color, .w = (Uint32)(t->lw * t->scale), .h = (Uint32)(t->lh * t->scale) },
            .destination = { .texture = small, .w = (Uint32)ow, .h = (Uint32)oh },
            .load_op = SDL_GPU_LOADOP_DONT_CARE,
            .filter = SDL_GPU_FILTER_LINEAR,
        };
        SDL_BlitGPUTexture(cmd, &bi);
        SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cmd);
        SDL_GPUTextureRegion src = { .texture = small, .w = (Uint32)ow, .h = (Uint32)oh, .d = 1 };
        SDL_GPUTextureTransferInfo dst = { .transfer_buffer = tb, .pixels_per_row = (Uint32)ow, .rows_per_layer = (Uint32)oh };
        SDL_DownloadFromGPUTexture(cp, &src, &dst);
        SDL_EndGPUCopyPass(cp);
        SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
        cmd = NULL;
        if (fence && SDL_WaitForGPUFences(dev, true, &fence, 1)) {
            const uint32_t *p = SDL_MapGPUTransferBuffer(dev, tb, false);
            uint32_t *out = realloc(*pixels, (size_t)ow * oh * 4);
            if (p && out) {
                for (int i = 0; i < ow * oh; i++) out[i] = p[i] | 0xff000000u;   // the screen has no alpha
                *pixels = out;
                *w = ow;
                *h = oh;
                ok = 1;
            }
            if (p) SDL_UnmapGPUTransferBuffer(dev, tb);
        }
        if (fence) SDL_ReleaseGPUFence(dev, fence);
    }
    if (cmd) SDL_CancelGPUCommandBuffer(cmd);
    if (tb) SDL_ReleaseGPUTransferBuffer(dev, tb);
    if (small) SDL_ReleaseGPUTexture(dev, small);
    return ok;
}
