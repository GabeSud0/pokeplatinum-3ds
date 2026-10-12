# libnitro

Nintendo's NitroSDK (the DS system library the game is built on), compiled
with GCC so it runs on the 3DS, plus unit tests that run on a PC.

Every NitroSDK function the game calls (281 of them) now exists in the
library; `tools/check_sdk_coverage.py` checks this in CI. That makes this the
foundation for the next step: compiling the game itself.

## How it fits together

```
 game code  ─►  NitroSDK (Nintendo's C, patched)  ─►  "DS hardware"
                                                       │
                       ┌───────────────────────────────┤
                       ▼                               ▼
         memory blocks standing in for       the port's software versions
         DS registers, RAM, VRAM, OAM        of hardware that does things:
         (src/port_ioreg.c, port_memory.c)   CPU state, interrupts, divider,
                                             BIOS, the ARM7 link
```

Everything is switched on by one define, `SDK_PORT_3DS`. Our patches to the
SDK (`patches/NitroSDK/`) only change behaviour when it's set, so the patched
SDK still builds the original game with Metrowerks.

## What's real and what's stubbed

**Real: works like the DS**

| Area | What | Where |
|---|---|---|
| Math | fixed point, trig, vectors, matrices (27 functions translated from assembly), CRC, MD5, SHA-1, HMAC | SDK + `src/dgt.c` |
| Divider and square root | the DS maths chip in software, including division by zero | `src/port_cp.c` |
| Memory | copy, fill, atomic swap | `src/mi_memory.c`, `mi_swap.c` |
| Decompression | LZ77 (both formats), run-length, Huffman, diff filters, bit unpacking. Checked against Nintendo's own compressors | `src/mi_uncompress.c` |
| CPU state | interrupt enable/disable, CPU mode | SDK `os_system.c` (patched) |
| Interrupts | IE/IF/IME rules, dispatch to the game's handlers, lowest first, in the CPU's interrupt mode; masked while a handler runs; pending ones taken as soon as the CPU unmasks them | SDK `os_irqHandler.c` (patched), `src/port_os.c` |
| Threads | NitroSDK's own scheduler, unchanged. Each SDK thread is played by a thread of the machine (3DS or POSIX), one running at a time, handed over where the SDK saves and loads contexts; a switch an interrupt asks for happens at its end, as on the DS. Each thread keeps its interrupt state and divider operands | `src/port_thread.c`, `src/platform/` |
| Timers | the four hardware timers counted from a real clock: reload, prescaler, cascade, overflow interrupts (none lost). The SDK's tick and alarms (`OS_Sleep`) and the game's own timer work. V-count follows the clock | `src/port_timer.c`, SDK `timer.h`, `os_tick.c` (patched) |
| 2D graphics | both engines drawn in software: text, affine and extended BGs (bitmaps too), OBJs (affine, 8-bit, extended palettes, bitmap, semi-transparent, OBJ window), windows, blending, master brightness, VRAM display mode. `NitroPort_RenderScreens` gives the two screens as 15-bit pixels | `src/port_render2d.c` |
| Overlays | all linked into the program; `FS_LoadOverlay` and co. describe each overlay and reset its data on loading, as reading it from the cartridge did. The program gives the port the overlays' data sections (`NitroPort_SetOverlays`) | `src/port_overlay.c`, SDK `fs_overlay.c` (patched) |
| Firmware state | the cartridge header, the user's settings (name, birthday, language, touch calibration), MAC address, buttons and lid, in DS memory where the game reads them | `src/port_boot.c` |
| DMA | immediate and 3D-FIFO transfers run at once, V-blank transfers at V-blank; completion interrupts and callbacks. H-blank, display, card and cartridge timings are reported as not implemented | `src/port_dma.c`, SDK `mi_dma.c` (patched) |
| Waiting for V-blank | `OS_Halt` advances one frame: calls the port's frame hook, starts V-blank (V-blank DMAs) and requests its interrupt, taken if the CPU has interrupts on | `src/port_os.c` |
| Memory map | DS main RAM (8 MB), ITCM, DTCM, WRAM, palettes, OAM, VRAM as memory blocks | `src/port_memory.c` |
| Heaps and arenas, lock IDs, alarms, messages, mutexes | Nintendo's C on top of the above | SDK |
| BIOS calls | CpuSet, CpuFastSet, CRC-16, square root, divide, decompression | `src/svc.c` |
| GBA slot | reports "no cartridge", which is correct on a 3DS (no Pal Park) | `src/ctrdg.c` |
| The ARM7 link (PXI) | message routing between the game's libraries and port handlers; answers arrive through the receive FIFO's interrupt, as on the DS | `src/pxi.c` |
| Virtual ARM7: card backup, RTC, PM and touch | save-card SPI requests, BCD local clock, PMIC replies, shared touch samples and autosampling | `src/port_arm7.c`, `port_card.c`, `port_rtc.c`, `port_pm.c`, `port_tp.c` |

