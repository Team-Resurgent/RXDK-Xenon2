// The display list: probes at the game's drawing routines (config/xenon2.json
// "probe" patches; the routines themselves still run) record what each game
// update draws. An update starts at $346B2, which draws the back layer and
// the walls into the buffer at ($408); the objects follow. The game
// double-buffers, so the last two finished lists are kept, by buffer.
//
// The formats, from the game's blits:
//  - an object's graphic: hotspot x, y, width, height - 1, then rows of the
//    four bitplanes and a mask, a word each (two when wider than 16);
//  - a wall tile: a map cell (a word) is an offset / 16 into the tiles;
//    with its top bit clear the tile is opaque, 16 rows of four planes,
//    with it set it is masked, 16 rows of the mask and four planes;
//  - the back layer: 192 rows of two bitplanes (40 bytes each), wrapping,
//    its row `scroll` at the top; the other two planes are clear under it.
#include "runtime/display.h"

#include "runtime/platform.h"

#include <cstring>

namespace {

DisplayList lists[3];        // two finished (one per buffer), one being built
DisplayList *building;
u32 updates;
u32 frames;                  // displayed frames (display_frame)
u32 last_finished;           // the frame the last list was finished on
constexpr u32 kStaleFrames = 12;     // no update for this long: the game shows something else
Shown shown_frame;

void add(DrawKind kind) {
    if (!building || building->count == DisplayList::kMaxObjects) return;
    DrawObject &o = building->objects[building->count++];
    o.graphic = R.a[0] & 0xFFFFFF;
    o.x = s16(R.d[0]);
    o.y = s16(R.d[1]);
    o.kind = kind;
}

void display_log(const DisplayList &l) {
    platform_log("display: update %u screen $%06X back $%06X scroll %u map $%06X y %d tiles $%06X%s, %d objects:",
                 l.update, l.screen, l.back, l.scroll, l.map, l.map_y, l.tiles, l.walls ? "" : " (no walls)", l.count);
    for (int i = 0; i < l.count && i < 12; ++i)
        platform_log(" %c$%06X@%d,%d", "NFM"[int(l.objects[i].kind)], l.objects[i].graphic, l.objects[i].x,
                     l.objects[i].y);
    platform_log("\n");
}

const DisplayList *list_for(u32 shown) {
    // A finished list (not the one being built) drawn into the buffer the
    // bitplanes show (they may start a few lines into it), while updates
    // still come (the title, shop and table screens draw without them).
    if (frames - last_finished > kStaleFrames) return nullptr;
    for (const DisplayList &l : lists)
        if (&l != building && l.screen && shown >= l.screen && shown - l.screen < 0x2000) return &l;
    return nullptr;
}

// Pixels not clear in a w x h block onto the playfield at x0, y0.
void put(u8 *out, const u8 *block, int x0, int y0, int w, int h) {
    for (int y = 0; y < h; ++y) {
        if (y0 + y < 0 || y0 + y >= kPlayH) continue;
        for (int x = 0; x < w; ++x)
            if (x0 + x >= 0 && x0 + x < kPlayW && block[y * w + x] != kClear)
                out[(y0 + y) * kPlayW + x0 + x] = block[y * w + x];
    }
}

} // namespace

void probe_update() {
    // The list being built is finished: it replaces the one for its buffer.
    if (building) {
        last_finished = frames;
        if (building->update % 100 == 0) display_log(*building);
        for (DisplayList &l : lists)
            if (&l != building && l.screen == building->screen) l.screen = 0;
    }
    DisplayList *next = nullptr;
    for (DisplayList &l : lists)
        if (&l != building && !l.screen) next = &l;
    if (!next) next = &lists[0] == building ? &lists[1] : &lists[0];
    building = next;
    DisplayList &l = *building;
    l.update = ++updates;
    l.screen = rl(0x408) & 0xFFFFFF;
    l.back = rl(0x4E42) & 0xFFFFFF;
    l.scroll = u16(rw(0x43C) % kPlayH);
    l.map = (rl(0x4E46) + s16(rw(0xCD4))) & 0xFFFFFF;
    l.map_y = s16(rw(0xCDC));
    l.tiles = rl(0x4E4A) & 0xFFFFFF;
    l.walls = !rb(0xD8B);
    l.count = 0;
    l.has_stars = false;
    // What the update draws from, as it is now (the game writes new rows of
    // the back layer, and moves on in the map, as it scrolls).
    if (l.back + sizeof(l.back_rows) <= kChipSize) std::memcpy(l.back_rows, MEM + l.back, sizeof(l.back_rows));
    for (int i = 0; i < kMapRows * kMapCols; ++i) l.map_cells[i] = rw(l.map + 2 * u32(i));
}

