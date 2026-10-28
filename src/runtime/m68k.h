// The 68000 state and helpers the recompiled code (build/m68k) runs on.
//
// Memory is the CDTV's 1 MB of chip RAM, big-endian as on the Amiga;
// addresses are 24-bit. Anything above 1 MB (the custom chips at $DFF000,
// the CIAs at $BFxxxx) goes to the chipset (chipset.cpp).
//
// Calls: the generated code pushes the 68000 return address on the 68000
// stack and makes a C call; rts pops it into R.pc and returns. A caller
// that finds R.pc is not its own return address (the game dropped or
// replaced a return address) returns too, until a frame that matches or the
// top-level loop, which resumes R.pc through the dispatch tables.
#pragma once

#include <cstdint>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;

struct M68K {
    u32 d[8], a[8];
    u32 usp, ssp;        // the inactive stack pointer is kept here
    u32 pc;              // set by rts/rte: where the code goes on
    u8 x, n, z, v, c;
    u16 sr_high;         // T, S and the interrupt mask (SR & 0xFF00)
    s32 cyc;             // 68000 cycles into this frame
    s32 event;           // cycle of the next chipset event (rt_events)
};

extern M68K R;
extern u8 MEM[];         // 1 MB chip RAM
extern u8 CODEMAP[];     // a bit per byte compiled as an instruction (self-modifying code check)

constexpr u32 kChipSize = 0x100000;

u32 chip_read8(u32 a);
u32 chip_read16(u32 a);
void chip_write8(u32 a, u32 v);
void chip_write16(u32 a, u32 v);
void rt_smc(u32 a);

static inline u16 be16(const u8 *p) { return u16(p[0] << 8 | p[1]); }
static inline u32 be32(const u8 *p) { return u32(p[0]) << 24 | u32(p[1]) << 16 | u32(p[2]) << 8 | p[3]; }

static inline u32 rb(u32 a) {
    a &= 0xFFFFFF;
    return a < kChipSize ? MEM[a] : chip_read8(a);
}
static inline u32 rw(u32 a) {
    a &= 0xFFFFFF;
    return a < kChipSize - 1 ? be16(MEM + a) : chip_read16(a);
}
static inline u32 rl(u32 a) {
    a &= 0xFFFFFF;
    return a < kChipSize - 3 ? be32(MEM + a) : (chip_read16(a) << 16 | chip_read16(a + 2));
}
static inline void smc_check(u32 a, int n) {
    for (int i = 0; i < n; ++i)
        if (CODEMAP[(a + i) >> 3] >> ((a + i) & 7) & 1) rt_smc(a + i);
}
static inline void wb(u32 a, u32 v) {
    a &= 0xFFFFFF;
    if (a < kChipSize) {
        MEM[a] = u8(v);
        if (CODEMAP[a >> 3]) smc_check(a, 1);
    } else {
        chip_write8(a, v & 0xFF);
    }
}
static inline void ww(u32 a, u32 v) {
    a &= 0xFFFFFF;
    if (a < kChipSize - 1) {
        MEM[a] = u8(v >> 8);
        MEM[a + 1] = u8(v);
        if (CODEMAP[a >> 3] | CODEMAP[(a + 1) >> 3]) smc_check(a, 2);
    } else {
        chip_write16(a, v & 0xFFFF);
    }
}
static inline void wl(u32 a, u32 v) {
    a &= 0xFFFFFF;
    if (a < kChipSize - 3) {
        MEM[a] = u8(v >> 24);
        MEM[a + 1] = u8(v >> 16);
        MEM[a + 2] = u8(v >> 8);
        MEM[a + 3] = u8(v);
        if (CODEMAP[a >> 3] | CODEMAP[(a + 3) >> 3]) smc_check(a, 4);
    } else {
        chip_write16(a, v >> 16);
        chip_write16(a + 2, v & 0xFFFF);
    }
}