**Not yet: hooks are in place, the behaviour comes in later steps**

| Area | Today | Comes with |
|---|---|---|
| ARM7 services: sound | second, prefixed build of the original ARM7 sound sequencer; plain-C 16-channel PCM/ADPCM/PSG/noise and capture mixer; audio-clock timers, PXI commands and alarms (`src/port_snd.c`, `port_sound_chip.c`) | live NDSP test and remaining chip accuracy |
| ARM7 services: microphone, wireless | not yet emulated | one service per PXI tag |
| 3D | the 3D engine reads as idle with an empty command FIFO, and the GXFIFO interrupt fires while its condition holds, so display lists sent by DMA complete (`src/port_gx.c`); nothing is drawn | the 3D command translator |
| H-blank DMA (the game's wavy-screen effects), synchronous `MI_VBlankDmaCopy*` | reported as not implemented; the transfer stays pending | the 2D renderer (per-line timing) and the 3DS main loop |
| VRAM banking | each CPU view of VRAM is its own block; the renderer reads BG/OBJ data from the views and extended palettes from the bank the VRAM registers name. A bank moved from one view to another doesn't take its contents along | if a game needs it |
| 2D: mosaic, large-bitmap BG (mode 6), main-memory display, display capture, mid-frame (H-blank) changes | not drawn; each frame is drawn from its end state | as the game needs them |
| `OS_ResetSystem` (soft reset) | stores the reset parameter, then the program reports it and stops | restarting the game in place |
| Cartridge ROM filesystem (`FS_`) | read-only `rom:` archive backed by the user's libpak data pack, with file-ID and manifest-based path access | directory listing and other archives, if the game needs them |

**Nothing to do on the 3DS:** caches, tightly-coupled memory and the
protection unit (`src/port_os.c`) are configuration for the DS's ARM946 CPU;
they're accepted and ignored. Timing delays (`OS_SpinWait`, `SVC_WaitByLoop`)
return straight away.

**Not built**, because the game doesn't use them: download play (mb), gesture
recognition (prc), wireless voice (wvr), the China-edition boot screen,
streaming decompression, and a few debug libraries. See the `Makefile`.

## Stage 1: DS 3D geometry capture and PC reference renderer

`src/port_gx3d.c` decodes the original DS geometry commands from NitroSDK's
inline `G3_*` write ports and from the `GXFIFO` DMA and CPU streams.
The interception of SDK inline writes is patch `0024` (`SDK_PORT_3DS`
only); `src/port_dma.c` and `MIi_CpuSend32` forward every FIFO-bound
word in order, including direct 0x440–0x5FF command-port writes. NitroSystem
patch `0003` intercepts real raw `reg_G3X_GXFIFO = x` stores from
`gecom.c` and `sbc.c`. Command decoding handles both direct opcodes and
packed words spanning DMA and CPU bursts.

The engine retains signed 20.12 matrices and stacks, per-light ambient,
diffuse, half-vector specular and shininess-table shading, texture-coordinate
transforms (TEXCOORD/NORMAL/VERTEX source), vertex formats, primitive assembly,
clip-space coordinates, clipping, texture state and test results.
POLYGON_ATTR latches at BEGIN_VTXS; texture state applies immediately.
Projection/vector readback is updated only on matrix changes, not per FIFO
word. The original 31-entry position/vector stack and the 2048-polygon /
6144-vertex hardware limits are enforced. `SWAP_BUFFERS` publishes a renderer-independent
array of screen-space triangles (including attributes, perspective W,
z-depth, colour and UV) through `NitroPort_Gx3dScene()`. FIFO commands execute
immediately, so GXSTAT still reports an empty FIFO and the level-based IRQs
used by `MI_SendGXCommandAsync` keep working; the matrix stack levels,
overflow flag and position/vector test results are no longer invented.

On the **PC only**, `src/port_gx3d_raster.c` renders that scene to 256×192
RGB555 with a z-buffer, perspective-correct nearest-neighbour sampling,
palette texture formats, A3I5/A5I3, direct colour and 4×4 compressed texture
selectors. The existing 2D renderer composites this into engine A's BG0
with BG priority, window visibility and scroll, before colour effects.
The 3DS GPU is used by the later Stage 2 plumbing described below;
the PC keeps the full Stage 1 reference rasterizer.

```sh
python3 tools/bootstrap.py
make -C 3ds_port/nitro test
# Use your own locally generated data pack; nothing is checked into Git:
tools/run_pc_wsl.sh  # supports --screenshots and --screenshot-every
```

`tests/test_gx3d.c` builds only synthetic triangles/texture bytes, verifies
matrix operations, packed command parsing, culling, z-order and texels, and
executes real `MI_CpuSend32` and the patched upstream NitroSystem
`NNS_G3dGeBufferOP_N`/`NNS_G3dGeSendDL` paths. Further tests cover
light-space transforms, emission/ambient/specular, the shininess table,
all texture-coordinate source modes, polygon latching, 31-level stack
boundaries, VEC_TEST precision, non-matrix GXSTAT refresh, viewport mapping,
translucent/opaque draw order and alpha blend-enable, scene depth mode and
hardware scene limits. The
existing DMA/IRQ regression test remains in `tests/test_threads.c`.

**Reference limitations:** this is a staging renderer, not cycle-exact DS 3D.
The virtual FIFO is consumed synchronously, and some DS details (polygon ID
effects, shadow/toon/edge marking, depth/W precision, bit-exact lighting
rounding and depth sorting within the translucent pass) still need
hardware-reference tests. The PC renderer draws translucent faces after
opaque ones and obeys DISP3DCNT's alpha-blend enable bit, but does not yet
model every DS polygon-ID or per-texel translucent sorting rule.
The new 3DS pipeline initially renders **opaque untextured** triangles only.

## Stage 2 PR 1: PICA plumbing and shared 3D BG0 image

The 3D scene now uses three statically allocated scene slots (building,
newest-finished and a scene acquired by the renderer), coordinated through
`port_gx3d_mailbox.c`. The producer never copies an entire scene, never
overwrites an acquired scene and never waits for the GPU. Acquire by
`NitroPort_Gx3dAcquireScene(&serial)` and **always release** with
`NitroPort_Gx3dReleaseScene(scene)`.

Both the PC and 3DS 2D compositors use
`NitroPort_Gx3dAcquireImage()` / `NitroPort_Gx3dReleaseImage()`: two
RGB555+coverage images are protected from overwrite while a 2D drawer
reads them. On PC the existing Stage 1 software rasterizer renders into
the image interface without changing the BG0 compositor; on 3DS the
separate GPU worker produces a transferred RGBA8 image and publishes its
converted RGB555 pixels when the GPU is finished. The 2D drawer never
waits for GPU completion and uses the most recently completed image.

On 3DS only, the game starts one PICA worker on core 2 (New 3DS) or
core 0 below the game (Old 3DS). It owns all citro3d commands and
off-screen target with 256x192 RGBA8 + depth/stencil, both double-buffered
vertex arrays and linear transfer output buffers. The shader is assembled
with `picasso` from `game/app/gx3d_passthrough.v.pica`.
`port_gx3d_vertex.c` builds clip-space float4 (preserving W and DS
screen-space mapping), colour bytes and normalized UV, but PR1 **draws only
untextured opaque triangles** with PICA depth testing. Its host-testable
depth function maps near to 0, far to 1; the PICA uses `GPU_LESS` with
maximum-depth clear so nearer geometry wins. Both scene and image gates
spin briefly then use the port's 50-us sleep (important for lower-priority
thread owners on Old 3DS). The [perf] line includes 3D milliseconds/scene,
triangles drawn, and dropped triangles per scene.

