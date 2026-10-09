#!/usr/bin/env python3
"""Fetch the exact NitroDWC/NitroWiFi header revisions pinned by Platinum.

This is a *header preparation step* for original-game GCC compilation only:
it does not build the DS networking libraries, emulate WFC, or enable online
play. Source checkouts live under ignored build/; never commit them.

The revisions and canonical upstream repositories come from the pinned
pret/pokeplatinum/subprojects/*.wrap metadata.
"""
from __future__ import annotations

import argparse
import configparser
import re
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
EXPECTED = {
    "NitroDWC": "https://github.com/ntrtwl/NitroDWC.git",
    "NitroWiFi": "https://github.com/ntrtwl/NitroWiFi.git",
}
SHA = re.compile(r"[0-9a-f]{40}\Z")


def pinned_source(wrap_file: Path, name: str) -> tuple[str, str]:
    """Never fetch arbitrary repositories or moving branch/tag references."""
    if name not in EXPECTED:
        raise ValueError(f"unrecognized networking SDK {name!r}")
    conf = configparser.ConfigParser(interpolation=None)
    if not conf.read(wrap_file, encoding="utf-8") or "wrap-git" not in conf:
        raise ValueError(f"{wrap_file}: missing [wrap-git]")
    url = conf["wrap-git"].get("url", "")
    rev = conf["wrap-git"].get("revision", "")
    if url != EXPECTED[name]:
        raise ValueError(f"{wrap_file}: unexpected repository URL: {url!r}")
    if not SHA.fullmatch(rev):
        raise ValueError(f"{wrap_file}: revision must be a pinned full SHA-1")
    return url, rev


def git(*args: str) -> str:
    result = subprocess.run(
        ["git", *args], check=True, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        timeout=120,
    )
    return result.stdout.strip()


def prepare_checkout(url: str, revision: str, destination: Path) -> None:
    """Fetch/check out the pinned SHA, refusing unrelated or dirty trees."""
    destination = destination.resolve()
    if destination.exists():
        if not (destination / ".git").is_dir():
            raise ValueError(f"{destination}: exists but is not a git checkout")
        origin = git("-C", str(destination), "remote", "get-url", "origin")
        if origin != url:
            raise ValueError(f"{destination}: unexpected origin repository: {origin!r}")
        if git("-C", str(destination), "status", "--porcelain"):
            raise ValueError(f"{destination}: checkout has local modifications")
        current = git("-C", str(destination), "rev-parse", "HEAD")
        if current == revision:
            return
    else:
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.mkdir()
        git("-C", str(destination), "init", "-q")
        git("-C", str(destination), "remote", "add", "origin", url)

    # A full commit hash is required: no moving ref or branch is accepted.
    git("-C", str(destination), "fetch", "--quiet", "--depth=1", "origin", revision)
    git("-C", str(destination), "checkout", "--quiet", "--detach", "FETCH_HEAD")
    actual = git("-C", str(destination), "rev-parse", "HEAD")
    if actual != revision:
        raise RuntimeError(f"{destination}: expected commit {revision}, got {actual}")


def prepare(upstream: Path, output: Path) -> dict[str, Path]:
    result: dict[str, Path] = {}
    for name in ("NitroDWC", "NitroWiFi"):
        url, revision = pinned_source(upstream / "subprojects" / f"{name}.wrap", name)
        checkout = output / name
        prepare_checkout(url, revision, checkout)
        inc = checkout / "include"
        expected_header = "dwc.h" if name == "NitroDWC" else "nitroWiFi.h"
        if not (inc / expected_header).is_file():
            raise FileNotFoundError(f"{inc / expected_header}: pinned SDK header missing")
        result[name] = inc
    return result


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--upstream", type=Path, default=ROOT / "build" / "upstream")
    parser.add_argument("--output", type=Path, default=ROOT / "build" / "game-network-sdks")
    args = parser.parse_args()

    upstream = args.upstream.resolve()
    if not (upstream / "subprojects" / "NitroDWC.wrap").is_file():
        parser.error("pinned upstream missing; run python tools/bootstrap.py --clean")
    result = prepare(upstream, args.output.resolve())
    for name, inc in result.items():
        print(f"{name}: using pinned headers in {inc}")
    print("Headers only — online networking and NitroDWC implementation remain unported.")


if __name__ == "__main__":
    main()
