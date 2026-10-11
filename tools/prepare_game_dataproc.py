#!/usr/bin/env python3
"""Build the decompilation's own data tools and use them to generate headers.

Usage:
    python tools/prepare_game_dataproc.py --upstream build/upstream \
        --constants build/game-constants --resources build/game-resource-headers \
        --work build/game-tools

pret/pokeplatinum generates several C headers with small C programs in
tools/dataproc (speciesproc, itemproc, frontierproc, trainerproc,
resdatproc): tables of egg moves, tutor moves, footprints, the berry list,
frontier trainer classes, sprite templates ... Each reads the decompilation's JSON data and fills a template
from tools/dataproc/data. The two images the game embeds in its code
(nitrogfx -embed in the original) are converted by tools/png_embed.py, the
same conversion in Python, so no image library is needed. The original builds them with Meson; Meson isn't
part of the port's toolchain, so this script builds them the same way
their meson.build files do, with the PC's C compiler:

- yyjson, at the version and with the patches pinned in
  subprojects/yyjson.wrap (downloaded from GitHub, SHA-256 checked),
- nitroarc's library, libenum and libexpr from tools/,
- the dataproc library, its common code, and each tool.

Then it runs each tool with the arguments and environment its meson.build
rule gives it, into the port's generated-header folder. The tools also
write the game's data archives (.narc) and text-bank JSON next to the
headers; the archives are deleted (the game reads them from the player's
own data pack), and trainerproc's npc_trainer_names.json becomes the
text-bank index header res/text/bank/npc_trainer_names.h.

Everything is written under build/, which is never committed.
"""

from __future__ import annotations

import argparse
import configparser
import hashlib
import os
import shutil
import subprocess
import sys
import tarfile
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import re  # noqa: E402

import png_embed  # noqa: E402
from prepare_game_graphics_naix import custom_targets  # noqa: E402
from prepare_game_ordergen_naix import folder_names  # noqa: E402
from prepare_game_text_bank_headers import render as render_text_bank  # noqa: E402

CC = os.environ.get("HOST_CC", "cc")
CFLAGS = ["-std=gnu17", "-O2", "-w"]


def run(cmd: list[str], **kwargs) -> None:
    subprocess.run(cmd, check=True, **kwargs)


def fetch_yyjson(upstream: Path, work: Path) -> Path:
    """yyjson as subprojects/yyjson.wrap pins it, with its diff_files applied."""
    wrap = configparser.ConfigParser()
    wrap.read(upstream / "subprojects" / "yyjson.wrap", encoding="utf-8")
    spec = wrap["wrap-file"]
    folder = work / spec["directory"]
    stamp = folder / ".port-patched"
    if stamp.is_file():
        return folder
    archive = work / spec["source_filename"]
    if not archive.is_file():
        urls = [spec["source_url"], spec.get("source_fallback_url", "")]
        for url in filter(None, urls):
            try:
                with urllib.request.urlopen(url, timeout=60) as response:
                    archive.write_bytes(response.read())
                break
            except OSError:
                continue
    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    if digest != spec["source_hash"]:
        archive.unlink()
        raise SystemExit(f"yyjson download has the wrong SHA-256 ({digest})")
    shutil.rmtree(folder, ignore_errors=True)
    with tarfile.open(archive) as tar:
        if hasattr(tarfile, "data_filter"):
            tar.extractall(work, filter="data")
        else:
            # Pythons before the extraction filters (Debian's 3.11.2 in the
            # devkitPro image): the archive's hash is pinned, but still refuse
            # anything that would land outside the work folder.
            for member in tar.getmembers():
                name = Path(member.name)
                if (name.is_absolute() or ".." in name.parts
                        or not (member.isfile() or member.isdir())):
                    raise SystemExit(f"yyjson archive: unexpected member {member.name}")
            tar.extractall(work)
    for diff in (d.strip() for d in spec.get("diff_files", "").split(",") if d.strip()):
        patch = upstream / "subprojects" / "packagefiles" / diff
        targets = [line.split()[1].split("/", 1)[1] for line in
                   patch.read_text(encoding="utf-8").splitlines() if line.startswith("+++ ")]
        # always-native.patch only edits the Meson build file (from wrapdb's
        # patch zip, which this build doesn't use); the tools are built for
        # the PC here anyway. Every patch to the C source is applied.
        if all(Path(t).name == "meson.build" and not (folder / t).exists() for t in targets):
            continue
        run(["patch", "-s", "-p1", "-d", str(folder), "-i", str(patch)])
    stamp.write_text("ok\n", encoding="utf-8")
    return folder


def compile_objects(sources: list[Path], out: Path, flags: list[str]) -> list[Path]:
    objects = []
    for source in sources:
        obj = out / (source.parent.name + "_" + source.stem + ".o")
        if not obj.is_file() or obj.stat().st_mtime < source.stat().st_mtime:
            run([CC, *CFLAGS, *flags, "-c", str(source), "-o", str(obj)])
        objects.append(obj)
    return objects