`tests/test_gx3d_plumbing.c` covers two-thread scene/image handoff, contended
spin-then-sleep locks, independent vertex/depth mapping (near wins under
`GPU_LESS`), full 12,288 triangle-list vertex capacity, truncation
counts for smaller buffers, and RGBA PNG colour/alpha/orientation with only
synthetic data. The GPU allocates two linear buffers of **12,288 vertices**,
enough for Stage 1's maximum 4,096 post-clipping triangles. Any scene
overflow or capacity drop is counted in `[perf]`.

**Comparing orientation on real hardware/Azahar:** press **L+R+Y** to write
`sdmc:/3ds/pokeplatinum/3d_frame_<frame>.png`. On PC, run
`pokeplatinum-pc ... --dump-3d FRAME` to write
`3d_frame_<frame>.png` in the working directory. Both use the exact same
RGBA8 PNG writer, with alpha 0 for transparent BG0 and row 0 at the top.
These are private outputs from your own pack; never commit them. Textures,
complex alpha, exact W-depth, toon and effects are for later PRs in
`docs/3D_STAGE2.md`.

## Virtual ARM7 sound driver and software sound chip

The port builds a **separate SDK_ARM7 copy** of Nintendo's pinned sound
driver (`libraries/snd/src/ARM7/` and its shared sources), then uses a
symbol-remapping table to prefix its exported functions/variables with
`N7_`. The game keeps its original ARM9 `SND_*` APIs unchanged.

