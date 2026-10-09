#!/usr/bin/env python3
"""Create a synthetic pack using the real Python writer for the C ABI test."""
from __future__ import annotations

import json
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[3]))
from builder.pack import pack


if __name__ == "__main__":
    output = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        (root / "a.bin").write_bytes(b"python-writer\x00\xff")
        (root / "b.bin").write_bytes(bytes(range(17)))
        (root / "empty.bin").write_bytes(b"")
        (root / "manifest.json").write_text(json.dumps({"files": [
            {"id": 70000, "path": "b.bin", "size": 17},
            {"id": 3, "path": "a.bin", "size": 15},
            {"id": 4096, "path": "empty.bin", "size": 0},
        ]}), encoding="utf-8")
        pack(root, output, build_id="c-python-v1",
             rom_sha1="0862ec35b24de5c7e2dcb88c9eea0873110d755c")
