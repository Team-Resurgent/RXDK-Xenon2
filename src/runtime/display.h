// What the game draws, as it draws it (display.cpp): one list per game
// update, from probes at its drawing routines (config/xenon2.json): the
// back layer's scroll, the wall map, and each object with its graphic and
// position. The HD renderer draws from these instead of the bitplanes.
#pragma once

#include "runtime/m68k.h"

enum class DrawKind : u8 { Normal, Flash, Masked };

struct DrawObject {
    u32 graphic;         // the graphic: hotspot x, y, width, height - 1, then its rows
    s16 x, y;            // where its hotspot goes on the playfield
    DrawKind kind;
};

struct DisplayList {
    u32 update;          // which game update (counts up)
    u32 screen;          // the buffer it was drawn into ($408)
    u32 back;            // the back layer's image ($4E42)
    u16 scroll;          // its row at the top of the playfield ($43C)
    u32 map;             // the wall tile map at the top row ($4E46 + $CD4)
    s16 map_y;           // the wall tiles' scroll ($CDC)
    u32 tiles;           // the wall tile graphics ($4E4A)
    bool walls;          // the walls are drawn ($D8B clear)
    static constexpr int kMaxObjects = 256;
    int count;
    DrawObject objects[kMaxObjects];
};

// Every displayed frame (lists not renewed for a while are stale).
void display_frame();

// The finished list drawn into the buffer the bitplanes show, or null.
const DisplayList *display_list_for(u32 shown);

// Debugging (build.py --boxes): each object's box drawn over the reference
// picture, whose row `top` is the playfield's first.
void display_debug_boxes(u32 *pixels, int width, int height, int top, u32 shown);
