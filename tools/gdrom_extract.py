#!/usr/bin/env python3
"""Extract files from a GD-ROM dump (Redump .cue or .gdi).

    py -3 tools/gdrom_extract.py game.cue --list
    py -3 tools/gdrom_extract.py game.cue --out extracted/game            # IP.BIN + boot file
    py -3 tools/gdrom_extract.py game.cue --out extracted/game STAGE.BIN  # specific files
    py -3 tools/gdrom_extract.py game.cue --out extracted/game --all

The game's filesystem lives in the high-density area (track 3 onward),
which starts at disc LBA 45000. Its first 16 sectors are IP.BIN, the boot
header that names the executable (normally 1ST_READ.BIN).
"""
import argparse
import os
import re
import struct
import sys

HD_AREA_LBA = 45000


class Track:
    def __init__(self, path, lba, sector_size):
        self.path = path
        self.lba = lba
        self.sector_size = sector_size
        # MODE1/2352 sectors carry a 16-byte sync+header before the data.
        self.data_offset = 16 if sector_size == 2352 else 0
        self.f = open(path, 'rb')
        self.nsectors = os.path.getsize(path) // sector_size

    def read(self, lba, count=1):
        rel = lba - self.lba
        if rel < 0 or rel + count > self.nsectors:
            raise ValueError(f'LBA {lba} is outside {os.path.basename(self.path)}')
        out = bytearray()
        for i in range(count):
            self.f.seek((rel + i) * self.sector_size + self.data_offset)
            out += self.f.read(2048)
        return bytes(out)


def open_cue(path):
    base = os.path.dirname(path)
    hd, cur_file, mode = False, None, None
    for line in open(path, encoding='utf-8', errors='replace'):
        line = line.strip()
        if line.upper().startswith('REM HIGH-DENSITY AREA'):
            hd = True
        m = re.match(r'FILE\s+"(.+)"\s+BINARY', line, re.I)
        if m:
            cur_file = os.path.join(base, m.group(1))
        m = re.match(r'TRACK\s+\d+\s+(\S+)', line, re.I)
        if m:
            mode = m.group(1).upper()
            # First data track of the high-density area holds the filesystem.
            if hd and mode.startswith('MODE1'):
                size = int(mode.split('/')[1]) if '/' in mode else 2048
                return Track(cur_file, HD_AREA_LBA, size)
    sys.exit('no MODE1 track found in the HIGH-DENSITY AREA of the cue sheet')


def open_gdi(path):
    base = os.path.dirname(path)
    lines = [l.split() for l in open(path) if l.strip()][1:]
    for parts in lines:
        num, lba, ttype, size = int(parts[0]), int(parts[1]), int(parts[2]), int(parts[3])
        name = ' '.join(parts[4:-1]).strip('"')
        if ttype == 4 and lba >= HD_AREA_LBA:
            return Track(os.path.join(base, name), lba, size)
    sys.exit('no high-density data track in .gdi')


def parse_ip(ip):
    def s(off, n):
        return ip[off:off + n].decode('ascii', 'replace').strip()
    return {
        'hardware': s(0x00, 16),
        'maker': s(0x10, 16),
        'device': s(0x20, 16),
        'area': s(0x30, 8),
        'product': s(0x40, 10),
        'version': s(0x4A, 6),
        'date': s(0x50, 16),
        'boot': s(0x60, 16),
        'company': s(0x70, 16),
        'title': s(0x80, 128),
    }


def walk(track, lba, size, prefix=''):
    data = track.read(lba, (size + 2047) // 2048)
    pos = 0
    while pos < size:
        rec_len = data[pos]
        if rec_len == 0:
            pos = (pos // 2048 + 1) * 2048   # records never span sectors
            continue
        rec = data[pos:pos + rec_len]
        ext_lba, = struct.unpack_from('<I', rec, 2)
        length, = struct.unpack_from('<I', rec, 10)
        flags = rec[25]
        name_len = rec[32]
        raw_name = rec[33:33 + name_len]
        pos += rec_len
        if raw_name in (b'\x00', b'\x01'):
            continue
        name = raw_name.decode('ascii', 'replace').split(';')[0]
        path = prefix + name
        if flags & 2:
            yield from walk(track, ext_lba, length, path + '/')
        else:
            yield path, ext_lba, length


def extract(track, entry, out_dir):
    path, lba, length = entry
    dest = os.path.join(out_dir, *path.split('/'))
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    with open(dest, 'wb') as f:
        remaining, cur = length, lba
        while remaining > 0:
            chunk = min(remaining, 2048 * 256)
            n = (chunk + 2047) // 2048
            f.write(track.read(cur, n)[:chunk])
            remaining -= chunk
            cur += n
    return dest


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('image', help='.cue or .gdi')
    ap.add_argument('files', nargs='*', help='files to extract (default: boot file)')
    ap.add_argument('--out', default='extracted')
    ap.add_argument('--list', action='store_true')
    ap.add_argument('--all', action='store_true')
    args = ap.parse_args()

    track = open_gdi(args.image) if args.image.lower().endswith('.gdi') else open_cue(args.image)

    ip_raw = track.read(HD_AREA_LBA, 16)
    if not ip_raw.startswith(b'SEGA SEGAKATANA'):
        sys.exit('sector 0 of the high-density track is not IP.BIN; wrong track or offset')
    ip = parse_ip(ip_raw)
    for k, v in ip.items():
        print(f'{k:>9}: {v}')

    pvd = track.read(HD_AREA_LBA + 16)
    if pvd[0] != 1 or pvd[1:6] != b'CD001':
        sys.exit('no ISO9660 primary volume descriptor at LBA 45016')
    root = pvd[156:156 + 34]
    root_lba, = struct.unpack_from('<I', root, 2)
    root_len, = struct.unpack_from('<I', root, 10)
    entries = list(walk(track, root_lba, root_len))
    print(f'{len(entries)} files')

    if args.list:
        for path, lba, length in sorted(entries, key=lambda e: e[1]):
            print(f'  {lba:8d}  {length:10d}  {path}')
        return

    os.makedirs(args.out, exist_ok=True)
    with open(os.path.join(args.out, 'IP.BIN'), 'wb') as f:
        f.write(ip_raw)
    print(f'wrote {os.path.join(args.out, "IP.BIN")}')

    wanted = None if args.all else {n.upper() for n in (args.files or [ip['boot']])}
    for e in entries:
        if wanted is None or e[0].upper() in wanted or os.path.basename(e[0]).upper() in wanted:
            print(f'wrote {extract(track, e, args.out)} ({e[2]} bytes)')


if __name__ == '__main__':
    main()
