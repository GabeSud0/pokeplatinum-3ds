#!/usr/bin/env python3
"""The HOME Menu's icon and banner for the installable program, made from
the icon in your own ROM (made at build time, never committed: it's the
game's art).

    python3 tools/home_menu_art.py platinum.nds OUT_DIR
    python3 tools/home_menu_art.py --blank OUT_DIR     (no ROM: the backdrop only)

writes OUT_DIR/icon.png (48x48: the HOME Menu's icon), OUT_DIR/banner.png
(256x128: the picture on the upper screen when the title is picked) and
OUT_DIR/banner.wav (its sound: a moment of silence).

A DS ROM's header says where its banner is (offset 0x68); the banner has a
32x32 icon at +0x20 (4 bits a pixel, in 8x8 tiles, the left pixel in the low
nibble) and its 16 colours at +0x220 (RGB555; colour 0 is transparent).

The icon: each pixel doubled, then scaled to 48x48 by area (the pixels as
they are, their borders a little soft), over a plain backdrop (the HOME
Menu's icons have no transparency). The banner: the icon at three times its
size on the same backdrop. Standard library only (zlib for the PNGs).
"""

import argparse
import os
import struct
import sys
import zlib

BACKDROP_TOP = (0x5C, 0x66, 0x78)
BACKDROP_BOTTOM = (0x23, 0x27, 0x30)


def read_icon(rom: bytes):
    """The ROM's icon: 32x32 rows of (r, g, b, a), a 0 or 255."""
    if len(rom) < 0x200:
        raise ValueError("too small for a DS ROM")
    banner = struct.unpack_from("<I", rom, 0x68)[0]
    if banner == 0 or banner + 0x240 > len(rom):
        raise ValueError("the ROM has no banner (no icon)")
    bitmap = rom[banner + 0x20:banner + 0x220]
    palette = struct.unpack_from("<16H", rom, banner + 0x220)
    colours = []
    for i, c in enumerate(palette):
        r, g, b = c & 31, (c >> 5) & 31, (c >> 10) & 31
        colours.append(((r << 3) | (r >> 2), (g << 3) | (g >> 2), (b << 3) | (b >> 2), 0 if i == 0 else 255))
    pixels = [[None] * 32 for _ in range(32)]
    for tile in range(16):
        tx, ty = tile % 4, tile // 4
        for y in range(8):
            for x in range(8):
                byte = bitmap[tile * 32 + y * 4 + x // 2]
                index = (byte >> 4) if x & 1 else (byte & 15)
                pixels[ty * 8 + y][tx * 8 + x] = colours[index]
    return pixels


def backdrop(w, h):
    rows = []
    for y in range(h):
        t = y / max(h - 1, 1)
        c = tuple(round(a + (b - a) * t) for a, b in zip(BACKDROP_TOP, BACKDROP_BOTTOM))
        rows.append([c] * w)
    return rows


def over(dst, src_rgba, x0, y0):
    """src (rows of r, g, b, a with alpha 0-255) over dst (rows of r, g, b)."""
    for y, row in enumerate(src_rgba):
        for x, (r, g, b, a) in enumerate(row):
            if a == 0:
                continue
            d = dst[y0 + y][x0 + x]
            dst[y0 + y][x0 + x] = tuple(round(s * a / 255 + v * (255 - a) / 255) for s, v in zip((r, g, b), d))


def scale_nearest(pixels, n):
    return [[p for p in row for _ in range(n)] for row in pixels for _ in range(n)]


def scale_area(pixels, size):
    """Pixels to size x size by area, alpha-weighted (colour from the opaque
    part of each output pixel, alpha by coverage)."""
    h, w = len(pixels), len(pixels[0])
    out = []
    for oy in range(size):
        y0, y1 = oy * h / size, (oy + 1) * h / size
        row = []
        for ox in range(size):
            x0, x1 = ox * w / size, (ox + 1) * w / size
            acc = [0.0, 0.0, 0.0, 0.0]
            total = 0.0
            for sy in range(int(y0), min(int(-(-y1 // 1)), h)):
                fy = min(y1, sy + 1) - max(y0, sy)
                for sx in range(int(x0), min(int(-(-x1 // 1)), w)):
                    f = fy * (min(x1, sx + 1) - max(x0, sx))
                    if f <= 0:
                        continue
                    r, g, b, a = pixels[sy][sx]
                    wa = f * a / 255
                    acc[0] += r * wa
                    acc[1] += g * wa
                    acc[2] += b * wa
                    acc[3] += wa
                    total += f
            if acc[3] > 0:
                row.append((round(acc[0] / acc[3]), round(acc[1] / acc[3]), round(acc[2] / acc[3]),
                            round(255 * acc[3] / total)))
            else:
                row.append((0, 0, 0, 0))
        out.append(row)
    return out


def png(rows) -> bytes:
    """rows of (r, g, b) as an RGB PNG."""
    h, w = len(rows), len(rows[0])
    raw = b"".join(b"\0" + bytes(v for p in row for v in p) for row in rows)

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def silent_wav(seconds=0.5, rate=22050) -> bytes:
    data = b"\0\0" * int(seconds * rate)
    return (b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVE" +
            b"fmt " + struct.pack("<IHHIIHH", 16, 1, 1, rate, rate * 2, 2, 16) +
            b"data" + struct.pack("<I", len(data)) + data)


def make(rom, out_dir: str):
    """rom: the ROM's bytes, or None (the backdrop alone)."""
    icon = read_icon(rom) if rom is not None else [[(0, 0, 0, 0)] * 32 for _ in range(32)]
    os.makedirs(out_dir, exist_ok=True)

    small = backdrop(48, 48)
    over(small, scale_area(scale_nearest(icon, 2), 48), 0, 0)
    with open(os.path.join(out_dir, "icon.png"), "wb") as f:
        f.write(png(small))

    big = backdrop(256, 128)
    over(big, scale_nearest(icon, 3), (256 - 96) // 2, (128 - 96) // 2)
    with open(os.path.join(out_dir, "banner.png"), "wb") as f:
        f.write(png(big))

    with open(os.path.join(out_dir, "banner.wav"), "wb") as f:
        f.write(silent_wav())


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("rom", nargs="?")
    parser.add_argument("out_dir")
    parser.add_argument("--blank", action="store_true", help="no ROM: the backdrop alone")
    args = parser.parse_args()
    if args.blank == (args.rom is not None):
        parser.error("give a ROM, or --blank")
    rom = None
    if args.rom is not None:
        with open(args.rom, "rb") as f:
            rom = f.read(0x10000000)
    try:
        make(rom, args.out_dir)
    except ValueError as e:
        sys.exit(f"{args.rom}: {e}")
    print(f"HOME Menu icon and banner in {args.out_dir}")


if __name__ == "__main__":
    main()
