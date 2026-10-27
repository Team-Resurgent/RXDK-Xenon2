# RXDK-Xenon2

Xenon 2 Megablast (the 1991 CDTV release) on the original Xbox, built with RXDK.

The Bitmap Brothers' own 68000 code is statically recompiled to C++: the
game logic is the original. The Amiga hardware it talks to is replaced by
native code. File and CD access are hooked at the routines the WHDLoad
install patches, and the display, sound and music move to native D3D8 and
DirectSound code.

You need your own copy of the game: the WHDLoad CDTV install
(`Xenon2_v1.7_CDTV.lha`, or its unpacked folder). No game data or generated
code is in this repository.

## Build

```
python tools/build.py --install <Xenon2_v1.7_CDTV.lha>
```

Later builds need no `--install`. You need RXDK (`C:\ProgramData\RXDK`, or
set `RXDK` / `RXDK_CLI`), Python 3 and 7-Zip (for `.lha` archives). The
output is `out/xenon2.xbe` and `out/XISO/xenon2.iso`.

## How it works

| Step | Tool | What it does |
|---|---|---|
| extract | `tools/extract.py` | copies the CDTV files into `game/data`; relocates the `X2` hunks at `$400` into `build/image.bin`, as WHDLoad's `resload_Relocate` does |
| analyse | `tools/m68k/analyze.py` | finds the functions of the main program and of the seven code overlays (Intro, Piccode, Level1-5: GEMDOS-format files loaded at `$4E26` and relocated by `$406`), from call targets, relocated pointers, jump tables and the `jmp` service table at `$E12` |
| recompile | `tools/recompile.py`, `tools/m68k/codegen.py` | writes one C++ function per 68000 function into `build/m68k`, with dispatch tables by address |
| runtime | `src/runtime` | the CPU state and helpers (`m68k.h`), dispatch, interrupts and exceptions (`cpu.cpp`), the chipset at line granularity (`chipset.cpp`), and native hooks (`hooks.cpp`) |
| Xbox | `src/platform/xbox/main.cpp` | D3D8 display, controller, pacing at 50 Hz |

`config/xenon2.json` lists the patched routines: the WHDLoad slave's OS
skips, the file loads, and the CDTV CD-audio calls (`cd_play` is track
`d0`).

## Status

The game runs in xemu: the joypad and language screens, the intro with
speech, high scores and Level 1 gameplay. It draws through the reference
renderer, which builds the frame from the bitplanes, sprites and copper as
the Amiga would.

Still to do:

- **HD renderer:** native rendering at 480p/720p from the game's own draw
  calls, with HD textures (the Xenon 2000 art where it fits, upscaled
  originals elsewhere) and 60 Hz interpolation (the game updates every third
  PAL frame).
- **Audio:** sound effects and speech as native DirectSound voices, and the
  CD tracks (Bomb the Bass) streamed from your own CDTV disc rip.
- **High scores:** saving them.

## Controller

D-pad or left stick to move. A or right trigger is button A (fire); B or X
is the second button.
