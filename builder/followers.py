#!/usr/bin/env python3
"""The follower Pokémon's sprites, for the data pack.

    python3 builder/followers.py -o followers.bin

Your lead Pokémon walks behind you (OPTIONS: FOLLOWER) on HeartGold and
SoulSilver's overworld sprites. They are Nintendo, Game Freak and
Creatures' work, never in this repository or its releases: this downloads
them on your computer from pret/pokeheartgold at a pinned commit
(builder/follower_sources.py, made by tools/make_follower_sources.py),
checks each file's size and SHA-256, and bundles them into one file that
builder/pack.py --followers puts in the data pack (file ID PAK_ID,
docs/PAK_FORMAT.md). Without them (no Internet connection) the game is
the same, and only the Pokémon Platinum has its own walking sprites for
follow you.

The web builder fetches the same files in the browser
(web/js/worker.mjs) and calls make_bundle() itself.
"""
from __future__ import annotations

import argparse
import hashlib
import struct
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

try:
    from . import follower_sources as sources
except ImportError:
    import follower_sources as sources

PAK_ID = 0x10000000          # above every NitroFS file ID
MAGIC = b"PFOL"
VERSION = 1
HEADER = struct.Struct("<4sHHII")      # magic, version, species, sprites, 0
SPECIES_ENTRY = struct.Struct("<HBB")  # first sprite, forms after it, females' own
SPRITE_ENTRY = struct.Struct("<IIBBH")  # offset, size, width, height, 0
FRAMES = 8                   # up, down, left, right: two frames each
SIZES = (32, 64)


class FollowersError(ValueError):
    pass


def sprite_count() -> int:
    return len(sources.FILES)


def sprite_url(n: int) -> str:
    """Where sprite n is fetched from (raw.githubusercontent.com, which
    browsers may fetch from any page)."""
    path = sources.SPRITE_PATH.format(sources.FIRST_FILE + n)
    return f"https://raw.githubusercontent.com/{sources.REPO}/{sources.COMMIT}/{path}"


def check_file(n: int, data: bytes) -> None:
    """A FollowersError unless data is sprite n as pinned."""
    size, digest = sources.FILES[n]
    if len(data) != size or hashlib.sha256(data).hexdigest() != digest:
        raise FollowersError(f"follower sprite {n} isn't the pinned file (size {len(data)}, expected {size})")


def texture_size(data: bytes, frames: int = FRAMES) -> tuple[int, int]:
    """(width, height) of an NSBTX of `frames` square 16-colour textures,
    all one size; a FollowersError for anything else."""
    try:
        if data[:4] != b"BTX0":
            raise FollowersError("not an NSBTX")
        tex = struct.unpack_from("<I", data, 16)[0]
        if data[tex:tex + 4] != b"TEX0":
            raise FollowersError("no TEX0 block")
        info = tex + struct.unpack_from("<H", data, tex + 0x0E)[0]
        count = data[info + 1]
        if count != frames:
            raise FollowersError(f"{count} textures, not {frames}")
        # The info dictionary: its header, the tree's header and count + 1
        # nodes, the entries' size and offset (u16 each), the entries.
        entries = info + 4 + 4 + 4 * (count + 1) + 4
        sizes = set()
        for i in range(count):
            param = struct.unpack_from("<I", data, entries + 8 * i)[0]
            fmt = (param >> 26) & 7
            w, h = 8 << ((param >> 20) & 7), 8 << ((param >> 23) & 7)
            if fmt != 3:
                raise FollowersError(f"texture format {fmt}, not 16 colours")
            sizes.add((w, h))
    except (struct.error, IndexError) as e:
        raise FollowersError(f"truncated NSBTX ({e})") from None
    if len(sizes) != 1:
        raise FollowersError("textures of different sizes")
    w, h = sizes.pop()
    if w != h or w not in SIZES:
        raise FollowersError(f"textures {w} x {h}")
    return w, h


# Platinum's overworld people and Pokémon have 16 textures, 4 a direction
# (north, south, west, east; named NAME.1 to NAME.16, the dictionary in
# name order), and its walk (generic_walk: BILLBOARD_FRAME_SEQ_GENERIC_WALK)
# shows them by index. HeartGold's followers have 8, 2 a direction (up,
# down, left, right), and two palettes: normal, shiny. So each is laid out
# as Platinum's (platinum_layout): 16 entries, entry NAME.k showing frame
# 2 * direction + (step % 2), the textures' pixels shared. With Platinum's
# walk the follower walks as its people do, its frames A B A B.
PLATINUM_FRAMES = 16
NAME = b"tsure_poke"


def _name_bits(name: bytes) -> int:
    return int.from_bytes(name.ljust(16, b"\0")[:16], "little")


