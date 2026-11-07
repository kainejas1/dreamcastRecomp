#!/usr/bin/env python3
"""Turn a CD-format .cdi of a Dreamcast game into the GD-ROM layout the rest of
the project expects.

    py -3 tools/cdi_convert.py game.cdi --out extracted/game_disc

Then use extracted/game_disc/disc.cue wherever the instructions say "sa2.cue".

A .cdi is a CD rip, and differs from a Redump GD-ROM dump in four ways that
matter here, all undone by this tool:

 - its sectors are in a different format, so the data is copied out into a
   MODE1/2352 track
 - its filesystem sits at a CD address, so everything is moved to the start of
   the high-density area (LBA 45000), and every address in the volume
   descriptor, path tables and directories is moved with it
 - its boot program is scrambled, as a CD-booted program has to be
 - its boot program was patched to look for the filesystem at the CD address

Audio tracks aren't carried over. Tracks 1 and 2 of the result are blank, there
to look like the low-density area of a real disc. A rip can differ from the real
disc in other ways this can't undo, so it isn't a substitute for a dump when
tracking down a bug that looks like bad data.
"""

import argparse
import math
import mmap
import os
import struct
import sys

HD_AREA_LBA = 45000
BLANK_SECTORS = 300        # size of the blank low-density tracks
PAD_SECTORS = 150          # blank sectors kept after the last file
KATANA = b'SEGA SEGAKATANA'
SYNC = b'\x00' + b'\xff' * 10 + b'\x00'
BLANK = bytes(2048)
ECC = bytes(288)


# ---- the boot program ----

class Scramble:
    """The Dreamcast boot ROM's pseudo-random sequence."""

    def __init__(self, size):
        self.seed = size & 0xffff

    def next(self):
        self.seed = (self.seed * 2109 + 9273) & 0x7fff
        return (self.seed + 0xc000) & 0xffff


def unscramble(data):
    """A scrambled program is its 32-byte slices shuffled: 2 MB at a time for as
    long as that fits, then in smaller and smaller windows."""
    out = bytearray(len(data))
    rng = Scramble(len(data))
    source, dest, left = 0, 0, len(data)
    window = 2048 * 1024
    while window >= 32:
        while left >= window:
            slices = window // 32
            order = list(range(slices))
            for i in range(slices - 1, -1, -1):
                x = (rng.next() * i) >> 16
                order[i], order[x] = order[x], order[i]
                at = dest + 32 * order[i]
                out[at:at + 32] = data[source:source + 32]
                source += 32
            left -= window
            dest += window
        window >>= 1
    out[dest:dest + left] = data[source:source + left]
    return bytes(out)


def code_score(data):
    """How much this looks like code in its right order: how many mov.l
    @(disp,PC) instructions land on a word that's a pointer into main RAM.
    Shuffling slices keeps the instructions but not what they land on."""
    score = 0
    for at in range(0, len(data) - 1, 2):
        if data[at + 1] >> 4 != 0xd:
            continue
        target = (at & ~3) + 4 + data[at] * 4
        if target + 4 <= len(data) and data[target + 3] == 0x8c:
            score += 1
    return score


def boot_program(data):
    """Returns (the program, whether it was scrambled)."""
    plain = unscramble(data)
    return (plain, True) if code_score(plain) > code_score(data) else (data, False)


def restore_disc_address(program, base):
    """Sega's filesystem library starts from a read request: the FAD of the
    volume descriptor, 1 sector, and the "CD001" it should find there. A rip
    points that at its own address, so point it back. Returns (the program,
    how many it changed)."""
    request = struct.pack('<I', 1) + b'CD001'
    ripped = struct.pack('<I', base + 150 + 16) + request
    real = struct.pack('<I', HD_AREA_LBA + 150 + 16) + request
    return program.replace(ripped, real), program.count(ripped)


# ---- the image ----

