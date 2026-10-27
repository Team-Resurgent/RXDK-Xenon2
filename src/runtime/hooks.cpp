// Native code in place of the game's OS calls (config/xenon2.json patches,
// after the WHDLoad slave's): loading files from the CD, and the CD audio.
#include "runtime/cpu.h"
#include "runtime/platform.h"

#include <cctype>
#include <cstring>
#include <vector>

namespace {

// The overlays the game loads into its code slot (config/xenon2.json).
const char *const kOverlays[] = {"Intro", "Piccode", "Level1", "Level2", "Level3", "Level4", "Level5"};
constexpr u32 kOverlaySlot = 0x4E26;
constexpr u32 kCdTrack = 0x3A8F2;      // the game's "track playing" (-1: none)

const char *canonical_overlay(const char *name) {
    for (const char *o : kOverlays) {
        size_t i = 0;
        while (o[i] && name[i] && std::tolower(u8(o[i])) == std::tolower(u8(name[i]))) ++i;
        if (!o[i] && !name[i]) return o;
    }
    return nullptr;
}

} // namespace

void hook_load_file(u32 dest) {
    char name[64] = {};
    const u32 at = R.d[1] + 4;       // past "CD0:"
    for (int i = 0; i < 63; ++i) {
        name[i] = char(rb(at + i));
        if (!name[i]) break;
    }
    std::vector<uint8_t> data;
    if (!platform_read(name, data)) platform_fatal("hooks: file %s missing", name);
    dest &= 0xFFFFFF;
    if (dest + data.size() > kChipSize) platform_fatal("hooks: %s does not fit at $%06X", name, dest);
    std::memcpy(MEM + dest, data.data(), data.size());
    if (dest == kOverlaySlot) {
        const char *overlay = canonical_overlay(name);
        cpu_set_overlay(overlay ? overlay : "");
    }
    platform_log("hooks: loaded %s (%u bytes) at $%06X\n", name, unsigned(data.size()), dest);
    R.d[0] = u32(data.size());
    R.d[1] = 0;
}

void hook_cd_play() {
    const u32 track = R.d[0] & 0xFFFF;
    wl(kCdTrack, track);
    platform_log("hooks: CD play track %u\n", track);
}

void hook_cd_poll() {}

void hook_cd_stop() {
    if (s32(rl(kCdTrack)) >= 0) platform_log("hooks: CD stop\n");
    wl(kCdTrack, 0xFFFFFFFF);
}

void hook_cd_fade_in() { platform_log("hooks: CD volume up\n"); }

void hook_cd_fade_out() { platform_log("hooks: CD volume down\n"); }
