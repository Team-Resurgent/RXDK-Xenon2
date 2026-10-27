// What the runtime needs from the platform (src/platform/xbox).
#pragma once

#include <cstdint>
#include <vector>

void platform_log(const char *format, ...);
[[noreturn]] void platform_fatal(const char *format, ...);

// A file of the game's (D:\data\<name>, case-insensitive); false if missing.
bool platform_read(const char *name, std::vector<uint8_t> &out);

// The joystick in port 2: Amiga direction bits and two fire buttons.
struct Joystick {
    bool up, down, left, right, fire, fire2;
};
Joystick platform_joystick();

// The audio output (runtime/sound.cpp's mix, pulled by its own thread), and
// the lock around the voices it shares with the game.
void platform_audio_start();
void platform_audio_lock();
void platform_audio_unlock();

// The CD music (D:\music), streamed by the platform's own thread: play
// track n (-1: stop), and the next frames of it as 16-bit stereo at 44.1 kHz
// (silence where the stream hasn't caught up; it never waits). The
// platform decides where a track (re)starts.
void platform_music_play(int track);
void platform_music_read(int16_t *out, int frames);

// A finished frame of the reference renderer (0xXXRRGGBB), then wait for
// the frame's 20 ms (PAL). Rows top to bottom are the display window (the
// rest is border): the platform centres them.
void platform_frame(const uint32_t *pixels, int width, int height, int top, int bottom);
