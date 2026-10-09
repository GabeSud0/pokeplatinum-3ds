# libnitrosystem

Nintendo's NitroSystem libraries compiled with GCC for PC and Nintendo 3DS.
The build uses the **pinned, publicly decompiled NitroSystem sources** fetched
by `tools/bootstrap.py`; source modifications are committed only as patches
under `patches/NitroSystem/`. No ROM or extracted game content is tracked.

## Architecture

```text
Pokémon Platinum game code (future 3DS build)
           |
      libnitrosystem.a       Fnd, G2d, G3d, Gfd, Snd
           |
        libnitro.a           NitroSDK compatibility library
           |
   virtual DS memory/registers and hooks -> future 3DS render/audio backends
```

`Makefile` builds the game's five NitroSystem families:

- **Fnd:** memory heaps, lists, archives, and allocators.
- **G2d:** 2D sprites, cell/animation systems, resource loading, OAM and fonts.
- **G3d:** model data, animations, material data, and model render-object setup.
- **Gfd:** frame and linked-list VRAM managers, palette allocations and transfers.
- **Snd:** sound archive, sequencing interfaces, players, streams and resource management.

The separate Nintendo `mcs` debugging interface is excluded. The
`SDK_PORT_3DS` definition enables the minimal GCC-specific upstream patches
without altering the original Metrowerks path. Common flags live in
`../common.mk`, including `-fno-short-enums` and `-fsigned-char` to match
the original data layouts. A generated list from
`tools/nns_inline_symbols.py` marks header-defined GCC inline functions weak
in compiled objects to avoid duplicate external definitions.

## Build and test

From the repository root:

```sh
python tools/bootstrap.py --clean
make -C 3ds_port/nitro
make -C 3ds_port/nitrosystem

# Requires 32-bit host GCC and C libraries (Ubuntu: gcc-multilib):
make -C 3ds_port/nitro TARGET=host32
make -C 3ds_port/nitrosystem TARGET=host32

# Requires devkitPro/devkitARM (3ds-dev):
make -C 3ds_port/nitro TARGET=3ds
make -C 3ds_port/nitrosystem TARGET=3ds

# Check NitroSDK + NitroSystem symbols against game calls:
python tools/check_sdk_coverage.py --strict
python tools/check_sdk_coverage.py \
  3ds_port/nitro/build/3ds/libnitro.a \
  3ds_port/nitrosystem/build/3ds/libnitrosystem.a \
  --nm "$DEVKITARM/bin/arm-none-eabi-nm" --strict
```

GitHub Actions runs the above three targets in separate jobs. It also tests
that a **clean** checkout applies the pinned patch series successfully.

### Host tests

`tests/test_nitrosystem.c` covers Fnd heap operations, Gfd texture/palette
allocators, G2d OAM buffering, G3d model/resource parsing and render-object
initialization, and G2d resource loaders. The tests read the data files
**fetched from the public decompilation** in `build/upstream/res`; do not
commit generated files, dumped ROM resources, or test build output.

**Important:** original NitroSystem 2D resource formats store four-byte
pointers/offsets and unpack them in place. G2d resource loading tests run only
with `TARGET=host32`, where pointer layout matches the 3DS. The regular
64-bit host test exercises the other subsystems and skips those specific
resource-file tests; it must not be mistaken for comprehensive 32-bit coverage.
The test retains untouched serialized bytes when checking loader pointer
fixups.

### Limits: compiling is not yet running the game

- **3D draw calls are not tested:** the project has not yet implemented the DS
  geometry FIFO / renderer backend. Tests initialize G3d model objects but
  deliberately do not call `NNS_G3dInit` or `NNS_G3dDraw`, which can block
  waiting for non-existent graphics hardware.
- Virtual DS registers, VRAM and OAM exist in NitroSDK, but hardware rendering,
  VRAM banking and command translation are future milestones.
- NitroSystem sound code builds, but actual SDAT playback needs the virtual
  ARM7 services / mixer / NDSP backend.
- NitroSDK OS thread switching and several ARM7 services are still hooks,
  not implemented runtime equivalents. See `../nitro/README.md`.
- Symbol-coverage checks verify *linkable definitions*, not emulator or real
  New 3DS gameplay. There is **no playable port or in-emulator boot** yet.

Please check the current PR's CI before treating any reported tests as passed.
For sequencing and intended hardware architecture, see
`../../docs/HARDWARE_LAYER.md` and `../../docs/ROADMAP.md`.
