// The 68000 runtime (cpu.cpp).
#pragma once

#include "runtime/m68k.h"

// A native level 3 handler at an unused vector table address, installed
// before the game starts as the WHDLoad slave installs its own.
constexpr u32 kNativeVblank = 0x0000F0;

void cpu_reset(u32 pc, u32 stack);
void cpu_run();                          // never returns
void cpu_set_overlay(const char *name);  // a file was loaded into the overlay slot
extern u32 g_last_resume;               // where the code last let events in (a busy wait shows here)
void cpu_dump();                        // recent dispatches and the stack, to the log
void cpu_watch(u32 address);            // log writes to this byte (debugging)
