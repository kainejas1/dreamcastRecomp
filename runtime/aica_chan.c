#include "aica_chan.h"
#include "memory.h"

#include <stdint.h>
#include <string.h>

#define CHANNELS       64
#define CH_WORDS       0x20
#define FRAC_BITS      10
#define FRAC_ONE       (1u << FRAC_BITS)
#define ENV_MAX        0x3ff
#define GAIN_ONE       65535u
#define GAIN_SIZE      2048

enum {
    ENV_ATTACK = 0,
    ENV_DECAY1 = 1,
    ENV_DECAY2 = 2,
    ENV_RELEASE = 3
};

typedef struct {
    int playing;

    /* Playback position: whole samples and a 10-bit fraction. */
    uint32_t pos;
    uint32_t frac;

    /* Envelope attenuation: 0 is loud, 0x3ff is silent. */
    int phase;
    int level;
    unsigned env_counter;

    /* Yamaha ADPCM predictor state. */
    int16_t adpcm_last;
    int adpcm_quant;
    int32_t adpcm_decoded;

    /* Predictor state saved at the loop start. */
    int16_t loop_last;
    int loop_quant;
    int loop_state_valid;

    /* Sticky status flag: the channel has passed its loop end. */
    int looped;
} channel;

static channel ch[CHANNELS];
static uint32_t *regs;
static uint16_t gain_table[GAIN_SIZE];

static const int adpcm_scale[8] = {
    230, 230, 230, 230, 307, 409, 512, 614
};

/*
 * Channel registers are 0x80 bytes apart, or 0x20 uint32_t words.
 */
enum {
    C_SA    = 0x00 / 4,
    C_SA_LO = 0x04 / 4,
    C_LSA   = 0x08 / 4,
    C_LEA   = 0x0c / 4,
    C_EG    = 0x10 / 4,
    C_EG2   = 0x14 / 4,
    C_PITCH = 0x18 / 4,
    C_SEND  = 0x24 / 4,
    C_TL    = 0x28 / 4
};

/* Only the low 16 bits of each AICA register are meaningful. */
static uint32_t cr(unsigned n, unsigned r)
{
    return regs[n * CH_WORDS + r] & 0xffffu;
}

/* Sample start address in bytes. */
static uint32_t start_address(unsigned n)
{
    return ((cr(n, C_SA) & 0x7fu) << 16) |
            cr(n, C_SA_LO);
}

/*
 * Sound RAM is 2 MiB. Masking implements address wrapping.
 * DC_ARAM_SIZE is a power of two in memory.h.
 */
static uint8_t ram8(uint32_t addr)
{
    return dc_aram[addr & (DC_ARAM_SIZE - 1)];
}

/* Little-endian signed PCM16. */
static int16_t ram16(uint32_t addr)
{
    uint32_t a = addr & (DC_ARAM_SIZE - 1);
    uint32_t b = (a + 1) & (DC_ARAM_SIZE - 1);

    uint16_t v = (uint16_t)dc_aram[a] |
                 ((uint16_t)dc_aram[b] << 8);

    return (int16_t)v;
}

static int clamp16(int x)
{
    if (x > 32767) return 32767;
    if (x < -32768) return -32768;
    return x;
}

/*
 * Integer attenuation lookup.
 *
 * The table approximates a logarithmic gain curve. Larger attenuation
 * values mean quieter audio. Out-of-range values are silent.
 */
static unsigned attenuation(unsigned level)
{
    if (level >= GAIN_SIZE)
        return 0;

    return gain_table[level];
}

/* Reset the ADPCM decoder to its initial predictor state. */
static void reset_adpcm(channel *c)
{
    c->adpcm_last = 0;
    c->adpcm_quant = 0x7f;
    c->adpcm_decoded = -1;

    c->loop_last = 0;
    c->loop_quant = 0x7f;
    c->loop_state_valid = 0;
}

/*
 * Decode one Yamaha ADPCM nibble.
 *
 * Nibble bit 3 selects subtraction. The lower three bits select
 * the magnitude and quantizer scaling.
 */
