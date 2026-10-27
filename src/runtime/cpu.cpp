// The 68000 runtime around the recompiled code: dispatch by address, the
// status register, exceptions and interrupts, the self-modifying code check.
#include "runtime/cpu.h"

#include "runtime/chipset.h"
#include "runtime/dispatch.h"
#include "runtime/platform.h"

#include <cstring>

M68K R;
alignas(16) u8 MEM[kChipSize + 16];
u8 CODEMAP[kChipSize / 8];

namespace {

const CodeSet *g_overlay = nullptr;     // the overlay loaded in the slot, if any
bool g_overlay_mapped = false;          // its code map applied (after the game relocated it)
int g_reported = 0;

const DispatchEntry *find(const CodeSet &set, u32 address) {
    u32 lo = 0, hi = set.count;
    while (lo < hi) {
        u32 mid = (lo + hi) / 2;
        if (set.table[mid].address < address) lo = mid + 1;
        else hi = mid;
    }
    return lo < set.count && set.table[lo].address == address ? &set.table[lo] : nullptr;
}

void apply_map(const CodeSet &set) {
    std::memcpy(CODEMAP + set.map_start / 8, set.map, set.map_bytes);
}

void report(const char *what, u32 address) {
    if (g_reported++ < 200) platform_log("68k: %s $%06X (overlay %s)\n", what, address, g_overlay ? g_overlay->name : "-");
}

} // namespace

void cpu_reset(u32 pc, u32 stack) {
    std::memset(&R, 0, sizeof(R));
    R.a[7] = stack;
    R.ssp = stack;
    R.usp = stack - 0x400;
    R.sr_high = 0x2000;        // supervisor, interrupts enabled (the WHDLoad slave's state)
    R.pc = pc;
    std::memset(CODEMAP, 0, sizeof(CODEMAP));
    apply_map(kCodeSets[0]);
}

void cpu_set_overlay(const char *name) {
    // The slot is now this file's (or not code at all).
    std::memset(CODEMAP + kOverlayStart / 8, 0, (kOverlayEnd - kOverlayStart) / 8);
    g_overlay = nullptr;
    g_overlay_mapped = false;
    for (int i = 1; i < kCodeSetCount; ++i)
        if (!strcmp(kCodeSets[i].name, name)) g_overlay = &kCodeSets[i];
    if (g_overlay) platform_log("68k: overlay %s\n", name);
}

void cpu_run() {
    for (;;) rt_call(R.pc);
}

u32 g_last_resume;     // where the code last let events in: a busy wait shows here
u32 g_calls[32];      // the last dynamic dispatches, for crash reports
u32 g_call_at;

void cpu_dump() {
    platform_log("68k: last dispatches:");
    for (int i = 0; i < 32; ++i) platform_log(" %06X", g_calls[(g_call_at + i) & 31]);
    platform_log("\n68k: stack at %06X:", R.a[7]);
    for (int i = 0; i < 24; ++i) platform_log(" %08X", rl(R.a[7] + 4 * i));
    platform_log("\n");
}

void rt_call(u32 address) {
    address &= 0xFFFFFF;
    g_calls[g_call_at++ & 31] = address;
    if (address == kNativeVblank) {
        // The WHDLoad slave's level 3 handler (the game chains to the vector
        // it found): acknowledge the vertical blank and return.
        chip_write16(0xDFF09C, 0x0020);
        const u32 sr = pop16();
        R.pc = pop32();
        rt_set_sr(sr);
        return;
    }
    const CodeSet *set = address >= kOverlayStart && address < kOverlayEnd ? g_overlay : &kCodeSets[0];
    if (set && set == g_overlay && !g_overlay_mapped) {
        // The game relocates the file after loading it (not self-modifying
        // code): watch its code from the first call into it.
        apply_map(*set);
        g_overlay_mapped = true;
    }
    const DispatchEntry *e = set ? find(*set, address) : nullptr;
    if (!e) {
        report("no recompiled code at", address);
        platform_fatal("68k: no recompiled code at $%06X (add it to config/xenon2.json entries)", address);
        return;
    }
    e->fn(e->at);
}

