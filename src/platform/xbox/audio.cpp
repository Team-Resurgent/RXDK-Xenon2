// The speakers: the runtime's mix (runtime/sound.cpp) through a looping
// DirectSound buffer (48 kHz stereo), which its own thread keeps a few
// blocks ahead of the play cursor; and the CD music streamed from the disc.
#include <xtl.h>
#include <dsound.h>

#include "runtime/platform.h"
#include "runtime/sound.h"

#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_NO_PUSHDATA_API
#define STB_VORBIS_HEADER_ONLY
#include "stb_vorbis/stb_vorbis.c"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

constexpr DWORD kBlockFrames = 256;
constexpr DWORD kBlockBytes = kBlockFrames * 4;
constexpr DWORD kBlocks = 32;                 // 170 ms of buffer
constexpr DWORD kAheadBlocks = 6;             // 32 ms queued
constexpr DWORD kBufferBytes = kBlocks * kBlockBytes;

CRITICAL_SECTION g_lock;
LPDIRECTSOUND g_ds;
LPDIRECTSOUNDBUFFER g_buffer;
DWORD g_write;                                // byte offset of the next block
DWORD g_blocks, g_resyncs;
int g_peak;                                   // the loudest sample since the last report

// Bytes written ahead of the play cursor.
DWORD queued() {
    DWORD play = 0, write = 0;
    g_buffer->GetCurrentPosition(&play, &write);
    return (g_write + kBufferBytes - play) % kBufferBytes;
}

void write_block() {
    static s16 block[kBlockFrames * 2];
    sound_mix(block, kBlockFrames);
    for (s16 v : block) g_peak = v > g_peak ? v : -v > g_peak ? -v : g_peak;
    // Every 10 s: the output is playing and how loud (0: silence).
    if (++g_blocks % (10 * kSoundRate / kBlockFrames) == 0) {
        platform_log("audio: %u blocks, peak %d, %u resyncs\n", g_blocks, g_peak, g_resyncs);
        g_peak = 0;
    }
    void *p1, *p2;
    DWORD n1, n2;
    if (SUCCEEDED(g_buffer->Lock(g_write, kBlockBytes, &p1, &n1, &p2, &n2, 0))) {
        std::memcpy(p1, block, n1);
        if (p2) std::memcpy(p2, reinterpret_cast<u8 *>(block) + n1, n2);
        g_buffer->Unlock(p1, n1, p2, n2);
    }
    g_write = (g_write + kBlockBytes) % kBufferBytes;
    // Half a buffer ahead is a block from the last lap: silenced, so a play
    // cursor that overtakes the blocks written plays nothing rather than old
    // sound again.
    const DWORD stale = (g_write + kBufferBytes / 2) % kBufferBytes;
    if (SUCCEEDED(g_buffer->Lock(stale, kBlockBytes, &p1, &n1, &p2, &n2, 0))) {
        std::memset(p1, 0, n1);
        if (p2) std::memset(p2, 0, n2);
        g_buffer->Unlock(p1, n1, p2, n2);
    }
}

DWORD WINAPI audio_thread(void *) {
    for (;;) {
        DirectSoundDoWork();
        const DWORD ahead = queued();
        if (ahead > (kBlocks - 2) * kBlockBytes) {
            // Fallen behind (the play cursor overtook the blocks written):
            // start again just ahead of it.
            DWORD play = 0, write = 0;
            g_buffer->GetCurrentPosition(&play, &write);
            g_write = (play / kBlockBytes + 2) % kBlocks * kBlockBytes;
            ++g_resyncs;
        } else if (ahead >= kAheadBlocks * kBlockBytes) {
            Sleep(2);
            continue;
        }
        write_block();
    }
}

} // namespace

namespace {

// The CD music: D:\music\trackNN.ogg from a rip of the CDTV disc, from its
// start as the CD plays it; or else btb.ogg (the Xenon2Audio add-on's Bomb
// the Bass), one piece for every track, which goes on from where it was
// rather than starting again at each scene. Its own thread loads a track
// and decodes it ahead into a ring, so the mix never waits for the disc
// (the game loads from it too) or the decoder.
constexpr DWORD kRingFrames = 1 << 17;        // 3 s of CD audio
constexpr int kDecodeFrames = 1024;
CRITICAL_SECTION g_music_lock;
u32 g_ring[kRingFrames];                      // frames: left | right << 16
DWORD g_ring_read, g_ring_write;              // frame counts (the ring index is the count mod its size)
int g_requested = -1;                         // the game's track (-1: stopped)
int g_streaming = -1;                         // the streamer's
std::vector<u8> g_ogg;                        // the track's file
stb_vorbis *g_vorbis;
bool g_one_piece;                             // g_vorbis is btb.ogg

void flush_ring() {
    EnterCriticalSection(&g_music_lock);
    g_ring_read = g_ring_write;
    LeaveCriticalSection(&g_music_lock);
}

void close_music() {
    if (g_vorbis) stb_vorbis_close(g_vorbis);
    g_vorbis = nullptr;
    g_one_piece = false;
    flush_ring();
}

bool load_music(const char *path) {
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    close_music();
    g_ogg.resize(GetFileSize(f, nullptr));
    DWORD at = 0, got = 1;
    while (at < g_ogg.size() && got)     // a piece at a time: the game's loads get in between
        if (!ReadFile(f, g_ogg.data() + at, std::min<DWORD>(DWORD(g_ogg.size()) - at, 256 << 10), &got, nullptr)) break;
        else at += got;
    CloseHandle(f);
    int error = 0;
    if (at == g_ogg.size()) g_vorbis = stb_vorbis_open_memory(g_ogg.data(), int(g_ogg.size()), &error, nullptr);
    if (!g_vorbis) platform_log("audio: can't play %s (error %d)\n", path, error);
    return g_vorbis != nullptr;
}

// The game asked for another track (or to stop).
void switch_track(int track) {
    g_streaming = track;
    if (track < 0) {
        if (!g_one_piece) close_music();      // the one piece only pauses
        return;
    }
    char path[64];
    std::snprintf(path, sizeof(path), "D:\\music\\track%02d.ogg", track);
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) {
        if (g_one_piece) return;
        std::snprintf(path, sizeof(path), "D:\\music\\btb.ogg");
        g_one_piece = load_music(path);
    } else {
        load_music(path);
    }
    if (g_vorbis) platform_log("audio: CD track %d from %s\n", track, path);
    else platform_log("audio: no music for CD track %d\n", track);
}

