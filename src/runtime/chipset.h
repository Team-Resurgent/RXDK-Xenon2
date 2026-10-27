// The Amiga chipset the game's code talks to (chipset.cpp): custom
// registers, interrupts, the CIAs, and the reference picture (bitplanes,
// sprites, copper, blitter) the native HD renderer is checked against.
#pragma once

#include "runtime/m68k.h"

constexpr int kLineCycles = 454;          // 68000 cycles per PAL line (227 colour clocks)
constexpr int kFrameLines = 313;          // PAL, long frames
constexpr int kScreenW = 320, kScreenH = 256;
constexpr int kScreenX = 0x81, kScreenY = 0x2C;   // beam position of the picture's top left

void chipset_reset();
void chipset_advance();                   // the next line (and the frame at its end)
int chipset_irq_level();                  // the highest pending enabled interrupt (0: none)
void chipset_ack_level(int level);        // clear the requests of a level (no handler installed)
u16 chipset_reg(u32 offset);              // a custom register as last written