`src/port_snd.c` accepts the original PXI SOUND command lists, executes
them in the genuine ARM7 sequencer/voice allocator, advances it at an
audio-rate-based 2,728 microsecond interval, and updates
`SNDSharedWork`. Alarms are dispatched back to the ARM9 from
`NitroPort_SoundPumpReplies()`, never by using the original game-thread
scheduler from the audio side.

`src/port_sound_chip.c` is separate, portable C: 16 channels of PCM8,
PCM16, IMA-ADPCM, square waves and noise, playback timer, volume, shift,
pan, master volume and two capture buffers. It produces interleaved
signed 16-bit stereo at 32,768 Hz. No DSP/libctru headers are needed
for PC unit tests. Source wave buffers are private game/runtime memory;
neither ROM files nor actual sound assets are in this repository.

In the 3DS application, an independent audio thread refills triple
NDSP buffers. If `sdmc:/3ds/dspfirm.cdc` is missing, the application
logs that sound output is unavailable and still runs the audio clock.
On the PC, an audio thread is silent by default but can optionally
write stereo to a `--wav FILE` output for inspection.

Unit tests exercise invented waveforms for all five channel modes,
channel volume/pan, looping and capture, and a synthetic
sequence/bank/wave archive passed to the **original** ARM9 SND API.
`test_arm7` also verifies that over 3,000 sound command batches return.

This isn't a promise of exact hardware-level audio fidelity. Mix/capture
timing and long-run game playback still need Azahar/console comparison.

## Virtual ARM7 services (card, RTC, power, touch)

Call this once **before** the original NitroSDK initializes CARD, RTC, PM
or TP (typically just after bringing up the portable platform):

```c
#include <nitro_port.h>

NitroPort_SetSavePath("sdmc:/3ds/platinum/platinum.sav");
NitroPort_InitArm7Services();
```

The save file is **private, raw 512 KiB FLASH data** (the same bytes as
an emulator `.sav`), created as 0xFF if it does not exist. Every successful
write or erase calls `fflush()` before the original CARD API reports
completion. Existing saves must be exactly 512 KiB; an invalid or unreadable
image causes `CARD_IdentifyBackup` to fail rather than truncating it.
The 3DS main program must mount the SD stdio filesystem before using an
`sdmc:` path. The host uses an ordinary filesystem path.

Each 3DS frame, forward touch-screen input in **DS bottom-screen coordinates**:

```c
NitroPort_SetTouch(touch_down, x_0_to_255, y_0_to_191);
```

