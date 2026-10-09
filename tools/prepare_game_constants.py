#!/usr/bin/env python3
"""Generate upstream Platinum C constant headers using its pinned Metang tool.

This intentionally uses the authoritative manifest settings in
build/upstream/generated/meson.build rather than guessing enum values, bit
masks, names, or tags. All generated headers and the fetched Metang checkout
stay in the ignored build/ directory.

Usage:
    python tools/bootstrap.py --clean
    python tools/prepare_game_constants.py
    python tools/prepare_game_constants.py --only abilities species move_flags
    python tools/prepare_game_constants.py --metang /path/to/metang.py
"""
from __future__ import annotations

import argparse
import ast
import configparser
import re
import subprocess
import sys
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_UPSTREAM = ROOT / "build" / "upstream"
DEFAULT_OUTPUT = ROOT / "build" / "game-constants"
DEFAULT_METANG_CACHE = ROOT / "build" / "metang"
METANG_GUARD = "POKEPLATINUM_GENERATED"

# The upstream Meson mapping is currently a dictionary of *literal* values.
# Parse only that dictionary with literal_eval; do NOT execute Meson or eval.
MAP_START = re.compile(r"(?m)^metang_generators\s*=\s*\{")
SAFE_NAME = re.compile(r"[A-Za-z_][A-Za-z_0-9]*\Z")
SAFE_KEY = re.compile(r"[a-z][a-z_0-9]*\Z")


def generator_specs(meson_file: Path) -> dict[str, dict[str, Any]]:
    """Read the authoritative upstream metang_generators mapping safely."""
    source = meson_file.read_text(encoding="utf-8")
    found = MAP_START.search(source)
    if found is None:
        raise ValueError(f"{meson_file}: metang_generators map not found")

    # Match the enclosing braces instead of depending on the position of
    # the next Meson statement. All current map entries are Python literals.
    start = source.index("{", found.start())
    depth = 0
    end = -1
    for pos in range(start, len(source)):
        char = source[pos]
        if char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                end = pos + 1
                break
    if end < 0:
        raise ValueError(f"{meson_file}: unfinished metang_generators map")

    try:
        mapping = ast.literal_eval(source[start:end])
    except (ValueError, SyntaxError) as exc:
        raise ValueError(f"{meson_file}: non-literal generator definitions") from exc
    if not isinstance(mapping, dict) or not mapping:
        raise ValueError(f"{meson_file}: expected a non-empty generator map")

    for key, spec in mapping.items():
        if not isinstance(key, str) or SAFE_KEY.fullmatch(key) is None:
            raise ValueError(f"{meson_file}: invalid manifest name {key!r}")
        if not isinstance(spec, dict):
            raise ValueError(f"{meson_file}: {key}: expected generator settings")
        if set(spec) - {"type", "tag", "extra"}:
            raise ValueError(f"{meson_file}: {key}: unexpected generator option")
        if spec.get("type") not in {"enum", "mask"}:
            raise ValueError(f"{meson_file}: {key}: unsupported generator type")
        tag = spec.get("tag")
        if not isinstance(tag, str) or SAFE_NAME.fullmatch(tag) is None:
            raise ValueError(f"{meson_file}: {key}: invalid enum tag {tag!r}")
        extra = spec.get("extra", [])
        if not isinstance(extra, list) or any(v != "--no-auto" for v in extra):
            raise ValueError(f"{meson_file}: {key}: unsupported metang argument")
        if extra and spec["type"] != "mask":
            raise ValueError(f"{meson_file}: {key}: --no-auto only works for masks")

    return mapping


