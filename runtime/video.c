#include "video.h"
#include "holly.h"
#include "memory.h"
#include "pvr.h"

#include <SDL3/SDL.h>
#define SDL_MAIN_HANDLED   // main() is ours, SDL just has to be told
#include <SDL3/SDL_main.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static SDL_Window *window;
static SDL_Renderer *renderer;
static SDL_Texture *texture;
static int tex_w, tex_h;
static uint32_t last_frame = ~0u;

static uint32_t *pixels;
static int pix_cap;

// the screen as ARGB8888: FB_R_SOF1 (and SOF2 for the odd lines when
// interlaced), FB_R_SIZE, FB_R_CTRL's pixel format
static int read_screen(int *w, int *h)
{
    uint32_t ctrl = holly_reg(PVR_FB_R_CTRL), size = holly_reg(PVR_FB_R_SIZE);
    if (!(ctrl & 1)) return 0;   // display off
    int depth = (ctrl >> 2) & 3;
    int bpp = depth == 0 || depth == 1 ? 2 : depth == 2 ? 3 : 4;
    uint32_t xwords = (size & 0x3ff) + 1, lines = ((size >> 10) & 0x3ff) + 1, mod = (size >> 20) & 0x3ff;
    uint32_t stride = (xwords + mod - 1) * 4;
    int interlace = (holly_reg(PVR_SPG_CONTROL) >> 4) & 1;
    *w = (int)(xwords * 4 / (uint32_t)bpp);
    *h = (int)lines * (interlace ? 2 : 1);
    if (*w <= 0 || *h <= 0 || *w > 2048 || *h > 2048) return 0;
    if (*w * *h > pix_cap) {
        pix_cap = *w * *h;
        pixels = realloc(pixels, (size_t)pix_cap * 4);
        if (!pixels) return 0;
    }

    for (int y = 0; y < *h; y++) {
        uint32_t line = interlace ? (uint32_t)y / 2 : (uint32_t)y;
        uint32_t base = (interlace && (y & 1) ? holly_reg(PVR_FB_R_SOF2) : holly_reg(PVR_FB_R_SOF1)) & 0x00fffffcu;
        for (int x = 0; x < *w; x++) {
            uint32_t o = (base + line * stride + (uint32_t)(x * bpp)) & (DC_VRAM_SIZE - 1), c = 0, r, g, b;
            if (o + 4 <= DC_VRAM_SIZE) memcpy(&c, &dc_vram[o], (size_t)bpp);
            switch (depth) {
            case 0:   // 0555
                r = (c >> 10) & 31; g = (c >> 5) & 31; b = c & 31;
                c = (r << 3 | r >> 2) << 16 | (g << 3 | g >> 2) << 8 | (b << 3 | b >> 2);
                break;
            case 1:   // 565
                r = (c >> 11) & 31; g = (c >> 5) & 63; b = c & 31;
                c = (r << 3 | r >> 2) << 16 | (g << 2 | g >> 4) << 8 | (b << 3 | b >> 2);
                break;
            default:  // 888, 0888
                c &= 0xffffff;
                break;
            }
            pixels[y * *w + x] = 0xff000000u | c;
        }
    }
    return 1;
}

// ---- input ----

static SDL_Gamepad *gamepad;

#define MAX_PRESSES 32
static struct { uint32_t frame; unsigned short button; } presses[MAX_PRESSES];
static int npresses;

static void parse_presses(void)
{
    static const struct { const char *name; unsigned short bit; } names[] = {
        { "a", PAD_A }, { "b", PAD_B }, { "x", PAD_X }, { "y", PAD_Y }, { "start", PAD_START },
        { "up", PAD_UP }, { "down", PAD_DOWN }, { "left", PAD_LEFT }, { "right", PAD_RIGHT },
    };
    const char *s = getenv("DC_PRESS");
    while (s && *s && npresses < MAX_PRESSES) {
        char *end;
        uint32_t frame = (uint32_t)strtoul(s, &end, 0);
        if (*end != ':') break;
        s = end + 1;
        size_t len = strcspn(s, ",");
        for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
            if (strlen(names[i].name) == len && !strncmp(s, names[i].name, len)) {
                presses[npresses].frame = frame;
                presses[npresses++].button = names[i].bit;
            }
        s += len;
        if (*s == ',') s++;
    }
}

