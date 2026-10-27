<p align="center">
  <img src="docs/images/logo.png" alt="Pokémon Platinum 3DS" width="600">
</p>

<p align="center"><b>English</b> · <a href="README.es.md">Español</a></p>

<p align="center">
  <a href="https://discord.gg/YjyA5ygpC">
    <img src="https://img.shields.io/badge/Discord-Join%20the%20server-5865F2?logo=discord&logoColor=white" alt="Discord">
  </a>
</p>

# PokePlatinum-3DS

A native Nintendo 3DS port of Pokémon Platinum, built for the New 3DS, from the
[pret/pokeplatinum](https://github.com/pret/pokeplatinum) decompilation. It
isn't an emulator and it isn't a remake: the game's own code runs on the 3DS,
and the port draws it the way a 3DS game would.

On a New 3DS:

- **Both screens filled.** The DS's pictures are drawn 1.25 times bigger: the
  lower screen fills its 320×240, and the upper screen is laid out across its
  full 400-pixel width (menus, text boxes and battles included). Nothing is
  ever stretched or mirrored to fill the extra space.
- **Sharper 3D.** The overworld and battles are drawn by the 3DS's GPU at the
  screen's own resolution with smooth (anti-aliased) edges, the game's fog,
  and a wider view of the world at the sides. (The DS's outlines round the
  3D are off by default; `edges on` in `remaster.txt` draws them.)
- **Stereoscopic 3D** from the 3D slider.
- **60 frames a second in the field.** The game runs its field at 30; the port
  draws a frame in between each pair, and drops back to 30 where the GPU can't
  keep up (new, still being tuned).
- **Even, crisp pixels** for the 2D, and optional xBRZ smoothing for the 3D's
  textures.
- **Installs on the HOME Menu** (`.cia`), with your cartridge's icon.

On an Old 3DS, it shows the DS's two screens in the middle of the 3DS's.

Platinum is the **prototype**. The long-term target is **Pokémon SoulSilver /
HeartGold**, once the [pret/pokeheartgold](https://github.com/pret/pokeheartgold)
decompilation is far enough along. Both are Generation 4 games that share most
of the same engine.

## Status

**Works**
- Playable on real hardware, from a new game or a save: exploring,
  battles, menus and saving. It still has some visual and gameplay bugs.
- A new UI for the 3DS's top screen: menus, text boxes and battles laid out
  across its full width.
- A new resolution for the New 3DS XL: both screens filled at the 3DS's own
  size, with the 3D drawn at the screen's resolution.
- Updated animations for the intro.
- New animations for some of the UI.
- 60 frames a second
- Better 3D quality

**Work in progress**
- A better UI for the bottom screen
- A Pokémon follower

**Future wishlist**
- Wi-Fi
- Better lighting

## This repository contains no game content

No ROM, graphics, music, text or any other data from the game is stored here
or in releases. The repository holds only:

- the port's own code (`3ds_port/`),
- patches applied on top of the decompilation and SDKs (`patches/`),
- tools that fetch the decompilation and build the port (`tools/`),
- the builder that turns **your own legally dumped cartridge** into the game's
  data, on your own computer (`builder/`).

**Builds of the game are never published**, and you shouldn't share yours:
they contain the game's code. CI builds and tests everything on every push
but uploads only text reports and tools with no game code in them
(`tools/check_ci_artifacts.py` fails the build otherwise).

Never commit a ROM, an extracted asset, or a build of the game. The
`.gitignore` blocks the common cases, but check `git status` before you commit.

## What you need

- A **3DS family console** with custom firmware (Luma3DS) and the Homebrew
  Launcher (and **FBI** for the HOME Menu version). A New 3DS, New 3DS XL or
  New 2DS XL is needed for the remastered display. Setup guide:
  <https://3ds.hacks.guide>
- Your own dump of **Pokémon Platinum (USA)**, Rev 0 or Rev 1.
- [devkitPro](https://devkitpro.org/wiki/Getting_Started) with the `3ds-dev`
  package group, Python 3.10+ and Git.
- Optional: the [Azahar](https://azahar-emu.org) emulator for testing on a PC.
  It runs the 3DS's CPU slower than a real New 3DS, so judge performance on
  the console.

## Building and running it

### On Windows

Download [`tools/build_windows_msys2.sh`](tools/build_windows_msys2.sh) and
run it in devkitPro's MSYS2 terminal (Start menu → devkitPro → MSYS2). It
does everything: installs what the build needs besides devkitPro, clones this
repository, builds the game (both versions), makes your data pack, and puts
the SD card files in `%USERPROFILE%\pokeplatinum-sd` (and in Azahar's SD
folder, if it finds it):

```sh
bash build_windows_msys2.sh 'C:\path\to\your\platinum.nds'
```

Then skip to [Copy to the SD card](#copy-to-the-sd-card).

### On Linux

Download [`tools/build_linux.sh`](tools/build_linux.sh) into your home folder
and run it. Like the Windows script, it does everything: installs devkitPro
and the rest of what the build needs, clones this repository, builds the game
(both versions), makes your data pack, and puts the SD card files in
`~/pokeplatinum-sd` (and in Azahar's SD folder, if it finds it):

```sh
bash ~/build_linux.sh ~/path/to/your/platinum.nds
```

On **Bazzite** and other immutable systems (Fedora Silverblue, Kinoite,
Aurora, Bluefin...), it works inside a distrobox it creates once
(`pokeplatinum-3ds`, from devkitPro's own image, the one CI builds with), so your system isn't
changed; keep the ROM in your home folder. On **Ubuntu or Debian** it runs directly. Run it again to
update and rebuild.

Then skip to [Copy to the SD card](#copy-to-the-sd-card).

### By hand (Linux or macOS)

1. **Fetch the decompilation and SDKs and build the game:**

   ```sh
   python tools/bootstrap.py
   make game                                   # -> 3ds_port/game/build/3ds/pokeplatinum.3dsx
   make game-cia ROM=/path/to/platinum.nds     # -> .../pokeplatinum.cia (HOME Menu version)
   ```

2. **Make your data pack** from your own dump
   ([`builder/`](builder/README.md)):

   ```sh
   python builder/verify_rom.py /path/to/platinum.nds    # checks it's a supported dump
   python builder/extract_rom.py /path/to/platinum.nds   # -> data_pack/
   python builder/pack.py data_pack/                     # -> platinum3ds.pak
   ```

### Copy to the SD card

```
sdmc:/3ds/pokeplatinum/platinum3ds.pak
sdmc:/3ds/pokeplatinum/manifest.json      (from data_pack/)
sdmc:/3ds/pokeplatinum.3dsx               (for the Homebrew Launcher)
sdmc:/cia/pokeplatinum.cia                (for the HOME Menu, optional)
```

In Azahar, these go in its emulated SD card folder.

- **Homebrew Launcher:** run `pokeplatinum.3dsx`, or open it in Azahar.
- **HOME Menu:** in FBI, go to **SD → cia → pokeplatinum.cia → Install CIA**.
  The game then opens and closes from the HOME Menu like any other title. To
  update it, install the new `.cia` the same way; your save stays.

The game saves to `sdmc:/3ds/pokeplatinum/platinum.sav`, a raw 512 KB save
like emulators keep, so you can copy your cartridge's save there.

### While playing

Every button is the game's. The port writes a log to
`sdmc:/3ds/pokeplatinum/log.txt` (send it along when you report a
problem); it comes up on screen by itself if something goes wrong.

The display has settings in `sdmc:/3ds/pokeplatinum/remaster.txt`; the
defaults are what most people want. See [`docs/REMASTER.md`](docs/REMASTER.md).

Where the port's own layout for the wider screen doesn't look right, you can
draw the bands yourself with the **WideArt** editor: see
[`docs/WIDE_ART.md`](docs/WIDE_ART.md).

More detail (the PC runner, debugging crashes, audio) is in
[`3ds_port/game/README.md`](3ds_port/game/README.md).

## How it works

```
your ROM ──► builder/ ──► platinum3ds.pak (stays on your computer / SD card)

pret/pokeplatinum @ pinned commit ──► + patches/ ──► + 3ds_port/ ──► .3dsx / .cia
```

- The game's original C is compiled with GCC for the 3DS's CPU. The
  decompilation is pinned in [`upstream.lock`](upstream.lock); the port's
  changes to it live as patch files, small and easy to rebase onto a newer
  upstream.
- Nintendo's DS libraries (NitroSDK and NitroSystem) are built for the 3DS too
  ([`3ds_port/nitro`](3ds_port/nitro), [`3ds_port/nitrosystem`](3ds_port/nitrosystem)).
  The DS hardware they talk to is provided by the port: a 2D renderer, a 3D
  geometry engine that hands scenes to the 3DS's GPU, a software DS sound chip,
  and the services of the DS's second CPU (save chip, clock, power, touch
  screen).
- The game reads its files from your data pack, by their original file IDs
  ([`docs/PAK_FORMAT.md`](docs/PAK_FORMAT.md)).
- The work is spread over the New 3DS's cores: the game, the geometry engine
  and the drawing each run on their own.

## Development

```sh
python tools/bootstrap.py   # once: fetch the decompilation and SDKs, apply patches
make test                   # build for the PC and run every test suite
make game                   # the game for the 3DS
make game-cia ROM=...       # the same, as a HOME Menu .cia
```

The same game code also builds as **`pokeplatinum-pc`**, a PC runner without
a screen, for finding quickly how far the game gets and where it stops
([`3ds_port/game/README.md`](3ds_port/game/README.md)).

To build the original DS ROM from the decompilation as a sanity check, follow
pret's [INSTALL.md](https://github.com/pret/pokeplatinum/blob/main/INSTALL.md)
inside `build/upstream/`.

## Roadmap

- **Done:** the toolchain; the game's code built and running on the 3DS; 2D,
  3D, sound, input, saving; the game fully playable; the new UI for the top
  screen; the New 3DS XL's resolution; updated intro and UI animations; the
  HOME Menu version.
- **Work in progress:** 60 frames a second, better 3D quality, a better UI
  for the bottom screen, better lighting.
- **Future wishlist:** Wi-Fi, a Pokémon follower.
- **Later:** SoulSilver / HeartGold on the same backend.

## Contributing

See [`CONTRIBUTING.md`](CONTRIBUTING.md) for how changes are made and tested,
and [`AI_DISCLOSURE.md`](AI_DISCLOSURE.md) for how AI assistance is used.

## Repository layout

```
3ds_port/          3DS-side code
  game/            the game: the 3DS program, the PC runner, the display, sound, input
  nitro/           NitroSDK built with GCC, the DS hardware layer, PC unit tests
  nitrosystem/     NitroSystem's Fnd/G2d/G3d/Gfd/Snd libraries and host tests
  pak/             the data pack reader
  common.mk        shared host, host32 and devkitARM toolchain flags
builder/           turns your own ROM into the data pack (Python)
docs/              the remaster display, wide art, the data pack
patches/           changes applied on top of the decompilation and SDKs
tools/             bootstrap, build helpers, the WideArt editor (Python)
Makefile           `make game`, `make game-cia`, `make test`, ...
LICENSE            GPL-3.0, for this project's own code
upstream.lock      the exact pret/pokeplatinum commit we build against
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
