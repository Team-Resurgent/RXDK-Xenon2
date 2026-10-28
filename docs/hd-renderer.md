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

1. **Display list (done).** Probes (`config/xenon2.json`, kind `probe`:
   native code before an instruction, which then runs as it is) at `$346B2`
   and the object draws (`$3414E`, `$34230`, `$34314`) build a list per
   update in `src/runtime/display.cpp`: the back layer's scroll row, the
   wall map position, and each object's graphic, x and y. The original
   blits still run, so the reference renderer stays a check:
   `python tools/build.py --boxes` draws each object's box over it.
   Checked in Level 1: the boxes sit on the ship, enemies, bullets,
   pickups and the score panel's icons. The game double-buffers at
   `$06B0A0` and `$077B20` (200 lines of `$A0` bytes); a list belongs to
   the buffer the bitplanes start the frame at, and lapses when updates
   stop (the title, shop and table screens draw without them). The wall
   turrets are wall tiles, not objects.
2. **Textures (done).** `display.cpp` decodes graphics, wall tiles (the
   map cells' top bit: masked, rows of mask + four planes; else opaque,
   four planes) and the back layer to colour indices, and composes a list
   as the blitter does: `python tools/build.py --check` logs how it
   differs from the reference picture's playfield, and in Level 1 it is 0
   of 61440 pixels (one frame where the palette changes mid-frame aside).
   The starfield is the one thing drawn with the CPU (`$35260`: four
   groups of 12 stars from `$85384`, colours 4 to 7, only on empty
   pixels), so a probe at `$35268` records it too.
3. **Drawing (done at 1x).** `src/platform/xbox/renderer.cpp` draws the
   playfield as D3D8 quads over the reference picture: the back layer
   decoded each frame into its own texture, the tiles and objects decoded
   once into an atlas keyed by their data and the palette, then the stars,
   in the game's order; the score panel stays the reference picture's.
   Back on the controller shows the reference picture alone. Next: the
   score panel, and scaling up (720p) once textures can come from a pack.
4. **Texture pack.** A replacement keyed by a hash of the decoded original
   (stable whichever address it loads at), loaded from the disc when
   present: the Xenon 2000 remake's art where it fits (level 1), upscaled
   originals elsewhere.
5. **60 Hz.** The game updates every third PAL frame; objects matched by
   their record between updates are drawn at interpolated positions, and
   the scroll likewise.