static int16_t decode_nibble(channel *c, unsigned nibble)
{
    int diff = (int)(((nibble & 7u) * 2u + 1u) *
                     (unsigned)c->adpcm_quant / 8u);

    int sample = c->adpcm_last;

    if (nibble & 8u)
        sample -= diff;
    else
        sample += diff;

    sample = clamp16(sample);
    c->adpcm_last = (int16_t)sample;

    c->adpcm_quant =
        c->adpcm_quant * adpcm_scale[nibble & 7u] / 256;

    if (c->adpcm_quant < 0x7f)
        c->adpcm_quant = 0x7f;

    if (c->adpcm_quant > 0x6000)
        c->adpcm_quant = 0x6000;

    return c->adpcm_last;
}

/*
 * Decode sequentially until the requested sample is available.
 *
 * This simple implementation caches the last decoded sample and
 * predictor state. Random access backwards resets and decodes again.
 */
static int16_t adpcm_at(unsigned n, channel *c, uint32_t index)
{
    uint32_t base = start_address(n);
    uint32_t lsa = cr(n, C_LSA);

    if ((int32_t)index < c->adpcm_decoded)
        reset_adpcm(c);

    while ((uint32_t)(c->adpcm_decoded + 1) <= index) {
        uint32_t next = (uint32_t)(c->adpcm_decoded + 1);

        /*
         * Save the decoder state immediately before decoding LSA.
         * Restoring it on a loop restarts decoding at the same point.
         */
        if (next == lsa && !c->loop_state_valid) {
            c->loop_last = c->adpcm_last;
            c->loop_quant = c->adpcm_quant;
            c->loop_state_valid = 1;
        }

        uint8_t packed = ram8(base + next / 2);

        /* AICA Yamaha ADPCM stores the low nibble first. */
        unsigned nibble = (next & 1u)
                        ? packed >> 4
                        : packed & 0x0fu;

        decode_nibble(c, nibble);
        c->adpcm_decoded = (int32_t)next;
    }

    return c->adpcm_last;
}

/* Restore the saved predictor state when looping ADPCM. */
static void adpcm_loop(channel *c, uint32_t lsa)
{
    if (c->loop_state_valid) {
        c->adpcm_last = c->loop_last;
        c->adpcm_quant = c->loop_quant;
        c->adpcm_decoded = (int32_t)lsa - 1;
    } else {
        /*
         * Fallback for an unusual loop setup where LSA's state
         * has not yet been saved.
         */
        reset_adpcm(c);
    }
}

/*
 * Approximate effective envelope rate.
 *
 * The hardware's exact key-rate calculation and timing tables are
 * more complicated; this is a functional first implementation.
 */
static unsigned effective_rate(unsigned rate, unsigned krs,
                               unsigned pitch_reg)
{
    if (rate == 0)
        return 0;

    unsigned oct = (pitch_reg >> 11) & 15u;
    int octave = (oct & 8u) ? (int)oct - 16 : (int)oct;

    unsigned scale = 0;

    if (krs != 15) {
        int key = octave + 8;

        if (key < 0)
            key = 0;

        scale = (unsigned)(key * (15 - (int)krs) / 4);
    }

    unsigned r = rate * 2u + scale;

    return r > 63 ? 63 : r;
}

/* Approximate number of output samples between envelope steps. */
static unsigned rate_interval(unsigned rate)
{
    if (!rate) return 0;

    if (rate >= 60) return 1;
    if (rate >= 54) return 2;
    if (rate >= 48) return 4;
    if (rate >= 42) return 8;
    if (rate >= 36) return 16;
    if (rate >= 30) return 32;
    if (rate >= 24) return 64;
    if (rate >= 18) return 128;
    if (rate >= 12) return 256;
    if (rate >= 6)  return 512;

    return 1024;
}

