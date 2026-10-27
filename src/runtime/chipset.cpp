// The Amiga chipset at line granularity. The CPU side (registers,
// interrupts, CIA timers, beam position, input) is what the game needs to
// run; the picture side (copper, bitplanes, sprites, blitter) builds the
// reference frame the HD renderer replaces.
#include "runtime/chipset.h"

#include "runtime/cpu.h"
#include "runtime/platform.h"

#include <cstring>

// The reference picture; a named symbol so test tools can read it from xemu.
extern "C" u32 xenon2_screen[kScreenW * kScreenH];
u32 xenon2_screen[kScreenW * kScreenH];

namespace {

enum : u32 {
    DMACONR = 0x002, VPOSR = 0x004, VHPOSR = 0x006, JOY0DAT = 0x00A, JOY1DAT = 0x00C, CLXDAT = 0x00E,
    ADKCONR = 0x010, POTGOR = 0x016, SERDATR = 0x018, INTENAR = 0x01C, INTREQR = 0x01E,
    BLTCON0 = 0x040, BLTCON1 = 0x042, BLTAFWM = 0x044, BLTALWM = 0x046, BLTCPTH = 0x048, BLTBPTH = 0x04C,
    BLTAPTH = 0x050, BLTDPTH = 0x054, BLTSIZE = 0x058, BLTCMOD = 0x060, BLTBMOD = 0x062, BLTAMOD = 0x064,
    BLTDMOD = 0x066, BLTCDAT = 0x070, BLTBDAT = 0x072, BLTADAT = 0x074,
    COP1LCH = 0x080, COP2LCH = 0x084, COPJMP1 = 0x088, COPJMP2 = 0x08A, DIWSTRT = 0x08E, DIWSTOP = 0x090,
    DDFSTRT = 0x092, DDFSTOP = 0x094, DMACON = 0x096, INTENA = 0x09A, INTREQ = 0x09C, ADKCON = 0x09E,
    AUD0 = 0x0A0, BPL1PTH = 0x0E0, BPLCON0 = 0x100, BPLCON1 = 0x102, BPLCON2 = 0x104, BPL1MOD = 0x108,
    BPL2MOD = 0x10A, SPR0PTH = 0x120, SPR0POS = 0x140, COLOR00 = 0x180,
};

u16 regs[256];               // custom registers by offset / 2, as written
u16 dmacon, intena, intreq, adkcon;
int line;                    // the beam's line
u32 frame_count;

inline u16 &reg(u32 offset) { return regs[(offset & 0x1FE) >> 1]; }
inline u32 ptr(u32 offset) { return (u32(reg(offset)) << 16 | reg(offset + 2)) & 0x1FFFFE; }
inline void set_ptr(u32 offset, u32 value) {
    reg(offset) = u16(value >> 16);
    reg(offset + 2) = u16(value);
}
inline u16 chip16(u32 a) { a &= 0xFFFFE; return u16(MEM[a] << 8 | MEM[a + 1]); }
inline void chip16w(u32 a, u16 v) { a &= 0xFFFFE; MEM[a] = u8(v >> 8); MEM[a + 1] = u8(v); }

void raise(int bit) { intreq |= u16(1u << bit); }

// ---- CIAs ---------------------------------------------------------------
struct Cia {
    u8 pra, prb, ddra, ddrb, sdr, icr, icr_mask, cra, crb;
    u16 ta, tb, ta_latch, tb_latch;
    u32 tod, tod_alarm;
    bool tod_latched;
    u32 tod_latch;
    int irq_bit;             // INTREQ bit: PORTS (A) or EXTER (B)

