"""Offline tests for pinned NitroDWC/NitroWiFi header fetch metadata."""
from __future__ import annotations

import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from prepare_game_network_headers import EXPECTED, pinned_source, prepare

HASHES = {
    "NitroDWC": "ba548986500a6628ec37c0603bdb94245ff12a63",
    "NitroWiFi": "e6048fc294480831e154b3d53205d6cbda1775cd",
}


class GameNetworkHeaderTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.upstream = self.root / "upstream"
        (self.upstream / "subprojects").mkdir(parents=True)

    def write_wrap(self, name, *, url=None, revision=None):
        path = self.upstream / "subprojects" / f"{name}.wrap"
        path.write_text(
            "[wrap-git]\n"
            f"url = {EXPECTED[name] if url is None else url}\n"
            f"revision = {HASHES[name] if revision is None else revision}\n"
            "depth = 1\n",
            encoding="utf-8",
        )
        return path

    def test_exact_shas_and_origins_from_pinned_wraps(self):
        for name in HASHES:
            url, rev = pinned_source(self.write_wrap(name), name)
            self.assertEqual(url, EXPECTED[name])
            self.assertEqual(rev, HASHES[name])

    def test_refuses_unrelated_repositories(self):
        path = self.write_wrap("NitroDWC", url="https://example.org/fake.git")
        with self.assertRaisesRegex(ValueError, "unexpected repository URL"):
            pinned_source(path, "NitroDWC")

    def test_refuses_moving_branch_or_abbreviated_revision(self):
        for ref in ("main", "v1.0", HASHES["NitroDWC"][:8]):
            with self.subTest(ref=ref):
                path = self.write_wrap("NitroDWC", revision=ref)
                with self.assertRaisesRegex(ValueError, "pinned full SHA-1"):
                    pinned_source(path, "NitroDWC")

    def test_prepares_both_original_header_directories(self):
        for name in HASHES:
            self.write_wrap(name)

        def fake_fetch(url, revision, destination):
            expected = "NitroDWC" if "NitroDWC" in url else "NitroWiFi"
            self.assertEqual(revision, HASHES[expected])
            p = destination / "include"
            p.mkdir(parents=True)
            (p / ("dwc.h" if expected == "NitroDWC" else "nitroWiFi.h")).write_text(
                "/* test header */\n", encoding="utf-8"
            )

        with patch("prepare_game_network_headers.prepare_checkout", side_effect=fake_fetch):
            results = prepare(self.upstream, self.root / "out")
        self.assertEqual(set(results), {"NitroDWC", "NitroWiFi"})
        self.assertTrue((results["NitroDWC"] / "dwc.h").is_file())

    def test_missing_header_is_fatal(self):
        for name in HASHES:
            self.write_wrap(name)

        def fake_fetch(url, revision, destination):
            (destination / "include").mkdir(parents=True)

        with patch("prepare_game_network_headers.prepare_checkout", side_effect=fake_fetch):
            with self.assertRaises(FileNotFoundError):
                prepare(self.upstream, self.root / "out")


if __name__ == "__main__":
    unittest.main()
