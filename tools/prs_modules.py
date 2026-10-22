#!/usr/bin/env python3
"""Find the code modules on a GD-ROM and unpack them.

    py -3 tools/prs_modules.py game.cue --list
    py -3 tools/prs_modules.py game.cue --out extracted/sa2/modules

SA2 keeps code for its screens and modes in PRS-compressed files that it
unpacks to 0x8C500000 and calls through a header:

    +0x00  name, 8 chars, space padded ("MCWARN  ")
    +0x08  1
    +0x0c  entry points, absolute addresses inside the module

A file counts as a module if it unpacks to that: a printable name and a first
entry point inside it. Each one is written as FILE.BIN (named after the file:
STG09.PRS says STG10 in its header too), plus modules.txt listing them with
their entry points, for `dcrecomp emit ... --modules extracted/sa2/modules/modules.txt`.
"""
import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gdrom_extract as gd

BASE = 0x8C500000


def prs_unpack(src, limit=None):
    """Sega's PRS (LZ77): control bits LSB first, 1 = literal byte, 01 = long
    copy (13-bit offset, 3-bit size or an extra size byte), 00 = short copy
    (2-bit size, 8-bit offset). A long copy with offset 0 ends the data."""
    out = bytearray()
    i = 0
    cmd = bits = 0

    def bit():
        nonlocal cmd, bits, i
        if bits == 0:
            cmd = src[i]
            i += 1
            bits = 8
        b = cmd & 1
        cmd >>= 1
        bits -= 1
        return b

    try:
        while limit is None or len(out) < limit:
            if bit():
                out.append(src[i])
                i += 1
                continue
            if bit():
                data = src[i] | src[i + 1] << 8
                i += 2
                if data == 0:
                    break
                off = (data >> 3) - 0x2000
                size = data & 7
                if size == 0:
                    size = src[i] + 1
                    i += 1
                else:
                    size += 2
            else:
                size = (bit() << 1 | bit()) + 2
                off = src[i] - 0x100
                i += 1
            start = len(out) + off
            if start < 0:
                return None
            for k in range(size):
                out.append(out[start + k])
    except IndexError:
        return None
    return bytes(out)


def module_name(data, size):
    if not data or len(data) < 0x10:
        return None
    name = data[:8]
    if not all(0x20 <= c < 0x7f for c in name) or name[0] == 0x20:
        return None
    entry, = struct.unpack_from('<I', data, 0x0c)
    if not BASE <= entry < BASE + size:
        return None
    return name.decode('ascii').strip()


def entries(data):
    out = []
    for off in (0x0c, 0x10, 0x14):
        p, = struct.unpack_from('<I', data, off)
        if BASE <= p < BASE + len(data):
            out.append(p)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('image', help='.cue or .gdi')
    ap.add_argument('--out', default='extracted/modules')
    ap.add_argument('--list', action='store_true')
    args = ap.parse_args()

    track = gd.open_gdi(args.image) if args.image.lower().endswith('.gdi') else gd.open_cue(args.image)
    pvd = track.read(gd.HD_AREA_LBA + 16)
    root = pvd[156:156 + 34]
    root_lba, = struct.unpack_from('<I', root, 2)
    root_len, = struct.unpack_from('<I', root, 10)

    found = []
    for path, lba, length in gd.walk(track, root_lba, root_len):
        if not path.upper().endswith('.PRS'):
            continue
        packed = track.read(lba, (length + 2047) // 2048)[:length]
        # the header decides, so only unpack the start first
        head = prs_unpack(packed, 0x20)
        if not head or not module_name(head, 1 << 20):
            continue
        data = prs_unpack(packed)
        header = module_name(data, len(data)) if data else None
        if header:
            name = os.path.splitext(os.path.basename(path))[0].upper()
            found.append((name, header, path, data))

    for name, header, path, data in found:
        print(f'  {name:14s} {header:10s} {len(data):8d} bytes  {path}')
    print(f'{len(found)} modules')
    if args.list:
        return

    os.makedirs(args.out, exist_ok=True)
    with open(os.path.join(args.out, 'modules.txt'), 'w') as lst:
        lst.write('# NAME  FILE  BASE  ENTRY...   (tools/prs_modules.py)\n')
        for name, header, path, data in found:
            with open(os.path.join(args.out, name + '.BIN'), 'wb') as f:
                f.write(data)
            lst.write(f'{name} {name}.BIN 0x{BASE:08x} ' + ' '.join(f'0x{e:08x}' for e in entries(data)) + '\n')
    print(f'wrote them and modules.txt to {args.out}')


if __name__ == '__main__':
    main()
