// What the game draws, as it draws it (display.cpp): one list per game
// update, from probes at its drawing routines (config/xenon2.json): the
// back layer, the wall tiles, and each object with its graphic and
// position. The HD renderer draws from these instead of the bitplanes.
#pragma once

#include "runtime/m68k.h"

constexpr int kPlayW = 320, kPlayH = 192;          // the playfield
constexpr int kMapCols = 20, kMapRows = 13, kTile = 16;
constexpr u8 kClear = 0xFF;                         // a decoded pixel not drawn

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
    u8 back_rows[kPlayH * 80];       // the back layer as drawn: 2 planes of 40 bytes a row
    u16 map_cells[kMapRows * kMapCols];
    static constexpr int kStars = 48;      // four groups of 12, colours 4 to 7
    u16 stars[kStars][2];                  // a byte offset into the buffer, and the pixel's bit in its word
    bool has_stars;
    static constexpr int kMaxObjects = 256;
    int count;
    DrawObject objects[kMaxObjects];
};

// The frame on screen: its list (null when the game shows something drawn
// without updates), the picture's row the playfield starts at, and the
// palette at its first line.
struct Shown {
    const DisplayList *list;
    int top;
    u32 palette[32];     // 0xRRGGBB
};
const Shown &display_shown();

// Called by the chipset: every displayed frame, with the buffer its
// bitplanes start at, the playfield's first row and the palette there.
void display_frame(u32 shown, int top, const u32 *palette);

// Decoders, to colour indices (kClear where nothing is drawn).
struct GraphicSize { int hx, hy, w, h; };
GraphicSize graphic_size(u32 graphic);
void decode_graphic(u32 graphic, bool flash, u8 *out);          // w x h
void decode_tile(u32 tiles, u16 cell, u8 *out);                  // 16 x 16 (opaque or masked by the cell)
void decode_back(const DisplayList &l, u8 *out);                // kPlayW x kPlayH, from the top row

// Star i's playfield position (false: none). It is colour 4 + i / 12, on
// an empty (colour 0) pixel.
bool star_position(const DisplayList &l, int i, int &x, int &y);

// The list as the blitter composes it, in colour indices (kPlayW x kPlayH).
void display_compose(const DisplayList &l, u8 *out);

// Debugging (build.py --boxes): each object's box drawn over the reference
// picture, whose row `top` is the playfield's first.
void display_debug_boxes(u32 *pixels, int width, int height);

// Debugging (build.py --check): the composed list against the reference
// picture's playfield, logged now and then.
void display_check(const u32 *pixels, int width, int height);
