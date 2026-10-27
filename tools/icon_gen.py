#!/usr/bin/env python3
"""
Menu icon generator for the e-Paper Reader.

Icons are described with simple vector shapes in a 0..1 coordinate space and
rasterized (4x4 supersampled, 50% coverage threshold) to 1-bit bitmaps at each
size the UI needs. No third-party dependencies.

Usage:
  python icon_gen.py              # writes ../src/icons.h
  python icon_gen.py --preview    # also prints ASCII previews
"""

import argparse
import math
import os

SIZES = {"large": 64, "small": 36}
SUPERSAMPLE = 4


# --- Shape primitives: each returns an inside(x, y) test in unit coordinates ---

def rect(x0, y0, x1, y1):
    return lambda x, y: x0 <= x <= x1 and y0 <= y <= y1


def seg_dist(px, py, x0, y0, x1, y1):
    dx, dy = x1 - x0, y1 - y0
    length2 = dx * dx + dy * dy
    t = 0.0 if length2 == 0 else max(0.0, min(1.0, ((px - x0) * dx + (py - y0) * dy) / length2))
    cx, cy = x0 + t * dx, y0 + t * dy
    return math.hypot(px - cx, py - cy)


def line(x0, y0, x1, y1, w):
    return lambda x, y: seg_dist(x, y, x0, y0, x1, y1) <= w / 2


def polyline(points, w, closed=False):
    segs = list(zip(points, points[1:] + (points[:1] if closed else [])))
    return lambda x, y: any(seg_dist(x, y, a[0], a[1], b[0], b[1]) <= w / 2 for a, b in segs)


def polygon(points):
    def inside(x, y):
        hit = False
        n = len(points)
        for i in range(n):
            (x0, y0), (x1, y1) = points[i], points[(i + 1) % n]
            if (y0 > y) != (y1 > y) and x < x0 + (y - y0) * (x1 - x0) / (y1 - y0):
                hit = not hit
        return hit
    return inside


def circle(cx, cy, r):
    return lambda x, y: math.hypot(x - cx, y - cy) <= r


def ring(cx, cy, r, w):
    return lambda x, y: abs(math.hypot(x - cx, y - cy) - r) <= w / 2


# --- Icons: lists of (shape, ink) painted in order; ink 0 erases ---

S = 0.075  # stroke width


def icon_continue():
    # Open book with a bookmark ribbon
    left = [(0.50, 0.30), (0.08, 0.22), (0.08, 0.80), (0.50, 0.88)]
    right = [(0.50, 0.30), (0.92, 0.22), (0.92, 0.80), (0.50, 0.88)]
    ops = [(polyline(left, S, closed=True), 1), (polyline(right, S, closed=True), 1)]

    # Text lines run parallel to the page tops, which slope up towards the outer edges
    def page_y(x, depth):
        return 0.30 - abs(0.50 - x) * (0.08 / 0.42) + depth

    for depth in (0.14, 0.26, 0.38):
        ops.append((line(0.16, page_y(0.16, depth), 0.42, page_y(0.42, depth), S * 0.6), 1))
        if depth > 0.2:  # leave room for the bookmark on the right page
            ops.append((line(0.58, page_y(0.58, depth), 0.84, page_y(0.84, depth), S * 0.6), 1))
    ops.append((polygon([(0.66, 0.16), (0.80, 0.16), (0.80, 0.44), (0.73, 0.38), (0.66, 0.44)]), 1))
    return ops


def icon_library():
    # Three upright books and one leaning on them
    ops = []
    for x0, x1, top in ((0.10, 0.26, 0.20), (0.30, 0.46, 0.12), (0.50, 0.64, 0.26)):
        ops.append((polyline([(x0, top), (x1, top), (x1, 0.86), (x0, 0.86)], S, closed=True), 1))
        ops.append((line(x0 + 0.03, top + 0.12, x1 - 0.03, top + 0.12, S * 0.6), 1))
    lean = [(0.68, 0.84), (0.82, 0.86), (0.93, 0.30), (0.79, 0.27)]
    ops.append((polyline(lean, S, closed=True), 1))
    ops.append((line(0.06, 0.92, 0.94, 0.92, S), 1))
    return ops


