"""68000 instruction decoder (no 68010+ instructions).

decode(memory, address) -> Insn. Operands are Ea objects that keep where
their extension words sit in the instruction, so the recompiler can read an
operand from memory at run time when the game patches it (self-modifying
code) instead of baking it in.
"""
from __future__ import annotations

import struct
from dataclasses import dataclass, field

CONDITIONS = ["t", "f", "hi", "ls", "cc", "cs", "ne", "eq", "vc", "vs", "pl", "mi", "ge", "lt", "gt", "le"]
SIZES = {1: "b", 2: "w", 4: "l"}


class DecodeError(Exception):
    pass


@dataclass
class Ea:
    mode: str               # dn an ind postinc predec disp index absw absl pcdisp pcindex imm
    reg: int = 0
    disp: int = 0           # disp/index displacement (signed)
    index: tuple | None = None   # (is_address_reg, number, is_long)
    value: int = 0          # absolute address, immediate, or pc-relative target
    ext: tuple | None = None     # (byte offset in the instruction, length) of the extension data

    def is_memory(self) -> bool:
        return self.mode not in ("dn", "an", "imm")

    def text(self) -> str:
        idx = lambda: f"{'a' if self.index[0] else 'd'}{self.index[1]}.{'l' if self.index[2] else 'w'}"
        return {
            "dn": lambda: f"d{self.reg}", "an": lambda: f"a{self.reg}", "ind": lambda: f"(a{self.reg})",
            "postinc": lambda: f"(a{self.reg})+", "predec": lambda: f"-(a{self.reg})",
            "disp": lambda: f"{self.disp}(a{self.reg})", "index": lambda: f"{self.disp}(a{self.reg},{idx()})",
            "absw": lambda: f"${self.value:x}.w", "absl": lambda: f"${self.value:x}.l",
            "pcdisp": lambda: f"${self.value:x}(pc)", "pcindex": lambda: f"${self.value:x}(pc,{idx()})",
            "imm": lambda: f"#${self.value:x}",
        }[self.mode]()


@dataclass
class Insn:
    address: int
    length: int
    op: str
    size: int = 0           # operand size in bytes (0 when none)
    src: Ea | None = None
    dst: Ea | None = None
    cond: str = ""          # Bcc/DBcc/Scc condition
    target: int | None = None    # branch/call target
    mask: int = 0           # MOVEM register mask (bit 0 = d0 ... bit 15 = a7, as for (An)+)
    extra: dict = field(default_factory=dict)

    def text(self) -> str:
        name = self.op + self.cond + (f".{SIZES[self.size]}" if self.size else "")
        args = [e.text() for e in (self.src, self.dst) if e is not None]
        if self.target is not None and self.op in ("b", "bra", "bsr", "db"):
            args.append(f"${self.target:x}")
        if self.op == "movem":
            regs = ",".join(("d%d" if i < 8 else "a%d") % (i & 7) for i in range(16) if self.mask >> i & 1)
            args = [regs, self.dst.text()] if self.extra["to_memory"] else [self.src.text(), regs]
        return f"{name:10} {', '.join(args)}"


class Reader:
    def __init__(self, memory: bytes, address: int):
        self.memory, self.start, self.at = memory, address, address

    def word(self) -> int:
        if self.at + 2 > len(self.memory):
            raise DecodeError("past the end of memory")
        (w,) = struct.unpack_from(">H", self.memory, self.at)
        self.at += 2
        return w

    def long(self) -> int:
        return self.word() << 16 | self.word()

    def offset(self) -> int:
        return self.at - self.start


def sext(value: int, bits: int) -> int:
    value &= (1 << bits) - 1
    return value - (1 << bits) if value >> (bits - 1) else value


