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


def texture_size(data: bytes) -> tuple[int, int]:
    """(width, height) of an NSBTX of FRAMES square 16-colour textures, all
    one size; a FollowersError for anything else."""
    try:
        if data[:4] != b"BTX0":
            raise FollowersError("not an NSBTX")
        tex = struct.unpack_from("<I", data, 16)[0]
        if data[tex:tex + 4] != b"TEX0":
            raise FollowersError("no TEX0 block")
        info = tex + struct.unpack_from("<H", data, tex + 0x0E)[0]
        count = data[info + 1]
        if count != FRAMES:
            raise FollowersError(f"{count} textures, not {FRAMES}")
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


def make_bundle(sprites: list[bytes]) -> bytes:
    """The data pack's follower file from the fetched sprites, in order
    (each checked). Layout (little endian):

        0   'PFOL', u16 version 1, u16 species S (493), u32 sprites N, u32 0
        16  S x {u16 first sprite, u8 forms after it, u8 females' own (1/0)}
            N x {u32 offset, u32 size, u8 width, u8 height, u16 0}
            the sprites (HeartGold's NSBTX files as they are), each at a
            4-byte boundary

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


def fetch(progress=None, workers: int = 8) -> list[bytes]:
    """Every sprite, downloaded (urllib: the proxy settings of the
    environment) and checked. OSError or FollowersError if one can't be."""
    import urllib.request

    def get(n: int) -> bytes:
        with urllib.request.urlopen(sprite_url(n), timeout=60) as response:
            data = response.read()
        check_file(n, data)
        return data

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
    args = parser.parse_args()
    if args.output.is_file():   # (kept from a build before: downloaded again only if re-pinned)
        try:
            kept = read_bundle(args.output.read_bytes())
            if len(kept["sprites"]) != sprite_count():
                raise FollowersError("another pin")
            for n, sprite in enumerate(kept["sprites"]):
                check_file(n, sprite["data"])
        except FollowersError:
            pass
        else:
            print(f"Follower sprites already in {args.output}")
            return 0
    print(f"Downloading the follower sprites ({sprite_count()} files, {sources.REPO}@{sources.COMMIT[:12]})...")
    try:
        bundle = make_bundle(fetch())
    except (OSError, FollowersError) as e:
        print(f"Couldn't get the follower sprites: {e}", file=sys.stderr)
        print("The game works without them (only Platinum's own Pokémon follow).", file=sys.stderr)
        return 2
    args.output.write_bytes(bundle)
    print(f"Wrote {args.output} ({len(bundle)} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
