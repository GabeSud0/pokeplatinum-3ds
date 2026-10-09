# Generating Platinum's original game constants for GCC

**Goal:** prepare the original game's generated headers before progressively
compiling the full game. This is an independent dependency step; it does
**not** mean that more game C sources, overlays, or a runnable 3DS executable
have been linked.

The pinned [pret/pokeplatinum](https://github.com/pret/pokeplatinum) project
uses Meson and Metang to transform more than a hundred
`generated/*.txt` manifests into the C headers expected by game code.
Those manifests include abilities, moves, Pokémon species, locations, flags,
battle logic, and saved variables. Some use explicit IDs or references to
other symbols; masks also have special bit and `--no-auto` behavior.

**Do not replace these with sequential enum guesses, placeholders, or copied
generated sources.** `tools/prepare_game_constants.py` reads the authoritative
`metang_generators` definitions from the pinned upstream
`generated/meson.build` and invokes the same version of Metang that its
`subprojects/metang.wrap` selects.

## Run it

From the repository root:

```sh
python tools/bootstrap.py --clean
python tools/prepare_game_constants.py
```

This fetches Metang from the exact repository/tag named in the upstream wrap,
stores it under `build/metang/`, and generates the C headers into
`build/game-constants/generated/`. Both are ignored build outputs, not files
to commit.

To generate only particular families:

```sh
python tools/prepare_game_constants.py --only species moves map_headers ai_flags
```

To use a Metang checkout you've already fetched:

```sh
python tools/prepare_game_constants.py --metang /path/to/metang.py
```

GCC-specific output correction: the wrapped Metang release formats bitmasks
with signed integer shifts. In particular its `AI_FLAG_ALL` expression
`(1 << 32) - 1` is undefined for the original 32-bit mask. The port
automatically changes generated mask operands to `1ULL << n`, retaining
exactly the same bit values (including `0xFFFFFFFF`) without modifying
Metang, the upstream manifest, or generated headers committed to Git.

Once needed in the game build, the include and preprocessor settings are:

```sh
-Ibuild/game-constants -DPOKEPLATINUM_GENERATED_ENUM
```

The game source Makefile currently generates `text_banks.h` separately using
`tools/generate_game_text_banks.py`; this PR does not rewrite that known-good
build or touch the concurrently active save/options PR. A subsequent reviewed
integration can replace the smaller custom generator with this **official**
generator and add dependencies for each required header.

## Validation

`.github/workflows/game-generated-constants.yml` separately runs:

- Offline unit tests for the literal Meson manifest parser and generator
  command selection.
- Fresh bootstrap and the original pinned Metang tool to generate all headers.
- GCC syntax checks and representative enum/bitmask identity assertions
  (including special flags and text-bank indices).
- A check that output was not inadvertently tracked in Git.

This workflow does **not** claim that every upstream game source compiles or
that a finished game can boot in Azahar. Runtime rendering, filesystem and
audio work is still outstanding.

As elsewhere in this project, do not commit ROMs, extracted assets, or
generated binaries. All work is reviewed through a PR.
