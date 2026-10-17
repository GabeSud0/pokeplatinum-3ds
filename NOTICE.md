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