void video_pad(dc_pad *pad)
{
    memset(pad, 0, sizeof *pad);
    pad->x = pad->y = 0x80;

    uint32_t frame = holly_frame_count();
    for (int i = 0; i < npresses; i++)   // held for 8 frames
        if (frame >= presses[i].frame && frame < presses[i].frame + 8) pad->buttons |= presses[i].button;

    if (!renderer) return;
    const bool *k = SDL_GetKeyboardState(NULL);
    if (k[SDL_SCANCODE_UP]) pad->buttons |= PAD_UP;
    if (k[SDL_SCANCODE_DOWN]) pad->buttons |= PAD_DOWN;
    if (k[SDL_SCANCODE_LEFT]) pad->buttons |= PAD_LEFT;
    if (k[SDL_SCANCODE_RIGHT]) pad->buttons |= PAD_RIGHT;
    if (k[SDL_SCANCODE_Z]) pad->buttons |= PAD_A;
    if (k[SDL_SCANCODE_X]) pad->buttons |= PAD_B;
    if (k[SDL_SCANCODE_A]) pad->buttons |= PAD_X;
    if (k[SDL_SCANCODE_S]) pad->buttons |= PAD_Y;
    if (k[SDL_SCANCODE_RETURN]) pad->buttons |= PAD_START;
    if (k[SDL_SCANCODE_Q]) pad->lt = 255;
    if (k[SDL_SCANCODE_W]) pad->rt = 255;

    if (gamepad) {
        static const struct { SDL_GamepadButton b; unsigned short bit; } map[] = {
            { SDL_GAMEPAD_BUTTON_SOUTH, PAD_A }, { SDL_GAMEPAD_BUTTON_EAST, PAD_B },
            { SDL_GAMEPAD_BUTTON_WEST, PAD_X }, { SDL_GAMEPAD_BUTTON_NORTH, PAD_Y },
            { SDL_GAMEPAD_BUTTON_START, PAD_START }, { SDL_GAMEPAD_BUTTON_DPAD_UP, PAD_UP },
            { SDL_GAMEPAD_BUTTON_DPAD_DOWN, PAD_DOWN }, { SDL_GAMEPAD_BUTTON_DPAD_LEFT, PAD_LEFT },
            { SDL_GAMEPAD_BUTTON_DPAD_RIGHT, PAD_RIGHT },
        };
        for (size_t i = 0; i < sizeof map / sizeof map[0]; i++)
            if (SDL_GetGamepadButton(gamepad, map[i].b)) pad->buttons |= map[i].bit;
        int x = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTX), y = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTY);
        int l = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER), r = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
        pad->x = (unsigned char)((x + 32768) >> 8);
        pad->y = (unsigned char)((y + 32768) >> 8);
        if (l > 0) pad->lt = (unsigned char)(l >> 7);
        if (r > 0) pad->rt = (unsigned char)(r >> 7);
    }
}

// ---- audio ----

static SDL_AudioStream *audio;
static FILE *wav;
static uint32_t wav_bytes;

static void le32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

// 44-byte header for 44.1 kHz 16-bit stereo, `bytes` of samples after it
static void wav_header(FILE *f, uint32_t bytes)
{
    uint8_t h[44];
    memcpy(h, "RIFF", 4); le32(h + 4, 36 + bytes); memcpy(h + 8, "WAVEfmt ", 8);
    le32(h + 16, 16); h[20] = 1; h[21] = 0; h[22] = 2; h[23] = 0;   // PCM, 2 channels
    le32(h + 24, 44100); le32(h + 28, 44100 * 4); h[32] = 4; h[33] = 0; h[34] = 16; h[35] = 0;
    memcpy(h + 36, "data", 4); le32(h + 40, bytes);
    fwrite(h, 1, sizeof h, f);
}

void video_audio(const short *stereo, int frames)
{
    static int wav_tried;
    if (!wav_tried) {
        wav_tried = 1;
        if (getenv("DC_WAV") && (wav = fopen(getenv("DC_WAV"), "wb"))) wav_header(wav, 0);
    }
    if (wav) { fwrite(stereo, 4, (size_t)frames, wav); wav_bytes += (uint32_t)frames * 4; }

    // more than a quarter second waiting (we ran ahead): drop it rather than lag behind
    if (audio && SDL_GetAudioStreamQueued(audio) < 44100)
        SDL_PutAudioStreamData(audio, stereo, frames * 4);
}

void video_init(void)
{
    parse_presses();
    if (getenv("DC_NOWINDOW")) return;
    SDL_SetMainReady();
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_AUDIO)) {
        fprintf(stderr, "[video] SDL_Init failed: %s, running without a window\n", SDL_GetError());
        return;
    }
    SDL_AudioSpec spec = { SDL_AUDIO_S16, 2, 44100 };
    audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
    if (audio) SDL_ResumeAudioStreamDevice(audio);
    else fprintf(stderr, "[video] no audio: %s\n", SDL_GetError());
    if (!SDL_CreateWindowAndRenderer("dcrecomp", 1280, 960, SDL_WINDOW_RESIZABLE, &window, &renderer)) {
        fprintf(stderr, "[video] no window: %s\n", SDL_GetError());
        window = NULL;
        renderer = NULL;
    }
}

