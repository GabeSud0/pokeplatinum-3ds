#!/usr/bin/env python3
"""Tests for tools/game_overlays.py, with a made-up linker spec and objects
compiled here (skipped if there's no C compiler)."""

from __future__ import annotations

import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import game_overlays as go  # noqa: E402

LSF = """\
Static main
{
\tAddress 0x02000000
\tObject main.nef.p/src_main.c.o
}

Autoload ITCM
{
\tAddress 0x01FF8000
}

Overlay first
{
\tAfter main
\tObject main.nef.p/src_field_map.c.o
}

Overlay second
{
\tAfter first
\tObject main.nef.p/src_field_map_extra.c.o
\tObject trainer_ai_script.o
\tLibrary libgds.a
\tLibrary libdwcauth.a
}

Overlay fast  # comment
{
\tAfter ITCM
}

Overlay placed
{
\tAddress 0x027E0100
}
"""


class ParseTests(unittest.TestCase):
    def test_overlays_in_order_with_contents(self):
        overlays = go.parse_lsf(LSF)
        self.assertEqual([o["name"] for o in overlays], ["first", "second", "fast", "placed"])
        self.assertEqual(overlays[1]["objects"],
                         ["main.nef.p/src_field_map_extra.c.o", "trainer_ai_script.o"])
        self.assertEqual(overlays[1]["libraries"], ["libgds.a", "libdwcauth.a"])
        self.assertEqual(overlays[3]["address"], 0x027E0100)

    def test_regions_follow_after_chains_and_addresses(self):
        overlays = go.parse_lsf(LSF)
        self.assertEqual(go.region(overlays, "first"), "MAIN")
        self.assertEqual(go.region(overlays, "second"), "MAIN")
        self.assertEqual(go.region(overlays, "fast"), "ITCM")
        self.assertEqual(go.region(overlays, "placed"), "DTCM")

    def test_rejects_bad_specs(self):
        with self.assertRaises(ValueError):
            go.parse_lsf("Static main\n{\n}\n")
        with self.assertRaises(ValueError):
            go.parse_lsf("Overlay a\n{\n}\nOverlay a\n{\n}\n")
        with self.assertRaises(ValueError):
            go.region(go.parse_lsf("Overlay a\n{\n\tAfter b\n}\nOverlay b\n{\n\tAfter a\n}\n"), "a")

    def test_meson_object_names(self):
        self.assertEqual(go.meson_object_name("field_map.c"), "main.nef.p/src_field_map.c.o")
        self.assertEqual(go.meson_object_name("overlay005/ov5_x.c"), "main.nef.p/src_overlay005_ov5_x.c.o")


class ObjectMapTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.up = Path(self.tmp.name) / "upstream"
        (self.up / "src").mkdir(parents=True)
        for name in ("main.c", "field_map.c", "field_map_extra.c"):
            (self.up / "src" / name).write_text("int x;\n", encoding="utf-8")
        (self.up / "src" / "meson.build").write_text(
            "srcs = files('main.c', 'field_map.c', 'field_map_extra.c')\n", encoding="utf-8")
        (self.up / "lib" / "gds" / "src").mkdir(parents=True)
        (self.up / "lib" / "gds" / "src" / "a.c").write_text("int a;\n", encoding="utf-8")

    def test_objects_per_overlay(self):
        objects = go.overlay_objects(self.up, go.parse_lsf(LSF), Path("obj"))
        self.assertEqual(objects["first"], [Path("obj/src/field_map.o")])
        # The AI script, then the game's own library; the Wi-Fi SDK has none.
        self.assertEqual(objects["second"], [Path("obj/src/field_map_extra.o"),
                                             Path("obj/src/battle/trainer_ai/script.o"),
                                             Path("obj/lib/gds/src/a.o")])
        self.assertEqual(objects["fast"], [])

    def test_unknown_object_is_an_error(self):
        bad = LSF.replace("src_field_map.c.o", "src_nope.c.o")
        with self.assertRaises(ValueError):
            go.overlay_objects(self.up, go.parse_lsf(bad), Path("obj"))


