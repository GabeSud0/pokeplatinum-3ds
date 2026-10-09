"""Offline tests for deterministic, non-destructive source compile surveys."""
from __future__ import annotations

import json
import sys
import tempfile
import unittest
from pathlib import Path

from probe_game_compile import categorize, probe, sample_sources


class GameCompileSurveyTests(unittest.TestCase):
    def test_spread_and_stability_independent_of_checkout_directory(self):
        with tempfile.TemporaryDirectory() as tmp:
            left = Path(tmp) / "checkout-a" / "src"
            right = Path(tmp) / "checkout-b" / "src"
            for root in (left, right):
                (root / "subsystem").mkdir(parents=True)
                for path in ("foo.c", "bar.c", "baz.c", "subsystem/field.c"):
                    (root / path).write_text("int x;\n", encoding="utf-8")
            aa, at = sample_sources(left, 2)
            bb, bt = sample_sources(right, 2)
            self.assertEqual((at, bt), (4, 4))
            self.assertEqual([p.relative_to(left) for p in aa],
                             [p.relative_to(right) for p in bb])
            self.assertEqual(len(aa), 2)
            self.assertEqual(len(sample_sources(left, 0)[0]), 4)

    def test_categorizes_genuine_header_and_type_failures(self):
        cases = (
            ("src/a.c:5:10: fatal error: generated/moves.h: No such file or directory",
             "missing_generated_header"),
            ("fatal error: res/graphics/foo.h: No such file or directory",
             "missing_resource_header"),
            ("fatal error: ungenerated.h: No such file or directory",
             "missing_header"),
            ("error: unknown type name 'FakeType'", "type_or_symbol"),
            ("error: expected ';' before '}'", "compiler_error"),
            ("unrelated failure", "other_failure"),
        )
        for stderr, expected in cases:
            with self.subTest(stderr=stderr):
                self.assertEqual(categorize(stderr)[0], expected)
        self.assertEqual(categorize("", timeout=True)[0], "timeout")

    def test_runs_real_subprocess_for_each_original_file_without_mutation(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            source = root / "src"
            source.mkdir()
            good = source / "compiles.c"
            bad = source / "needs_header.c"
            good.write_text("good source", encoding="utf-8")
            bad.write_text("missing resource", encoding="utf-8")
            fake = root / "fake_compiler.py"
            fake.write_text(
                "import sys\n"
                "if 'needs_header.c' in sys.argv[-1]:\n"
                "  sys.stderr.write('fatal error: generated/moves.h: No such file or directory\\n')\n"
                "  sys.exit(1)\n",
                encoding="utf-8",
            )
            result = probe(
                source, [sys.executable, str(fake)], ["-std=gnu99"],
                limit=0, timeout=5, target="test",
            )
            self.assertEqual(result["sample_count"], 2)
            self.assertEqual(result["summary"].get("passed"), 1)
            self.assertEqual(result["summary"].get("missing_generated_header"), 1)
            self.assertEqual(good.read_text(encoding="utf-8"), "good source")
            self.assertEqual(bad.read_text(encoding="utf-8"), "missing resource")
            self.assertEqual(result["source_count"], 2)
            json.dumps(result)

    def test_rejects_empty_sources_and_invalid_limits(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            with self.assertRaisesRegex(ValueError, "no original C files"):
                sample_sources(root, 5)
            (root / "a.c").write_text("int x;\n", encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "limit"):
                sample_sources(root, -1)


if __name__ == "__main__":
    unittest.main()
