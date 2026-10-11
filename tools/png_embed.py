#!/usr/bin/env python3
"""Turn an indexed PNG into a C header of DS tiles, as `nitrogfx -embed` does.

Usage:
    python tools/png_embed.py in.png out.4bpp.h NAME

The game embeds two small images in its code (the font's arrow cursor and
the battle health box parts). The decompilation makes their headers with
its nitrogfx tool (`nitrogfx in.png out.4bpp -embed NAME`), which needs
libpng to build. This does the same in plain Python, so building the game
needs no image library (MSYS2 on Windows has none for its own compiler).
The output is byte-for-byte nitrogfx's; tools/test_png_embed.py checks the
conversion.

Only what those two images use is supported: a non-interlaced grayscale or
palette PNG whose bit depth is the output's (4bpp or 8bpp), cut into 8x8
tiles left to right, top to bottom.
"""

from __future__ import annotations

import argparse
import re
import struct
import sys
import zlib
from pathlib import Path

PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"
COLOR_GRAY = 0
COLOR_PALETTE = 3


class PngError(Exception):
    pass


def read_png(data: bytes) -> tuple[int, int, int, bool, list[bytes]]:
    """(width, height, bit depth, has a palette, rows of packed pixels)."""
    if not data.startswith(PNG_SIGNATURE):
        raise PngError("not a PNG file")
    pos = len(PNG_SIGNATURE)
    header = None
    idat = bytearray()
    while pos + 8 <= len(data):
        length, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if kind == b"IHDR":
            header = struct.unpack(">IIBBBBB", body)
        elif kind == b"IDAT":
            idat += body
        elif kind == b"IEND":
            break
    if header is None:
        raise PngError("no IHDR chunk")
    width, height, depth, color, _compression, _filter, interlace = header
    if color not in (COLOR_GRAY, COLOR_PALETTE):
        raise PngError("unsupported color type (only grayscale and palette)")
    if depth not in (1, 2, 4, 8):
        raise PngError(f"unsupported bit depth {depth}")
    if interlace:
        raise PngError("interlaced PNGs aren't supported")

    raw = zlib.decompress(bytes(idat))
    stride = (width * depth + 7) // 8
    bpp = 1                      # bytes per pixel for filtering: at least 1
    rows: list[bytes] = []
    prev = bytearray(stride)
    pos = 0
    for _ in range(height):
        kind = raw[pos]
        line = bytearray(raw[pos + 1:pos + 1 + stride])
        pos += 1 + stride
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if kind == 0:
                continue
            if kind == 1:
                line[i] = (line[i] + a) & 0xFF
            elif kind == 2:
                line[i] = (line[i] + b) & 0xFF
            elif kind == 3:
                line[i] = (line[i] + ((a + b) >> 1)) & 0xFF
            elif kind == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pred = a if pa <= pb and pa <= pc else (b if pb <= pc else c)
                line[i] = (line[i] + pred) & 0xFF
            else:
                raise PngError(f"bad filter type {kind}")
        rows.append(bytes(line))
        prev = line
    return width, height, depth, color == COLOR_PALETTE, rows


def to_tiles(width: int, height: int, depth: int, invert: bool, rows: list[bytes]) -> bytes:
    """8x8 tiles, left to right then top to bottom; within a 4bpp byte the
    left pixel is the low nibble (the DS's order; PNG's is the opposite)."""
    if width % 8 or height % 8:
        raise PngError(f"{width}x{height} isn't a multiple of 8x8 tiles")
    out = bytearray()
    for ty in range(height // 8):
        for tx in range(width // 8):
            for y in range(8):
                row = rows[ty * 8 + y]
                if depth == 4:
                    for byte in row[tx * 4:tx * 4 + 4]:
                        left, right = byte >> 4, byte & 0xF
                        if invert:
                            left, right = 15 - left, 15 - right
                        out.append((right << 4) | left)
                elif depth == 8:
                    for byte in row[tx * 8:tx * 8 + 8]:
                        out.append(255 - byte if invert else byte)
                else:
                    raise PngError(f"{depth}-bit tiles aren't supported")
    return bytes(out)


def render_header(data: bytes, name: str) -> str:
    """nitrogfx's WriteEmbeddableHeader, character for character."""
    lines = [f"#ifndef GUARD_EMBEDDABLE_{name}_H",
             f"#define GUARD_EMBEDDABLE_{name}_H",
             "",
             f"__attribute__((aligned(4))) const u8 {name}[] = {{"]
    lines += [f"    0x{byte:02X}," for byte in data]
    lines += ["};", "", f"#endif // GUARD_EMBEDDABLE_{name}_H", ""]
    return "\n".join(lines)


def embed(png: Path, header: Path, name: str) -> None:
    m = re.search(r"\.(\d)bpp\.h$", header.name)
    if not m:
        raise PngError(f"{header.name}: expected a name ending in .4bpp.h or .8bpp.h")
    width, height, depth, has_palette, rows = read_png(png.read_bytes())
    if depth != int(m.group(1)):
        raise PngError(f"{png.name} is {depth}-bit; converting to {m.group(1)}bpp isn't supported")
    tiles = to_tiles(width, height, depth, not has_palette, rows)
    header.write_bytes(render_header(tiles, name).encode("ascii"))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("png", type=Path)
    parser.add_argument("header", type=Path)
    parser.add_argument("name")
    args = parser.parse_args()
    try:
        embed(args.png, args.header, args.name)
    except PngError as e:
        sys.exit(f"{args.png}: {e}")


if __name__ == "__main__":
    main()
