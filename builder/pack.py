#!/usr/bin/env python3
"""Pack an extracted NitroFS tree into a versioned, ID-indexed SD-card file.

Only local, user-provided game data is read. Never commit the resulting .pak.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import struct
import tempfile
from collections.abc import Collection
from pathlib import Path

MAGIC = b"PLATPAK\0"
# The build ID the game program expects (PLATINUM_PAK_BUILD_ID in
# 3ds_port/game/app/platinum.h): packs of the ROM's files, unchanged.
DEFAULT_BUILD_ID = "platinum-nitrofs-1"
VERSION = 1
BUILD_ID_BYTES = 32
HEADER = struct.Struct("<8sI32s20sI")
ENTRY = struct.Struct("<III")
MAX_U32 = (1 << 32) - 1
SHA1_PATTERN = re.compile(r"[0-9a-fA-F]{40}\Z")


def _align4(value: int) -> int:
    return (value + 3) & ~3


def _build_id_bytes(value: str) -> bytes:
    if not isinstance(value, str) or not value or "\0" in value:
        raise ValueError("Build ID must be a nonempty UTF-8 string without NUL")
    raw = value.encode("utf-8")
    if len(raw) > BUILD_ID_BYTES:
        raise ValueError(f"Build ID exceeds {BUILD_ID_BYTES} UTF-8 bytes")
    return raw.ljust(BUILD_ID_BYTES, b"\0")


def _sha1_bytes(value: str) -> bytes:
    if not isinstance(value, str) or not SHA1_PATTERN.fullmatch(value):
        raise ValueError("Source ROM SHA-1 must be exactly 40 hex characters")
    return bytes.fromhex(value)


def _source_path(root: Path, relative: str) -> Path:
    """Reject traversal, absolute paths, backslashes and escaping symlinks."""
    if (not isinstance(relative, str) or not relative or "\0" in relative
            or "\\" in relative or relative.startswith("/")
            or any(part in ("", ".", "..") for part in relative.split("/"))
            or ":" in relative.split("/")[0]):
        raise ValueError(f"Unsafe manifest path: {relative!r}")
    path = (root / relative).resolve()
    if not path.is_relative_to(root):
        raise ValueError(f"Manifest path escapes data pack: {relative!r}")
    if not path.is_file():
        raise ValueError(f"Missing manifest file: {relative!r}")
    return path


def _load_manifest(root: Path) -> tuple[list[tuple[int, Path, int]], dict]:
    manifest_path = root / "manifest.json"
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise ValueError(f"Cannot read manifest.json: {exc}") from exc
    if not isinstance(manifest, dict) or not isinstance(manifest.get("files"), list):
        raise ValueError("manifest.json must contain a 'files' array")
    entries = []
    ids: set[int] = set()
    paths: set[str] = set()
    for item in manifest["files"]:
        if not isinstance(item, dict):
            raise ValueError("Each manifest file must be an object")
        file_id, relative, size = item.get("id"), item.get("path"), item.get("size")
        if type(file_id) is not int or not 0 <= file_id <= MAX_U32:
            raise ValueError(f"Invalid NitroFS file ID: {file_id!r}")
        if type(size) is not int or not 0 <= size <= MAX_U32:
            raise ValueError(f"Invalid size for file ID {file_id}: {size!r}")
        if file_id in ids:
            raise ValueError(f"Duplicate NitroFS file ID: {file_id}")
        if not isinstance(relative, str) or relative in paths:
            raise ValueError(f"Duplicate or invalid manifest path: {relative!r}")
        path = _source_path(root, relative)
        if path == manifest_path.resolve():
            raise ValueError("manifest.json cannot be packed as a game file")
        actual_size = path.stat().st_size
        if actual_size != size:
            raise ValueError(f"Size mismatch for {relative!r}: manifest={size}, actual={actual_size}")
        ids.add(file_id)
        paths.add(relative)
        entries.append((file_id, path, size))
    entries.sort(key=lambda entry: entry[0])
    return entries, manifest


def _index(entries: list[tuple[int, int]]) -> list[tuple[int, int, int]]:
    """(file ID, offset, size) of each (file ID, size), in the given order."""
    count = len(entries)
    if count > (MAX_U32 - HEADER.size) // ENTRY.size:
        raise ValueError("Too many NitroFS files")
    offset = _align4(HEADER.size + ENTRY.size * count)
    index = []
    for file_id, size in entries:
        if offset > MAX_U32 or size > MAX_U32 - offset:
            raise ValueError("Pack exceeds 32-bit offset/size limit")
        index.append((file_id, offset, size))
        offset = _align4(offset + size)
        if offset > MAX_U32:
            raise ValueError("Pack exceeds 32-bit offset/size limit")
    return index


def write_pak(handle, files, *, build_id: str, rom_sha1: str, progress=None) -> int:
    """Write a v1 pack of in-memory files to a binary file object: the same
    bytes pack() writes from a data pack folder (the web builder's way: no
    folder of extracted files). files: (NitroFS file ID, bytes-like) pairs.
    progress(done, total), if given, is called after each file. Returns the
    pack's size."""
    files = sorted(files, key=lambda item: item[0])
    ids = [file_id for file_id, _ in files]
    if any(type(i) is not int or not 0 <= i <= MAX_U32 for i in ids) or len(set(ids)) != len(ids):
        raise ValueError("NitroFS file IDs must be unique 32-bit integers")
    index = _index([(file_id, len(data)) for file_id, data in files])
    written = HEADER.size
    handle.write(HEADER.pack(MAGIC, VERSION, _build_id_bytes(build_id), _sha1_bytes(rom_sha1), len(files)))
    for record in index:
        handle.write(ENTRY.pack(*record))
        written += ENTRY.size
    for n, ((_, data), (_, offset, size)) in enumerate(zip(files, index)):
        handle.write(b"\0" * (offset - written))
        handle.write(data)
        written = offset + size
        handle.write(b"\0" * (_align4(written) - written))
        written = _align4(written)
        if progress is not None:
            progress(n + 1, len(files))
    return written


