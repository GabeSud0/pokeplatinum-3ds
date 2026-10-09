# Native Platinum smoke test (.3dsx)

This **is a real Nintendo 3DS homebrew application**. Unlike `hello3d`,
it invokes original (unchanged) **Pokémon Platinum C functions** linked from
`3ds_port/game/build/3ds/libplatinum_game.a`:

- `coins.c`: `Coins_Init`, `Coins_GetValue`, `Coins_CanAdd`,
  `Coins_Add`, `Coins_Subtract`
- `math_util.c`: `LCRNG_SetSeed`, `LCRNG_GetSeed`, `LCRNG_Next`
- `pokedex_language.c`: both original language conversion functions

The game functions were compiled with the same Nintendo SDK headers and
3DS GCC settings as the growing original-game static library. The app
itself uses libctru for the console screens and HID buttons. The small
`source/platinum_api.h` declares the **unchanged original function
signatures** using standard C integer types (Nintendo `u16` / `u32`
and `BOOL` match `uint16_t` / `uint32_t` and `int`). This keeps
libctru and the original DS SDK headers in separate translation units.

## Try it in Azahar / on homebrew-enabled 3DS

1. Open a successful CI run from the GitHub Actions **Build** workflow.
2. Download the **platinum-smoke** artifact, extract
   `platinum-smoke.3dsx` and load it in Azahar, or put it under
   `/3ds/platinum-smoke/` on your 3DS SD card and launch via Homebrew
   Launcher.
3. Test original functions with the controls:

| Button | Original game function |
|---|---|
| A | `Coins_Add(+100)` |
| B | `Coins_Subtract(-25)` |
| SELECT | `Coins_Init` |
| X | `LCRNG_Next` |
| Y | `LCRNG_SetSeed` |
| L/R | `PokedexLanguage_IndexToLanguage` and reverse lookup |
| START | Exit |

The upper screen shows current coin value, random seed, and language
mapping. The lower screen shows the controls. The app contains **no**
ROM graphics, maps, text, music or other game resources.

## Local build

Install devkitPro's `3ds-dev` toolchain, plus Git/Python, then from the
repository root:

```sh
python3 tools/bootstrap.py --clean
make -C 3ds_port/platinum_smoke
```

Building this target compiles the pinned original Platinum game source
library and the patched original NitroSDK/NitroSystem automatically.
The output is `3ds_port/platinum_smoke/platinum-smoke.3dsx`.

## What this **does not** prove

This is **not** `NitroMain`, the original game loop, a DS graphics
compatibility layer, original assets, or a playable Platinum build.
It proves selected original game code can link into and execute inside
a normal **ARM11 native 3DSX**, not just compile as a static library.
Next target: introduce a virtual DS register/input/frame-timing bridge
and progressively run the original game initialization.

Never add a ROM, game assets, or private dumps to git or CI.