void probe_stars() {
    // $35268: the stars have moved; the game plots them next (CPU writes,
    // not blits), each only on an empty pixel: entries of offset, bit, and
    // a word it doesn't use here, at $85384.
    if (!building) return;
    for (int i = 0; i < DisplayList::kStars; ++i) {
        building->stars[i][0] = rw(0x85384 + 6 * u32(i));
        building->stars[i][1] = rw(0x85384 + 6 * u32(i) + 2);
    }
    building->has_stars = true;
}

void probe_object() { add(DrawKind::Normal); }
void probe_object_flash() { add(DrawKind::Flash); }
void probe_object_masked() { add(DrawKind::Masked); }

const Shown &display_shown() { return shown_frame; }

void display_frame(u32 shown, int top, const u32 *palette) {
    ++frames;
    shown_frame.list = list_for(shown);
    // The playfield's first row: the picture's `top`, less the lines the
    // bitplanes start into the buffer ($A0 bytes a line: 4 planes, interleaved).
    shown_frame.top = shown_frame.list ? top - int(shown - shown_frame.list->screen) / 0xA0 : top;
    std::memcpy(shown_frame.palette, palette, sizeof(shown_frame.palette));
}

GraphicSize graphic_size(u32 g) {
    return {s16(rw(g)), s16(rw(g + 2)), rw(g + 4), rw(g + 6) + 1};
}

void decode_graphic(u32 g, bool flash, u8 *out) {
    const GraphicSize s = graphic_size(g);
    const int words = s.w > 16 ? 2 : 1;
    const u32 stride = 10 * words;              // a row: four planes, then the mask
    for (int y = 0; y < s.h; ++y) {
        const u32 row = g + 8 + u32(y) * stride;
        for (int x = 0; x < s.w; ++x) {
            const int word = x >> 4, bit = 15 - (x & 15);
            const auto plane = [&](int p) { return rw(row + u32(p * words + word) * 2) >> bit & 1; };
            u8 &o = out[y * s.w + x];
            if (!plane(4)) o = kClear;
            else if (flash) o = 15;                 // the mask into every plane
            else o = u8(plane(0) | plane(1) << 1 | plane(2) << 2 | plane(3) << 3);
        }
    }
}

void decode_tile(u32 tiles, u16 cell, u8 *out) {
    const bool masked = cell & 0x8000;
    const u32 at = tiles + u32(cell & 0x7FFF) * 16;
    for (int y = 0; y < kTile; ++y)
        for (int x = 0; x < kTile; ++x) {
            const int bit = 15 - x;
            if (masked) {                           // a row: the mask, then four planes
                const u32 row = at + u32(y) * 10;
                if (!(rw(row) >> bit & 1)) { out[y * kTile + x] = kClear; continue; }
                u8 c = 0;
                for (int p = 0; p < 4; ++p) c |= u8((rw(row + 2 + 2 * u32(p)) >> bit & 1) << p);
                out[y * kTile + x] = c;
            } else {                                // a row: four planes
                u8 c = 0;
                for (int p = 0; p < 4; ++p) c |= u8((rw(at + u32(y * 4 + p) * 2) >> bit & 1) << p);
                out[y * kTile + x] = c;
            }
        }
}

void decode_back(const DisplayList &l, u8 *out) {
    for (int y = 0; y < kPlayH; ++y) {
        const u8 *row = l.back_rows + ((l.scroll + y) % kPlayH) * 80;
        for (int x = 0; x < kPlayW; ++x) {
            const int bit = 7 - (x & 7);
            out[y * kPlayW + x] = u8((row[x >> 3] >> bit & 1) | (row[40 + (x >> 3)] >> bit & 1) << 1);
        }
    }
}