class Image:
    def __init__(self, data):
        self.data = data
        self.first = data.find(KATANA)
        if self.first < 0:
            sys.exit('no Dreamcast IP.BIN in this image')
        self.stride = self.find_stride()
        self.total = (len(data) - self.first) // self.stride

    def find_stride(self):
        for stride in (2048, 2336, 2352):
            at = self.first + 16 * stride
            if self.data[at + 1:at + 6] == b'CD001':
                return stride
        sys.exit('no ISO 9660 volume descriptor after the IP.BIN')

    def sector(self, index):
        at = self.first + index * self.stride
        return self.data[at:at + 2048]


def ip_field(ip, at, size):
    return ip[at:at + size].decode('ascii', 'replace').strip()


def find_base(image, root_lba, root_len):
    """The LBA of sector 0. The volume's addresses are absolute, so find the root
    directory by content: its first record points at itself."""
    for index in range(17, min(image.total, 4096)):
        s = image.sector(index)
        if s[0] == 34 and s[33] == 0 and s[32] == 1 and \
                struct.unpack_from('<I', s, 2)[0] == root_lba and \
                struct.unpack_from('<I', s, 10)[0] == root_len:
            return root_lba - index
    sys.exit('root directory not found')


class Volume:
    """Reads the filesystem, keeping the sectors that have to change in `patched`."""

    def __init__(self, image):
        self.image = image
        self.patched = {}
        pvd = bytearray(image.sector(16))
        self.pvd = pvd
        self.root_lba, self.root_len = struct.unpack_from('<I', pvd, 158)[0], struct.unpack_from('<I', pvd, 166)[0]
        self.base = find_base(image, self.root_lba, self.root_len)
        self.delta = HD_AREA_LBA - self.base
        self.files = {}     # path -> (index of the first sector, size)
        self.seen = set()

    def writable(self, index):
        if index not in self.patched:
            self.patched[index] = bytearray(self.image.sector(index))
        return self.patched[index]

    def shift_record(self, buf, at):
        extent = struct.unpack_from('<I', buf, at + 2)[0]
        if extent >= self.base:
            extent += self.delta
            struct.pack_into('<I', buf, at + 2, extent)
            struct.pack_into('>I', buf, at + 6, extent)

    def walk(self, lba, length, prefix):
        first = lba - self.base
        if first in self.seen:
            return
        self.seen.add(first)
        subdirs = []
        for n in range(math.ceil(length / 2048)):
            buf = self.writable(first + n)
            at = 0
            while at < 2048 and buf[at]:
                size = buf[at]
                extent = struct.unpack_from('<I', buf, at + 2)[0]
                data_len = struct.unpack_from('<I', buf, at + 10)[0]
                name_len = buf[at + 32]
                name = bytes(buf[at + 33:at + 33 + name_len])
                if name not in (b'\x00', b'\x01'):
                    text = name.decode('ascii', 'replace').split(';')[0]
                    if buf[at + 25] & 2:
                        subdirs.append((extent, data_len, prefix + text + '/'))
                    else:
                        self.files[prefix + text] = (extent - self.base, data_len)
                self.shift_record(buf, at)
                at += size
        for extent, data_len, path in subdirs:
            self.walk(extent, data_len, path)

    def shift_path_table(self, lba, size, order):
        if not lba:
            return
        first = lba - self.base
        sectors = math.ceil(size / 2048)
        table = bytearray()
        for n in range(sectors):
            table += self.image.sector(first + n)
        at = 0
        while at < size:
            name_len = table[at]
            if not name_len:
                break
            extent = struct.unpack_from(order + 'I', table, at + 2)[0]
            struct.pack_into(order + 'I', table, at + 2, extent + self.delta)
            at += 8 + name_len + (name_len & 1)
        for n in range(sectors):
            self.patched[first + n] = table[n * 2048:(n + 1) * 2048]

    def shift_descriptor(self):
        pvd = self.pvd
        for at, order in ((80, '<'), (84, '>')):       # volume size
            struct.pack_into(order + 'I', pvd, at, struct.unpack_from(order + 'I', pvd, at)[0] + self.delta)

        table_size = struct.unpack_from('<I', pvd, 132)[0]
        for at, order in ((140, '<'), (144, '<'), (148, '>'), (152, '>')):   # path tables
            lba = struct.unpack_from(order + 'I', pvd, at)[0]
            self.shift_path_table(lba, table_size, order)
            if lba:
                struct.pack_into(order + 'I', pvd, at, lba + self.delta)

        self.shift_record(pvd, 156)                    # root directory
        self.patched[16] = pvd


