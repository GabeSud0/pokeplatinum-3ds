"""Offline tests for original NitroArc's explicit archive-index semantics."""
from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from prepare_game_graphics_naix import generate, indices, render


class GameGraphicsIndexTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.source = self.root / "upstream"
        self.order = self.source / "res/graphics/poketch/poketch.order"
        self.order.parent.mkdir(parents=True)
        self.output = self.root / "build/game-resource-headers"
        # The extra source-backed .order files are mandatory at the pinned
        # upstream revision. No binary scripts or images are read.
        for name, member in (
            ("res/trainers/classes/field_encounteffect.order", ".shared/enc_trainer.NCLR"),
            ("res/battle/scripts/subscripts/sub_seq.order", "subscript_start_encounter"),
        ):
            source = self.source / name
            source.parent.mkdir(parents=True, exist_ok=True)
            source.write_text(member + "\n", encoding="utf-8")

    def write(self, data):
        self.order.write_text(data, encoding="utf-8")

    def test_exact_member_order_and_duplicate_suffix(self):
        self.write(
            "generic_bg_tiles.NCLR\n"
            "digits.NCGR.lz\n"
            "generic_bg_tiles.NCLR\n"
            "folder/tiles.NSCR.lz\n"
            "generic_bg_tiles.NCLR\n"
        )
        self.assertEqual(indices(self.order), [
            ("generic_bg_tiles_NCLR", 0),
            ("digits_NCGR_lz", 1),
            ("generic_bg_tiles_NCLR_1", 2),
            ("folder_tiles_NSCR_lz", 3),
            ("generic_bg_tiles_NCLR_2", 4),
        ])

    def test_emits_only_explicit_indices_not_images_or_unsupported_count(self):
        self.write("digits.NCGR.lz\nunused_apps.NCLR\n")
        h = render(self.order, self.order.relative_to(self.source))
        self.assertIn("#define digits_NCGR_lz 0\n", h)
        self.assertIn("#define unused_apps_NCLR 1\n", h)
        self.assertIn("#ifndef PORT_NITROARC_", h)
        self.assertNotIn("#define poketch_COUNT", h)
        self.assertNotIn("sprite data", h)

    def test_regenerates_incrementally_and_preserves_other_archives(self):
        self.write("digits.NCGR.lz\n")
        second = self.source / "res/graphics/frontier/backgrounds/frontier_backgrounds.order"
        second.parent.mkdir(parents=True)
        second.write_text("battle_factory.NCGR.lz\n", encoding="utf-8")
        self.assertEqual(generate(self.source, self.output), (4, 4, 4))
        self.assertEqual(generate(self.source, self.output), (4, 4, 0))
        self.assertTrue((self.output / "res/graphics/poketch/poketch.naix").is_file())
        self.assertTrue((self.output / "res/graphics/frontier/backgrounds/frontier_backgrounds.naix").is_file())

    def test_meson_archive_output_mapping_and_metadata_only(self):
        self.write("digits.NCGR.lz\n")
        pokedex = self.source / "res/graphics/pokedex/pokedex.order"
        pokedex.parent.mkdir(parents=True)
        pokedex.write_text("background_scroll_default.NCLR\nsearch.NCLR\n", encoding="utf-8")
        self.assertEqual(generate(self.source, self.output), (4, 5, 4))
        self.assertEqual(generate(self.source, self.output), (4, 5, 0))
        zukan = self.output / "res/graphics/pokedex/zukan.naix"
        self.assertIn("#define background_scroll_default_NCLR 0", zukan.read_text())
        self.assertIn("#define search_NCLR 1", zukan.read_text())
        self.assertFalse((self.output / "res/graphics/pokedex/pokedex.naix").exists())
        encounter = self.output / "res/trainers/classes/field_encounteffect.naix"
        self.assertIn("#define _shared_enc_trainer_NCLR 0", encounter.read_text())
        scripts = self.output / "res/battle/scripts/sub_seq.naix"
        self.assertIn("#define subscript_start_encounter 0", scripts.read_text())
        for generated in (zukan, encounter, scripts):
            self.assertNotIn("#define ARCHIVE_COUNT", generated.read_text())
            self.assertNotIn("NARC binary", generated.read_text())

    def test_missing_original_non_graphics_manifest_is_error(self):
        self.write("digits.NCGR.lz\n")
        (self.source / "res/battle/scripts/subscripts/sub_seq.order").unlink()
        with self.assertRaises(FileNotFoundError):
            generate(self.source, self.output)

    def test_rejects_traversal_and_bad_characters(self):
        for name in ("../bad.NCGR", "folder/../escape.NCGR", "/absolute.NCGR",
                     "folder//double.NSCR", "bad name.NCGR", "evil;rm.NCGR",
                     "12345.NCLR", "./file.NCLR"):
            with self.subTest(name=name):
                self.write(name + "\n")
                with self.assertRaises(ValueError):
                    indices(self.order)

    def test_rejects_macro_collision_after_sanitization(self):
        self.write("a/b.NCGR\na_b.NCGR\n")
        with self.assertRaisesRegex(ValueError, "collision"):
            indices(self.order)

    def test_missing_or_empty_order_is_not_silently_fabricated(self):
        self.write("\n")
        with self.assertRaisesRegex(ValueError, "no explicitly ordered"):
            indices(self.order)
        self.order.unlink()
        with self.assertRaises(ValueError):
            generate(self.source, self.output)


if __name__ == "__main__":
    unittest.main()
