#!/usr/bin/env python3
"""Give each game overlay its own data sections, and write the overlay table.

Usage:
    python tools/game_overlays.py --lsf build/upstream/platinum.us/main.lsf \\
        --upstream build/upstream --objdir 3ds_port/game/build/3ds \\
        --objcopy arm-none-eabi-objcopy --output .../overlay_table.c

On the DS, an overlay is loaded from the cartridge each time it's needed,
which resets its data: its initialised variables get their initial values
back and its zero-initialised ones are cleared. The port links every
overlay into the program, so it has to do that reset itself
(3ds_port/nitro/src/port_overlay.c). For that it needs each overlay's data
in one place.

The linker spec (main.lsf) says which objects make up each overlay. This
renames those objects' data sections (.data, .data.*, .bss, .bss.*) to
ovl_data_<overlay> and ovl_bss_<overlay>, so the linker gathers them and
defines __start_/__stop_ symbols around them, and writes a C table of those
bounds in overlay-ID order (the order of the Overlay entries in main.lsf),
for the program to pass to NitroPort_SetOverlays (see render_table for why
the program links it last). Renaming is idempotent.

Overlay regions: an overlay placed "After" the ITCM or DTCM autoload was
loaded into that memory on the DS; the game's overlay manager keeps a list
of loaded overlays per region, so the table records it.
"""

from __future__ import annotations

import argparse
import re
import struct
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from game_sources import listed_sources  # noqa: E402

REGIONS = {"main": "MAIN", "ITCM": "ITCM", "DTCM": "DTCM"}

# The game's own libraries placed in overlays, and how to find their
# sources: libgds is all of lib/gds; libcrypto_ov97 is the list in
# lib/crypto/meson.build. The other libraries in overlays are Nintendo's
# Wi-Fi SDKs, which the port replaces with stand-ins that hold no data.
LIBRARY_SOURCES = {
    "libgds.a": ("lib/gds/src", None),
    "libcrypto_ov97.a": ("lib/crypto/src", "libcrypto_ov97_srcs"),
}


# Objects main.lsf names that aren't compiled from src/*.c: the trainer AI
# script, which both builds assemble from src/battle/trainer_ai/script.s.
SPECIAL_OBJECTS = {
    "trainer_ai_script.o": "src/battle/trainer_ai/script.o",
}


# --------------------------------------------------------------- main.lsf

def parse_lsf(text: str) -> list[dict]:
    """The Overlay entries, in order: name, what it comes after (or its
    address), its objects and libraries."""
    overlays = []
    current = None
    for raw in text.splitlines():
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        m = re.match(r"^(Static|Autoload|Overlay)\s+([A-Za-z_][A-Za-z0-9_]*)$", line)
        if m:
            current = None
            if m.group(1) == "Overlay":
                current = {"name": m.group(2), "after": None, "address": None,
                           "objects": [], "libraries": []}
                overlays.append(current)
            continue
        if current is None or line in ("{", "}"):
            continue
        words = line.split()
        if words[0] == "After":
            current["after"] = words[1]
        elif words[0] == "Address":
            current["address"] = int(words[1], 0)
        elif words[0] == "Object":
            current["objects"].append(words[1])
        elif words[0] == "Library":
            current["libraries"].append(words[1])
    if not overlays:
        raise ValueError("no Overlay entries in the linker spec")
    names = [o["name"] for o in overlays]
    if len(set(names)) != len(names):
        raise ValueError("an overlay name appears twice")
    return overlays


def region(overlays: list[dict], name: str) -> str:
    """MAIN, ITCM or DTCM: where the overlay was loaded on the DS."""
    by_name = {o["name"]: o for o in overlays}
    seen = set()
    while name not in REGIONS:
        if name in seen or name not in by_name:
            raise ValueError(f"can't place overlay {name!r}")
        seen.add(name)
        entry = by_name[name]
        if entry["address"] is not None:
            address = entry["address"]
            if 0x01FF8000 <= address < 0x02000000:
                return "ITCM"
            if 0x027E0000 <= address < 0x027E4000:
                return "DTCM"
            return "MAIN"
        if entry["after"] is None:
            raise ValueError(f"overlay {name!r} has no After or Address")
        name = entry["after"]
    return REGIONS[name]


