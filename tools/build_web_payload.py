#!/usr/bin/env python3
"""A release's files for the web builder (the website that builds the port
in the player's browser, from their own ROM):

    python3 tools/build_web_payload.py --version 0.1.0 --commit SHA \\
        --program pokeplatinum.3dsx --elf pokeplatinum.elf --smdh pokeplatinum.smdh \\
        --cia-tools build/cia-tools-wasm --pycgfx build/pycgfx --out dist

writes, in dist/ (the release's assets):

    pokeplatinum.3dsx                  the program (for the Homebrew Launcher)
    PokePlatinum3DS-WebPayload.zip     what the website runs (below)
    web-manifest.json                  what's in it (also inside it)
    SHA256SUMS.txt                     the three files' SHA-256

The ZIP holds the program; what the HOME Menu version (.cia) is made of: the
program's ELF, its icon (SMDH), the game's RSF and the banner's chime, and
bannertool and makerom compiled for the browser (tools/build_cia_tools_wasm.sh,
run by web/js/cia.mjs); the builder's Python (builder/web.py and what it
imports); and the licences. No game data: everything from the ROM is made in the
browser. pycgfx (the 3D banner's converter) isn't in it either, as it has no
licence that allows sharing it: the manifest lists its files, at the pinned
commit, with their SHA-256, and the website fetches them from GitHub.

The ZIP is deterministic (sorted names, fixed dates): the same inputs give
the same bytes.

    python3 tools/build_web_payload.py --synthetic --out DIR

does the same with a made-up program (for the tests: no game code); any of
--program, --elf, --smdh, --cia-tools and --pycgfx given are used as they
are, and --synthetic-rom ROM makes the release accept that made-up ROM too.
    python3 tools/build_web_payload.py --pyodide-version
prints the Pyodide the website loads (and CI tests the builder in).
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "builder"))
sys.path.insert(0, str(ROOT / "tools" / "banner3d"))

import followers  # noqa: E402
import pack  # noqa: E402
import verify_rom  # noqa: E402
import web  # noqa: E402
from to_cgfx import PYCGFX_COMMIT  # noqa: E402

SCHEMA_VERSION = 1
KIND = "pokeplatinum-3ds-web-payload"
PAYLOAD_NAME = "PokePlatinum3DS-WebPayload.zip"
MANIFEST_NAME = "web-manifest.json"
PROGRAM_NAME = "pokeplatinum.3dsx"
SUMS_NAME = "SHA256SUMS.txt"
# The Pyodide the website loads (and CI runs the builder's tests in).
PYODIDE_VERSION = "0.27.7"
PYCGFX_REPOSITORY = "https://github.com/skyfloogle/pycgfx"
FIXED_TIME = (2020, 1, 1, 0, 0, 0)

# The builder's Python, as it is in the repository (web.py finds the rest
# from where it is).
PYTHON = ["builder/web.py", "builder/extract_rom.py", "builder/pack.py", "builder/verify_rom.py",
          "builder/followers.py", "builder/follower_sources.py", "builder/ball_sprites.py",
          "tools/home_menu_art.py"]
BANNER3D = "tools/banner3d"
CIA = {"cia/pokeplatinum.rsf": "3ds_port/game/cia/pokeplatinum.rsf",
       "cia/chime.wav": "3ds_port/game/cia/chime.wav"}
LICENSES = {"licenses/LICENSE": "LICENSE", "licenses/NOTICE.md": "NOTICE.md"}
# tools/build_cia_tools_wasm.sh's files: where they go in the ZIP
CIA_TOOLS = {"bannertool.wasm": "cia/tools/bannertool.wasm", "makerom.wasm": "cia/tools/makerom.wasm",
             "LICENSES.md": "licenses/cia-tools.md"}


def cia_tools_files(folder: Path) -> dict[str, bytes]:
    missing = [name for name in CIA_TOOLS if not (folder / name).is_file()]
    if missing:
        raise SystemExit(f"build_web_payload: {', '.join(missing)} not in {folder} (tools/build_cia_tools_wasm.sh)")
    return {dst: (folder / name).read_bytes() for name, dst in CIA_TOOLS.items()}


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def program_build_id() -> str:
    """PLATINUM_PAK_BUILD_ID: the data pack's build ID the program opens."""
    text = (ROOT / "3ds_port/game/app/platinum.h").read_text(encoding="utf-8")
    found = re.search(r'#define PLATINUM_PAK_BUILD_ID "([^"]+)"', text)
    if not found:
        raise SystemExit("build_web_payload: PLATINUM_PAK_BUILD_ID isn't in 3ds_port/game/app/platinum.h")
    return found.group(1)


