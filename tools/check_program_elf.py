#!/usr/bin/env python3
"""Check that every overlay's data ended up where the program loads it.

Usage:
    python tools/check_program_elf.py --readelf arm-none-eabi-readelf \\
        3ds_port/game/build/3ds/pokeplatinum.elf

tools/game_overlays.py gives each overlay's data sections of its own
(ovl_data_<name>, ovl_bss_<name>), and its table of static initializers
(ovl_sinit_<name>); the linker places them by itself. This checks the
result: each one is in the program's writable segment (so the 3DS loader
allocates it), that segment's sizes are multiples of 4, as 3dsxtool
requires, and no static initializers were left outside an overlay's table
(nothing would call them).
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path


def segments(readelf: str, elf: Path) -> tuple[list[dict], list[list[str]]]:
    """The PT_LOAD segments and, for each program header, its sections."""
    text = subprocess.run([readelf, "-lW", str(elf)], check=True, capture_output=True,
                          text=True).stdout
    headers = []
    for line in text.splitlines():
        m = re.match(r"\s*(\w+)\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+"
                     r"0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+([RWE ]+?)\s+0x", line)
        if m:
            headers.append({"type": m.group(1), "filesz": int(m.group(5), 16),
                            "memsz": int(m.group(6), 16), "flags": m.group(7).replace(" ", "")})
    mapping = []
    in_map = False
    for line in text.splitlines():
        if "Section to Segment mapping" in line:
            in_map = True
            continue
        if in_map:
            m = re.match(r"\s*(\d+)\s*(.*)$", line)
            if m and line.strip() and not line.strip().startswith("Segment"):
                mapping.append(m.group(2).split())
    return headers, mapping


def check(headers: list[dict], mapping: list[list[str]], overlay_sections: list[str]) -> list[str]:
    problems = []
    writable = [i for i, h in enumerate(headers) if h["type"] == "LOAD" and "W" in h["flags"]]
    if len(writable) != 1:
        return [f"expected one writable segment, found {len(writable)}"]
    data = writable[0]
    h = headers[data]
    if h["filesz"] % 4 or h["memsz"] % 4:
        problems.append(f"writable segment sizes aren't multiples of 4 "
                        f"(file {h['filesz']:#x}, memory {h['memsz']:#x})")
    in_data = set(mapping[data]) if data < len(mapping) else set()
    for name in overlay_sections:
        if name not in in_data:
            problems.append(f"{name} isn't in the writable segment")
    return problems


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--readelf", default="readelf")
    parser.add_argument("elf", type=Path)
    args = parser.parse_args()
    headers, mapping = segments(args.readelf, args.elf)
    names = sorted({s for row in mapping for s in row
                    if s.startswith(("ovl_data_", "ovl_bss_", "ovl_sinit_"))})
    sections = subprocess.run([args.readelf, "-SW", str(args.elf)], check=True,
                              capture_output=True, text=True).stdout
    all_ovl = sorted(set(re.findall(r"\b(ovl_(?:data|bss|sinit)_\w+)", sections)))
    problems = check(headers, mapping, all_ovl)
    if not all_ovl:
        problems.append("no overlay data sections at all")
    if re.search(r"\s\.sinit\s", sections):
        problems.append("static initializers (.sinit) outside any overlay's table")
    for p in problems:
        print(f"::error::{p}" if "GITHUB_ACTIONS" in __import__("os").environ else p)
    print(f"{len(all_ovl)} overlay sections, {len(names)} in segments: "
          + ("OK" if not problems else f"{len(problems)} problems"))
    sys.exit(1 if problems else 0)


if __name__ == "__main__":
    main()
