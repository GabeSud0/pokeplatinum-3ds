#!/usr/bin/env python3
"""The follower Pokémon's Poké Balls, for the data pack: each kind of ball
as the battles draw it, made from the player's own ROM.

When the follower comes out of its ball or goes back in (OPTIONS:
FOLLOWER), the ball it was caught in is seen. The battles' ball sprites
(battle/graphic/pl_batt_obj.narc: a ball's tiles, palette, cells) are
turned here into overworld sprites, one for each of the 17 kinds, at
about 3/4 of their size (SCALE: the overworld item ball's), laid out as
the follower sprites are (Platinum's walking sprites: 16 textures of 32 x
32, builder/followers.py's platinum_layout), the ball's bottom at the
texture's: facing north it's open, any other way closed, so the game
opens and closes it by turning it (one object, one texture: nothing
loaded or made again as it opens). pack.py and web.py put the file in the
data pack (file ID PAK_ID) with the follower sprites. Nothing here is game
data: the pictures come from the ROM as the player builds.

The file (little endian):

    0   'PFBL', u16 version 2, u16 0, u32 sprites N (22), u32 0
    16  N x {u32 offset, u32 size}
        the sprites, each at a 4-byte boundary: ball k at k (the game's
        ball numbers less one: 0 the Master Ball, 3 the Poké Ball ... 16
        the Park Ball); then the emote bubbles (EMOTES, in that order)

The emote bubbles the follower shows when it's talked to (a heart, a
note, "...", "!", "?") are this file's own drawings (_BUBBLE, _SYMBOLS), not the
game's: plain symbols in a speech bubble, 16 x 16 at the texture's bottom
middle.
"""
from __future__ import annotations

import struct

try:
    from . import followers
except ImportError:
    import followers

PAK_ID = 0x10000001
MAGIC = b"PFBL"
VERSION = 2
NARC_PATH = "battle/graphic/pl_batt_obj.narc"
SIZE = 32
ORIGIN = (16, 24)            # the cells' (0, 0) in the texture: the ball's bottom at its bottom
CLOSED_CELL, OPEN_CELL = 0, 9
# The battles' balls are bigger than the overworld's item ball (14 pixels
# across to its 12): drawn smaller (nearest texel, from the bottom middle),
# so they stay a texel a pixel and sharp. (4/5 looked a little big on the
# 3DS: 2026-10-29.)
SCALE = 0.74

# The battles' ball throw graphics (src/battle_anim/ov12_02235E94.c,
# sBallThrowGraphics; res/graphics/battle/sprites.naix): (tiles, palette,
# cells) for each ball, in the game's ball order.
BALLS = (
    (288, 92, 287),    # Master Ball
    (291, 93, 290),    # Ultra Ball
    (294, 94, 293),    # Great Ball
    (285, 91, 284),    # Poké Ball
    (297, 95, 296),    # Safari Ball
    (300, 96, 299),    # Net Ball
    (303, 97, 302),    # Dive Ball
    (306, 98, 305),    # Nest Ball
    (309, 99, 308),    # Repeat Ball
    (312, 100, 311),   # Timer Ball
    (315, 101, 314),   # Luxury Ball
    (318, 102, 317),   # Premier Ball
    (324, 104, 323),   # Dusk Ball
    (327, 105, 326),   # Heal Ball
    (321, 103, 320),   # Quick Ball
    (330, 106, 329),   # Cherish Ball
    (333, 107, 332),   # Park Ball
)

# OBJ shape x size: (width, height)
OBJ_SIZES = {(0, 0): (8, 8), (0, 1): (16, 16), (0, 2): (32, 32), (0, 3): (64, 64),
             (1, 0): (16, 8), (1, 1): (32, 8), (1, 2): (32, 16), (1, 3): (64, 32),
             (2, 0): (8, 16), (2, 1): (8, 32), (2, 2): (16, 32), (2, 3): (32, 64)}


class BallSpritesError(ValueError):
    pass


def narc_members(data: bytes) -> list[bytes]:
    try:
        if data[:4] != b"NARC" or data[16:20] != b"BTAF":
            raise BallSpritesError("not a NARC")
        off = 16
        size, count = struct.unpack_from("<II", data, off + 4)
        fat = [struct.unpack_from("<II", data, off + 12 + 8 * i) for i in range(count)]
        off += size
        if data[off:off + 4] != b"BTNF":
            raise BallSpritesError("NARC without its names block")
        off += struct.unpack_from("<I", data, off + 4)[0]
        if data[off:off + 4] != b"GMIF":
            raise BallSpritesError("NARC without its files block")
        base = off + 8
        return [data[base + s:base + e] for s, e in fat]
    except struct.error as e:
        raise BallSpritesError(f"truncated NARC ({e})") from None


