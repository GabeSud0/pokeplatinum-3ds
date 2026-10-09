# Game source GCC build — first incremental slice

This is the beginning of compiling **the game's original C** for a native
3DS port. It does **not** reimplement Pokémon Platinum, package game data,
produce a playable executable, or compile all 1,013 C files yet.

The first five files come directly from the pinned pret/pokeplatinum source
checkout under `build/upstream/src/`:

- `coins.c` — coin case state and overflow logic
- `charcode.c` and `charcode_util.c` — string codepoint and formatting helpers
- `ascii_util.c` — ASCII conversion and G3d resource names
- `game_version.c` — Platinum/English game-version constants

The Makefile uses the same shared flags, Nintendo headers, and GCC weak
NitroSystem inline-symbol fix as `libnitro` and `libnitrosystem`. It also
forces the original `pch/global_pch.h`, normally provided by Meson. Sources
remain upstream; **never copy or commit fetched game code or assets**.

## Building

From the repository root:

```sh
python tools/bootstrap.py --clean
make -C 3ds_port/game               # host compile, link, run smoke tests
make -C 3ds_port/game TARGET=host32 # 32-bit host smoke tests (gcc-multilib)
make -C 3ds_port/game TARGET=3ds    # 3DS ARM11 object library (devkitARM)
```

Outputs are ignored under `3ds_port/game/build/<target>/`:
`libplatinum_game.a` (an incomplete, relocatable static archive) and host
test binaries. The smoke tests call real game code and at least one externally
linked NitroSystem function, so they test the compatibility between code and
both SDK libraries.

### Why only a slice?

The original Metrowerks/Meson build uses generated constants, overlay
metadata, compiled resources, binary structures and a DS-specific linker
layout. Those cannot be assumed to work under GCC merely because NitroSDK
and NitroSystem compile. Grow `GAME_SRCS` in reviewable increments, address
problems through focused `patches/pokeplatinum/*.patch` files wrapped in
`SDK_PORT_3DS` when changing upstream behavior, and add targeted tests.

Full compilation, overlay linking/reset semantics, ROM data loading,
graphics, sound and startup are **not** complete. No Azahar or console test
is implied by these GCC compile checks.

See `docs/ROADMAP.md` and `docs/HARDWARE_LAYER.md`.
