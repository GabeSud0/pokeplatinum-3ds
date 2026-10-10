# The game

The game's **original C** (pinned pret/pokeplatinum, `build/upstream/src/`),
compiled with GCC, and the programs that run it:

- **`pokeplatinum.3dsx`**: the game on a 3DS (or in Azahar).
- **`pokeplatinum-pc`**: the same game code on a PC, without a screen, for
  finding out quickly how far it gets and where it stops.

Both read the game's files from **your own data pack**, made from your own
cartridge by the builder. Nothing from the game is in this repository, and
programs built from it are for your own use: never share them.

**Where it stands:** the game starts. `NitroMain` runs with the SDK's
threads, interrupts, timers and overlays working, reads its files from the
data pack, and gets answers from the ARM7's services (power, clock, save
chip, touch screen). Nothing is drawn yet: the renderer and sound are next.
The 3DS program shows its log instead of the game's screens. How far the
game gets with a real data pack hasn't been tried yet; without one it goes
as far as its first file (the font).

## Running it on a 3DS

1. **Build the program** (devkitPro with `3ds-dev`, Python 3, Git):

   ```sh
   python tools/bootstrap.py
   make game        # -> 3ds_port/game/build/3ds/pokeplatinum.3dsx
   ```

2. **Make your data pack** from your own Pokémon Platinum (USA) dump, on
   your computer ([`builder/`](../../builder)):

   ```sh
   python builder/extract_rom.py /path/to/platinum.nds      # -> data_pack/
   python builder/pack.py data_pack/ -o platinum3ds.pak
   ```

3. **Copy to the SD card** (Azahar: its emulated SD card folder):

   ```
   sdmc:/3ds/pokeplatinum/platinum3ds.pak
   sdmc:/3ds/pokeplatinum/manifest.json        (from data_pack/)
   sdmc:/3ds/pokeplatinum.3dsx                 (anywhere the Homebrew Launcher looks)
   ```

   The game saves to `sdmc:/3ds/pokeplatinum/platinum.sav`, made on the
   first run. It's a raw 512 KB save, the same as emulators keep: you can
   copy your cartridge's save there (or take it out to an emulator).

4. **Run it** from the Homebrew Launcher, or open the `.3dsx` in Azahar.

What you'll see: the top screen shows the program's status, the bottom
screen the log, which is also written to `sdmc:/3ds/pokeplatinum/log.txt`.
When the game stops at something the port doesn't do yet, the log says
what; after 5 seconds without a frame the program says the game is stuck,
and HOME → Close quits. If anything crashes, the log has the crash address:
look it up in `build/3ds/pokeplatinum.elf` with
`arm-none-eabi-addr2line -e pokeplatinum.elf <address>`. Failed `GF_ASSERT`s
in the game are logged with the caller's address the same way.

The program uses the console's profile for the DS user settings the game
reads (name, birthday, language).

## Running it on a PC

```sh
make -C 3ds_port/game TARGET=host32 program       # 32-bit, as on the 3DS (gcc-multilib)
3ds_port/game/build/host32/pokeplatinum-pc platinum3ds.pak data_pack/manifest.json
```

Linux (or WSL) with GCC. Everything the port logs goes to stderr with the
frame number; a crash prints a backtrace. The save is `platinum.sav` next
to the data pack, or `--save FILE`. `--frames N` stops after N
frames; `--stop-at-missing-file` stops at the first file the pack doesn't
have. `TARGET=host` (64-bit) works too, but Nintendo's graphics formats
hold 4-byte pointers, so the game can't get far that way once it loads
graphics.

Without a data pack, two checks run in CI:

- `pokeplatinum-pc --check-overlays` loads every overlay with the game's own
  overlay manager and checks that each load resets the overlay's data.
- `python tools/boot_smoke.py <pokeplatinum-pc>` starts the game with a
  made-up pack and reports where it stops.

## How the program runs the game

`app/platinum.c` does what the DS's firmware and cartridge loader did before
`NitroMain`, then calls it; `app/main_3ds.c` and `app/main_pc.c` are the
machine-specific parts. The rest is in libnitro ([`../nitro`](../nitro)):

- **Firmware state.** The cartridge header (the game checks its maker code
  at boot), the user's settings, a MAC address (fixed, so a save stays "from
  this DS" on every console) and the button/lid state go into the port's
  DS memory, where the game and the SDK read them.