def build_tools(upstream: Path, buildroot: Path, work: Path, tools: list[str]) -> dict[str, Path]:
    """The libraries and tools from tools/dataproc/meson.build."""
    yyjson = fetch_yyjson(upstream, work)
    objs = work / "obj"
    objs.mkdir(parents=True, exist_ok=True)
    t = upstream / "tools"
    dp, arc, enum = t / "dataproc", t / "nitroarc", t / "enumproc"
    includes = ["-I", str(dp / "lib" / "include"), "-I", str(yyjson / "src"),
                "-I", str(arc / "lib" / "include"), "-I", str(enum)]
    libs = compile_objects([yyjson / "src" / "yyjson.c"], objs, [])
    libs += compile_objects([arc / "lib" / "nitroarc.c"], objs, ["-DNDEBUG", *includes])
    libs += compile_objects([enum / "libenum.c", enum / "libexpr.c"], objs, includes)
    libs += compile_objects([dp / "lib" / "dataproc.c", dp / "lib" / "json.c"], objs, includes)
    libs += compile_objects([dp / "src" / "common.c"], objs, [
        *includes,
        f'-DREPO_INCLUDE="{upstream / "include"}"',
        f'-DREPO_BUILD="{buildroot}"',
        f'-DTEMPLATES_DIR="{dp / "data"}"',
    ])
    built = {}
    for tool in tools:
        exe = work / tool
        source = dp / "src" / f"{tool}.c"
        newest = max(p.stat().st_mtime for p in [source, *libs])
        if not exe.is_file() or exe.stat().st_mtime < newest:
            run([CC, *CFLAGS, *includes, "-I", str(dp / "src"), "-I", str(upstream / "include"),
                 "-I", str(upstream / "asm"), "-I", str(upstream), "-I", str(buildroot),
                 str(source), *map(str, libs), "-lm", "-o", str(exe)])
        built[tool] = exe
    return built


def embed_jobs(upstream: Path) -> list[tuple[Path, Path, str]]:
    """Every res/**/meson.build rule that turns a PNG into a C header with
    nitrogfx -embed NAME: (the PNG, the header's path, NAME)."""
    jobs = []
    for build in sorted((upstream / "res").rglob("meson.build")):
        folder = build.parent.relative_to(upstream)
        for block in custom_targets(build.read_text(encoding="utf-8")):
            embed = re.search(r"'-embed',\s*'(\w+)'", block)
            header = re.search(r"'([\w.]+\.h)'", block)
            png = re.search(r"files\('([\w./-]+\.png)'\)", block)
            if embed and header and png:
                jobs.append((build.parent / png.group(1), folder / header.group(1), embed.group(1)))
    return jobs


def meson_build_root(constants: Path, resources: Path, work: Path) -> Path:
    """What the tools expect at Meson's build root: generated/ (Metang's
    headers) and res/ (generated indices such as res/items/item_icon.naix).

    The tools parse the generated headers' enum values with libexpr, which
    reads plain C numbers. For the game, tools/prepare_game_constants.py
    widens Metang's mask values from (1 << n) to (1ULL << n) (GCC rejects
    1 << 31 in an int); the tools get Metang's own output back, as in the
    original build."""
    root = work / "buildroot"
    root.mkdir(parents=True, exist_ok=True)
    res = root / "res"
    if res.is_symlink() or res.is_file():
        res.unlink()
    elif res.is_dir():
        # MSYS2 on Windows makes a copy where asked for a symbolic link
        # (unless set up otherwise): remove the old copy to make a fresh one.
        shutil.rmtree(res)
    res.symlink_to((resources / "res").resolve(), target_is_directory=True)
    generated = root / "generated"
    if generated.is_symlink():
        generated.unlink()
    generated.mkdir(exist_ok=True)
    for header in (constants / "generated").glob("*.h"):
        text = header.read_text(encoding="utf-8").replace("1ULL << ", "1 << ")
        dest = generated / header.name
        if not dest.is_file() or dest.read_text(encoding="utf-8") != text:
            dest.write_text(text, encoding="utf-8")
    return root


def naix_by_variable(upstream: Path) -> dict[str, Path]:
    """Meson variable -> the one .naix its custom_target outputs (relative
    to the build root), from every res/**/meson.build."""
    found: dict[str, Path] = {}
    for build in sorted((upstream / "res").rglob("meson.build")):
        text = build.read_text(encoding="utf-8")
        folder = build.parent.relative_to(upstream)
        for match in re.finditer(r"^(\w+)\s*=\s*custom_target\(", text, re.M):
            block = custom_targets(text[match.start():])[0]
            naix = re.findall(r"'([A-Za-z0-9_]+\.naix)'", block)
            if len(naix) == 1:
                found[match.group(1)] = folder / naix[0]
    return found


