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

    def test_sprite_layers(self):
        cap_dir = core.make_sample_capture(self.wide, 2, sprites=True)
        cap = core.read_capture(cap_dir)
        obj = cap.layer("obj102")
        self.assertIsNotNone(obj)
        self.assertEqual((obj.kind, obj.group, obj.prio, obj.hofs, obj.vofs, obj.count),
                         ("obj", 102, 3, 368, 64, 72))
        self.assertEqual(obj.art_name(False), "10d372806e45f5c2_368_64.png")
        self.assertIn("palette 6", obj.label)
        self.assertIs(cap.layer(3), cap.layer("bg3"))
        # sprites of a priority are in front of its BGs
        self.assertEqual([l.id for l in core.draw_order(cap.layers)], ["bg3", "obj102", "bg1"])
        images = {l.id: cap.layer_image(l) for l in cap.layers}
        out = core.composite(cap, images)
        self.assertEqual(out.getpixel((100, 130))[:3], (0, 200, 0))
        hidden = core.composite(cap, images, hidden={"obj102"})
        self.assertNotEqual(hidden.getpixel((100, 130))[:3], (0, 200, 0))

    def test_drawing_layers(self):
        red = (255, 0, 0, 255)
        a = core.Paint("a", Image.new("RGBA", (core.W, core.H), (0, 0, 0, 0)))
        a.plot(5, 5, red)
        b = a.copy("b")
        b.x, b.y = 10, 3                       # moved
        flat = core.flatten([a, b])
        self.assertEqual(flat.getpixel((5, 5)), red)
        self.assertEqual(flat.getpixel((15, 8)), red)
        b.x = -10                              # moved past the edge: nothing lost
        self.assertEqual(core.flatten([b]).getpixel((5, 5))[3], 0)
        b.x = 10
        self.assertEqual(core.flatten([b]).getpixel((15, 8)), red)
        b.visible = False
        self.assertEqual(core.flatten([a, b]).getpixel((15, 8))[3], 0)
        # drawing where the moved layer's image doesn't reach: it grows
        b.plot(2, 1, red)
        self.assertEqual((b.x, b.y), (0, 0))
        self.assertEqual(b.view().getpixel((2, 1)), red)
        self.assertEqual(b.view().getpixel((15, 8)), red)
        view = b.view()
        view.putpixel((15, 8), (0, 0, 0, 0))
        b.set_view(view)
        self.assertEqual(b.view().getpixel((15, 8))[3], 0)

    def test_lock_and_band_fill(self):
        cap = core.list_captures(self.wide)[0]
        original = cap.layer_image(cap.layer(3))
        changed = original.copy()
        changed.putpixel((100, 5), (1, 2, 3, 255))
        changed.putpixel((5, 5), (1, 2, 3, 255))
        locked = core.lock_middle(changed, original)
        self.assertEqual(locked.getpixel((100, 5)), original.getpixel((100, 5)))
        self.assertEqual(locked.getpixel((5, 5)), (1, 2, 3, 255))
        blank = Image.new("RGBA", (core.W, core.H), (0, 0, 0, 0))
        out = core.shift_band_fill(blank, original, "mirror", "left")
        self.assertEqual(out.getpixel((31, 50))[0], 32)
        self.assertEqual(out.getpixel((300, 50))[3], 0)     # (the right band not chosen)
        self.assertEqual(out.getpixel((100, 50))[3], 0)     # (the middle never)

    def test_projects(self):
        cap = core.list_captures(self.wide)[0]
        layer = cap.layer(3)
        original = cap.layer_image(layer)
        stack = [core.Paint("base", original.copy()),
                 core.Paint("top", Image.new("RGBA", (10, 10), (255, 0, 0, 255)), 3, 4, False)]
        core.save_art(self.wide, layer, core.flatten(stack), original)
        core.save_project(self.wide, layer.key, stack, 1)
        loaded, active = core.load_project(self.wide, layer.key)
        self.assertEqual(active, 1)
        self.assertEqual([(p.name, p.x, p.y, p.visible) for p in loaded],
                         [("base", 0, 0, True), ("top", 3, 4, False)])
        self.assertEqual(loaded[1].image.size, (10, 10))
        # the game's picture changed elsewhere since: the layers aren't used
        art = core.existing_art(self.wide, layer.key)
        import os
        os.utime(art, (1, 1))
        self.assertIsNone(core.load_project(self.wide, layer.key))
        core.delete_project(self.wide, layer.key)
        self.assertFalse(core.project_dir(self.wide, layer.key).exists())
        # the game doesn't read the editor's folder (its names aren't pictures')
        self.assertEqual(sorted(p.name for p in self.wide.iterdir() if p.suffix == ".png"), [art.name])

    def test_brush(self):
        self.assertEqual(core.brush(1), [(0, 0)])
        self.assertEqual(sorted(core.brush(2)), [(0, 0), (0, 1), (1, 0), (1, 1)])
        b5 = core.brush(5)
        self.assertIn((0, 0), b5)
        self.assertIn((-2, 0), b5)
        self.assertNotIn((-2, -2), b5)             # round
        self.assertEqual(len(core.brush(32)), len(set(core.brush(32))))

    def test_colours(self):
        self.assertEqual(core.hsv_to_rgb(0, 1, 1), (255, 0, 0))
        self.assertEqual(core.hsv_to_rgb(120, 1, 1), (0, 255, 0))
        h, s, v = core.rgb_to_hsv((0, 0, 255))
        self.assertEqual((round(h), s, v), (240, 1.0, 1.0))
        self.assertEqual(core.parse_hex("#ff8000"), (255, 128, 0))
        self.assertEqual(core.parse_hex("f80"), (255, 136, 0))
        self.assertIsNone(core.parse_hex("zz"))
        sq = core.sv_square(0, 64)
        self.assertEqual(sq.getpixel((0, 0)), (255, 255, 255))      # white at the top left
        self.assertEqual(sq.getpixel((63, 0)), (255, 0, 0))         # the hue at the top right
        self.assertEqual(sq.getpixel((40, 63)), (0, 0, 0))          # black at the bottom
        self.assertEqual(core.hue_bar(4, 64).size, (4, 64))

    def test_transform(self):
        img = Image.new("RGBA", (4, 2), (0, 0, 0, 0))
        img.putpixel((0, 0), (255, 0, 0, 255))
        self.assertEqual(core.transform(img, 2, 2).size, (8, 4))
        big = core.transform(img, 2, 2)
        self.assertEqual(big.getpixel((1, 1)), (255, 0, 0, 255))      # sharp: whole pixels
        self.assertEqual(big.getpixel((2, 0))[3], 0)
        r = core.transform(img, angle=90)                              # anticlockwise
        self.assertEqual(r.size, (2, 4))
        self.assertEqual(r.getpixel((0, 3)), (255, 0, 0, 255))
        self.assertEqual(core.transform(img, angle=-90).getpixel((1, 0)), (255, 0, 0, 255))
        t = core.transform(img, angle=30)
        self.assertGreater(t.width, 4)
        self.assertTrue({p[3] for p in t.getdata()} <= {0, 255})      # no half-transparent pixels
        self.assertEqual(core.transform(img, 0.1, 0.1).size, (1, 1))

    def test_pictures_by_place(self):
        cap = core.list_captures(self.wide)[0]
        layer = cap.layer(3)                       # scroll 8, 300
        original = cap.layer_image(layer)
        img = core.fill_bands(original, "extend")
        self.assertEqual(core.parse_art_name("6d5a3d55615351bf_8_300_full_loop.png"),
                         ("6d5a3d55615351bf", 8, 300, True, True))
        self.assertIsNone(core.parse_art_name("6d5a3d55615351bf_8_300.jpg"))
        self.assertIsNone(core.find_art(self.wide, layer))
        p1 = core.save_art(self.wide, layer, img, original, loop=True)
        self.assertEqual(p1.name, "6d5a3d55615351bf_8_300_loop.png")
        ref = core.find_art(self.wide, layer)
        self.assertEqual((ref.path, ref.dx, ref.dy, ref.loop), (p1, 0, 0, True))
        # the same layer captured scrolled 4 further: that picture, lined up
        near = core.Layer(3, 3, layer.key, 12, 300, layer.file)
        ref = core.find_art(self.wide, near)
        self.assertEqual((ref.path, ref.dx, ref.dy), (p1, -4, 0))
        # another part of the map (256 down): not that one; saved, both stay
        far = core.Layer(3, 3, layer.key, 8, 44, layer.file)   # (300 + 256 = 556 = 44 round 512)
        self.assertIsNone(core.find_art(self.wide, far))
        p2 = core.save_art(self.wide, far, img, original)
        self.assertTrue(p1.exists() and p2.exists())
        self.assertEqual(core.find_art(self.wide, far).path, p2)
        self.assertEqual(core.find_art(self.wide, layer).path, p1)
        # saved again from the near capture: it takes the place of the one it was drawn from
        p3 = core.save_art(self.wide, near, img, original, replace=p1)
        self.assertFalse(p1.exists())
        self.assertTrue(p2.exists() and p3.exists())
        self.assertEqual(p3.name, "6d5a3d55615351bf_12_300.png")
        # a sprite group's: the nearest, however far (they move round)
        obj = core.Layer(-1, 3, "10d372806e45f5c2", 368, 64, "obj102.png", "obj", 102)
        core.save_art(self.wide, obj, img, original)
        moved = core.Layer(-1, 3, "10d372806e45f5c2", 100, 64, "obj102.png", "obj", 102)
        self.assertEqual(core.find_art(self.wide, moved).dx, 268 - 512)
        # drawing layers kept for each place
        stack = [core.Paint("a", img.copy())]
        core.save_project(self.wide, far.key, stack, 0, far.hofs, far.vofs, p2)
        self.assertIsNotNone(core.load_project(self.wide, far.key, far.hofs, far.vofs, p2))
        self.assertIsNone(core.load_project(self.wide, far.key, 12, 300, p3))

    def test_moved_to_another_place(self):
        """A layer stopped 60 px to the right (scroll 452 on a 512-wide map):
        its picture moved by 60, used only if it lines up with the DS's."""
        cap = core.list_captures(self.wide)[0]
        layer = cap.layer(1)                              # scroll 0, 0
        pic = Image.new("RGBA", (core.W, core.H))
        pic.paste((255, 255, 255, 255), (200, 80, 320, 112))   # a bar cut at the wide edge
        pic.paste((0, 0, 255, 255), (0, 0, 10, 4))
        pic.save(self.wide / f"{layer.key}_0_0_full.png")
        self.assertIsNone(core.moved_art(self.wide, cap, layer))   # it has one there
        shown = Image.new("RGBA", (core.W, core.H))
        shown.paste((255, 255, 255, 255), (260, 80, core.DS_X1, 112))   # what the DS shows of it there
        shown.paste((0, 0, 255, 255), (60, 0, 70, 4))
        shown.save(cap.path / "bg1.png")
        there = core.Layer(1, 0, layer.key, 452, 0, "bg1.png")
        img, ref = core.moved_art(self.wide, cap, there)
        self.assertEqual((ref.dx, ref.dy), (60, 0))
        self.assertEqual(img.getpixel((319, 90)), (255, 255, 255, 255))   # on to the edge
        self.assertEqual(img.getpixel((259, 90))[3], 0)
        self.assertEqual(img.getpixel((60, 0)), (0, 0, 255, 255))
        # not lining up with what the DS shows: none
        shown.paste((255, 0, 0, 255), (100, 100, 101, 101))
        shown.save(cap.path / "bg1.png")
        self.assertIsNone(core.moved_art(self.wide, cap, there))
        # the command
        sys.path.insert(0, str(Path(__file__).resolve().parent / "wideart"))
        import fill_places
        shown.paste((0, 0, 0, 0), (100, 100, 101, 101))
        shown.save(cap.path / "bg1.png")
        (cap.path / "info.txt").write_text((cap.path / "info.txt").read_text().replace(
            "key c348e56dbb30d48a hofs 0 vofs 0", "key c348e56dbb30d48a hofs 452 vofs 0"))
        made = fill_places.fill(self.wide, core.list_captures(self.wide), write=True)
        self.assertEqual([p.name for p in made], [f"{layer.key}_452_0_full.png"])
        self.assertTrue(made[0].exists())

    def test_layout_pictures(self):
        """A picture the screen's layout draws (the party's panel...): a
        layer of the capture, just in front of its BG; its file the drawing
        layer, where the game puts it (past the canvas's edge too); saved
        back from its top-left corner, bigger if drawn past its right or
        bottom edge, what's left of or above it left out."""
        cap_dir = core.make_sample_capture(self.root / "p", 1, sprites=True, picture=True)
        art = cap_dir.parent.parent
        cap = core.list_captures(art)[0]
        pic = cap.layer("pic_party_ball")
        self.assertEqual((pic.kind, pic.key, pic.bg, pic.prio, pic.hofs, pic.vofs, pic.w, pic.h),
                         ("picture", "party_ball", 3, 3, -4, 10, 8, 6))
        self.assertEqual(pic.art_name(True), "party_ball.png")
        self.assertEqual([l.id for l in core.draw_order(cap.layers)], ["bg3", "pic_party_ball", "obj102", "bg1"])
        ref = core.find_art(art, pic)
        self.assertEqual(ref.path, art / "party_ball.png")
        paint = core.picture_paint(art, cap, pic)
        self.assertEqual((paint.x, paint.y, paint.image.size), (-4, 10, (8, 6)))
        self.assertEqual(paint.view().getpixel((0, 10))[:3], (200, 40, 60))   # its x 4
        # drawn on: its x 5, and past its right edge (x 9) and its top (canvas y 9)
        paint.plot(1, 11, (0, 0, 255, 255))
        paint.plot(5, 14, (0, 255, 0, 255))
        path, lost = core.save_picture(art, pic, [paint])
        saved = core.load_rgba(path)
        self.assertEqual(saved.size, (10, 6))
        self.assertFalse(lost)
        self.assertEqual(saved.getpixel((0, 0))[:3], core.snap((200, 40, 60))[:3])   # (off the canvas: kept)
        self.assertEqual(saved.getpixel((5, 1))[:3], (0, 0, 255))
        self.assertEqual(saved.getpixel((9, 4))[:3], (0, 255, 0))
        self.assertEqual(saved.getpixel((9, 0))[3], 0)
        paint.plot(2, 9, (255, 255, 0, 255))
        path, lost = core.save_picture(art, pic, [paint])
        self.assertTrue(lost)
        self.assertEqual(core.load_rgba(path).size, (10, 6))
        # its file gone: the capture's picture of it (what was on the canvas)
        path.unlink()
        self.assertIsNone(core.find_art(art, pic))
        paint = core.picture_paint(art, cap, pic)
        self.assertEqual((paint.x, paint.y, paint.image.size), (0, 0, (core.W, core.H)))
        self.assertEqual(paint.image.getpixel((0, 10))[:3], (200, 40, 60))
        self.assertEqual(core.delete_art(art, pic), 0)


if __name__ == "__main__":
    unittest.main()
