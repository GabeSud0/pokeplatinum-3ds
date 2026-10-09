"""Offline tests for upstream Metang-based game header preparation."""
from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from prepare_game_constants import fix_gcc_mask_shifts, generate, generator_specs, pinned_metang


MOCK_MESON = """
# Comments and masks preserve the original generator settings.
metang_generators = {
    'abilities': { 'type': 'enum', 'tag': 'Ability' },
    'ai_flags': { 'type': 'mask', 'tag': 'AIFlag', 'extra': ['--no-auto'] },
    'move_flags': { 'type': 'mask', 'tag': 'MoveFlag' },
}

c_consts_generators = []
"""

# A tiny executable that accepts exactly the same command shape as Metang.
# Full semantic fidelity is verified separately in the integration workflow
# against the *actual* wrapped Metang tool and the real upstream manifests.
FAKE_METANG = r'''
import argparse
from pathlib import Path

p = argparse.ArgumentParser()
p.add_argument("kind")
p.add_argument("--tag-name", required=True)
p.add_argument("--guard", required=True)
p.add_argument("--output", required=True)
p.add_argument("--no-auto", action="store_true")
p.add_argument("source")
a = p.parse_args()
text = Path(a.source).read_text()
Path(a.output).write_text(
    "/* generated */\n"
    + "enum " + a.tag_name + " {\n"
    + ",\n".join("  " + x for x in text.splitlines())
    + "\n};\n"
    + "/* mode=" + a.kind + ",no_auto=" + str(a.no_auto) + " */\n"
)
'''


class GameConstantGenerationTests(unittest.TestCase):
    def make_upstream(self, root: Path) -> Path:
        upstream = root / "upstream"
        generated = upstream / "generated"
        generated.mkdir(parents=True)
        (generated / "meson.build").write_text(MOCK_MESON, encoding="utf-8")
        (generated / "abilities.txt").write_text(
            "ABILITY_NONE\nABILITY_STENCH\n", encoding="utf-8"
        )
        (generated / "ai_flags.txt").write_text(
            "AI_FLAG_NONE\nAI_FLAG_BASIC\nAI_FLAG_ALL\n", encoding="utf-8"
        )
        (generated / "move_flags.txt").write_text(
            "MOVE_FLAG_CONTACT\nMOVE_FLAG_PROTECT\n", encoding="utf-8"
        )
        return upstream

    def test_extracts_correct_tags_modes_and_options(self):
        with tempfile.TemporaryDirectory() as tmp:
            upstream = self.make_upstream(Path(tmp))
            result = generator_specs(upstream / "generated" / "meson.build")
            self.assertEqual(result["abilities"]["tag"], "Ability")
            self.assertEqual(result["move_flags"]["type"], "mask")
            self.assertEqual(result["ai_flags"]["extra"], ["--no-auto"])

    def test_generates_files_from_unmodified_manifests(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            upstream = self.make_upstream(root)
            stub = root / "metang.py"
            stub.write_text(FAKE_METANG, encoding="utf-8")
            result = generate(upstream, root / "output", stub)
            self.assertEqual(len(result), 3)
            self.assertEqual([x.name for x in result], ["abilities.h", "ai_flags.h", "move_flags.h"])
            self.assertIn("ABILITY_STENCH", result[0].read_text())
            self.assertIn("mode=mask,no_auto=True", result[1].read_text())
            self.assertIn("mode=mask,no_auto=False", result[2].read_text())

    def test_only_selects_named_manifests(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            upstream = self.make_upstream(root)
            stub = root / "metang.py"
            stub.write_text(FAKE_METANG, encoding="utf-8")
            result = generate(upstream, root / "output", stub, ["move_flags"])
            self.assertEqual([x.name for x in result], ["move_flags.h"])
            self.assertFalse((root / "output/generated/abilities.h").exists())
            with self.assertRaisesRegex(ValueError, "unknown"):
                generate(upstream, root / "output", stub, ["bad_name"])

    def test_rejects_untrusted_generator_expression(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "meson.build"
            path.write_text(
                "metang_generators = {'x': __import__('os').system('true')}\n",
                encoding="utf-8",
            )
            with self.assertRaisesRegex(ValueError, "non-literal"):
                generator_specs(path)

    def test_fixes_signed_shift_overflow_without_changing_enum_values(self):
        text = "AI_FLAG_BASIC = (1 << 0),\\nAI_FLAG_TOP = (1 << 31),\\nAI_FLAG_ALL = ((1 << 32) - 1),"
        fixed = fix_gcc_mask_shifts(text)
        self.assertIn("(1ULL << 0)", fixed)
        self.assertIn("(1ULL << 31)", fixed)
        self.assertIn("((1ULL << 32) - 1)", fixed)
        with self.assertRaisesRegex(ValueError, "exceeds 64-bit"):
            fix_gcc_mask_shifts("X = (1 << 64)")

    def test_refuses_unknown_metang_source(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            upstream = root / "upstream"
            (upstream / "subprojects").mkdir(parents=True)
            (upstream / "subprojects/metang.wrap").write_text(
                "[wrap-git]\n"
                "url = https://example.com/impostor.git\n"
                "revision = main\n",
                encoding="utf-8",
            )
            with self.assertRaisesRegex(ValueError, "unexpected Metang repository"):
                pinned_metang(upstream, root / "metang")


if __name__ == "__main__":
    unittest.main()
