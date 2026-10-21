# dcrecomp

Static recompiler for Sega Dreamcast (SH-4) games. It reads a game's
`1ST_READ.BIN`, finds its functions and translates each one to C that runs on
a PC against a runtime that stands in for the Dreamcast hardware.

Being worked on with Sonic Adventure 2 (USA). The recompiler side works: all
6,083 functions in SA2 are found and translated, and the output compiles. The
runtime (memory map, BIOS, PowerVR2, AICA, controllers, GD-ROM) is next.

No game data is included. You need your own disc dump, and the generated C is
a translation of the game's code, so it stays on your machine (`out/` is
ignored by git).

## Build

```
cmake -S . -B build -G Ninja -DCMAKE_C_COMPILER=gcc
cmake --build build
build/dcrecomp selftest
```

## Usage

```
py -3 tools/gdrom_extract.py game.cue --out extracted/game     # IP.BIN + 1ST_READ.BIN
dcrecomp disasm extracted/game/1ST_READ.BIN --bin
dcrecomp funcs  extracted/game/1ST_READ.BIN --bin              # function list + coverage
dcrecomp emit   extracted/game/1ST_READ.BIN --bin --out out/game
dcrecomp emit   extracted/game/1ST_READ.BIN --bin --only 0x8c1004e0   # one function to stdout
```

`emit` writes `funcs.h`, `funcs_NNN.c` and `lookup.c`; build them with
`-Iruntime -Iout/game` together with `runtime/sh4_helpers.c` and a runtime.

Retail GD-ROM `1ST_READ.BIN` files are plain binaries loaded at `0x8C010000`.
Self-boot CD-R images (CDI) usually have a scrambled `1ST_READ.BIN` that must
be descrambled first.

## Layout

- `src/sh4_ops.def`, `src/sh4.c` - SH-4 instruction table, decoder, disassembler
- `src/loader.c` - ELF / raw binary loader
- `src/analysis.c` - function discovery: calls, tail calls, jump tables, pointer tables, gaps
- `src/emit.c` - C emitter
- `runtime/` - CPU state and the interface the generated code calls, plus CPU helpers
- `tools/` - GD-ROM extractor, objdump comparison script

## Notes

- FPU instructions flagged `F_FPMODE` change meaning with `FPSCR.SZ`/`FPSCR.PR`
  (single vs double, 32- vs 64-bit `fmov`). The disassembler prints the single
  form; the emitted code checks the mode at runtime.
- Branches flagged `F_DELAY` execute the following instruction first.
