#!/usr/bin/env python3
"""Generate the original game's TextBank enum from the pinned upstream list.

The pinned pret/pokeplatinum Meson build normally invokes 'metang enum'
over generated/text_banks.txt. This standalone generator reproduces the
sequential enum IDs for the GCC game build, without copying source files,
bundling game data, or depending on Metrowerks/Meson tools.

Usage:
  python tools/generate_game_text_banks.py SOURCE.txt OUTPUT.h
"""
from __future__ import annotations

import argparse
import re
from pathlib import Path

BANK = re.compile(r"TEXT_BANK_[A-Z0-9_]+\Z")
GUARD = "POKEPLATINUM_GENERATED_TEXT_BANKS_H"


def generate(source: Path) -> str:
    names: list[str] = []
    seen: set[str] = set()
    for line_no, raw in enumerate(source.read_text(encoding="utf-8").splitlines(), 1):
        name = raw.strip()
        if not name:
            continue
        if not BANK.fullmatch(name):
            raise ValueError(f"{source}:{line_no}: not a TEXT_BANK identifier: {name!r}")
        if name in seen:
            raise ValueError(f"{source}:{line_no}: duplicate identifier: {name}")
        seen.add(name)
        names.append(name)
    if not names:
        raise ValueError(f"{source}: no text banks found")

    entries = "\n".join(f"    {name} = {i}," for i, name in enumerate(names))
    return (
        f"/* Generated from {source.name}; do not edit. */\n"
        f"#ifndef {GUARD}\n#define {GUARD}\n\n"
        f"enum TextBank {{\n{entries}\n}};\n\n"
        f"#endif /* {GUARD} */\n"
    )


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    result = generate(args.source)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    if not args.output.exists() or args.output.read_text(encoding="utf-8") != result:
        args.output.write_text(result, encoding="utf-8")
    print(f"generated {args.output}")


if __name__ == "__main__":
    main()
