#include "sh4.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static sh4_opinfo table[SH4_OP_COUNT] = {
    [SH4_OP_UNKNOWN] = { "UNKNOWN", "", ".word", "", E_NONE, 0, 0, 0 },
#define X(id, pat, mn, ops, enc, fl) [SH4_OP_##id] = { #id, pat, mn, ops, enc, fl, 0, 0 },
#include "sh4_ops.def"
#undef X
};

static uint8_t lut[65536];   /* raw -> sh4_op */
static int initialized;

static int popcount16(uint16_t v)
{
    int c = 0;
    for (; v; v &= v - 1) c++;
    return c;
}

void sh4_init(void)
{
    if (initialized) return;
    _Static_assert(SH4_OP_COUNT <= 256, "lut entries are uint8_t");

    for (int op = 1; op < SH4_OP_COUNT; op++) {
        const char *p = table[op].pattern;
        uint16_t mask = 0, match = 0;
        for (int i = 0; i < 16 && p[i]; i++) {
            uint16_t bit = (uint16_t)(0x8000u >> i);
            if (p[i] == '0' || p[i] == '1') {
                mask |= bit;
                if (p[i] == '1') match |= bit;
            }
        }
        table[op].mask = mask;
        table[op].match = match;
    }

    for (uint32_t raw = 0; raw < 65536; raw++) {
        int best = 0, best_bits = -1;
        for (int op = 1; op < SH4_OP_COUNT; op++) {
            if ((raw & table[op].mask) != table[op].match) continue;
            int bits = popcount16(table[op].mask);
            if (bits > best_bits) { best = op; best_bits = bits; }
        }
        lut[raw] = (uint8_t)best;
    }
    initialized = 1;
}

const sh4_opinfo *sh4_info(sh4_op op)
{
    return &table[op < SH4_OP_COUNT ? op : SH4_OP_UNKNOWN];
}

void sh4_decode(uint32_t pc, uint16_t raw, sh4_insn *out)
{
    const sh4_opinfo *info = &table[lut[raw]];
    memset(out, 0, sizeof *out);
    out->pc = pc;
    out->raw = raw;
    out->op = (sh4_op)lut[raw];
    out->n = (raw >> 8) & 0xF;
    out->m = (raw >> 4) & 0xF;
    out->flags = info->flags;

    switch (info->enc) {
    case E_NONE: break;
    case E_IMMS: out->imm = (int8_t)(raw & 0xFF); break;
    case E_IMMU: out->imm = raw & 0xFF; break;
    case E_D4B:  out->disp = raw & 0xF; break;
    case E_D4W:  out->disp = (raw & 0xF) * 2; break;
    case E_D4L:  out->disp = (raw & 0xF) * 4; break;
    case E_D8B:  out->disp = raw & 0xFF; break;
    case E_D8W:  out->disp = (raw & 0xFF) * 2; break;
    case E_D8L:  out->disp = (raw & 0xFF) * 4; break;
    case E_PCW:
        out->disp = (raw & 0xFF) * 2;
        out->target = pc + 4 + (uint32_t)out->disp;
        break;
    case E_PCL:
        out->disp = (raw & 0xFF) * 4;
        out->target = (pc & ~3u) + 4 + (uint32_t)out->disp;
        break;
    case E_BR8:
        out->disp = (int8_t)(raw & 0xFF) * 2;
        out->target = pc + 4 + (uint32_t)out->disp;
        break;
    case E_BR12: {
        int32_t d = raw & 0xFFF;
        if (d & 0x800) d -= 0x1000;
        out->disp = d * 2;
        out->target = pc + 4 + (uint32_t)out->disp;
        break;
    }
    }
}

typedef struct { char *buf; size_t len, pos; } outbuf;

static void put(outbuf *o, const char *fmt, ...)
{
    if (o->pos >= o->len) return;
    va_list ap;
    va_start(ap, fmt);
    int w = vsnprintf(o->buf + o->pos, o->len - o->pos, fmt, ap);
    va_end(ap);
    if (w > 0) o->pos += (size_t)w;
    if (o->pos >= o->len) o->pos = o->len - 1;
}

void sh4_format(const sh4_insn *in, char *buf, size_t len)
{
    outbuf o = { buf, len, 0 };
    const sh4_opinfo *info = sh4_info(in->op);
    if (len == 0) return;
    buf[0] = '\0';

    if (in->op == SH4_OP_UNKNOWN) {
        put(&o, ".word\t0x%04x", in->raw);
        return;
    }

    put(&o, "%s", info->mnemonic);
    if (!info->operands[0]) return;
    put(&o, "\t");

    for (const char *p = info->operands; *p; p++) {
        if (*p != '%') { put(&o, "%c", *p); continue; }
        switch (*++p) {
        case 'n': put(&o, "r%u", in->n); break;
        case 'm': put(&o, "r%u", in->m); break;
        case 'k': put(&o, "r%u_bank", in->m & 7); break;
        case 'i': put(&o, "#%d", in->imm); break;
        case 'd': put(&o, "%d", in->disp); break;
        case 'a': put(&o, "%08x", in->target); break;
        case 'f': put(&o, "fr%u", in->n); break;
        case 'g': put(&o, "fr%u", in->m); break;
        case 'D': put(&o, "dr%u", in->n & 0xE); break;
        case 'V': put(&o, "fv%u", in->n & 0xC); break;
        case 'W': put(&o, "fv%u", (in->n & 3) * 4); break;
        case '\0': return;
        default:  put(&o, "?%c", *p); break;
        }
    }
}
