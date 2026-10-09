#!/usr/bin/env python3
"""Print the game's C sources as the original build lists them.

Usage:
    python tools/game_sources.py build/upstream

pret/pokeplatinum compiles the files named in src/meson.build (and the
meson.build files of src/'s subfolders), relative to src/. A few files in
src/ aren't on that list and aren't part of the game (unk_02095CD4.c
includes a header that doesn't exist). The port builds the same list.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path


def listed_sources(upstream: Path) -> list[str]:
    src = upstream / "src"
    found: set[str] = set()
    for build in src.rglob("meson.build"):
        for name in re.findall(r"'([^']+\.c)'", build.read_text(encoding="utf-8")):
            path = build.parent / name
            if path.is_file():
                found.add(path.relative_to(src).as_posix())
    if not found:
        raise SystemExit(f"{src}: no sources listed in meson.build")
    return sorted(found)


if __name__ == "__main__":
    print("\n".join(listed_sources(Path(sys.argv[1]))))
