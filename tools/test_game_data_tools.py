"""Offline tests for the source list and the data-tool rule parsing."""
from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from game_sources import listed_sources
from prepare_game_dataproc import embed_jobs, naix_by_variable, sprite_template_jobs
from prepare_game_ordergen_naix import otherpoke_order, poke_icon_order


class DataToolTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.up = Path(self.tmp.name) / "upstream"

    def write(self, relative: str, text: str) -> None:
        path = self.up / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")

    def test_sources_are_the_meson_lists(self):
        self.write("src/a.c", "")
        self.write("src/sub/b.c", "")
        self.write("src/unlisted.c", "")
        self.write("src/meson.build", "pokeplatinum_c = files(\n    'a.c',\n    'sub/b.c',\n)\n")
        self.assertEqual(listed_sources(self.up), ["a.c", "sub/b.c"])

    def test_sprite_template_rule(self):
        self.write("res/graphics/menu/meson.build",
                   "menu_narc = custom_target('menu.narc',\n"
                   "    output: [ 'menu.narc', 'menu_gra.naix' ],\n"
                   "    command: [ arc ])\n")
        self.write("res/graphics/sprite_templates/meson.build",
                   "menu_resdat = custom_target('menu_resdat',\n"
                   "    output: [ 'menu.h' ],\n"
                   "    input: [\n        menu_narc[1],\n    ],\n"
                   "    command: [\n        resdatproc_exe,\n"
                   "        '-D', 'NARC_INDEX_MENU:@INPUT0@',\n"
                   "        '-p', 'menu',\n        '-N',\n"
                   "        '-o', meson.current_build_dir(),\n"
                   "        files('menu.json'),\n    ],\n)\n")
        resources = Path(self.tmp.name) / "resources"
        (resources / "res/graphics/menu").mkdir(parents=True)
        (resources / "res/graphics/menu/menu_gra.naix").write_text("", encoding="utf-8")
        self.assertEqual(naix_by_variable(self.up), {"menu_narc": Path("res/graphics/menu/menu_gra.naix")})
        jobs = sprite_template_jobs(self.up, resources)
        self.assertEqual(jobs, [["-D", "NARC_INDEX_MENU:res/graphics/menu/menu_gra.naix", "-p", "menu",
                                 "-N", str(self.up / "res/graphics/sprite_templates/menu.json")]])

    def test_embed_rule(self):
        self.write("res/fonts/meson.build",
                   "cursor_embed = custom_target('cursor.4bpp.h',\n"
                   "    output: [ 'cursor.4bpp', 'cursor.4bpp.h' ],\n"
                   "    input: files('cursor.png'),\n"
                   "    command: [ nitrogfx_exe, '@INPUT@', '@OUTPUT0@', '-embed', 'sCursorBitmap' ],\n)\n")
        self.assertEqual(embed_jobs(self.up), [(self.up / "res/fonts/cursor.png",
                                                Path("res/fonts/cursor.4bpp.h"), "sCursorBitmap")])

    def test_script_packed_archives(self):
        self.write("generated/species.txt", "SPECIES_NONE\nSPECIES_BULBASAUR\nSPECIES_BAD_EGG\n")
        self.write("res/pokemon/species_icons.order", "shared_pals.NCLR\nshared_anim.NANR\n")
        self.write("res/pokemon/none/meson.build", "poke_icon_files += files('icon.png')\n")
        self.write("res/pokemon/bulbasaur/meson.build", "poke_icon_files += files('icon.png')\n")
        self.write("res/pokemon/bad_egg/meson.build", "species_data_files += files('data.json')\n")
        self.write("res/pokemon/meson.build",
                   "# The last icon added to the list before this should have been\n"
                   "poke_icon_files += [\n    manaphy_egg_icon,\n    deoxys_attack_icon,\n]\n"
                   "x = custom_target('pl_otherpoke.narc', command: [ script,\n"
                   "    '--sprite-entries', '2',\n    '--palette-entries', '1',\n ])\n")
        self.assertEqual(poke_icon_order(self.up), [
            "shared_pals.NCLR", "shared_anim.NANR",
            "icon_00000.NCGR", "icon_00001.NCGR", "icon_00002.NCGR", "icon_00003.NCGR"])
        self.assertEqual(otherpoke_order(self.up), [
            "pl_otherpoke_0000.NCGR", "pl_otherpoke_0001.NCGR", "pl_otherpoke_0002.NCLR",
            "pl_otherpoke_0003.NCGR", "pl_otherpoke_0004.NCGR", "pl_otherpoke_0005.NCLR",
            "pokemon_shadows.NCGR", "pokemon_shadows_pal.NCLR"])


if __name__ == "__main__":
    unittest.main()
