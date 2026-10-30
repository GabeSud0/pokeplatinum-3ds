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
    """A made-up follower NSBTX as HeartGold's are laid out: the TEX0
    block's header, a texture dictionary of `frames` 16-colour textures
    (pixels: the frame's number), two palettes (normal, shiny)."""
    code = {8: 0, 16: 1, 32: 2, 64: 3}   # (width = 8 << code)
    texel = size * size // 2
    names = sorted(b"tsure_poke.%d" % k for k in [1] + list(range(10, 10 + frames - 1)))
    params = []
    for i in range(frames):
        w = size // (2 if mixed and i else 1)
        params.append(struct.pack("<II", (fmt << 26) | (code[w] << 20) | (code[size] << 23) | (i * texel >> 3), 0))
    tex_dict = followers._dictionary(names, params)
    pltt_dict = followers._dictionary([b"tsure_poke0", b"tsure_poke1"], [struct.pack("<HH", 0, 0), struct.pack("<HH", 4, 0)])
    tex_data = b"".join(bytes([i]) * texel for i in range(frames))
    pltt_data = bytes(range(32)) + bytes(range(100, 132))
    header = bytearray(0x3C)
    header[0:4] = b"TEX0"
    td = 0x3C + len(tex_dict) + len(pltt_dict)
    pd = td + len(tex_data)
    struct.pack_into("<HH", header, 0x0C, len(tex_data) >> 3, 0x3C)
    struct.pack_into("<I", header, 0x14, td)
    struct.pack_into("<II", header, 0x24, pd, pd)
    struct.pack_into("<H", header, 0x30, len(pltt_data) >> 3)
    struct.pack_into("<II", header, 0x34, 0x3C + len(tex_dict), pd)
    body = bytes(header) + tex_dict + pltt_dict + tex_data + pltt_data
    struct.pack_into("<I", header, 0x04, len(body))
    body = bytes(header) + body[0x3C:]
    return b"BTX0" + struct.pack("<HHIHH", 0xFEFF, 1, 20 + len(body), 16, 1) + struct.pack("<I", 20) + body


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
        self.assertEqual([s["data"] for s in out["sprites"]], [followers.platinum_layout(s) for s in self.sprites])
        self.assertEqual([s["width"] for s in out["sprites"]], [32, 32, 32, 64])

    def test_checks_each_file(self):
        changed = list(self.sprites)
        changed[1] = changed[1][:-1] + b"x"
        with self.assertRaises(followers.FollowersError):
            followers.make_bundle(changed)
        with self.assertRaises(followers.FollowersError):
            followers.make_bundle(self.sprites[:3])

    def test_cache(self):
        """Files kept in the cache and right aren't downloaded; wrong ones are."""
        with tempfile.TemporaryDirectory() as d:
            cache = Path(d)
            for n, sprite in enumerate(self.sprites):
                (cache / f"{followers.sources.FIRST_FILE + n:08d}.NSBTX").write_bytes(sprite if n else b"stale")
            asked = []

            class Response:
                def __init__(self, data):
                    self.data = data

                def __enter__(self):
                    return self

                def __exit__(self, *exc):
                    return False

                def read(self):
                    return self.data

            def urlopen(url, timeout):
                asked.append(url)
                return Response(self.sprites[0])

            with mock.patch("urllib.request.urlopen", urlopen):
                got = followers.fetch(cache=cache)
            self.assertEqual(got, self.sprites)
            self.assertEqual(asked, [followers.sprite_url(0)])
            self.assertEqual((cache / f"{followers.sources.FIRST_FILE:08d}.NSBTX").read_bytes(), self.sprites[0])

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


class PlatinumLayoutTest(unittest.TestCase):
    """A follower laid out as Platinum's walking sprites."""

    def entries(self, data):
        tex = struct.unpack_from("<I", data, 16)[0]
        d = tex + struct.unpack_from("<H", data, tex + 0x0E)[0]
        n = data[d + 1]
        at = d + 8 + 4 * (n + 1) + 4
        names = [data[at + 8 * n + 16 * i:at + 8 * n + 16 * i + 16].rstrip(b"\0") for i in range(n)]
        params = [struct.unpack_from("<I", data, at + 8 * i)[0] for i in range(n)]
        return names, params, tex

    def test_frames(self):
        original = make_nsbtx(32)
        out = followers.platinum_layout(original)
        names, params, tex = self.entries(out)
        self.assertEqual(len(names), 16)
        self.assertEqual(names, sorted(names))
        texel = 32 * 32 // 2
        for name, param in zip(names, params):
            k = int(name.rsplit(b".", 1)[1]) - 1
            frame = (param & 0xFFFF) * 8 // texel
            self.assertEqual(frame, 2 * (k // 4) + k % 2, name)   # (direction k // 4: its two frames by turns)
        tree = followers.dictionary_tree(names)
        for i, name in enumerate(names):
            self.assertEqual(followers.dictionary_lookup(tree, names, name), i)
        self.assertEqual(followers.texture_size(out, 16), (32, 32))
        # the pixels and palettes kept
        texdata = tex + struct.unpack_from("<I", out, tex + 0x14)[0]
        self.assertEqual(out[texdata:texdata + 8 * texel], b"".join(bytes([i]) * texel for i in range(8)))
        self.assertTrue(out.endswith(bytes(range(32)) + bytes(range(100, 132))))
        self.assertEqual(struct.unpack_from("<I", out, 8)[0], len(out))

    def test_big(self):
        out = followers.platinum_layout(make_nsbtx(64))
        self.assertEqual(followers.texture_size(out, 16), (64, 64))


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
