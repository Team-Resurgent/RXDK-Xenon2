"""68000 -> C for one code set (see analyze.py).

Every function becomes  void NAME(u32 at): at == 0 enters at the top, any
other value resumes at that return address (after the C stack was unwound;
see src/runtime/m68k.h). Registers, flags and memory are the runtime's
(R, rb/rw/rl, wb/ww/wl); calls push the real 68000 return address, and a
call whose callee returned somewhere else (stack tricks) unwinds.
"""
from __future__ import annotations

from .analyze import CodeSet, Function, static_target
from .decode import Ea, Insn

U = {1: "u8", 2: "u16", 4: "u32"}
S = {1: "s8", 2: "s16", 4: "s32"}
MASK = {1: "0xFFu", 2: "0xFFFFu", 4: "0xFFFFFFFFu"}
BITS = {1: 8, 2: 16, 4: 32}

COND = {
    "t": "1", "f": "0", "hi": "(!R.c && !R.z)", "ls": "(R.c || R.z)", "cc": "!R.c", "cs": "R.c",
    "ne": "!R.z", "eq": "R.z", "vc": "!R.v", "vs": "R.v", "pl": "!R.n", "mi": "R.n",
    "ge": "(R.n == R.v)", "lt": "(R.n != R.v)", "gt": "(!R.z && R.n == R.v)", "le": "(R.z || R.n != R.v)",
}


class Unsupported(Exception):
    pass