    void underflow_a() {
        icr |= 0x01;
        ta = ta_latch;
        if (cra & 0x08) cra &= ~0x01;        // one-shot
    }
    void underflow_b() {
        icr |= 0x02;
        tb = tb_latch;
        if (crb & 0x08) crb &= ~0x01;
    }
    void tick(int eclocks) {
        if (cra & 0x01) {
            int left = int(ta) - eclocks;
            while (left < 0 && (cra & 0x01)) { underflow_a(); left += ta_latch + 1; }
            ta = u16(left < 0 ? 0 : left);
        }
        if ((crb & 0x01) && !(crb & 0x40)) {
            int left = int(tb) - eclocks;
            while (left < 0 && (crb & 0x01)) { underflow_b(); left += tb_latch + 1; }
            tb = u16(left < 0 ? 0 : left);
        }
        if (icr & icr_mask & 0x1F) raise(irq_bit);
    }
    void tod_tick() {
        tod = (tod + 1) & 0xFFFFFF;
        if (tod == tod_alarm) icr |= 0x04;
    }
    u8 read(int r) {
        switch (r) {
            case 0x0: return pra;
            case 0x1: return prb;
            case 0x2: return ddra;
            case 0x3: return ddrb;
            case 0x4: return u8(ta);
            case 0x5: return u8(ta >> 8);
            case 0x6: return u8(tb);
            case 0x7: return u8(tb >> 8);
            case 0x8: { u32 t = tod_latched ? tod_latch : tod; tod_latched = false; return u8(t); }
            case 0x9: return u8((tod_latched ? tod_latch : tod) >> 8);
            case 0xA: tod_latch = tod; tod_latched = true; return u8(tod >> 16);
            case 0xC: return sdr;
            case 0xD: {
                u8 v = icr;
                if (icr & icr_mask & 0x1F) v |= 0x80;
                icr = 0;
                return v;
            }
            case 0xE: return cra;
            case 0xF: return crb;
        }
        return 0xFF;
    }
    void write(int r, u8 v) {
        switch (r) {
            case 0x0: pra = v; break;
            case 0x1: prb = v; break;
            case 0x2: ddra = v; break;
            case 0x3: ddrb = v; break;
            case 0x4: ta_latch = u16((ta_latch & 0xFF00) | v); break;
            case 0x5:
                ta_latch = u16((ta_latch & 0x00FF) | v << 8);
                if (!(cra & 0x01)) ta = ta_latch;
                if (cra & 0x08) { ta = ta_latch; cra |= 0x01; }
                break;
            case 0x6: tb_latch = u16((tb_latch & 0xFF00) | v); break;
            case 0x7:
                tb_latch = u16((tb_latch & 0x00FF) | v << 8);
                if (!(crb & 0x01)) tb = tb_latch;
                if (crb & 0x08) { tb = tb_latch; crb |= 0x01; }
                break;
            case 0x8: tod = (tod & 0xFFFF00) | v; break;
            case 0x9: tod = (tod & 0xFF00FF) | v << 8; break;
            case 0xA: tod = (tod & 0x00FFFF) | u32(v) << 16; break;
            case 0xC: sdr = v; break;
            case 0xD:
                if (v & 0x80) icr_mask |= v & 0x1F;
                else icr_mask &= ~(v & 0x1F);
                break;
            case 0xE:
                if (v & 0x10) ta = ta_latch;
                cra = v & ~0x10;
                break;
            case 0xF:
                if (v & 0x10) tb = tb_latch;
                crb = v & ~0x10;
                break;
        }
    }
};

Cia cia_a, cia_b;
int eclock_frac;
Joystick joystick;

// ---- copper -------------------------------------------------------------
u32 cop_pc;
bool cop_waiting, cop_halted;
u16 cop_wait1, cop_wait2;

void custom_write(u32 offset, u16 v);

bool position_reached(u16 w1, u16 w2) {
    // Line granularity: a wait for any point of this line is met on it.
    const int vmask = (w2 >> 8) & 0x7F | 0x80, hmask = w2 & 0xFE;
    const int vp = (w1 >> 8) & vmask, hp = w1 & hmask;
    const int beam_v = line & 0xFF & vmask;
    if (beam_v != vp) return beam_v > vp;
    return hp <= (0xE2 & hmask);
}

void copper_run() {
    for (int steps = 0; steps < 1000 && !cop_halted; ++steps) {
        if (cop_waiting) {
            if (!position_reached(cop_wait1, cop_wait2)) return;
            cop_waiting = false;
        }
        const u16 w1 = chip16(cop_pc), w2 = chip16(cop_pc + 2);
        cop_pc += 4;
        if (!(w1 & 1)) {                       // MOVE
            const u32 offset = w1 & 0x1FE;
            if (offset < 0x40 && !(regs[0x02E >> 1] & 2) && offset < 0x20) { cop_halted = true; return; }
            custom_write(offset, w2);
        } else if (!(w2 & 1)) {                // WAIT
            cop_waiting = true;
            cop_wait1 = w1;
            cop_wait2 = w2;
        } else if (position_reached(w1, w2)) { // SKIP
            cop_pc += 4;
        }
    }
}

void copper_jump(u32 location) {
    cop_pc = location;
    cop_waiting = false;
    cop_halted = false;
}

// ---- blitter ------------------------------------------------------------
inline u16 minterm(u8 lf, u16 a, u16 b, u16 c) {
    u16 d = 0;
    if (lf & 0x80) d |= a & b & c;
    if (lf & 0x40) d |= a & b & ~c;
    if (lf & 0x20) d |= a & ~b & c;
    if (lf & 0x10) d |= a & ~b & ~c;
    if (lf & 0x08) d |= ~a & b & c;
    if (lf & 0x04) d |= ~a & b & ~c;
    if (lf & 0x02) d |= ~a & ~b & c;
    if (lf & 0x01) d |= ~a & ~b & ~c;
    return d;
}

bool blit_zero;

void blit_area(u16 con0, u16 con1, int width, int height) {
    const int ash = con0 >> 12, bsh = con1 >> 12;
    const bool use_a = con0 & 0x800, use_b = con0 & 0x400, use_c = con0 & 0x200, use_d = con0 & 0x100;
    const bool desc = con1 & 2, fill = con1 & 0x18, ife = con1 & 0x08;
    const u8 lf = u8(con0);
    u32 apt = ptr(BLTAPTH), bpt = ptr(BLTBPTH), cpt = ptr(BLTCPTH), dpt = ptr(BLTDPTH);
    const int step = desc ? -2 : 2;
    const s32 amod = s16(reg(BLTAMOD)) & ~1, bmod = s16(reg(BLTBMOD)) & ~1, cmod = s16(reg(BLTCMOD)) & ~1,
              dmod = s16(reg(BLTDMOD)) & ~1;
    u16 adat = reg(BLTADAT), bdat = reg(BLTBDAT), cdat = reg(BLTCDAT);
    u16 aprev = 0, bprev = 0;
    blit_zero = true;
    for (int y = 0; y < height; ++y) {
        bool fc = con1 & 4;
        for (int x = 0; x < width; ++x) {
            if (use_a) { adat = chip16(apt); apt += step; }
            if (use_b) { bdat = chip16(bpt); bpt += step; }
            if (use_c) { cdat = chip16(cpt); cpt += step; }
            u16 am = adat;
            if (x == 0) am &= reg(BLTAFWM);
            if (x == width - 1) am &= reg(BLTALWM);
            u16 as, bs;
            if (!desc) {
                as = u16((u32(aprev) << 16 | am) >> ash);
                bs = u16((u32(bprev) << 16 | bdat) >> bsh);
            } else {
                as = u16((u32(am) << 16 | aprev) >> (16 - ash));
                bs = u16((u32(bdat) << 16 | bprev) >> (16 - bsh));
            }
            aprev = am;
            bprev = bdat;
            u16 d = minterm(lf, as, bs, cdat);
            if (fill) {
                for (int bit = 0; bit < 16; ++bit) {
                    const u16 m = u16(1u << bit);
                    const bool edge = d & m;
                    if (edge) fc = !fc;
                    if (ife) { if (fc) d |= m; }
                    else d = u16((d & ~m) | (fc ? m : 0));
                }
            }
            if (d) blit_zero = false;
            if (use_d) { chip16w(dpt, d); dpt += step; }
        }
        if (use_a) apt += desc ? -amod : amod;
        if (use_b) bpt += desc ? -bmod : bmod;
        if (use_c) cpt += desc ? -cmod : cmod;
        if (use_d) dpt += desc ? -dmod : dmod;
    }
    set_ptr(BLTAPTH, apt);
    set_ptr(BLTBPTH, bpt);
    set_ptr(BLTCPTH, cpt);
    set_ptr(BLTDPTH, dpt);
    reg(BLTADAT) = adat;
    reg(BLTBDAT) = bdat;
    reg(BLTCDAT) = cdat;
}

void blit_line(u16 con0, u16 con1, int length) {
    // The Hardware Reference Manual's line algorithm (octants in BLTCON1).
    const u8 lf = u8(con0);
    const bool sud = con1 & 0x10, sul = con1 & 0x08, aul = con1 & 0x04, sing = con1 & 0x02;
    s32 error = s16(reg(BLTAPTH + 2));
    const s32 amod = s16(reg(BLTAMOD)), bmod = s16(reg(BLTBMOD));
    const s32 row = s16(reg(BLTCMOD));
    u32 cpt = ptr(BLTCPTH), dpt = ptr(BLTDPTH);
    int x = con0 >> 12;
    u16 texture = reg(BLTBDAT);
    int tbit = con1 >> 12;
    bool sign = con1 & 0x40;
    bool dot_this_row = false;
    blit_zero = true;
    for (int i = 0; i < length; ++i) {
        const u16 a = u16(0x8000u >> x);
        const u16 b = (texture >> (15 - tbit)) & 1 ? 0xFFFF : 0;
        const u16 c = chip16(cpt);
        u16 d = minterm(lf, a, b, c);
        if (!(sing && dot_this_row)) {
            chip16w(dpt, d);
            dot_this_row = true;
        }
        if (d) blit_zero = false;
        tbit = (tbit - 1) & 15;
        auto step_x = [&](int dir) {
            x += dir;
            if (x > 15) { x = 0; cpt += 2; }
            if (x < 0) { x = 15; cpt -= 2; }
        };
        auto step_y = [&](int dir) {
            cpt += dir * row;
            dot_this_row = false;
        };
        // Major axis: Y when SUD, else X. SUL/AUL give the directions.
        if (!sign) {
            if (sud) step_x(sul ? -1 : 1);
            else step_y(sul ? -1 : 1);
            error += amod;
        } else {
            error += bmod;
        }
        if (sud) step_y(aul ? -1 : 1);
        else step_x(aul ? -1 : 1);
        sign = error < 0;
        dpt = cpt;
    }
    set_ptr(BLTCPTH, cpt);
    set_ptr(BLTDPTH, dpt);
    reg(BLTAPTH + 2) = u16(error);
}

void blit(u16 size) {
    const u16 con0 = reg(BLTCON0), con1 = reg(BLTCON1);
    int height = size >> 6, width = size & 0x3F;
    if (!height) height = 1024;
    if (!width) width = 64;
    if (con1 & 1) blit_line(con0, con1, height);
    else blit_area(con0, con1, width, height);
    raise(6);
}

// ---- sprites ------------------------------------------------------------
struct Sprite {
    enum State { kControl, kWait, kActive, kDone } state;
    u16 pos, ctl, data, datb;
    bool armed;
    int vstart, vstop, hstart;
};
Sprite sprites[8];

void sprite_control(Sprite &s) {
    s.vstart = (s.pos >> 8) | (s.ctl & 4) << 6;
    s.vstop = (s.ctl >> 8) | (s.ctl & 2) << 7;
    s.hstart = (s.pos & 0xFF) << 1 | (s.ctl & 1);
}

void sprites_dma() {
    if (!(dmacon & 0x200) || !(dmacon & 0x20) || line < 25) return;
    for (int n = 0; n < 8; ++n) {
        Sprite &s = sprites[n];
        const u32 pt_reg = SPR0PTH + 4 * n;
        u32 pt = ptr(pt_reg);
        if (s.state == Sprite::kActive && line >= s.vstop) s.state = Sprite::kControl;
        if (s.state == Sprite::kControl) {
            s.pos = chip16(pt);
            s.ctl = chip16(pt + 2);
            pt += 4;
            sprite_control(s);
            s.armed = false;
            s.state = (s.pos | s.ctl) ? Sprite::kWait : Sprite::kDone;
        }
        if (s.state == Sprite::kWait && line >= s.vstart && line < s.vstop) s.state = Sprite::kActive;
        if (s.state == Sprite::kActive) {
            s.data = chip16(pt);
            s.datb = chip16(pt + 2);
            pt += 4;
            s.armed = true;
        } else {
            s.armed = false;
        }
        set_ptr(pt_reg, pt);
    }
}

// ---- the picture --------------------------------------------------------
u32 *const screen = xenon2_screen;
u32 palette[32];

inline u32 rgb12(u16 c) {
    const u32 r = (c >> 8) & 15, g = (c >> 4) & 15, b = c & 15;
    return (r * 0x11) << 16 | (g * 0x11) << 8 | (b * 0x11);
}

void render_line() {
    const int y = line - kScreenY;
    const bool visible = y >= 0 && y < kScreenH;
    for (int i = 0; i < 32; ++i) palette[i] = rgb12(regs[(COLOR00 >> 1) + i]);
    u32 *out = visible ? screen + y * kScreenW : nullptr;

    const u16 diwstrt = reg(DIWSTRT), diwstop = reg(DIWSTOP);
    const int vstart = diwstrt >> 8, vstop = (diwstop >> 8) | ((~diwstop >> 7) & 0x100);
    const int hstart = diwstrt & 0xFF, hstop = (diwstop & 0xFF) | 0x100;
    const u16 con0 = reg(BPLCON0);
    const int planes = (con0 >> 12) & 7;
    const bool dual = con0 & 0x400, hires = con0 & 0x8000;
    const bool fetch = (dmacon & 0x300) == 0x300 && planes && line >= vstart && line < vstop && !hires;

    // Playfield pixels of this line by beam position (lowres).
    static u8 pix[0x200];
    std::memset(pix, 0, sizeof(pix));
    if (fetch) {
        const int ddfstrt = reg(DDFSTRT) & 0xFC, ddfstop = reg(DDFSTOP) & 0xFC;
        const int words = ((ddfstop - ddfstrt) >> 3) + 1;
        const int first = ddfstrt * 2 + 17;
        const u16 con1 = reg(BPLCON1);
        for (int p = 0; p < planes; ++p) {
            u32 pt = ptr(BPL1PTH + 4 * p);
            const int delay = (p & 1) ? (con1 >> 4) & 15 : con1 & 15;
            for (int w = 0; w < words; ++w) {
                const u16 bits = chip16(pt + 2 * w);
                for (int b = 0; b < 16; ++b) {
                    const int x = first + delay + w * 16 + b;
                    if (x >= 0 && x < 0x200 && (bits >> (15 - b) & 1)) pix[x] |= u8(1 << p);
                }
            }
            pt += 2 * words + s16(reg((p & 1) ? BPL2MOD : BPL1MOD));
            set_ptr(BPL1PTH + 4 * p, pt);
        }
    }
    sprites_dma();
    if (!out) return;

    const u16 con2 = reg(BPLCON2);
    const int pf1p = con2 & 7, pf2p = (con2 >> 3) & 7;
    const bool pf2pri = con2 & 0x40;
    const bool ehb = planes == 6 && !dual && !(con0 & 0x800);
    for (int sx = 0; sx < kScreenW; ++sx) {
        const int x = kScreenX + sx;
        const bool inside = line >= vstart && line < vstop && x >= hstart && x < hstop;
        if (!inside) { out[sx] = palette[0]; continue; }
        const u8 p = pix[x];
        int color = 0, pf_front = 0;      // pf_front: the front playfield's sprite priority
        bool opaque = false;
        if (dual) {
            const int c1 = (p & 1) | (p >> 1 & 2) | (p >> 2 & 4);
            const int c2 = (p >> 1 & 1) | (p >> 2 & 2) | (p >> 3 & 4);
            const bool front2 = pf2pri;
            if (front2 ? c2 : !c1 && c2) { color = 8 + c2; pf_front = pf2p; }
            else if (c1) { color = c1; pf_front = pf1p; }
            else if (c2) { color = 8 + c2; pf_front = pf2p; }
            opaque = c1 || c2;
        } else {
            color = p;
            opaque = p != 0;
            pf_front = pf2p;
        }
        u32 rgb = ehb && color >= 32 ? (palette[color - 32] >> 1) & 0x777777 : palette[color & 31];
        // Sprites, pair 0 in front; a pair shows over the playfield when its
        // number is below the playfield's priority.
        for (int pair = 0; pair < 4; ++pair) {
            const Sprite &e = sprites[2 * pair], &o = sprites[2 * pair + 1];
            int ce = 0, co = 0;
            if (e.armed && x >= e.hstart && x < e.hstart + 16) {
                const int b = 15 - (x - e.hstart);
                ce = (e.data >> b & 1) | (e.datb >> b & 1) << 1;
            }
            if (o.armed && x >= o.hstart && x < o.hstart + 16) {
                const int b = 15 - (x - o.hstart);
                co = (o.data >> b & 1) | (o.datb >> b & 1) << 1;
            }
            int sc = 0;
            if (o.ctl & 0x80) sc = ce | co << 2;                 // attached: 16 colours
            else if (ce) sc = 4 * pair + ce;
            else if (co) sc = 4 * pair + co;
            if (sc && (!opaque || pair < pf_front)) { rgb = palette[16 + (sc & 15)]; break; }
        }
        out[sx] = rgb;
    }
}


void frame_done() {
    if (frame_count % 100 == 0)
        platform_log("chip: frame %u at $%06X dmacon %04X intena %04X intreq %04X bplcon0 %04X cop1lc %06X "
                     "diw %04X-%04X ddf %04X-%04X bpl1pt %06X sr %04X a7 %06X\n", frame_count, g_last_resume, dmacon,
                     intena, intreq, reg(BPLCON0), ptr(COP1LCH), reg(DIWSTRT), reg(DIWSTOP), reg(DDFSTRT),
                     reg(DDFSTOP), ptr(BPL1PTH), rt_get_sr(), R.a[7]);
    platform_frame(screen, kScreenW, kScreenH);
    joystick = platform_joystick();
    ++frame_count;
}

// ---- Paula: the interrupts the sound code waits for (sound itself is native) ----
struct Channel {
    bool on;
    s32 left;                // cycles to the end of the current block
};
Channel channels[4];

void audio_tick(int cycles) {
    for (int n = 0; n < 4; ++n) {
        Channel &c = channels[n];
        if (!c.on) continue;
        c.left -= cycles;
        if (c.left <= 0) {
            const u32 len = reg(AUD0 + 16 * n + 4) ? reg(AUD0 + 16 * n + 4) : 0x10000;
            const u32 per = reg(AUD0 + 16 * n + 6) < 124 ? 124 : reg(AUD0 + 16 * n + 6);
            c.left += s32(len * 2 * per * 2);
            raise(7 + n);
        }
    }
}

void audio_dma(u16 old_dmacon) {
    for (int n = 0; n < 4; ++n) {
        const bool now = (dmacon & 0x200) && (dmacon & (1 << n)), was = (old_dmacon & 0x200) && (old_dmacon & (1 << n));
        if (now && !was) {
            channels[n].on = true;
            channels[n].left = 2 * kLineCycles;      // the block starts: interrupt soon
        } else if (!now) {
            channels[n].on = false;
        }
    }
}

// ---- registers ----------------------------------------------------------
u16 joy1dat() {
    const Joystick &j = joystick;
    u16 v = 0;
    if (j.right) v |= 0x0002;
    if (j.left) v |= 0x0200;
    if (j.down != j.right) v |= 0x0001;
    if (j.up != j.left) v |= 0x0100;
    return v;
}

void custom_write(u32 offset, u16 v) {
    offset &= 0x1FE;
    switch (offset) {
        case DMACON: {
            const u16 old = dmacon;
            dmacon = v & 0x8000 ? u16(dmacon | (v & 0x7FF)) : u16(dmacon & ~v);
            audio_dma(old);
            break;
        }
        case INTENA: intena = v & 0x8000 ? u16(intena | (v & 0x7FFF)) : u16(intena & ~v); break;
        case INTREQ: intreq = v & 0x8000 ? u16(intreq | (v & 0x7FFF)) : u16(intreq & ~v); break;
        case ADKCON: adkcon = v & 0x8000 ? u16(adkcon | (v & 0x7FFF)) : u16(adkcon & ~v); break;
        case COPJMP1: copper_jump(ptr(COP1LCH)); copper_run(); break;
        case COPJMP2: copper_jump(ptr(COP2LCH)); copper_run(); break;
        case BLTSIZE: reg(offset) = v; blit(v); break;
        default:
            reg(offset) = v;
            if (offset >= SPR0PTH && offset < SPR0POS && (offset & 2) == 0) {
                // A new sprite list: control words next.
                sprites[(offset - SPR0PTH) >> 2].state = Sprite::kControl;
            } else if (offset >= SPR0POS && offset < COLOR00) {
                Sprite &s = sprites[(offset - SPR0POS) >> 3];
                switch (offset & 6) {
                    case 0: s.pos = v; sprite_control(s); break;
                    case 2: s.ctl = v; sprite_control(s); s.armed = false; break;
                    case 4: s.data = v; s.armed = true; break;
                    case 6: s.datb = v; break;
                }
            }
            break;
    }
}

u16 custom_read(u32 offset) {
    offset &= 0x1FE;
    switch (offset) {
        case DMACONR: return u16((dmacon & 0x7FF) | (blit_zero ? 0x2000 : 0));
        case VPOSR: {
            const int l = R.cyc / kLineCycles;
            return u16(0x8000 | ((l >> 8) & 1));
        }
        case VHPOSR: {
            const int l = R.cyc / kLineCycles, h = (R.cyc % kLineCycles) / 2;
            return u16((l & 0xFF) << 8 | (h & 0xFF));
        }
        case JOY0DAT: return 0;
        case JOY1DAT: return joy1dat();
        case CLXDAT: return 0x8000;
        case ADKCONR: return adkcon;
        case POTGOR: return u16(0xFF00 & ~(joystick.fire2 ? 0x4000 : 0));
        case SERDATR: return 0x3000;
        case INTENAR: return intena;
        case INTREQR: return intreq;
    }
    return reg(offset);
}

int g_logged_reads;

} // namespace