def ea(r: Reader, mode: int, reg: int, size: int) -> Ea:
    if mode == 0:
        return Ea("dn", reg)
    if mode == 1:
        return Ea("an", reg)
    if mode == 2:
        return Ea("ind", reg)
    if mode == 3:
        return Ea("postinc", reg)
    if mode == 4:
        return Ea("predec", reg)
    start = r.offset()
    if mode == 5:
        return Ea("disp", reg, disp=sext(r.word(), 16), ext=(start, 2))
    if mode == 6:
        e = r.word()
        if e & 0x0700:
            raise DecodeError("68020 index extension")
        return Ea("index", reg, disp=sext(e, 8), index=(bool(e & 0x8000), e >> 12 & 7, bool(e & 0x800)), ext=(start, 2))
    if reg == 0:
        return Ea("absw", value=sext(r.word(), 16) & 0xFFFFFFFF, ext=(start, 2))
    if reg == 1:
        return Ea("absl", value=r.long(), ext=(start, 4))
    if reg == 2:
        base = r.start + start
        return Ea("pcdisp", value=(base + sext(r.word(), 16)) & 0xFFFFFFFF, ext=(start, 2))
    if reg == 3:
        base = r.start + start
        e = r.word()
        if e & 0x0700:
            raise DecodeError("68020 index extension")
        return Ea("pcindex", value=(base + sext(e, 8)) & 0xFFFFFFFF,
                  index=(bool(e & 0x8000), e >> 12 & 7, bool(e & 0x800)), ext=(start, 2))
    if reg == 4:
        if size == 4:
            return Ea("imm", value=r.long(), ext=(start, 4))
        w = r.word()
        return Ea("imm", value=w & 0xFF if size == 1 else w, ext=(start + (1 if size == 1 else 0), size))
    raise DecodeError(f"bad addressing mode 7/{reg}")


def imm(r: Reader, size: int) -> Ea:
    return ea(r, 7, 4, size)


def check(e: Ea, *allowed: str) -> Ea:
    if e.mode not in allowed:
        raise DecodeError(f"addressing mode {e.mode} not allowed here")
    return e


DATA_ALTERABLE = ("dn", "ind", "postinc", "predec", "disp", "index", "absw", "absl")
MEMORY_ALTERABLE = DATA_ALTERABLE[1:]
ALL = DATA_ALTERABLE + ("an", "pcdisp", "pcindex", "imm")
DATA = tuple(m for m in ALL if m != "an")
CONTROL = ("ind", "disp", "index", "absw", "absl", "pcdisp", "pcindex")
ALTERABLE = DATA_ALTERABLE + ("an",)


