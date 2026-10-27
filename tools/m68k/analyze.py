"""Find the functions of the game's 68000 code.

The game is the main program (X2 at $400) plus overlays: Intro, Piccode and
Level1-5 are GEMDOS-format files the game loads into a slot inside its code
hunk ($4E26) and relocates by $406. Each is a code set of its own; a call
from the main program into the slot goes to whichever overlay is loaded.

A function is everything reachable from an entry address through branches
(calls start new functions; code shared by two functions is generated in
both). Entries: the program start, interrupt vectors the game installs,
call targets, native hook continuations, and config/xenon2.json "entries"
(indirect targets the runtime reported).
"""
from __future__ import annotations

import json
import struct
from dataclasses import dataclass, field
from pathlib import Path

from .decode import DecodeError, Insn, decode

VECTORS = range(0x64, 0xC0, 4)      # autovectors 1-7 and the traps
# Operands the game fills in at run time carry these placeholder values.
PLACEHOLDERS = {0xAAAAAAAA, 0xAAAA, 0xAA}


@dataclass
class Function:
    entry: int
    insns: dict[int, Insn] = field(default_factory=dict)
    labels: set[int] = field(default_factory=set)      # branch targets inside
    resumes: set[int] = field(default_factory=set)     # return addresses after calls
    error: str = ""
    speculative: bool = False


@dataclass
class CodeSet:
    name: str                       # "main" or the overlay's file name
    memory: bytearray
    regions: list[tuple[int, int]]  # where this set's code lives
    functions: dict[int, Function] = field(default_factory=dict)
    insns: dict[int, Insn] = field(default_factory=dict)
    covered: set[int] = field(default_factory=set)      # bytes compiled as code
    dynamic: set[int] = field(default_factory=set)      # operand bytes read at run time

    def owns(self, address: int) -> bool:
        return any(a <= address < b for a, b in self.regions)


def static_target(insn: Insn) -> int | None:
    """The address a jsr/jmp goes to when it is known at compile time."""
    e = insn.src
    if e.mode in ("absw", "absl", "pcdisp"):
        return e.value & 0xFFFFFF
    return None


def successors(insn: Insn) -> tuple[list[int], list[int]]:
    """(addresses inside this function, called entries)."""
    nxt = insn.address + insn.length
    op = insn.op
    if op == "bra":
        return [insn.target], []
    if op in ("b", "db"):
        return [insn.target, nxt], []
    if op == "bsr":
        return [nxt], [insn.target]
    if op == "jsr":
        t = static_target(insn)
        return [nxt], [t] if t is not None else []
    if op == "jmp":
        t = static_target(insn)
        return [], [t] if t is not None else []      # a tail call
    if op == "hook":
        resume = insn.extra.get("resume")
        return ([resume] if resume is not None else []), []
    if op in ("rts", "rte", "rtr", "illegal"):
        return [], []
    return [nxt], []


def decode_patched(config: dict, memory: bytes, address: int) -> Insn:
    """The instruction at address, with the WHDLoad slave's patches applied."""
    patch = config["patch_by_address"].get(address)
    if patch:
        kind = patch["kind"]
        if kind == "skip":
            return Insn(address, 2, "bra", target=patch["to"])
        if kind == "rts":
            return Insn(address, 2, "rts")
        if kind == "hook":       # native code, then on at patch["resume"] (or rts)
            return Insn(address, 6, "hook", extra=patch)
    return decode(memory, address)


def jump_table(cs: CodeSet, insn: Insn) -> list[int]:
    """Targets of  move.w T(pc,dN.w),dN / jmp T(pc,dN.w): word offsets from T,
    the table ending where the first target starts."""
    try:
        prev = decode(cs.memory, insn.address - 4)
    except DecodeError:
        return []
    e = insn.src
    if not (prev.op == "move" and prev.size == 2 and prev.src.mode == "pcindex" and prev.src.value == e.value
            and prev.dst.mode == "dn" and prev.dst.reg == e.index[1] and not e.index[0]):
        return []
    base, targets, at = e.value, [], e.value
    end = 1 << 24
    while at < end and len(targets) < 256:
        (off,) = struct.unpack_from(">h", cs.memory, at)
        t = (base + off) & 0xFFFFFF
        if t & 1 or not cs.owns(t) or off <= 0:
            break
        targets.append(t)
        end = min(end, t)
        at += 2
    return sorted(set(targets))


