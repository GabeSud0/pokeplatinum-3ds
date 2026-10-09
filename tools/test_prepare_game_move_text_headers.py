"""Offline tests of moveproc-compatible Platinum message-bank IDs."""
from __future__ import annotations

import json
import tempfile
import unittest
from pathlib import Path

from prepare_game_move_text_headers import bank_ids, generate, render, source_moves


class MoveTextBankHeaderTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.upstream = self.root / "upstream"
        (self.upstream / "generated").mkdir(parents=True)
        self.output = self.root / "build/game-resource-headers"

    def manifest(self, tokens=("MOVE_NONE", "MOVE_POUND", "MOVE_U_TURN", "MAX_MOVES")):
        (self.upstream / "generated/moves.txt").write_text(
            "\n".join(tokens) + "\n", encoding="utf-8"
        )
        for token in tokens:
            if not token.startswith("MOVE_"):
                continue
            stem = token[5:].lower()
            folder = self.upstream / "res/moves" / stem
            folder.mkdir(parents=True, exist_ok=True)
            (folder / "data.json").write_text(
                json.dumps({
                    "name": f"original {stem} name is private",
                    "description": [f"original {stem} text is private"],
                }), encoding="utf-8"
            )

    def test_all_four_banks_match_original_moveproc_push_order(self):
        self.manifest()
        stems = source_moves(self.upstream)
        self.assertEqual(stems, ["none", "pound", "u_turn"])
        banks = bank_ids(stems)
        self.assertEqual(banks["move_names"], [
            "MoveNames_Text_none",
            "MoveNames_Text_pound",
            "MoveNames_Text_u_turn",
        ])
        self.assertEqual(banks["move_names_uppercase"][1], "MoveNamesUppercase_Text_pound")
        self.assertEqual(banks["move_descriptions"][2], "MoveDescriptions_Text_u_turn")
        self.assertEqual(banks["moves_used_in_battle"][:6], [
            "MovesUsedInBattle_Text_none_Ally",
            "MovesUsedInBattle_Text_none_Wild",
            "MovesUsedInBattle_Text_none_Foe",
            "MovesUsedInBattle_Text_pound_Ally",
            "MovesUsedInBattle_Text_pound_Wild",
            "MovesUsedInBattle_Text_pound_Foe",
        ])
        self.assertEqual(banks["moves_used_in_battle"][-1], "MovesUsedInBattle_Text_u_turn_Foe")

    def test_header_has_correct_original_indices_and_count(self):
        self.manifest()
        banks = bank_ids(source_moves(self.upstream))
        h = render("move_names_uppercase", banks["move_names_uppercase"])
        self.assertIn("#define MoveNamesUppercase_Text_none 0\n", h)
        self.assertIn("#define MoveNamesUppercase_Text_pound 1\n", h)
        self.assertIn("#define TEXT_BANK_MOVE_NAMES_UPPERCASE_ENTRY_COUNT 3\n", h)
        self.assertIn("#ifndef MSGENC_RES_TEXT_BANK_MOVE_NAMES_UPPERCASE_H", h)
        battle = render("moves_used_in_battle", banks["moves_used_in_battle"])
        self.assertIn("#define MovesUsedInBattle_Text_u_turn_Foe 8\n", battle)
        self.assertIn("#define TEXT_BANK_MOVES_USED_IN_BATTLE_ENTRY_COUNT 9\n", battle)

    def test_only_identifiers_are_written_and_regeneration_is_idempotent(self):
        self.manifest()
        self.assertEqual(generate(self.upstream, self.output), (3, 4))
        self.assertEqual(generate(self.upstream, self.output), (3, 0))
        result = self.output / "res/text/bank/move_names_uppercase.h"
        self.assertIn("#define MoveNamesUppercase_Text_pound 1", result.read_text())
        self.assertNotIn("private", result.read_text())
        self.assertEqual(len(list(result.parent.glob("*.h"))), 4)

    def test_rejects_missing_and_malformed_move_metadata(self):
        self.manifest()
        folder = self.upstream / "res/moves/pound/data.json"
        folder.unlink()
        with self.assertRaises(FileNotFoundError):
            source_moves(self.upstream)
        folder.write_text('{"name":"Pound","description":17}', encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "invalid move description"):
            source_moves(self.upstream)

    def test_rejects_duplicate_invalid_and_wrong_sentinel_entries(self):
        for tokens in (
            ("MOVE_NONE", "MOVE_NONE", "MAX_MOVES"),
            ("MOVE_NONE", "MOVE_OTHER", "MOVE_WITH_SPACE", "MAX_MOVES"),
            ("MOVE_POUND", "MAX_MOVES"),
            ("MOVE_NONE", "MOVE_POUND"),
        ):
            with self.subTest(tokens=tokens):
                self.manifest(tokens)
                with self.assertRaises((ValueError, FileNotFoundError)):
                    source_moves(self.upstream)

    def test_rejects_unknown_bank_and_duplicate_macro(self):
        with self.assertRaisesRegex(ValueError, "not an upstream"):
            render("not_a_bank", ["Fake"])
        with self.assertRaisesRegex(ValueError, "duplicated"):
            render("move_names", ["MoveNames_Text_none", "MoveNames_Text_none"])


if __name__ == "__main__":
    unittest.main()
