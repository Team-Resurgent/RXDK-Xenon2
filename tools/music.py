#!/usr/bin/env python3
"""The game's CD music for game/music, as Ogg Vorbis (from CD audio:
16-bit stereo at 44.1 kHz), which the Xbox build decodes (stb_vorbis) when
the game plays a CD track. ffmpeg (with libvorbis) does the encoding.

  python tools/music.py <the CDTV disc's .cue | its folder>
  python tools/music.py <Xenon2Audio_v0.2b_BETA.lha | its folder | BTB.8svx>

A rip of the CDTV disc (BIN/CUE) gives the game's own tracks, written as
game/music/trackNN.ogg from each track's INDEX 01 (past its pregap).
Without one there is one piece of Bomb the Bass music: BTB.8svx from the
Xenon2Audio add-on (8-bit stereo 8SVX), written as game/music/btb.ogg and
played for every track the game asks for.
"""
from __future__ import annotations

import re
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SEVEN_ZIP = [Path(r"C:\Program Files\7-Zip\7z.exe"), Path(r"C:\Program Files (x86)\7-Zip\7z.exe")]


def find_8svx(source: Path, into: Path) -> Path:
    if source.is_file() and source.suffix.lower() == ".8svx":
        return source
    if source.is_file():
        tool = shutil.which("7z") or next((str(p) for p in SEVEN_ZIP if p.exists()), None)
        if not tool:
            raise SystemExit("music: 7-Zip is needed to open .lha archives (or pass the unpacked folder)")
        subprocess.run([tool, "x", "-y", f"-o{into}", str(source)], check=True, stdout=subprocess.DEVNULL)
        source = into
    found = next((p for p in source.rglob("*") if p.name.lower() == "btb.8svx"), None)
    if not found:
        raise SystemExit(f"music: no BTB.8svx in {source}")
    return found


def convert_8svx(path: Path) -> bytes:
    """An uncompressed 8SVX (mono, or stereo as all left then all right) as
    16-bit stereo at 44.1 kHz."""
    data = path.read_bytes()
    if data[:4] != b"FORM" or data[8:12] != b"8SVX":
        raise SystemExit(f"music: {path} is not an 8SVX file")
    rate, channels, body = 0, 2, None
    at = 12
    while at + 8 <= len(data):
        cid, size = data[at:at + 4], struct.unpack(">I", data[at + 4:at + 8])[0]
        chunk = data[at + 8:at + 8 + size]
        if cid == b"VHDR":
            rate, compression = struct.unpack(">H", chunk[12:14])[0], chunk[15]
            if compression:
                raise SystemExit("music: compressed 8SVX is not supported")
        elif cid == b"CHAN":
            channels = 2 if struct.unpack(">I", chunk[:4])[0] == 6 else 1
        elif cid == b"BODY":
            body = chunk
        at += 8 + size + (size & 1)
    if rate != 44100 or body is None:
        raise SystemExit(f"music: expected 44.1 kHz sound in {path} (rate {rate})")
    frames = len(body) // channels
    left = body[:frames]
    right = body[frames:2 * frames] if channels == 2 else left
    out = bytearray(frames * 4)
    # Signed 8-bit to 16-bit: the byte as both halves (s * 257, full scale).
    out[0::4] = out[1::4] = left
    out[2::4] = out[3::4] = right
    return bytes(out)


QUALITY = "5"           # libvorbis -q: about 160 kbit/s


def encode(pcm, out: Path) -> None:
    """CD audio (bytes, or a file positioned at it) as Ogg Vorbis."""
    ffmpeg = shutil.which("ffmpeg")
    if not ffmpeg:
        raise SystemExit("music: ffmpeg (with libvorbis) is needed to encode the music")
    cmd = [ffmpeg, "-hide_banner", "-loglevel", "error", "-y", "-f", "s16le", "-ar", "44100", "-ac", "2",
           "-i", "-", "-c:a", "libvorbis", "-q:a", QUALITY, str(out)]
    if isinstance(pcm, bytes):
        subprocess.run(cmd, input=pcm, check=True)
    else:
        subprocess.run(cmd, stdin=pcm, check=True)
    print(f"music: {out} ({out.stat().st_size >> 10} KB)")


def find_cue(source: Path) -> Path | None:
    if source.is_file():
        return source if source.suffix.lower() == ".cue" else None
    return next(source.glob("*.cue"), None) if source.is_dir() else None


def write_tracks(cue: Path, music: Path) -> None:
    """The audio tracks of a BIN/CUE rip (one bin per track, as Redump's)."""
    sector = 2352
    current, tracks = None, []
    for line in cue.read_text(errors="replace").splitlines():
        words = line.split()
        if not words:
            continue
        if words[0] == "FILE":
            current = cue.parent / re.match(r'\s*FILE\s+"(.*)"', line).group(1)
        elif words[0] == "TRACK":
            tracks.append({"number": int(words[1]), "audio": words[2] == "AUDIO", "file": current, "start": 0})
        elif words[0] == "INDEX" and words[1] == "01" and tracks:
            m, s, f = (int(x) for x in words[2].split(":"))
            tracks[-1]["start"] = ((m * 60 + s) * 75 + f) * sector
    for t in tracks:
        if not t["audio"]:
            continue
        with open(t["file"], "rb") as src:
            src.seek(t["start"])
            encode(src, music / f"track{t['number']:02d}.ogg")


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__)
        return 1
    music = ROOT / "game/music"
    music.mkdir(parents=True, exist_ok=True)
    cue = find_cue(Path(sys.argv[1]))
    if cue:
        write_tracks(cue, music)
        return 0
    with tempfile.TemporaryDirectory() as tmp:
        path = find_8svx(Path(sys.argv[1]), Path(tmp))
        pcm = convert_8svx(path)
    encode(pcm, music / "btb.ogg")
    return 0


if __name__ == "__main__":
    sys.exit(main())
