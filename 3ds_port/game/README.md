# Game source GCC build — first incremental slice

This is the beginning of compiling **the game's original C** for a native
3DS port. It does **not** reimplement Pokémon Platinum, package game data,
produce a playable executable, or compile all 1,013 C files yet.

The first fourteen files come directly from the pinned pret/pokeplatinum source
checkout under `build/upstream/src/`:

- `coins.c` — coin case state and overflow logic
- `charcode.c` and `charcode_util.c` — string codepoint and formatting helpers
- `ascii_util.c` — ASCII conversion and G3d resource names
- `game_version.c` — Platinum/English game-version constants
- `play_time.c` — in-save play-time rollover and saturation
- `int_distance.c` — fixed-point 2D game-world distances
- `math_util.c` and `fx_util.c` — RNG, simple encryption, fixed-point math, transforms
- `map_tile_behavior.c` — terrain types, collisions, water and encounters
- `field_script_context.c` — field script bytecode interpreter and call stack
- `game_options.c` — save-file settings, including battle, audio, UI and controls
- `poffin.c` — original Poffin creation and Poffin Case save inventory
- `play_time_manager.c` — tick-to-game-time tracking, tested with a fake clock

The Makefile uses the same shared flags, Nintendo headers, and GCC weak
NitroSystem inline-symbol fix as `libnitro` and `libnitrosystem`. It also
forces the original `pch/global_pch.h`, normally provided by Meson. Sources
remain upstream; **never copy or commit fetched game code or assets**.

The game compilation uses `-ffunction-sections -fdata-sections` and the
host-test linker uses `--gc-sections`. This permits validating standalone
functions from an otherwise incomplete game source archive without forcing
unported systems (heap, renderer, timers, game overlays) into the test binary.
**It does not imply the full game can link or run.**

The upstream Meson build generates `generated/text_banks.h` from
`generated/text_banks.txt`. The GCC game Makefile now recreates its ordered
`enum TextBank` using `tools/generate_game_text_banks.py` in the ignored
`build/<target>/gen/` directory. No dummy bank IDs or copied resources are
committed. Other Meson-generated enums and assets remain future work.

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
test binaries. The smoke tests cover save play-time, play-time manager rollover,
settings and Poffin inventory, math/RNG, tiles and field-script
execution, and call real game code and at least one externally
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