/* Advance one channel's envelope by one output sample. */
static void envelope_tick(unsigned n, channel *c)
{
    uint32_t eg = cr(n, C_EG);
    uint32_t eg2 = cr(n, C_EG2);
    uint32_t pitch = cr(n, C_PITCH);

    unsigned ar  = eg & 31u;
    unsigned d1r = (eg >> 6) & 31u;
    unsigned d2r = (eg >> 11) & 31u;

    unsigned rr  = eg2 & 31u;
    unsigned krs = (eg2 >> 10) & 15u;
    unsigned dl  = ((eg2 >> 5) & 31u) << 5;

    unsigned rate;

    switch (c->phase) {
    case ENV_ATTACK:
        rate = effective_rate(ar, krs, pitch);
        break;

    case ENV_DECAY1:
        rate = effective_rate(d1r, krs, pitch);
        break;

    case ENV_DECAY2:
        rate = effective_rate(d2r, krs, pitch);
        break;

    default:
        rate = effective_rate(rr, krs, pitch);
        break;
    }

    unsigned interval = rate_interval(rate);

    if (!interval)
        return;

    if (++c->env_counter < interval)
        return;

    c->env_counter = 0;

    /*
     * The envelope attenuation is approximate. Attack decreases
     * attenuation; decay and release increase it.
     */
    switch (c->phase) {
    case ENV_ATTACK: {
        unsigned shift = (63u - rate) / 8u;

        if (shift > 10)
            shift = 10;

        c->level -= (c->level >> shift) + 1;

        if (c->level <= 0) {
            c->level = 0;
            c->phase = ENV_DECAY1;
        }
        break;
    }

    case ENV_DECAY1:
        if (c->level < (int)dl) {
            unsigned shift = (63u - rate) / 8u;

            if (shift > 10)
                shift = 10;

            c->level += (c->level >> shift) + 1;

            if (c->level >= (int)dl)
                c->level = (int)dl;
        } else {
            c->phase = ENV_DECAY2;
        }
        break;

    case ENV_DECAY2:
        if (c->level < ENV_MAX) {
            unsigned shift = (63u - rate) / 8u;

            if (shift > 10)
                shift = 10;

            c->level += (c->level >> shift) + 1;

            if (c->level > ENV_MAX)
                c->level = ENV_MAX;
        }
        break;

    case ENV_RELEASE:
        if (c->level < ENV_MAX) {
            unsigned shift = (63u - rate) / 8u;

            if (shift > 10)
                shift = 10;

            c->level += (c->level >> shift) + 1;

            if (c->level >= ENV_MAX) {
                c->level = ENV_MAX;
                c->playing = 0;
            }
        } else {
            c->playing = 0;
        }
        break;
    }
}

/* Fetch one source sample in the channel's selected format. */
static int16_t read_sample(unsigned n, channel *c)
{
    uint32_t fmt = (cr(n, C_SA) >> 7) & 3u;
    uint32_t addr = start_address(n);

    switch (fmt) {
    case 0: /* PCM16 */
        return ram16(addr + c->pos * 2u);

    case 1: /* PCM8 */
        return (int16_t)(
            (int16_t)(int8_t)ram8(addr + c->pos) * 256
        );

    case 2: /* Yamaha ADPCM */
    case 3: /* ADPCM long stream: basic decoder for now */
        return adpcm_at(n, c, c->pos);

    default:
        return 0;
    }
}

/*
 * Calculate the fixed-point pitch step.
 *
 * FNS is a 10-bit fraction and OCT is a signed four-bit exponent.
 */
static uint32_t pitch_step(unsigned n)
{
    uint32_t p = cr(n, C_PITCH);

    int octave = (int)((p >> 11) & 15u);

    if (octave & 8)
        octave -= 16;

    uint32_t step = 1024u + (p & 0x3ffu);

    if (octave >= 0)
        step <<= (unsigned)octave;
    else
        step >>= (unsigned)-octave;

    return step;
}

void aica_chan_init(uint32_t *r)
{
    regs = r;

    memset(ch, 0, sizeof ch);

    for (unsigned i = 0; i < CHANNELS; ++i) {
        ch[i].level = ENV_MAX;
        ch[i].phase = ENV_RELEASE;
        ch[i].adpcm_quant = 0x7f;
        ch[i].adpcm_decoded = -1;
    }

    /*
     * Approximate attenuation gain.
     *
     * The table is generated using integer arithmetic to avoid
     * logarithms and floating-point operations in the audio loop.
     */
    gain_table[0] = GAIN_ONE;

    for (unsigned i = 1; i < GAIN_SIZE; ++i) {
        gain_table[i] =
            (uint16_t)(((uint32_t)gain_table[i - 1] * 64830u
                        + 32768u) >> 16);
    }
}

/*
 * KYONEX triggers processing of all channels' KYONB bits.
 * KYONEX itself is a write trigger and is not stored as channel state.
 */