# ------------------------------------------------------- objects of each one

def meson_object_name(source: str) -> str:
    """What Meson calls the object of src/<source> in main.lsf."""
    return "main.nef.p/src_" + source.replace("/", "_") + ".o"


def library_sources(upstream: Path, library: str) -> list[str]:
    """Upstream-relative C sources of one of the game's libraries."""
    folder, variable = LIBRARY_SOURCES[library]
    if variable is None:
        return sorted(p.relative_to(upstream).as_posix()
                      for p in (upstream / folder).glob("*.c"))
    text = (upstream / Path(folder).parent / "meson.build").read_text(encoding="utf-8")
    m = re.search(variable + r"\s*=\s*files\(([^)]*)\)", text)
    if not m:
        raise ValueError(f"{variable} not found for {library}")
    names = re.findall(r"'([^']+\.c)'", m.group(1))
    return sorted((Path(folder).parent / n).as_posix() for n in names)


def overlay_objects(upstream: Path, overlays: list[dict], objdir: Path) -> dict[str, list[Path]]:
    """Each overlay's object files in the port's build."""
    sources = listed_sources(upstream)
    by_meson: dict[str, str] = {}
    for source in sources:
        name = meson_object_name(source)
        if name in by_meson:
            raise ValueError(f"{by_meson[name]} and {source} have the same Meson object name")
        by_meson[name] = source
    result: dict[str, list[Path]] = {}
    for o in overlays:
        files = []
        for obj in o["objects"]:
            if obj in SPECIAL_OBJECTS:
                files.append(objdir / SPECIAL_OBJECTS[obj])
                continue
            if obj not in by_meson:
                raise ValueError(f"overlay {o['name']}: {obj} is not a listed source")
            files.append(objdir / "src" / (by_meson[obj][:-2] + ".o"))
        for library in o["libraries"]:
            if library in LIBRARY_SOURCES:
                for source in library_sources(upstream, library):
                    files.append(objdir / (source[:-2] + ".o"))
        result[o["name"]] = files
    return result


# ------------------------------------------------------------- ELF sections

def sections(path: Path) -> list[tuple[str, int]]:
    """An ELF object's sections, as (name, size) (32- or 64-bit,
    little-endian)."""
    data = path.read_bytes()
    if data[:4] != b"\x7fELF" or data[5] != 1:
        raise ValueError(f"{path}: not a little-endian ELF file")
    if data[4] == 1:
        shoff, = struct.unpack_from("<I", data, 0x20)
        shentsize, shnum, shstrndx = struct.unpack_from("<HHH", data, 0x2E)
        def header(i):
            name, _, _, _, offset, size = struct.unpack_from("<IIIIII", data, shoff + i * shentsize)
            return name, offset, size
    else:
        shoff, = struct.unpack_from("<Q", data, 0x28)
        shentsize, shnum, shstrndx = struct.unpack_from("<HHH", data, 0x3A)
        def header(i):
            name, _, _, _, offset, size = struct.unpack_from("<IIQQQQ", data, shoff + i * shentsize)
            return name, offset, size
    _, stroff, strsize = header(shstrndx)
    strtab = data[stroff:stroff + strsize]
    result = []
    for i in range(shnum):
        name, _, size = header(i)
        end = strtab.index(b"\0", name)
        result.append((strtab[name:end].decode("ascii"), size))
    return result


def section_names(path: Path) -> list[str]:
    return [name for name, _ in sections(path)]


def data_kind(section: str) -> str | None:
    """'data' or 'bss' for a writable data section the reset must cover."""
    if section == ".data" or (section.startswith(".data.") and not section.startswith(".data.rel.ro")):
        return "data"
    if section == ".bss" or section.startswith(".bss."):
        return "bss"
    return None


