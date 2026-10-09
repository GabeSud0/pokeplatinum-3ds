#!/usr/bin/env python3
"""Check that a ROM dump is a version this project supports.

Usage:
    python builder/verify_rom.py path/to/rom.nds

The ROM is only read. Nothing is copied, uploaded or modified.
"""

from __future__ import annotations

import hashlib
import sys
from pathlib import Path

# SHA-1 hashes of clean dumps (No-Intro), as listed by the pret decompilations.
SUPPORTED = {
    "0862ec35b24de5c7e2dcb88c9eea0873110d755c": "Pokémon Platinum (USA) Rev 1",
    "ce81046eda7d232513069519cb2085349896dec7": "Pokémon Platinum (USA) Rev 0",
}


def sha1_of(path: Path) -> str:
    digest = hashlib.sha1()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__.strip())
        return 2

    rom = Path(sys.argv[1])
    if not rom.is_file():
        print(f"Not a file: {rom}")
        return 2

    digest = sha1_of(rom)
    name = SUPPORTED.get(digest)
    if name:
        print(f"OK: {name}")
        return 0

    print(f"Unsupported ROM (SHA-1 {digest}).")
    print("Supported versions:")
    for h, n in SUPPORTED.items():
        print(f"  {n}: {h}")
    print("A modified, trimmed or other-region dump will not match.")
    return 1


if __name__ == "__main__":
    sys.exit(main())
