# The DS hardware layer of Pokémon Platinum

This is the map for porting the game to the 3DS: every place where the game
depends on Nintendo DS hardware, how much code touches it, and what each piece
becomes on the 3DS.

Surveyed against pret/pokeplatinum at the commit in [`upstream.lock`](../upstream.lock).
The usage numbers come from `tools/survey_sdk_usage.py`, so they can be
re-checked after any upstream update:

```sh
python tools/bootstrap.py
python tools/survey_sdk_usage.py --files
```

---

## The short version

1. **The whole software stack is C.** The game (1,013 C files, ~720,000
   lines, plus one assembly file holding the trainer AI scripts), and the two
   Nintendo libraries it is built on, NitroSDK and NitroSystem, are all
   decompiled to C (in the [ntrtwl](https://github.com/ntrtwl) repositories that
   pokeplatinum pulls in as Meson subprojects). Even the sound sequencer, which
   ran on the DS's second CPU (the ARM7), is C.
2. **So we don't have to rewrite the game, or even most of the libraries.**
   Almost everything can be compiled for the 3DS's ARM11 as it is. What has to
   be replaced is the thin bottom layer where the libraries poke the DS's
   hardware registers and video memory.
3. **The game rarely touches hardware directly.** Only 5 game files use raw
   register names. Everything else goes through the SDK, and several
   subsystems are funnelled through a handful of files (input: 1, touch: 2,
   saves: 4, sound: 4).
4. **Graphics is the big job.** 2D display features are used in hundreds of
   files, and the 3D engine in roughly 90. That's where most of the porting
   effort and all of the visual upgrades (resolution, widescreen, stereo) live.

## How the game is layered

```
 ┌─────────────────────────────────────────────────────────────┐
 │ Game code (src/)                                            │  compile as-is
 │   field, battle, menus, Pokétch, cutscenes ... 122 overlays │
 ├─────────────────────────────────────────────────────────────┤
 │ Game's own wrappers                                         │  compile as-is,
 │   bg_window.c, sprite_system.c, gx_layers.c, g3d_pipeline.c │  small patches
 │   sound*.c, savedata.c, system.c, heap.c, narc.c ...        │
 ├─────────────────────────────────────────────────────────────┤
 │ NitroSystem (NNS_*): G3d models, G2d sprites, Snd, Fnd heaps│  compile as-is
 ├─────────────────────────────────────────────────────────────┤
 │ NitroSDK: GX/G2/G3 display, OS, FS, MI, SND, CARD, PAD ...  │  partly replace
 ├─────────────────────────────────────────────────────────────┤
 │ DS hardware: I/O registers at 0x04000000, VRAM banks,       │  ◄── this is what
 │ 2D engines A/B, 3D geometry engine, ARM7 sound, flash save  │      we rebuild
 └─────────────────────────────────────────────────────────────┘
```

## How much code depends on what

From `tools/survey_sdk_usage.py`. "Names" counts functions, macros and
constants (for example `GX_RGB` and `PAD_BUTTON_A` count), so these are upper
bounds on how much code is affected. Of 1,013 C files, 713 use at least one SDK
name; 1,320 distinct names are used in total.

| Module | Files | What it is | Porting difficulty |
|---|---:|---|---|
| `FX32_`, `FX_`, `FX16_`, `VEC_`, `MTX_`, `MATH_` | 333 / 93 / 6 / 28 / 28 / 36 | Fixed-point and vector math | **None.** Pure software, compiles as-is. |
| `MI_` | 308 | Memory copy and fill (`MI_CpuCopy8` …), DMA | **Easy.** CPU versions are plain C; DMA becomes `memcpy` plus side effects (see VRAM). |
| `GX_` | 324 | VRAM bank setup, palettes, master brightness, display mode, `GX_RGB` colors | **Hard.** Core of the display. |
| `G2_` / `G2S_` | 144 / 69 | 2D engines A and B: blending, windows, BG priorities, mosaic | **Hard.** Part of the 2D renderer. |
| `GXS_` | 132 | Engine B uploads, visible layers | **Hard.** Part of the 2D renderer. |
| `NNS_G2d` | 113 | Sprite cells, animations, OAM manager, screen data | **Easy.** Library is C; sits on top of OAM/VRAM. |
| `PAD_` | 150 | Mostly button constants; actually *read* in `system.c`, `main.c` (and a lid-fold check in a few files) | **Easy.** |
| `OS_` | 104 | Interrupts, V-blank waits, reset, ticks, threads | **Medium.** Main loop timing and V-blank callbacks. |
| `NNS_G3d` | 84 | 3D models (NSBMD), animations, materials, lights | **Medium.** Library is C; its output goes to the geometry engine. |
| `G3_` / `G3X_` / `G3B_` | 44 / 49 / 1 | Geometry engine commands; fog, edge marking, alpha test, clear color | **Hard.** Becomes the Citro3D translator. |
| `NNS_Gfd` | 48 | Texture and palette VRAM allocators | **Easy.** Library is C. |
| `FS_` | 77 | Files on the cartridge, NARC archives, overlays | **Medium.** |
| `DC_` | 79 | Data cache flush/invalidate before DMA | **None.** No-op (or a GSP flush). |
| `CP_` | 14 | Hardware divider/square-root unit | **Easy.** Replace with C division. |
| `NNS_Snd` / `SND_` | 4 / 1 | Sound archive (SDAT) player, sequencer | **Medium.** See Sound. |
| `CARD_` | 4 | Save flash | **Easy.** File on SD. |
| `TP_` | 2 | Touch screen | **Easy.** |
| `RTC_` | 11 | Clock (Pokétch, berries, day/night) | **Easy.** |
| `PM_`, `MIC_`, `SVC_` | 7 / 6 / 3 | Backlight, speaker amp, microphone, BIOS CRC/sqrt | **Easy.** Stub or tiny C. |
| `WM_`, `DWC_`, `WCM_` | 16 / 37 / 2 | Local wireless and Nintendo Wi-Fi Connection | **Out of scope.** Stub out. |

Direct register access in game code is limited to `src/timer.c`,
`src/billboard.c`, `src/applications/pc_boxes/ov19_021D79F8.c` and two files in
`src/overlay100/`.

---

## Subsystem by subsystem

### 1. Main loop and frame timing

**Where:** `src/main.c` (`NitroMain`), `src/system.c`.

The main loop reads input, runs the current "application" (title screen, field,
battle …), runs task lists, then waits for V-blank with
`OS_WaitIrq(TRUE, OS_IE_V_BLANK)`, then runs the registered V-blank callback,
sound tick and post-V-blank tasks. Game logic normally runs once every two
V-blanks, so 30 updates per second; `gSystem.frameCounter`, bumped by the
V-blank interrupt in `system.c`, lets a frame that ran long skip the extra wait.

**On 3DS:** keep the loop as it is. `OS_WaitIrq` for V-blank becomes "finish the
frame and wait for the 3DS's V-blank" (`gspWaitForVBlank`), the V-blank
interrupt's counter is updated from there, and `aptMainLoop()` gets checked
there so the HOME button works. After the V-blank work (screen fades, the
V-blank callback, the post-V-blank tasks) the loop calls
`NitroPort_VBlankWorkDone` (`patches/pokeplatinum/0011`): the display state is
then what the DS shows that frame, and the screens are drawn from it on other
cores. Before the next V-blank's work the game waits for any of them still
reading it, so a screen never shows that work half done.

### 2. Memory, heaps and overlays

**Where:** `src/heap.c` (NitroSystem expanded heaps), `src/game_overlay.c`,
`src/overlay_manager.c`.

The DS has 4 MB of main RAM, so the game is split into **122 overlays**: chunks
of code loaded into the same addresses one at a time (the field, each battle
module, each menu …). `Overlay_LoadByID` reads one from the cartridge.

**On 3DS:** the New 3DS has far more memory, so we link **all overlays into one
program** and `Overlay_LoadByID` stops loading code. One trap: on the DS, an
overlay's global variables start fresh every time it is loaded. Linked
permanently, they would keep old values. So each overlay's data and BSS
sections need to be reset on "load". That needs each overlay's sections kept
separate, plus a small table of start/end addresses. *Done:* the build renames
each overlay's data sections after it (`tools/game_overlays.py`), the linker
gathers them, and loading an overlay restores its data
(`3ds_port/nitro/src/port_overlay.c`).

Heaps can stay as they are, allocated from a larger block.

### 3. Files and NARC archives

**Where:** `FS_*` calls in 77 files; archive handling in `src/narc.c`.

Game data (graphics, maps, text, Pokémon data, sound) lives in the cartridge's
filesystem, mostly inside NARC archives.

**On 3DS:** this is where the builder comes in. It extracts the filesystem from
the player's own ROM into a data pack on the SD card, and `FS_OpenFile` /
`FS_ReadFile` are reimplemented on top of normal file reads (or a single pack
file, for speed). The game keeps using file IDs and NARC member indexes
unchanged. Later the builder can also pre-convert textures into 3DS formats so
the console doesn't have to at runtime.

### 4. 2D graphics: the two display engines

**Where:** `src/gx_layers.c` (VRAM bank layout and visible layers),
`src/bg_window.c` (backgrounds, tilemaps, windows: the heaviest user of `G2_`),
`src/render_oam.c`, `src/sprite.c`, `src/sprite_system.c`, `src/palette.c`,
`src/graphics.c`; plus effects code in many scenes (encounter transitions,
cutscenes) that sets blending, windows and mosaic directly.

The DS has two 2D engines, **A** (main) and **B** (sub), one per screen. Each
draws up to 4 background layers plus 128 hardware sprites, with per-layer
priority, alpha blending, brightness fades, windows (masked regions) and mosaic.
The game writes tile graphics, tilemaps and palettes into VRAM banks, and sprite
attributes into OAM. On Platinum, the 3D engine's output appears as engine A's
background layer 0.

**On 3DS, recommended approach:** keep all of this exactly as the game expects,
in memory:

- a **virtual register block** standing in for the DS's I/O area at
  `0x04000000` (NitroSDK reaches every register through `HW_REG_BASE`, so this
  is largely a header change),
- **virtual VRAM banks** and OAM as plain arrays, with the bank mapping that
  `GX_SetBankFor*` asks for,
- a **2D renderer** that, once per frame, reads that state and draws each
  layer: decode the tiles to textures, then let Citro3D composite layers,
  sprites, blending and windows on the GPU.

This is the same idea the Emerald 3DS port uses for the GBA's display, and it
means 2D game code never needs editing. The DS 2D image is 256×192; on the 3DS
it is drawn pixel-exact and centred, or scaled. The 2D layers stay at their
original resolution: there is nothing more to gain there.

**Which screen is which:** `GXLayers_SwapDisplay` / `GX_SetDispSelect` decide
whether engine A shows on the top or bottom screen. The 3DS top screen
(400×240) shows whichever engine is "top", and the bottom (320×240) shows the
other.

### 5. 3D graphics: where the upgrade happens

**Where:** `NNS_G3d` in 84 files (heaviest: `src/easy3d.c`, `src/billboard.c`,
`src/overlay005/model_attributes.c` for the overworld, `src/choose_starter/`,
cutscenes), `G3X_` global settings in 49 files, `src/g3d_pipeline.c` (the
game's 3D setup), `src/camera.c`, and `NNS_Gfd` for texture memory.

On the DS, NitroSystem's G3d library walks a model, computes matrices, and
sends a stream of **geometry engine commands** (matrix ops, vertices, colors,
texture coordinates, polygon attributes) into the command FIFO
(`reg_G3X_GXFIFO`, via `src/gecom.c` in NitroSystem). The geometry engine
transforms and rasterises at 256×192.

**On 3DS:** compile G3d as-is, and replace the hardware at the command stream:

- capture every command written to the FIFO and the direct `G3_*` command
  registers, and run a **software geometry-command interpreter**: keep the DS
  matrix stacks, build vertex lists, and issue them to Citro3D as triangle
  batches at **any resolution we like**,
- convert DS texture formats when textures are loaded. Paletted formats (A3I5,
  A5I3, 2/4/8-bit) and the DS's 4×4 compressed format have no PICA200
  equivalent, so they become RGBA textures (cached, and later pre-converted by
  the builder),
- recreate the DS 3D features the game turns on: fog, edge marking (outlines),
  toon/highlight shading, alpha test, anti-aliasing, translucent polygon IDs.
  Some map directly to PICA200 features, some need a shader or are approximated.

This is where the project's goals are met:

- **Resolution:** the 3D scene is rendered at 400×240 (or 800×240) instead of
  256×192.
- **Widescreen:** `src/camera.c` sets the field of view; widening it fills the
  whole top screen. The overworld then shows more of the map at the sides, so
  the game must load or draw a little more of it (map culling may need
  adjustment).
- **Stereo:** because we issue the draw calls ourselves, we render the 3D
  scene twice with per-eye projections, exactly as `hello3d` does. 2D layers
  are placed at screen depth (or at their own depth for a nice effect).

The 3D-to-2D link matters: on the DS, the 3D image is blended as a 2D layer
(BG0) with the other layers. Our compositor keeps that ordering, but draws the
3D layer at high resolution.

### 6. Sound

**Where:** `src/sound_system.c`, `src/sound.c`, `src/sound_playback.c` (the only
users of `NNS_Snd`, apart from Wi-Fi voice chat), `src/sound_chatot.c`
(recorded Chatot cry),
`src/scrcmd_sound.c`, `src/field_bgm.c`.

On the DS, the ARM9 game side sends commands; the **ARM7** runs the sequencer
(SSEQ music, SBNK instruments, SWAR samples) and drives 16 hardware sound
channels. The ARM7 sequencer is part of the decompiled NitroSDK
(`libraries/snd/src/ARM7/`: sequencer, channels, MIDI player, capture).

**On 3DS:** compile the ARM7 sequencer for the ARM11 too, run it on its own
thread, and replace its 16 hardware channels with a software mixer that outputs
to the 3DS's DSP (NDSP). The command link between the two CPUs (PXI) becomes a
simple queue.

### 7. Input, touch and the lid

**Where:** `src/system.c` (`ReadKeypadAndTouchpad`, the only `PAD_Read`),
`src/touch_pad.c`, `src/touch_screen.c`; `PAD_DetectFold` in `main.c` and a few
others.

**On 3DS:** `hidScanInput` / `hidKeysHeld` mapped to DS buttons, and
`hidTouchRead` for the touch screen (the 3DS bottom screen is 320 wide, the DS
screen 256, so touch coordinates are offset or scaled to match where the DS image
is drawn). The lid check can use the 3DS's own lid state, or simply report
"open".

### 8. Saves

**Where:** `src/savedata.c` (plus `src/boot.c`, `src/main.c`, `src/system.c` for
setup); `CARD_ReadFlash`, `CARD_WriteAndVerifyFlashAsync` …

**On 3DS:** read and write a 512 KB file on the SD card in the same layout as
the cartridge flash. That keeps saves compatible with the original game and
with DS save managers, which players will appreciate.

### 9. Clock

**Where:** `RTC_*` in 11 files (Pokétch apps, day/night, berries).

**On 3DS:** answer from the 3DS system clock.

### 10. Wireless and Wi-Fi

**Where:** `WM_*` (local wireless: Union Room, contests, Underground) and
`DWC_*` (Nintendo Wi-Fi Connection: GTS, Wi-Fi battles) in ~50 files.

**On 3DS:** out of scope. Nintendo WFC has been offline since 2014, and the
3DS's wireless can't talk to DS local wireless from native homebrew. These
calls are stubbed to report "no connection", which the game already handles
because players can always be out of range.

---

## Lessons from the SDL3 port of Platinum

[pokeplatinum-portable](https://github.com/arcanite24/pokeplatinum-portable)
first tried to port the decompilation by stubbing the SDK, then gave up on that
and switched to writing a new engine that uses the decomp only as a reference.
Its plan names the problems: builds broke constantly, type dependencies
cascaded, networking stubs were impractical, and there was no visual feedback
while stubbing. Its 3D and audio are still stubs.

What we do differently:

- **We don't stub the SDK, we rebuild the hardware under it.** NitroSDK and
  NitroSystem keep their real code and headers, so their types and behaviour
  stay intact and the game compiles against what it was written for.
- **Visual feedback first.** The first goal of Phase 3 is the 2D renderer
  showing the title screen. Everything is built to be seen working in Azahar
  as early as possible.
- **One subsystem at a time, always building.** Each pull request keeps CI
  green.

---

## Suggested order of work

This refines Phases 2 and 3 of the [roadmap](ROADMAP.md):

1. **Get the game, NitroSDK and NitroSystem compiling for the 3DS**, with
   hardware functions as loud stubs ("not implemented: G2_SetBlendAlpha"). No
   graphics yet; the success test is reaching the main loop.
   *NitroSDK done:* the game-used SDK function definitions build for the
   3DS, with the memory map, CPU state, interrupts, BIOS, decompression
   and the ARM7 link in software ([`3ds_port/nitro`](../3ds_port/nitro)).
   *NitroSystem port under validation:* GCC builds of its Fnd, G2d, G3d,
   Gfd and Snd libraries have been added, with independent host and 32-bit
   resource tests and 3DS symbol coverage
   ([`3ds_port/nitrosystem`](../3ds_port/nitrosystem)).
   *Game done:* all 1,012 game files compile and link into a 3DS program
   whose `NitroMain` runs, with the SDK's threads, interrupts, timers and
   frame loop working ([`3ds_port/game`](../3ds_port/game)), and the
   ARM7's services (power, clock, save chip, touch screen) answering it.
2. **Files:** builder extracts the filesystem; `FS_*` reads from the SD card.
   *Done* (the data pack, `3ds_port/nitro/src/port_fs.c`).
3. **Overlays:** link everything, reset overlay data on load. *Done*
   (`tools/game_overlays.py`, `3ds_port/nitro/src/port_overlay.c`).
4. **Virtual registers, VRAM and OAM.**
5. **2D renderer**, enough for the title screen, then menus and text boxes.
   *First version done:* both engines in software
   (`3ds_port/nitro/src/port_render2d.c`), on the 3DS screens and as PNG
   screenshots from the PC runner.
6. **Input and touch.** At this point menus work.
7. **Saves**, then the **clock**.
8. **3D command interpreter** at the original 256×192, until it matches the DS.
9. **Sound.**
10. **The upgrade:** 400×240 3D, widescreen camera, stereo.

Steps 1 to 6 alone should produce a title screen and working menus in Azahar.
