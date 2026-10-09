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
| Interrupts | IE/IF/IME rules, dispatch to the game's handlers, lowest first; masked while a handler runs; pending ones taken as soon as the CPU unmasks them | SDK `os_irqHandler.c` (patched), `src/port_os.c` |
| DMA | immediate and 3D-FIFO transfers run at once, V-blank transfers at V-blank; completion interrupts and callbacks. H-blank, display, card and cartridge timings are reported as not implemented | `src/port_dma.c`, SDK `mi_dma.c` (patched) |
| Waiting for V-blank | `OS_Halt` advances one frame (calls the port's frame hook) and raises V-blank | `src/port_os.c` |
| Memory map | DS main RAM (8 MB), ITCM, DTCM, WRAM, palettes, OAM, VRAM as memory blocks | `src/port_memory.c` |
| Heaps and arenas, lock IDs, alarms, messages, mutexes | Nintendo's C on top of the above | SDK |
| BIOS calls | CpuSet, CpuFastSet, CRC-16, square root, divide, decompression | `src/svc.c` |
| GBA slot | reports "no cartridge", which is correct on a 3DS (no Pal Park) | `src/ctrdg.c` |
| The ARM7 link (PXI) | message routing between the game's libraries and port handlers | `src/pxi.c` |

**Not yet: hooks are in place, the behaviour comes in later steps**

| Area | Today | Comes with |
|---|---|---|
| Thread switching | `OS_SaveContext` / `OS_LoadContext` report "not implemented" | the 3DS main program (next step) |
| ARM7 services: clock, touch screen, power, microphone, sound, save chip, wireless | messages are accepted and reported once per service; nothing answers | one handler per service (`NitroPort_SetArm7Handler`) |
| Display, 2D and 3D | the registers and VRAM exist; nothing draws them | the 2D renderer and the 3D command translator |
| H-blank DMA (the game's wavy-screen effects), synchronous `MI_VBlankDmaCopy*` | reported as not implemented; the transfer stays pending | the 2D renderer (per-line timing) and the 3DS main loop |
| VRAM banking | each CPU view of VRAM is its own block | the 2D renderer |
| `OS_ResetSystem` (soft reset) | stores the reset parameter, then reports and stops | the 3DS main program |
| Cartridge ROM filesystem (`FS_`) | read-only `rom:` archive backed by the user's libpak data pack, with file-ID and manifest-based path access | overlay-table metadata and other archives remain future work |

**Nothing to do on the 3DS:** caches, tightly-coupled memory and the
protection unit (`src/port_os.c`) are configuration for the DS's ARM946 CPU;
they're accepted and ignored. Timing delays (`OS_SpinWait`, `SVC_WaitByLoop`)
return straight away.

**Not built**, because the game doesn't use them: download play (mb), gesture
recognition (prc), wireless voice (wvr), the China-edition boot screen,
streaming decompression, and a few debug libraries. See the `Makefile`.

## Read the player's ROM data pack through NitroSDK

The SDK `rom:` archive now uses the portable C libpak reader instead of DS
cartridge hardware. It preserves the original `FSFile` API: open by path or
original NitroFS file ID, read a slice, seek, get file length, and close.
`FS_LoadOverlayImage` uses the same file-ID path; `FS_LoadOverlay` additionally
requires valid overlay-table metadata, and executable overlays are **not**
yet a functioning 3DS runtime.

**Why a manifest sidecar?** The v1 pack intentionally stores only file IDs,
not paths. For path-based `FS_OpenFile` the port reads the *small* original
`data_pack/manifest.json` alongside `platinum3ds.pak`. Neither the full ROM
nor the full pack is loaded into RAM. Copy both files privately to the SD card
(or point at both files on a PC), and use a matching build ID:

```c
#include <nitro/fs.h>
#include <nitro_port_fs.h>

FS_Init(FS_DMA_NOT_USE);
if (NitroPort_OpenPak("sdmc:/3ds/platinum/platinum3ds.pak",
                      "sdmc:/3ds/platinum/manifest.json",
                      "YOUR_BUILD_ID") != PAK_OK) {
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
and exercise the real SDK `FS_*` path and file-ID commands, read, seek,
length and overlay-image payload loading. No copyrighted content is used.

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
