// The display list: probes at the game's drawing routines (config/xenon2.json
// "probe" patches; the routines themselves still run) record what each game
// update draws. An update starts at $346B2, which draws the back layer and
// the walls into the buffer at ($408); the objects follow. The game
// double-buffers, so the last two finished lists are kept, by buffer.
#include "runtime/display.h"

#include "runtime/platform.h"

namespace {

DisplayList lists[3];        // two finished (one per buffer), one being built
DisplayList *building;
u32 updates;
u32 frames;                  // displayed frames (display_frame)
u32 last_finished;           // the frame the last list was finished on
constexpr u32 kStaleFrames = 12;     // no update for this long: the game shows something else

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
    building->update = ++updates;
    building->screen = rl(0x408) & 0xFFFFFF;
    building->back = rl(0x4E42) & 0xFFFFFF;
    building->scroll = rw(0x43C);
    building->map = (rl(0x4E46) + rw(0xCD4)) & 0xFFFFFF;
    building->map_y = s16(rw(0xCDC));
    building->tiles = rl(0x4E4A) & 0xFFFFFF;
    building->walls = !rb(0xD8B);
    building->count = 0;
}

void display_frame() { ++frames; }

void probe_object() { add(DrawKind::Normal); }
void probe_object_flash() { add(DrawKind::Flash); }
void probe_object_masked() { add(DrawKind::Masked); }

const DisplayList *display_list_for(u32 shown) {
    // A finished list (not the one being built) drawn into the buffer the
    // bitplanes show (they may start a few lines into it), while updates
    // still come (the title, shop and table screens draw without them).
    if (frames - last_finished > kStaleFrames) return nullptr;
    for (const DisplayList &l : lists)
        if (&l != building && l.screen && shown >= l.screen && shown - l.screen < 0x2000) return &l;
    return nullptr;
}

void display_debug_boxes(u32 *pixels, int width, int height, int top, u32 shown) {
    // Each object's box (its graphic's size at its position) over the
    // reference picture: they sit on the objects when the list is right.
    const DisplayList *l = display_list_for(shown);
    if (!l) return;
    const int first_row = int(shown - l->screen) / 0xA0;     // $A0 bytes a line (4 planes, interleaved)
    for (int i = 0; i < l->count; ++i) {
        const DrawObject &o = l->objects[i];
        const int x0 = o.x - s16(rw(o.graphic)), w = rw(o.graphic + 4);
        const int y0 = top + o.y - s16(rw(o.graphic + 2)) - first_row, h = rw(o.graphic + 6) + 1;
        const u32 colour = o.kind == DrawKind::Normal ? 0x00FF00 : o.kind == DrawKind::Flash ? 0xFFFF00 : 0xFF00FF;
        for (int x = x0; x < x0 + w; ++x)
            for (int y : {y0, y0 + h - 1})
                if (x >= 0 && x < width && y >= 0 && y < height) pixels[y * width + x] = colour;
        for (int y = y0; y < y0 + h; ++y)
            for (int x : {x0, x0 + w - 1})
                if (x >= 0 && x < width && y >= 0 && y < height) pixels[y * width + x] = colour;
    }
}