def decode(memory: bytes, address: int) -> Insn:
    r = Reader(memory, address)
    op = r.word()
    top = op >> 12
    mode, reg = op >> 3 & 7, op & 7
    reg9 = op >> 9 & 7
    ss = op >> 6 & 3

    def done(name: str, **kw) -> Insn:
        return Insn(address, r.at - address, name, **kw)

    if top == 0:
        if op & 0x0100:
            if mode == 1:
                size = 4 if op & 0x40 else 2
                d = sext(r.word(), 16)
                mem = Ea("disp", reg, disp=d, ext=(2, 2))
                if op & 0x80:
                    return done("movep", size=size, src=Ea("dn", reg9), dst=mem)
                return done("movep", size=size, src=mem, dst=Ea("dn", reg9))
            name = ["btst", "bchg", "bclr", "bset"][ss]
            dst = ea(r, mode, reg, 1)
            check(dst, *(DATA if name == "btst" else DATA_ALTERABLE))
            return done(name, size=4 if dst.mode == "dn" else 1, src=Ea("dn", reg9), dst=dst)
        kind = reg9
        if kind == 4:
            name = ["btst", "bchg", "bclr", "bset"][ss]
            bit = imm(r, 1)
            dst = ea(r, mode, reg, 1)
            check(dst, *(tuple(m for m in DATA if m != "imm") if name == "btst" else DATA_ALTERABLE))
            return done(name, size=4 if dst.mode == "dn" else 1, src=bit, dst=dst)
        if kind == 7:
            raise DecodeError("illegal 0000 111x")
        name = ["ori", "andi", "subi", "addi", None, "eori", "cmpi"][kind]
        if ss == 3:
            raise DecodeError("bad immediate size")
        size = 1 << ss
        if op & 0x3F == 0x3C and name in ("ori", "andi", "eori"):
            if ss == 0:
                return done(name + "_ccr", size=1, src=imm(r, 1))
            if ss == 1:
                return done(name + "_sr", size=2, src=imm(r, 2))
        src = imm(r, size)
        dst = check(ea(r, mode, reg, size), *DATA_ALTERABLE)
        return done(name, size=size, src=src, dst=dst)

    if top in (1, 2, 3):
        size = {1: 1, 3: 2, 2: 4}[top]
        src = check(ea(r, mode, reg, size), *ALL)
        if size == 1 and src.mode == "an":
            raise DecodeError("move.b from an")
        dmode = op >> 6 & 7
        dst = ea(r, dmode, reg9, size)
        if dst.mode == "an":
            if size == 1:
                raise DecodeError("movea.b")
            return done("movea", size=size, src=src, dst=dst)
        check(dst, *DATA_ALTERABLE)
        return done("move", size=size, src=src, dst=dst)

    if top == 4:
        if op == 0x4AFC:
            return done("illegal")
        if op & 0x01C0 == 0x01C0:
            return done("lea", size=4, src=check(ea(r, mode, reg, 4), *CONTROL), dst=Ea("an", reg9))
        if op & 0x01C0 == 0x0180:
            return done("chk", size=2, src=check(ea(r, mode, reg, 2), *DATA), dst=Ea("dn", reg9))
        if op & 0x0100:
            raise DecodeError("68020 chk.l")
        sub = op >> 8 & 0xF
        if sub in (0, 2, 4, 6):
            if ss == 3:
                if sub == 0:
                    return done("move_from_sr", size=2, dst=check(ea(r, mode, reg, 2), *DATA_ALTERABLE))
                if sub == 2:
                    raise DecodeError("68010 move from ccr")
                if sub == 4:
                    return done("move_to_ccr", size=2, src=check(ea(r, mode, reg, 2), *DATA))
                return done("move_to_sr", size=2, src=check(ea(r, mode, reg, 2), *DATA))
            name = {0: "negx", 2: "clr", 4: "neg", 6: "not"}[sub]
            return done(name, size=1 << ss, dst=check(ea(r, mode, reg, 1 << ss), *DATA_ALTERABLE))
        if sub == 8:
            if ss == 0:
                return done("nbcd", size=1, dst=check(ea(r, mode, reg, 1), *DATA_ALTERABLE))
            if ss == 1:
                if mode == 0:
                    return done("swap", size=4, dst=Ea("dn", reg))
                return done("pea", size=4, src=check(ea(r, mode, reg, 4), *CONTROL))
            if mode == 0:
                return done("ext", size=2 if ss == 2 else 4, dst=Ea("dn", reg))
        if sub in (8, 0xC) and ss >= 2:
            size = 2 if ss == 2 else 4
            mask = r.word()
            to_memory = sub == 8
            e = ea(r, mode, reg, size)
            if to_memory:
                check(e, "ind", "predec", "disp", "index", "absw", "absl")
                if e.mode == "predec":   # mask is reversed: bit 0 = a7
                    mask = int(f"{mask:016b}"[::-1], 2)
                return done("movem", size=size, dst=e, mask=mask, extra={"to_memory": True})
            check(e, "ind", "postinc", "disp", "index", "absw", "absl", "pcdisp", "pcindex")
            return done("movem", size=size, src=e, mask=mask, extra={"to_memory": False})
        if sub == 0xA:
            if ss == 3:
                return done("tas", size=1, dst=check(ea(r, mode, reg, 1), *DATA_ALTERABLE))
            return done("tst", size=1 << ss, src=check(ea(r, mode, reg, 1 << ss), *DATA_ALTERABLE))
        if sub == 0xE:
            if op & 0xFFF0 == 0x4E40:
                return done("trap", extra={"vector": op & 15})
            if op & 0xFFF8 == 0x4E50:
                return done("link", src=Ea("an", reg), dst=Ea("imm", value=sext(r.word(), 16), ext=(2, 2)))
            if op & 0xFFF8 == 0x4E58:
                return done("unlk", dst=Ea("an", reg))
            if op & 0xFFF8 == 0x4E60:
                return done("move_to_usp", src=Ea("an", reg))
            if op & 0xFFF8 == 0x4E68:
                return done("move_from_usp", dst=Ea("an", reg))
            simple = {0x4E70: "reset", 0x4E71: "nop", 0x4E73: "rte", 0x4E75: "rts", 0x4E76: "trapv", 0x4E77: "rtr"}
            if op in simple:
                return done(simple[op])
            if op == 0x4E72:
                return done("stop", src=imm(r, 2))
            if ss == 2:
                return done("jsr", src=check(ea(r, mode, reg, 4), *CONTROL))
            if ss == 3:
                return done("jmp", src=check(ea(r, mode, reg, 4), *CONTROL))
        raise DecodeError(f"unknown 0100 opcode {op:04x}")

    if top == 5:
        if ss == 3:
            cond = CONDITIONS[op >> 8 & 15]
            if mode == 1:
                target = (address + 2 + sext(r.word(), 16)) & 0xFFFFFFFF
                return done("db", cond=cond, dst=Ea("dn", reg), target=target)
            return done("s", size=1, cond=cond, dst=check(ea(r, mode, reg, 1), *DATA_ALTERABLE))
        quick = reg9 or 8
        size = 1 << ss
        dst = check(ea(r, mode, reg, size), *ALTERABLE)
        if dst.mode == "an" and size == 1:
            raise DecodeError("addq.b to an")
        return done("subq" if op & 0x100 else "addq", size=size, src=Ea("imm", value=quick), dst=dst)

    if top == 6:
        cond = op >> 8 & 15
        d = sext(op, 8)
        if d == 0:
            d = sext(r.word(), 16)
        elif op & 0xFF == 0xFF:
            raise DecodeError("68020 long branch")
        target = (address + 2 + d) & 0xFFFFFFFF
        if cond == 0:
            return done("bra", target=target)
        if cond == 1:
            return done("bsr", target=target)
        return done("b", cond=CONDITIONS[cond], target=target)

    if top == 7:
        if op & 0x100:
            raise DecodeError("bad moveq")
        return done("moveq", size=4, src=Ea("imm", value=sext(op, 8) & 0xFFFFFFFF), dst=Ea("dn", reg9))

    if top in (8, 0xC):
        if ss == 3:
            name = ("divu" if top == 8 else "mulu") if not op & 0x100 else ("divs" if top == 8 else "muls")
            return done(name, size=2, src=check(ea(r, mode, reg, 2), *DATA), dst=Ea("dn", reg9))
        if op & 0x1F0 == 0x100:
            name = "sbcd" if top == 8 else "abcd"
            if op & 8:
                return done(name, size=1, src=Ea("predec", reg), dst=Ea("predec", reg9))
            return done(name, size=1, src=Ea("dn", reg), dst=Ea("dn", reg9))
        if top == 0xC and op & 0x130 == 0x100:
            opm = op >> 3 & 0x1F
            if opm == 0x08:
                return done("exg", src=Ea("dn", reg9), dst=Ea("dn", reg))
            if opm == 0x09:
                return done("exg", src=Ea("an", reg9), dst=Ea("an", reg))
            if opm == 0x11:
                return done("exg", src=Ea("dn", reg9), dst=Ea("an", reg))
            raise DecodeError("bad exg")
        name = "or" if top == 8 else "and"
        size = 1 << ss
        if op & 0x100:
            return done(name, size=size, src=Ea("dn", reg9), dst=check(ea(r, mode, reg, size), *MEMORY_ALTERABLE))
        return done(name, size=size, src=check(ea(r, mode, reg, size), *DATA), dst=Ea("dn", reg9))

    if top in (9, 0xD):
        base = "sub" if top == 9 else "add"
        if ss == 3:
            size = 4 if op & 0x100 else 2
            return done(base + "a", size=size, src=check(ea(r, mode, reg, size), *ALL), dst=Ea("an", reg9))
        size = 1 << ss
        if op & 0x100:
            if mode in (0, 1):
                name = base + "x"
                if mode:
                    return done(name, size=size, src=Ea("predec", reg), dst=Ea("predec", reg9))
                return done(name, size=size, src=Ea("dn", reg), dst=Ea("dn", reg9))
            return done(base, size=size, src=Ea("dn", reg9), dst=check(ea(r, mode, reg, size), *MEMORY_ALTERABLE))
        src = check(ea(r, mode, reg, size), *ALL)
        if src.mode == "an" and size == 1:
            raise DecodeError("byte op from an")
        return done(base, size=size, src=src, dst=Ea("dn", reg9))

    if top == 0xB:
        if ss == 3:
            size = 4 if op & 0x100 else 2
            return done("cmpa", size=size, src=check(ea(r, mode, reg, size), *ALL), dst=Ea("an", reg9))
        size = 1 << ss
        if op & 0x100:
            if mode == 1:
                return done("cmpm", size=size, src=Ea("postinc", reg), dst=Ea("postinc", reg9))
            return done("eor", size=size, src=Ea("dn", reg9), dst=check(ea(r, mode, reg, size), *DATA_ALTERABLE))
        src = check(ea(r, mode, reg, size), *ALL)
        if src.mode == "an" and size == 1:
            raise DecodeError("cmp.b from an")
        return done("cmp", size=size, src=src, dst=Ea("dn", reg9))

    if top == 0xE:
        kinds = ["as", "ls", "rox", "ro"]
        direction = "l" if op & 0x100 else "r"
        if ss == 3:
            if op & 0x800:
                raise DecodeError("68020 bitfield")
            name = kinds[op >> 9 & 3] + direction
            return done(name, size=2, src=Ea("imm", value=1), dst=check(ea(r, mode, reg, 2), *MEMORY_ALTERABLE))
        name = kinds[op >> 3 & 3] + direction
        count = Ea("dn", reg9) if op & 0x20 else Ea("imm", value=reg9 or 8)
        return done(name, size=1 << ss, src=count, dst=Ea("dn", reg))

    raise DecodeError(f"line {top:x} opcode {op:04x}")


def operand_reads_memory(e: Ea | None) -> bool:
    return e is not None and e.is_memory()