def dictionary_tree(names: list[bytes]) -> list[tuple[int, int, int, int]]:
    """The NNS dictionary's Patricia tree for its names (by index): node 0
    the header, each (bit tested, left, right, entry). NNS_G3dGetResDictIdxByName
    walks it; the billboards don't (they go by index), but it's kept right."""
    keys = [_name_bits(n) for n in names]
    nodes = [[127, 0, 0, 0]]
    node_key = [0]

    def bit(value: int, b: int) -> int:
        return (value >> b) & 1

    for entry, key in enumerate(keys):
        p, x = 0, nodes[0][1]
        while nodes[p][0] > nodes[x][0]:
            p, x = x, nodes[x][2] if bit(key, nodes[x][0]) else nodes[x][1]
        i = 127
        while i >= 0 and bit(key, i) == bit(node_key[x], i):
            i -= 1
        if i < 0:
            raise FollowersError(f"names {names[entry]!r} twice")
        p, x = 0, nodes[0][1]
        while nodes[p][0] > nodes[x][0] > i:
            p, x = x, nodes[x][2] if bit(key, nodes[x][0]) else nodes[x][1]
        t = len(nodes)
        nodes.append([i, x if bit(key, i) else t, t if bit(key, i) else x, entry])
        node_key.append(key)
        if p == 0:
            nodes[0][1] = t
        elif bit(key, nodes[p][0]):
            nodes[p][2] = t
        else:
            nodes[p][1] = t
    return [tuple(n) for n in nodes]


def dictionary_lookup(tree, names: list[bytes], name: bytes) -> int:
    """The entry of name, as NNS looks it up (-1: none). For the tests."""
    key = _name_bits(name)
    p, x = tree[0], tree[tree[0][1]]
    while p[0] > x[0]:
        p, x = x, tree[x[2]] if (key >> x[0]) & 1 else tree[x[1]]
    return x[3] if names[x[3]].ljust(16, b"\0") == name.ljust(16, b"\0") else -1


def _dictionary(names: list[bytes], entries: list[bytes]) -> bytes:
    n, size = len(names), len(entries[0])
    tree = dictionary_tree(names)
    tree_part = struct.pack("<HH", 8, 8 + 4 * (n + 1)) + b"".join(struct.pack("<4B", *node) for node in tree)
    entry_part = struct.pack("<HH", size, 4 + size * n) + b"".join(entries) + b"".join(m.ljust(16, b"\0") for m in names)
    return struct.pack("<BBH", 0, n, 4 + len(tree_part) + len(entry_part)) + tree_part + entry_part