def pinned_metang(upstream: Path, cache: Path) -> Path:
    """Retrieve only the original Metang release selected by the upstream wrap."""
    wrap_file = upstream / "subprojects" / "metang.wrap"
    parser = configparser.ConfigParser()
    if not parser.read(wrap_file, encoding="utf-8") or "wrap-git" not in parser:
        raise ValueError(f"{wrap_file}: missing wrap-git configuration")
    url = parser["wrap-git"].get("url", "")
    revision = parser["wrap-git"].get("revision", "")

    # Defend against accidentally executing an unrelated or arbitrary tool
    # if the source checkout / wrapper is corrupted.
    if url != "https://github.com/lhearachel/metang.git":
        raise ValueError(f"{wrap_file}: unexpected Metang repository URL")
    if not revision or revision.startswith("-") or re.fullmatch(r"[\w./-]+", revision) is None:
        raise ValueError(f"{wrap_file}: unsafe Metang revision")

    cache.parent.mkdir(parents=True, exist_ok=True)
    if not (cache / ".git").is_dir():
        if cache.exists():
            raise ValueError(f"{cache}: exists but is not a Git checkout")
        subprocess.run(
            ["git", "clone", "--quiet", "--depth", "1", "--branch", revision, url, str(cache)],
            check=True,
        )
    else:
        # Refresh to the wrapped revision, even if a previous run had a
        # different version of upstream checked out in the same cache.
        subprocess.run(
            ["git", "-C", str(cache), "fetch", "--quiet", "--depth", "1", "origin", revision],
            check=True,
        )
        subprocess.run(
            ["git", "-C", str(cache), "checkout", "--quiet", "--detach", "FETCH_HEAD"],
            check=True,
        )

    executable = cache / "metang.py"
    if not executable.is_file():
        raise FileNotFoundError(f"{executable}: pinned Metang entry point missing")
    return executable


def fix_gcc_mask_shifts(content: str) -> str:
    """Preserve original mask values without signed-int overflow in GCC.

    Metang v0.2.0-python emits expressions such as (1 << 31) and
    ((1 << 32) - 1) for the original 32-bit AI_FLAG_ALL value. Shifting a
    signed 32-bit '1' by 31/32 is undefined/invalid. Widen ONLY the shift
    operand in the generated header; the bit values remain identical.
    The pinned tool and original source manifests remain unchanged.
    """
    def widen(match: re.Match[str]) -> str:
        bits = int(match.group(1))
        if bits >= 64:
            raise ValueError(f"Metang mask shift {bits} exceeds 64-bit intermediate")
        return "1ULL << " + match.group(1)

    return re.sub(r"\b1\s*<<\s*(\d+)\b", widen, content)


def generate(
    upstream: Path,
    output: Path,
    metang: Path,
    only: list[str] | None = None,
) -> list[Path]:
    """Generate official Metang C headers for all or selected upstream lists."""
    definitions = generator_specs(upstream / "generated" / "meson.build")
    selected = sorted(definitions) if not only else sorted(set(only))
    unknown = set(selected) - set(definitions)
    if unknown:
        raise ValueError(f"unknown generated constant groups: {', '.join(sorted(unknown))}")
    if not metang.is_file():
        raise FileNotFoundError(metang)

    result: list[Path] = []
    target_dir = output / "generated"
    target_dir.mkdir(parents=True, exist_ok=True)
    for key in selected:
        spec = definitions[key]
        src = upstream / "generated" / f"{key}.txt"
        if not src.is_file():
            raise FileNotFoundError(f"{src}: pinned source list missing")
        dest = target_dir / f"{key}.h"
        cmd = [
            sys.executable,
            str(metang),
            spec["type"],
            "--tag-name",
            spec["tag"],
            "--guard",
            METANG_GUARD,
            "--output",
            str(dest),
            *spec.get("extra", []),
            str(src),
        ]
        subprocess.run(cmd, check=True, cwd=metang.parent)
        if not dest.is_file():
            raise RuntimeError(f"Metang did not produce {dest}")
        if spec["type"] == "mask":
            raw = dest.read_text(encoding="utf-8")
            corrected = fix_gcc_mask_shifts(raw)
            if raw != corrected:
                dest.write_text(corrected, encoding="utf-8")
        result.append(dest)

    return result


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--upstream", type=Path, default=DEFAULT_UPSTREAM)
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument("--metang", type=Path, help="Existing metang.py; skips fetching the pinned tool")
    parser.add_argument("--only", nargs="+", metavar="GROUP", help="Select specific manifest names")
    args = parser.parse_args()

    upstream = args.upstream.resolve()
    if not (upstream / "generated" / "meson.build").is_file():
        parser.error(f"{upstream} is missing; run 'python tools/bootstrap.py --clean' first")

    metang = args.metang.resolve() if args.metang else pinned_metang(upstream, DEFAULT_METANG_CACHE)
    files = generate(upstream, args.output.resolve(), metang, args.only)
    print(f"Generated {len(files)} official Platinum C headers in {args.output.resolve() / 'generated'}")
    print(f"Add -I{args.output.resolve()} and -D{METANG_GUARD}_ENUM to a GCC compile.")


if __name__ == "__main__":
    main()
