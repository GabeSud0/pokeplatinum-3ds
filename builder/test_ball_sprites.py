"""The follower's Poké Balls (builder/ball_sprites.py), on a made-up battle
graphics archive: no game data."""
import os
import struct
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import ball_sprites  # noqa: E402
import followers  # noqa: E402


def lz10(data: bytes) -> bytes:
    """LZ77 (type 0x10) with literals only: what ball_sprites.lz10 reads."""
    out = bytearray([0x10, len(data) & 0xFF, len(data) >> 8 & 0xFF, len(data) >> 16])
    for i in range(0, len(data), 8):
        out.append(0)
        out += data[i:i + 8]
    return bytes(out)


def nitro_file(magic: bytes, block: bytes, content: bytes) -> bytes:
    return struct.pack("<4sHHIHH", magic, 0xFEFF, 1, 16 + 8 + len(content), 16, 1) + block + struct.pack("<I", 8 + len(content)) + content


def ncgr(tiles: bytes) -> bytes:
    return nitro_file(b"RGCN", b"RAHC", struct.pack("<HHIIIII", 0xFFFF, 0xFFFF, 3, 0x10, 0, len(tiles), 24) + tiles)


def nclr(colours: list[int]) -> bytes:
    raw = b"".join(struct.pack("<H", c) for c in colours)
    return nitro_file(b"RLCN", b"TTLP", struct.pack("<IIII", 3, 0, len(raw), 16) + raw)


def ncer(cells: list[list[tuple[int, int, int]]]) -> bytes:
    head = struct.pack("<HHI", len(cells), 0, 8)
    table, oams = b"", b""
    for cell in cells:
        table += struct.pack("<HHI", len(cell), 0, len(oams))
        oams += b"".join(struct.pack("<HHH", *o) for o in cell)
    return nitro_file(b"RECN", b"KBEC", head + table + oams)


def narc(members: list[bytes]) -> bytes:
    fat, data = b"", b""
    for m in members:
        fat += struct.pack("<II", len(data), len(data) + len(m))
        data += m + b"\0" * (-len(m) % 4)
    btaf = b"BTAF" + struct.pack("<II", 12 + len(fat), len(members)) + fat
    btnf = b"BTNF" + struct.pack("<I", 16) + struct.pack("<IHH", 4, 0, 1)
    gmif = b"GMIF" + struct.pack("<I", 8 + len(data)) + data
    body = btaf + btnf + gmif
    return b"NARC" + struct.pack("<HHIHH", 0xFFFE, 0x100, 16 + len(body), 16, 3) + body


def made_up_archive() -> bytes:
    """Each ball: one 16 x 16 OBJ (4 tiles) at (-8, -8) for its 9 first
    cells, colour 1 + its ball number % 15 in its first tile's first texel;
    a 32 x 32 OBJ at (-16, -16) for its open cell, colour 2."""
    members = [b""] * 340
    for k, (tiles_id, palette_id, cells_id) in enumerate(ball_sprites.BALLS):
        tiles = bytearray(32 * 4 + 32 * 16)
        tiles[0] = 1 + k % 15
        tiles[128] = 2
        members[tiles_id] = lz10(ncgr(bytes(tiles)))
        members[palette_id] = nclr([0] + [0x7FFF - k] * 15)
        closed = [(0xF8, 0x41F8, 0)]
        opened = [(0xF0, 0x81F0, 2)]   # (tile 2 << 1: the 5th tile)
        members[cells_id] = lz10(ncer([closed] * 9 + [opened]))
    return narc(members)


