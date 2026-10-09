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
from pathlib import Path

MAGIC = b"PLATPAK\0"
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


def pack(data_pack: Path, output: Path, *, build_id: str,
         rom_sha1: str | None = None) -> None:
    """Write a canonical v1 pack; use a temporary file to avoid partial output."""
    root = Path(data_pack).resolve()
    entries, manifest = _load_manifest(root)
    header_build_id = _build_id_bytes(build_id)
    manifest_sha1 = manifest.get("rom_sha1", manifest.get("source_rom_sha1"))
    if manifest_sha1 is not None:
        manifest_digest = _sha1_bytes(manifest_sha1)
        if rom_sha1 is not None and _sha1_bytes(rom_sha1) != manifest_digest:
            raise ValueError("Source ROM SHA-1 disagrees with manifest.json")
        digest = manifest_digest
    elif rom_sha1 is not None:
        digest = _sha1_bytes(rom_sha1)
    else:
        raise ValueError("manifest.json has no ROM SHA-1; provide --rom or --rom-sha1")

    count = len(entries)
    if count > (MAX_U32 - HEADER.size) // ENTRY.size:
        raise ValueError("Too many NitroFS files")
    offset = _align4(HEADER.size + ENTRY.size * count)
    index = []
    for file_id, path, size in entries:
        if offset > MAX_U32 or size > MAX_U32 - offset:
            raise ValueError("Pack exceeds 32-bit offset/size limit")
        index.append((file_id, offset, size))
        offset = _align4(offset + size)
        if offset > MAX_U32:
            raise ValueError("Pack exceeds 32-bit offset/size limit")

    output = Path(output).resolve()
    if output in {path for _, path, _ in entries} or output == (root / "manifest.json").resolve():
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
    parser.add_argument("--build-id", required=True, help="Release/build ID (1–32 UTF-8 bytes)")
    rom = parser.add_mutually_exclusive_group()
    rom.add_argument("--rom", type=Path, help="Read-only verified source ROM to obtain SHA-1")
    rom.add_argument("--rom-sha1", help="40-character SHA-1 of the verified source ROM")
    args = parser.parse_args()
    try:
        try:
            from . import verify_rom
        except ImportError:
            import verify_rom
        sha1 = args.rom_sha1.lower() if args.rom_sha1 else None
        if args.rom is not None:
            sha1 = verify_rom.sha1_of(args.rom)
        if sha1 is not None and sha1 not in verify_rom.SUPPORTED:
            raise ValueError("Unsupported ROM: run builder/verify_rom.py first")
        pack(args.data_pack, args.output, build_id=args.build_id, rom_sha1=sha1)
    except (OSError, ValueError, UnicodeError) as exc:
        parser.exit(1, f"Packing failed: {exc}\n")
    print(f"Wrote {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