DWORD WINAPI music_thread(void *) {
    static s16 frames[kDecodeFrames * 2];
    for (;;) {
        EnterCriticalSection(&g_music_lock);
        const int requested = g_requested;
        const DWORD space = kRingFrames - (g_ring_write - g_ring_read);
        LeaveCriticalSection(&g_music_lock);
        if (requested != g_streaming) switch_track(requested);
        if (g_streaming < 0 || !g_vorbis || space < DWORD(kDecodeFrames)) {
            Sleep(10);
            continue;
        }
        // The next frames, from the start again at the end.
        const int got = stb_vorbis_get_samples_short_interleaved(g_vorbis, 2, frames, kDecodeFrames * 2);
        if (got <= 0) {
            stb_vorbis_seek_start(g_vorbis);
            continue;
        }
        EnterCriticalSection(&g_music_lock);
        if (g_requested == g_streaming)        // not a track the game has left meanwhile
            for (int i = 0; i < got; ++i)
                g_ring[g_ring_write++ % kRingFrames] = u16(frames[2 * i]) | u32(u16(frames[2 * i + 1])) << 16;
        LeaveCriticalSection(&g_music_lock);
    }
}

} // namespace

void platform_music_play(int track) {
    EnterCriticalSection(&g_music_lock);
    g_requested = track;
    LeaveCriticalSection(&g_music_lock);
}

void platform_music_read(int16_t *out, int frames) {
    EnterCriticalSection(&g_music_lock);
    int i = 0;
    for (; i < frames && g_ring_read != g_ring_write; ++i) {
        const u32 f = g_ring[g_ring_read++ % kRingFrames];
        out[2 * i] = int16_t(f & 0xFFFF);
        out[2 * i + 1] = int16_t(f >> 16);
    }
    LeaveCriticalSection(&g_music_lock);
    std::memset(out + 2 * i, 0, size_t(frames - i) * 4);     // not read yet: silence, not a wait
}

void platform_audio_lock() { EnterCriticalSection(&g_lock); }
void platform_audio_unlock() { LeaveCriticalSection(&g_lock); }

void platform_audio_start() {
    InitializeCriticalSection(&g_lock);
    InitializeCriticalSection(&g_music_lock);
    // Decoding a track's first seconds is a burst of work: below the game,
    // and far below the speakers.
    SetThreadPriority(CreateThread(nullptr, 0, music_thread, nullptr, 0, nullptr), THREAD_PRIORITY_BELOW_NORMAL);
    if (FAILED(DirectSoundCreate(nullptr, &g_ds, nullptr))) {
        platform_log("audio: no DirectSound: silent\n");
        return;
    }
    WAVEFORMATEX format = {};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = 2;
    format.nSamplesPerSec = kSoundRate;
    format.wBitsPerSample = 16;
    format.nBlockAlign = 4;
    format.nAvgBytesPerSec = kSoundRate * 4;
    DSBUFFERDESC desc = {};
    desc.dwSize = sizeof(desc);
    desc.dwBufferBytes = kBufferBytes;
    desc.lpwfxFormat = &format;
    if (FAILED(g_ds->CreateSoundBuffer(&desc, &g_buffer, nullptr))) {
        platform_log("audio: no DirectSound buffer: silent\n");
        return;
    }
    void *p;
    DWORD n;
    if (SUCCEEDED(g_buffer->Lock(0, kBufferBytes, &p, &n, nullptr, nullptr, 0))) {
        std::memset(p, 0, n);
        g_buffer->Unlock(p, n, nullptr, 0);
    }
    g_buffer->Play(0, 0, DSBPLAY_LOOPING);
    SetThreadPriority(CreateThread(nullptr, 0, audio_thread, nullptr, 0, nullptr), THREAD_PRIORITY_TIME_CRITICAL);
    platform_log("audio: DirectSound output, 48 kHz stereo\n");
}
