# libnitro

Nintendo's NitroSDK (the DS system library the game is built on), compiled
with GCC so it runs on the 3DS, plus unit tests that run on a PC.

This is step 1 of the plan in [`docs/HARDWARE_LAYER.md`](../../docs/HARDWARE_LAYER.md):
prove that Nintendo's decompiled code can be built with devkitARM, and set up
the pieces every later step uses.

## What's in it so far

| Part | SDK files | How |
|---|---|---|
| Fixed-point math, trig, vectors | `fx/src/*.c` | Nintendo's C, compiled as-is |
| Matrices | `fx/src/fx_mtx*.c` | 27 assembly functions given C versions (patch 0003) |
| Divider and square root | `fx/src/fx_cp.c` + `cp/*.h` | hardware replaced by `src/port_cp.c` (patch 0002) |
| CRC, checksums, FFT, bit tricks | `math/src/*.c` | Nintendo's C; one Metrowerks-only syntax fixed (patch 0004) |
| MD5, SHA-1 | prebuilt binary in the SDK | our own C in `src/dgt.c` |
| Memory copy and fill | `mi/src/mi_memory.c`, `mi_swap.c` | assembly only; our own C in `src/` |

## Building and testing

```sh
python tools/bootstrap.py         # once: fetches pokeplatinum, NitroSDK, NitroSystem
cd 3ds_port/nitro
make                              # builds for PC and runs the tests
make TARGET=3ds                   # builds libnitro.a for the 3DS (devkitARM)
```

The tests compare against values worked out independently (hardware docs,
Python, standard test vectors) and against parts of the SDK that are still
Nintendo's original C code. They currently run 544 checks.

## How the port is switched on

Everything is controlled by one define, `SDK_PORT_3DS`. Our patches to the
SDK (in `patches/NitroSDK/`) only change behaviour when it's set, so the
patched SDK still builds the original game with Metrowerks.

- **Integer types** use `<stdint.h>`, so `u32` is 32 bits on every compiler.
- **I/O registers**: `HW_IOREG` points at `NitroPort_IoReg[]` (in
  `src/port_ioreg.c`) instead of address `0x04000000`. Registers that only
  store values just work; registers with side effects get a software version.
  The divider and square root are the first.
- **Assembly**: Metrowerks inline assembly stays for the original build;
  GCC gets C versions decoded from it.

## Testing on a 64-bit PC

The SDK keeps addresses in 32-bit integers. The PC build is linked as a
non-PIE executable so its data sits below 4 GB and those addresses fit. On the
3DS (32-bit) this isn't an issue. Structure-size checks that involve pointers
are skipped on the PC only.