def platinum_layout(data: bytes) -> bytes:
    """A HeartGold follower's NSBTX (texture_size checked) laid out as
    Platinum's walking sprites (PLATINUM_FRAMES textures, see above); the
    pixels and both palettes as they are."""
    texture_size(data)
    tex = struct.unpack_from("<I", data, 16)[0]
    block = data[tex:tex + struct.unpack_from("<I", data, tex + 4)[0]]
    tex_dict = struct.unpack_from("<H", block, 0x0E)[0]
    tex_data = struct.unpack_from("<I", block, 0x14)[0]
    pltt_dict = struct.unpack_from("<I", block, 0x34)[0]
    pltt_data = struct.unpack_from("<I", block, 0x38)[0]
    tex_size = struct.unpack_from("<H", block, 0x0C)[0] << 3
    pltt_size = struct.unpack_from("<H", block, 0x30)[0] << 3
    count = block[tex_dict + 1]
    at = tex_dict + 8 + 4 * (count + 1) + 4
    params = [block[at + 8 * i:at + 8 * i + 8] for i in range(count)]
    pltt = block[pltt_dict:pltt_dict + struct.unpack_from("<H", block, pltt_dict + 2)[0]]
    if not (tex_dict < pltt_dict < tex_data and tex_data + tex_size <= pltt_data and pltt_data + pltt_size <= len(block)):
        raise FollowersError("NSBTX blocks out of order")

    names = sorted(NAME + b".%d" % k for k in range(1, PLATINUM_FRAMES + 1))
    frames = []
    for name in names:
        k = int(name.rsplit(b".", 1)[1]) - 1
        frames.append(params[2 * (k // 4) + (k % 2)])
    new_tex_dict = _dictionary(names, frames)

    header = bytearray(block[:0x3C])
    new_pltt_dict = 0x3C + len(new_tex_dict)
    new_tex_data = new_pltt_dict + len(pltt)
    new_pltt_data = new_tex_data + tex_size
    struct.pack_into("<H", header, 0x0E, 0x3C)
    struct.pack_into("<I", header, 0x14, new_tex_data)
    struct.pack_into("<I", header, 0x24, new_pltt_data)   # (no 4x4 textures: at the palettes, as theirs)
    struct.pack_into("<I", header, 0x28, new_pltt_data)
    struct.pack_into("<I", header, 0x34, new_pltt_dict)
    struct.pack_into("<I", header, 0x38, new_pltt_data)
    body = (bytes(header) + new_tex_dict + pltt + block[tex_data:tex_data + tex_size]
            + block[pltt_data:pltt_data + pltt_size])
    struct.pack_into("<I", header, 0x04, len(body))
    body = bytes(header) + body[0x3C:]
    head = bytearray(data[:16])
    struct.pack_into("<I", head, 8, 16 + 4 + len(body))
    struct.pack_into("<HH", head, 12, 16, 1)
    return bytes(head) + struct.pack("<I", 20) + body


def make_bundle(sprites: list[bytes]) -> bytes:
    """The data pack's follower file from the fetched sprites, in order
    (each checked). Layout (little endian):

        0   'PFOL', u16 version 1, u16 species S (493), u32 sprites N, u32 0
        16  S x {u16 first sprite, u8 forms after it, u8 females' own (1/0)}
            N x {u32 offset, u32 size, u8 width, u8 height, u16 0}
            the sprites (HeartGold's NSBTX files laid out as Platinum's
            walking sprites: platinum_layout), each at a 4-byte boundary

    Species s's sprite: first + 1 for a female when females' own is 1, else
    first + form when form <= forms (FollowMon_GetSpriteID's way)."""
    if len(sprites) != sprite_count():
        raise FollowersError(f"{len(sprites)} sprites, not {sprite_count()}")
    species = len(sources.MODEL)
    head = HEADER.pack(MAGIC, VERSION, species, len(sprites), 0)
    table = b"".join(SPECIES_ENTRY.pack(m, f, g) for m, f, g in zip(sources.MODEL, sources.FORM_MAX, sources.FEMALE))
    offset = len(head) + len(table) + SPRITE_ENTRY.size * len(sprites)
    directory, data = [], []
    for n, sprite in enumerate(sprites):
        check_file(n, sprite)
        w, h = texture_size(sprite)
        sprite = platinum_layout(sprite)
        offset = (offset + 3) & ~3
        directory.append(SPRITE_ENTRY.pack(offset, len(sprite), w, h, 0))
        data.append((offset, sprite))
        offset += len(sprite)
    out = bytearray(head + table + b"".join(directory))
    for at, sprite in data:
        out += b"\0" * (at - len(out))
        out += sprite
    out += b"\0" * (-len(out) % 4)
    return bytes(out)


def read_bundle(bundle: bytes) -> dict:
    """The bundle's parts, checked (the tests' and pack.py's look)."""
    if len(bundle) < HEADER.size:
        raise FollowersError("follower file too short")
    magic, version, species, count, _ = HEADER.unpack_from(bundle, 0)
    if magic != MAGIC or version != VERSION:
        raise FollowersError("not a follower file (version 1)")
    at = HEADER.size
    table = [SPECIES_ENTRY.unpack_from(bundle, at + SPECIES_ENTRY.size * i) for i in range(species)]
    at += SPECIES_ENTRY.size * species
    sprites = []
    for i in range(count):
        offset, size, w, h, _ = SPRITE_ENTRY.unpack_from(bundle, at + SPRITE_ENTRY.size * i)
        if offset % 4 or offset + size > len(bundle):
            raise FollowersError(f"sprite {i} outside the file")
        sprites.append({"data": bundle[offset:offset + size], "width": w, "height": h})
    for s, (first, forms, female) in enumerate(table):
        if first + max(forms, female) >= count:
            raise FollowersError(f"species {s + 1}'s sprite outside the file")
    return {"species": table, "sprites": sprites}


def fetch(progress=None, workers: int = 8, cache: Path | None = None) -> list[bytes]:
    """Every sprite, downloaded (urllib: the proxy settings of the
    environment) and checked. With a cache folder, the files already there
    and right aren't downloaded again, and the others are kept there.
    OSError or FollowersError if one can't be had."""
    import urllib.request

    def get(n: int) -> bytes:
        kept = cache / f"{sources.FIRST_FILE + n:08d}.NSBTX" if cache is not None else None
        if kept is not None and kept.is_file():
            data = kept.read_bytes()
            try:
                check_file(n, data)
                return data
            except FollowersError:
                pass   # (another pin's: downloaded again)
        with urllib.request.urlopen(sprite_url(n), timeout=60) as response:
            data = response.read()
        check_file(n, data)
        if kept is not None:
            kept.write_bytes(data)
        return data

    if cache is not None:
        cache.mkdir(parents=True, exist_ok=True)

    count = sprite_count()
    out = [b""] * count
    with ThreadPoolExecutor(max_workers=workers) as pool:
        for done, (n, data) in enumerate(zip(range(count), pool.map(get, range(count))), 1):
            out[n] = data
            if progress is not None:
                progress(done, count)
    return out


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("-o", "--output", type=Path, default=Path("followers.bin"))
    parser.add_argument("--cache", type=Path,
                        help="where the downloaded files are kept between builds (default: beside the output, follower-sprites)")
    args = parser.parse_args()
    print(f"The follower sprites ({sprite_count()} files, {sources.REPO}@{sources.COMMIT[:12]}): downloading the ones not kept...")
    try:
        bundle = make_bundle(fetch(cache=args.cache or args.output.parent / "follower-sprites"))
    except (OSError, FollowersError) as e:
        print(f"Couldn't get the follower sprites: {e}", file=sys.stderr)
        print("The game works without them (only Platinum's own Pokémon follow).", file=sys.stderr)
        return 2
    args.output.write_bytes(bundle)
    print(f"Wrote {args.output} ({len(bundle)} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