def lz10(data: bytes) -> bytes:
    """The DS BIOS's LZ77 (type 0x10) decompressed."""
    if not data or data[0] != 0x10:
        raise BallSpritesError("not LZ77 data")
    size = data[1] | data[2] << 8 | data[3] << 16
    out = bytearray()
    i = 4
    try:
        while len(out) < size:
            flags = data[i]
            i += 1
            for bit in range(8):
                if len(out) >= size:
                    break
                if flags & (0x80 >> bit):
                    pair = data[i] << 8 | data[i + 1]
                    i += 2
                    length, back = (pair >> 12) + 3, (pair & 0xFFF) + 1
                    if back > len(out):
                        raise BallSpritesError("LZ77 data reaches back before its start")
                    for _ in range(length):
                        out.append(out[-back])
                else:
                    out.append(data[i])
                    i += 1
    except IndexError:
        raise BallSpritesError("truncated LZ77 data") from None
    return bytes(out[:size])


def _blocks(data: bytes) -> dict[bytes, bytes]:
    _, _, _, _, header, count = struct.unpack_from("<4sHHIHH", data, 0)
    off, out = header, {}
    for _ in range(count):
        magic = data[off:off + 4]
        size = struct.unpack_from("<I", data, off + 4)[0]
        if size < 8:
            raise BallSpritesError("bad block size")
        out[magic] = data[off + 8:off + size]
        off += size
    return out


def read_tiles(ncgr: bytes) -> bytes:
    """A 16-colour NCGR's tiles (32 bytes each)."""
    b = _blocks(ncgr).get(b"RAHC")
    if b is None:
        raise BallSpritesError("no tiles block")
    _, _, depth, _, _, size, off = struct.unpack_from("<HHIIIII", b, 0)
    if depth != 3:
        raise BallSpritesError("not 16-colour tiles")
    return b[off:off + size]


