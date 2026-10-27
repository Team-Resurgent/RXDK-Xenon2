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

// A finished frame of the reference renderer (0xXXRRGGBB), then wait for
// the frame's 20 ms (PAL).
void platform_frame(const uint32_t *pixels, int width, int height);
