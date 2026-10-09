#!/usr/bin/env python3
"""Generate source-backed NitroArc graphics archive index headers.

Pinned pret/pokeplatinum uses nitroarc --create --index --files-from
<archive>.order. Original nitroarc/src/opmode_create.c::write_index()
assigns each explicitly ordered filename its zero-based archive position;
it replaces non-alphanumeric/non-underscore characters with underscores
and appends _1, _2, ... for repeated *identical* filenames.

The pinned Meson rules also map pokedex.order to zukan.naix and use
explicit .order lists for trainer encounter effects and battle subscripts.
Those exact --files-from inputs produce index headers only, not .narc
archives, graphics, scripts, or any generated implicit archive members.
Because implicit members cannot be inventoried without generating assets,
the original archive COUNT macro is deliberately not synthesized.
"""
from __future__ import annotations

import argparse
import re
from pathlib import Path

VALID_PATH = re.compile(r"[A-Za-z0-9_./-]+\Z")
IDENTIFIER = re.compile(r"[A-Za-z_][A-Za-z0-9_]*\Z")


# Exact pinned res/*/meson.build --files-from mappings whose .naix output
# does not follow the generic graphics <order-stem>.naix rule.
# The two non-graphics archives are included by this generator as well.
ARCHIVE_OUTPUT_OVERRIDES = {
    Path("res/graphics/pokedex/pokedex.order"): Path("res/graphics/pokedex/zukan.naix"),
    Path("res/trainers/classes/field_encounteffect.order"): Path("res/trainers/classes/field_encounteffect.naix"),
    Path("res/battle/scripts/subscripts/sub_seq.order"): Path("res/battle/scripts/sub_seq.naix"),
}
NON_GRAPHICS_ORDERS = (
    Path("res/trainers/classes/field_encounteffect.order"),
    Path("res/battle/scripts/subscripts/sub_seq.order"),
)


def read_order(path: Path) -> list[str]:
    entries = []
    for line_no, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        value = line.strip(" \t\r")
        if not value:
            continue
        # Forbid directory traversal and absolute paths. The pinned source
        # uses ordinary archive-relative member paths.
        parts = value.split("/")
        if (
            not VALID_PATH.fullmatch(value)
            or value.startswith("/")
            or any(part in ("", ".", "..") for part in parts)
        ):
            raise ValueError(f"{path}:{line_no}: unsafe NitroArc filename {value!r}")
        entries.append(value)
    if not entries:
        raise ValueError(f"{path}: no explicitly ordered archive members")
    return entries


def to_macro(filename: str) -> str:
    # Match original nitroarc guardify(): punctuation, dots and '/' -> '_',
    # but keep original casing.
    symbol = re.sub(r"[^A-Za-z0-9_]", "_", filename)
    if not IDENTIFIER.fullmatch(symbol):
        raise ValueError(f"invalid C index identifier for {filename!r}")
    return symbol


def indices(order: Path) -> list[tuple[str, int]]:
    seen: dict[str, int] = {}
    used: set[str] = set()
    result: list[tuple[str, int]] = []
    for index, filename in enumerate(read_order(order)):
        nhits = seen.get(filename, 0)
        seen[filename] = nhits + 1
        name = to_macro(filename) + (f"_{nhits}" if nhits else "")
        if name in used:
            raise ValueError(f"{order}: index name collision {name!r}")
        used.add(name)
        result.append((name, index))
    return result


def render(order: Path, relative: Path) -> str:
    guard = "PORT_NITROARC_" + to_macro(relative.as_posix()).upper() + "_NAIX_H"
    values = indices(order)
    lines = [
        "/* Archive index metadata from pinned upstream NitroArc .order input. */",
        "/* Explicit members only; no graphics or NARC content is included. */",
        "/* Implicit archive members and total COUNT are not inferred. */",
        f"#ifndef {guard}",
        f"#define {guard}",
        "",
    ]
    lines.extend(f"#define {name} {index}" for name, index in values)
    lines.extend(["", f"#endif /* {guard} */", ""])
    return "\n".join(lines)


def generate(source_root: Path, output_root: Path) -> tuple[int, int, int]:
    graphics = source_root / "res" / "graphics"
    if not graphics.is_dir():
        raise FileNotFoundError(graphics)
    files = sorted(graphics.rglob("*.order"))
    if not files:
        raise ValueError(f"{graphics}: no source archive manifests")
    # These are generated using --index and pinned .order manifests just
    # like graphics archives. Never infer indices from compiled files.
    for relative in NON_GRAPHICS_ORDERS:
        order = source_root / relative
        if not order.is_file():
            raise FileNotFoundError(order)
        files.append(order)
    files.sort(key=lambda p: p.relative_to(source_root).as_posix())
    made = 0
    members = 0
    for order in files:
        relative = order.relative_to(source_root)
        # Meson sets zukan and battle script archive names explicitly.
        # Other graphics archives use their .order filename stems.
        dest = output_root / ARCHIVE_OUTPUT_OVERRIDES.get(relative, relative.with_suffix(".naix"))
        payload = render(order, relative)
        members += len(indices(order))
        if not dest.is_file() or dest.read_text(encoding="utf-8") != payload:
            dest.parent.mkdir(parents=True, exist_ok=True)
            dest.write_text(payload, encoding="utf-8")
            made += 1
    return len(files), members, made


def main() -> None:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--upstream", type=Path, required=True)
    p.add_argument("--output", type=Path, required=True)
    args = p.parse_args()
    count, indices_count, updated = generate(args.upstream.resolve(), args.output.resolve())
    print(f"Prepared {count} archive index headers with {indices_count} explicit member indices ({updated} updated)")
    print("Index metadata only — NO graphics, scripts, NARC archives or implied members")


if __name__ == "__main__":
    main()
