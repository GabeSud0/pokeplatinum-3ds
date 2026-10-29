# Notice

## What this project licenses

Only the original code, tools and documentation written for this project are
covered by this project's license, the GNU General Public License v3.0 (see
[`LICENSE`](LICENSE)).

## What it does not

- **The game.** Pokémon Platinum, Pokémon HeartGold/SoulSilver and all of their
  content belong to Nintendo, Game Freak, Creatures and The Pokémon Company.
  None of it is included here. Players supply their own legally dumped
  cartridge, and the data is extracted on their own computer.
- **The decompilation.** pret/pokeplatinum and pret/pokeheartgold are separate
  projects with their own terms. They are downloaded at build time, not
  redistributed here.
- **Third-party libraries.** devkitARM, libctru and citro3d keep their own
  licenses.

## Releases and the web builder

The releases carry the 3DS program, built from this project's code and the
decompilation, and the web builder's files: never a ROM or anything taken from
one. The web builder (`web/`) makes the data pack and the HOME Menu version in
the player's browser, from their own cartridge, with:

- [Pyodide](https://pyodide.org) (MPL-2.0), loaded from its CDN;
- bannertool (MIT) and makerom (MIT, with mbedtls under Apache-2.0, libyaml
  under MIT and blz under GPL-3.0-or-later), compiled to WebAssembly and
  carried in the release with their licences
  (`tools/build_cia_tools_wasm.sh`);
- [pycgfx](https://github.com/skyfloogle/pycgfx), which has no licence that
  allows sharing it: it is never copied here or into a release, and the page
  fetches it from its own repository at a pinned commit;
- the Chakra Petch and Atkinson Hyperlegible fonts (SIL Open Font License,
  `web/fonts/`).

**HeartGold and SoulSilver's follower sprites** (the Pokémon that walks
behind the player, OPTIONS: FOLLOWER) are Nintendo, Game Freak and
Creatures' work, not relicensed. They are never in this repository or a
release: the web builder (in the player's browser) and `builder/followers.py`
(on their computer) download them from
[pret/pokeheartgold](https://github.com/pret/pokeheartgold) at the commit
pinned in `builder/follower_sources.py`, check each file's size and SHA-256,
and put them in the player's own data pack. That file holds only numbers:
the commit, the files' sizes and digests, and which sprite each species uses.

## Rules for contributors

- Never commit ROMs, extracted assets, save files or built game binaries.
- Never add code or assets copied from leaked source code, or from ports that
  bundle the game's assets.
- Keep changes to the decompilation as patch files in `patches/`.

## License

The original code in this repository is licensed under the GNU General Public
License v3.0. Anyone may use, modify and share it, but distributed modified
versions must also be released as source code under GPL-3.0. By contributing,
you agree that your contributions are licensed the same way.

libctru and citro3d, which the 3DS code links against, are under the zlib
license, which is compatible with GPL-3.0.

`3ds_port/nitro/src/port_xbrz.c` (the 3D's textures at 2x, `filter3d xbrz`)
is a C version of the 2x scaler of xBRZ by Zenju, from the HqMAME project
(the copy in PPSSPP's `ext/xbrz`), which is distributed under the GNU General
Public License. This version is GPL-3.0 like the rest of the project.

`3ds_port/game/cia/icon.png` (the HOME Menu icon) is original art made for
this project, licensed with it.

The HOME Menu's 3D banner (`tools/banner3d`) is made at build time from the
player's own ROM: its Giratina model, textures and animation are the game's
and are never included here. Its portal glow and logo (`tools/banner3d/art`)
are original art made for this project, licensed with it. The banner's
converter, skyfloogle/pycgfx, is downloaded at build time and not
redistributed here (it has no licence that allows sharing it). The run-time
fixes to it in `tools/banner3d/to_cgfx.py` follow the ones found by
daaaaavia/CBC-for-YANBF (MIT licence). The NSBMD/NSBCA reading in
`tools/banner3d/nitro.py` follows apicula by scurest (0BSD-style licence:
use, copy, modify and distribute for any purpose).

`3ds_port/game/cia/chime.wav` (the HOME Menu banner's chime) is original
music written for this project, licensed with it. It was played with the
GeneralUser GS SoundFont by S. Christian Collins
(<https://www.schristiancollins.com>), whose license allows music made with
it to be used without restriction.
