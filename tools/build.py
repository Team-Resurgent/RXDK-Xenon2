#!/usr/bin/env python3
"""Build Xenon 2 for the original Xbox from your own CDTV install.

  python tools/build.py --install <Xenon2_v1.7_CDTV.lha | folder>   (once)
  python tools/build.py --music <Xenon2Audio_v0.2b_BETA.lha>         (once, for music)
  python tools/build.py

1. extract.py: game/data (your files) and build/image.bin (X2 at $400);
   music.py: game/music (the CD music)
2. recompile.py: the 68000 code as C++ in build/m68k
3. the RXDK projects (gamecode: the generated code; the title: the runtime)
   and the RXDK build: out/xenon2.xbe and out/XISO/xenon2.iso
"""
from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build"
FLAGS = ["-fno-strict-aliasing", "-Wno-unused-label", "-Wno-unused-variable", "-Wno-unused-but-set-variable",
         "-Wno-parentheses", "-Wno-unused-function"]
LIBRARIES = ["libd3d8.lib", "libxgraphics.lib", "libdsound.lib", "libxapi.lib", "libcpp.lib", "libc.lib",
             "libkernel.lib"]


def run(cmd: list, **kw) -> None:
    print("build:", " ".join(str(c) for c in cmd), flush=True)
    subprocess.run([str(c) for c in cmd], check=True, **kw)


def rxdk() -> Path:
    root = Path(os.environ.get("RXDK", r"C:\ProgramData\RXDK"))
    cli = os.environ.get("RXDK_CLI")
    for candidate in ([Path(cli)] if cli else [root / "tools/Rxdk.Cli.exe", root / "engine/Rxdk.Cli.exe"]):
        if candidate.exists():
            return candidate
    sys.exit("build: RXDK not found (install it, or set RXDK or RXDK_CLI)")


def rel(path: Path, base: Path) -> str:
    return Path(os.path.relpath(path, base)).as_posix()


def write_project(directory: Path, project: dict) -> None:
    directory.mkdir(parents=True, exist_ok=True)
    text = json.dumps(project, indent=2) + "\n"
    path = directory / "rxdk.project.json"
    if not path.exists() or path.read_text() != text:
        path.write_text(text)


def projects() -> None:
    generated = BUILD / "m68k"
    sources = json.loads((generated / "sources.json").read_text())
    gamecode = ROOT / "gamecode"
    write_project(gamecode, {
        "name": "gamecode", "type": "library", "configuration": "release",
        "sources": [rel(generated / s, gamecode) for s in sources],
        "includePaths": [rel(generated, gamecode), rel(ROOT / "src", gamecode)],
        "compileFlags": FLAGS + ["-O2"],
        "cppStandard": "c++20", "outputDir": "out"})
    runtime = sorted((ROOT / "src/runtime").glob("*.cpp")) + sorted((ROOT / "src/platform/xbox").glob("*.cpp"))
    write_project(ROOT, {
        "name": "xenon2", "configuration": "release",
        "sources": [rel(p, ROOT) for p in runtime],
        "includePaths": ["src", "build/m68k", "third_party"],
        "projectReferences": ["gamecode"],
        "libraries": LIBRARIES,
        "compileFlags": FLAGS,
        "cppStandard": "c++20", "outputDir": "out",
        "deployPaths": ["image.bin=build/image.bin", "data=game/data"]
        + (["music=game/music"] if (ROOT / "game/music").exists() else [])
        # A test input script (src/platform/xbox/main.cpp), only when there is one.
        + (["inputs.txt=build/inputs.txt"] if (BUILD / "inputs.txt").exists() else []),
        "imageBuild": {"testName": "Xenon 2 Megablast"}})


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--install", type=Path, help="your Xenon 2 CDTV WHDLoad install (.lha or folder), once")
    parser.add_argument("--music", type=Path, help="the Xenon2Audio add-on (.lha, folder or BTB.8svx), once")
    parser.add_argument("--no-xbox", action="store_true", help="stop after recompiling")
    args = parser.parse_args()
    if args.install or not (ROOT / "game/data/X2").exists() or not (BUILD / "image.bin").exists():
        if not args.install:
            sys.exit("build: give your Xenon 2 CDTV install with --install (Xenon2_v1.7_CDTV.lha)")
        run([sys.executable, ROOT / "tools/extract.py", args.install])
    if args.music:
        run([sys.executable, ROOT / "tools/music.py", args.music])
    run([sys.executable, ROOT / "tools/recompile.py"])
    if args.no_xbox:
        return 0
    projects()
    # The RXDK build keeps staged files that left the manifest: a test input
    # script must not stay on the disc once build/inputs.txt is gone.
    if not (BUILD / "inputs.txt").exists():
        (ROOT / "out/Build/xenon2/inputs.txt").unlink(missing_ok=True)
    run([rxdk(), "build", "--project-root", ROOT])
    print(f"build: {ROOT / 'out/xenon2.xbe'} and {ROOT / 'out/XISO/xenon2.iso'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