def walk(config: dict, cs: CodeSet, entry: int) -> tuple[Function, list[int]]:
    f = Function(entry)
    todo, calls = [entry], []
    while todo:
        a = todo.pop()
        if a in f.insns:
            continue
        if not cs.owns(a) or a & 1:
            f.error = f"flow leaves the code set at ${a:06x}"
            continue
        try:
            insn = cs.insns.get(a) or decode_patched(config, cs.memory, a)
        except DecodeError as e:
            f.error = f"${a:06x}: {e}"
            continue
        cs.insns[a] = insn
        f.insns[a] = insn
        inside, called = successors(insn)
        if insn.op in ("b", "db", "bra"):
            f.labels.add(insn.target)
        if insn.op == "hook" and insn.extra.get("resume") is not None:
            f.labels.add(insn.extra["resume"])
        if insn.op == "jmp" and called and called[0] in f.insns:
            f.labels.add(called[0])
        if insn.op == "jmp" and insn.src.mode == "pcindex":
            table = jump_table(cs, insn)
            if table:
                insn.extra["table"] = table
                f.labels.update(table)
                inside = inside + table
        if insn.op in ("bsr", "jsr"):
            f.resumes.add(a + insn.length)
        calls += called
        todo += inside
    return f, calls


def installed_vectors(cs: CodeSet) -> list[int]:
    """Handlers stored into the exception vectors: move.l #handler,$6c and friends."""
    found = []
    for insn in cs.insns.values():
        if insn.op == "move" and insn.size == 4 and insn.src.mode == "imm" and insn.dst and \
                insn.dst.mode in ("absw", "absl") and insn.dst.value & 0xFFFFFF in VECTORS:
            found.append(insn.src.value & 0xFFFFFF)
    return found


def mark_dynamic(cs: CodeSet, config_dynamic: set[int]) -> None:
    """Operand bytes to read from memory at run time: placeholders, config list,
    and anything an absolute-addressed write stores into."""
    written = set()
    for insn in cs.insns.values():
        dst = insn.dst
        if dst is not None and dst.mode in ("absw", "absl") and insn.op not in ("cmp", "cmpi", "tst", "btst"):
            a = dst.value & 0xFFFFFF
            written.update(range(a, a + max(insn.size, 1)))
    for insn in cs.insns.values():
        for e in (insn.src, insn.dst):
            if e is None or e.ext is None:
                continue
            start = insn.address + e.ext[0]
            span = set(range(start, start + e.ext[1]))
            value = e.value if e.mode in ("absw", "absl", "imm") else None
            if span & (written | config_dynamic) or value in PLACEHOLDERS:
                cs.dynamic.update(span)


def load_config(path: Path) -> dict:
    config = json.loads(path.read_text())
    num = lambda v: int(v, 16) if isinstance(v, str) else v
    by = {}
    for p in config.get("patches", []):
        p = dict(p)
        for k in ("at", "to", "resume", "buffer", "pointer"):
            if k in p:
                p[k] = num(p[k])
        by[p["at"]] = p
    config["patch_by_address"] = by
    config["entry_list"] = [num(e) for e in config.get("entries", [])]
    config["code_range"] = tuple(num(v) for v in config["code"])
    ov = config["overlays"]
    config["overlay_range"] = (num(ov["at"]), num(ov["end"]))
    config["overlay_base"] = num(ov["base"])
    config["dynamic_set"] = {num(v) for v in config.get("dynamic", [])}
    config["overlay_entries"] = {k: [num(v) for v in vs] for k, vs in config.get("overlay_entries", {}).items()}
    return config


def load_overlay(memory: bytearray, data: bytes, at: int, base: int) -> tuple[int, list[int]]:
    """Copy a GEMDOS overlay to at and relocate it by base, the way the game
    does it ($3D2EE): returns the end of its text and the relocated longs."""
    memory[at:at + len(data)] = data
    text_size = struct.unpack_from(">I", data, 2)[0]
    text = at + 0x1C
    r = 0x1C + text_size + struct.unpack_from(">I", data, 6)[0] + struct.unpack_from(">I", data, 14)[0]
    a = text + struct.unpack_from(">I", data, r)[0]
    r += 4
    relocs = []
    while True:
        relocs.append(a)
        struct.pack_into(">I", memory, a, (struct.unpack_from(">I", memory, a)[0] + base) & 0xFFFFFFFF)
        while True:
            b = data[r]
            r += 1
            if b != 1:
                break
            a += 254
        if b == 0:
            break
        a += b
    return text + text_size, relocs


