#!/usr/bin/env python3
"""make_circle_png.py — generate img/circle.png, the disc texture the SFML
renderer tints to draw creatures, food and predators.

A white disc on a transparent square, edge anti-aliased by 4x4 supersampling.
Original to this project (it replaced a third-party file in v0.25.0); rerun
to regenerate. Requires Pillow.

usage: python3 tools/make_circle_png.py [out.png] [size]
"""
import sys

from PIL import Image


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "img/circle.png"
    size = int(sys.argv[2]) if len(sys.argv) > 2 else 1024
    ss = 4                        # supersampling factor per axis
    c = (size - 1) / 2.0          # centre, in pixel-centre coordinates
    r = size / 2.0 - 1.0          # leave a one-pixel transparent border
    alpha = bytearray(size * size)
    offsets = [(i + 0.5) / ss - 0.5 for i in range(ss)]
    r2 = r * r
    for y in range(size):
        for x in range(size):
            d2 = (x - c) ** 2 + (y - c) ** 2
            if d2 <= (r - 1) ** 2:    # well inside: opaque, no sampling needed
                alpha[y * size + x] = 255
                continue
            if d2 >= (r + 1) ** 2:    # well outside: transparent
                continue
            inside = 0
            for oy in offsets:
                dy = y + oy - c
                for ox in offsets:
                    dx = x + ox - c
                    if dx * dx + dy * dy <= r2:
                        inside += 1
            alpha[y * size + x] = round(255 * inside / (ss * ss))
    img = Image.new("RGBA", (size, size), (255, 255, 255, 0))
    img.putalpha(Image.frombytes("L", (size, size), bytes(alpha)))
    img.save(out, optimize=True)
    print(f"wrote {out} ({size}x{size})")


if __name__ == "__main__":
    main()