def pack(data_pack: Path, output: Path, *, build_id: str,
         rom_sha1: str | None = None,
         supported: Collection[str] | None = None,
         extra: Collection[tuple[int, bytes]] = ()) -> None:
    """Write a canonical v1 pack; use a temporary file to avoid partial output.

    If `supported` is given, the ROM SHA-1 that would be written (from the
    manifest or `rom_sha1`) must be one of those lowercase hex digests.
    `extra`: (file ID, bytes) pairs the port adds to the ROM's files (the
    follower sprites: builder/followers.py), at IDs no NitroFS file has.
    """
    root = Path(data_pack).resolve()
    entries, manifest = _load_manifest(root)
    ids = {file_id for file_id, _, _ in entries}
    for file_id, data in extra:
        if type(file_id) is not int or not 0 <= file_id <= MAX_U32 or file_id in ids:
            raise ValueError(f"Extra file ID {file_id!r} isn't free")
        ids.add(file_id)
        entries.append((file_id, bytes(data), len(data)))
    entries.sort(key=lambda entry: entry[0])
    header_build_id = _build_id_bytes(build_id)
    manifest_sha1 = manifest.get("rom_sha1")
    if manifest_sha1 is not None:
        manifest_digest = _sha1_bytes(manifest_sha1)
        if rom_sha1 is not None and _sha1_bytes(rom_sha1) != manifest_digest:
            raise ValueError("Source ROM SHA-1 disagrees with manifest.json")
        digest = manifest_digest
    elif rom_sha1 is not None:
        digest = _sha1_bytes(rom_sha1)
    else:
        raise ValueError("manifest.json has no ROM SHA-1; provide --rom or --rom-sha1")
    if supported is not None and digest.hex() not in supported:
        raise ValueError("Unsupported ROM: run builder/verify_rom.py first")

    count = len(entries)
    index = _index([(file_id, size) for file_id, _, size in entries])

    output = Path(output).resolve()
    if output in {path for _, path, _ in entries if isinstance(path, Path)} or output == (root / "manifest.json").resolve():
        raise ValueError("Output would overwrite an input file")
    temp_name = None
    try:
        with tempfile.NamedTemporaryFile(dir=output.parent, prefix=".platinum-pak-",
                                         suffix=".tmp", delete=False) as handle:
            temp_name = handle.name
            handle.write(HEADER.pack(MAGIC, VERSION, header_build_id, digest, count))
            for record in index:
                handle.write(ENTRY.pack(*record))
            for (_, path, size), (_, expected_offset, _) in zip(entries, index):
                handle.write(b"\0" * (expected_offset - handle.tell()))
                if isinstance(path, bytes):   # (an extra file, in memory)
                    handle.write(path)
                    handle.write(b"\0" * (_align4(handle.tell()) - handle.tell()))
                    continue
                with path.open("rb") as source:
                    remaining = size
                    while remaining:
                        chunk = source.read(min(1 << 20, remaining))
                        if not chunk:
                            raise ValueError(f"File changed while packing: {path}")
                        handle.write(chunk)
                        remaining -= len(chunk)
                    if source.read(1):
                        raise ValueError(f"File grew while packing: {path}")
                handle.write(b"\0" * (_align4(handle.tell()) - handle.tell()))
        os.replace(temp_name, output)
    finally:
        if temp_name is not None and os.path.exists(temp_name):
            os.unlink(temp_name)


