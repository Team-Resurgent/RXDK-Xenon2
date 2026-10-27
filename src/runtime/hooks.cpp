// Native code in place of the game's OS calls (config/xenon2.json patches,
// after the WHDLoad slave's): loading files from the CD, the CD audio, and
// starting the sound effects and speech.
#include "runtime/cpu.h"
#include "runtime/platform.h"
#include "runtime/sound.h"

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
    sound_music(int(track));
    platform_log("hooks: CD play track %u\n", track);
}

void hook_cd_poll() {}

void hook_cd_stop() {
    if (s32(rl(kCdTrack)) >= 0) platform_log("hooks: CD stop\n");
    wl(kCdTrack, 0xFFFFFFFF);
    sound_music(-1);
}

void hook_cd_fade_in() {
    sound_music_fade(true);
    platform_log("hooks: CD volume up\n");
}

void hook_cd_fade_out() {
    sound_music_fade(false);
    platform_log("hooks: CD volume down\n");
}

void hook_sound_voice() {
    // The game's start of a sound ($3351E, from its player at $334B6): a0 =
    // the voice's request ({w sample, w x 0-$140, b go}), a3 = the sound
    // bank (12-byte entries: offset, length in words, volume, period, 1 =
    // loops), d0 = the voice's DMA bits (3: channels 0 and 1, $C: 3 and 2).
    // The sound's screen position shares its volume between the left and
    // right channel, as the game works it out.
    const u32 request = R.a[0];
    const u16 number = rw(request);
    s16 x = s16(rw(request + 2));
    wb(request + 4, 0);
    ww(request, 0xFFFF);
    const u32 entry = R.a[3] + u32(number) * 12;
    if (rl(entry) == 0xFFFFFFFF) return;
    if (x < 0) x = 0;
    if (x > 0x140) x = 0x140;
    const u16 volume = rw(entry + 6);
    u16 right = u16(((u32(x) * volume & 0xFFFF) * 0xCC + 0x8000) >> 16);
    u16 left = u16(volume - right);
    if (right >= 0x40) right = 0x40;
    if (left >= 0x40) left = 0x40;
    const u32 data = (R.a[3] + rl(entry)) & 0xFFFFFF;
    u32 bytes = (rw(entry + 4) ? rw(entry + 4) : 0x10000) * 2;
    if (data >= kChipSize) return;
    if (data + bytes > kChipSize) bytes = kChipSize - data;
    const int voice = (R.d[0] & 0xFFFF) == 3 ? 0 : 1;
    static int logged;
    if (logged++ < 40)
        platform_log("hooks: sound %u on voice %d (%u bytes, period %u, volume %u/%u%s)\n", number, voice, bytes,
                     rw(entry + 8), left, right, rw(entry + 10) == 1 ? ", loops" : "");
    sound_play(voice, reinterpret_cast<const s8 *>(MEM + data), bytes, rw(entry + 8), left, right, rw(entry + 10) == 1);
}