def rename_sections(path: Path, overlay: str, objcopy: str) -> set[str]:
    """Rename one object's data sections for `overlay`. Returns the kinds
    ('data', 'bss') of data the object holds for it (in sections renamed
    now or before; empty sections don't count)."""
    renames = []
    kinds = set()
    for name, size in sections(path):
        kind = data_kind(name)
        if kind is not None:
            renames += ["--rename-section", f"{name}=ovl_{kind}_{overlay}"]
        elif name == f"ovl_data_{overlay}":
            kind = "data"
        elif name == f"ovl_bss_{overlay}":
            kind = "bss"
        if kind is not None and size > 0:
            kinds.add(kind)
    if renames:
        subprocess.run([objcopy, *renames, str(path)], check=True)
    return kinds


# ------------------------------------------------------------------ table

def render_table(overlays: list[dict]) -> str:
    """The overlay table, in C. It also ends each overlay's two sections with
    4 bytes of its own: so every overlay has both (the linker defines the
    __start_/__stop_ symbols only for sections that exist), and each ends on
    a 4-byte boundary (3dsxtool requires the program's data to). The program
    links this object after the game's, which puts those bytes last."""
    lines = [
        "/* Generated by tools/game_overlays.py from main.lsf: every overlay, in",
        " * overlay-ID order, with the bounds of its data. Passed to",
        " * NitroPort_SetOverlays by the program; linked after the game's objects. */",
        "",
        "#include <nitro_port.h>",
        "",
    ]
    for o in overlays:
        name = o["name"]
        lines.append(f'__asm__(".section ovl_data_{name},\\"aw\\",%progbits\\n\\t.balign 4\\n'
                     f'\\t.space 4\\n\\t.previous\\n"'
                     f'        ".section ovl_bss_{name},\\"aw\\",%nobits\\n\\t.balign 4\\n'
                     f'\\t.space 4\\n\\t.previous\\n");')
        lines.append(f"extern u8 __start_ovl_data_{name}[], __stop_ovl_data_{name}[];")
        lines.append(f"extern u8 __start_ovl_bss_{name}[], __stop_ovl_bss_{name}[];")
    lines += ["", "const NitroPortOverlay gPlatinumOverlays[] = {"]
    for index, o in enumerate(overlays):
        name = o["name"]
        lines.append(f"    /* {index:3} */ {{ \"{name}\", NITRO_PORT_OVERLAY_{region(overlays, name)}, "
                     f"__start_ovl_data_{name}, __stop_ovl_data_{name}, "
                     f"__start_ovl_bss_{name}, __stop_ovl_bss_{name} }},")
    lines += ["};", "",
              "const u32 gPlatinumOverlayCount = sizeof(gPlatinumOverlays) / sizeof(gPlatinumOverlays[0]);",
              ""]
    return "\n".join(lines)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--lsf", type=Path, required=True)
    parser.add_argument("--upstream", type=Path, required=True)
    parser.add_argument("--objdir", type=Path, required=True)
    parser.add_argument("--objcopy", default="objcopy")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    overlays = parse_lsf(args.lsf.read_text(encoding="utf-8"))
    objects = overlay_objects(args.upstream, overlays, args.objdir)
    kinds: dict[str, set[str]] = {}
    count = missing = 0
    for name, files in objects.items():
        for path in files:
            if not path.is_file():
                missing += 1   # left out of this build (blocked_sources*.txt)
                continue
            kinds.setdefault(name, set()).update(rename_sections(path, name, args.objcopy))
            count += 1
    text = render_table(overlays)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    if not args.output.is_file() or args.output.read_text(encoding="utf-8") != text:
        args.output.write_text(text, encoding="utf-8")
    note = f" ({missing} not in this build)" if missing else ""
    with_data = sum(1 for k in kinds.values() if k)
    print(f"  {len(overlays)} overlays ({with_data} with data), {count} objects{note}: "
          "data sections grouped per overlay")


if __name__ == "__main__":
    main()
