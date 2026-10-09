# Roadmap

Each item below is written to be pasted into a GitHub issue. Suggested labels:
`phase-0` … `phase-5`, plus `c`, `python`, `docs`, `hardware-test`.

Target hardware: New 3DS family first. Old 3DS support is a "maybe later".

---

## Phase 0: Toolchain

**0.1 Install devkitPro and build hello3d** (`c`) ✅ done
Install devkitPro with `3ds-dev`, run `make` in `3ds_port/hello3d`.
Done when: `hello3d.3dsx` builds locally and in GitHub Actions.

**0.2 Run hello3d on Azahar** (`hardware-test`) ✅ done, stereo confirmed
Done when: the cube spins and the bottom screen shows frame times.

**0.3 Run hello3d on the New 3DS XL via 3dslink** (`hardware-test`) ⏸ deferred
Development is emulator-first for now; this becomes the baseline before Phase 4.
Done when: the cube spins at a stable 60 fps and gains depth with the 3D slider.
Record the CPU/GPU ms numbers in the issue as a baseline.

---

## Phase 1: Decompilation

**1.1 Bootstrap pokeplatinum** (`python`) ✅ done
`python tools/bootstrap.py` fetches the pinned commit and applies patches.

**1.2 Build the original DS ROM from the decomp** (`hardware-test`)
Follow pret's INSTALL.md inside `build/upstream/`. Done when the built ROM's
SHA-1 matches your dump (`builder/verify_rom.py`).

**1.3 Map the DS hardware layer** (`docs`) ✅ done
See [`HARDWARE_LAYER.md`](HARDWARE_LAYER.md). Its "Suggested order of work"
refines Phases 2 and 3 below.

**1.4 Study the SDL3 port of pokeplatinum** (`docs`) ✅ done
<https://github.com/arcanite24/pokeplatinum-portable> moved from porting the
decomp to writing a new engine. Lessons are in `HARDWARE_LAYER.md`.

---

## Phase 2: Platform layer

**2.1 Host (PC) build of game logic** (`c`, `python`)
Compile the non-hardware parts of the game for PC so logic and data tools can
be tested without a 3DS.

**2.2 Hardware abstraction interface** (`c`)
Define the small set of functions the game needs (draw backgrounds, draw
sprites, submit 3D geometry, play sound, read input, read/write save) so a DS
backend and a 3DS backend can sit behind the same interface.

---

## Phase 3: 3DS backend

**3.1 Data builder** (`python`)
Extract the files the game needs from the player's own ROM into a data pack on
the SD card. Nothing extracted is ever committed.

**3.2 2D: backgrounds and sprites with Citro2D/Citro3D** (`c`)
**3.3 3D: translate DS geometry to Citro3D** (`c`)
**3.4 Audio: SDAT playback through NDSP** (`c`)
**3.5 Input, touch and save data** (`c`)
**3.6 First boot: title screen on the 3DS** (`hardware-test`) ← big milestone
**3.7 Playable: start a new game and walk around Twinleaf Town** (`hardware-test`)

---

## Phase 4: The upgrade

**4.1 Render the 3D at 400×240 widescreen** (`c`)
Widen the camera's field of view and fill the top screen instead of a 256×192
box.

**4.2 Stereoscopic 3D** (`c`, `hardware-test`)
Per-eye projection driven by the 3D slider, tuned on real hardware.

**4.3 Bottom-screen touch UI** (`c`)

**4.4 Performance pass on New 3DS** (`hardware-test`)
Steady 60 fps (or a locked 30) with stereo on, measured on the console.

**4.5 Optional: 800×240 high-density mode** (`c`)
Sharper image when the 3D slider is off.

---

## Phase 5: SoulSilver

**5.1 Track pokeheartgold decompilation progress** (`docs`)
**5.2 Port the backend to pokeheartgold** (`c`)
**5.3 HGSS-specific features** (Pokéwalker data, following Pokémon rendering)