u16 chipset_reg(u32 offset) { return reg(offset); }

void chipset_reset() {
    std::memset(regs, 0, sizeof(regs));
    dmacon = intreq = adkcon = 0;
    // As the WHDLoad slave leaves it (and the CDTV's OS before it): the
    // vertical blank interrupt enabled. The game never enables it itself.
    intena = 0x4020;
    line = 0;
    std::memset(&cia_a, 0, sizeof(cia_a));
    std::memset(&cia_b, 0, sizeof(cia_b));
    cia_a.irq_bit = 3;
    cia_b.irq_bit = 13;
    cia_a.pra = 0xFF;
    cia_a.ta_latch = cia_a.tb_latch = cia_b.ta_latch = cia_b.tb_latch = 0xFFFF;
    cop_halted = true;
    std::memset(sprites, 0, sizeof(sprites));
    for (Sprite &s : sprites) s.state = Sprite::kDone;
    std::memset(channels, 0, sizeof(channels));
    R.cyc = 0;
    R.event = kLineCycles;
}

void chipset_advance() {
    // The line that just ended was `line`: its CIA time and audio blocks.
    eclock_frac += kLineCycles;
    const int ticks = eclock_frac / 10;
    eclock_frac %= 10;
    cia_a.tick(ticks);
    cia_b.tick(ticks);
    cia_b.tod_tick();
    audio_tick(kLineCycles);

    ++line;
    R.event += kLineCycles;
    if (line >= kFrameLines) {
        frame_done();
        line = 0;
        R.cyc -= kFrameLines * kLineCycles;
        R.event -= kFrameLines * kLineCycles;
        raise(5);                               // VERTB
        cia_a.tod_tick();
        cia_a.tick(0);
        // The copper starts over; sprites fetch their control words again.
        copper_jump(ptr(COP1LCH));
        for (Sprite &s : sprites) s.state = Sprite::kControl;
    }
    if (dmacon & 0x200 && dmacon & 0x80) copper_run();
    render_line();
}

