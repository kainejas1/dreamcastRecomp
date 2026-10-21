#!/usr/bin/env python3
"""Compare dcrecomp's disassembly against GNU objdump, line by line.

    py -3 tools/objdump_diff.py game.elf
    py -3 tools/objdump_diff.py 1ST_READ.BIN --bin --base 0x8c010000

Needs an SH objdump, e.g. sh-elf-objdump from KallistiOS's dc-chain, or
sh4-linux-gnu-objdump (Ubuntu: apt install binutils-sh4-linux-gnu) in WSL.
Pass it with --objdump, e.g. --objdump "wsl sh4-linux-gnu-objdump".
"""
import argparse
import collections
import re
import shlex
import subprocess
import sys

LINE = re.compile(r'^\s*([0-9a-f]+):\s+((?:[0-9a-f]{2} )+)\s*(.*)$')


def normalize(asm):
    asm = asm.split('!')[0]                 # literal-value comments
    asm = re.sub(r'<[^>]*>', '', asm)       # symbol annotations
    asm = re.sub(r'\s+', ' ', asm).strip().lower()
    return asm.replace(', ', ',')


def parse(text):
    out = {}
    for line in text.splitlines():
        m = LINE.match(line)
        if m:
            out[int(m.group(1), 16)] = normalize(m.group(3))
    return out


def run(cmd):
    res = subprocess.run(cmd, capture_output=True, text=True)
    if res.returncode != 0:
        sys.exit(f"command failed: {' '.join(cmd)}\n{res.stderr}")
    return res.stdout


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('file')
    ap.add_argument('--bin', action='store_true', help='raw binary instead of ELF')
    ap.add_argument('--base', default='0x8c010000')
    ap.add_argument('--objdump', default='sh-elf-objdump')
    ap.add_argument('--dcrecomp', default='build/dcrecomp.exe' if sys.platform == 'win32' else 'build/dcrecomp')
    ap.add_argument('--show', type=int, default=40, help='mismatches to print')
    args = ap.parse_args()

    objdump = shlex.split(args.objdump)
    ours_cmd = [args.dcrecomp, 'disasm', args.file]
    if args.bin:
        ours_cmd += ['--bin', '--base', args.base]
        theirs_cmd = objdump + ['-D', '-b', 'binary', '-m', 'sh4', '-EL',
                                f'--adjust-vma={args.base}', args.file]
    else:
        theirs_cmd = objdump + ['-d', '-m', 'sh4', args.file]

    ours = parse(run(ours_cmd))
    theirs = parse(run(theirs_cmd))

    common = sorted(set(ours) & set(theirs))
    mismatches = [a for a in common if ours[a] != theirs[a]]
    by_kind = collections.Counter(
        (ours[a].split(' ')[0], theirs[a].split(' ')[0]) for a in mismatches)

    print(f'ours: {len(ours)} lines, objdump: {len(theirs)} lines, compared: {len(common)}')
    print(f'only in ours: {len(set(ours) - set(theirs))}, only in objdump: {len(set(theirs) - set(ours))}')
    print(f'mismatches: {len(mismatches)}')
    if by_kind:
        print('\nby mnemonic (ours -> objdump):')
        for (o, t), n in by_kind.most_common(30):
            print(f'  {n:6d}  {o} -> {t}')
        print('\nexamples:')
        for a in mismatches[:args.show]:
            print(f'  {a:08x}  ours: {ours[a]:<32} objdump: {theirs[a]}')
    return 1 if mismatches else 0


if __name__ == '__main__':
    sys.exit(main())