u32 rt_get_sr() {
    return R.sr_high | R.x << 4 | R.n << 3 | R.z << 2 | R.v << 1 | R.c;
}

void rt_set_ccr(u32 ccr) {
    R.x = ccr >> 4 & 1;
    R.n = ccr >> 3 & 1;
    R.z = ccr >> 2 & 1;
    R.v = ccr >> 1 & 1;
    R.c = ccr & 1;
}

void rt_set_sr(u32 sr) {
    const bool was = R.sr_high & 0x2000, now = sr & 0x2000;
    if (was && !now) {
        R.ssp = R.a[7];
        R.a[7] = R.usp;
    } else if (!was && now) {
        R.usp = R.a[7];
        R.a[7] = R.ssp;
    }
    R.sr_high = u16(sr & 0xA700);
    rt_set_ccr(sr);
}

// Enter an exception handler as the 68000 does and run it to its rte.
// Returns 1 when it returned somewhere other than resume.
static int take(int vector, u32 resume, int level) {
    const u32 sr = rt_get_sr();
    rt_set_sr((sr | 0x2000) & ~0x8000u);
    if (level >= 0) R.sr_high = u16((R.sr_high & ~0x0700) | level << 8);
    push32(resume);
    push16(sr);
    const u32 handler = rl(u32(vector) * 4);
    rt_call(handler);
    // A handler that ends by "returning" into another (move.l old,-(a7); rts:
    // chaining to the vector it replaced) has left R.pc at that one.
    for (int chain = 0; chain < 8 && R.pc != resume; ++chain) rt_call(R.pc);
    if (R.pc != resume) {
        report("exception returned elsewhere", R.pc);
        return 1;
    }
    return 0;
}

int rt_exception(int vector, u32 resume) {
    if (vector != 32 + 0) report("exception", u32(vector));
    return take(vector, resume, -1);
}


int rt_events(u32 resume) {
    g_last_resume = resume;
    // Chipset time first (lines, the frame, CIA timers), then interrupts.
    while (R.cyc >= R.event) chipset_advance();
    for (int guard = 0; guard < 8; ++guard) {
        const int level = chipset_irq_level();
        if (level <= int(R.sr_high >> 8 & 7)) return 0;
        if (!rl(u32(24 + level) * 4)) {
            // No handler yet (the slave's own acknowledged and returned).
            chipset_ack_level(level);
            continue;
        }
        if (take(24 + level, resume, level)) return 1;
    }
    return 0;
}

int rt_stop(u32 resume) {
    // Sleep to the next event; then any interrupt.
    if (R.cyc < R.event) R.cyc = R.event;
    return rt_events(resume);
}

u32 g_watch[8];
int g_watches;

void cpu_watch(u32 a) {
    g_watch[g_watches++ & 7] = a;
    CODEMAP[a >> 3] |= u8(1u << (a & 7));
}

void rt_smc(u32 a) {
    for (int i = 0; i < g_watches; ++i)
        if (g_watch[i] == a) {
            platform_log("68k: watch $%06X <- %02X (near $%06X; last dispatch $%06X) d0 %08X a0 %08X a1 %08X a2 %08X a4 %08X a7 %08X\n",
                         a, MEM[a], g_last_resume, g_calls[(g_call_at - 1) & 31], R.d[0], R.a[0], R.a[1], R.a[2], R.a[4], R.a[7]);
            return;
        }
    report("self-modifying write at", a);
    CODEMAP[a >> 3] &= u8(~(1u << (a & 7)));   // once per byte
}

void rt_bad_resume(u32 at) {
    platform_fatal("68k: bad resume $%06X", at);
}

void rt_unsupported(u32 address, const char *what) {
    platform_fatal("68k: unsupported instruction at $%06X: %s", address, what);
}

void rt_fell_off(u32 address) {
    platform_fatal("68k: flow fell off into unanalysed code at $%06X", address);
}

void rt_illegal(u32 address) {
    platform_fatal("68k: illegal instruction at $%06X", address);
}
