"""The web builder's build (builder/web.py) makes what the desktop tools make.
Made-up ROM images only: no game data."""
import io
import json
import struct
import tempfile
import unittest
from pathlib import Path

from builder import web
from builder.extract_rom import extract
from builder.pack import pack, read_pak, write_pak, DEFAULT_BUILD_ID
from builder.test_extract_rom import synthetic_nds


def synthetic_nds_with_banner():
    """synthetic_nds and a DS banner (an icon of colour 1 on a transparent
    background), as a real ROM has at the header's offset 0x68."""
    rom = bytearray(synthetic_nds())
    banner = len(rom)
    struct.pack_into("<I", rom, 0x68, banner)
    rom += bytes(0x240)
    rom[banner + 0x20:banner + 0x220] = bytes([0x11]) * 0x200      # every pixel colour 1
    struct.pack_into("<HH", rom, banner + 0x220, 0, 0x7C1F)         # colour 1: magenta
    return bytes(rom)


class WebBuild(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.dir = Path(self.temp.name)

    def desktop(self, rom: bytes):
        """extract_rom.py then pack.py, as build_linux.sh runs them."""
        rom_path = self.dir / "test.nds"
        rom_path.write_bytes(rom)
        extract(rom_path, self.dir / "data_pack", verify=False)
        pack(self.dir / "data_pack", self.dir / "desktop.pak", build_id=DEFAULT_BUILD_ID)
        return (self.dir / "desktop.pak").read_bytes(), (self.dir / "data_pack" / "manifest.json").read_bytes()

    def test_same_bytes_as_the_desktop_tools(self):
        rom = synthetic_nds_with_banner()
        pak, manifest = self.desktop(rom)
        steps = []
        result = web.run_web_build(rom, str(self.dir / "web"), verify=False,
                                   progress=lambda *a: steps.append(a))
        self.assertEqual((self.dir / "web" / web.PAK).read_bytes(), pak)
        self.assertEqual((self.dir / "web" / web.MANIFEST).read_bytes(), manifest)
        self.assertEqual(read_pak(self.dir / "web" / web.PAK)["files"], {0: b"abc", 1: b"defg"})
        self.assertEqual(steps[0], ("rom", 0, 1))
        self.assertIn(("pack", 2, 2), steps)
        self.assertIsNone(result["banner_error"])        # (no 3D banner asked for)
        self.assertNotIn(web.CGFX, result["files"])
        self.assertTrue((self.dir / "web" / web.HOME_ART / "banner.png").read_bytes().startswith(b"\x89PNG"))
        self.assertFalse(list((self.dir / "web").rglob("*.part")))

    def test_bytes_like_roms(self):
        # The browser hands over a memoryview or a bytearray, not bytes.
        rom = synthetic_nds_with_banner()
        pak, _ = self.desktop(rom)
        for kind in (bytearray, memoryview):
            out = io.BytesIO()
            text = web.build_data(kind(rom), out, rom_sha1=web.check_rom(rom, verify=False)["sha1"])
            self.assertEqual(out.getvalue(), pak)
            self.assertEqual(json.loads(text)["files"][1]["path"], "d/b.bin")

    def test_unsupported_rom(self):
        with self.assertRaisesRegex(ValueError, "Unsupported ROM"):
            web.run_web_build(synthetic_nds_with_banner(), str(self.dir / "web"))
        self.assertFalse((self.dir / "web").exists())

    def test_no_3d_banner_is_not_a_failure(self):
        # A pycgfx folder without pycgfx: the data pack and the flat banner still made.
        result = web.run_web_build(synthetic_nds_with_banner(), str(self.dir / "web"), verify=False,
                                   pycgfx_dir=str(self.dir / "no-pycgfx"))
        self.assertIsNotNone(result["banner_error"])
        self.assertIn(web.PAK, result["files"])
        self.assertNotIn(web.CGFX, result["files"])

    def test_write_pak_rejects_duplicate_ids(self):
        with self.assertRaisesRegex(ValueError, "unique"):
            write_pak(io.BytesIO(), [(1, b"a"), (1, b"b")], build_id="x", rom_sha1="0" * 40)


if __name__ == "__main__":
    unittest.main()
