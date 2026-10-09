"""Offline tests for original speciesproc name-message IDs and metadata only."""
from __future__ import annotations

import json
import tempfile
import unittest
from pathlib import Path

from prepare_game_species_text_headers import (
    bank_ids, generate, ordered_species, render,
)


class SpeciesTextHeaderTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.upstream = self.root / "upstream"
        (self.upstream / "generated").mkdir(parents=True)
        (self.upstream / "res/pokemon").mkdir(parents=True)
        self.output = self.root / "build/generated-ids"

    def write(self, symbols=None):
        if symbols is None:
            symbols = [
                "SPECIES_NONE", "SPECIES_BULBASAUR",
                "SPECIES_ARCEUS", "SPECIES_EGG", "SPECIES_BAD_EGG",
            ]
        (self.upstream / "generated/species.txt").write_text(
            "\n".join(symbols) + "\n", encoding="utf-8"
        )
        for symbol in symbols:
            stem = symbol.removeprefix("SPECIES_").lower()
            directory = self.upstream / "res/pokemon" / stem
            directory.mkdir(parents=True, exist_ok=True)
            # Text payload is deliberately not serialized by the generator.
            (directory / "data.json").write_text(json.dumps({
                "pokedex_data": {"en": {"name": "PRIVATE GAME NAME"}},
                "other_data": "PRIVATE GAME DATA",
            }), encoding="utf-8")

    def test_speciesproc_indices_include_none_egg_and_bad_egg(self):
        self.write()
        names = ordered_species(self.upstream)
        self.assertEqual(names, ["none", "bulbasaur", "arceus", "egg", "bad_egg"])
        banks = bank_ids(names)
        self.assertEqual(banks["species_name"][1], "species_name_bulbasaur")
        self.assertEqual(
            banks["species_name_with_articles"][4],
            "species_name_with_articles_bad_egg",
        )
        header = render("species_name", banks["species_name"])
        self.assertIn("#define species_name_none 0\n", header)
        self.assertIn("#define species_name_bulbasaur 1\n", header)
        self.assertIn("#define species_name_bad_egg 4\n", header)
        self.assertIn("#define TEXT_BANK_SPECIES_NAME_ENTRY_COUNT 5\n", header)

    def test_emits_two_headers_and_never_embeds_names(self):
        self.write()
        self.assertEqual(generate(self.upstream, self.output), (5, 2))
        header = self.output / "res/text/bank/species_name.h"
        second = self.output / "res/text/bank/species_name_with_articles.h"
        self.assertTrue(second.is_file())
        self.assertIn("#define species_name_with_articles_egg 3", second.read_text())
        self.assertNotIn("PRIVATE GAME NAME", header.read_text())
        self.assertNotIn("PRIVATE GAME DATA", header.read_text())
        self.assertEqual(generate(self.upstream, self.output), (5, 0))

    def test_rejects_invalid_enum_registry_and_duplicates(self):
        for values in (
            ["SPECIES_NONE", "SPECIES_NONE", "SPECIES_EGG", "SPECIES_BAD_EGG"],
            ["SPECIES_NONE", "SPECIES_BULBASAUR", "SPECIES_EGG"],
            ["SPECIES_NONE", "SPECIES_BULBASAUR", "SPECIES_BAD_EGG", "SPECIES_EGG"],
            ["SPECIES_NONE", "SPECIES_BAD-PATH", "SPECIES_EGG", "SPECIES_BAD_EGG"],
        ):
            with self.subTest(values=values):
                self.write(["SPECIES_NONE", "SPECIES_EGG", "SPECIES_BAD_EGG"])
                (self.upstream / "generated/species.txt").write_text(
                    "\n".join(values), encoding="utf-8"
                )
                with self.assertRaises(ValueError):
                    ordered_species(self.upstream)

    def test_refuses_missing_or_malformed_species_data(self):
        self.write()
        data = self.upstream / "res/pokemon/bulbasaur/data.json"
        data.unlink()
        with self.assertRaises(FileNotFoundError):
            ordered_species(self.upstream)
        data.write_text('{"pokedex_data": {}}', encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "Pokédex"):
            ordered_species(self.upstream)

    def test_refuses_invalid_bank_ids(self):
        with self.assertRaises(ValueError):
            render("fake_bank", ["species_name_bulbasaur"])
        with self.assertRaises(ValueError):
            render("species_name", ["species_name_same", "species_name_same"])
        with self.assertRaises(ValueError):
            render("species_name", ["wrong_prefix"])


if __name__ == "__main__":
    unittest.main()
