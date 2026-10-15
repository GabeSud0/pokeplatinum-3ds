"""Pictures for the places a layer stops at that it has none for, made
from its pictures at other places (docs/WIDE_ART.md, "Placing a picture").

A layer that stops somewhere else (the opening's Pokemon, moved 60 pixels
to the right for a while) shows its picture's bands only near the place
the picture was made at. For each capture given, each BG with a whole-layer
picture elsewhere gets that picture moved to where it is now, nothing
stretched or mirrored, if it lines up exactly with what the DS shows there.

    python tools/wideart/fill_places.py WIDE_FOLDER [CAPTURE ...] [--write]

Without CAPTURE: all of the folder's captures. Without --write it only
says what it would make.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import wideart_core as core  # noqa: E402


def fill(art_dir: Path, captures: list[core.Capture], write: bool) -> list[Path]:
    made = []
    for cap in captures:
        for layer in cap.layers:
            found = core.moved_art(art_dir, cap, layer)
            if not found:
                continue
            img, ref = found
            path = art_dir / layer.art_name(True, ref.loop)
            if path in made:
                continue
            print(f"{cap.name} {layer.id}: {path.name} (from {ref.path.name}, moved {ref.dx}, {ref.dy})")
            if write:
                img.save(path)
            made.append(path)
    return made


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("wide", help="the wide folder (3ds/pokeplatinum/wide)")
    ap.add_argument("captures", nargs="*", help="capture folders (default: all of the folder's)")
    ap.add_argument("--write", action="store_true", help="make the pictures (else only list them)")
    a = ap.parse_args(argv)
    art_dir = Path(a.wide)
    caps = [core.read_capture(c) for c in a.captures] if a.captures else core.list_captures(art_dir)
    made = fill(art_dir, caps, a.write)
    print(f"{len(made)} picture(s) {'made' if a.write else 'to make (--write makes them)'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
