#!/usr/bin/env python3
"""The HOME Menu's animated 3D banner, made from your own copy of the game:

    python3 tools/banner3d/make_banner.py --rom platinum.nds --bannertool BANNERTOOL \\
        --chime 3ds_port/game/cia/chime.wav -o build/home-menu-art/banner3d.bnr
    (or --data-pack data_pack/ instead of --rom)

Giratina, its idle animation and its textures come from the title screen in
your ROM (demo/title/titledemo.narc), at build time; the banner made is
yours and never committed (it contains the game's model). The portal and the
logo are this project's own art (art/). The scene is turned into CGFX by
skyfloogle/pycgfx, fetched once at a pinned commit into build/pycgfx (needs
git), with the fixes in to_cgfx.py. Needs Python 3.11 or later; standard
library only. See README.md for what the banner is and how it was tested.
"""
import argparse
import hashlib
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "builder"))

TITLEDEMO = "demo/title/titledemo.narc"
GIRATINA_NSBMD, GIRATINA_NSBCA = 1, 2          # res/graphics/title_screen/titledemo.order
PYCGFX_REPO = "https://github.com/skyfloogle/pycgfx.git"
# The CGFX this makes from the supported ROM, as tested on a New 3DS XL
# (2026-10-23): a different hash means the scene or a tool changed.
TESTED_CGFX_SHA1 = "61f6c04af266d2d1207f9f8de7957613bc6ebd1d"


def titledemo_from_rom_data(data):
    """The title screen's archive, from the ROM's bytes."""
    import extract_rom
    for entry in extract_rom.parse_nitrofs(data):
        if entry["path"] == TITLEDEMO:
            return bytes(data[entry["offset"]:entry["offset"] + entry["size"]])
    raise ValueError(f"{TITLEDEMO} isn't in this ROM")


def titledemo_from_rom(rom_path):
    with open(rom_path, "rb") as f:
        return titledemo_from_rom_data(f.read())


def cgfx_from_titledemo(archive, pycgfx_dir, work):
    """The banner's CGFX: Giratina from the title screen's archive, the
    project's art, turned into CGFX with pycgfx (already in pycgfx_dir).
    The glTF scene is written to the folder work."""
    import narc
    import scene
    import to_cgfx

    nsbmd, nsbca = narc.member(archive, GIRATINA_NSBMD), narc.member(archive, GIRATINA_NSBCA)
    if nsbmd[:4] != b"BMD0" or nsbca[:4] != b"BCA0":
        raise ValueError("titledemo.narc doesn't hold the title-screen Giratina where expected")
    gltf = scene.build(nsbmd, nsbca, os.path.join(HERE, "art"), work)
    return to_cgfx.convert(pycgfx_dir, gltf)


def fetch_pycgfx(dest, commit):
    if os.path.isfile(os.path.join(dest, "main.py")):
        head = subprocess.run(["git", "-C", dest, "rev-parse", "HEAD"], capture_output=True, text=True).stdout.strip()
        if head == commit:
            return
    print(f"Fetching pycgfx ({commit[:7]}) into {dest}")
    os.makedirs(dest, exist_ok=True)
    git = lambda *a: subprocess.run(["git", "-C", dest, *a], check=True, stdout=subprocess.DEVNULL)
    if not os.path.isdir(os.path.join(dest, ".git")):
        git("init", "-q")
    git("fetch", "-q", "--depth", "1", PYCGFX_REPO, commit)
    git("checkout", "-q", "--force", "FETCH_HEAD")


def main():
    if sys.version_info < (3, 11):
        sys.exit("The 3D banner needs Python 3.11 or later (pycgfx uses it).")
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--fetch-pycgfx-only", action="store_true", help="just fetch pycgfx (for the tests)")
    src = ap.add_mutually_exclusive_group()
    src.add_argument("--rom", help="your Pokemon Platinum ROM")
    src.add_argument("--data-pack", help="your extracted data pack (builder/extract_rom.py)")
    ap.add_argument("--bannertool", help="bannertool (tools/build_cia_tools.sh)")
    ap.add_argument("--chime", help="the banner's sound (.wav, at most 3 seconds)")
    ap.add_argument("--pycgfx", default=os.path.join(ROOT, "build", "pycgfx"), help="where pycgfx is fetched")
    ap.add_argument("-o", "--output", help="the .bnr to write")
    ap.add_argument("--keep", help="keep the glTF scene and CGFX in this folder")
    args = ap.parse_args()

    import to_cgfx

    if args.fetch_pycgfx_only:
        fetch_pycgfx(args.pycgfx, to_cgfx.PYCGFX_COMMIT)
        return
    if not ((args.rom or args.data_pack) and args.bannertool and args.chime and args.output):
        ap.error("needs --rom or --data-pack, --bannertool, --chime and -o")

    if args.rom:
        archive = titledemo_from_rom(args.rom)
    else:
        archive = open(os.path.join(args.data_pack, *TITLEDEMO.split("/")), "rb").read()

    fetch_pycgfx(args.pycgfx, to_cgfx.PYCGFX_COMMIT)
    with tempfile.TemporaryDirectory() as tmp:
        work = args.keep or tmp
        try:
            cgfx = cgfx_from_titledemo(archive, args.pycgfx, work)
        except ValueError as e:
            raise SystemExit(str(e))
        cgfx_path = os.path.join(work, "banner.cgfx")
        with open(cgfx_path, "wb") as f:
            f.write(cgfx)
        digest = hashlib.sha1(cgfx).hexdigest()
        os.makedirs(os.path.dirname(os.path.abspath(args.output)), exist_ok=True)
        subprocess.run([args.bannertool, "makebanner", "-ci", cgfx_path, "-a", args.chime, "-o", args.output],
                       check=True, stdout=subprocess.DEVNULL)
    note = "the tested banner" if digest == TESTED_CGFX_SHA1 else "differs from the tested banner"
    print(f"Made {args.output} (3D banner, CGFX {len(cgfx)} bytes, {note})")


if __name__ == "__main__":
    main()