bool star_position(const DisplayList &l, int i, int &x, int &y) {
    const u16 offset = l.stars[i][0], bit = l.stars[i][1];
    if (!bit) return false;
    int b = 15;
    while (!(bit >> b & 1)) --b;
    y = offset / 0xA0;
    x = (offset % 0xA0) * 8 + (15 - b);
    return y < kPlayH && x < kPlayW;
}

void display_compose(const DisplayList &l, u8 *out) {
    decode_back(l, out);
    static u8 block[64 * 256];
    if (l.walls) {
        // Rows of tiles from -map_y down, as far as the playfield goes.
        for (int r = 0; r < kMapRows; ++r) {
            const int y = r * kTile - l.map_y;
            if (y <= -kTile || y >= kPlayH) continue;
            for (int c = 0; c < kMapCols; ++c) {
                const u16 cell = l.map_cells[r * kMapCols + c];
                if (!cell) continue;
                decode_tile(l.tiles, cell, block);
                put(out, block, c * kTile, y, kTile, kTile);
            }
        }
    }
    if (l.has_stars)
        for (int i = 0; i < DisplayList::kStars; ++i) {
            int x, y;
            if (!star_position(l, i, x, y)) continue;
            u8 &p = out[y * kPlayW + x];
            if (p == 0) p = u8(4 + i / 12);
        }
    for (int i = 0; i < l.count; ++i) {
        const DrawObject &o = l.objects[i];
        const GraphicSize s = graphic_size(o.graphic);
        if (s.w <= 0 || s.w > 32 || s.h <= 0 || s.h > 256) continue;
        decode_graphic(o.graphic, o.kind == DrawKind::Flash, block);
        put(out, block, o.x - s.hx, o.y - s.hy, s.w, s.h);
    }
}

void display_debug_boxes(u32 *pixels, int width, int height) {
    // Each object's box (its graphic's size at its position) over the
    // reference picture: they sit on the objects when the list is right.
    const DisplayList *l = shown_frame.list;
    if (!l) return;
    for (int i = 0; i < l->count; ++i) {
        const DrawObject &o = l->objects[i];
        const GraphicSize s = graphic_size(o.graphic);
        const int x0 = o.x - s.hx, y0 = shown_frame.top + o.y - s.hy;
        const u32 colour = o.kind == DrawKind::Normal ? 0x00FF00 : o.kind == DrawKind::Flash ? 0xFFFF00 : 0xFF00FF;
        for (int x = x0; x < x0 + s.w; ++x)
            for (int y : {y0, y0 + s.h - 1})
                if (x >= 0 && x < width && y >= 0 && y < height) pixels[y * width + x] = colour;
        for (int y = y0; y < y0 + s.h; ++y)
            for (int x : {x0, x0 + s.w - 1})
                if (x >= 0 && x < width && y >= 0 && y < height) pixels[y * width + x] = colour;
    }
}

void display_check(const u32 *pixels, int width, int height) {
    static u32 checked;
    const DisplayList *l = shown_frame.list;
    if (!l || ++checked % 50) return;
    static u8 composed[kPlayW * kPlayH];
    display_compose(*l, composed);
    int differ = 0, fx = -1, fy = -1;
    for (int y = 0; y < kPlayH; ++y) {
        const int py = shown_frame.top + y;
        if (py < 0 || py >= height) continue;
        for (int x = 0; x < kPlayW && x < width; ++x)
            if ((shown_frame.palette[composed[y * kPlayW + x]] & 0xFFFFFF) != (pixels[py * width + x] & 0xFFFFFF)) {
                if (!differ++) fx = x, fy = y;
            }
    }
    platform_log("check: update %u, %d of %d playfield pixels differ", l->update, differ, kPlayW * kPlayH);
    if (differ)
        platform_log(" (first at %d,%d: composed %u = %06X, picture %06X)", fx, fy, composed[fy * kPlayW + fx],
                     shown_frame.palette[composed[fy * kPlayW + fx]] & 0xFFFFFF, pixels[(shown_frame.top + fy) * width + fx] & 0xFFFFFF);
    platform_log("\n");
}