def sprite_template_jobs(upstream: Path, resources: Path) -> list[list[str]]:
    """resdatproc arguments for each rule in res/graphics/sprite_templates:
    -D NAME:<the .naix of the archive given as input n>, -p, -N, the JSON."""
    folder = upstream / "res" / "graphics" / "sprite_templates"
    variables = naix_by_variable(upstream)
    jobs = []
    for block in custom_targets((folder / "meson.build").read_text(encoding="utf-8")):
        if "resdatproc_exe" not in block:
            continue
        # The input list (archives as name_narc[1]) runs until "command:".
        inputs_match = re.search(r"input:(.*?)command:", block, re.S)
        inputs = re.findall(r"(\w+)\[1\]", inputs_match.group(1)) if inputs_match else []
        args: list[str] = []
        for name, index in re.findall(r"'-D',\s*'(\w+):@INPUT(\d+)@'", block):
            # Relative to the build root, as Meson's @INPUTn@ is: resdatproc
            # looks it up through its include paths (REPO_BUILD).
            relative = variables[inputs[int(index)]]
            if not (resources / relative).is_file():
                raise FileNotFoundError(f"{resources / relative} (needed for {name})")
            args += ["-D", f"{name}:{relative.as_posix()}"]
        args += ["-p", re.search(r"'-p',\s*'(\w+)'", block).group(1)]
        if "'-N'" in block:
            args.append("-N")
        args.append(str(folder / re.search(r"files\('(\w+\.json)'\)", block).group(1)))
        jobs.append(args)
    return jobs


def generate(upstream: Path, constants: Path, resources: Path, work: Path) -> list[Path]:
    work.mkdir(parents=True, exist_ok=True)
    buildroot = meson_build_root(constants, resources, work)
    tools = build_tools(upstream, buildroot, work,
                        ["speciesproc", "itemproc", "frontierproc", "trainerproc", "resdatproc"])
    res = upstream / "res"
    out = resources / "res"
    gen = buildroot / "generated"
    frontier_trainers = [line.strip()[len("FRONTIER_TRAINER_"):].lower()
                         for line in (upstream / "generated" / "frontier_trainers.txt")
                         .read_text(encoding="utf-8").splitlines() if line.strip()]
    frontier_pokemon = [line.strip() for line in (res / "trainers" / "frontier" / "frontier_pokemon.order")
                        .read_text(encoding="utf-8").splitlines() if line.strip()]
    jobs = [
        ("speciesproc", out / "pokemon",
         {"SPECIES": ";".join(folder_names(upstream, "generated/species.txt", "SPECIES_"))},
         ["-t", res / "pokemon" / "move_tutors.json", "-s", res / "pokemon" / "sinnoh_pokedex.json",
          "-o", out / "pokemon", res / "pokemon"]),
        ("itemproc", out / "items", {},
         ["-o", out / "items", gen / "items.h", res / "items" / "data"]),
        ("frontierproc", out / "trainers" / "frontier",
         {"TRAINERS": ";".join(frontier_trainers), "POKEMON": ";".join(frontier_pokemon)},
         ["-o", out / "trainers" / "frontier", res / "trainers" / "frontier"]),
        ("trainerproc", out / "trainers", {},
         ["-o", out / "trainers", gen / "trainers.h", res / "trainers" / "data"]),
    ]
    templates = out / "graphics" / "sprite_templates"
    for args in sprite_template_jobs(upstream, resources):
        jobs.append(("resdatproc", templates, {}, [*args[:-1], "-o", templates, args[-1]]))
    headers: list[Path] = []
    for tool, folder, env, args in jobs:
        folder.mkdir(parents=True, exist_ok=True)
        depfile = work / f"{tool}.d"
        run([str(tools[tool]), "-M", str(depfile), *map(str, args)],
            env={**os.environ, **env})
        for path in folder.iterdir():
            if path.suffix in (".narc", ".resdat", ".cldat"):
                path.unlink()        # game data: the game reads it from the data pack
            elif path.suffix == ".h" and path.stat().st_mtime >= depfile.stat().st_mtime - 5:
                headers.append(path)
    # Graphics the game embeds in its code as C arrays (nitrogfx -embed in
    # the original; tools/png_embed.py here).
    for png, relative, name in embed_jobs(upstream):
        header = resources / relative
        header.parent.mkdir(parents=True, exist_ok=True)
        png_embed.embed(png, header, name)
        headers.append(header)
    # trainerproc writes the trainer-name text bank; make its index header
    # as msgenc would (only the message IDs).
    names = out / "trainers" / "npc_trainer_names.json"
    if names.is_file():
        bank = out / "text" / "bank" / "npc_trainer_names.h"
        bank.parent.mkdir(parents=True, exist_ok=True)
        bank.write_text(render_text_bank(names), encoding="utf-8")
        headers.append(bank)
    return sorted(headers)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--upstream", type=Path, required=True)
    parser.add_argument("--constants", type=Path, required=True)
    parser.add_argument("--resources", type=Path, required=True)
    parser.add_argument("--work", type=Path, required=True)
    args = parser.parse_args()
    headers = generate(args.upstream.resolve(), args.constants.resolve(),
                       args.resources.resolve(), args.work.resolve())
    print(f"Generated {len(headers)} headers with the decompilation's data tools")


if __name__ == "__main__":
    main()
