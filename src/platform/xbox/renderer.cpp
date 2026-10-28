// The native playfield: the display list (runtime/display.h) as D3D quads,
// layered as the game draws them: the back layer, the wall tiles, the
// stars, then the objects. The back layer is decoded each frame (the game
// rewrites it as it scrolls) into one of two textures; tiles and objects
// are decoded once into an atlas, keyed by where they are, what they hold
// and the palette, so a texture pack can later replace them one by one.
#include "platform/xbox/renderer.h"

#include "runtime/display.h"
#include "runtime/platform.h"

#include <cstring>
#include <unordered_map>
#include <vector>

namespace {

constexpr int kAtlas = 1024;

IDirect3DDevice8 *g_device;
IDirect3DTexture8 *g_back[2];        // alternate frames: the GPU may still read the last
int g_back_at;
IDirect3DTexture8 *g_atlas;

struct Rect { u16 x, y, w, h; };
std::unordered_map<uint64_t, Rect> g_cache;
int g_shelf_x, g_shelf_y, g_shelf_h;
Rect g_white;                        // a white texel, for flat colours (the stars)

struct Vertex { float x, y, z, rhw; DWORD colour; float u, v; };
constexpr DWORD kFvf = D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1;
std::vector<Vertex> g_quads;         // triangle list, 6 a quad

u32 hash(u32 h, u32 v) { return (h ^ v) * 16777619u; }

u32 palette_hash(const u32 *palette) {
    u32 h = 2166136261u;
    for (int i = 0; i < 16; ++i) h = hash(h, palette[i]);
    return h;
}

u32 memory_hash(u32 at, u32 bytes) {
    u32 h = 2166136261u;
    for (u32 i = 0; i < bytes && at + i < kChipSize; i += 2) h = hash(h, rw(at + i));
    return h;
}

void clear_atlas() {
    g_cache.clear();
    g_shelf_x = g_shelf_y = g_shelf_h = 0;
    D3DLOCKED_RECT locked;
    if (SUCCEEDED(g_atlas->LockRect(0, &locked, nullptr, 0))) {
        std::memset(locked.pBits, 0, size_t(locked.Pitch) * kAtlas);
        // The white texel at the top left.
        *static_cast<DWORD *>(locked.pBits) = 0xFFFFFFFF;
        g_atlas->UnlockRect(0);
    }
    g_white = {0, 0, 1, 1};
    g_shelf_x = 2;
    g_shelf_h = 2;
}

// Room for w x h in the atlas (rows of shelves); emptied when full.
bool allocate(int w, int h, Rect &r) {
    if (w > kAtlas || h > kAtlas) return false;
    if (g_shelf_x + w + 1 > kAtlas) {
        g_shelf_y += g_shelf_h + 1;
        g_shelf_x = 0;
        g_shelf_h = 0;
    }
    if (g_shelf_y + h > kAtlas) {
        clear_atlas();
        return allocate(w, h, r);
    }
    r = {u16(g_shelf_x), u16(g_shelf_y), u16(w), u16(h)};
    g_shelf_x += w + 1;
    if (h > g_shelf_h) g_shelf_h = h;
    return true;
}

// Colour indices (kClear: transparent) into the atlas at r.
void upload(const Rect &r, const u8 *indices, const u32 *palette) {
    D3DLOCKED_RECT locked;
    if (FAILED(g_atlas->LockRect(0, &locked, nullptr, 0))) return;
    for (int y = 0; y < r.h; ++y) {
        DWORD *row = reinterpret_cast<DWORD *>(static_cast<u8 *>(locked.pBits) + size_t(r.y + y) * locked.Pitch) + r.x;
        for (int x = 0; x < r.w; ++x) {
            const u8 c = indices[y * r.w + x];
            row[x] = c == kClear ? 0 : 0xFF000000u | (palette[c] & 0xFFFFFF);
        }
    }
    g_atlas->UnlockRect(0);
}

// A quad of the atlas rect r at playfield (x, y), clipped to the playfield,
// on screen through the mapping.
struct Mapping { float sx, y0, sy; int top; };

void quad(const Mapping &m, const Rect &r, int x, int y, int w, int h, DWORD colour) {
    int u0 = r.x, v0 = r.y;
    if (x < 0) { u0 -= x; w += x; x = 0; }
    if (y < 0) { v0 -= y; h += y; y = 0; }
    if (x + w > kPlayW) w = kPlayW - x;
    if (y + h > kPlayH) h = kPlayH - y;
    if (w <= 0 || h <= 0) return;
    const float X0 = x * m.sx - .5f, X1 = (x + w) * m.sx - .5f;
    const float Y0 = m.y0 + (m.top + y) * m.sy, Y1 = m.y0 + (m.top + y + h) * m.sy;
    const float U0 = float(u0), U1 = float(u0 + (r.w == 1 ? 1 : w)), V0 = float(v0), V1 = float(v0 + (r.h == 1 ? 1 : h));
    const Vertex a = {X0, Y0, 0, 1, colour, U0, V0}, b = {X1, Y0, 0, 1, colour, U1, V0};
    const Vertex c = {X0, Y1, 0, 1, colour, U0, V1}, d = {X1, Y1, 0, 1, colour, U1, V1};
    g_quads.insert(g_quads.end(), {a, b, c, b, d, c});
}

const Rect *cached(uint64_t key, int w, int h, const u32 *palette, void (*decode)(u32, u32, u8 *), u32 p0, u32 p1) {
    auto it = g_cache.find(key);
    if (it != g_cache.end()) return &it->second;
    static u8 indices[64 * 256];
    if (w * h > int(sizeof(indices))) return nullptr;
    Rect r;
    if (!allocate(w, h, r)) return nullptr;
    decode(p0, p1, indices);
    upload(r, indices, palette);
    return &(g_cache[key] = r);
}

} // namespace

