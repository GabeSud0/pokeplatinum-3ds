# Game source GCC build

The game's **original C** (pinned pret/pokeplatinum, `build/upstream/src/`),
compiled with GCC for the 3DS and, for tests, the PC. It does **not**
reimplement Pokémon Platinum, package game data or produce a playable
executable.

## What gets compiled

**Every** game C file except those listed in
[`blocked_sources.txt`](blocked_sources.txt), each with the first error
that stops it:

| Target | Compiled | Blocked |
|---|---|---|
| 3DS, 32-bit PC | 984 of 1,013 | 29 |
| 64-bit PC | 960 of 1,013 | 29 + 24 in [`blocked_sources_64bit.txt`](blocked_sources_64bit.txt) |

The 24 extra files on a 64-bit PC store overlay IDs (addresses of linker
symbols) as `u32` in static initializers, which is only constant with
4-byte pointers.

What still blocks the 29: **headers the original build generates and the
port doesn't yet**: data tables from the decomp's JSON (tutor moves, egg
moves, footprints, berry list ...), sprite templates, a few archive indices
made by special scripts, and two headers that embed graphics. Plus two
decompilation slips: `<cstdlib>` (a C++ header) and a missing
`unk_02095CD4.h`.

Code that only Metrowerks accepted is fixed by patches in
`patches/pokeplatinum/` (each change under `SDK_PORT_3DS`, so the original
build is unchanged):