def analyze_set(config: dict, cs: CodeSet, entries: list[int]) -> list[int]:
    """Walk cs from entries; returns calls that leave it."""
    outside = []
    while entries:
        pending, entries = entries, []
        for e in pending:
            if not cs.owns(e):
                outside.append(e)
                continue
            if e in cs.functions:
                continue
            f, calls = walk(config, cs, e)
            cs.functions[e] = f
            entries += calls
        if not entries:      # handlers the game installs, once everything is decoded
            entries = [v for v in installed_vectors(cs) if v not in cs.functions and cs.owns(v)]
    return outside


def plausible(f: Function) -> bool:
    """A speculative entry is kept when it decodes cleanly and has no
    ori.b #0,d0 (zero words: data, not code)."""
    if f.error:
        return False
    return not any(i.op == "ori" and i.size == 1 and i.dst.mode == "dn" and i.src.value == 0 and
                   i.dst.reg == 0 for i in f.insns.values())


def speculate(config: dict, cs: CodeSet, pointers: list[int]) -> int:
    """Walk relocated pointers into the set's code; keep plausible functions
    (and what they call). Returns how many were kept."""
    kept = 0
    for p in sorted(set(pointers)):
        if p & 1 or p in cs.functions or not cs.owns(p):
            continue
        trial = CodeSet(cs.name, cs.memory, cs.regions)
        trial.functions = dict(cs.functions)
        trial.insns = dict(cs.insns)
        before = set(trial.functions)
        analyze_set(config, trial, [p])
        new = [trial.functions[e] for e in set(trial.functions) - before]
        if new and all(plausible(f) for f in new):
            cs.functions, cs.insns = trial.functions, trial.insns
            for f in new:
                f.speculative = True
            kept += 1
    return kept


def jump_vectors(cs: CodeSet) -> list[int]:
    """Tables of jmp abs.l slots (4EF9 xxxxxxxx, three or more in a row): the
    services the overlays call through registers, like the one at $E60."""
    found = []
    for lo, hi in cs.regions:
        a = lo & ~1
        while a + 6 <= hi:
            run = a
            while run + 6 <= hi and cs.memory[run:run + 2] == b"\x4e\xf9" and \
                    cs.owns(struct.unpack_from(">I", cs.memory, run + 2)[0] & 0xFFFFFF):
                run += 6
            if (run - a) // 6 >= 3:
                found += list(range(a, run, 6))
                a = run
            else:
                a += 2
    return found


def analyze(image: bytearray, data_dir: Path, config: dict, relocs: list[int]) -> list[CodeSet]:
    lo, hi = config["code_range"]
    ov_lo, ov_hi = config["overlay_range"]
    main = CodeSet("main", image, [(lo, ov_lo), (ov_hi, hi)])
    main.pointers = [struct.unpack_from(">I", image, a)[0] & 0xFFFFFF for a in relocs]
    sets = [main]
    for name in config["overlays"]["files"]:
        memory = bytearray(image)
        end, orelocs = load_overlay(memory, (data_dir / name).read_bytes(), ov_lo, config["overlay_base"])
        cs = CodeSet(name, memory, [(ov_lo + 0x1C, end)])
        cs.pointers = [struct.unpack_from(">I", memory, a)[0] & 0xFFFFFF for a in orelocs]
        sets.append(cs)

    main_entries = list(config["entry_list"]) + jump_vectors(main)
    main_entries += [p["resume"] for p in config["patch_by_address"].values() if p.get("resume") is not None]
    into_overlays: set[int] = set()
    while True:
        calls = analyze_set(config, main, main_entries)
        into_overlays.update(c for c in calls if ov_lo <= c < ov_hi)
        main_entries = []
        for cs in sets[1:]:
            entries = sorted(into_overlays) + config["overlay_entries"].get(cs.name, []) + jump_vectors(cs)
            entries = [e for e in entries if cs.owns(e)]
            back = analyze_set(config, cs, entries)
            main_entries += [c for c in back if main.owns(c) and c not in main.functions]
        if not main_entries:
            break
    for cs in sets:
        cs.speculated = speculate(config, cs, cs.pointers + (main.pointers if cs is not main else []))
    for cs in sets:
        for insn in cs.insns.values():
            cs.covered.update(range(insn.address, insn.address + insn.length))
        mark_dynamic(cs, config["dynamic_set"])
        cs.covered -= cs.dynamic
    return sets
