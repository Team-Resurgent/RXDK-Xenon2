#!/usr/bin/env python3
"""Report on the 68000 analysis (functions, indirect jumps, run-time
operands), checking the decoder against capstone: python tools/analyze.py [--capstone]"""
import sys
from collections import Counter
from pathlib import Path
sys.path.insert(0, str(Path(__file__).parent))
from m68k.analyze import analyze, load_config

ROOT = Path(__file__).resolve().parents[1]
config = load_config(ROOT / "config/xenon2.json")
import json
relocs = [a for h in json.loads((ROOT / "build/hunks.json").read_text())["hunks"] for a in h.get("reloc_at", [])]
sets = analyze(bytearray((ROOT / "build/image.bin").read_bytes()), ROOT / "game/data", config, relocs)
md = None
if "--capstone" in sys.argv:
    import capstone
    md = capstone.Cs(capstone.CS_ARCH_M68K, capstone.CS_MODE_BIG_ENDIAN | capstone.CS_MODE_M68K_000)
for cs in sets:
    print(f"== {cs.name}: {len(cs.functions)} functions, {len(cs.insns)} instructions, "
          f"{len(cs.covered)} code bytes, {len(cs.dynamic)} run-time operand bytes, {cs.speculated} pointer entries kept")
    for f in cs.functions.values():
        if f.error:
            print(f"  error in ${f.entry:06x}: {f.error}")
    ops = Counter(i.op for i in cs.insns.values())
    print("  ops:", " ".join(f"{k}:{v}" for k, v in sorted(ops.items(), key=lambda x: -x[1])[:30]))
    for a, i in sorted(cs.insns.items()):
        if i.op in ("jsr", "jmp") and i.src.mode not in ("absw", "absl", "pcdisp"):
            print(f"  indirect ${a:06x} {i.text()}")
    for a in sorted({i.address for i in cs.insns.values() for b in range(i.address, i.address + i.length) if b in cs.dynamic}):
        print(f"  run-time operand ${a:06x} {cs.insns[a].text()}")
    if md:
        bad = 0
        for a, i in sorted(cs.insns.items()):
            if i.op == "hook" or a in config["patch_by_address"]:
                continue
            c = next(md.disasm(bytes(cs.memory[a:a + 12]), a), None)
            if c is None or c.size != i.length:
                bad += 1
                if bad < 10:
                    print(f"  MISMATCH ${a:06x}: {i.text()} / {c.mnemonic + ' ' + c.op_str if c else None}")
        print(f"  capstone length mismatches: {bad}")
