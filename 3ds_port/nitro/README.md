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
| ARM7 services: sound | requests are taken and counted finished (so the game never waits for them), nothing plays; alarms never fire (`src/port_snd.c`) | the sound step (the ARM7 sequencer, mixed to the 3DS) |
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
Other ARM7 tags (including real sound output and wireless) remain unimplemented.

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