void video_frame(void)
{
    uint32_t frame = holly_frame_count();
    if (frame == last_frame) return;
    last_frame = frame;
    if (!renderer) return;

    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_EVENT_QUIT) exit(0);
        if (e.type == SDL_EVENT_GAMEPAD_ADDED && !gamepad) gamepad = SDL_OpenGamepad(e.gdevice.which);
        if (e.type == SDL_EVENT_GAMEPAD_REMOVED && gamepad && SDL_GetGamepadID(gamepad) == e.gdevice.which) {
            SDL_CloseGamepad(gamepad);
            gamepad = NULL;
        }
    }

    int w, h;
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);
    if (read_screen(&w, &h)) {
        if (!texture || w != tex_w || h != tex_h) {
            if (texture) SDL_DestroyTexture(texture);
            texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, w, h);
            tex_w = w;
            tex_h = h;
            SDL_SetRenderLogicalPresentation(renderer, 640, 480, SDL_LOGICAL_PRESENTATION_LETTERBOX);
        }
        if (texture) {
            SDL_UpdateTexture(texture, NULL, pixels, w * 4);
            SDL_RenderTexture(renderer, texture, NULL, NULL);
        }
    }
    SDL_RenderPresent(renderer);
}

// ---- PNG, uncompressed (stored deflate blocks), so no zlib needed ----

static uint32_t crc_table[256];

static uint32_t crc(uint32_t c, const uint8_t *p, size_t n)
{
    if (!crc_table[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t v = i;
            for (int k = 0; k < 8; k++) v = v & 1 ? 0xedb88320u ^ (v >> 1) : v >> 1;
            crc_table[i] = v;
        }
    c = ~c;
    while (n--) c = crc_table[(c ^ *p++) & 0xff] ^ (c >> 8);
    return ~c;
}

static void be32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }

static void chunk(FILE *f, const char *type, const uint8_t *data, uint32_t len)
{
    uint8_t h[8];
    be32(h, len);
    memcpy(h + 4, type, 4);
    fwrite(h, 1, 8, f);
    if (len) fwrite(data, 1, len, f);
    uint32_t c = crc(crc(0, (const uint8_t *)type, 4), data, len);
    be32(h, c);
    fwrite(h, 1, 4, f);
}

static int save_png(const char *path, const uint32_t *px, int w, int h)
{
    size_t row = (size_t)w * 3 + 1, raw_len = row * (size_t)h;
    uint8_t *raw = malloc(raw_len);
    if (!raw) return 0;
    for (int y = 0; y < h; y++) {
        uint8_t *r = raw + row * (size_t)y;
        *r++ = 0;   // no filter
        for (int x = 0; x < w; x++) {
            uint32_t c = px[y * w + x];
            *r++ = (uint8_t)(c >> 16); *r++ = (uint8_t)(c >> 8); *r++ = (uint8_t)c;
        }
    }

    size_t nblocks = (raw_len + 65534) / 65535, zlen = 2 + raw_len + nblocks * 5 + 4;
    uint8_t *z = malloc(zlen), *p = z;
    if (!z) { free(raw); return 0; }
    *p++ = 0x78; *p++ = 0x01;
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < raw_len; i += 65535) {
        size_t n = raw_len - i < 65535 ? raw_len - i : 65535;
        *p++ = i + n == raw_len;
        *p++ = (uint8_t)n; *p++ = (uint8_t)(n >> 8);
        *p++ = (uint8_t)~n; *p++ = (uint8_t)(~n >> 8);
        memcpy(p, raw + i, n);
        p += n;
    }
    for (size_t i = 0; i < raw_len; i++) { a = (a + raw[i]) % 65521; b = (b + a) % 65521; }
    be32(p, b << 16 | a);
    p += 4;

    FILE *f = fopen(path, "wb");
    if (!f) { free(raw); free(z); return 0; }
    uint8_t ihdr[13];
    be32(ihdr, (uint32_t)w);
    be32(ihdr + 4, (uint32_t)h);
    ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;   // 8-bit RGB
    fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
    chunk(f, "IHDR", ihdr, 13);
    chunk(f, "IDAT", z, (uint32_t)(p - z));
    chunk(f, "IEND", NULL, 0);
    fclose(f);
    free(raw);
    free(z);
    return 1;
}

void video_shutdown(void)
{
    if (wav) {
        fseek(wav, 0, SEEK_SET);
        wav_header(wav, wav_bytes);
        fclose(wav);
        wav = NULL;
        fprintf(stderr, "  audio (%.1f s) saved to %s\n", wav_bytes / 4 / 44100.0, getenv("DC_WAV"));
    }
    const char *path = getenv("DC_SHOT");
    int w, h;
    if (path) {
        if (read_screen(&w, &h) && save_png(path, pixels, w, h))
            fprintf(stderr, "  screen (%dx%d, %u frames rendered) saved to %s\n", w, h, pvr_render_count(), path);
        else
            fprintf(stderr, "  no screen to save (display off?)\n");
    }
}