def builder_files() -> dict[str, bytes]:
    """The builder's Python and the 3D banner's art: {path in the ZIP: bytes}."""
    files = {"python/" + p: (ROOT / p).read_bytes() for p in PYTHON}
    banner = ROOT / BANNER3D
    for path in sorted(banner.rglob("*")):
        rel = path.relative_to(banner)
        if (path.is_file() and rel.parts[0] not in ("dev",) and "__pycache__" not in rel.parts
                and path.suffix in (".py", ".png") and not path.name.startswith("test_")):
            files["python/" + BANNER3D + "/" + rel.as_posix()] = path.read_bytes()
    return files


def follower_manifest() -> dict:
    """Where the website fetches the follower sprites, in order, with their
    SHA-256 (builder/follower_sources.py)."""
    src = followers.sources
    return {"repository": f"https://github.com/{src.REPO}", "commit": src.COMMIT,
            "rawBase": f"https://raw.githubusercontent.com/{src.REPO}/{src.COMMIT}/",
            "files": [{"path": src.SPRITE_PATH.format(src.FIRST_FILE + n), "size": size, "sha256": digest}
                      for n, (size, digest) in enumerate(src.FILES)]}


def pycgfx_files(pycgfx: Path) -> list[dict]:
    """pycgfx's Python files (what to_cgfx loads), with their SHA-256."""
    out = []
    for path in sorted(pycgfx.rglob("*.py")):
        rel = path.relative_to(pycgfx)
        if ".git" in rel.parts or "__pycache__" in rel.parts:
            continue
        data = path.read_bytes()
        out.append({"path": rel.as_posix(), "sha256": sha256(data), "size": len(data)})
    if not any(f["path"] == "main.py" for f in out):
        raise SystemExit(f"build_web_payload: {pycgfx} has no main.py (make_banner.py --fetch-pycgfx-only)")
    return out


def make_manifest(files: dict[str, bytes], *, version: str, commit: str, pycgfx: list[dict],
                  synthetic: bool = False, extra_roms: list[dict] = ()) -> dict:
    build_id = program_build_id()
    if build_id != pack.DEFAULT_BUILD_ID:
        raise SystemExit(f"build_web_payload: the program opens packs with build ID {build_id!r}, "
                         f"but builder/pack.py makes {pack.DEFAULT_BUILD_ID!r}")
    roms = [{"sha1": sha1, "name": name} for sha1, name in sorted(verify_rom.SUPPORTED.items(),
                                                                  key=lambda item: item[1])]
    roms += list(extra_roms)
    return {
        "schemaVersion": SCHEMA_VERSION,
        "kind": KIND,
        "synthetic": synthetic,
        "releaseVersion": version,
        "releaseTag": "v" + version,
        "commit": commit,
        "supportedRoms": roms,
        # Packs of the same format and build ID work with this release's
        # program: if a player's pack matches, only the program needs replacing.
        "dataPack": {"format": pack.VERSION, "buildId": build_id},
        "pyodide": {"version": PYODIDE_VERSION, "packages": []},
        "entrypoint": {
            "pythonPath": ["python/builder"],
            "module": "web",
            "function": "run_web_build",
            "outputs": {"pak": web.PAK, "manifest": web.MANIFEST, "cgfx": web.CGFX,
                        "flatBanner": web.HOME_ART + "/banner.png"},
        },
        "sdCard": {
            "program/" + PROGRAM_NAME: "3ds/" + PROGRAM_NAME,
            web.PAK: web.PAK,
            web.MANIFEST: web.MANIFEST,
            "cia": "cia/pokeplatinum.cia",
        },
        "cia": {"elf": "cia/pokeplatinum.elf", "icon": "cia/pokeplatinum.smdh",
                "rsf": "cia/pokeplatinum.rsf", "chime": "cia/chime.wav",
                "bannertool": CIA_TOOLS["bannertool.wasm"], "makerom": CIA_TOOLS["makerom.wasm"]},
        "pycgfx": {"repository": PYCGFX_REPOSITORY, "commit": PYCGFX_COMMIT,
                   "rawBase": f"https://raw.githubusercontent.com/skyfloogle/pycgfx/{PYCGFX_COMMIT}/",
                   "files": pycgfx},
        # The follower Pokémon's sprites (HeartGold and SoulSilver's): fetched
        # by the website in the player's browser, checked, never in the
        # payload (builder/followers.py). The site passes them to the
        # entrypoint only when this is here.
        "followers": follower_manifest(),
        "files": {name: {"sha256": sha256(data), "size": len(data)} for name, data in sorted(files.items())},
    }


def write_zip(path: Path, files: dict[str, bytes]) -> None:
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for name in sorted(files):
            info = zipfile.ZipInfo(name, FIXED_TIME)
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o644 << 16
            info.create_system = 3
            z.writestr(info, files[name])


