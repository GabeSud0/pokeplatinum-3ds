"""Generate a private, entirely synthetic ROM filesystem fixture for libnitro.

The digest identifies a supported revision but NO ROM/game bytes are used.
Both pak and manifest are written under ignored build/ paths only.
"""
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT))
from builder.pack import pack  # noqa: E402

BUILD_ID = "fs-pak-test"
# Supported metadata hash; fixture payloads are arbitrary test strings.
ROM_SHA1 = "0862ec35b24de5c7e2dcb88c9eea0873110d755c"

FILES = [
    (0, "hello.bin", b"abcdefghij"),
    (8, "data/nested.bin", b"0123456789ABCDEF"),
    (23, "overlay/synthetic.bin", b"FAKE-OVERLAY"),
    (31, "empty.dat", b""),
]


def main(output: Path) -> None:
    output.mkdir(parents=True, exist_ok=True)
    manifest = []
    for file_id, name, payload in FILES:
        target = output / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(payload)
        manifest.append({"id": file_id, "path": name, "size": len(payload)})
    (output / "manifest.json").write_text(
        json.dumps({"rom_sha1": ROM_SHA1, "files": manifest}, indent=2) + "\n",
        encoding="utf-8",
    )
    pack(output, output / "synthetic.pak", build_id=BUILD_ID)


if __name__ == "__main__":
    main(Path(sys.argv[1]))
