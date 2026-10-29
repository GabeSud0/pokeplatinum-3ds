"""The follower sprites' bundle (builder/followers.py), on made-up files
only: no HeartGold/SoulSilver data and no download."""
import hashlib
import os
import struct
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import follower_sources  # noqa: E402
import followers  # noqa: E402
import pack  # noqa: E402


def make_nsbtx(size=32, frames=8, fmt=3, mixed=False) -> bytes:
    """An NSBTX with only what texture_size reads: the headers and the
    texture info dictionary's parameters."""
    count = frames
    code = {8: 0, 16: 1, 32: 2, 64: 3}   # (width = 8 << code)
    entries = b"".join(struct.pack("<II", (fmt << 26) | (code[size // (2 if mixed and i else 1)] << 20)
                                   | (code[size] << 23), 0) for i in range(count))
    info = bytes((0, count)) + struct.pack("<H", 0) + b"\0" * 4 + b"\0" * (4 * (count + 1)) + b"\0" * 4 + entries
    tex0 = b"TEX0" + b"\0" * 10 + struct.pack("<H", 0x3C) + b"\0" * (0x3C - 16) + info
    return b"BTX0" + b"\0" * 12 + struct.pack("<I", 20) + tex0


class FakeSources:
    """follower_sources with made-up files: 4 sprites, 3 species."""

    def __init__(self, sprites):
        self.REPO = "example/repo"
        self.COMMIT = "0" * 40
        self.SPRITE_PATH = "dir/mmodel_{:08d}.NSBTX"
        self.FIRST_FILE = 10
        self.FILES = [(len(s), hashlib.sha256(s).hexdigest()) for s in sprites]
        self.MODEL = [0, 1, 3]       # species 2: 1 or (female) 2
        self.FORM_MAX = [0, 0, 0]
        self.FEMALE = [0, 1, 0]


class TextureSizeTest(unittest.TestCase):
    def test_sizes(self):
        self.assertEqual(followers.texture_size(make_nsbtx(32)), (32, 32))
        self.assertEqual(followers.texture_size(make_nsbtx(64)), (64, 64))

    def test_rejects(self):
        for bad in (make_nsbtx(frames=6), make_nsbtx(fmt=4), make_nsbtx(16), make_nsbtx(mixed=True),
                    b"NOPE" + b"\0" * 60, make_nsbtx()[:40]):
            with self.assertRaises(followers.FollowersError):
                followers.texture_size(bad)


class BundleTest(unittest.TestCase):
    def setUp(self):
        self.sprites = [make_nsbtx(32) + b"a", make_nsbtx(32) + b"bc", make_nsbtx(32), make_nsbtx(64)]
        patcher = mock.patch.object(followers, "sources", FakeSources(self.sprites))
        patcher.start()
        self.addCleanup(patcher.stop)

    def test_round_trip(self):
        bundle = followers.make_bundle(self.sprites)
        self.assertEqual(bundle[:4], b"PFOL")
        self.assertEqual(len(bundle) % 4, 0)
        out = followers.read_bundle(bundle)
        self.assertEqual(out["species"], [(0, 0, 0), (1, 0, 1), (3, 0, 0)])
        self.assertEqual([s["data"] for s in out["sprites"]], self.sprites)
        self.assertEqual([s["width"] for s in out["sprites"]], [32, 32, 32, 64])

    def test_checks_each_file(self):
        changed = list(self.sprites)
        changed[1] = changed[1][:-1] + b"x"
        with self.assertRaises(followers.FollowersError):
            followers.make_bundle(changed)
        with self.assertRaises(followers.FollowersError):
            followers.make_bundle(self.sprites[:3])

    def test_url(self):
        self.assertEqual(followers.sprite_url(1),
                         "https://raw.githubusercontent.com/example/repo/" + "0" * 40 + "/dir/mmodel_00000011.NSBTX")

    def test_web_reads_the_download(self):
        import web
        with tempfile.TemporaryDirectory() as d, mock.patch.object(web.followers, "sources", followers.sources):
            for n, sprite in enumerate(self.sprites):
                path = Path(d) / followers.sources.SPRITE_PATH.format(followers.sources.FIRST_FILE + n)
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(sprite)
            self.assertEqual(web.read_followers(d), followers.make_bundle(self.sprites))

    def test_packed(self):
        bundle = followers.make_bundle(self.sprites)
        with tempfile.TemporaryDirectory() as d:
            root = Path(d) / "data_pack"
            root.mkdir()
            (root / "a.bin").write_bytes(b"abc")
            (root / "manifest.json").write_text(
                '{"rom_sha1": "' + "1" * 40 + '", "files": [{"id": 5, "path": "a.bin", "size": 3}]}')
            out = Path(d) / "out.pak"
            pack.pack(root, out, build_id="t", extra=[(followers.PAK_ID, bundle)])
            files = pack.read_pak(out)["files"]
            self.assertEqual(files, {5: b"abc", followers.PAK_ID: bundle})
            with self.assertRaises(ValueError):
                pack.pack(root, out, build_id="t", extra=[(5, b"x")])


class PinnedSourcesTest(unittest.TestCase):
    """The pinned list's shape (tools/make_follower_sources.py)."""

    def test_shape(self):
        s = follower_sources
        self.assertEqual(len(s.FILES), 566)
        self.assertEqual(len(s.MODEL), 493)
        self.assertEqual(len(s.FORM_MAX), 493)
        self.assertEqual(len(s.FEMALE), 493)
        self.assertRegex(s.COMMIT, r"^[0-9a-f]{40}$")
        for size, digest in s.FILES:
            self.assertGreater(size, 0)
            self.assertRegex(digest, r"^[0-9a-f]{64}$")
        for m, f, g in zip(s.MODEL, s.FORM_MAX, s.FEMALE):
            self.assertLess(m + max(f, g), len(s.FILES))
            self.assertIn(g, (0, 1))
        self.assertEqual(s.MODEL[0], 0)   # Bulbasaur: the first sprite


if __name__ == "__main__":
    unittest.main()