def cycles(insn: Insn) -> int:
    """A rough 68000 timing: 4 per word fetched, 4 per bus access, extra for
    the slow instructions. Only the frame's busy waits depend on it."""
    n = 2 * insn.length
    for e in (insn.src, insn.dst):
        if e is not None and e.is_memory():
            n += 4 * max(1, insn.size // 2)
    op = insn.op
    if op in ("mulu", "muls"):
        n += 38
    elif op in ("divu", "divs"):
        n += 120
    elif op == "movem":
        n += 4 * bin(insn.mask).count("1") * max(1, insn.size // 2)
    elif op in ("asl", "asr", "lsl", "lsr", "rol", "ror", "roxl", "roxr") and insn.src.mode == "imm":
        n += 2 * insn.src.value
    elif op in ("bsr", "jsr", "rts", "rte"):
        n += 8
    return n


class Gen:
    def __init__(self, cs: CodeSet, sets: list[CodeSet], names: dict[str, str], overlay_range: tuple[int, int]):
        self.cs, self.sets, self.names, self.ov = cs, sets, names, overlay_range
        self.main = sets[0]
        self.out: list[str] = []
        self.tmp = 0

    # ---- names -----------------------------------------------------------
    def fname(self, cs: CodeSet, address: int) -> str:
        return f"{self.names[cs.name]}_{address:06x}"

    def call_expr(self, target: int, at: str = "0") -> str:
        """C call of a static target."""
        if self.cs.owns(target) and target in self.cs.functions:
            return f"{self.fname(self.cs, target)}({at})"
        if self.main.owns(target) and target in self.main.functions:
            return f"{self.fname(self.main, target)}({at})"
        return f"rt_call(0x{target:06x})"

    def new(self) -> str:
        self.tmp += 1
        return f"t{self.tmp}"

    def emit(self, line: str) -> None:
        self.out.append(line)

    # ---- operands --------------------------------------------------------
    def dyn(self, insn: Insn, e: Ea) -> bool:
        if e.ext is None:
            return False
        start = insn.address + e.ext[0]
        return any(b in self.cs.dynamic for b in range(start, start + e.ext[1]))

    def ext_read(self, insn: Insn, e: Ea, size: int) -> str:
        a = insn.address + e.ext[0]
        return {1: f"rb(0x{a:06x})", 2: f"rw(0x{a:06x})", 4: f"rl(0x{a:06x})"}[size]

    def address(self, insn: Insn, e: Ea, size: int) -> str:
        """Statements computing e's address into a new temporary; returns its name."""
        t = self.new()
        dyn = self.dyn(insn, e)
        step = 2 if size == 1 and e.reg == 7 else size
        m = e.mode
        if m == "ind":
            self.emit(f"u32 {t} = R.a[{e.reg}];")
        elif m == "postinc":
            self.emit(f"u32 {t} = R.a[{e.reg}]; R.a[{e.reg}] += {step};")
        elif m == "predec":
            self.emit(f"R.a[{e.reg}] -= {step}; u32 {t} = R.a[{e.reg}];")
        elif m == "disp":
            d = f"(u32)(s32)(s16){self.ext_read(insn, e, 2)}" if dyn else f"{e.disp & 0xFFFFFFFF:#x}u"
            self.emit(f"u32 {t} = R.a[{e.reg}] + {d};")
        elif m in ("index", "pcindex"):
            if dyn:
                raise Unsupported("run-time index extension")
            ia, ir, il = e.index
            idx = f"R.{'a' if ia else 'd'}[{ir}]" if il else f"(u32)(s32)(s16)R.{'a' if ia else 'd'}[{ir}]"
            base = f"R.a[{e.reg}] + {e.disp & 0xFFFFFFFF:#x}u" if m == "index" else f"0x{e.value & 0xFFFFFF:06x}u"
            self.emit(f"u32 {t} = {base} + {idx};")
        elif m in ("absw", "absl"):
            if dyn:
                v = f"(u32)(s32)(s16){self.ext_read(insn, e, 2)}" if m == "absw" else self.ext_read(insn, e, 4)
                self.emit(f"u32 {t} = {v};")
            else:
                self.emit(f"u32 {t} = 0x{e.value & 0xFFFFFFFF:x}u;")
        elif m == "pcdisp":
            if dyn:
                raise Unsupported("run-time pc displacement")
            self.emit(f"u32 {t} = 0x{e.value & 0xFFFFFF:06x}u;")
        else:
            raise Unsupported(f"address of {m}")
        return t

    def mem_read(self, addr: str, size: int) -> str:
        return {1: f"rb({addr})", 2: f"rw({addr})", 4: f"rl({addr})"}[size]

    def mem_write(self, addr: str, size: int, value: str) -> None:
        self.emit({1: f"wb({addr}, {value});", 2: f"ww({addr}, {value});", 4: f"wl({addr}, {value});"}[size])

    def read(self, insn: Insn, e: Ea, size: int) -> str:
        """An expression for e's value (zero-extended, size bytes). May emit statements."""
        if e.mode == "dn":
            return f"({U[size]})R.d[{e.reg}]" if size < 4 else f"R.d[{e.reg}]"
        if e.mode == "an":
            return f"({U[size]})R.a[{e.reg}]" if size < 4 else f"R.a[{e.reg}]"
        if e.mode == "imm":
            if self.dyn(insn, e):
                return self.ext_read(insn, e, e.ext[1] if e.ext[1] <= size else size)
            return f"0x{e.value & int(MASK[size][:-1], 16):x}u"
        t = self.new()
        a = self.address(insn, e, size)
        self.emit(f"u32 {t} = {self.mem_read(a, size)};")
        return t

    def write_reg(self, e: Ea, size: int, value: str) -> None:
        r = f"R.{'d' if e.mode == 'dn' else 'a'}[{e.reg}]"
        if size == 4:
            self.emit(f"{r} = {value};")
        else:
            m = MASK[size]
            self.emit(f"{r} = ({r} & ~{m}) | (({value}) & {m});")

    def rmw(self, insn: Insn, e: Ea, size: int) -> tuple[str, callable]:
        """(value expression, writer) for read-modify-write operands."""
        if e.mode in ("dn", "an"):
            return self.read(insn, e, size), lambda v: self.write_reg(e, size, v)
        a = self.address(insn, e, size)
        t = self.new()
        self.emit(f"u32 {t} = {self.mem_read(a, size)};")
        return t, lambda v: self.mem_write(a, size, v)

    def write(self, insn: Insn, e: Ea, size: int, value: str) -> None:
        if e.mode in ("dn", "an"):
            self.write_reg(e, size, value)
            return
        a = self.address(insn, e, size)
        self.mem_write(a, size, value)

    # ---- control ---------------------------------------------------------
    def label(self, address: int) -> str:
        return f"L_{address:06x}"

    def check(self, resume: int) -> None:
        """Let pending chipset events and interrupts run before going on at resume."""
        self.checks.add(resume)
        self.emit(f"if (R.cyc >= R.event && rt_events(0x{resume:06x})) return;")

    def goto(self, f: Function, insn: Insn, target: int) -> None:
        if target <= insn.address:
            self.check(target)
        self.emit(f"goto {self.label(target)};")

    def call(self, insn: Insn, target_expr: str, ret: int) -> None:
        self.emit(f"push32(0x{ret:06x}); {target_expr};")
        self.emit(f"if (R.pc != 0x{ret:06x}) return;")

    # ---- function --------------------------------------------------------
    def function(self, f: Function) -> None:
        name = self.fname(self.cs, f.entry)
        self.checks = set()
        self.emit(f"void {name}(u32 at)\n{{")
        switch_at = len(self.out)
        self.emit(f"    if (R.cyc >= R.event && rt_events(0x{f.entry:06x})) return;")
        order = sorted(f.insns)
        nxt_of = {a: f.insns[a].address + f.insns[a].length for a in order}
        for i, a in enumerate(order):
            insn = f.insns[a]
            if i == 0 and a != f.entry:
                self.emit(f"    goto {self.label(f.entry)};")
            self.emit(f"{self.label(a)}:")
            self.emit(f"    /* {a:06x}: {insn.text()} */")
            self.emit(f"    R.cyc += {cycles(insn)};")
            body_start = len(self.out)
            try:
                self.insn(f, insn)
            except Unsupported as e:
                del self.out[body_start:]
                self.emit(f"    rt_unsupported(0x{a:06x}, \"{e}\"); return;")
            self.out[body_start:] = ["    { " + " ".join(l.strip() for l in self.out[body_start:]) + " }"] \
                if len(self.out) > body_start else []
            # Falling out of the instruction into code that is not next in order.
            n = nxt_of[a]
            if self.falls_through(insn) and (i + 1 >= len(order) or order[i + 1] != n):
                if n in f.insns:
                    self.emit(f"    goto {self.label(n)};")
                else:
                    self.emit(f"    rt_fell_off(0x{n:06x}); return;")
        if f.error:
            self.emit(f"    /* analysis: {f.error} */")
        self.emit("}\n")
        # Resumable: after every call, and wherever events may run (after an
        # unwind, the code goes on there).
        f.resumes.update(a for a in self.checks if a in f.insns and a != f.entry)
        resumes = sorted(r for r in f.resumes if r in f.insns)
        if resumes:
            head = ["    if (at) switch (at) {"]
            head += [f"        case 0x{r:06x}: goto {self.label(r)};" for r in resumes]
            head += ["        default: rt_bad_resume(at); return;", "    }"]
            self.out[switch_at:switch_at] = head

    @staticmethod
    def falls_through(insn: Insn) -> bool:
        return insn.op not in ("bra", "jmp", "rts", "rte", "rtr", "illegal") and \
            not (insn.op == "hook")

    # ---- instructions ----------------------------------------------------
    def insn(self, f: Function, i: Insn) -> None:
        op, sz = i.op, i.size
        nxt = i.address + i.length
        e = self.emit

        if op == "bra":
            self.goto(f, i, i.target)
        elif op == "b":
            e(f"if ({COND[i.cond]}) {{")
            self.goto(f, i, i.target)
            e("}")
        elif op == "db":
            e(f"if (!{COND[i.cond]}) {{ u16 c = (u16)R.d[{i.dst.reg}] - 1; "
              f"R.d[{i.dst.reg}] = (R.d[{i.dst.reg}] & ~0xFFFFu) | c; if (c != 0xFFFF) {{")
            self.goto(f, i, i.target)
            e("} }")
        elif op == "bsr":
            self.call(i, self.call_expr(i.target), nxt)
        elif op in ("jsr", "jmp"):
            t = static_target(i)
            if op == "jmp" and t is not None and t in f.insns:
                self.goto(f, i, t)
            elif t is not None:
                if op == "jsr":
                    self.call(i, self.call_expr(t), nxt)
                else:
                    e(f"{self.call_expr(t)}; return;")
            else:
                a = self.address(i, i.src, 4)
                a24 = f"({a} & 0xFFFFFFu)"
                if op == "jsr":
                    self.call(i, f"rt_call({a24})", nxt)
                else:
                    table = i.extra.get("table", [])
                    if table:
                        e(f"switch ({a24}) {{")
                        for t in table:
                            e(f"case 0x{t:06x}: goto {self.label(t)};")
                        e("}")
                    e(f"rt_call({a24}); return;")
        elif op == "rts":
            e("R.pc = pop32(); return;")
        elif op == "rte":
            e("{ u16 sr = pop16(); R.pc = pop32(); rt_set_sr(sr); } return;")
        elif op == "rtr":
            e("{ u16 cc = pop16(); R.pc = pop32(); rt_set_ccr(cc); } return;")
        elif op == "hook":
            p = i.extra
            if p["name"] == "load_file":
                dest = f"0x{p['buffer']:x}u" if "buffer" in p else f"rl(0x{p['pointer']:x})"
                e(f"hook_load_file({dest});")
            else:
                e(f"hook_{p['name']}();")
            if p.get("resume") is not None:
                self.goto(f, i, p["resume"])
            else:
                e("R.pc = pop32(); return;")
        elif op == "trap":
            e(f"if (rt_exception({32 + i.extra['vector']}, 0x{nxt:06x})) return;")
        elif op == "trapv":
            e(f"if (R.v && rt_exception(7, 0x{nxt:06x})) return;")
        elif op == "illegal":
            e(f"rt_illegal(0x{i.address:06x}); return;")
        elif op == "stop":
            e(f"rt_set_sr({self.read(i, i.src, 2)}); if (rt_stop(0x{nxt:06x})) return;")
        elif op in ("nop", "reset"):
            pass
        elif op in ("move", "movea"):
            v = self.read(i, i.src, sz)
            if op == "movea":
                val = f"(u32)(s32)(s16)({v})" if sz == 2 else v
                e(f"R.a[{i.dst.reg}] = {val};")
            else:
                t = self.new()
                e(f"u32 {t} = {v};")
                self.write(i, i.dst, sz, t)
                e(f"logic{BITS[sz]}({t});")
        elif op == "moveq":
            e(f"R.d[{i.dst.reg}] = 0x{i.src.value:x}u; logic32(R.d[{i.dst.reg}]);")
        elif op == "lea":
            a = self.address(i, i.src, 4)
            e(f"R.a[{i.dst.reg}] = {a};")
        elif op == "pea":
            a = self.address(i, i.src, 4)
            e(f"push32({a});")
        elif op == "clr":
            if i.dst.mode in ("dn",):
                self.write(i, i.dst, sz, "0")
            else:
                a = self.address(i, i.dst, sz)
                self.mem_write(a, sz, "0")
            e("R.n = 0; R.z = 1; R.v = 0; R.c = 0;")
        elif op in ("tst",):
            v = self.read(i, i.src, sz)
            e(f"logic{BITS[sz]}({v});")
        elif op in ("not",):
            v, w = self.rmw(i, i.dst, sz)
            t = self.new()
            e(f"u32 {t} = ~{v} & {MASK[sz]};")
            w(t)
            e(f"logic{BITS[sz]}({t});")
        elif op in ("neg", "negx"):
            v, w = self.rmw(i, i.dst, sz)
            t = self.new()
            e(f"u32 {t} = {op}{BITS[sz]}({v});")
            w(t)
        elif op in ("and", "or", "eor", "andi", "ori", "eori"):
            cop = {"and": "&", "or": "|", "eor": "^"}[op.rstrip("i")]
            s = self.read(i, i.src, sz)
            v, w = self.rmw(i, i.dst, sz)
            t = self.new()
            e(f"u32 {t} = ({v} {cop} {s}) & {MASK[sz]};")
            w(t)
            e(f"logic{BITS[sz]}({t});")
        elif op in ("andi_ccr", "ori_ccr", "eori_ccr"):
            cop = {"andi": "&", "ori": "|", "eori": "^"}[op[:-4]]
            e(f"rt_set_ccr(rt_get_sr() {cop} {self.read(i, i.src, 1)});")
        elif op in ("andi_sr", "ori_sr", "eori_sr"):
            cop = {"andi": "&", "ori": "|", "eori": "^"}[op[:-3]]
            e(f"rt_set_sr(rt_get_sr() {cop} {self.read(i, i.src, 2)});")
            self.check(nxt)
        elif op == "move_to_ccr":
            e(f"rt_set_ccr({self.read(i, i.src, 2)});")
        elif op == "move_to_sr":
            e(f"rt_set_sr({self.read(i, i.src, 2)});")
            self.check(nxt)
        elif op == "move_from_sr":
            self.write(i, i.dst, 2, "rt_get_sr()")
        elif op == "move_to_usp":
            e(f"R.usp = R.a[{i.src.reg}];")
        elif op == "move_from_usp":
            e(f"R.a[{i.dst.reg}] = R.usp;")
        elif op in ("add", "sub", "addi", "subi", "addq", "subq"):
            base = op.rstrip("iq")
            s = self.read(i, i.src, sz)
            if i.dst.mode == "an":      # addq/subq to an: whole register, no flags
                e(f"R.a[{i.dst.reg}] {'+' if base == 'add' else '-'}= {s};")
            else:
                v, w = self.rmw(i, i.dst, sz)
                t = self.new()
                e(f"u32 {t} = {base}{BITS[sz]}({v}, {s});")
                w(t)
        elif op in ("adda", "suba"):
            s = self.read(i, i.src, sz)
            val = f"(u32)(s32)(s16)({s})" if sz == 2 else s
            e(f"R.a[{i.dst.reg}] {'+' if op == 'adda' else '-'}= {val};")
        elif op in ("addx", "subx"):
            s = self.read(i, i.src, sz)
            v, w = self.rmw(i, i.dst, sz)
            t = self.new()
            e(f"u32 {t} = {op}{BITS[sz]}({v}, {s});")
            w(t)
        elif op in ("cmp", "cmpi", "cmpm"):
            s = self.read(i, i.src, sz)
            d = self.read(i, i.dst, sz)
            e(f"cmp{BITS[sz]}({d}, {s});")
        elif op == "cmpa":
            s = self.read(i, i.src, sz)
            val = f"(u32)(s32)(s16)({s})" if sz == 2 else s
            e(f"cmp32(R.a[{i.dst.reg}], {val});")
        elif op in ("mulu", "muls"):
            s = self.read(i, i.src, 2)
            r = f"R.d[{i.dst.reg}]"
            if op == "mulu":
                e(f"{r} = (u32)(u16){r} * (u32)(u16)({s}); logic32({r});")
            else:
                e(f"{r} = (u32)((s32)(s16){r} * (s32)(s16)({s})); logic32({r});")
        elif op in ("divu", "divs"):
            s = self.read(i, i.src, 2)
            e(f"if ({op}(&R.d[{i.dst.reg}], {s}) && rt_exception(5, 0x{nxt:06x})) return;")
        elif op in ("asl", "asr", "lsl", "lsr", "rol", "ror", "roxl", "roxr"):
            if i.dst.mode == "dn":
                cnt = f"{i.src.value}u" if i.src.mode == "imm" else f"(R.d[{i.src.reg}] & 63)"
                v = self.read(i, i.dst, sz)
                t = self.new()
                e(f"u32 {t} = {op}{BITS[sz]}({v}, {cnt});")
                self.write_reg(i.dst, sz, t)
            else:
                v, w = self.rmw(i, i.dst, 2)
                t = self.new()
                e(f"u32 {t} = {op}16({v}, 1);")
                w(t)
        elif op in ("btst", "bchg", "bclr", "bset"):
            if i.src.mode == "imm":
                bit = f"{i.src.value & (31 if i.dst.mode == 'dn' else 7)}u"
            else:
                bit = f"(R.d[{i.src.reg}] & {31 if i.dst.mode == 'dn' else 7})"
            size = 4 if i.dst.mode == "dn" else 1
            if op == "btst":
                v = self.read(i, i.dst, size)
                e(f"R.z = !(({v} >> {bit}) & 1);")
            else:
                v, w = self.rmw(i, i.dst, size)
                t = self.new()
                e(f"R.z = !(({v} >> {bit}) & 1);")
                expr = {"bchg": f"{v} ^ (1u << {bit})", "bclr": f"{v} & ~(1u << {bit})",
                        "bset": f"{v} | (1u << {bit})"}[op]
                e(f"u32 {t} = {expr};")
                w(t)
        elif op == "s":
            self.write(i, i.dst, 1, f"({COND[i.cond]} ? 0xFFu : 0u)")
        elif op == "swap":
            r = f"R.d[{i.dst.reg}]"
            e(f"{r} = ({r} >> 16) | ({r} << 16); logic32({r});")
        elif op == "ext":
            r = f"R.d[{i.dst.reg}]"
            if sz == 2:
                e(f"{r} = ({r} & 0xFFFF0000u) | (u16)(s16)(s8){r}; logic16((u16){r});")
            else:
                e(f"{r} = (u32)(s32)(s16){r}; logic32({r});")
        elif op == "exg":
            a = f"R.{'d' if i.src.mode == 'dn' else 'a'}[{i.src.reg}]"
            b = f"R.{'d' if i.dst.mode == 'dn' else 'a'}[{i.dst.reg}]"
            e(f"u32 t = {a}; {a} = {b}; {b} = t;")
        elif op == "link":
            e(f"push32(R.a[{i.src.reg}]); R.a[{i.src.reg}] = R.a[7]; R.a[7] += 0x{i.dst.value & 0xFFFFFFFF:x}u;")
        elif op == "unlk":
            e(f"R.a[7] = R.a[{i.dst.reg}]; R.a[{i.dst.reg}] = pop32();")
        elif op == "movem":
            self.movem(i)
        elif op == "tas":
            v, w = self.rmw(i, i.dst, 1)
            e(f"logic8({v});")
            w(f"{v} | 0x80u")
        elif op == "chk":
            s = self.read(i, i.src, 2)
            e(f"if (chk((s16)R.d[{i.dst.reg}], (s16){s}) && rt_exception(6, 0x{nxt:06x})) return;")
        elif op in ("abcd", "sbcd"):
            s = self.read(i, i.src, 1)
            v, w = self.rmw(i, i.dst, 1)
            t = self.new()
            e(f"u32 {t} = {op}({v}, {s});")
            w(t)
        elif op == "nbcd":
            v, w = self.rmw(i, i.dst, 1)
            t = self.new()
            e(f"u32 {t} = sbcd(0, {v});")
            w(t)
        elif op == "movep":
            if i.src.mode == "dn":
                a = self.address(i, i.dst, 1)
                r = f"R.d[{i.src.reg}]"
                shifts = [24, 16, 8, 0] if sz == 4 else [8, 0]
                for k, s in enumerate(shifts):
                    e(f"wb({a} + {2 * k}, (u8)({r} >> {s}));")
            else:
                a = self.address(i, i.src, 1)
                r = f"R.d[{i.dst.reg}]"
                if sz == 4:
                    e(f"{r} = rb({a}) << 24 | rb({a} + 2) << 16 | rb({a} + 4) << 8 | rb({a} + 6);")
                else:
                    e(f"{r} = ({r} & 0xFFFF0000u) | rb({a}) << 8 | rb({a} + 2);")
        else:
            raise Unsupported(f"op {op}")

    def movem(self, i: Insn) -> None:
        e = self.emit
        regs = [i for i in range(16)]
        sz = i.size
        names = [f"R.{'d' if r < 8 else 'a'}[{r & 7}]" for r in regs]
        chosen = [r for r in regs if i.mask >> r & 1]
        if i.extra["to_memory"]:
            d = i.dst
            if d.mode == "predec":
                a = self.new()
                e(f"u32 {a} = R.a[{d.reg}];")
                for r in reversed(chosen):
                    e(f"{a} -= {sz};")
                    self.mem_write(a, sz, f"({U[sz]}){names[r]}" if sz == 2 else names[r])
                e(f"R.a[{d.reg}] = {a};")
            else:
                a = self.address(i, d, sz)
                for r in chosen:
                    self.mem_write(a, sz, f"({U[sz]}){names[r]}" if sz == 2 else names[r])
                    e(f"{a} += {sz};")
        else:
            s = i.src
            if s.mode == "postinc":
                a = self.new()
                e(f"u32 {a} = R.a[{s.reg}];")
            else:
                a = self.address(i, s, sz)
            for r in chosen:
                v = self.mem_read(a, sz)
                e(f"{names[r]} = {'(u32)(s32)(s16)' + v if sz == 2 else v}; {a} += {sz};")
            if s.mode == "postinc":
                e(f"R.a[{s.reg}] = {a};")


def generate(cs: CodeSet, sets: list[CodeSet], names: dict[str, str], overlay_range) -> list[str]:
    """C for each function of cs, in address order."""
    g = Gen(cs, sets, names, overlay_range)
    chunks = []
    for entry in sorted(cs.functions):
        g.out = []
        g.tmp = 0
        g.function(cs.functions[entry])
        chunks.append("\n".join(g.out))
    return chunks
