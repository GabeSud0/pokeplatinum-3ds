#!/usr/bin/env python3
"""Generate identifier-only Platinum move text-bank headers from pinned sources.

This reproduces the four bank_push() sequences in pinned upstream
tools/dataproc/src/moveproc.c::proc_text() using the authoritative ordered
generated/moves.txt manifest. It does not run moveproc, copy any original
move text, generate message JSON, encode text, or create a NARC.

Each move produces one entry in move_names, move_names_uppercase and
move_descriptions, and three consecutive ally/wild/foe entries in
moves_used_in_battle. Generated headers stay under ignored build/.
"""
from __future__ import annotations

import argparse
import json
import re
from pathlib import Path

MOVE_SYMBOL = re.compile(r"MOVE_[A-Z0-9_]+\Z")
MOVE_STEM = re.compile(r"[a-z0-9_]+\Z")
BANK_NAMES = {
    "move_names": "MoveNames_Text",
    "move_names_uppercase": "MoveNamesUppercase_Text",
    "move_descriptions": "MoveDescriptions_Text",
    "moves_used_in_battle": "MovesUsedInBattle_Text",
}


def source_moves(upstream: Path) -> list[str]:
    """Read the authoritative sequential enum list, excluding MAX_MOVES."""
    manifest = upstream / "generated/moves.txt"
    lines = [line.strip() for line in manifest.read_text(encoding="utf-8").splitlines()]
    tokens = [line for line in lines if line and not line.startswith("#")]
    if len(tokens) < 2 or tokens[0] != "MOVE_NONE" or tokens[-1] != "MAX_MOVES":
        raise ValueError(f"{manifest}: expected MOVE_NONE ... MAX_MOVES")
    tokens = tokens[:-1]
    if len(set(tokens)) != len(tokens):
        raise ValueError(f"{manifest}: duplicate move token")
    for token in tokens:
        if not MOVE_SYMBOL.fullmatch(token):
            raise ValueError(f"{manifest}: invalid move enum token {token!r}")
        stem = token.removeprefix("MOVE_").lower()
        if not MOVE_STEM.fullmatch(stem):
            raise ValueError(f"{manifest}: invalid move path stem {stem!r}")
        source = upstream / "res" / "moves" / stem / "data.json"
        # Moveproc adds messages only when dp_load succeeds. Ensure the
        # corresponding original data really exists for each ordered enum
        # rather than fabricating bank entries for missing data.
        data = json.loads(source.read_text(encoding="utf-8"))
        if not isinstance(data, dict) or not isinstance(data.get("name"), str):
            raise ValueError(f"{source}: missing original move name")
        description = data.get("description")
        if not isinstance(description, (str, list)) or (
            isinstance(description, list)
            and any(not isinstance(value, str) for value in description)
        ):
            raise ValueError(f"{source}: invalid move description")
    return [token[5:].lower() for token in tokens]


def bank_ids(stems: list[str]) -> dict[str, list[str]]:
    banks: dict[str, list[str]] = {name: [] for name in BANK_NAMES}
    for stem in stems:
        banks["move_names"].append(f"MoveNames_Text_{stem}")
        banks["move_names_uppercase"].append(f"MoveNamesUppercase_Text_{stem}")
        banks["move_descriptions"].append(f"MoveDescriptions_Text_{stem}")
        for side in ("Ally", "Wild", "Foe"):
            banks["moves_used_in_battle"].append(
                f"MovesUsedInBattle_Text_{stem}_{side}"
            )
    return banks


def render(bank: str, ids: list[str]) -> str:
    """Follow the existing original msgenc-style header guard and count."""
    if bank not in BANK_NAMES:
        raise ValueError(f"not an upstream move message bank: {bank!r}")
    upper = bank.upper()
    guard = f"MSGENC_RES_TEXT_BANK_{upper}_H"
    count = f"TEXT_BANK_{upper}_ENTRY_COUNT"
    if len(ids) != len(set(ids)) or count in ids:
        raise ValueError(f"{bank}: duplicated or reserved message ID")
    lines = [
        "/* Generated indices from pinned moveproc order. No game dialogue. */",
        f"#ifndef {guard}",
        f"#define {guard}",
        "",
    ]
    lines.extend(f"#define {name} {i}" for i, name in enumerate(ids))
    lines.extend([f"#define {count} {len(ids)}", "", f"#endif /* {guard} */", ""])
    return "\n".join(lines)


def generate(upstream: Path, output: Path) -> tuple[int, int]:
    stems = source_moves(upstream)
    banks = bank_ids(stems)
    destination = output / "res" / "text" / "bank"
    destination.mkdir(parents=True, exist_ok=True)
    changed = 0
    for bank, ids in banks.items():
        header = destination / f"{bank}.h"
        contents = render(bank, ids)
        if not header.is_file() or header.read_text(encoding="utf-8") != contents:
            header.write_text(contents, encoding="utf-8")
            changed += 1
    return len(stems), changed


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--upstream", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    count, changed = generate(args.upstream.resolve(), args.output.resolve())
    print(f"Generated metadata for {count} pinned moves in four text-bank headers ({changed} updated)")
    print("Identifiers/counts only; move names, descriptions and battle text are not emitted")


if __name__ == "__main__":
    main()