class BallSpritesTest(unittest.TestCase):
    def test_file(self):
        data = ball_sprites.make_file(made_up_archive())
        self.assertEqual(data[:4], b"PFBL")
        self.assertEqual(len(data) % 4, 0)
        sprites = ball_sprites.read_file(data)
        self.assertEqual(len(sprites), len(ball_sprites.BALLS) + len(ball_sprites.EMOTES))
        for s in sprites:
            self.assertEqual(followers.texture_size(s, 16), (32, 32))

    def test_cells(self):
        members = ball_sprites.narc_members(made_up_archive())
        tiles_id, _, cells_id = ball_sprites.BALLS[2]
        tiles = ball_sprites.read_tiles(ball_sprites.lz10(members[tiles_id]))
        cells = ball_sprites.read_cells(ball_sprites.lz10(members[cells_id]))
        ox, oy = ball_sprites.ORIGIN
        closed = ball_sprites.draw_cell(cells[0], tiles)
        opened = ball_sprites.draw_cell(cells[9], tiles)
        # (the closed ball's first texel at the origin less 8 each way)
        self.assertEqual(closed[(oy - 8) * 32 + ox - 8], 1 + 2)
        self.assertEqual(sum(1 for c in closed if c), 1)
        self.assertEqual(opened[(oy - 16) * 32 + ox - 16], 2)

    def test_pictures(self):
        pictures = ball_sprites.ball_pictures(made_up_archive())
        for k, (closed, opened, palette) in enumerate(pictures):
            self.assertEqual(palette[1], 0x7FFF - k)

    def test_carve(self):
        """A 14 x 14 square with a band two rows tall and a button column:
        12 x 12 after, the band and the button kept, the bottom row and the
        middle where they were."""
        pic = [0] * 1024
        for y in range(17, 31):
            for x in range(9, 23):
                pic[y * 32 + x] = 1                         # body
        for x in range(9, 23):
            pic[23 * 32 + x] = pic[24 * 32 + x] = 2         # band
        for y in range(17, 31):
            pic[y * 32 + 15] = pic[y * 32 + 16] = 3         # button column
        out = ball_sprites.carve(pic)
        ys = [y for y in range(32) if any(out[y * 32:(y + 1) * 32])]
        xs = [x for x in range(32) if any(out[y * 32 + x] for y in range(32))]
        self.assertEqual((len(xs), len(ys)), (12, 12))
        self.assertEqual(ys[-1], 30)
        self.assertEqual((xs[0] + xs[-1] + 1) / 2, 16)
        self.assertIn(2, out)
        self.assertTrue(all(any(out[y * 32 + x] == 3 for x in xs) for y in ys))   # (the button kept)
        self.assertEqual(ball_sprites.carve([0] * 1024), [0] * 1024)

    def test_open_facing_north(self):
        """A ball's 16 textures: the 4 facing north (Platinum's NAME.1-4)
        show its open picture, the rest its closed one."""
        data = ball_sprites.read_file(ball_sprites.make_file(made_up_archive()))[3]
        tex = struct.unpack_from("<I", data, 16)[0]
        d = tex + struct.unpack_from("<H", data, tex + 0x0E)[0]
        n = data[d + 1]
        at = d + 8 + 4 * (n + 1) + 4
        names = [data[at + 8 * n + 16 * i:at + 8 * n + 16 * i + 16].rstrip(b"\0") for i in range(n)]
        offsets = {int(names[i].rsplit(b".", 1)[1]): struct.unpack_from("<I", data, at + 8 * i)[0] & 0xFFFF for i in range(n)}
        self.assertEqual(len(offsets), 16)
        for k, off in offsets.items():
            self.assertEqual(off, 64 if k <= 4 else 0, k)   # (the second picture at 512 bytes)

    def test_emotes(self):
        for name in ball_sprites.EMOTES:
            texels = ball_sprites.emote_picture(name)
            self.assertEqual(len(texels), 32 * 32)
            self.assertTrue(all(0 <= c < 16 for c in texels))
            # the bubble's tail's tip on the texture's bottom row, nothing above its top
            self.assertEqual(texels[31 * 32 + 8 + 7], 1)
            self.assertEqual(sum(1 for c in texels[:16 * 32] if c), 0)

    def test_rejects(self):
        with self.assertRaises(ball_sprites.BallSpritesError):
            ball_sprites.make_file(b"NOPE" + b"\0" * 60)
        with self.assertRaises(ball_sprites.BallSpritesError):
            ball_sprites.make_file(narc([b""] * 10))   # (too short)
        with self.assertRaises(ball_sprites.BallSpritesError):
            ball_sprites.read_file(b"PFOL" + b"\0" * 12)

    def test_lz10(self):
        self.assertEqual(ball_sprites.lz10(lz10(b"abcdefghijk")), b"abcdefghijk")
        # a back reference: "abc" then 6 copied from 3 back
        self.assertEqual(ball_sprites.lz10(bytes([0x10, 9, 0, 0, 0x10]) + b"abc" + bytes([0x30, 0x02])), b"abcabcabc")


if __name__ == "__main__":
    unittest.main()
