# dcrecomp

Static recompiler for Sega Dreamcast (SH-4) games. It translates a game's code
to C and runs it on a PC against a runtime that stands in for the hardware.
Being built with Sonic Adventure 2 (USA).

No game data is included. You need your own disc dump, and the generated C
stays on your machine (`extracted/` and `out/` are ignored by git).

## What's here

- `dcrecomp disasm` - disassemble a binary
- `dcrecomp funcs` - find the functions, show coverage
- `dcrecomp emit` - translate every function to C
- `dcrecomp selftest` - check the opcode table
- `tools/gdrom_extract.py` - get IP.BIN and 1ST_READ.BIN off a .cue/.gdi
- `tools/prs_modules.py` - unpack the code modules a game loads at runtime
- `runtime/` - memory map, HLE BIOS, GD-ROM, video timing, interrupts, sound
  RAM and AICA registers, G2 DMA, store queues, game threads on host fibers,
  PowerVR (TA + software renderer), Maple (controller + VMU), ARM7 sound CPU,
  SDL3 window

## Progress

- SH-4 decoder, disassembler, function discovery
- C emitter: all of SA2's functions translate
- runtime boots SA2: BIOS calls, disc reads, interrupts
- sound driver handshake faked, game's thread scheduler runs
- code modules (screens, stages) recompiled too; MCWARN runs
- PowerVR: TA display lists, software renderer, SDL3 window
- first frame: the memory card warning screen
- Maple: controller and VMU; through the title screen to file select
- ARM7 sound CPU: the real sound driver (manatee.drv) runs
- AICA channels: music and sound through SDL audio
- next: the first stage, then a GPU renderer

## Build

```
cmake -S . -B build -G Ninja -DCMAKE_C_COMPILER=gcc
cmake --build build
build/dcrecomp selftest
```

## Sonic Adventure 2

```
py -3 tools/gdrom_extract.py "path/to/sa2.cue" --out extracted/sa2
py -3 tools/prs_modules.py "path/to/sa2.cue" --out extracted/sa2/modules
build/dcrecomp emit extracted/sa2/1ST_READ.BIN --bin --out out/sa2 \
    --copy 0x8c000000=0x8c15aba0,0x34 --copy 0x8c000100=0x8c15ab0c,0x3c \
    --copy 0x8c000400=0x8c15ab0c,0x3c --copy 0x8c000600=0x8c15ab44,0x60 \
    --setjmp 0x8c0ccfb4 --longjmp 0x8c0ccf40 --modules extracted/sa2/modules/modules.txt
cmake -S . -B build && cmake --build build
build/sa2 extracted/sa2/1ST_READ.BIN extracted/sa2/IP.BIN "path/to/sa2.cue"
```

Debug switches: `DC_FRAMES=n`, `DC_DUMP=file`, `DC_SHOT=file.png`, `DC_NOWINDOW=1`,
`DC_HOLLYLOG=1`, `DC_IRQLOG=n`, `DC_BIOSLOG=n`, `DC_AICALOG=1`, `DC_COROLOG=n`,
`DC_PVRLOG=n`, `DC_MAPLELOG=n`, `DC_FAKEDRIVER=1`, `DC_WAV=file`, `DC_VMU=file`, `DC_PRESS=600:start,...`, and `-DDC_TRACE=ON` with `DC_TRACELOG=n`.
