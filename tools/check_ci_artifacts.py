#!/usr/bin/env python3
"""Fail if a CI workflow uploads anything but the allowed artifacts.

Usage:
    python tools/check_ci_artifacts.py [WORKFLOW.yml ...]

Anything built from the game's code (the smoke app, the linked game) is a
build of the game, and the repository never publishes one: CI builds and
tests it, but only the toolchain test app, text reports and the WideArt
editor (a drawing tool) leave the run.
This reads every actions/upload-artifact step in .github/workflows and
checks each uploaded path against the list below. Add to the list only
things that contain no game code or data.
"""

from __future__ import annotations

import fnmatch
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

ALLOWED = [
    "3ds_port/hello3d/hello3d.3dsx",          # toolchain test app, no game code
    "build/game-link-report/*.md",            # lists of symbol names
    "build/game-compile-survey/*.json",       # compiler messages per file
    "dist/WideArt.exe",                       # the wide art editor (tools/wideart), no game code
]


def uploaded_paths(text: str) -> list[str]:
    """The `path:` values of every upload-artifact step (one per line or a | block)."""
    paths: list[str] = []
    for step in re.split(r"\n\s*- (?=name:|uses:)", text):
        if "actions/upload-artifact" not in step:
            continue
        # [ \t]* rather than \s*: \s would also match the line break after
        # "path: |" and swallow the block's first path.
        block = re.search(r"^[ \t]*path:[ \t]*(\|)?[ \t]*(.*?)[ \t]*$((?:\n[ \t]{12,}\S.*)*)", step, re.M)
        if not block:
            paths.append("<upload step without a path>")
            continue
        first, rest = block.group(2), block.group(3)
        values = [first] if first and not block.group(1) else []
        values += [line.strip() for line in rest.splitlines() if line.strip()]
        paths += values or ["<empty path>"]
    return paths


def main() -> int:
    files = [Path(a) for a in sys.argv[1:]] or sorted((ROOT / ".github" / "workflows").glob("*.y*ml"))
    bad = []
    for workflow in files:
        for path in uploaded_paths(workflow.read_text(encoding="utf-8")):
            if not any(fnmatch.fnmatch(path, pattern) for pattern in ALLOWED):
                bad.append(f"{workflow.name}: {path}")
    if bad:
        print("CI uploads something that isn't on the allowed list (tools/check_ci_artifacts.py):")
        for line in bad:
            print(f"  {line}")
        print("Builds containing game code must never be uploaded.")
        return 1
    print(f"CI artifacts OK ({len(files)} workflow files checked)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
