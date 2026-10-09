"""Offline tests for script-ordered archive index headers."""
from __future__ import annotations

import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import prepare_game_ordergen_naix as tool

# A stand-in for a tools/ordergen script: one member per folder it is given.
SCRIPT = """
import os, sys
with open(sys.argv[1], "w") as out:
    out.write("shared.NCLR\\n")
    for name in os.environ["THINGS"].split(";"):
        out.write(name + "/sprite.NCGR\\n")
"""


class OrdergenNaixTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        root = Path(self.tmp.name)
        self.upstream = root / "upstream"
        (self.upstream / "generated").mkdir(parents=True)
        (self.upstream / "tools/ordergen").mkdir(parents=True)
        (self.upstream / "tools/ordergen/things.py").write_text(SCRIPT, encoding="utf-8")
        (self.upstream / "generated/things.txt").write_text("THING_ALPHA\n\nTHING_BETA_TWO\n", encoding="utf-8")
        self.output = root / "out"
        self.entries = [{
            "script": "tools/ordergen/things.py",
            "env": ("THINGS", "generated/things.txt", "THING_"),
            "args": ["@OUT:things.order"],
            "naix": {"things.order": "res/things/things.naix"},
        }]

    def test_folder_names_like_meson(self):
        self.assertEqual(tool.folder_names(self.upstream, "generated/things.txt", "THING_"),
                         ["alpha", "beta_two"])

    def test_runs_the_script_and_indexes_its_order(self):
        with patch.object(tool, "ORDERGEN", self.entries):
            made, updated = tool.generate(self.upstream, self.output)
        self.assertEqual((made, updated), (1, 1))
        text = (self.output / "res/things/things.naix").read_text(encoding="utf-8")
        self.assertIn("#define shared_NCLR 0", text)
        self.assertIn("#define alpha_sprite_NCGR 1", text)
        self.assertIn("#define beta_two_sprite_NCGR 2", text)
        with patch.object(tool, "ORDERGEN", self.entries):
            self.assertEqual(tool.generate(self.upstream, self.output), (1, 0))

    def test_rejects_unexpected_constant_names(self):
        (self.upstream / "generated/things.txt").write_text("OTHER_X\n", encoding="utf-8")
        with self.assertRaises(ValueError):
            tool.folder_names(self.upstream, "generated/things.txt", "THING_")


if __name__ == "__main__":
    unittest.main()