void renderer_init(IDirect3DDevice8 *device) {
    g_device = device;
    for (IDirect3DTexture8 *&t : g_back)
        if (FAILED(device->CreateTexture(512, 256, 1, 0, D3DFMT_LIN_X8R8G8B8, 0, &t)))
            platform_fatal("no memory for the back layer texture");
    if (FAILED(device->CreateTexture(kAtlas, kAtlas, 1, 0, D3DFMT_LIN_A8R8G8B8, 0, &g_atlas)))
        platform_fatal("no memory for the texture atlas");
    clear_atlas();
    g_quads.reserve(6 * 1024);
}

bool renderer_draw_playfield(float sx, float y0, float sy) {
    const Shown &shown = display_shown();
    if (!shown.list) return false;
    const DisplayList &l = *shown.list;
    const u32 *palette = shown.palette;
    const Mapping m = {sx, y0, sy, shown.top};
    const u32 pal = palette_hash(palette);

    // The back layer: decoded whole into this frame's texture.
    static u8 back[kPlayW * kPlayH];
    decode_back(l, back);
    IDirect3DTexture8 *tex = g_back[g_back_at ^= 1];
    D3DLOCKED_RECT locked;
    if (SUCCEEDED(tex->LockRect(0, &locked, nullptr, 0))) {
        for (int y = 0; y < kPlayH; ++y) {
            DWORD *row = reinterpret_cast<DWORD *>(static_cast<u8 *>(locked.pBits) + size_t(y) * locked.Pitch);
            for (int x = 0; x < kPlayW; ++x) row[x] = palette[back[y * kPlayW + x]] & 0xFFFFFF;
        }
        tex->UnlockRect(0);
    }

    g_device->SetVertexShader(kFvf);
    g_device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    g_device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    g_device->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    g_device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    g_device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    g_device->SetTextureStageState(0, D3DTSS_MINFILTER, D3DTEXF_POINT);
    g_device->SetTextureStageState(0, D3DTSS_MAGFILTER, D3DTEXF_POINT);

    g_quads.clear();
    quad(m, {0, 0, kPlayW, kPlayH}, 0, 0, kPlayW, kPlayH, 0xFFFFFFFF);
    g_device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    g_device->SetTexture(0, tex);
    g_device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, UINT(g_quads.size() / 3), g_quads.data(), sizeof(Vertex));

    // The rest from the atlas, in the game's order.
    g_quads.clear();
    static u8 tile[kTile * kTile];
    if (l.walls)
        for (int r = 0; r < kMapRows; ++r) {
            const int y = r * kTile - l.map_y;
            if (y <= -kTile || y >= kPlayH) continue;
            for (int c = 0; c < kMapCols; ++c) {
                const u16 cell = l.map_cells[r * kMapCols + c];
                if (!cell) continue;
                const u32 at = l.tiles + u32(cell & 0x7FFF) * 16;
                const uint64_t key = uint64_t(memory_hash(at, (cell & 0x8000) ? 160 : 128) ^ cell) << 32 | pal;
                const Rect *rect = cached(key, kTile, kTile, palette,
                                          [](u32 tiles, u32 cell, u8 *out) { decode_tile(tiles, u16(cell), out); },
                                          l.tiles, cell);
                if (rect) quad(m, *rect, c * kTile, y, kTile, kTile, 0xFFFFFFFF);
            }
        }
    if (l.has_stars)
        for (int i = 0; i < DisplayList::kStars; ++i) {
            int x, y;
            if (!star_position(l, i, x, y) || back[y * kPlayW + x]) continue;
            // Not on a wall tile's pixel either.
            bool covered = false;
            if (l.walls) {
                const int r = (y + l.map_y) / kTile;
                if (r >= 0 && r < kMapRows) {
                    const u16 cell = l.map_cells[r * kMapCols + x / kTile];
                    if (cell) {
                        decode_tile(l.tiles, cell, tile);
                        covered = tile[((y + l.map_y) % kTile) * kTile + x % kTile] != kClear;
                    }
                }
            }
            if (!covered) quad(m, g_white, x, y, 1, 1, 0xFF000000u | (palette[4 + i / 12] & 0xFFFFFF));
        }
    for (int i = 0; i < l.count; ++i) {
        const DrawObject &o = l.objects[i];
        const GraphicSize s = graphic_size(o.graphic);
        if (s.w <= 0 || s.w > 32 || s.h <= 0 || s.h > 256) continue;
        const bool flash = o.kind == DrawKind::Flash;
        const uint64_t key = uint64_t(memory_hash(o.graphic, 8 + u32(s.h) * (s.w > 16 ? 20 : 10)) ^ (flash ? 1u : 0u)) << 32 | pal;
        const Rect *rect = cached(key, s.w, s.h, palette,
                                  [](u32 graphic, u32 flash, u8 *out) { decode_graphic(graphic, flash, out); },
                                  o.graphic, flash);
        if (rect) quad(m, *rect, o.x - s.hx, o.y - s.hy, s.w, s.h, 0xFFFFFFFF);
    }
    static u32 drawn;
    if (++drawn % 500 == 0)
        platform_log("renderer: %u playfields drawn; this one %u quads, %u in the atlas\n", drawn,
                     unsigned(g_quads.size() / 6 + 1), unsigned(g_cache.size()));
    if (!g_quads.empty()) {
        g_device->SetRenderState(D3DRS_ALPHATESTENABLE, TRUE);
        g_device->SetRenderState(D3DRS_ALPHAREF, 0x80);
        g_device->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATEREQUAL);
        g_device->SetTexture(0, g_atlas);
        g_device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, UINT(g_quads.size() / 3), g_quads.data(), sizeof(Vertex));
        g_device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    }
    g_device->SetTexture(0, nullptr);
    return true;
}
