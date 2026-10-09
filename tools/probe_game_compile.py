#!/usr/bin/env python3
"""Diagnose GCC compilation blockers across a sample of original game sources.

Does not change, copy, or package upstream game code and does not link
executables. Every probe uses the game library compile flags plus documented, target-specific\nGameSpy header-only platform selectors (not game/runtime build flags).
A failing probe is a data point, not an overall CI failure unless --strict
is selected.

Examples (after bootstrap and preparing the SDKs):
  make -C 3ds_port/game probe PROBE_LIMIT=64
  make -C 3ds_port/game TARGET=3ds probe PROBE_LIMIT=32
"""
from __future__ import annotations

import argparse
import collections
import hashlib
import json
import re
import shlex
import subprocess
import sys
import time
from pathlib import Path
from typing import Any

MISSING_FILE = re.compile(r"(?:fatal\s+)?error:.*No such file or directory", re.I)
MISSING_INCLUDED = re.compile(r"(?:fatal\s+)?error:\s*([^:]+):\s*No such file or directory", re.I)
TYPE_ERROR = re.compile(
    r"error:.*(?:unknown type name|incomplete type|conflicting types|"
    r"has no member named|undeclared|not declared|invalid use of|"
    r"incompatible type|implicit declaration)",
    re.I,
)


def categorize(stderr: str, timeout: bool = False) -> tuple[str, str]:
    """Return a stable blocker category and a single actionable diagnostic."""
    if timeout:
        return "timeout", "compiler exceeded the per-file timeout"

    first_error = next(
        (line.strip() for line in stderr.splitlines()
         if re.search(r"(?:fatal )?error:|undefined reference", line, re.I)),
        next((line.strip() for line in stderr.splitlines() if line.strip()), "compiler exited unsuccessfully"),
    )
    missing = MISSING_INCLUDED.search(first_error)
    if missing:
        name = missing.group(1).strip("'\" <>")
        if "generated/" in name or name.startswith("generated/"):
            return "missing_generated_header", first_error
        if name.startswith("res/") or name.startswith("data/"):
            return "missing_resource_header", first_error
        return "missing_header", first_error
    if MISSING_FILE.search(first_error):
        return "missing_file", first_error
    if "The GameSpy SDKs do not support this operating system" in first_error:
        return "unsupported_gamespy_platform", first_error
    if TYPE_ERROR.search(stderr):
        return "type_or_symbol", first_error
    if "error:" in stderr or "fatal error:" in stderr:
        return "compiler_error", first_error
    return "other_failure", first_error


def sample_sources(source_dir: Path, limit: int) -> tuple[list[Path], int]:
    """Deterministic spread across all src/ subdirectories, not only A–C."""
    if limit < 0:
        raise ValueError("limit cannot be negative")
    # The files the original build compiles (src/meson.build), where there
    # is one; otherwise every .c file.
    if (source_dir / "meson.build").is_file():
        sys.path.insert(0, str(Path(__file__).resolve().parent))
        from game_sources import listed_sources
        all_sources = [source_dir / name for name in listed_sources(source_dir.parent)]
    else:
        all_sources = sorted(source_dir.rglob("*.c"))
    if not all_sources:
        raise ValueError(f"{source_dir}: no original C files were found")
    # Hash each RELATIVE path, making the sample independent of temp paths,
    # OS runner checkout directories, and enumeration order.
    ranked = sorted(
        all_sources,
        key=lambda path: (
            hashlib.sha256(path.relative_to(source_dir).as_posix().encode()).digest(),
            path.relative_to(source_dir).as_posix(),
        ),
    )
    chosen = ranked if limit == 0 else ranked[:limit]
    return sorted(chosen), len(all_sources)


def probe(
    source_dir: Path,
    compiler: list[str],
    flags: list[str],
    *,
    limit: int,
    timeout: float,
    target: str,
) -> dict[str, Any]:
    files, total = sample_sources(source_dir, limit)
    results: list[dict[str, Any]] = []

    for source in files:
        rel = source.relative_to(source_dir).as_posix()
        cmd = [*compiler, *flags, "-fsyntax-only", str(source)]
        started = time.monotonic()
        try:
            proc = subprocess.run(
                cmd,
                check=False,
                capture_output=True,
                text=True,
                errors="replace",
                timeout=timeout,
            )
            if proc.returncode == 0:
                status, detail = "passed", ""
            else:
                status, detail = categorize(proc.stderr + "\n" + proc.stdout)
        except subprocess.TimeoutExpired:
            status, detail = categorize("", timeout=True)
        except OSError as exc:
            status, detail = "compiler_unavailable", str(exc)

        results.append({
            "source": rel,
            "category": status,
            "seconds": round(time.monotonic() - started, 3),
            "diagnostic": detail[:1000],
        })

    counts = dict(sorted(collections.Counter(r["category"] for r in results).items()))
    return {
        "schema_version": 1,
        "target": target,
        "source_root": "build/upstream/src",
        "source_count": total,
        "sample_count": len(results),
        "selection": "first N SHA-256-ranked relative paths (0 = all), then sorted",
        "compiler": compiler,
        "summary": counts,
        "results": results,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--source-dir", type=Path, required=True)
    parser.add_argument("--compiler", required=True, help="Compiler executable with optional arguments")
    parser.add_argument("--flags", default="", help="Existing game GCC flags, passed as a single quoted value")
    parser.add_argument("--target", default="host", help="Target label in the report")
    parser.add_argument("--limit", type=int, default=64, help="Number of deterministic samples; 0 probes all")
    parser.add_argument("--timeout", type=float, default=10.0, help="Per-source compile time limit in seconds")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--strict", action="store_true", help="Exit nonzero when any sampled source fails")
    args = parser.parse_args()

    if args.limit < 0 or args.timeout <= 0:
        parser.error("--limit must be >= 0 and --timeout must be positive")
    if not args.source_dir.is_dir():
        parser.error(f"{args.source_dir} does not exist; run bootstrap first")

    try:
        report = probe(
            args.source_dir.resolve(),
            shlex.split(args.compiler),
            shlex.split(args.flags),
            limit=args.limit,
            timeout=args.timeout,
            target=args.target,
        )
    except ValueError as exc:
        parser.error(str(exc))

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")

    summary = report["summary"]
    print(f"{args.target}: sampled {report['sample_count']} / {report['source_count']} original game C files")
    print("  " + ", ".join(f"{key}: {count}" for key, count in summary.items()))
    failures = [x for x in report["results"] if x["category"] != "passed"]
    for x in failures[:15]:
        print(f"  {x['source']} [{x['category']}]: {x['diagnostic']}")
    if len(failures) > 15:
        print(f"  ... {len(failures) - 15} further failures in the JSON report")
    print(f"Report: {args.output}")
    # A survey's purpose is to *find* errors; default non-gating.
    return 1 if args.strict and failures else 0


if __name__ == "__main__":
    sys.exit(main())
