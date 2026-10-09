"""Synthetic pack-format tests. No ROMs or game assets are used."""
import contextlib
import hashlib
import io
import json
import struct
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from builder.extract_rom import extract
from builder.pack import ENTRY, HEADER, main, pack, read_pak
from builder.test_extract_rom import synthetic_nds


SHA1 = "0123456789abcdef0123456789abcdef01234567"


class PakTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name) / "data_pack"
        self.root.mkdir()
        (self.root / "sub").mkdir()
        (self.root / "first.bin").write_bytes(b"abc")
        (self.root / "sub" / "second.bin").write_bytes(b"\x00\x01\xff\x02\x03")
        self.out = Path(self.temp.name) / "platinum3ds.pak"
        self.files = [
            {"id": 99, "path": "sub/second.bin", "size": 5},
            {"id": 2, "path": "first.bin", "size": 3},
        ]
        self.write_manifest(self.files)

    def write_manifest(self, files, **extra):
        (self.root / "manifest.json").write_text(
            json.dumps({"files": files, **extra}), encoding="utf-8")

    def test_round_trip_sorted_ids_and_padding(self):
        pack(self.root, self.out, build_id="release-2026.10", rom_sha1=SHA1)
        decoded = read_pak(self.out)
        self.assertEqual(decoded["version"], 1)
        self.assertEqual(decoded["build_id"], "release-2026.10")
        self.assertEqual(decoded["rom_sha1"], SHA1)
        self.assertEqual(decoded["files"], {2: b"abc", 99: b"\x00\x01\xff\x02\x03"})
        raw = self.out.read_bytes()
        self.assertEqual(struct.unpack_from("<I", raw, HEADER.size)[0], 2)
        self.assertEqual(struct.unpack_from("<I", raw, HEADER.size + ENTRY.size)[0], 99)
        self.assertEqual(len(raw) % 4, 0)
        self.assertEqual(struct.unpack_from("<I", raw, HEADER.size + 4)[0] % 4, 0)

    def test_manifest_rom_sha1(self):
        self.write_manifest(self.files, rom_sha1=SHA1)
        pack(self.root, self.out, build_id="v1")
        self.assertEqual(read_pak(self.out)["rom_sha1"], SHA1)
        with self.assertRaisesRegex(ValueError, "disagrees"):
            pack(self.root, self.out, build_id="v1", rom_sha1="f" * 40)

    def test_extract_then_pack_uses_manifest_sha1(self):
        with tempfile.TemporaryDirectory() as tmp:
            rom_bytes = synthetic_nds()
            rom = Path(tmp) / "synthetic.nds"
            rom.write_bytes(rom_bytes)
            extracted = Path(tmp) / "extracted"
            extract(rom, extracted, verify=False)
            packed = Path(tmp) / "synthetic.pak"
            pack(extracted, packed, build_id="synthetic-e2e")
            decoded = read_pak(packed)
            self.assertEqual(decoded["build_id"], "synthetic-e2e")
            self.assertEqual(decoded["rom_sha1"], hashlib.sha1(rom_bytes).hexdigest())
            self.assertEqual(decoded["files"], {0: b"abc", 1: b"defg"})

    def test_cli_rejects_unsupported_manifest_sha1(self):
        self.write_manifest(self.files, rom_sha1=SHA1)
        with patch.object(sys, "argv", ["pack.py", str(self.root), "-o", str(self.out),
                                       "--build-id", "dev-001"]):
            with contextlib.redirect_stderr(io.StringIO()) as errors:
                with self.assertRaises(SystemExit) as result:
                    main()
        self.assertEqual(result.exception.code, 1)
        self.assertIn("Unsupported ROM", errors.getvalue())
        self.assertFalse(self.out.exists())

    def test_missing_rom_sha1_is_not_guessed(self):
        with self.assertRaisesRegex(ValueError, "--rom"):
            pack(self.root, self.out, build_id="v1")

    def test_mismatched_manifest_size(self):
        self.files[0]["size"] = 6
        self.write_manifest(self.files)
        with self.assertRaisesRegex(ValueError, "Size mismatch"):
            pack(self.root, self.out, build_id="v1", rom_sha1=SHA1)
        self.assertFalse(self.out.exists())

    def test_truncated_pak(self):
        pack(self.root, self.out, build_id="v1", rom_sha1=SHA1)
        self.out.write_bytes(self.out.read_bytes()[:-2])
        with self.assertRaises(ValueError):
            read_pak(self.out)

    def test_truncated_index(self):
        pack(self.root, self.out, build_id="v1", rom_sha1=SHA1)
        self.out.write_bytes(self.out.read_bytes()[:HEADER.size + 1])
        with self.assertRaisesRegex(ValueError, "Truncated pak index"):
            read_pak(self.out)

    def test_out_of_range_offset(self):
        pack(self.root, self.out, build_id="v1", rom_sha1=SHA1)
        raw = bytearray(self.out.read_bytes())
        struct.pack_into("<I", raw, HEADER.size + 4, 0xfffffff0)
        self.out.write_bytes(raw)
        with self.assertRaises(ValueError):
            read_pak(self.out)

    def test_out_of_range_size(self):
        pack(self.root, self.out, build_id="v1", rom_sha1=SHA1)
        raw = bytearray(self.out.read_bytes())
        struct.pack_into("<I", raw, HEADER.size + 8, 0xffffffff)
        self.out.write_bytes(raw)
        with self.assertRaises(ValueError):
            read_pak(self.out)

    def test_duplicate_id_and_path(self):
        self.write_manifest(self.files + [{"id": 2, "path": "sub/second.bin", "size": 5}])
        with self.assertRaisesRegex(ValueError, "Duplicate NitroFS"):
            pack(self.root, self.out, build_id="v1", rom_sha1=SHA1)
        self.write_manifest(self.files + [{"id": 3, "path": "first.bin", "size": 3}])
        with self.assertRaisesRegex(ValueError, "Duplicate or invalid manifest path"):
            pack(self.root, self.out, build_id="v1", rom_sha1=SHA1)

    def test_path_traversal_and_absolute_paths(self):
        for bad in ("../outside.bin", "/tmp/other.bin", "sub/../../outside.bin",
                    "sub\\second.bin", "sub/./second.bin", "C:/windows/file"):
            with self.subTest(path=bad):
                self.write_manifest([{"id": 1, "path": bad, "size": 3}])
                with self.assertRaises(ValueError):
                    pack(self.root, self.out, build_id="v1", rom_sha1=SHA1)

    def test_escaping_symlink(self):
        outside = Path(self.temp.name) / "outside.bin"
        outside.write_bytes(b"abc")
        (self.root / "link.bin").symlink_to(outside)
        self.write_manifest([{"id": 1, "path": "link.bin", "size": 3}])
        with self.assertRaisesRegex(ValueError, "escapes"):
            pack(self.root, self.out, build_id="v1", rom_sha1=SHA1)

    def test_invalid_index_order(self):
        pack(self.root, self.out, build_id="v1", rom_sha1=SHA1)
        raw = bytearray(self.out.read_bytes())
        struct.pack_into("<I", raw, HEADER.size + ENTRY.size, 2)
        self.out.write_bytes(raw)
        with self.assertRaisesRegex(ValueError, "sorted"):
            read_pak(self.out)

    def test_bad_magic_and_version(self):
        pack(self.root, self.out, build_id="v1", rom_sha1=SHA1)
        original = self.out.read_bytes()
        for at, value in ((0, b"BADMAGIC"), (8, struct.pack("<I", 2))):
            with self.subTest(at=at):
                raw = bytearray(original)
                raw[at:at + len(value)] = value
                self.out.write_bytes(raw)
                with self.assertRaises(ValueError):
                    read_pak(self.out)

    def test_output_cannot_overwrite_input(self):
        with self.assertRaisesRegex(ValueError, "overwrite"):
            pack(self.root, self.root / "first.bin", build_id="v1", rom_sha1=SHA1)


if __name__ == "__main__":
    unittest.main()
