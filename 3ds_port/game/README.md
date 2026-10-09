# Game source GCC build — incremental source compilation

This is the beginning of compiling **the game's original C** for a native
3DS port. It does **not** reimplement Pokémon Platinum, package game data,
produce a playable executable, or compile all 1,013 C files yet.

The first sixteen files come directly from the pinned pret/pokeplatinum source
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
- `play_time_manager.c` — accumulation of gameplay time from timer ticks
- `poffin.c` — Poffin flavor attributes and Poffin Case storage
- `game_options.c` — saved options, button mode, battle settings and text speed
- `berry_data.c` — Berry attribute lookup and file-backed load interfaces
- `poffin_types.c` — Poffin names/message lookup interfaces

The pinned original source tree includes `"badges.h"` in trainer-case
headers, but stores the authoritative badge enum as
`generated/badges.txt` (Metang produces `generated/badges.h`). The
port-owned `include/badges.h` forwards to the generated enum rather than
inventing badge constants or modifying upstream game source. The GCC
include path is shared by host builds and devkitARM.

The Makefile uses the same shared flags, Nintendo headers, and GCC weak
NitroSystem inline-symbol fix as `libnitro` and `libnitrosystem`. It also
forces the original `pch/global_pch.h`, normally provided by Meson. Sources
remain upstream; **never copy or commit fetched game code or assets**.

The game compilation uses `-ffunction-sections -fdata-sections` and the
host-test linker uses `--gc-sections`. This permits validating standalone
functions from an otherwise incomplete game source archive without forcing
unported systems (heap, renderer, timers, game overlays) into the test binary.
**It does not imply the full game can link or run.**

The original Meson build uses `metang` to generate 107 C enum/mask headers
from the ordered upstream `generated/*.txt` manifests (including
`text_banks.h`, `abilities.h`, `species.h`, and AI/move flags).
The GCC game Makefile automatically invokes `tools/prepare_game_constants.py`,
which reads the pinned `generated/meson.build` rules and runs the **same
Metang version selected by upstream**. Headers are generated into
`build/game-constants/generated/` and included via `-Ibuild/game-constants`
and `-DPOKEPLATINUM_GENERATED_ENUM`.

The generated-header stamp depends on the upstream generator settings,
manifests, and preparation script so builds regenerate after source changes.
There are no dummy IDs, hand-copied generated files, or game assets tracked.
`tools/generate_game_text_banks.py` remains available for standalone
compatibility/testing, but no longer supplies the game's build headers.
See [game constants](../../docs/GAME_CONSTANTS.md) for more information.

The build also produces **identifier-only** message bank headers from pinned
`res/text/*.json` using `tools/prepare_game_text_bank_headers.py`. These
match the original `msgenc` ordering and entry-count macros and live under
ignored `build/game-resource-headers/res/text/bank/`. They never contain
dialogue, game assets, encryption keys, or binary messages. Four dynamic move text-bank headers are now also generated from the
original `moveproc` ordering in `generated/moves.txt` and
`res/moves/*/data.json`. The output contains only move-message ID
macros and counts, not game dialogue. Two original species-name ID
banks are now generated from `generated/species.txt` and validated
`res/pokemon/*/data.json`, producing only 496 indices per bank
(including reserved None/Egg/Bad Egg slots). The generated headers
contain no species-name strings or Pokedex descriptions. Other dynamic
text banks and the actual `pl_msg.narc` still require further integration.

The original NitroSFX `pl_sound_data.naix` header is also generated from
the pinned `res/sound/pl_sound_data.json` list. This is **symbolic sound
index metadata only**, not SDAT music/sound samples or a working sound
runtime. The output stays in `build/game-resource-headers/res/sound/`.

The NitroArc `res/graphics/**/*.order` manifests produce
**explicit graphics member indices** as `.naix` headers in the same
ignored resource-header output directory. The generator also follows the
three *exact pinned Meson* archive mappings that do not fit the default
graphics-name layout:

- `res/graphics/pokedex/pokedex.order` → `res/graphics/pokedex/zukan.naix`
- `res/trainers/classes/field_encounteffect.order` → its same-stem `.naix`
- `res/battle/scripts/subscripts/sub_seq.order` → `res/battle/scripts/sub_seq.naix`

The index generator reads only these explicit member names and ordering;
it does not generate graphics, battle scripts, or NARC archives. The
implicit (unlisted) member indices and full archive count are not inferred;
see [the compiler survey](../../docs/GAME_COMPILE_SURVEY.md).

The default Make target is explicitly `all`. CI requests `test` on
host and host32 and `lib` for 3DS; this is important because the generated
`text_banks.h` Make rule formerly became the first/default target and a
successful workflow could skip game compilation entirely.

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
test binaries. The smoke tests cover save play-time, Poffin Case, game
options, Berry attributes, authentic generated IDs, math/RNG, tiles and
field-script execution, and call real game code and an externally
linked NitroSystem function, so they test the compatibility between code and
both SDK libraries.

### Diagnose the next compilation blockers

You can survey a deterministic sample of the **remaining original** game
source tree with the same real GCC flags and generated headers:

```sh
make -C 3ds_port/game TARGET=host probe PROBE_LIMIT=80
make -C 3ds_port/game TARGET=3ds probe PROBE_LIMIT=40
```

The survey runs `-fsyntax-only` per file and categorizes failures into
missing resource/generated headers, type/symbol errors, and other causes.
It writes a JSON report under ignored `build/game-compile-survey/` and
never links or runs unported game code. CI uploads those reports.

**A green survey step means diagnostics were collected, not that every
sample compiled.** The existing library compilation and host tests remain
CI gates. For instructions and interpretation, see
[Game compile survey](../../docs/GAME_COMPILE_SURVEY.md).

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
