#!/usr/bin/env python3
"""
Renders firmware screens on the PC, without flashing.

Compiles src/display.c and src/font_extended.c against the stub headers in
./stubs together with ui_preview.c (which draws the screens to preview), runs
it, and converts each captured framebuffer to a PNG at 2x scale.

Usage:
  python tools/ui_preview/preview.py              # writes PNGs to tools/ui_preview/out
  python tools/ui_preview/preview.py --cc clang   # pick a different C compiler
  python tools/ui_preview/preview.py --scale 3

To preview another screen, add a draw call + dump() to ui_preview.c.
Needs a host C compiler (gcc or clang); no Python packages.
"""

import argparse
import glob
import os
import struct
import subprocess
import sys
import tempfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, "..", ".."))


def read_pbm(path):
    with open(path, "rb") as f:
        data = f.read()
    magic, dims, pixels = data.split(b"\n", 2)
    assert magic == b"P4"
    width, height = map(int, dims.split())
    stride = (width + 7) // 8
    return [[(pixels[y * stride + x // 8] >> (7 - x % 8)) & 1 for x in range(width)] for y in range(height)]


def write_png(path, rows):
    # Grayscale 8-bit PNG; rows hold 1 = black
    raw = b"".join(b"\x00" + bytes(0 if v else 255 for v in row) for row in rows)

    def chunk(tag, body):
        return struct.pack(">I", len(body)) + tag + body + struct.pack(">I", zlib.crc32(tag + body) & 0xFFFFFFFF)

    header = struct.pack(">IIBBBBB", len(rows[0]), len(rows), 8, 0, 0, 0, 0)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def framed(img, scale, border=2):
    # Scales up and adds a thin black frame so the panel edges are visible
    w = len(img[0]) * scale
    out = [[1] * (w + border * 2) for _ in range(border)]
    for row in img:
        line = [1] * border + [v for v in row for _ in range(scale)] + [1] * border
        out.extend(list(line) for _ in range(scale))
    out.extend([1] * (w + border * 2) for _ in range(border))
    return out


def main():
    parser = argparse.ArgumentParser(description="Render firmware screens to PNG on the PC")
    parser.add_argument("--cc", default=os.environ.get("CC", "gcc"), help="host C compiler (default: gcc)")
    parser.add_argument("--out", default=os.path.join(HERE, "out"), help="output directory for PNGs")
    parser.add_argument("--scale", type=int, default=2, help="pixel scale factor")
    args = parser.parse_args()

    os.makedirs(args.out, exist_ok=True)
    with tempfile.TemporaryDirectory() as tmp:
        exe = os.path.join(tmp, "ui_preview.exe" if os.name == "nt" else "ui_preview")
        cmd = [
            args.cc, "-O1", "-w",
            "-I", os.path.join(HERE, "stubs"),
            "-I", os.path.join(REPO, "src"),
            "-I", os.path.join(REPO, "drivers", "display"),
            os.path.join(HERE, "ui_preview.c"),
            os.path.join(REPO, "src", "display.c"),
            os.path.join(REPO, "src", "font_extended.c"),
            "-o", exe,
        ]
        subprocess.run(cmd, check=True)
        subprocess.run([exe, tmp], check=True)

        for pbm in sorted(glob.glob(os.path.join(tmp, "*.pbm"))):
            name = os.path.splitext(os.path.basename(pbm))[0] + ".png"
            write_png(os.path.join(args.out, name), framed(read_pbm(pbm), args.scale))
            print("wrote", os.path.join(args.out, name))


if __name__ == "__main__":
    sys.exit(main())
