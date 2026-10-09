#!/usr/bin/env python3
"""Rewrite 3ds_port/game/blocked_sources*.txt from compile survey reports.

Usage:
    python tools/update_game_blocklist.py REPORT_32BIT.json REPORT_64BIT.json

The reports come from tools/probe_game_compile.py with --limit 0 (every
file), e.g. `make -C 3ds_port/game TARGET=host32 probe PROBE_LIMIT=0` and
`make -C 3ds_port/game TARGET=host probe PROBE_LIMIT=0`. The 32-bit report
decides blocked_sources.txt (what the 3DS can't compile; the game flags
already set GCC 14's errors and warnings as devkitARM has them). Files that compile at 32-bit but not on a
64-bit PC go in blocked_sources_64bit.txt. Each line keeps the file's first
error as its reason. The header comments of both files are kept.
"""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
GAME = ROOT / "3ds_port" / "game"


def results(path: Path) -> dict[str, dict]:
    report = json.loads(path.read_text(encoding="utf-8"))
    if report.get("sample_count") != report.get("source_count"):
        raise SystemExit(f"{path}: not a full survey (run the probe with PROBE_LIMIT=0)")
    return {r["source"]: r for r in report["results"]}


def reason(result: dict, width: int) -> str:
    text = result.get("diagnostic") or ""
    missing = re.search(r"fatal error: (\S+): No such", text)
    if missing:
        return "missing header " + missing.group(1)
    errors = [line for line in text.splitlines() if "error:" in line]
    line = errors[0] if errors else (text.splitlines() or [result["category"]])[0]
    return re.sub(r"^.*?error: ", "", line).strip()[:width]


def header(path: Path) -> str:
    """The leading comment lines of an existing list."""
    lines = path.read_text(encoding="utf-8").splitlines(keepends=True) if path.is_file() else []
    kept = []
    for line in lines:
        if not line.startswith("#"):
            break
        kept.append(line)
    return "".join(kept)


def write(path: Path, entries: list[tuple[str, str]]) -> None:
    body = "".join(f"{source:58} # {why}\n" for source, why in entries)
    path.write_text(header(path) + body, encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("report32", type=Path, help="full survey, TARGET=host32 (or 3ds)")
    parser.add_argument("report64", type=Path, help="full survey, TARGET=host")
    args = parser.parse_args()

    r32, r64 = results(args.report32), results(args.report64)
    if set(r32) != set(r64):
        raise SystemExit("the two reports cover different source files")
    only64 = [s for s in r32 if r32[s]["category"] == "passed" and r64[s]["category"] != "passed"]
    blocked = sorted(s for s in r32 if r32[s]["category"] != "passed")
    write(GAME / "blocked_sources.txt", [(s, reason(r32[s], 110)) for s in blocked])
    write(GAME / "blocked_sources_64bit.txt", [(s, reason(r64[s], 60)) for s in sorted(only64)])
    total = len(r32)
    print(f"3DS / 32-bit: {total - len(blocked)} of {total} compile; "
          f"64-bit PC: {total - len(blocked) - len(only64)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
