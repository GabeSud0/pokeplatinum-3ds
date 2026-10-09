"""Synthetic NitroFS tests: never use actual game data."""
import struct
import tempfile
import unittest
from pathlib import Path

from builder.extract_rom import extract, parse_nitrofs


def synthetic_nds():
    rom = bytearray(0x400)
    struct.pack_into("<IIII", rom, 0x40, 0x200, 0x20, 0x240, 0x10)
    # Root contains one file and one directory; child contains another file.
    struct.pack_into("<IHH", rom, 0x200, 0x10, 0, 2)
    struct.pack_into("<IHH", rom, 0x208, 0x19, 1, 0xf000)
    rom[0x210:0x219] = b"\x05a.bin\x81d\x01"
    rom[0x219] = 0
    rom[0x21a:0x221] = b"\x05b.bin\x00"
    struct.pack_into("<IIII", rom, 0x240, 0x300, 0x303, 0x310, 0x314)
    rom[0x300:0x303] = b"abc"
    rom[0x310:0x314] = b"defg"
    return bytes(rom)


class NitroFSTests(unittest.TestCase):
    def test_extract(self):
        with tempfile.TemporaryDirectory() as tmp:
            rom = Path(tmp) / "test.nds"
            rom.write_bytes(synthetic_nds())
            output = Path(tmp) / "data_pack"
            entries = extract(rom, output, verify=False)
            self.assertEqual([(e["id"], e["path"], e["size"]) for e in entries],
                             [(0, "a.bin", 3), (1, "d/b.bin", 4)])
            self.assertEqual((output / "a.bin").read_bytes(), b"abc")
            self.assertEqual((output / "d/b.bin").read_bytes(), b"defg")
            self.assertTrue((output / "manifest.json").exists())

    def test_verification_required(self):
        with tempfile.TemporaryDirectory() as tmp:
            rom = Path(tmp) / "test.nds"
            rom.write_bytes(synthetic_nds())
            with self.assertRaisesRegex(ValueError, "Unsupported ROM"):
                extract(rom, Path(tmp) / "out")

    def test_invalid_fat(self):
        rom = bytearray(synthetic_nds())
        struct.pack_into("<II", rom, 0x240, 0x300, 0x500)
        with self.assertRaisesRegex(ValueError, "FAT"):
            parse_nitrofs(bytes(rom))


if __name__ == "__main__":
    unittest.main()