def read_pak(path: Path) -> dict:
    """Read and strictly validate a v1 pack; return file bytes keyed by NitroFS ID.

    This Python helper loads file data into memory. The 3DS reader can instead
    seek to HEADER.size + id-index * ENTRY.size and stream each entry.
    """
    data = Path(path).read_bytes()
    if len(data) < HEADER.size:
        raise ValueError("Truncated pak header")
    magic, version, raw_build_id, rom_sha1, count = HEADER.unpack_from(data)
    if magic != MAGIC:
        raise ValueError("Invalid pak magic")
    if version != VERSION:
        raise ValueError(f"Unsupported pak format version: {version}")
    build_bytes, separator, padding = raw_build_id.partition(b"\0")
    if not build_bytes or (separator and any(padding)):
        raise ValueError("Invalid build ID padding")
    try:
        build_id = build_bytes.decode("utf-8")
    except UnicodeDecodeError as exc:
        raise ValueError("Invalid build ID UTF-8") from exc
    table_end = HEADER.size + count * ENTRY.size
    if table_end > len(data):
        raise ValueError("Truncated pak index")
    cursor = _align4(table_end)
    if cursor > len(data) or any(data[table_end:cursor]):
        raise ValueError("Invalid pak index padding")
    files: dict[int, bytes] = {}
    previous_id = -1
    for n in range(count):
        file_id, offset, size = ENTRY.unpack_from(data, HEADER.size + n * ENTRY.size)
        if file_id <= previous_id:
            raise ValueError("Pak file IDs must be unique and sorted")
        if offset % 4 or offset != cursor:
            raise ValueError(f"Invalid or overlapping offset for file ID {file_id}")
        if offset > len(data) or size > len(data) - offset:
            raise ValueError(f"Out-of-range offset or size for file ID {file_id}")
        end = offset + size
        next_offset = _align4(end)
        if next_offset > len(data) or any(data[end:next_offset]):
            raise ValueError(f"Invalid padding for file ID {file_id}")
        files[file_id] = data[offset:end]
        cursor = next_offset
        previous_id = file_id
    if cursor != len(data):
        raise ValueError("Unexpected trailing pak data")
    return {"version": version, "build_id": build_id,
            "rom_sha1": rom_sha1.hex(), "files": files}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("data_pack", type=Path, help="Directory containing manifest.json")
    parser.add_argument("-o", "--output", type=Path, default=Path("platinum3ds.pak"))
    parser.add_argument("--build-id", default=DEFAULT_BUILD_ID,
                        help=f"Release/build ID (1–32 UTF-8 bytes; default {DEFAULT_BUILD_ID}, "
                             "what the game program expects)")
    rom = parser.add_mutually_exclusive_group()
    rom.add_argument("--rom", type=Path, help="Read-only verified source ROM to obtain SHA-1")
    rom.add_argument("--rom-sha1", help="40-character SHA-1 of the verified source ROM")
    parser.add_argument("--followers", type=Path,
                        help="the follower sprites (builder/followers.py's file), if you have them")
    args = parser.parse_args()
    try:
        try:
            from . import verify_rom
        except ImportError:
            import verify_rom
        sha1 = verify_rom.sha1_of(args.rom) if args.rom is not None else args.rom_sha1
        extra = []
        if args.followers is not None:
            try:
                from . import followers
            except ImportError:
                import followers
            bundle = args.followers.read_bytes()
            followers.read_bundle(bundle)
            extra.append((followers.PAK_ID, bundle))
        pack(args.data_pack, args.output, build_id=args.build_id, rom_sha1=sha1,
             supported=verify_rom.SUPPORTED, extra=extra)
    except (OSError, ValueError, UnicodeError) as exc:
        parser.exit(1, f"Packing failed: {exc}\n")
    print(f"Wrote {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
