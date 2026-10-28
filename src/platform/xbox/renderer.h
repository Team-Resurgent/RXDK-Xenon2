// The native playfield (renderer.cpp): the game's display list drawn as
// textured quads over the reference picture's playfield.
#pragma once

#include <xtl.h>

void renderer_init(IDirect3DDevice8 *device);

// Draws the shown frame's playfield (runtime/display.h), if it has a list:
// picture pixel (x, y) is at screen (x * sx - .5, y0 + y * sy).
// Returns false when there is none (the reference picture stands).
bool renderer_draw_playfield(float sx, float y0, float sy);
