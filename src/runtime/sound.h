// The game's sound effects and speech, played natively (sound.cpp): its two
// voices, each a sample on a left and a right Paula channel, and the CD
// music, resampled and mixed at 48 kHz for the platform's audio output.
#pragma once

#include "runtime/m68k.h"

constexpr int kSoundRate = 48000;

// Start a sample on voice 0 or 1 (replacing what it plays): Paula period,
// left and right volumes 0-64, looping or once.
void sound_play(int voice, const s8 *data, u32 bytes, u32 period, int left, int right, bool loop);

// The CD music: play a track (-1: stop), fade it up or down.
void sound_music(int track);
void sound_music_fade(bool up);

// The next frames of the mix: interleaved 16-bit stereo. Called by the
// platform's audio thread.
void sound_mix(s16 *out, int frames);
