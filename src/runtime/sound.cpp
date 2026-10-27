// The game's sound effects and speech, played natively. hook_sound_voice
// (hooks.cpp) starts a sample on one of the game's two voices; here they
// are resampled (linear interpolation, not Paula's held samples) and mixed
// at 48 kHz, as the Amiga's left (channels 0 and 3) and right (1 and 2).
// The CD hooks play the music under them, streamed by the platform.
#include "runtime/sound.h"

#include "runtime/platform.h"

#include <cstring>

namespace {

constexpr double kPaulaClock = 3546895.0;     // PAL: a sample every `period` of these

struct Voice {
    s8 data[0x20000];    // a copy of the sample (the game reloads its banks, and its speech buffer)
    u32 bytes;           // 0: silent
    u32 step;            // source bytes per output frame, 16.16
    uint64_t pos;        // 16.16
    int left, right;     // 0-64
    bool loop;
};
Voice voices[2];

// The CD music: CD audio (44.1 kHz) streamed in chunks and resampled.
constexpr u32 kMusicStep = u32(44100.0 / kSoundRate * 65536.0);
constexpr float kMusicGain = 0.6f;            // under the sound effects
constexpr float kFadeStep = 1.0f / kSoundRate; // a whole fade in a second
struct Music {
    bool on;                                  // a track playing (or fading out to stop)
    bool stopping;                            // fading out: stop at silence
    float volume = 1, target = 1;
    s16 chunk[1024 * 2];
    int at, have;
    u32 frac;                                 // between prev and next, 16.16
    int prev[2], next[2];
} music;

void music_next() {
    if (music.at == music.have) {
        platform_music_read(music.chunk, 1024);
        music.at = 0;
        music.have = 1024;
    }
    music.prev[0] = music.next[0];
    music.prev[1] = music.next[1];
    music.next[0] = music.chunk[2 * music.at];
    music.next[1] = music.chunk[2 * music.at + 1];
    ++music.at;
}

} // namespace

void sound_music(int track) {
    platform_audio_lock();
    if (track >= 0) {
        // A new track (or the same again: the platform says where it starts).
        platform_music_play(track);
        music.on = true;
        music.stopping = false;
        music.target = 1;
        music.at = music.have = 0;
        music.frac = 0;
        music.next[0] = music.next[1] = 0;
        music_next();
    } else if (music.on) {
        // Stopped (as for loading, where the CDTV's drive can't play): faded
        // out rather than cut.
        music.stopping = true;
        music.target = 0;
    }
    platform_audio_unlock();
}

void sound_music_fade(bool up) {
    platform_audio_lock();
    music.target = up ? 1.0f : 0.0f;
    platform_audio_unlock();
}

void sound_play(int voice, const s8 *data, u32 bytes, u32 period, int left, int right, bool loop) {
    if (bytes > sizeof(Voice::data)) bytes = sizeof(Voice::data);
    if (period < 124) period = 124;
    platform_audio_lock();
    Voice &v = voices[voice & 1];
    std::memcpy(v.data, data, bytes);
    v.bytes = bytes;
    v.step = u32(kPaulaClock / period / kSoundRate * 65536.0);
    v.pos = 0;
    v.left = left;
    v.right = right;
    v.loop = loop;
    platform_audio_unlock();
}

void sound_mix(s16 *out, int frames) {
    platform_audio_lock();
    for (int i = 0; i < frames; ++i) {
        int left = 0, right = 0;
        for (Voice &v : voices) {
            if (!v.bytes) continue;
            const u32 at = u32(v.pos >> 16), frac = u32(v.pos & 0xFFFF);
            const int a = v.data[at];
            const int b = at + 1 < v.bytes ? v.data[at + 1] : v.loop ? v.data[0] : 0;
            const int s = a + ((b - a) * int(frac) >> 16);
            left += s * v.left;
            right += s * v.right;
            v.pos += v.step;
            if (v.pos >> 16 >= v.bytes) {
                if (v.loop) v.pos -= uint64_t(v.bytes) << 16;
                else v.bytes = 0;
            }
        }
        // Two channels a side at full volume are full scale.
        left *= 2;
        right *= 2;
        if (music.on) {
            music.frac += kMusicStep;
            while (music.frac >= 0x10000) {
                music.frac -= 0x10000;
                music_next();
            }
            const int f = int(music.frac >> 1);    // 15 bits: a 16-bit difference times it fits
            const int l = music.prev[0] + ((music.next[0] - music.prev[0]) * f >> 15);
            const int r = music.prev[1] + ((music.next[1] - music.prev[1]) * f >> 15);
            if (music.volume < music.target) {
                music.volume += kFadeStep;
                if (music.volume > music.target) music.volume = music.target;
            } else if (music.volume > music.target) {
                music.volume -= kFadeStep;
                if (music.volume < music.target) music.volume = music.target;
            }
            left += int(l * music.volume * kMusicGain);
            right += int(r * music.volume * kMusicGain);
            if (music.stopping && music.volume == 0) {
                music.on = false;
                platform_music_play(-1);
            }
        }
        out[2 * i] = s16(left < -32768 ? -32768 : left > 32767 ? 32767 : left);
        out[2 * i + 1] = s16(right < -32768 ? -32768 : right > 32767 ? 32767 : right);
    }
    platform_audio_unlock();
}