- `0003`: functions called without a declaration in scope. All of them
  exist in the game; the decompilation just doesn't declare them where
  they're called. Includes an include-guard clash
  (`link_contest_records.h` uses `contest.h`'s guard).
- `0004`: `static` definitions of symbols a header declares non-static.
- `0005`: assigning a `const` member, casts as assignment targets, `const`
  variables as array sizes, `NULL` for an enum, `\u00XX` byte escapes.

### Updating the lists

Fix a cause, then compile every file on both kinds of target and rewrite the
lists from the reports:

```sh
make -C 3ds_port/game TARGET=host32 probe PROBE_LIMIT=0   # needs gcc-multilib
make -C 3ds_port/game TARGET=host   probe PROBE_LIMIT=0
python tools/update_game_blocklist.py build/game-compile-survey/host32.json \
                                      build/game-compile-survey/host.json
```

### Linking the whole game

```sh
make -C 3ds_port/game TARGET=host32 link-check   # or TARGET=3ds
```

links the game from `NitroMain`, as the original program starts, and writes
`build/game-link-report/<target>.md`: every symbol the game reaches that
nothing defines, grouped by why (a blocked source file, the Wi-Fi SDKs,
overlay IDs from the DS linker script, assembly). CI uploads the 32-bit and
3DS reports. On a 64-bit PC (with the overlay-ID files blocked too) the
first run listed 556 missing symbols: about 450 in blocked files (86 in
`pokemon.c` alone), 80 from the Wi-Fi SDKs, 24 overlay IDs and one function
still in assembly (`CRYPTO_RC4Encrypt`). Nothing from NitroSDK or
NitroSystem is missing.

Compiling a file doesn't mean it runs: most of these files need the heap,
filesystem, renderer, overlays and so on, which aren't ported yet. The
library is a static archive, so programs only pull in what they use.

## How it's built

- The same shared flags and Nintendo headers as `libnitro` and
  `libnitrosystem` (`../common.mk`), plus the original
  `pch/global_pch.h`, forced as Meson does.
- `-D_NITRO -DLINK_PPWLOBBY` on every target, as NitroDWC's build passes
  them to the game in the original. NitroDWC's GameSpy headers, included by
  about 90 game files (`main.c` among them), choose their platform from
  `_NITRO`.
  [`include/extras.h`](include/extras.h) stands in for the Metrowerks C
  library header they include, and [`include/null.h`](include/null.h) for
  another one. Declarations only; networking isn't ported.
- **Compiler rules as Metrowerks had them, the same on every compiler:**
  calls without a declaration are errors (GCC 14's default; a wrong
  implicit declaration can silently break a call). Integer/pointer
  conversions and implicit `int` are warnings: Metrowerks accepted them, and
  with 32-bit pointers the result is the same.
- Include paths as in the original `meson.build`: the game's own libraries
  (`lib/*/include`), libvct, and the generated headers' folder.
- **The game's own libraries** `lib/crypto`, `lib/gds` and `lib/spl` are
  compiled into the same archive, as the original links them.
- **Header functions:** game headers (`inlines.h`, `pokemon_sprite.h`,
  NitroDWC's `dwc_friend.h` ...) define functions with plain `inline`.
  Metrowerks kept one copy; GCC exports one from every file that includes
  them, and those files then can't be linked together. The original game
  linked as one program, so any symbol defined in more than one object is
  such a header copy. After compiling, the Makefile lists those symbols
  (`build/<target>/gen/game_header_defs.syms`) and marks them weak in every
  object, as `libnitrosystem` does for NitroSystem's headers.
- Sources stay upstream; **never copy or commit fetched game code or
  assets**.

The game compilation uses `-ffunction-sections -fdata-sections` and the
host-test linker uses `--gc-sections`. This permits validating standalone
functions from an otherwise incomplete game source archive without forcing
unported systems (heap, renderer, timers, game overlays) into the test binary.
For example, the Berry Tag, Ribbon and Pokédex state smoke tests operate on
caller-owned structs or static metadata; their real heap/save-table/NARC
interfaces are **not** claimed to be runtime-functional. The Move Reminder,
Pokédex measurement and Day Care smoke checks similarly use the original
standalone functions with caller-owned input. Their full Pokémon, NARC,
BoxPokemon initialization and heap backends are not implemented yet.
The Easy Chat tests use the original sentence structures and source-derived
text-bank **indices**, not the message content. Expanding sentences or
counting substitution slots still needs the original message NARC and
string/template runtime. The string-list test uses caller-owned entries;
heap allocation, cloning and message loading are not claimed as supported.
These modules do not require Nintendo Wi-Fi headers to compile.
Battle Factory and Battle Arcade, however, include `struct_defs/battle_frontier.h`
which includes `wifi_list.h` for `DWCUserData` and `DWCFriendData`. For the
3DS port the source patch `patches/pokeplatinum/0002-*.patch` switches just
that save header to the **original pinned NitroDWC** `base/dwc_account.h`,
rather than the entire GameSpy-dependent `dwc.h`. The non-port DS build
keeps including `dwc.h`. The game Makefile fetches both SDK header trees
before compiling, not only before compiler surveys. This is limited to
data-structure compatibility: no NitroDWC objects, GameSpy transport,
Nintendo WFC, online play or actual save file writing is implemented. Likewise,
battle-rule and record-mixing tests operate on original tables and
caller-owned arrays; they do not implement Nintendo Wi-Fi/record exchange.
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
options, Berry attributes, authentic generated IDs, math/RNG, tiles,
field-script execution, menu cursor navigation, map history rollover,
Pokédex language ordering, persisted map-feature buffers, Berry Tag bits,
Pokédex selection/data banks, Ribbon initialization, battle rule presets,
mixed-record RNG initialization/entry matching, move-list sentinel detection,
Pokédex size/weight accessors, Day Care state transitions, Easy Chat
sentence operations, string-list entry insertion, and Battle Factory /
Arcade saved-state setters/getters and streak-bit updates. They call
real game code and an externally linked NitroSystem function, so they
test compatibility between original game code and both SDK libraries.

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

### What isn't done

Overlay linking and reset, ROM data loading, graphics, sound and startup are
**not** done. No Azahar or console test is implied by these compile checks.

See `docs/ROADMAP.md` and `docs/HARDWARE_LAYER.md`.