static inline void push32(u32 v) { R.a[7] -= 4; wl(R.a[7], v); }
static inline void push16(u32 v) { R.a[7] -= 2; ww(R.a[7], v); }
static inline u32 pop32() { u32 v = rl(R.a[7]); R.a[7] += 4; return v; }
static inline u32 pop16() { u32 v = rw(R.a[7]); R.a[7] += 2; return v; }

// ---- flags ----------------------------------------------------------------
static inline void logic8(u32 v) { R.n = (v >> 7) & 1; R.z = (v & 0xFF) == 0; R.v = 0; R.c = 0; }
static inline void logic16(u32 v) { R.n = (v >> 15) & 1; R.z = (v & 0xFFFF) == 0; R.v = 0; R.c = 0; }
static inline void logic32(u32 v) { R.n = v >> 31; R.z = v == 0; R.v = 0; R.c = 0; }

#define M68K_ARITH(BITS, MASKV, SIGN)                                                           \
    static inline u32 add##BITS(u32 d, u32 s) {                                                \
        u32 r = (d + s) & MASKV;                                                               \
        R.n = (r >> SIGN) & 1; R.z = r == 0;                                                   \
        R.v = (((s ^ r) & (d ^ r)) >> SIGN) & 1;                                               \
        R.c = R.x = (((s & d) | (~r & (s | d))) >> SIGN) & 1;                                  \
        return r;                                                                              \
    }                                                                                          \
    static inline u32 sub##BITS(u32 d, u32 s) {                                                \
        u32 r = (d - s) & MASKV;                                                               \
        R.n = (r >> SIGN) & 1; R.z = r == 0;                                                   \
        R.v = (((s ^ d) & (r ^ d)) >> SIGN) & 1;                                               \
        R.c = R.x = (((s & ~d) | (r & ~d) | (s & r)) >> SIGN) & 1;                             \
        return r;                                                                              \
    }                                                                                          \
    static inline void cmp##BITS(u32 d, u32 s) {                                               \
        u32 r = (d - s) & MASKV;                                                               \
        R.n = (r >> SIGN) & 1; R.z = r == 0;                                                   \
        R.v = (((s ^ d) & (r ^ d)) >> SIGN) & 1;                                               \
        R.c = (((s & ~d) | (r & ~d) | (s & r)) >> SIGN) & 1;                                   \
    }                                                                                          \
    static inline u32 addx##BITS(u32 d, u32 s) {                                               \
        u32 r = (d + s + R.x) & MASKV;                                                         \
        R.n = (r >> SIGN) & 1; if (r) R.z = 0;                                                 \
        R.v = (((s ^ r) & (d ^ r)) >> SIGN) & 1;                                               \
        R.c = R.x = (((s & d) | (~r & (s | d))) >> SIGN) & 1;                                  \
        return r;                                                                              \
    }                                                                                          \
    static inline u32 subx##BITS(u32 d, u32 s) {                                               \
        u32 r = (d - s - R.x) & MASKV;                                                         \
        R.n = (r >> SIGN) & 1; if (r) R.z = 0;                                                 \
        R.v = (((s ^ d) & (r ^ d)) >> SIGN) & 1;                                               \
        R.c = R.x = (((s & ~d) | (r & ~d) | (s & r)) >> SIGN) & 1;                             \
        return r;                                                                              \
    }                                                                                          \
    static inline u32 neg##BITS(u32 d) { return sub##BITS(0, d); }                             \
    static inline u32 negx##BITS(u32 d) { return subx##BITS(0, d); }

M68K_ARITH(8, 0xFFu, 7)
M68K_ARITH(16, 0xFFFFu, 15)
M68K_ARITH(32, 0xFFFFFFFFu, 31)

