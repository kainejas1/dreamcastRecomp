"""Small ARMv4 disassembler, for reading sound drivers (ARM7 code).

    py -3 tools/armdis.py FILE START END [SKIP]

SKIP bytes are dropped from the start of the file first: SA2's MANATEE.DRV
has a 32-byte header, so `... MANATEE.DRV 0x100 0x200 0x20` shows ARM
addresses 0x100-0x200 as they are in sound RAM. Literal loads show the value.
"""
import struct
import sys

COND = ['eq', 'ne', 'cs', 'cc', 'mi', 'pl', 'vs', 'vc', 'hi', 'ls', 'ge', 'lt', 'gt', 'le', '', 'nv']
DP = ['and', 'eor', 'sub', 'rsb', 'add', 'adc', 'sbc', 'rsc', 'tst', 'teq', 'cmp', 'cmn', 'orr', 'mov', 'bic', 'mvn']
SH = ['lsl', 'lsr', 'asr', 'ror']
R = ['r0', 'r1', 'r2', 'r3', 'r4', 'r5', 'r6', 'r7', 'r8', 'r9', 'r10', 'r11', 'r12', 'sp', 'lr', 'pc']


def ror(v, n):
    n &= 31
    return ((v >> n) | (v << (32 - n))) & 0xffffffff


def shift_op(w):
    rm = R[w & 15]
    t = (w >> 5) & 3
    if w & 0x10:
        return f'{rm}, {SH[t]} {R[(w >> 8) & 15]}'
    amt = (w >> 7) & 31
    if amt == 0:
        if t == 0: return rm
        if t == 3: return f'{rm}, rrx'
        amt = 32
    return f'{rm}, {SH[t]} #{amt}'


def dis(w, pc, data):
    c = COND[w >> 28]
    if (w >> 28) == 15:
        return f'.word {w:08x}'
    if (w & 0x0ffffff0) == 0x012fff10:
        return f'bx{c} {R[w & 15]}'
    if (w & 0x0e000000) == 0x0a000000:
        off = w & 0xffffff
        if off & 0x800000: off -= 0x1000000
        return f'b{"l" if w & 0x01000000 else ""}{c} {pc + 8 + off * 4:08x}'
    if (w & 0x0f000000) == 0x0f000000:
        return f'swi{c} #{w & 0xffffff:x}'
    if (w & 0x0fc000f0) == 0x00000090:
        rd, rn, rs, rm = (w >> 16) & 15, (w >> 12) & 15, (w >> 8) & 15, w & 15
        if w & 0x00200000:
            return f'mla{c}{"s" if w & 0x100000 else ""} {R[rd]}, {R[rm]}, {R[rs]}, {R[rn]}'
        return f'mul{c}{"s" if w & 0x100000 else ""} {R[rd]}, {R[rm]}, {R[rs]}'
    if (w & 0x0f8000f0) == 0x00800090:
        names = ['umull', 'umlal', 'smull', 'smlal']
        return f'{names[(w >> 21) & 3]}{c} {R[(w >> 12) & 15]}, {R[(w >> 16) & 15]}, {R[w & 15]}, {R[(w >> 8) & 15]}'
    if (w & 0x0fb00ff0) == 0x01000090:
        return f'swp{c}{"b" if w & 0x400000 else ""} {R[(w >> 12) & 15]}, {R[w & 15]}, [{R[(w >> 16) & 15]}]'
    if (w & 0x0fbf0fff) == 0x010f0000:
        return f'mrs{c} {R[(w >> 12) & 15]}, {"spsr" if w & 0x400000 else "cpsr"}'
    if (w & 0x0db0f000) == 0x0120f000:
        psr = 'spsr' if w & 0x400000 else 'cpsr'
        f = ''.join(x for b, x in ((16, 'c'), (17, 'x'), (18, 's'), (19, 'f')) if w & (1 << b))
        src = f'#{ror(w & 0xff, ((w >> 8) & 15) * 2):x}' if w & 0x02000000 else R[w & 15]
        return f'msr{c} {psr}_{f}, {src}'
    if (w & 0x0e000090) == 0x00000090 and (w & 0x60):   # halfword / signed transfers
        sh = (w >> 5) & 3
        op = ('ldr' if w & 0x100000 else 'str') + {1: 'h', 2: 'sb', 3: 'sh'}[sh]
        rn, rd = R[(w >> 16) & 15], R[(w >> 12) & 15]
        off = f'#{((w >> 4) & 0xf0) | (w & 15):x}' if w & 0x400000 else R[w & 15]
        sign = '' if w & 0x800000 else '-'
        if w & 0x1000000:
            return f'{op}{c} {rd}, [{rn}, {sign}{off}]{"!" if w & 0x200000 else ""}'
        return f'{op}{c} {rd}, [{rn}], {sign}{off}'
    if (w & 0x0c000000) == 0x00000000:   # data processing
        op = (w >> 21) & 15
        s = 's' if (w & 0x100000) and op not in (8, 9, 10, 11) else ''
        rn, rd = R[(w >> 16) & 15], R[(w >> 12) & 15]
        op2 = f'#{ror(w & 0xff, ((w >> 8) & 15) * 2):x}' if w & 0x02000000 else shift_op(w)
        if op in (13, 15): return f'{DP[op]}{c}{s} {rd}, {op2}'
        if op in (8, 9, 10, 11): return f'{DP[op]}{c} {rn}, {op2}'
        return f'{DP[op]}{c}{s} {rd}, {rn}, {op2}'
    if (w & 0x0c000000) == 0x04000000:   # ldr/str
        op = ('ldr' if w & 0x100000 else 'str') + ('b' if w & 0x400000 else '')
        rn, rd = (w >> 16) & 15, R[(w >> 12) & 15]
        sign = '' if w & 0x800000 else '-'
        if w & 0x02000000:
            off = sign + shift_op(w)
        else:
            off = f'#{sign}{w & 0xfff:x}'
            if rn == 15 and (w & 0x1000000):
                a = pc + 8 + ((w & 0xfff) if w & 0x800000 else -(w & 0xfff))
                lit = struct.unpack_from('<I', data, a)[0] if a + 4 <= len(data) else 0
                return f'{op}{c} {rd}, [pc, {off}]   ; ={lit:08x} (at {a:x})'
        if w & 0x1000000:
            return f'{op}{c} {rd}, [{R[rn]}, {off}]{"!" if w & 0x200000 else ""}'
        return f'{op}{c} {rd}, [{R[rn]}], {off}'
    if (w & 0x0e000000) == 0x08000000:   # ldm/stm
        op = 'ldm' if w & 0x100000 else 'stm'
        mode = ['da', 'ia', 'db', 'ib'][(w >> 23) & 3]
        regs = ', '.join(R[i] for i in range(16) if w & (1 << i))
        return f'{op}{mode}{c} {R[(w >> 16) & 15]}{"!" if w & 0x200000 else ""}, {{{regs}}}{"^" if w & 0x400000 else ""}'
    return f'??? {w:08x}'


data = open(sys.argv[1], 'rb').read()[int(sys.argv[4], 0) if len(sys.argv) > 4 else 0:]
start, end = int(sys.argv[2], 0), int(sys.argv[3], 0)
for pc in range(start, end, 4):
    w = struct.unpack_from('<I', data, pc)[0]
    print(f'{pc:06x}: {w:08x}  {dis(w, pc, data)}')