def read_palette(nclr: bytes) -> list[int]:
    b = _blocks(nclr).get(b"TTLP")
    if b is None:
        raise BallSpritesError("no palette block")
    _, _, size, off = struct.unpack_from("<IIII", b, 0)
    raw = b[off:off + size]
    return [struct.unpack_from("<H", raw, 2 * i)[0] & 0x7FFF for i in range(len(raw) // 2)]


def read_cells(ncer: bytes) -> list[list[tuple[int, int, int]]]:
    """A cell bank's cells: each its OAM entries (attributes 0, 1, 2)."""
    b = _blocks(ncer).get(b"KBEC")
    if b is None:
        raise BallSpritesError("no cells block")
    count, attr, cell_off = struct.unpack_from("<HHI", b, 0)
    size = 16 if attr == 1 else 8
    oam_base = cell_off + count * size
    cells = []
    for i in range(count):
        n, _, oam_off = struct.unpack_from("<HHI", b, cell_off + i * size)
        cells.append([struct.unpack_from("<HHH", b, oam_base + oam_off + 6 * k) for k in range(n)])
    return cells


def draw_cell(cell, tiles: bytes, shift: int = 1) -> list[int]:
    """The cell's colour indices on a SIZE x SIZE texture, its (0, 0) at
    ORIGIN (0: transparent). shift: the OBJ tile numbers' unit (1D mapping
    at 64-byte boundaries: 2 tiles)."""
    texels = [0] * (SIZE * SIZE)
    for a0, a1, a2 in cell:
        y = a0 & 0xFF
        y = y - 256 if y >= 128 else y
        x = a1 & 0x1FF
        x = x - 512 if x >= 256 else x
        w, h = OBJ_SIZES[(a0 >> 14, a1 >> 14)]
        hflip, vflip = (a1 >> 12) & 1, (a1 >> 13) & 1
        first = (a2 & 0x3FF) << shift
        for ty in range(h // 8):
            for tx in range(w // 8):
                at = (first + ty * (w // 8) + tx) * 32
                tile = tiles[at:at + 32]
                if len(tile) < 32:
                    raise BallSpritesError("a cell's tile past the tiles")
                for py in range(8):
                    for px in range(8):
                        byte = tile[py * 4 + px // 2]
                        c = byte >> 4 if px & 1 else byte & 15
                        if not c:
                            continue
                        dx, dy = tx * 8 + px, ty * 8 + py
                        if hflip:
                            dx = w - 1 - dx
                        if vflip:
                            dy = h - 1 - dy
                        X, Y = ORIGIN[0] + x + dx, ORIGIN[1] + y + dy
                        if 0 <= X < SIZE and 0 <= Y < SIZE:
                            texels[Y * SIZE + X] = c
    return texels


def shrink(texels: list[int], scale: float = SCALE) -> list[int]:
    """The picture at `scale`, each texel the nearest of the original's,
    its bottom middle (ORIGIN's column, the texture's bottom) where it was."""
    ox, oy = ORIGIN[0], SIZE - 1
    out = [0] * (SIZE * SIZE)
    for y in range(SIZE):
        for x in range(SIZE):
            X = int(round(ox + (x - ox + 0.5) / scale - 0.5))
            Y = int(round(oy + (y - oy + 0.5) / scale - 0.5))
            if 0 <= X < SIZE and 0 <= Y < SIZE:
                out[y * SIZE + x] = texels[Y * SIZE + X]
    return out


def nsbtx(texels: list[int], palette: list[int], north: list[int] | None = None) -> bytes:
    """One 16-colour SIZE x SIZE picture as Platinum's walking sprites (16
    textures sharing it), through followers.platinum_layout: made first as
    HeartGold's followers are (8 textures, two palettes, both this one).
    north: another picture for the north-facing textures (a ball's open
    one: the ball opens by turning north, the same object and texture)."""
    code = {8: 0, 16: 1, 32: 2, 64: 3}[SIZE]
    pictures = [texels] + ([north] if north is not None else [])
    tex_data = b"".join(bytes(t[i] | t[i + 1] << 4 for i in range(0, len(t), 2)) for t in pictures)
    pal = b"".join(struct.pack("<H", c) for c in (palette + [0] * 16)[:16])
    names = sorted(b"tsure_poke.%d" % k for k in [1] + list(range(10, 17)))
    second = (SIZE * SIZE // 2) >> 3 if north is not None else 0   # (the north picture's offset, in 8 bytes)
    # (HeartGold's frames: up, up, down, down, left, left, right, right)
    params = [struct.pack("<II", (3 << 26) | (code << 20) | (code << 23) | (second if i < 2 else 0), 0) for i in range(8)]
    tex_dict = followers._dictionary(names, params)
    pltt_dict = followers._dictionary([b"tsure_poke0", b"tsure_poke1"], [struct.pack("<HH", 0, 0), struct.pack("<HH", 4, 0)])
    pltt_data = pal + pal
    header = bytearray(0x3C)
    header[0:4] = b"TEX0"
    td = 0x3C + len(tex_dict) + len(pltt_dict)
    pd = td + len(tex_data)
    struct.pack_into("<HH", header, 0x0C, len(tex_data) >> 3, 0x3C)
    struct.pack_into("<I", header, 0x14, td)
    struct.pack_into("<II", header, 0x24, pd, pd)
    struct.pack_into("<H", header, 0x30, len(pltt_data) >> 3)
    struct.pack_into("<II", header, 0x34, 0x3C + len(tex_dict), pd)
    body = bytes(header) + tex_dict + pltt_dict + tex_data + pltt_data
    struct.pack_into("<I", header, 0x04, len(body))
    body = bytes(header) + body[0x3C:]
    hg = b"BTX0" + struct.pack("<HHIHH", 0xFEFF, 1, 20 + len(body), 16, 1) + struct.pack("<I", 20) + body
    return followers.platinum_layout(hg)


def ball_pictures(narc: bytes) -> list[tuple[list[int], list[int], list[int]]]:
    """(closed texels, open texels, palette) for each ball."""
    members = narc_members(narc)
    out = []
    for tiles_id, palette_id, cells_id in BALLS:
        try:
            tiles = read_tiles(lz10(members[tiles_id]))
            palette = read_palette(members[palette_id])
            cells = read_cells(lz10(members[cells_id]))
        except IndexError:
            raise BallSpritesError("the battle graphics archive is too short") from None
        if len(cells) <= OPEN_CELL:
            raise BallSpritesError("a ball with too few cells")
        out.append((shrink(draw_cell(cells[CLOSED_CELL], tiles)), shrink(draw_cell(cells[OPEN_CELL], tiles)), palette))
    return out


EMOTES = ("heart", "note", "dots", "exclaim", "question")

# The bubbles: '#' outline, 'w' white, 's' its shade, 'r' red, 'h' red's
# light, 'k' the symbols' dark. Each its 16 x 16 in the texture's bottom
# middle.
EMOTE_COLOURS = {".": 0, "#": 1, "w": 2, "s": 3, "r": 4, "h": 5, "k": 6}
EMOTE_PALETTE = [0, 0x0842, 0x7FFF, 0x5EF7, 0x1CFF, 0x4E7F, 0x1CE7] + [0] * 9
_BUBBLE = [
    "...##########...",
    "..#wwwwwwwwwww#.",
    ".#wwwwwwwwwwwww#",
    ".#wwwwwwwwwwwww#",
    ".#wwwwwwwwwwwww#",
    ".#wwwwwwwwwwwww#",
    ".#wwwwwwwwwwwww#",
    ".#wwwwwwwwwwwww#",
    ".#wwwwwwwwwwwww#",
    ".#wwwwwwwwwwwww#",
    ".#swwwwwwwwwwws#",
    "..#sssswwssssss#",
    "...####ws######.",
    "......#ws#......",
    "......#s#.......",
    ".......#........",
]
_SYMBOLS = {
    "heart": [
        "....hh...rr.....",
        "...hhrr.rrrr....",
        "...hrrrrrrrr....",
        "...rrrrrrrrr....",
        "....rrrrrrr.....",
        ".....rrrrr......",
        "......rrr.......",
        ".......r........",
    ],
    "note": [
        "........kkkk....",
        "........k..kk...",
        "........k...k...",
        "........k.......",
        "......kkk.......",
        ".....kkkk.......",
        ".....kkkk.......",
        "......kk........",
    ],
    "dots": [
        "................",
        "................",
        "................",
        "................",
        "....kk.kk.kk....",
        "....kk.kk.kk....",
        "................",
        "................",
    ],
    "exclaim": [
        ".......rr.......",
        ".......rr.......",
        ".......rr.......",
        ".......rr.......",
        ".......rr.......",
        "................",
        ".......rr.......",
        ".......rr.......",
    ],
    "question": [
        "......kkkk......",
        ".....kk..kk.....",
        ".........kk.....",
        "........kk......",
        ".......kk.......",
        "................",
        ".......kk.......",
        ".......kk.......",
    ],
}


def emote_picture(name: str) -> list[int]:
    """An emote bubble's colour indices on a SIZE x SIZE texture."""
    rows = [list(r) for r in _BUBBLE]
    for y, line in enumerate(_SYMBOLS[name]):
        for x, c in enumerate(line):
            if c != ".":
                rows[2 + y][x] = c
    texels = [0] * (SIZE * SIZE)
    left, top = (SIZE - 16) // 2, SIZE - 16
    for y, line in enumerate(rows):
        for x, c in enumerate(line):
            texels[(top + y) * SIZE + left + x] = EMOTE_COLOURS[c]
    return texels


def make_file(narc: bytes) -> bytes:
    """The data pack's ball file from the ROM's battle graphics archive."""
    sprites = []
    for closed, opened, palette in ball_pictures(narc):
        sprites.append(nsbtx(closed, palette, north=opened))
    sprites += [nsbtx(emote_picture(name), EMOTE_PALETTE) for name in EMOTES]
    head = struct.pack("<4sHHII", MAGIC, VERSION, 0, len(sprites), 0)
    offset = len(head) + 8 * len(sprites)
    directory, data = [], []
    for sprite in sprites:
        offset = (offset + 3) & ~3
        directory.append(struct.pack("<II", offset, len(sprite)))
        data.append((offset, sprite))
        offset += len(sprite)
    out = bytearray(head + b"".join(directory))
    for at, sprite in data:
        out += b"\0" * (at - len(out))
        out += sprite
    out += b"\0" * (-len(out) % 4)
    return bytes(out)


def read_file(data: bytes) -> list[bytes]:
    """The file's sprites, checked."""
    if len(data) < 16:
        raise BallSpritesError("ball file too short")
    magic, version, _, count, _ = struct.unpack_from("<4sHHII", data, 0)
    if magic != MAGIC or version != VERSION:
        raise BallSpritesError("not a ball file (version 1)")
    out = []
    for i in range(count):
        offset, size = struct.unpack_from("<II", data, 16 + 8 * i)
        if offset % 4 or offset + size > len(data):
            raise BallSpritesError(f"ball sprite {i} outside the file")
        out.append(data[offset:offset + size])
    return out
