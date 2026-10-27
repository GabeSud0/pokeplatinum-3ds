#!/usr/bin/env python3
"""The web builder's part in Python: everything made from the player's ROM,
from its bytes, in memory. It runs in a browser (Pyodide, in a Web Worker),
so it starts no programs and writes no folder of extracted files; it runs
the same way on a PC:

    python3 builder/web.py platinum.nds OUT_DIR [--pycgfx build/pycgfx]

It writes, in OUT_DIR:

    3ds/pokeplatinum/platinum3ds.pak    the data pack   (= extract_rom.py + pack.py)
    3ds/pokeplatinum/manifest.json      its manifest    (the same bytes as theirs)
    work/banner.cgfx                    the 3D banner's scene (with --pycgfx; for bannertool)
    work/home-art/banner.png, .wav      the flat banner (the fallback for the 3D one)

The program (pokeplatinum.3dsx) isn't made here: it's the release's, built
without the ROM. Nothing is uploaded: the ROM and everything made from it
stay with the player. The release's web-manifest.json (made by
tools/build_web_payload.py) tells the website where this module is and what
it needs.
"""
from __future__ import annotations

import argparse
import hashlib
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
BANNER3D = os.path.join(ROOT, "tools", "banner3d")
TOOLS = os.path.join(ROOT, "tools")
for _path in (HERE, BANNER3D, TOOLS):
    if _path not in sys.path:
        sys.path.append(_path)

import extract_rom  # noqa: E402
import pack  # noqa: E402
import verify_rom  # noqa: E402

PAK = "3ds/pokeplatinum/platinum3ds.pak"
MANIFEST = "3ds/pokeplatinum/manifest.json"
CGFX = "work/banner.cgfx"
HOME_ART = "work/home-art"


def check_rom(rom, *, verify: bool = True) -> dict:
    """The ROM's SHA-1 and name; a ValueError if it isn't a supported dump
    (unless verify is False: the tests' made-up ROMs)."""
    sha1 = hashlib.sha1(rom).hexdigest()
    name = verify_rom.SUPPORTED.get(sha1)
    if name is None and verify:
        raise ValueError(f"Unsupported ROM (SHA-1 {sha1}): only clean Pokémon Platinum (USA) dumps work")
    return {"sha1": sha1, "name": name or "(not checked)"}


def build_data(rom, pak_file, *, rom_sha1: str, progress=None) -> str:
    """The data pack, written to the binary file pak_file, and its
    manifest.json's text (returned): the same bytes builder/extract_rom.py
    and builder/pack.py make from this ROM."""
    view = memoryview(rom)
    entries = extract_rom.parse_nitrofs(view)
    if any(e["path"] == "manifest.json" for e in entries):
        raise ValueError("ROM file would collide with manifest.json")
    files = [(e["id"], view[e["offset"]:e["offset"] + e["size"]]) for e in entries]
    pack.write_pak(pak_file, files, build_id=pack.DEFAULT_BUILD_ID, rom_sha1=rom_sha1, progress=progress)
    return extract_rom.manifest_text(rom_sha1, entries)


def build_banner_cgfx(rom, pycgfx_dir: str, work_dir: str) -> bytes:
    """The HOME Menu's 3D banner's scene (CGFX), from the ROM's title screen;
    pycgfx must already be in pycgfx_dir (the website fetches it)."""
    import make_banner
    archive = make_banner.titledemo_from_rom_data(memoryview(rom))
    return make_banner.cgfx_from_titledemo(archive, pycgfx_dir, work_dir)


def run_web_build(rom, out_dir: str, *, pycgfx_dir: str | None = None, progress=None,
                  verify: bool = True) -> dict:
    """The whole build (see the top of this file). progress(stage, done,
    total), if given, is called as it goes: stage "rom", "pack" or "banner".
    Returns what was made: {"rom": check_rom's answer, "files": {name:
    path}, "banner_error": why the 3D banner couldn't be made, or None}."""
    def report(stage, done, total):
        if progress is not None:
            progress(stage, done, total)

    report("rom", 0, 1)
    rom_info = check_rom(rom, verify=verify)
    report("rom", 1, 1)

    made = {}
    pak_path = os.path.join(out_dir, *PAK.split("/"))
    os.makedirs(os.path.dirname(pak_path), exist_ok=True)
    temp = pak_path + ".part"
    try:
        with open(temp, "wb") as f:
            manifest = build_data(rom, f, rom_sha1=rom_info["sha1"],
                                  progress=lambda done, total: report("pack", done, total))
        os.replace(temp, pak_path)
    finally:
        if os.path.exists(temp):
            os.unlink(temp)
    made[PAK] = pak_path
    manifest_path = os.path.join(out_dir, *MANIFEST.split("/"))
    with open(manifest_path, "w", encoding="utf-8", newline="\n") as f:
        f.write(manifest)
    made[MANIFEST] = manifest_path

    # The flat banner: quick, and what the .cia gets if the 3D one can't be made.
    import home_menu_art
    art_dir = os.path.join(out_dir, *HOME_ART.split("/"))
    home_menu_art.make(memoryview(rom), art_dir)
    made[HOME_ART + "/banner.png"] = os.path.join(art_dir, "banner.png")
    made[HOME_ART + "/banner.wav"] = os.path.join(art_dir, "banner.wav")

    banner_error = None
    if pycgfx_dir is not None:
        report("banner", 0, 1)
        work = os.path.join(out_dir, "work", "banner3d")
        try:
            cgfx = build_banner_cgfx(rom, pycgfx_dir, work)
        except Exception as e:  # the .cia still gets the flat banner
            banner_error = f"{type(e).__name__}: {e}"
        else:
            cgfx_path = os.path.join(out_dir, *CGFX.split("/"))
            with open(cgfx_path, "wb") as f:
                f.write(cgfx)
            made[CGFX] = cgfx_path
        report("banner", 1, 1)
    return {"rom": rom_info, "files": made, "banner_error": banner_error}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("rom", help="your own Pokémon Platinum (USA) .nds")
    parser.add_argument("out_dir")
    parser.add_argument("--pycgfx", help="pycgfx's folder (tools/banner3d/make_banner.py --fetch-pycgfx-only)")
    args = parser.parse_args()
    with open(args.rom, "rb") as f:
        rom = f.read()
    try:
        result = run_web_build(rom, args.out_dir, pycgfx_dir=args.pycgfx)
    except (OSError, ValueError) as e:
        parser.exit(1, f"Build failed: {e}\n")
    print(f"{result['rom']['name']}:")
    for name in result["files"]:
        print(f"  {name}")
    if result["banner_error"]:
        print(f"(no 3D banner: {result['banner_error']})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