With TP autosampling enabled this also emits the sampling notification the
original ARM9 SDK uses to update its ring buffer. The RTC uses the platform
C library's `localtime()`, so the 3DS clock/time zone must be configured
by the hosting application.

The services answer the way the ARM7 does: through the receive FIFO's
interrupt, so a reply is taken as soon as the game has interrupts enabled
(at once if it has, otherwise when it re-enables them). The card library runs
its tasks on its own thread, as on the DS. The PMIC returns battery OK and
tracks backlight/sound state but does **not** physically power off the console.
Other ARM7 tags (including wireless) remain unimplemented.

Host and host32 `make test` execute `tests/test_arm7.c` using the original
`CARD_*`, `RTC_*`, `PM_*` and `TP_*` SDK entry points and a removable
temporary save, never ROM or actual user data. The SDK's ARM9 side of these
libraries is unchanged.

## Read the player's ROM data pack through NitroSDK

The SDK `rom:` archive now uses the portable C libpak reader instead of DS
cartridge hardware. It preserves the original `FSFile` API: open by path or
original NitroFS file ID, read a slice, seek, get file length, and close.
Overlays don't come from the pack: their code is linked into the program
(see the table above).

**Why a manifest sidecar?** The v1 pack intentionally stores only file IDs,
not paths. For path-based `FS_OpenFile` the port reads the *small* original
`data_pack/manifest.json` alongside `platinum3ds.pak`. Neither the full ROM
nor the full pack is loaded into RAM. Copy both files privately to the SD card
(or point at both files on a PC), and use a matching build ID:

```c
#include <nitro/fs.h>
#include <nitro_port_fs.h>

FS_Init(FS_DMA_NOT_USE);
if (NitroPort_OpenPak("sdmc:/3ds/pokeplatinum/platinum3ds.pak",
                      "sdmc:/3ds/pokeplatinum/manifest.json",
                      "platinum-nitrofs-1") != PAK_OK) {
    /* Stop initialization: wrong pack, unsupported ROM, or missing sidecar. */
}

/* After all open FSFile handles have been closed: */
NitroPort_ClosePak();
FS_End();
```

Mount/initialize the SD filesystem first on the 3DS, so its `sdmc:`
stdio device is available. Host tests use standard paths. File IDs remain
sparse and are not renumbered. Writes and direct cartridge offsets are
intentionally unsupported; the archive never pretends to have the DS's ROM
header, overlay table, FAT or FNT in the pack. The caller must close any
FS handles before replacing or closing a mounted pack.

For standalone synthetic integration checks:

```sh
make -C 3ds_port/nitro test
```

Those tests generate a pack with `builder/pack.py` in ignored `build/`,
and exercise the real SDK `FS_*` path and file-ID commands, read, seek and
length, and overlay loading resetting an overlay's data. No copyrighted
content is used. `tests/test_threads.c` covers threads, interrupts with
threads, timers, alarms and the PXI FIFO through the SDK's own API, with a
clock the test drives.

## "Not implemented" reports

Anything the port can't do yet calls `NitroPort_Unimplemented("...")`, which
logs one line the first time, for example:

```
[nitro] not implemented: ARM7 service: microphone
```

When the game first runs, these lines are the to-do list.

## Building and testing

```sh
python tools/bootstrap.py         # fetches pokeplatinum, NitroSDK, NitroSystem
cd 3ds_port/nitro
make                              # builds for PC and runs the tests
make TARGET=3ds                   # builds libnitro.a for the 3DS (devkitARM)
python ../../tools/check_sdk_coverage.py   # every SDK function the game calls exists?
```

The tests (664 checks) compare against values worked out independently
(hardware docs, standard test vectors, Python) and against Nintendo's own C
code: for example, every decompressor must undo Nintendo's compressor, and our
rotation matrices must equal the SDK's own axis-angle rotation. They were also
checked against deliberately broken code, to make sure they catch mistakes.

## Testing on a 64-bit PC

The SDK keeps addresses in 32-bit integers. The PC build is linked as a
non-PIE executable so its data sits below 4 GB and those addresses fit. On the
3DS (32-bit) this isn't an issue. Structure-size checks that involve pointers
are skipped on the PC only; they run in the 3DS build.
