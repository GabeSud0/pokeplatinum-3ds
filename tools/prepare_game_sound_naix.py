#!/usr/bin/env python3
"""Generate NitroSFX sound *index metadata* from the pinned Platinum SDAT JSON.

The upstream tools/nitrosfx/sdat.c::WriteNaix function, invoked with -naix,
emits a C #define for every nonempty sound-info record, using each record's
filename (or the name for types without a filename), its zero-based index
within its own info array and per-array duplicate suffixes. This tool
reproduces only those symbolic indices; it NEVER builds sound samples,
wave archives, music, the SDAT file, or any game payload.
"""
from __future__ import annotations

import argparse
import json
import re
from pathlib import Path

# Exact DATATYPE order from upstream NitroSFX SDAT.cpp / sdat.c.
CATEGORIES = (
    ("seqInfo", True),
    ("seqarcInfo", True),
    ("bankInfo", True),
    ("wavarcInfo", True),
    ("playerInfo", False),
    ("groupInfo", False),
    ("streamPlayerInfo", False),
    ("strmInfo", False),
)
ALLOWED_NAME = re.compile(r"[A-Za-z0-9_./-]+\Z")
SAFE_MACRO = re.compile(r"[A-Za-z_][A-Za-z0-9_]*\Z")


def normalize_identifier(value: str) -> str:
    """Match upstream NitroSFX guardify(): non-word bytes become underscores."""
    if not isinstance(value, str) or not value or not ALLOWED_NAME.fullmatch(value):
        raise ValueError(f"unsafe or empty sound resource name {value!r}")
    symbol = "".join(c if c.isascii() and (c.isalnum() or c == "_") else "_" for c in value)
    if not SAFE_MACRO.fullmatch(symbol):
        raise ValueError(f"invalid generated macro {symbol!r}")
    return symbol


def sound_symbols(source: Path) -> list[tuple[str, int]]:
    data = json.loads(source.read_text(encoding="utf-8"))
    if not isinstance(data, dict):
        raise ValueError(f"{source}: expected a JSON object")
    symbols: list[tuple[str, int]] = []
    all_names: set[str] = set()
    for key, has_file in CATEGORIES:
        records = data.get(key)
        if not isinstance(records, list):
            raise ValueError(f"{source}: missing {key} array")
        seen: dict[str, int] = {}
        for idx, record in enumerate(records):
            if not isinstance(record, dict) or not isinstance(record.get("name"), str):
                raise ValueError(f"{source}: invalid {key}[{idx}]")
            # Empty name yields a zero-sized package record. NitroSFX omits
            # these definitions while keeping their index slots reserved.
            if not record["name"]:
                continue
            name = record.get("fileName") if has_file else record["name"]
            base = normalize_identifier(name)
            duplicates = seen.get(name, 0)
            seen[name] = duplicates + 1
            macro = f"{base}_{duplicates}" if duplicates else base
            if macro in all_names:
                raise ValueError(f"{source}: duplicate cross-category macro {macro}")
            all_names.add(macro)
            symbols.append((macro, idx))
    return symbols


def render(source: Path) -> str:
    symbols = sound_symbols(source)
    guard = "pl_sound_data_naix"  # NitroSFX guardify("pl_sound_data.naix")
    lines = [
        "/* Generated sound index metadata from the pinned Platinum SDAT JSON. */",
        "/* This contains no sound samples or playable sound archive. */",
        f"#ifndef {guard}",
        f"#define {guard}",
        "",
    ]
    lines.extend(f"#define {name} {index}" for name, index in symbols)
    lines.extend(["", f"#endif /* {guard} */", ""])
    return "\n".join(lines)


def generate(source: Path, destination: Path) -> bool:
    content = render(source)
    if destination.is_file() and destination.read_text(encoding="utf-8") == content:
        return False
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_text(content, encoding="utf-8")
    return True


def main() -> None:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--upstream", type=Path, required=True)
    p.add_argument("--output", type=Path, required=True)
    args = p.parse_args()
    source = args.upstream.resolve() / "res/sound/pl_sound_data.json"
    destination = args.output.resolve() / "res/sound/pl_sound_data.naix"
    changed = generate(source, destination)
    print(f"{'Generated' if changed else 'Unchanged'}: {destination}")
    print(f"Sound index definitions: {len(sound_symbols(source))}")
    print("Index metadata only — SDAT, SEQ, BANK, WAVARC and audio are NOT generated")


if __name__ == "__main__":
    main()