def build(out: Path, *, program: bytes, elf: bytes, smdh: bytes, cia_tools: dict[str, bytes], version: str,
          commit: str, pycgfx: list[dict], synthetic: bool = False, extra_roms: list[dict] = ()) -> dict:
    files = {"program/" + PROGRAM_NAME: program, "cia/pokeplatinum.elf": elf, "cia/pokeplatinum.smdh": smdh}
    files.update(cia_tools)
    files.update({dst: (ROOT / src).read_bytes() for dst, src in CIA.items()})
    files.update({dst: (ROOT / src).read_bytes() for dst, src in LICENSES.items()})
    files.update(builder_files())
    manifest = make_manifest(files, version=version, commit=commit, pycgfx=pycgfx, synthetic=synthetic,
                             extra_roms=extra_roms)
    manifest_bytes = (json.dumps(manifest, indent=2, ensure_ascii=False) + "\n").encode("utf-8")
    out.mkdir(parents=True, exist_ok=True)
    write_zip(out / PAYLOAD_NAME, {**files, MANIFEST_NAME: manifest_bytes})
    (out / MANIFEST_NAME).write_bytes(manifest_bytes)
    (out / PROGRAM_NAME).write_bytes(program)
    sums = "".join(f"{sha256((out / name).read_bytes())}  {name}\n"
                   for name in (PROGRAM_NAME, PAYLOAD_NAME, MANIFEST_NAME))
    (out / SUMS_NAME).write_text(sums, encoding="utf-8", newline="\n")
    return manifest


def synthetic_pycgfx() -> list[dict]:
    data = b"# not pycgfx\n"
    return [{"path": "main.py", "sha256": sha256(data), "size": len(data)}]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--pyodide-version", action="store_true", help="print the Pyodide version and stop")
    ap.add_argument("--synthetic", action="store_true", help="a made-up program (tests)")
    ap.add_argument("--synthetic-rom", type=Path, help="with --synthetic: a made-up ROM the release accepts too")
    ap.add_argument("--version", help="the release's version (0.1.0: its tag is v0.1.0)")
    ap.add_argument("--commit", help="the commit it's built from")
    ap.add_argument("--program", type=Path, help="pokeplatinum.3dsx")
    ap.add_argument("--elf", type=Path, help="the program's ELF (for makerom; its debug info can be stripped)")
    ap.add_argument("--smdh", type=Path, help="the program's icon (pokeplatinum.smdh)")
    ap.add_argument("--cia-tools", type=Path, help="bannertool.wasm and makerom.wasm (tools/build_cia_tools_wasm.sh)")
    ap.add_argument("--pycgfx", type=Path, help="pycgfx, fetched at the pinned commit")
    ap.add_argument("--out", type=Path, help="where to write the release's files")
    args = ap.parse_args()
    if args.pyodide_version:
        print(PYODIDE_VERSION)
        return 0
    if not args.out:
        ap.error("needs --out")
    if args.synthetic:
        # Made-up files, or any of the real ones given (the web builder's tests in a browser).
        read = lambda path, fake: path.read_bytes() if path else fake
        tools = (cia_tools_files(args.cia_tools) if args.cia_tools else
                 {dst: f"synthetic {name}".encode() for name, dst in CIA_TOOLS.items()})
        manifest = build(args.out, program=read(args.program, b"3DSX synthetic"),
                         elf=read(args.elf, b"\x7fELF synthetic"), smdh=read(args.smdh, b"SMDH synthetic"),
                         cia_tools=tools, version=args.version or "0.0.0", commit=args.commit or "0" * 40,
                         pycgfx=pycgfx_files(args.pycgfx) if args.pycgfx else synthetic_pycgfx(),
                         synthetic=True, extra_roms=[{
                             "sha1": hashlib.sha1(args.synthetic_rom.read_bytes()).hexdigest(),
                             "name": "Made-up test ROM (not a game)"}] if args.synthetic_rom else [])
    else:
        if not (args.version and args.commit and args.program and args.elf and args.smdh and args.cia_tools
                and args.pycgfx):
            ap.error("needs --version, --commit, --program, --elf, --smdh, --cia-tools and --pycgfx (or --synthetic)")
        if not re.fullmatch(r"\d+\.\d+\.\d+(-[0-9A-Za-z.]+)?", args.version):
            ap.error(f"--version {args.version!r}: like 0.1.0 (the tag v0.1.0)")
        head = subprocess.run(["git", "-C", str(args.pycgfx), "rev-parse", "HEAD"],
                              capture_output=True, text=True).stdout.strip()
        if head != PYCGFX_COMMIT:
            raise SystemExit(f"build_web_payload: {args.pycgfx} is at {head or '?'}, not {PYCGFX_COMMIT}")
        manifest = build(args.out, program=args.program.read_bytes(), elf=args.elf.read_bytes(),
                         smdh=args.smdh.read_bytes(), cia_tools=cia_tools_files(args.cia_tools),
                         version=args.version, commit=args.commit,
                         pycgfx=pycgfx_files(args.pycgfx))
    size = (args.out / PAYLOAD_NAME).stat().st_size
    print(f"Release {manifest['releaseTag']}: {PROGRAM_NAME}, {PAYLOAD_NAME} ({size:,} bytes, "
          f"{len(manifest['files'])} files), {MANIFEST_NAME}, {SUMS_NAME} in {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