- **Threads.** NitroSDK schedules its threads itself (the game's main
  thread, the card library's, the idle thread ...). Each one is played by a
  3DS thread, and only one runs at a time, handed over exactly where the
  SDK switches: so the SDK's scheduling is unchanged, priorities and all.
- **Frames.** When every thread waits, the SDK's idle thread halts; the
  port shows the frame, waits for the 3DS's V-blank, reads the buttons and
  touch screen, and raises the DS's V-blank interrupt, which wakes the
  game's main loop.
- **Timers.** The DS's four hardware timers run from the 3DS's clock: the
  SDK's tick, its alarms and the game's play-time clock work.
- **Overlays.** On the DS, much of the game is in overlays, loaded from the
  cartridge when needed, each load starting the overlay's data afresh. Here
  all of them are linked into the program, and loading one resets its data
  in the same way: the build gathers each overlay's data into sections of
  its own (`tools/game_overlays.py`), and loading copies back the initial
  values and clears the rest. The game's own overlay manager does the rest.
- **Files** come from the data pack (`NitroPort_OpenPak`); a file the game
  asks for that the pack doesn't have is logged.
- **The ARM7.** The DS's second CPU answered the game's requests for the
  save chip, clock, touch screen and power. The port answers them
  ([`nitro/src/port_arm7.c`](../nitro/src/port_arm7.c) and its neighbours),
  through the FIFO-receive interrupt as on the DS: the save chip is the save
  file, the clock is the console's, touch comes from the 3DS's bottom
  screen, and power management keeps track of the backlights.

## What gets compiled

**Every** game C file except those listed in
[`blocked_sources.txt`](blocked_sources.txt), each with the first error
that stops it:

| Target | Compiled | Blocked |
|---|---|---|
| 3DS, 32-bit PC, 64-bit PC | **all 1,012** | none |

1,012 is the original build's own list (`src/meson.build`;
`tools/game_sources.py`). `src/unk_02095CD4.c` isn't on it: it includes a
header that doesn't exist and isn't part of the game. The blocklists
([`blocked_sources.txt`](blocked_sources.txt), and
[`blocked_sources_64bit.txt`](blocked_sources_64bit.txt) for 64-bit PCs)
are empty but stay, for whatever the next pinned decompilation brings.

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
- `0006`: C versions of the RC4 encrypt functions (see below).
- `0007`: `<cstdlib>` (a C++ header), two more static-after-extern
  definitions, a missing include.
- `0008`: three tables of function addresses as `uintptr_t` (the size of
  `u32` on the DS and 3DS), so they're constant on a 64-bit PC too.

**Compiler rules as Metrowerks had them, the same on every compiler:**
calls without a declaration are errors (GCC 14's default; a wrong implicit
declaration can silently break a call); integer/pointer conversions and
implicit `int` are warnings. Signed overflow wraps and GCC doesn't drop
loop iterations or null checks it could prove "can't matter"
(`-fwrapv -fno-aggressive-loop-optimizations -fno-delete-null-pointer-checks`,
in `../common.mk`): Metrowerks made no such assumptions, and the game has
places that would be affected.

### Generated headers

Everything the original build generates and the game's code includes is
generated the same way, into `build/` (never committed):

- Metang's constants (`tools/prepare_game_constants.py`), text-bank,
  move, species and sound indices, and archive indices (`.naix`) from the
  `.order` lists and `meson.build` rules (`prepare_game_*`).
- Archives whose member list a script makes: `tools/ordergen/` (trainer
  sprites, footprints) and `tools/scripts/` (Pokémon icons, alternate-form
  sprites) (`tools/prepare_game_ordergen_naix.py`).
- **The decompilation's own data tools** (`tools/prepare_game_dataproc.py`):
  it builds `tools/dataproc` (speciesproc, itemproc, frontierproc,
  trainerproc, resdatproc) with the PC's C compiler, as their `meson.build`
  files say, and runs them as the original rules do: egg/tutor moves,
  footprints, the berry list, frontier trainer classes, sprite templates.
  The two images the game embeds in its code are converted by
  `tools/png_embed.py` (what `nitrogfx -embed` does, in Python, so no image
  library is needed). Needs a PC C compiler. The data archives these tools also write are
  deleted; the game reads its data from the player's own data pack.
- **Overlay IDs** (`FS_OVERLAY_ID(name)`): each overlay's position in
  `platinum.us/main.lsf`, as constants in `build/game-link/overlay_ids.h`,
  which every game file is compiled with (`tools/prepare_game_overlay_ids.py`;
  NitroSDK patch `0014`). In NitroSDK an overlay ID is the address of a
  symbol Nintendo's linker placed at that number, which doesn't fit the
  game's 32-bit overlay-ID fields on a 64-bit PC (and 3dsxtool refuses
  addresses that low).
- Like Meson, the folders of generated headers are on the include path
  (some files include `"ending.naix"` by bare name).

### Updating the lists

Fix a cause, then compile every file on both kinds of target and rewrite the
lists from the reports:

