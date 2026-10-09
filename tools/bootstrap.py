#!/usr/bin/env python3
"""Fetch the pinned pret/pokeplatinum commit and apply our patches.

Usage:
    python tools/bootstrap.py           # fetch (if needed) + apply patches
    python tools/bootstrap.py --clean   # delete build/upstream and start over

Result: build/upstream/ contains the decompilation at the commit named in
upstream.lock, with every patches/pokeplatinum/*.patch applied in order.
"""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
LOCK_FILE = ROOT / "upstream.lock"
UPSTREAM_DIR = ROOT / "build" / "upstream"
PATCH_DIR = ROOT / "patches" / "pokeplatinum"


def read_lock(path: Path) -> dict[str, str]:
    """Parse key=value lines, ignoring blanks and # comments."""
    values: dict[str, str] = {}
    for line_no, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if "=" not in line:
            sys.exit(f"{path.name}:{line_no}: expected key=value, got {raw!r}")
        key, value = line.split("=", 1)
        values[key.strip()] = value.strip()
    for required in ("url", "commit"):
        if required not in values:
            sys.exit(f"{path.name}: missing '{required}='")
    return values


def git(*args: str, cwd: Path | None = None, capture: bool = False) -> str:
    result = subprocess.run(
        ["git", *args],
        cwd=cwd,
        check=False,
        text=True,
        capture_output=capture,
    )
    if result.returncode != 0:
        detail = (result.stderr or "").strip() if capture else ""
        sys.exit(f"git {' '.join(args)} failed{': ' + detail if detail else ''}")
    return (result.stdout or "").strip() if capture else ""


def current_commit(repo: Path) -> str | None:
    if not (repo / ".git").exists():
        return None
    return git("rev-parse", "HEAD", cwd=repo, capture=True)


def fetch(url: str, commit: str) -> None:
    """Shallow-fetch exactly one commit (fast: no full history download)."""
    print(f"Fetching {url} @ {commit[:12]} ...")
    UPSTREAM_DIR.mkdir(parents=True, exist_ok=True)
    git("init", "--quiet", cwd=UPSTREAM_DIR)
    git("remote", "add", "origin", url, cwd=UPSTREAM_DIR)
    git("fetch", "--quiet", "--depth", "1", "origin", commit, cwd=UPSTREAM_DIR)
    git("checkout", "--quiet", "--detach", "FETCH_HEAD", cwd=UPSTREAM_DIR)


def apply_patches() -> None:
    patches = sorted(PATCH_DIR.glob("*.patch"))
    if not patches:
        print("No patches to apply yet.")
        return
    for patch in patches:
        print(f"Applying {patch.name}")
        git("apply", "--check", str(patch), cwd=UPSTREAM_DIR)
        git("apply", str(patch), cwd=UPSTREAM_DIR)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--clean", action="store_true", help="delete build/upstream first")
    args = parser.parse_args()

    if shutil.which("git") is None:
        sys.exit("Git is not installed or not on PATH.")

    lock = read_lock(LOCK_FILE)

    if args.clean and UPSTREAM_DIR.exists():
        print(f"Removing {UPSTREAM_DIR.relative_to(ROOT)}")
        shutil.rmtree(UPSTREAM_DIR)

    have = current_commit(UPSTREAM_DIR)
    if have is None:
        fetch(lock["url"], lock["commit"])
    elif have != lock["commit"]:
        sys.exit(
            f"build/upstream is at {have[:12]}, but upstream.lock wants "
            f"{lock['commit'][:12]}.\nRun with --clean to re-fetch."
        )
    else:
        dirty = git("status", "--porcelain", cwd=UPSTREAM_DIR, capture=True)
        if dirty:
            sys.exit(
                "build/upstream already has local changes (patches applied or edits).\n"
                "Run with --clean to start fresh."
            )
        print(f"build/upstream already at {have[:12]}.")

    apply_patches()
    print(f"Done: {UPSTREAM_DIR.relative_to(ROOT)}")


if __name__ == "__main__":
    main()
