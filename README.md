# pokeplatinum-3ds

A native Nintendo 3DS port of Pokémon Platinum, built from the
[pret/pokeplatinum](https://github.com/pret/pokeplatinum) decompilation, with
the goal of pushing the game's 3D further than the original hardware could:

- **Higher resolution**: 3D rendered at the 3DS top screen's native 400×240
  (vs. 256×192 on the DS), in widescreen.
- **Stereoscopic 3D**: real depth for the overworld and battles, driven by the
  3D slider.
- **Both screens, redesigned**: a touch interface on the bottom screen.
- **New 3DS first**: uses the faster CPU and extra memory of the New 3DS / New
  3DS XL / New 2DS XL.

Platinum is the **prototype**. The long-term target is **Pokémon SoulSilver /
HeartGold**, once the [pret/pokeheartgold](https://github.com/pret/pokeheartgold)
decompilation is far enough along. Both are Generation 4 games sharing much of
the same engine, so most of the 3DS backend should carry over.

> **Status:** Phase 1. Toolchain working (stereoscopic test app runs in
> Azahar); DS hardware layer mapped in
> [`docs/HARDWARE_LAYER.md`](docs/HARDWARE_LAYER.md). NitroSDK and
> NitroSystem (Fnd, G2d, G3d, Gfd, Snd) compile for the 3DS using GCC;
> PC/32-bit resource tests and CI status are tracked in
> [`3ds_port/nitro`](3ds_port/nitro) and
> [`3ds_port/nitrosystem`](3ds_port/nitrosystem). The game itself and
> its 3DS hardware/render/audio backends are not yet running. **Nothing
> playable yet.**

## This repository contains no game content

No ROM, graphics, music, text or any other data from the game is stored here
or in releases. The repository holds only:

- the port's own code (`3ds_port/`),
- patches applied on top of the decompilation and SDKs (`patches/`),
- tools that fetch the decompilation and build the port (`tools/`),
- the builder that turns **your own legally dumped cartridge** into the game's
  data, on your own computer (`builder/`).

Never commit a ROM, an extracted asset, or a build of the game. The
`.gitignore` blocks the common cases, but check `git status` before you commit.

## How it works

```
your ROM ──► builder/ ──► game data pack (stays on your computer / SD card)

pret/pokeplatinum @ pinned commit ──► + patches/pokeplatinum/ ──► + 3ds_port/ ──► .3dsx
```

The decompilation is pinned in [`upstream.lock`](upstream.lock) so everyone
builds against the same code. Our changes to it live as patch files, which keeps
them small, reviewable and easy to rebase onto a newer upstream.

## Requirements

- A **New 3DS family console** with custom firmware (Luma3DS) and the Homebrew
  Launcher. Setup guide: <https://3ds.hacks.guide>
- [devkitPro](https://devkitpro.org/wiki/Getting_Started) with the `3ds-dev`
  package group (devkitARM, libctru, citro3d, picasso, 3dslink).
- Python 3.10+ and Git.
- Optional: the [Azahar](https://azahar-emu.org) emulator for fast testing on PC.

## Quick start: the first milestone

Before touching the game, prove the toolchain works with the `hello3d` test
app: a spinning cube in stereoscopic 3D at 400×240.

```sh
cd 3ds_port/hello3d
make
```

Then either open `hello3d.3dsx` in Azahar, or send it to your console over
Wi‑Fi: open the Homebrew Launcher, press **Y**, and run

```sh
3dslink hello3d.3dsx
```

Move the 3D slider: the cube should gain depth. The bottom screen shows the
CPU and GPU time per frame. **START** exits.

Every push to GitHub also builds `hello3d` automatically (see the **Actions**
tab), and the `.3dsx` can be downloaded from the run.

## Fetching the decompilation

```sh
python tools/bootstrap.py
```

This downloads pret/pokeplatinum at the pinned commit into `build/upstream/`,
plus the exact NitroSDK, NitroSystem and NitroDWC revisions referenced by
the pinned decompilation's `subprojects/*.wrap` files into
`build/NitroSDK/`, `build/NitroSystem/` and `build/NitroDWC/`, and
applies any corresponding port patches from `patches/`. NitroDWC is
**fetched as a source dependency only**: Nintendo WFC, GameSpy and
NitroWiFi networking are not yet ported, compiled or functional.
To build the original DS
ROM from it (a good sanity check), follow pret's
[INSTALL.md](https://github.com/pret/pokeplatinum/blob/main/INSTALL.md) inside
`build/upstream/`.

To check that your own dump is the right version:

```sh
python builder/verify_rom.py path/to/your/platinum.nds
```

## Roadmap

See [`docs/ROADMAP.md`](docs/ROADMAP.md). In short:

0. Toolchain: hello3d running on Azahar and on a New 3DS.
1. Decomp: pokeplatinum fetched, patched and building.
2. Platform layer: separate the game from the DS hardware.
3. 3DS backend: 2D, 3D, audio, input, save data on the 3DS.
4. The upgrade: 400×240 widescreen 3D, stereoscopic depth, touch UI.
5. SoulSilver: move the backend onto pokeheartgold.

## Contributing

See [`CONTRIBUTING.md`](CONTRIBUTING.md) for how changes are made and tested,
and [`AI_DISCLOSURE.md`](AI_DISCLOSURE.md) for how AI assistance is used.

## Repository layout

```
3ds_port/        3DS-side code
  hello3d/       toolchain test app (stereoscopic cube)
  nitro/         NitroSDK built with GCC, plus PC unit tests
  nitrosystem/   NitroSystem's Fnd/G2d/G3d/Gfd/Snd libraries and host tests
  common.mk      shared host, host32 and devkitARM toolchain flags
builder/         turns the player's own ROM into the data pack (Python)
docs/            roadmap and design notes
patches/         changes applied on top of the decompilation and SDKs
tools/           bootstrap and helper scripts (Python)
LICENSE          GPL-3.0, for this project's own code
upstream.lock    the exact pret/pokeplatinum commit we build against
```

## Credits and legal

The original code in this repository is licensed under the
[GNU General Public License v3.0](LICENSE). That covers only this project's own
code, not the game or the decompilation.

Built on the work of the [pret](https://pret.github.io/) decompilation teams,
and inspired by
[pokeemerald-3Ds-dualscreen](https://github.com/ZallaxDev/pokeemerald-3Ds-dualscreen).
See [`NOTICE.md`](NOTICE.md).

This is an unofficial fan project, not affiliated with or endorsed by Nintendo,
Game Freak, Creatures or The Pokémon Company. Pokémon is a trademark of its
respective owners.
