#!/usr/bin/env python3
"""Tests tools/cdi_convert.py with no game data: builds a tiny CD-format image
with a scrambled boot program, converts it, then reads the result back with the
project's own gdrom_extract.py the way the real pipeline does.

The tiny image has the sector format and addresses of a real rip, 2336-byte
sectors with the filesystem at LBA 11700.
"""

import os
import struct
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'tools'))

import cdi_convert          # noqa: E402
import gdrom_extract as gd  # noqa: E402

BASE = 11700
STRIDE = 2336
LEAD = 5000        # bytes of something else ahead of the data track

SECTORS = {'pvd': 16, 'end': 17, 'path_l': 18, 'path_m': 19, 'root': 20, 'sub': 21, 'boot': 30}


def both(value):
    return struct.pack('<I', value) + struct.pack('>I', value)


def record(extent, size, name, directory=False):
    length = 33 + len(name)
    length += length & 1
    return bytes((length, 0)) + both(extent) + both(size) + bytes(7) + \
        bytes((2 if directory else 0, 0, 0)) + b'\x01\x00\x00\x01' + bytes((len(name),)) + name + \
        bytes(length - 33 - len(name))


def directory(self_lba, parent_lba, entries):
    data = record(self_lba, 2048, b'\x00', True) + record(parent_lba, 2048, b'\x01', True)
    for entry in entries:
        data += entry
    return data.ljust(2048, b'\0')


def scramble(data):
    """The inverse of what the boot ROM does, written out separately here."""
    out = bytearray()
    seed = len(data) & 0xffff
    start, left = 0, len(data)
    window = 2048 * 1024
    while window >= 32:
        while left >= window:
            slices = window // 32
            order = list(range(slices))
            for i in range(slices - 1, -1, -1):
                seed = (seed * 2109 + 9273) & 0x7fff
                x = (((seed + 0xc000) & 0xffff) * i) >> 16
                order[i], order[x] = order[x], order[i]
                at = start + 32 * order[i]
                out += data[at:at + 32]
            left -= window
            start += window
        window >>= 1
    return bytes(out) + data[start:start + left]


def code(size):
    """Looks like SH-4 code: mov.l instructions far from the pointers they load."""
    out = bytearray(size)
    for block in range(0, size - 1024, 1024):
        for word in range(0, 256, 2):
            target = block + 900
            disp = (target - ((block + word) & ~3) - 4) // 4
            struct.pack_into('<H', out, block + word, 0xd000 | disp)
        struct.pack_into('<I', out, block + 900, 0x8c010000 + block)
    return bytes(out)


def filesystem_request(fad):
    return struct.pack('<II', fad, 1) + b'CD001'


