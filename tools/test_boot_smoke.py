#!/usr/bin/env python3
"""Tests for tools/boot_smoke.py's verdicts (no game needed)."""

from __future__ import annotations

import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import boot_smoke  # noqa: E402

STARTED = "[     0] [platinum] starting the game\n"


class ClassifyTests(unittest.TestCase):
    def test_first_missing_file_is_a_pass(self):
        log = STARTED + "[     3] [nitro] the game asked for a file that isn't in the data pack: font/x.narc\n"
        passed, what = boot_smoke.classify(5, log)
        self.assertTrue(passed)
        self.assertIn("font/x.narc", what)

    def test_waiting_for_a_missing_service_is_a_pass(self):
        log = STARTED + "[     0] [nitro] not implemented: ARM7 service: power management\n"
        passed, what = boot_smoke.classify(None, log)
        self.assertTrue(passed)
        self.assertIn("power management", what)

    def test_frame_limit_is_a_pass(self):
        self.assertTrue(boot_smoke.classify(0, STARTED)[0])

    def test_failures(self):
        self.assertFalse(boot_smoke.classify(139, STARTED + "[pc] crashed (signal 11)\n")[0])
        self.assertFalse(boot_smoke.classify(3, STARTED)[0])
        self.assertFalse(boot_smoke.classify(None, STARTED)[0])     # silent hang
        self.assertFalse(boot_smoke.classify(0, "usage: ...\n")[0])  # never started


class PackTests(unittest.TestCase):
    def test_pack_is_accepted_format(self):
        with tempfile.TemporaryDirectory() as tmp:
            pak, manifest = boot_smoke.make_pack(Path(tmp))
            header = pak.read_bytes()[:64]
            self.assertTrue(manifest.is_file())
            self.assertEqual(header[:8], b"PLATPAK\0")
            # The program only opens packs with its build ID.
            self.assertIn(boot_smoke.BUILD_ID.encode(), header)


class BuildIdTests(unittest.TestCase):
    def test_builder_and_program_agree(self):
        import re
        header = (Path(__file__).resolve().parents[1] / "3ds_port/game/app/platinum.h").read_text(encoding="utf-8")
        program_id = re.search(r'#define PLATINUM_PAK_BUILD_ID "([^"]+)"', header).group(1)
        self.assertEqual(program_id, boot_smoke.BUILD_ID)


if __name__ == "__main__":
    unittest.main()
