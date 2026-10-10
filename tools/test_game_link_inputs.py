"""Offline tests for the overlay ID script and the Wi-Fi stand-in generator."""
from __future__ import annotations

import stat
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import make_game_wifi_stubs as stubs
import prepare_game_overlay_ids as overlays

LSF = """Static main
{
\tAddress 0x02000000
}

Overlay dummy1
{
\tAfter main
}

Overlay overlay5
{
\tAfter main
\tObject main.nef.p/src_x.c.o
}

Overlay battle
{
\tAfter overlay5
}
"""


class OverlayIdTests(unittest.TestCase):
    def test_numbers_follow_linker_spec_order(self):
        names = overlays.overlay_names(LSF)
        self.assertEqual(names, ["dummy1", "overlay5", "battle"])
        text = overlays.render(names)
        self.assertIn("SDK_OVERLAY_dummy1_ID = 0;", text)
        self.assertIn("SDK_OVERLAY_overlay5_ID = 1;", text)
        self.assertIn("SDK_OVERLAY_battle_ID = 2;", text)

    def test_header_has_the_same_numbers(self):
        header = overlays.render_header(overlays.overlay_names(LSF))
        self.assertIn("SDK_PORT_OVERLAY_ID_dummy1 = 0,", header)
        self.assertIn("SDK_PORT_OVERLAY_ID_overlay5 = 1,", header)
        self.assertIn("SDK_PORT_OVERLAY_ID_battle = 2,", header)
        self.assertTrue(header.startswith("/* Generated"))

    def test_rejects_empty_or_duplicate(self):
        with self.assertRaises(ValueError):
            overlays.overlay_names("Static main\n{\n}\n")
        with self.assertRaises(ValueError):
            overlays.overlay_names("Overlay a\n{\n}\nOverlay a\n{\n}\n")


class WifiStubTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)

    def fake_nm(self, listing: str) -> str:
        """An `nm` stand-in that prints the given listing."""
        script = self.root / "nm"
        script.write_text(f"#!{sys.executable}\nimport sys\nsys.stdout.write({listing!r})\n", encoding="utf-8")
        script.chmod(script.stat().st_mode | stat.S_IEXEC)
        return str(script)

    def test_only_undefined_wifi_symbols(self):
        nm = self.fake_nm(
            "a.o:\n"
            "         U DWC_Init\n"
            "         U DWC_CreateFriendKey\n"
            "         U PPW_LobbyProcess\n"
            "         U Heap_Alloc\n"
            "         U VCT_Main\n"
            "b.o:\n"
            "00000000 T VCT_Main\n"
            "00000000 T Heap_Alloc\n")
        defined, undefined = stubs.symbols(nm, ["a.o", "b.o"])
        names = sorted(s for s in undefined - defined if stubs.WIFI.match(s))
        self.assertEqual(names, ["DWC_CreateFriendKey", "DWC_Init", "PPW_LobbyProcess"])

    def test_64_bit_returns_from_headers(self):
        headers = self.root / "include"
        headers.mkdir()
        (headers / "dwc.h").write_text(
            "/* u64 DWC_Init(void); in a comment */\n"
            "BOOL DWC_Init(void);\n"
            "u64 DWC_CreateFriendKey(const DWCUserData *userdata);\n", encoding="utf-8")
        wide = stubs.wide_returns([headers], {"DWC_Init", "DWC_CreateFriendKey"})
        self.assertEqual(wide, {"DWC_CreateFriendKey"})
        text = stubs.render(["DWC_CreateFriendKey", "DWC_Init"], wide)
        self.assertIn('unsigned long long DWC_CreateFriendKey(void)', text)
        self.assertIn('int DWC_Init(void) { return Unavailable("DWC_Init"); }', text)


if __name__ == "__main__":
    unittest.main()