def build_image(path, boot, other):
    ip = bytearray(2048)
    ip[0:16] = b'SEGA SEGAKATANA '
    ip[0x40:0x4a] = b'MK-00000  '
    ip[0x4a:0x50] = b'V1.000'
    ip[0x60:0x70] = b'1ST_READ.BIN'.ljust(16)
    ip[0x80:0xa0] = b'TEST GAME'.ljust(32)

    boot_sectors = (len(boot) + 2047) // 2048
    other_at = SECTORS['boot'] + boot_sectors
    other_sectors = (len(other) + 2047) // 2048
    count = other_at + other_sectors
    root_lba, sub_lba = BASE + SECTORS['root'], BASE + SECTORS['sub']

    pvd = bytearray(2048)
    pvd[0:6] = b'\x01CD001'
    pvd[80:88] = both(count)
    pvd[132:140] = both(22)
    struct.pack_into('<I', pvd, 140, BASE + SECTORS['path_l'])
    struct.pack_into('>I', pvd, 148, BASE + SECTORS['path_m'])
    pvd[156:190] = record(root_lba, 2048, b'\x00', True)

    def path_table(order):
        entry = bytes((1, 0)) + struct.pack(order + 'I', root_lba) + struct.pack(order + 'H', 1) + b'\0\0'
        entry += bytes((3, 0)) + struct.pack(order + 'I', sub_lba) + struct.pack(order + 'H', 1) + b'SUB\0'
        return entry.ljust(2048, b'\0')

    sectors = {
        0: bytes(ip),
        SECTORS['pvd']: bytes(pvd),
        SECTORS['end']: b'\xffCD001'.ljust(2048, b'\0'),
        SECTORS['path_l']: path_table('<'),
        SECTORS['path_m']: path_table('>'),
        SECTORS['root']: directory(root_lba, root_lba, [
            record(BASE + SECTORS['boot'], len(boot), b'1ST_READ.BIN;1'),
            record(sub_lba, 2048, b'SUB', True),
        ]),
        SECTORS['sub']: directory(sub_lba, root_lba, [
            record(BASE + other_at, len(other), b'OTHER.DAT;1'),
        ]),
    }
    scrambled = scramble(boot)
    for n in range(boot_sectors):
        sectors[SECTORS['boot'] + n] = scrambled[n * 2048:(n + 1) * 2048].ljust(2048, b'\0')
    for n in range(other_sectors):
        sectors[other_at + n] = other[n * 2048:(n + 1) * 2048].ljust(2048, b'\0')

    with open(path, 'wb') as f:
        f.write(bytes(range(256)) * (LEAD // 256) + bytes(LEAD % 256))
        for n in range(count):
            f.write(sectors.get(n, bytes(2048)).ljust(STRIDE, b'\xee'))
        f.write(b'trailer')


def le(buf, at):
    return struct.unpack_from('<I', buf, at)[0]


def be(buf, at):
    return struct.unpack_from('>I', buf, at)[0]


failed = False


def expect(what, actual, wanted):
    global failed
    if actual != wanted:
        shown = (actual, wanted) if not isinstance(actual, bytes) else (f'{len(actual)} bytes', f'{len(wanted)} bytes')
        print(f'{what}: got {shown[0]}, expected {shown[1]}')
        failed = True


def test_unscramble_undoes_scramble():
    for size in (32, 33, 1024, 4096 + 100, 100000):
        data = bytes((i * 7 + (i >> 8)) & 0xff for i in range(size))
        expect(f'unscramble {size}', cdi_convert.unscramble(scramble(data)), data)


def test_only_scrambled_programs_get_unscrambled():
    data = code(0x10000 + 300)
    program, was_scrambled = cdi_convert.boot_program(scramble(data))
    expect('scrambled program', program, data)
    expect('scrambled program detected', was_scrambled, True)
    program, was_scrambled = cdi_convert.boot_program(data)
    expect('plain program', program, data)
    expect('plain program detected', was_scrambled, False)


def test_converted_disc_reads_like_a_gd_rom():
    boot = code(0x10000 + 300)
    boot = boot[:0x8000] + filesystem_request(BASE + 150 + 16) + boot[0x8000 + 13:]
    other = bytes((i * 13) & 0xff for i in range(5000))
    with tempfile.TemporaryDirectory() as tmp:
        cdi = os.path.join(tmp, 'game.cdi')
        out = os.path.join(tmp, 'disc')
        build_image(cdi, boot, other)
        cdi_convert.convert(cdi, out)

        track = gd.open_cue(os.path.join(out, 'disc.cue'))
        expect('sector size', track.sector_size, 2352)
        expect('track position', track.lba, 45000)

        pvd = track.read(45016)
        expect('descriptor', pvd[1:6], b'CD001')
        root_lba = le(pvd, 158)
        expect('root moved', root_lba, 45000 + SECTORS['root'])
        expect('root moved (big endian)', be(pvd, 162), root_lba)
        expect('path table moved', le(pvd, 140), 45000 + SECTORS['path_l'])
        expect('path table moved (big endian)', be(pvd, 148), 45000 + SECTORS['path_m'])

        path_l = track.read(45000 + SECTORS['path_l'])
        path_m = track.read(45000 + SECTORS['path_m'])
        expect('path table entry', le(path_l, 2), root_lba)
        expect('path table entry 2', le(path_l, 12), 45000 + SECTORS['sub'])
        expect('path table entry (big endian)', be(path_m, 2), root_lba)
        expect('path table entry 2 (big endian)', be(path_m, 12), 45000 + SECTORS['sub'])

        files = {path: (lba, size) for path, lba, size in gd.walk(track, root_lba, le(pvd, 166))}
        expect('files', sorted(files), ['1ST_READ.BIN', 'SUB/OTHER.DAT'])

        def contents(name):
            lba, size = files[name]
            return track.read(lba, (size + 2047) // 2048)[:size]

        restored = boot.replace(filesystem_request(BASE + 150 + 16), filesystem_request(45166))
        expect('boot program looks at the GD-ROM address', contents('1ST_READ.BIN'), restored)
        expect('other file', contents('SUB/OTHER.DAT'), other)
        expect('IP.BIN', track.read(45000)[:15], b'SEGA SEGAKATANA')
        track.f.close()


def main():
    test_unscramble_undoes_scramble()
    test_only_scrambled_programs_get_unscrambled()
    test_converted_disc_reads_like_a_gd_rom()
    print('CDI CONVERT TESTS FAILED' if failed else 'CDI CONVERT TESTS PASSED')
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
