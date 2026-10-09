#!/usr/bin/env python3
"""Prepare build-only resource metadata headers from pinned Platinum manifests.

The original pret/pokeplatinum dexproc generates
res/pokemon/regional_pokedex_size.h from res/pokemon/sinnoh_pokedex.json.
This script implements *only* that header's documented count formula:
REGIONAL_DEX_COUNT = len(the ordered JSON species list) - 1.

It does not create NARC assets, ROM data, game images, or placeholder IDs.
The rest of the resource build must be ported separately.
"""
from __future__ import annotations

import argparse
import json
import re
from pathlib import Path

SPECIES = re.compile(r"SPECIES_[A-Z0-9_]+\Z")
GUARD = "POKEPLATINUM_GENERATED_REGIONAL_POKEDEX_SIZE_H"


def render_regional_pokedex_header(path: Path) -> str:
    entries = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(entries, list) or len(entries) < 2 or len(entries) > 65535:
        raise ValueError("regional Pokédex must be a JSON array of 2..65535 species")

    seen: set[str] = set()
    for index, entry in enumerate(entries):
        if not isinstance(entry, str) or not SPECIES.fullmatch(entry):
            raise ValueError(f"regional species entry {index} is not a valid Species symbol")
        if entry in seen:
            raise ValueError(f"duplicate regional species entry: {entry}")
        seen.add(entry)

    # Exact logic in pinned upstream tools/dataproc/src/dexproc.c:
    # fprintf(headers[0].out_file, "#define REGIONAL_DEX_COUNT %u\n",
    #         regional_count - 1);
    count = len(entries) - 1
    return (
        "/* Generated from the pinned upstream Sinnoh Pokédex list. */\n"
        "/* Metadata only: no ROM or graphics/audio resource content. */\n"
        f"#ifndef {GUARD}\n"
        f"#define {GUARD}\n\n"
        f"#define REGIONAL_DEX_COUNT {count}\n\n"
        f"#endif /* {GUARD} */\n"
    )


def generate(source: Path, output: Path) -> bool:
    """Write only when content changes; return whether output was modified."""
    content = render_regional_pokedex_header(source)
    if output.is_file() and output.read_text(encoding="utf-8") == content:
        return False
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(content, encoding="utf-8")
    return True


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    changed = generate(args.source, args.output)
    print(f"{'Generated' if changed else 'Unchanged'}: {args.output}")


if __name__ == "__main__":
    main()
