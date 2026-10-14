"""The WideArt editor's workings (tools/wideart/wideart_core.py): reading a
capture as the game writes it, the band fills, saving where the game reads
(and as the game names the files: port_wideart.c)."""

import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / "wideart"))

try:
    from PIL import Image
    import wideart_core as core
except ImportError:  # (Pillow missing: the editor can't run either)
    core = None


def make_capture(root: Path, number: int = 1) -> Path:
    """A capture folder as NitroPort_WideArtCapture writes one: BG3 a
    gradient across x (red = x), BG1 transparent but for a white middle."""
    return core.make_sample_capture(root, number)


@unittest.skipIf(core is None, "Pillow isn't installed")
class WideArtTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.wide = self.root / "3ds" / "pokeplatinum" / "wide"
        self.cap_dir = make_capture(self.wide)

    def tearDown(self):
        self.tmp.cleanup()

    def test_finding_the_folder(self):
        for picked in (self.root, self.root / "3ds" / "pokeplatinum" / "wide", self.wide / "captures",
                       self.cap_dir):
            self.assertEqual(core.find_art_dir(picked), self.wide, picked)
        self.assertIsNone(core.find_art_dir(self.root / "3ds"))

    def test_reading_a_capture(self):
        caps = core.list_captures(self.wide)
        self.assertEqual(len(caps), 1)
        cap = caps[0]
        self.assertEqual(cap.backdrop, (255, 0, 0))
        self.assertEqual([l.bg for l in cap.layers], [1, 3])
        bg3 = cap.layer(3)
        self.assertEqual((bg3.prio, bg3.key, bg3.hofs, bg3.vofs), (3, "6d5a3d55615351bf", 8, 300))
        self.assertEqual(bg3.art_name(False), "6d5a3d55615351bf_8_300.png")
        self.assertEqual(bg3.art_name(True), "6d5a3d55615351bf_8_300_full.png")
        self.assertEqual([l.bg for l in core.draw_order(cap.layers)], [3, 1])

    def test_newest_capture_first(self):
        make_capture(self.wide, 2)
        self.assertEqual([c.name for c in core.list_captures(self.wide)], ["0002", "0001"])

    def test_band_fills(self):
        cap = core.list_captures(self.wide)[0]
        img = cap.layer_image(cap.layer(3))
        red = lambda im, x: im.getpixel((x, 50))[0]
        m = core.fill_bands(img, "mirror")
        self.assertEqual(red(m, 31), 32)        # x -1 shows the DS's 0 (canvas 32)
        self.assertEqual(red(m, 0), 63)
        self.assertEqual(red(m, 288), 287 % 256)
        self.assertEqual(red(m, 319), 256 % 256)
        e = core.fill_bands(img, "extend")
        self.assertEqual({red(e, x) for x in range(32)}, {32})
        r = core.fill_bands(img, "repeat")
        self.assertEqual(red(r, 0), 256 % 256)  # the DS's picture as if it wrapped
        self.assertEqual(red(r, 288), 32)
        c = core.fill_bands(img, "clear", "left")
        self.assertEqual(c.getpixel((5, 5))[3], 0)
        self.assertEqual(c.getpixel((300, 5)), img.getpixel((300, 5)))   # (the right side kept)
        self.assertEqual(c.getpixel((100, 5)), img.getpixel((100, 5)))   # (the middle never)

    def test_saving_where_the_game_reads(self):
        cap = core.list_captures(self.wide)[0]
        layer = cap.layer(3)
        original = cap.layer_image(layer)
        bands = core.fill_bands(original, "extend")
        path = core.save_art(self.wide, layer, bands, original)
        self.assertEqual(path.name, "6d5a3d55615351bf_8_300.png")
        self.assertEqual(core.existing_art(self.wide, layer.key), path)
        with Image.open(path) as saved:
            self.assertEqual(saved.size, (320, 192))
            self.assertEqual(saved.mode, "RGBA")
        # the middle changed: saved as the whole layer, the bands-only file gone
        whole = bands.copy()
        whole.putpixel((100, 100), (255, 255, 255, 255))
        path2 = core.save_art(self.wide, layer, whole, original)
        self.assertEqual(path2.name, "6d5a3d55615351bf_8_300_full.png")
        self.assertFalse(path.exists())
        self.assertEqual(core.delete_art(self.wide, layer), 1)
        self.assertIsNone(core.existing_art(self.wide, layer.key))

    def test_colours_as_the_ds_shows_them(self):
        self.assertEqual(core.snap((255, 0, 7, 255)), (255, 0, 0, 255))
        self.assertEqual(core.snap((8, 16, 255, 100)), (8, 16, 255, 0))
        cap = core.list_captures(self.wide)[0]
        img = Image.new("RGBA", (320, 192), (0, 0, 0, 0))
        img.paste((7, 7, 7, 255), (0, 0, 2, 1))
        layer = cap.layer(1)
        path = core.save_art(self.wide, layer, img, cap.layer_image(layer))
        self.assertEqual(core.load_rgba(path).getpixel((0, 0)), (0, 0, 0, 255))

    def test_flood_fill_keeps_off_the_locked_middle(self):
        img = Image.new("RGBA", (320, 192), (0, 0, 0, 0))
        allowed = lambda x, y: not (32 <= x < 288)
        out = core.flood_fill(img, 0, 0, (255, 0, 0, 255), allowed)
        self.assertEqual(out.getpixel((10, 100)), (255, 0, 0, 255))
        self.assertEqual(out.getpixel((100, 100))[3], 0)
        self.assertEqual(out.getpixel((300, 100))[3], 0)   # (not reached across the middle)

    def test_composite_and_preview(self):
        cap = core.list_captures(self.wide)[0]
        images = {l.bg: cap.layer_image(l) for l in cap.layers}
        out = core.composite(cap, images)
        self.assertEqual(out.getpixel((100, 90)), (255, 255, 255, 255))   # BG1 in front
        self.assertEqual(out.getpixel((100, 10))[:3], (100, 7, 9))        # BG3 behind
        faint = core.composite(cap, images, see_through=3)
        self.assertNotEqual(faint.getpixel((100, 90)), (255, 255, 255, 255))
        self.assertEqual(core.preview_3ds(out, 1).size, (400, 240))


if __name__ == "__main__":
    unittest.main()
