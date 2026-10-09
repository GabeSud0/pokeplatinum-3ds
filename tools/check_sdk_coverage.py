#!/usr/bin/env python3
"""Check that every NitroSDK function the game calls exists in libnitro.

Usage:
    python tools/check_sdk_coverage.py [path/to/libnitro.a] [--nm NM] [--strict]

Defaults to the PC build (3ds_port/nitro/build/host/libnitro.a) and `nm`.

How it works: scans the game's C files (build/upstream/src) for calls to
NitroSDK names (GX_..., OS_..., FS_... and so on), drops names the game
defines itself and names that are macros or inline functions (in the SDK
headers or the game's own), and looks for the rest among the library's defined symbols.

Not covered here: NitroSystem (NNS_...), Nintendo Wi-Fi (DWC_...), and
NitroWiFi (WCM_..., SOC_...); they are separate libraries.
Also lists symbols the library itself uses that nothing defines.
With --strict, exits with status 1 if anything is missing.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
GAME_SRC = ROOT / "build" / "upstream" / "src"
SDK_INCLUDE = ROOT / "build" / "NitroSDK" / "include"
DEFAULT_LIB = ROOT / "3ds_port" / "nitro" / "build" / "host" / "libnitro.a"

PREFIXES = ("GX", "GXS", "G2", "G2S", "G3", "G3X", "G3B", "OS", "FS", "MI", "MIi",
            "DC", "IC", "CP", "FX", "VEC", "MTX", "MATH", "SVC", "PAD", "TP", "CARD",
            "RTC", "PM", "MIC", "SND", "WM", "SPI", "CTRDG", "MB", "PXI", "STD", "WBT", "WFS")

CALL_RE = re.compile(r"\b((?:" + "|".join(PREFIXES) + r")_\w+)\s*\(")
COMMENT_RE = re.compile(r"//[^\n]*|/\*.*?\*/", re.S)
DEF_RE = re.compile(r"^[A-Za-z_][\w \*]*?\b([A-Za-z_]\w*)\s*\([^;{]*\)\s*\{", re.M)
MACRO_RE = re.compile(r"^\s*#\s*define\s+(\w+)", re.M)
INLINE_RE = re.compile(r"(?:static\s+inline|SDK_INLINE|inline\s+static)\s+[\w \*]*?\b(\w+)\s*\(", re.S)


def game_calls() -> dict[str, set[str]]:
    calls: dict[str, set[str]] = defaultdict(set)
    defined: set[str] = set()
    for path in GAME_SRC.rglob("*.c"):
        text = COMMENT_RE.sub("", path.read_text(encoding="utf-8", errors="replace"))
        defined.update(DEF_RE.findall(text))
        for name in CALL_RE.findall(text):
            calls[name].add(path.relative_to(GAME_SRC.parent).as_posix())
    return {n: f for n, f in calls.items() if n not in defined}


def header_only_names() -> set[str]:
    """Macros and inline functions: in the SDK headers, or defined by the game itself."""
    names: set[str] = set()
    paths = list(SDK_INCLUDE.rglob("*.h"))
    paths += list((GAME_SRC.parent / "include").rglob("*.h")) + list(GAME_SRC.rglob("*.[ch]"))
    paths += list((GAME_SRC.parent / "lib").rglob("*.h"))  # the game's own libraries
    for path in paths:
        text = path.read_text(encoding="utf-8", errors="replace")
        names.update(MACRO_RE.findall(text))
        names.update(INLINE_RE.findall(text))
    return names


def library_symbols(lib: Path, nm: str) -> set[str]:
    out = subprocess.run([nm, "-g", "--defined-only", str(lib)], check=True,
                         capture_output=True, text=True).stdout
    return {line.split()[-1] for line in out.splitlines() if len(line.split()) == 3}


# Things the C library provides on every platform we build for.
LIBC = {"memcpy", "memset", "memmove", "memcmp", "strcmp", "strlen", "strncpy",
        "abort", "fputc", "stderr", "vfprintf", "vsnprintf", "printf", "sqrt"}


def library_unresolved(lib: Path, nm: str, have: set[str]) -> set[str]:
    """Symbols the library uses but neither defines nor gets from libc."""
    out = subprocess.run([nm, "-u", str(lib)], check=True, capture_output=True, text=True).stdout
    used = {line.split()[-1] for line in out.splitlines() if line.strip() and not line.endswith(":")}
    return {s for s in used - have if s not in LIBC and not s.startswith("__")}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("lib", nargs="?", default=str(DEFAULT_LIB))
    parser.add_argument("--nm", default="nm", help="nm program (arm-none-eabi-nm for 3DS builds)")
    parser.add_argument("--strict", action="store_true", help="fail if anything is missing")
    args = parser.parse_args()

    lib = Path(args.lib)
    if not GAME_SRC.is_dir():
        print("build/upstream not found. Run: python tools/bootstrap.py")
        return 1
    if not lib.is_file():
        print(f"{lib} not found. Build it first: make -C 3ds_port/nitro")
        return 1

    calls = game_calls()
    inline = header_only_names()
    needed = {n: f for n, f in calls.items() if n not in inline}
    have = library_symbols(lib, args.nm)
    missing = {n: f for n, f in needed.items() if n not in have}

    print(f"SDK names the game calls:        {len(calls)}")
    print(f"  macros / inline functions:      {len(calls) - len(needed)}")
    print(f"  need a library function:        {len(needed)}")
    print(f"  found in {lib.name}:            {len(needed) - len(missing)}")
    print(f"  missing:                        {len(missing)}")
    for name in sorted(missing):
        files = sorted(missing[name])
        more = f" (+{len(files) - 2} more)" if len(files) > 2 else ""
        print(f"    {name:40} {', '.join(files[:2])}{more}")

    unresolved = library_unresolved(lib, args.nm, have)
    print(f"Symbols {lib.name} uses but nothing provides: {len(unresolved)}")
    for name in sorted(unresolved):
        print(f"    {name}")

    return 1 if ((missing or unresolved) and args.strict) else 0


if __name__ == "__main__":
    sys.exit(main())
