#!/usr/bin/env python3
"""Generate two original Platinum species-name text-bank ID headers.

Source: pinned pret/pokeplatinum/tools/dataproc/src/speciesproc.c,
emit_textbanks(). Each registry entry up to SPECIES_BAD_EGG contributes
one sequential message ID to species_name and species_name_with_articles:
  species_name_<lowercase species enum suffix>
  species_name_with_articles_<lowercase species enum suffix>

The registry's order comes from pinned generated/species.txt. Alternate
forms appended to the speciesproc registry occur *after* SPECIES_BAD_EGG
and do not contribute names. The original species data.json files are
validated but **not copied** into output. No names, descriptions, game
dialogue, message JSON, NARC archives or binary assets are generated.
"""
from __future__ import annotations

import argparse
import json
import re
from pathlib import Path

SYMBOL = re.compile(r"SPECIES_[A-Z0-9_]+\Z")
STEM = re.compile(r"[a-z0-9_]+\Z")
BANK_PREFIXES = {
    "species_name": "species_name",
    "species_name_with_articles": "species_name_with_articles",
}


def ordered_species(upstream: Path) -> list[str]:
    manifest = upstream / "generated/species.txt"
    names = [
        line.strip() for line in manifest.read_text(encoding="utf-8").splitlines()
        if line.strip() and not line.lstrip().startswith("#")
    ]
    if len(names) < 3 or names[0] != "SPECIES_NONE" or names[-2:] != [
        "SPECIES_EGG", "SPECIES_BAD_EGG"
    ]:
        raise ValueError(f"{manifest}: unexpected species enum boundary/order")
    if len(set(names)) != len(names):
        raise ValueError(f"{manifest}: duplicate species enum entry")

    stems: list[str] = []
    for token in names:
        if not SYMBOL.fullmatch(token):
            raise ValueError(f"{manifest}: invalid enum symbol {token!r}")
        stem = token.removeprefix("SPECIES_").lower()
        if not STEM.fullmatch(stem):
            raise ValueError(f"{manifest}: unsafe species directory name {stem!r}")
        source = upstream / "res" / "pokemon" / stem / "data.json"
        # speciesproc::emit_textbanks() runs only after its dp_load()
        # succeeds, so refuse to fabricate an entry with missing input.
        data = json.loads(source.read_text(encoding="utf-8"))
        if not isinstance(data, dict):
            raise ValueError(f"{source}: expected a species data object")
        dex = data.get("pokedex_data")
        if not isinstance(dex, dict) or not isinstance(dex.get("en"), dict):
            raise ValueError(f"{source}: missing original Pokédex data")
        if not isinstance(dex["en"].get("name"), str):
            raise ValueError(f"{source}: missing original species name")
        stems.append(stem)
    return stems


def bank_ids(stems: list[str]) -> dict[str, list[str]]:
    return {
        bank: [f"{prefix}_{stem}" for stem in stems]
        for bank, prefix in BANK_PREFIXES.items()
    }


def render(bank: str, ids: list[str]) -> str:
    if bank not in BANK_PREFIXES:
        raise ValueError(f"unknown original species bank: {bank!r}")
    if len(set(ids)) != len(ids):
        raise ValueError(f"{bank}: duplicate message IDs")
    prefix = BANK_PREFIXES[bank] + "_"
    if any(not name.startswith(prefix) or not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", name)
           for name in ids):
        raise ValueError(f"{bank}: invalid message ID")
    upper = bank.upper()
    guard = f"MSGENC_RES_TEXT_BANK_{upper}_H"
    count = f"TEXT_BANK_{upper}_ENTRY_COUNT"
    if count in ids:
        raise ValueError(f"{bank}: count name conflicts with message ID")
    lines = [
        "/* Pinned speciesproc name IDs; contains no Pokemon names or dialogue. */",
        f"#ifndef {guard}",
        f"#define {guard}",
        "",
    ]
    lines.extend(f"#define {name} {index}" for index, name in enumerate(ids))
    lines.extend([f"#define {count} {len(ids)}", "", f"#endif /* {guard} */", ""])
    return "\n".join(lines)


def generate(upstream: Path, output: Path) -> tuple[int, int]:
    stems = ordered_species(upstream)
    banks = bank_ids(stems)
    dest = output / "res/text/bank"
    dest.mkdir(parents=True, exist_ok=True)
    changed = 0
    for name, ids in banks.items():
        path = dest / (name + ".h")
        content = render(name, ids)
        if not path.exists() or path.read_text(encoding="utf-8") != content:
            path.write_text(content, encoding="utf-8")
            changed += 1
    return len(stems), changed


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--upstream", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    count, changed = generate(args.upstream.resolve(), args.output.resolve())
    print(f"Generated metadata for {count} original Platinum species in two text banks ({changed} updated)")
    print("IDs and counts only; no species names, game text or message archives")


if __name__ == "__main__":
    main()
