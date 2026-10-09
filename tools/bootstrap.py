#!/usr/bin/env python3
"""Fetch the pinned decompilation and SDK sources, and apply our patches.

Usage:
    python tools/bootstrap.py           # fetch (if needed) + apply patches
    python tools/bootstrap.py --clean   # delete the fetched sources and start over

Result:
    build/upstream/     pret/pokeplatinum at the commit in upstream.lock,
                        with patches/pokeplatinum/*.patch applied
    build/NitroSDK/     the NitroSDK decompilation, at the revision the pinned
    build/NitroSystem/  pokeplatinum uses (its subprojects/*.wrap files),
                        with patches/<name>/*.patch applied
"""

from __future__ import annotations

import argparse
import configparser
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
LOCK_FILE = ROOT / "upstream.lock"
BUILD_DIR = ROOT / "build"
UPSTREAM_DIR = BUILD_DIR / "upstream"
PATCH_ROOT = ROOT / "patches"

# Libraries pokeplatinum pulls in that the 3DS port compiles too.
SUBPROJECTS = ("NitroSDK", "NitroSystem")


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


def read_wrap(name: str) -> tuple[str, str]:
    """Return (url, revision) from build/upstream/subprojects/<name>.wrap."""
    wrap = UPSTREAM_DIR / "subprojects" / f"{name}.wrap"
    if not wrap.is_file():
        sys.exit(f"{wrap.relative_to(ROOT)} not found; is build/upstream complete?")
    parser = configparser.ConfigParser()
    parser.read(wrap, encoding="utf-8")
    section = parser["wrap-git"]
    return section["url"], section["revision"]


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


def fetch(dest: Path, url: str, commit: str) -> None:
    """Shallow-fetch exactly one commit (fast: no full history download)."""
    print(f"Fetching {url} @ {commit[:12]} ...")
    dest.mkdir(parents=True, exist_ok=True)
    git("init", "--quiet", cwd=dest)
    git("remote", "add", "origin", url, cwd=dest)
    git("fetch", "--quiet", "--depth", "1", "origin", commit, cwd=dest)
    git("checkout", "--quiet", "--detach", "FETCH_HEAD", cwd=dest)


def apply_patches(dest: Path, patch_dir: Path) -> None:
    patches = sorted(patch_dir.glob("*.patch"))
    for patch in patches:
        print(f"  applying {patch_dir.name}/{patch.name}")
        git("apply", "--check", str(patch), cwd=dest)
        git("apply", str(patch), cwd=dest)


def prepare(name: str, dest: Path, url: str, commit: str, patch_dir: Path) -> None:
    """Make dest a clean checkout of url@commit with patch_dir applied."""
    have = current_commit(dest)
    if have is None:
        fetch(dest, url, commit)
    elif have != commit:
        sys.exit(
            f"{dest.relative_to(ROOT)} is at {have[:12]}, but {commit[:12]} is wanted.\n"
            "Run with --clean to re-fetch."
        )
    else:
        dirty = git("status", "--porcelain", cwd=dest, capture=True)
        if dirty:
            sys.exit(
                f"{dest.relative_to(ROOT)} already has local changes "
                "(patches applied or edits).\nRun with --clean to start fresh."
            )
        print(f"{name}: already at {commit[:12]}")
    apply_patches(dest, patch_dir)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--clean", action="store_true", help="delete fetched sources first")
    args = parser.parse_args()

    if shutil.which("git") is None:
        sys.exit("Git is not installed or not on PATH.")

    if args.clean:
        for d in (UPSTREAM_DIR, *(BUILD_DIR / n for n in SUBPROJECTS)):
            if d.exists():
                print(f"Removing {d.relative_to(ROOT)}")
                shutil.rmtree(d)

    lock = read_lock(LOCK_FILE)
    prepare("pokeplatinum", UPSTREAM_DIR, lock["url"], lock["commit"], PATCH_ROOT / "pokeplatinum")

    for name in SUBPROJECTS:
        url, revision = read_wrap(name)
        prepare(name, BUILD_DIR / name, url, revision, PATCH_ROOT / name)

    print("Done: build/upstream, " + ", ".join(f"build/{n}" for n in SUBPROJECTS))


if __name__ == "__main__":
    main()