class TableTests(unittest.TestCase):
    def test_every_overlay_gets_both_sections(self):
        text = go.render_table(go.parse_lsf(LSF))
        self.assertIn("extern u8 __start_ovl_data_first[], __stop_ovl_data_first[];", text)
        self.assertIn("extern u8 __start_ovl_bss_fast[], __stop_ovl_bss_fast[];", text)
        self.assertIn('{ "second", NITRO_PORT_OVERLAY_MAIN, __start_ovl_data_second, '
                      '__stop_ovl_data_second, __start_ovl_bss_second, __stop_ovl_bss_second }', text)
        self.assertIn('NITRO_PORT_OVERLAY_ITCM', text)
        self.assertIn('".section ovl_bss_placed,\\"aw\\",%nobits', text)
        self.assertIn("/*   3 */", text)

    @unittest.skipUnless(shutil.which("cc"), "needs cc")
    def test_table_compiles_and_pads_sections(self):
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            (d / "nitro_port.h").write_text(
                "typedef unsigned char u8; typedef unsigned int u32;\n"
                "enum { NITRO_PORT_OVERLAY_MAIN, NITRO_PORT_OVERLAY_ITCM, NITRO_PORT_OVERLAY_DTCM };\n"
                "typedef struct { const char *name; u32 region; u8 *a, *b, *c, *d; } NitroPortOverlay;\n",
                encoding="utf-8")
            (d / "t.c").write_text(go.render_table(go.parse_lsf(LSF)), encoding="utf-8")
            subprocess.run(["cc", "-c", "-I", str(d), str(d / "t.c"), "-o", str(d / "t.o")], check=True)
            sections = dict(go.sections(d / "t.o"))
            self.assertEqual(sections["ovl_data_first"], 4)
            self.assertEqual(sections["ovl_bss_fast"], 4)


@unittest.skipUnless(shutil.which("cc") and shutil.which("objcopy"), "needs cc and objcopy")
class RenameTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.dir = Path(self.tmp.name)

    def compile(self, source: str, *flags: str) -> Path:
        c = self.dir / "x.c"
        o = self.dir / "x.o"
        c.write_text(source, encoding="utf-8")
        subprocess.run(["cc", "-c", "-O2", *flags, str(c), "-o", str(o)], check=True)
        return o

    def test_data_and_bss_renamed_once(self):
        obj = self.compile("int a = 1; int z; static int s = 2;\n"
                           "const int k = 3; int *f(void) { static int t; return &t + s; }\n",
                           "-fdata-sections")
        kinds = go.rename_sections(obj, "field", "objcopy")
        self.assertEqual(kinds, {"data", "bss"})
        names = go.section_names(obj)
        self.assertIn("ovl_data_field", names)
        self.assertIn("ovl_bss_field", names)
        self.assertFalse([n for n in names if go.data_kind(n)])
        self.assertTrue([n for n in names if n.startswith(".rodata")])  # constants stay
        # A second pass changes nothing and reports the same.
        before = obj.read_bytes()
        self.assertEqual(go.rename_sections(obj, "field", "objcopy"), {"data", "bss"})
        self.assertEqual(obj.read_bytes(), before)

    def test_object_without_data(self):
        obj = self.compile("int f(int x) { return x + 1; }\n")
        self.assertEqual(go.rename_sections(obj, "code", "objcopy"), set())


class SectionKindTests(unittest.TestCase):
    def test_kinds(self):
        self.assertEqual(go.data_kind(".data"), "data")
        self.assertEqual(go.data_kind(".data.sCount"), "data")
        self.assertEqual(go.data_kind(".bss.sBuffer"), "bss")
        self.assertIsNone(go.data_kind(".data.rel.ro.table"))
        self.assertIsNone(go.data_kind(".rodata.str1.1"))
        self.assertIsNone(go.data_kind(".text.Func"))


if __name__ == "__main__":
    unittest.main()