def icon_settings():
    # Gear: body ring with eight teeth and a hole
    ops = []
    teeth = 8
    for i in range(teeth):
        a = 2 * math.pi * i / teeth
        ca, sa = math.cos(a), math.sin(a)
        pts = []
        for r, half in ((0.30, 0.10), (0.44, 0.075)):
            px, py = -sa * half, ca * half
            pts.append((0.5 + ca * r + px, 0.5 + sa * r + py))
            pts.append((0.5 + ca * r - px, 0.5 + sa * r - py))
        ops.append((polygon([pts[0], pts[2], pts[3], pts[1]]), 1))
    ops.append((circle(0.5, 0.5, 0.33), 1))
    ops.append((circle(0.5, 0.5, 0.14), 0))
    return ops


def icon_transfer():
    # Up and down arrows side by side
    ops = [
        (line(0.33, 0.20, 0.33, 0.86, S), 1),
        (polygon([(0.33, 0.08), (0.16, 0.32), (0.50, 0.32)]), 1),
        (line(0.67, 0.14, 0.67, 0.80, S), 1),
        (polygon([(0.67, 0.92), (0.50, 0.68), (0.84, 0.68)]), 1),
    ]
    return ops


def icon_stats():
    # Bar chart on a baseline
    ops = [(line(0.08, 0.88, 0.92, 0.88, S), 1)]
    for x0, top in ((0.14, 0.56), (0.40, 0.30), (0.66, 0.12)):
        ops.append((rect(x0, top, x0 + 0.20, 0.86), 1))
    return ops


def icon_sleep():
    # Crescent moon with two small stars
    ops = [
        (circle(0.44, 0.56, 0.34), 1),
        (circle(0.62, 0.44, 0.30), 0),
    ]
    for cx, cy, r in ((0.78, 0.18, 0.07), (0.88, 0.42, 0.05)):
        ops.append((line(cx - r, cy, cx + r, cy, S * 0.7), 1))
        ops.append((line(cx, cy - r, cx, cy + r, S * 0.7), 1))
    return ops


ICONS = {
    "continue": icon_continue,
    "library": icon_library,
    "settings": icon_settings,
    "transfer": icon_transfer,
    "stats": icon_stats,
    "sleep": icon_sleep,
}


def rasterize(ops, size):
    rows = []
    n = SUPERSAMPLE
    for py in range(size):
        row = []
        for px in range(size):
            covered = 0
            for sy in range(n):
                for sx in range(n):
                    x = (px + (sx + 0.5) / n) / size
                    y = (py + (sy + 0.5) / n) / size
                    ink = 0
                    for shape, value in ops:
                        if shape(x, y):
                            ink = value
                    covered += ink
            row.append(1 if covered * 2 >= n * n else 0)
        rows.append(row)
    return rows


def pack(rows):
    out = []
    for row in rows:
        for i in range(0, len(row), 8):
            byte = 0
            for bit, v in enumerate(row[i:i + 8]):
                if v:
                    byte |= 0x80 >> bit
            out.append(byte)
    return out


def main():
    parser = argparse.ArgumentParser(description="Generate menu icon bitmaps")
    parser.add_argument("--output", default=os.path.join(os.path.dirname(__file__), "..", "src", "icons.h"))
    parser.add_argument("--preview", action="store_true", help="print ASCII previews")
    args = parser.parse_args()

    lines = [
        "// Generated by tools/icon_gen.py - do not edit by hand",
        "#ifndef ICONS_H",
        "#define ICONS_H",
        "",
        "#include <stdint.h>",
        "",
        "typedef struct {",
        "    uint8_t width;",
        "    uint8_t height;",
        "    const uint8_t *bits; // row-major, MSB first, rows padded to whole bytes",
        "} Icon;",
        "",
    ]
    for name, build in ICONS.items():
        ops = build()
        for size_name, size in SIZES.items():
            rows = rasterize(ops, size)
            if args.preview:
                print(f"{name} {size}px")
                print("\n".join("".join("#" if v else "." for v in r) for r in rows))
            data = pack(rows)
            ident = f"icon_{name}_{size_name}"
            lines.append(f"static const uint8_t {ident}_bits[] = {{")
            for i in range(0, len(data), 16):
                lines.append("    " + ", ".join(f"0x{b:02X}" for b in data[i:i + 16]) + ",")
            lines.append("};")
            lines.append(f"static const Icon {ident} = {{ {size}, {size}, {ident}_bits }};")
            lines.append("")
    lines.append("#endif // ICONS_H")

    with open(args.output, "w", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    print(f"Wrote {args.output}")


if __name__ == "__main__":
    main()
