"""Offline tests for deterministic, metadata-only text-bank index generation."""
from __future__ import annotations

import json
import tempfile
import unittest
from pathlib import Path

from prepare_game_text_bank_headers import generate, read_ids, render


class GameTextBankHeaderTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.source = self.root / "upstream/res/text"
        self.source.mkdir(parents=True)
        self.output = self.root / "build/game-resource-headers"

    def write(self, filename="box_messages.json", messages=None):
        if messages is None:
            messages = [
                {"id": "BoxText_MonSelected", "en_US": "Private dialogue"},
                {"id": "BoxText_MarkMon", "garbage": 30},
                {"id": "BoxText_ReleaseMon", "en_US": ["Line one", "Line two"]},
            ]
        path = self.source / filename
        path.write_text(json.dumps({"key": 50258, "messages": messages}), encoding="utf-8")
        return path

    def test_original_msgenc_index_assignments_and_count(self):
        source = self.write()
        result = render(source)
        self.assertIn("#define BoxText_MonSelected 0\n", result)
        self.assertIn("#define BoxText_MarkMon 1\n", result)
        self.assertIn("#define BoxText_ReleaseMon 2\n", result)
        self.assertIn("#define TEXT_BANK_BOX_MESSAGES_ENTRY_COUNT 3\n", result)
        self.assertIn("#ifndef MSGENC_RES_TEXT_BANK_BOX_MESSAGES_H", result)
        self.assertNotIn("Private dialogue", result)
        self.assertNotIn("50258", result)

    def test_incremental_generation_without_game_payload(self):
        self.write()
        self.write("ability_names_uppercase.json", [{"id": "pl_msg_00000611_00000", "en_US": "Ability"}])
        self.assertEqual(generate(self.source, self.output), (2, 2))
        path = self.output / "res/text/bank/box_messages.h"
        expected = path.read_bytes()
        self.assertEqual(generate(self.source, self.output), (2, 0))
        self.assertEqual(path.read_bytes(), expected)
        self.assertTrue((self.output / "res/text/bank/ability_names_uppercase.h").is_file())

    def test_rejects_invalid_duplicate_and_count_name(self):
        bad_cases = [
            [{"id": "same"}, {"id": "same"}],
            [{"id": "bad-name"}],
            [{"id": "../bad"}],
            [{"id": "TEXT_BANK_BOX_MESSAGES_ENTRY_COUNT"}],
            [{"en_US": "missing id"}],
            [42],
        ]
        for msgs in bad_cases:
            with self.subTest(msgs=msgs):
                p = self.write(messages=msgs)
                with self.assertRaises(ValueError):
                    render(p)

    def test_allows_empty_bank_as_msgenc_does(self):
        path = self.write(messages=[])
        self.assertIn("#define TEXT_BANK_BOX_MESSAGES_ENTRY_COUNT 0", render(path))

    def test_rejects_malformed_json_and_no_inputs(self):
        with self.assertRaises(ValueError):
            generate(self.source, self.output)
        file = self.source / "box_messages.json"
        file.write_text("{invalid", encoding="utf-8")
        with self.assertRaises(json.JSONDecodeError):
            read_ids(file)

    def test_rejects_non_bank_filenames_and_missing_directory(self):
        p = self.write(filename="bad-bank.json")
        with self.assertRaisesRegex(ValueError, "invalid text-bank name"):
            render(p)
        with self.assertRaises(FileNotFoundError):
            generate(self.root / "missing", self.output)


if __name__ == "__main__":
    unittest.main()
