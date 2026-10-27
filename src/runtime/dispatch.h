// Address -> recompiled function, per code set (build/m68k/m68k_tables.cpp).
#pragma once

#include "runtime/m68k.h"

struct DispatchEntry {
    u32 address;
    m68k_fn fn;
    u32 at;          // 0: the function's entry; else the resume address
};

struct CodeSet {
    const char *name;            // "main" or the overlay's file name
    const DispatchEntry *table;
    u32 count;
    const u8 *map;               // code map bits from map_start
    u32 map_start, map_bytes;
};

extern const CodeSet kCodeSets[];
extern const int kCodeSetCount;
extern const u32 kOverlayStart, kOverlayEnd;
