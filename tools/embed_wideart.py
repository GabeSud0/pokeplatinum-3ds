#!/usr/bin/env python3
"""The wide art built into the program: the repository's wideart/ pictures
(docs/WIDE_ART.md) as a C file defining NitroPort_WideArtBuiltIn
(3ds_port/nitro/include/nitro_port_wideart.h).

    embed_wideart.py --output OUT.c [PICTURE.png ...]

Each PNG goes in as it is, its bytes in an array, under its file name. The
output is written only if it changes (so make rebuilds the object only
then). Without pictures: an empty table.
"""
import argparse
import os
import sys


def c_string(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--output", required=True)
    ap.add_argument("pictures", nargs="*")
    args = ap.parse_args()

    names = set()
    parts = [
        "/* Made by tools/embed_wideart.py from wideart/: don't edit. */",
        "#include <nitro_port_wideart.h>",
        "",
    ]
    table = []
    for i, path in enumerate(sorted(args.pictures, key=os.path.basename)):
        name = os.path.basename(path)
        if not name.lower().endswith(".png") or name in names:
            continue
        names.add(name)
        with open(path, "rb") as f:
            data = f.read()
        if data[:8] != b"\x89PNG\r\n\x1a\n":
            sys.exit(f"embed_wideart: {path} isn't a PNG")
        lines = []
        for at in range(0, len(data), 24):
            lines.append("    " + ", ".join(f"0x{b:02x}" for b in data[at:at + 24]) + ",")
        parts.append(f"static const unsigned char sFile{i}[{len(data)}] = {{")
        parts.extend(lines)
        parts.append("};")
        table.append(f"    {{ {c_string(name)}, sFile{i}, {len(data)}u }},")
    if table:
        parts.append("")
        parts.append("static const NitroWideArtFile sFiles[] = {")
        parts.extend(table)
        parts.append("};")
        parts.append("")
        parts.append("const NitroWideArtFile *NitroPort_WideArtBuiltIn(int *count)")
        parts.append("{")
        parts.append(f"    *count = {len(table)};")
        parts.append("    return sFiles;")
        parts.append("}")
    else:
        parts.append("const NitroWideArtFile *NitroPort_WideArtBuiltIn(int *count)")
        parts.append("{")
        parts.append("    *count = 0;")
        parts.append("    return (const NitroWideArtFile *)0;")
        parts.append("}")
    text = "\n".join(parts) + "\n"
    try:
        with open(args.output, "r", encoding="utf-8") as f:
            if f.read() == text:
                return
    except OSError:
        pass
    os.makedirs(os.path.dirname(os.path.abspath(args.output)), exist_ok=True)
    with open(args.output, "w", encoding="utf-8") as f:
        f.write(text)
    print(f"  {len(table)} wide art pictures built in")


if __name__ == "__main__":
    main()
