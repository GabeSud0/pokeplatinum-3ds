#!/usr/bin/env python3
"""Survey how the game code depends on the Nintendo DS SDK.

Usage:
    python tools/bootstrap.py                 # fetch the decomp first
    python tools/survey_sdk_usage.py          # summary by SDK module
    python tools/survey_sdk_usage.py --files  # also list the busiest files per module

Scans build/upstream/src/**/*.c for names from NitroSDK (GX_, G3_, OS_, FS_ ...)
and NitroSystem (NNS_G3d, NNS_G2d ...). Functions the game defines itself
(for example GXLayers_*) are excluded, so the numbers show real SDK use.
The numbers in docs/HARDWARE_LAYER.md come from this script.
"""

from __future__ import annotations

import argparse
import re
import sys
from collections import Counter, defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "build" / "upstream" / "src"

# Module prefix -> what it is. Order matters: longer prefixes first.
MODULES = [
    ("NNS_G3d", "NitroSystem 3D models, animation, materials"),
    ("NNS_G2d", "NitroSystem 2D sprites, cells, screens"),
    ("NNS_Gfd", "NitroSystem texture/palette VRAM managers"),
    ("NNS_Snd", "NitroSystem sound archive player"),
    ("NNS_Fnd", "NitroSystem heaps"),
    ("G3X_", "3D engine: global state (fog, edges, alpha)"),
    ("G3B_", "3D engine: command buffer building"),
    ("G3_", "3D engine: geometry commands"),
    ("G2S_", "2D engine B (sub screen): blending, windows, BG"),
    ("G2_", "2D engine A (main screen): blending, windows, BG"),
    ("GXS_", "2D engine B: load data, visible planes"),
    ("GX_", "display: VRAM banks, palettes, master brightness"),
    ("OS_", "interrupts, threads, reset, timing"),
    ("FS_", "filesystem and overlays"),
    ("MI_", "memory copy/fill, DMA"),
    ("DC_", "data cache"),
    ("CP_", "hardware divider"),
    ("FX32_", "fixed-point math"),
    ("FX16_", "fixed-point math"),
    ("FX_", "fixed-point math"),
    ("VEC_", "vector math"),
    ("MTX_", "matrix math"),
    ("MATH_", "math helpers, RNG, CRC"),
    ("SVC_", "BIOS calls"),
    ("PAD_", "buttons, lid fold"),
    ("TP_", "touch screen"),
    ("CARD_", "save memory (flash)"),
    ("RTC_", "real-time clock"),
    ("PM_", "power, backlight, speaker amp"),
    ("MIC_", "microphone"),
    ("SND_", "low-level sound"),
    ("WM_", "local wireless"),
    ("WCM_", "Wi-Fi connection manager"),
    ("DWC_", "Nintendo Wi-Fi Connection"),
]

NAME_RE = re.compile(
    r"\b((?:" + "|".join(re.escape(p) for p, _ in MODULES) + r")\w*)\b"
)
COMMENT_RE = re.compile(r"//[^\n]*|/\*.*?\*/", re.S)
DEF_RE = re.compile(r"^[A-Za-z_][\w \*]*?\b([A-Za-z_]\w*)\s*\([^;{]*\)\s*\{", re.M)


def module_of(name: str) -> str:
    for prefix, _ in MODULES:
        if name.startswith(prefix):
            return prefix
    return "?"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--files", action="store_true", help="list the busiest files per module")
    args = parser.parse_args()

    if not SRC.is_dir():
        print("build/upstream/src not found. Run: python tools/bootstrap.py")
        return 1

    sources: dict[str, str] = {}
    defined: set[str] = set()
    for path in sorted(SRC.rglob("*.c")):
        text = COMMENT_RE.sub("", path.read_text(encoding="utf-8", errors="replace"))
        sources[path.relative_to(SRC.parent).as_posix()] = text
        defined.update(DEF_RE.findall(text))

    uses: Counter[str] = Counter()          # name -> number of uses
    files_by_name: dict[str, set[str]] = defaultdict(set)
    for rel, text in sources.items():
        for match in NAME_RE.finditer(text):
            name = match.group(1)
            if name in defined:
                continue
            uses[name] += 1
            files_by_name[name].add(rel)

    files_with_sdk = set().union(*files_by_name.values()) if files_by_name else set()
    print(f"C files scanned:          {len(sources)}")
    print(f"Files using the SDK:      {len(files_with_sdk)}")
    print(f"Distinct SDK names used:  {len(uses)}  (functions, macros and constants)\n")

    description = dict(MODULES)
    rows = []
    for prefix, _ in MODULES:
        names = [n for n in uses if module_of(n) == prefix]
        if not names:
            continue
        files = set().union(*(files_by_name[n] for n in names))
        rows.append((prefix, sum(uses[n] for n in names), len(names), files, names))

    print(f"{'module':10} {'uses':>6} {'names':>6} {'files':>6}  description")
    for prefix, count, nnames, files, _ in sorted(rows, key=lambda r: -len(r[3])):
        print(f"{prefix:10} {count:6} {nnames:6} {len(files):6}  {description[prefix]}")

    if args.files:
        for prefix, _, _, _, names in sorted(rows, key=lambda r: -len(r[3])):
            # How many distinct names from this module each file uses.
            per_file = Counter(f for n in names for f in files_by_name[n])
            print(f"\n{prefix}  (distinct names per file):")
            for f, c in per_file.most_common(8):
                print(f"  {c:4}  {f}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