// ---- shifts: count 0-63 ---------------------------------------------------
#define M68K_SHIFTS(BITS, MASKV, SIGN)                                                          \
    static inline u32 lsl##BITS(u32 v, u32 n) {                                                \
        v &= MASKV;                                                                            \
        if (n == 0) { R.c = 0; } else if (n <= BITS) {                                         \
            R.c = R.x = (u32)((uint64_t)v >> (BITS - n)) & 1; v = (u32)((uint64_t)v << n) & MASKV; \
        } else { R.c = R.x = 0; v = 0; }                                                       \
        R.n = (v >> SIGN) & 1; R.z = v == 0; R.v = 0; return v;                                \
    }                                                                                          \
    static inline u32 lsr##BITS(u32 v, u32 n) {                                                \
        v &= MASKV;                                                                            \
        if (n == 0) { R.c = 0; } else if (n <= BITS) {                                         \
            R.c = R.x = (u32)((uint64_t)v >> (n - 1)) & 1; v = (u32)((uint64_t)v >> n);        \
        } else { R.c = R.x = 0; v = 0; }                                                       \
        R.n = (v >> SIGN) & 1; R.z = v == 0; R.v = 0; return v;                                \
    }                                                                                          \
    static inline u32 asl##BITS(u32 v, u32 n) {                                                \
        v &= MASKV;                                                                            \
        u32 vflag = 0;                                                                         \
        for (u32 i = 0; i < n; ++i) {                                                          \
            u32 top = (v >> SIGN) & 1;                                                         \
            R.c = R.x = top; v = (v << 1) & MASKV;                                             \
            if (((v >> SIGN) & 1) != top) vflag = 1;                                           \
        }                                                                                      \
        if (n == 0) R.c = 0;                                                                   \
        R.n = (v >> SIGN) & 1; R.z = v == 0; R.v = vflag; return v;                            \
    }                                                                                          \
    static inline u32 asr##BITS(u32 v, u32 n) {                                                \
        v &= MASKV;                                                                            \
        u32 sign = (v >> SIGN) & 1;                                                            \
        for (u32 i = 0; i < n; ++i) { R.c = R.x = v & 1; v = (v >> 1) | (sign << SIGN); }      \
        if (n == 0) R.c = 0;                                                                   \
        R.n = (v >> SIGN) & 1; R.z = v == 0; R.v = 0; return v;                                \
    }                                                                                          \
    static inline u32 rol##BITS(u32 v, u32 n) {                                                \
        v &= MASKV;                                                                            \
        for (u32 i = 0; i < n; ++i) { R.c = (v >> SIGN) & 1; v = ((v << 1) | R.c) & MASKV; }   \
        if (n == 0) R.c = 0;                                                                   \
        R.n = (v >> SIGN) & 1; R.z = v == 0; R.v = 0; return v;                                \
    }                                                                                          \
    static inline u32 ror##BITS(u32 v, u32 n) {                                                \
        v &= MASKV;                                                                            \
        for (u32 i = 0; i < n; ++i) { R.c = v & 1; v = (v >> 1) | (R.c << SIGN); }             \
        if (n == 0) R.c = 0;                                                                   \
        R.n = (v >> SIGN) & 1; R.z = v == 0; R.v = 0; return v;                                \
    }                                                                                          \
    static inline u32 roxl##BITS(u32 v, u32 n) {                                               \
        v &= MASKV;                                                                            \
        for (u32 i = 0; i < n; ++i) { u32 out = (v >> SIGN) & 1; v = ((v << 1) | R.x) & MASKV; R.x = out; } \
        R.c = R.x;                                                                             \
        R.n = (v >> SIGN) & 1; R.z = v == 0; R.v = 0; return v;                                \
    }                                                                                          \
    static inline u32 roxr##BITS(u32 v, u32 n) {                                               \
        v &= MASKV;                                                                            \
        for (u32 i = 0; i < n; ++i) { u32 out = v & 1; v = (v >> 1) | (R.x << SIGN); R.x = out; } \
        R.c = R.x;                                                                             \
        R.n = (v >> SIGN) & 1; R.z = v == 0; R.v = 0; return v;                                \
    }

