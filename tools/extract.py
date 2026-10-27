#!/usr/bin/env python3
"""Unpack the user's Xenon 2 CDTV WHDLoad install into game/.

  python tools/extract.py <Xenon2_v1.7_CDTV.lha | extracted folder>

game/data/       the CDTV files (X2, Level1-5, Sounds00-09, Speech00-66, ...)
build/image.bin  memory image: X2's hunks relocated at $400, the way the
                 WHDLoad slave's resload_Relocate places them
build/hunks.json where each hunk landed (code/data/bss, address, size)
"""
from __future__ import annotations

import json
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
LOAD = 0x400            # the slave loads X2 here (Xenon2CDTV.Slave)
MEMORY = 0x100000       # 1 MB: the CDTV's chip memory

HUNK_HEADER, HUNK_CODE, HUNK_DATA, HUNK_BSS = 0x3F3, 0x3E9, 0x3EA, 0x3EB
HUNK_RELOC32, HUNK_SYMBOL, HUNK_DEBUG, HUNK_END = 0x3EC, 0x3F0, 0x3F1, 0x3F2
SEVEN_ZIP = [Path(r"C:\Program Files\7-Zip\7z.exe"), Path(r"C:\Program Files (x86)\7-Zip\7z.exe")]


def unpack(source: Path, into: Path) -> Path:
    """The folder holding X2: the given folder, or the archive unpacked."""
    if source.is_dir():
        found = next(source.rglob("X2"), None)
    else:
        tool = shutil.which("7z") or next((str(p) for p in SEVEN_ZIP if p.exists()), None)
        if not tool:
            raise SystemExit("extract: 7-Zip is needed to open .lha archives (or pass the unpacked folder)")
        subprocess.run([tool, "x", "-y", f"-o{into}", str(source)], check=True, stdout=subprocess.DEVNULL)
        found = next(into.rglob("X2"), None)
    if not found:
        raise SystemExit(f"extract: no data/X2 in {source}: is this the CDTV install (Xenon2_v1.7_CDTV)?")
    return found.parent


def load_hunks(exe: bytes) -> tuple[bytearray, list[dict]]:
    words = lambda at, n: struct.unpack_from(f">{n}I", exe, at)
    at = 0
    (magic,) = words(at, 1)
    if magic != HUNK_HEADER:
        raise SystemExit("extract: X2 is not an AmigaDOS executable")
    at += 4
    while words(at, 1)[0]:                       # resident library names (none)
        at += 4 + 4 * words(at, 1)[0]
    at += 4
    count, first, last = words(at, 3)
    at += 12
    sizes = [s & 0x3FFFFFFF for s in words(at, last - first + 1)]
    at += 4 * len(sizes)

    # One after another from LOAD, 8-byte aligned.
    hunks, base = [], LOAD
    for size in sizes:
        hunks.append({"address": base, "size": size * 4})
        base = (base + size * 4 + 7) & ~7
    memory = bytearray(MEMORY)
    if base > MEMORY:
        raise SystemExit("extract: X2 does not fit in 1 MB")

    index = 0
    while at < len(exe) and index < len(hunks):
        (kind,) = words(at, 1)
        kind &= 0x3FFFFFFF
        at += 4
        hunk = hunks[index]
        if kind in (HUNK_CODE, HUNK_DATA):
            (n,) = words(at, 1)
            at += 4
            memory[hunk["address"]:hunk["address"] + n * 4] = exe[at:at + n * 4]
            hunk["kind"] = "code" if kind == HUNK_CODE else "data"
            hunk["loaded"] = n * 4
            at += n * 4
        elif kind == HUNK_BSS:
            at += 4
            hunk["kind"], hunk["loaded"] = "bss", 0
        elif kind == HUNK_RELOC32:
            relocs = 0
            while True:
                (n,) = words(at, 1)
                at += 4
                if not n:
                    break
                (target,) = words(at, 1)
                for offset in words(at + 4, n):
                    where = hunk["address"] + offset
                    hunk.setdefault("reloc_at", []).append(where)
                    value = struct.unpack_from(">I", memory, where)[0] + hunks[target]["address"]
                    struct.pack_into(">I", memory, where, value)
                relocs += n
                at += 4 + 4 * n
            hunk["relocs"] = hunk.get("relocs", 0) + relocs
        elif kind in (HUNK_SYMBOL,):
            while words(at, 1)[0]:
                at += 4 + 4 * words(at, 1)[0] + 4
            at += 4
        elif kind == HUNK_DEBUG:
            at += 4 + 4 * words(at, 1)[0]
        elif kind == HUNK_END:
            index += 1
        else:
            raise SystemExit(f"extract: unknown hunk type {kind:#x} at {at - 4:#x}")
    return memory, hunks


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__)
        return 1
    with tempfile.TemporaryDirectory() as tmp:
        data = unpack(Path(sys.argv[1]), Path(tmp))
        out = ROOT / "game/data"
        if out.exists():
            shutil.rmtree(out)
        shutil.copytree(data, out)
    memory, hunks = load_hunks((out / "X2").read_bytes())
    build = ROOT / "build"
    build.mkdir(exist_ok=True)
    (build / "image.bin").write_bytes(memory)
    (build / "hunks.json").write_text(json.dumps({"load": LOAD, "hunks": hunks}, indent=2) + "\n")
    for i, h in enumerate(hunks):
        print(f"extract: hunk {i} {h.get('kind', '?'):4} ${h['address']:06X}-${h['address'] + h['size']:06X}"
              f" loaded {h.get('loaded', 0):#x} relocs {h.get('relocs', 0)}")
    print(f"extract: {len(list(out.iterdir()))} files in game/data")
    return 0


if __name__ == "__main__":
    sys.exit(main())