void chipset_ack_level(int level) {
    static const u8 level_of[14] = {1, 1, 1, 2, 3, 3, 3, 4, 4, 4, 4, 5, 5, 6};
    for (int bit = 0; bit < 14; ++bit)
        if (level_of[bit] == level) intreq &= u16(~(1u << bit));
}

int chipset_irq_level() {
    if (!(intena & 0x4000)) return 0;
    const u16 pending = intena & intreq & 0x3FFF;
    if (!pending) return 0;
    static const u8 level_of[14] = {1, 1, 1, 2, 3, 3, 3, 4, 4, 4, 4, 5, 5, 6};
    int level = 0;
    for (int bit = 0; bit < 14; ++bit)
        if (pending & (1 << bit) && level_of[bit] > level) level = level_of[bit];
    return level;
}

// ---- the bus above chip RAM ---------------------------------------------
static bool is_custom(u32 a) { return (a & 0xFFF000) == 0xDFF000; }
static bool is_cia(u32 a) { return (a & 0xFF0000) == 0xBF0000; }

u32 chip_read8(u32 a) {
    if (is_custom(a)) {
        const u16 v = custom_read(a & 0x1FE);
        return a & 1 ? v & 0xFF : v >> 8;
    }
    if (is_cia(a)) {
        const int r = (a >> 8) & 15;
        if (!(a & 0x1000) && (a & 1)) {
            u8 v = cia_a.read(r);
            if (r == 0) v = u8((v & 0x3F) | (joystick.fire ? 0 : 0x80) | 0x40);
            return v;
        }
        if (!(a & 0x2000) && !(a & 1)) return cia_b.read(r);
        return 0xFF;
    }
    if (g_logged_reads++ < 32) platform_log("chip: read8 $%06X\n", a);
    return 0;
}

u32 chip_read16(u32 a) {
    if (is_custom(a)) return custom_read(a & 0x1FE);
    if (is_cia(a)) return chip_read8(a) << 8 | chip_read8(a + 1);
    if (g_logged_reads++ < 32) platform_log("chip: read16 $%06X\n", a);
    return 0;
}

void chip_write8(u32 a, u32 v) {
    if (is_custom(a)) {
        custom_write(a & 0x1FE, u16(v << 8 | v));
        return;
    }
    if (is_cia(a)) {
        const int r = (a >> 8) & 15;
        if (!(a & 0x1000) && (a & 1)) cia_a.write(r, u8(v));
        if (!(a & 0x2000) && !(a & 1)) cia_b.write(r, u8(v));
    }
}

void chip_write16(u32 a, u32 v) {
    if (is_custom(a)) {
        custom_write(a & 0x1FE, u16(v));
        return;
    }
    if (is_cia(a)) {
        chip_write8(a, v >> 8);
        chip_write8(a + 1, v & 0xFF);
    }
}