M68K_SHIFTS(8, 0xFFu, 7)
M68K_SHIFTS(16, 0xFFFFu, 15)
M68K_SHIFTS(32, 0xFFFFFFFFu, 31)

// ---- division, chk, bcd ---------------------------------------------------
// Returns 1 on division by zero (the caller raises the exception).
static inline int divu(u32 *d, u32 s) {
    s &= 0xFFFF;
    if (!s) return 1;
    u32 q = *d / s, r = *d % s;
    R.c = 0;
    if (q > 0xFFFF) { R.v = 1; R.n = 1; return 0; }
    *d = r << 16 | q;
    R.v = 0; R.n = (q >> 15) & 1; R.z = q == 0;
    return 0;
}
static inline int divs(u32 *d, u32 s) {
    s32 div = (s16)s;
    if (!div) return 1;
    s32 num = (s32)*d;
    R.c = 0;
    if (num == INT32_MIN && div == -1) { R.v = 1; R.n = 1; return 0; }
    s32 q = num / div, r = num % div;
    if (q < -32768 || q > 32767) { R.v = 1; R.n = 1; return 0; }
    *d = (u32)(u16)r << 16 | (u16)q;
    R.v = 0; R.n = ((u32)q >> 15) & 1; R.z = (q & 0xFFFF) == 0;
    return 0;
}
static inline int chk(s16 d, s16 bound) {
    if (d < 0) { R.n = 1; return 1; }
    if (d > bound) { R.n = 0; return 1; }
    return 0;
}
static inline u32 abcd(u32 d, u32 s) {
    u32 lo = (d & 15) + (s & 15) + R.x, r = (d & 0xF0) + (s & 0xF0) + lo;
    if (lo > 9) r += 6;
    if (r > 0x99) { r += 0x60; R.c = R.x = 1; } else { R.c = R.x = 0; }
    r &= 0xFF;
    if (r) R.z = 0;
    R.n = r >> 7;
    return r;
}
static inline u32 sbcd(u32 d, u32 s) {
    s32 lo = (s32)(d & 15) - (s32)(s & 15) - R.x, r = (s32)(d & 0xF0) - (s32)(s & 0xF0) + lo;
    if (lo < 0) r -= 6;
    if (r < 0) { r -= 0x60; R.c = R.x = 1; } else { R.c = R.x = 0; }
    r &= 0xFF;
    if (r) R.z = 0;
    R.n = (u32)r >> 7;
    return (u32)r;
}

// ---- runtime (cpu.cpp) ----------------------------------------------------
typedef void (*m68k_fn)(u32 at);

u32 rt_get_sr();
void rt_set_sr(u32 sr);
void rt_set_ccr(u32 ccr);
// Chipset events and interrupts due at this point; resume is where the
// interrupted code goes on. Returns 1 when it must not (unwind).
int rt_events(u32 resume);
int rt_exception(int vector, u32 resume);
int rt_stop(u32 resume);
void rt_call(u32 address);
void rt_bad_resume(u32 at);
void rt_unsupported(u32 address, const char *what);
void rt_fell_off(u32 address);
void rt_illegal(u32 address);

// Native replacements (hooks.cpp), named in config/xenon2.json.
void hook_load_file(u32 dest);   // name at d1 + 4 (past "CD0:"); d0 = size, d1 = 0
void hook_cd_play();             // d0 = track
void hook_cd_poll();
void hook_cd_stop();
void hook_cd_fade_in();
void hook_cd_fade_out();
void hook_sound_voice();         // a0 = the voice's request, a3 = the sound bank, d0 = its DMA bits

// Probes (display.cpp): called before the instruction at their address.
void probe_update();             // $346B2: an update's drawing starts
void probe_stars();              // $35268: the stars have moved
void probe_object();             // $3414E: d0 = x, d1 = y, a0 = graphic
void probe_object_flash();       // $34230: the same, drawn in one colour
void probe_object_masked();      // $34314: the same, through a mask
