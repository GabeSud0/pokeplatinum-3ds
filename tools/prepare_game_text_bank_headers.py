#!/usr/bin/env python3
"""Generate Platinum text-bank C *index* headers from pinned upstream JSON.

Original pret/pokeplatinum tools/msgenc/Json.cpp::WriteHeader emits a
#define for each message's "id", numbered sequentially from zero, plus
a TEXT_BANK_<BANK>_ENTRY_COUNT macro. This reproduces those identifier
assignments, NOT the text encoder, message payload, or pl_msg.narc.

Only the symbol names, ordinal indices and counts are written to ignored
build output; no actual dialogue, encryption keys, ROM resources or data
archives are packaged.
"""
from __future__ import annotations

import argparse
import json
import re
from pathlib import Path

IDENTIFIER = re.compile(r"[A-Za-z_][A-Za-z0-9_]*\Z")
BANK_NAME = re.compile(r"[a-z][a-z0-9_]*\Z")


def read_ids(source: Path) -> list[str]:
    data = json.loads(source.read_text(encoding="utf-8"))
    if not isinstance(data, dict) or not isinstance(data.get("messages"), list):
        raise ValueError(f"{source}: expected object containing messages array")
    messages = data["messages"]
    if not messages:
        raise ValueError(f"{source}: messages list must not be empty")

    seen: set[str] = set()
    ids: list[str] = []
    for index, message in enumerate(messages):
        if not isinstance(message, dict):
            raise ValueError(f"{source}: message {index} must be an object")
        name = message.get("id")
        if not isinstance(name, str) or not IDENTIFIER.fullmatch(name):
            raise ValueError(f"{source}: invalid message id at index {index}: {name!r}")
        if name in seen:
            raise ValueError(f"{source}: duplicate message id {name!r}")
        seen.add(name)
        ids.append(name)
    return ids


def render(source: Path) -> str:
    bank = source.stem
    if not BANK_NAME.fullmatch(bank):
        raise ValueError(f"{source}: invalid text-bank name")
    ids = read_ids(source)

    # This matches the original msgenc's guard formed from its relative
    # output path "res/text/bank/<bank>.h" and its suffix count symbol:
    #   MSGENC_RES_TEXT_BANK_<BANK>_H
    #   TEXT_BANK_<BANK>_ENTRY_COUNT
    upper = bank.upper()
    guard = f"MSGENC_RES_TEXT_BANK_{upper}_H"
    count_macro = f"TEXT_BANK_{upper}_ENTRY_COUNT"
    if any(name == count_macro for name in ids):
        raise ValueError(f"{source}: message ID collides with count macro")

    lines = [
        "/* Generated index metadata from pinned upstream JSON. */",
        "/* The text payload and ROM content are not included. */",
        f"#ifndef {guard}",
        f"#define {guard}",
        "",
    ]
    lines.extend(f"#define {name} {i}" for i, name in enumerate(ids))
    lines.extend([f"#define {count_macro} {len(ids)}", "", f"#endif /* {guard} */", ""])
    return "\n".join(lines)


def generate(source_dir: Path, output_root: Path) -> tuple[int, int]:
    if not source_dir.is_dir():
        raise FileNotFoundError(source_dir)
    inputs = sorted(source_dir.glob("*.json"))
    if not inputs:
        raise ValueError(f"{source_dir}: no static text bank JSON files")
    dest = output_root / "res" / "text" / "bank"
    dest.mkdir(parents=True, exist_ok=True)

    updated = 0
    for source in inputs:
        content = render(source)
        target = dest / (source.stem + ".h")
        if not target.is_file() or target.read_text(encoding="utf-8") != content:
            target.write_text(content, encoding="utf-8")
            updated += 1
    return len(inputs), updated


def main() -> None:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--upstream", type=Path, required=True)
    p.add_argument("--output", type=Path, required=True)
    args = p.parse_args()
    count, changed = generate(
        args.upstream.resolve() / "res" / "text",
        args.output.resolve(),
    )
    print(f"Prepared {count} original Platinum text-bank index headers ({changed} updated)")
    print("Metadata only: no encoded messages, dialogue, or NARC archives")


if __name__ == "__main__":
    main()
