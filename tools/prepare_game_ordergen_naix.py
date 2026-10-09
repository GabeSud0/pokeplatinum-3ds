#!/usr/bin/env python3
"""Archive index headers (.naix) for archives whose member lists are made by a script.

Usage:
    python tools/prepare_game_ordergen_naix.py --upstream build/upstream \
        --output build/game-resource-headers

Some archives in pinned pret/pokeplatinum get their --files-from list from
a script in tools/ordergen/ rather than a checked-in .order file: the
trainer front/back sprite archives (trfgra, trbgra) and the footprints
(pokefoot). Their meson.build rules run the script with an environment
variable listing the folders to visit, which they derive from a generated
constants list (generated/trainer_classes.txt, generated/species.txt): each
line, without its prefix, in lower case.

This does the same: it builds that environment, runs the decompilation's
own script at the pinned revision to write the .order files, and turns each
into a .naix with prepare_game_graphics_naix (NitroArc's naming rules). Only
member names and positions are produced; no graphics are read or built.
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from prepare_game_graphics_naix import render  # noqa: E402

# Each script, the environment variable its meson.build sets (from which
# generated list, minus which prefix), its arguments, and the .naix each
# output .order becomes (where Meson writes it).
ORDERGEN = [
    {
        "script": "tools/ordergen/trainer_graphics.py",
        "env": ("TRAINER_CLASSES", "generated/trainer_classes.txt", "TRAINER_CLASS_"),
        "args": ["@SRC:res/trainers/classes", "@OUT:trfgra.order", "@OUT:trbgra.order"],
        "naix": {
            "trfgra.order": "res/trainers/classes/trfgra.naix",
            "trbgra.order": "res/trainers/classes/trbgra.naix",
        },
    },
    {
        "script": "tools/ordergen/species.py",
        "env": ("SPECIES", "generated/species.txt", "SPECIES_"),
        "args": ["@OUT:pokefoot.order"],
        "naix": {"pokefoot.order": "res/pokemon/pokefoot.naix"},
    },
]


def folder_names(upstream: Path, listing: str, prefix: str) -> list[str]:
    """What meson.build computes: each constant without its prefix, lower case."""
    names = []
    for line in (upstream / listing).read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line:
            continue
        if not line.startswith(prefix):
            raise ValueError(f"{listing}: {line!r} lacks the prefix {prefix!r}")
        names.append(line[len(prefix):].lower())
    if not names:
        raise ValueError(f"{listing}: empty")
    return names


def run(upstream: Path, work: Path, entry: dict) -> dict[str, Path]:
    variable, listing, prefix = entry["env"]
    env = dict(os.environ)
    env[variable] = ";".join(folder_names(upstream, listing, prefix))
    outputs: dict[str, Path] = {}
    argv = []
    for arg in entry["args"]:
        kind, _, value = arg.partition(":")
        if kind == "@SRC":
            argv.append(str(upstream / value))
        else:
            outputs[value] = work / value
            argv.append(str(outputs[value]))
    script = upstream / entry["script"]
    if not script.is_file():
        raise FileNotFoundError(script)
    subprocess.run([sys.executable, "-I", str(script), *argv], check=True, env=env)
    return outputs


def generate(upstream: Path, output: Path) -> tuple[int, int]:
    work = output / ".ordergen"
    work.mkdir(parents=True, exist_ok=True)
    made = updated = 0
    for entry in ORDERGEN:
        for order_name, order_path in run(upstream, work, entry).items():
            relative = Path(entry["naix"][order_name])
            payload = render(order_path, relative)
            dest = output / relative
            made += 1
            if not dest.is_file() or dest.read_text(encoding="utf-8") != payload:
                dest.parent.mkdir(parents=True, exist_ok=True)
                dest.write_text(payload, encoding="utf-8")
                updated += 1
    return made, updated


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--upstream", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    made, updated = generate(args.upstream.resolve(), args.output.resolve())
    print(f"Prepared {made} script-ordered archive index headers ({updated} updated)")


if __name__ == "__main__":
    main()
