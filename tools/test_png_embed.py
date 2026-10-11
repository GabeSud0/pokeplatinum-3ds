#!/usr/bin/env python3
"""Tests for tools/png_embed.py (nitrogfx -embed's conversion in Python).

The PNGs are made here, so the tests need no game files. The expected bytes
follow nitrogfx's ConvertToTiles4Bpp/8Bpp and WriteEmbeddableHeader (its
gfx.c); the real images were also checked byte for byte against nitrogfx.
"""

from __future__ import annotations

import struct
import sys
import tempfile
import unittest
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import png_embed  # noqa: E402


def chunk(kind: bytes, body: bytes) -> bytes:
    return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))


def make_png(width: int, height: int, depth: int, color: int, rows: list[bytes],
             filters: list[int] | None = None) -> bytes:
    """A PNG of the given packed rows, each row filtered with the given type."""
    filters = filters or [0] * height
    raw = bytearray()
    prev = bytes(len(rows[0]))
    for row, kind in zip(rows, filters):
        out = bytearray()
        for i, x in enumerate(row):
            a = row[i - 1] if i else 0
            b = prev[i]
            c = prev[i - 1] if i else 0
            if kind == 0:
                pred = 0
            elif kind == 1:
                pred = a
            elif kind == 2:
                pred = b
            elif kind == 3:
                pred = (a + b) >> 1
            else:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pred = a if pa <= pb and pa <= pc else (b if pb <= pc else c)
            out.append((x - pred) & 0xFF)
        raw += bytes([kind]) + out
        prev = row
    body = chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, depth, color, 0, 0, 0))
    if color == png_embed.COLOR_PALETTE:
        body += chunk(b"PLTE", bytes(3 * 16))
    body += chunk(b"IDAT", zlib.compress(bytes(raw)))
    body += chunk(b"IEND", b"")
    return png_embed.PNG_SIGNATURE + body


def pattern_4bpp(width: int, height: int) -> list[list[int]]:
    """Pixel values that tell every position apart (mod 16)."""
    return [[(x * 3 + y * 5) % 16 for x in range(width)] for y in range(height)]


def pack_4bpp(pixels: list[list[int]]) -> list[bytes]:
    """PNG's packing: the left pixel in the high nibble."""
    return [bytes((row[x] << 4) | row[x + 1] for x in range(0, len(row), 2)) for row in pixels]


def expected_tiles_4bpp(pixels: list[list[int]], invert: bool = False) -> bytes:
    """nitrogfx's order: tiles left to right, top to bottom; within a byte
    the left pixel in the low nibble."""
    out = bytearray()
    height, width = len(pixels), len(pixels[0])
    for ty in range(height // 8):
        for tx in range(width // 8):
            for y in range(8):
                for x in range(0, 8, 2):
                    left = pixels[ty * 8 + y][tx * 8 + x]
                    right = pixels[ty * 8 + y][tx * 8 + x + 1]
                    if invert:
                        left, right = 15 - left, 15 - right
                    out.append((right << 4) | left)
    return bytes(out)


class PngEmbedTest(unittest.TestCase):
    def convert(self, png: bytes, header_name: str = "image.4bpp.h", name: str = "sImage") -> str:
        with tempfile.TemporaryDirectory() as tmp:
            src = Path(tmp) / "image.png"
            dst = Path(tmp) / header_name
            src.write_bytes(png)
            png_embed.embed(src, dst, name)
            return dst.read_text(encoding="ascii")

    def test_4bpp_palette_tiles_in_nitrogfx_order(self):
        pixels = pattern_4bpp(24, 16)          # 3 x 2 tiles
        png = make_png(24, 16, 4, png_embed.COLOR_PALETTE, pack_4bpp(pixels))
        width, height, depth, has_palette, rows = png_embed.read_png(png)
        self.assertEqual((width, height, depth, has_palette), (24, 16, 4, True))
        tiles = png_embed.to_tiles(width, height, depth, False, rows)
        self.assertEqual(tiles, expected_tiles_4bpp(pixels))
        self.assertEqual(len(tiles), 6 * 32)

    def test_every_filter_type_decodes(self):
        pixels = pattern_4bpp(16, 8)
        rows = pack_4bpp(pixels)
        for kind in range(5):
            png = make_png(16, 8, 4, png_embed.COLOR_PALETTE, rows, [kind] * 8)
            self.assertEqual(png_embed.read_png(png)[4], rows, f"filter {kind}")
        mixed = make_png(16, 8, 4, png_embed.COLOR_PALETTE, rows, [0, 1, 2, 3, 4, 4, 3, 1])
        self.assertEqual(png_embed.read_png(mixed)[4], rows)

    def test_grayscale_is_inverted_as_nitrogfx_does(self):
        pixels = pattern_4bpp(8, 8)
        png = make_png(8, 8, 4, png_embed.COLOR_GRAY, pack_4bpp(pixels))
        width, height, depth, has_palette, rows = png_embed.read_png(png)
        self.assertFalse(has_palette)
        self.assertEqual(png_embed.to_tiles(width, height, depth, True, rows),
                         expected_tiles_4bpp(pixels, invert=True))

    def test_8bpp(self):
        pixels = [[(x + 8 * y) for x in range(16)] for y in range(8)]
        png = make_png(16, 8, 8, png_embed.COLOR_PALETTE, [bytes(r) for r in pixels])
        text = self.convert(png, "image.8bpp.h")
        values = [int(line.strip().rstrip(","), 16) for line in text.splitlines()
                  if line.startswith("    0x")]
        expected = [pixels[y][tx * 8 + x] for tx in range(2) for y in range(8) for x in range(8)]
        self.assertEqual(values, expected)

    def test_header_text_matches_nitrogfx(self):
        pixels = [[0] * 8 for _ in range(8)]
        pixels[0][0], pixels[0][1] = 1, 2
        png = make_png(8, 8, 4, png_embed.COLOR_PALETTE, pack_4bpp(pixels))
        text = self.convert(png, "cursor.4bpp.h", "sCursorBitmap")
        lines = text.split("\n")
        self.assertEqual(lines[:4], ["#ifndef GUARD_EMBEDDABLE_sCursorBitmap_H",
                                     "#define GUARD_EMBEDDABLE_sCursorBitmap_H",
                                     "",
                                     "__attribute__((aligned(4))) const u8 sCursorBitmap[] = {"])
        self.assertEqual(lines[4], "    0x21,")        # left pixel 1 low, right pixel 2 high
        self.assertEqual(lines[4:36], ["    0x21,"] + ["    0x00,"] * 31)
        self.assertEqual(lines[36:], ["};", "", "#endif // GUARD_EMBEDDABLE_sCursorBitmap_H", ""])

    def test_rejects_what_it_cannot_convert(self):
        rows = pack_4bpp(pattern_4bpp(8, 8))
        with self.assertRaises(png_embed.PngError):
            self.convert(make_png(8, 8, 4, png_embed.COLOR_PALETTE, rows), "image.8bpp.h")
        with self.assertRaises(png_embed.PngError):
            png_embed.to_tiles(12, 8, 4, False, [bytes(6)] * 8)
        with self.assertRaises(png_embed.PngError):
            png_embed.read_png(b"not a png")
        rgb = png_embed.PNG_SIGNATURE + chunk(b"IHDR", struct.pack(">IIBBBBB", 8, 8, 8, 2, 0, 0, 0))
        with self.assertRaises(png_embed.PngError):
            png_embed.read_png(rgb)


if __name__ == "__main__":
    unittest.main()
