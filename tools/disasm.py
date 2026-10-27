#!/usr/bin/env python3
"""Disassemble build/image.bin: python tools/disasm.py <start hex> [count]"""
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).parent))
from m68k.decode import decode, DecodeError
mem = (Path(__file__).resolve().parents[1] / "build/image.bin").read_bytes()
a, n = int(sys.argv[1], 16), int(sys.argv[2]) if len(sys.argv) > 2 else 20
for _ in range(n):
    try:
        i = decode(mem, a)
    except DecodeError as e:
        print(f"{a:06x}  ?? {e}"); a += 2; continue
    print(f"{a:06x}  {mem[a:a+i.length].hex():20} {i.text()}")
    a += i.length
