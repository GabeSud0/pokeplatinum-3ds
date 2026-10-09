"""Offline behavioral tests of pinned NitroSFX sound index header semantics."""
from __future__ import annotations

import json
import tempfile
import unittest
from pathlib import Path

from prepare_game_sound_naix import CATEGORIES, generate, render, sound_symbols


class SoundIndexTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.source = self.root / "pl_sound_data.json"
        self.dest = self.root / "res/sound/pl_sound_data.naix"

    def save(self, overrides=None):
        data = {name: [] for name, _ in CATEGORIES}
        data.update(overrides or {})
        self.source.write_text(json.dumps(data), encoding="utf-8")

    def test_uses_original_type_local_indices_and_duplicate_suffixes(self):
        self.save({
            "seqInfo": [
                {"name": ""},
                {"name": "SEQ_FIRST", "fileName": "music/hello.sseq"},
                {"name": "SEQ_AGAIN", "fileName": "music/hello.sseq"},
            ],
            "bankInfo": [{"name": "BANK_TEST", "fileName": "bank/file.sbnk"}],
            "playerInfo": [{"name": "PLAYER_PV"}],
            "groupInfo": [{"name": "GROUP_GLOBAL"}],
        })
        output = render(self.source)
        self.assertIn("#define music_hello_sseq 1\n", output)
        self.assertIn("#define music_hello_sseq_1 2\n", output)
        self.assertIn("#define bank_file_sbnk 0\n", output)
        self.assertIn("#define PLAYER_PV 0\n", output)
        self.assertIn("#define GROUP_GLOBAL 0\n", output)
        self.assertNotIn("#define SEQ_FIRST", output)
        self.assertNotIn("#define SEQ_AGAIN", output)
        self.assertNotIn("#define pl_sound_data_COUNT", output)

    def test_skips_empty_reserved_slots_preserving_positions(self):
        self.save({"seqInfo": [{"name": ""}, {"name": ""}, {"name": "SEQ_TEST", "fileName": "SEQ_TEST.sseq"}]})
        self.assertEqual(sound_symbols(self.source), [("SEQ_TEST_sseq", 2)])

    def test_writes_only_indices_idempotently(self):
        self.save({"seqInfo": [{"name": "SECRET_TITLE", "fileName": "sound/test.sseq", "actualAudio": "do not include"}]})
        self.assertTrue(generate(self.source, self.dest))
        self.assertFalse(generate(self.source, self.dest))
        data = self.dest.read_text(encoding="utf-8")
        self.assertIn("#define sound_test_sseq 0", data)
        self.assertNotIn("do not include", data)
        self.assertNotIn("SECRET_TITLE", data)

    def test_rejects_unsafe_filename_and_missing_record(self):
        for value in ("../bad;include", "foo bar.sseq", "", 123):
            with self.subTest(value=value):
                self.save({"seqInfo": [{"name": "SEQ_TEST", "fileName": value}]})
                with self.assertRaises(ValueError):
                    render(self.source)

    def test_validates_all_categories_and_record_types(self):
        self.save()
        self.assertEqual(sound_symbols(self.source), [])
        self.save({"seqInfo": [{"invalid": 1}]})
        with self.assertRaises(ValueError):
            render(self.source)
        self.source.write_text('{"seqInfo": []}', encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "missing"):
            render(self.source)

    def test_rejects_conflicting_macro_names(self):
        self.save({"playerInfo": [{"name": "SAME"}], "groupInfo": [{"name": "SAME"}]})
        with self.assertRaisesRegex(ValueError, "cross-category"):
            render(self.source)


if __name__ == "__main__":
    unittest.main()
