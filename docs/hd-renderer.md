# HD renderer: design

The reference renderer (`src/runtime/chipset.cpp`) rebuilds the Amiga's
picture line by line from its bitplanes, sprites and copper: 320x256, the
game's own pixels. The HD renderer instead draws what the game *meant* to
draw, from its own drawing routines, as textured quads at 480p/720p, so
the art can be replaced (an HD texture pack) and movement smoothed to 60 Hz.

## What the game draws each frame

All blitter work starts in seven routines of the main program (none in the
level overlays). Per frame, after the main loop's wait for the vertical
blank count (`$422`, advanced by `$412` = 3: the game updates at ~17 Hz):

| Layer | Routine | Inputs |
|---|---|---|
| back layer | `$346B2`, first blits | a pre-drawn image at `($4E42)`, 192 lines of `$50` bytes, wrapping; the row at the top of the screen is `$43C` (the scroll) |
| rock walls | `$346B2`, the tile loop (`$34784`) | a tile map at `($4E46)` + `$CD4`, 20x13 tiles of 16x16, scrolled by `$CDC`; tile graphics at `($4E4A)`; skipped while `$D8B` is set |
| objects | `$34142`, `$34230`, `$34308` (and the others with a write to `BLTSIZE`) | an object record in `a0`: x = `32(a0)`, y = `36(a0)`, graphics = `22(a0)` (its size and planes worked out by `$33F0C`/`$33F2E`/`$33FAA`); `$34308` draws through a mask built at `$85134` |
| ship, HUD | hardware sprites and the bottom rows | the chipset's sprite DMA; the score panel below the playfield |

The screen buffer is at `($408)`, interleaved: `$A0` bytes a line.

## Plan

1. **Display list.** Hooks at the layer routines record, every game update,
   the back layer's scroll row, the wall map position, and each object
   drawn: (graphics pointer, x, y, routine, object record). The original
   blits still run, so the reference renderer stays a check.
2. **Textures.** Each graphics pointer decoded once (planar to RGBA, with
   the palette from the colour registers and the copper) and cached; the
   back layer and the tile set as textures of their own.
3. **Drawing.** D3D8 quads at 480p/720p in the same coordinates (the
   picture's 320x256 scaled), in layer order: back layer, walls, objects,
   sprites, HUD.
4. **Texture pack.** A replacement keyed by a hash of the decoded original
   (stable whichever address it loads at), loaded from the disc when
   present: the Xenon 2000 remake's art where it fits (level 1), upscaled
   originals elsewhere.
5. **60 Hz.** The game updates every third PAL frame; objects matched by
   their record between updates are drawn at interpolated positions, and
   the scroll likewise.
