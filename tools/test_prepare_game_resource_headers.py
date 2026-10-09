"""Tests for manifest-only regional Pokédex header generation."""
from __future__ import annotations

import json
import tempfile
import unittest
from pathlib import Path

from prepare_game_resource_headers import generate, render_regional_pokedex_header


class RegionalPokedexHeaderTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.source = self.root / "sinnoh_pokedex.json"
        self.out = self.root / "out/res/pokemon/regional_pokedex_size.h"

    def store(self, entries):
        self.source.write_text(json.dumps(entries), encoding="utf-8")

    def test_matches_upstream_count_minus_one(self):
        self.store(["SPECIES_ARCEUS", "SPECIES_TURTWIG", "SPECIES_GROTLE"])
        header = render_regional_pokedex_header(self.source)
        self.assertIn("#define REGIONAL_DEX_COUNT 2\n", header)
        self.assertIn("#ifndef POKEPLATINUM_GENERATED_REGIONAL_POKEDEX_SIZE_H", header)
        self.assertNotIn("SPECIES_TURTWIG", header)

    def test_generates_nested_header_and_is_idempotent(self):
        self.store(["SPECIES_ARCEUS", "SPECIES_TURTWIG"])
        self.assertTrue(generate(self.source, self.out))
        first = self.out.read_bytes()
        self.assertFalse(generate(self.source, self.out))
        self.assertEqual(self.out.read_bytes(), first)
        self.assertTrue(self.out.is_file())

    def test_rejects_invalid_and_duplicate_species(self):
        for entries in (
            [],
            ["SPECIES_ARCEUS"],
            ["SPECIES_ARCEUS", "SPECIES_ARCEUS"],
            ["SPECIES_ARCEUS", "../secret"],
            ["SPECIES_ARCEUS", 2],
            {"not": "a list"},
        ):
            with self.subTest(entries=entries):
                self.store(entries)
                with self.assertRaises(ValueError):
                    render_regional_pokedex_header(self.source)

    def test_rejects_malformed_json(self):
        self.source.write_text('["SPECIES_ARCEUS",', encoding="utf-8")
        with self.assertRaises(json.JSONDecodeError):
            render_regional_pokedex_header(self.source)


if __name__ == "__main__":
    unittest.main()