def read_file(image, first, size):
    data = bytearray()
    for n in range(math.ceil(size / 2048)):
        data += image.sector(first + n)
    return bytes(data[:size])


def bcd(value):
    return (value // 10) << 4 | value % 10


def raw_sector(lba, data):
    frames = lba + 150
    header = bytes((bcd(frames // 4500), bcd(frames // 75 % 60), bcd(frames % 75), 1))
    return SYNC + header + data + ECC


def write_track(path, start_lba, count, sector_at):
    with open(path, 'wb', buffering=1 << 22) as f:
        for n in range(count):
            f.write(raw_sector(start_lba + n, sector_at(n)))


def write_disc(out_dir, volume, count):
    image, patched = volume.image, volume.patched

    def data_sector(n):
        if n in patched:
            return bytes(patched[n])
        s = image.sector(n)
        return s if len(s) == 2048 else s.ljust(2048, b'\0')

    os.makedirs(out_dir, exist_ok=True)
    write_track(os.path.join(out_dir, 'track01.bin'), 0, BLANK_SECTORS, lambda n: BLANK)
    with open(os.path.join(out_dir, 'track02.raw'), 'wb') as f:
        f.write(bytes(2352 * BLANK_SECTORS))
    write_track(os.path.join(out_dir, 'track03.bin'), HD_AREA_LBA, count, data_sector)

    with open(os.path.join(out_dir, 'disc.cue'), 'w') as f:
        f.write('REM SINGLE-DENSITY AREA\n'
                'FILE "track01.bin" BINARY\n'
                '  TRACK 01 MODE1/2352\n'
                '    INDEX 01 00:00:00\n'
                'FILE "track02.raw" BINARY\n'
                '  TRACK 02 AUDIO\n'
                '    INDEX 01 00:00:00\n'
                'REM HIGH-DENSITY AREA\n'
                'FILE "track03.bin" BINARY\n'
                '  TRACK 03 MODE1/2352\n'
                '    INDEX 01 00:00:00\n')


def convert(path, out_dir):
    with open(path, 'rb') as f:
        data = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
        image = Image(data)
        ip = image.sector(0)
        print(f'{ip_field(ip, 0x80, 32)}: {ip_field(ip, 0x40, 10)} {ip_field(ip, 0x4a, 6)}, '
              f'{image.stride}-byte sectors')

        volume = Volume(image)
        volume.walk(volume.root_lba, volume.root_len, '')
        volume.shift_descriptor()
        print(f'{len(volume.files)} files, moved from LBA {volume.base} to {HD_AREA_LBA}')

        last = max(first + math.ceil(size / 2048) for first, size in volume.files.values())
        if last > image.total:
            sys.exit(f'image is cut short: files go to sector {last}, it has {image.total}')
        count = min(last + PAD_SECTORS, image.total)

        boot_name = ip_field(ip, 0x60, 16)
        if boot_name not in volume.files:
            sys.exit(f'boot program {boot_name} not found')
        first, size = volume.files[boot_name]
        original = read_file(image, first, size)
        program, scrambled = boot_program(original)
        program, restored = restore_disc_address(program, volume.base)
        print(f'{boot_name}: {size} bytes, {"unscrambled" if scrambled else "not scrambled"}, '
              f'{restored} disc address restored')
        if program != original:
            for n in range(math.ceil(size / 2048)):
                buf = bytearray(image.sector(first + n))
                part = program[n * 2048:(n + 1) * 2048]
                buf[:len(part)] = part
                volume.patched[first + n] = buf

        write_disc(out_dir, volume, count)
        print(f'wrote {count} sectors to {os.path.join(out_dir, "disc.cue")}')
        data.close()


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('image', help='.cdi')
    ap.add_argument('--out', default='extracted/disc', help='directory for disc.cue and its tracks')
    args = ap.parse_args()
    convert(args.image, args.out)


if __name__ == '__main__':
    main()