void aica_chan_keyon(void)
{
    for (unsigned n = 0; n < CHANNELS; ++n) {
        channel *c = &ch[n];

        uint32_t control = cr(n, C_SA);
        int keyon = (control & 0x4000u) != 0;

        if (keyon && !c->playing) {
            c->playing = 1;
            c->pos = 0;
            c->frac = 0;

            c->phase = ENV_ATTACK;
            c->level = ENV_MAX;
            c->env_counter = 0;

            c->looped = 0;

            reset_adpcm(c);
        } else if (!keyon && c->playing &&
                   c->phase != ENV_RELEASE) {
            c->phase = ENV_RELEASE;
            c->env_counter = 0;
        }
    }
}

/* Generate one stereo output sample. */
void aica_chan_sample(int16_t out[2])
{
    int64_t left = 0;
    int64_t right = 0;

    for (unsigned n = 0; n < CHANNELS; ++n) {
        channel *c = &ch[n];

        if (!c->playing)
            continue;

        uint32_t control = cr(n, C_SA);
        uint32_t lsa = cr(n, C_LSA);
        uint32_t lea = cr(n, C_LEA);

        if (lea == 0) {
            c->playing = 0;
            continue;
        }

        int sample = read_sample(n, c);

        /* Total level is an attenuation in the TL register. */
        unsigned tl = (cr(n, C_TL) >> 8) & 0xffu;
        unsigned env_att = (unsigned)c->level + tl * 4u;
        unsigned env_gain = attenuation(env_att);

        uint32_t send = cr(n, C_SEND);

        unsigned disdl = (send >> 8) & 15u;
        unsigned dipan = send & 31u;

        unsigned pan = dipan & 15u;
        unsigned side = (dipan >> 4) & 1u;

        unsigned send_gain = attenuation((15u - disdl) * 32u);
        unsigned other_gain = attenuation(pan * 32u);

        /*
         * DISDL=0 means direct output is disabled.
         * DIPAN selects the side and attenuates the opposite side.
         */
        if (disdl) {
            int64_t v = (int64_t)sample * env_gain / 65536;
            v = v * send_gain / 65536;

            if (side) {
                left += v * other_gain / 65536;
                right += v;
            } else {
                left += v;
                right += v * other_gain / 65536;
            }
        }

        envelope_tick(n, c);

        if (!c->playing)
            continue;

        /*
         * Advance fixed-point playback position.
         */
        uint64_t fp = (uint64_t)c->pos * FRAC_ONE + c->frac;

        fp += pitch_step(n);

        if ((fp >> FRAC_BITS) >= lea) {
            if (control & 0x0200u) {
                uint64_t end = (uint64_t)lea * FRAC_ONE;
                uint64_t start = (uint64_t)lsa * FRAC_ONE;
                uint64_t length = end - start;

                if (length) {
                    fp = start + (fp - end) % length;
                    c->looped = 1;

                    if (((control >> 7) & 3u) >= 2u)
                        adpcm_loop(c, lsa);
                } else {
                    c->playing = 0;
                }
            } else {
                c->playing = 0;
            }
        }

        c->pos = (uint32_t)(fp >> FRAC_BITS);
        c->frac = (uint32_t)(fp & (FRAC_ONE - 1));
    }

    /*
     * MVOL is in the common register at 0x2800.
     * The 0..15 setting is converted to an approximate gain.
     */
    unsigned mvol = regs[0x2800 / 4] & 15u;

    unsigned master_gain =
        mvol ? attenuation((15u - mvol) * 32u) : 0;

    left = left * master_gain / 65536;
    right = right * master_gain / 65536;

    out[0] = (int16_t)clamp16((int)left);
    out[1] = (int16_t)clamp16((int)right);
}

/* Return LP, envelope phase and attenuation for the selected channel. */
uint32_t aica_chan_info(void)
{
    unsigned n = (regs[0x280c / 4] >> 8) & 0x3fu;
    channel *c = &ch[n];

    if (!c->playing && c->level >= ENV_MAX)
        return 0x7fff;

    return ((uint32_t)(c->looped != 0) << 15) |
           ((uint32_t)(c->phase & 3) << 13) |
           ((uint32_t)c->level & 0x1fffu);
}

/* Return the selected channel's current whole-sample position. */
uint32_t aica_chan_pos(void)
{
    unsigned n = (regs[0x280c / 4] >> 8) & 0x3fu;

    return ch[n].pos;
}