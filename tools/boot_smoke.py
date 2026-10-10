#!/usr/bin/env python3
"""Start the game on a PC with a made-up data pack, and say how far it gets.

Usage:
    python tools/boot_smoke.py 3ds_port/game/build/host32/pokeplatinum-pc

Runs the PC runner (3ds_port/game/app/main_pc.c) with a data pack holding
one made-up file and no game data. The game's start-up runs for real: the
SDK's initialisation, its threads, the interrupt handlers, the heaps, the
file system, the checks the game makes of its cartridge. It then stops at
the first thing the port doesn't do yet, or at the first file the game
reads, which this pack doesn't have. Either is a pass, and the log says
which. A crash, or the game stopping itself, is a failure.

On GitHub Actions the outcome is also printed as an annotation.
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from builder.pack import DEFAULT_BUILD_ID, pack  # noqa: E402
from builder.verify_rom import SUPPORTED  # noqa: E402

BUILD_ID = DEFAULT_BUILD_ID   # = PLATINUM_PAK_BUILD_ID in 3ds_port/game/app/platinum.h
TIMEOUT = 15


def make_pack(folder: Path) -> tuple[Path, Path]:
    """A pack with one made-up file (no game data), claiming a supported
    ROM so the program accepts it."""
    (folder / "made_up.bin").write_bytes(b"not game data")
    manifest = folder / "manifest.json"
    manifest.write_text(json.dumps({
        "rom_sha1": sorted(SUPPORTED)[0],
        "files": [{"id": 4000, "path": "made_up.bin", "size": 13}],
    }), encoding="utf-8")
    pak = folder / "platinum3ds.pak"
    pack(folder, pak, build_id=BUILD_ID)
    return pak, manifest


def classify(returncode: int | None, log: str) -> tuple[bool, str]:
    """(passed, what happened) from the runner's exit status (None: still
    running at the time limit) and its log."""
    lines = [line for line in log.splitlines() if line.strip()]
    last = lines[-1] if lines else "(no output)"
    if "[platinum] starting the game" not in log:
        return False, f"the game didn't start: {last}"
    if returncode == 5:
        missing = [l for l in lines if "isn't in the data pack" in l]
        where = missing[-1].split(": ", 1)[-1] if missing else "?"
        return True, f"reached the first file it reads ({where})"
    if returncode == 0:
        return True, "ran to the frame limit"
    if returncode is None:
        waiting = [l for l in lines if "not implemented:" in l]
        if waiting:
            what = waiting[-1].split("not implemented:", 1)[1].strip()
            return True, f"waits for something the port doesn't do yet ({what})"
        return False, f"stopped without saying why; last line: {last}"
    if returncode == 3:
        return False, "the game stopped itself (OS_Terminate)"
    if returncode >= 128 or returncode < 0:
        return False, f"crashed: {last}"
    return False, f"exit status {returncode}: {last}"


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("runner", type=Path)
    args = parser.parse_args()

    with tempfile.TemporaryDirectory() as tmp:
        pak, manifest = make_pack(Path(tmp))
        cmd = [str(args.runner.resolve()), str(pak), str(manifest),
               "--frames", "600", "--stop-at-missing-file"]
        try:
            run = subprocess.run(cmd, stderr=subprocess.PIPE, stdout=subprocess.DEVNULL,
                                 timeout=TIMEOUT, text=True, errors="replace")
            returncode, log = run.returncode, run.stderr
        except subprocess.TimeoutExpired as e:
            returncode = None
            log = e.stderr.decode("utf-8", "replace") if isinstance(e.stderr, bytes) else (e.stderr or "")

    passed, outcome = classify(returncode, log)
    print(log, end="" if log.endswith("\n") else "\n")
    print(f"Boot smoke test: {outcome}")
    if os.environ.get("GITHUB_ACTIONS"):
        level = "notice" if passed else "error"
        print(f"::{level} title=Boot smoke test ({args.runner.parent.name})::{outcome}")
    sys.exit(0 if passed else 1)


if __name__ == "__main__":
    main()
