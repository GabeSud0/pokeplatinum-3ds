"""Unit tests for the clean-room game header generation from manifest names."""
import tempfile
import unittest
from pathlib import Path

from generate_game_text_banks import generate


class GameTextBankTests(unittest.TestCase):
    def test_order_matches_original_manifest(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "text_banks.txt"
            source.write_text("TEXT_BANK_FIRST\nTEXT_BANK_SECOND\nTEXT_BANK_THIRD\n", encoding="utf-8")
            result = generate(source)
            self.assertIn("enum TextBank {", result)
            self.assertIn("TEXT_BANK_FIRST = 0,", result)
            self.assertIn("TEXT_BANK_SECOND = 1,", result)
            self.assertIn("TEXT_BANK_THIRD = 2,", result)

    def test_duplicate_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "text_banks.txt"
            source.write_text("TEXT_BANK_TEST\nTEXT_BANK_TEST\n", encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "duplicate"):
                generate(source)

    def test_invalid_identifier_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "text_banks.txt"
            source.write_text("TEXT_BANK_VALID\ninvalid name ;\n", encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "not a TEXT_BANK identifier"):
                generate(source)


if __name__ == "__main__":
    unittest.main()