```sh
make -C 3ds_port/game TARGET=host32 probe PROBE_LIMIT=0   # needs gcc-multilib
make -C 3ds_port/game TARGET=host   probe PROBE_LIMIT=0
python tools/update_game_blocklist.py build/game-compile-survey/host32.json \
                                      build/game-compile-survey/host.json
```

## How it's built

```sh
make -C 3ds_port/game                       # compile for the PC, run the tests
make -C 3ds_port/game TARGET=host32         # the same, 32-bit (gcc-multilib)
make -C 3ds_port/game TARGET=3ds            # compile for the 3DS (devkitARM)
make -C 3ds_port/game TARGET=3ds program    # pokeplatinum.3dsx
make -C 3ds_port/game TARGET=host program   # pokeplatinum-pc
make -C 3ds_port/game TARGET=3ds link-check # link, reporting what's missing
```

- The same shared flags and Nintendo headers as `libnitro` and
  `libnitrosystem` (`../common.mk`), plus the original
  `pch/global_pch.h`, forced as Meson does.
- `-D_NITRO -DLINK_PPWLOBBY` on every target, as NitroDWC's build passes
  them to the game in the original. NitroDWC's GameSpy headers, included by
  about 90 game files (`main.c` among them), choose their platform from
  `_NITRO`. [`include/extras.h`](include/extras.h) stands in for the
  Metrowerks C library header they include, and
  [`include/null.h`](include/null.h) for another one. Declarations only;
  networking isn't ported.
- Include paths as in the original `meson.build`: the game's own libraries
  (`lib/*/include`), libvct, and the generated headers' folder.
- **The game's own libraries** `lib/crypto`, `lib/gds` and `lib/spl` are
  compiled with the game, as the original links them.
- **Header functions:** game headers (`inlines.h`, `pokemon_sprite.h`,
  NitroDWC's `dwc_friend.h` ...) define functions with plain `inline`.
  Metrowerks kept one copy; GCC exports one from every file that includes
  them, and those files then can't be linked together. The original game
  linked as one program, so any symbol defined in more than one object is
  such a header copy. After compiling, the Makefile lists those symbols
  (`build/<target>/gen/game_header_defs.syms`) and marks them weak in every
  object, as `libnitrosystem` does for NitroSystem's headers.
- **Overlays' data:** the same step renames each overlay's objects' data
  sections to `ovl_data_<overlay>` / `ovl_bss_<overlay>` and writes the
  overlay table (`build/<target>/gen/overlay_table.c`), which the programs
  link after the game's objects (`tools/game_overlays.py`; CI checks where
  the sections land in the 3DS program with `tools/check_program_elf.py`).
- **RC4** (Mystery Gift): `lib/crypto` keeps its encrypt functions in
  assembly; patch `0006` adds C versions, checked against the published
  test vectors.
- **Nintendo's Wi-Fi libraries** (DWC, PPW lobby, VCT voice chat, WVR):
  online play isn't part of the port. After compiling,
  `tools/make_game_wifi_stubs.py` writes a stand-in for every such function
  the game objects call: each logs "not implemented" once and returns 0, so
  online menus see a failure.
- Sources stay upstream; **never copy or commit fetched game code or
  assets**.

The programs link every game object; the library
(`build/<target>/libplatinum_game.a`) is for the tests and the smoke app.

### The tests

`make` runs `tests/test_game.c` (585 checks): original game functions
linked against NitroSDK and NitroSystem, without any game data: save
play-time, Poffin Case, game options, Berry attributes, generated IDs,
math/RNG, tiles, field-script execution, menu cursor navigation, map
history, Pokédex ordering, map-feature buffers, Berry Tag bits, Ribbon
initialization, battle rule presets, record mixing, move lists, Day Care
transitions, Easy Chat sentences, string lists, Battle Factory / Arcade
saved state, RC4, overlay IDs. They test that the game's code and both
SDK libraries work together; tests that need a module's data (NARCs,
messages) or heap aren't claimed by them.

### Diagnose compilation blockers

```sh
make -C 3ds_port/game TARGET=host probe PROBE_LIMIT=80
make -C 3ds_port/game TARGET=3ds probe PROBE_LIMIT=40
```

runs `-fsyntax-only` per file and writes a JSON report under ignored
`build/game-compile-survey/` ([Game compile survey](../../docs/GAME_COMPILE_SURVEY.md)).

## What isn't done

- **Graphics**: the DS's 2D engines and 3D, drawn on the 3DS's screens.
- **Sound**, and the ARM7's other services (microphone, wireless).
- **Restarting** (`OS_ResetSystem`, the soft reset): the program says so and
  stops. **Sleep mode** (closing the lid) and **H-blank effects** too.
- **Online play** (Nintendo Wi-Fi Connection): not part of the port.

See `docs/ROADMAP.md` and `docs/HARDWARE_LAYER.md`.
