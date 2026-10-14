"""tools/home_menu_art.py on a made-up ROM: a header pointing at a banner
whose icon is a known pattern."""

import os
import struct
import tempfile
import unittest
import zlib

import home_menu_art


def fake_rom(pattern):
    """A ROM whose 32x32 icon is pattern(x, y) -> colour index 0-15, with
    palette entry i = RGB555 (i, 2 * i, 31 - i)."""
    rom = bytearray(0x1000)
    banner = 0x400
    struct.pack_into("<I", rom, 0x68, banner)
    for tile in range(16):
        tx, ty = tile % 4, tile // 4
        for y in range(8):
            for x in range(0, 8, 2):
                lo = pattern(tx * 8 + x, ty * 8 + y)
                hi = pattern(tx * 8 + x + 1, ty * 8 + y)
                rom[banner + 0x20 + tile * 32 + y * 4 + x // 2] = lo | (hi << 4)
    for i in range(16):
        struct.pack_into("<H", rom, banner + 0x220 + 2 * i, i | ((2 * i) << 5) | ((31 - i) << 10))
    return bytes(rom)


def widen(v):
    return (v << 3) | (v >> 2)


def read_png(path):
    """(width, height, rows of (r, g, b)) of an RGB PNG as png() writes it."""
    with open(path, "rb") as f:
        data = f.read()
    assert data[:8] == b"\x89PNG\r\n\x1a\n"
    pos, idat, w, h = 8, b"", 0, 0
    while pos < len(data):
        n = struct.unpack_from(">I", data, pos)[0]
        kind, body = data[pos + 4:pos + 8], data[pos + 8:pos + 8 + n]
        if kind == b"IHDR":
            w, h = struct.unpack_from(">II", body)
        elif kind == b"IDAT":
            idat += body
        pos += 12 + n
    raw = zlib.decompress(idat)
    rows = []
    for y in range(h):
        line = raw[y * (1 + 3 * w):(y + 1) * (1 + 3 * w)]
        assert line[0] == 0
        rows.append([tuple(line[1 + 3 * x:4 + 3 * x]) for x in range(w)])
    return w, h, rows


class HomeMenuArtTest(unittest.TestCase):
    def test_icon_decoding(self):
        icon = home_menu_art.read_icon(fake_rom(lambda x, y: (x + y) % 16))
        self.assertEqual(len(icon), 32)
        self.assertEqual(icon[0][0][3], 0)  # colour 0: transparent
        self.assertEqual(icon[0][1], (widen(1), widen(2), widen(30), 255))
        self.assertEqual(icon[5][9], (widen(14), widen(28), widen(17), 255))
        self.assertEqual(icon[31][31], (widen(14), widen(28), widen(17), 255))  # (62 % 16)

    def test_outputs(self):
        # A filled square of colour 5 in the middle, transparent round it.
        rom = fake_rom(lambda x, y: 5 if 8 <= x < 24 and 8 <= y < 24 else 0)
        with tempfile.TemporaryDirectory() as out:
            home_menu_art.make(rom, out)
            w, h, icon = read_png(os.path.join(out, "icon.png"))
            self.assertEqual((w, h), (48, 48))
            colour5 = (widen(5), widen(10), widen(26))
            self.assertEqual(icon[24][24], colour5)       # the square, 1.5 times bigger
            self.assertEqual(icon[0][0], home_menu_art.BACKDROP_TOP)
            w, h, banner = read_png(os.path.join(out, "banner.png"))
            self.assertEqual((w, h), (256, 128))
            self.assertEqual(banner[64][128], colour5)    # 3 times bigger, centred
            self.assertEqual(banner[16 + 24][80 + 24], colour5)
            self.assertNotEqual(banner[16 + 23][80 + 23], colour5)
            with open(os.path.join(out, "banner.wav"), "rb") as f:
                wav = f.read()
            self.assertEqual(wav[:4], b"RIFF")
            self.assertEqual(wav[8:12], b"WAVE")

    def test_blank(self):
        with tempfile.TemporaryDirectory() as out:
            home_menu_art.make(None, out)
            w, h, banner = read_png(os.path.join(out, "banner.png"))
            self.assertEqual((w, h), (256, 128))
            self.assertEqual(banner[0][0], home_menu_art.BACKDROP_TOP)

    def test_no_banner(self):
        with self.assertRaises(ValueError):
            home_menu_art.read_icon(bytes(0x1000))


if __name__ == "__main__":
    unittest.main()
