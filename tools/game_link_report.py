#!/usr/bin/env python3
"""Turn the linker's "undefined reference" errors for the full game into a report.

Usage:
    python tools/game_link_report.py LINKER_OUTPUT.txt --target 3ds [--output REPORT.md]

`make -C 3ds_port/game link-check` links the whole game from NitroMain (as the
original program starts) and runs this on what the linker printed. Every
symbol the game reaches but nothing defines is listed once, sorted by why:

- defined in a game source that doesn't compile yet (blocked_sources*.txt),
- Nintendo Wi-Fi SDKs (NitroDWC, PPW lobby, VCT voice chat, WVR), which the
  port doesn't include,
- overlay IDs, which the DS linker script generated (SDK_OVERLAY_*_ID),
- written in assembly in the decompilation (lib/*/asm, src/**/*.s), which
  needs a C version or the 3DS assembler,
- anything else.

It's a to-do list, not a pass/fail check: the script exits 0 either way.
"""

from __future__ import annotations

import argparse
import os
import re
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
GAME = ROOT / "build" / "upstream"
PORT = ROOT / "3ds_port" / "game"

UNDEFINED = re.compile(r"undefined reference to [`'‘]([^'’`]+)['’]")
DEFINITION = re.compile(r"^[A-Za-z_][\w \*]*?\b([A-Za-z_]\w*)\s*\([^;{]*\)\s*\{", re.M)
DATA = re.compile(r"^(?:const\s+)?[A-Za-z_][\w \*]*?\b([A-Za-z_]\w*)(?:\[[^\]]*\])*\s*=", re.M)
WIFI = re.compile(r"(DWC|DWCi|PPW|VCT|WVR|SOC|SOCL|WCM|CPS|NHTTP)_")


def blocked(name: str) -> list[str]:
    path = PORT / name
    if not path.is_file():
        return []
    return [line.split("#")[0].strip() for line in path.read_text(encoding="utf-8").splitlines()
            if line.strip() and not line.startswith("#")]


def definitions(sources: list[str]) -> dict[str, str]:
    found: dict[str, str] = {}
    for source in sources:
        path = GAME / "src" / source
        if not path.is_file():
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        for pattern in (DEFINITION, DATA):
            for match in pattern.finditer(text):
                found.setdefault(match.group(1), source)
    return found


def assembly_labels() -> dict[str, str]:
    """Function labels defined in the game's linked assembly files."""
    found: dict[str, str] = {}
    paths = list((GAME / "lib").glob("*/asm/*.s")) + list((GAME / "src").rglob("*.s"))
    for path in paths:
        for match in re.finditer(r"^\s*(?:arm|thumb)_func_start\s+(\w+)|^(\w+):", path.read_text(
                encoding="utf-8", errors="replace"), re.M):
            found.setdefault(match.group(1) or match.group(2), str(path.relative_to(GAME)))
    return found


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("linker_output", type=Path)
    parser.add_argument("--target", default="?")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()

    output = args.linker_output.read_text(encoding="utf-8", errors="replace")
    symbols = sorted(set(UNDEFINED.findall(output)))
    # Anything else the linker complained about (a missing library, a
    # memory region overflow ...): the link failed for another reason.
    other_errors = [line for line in output.splitlines()
                    if re.search(r"\berror\b|cannot find|multiple definition|overflow", line, re.I)
                    and not UNDEFINED.search(line) and "ld returned" not in line]
    in_blocked = definitions(blocked("blocked_sources.txt"))
    in_blocked_64 = definitions(blocked("blocked_sources_64bit.txt"))
    in_assembly = assembly_labels()

    groups: dict[str, list[str]] = defaultdict(list)
    by_file: dict[str, list[str]] = defaultdict(list)
    for symbol in symbols:
        if symbol.startswith("SDK_OVERLAY_"):
            groups["Overlay IDs (made by the DS linker script)"].append(symbol)
        elif symbol in in_blocked or symbol in in_blocked_64:
            source = in_blocked.get(symbol) or in_blocked_64[symbol]
            groups["In game sources that don't compile yet"].append(symbol)
            by_file[source].append(symbol)
        elif symbol in in_assembly:
            groups["Written in assembly (needs a C version or the 3DS assembler)"].append(symbol)
        elif WIFI.match(symbol):
            groups["Nintendo Wi-Fi SDKs (not part of the port)"].append(symbol)
        else:
            groups["Other"].append(symbol)

    lines = [f"# Full game link: {len(symbols)} missing symbols ({args.target})", ""]
    if other_errors:
        lines += [f"## The link failed for other reasons: {len(other_errors)} lines", "", "```"]
        lines += other_errors[:40] + ["```", ""]
    elif not symbols:
        lines.append("The whole game links.")
    for title, names in sorted(groups.items(), key=lambda kv: -len(kv[1])):
        lines += [f"## {title}: {len(names)}", ""]
        if title.startswith("In game sources"):
            for source, names_here in sorted(by_file.items(), key=lambda kv: (-len(kv[1]), kv[0])):
                lines.append(f"- `{source}` ({len(names_here)}): " + ", ".join(names_here[:6])
                             + (" ..." if len(names_here) > 6 else ""))
        else:
            lines.append(", ".join(f"`{n}`" for n in names))
        lines.append("")
    report = "\n".join(lines)
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(report, encoding="utf-8")
    summary = ", ".join(f"{len(v)} {k.split(' (')[0].lower()}" for k, v in groups.items())
    line = f"Full game link ({args.target}): {len(symbols)} missing symbols" + (f" — {summary}" if summary else "")
    if other_errors:
        line += f"; the link also failed for other reasons, first: {other_errors[0].strip()[:200]}"
    print(line)
    if os.environ.get("GITHUB_ACTIONS") == "true":
        # Shown on the run's summary page, next to the uploaded report.
        print(f"::notice title=Full game link ({args.target})::{line}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
