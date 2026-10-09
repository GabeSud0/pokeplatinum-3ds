"""Offline tests for the full-game link report and the blocklist updater."""
from __future__ import annotations

import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import game_link_report
import update_game_blocklist


class GameLinkReportTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        root = Path(self.tmp.name)
        self.game = root / "upstream"
        self.port = root / "port"
        (self.game / "src").mkdir(parents=True)
        (self.game / "lib/crypto/asm").mkdir(parents=True)
        self.port.mkdir()
        (self.game / "src/blocked.c").write_text(
            "#include \"x.h\"\n\nint Blocked_Function(int a)\n{\n    return a;\n}\n"
            "const int sBlockedTable[2] = { 1, 2 };\n", encoding="utf-8")
        (self.game / "lib/crypto/asm/rc4.s").write_text(
            "\tarm_func_start CRYPTO_RC4Encrypt\nCRYPTO_RC4Encrypt:\n", encoding="utf-8")
        (self.port / "blocked_sources.txt").write_text(
            "# comment\nblocked.c    # missing header x.h\n", encoding="utf-8")
        self.linker = root / "linker.txt"
        self.linker.write_text(
            "ld: a.o: in function `f':\n"
            "a.c:(.text+0x1): undefined reference to `Blocked_Function'\n"
            "a.c:(.text+0x2): undefined reference to `Blocked_Function'\n"
            "a.c:(.text+0x3): undefined reference to `sBlockedTable'\n"
            "a.c:(.text+0x4): undefined reference to `DWC_Init'\n"
            "a.c:(.text+0x5): undefined reference to `SDK_OVERLAY_bag_ID'\n"
            "a.c:(.text+0x6): undefined reference to `CRYPTO_RC4Encrypt'\n"
            "a.c:(.text+0x7): undefined reference to `Something_Else'\n", encoding="utf-8")
        self.report = root / "report.md"

    def run_report(self):
        argv = ["game_link_report.py", str(self.linker), "--target", "test", "--output", str(self.report)]
        with patch.object(game_link_report, "GAME", self.game), \
             patch.object(game_link_report, "PORT", self.port), \
             patch.object(sys, "argv", argv):
            self.assertEqual(game_link_report.main(), 0)
        return self.report.read_text(encoding="utf-8")

    def test_each_symbol_once_in_its_group(self):
        text = self.run_report()
        self.assertIn("6 missing symbols", text)
        self.assertIn("`blocked.c` (2): Blocked_Function, sBlockedTable", text)
        self.assertIn("Overlay IDs (made by the DS linker script): 1", text)
        self.assertIn("Written in assembly", text)
        self.assertIn("`DWC_Init`", text)
        self.assertIn("## Other: 1", text)
        self.assertEqual(text.count("Blocked_Function"), 1)

    def test_other_link_failures_are_reported(self):
        self.linker.write_text("ld: cannot find -lctru: No such file or directory\n"
                               "collect2: error: ld returned 1 exit status\n", encoding="utf-8")
        text = self.run_report()
        self.assertIn("failed for other reasons", text)
        self.assertIn("cannot find -lctru", text)
        self.assertNotIn("The whole game links.", text)

    def test_clean_link(self):
        self.linker.write_text("", encoding="utf-8")
        self.assertIn("The whole game links.", self.run_report())


class BlocklistUpdaterTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        (self.root / "blocked_sources.txt").write_text("# keep me\n# Format: x\nold.c   # old\n", encoding="utf-8")
        (self.root / "blocked_sources_64bit.txt").write_text("# 64-bit header\n", encoding="utf-8")

    def report(self, name, results, sample=None):
        path = self.root / name
        path.write_text(json.dumps({
            "source_count": 3, "sample_count": 3 if sample is None else sample,
            "results": [{"source": s, "category": c, "diagnostic": d} for s, c, d in results]}),
            encoding="utf-8")
        return path

    def run_update(self, r32, r64):
        with patch.object(update_game_blocklist, "GAME", self.root), \
             patch.object(sys, "argv", ["update_game_blocklist.py", str(r32), str(r64)]):
            return update_game_blocklist.main()

    def test_lists_with_reasons_and_kept_header(self):
        r32 = self.report("32.json", [
            ("a.c", "passed", ""),
            ("b.c", "missing_header", "b.c:1:10: fatal error: x.h: No such file or directory"),
            ("c.c", "passed", "")])
        r64 = self.report("64.json", [
            ("a.c", "passed", ""),
            ("b.c", "missing_header", "b.c:1:10: fatal error: x.h: No such file or directory"),
            ("c.c", "compiler_error", "c.c:5:3: error: initializer element is not constant")])
        self.assertEqual(self.run_update(r32, r64), 0)
        main = (self.root / "blocked_sources.txt").read_text(encoding="utf-8")
        self.assertTrue(main.startswith("# keep me\n# Format: x\n"))
        self.assertIn("b.c", main)
        self.assertIn("# missing header x.h", main)
        self.assertNotIn("old.c", main)
        extra = (self.root / "blocked_sources_64bit.txt").read_text(encoding="utf-8")
        self.assertIn("c.c", extra)
        self.assertIn("initializer element is not constant", extra)

    def test_refuses_a_sampled_survey(self):
        r = self.report("s.json", [("a.c", "passed", "")], sample=1)
        with self.assertRaises(SystemExit):
            self.run_update(r, r)


if __name__ == "__main__":
    unittest.main()
