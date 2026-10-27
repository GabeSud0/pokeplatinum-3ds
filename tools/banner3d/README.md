# The HOME Menu's animated 3D banner

When the game is highlighted on the HOME Menu, the upper screen shows an
animated 3D banner, laid out like Pokémon Y's: a soft portal glow at the back,
Platinum's own title-screen Giratina in the middle playing its idle, and the
Pokémon Platinum 3DS logo in front, with the port's chime.

`make game-cia ROM=…` makes it from your ROM (and so does
`tools/build_windows_msys2.sh`):

```sh
python3 tools/banner3d/make_banner.py --rom platinum.nds \
    --bannertool build/cia-tools/bin/bannertool \
    --chime 3ds_port/game/cia/chime.wav -o build/home-menu-art/banner3d.bnr
```

If it can't be made, the `.cia` gets the flat banner instead
(`make game-cia BANNER=flat` asks for that one).

**Needs** Python 3.11 or later and git: nothing else (the standard library
only, MSYS2's Python and the Linux build's Debian 12 one included).

## What comes from where

- **Giratina** (its model, textures and idle animation) comes from your ROM,
  at build time: `demo/title/titledemo.narc`, members 1 and 2 (the title
  screen's `giratina.nsbmd` and `giratina.nsbca`). The banner made is yours:
  it contains the game's model, so it's never committed or shared.
- **The portal glow and the logo** ([`art/`](art)) are this project's own
  art. [`dev/`](dev) remakes them (needs numpy and Pillow; not needed to build).
- **The converter** is [skyfloogle/pycgfx](https://github.com/skyfloogle/pycgfx),
  fetched once at a pinned commit into `build/pycgfx`. It isn't included here:
  it has no licence that allows sharing it. Its two libraries are replaced by
  small standard-library stand-ins ([`shims/`](shims): only the parts of
  Pillow and gltflib that pycgfx calls).

## How it's made

| File | |
|---|---|
| [`nitro.py`](nitro.py) | reads NitroSystem 3D models (NSBMD) and joint animations (NSBCA); format knowledge from [apicula](https://github.com/scurest/apicula) |
| [`giratina_parts.py`](giratina_parts.py) | runs the model's render commands: every vertex is bound to one joint, with each joint's matrix for all 121 frames |
| [`scene.py`](scene.py) | the banner as a glTF scene: placement, joints, animation |
| [`to_cgfx.py`](to_cgfx.py) | glTF to CGFX with pycgfx, plus the fixes the HOME Menu needs |
| [`make_banner.py`](make_banner.py) | the whole thing, from your ROM to the `.bnr` |
| [`linalg.py`](linalg.py), [`pngio.py`](pngio.py), [`narc.py`](narc.py) | matrices, PNGs and Nitro archives, in plain Python |

It makes exactly the banner that was tested on a New 3DS XL (`make_banner.py`
says "the tested banner" when the CGFX's SHA-1 matches).

## What the HOME Menu needs (learnt on hardware)

These cost a frozen or crashed HOME Menu each before they were found.

- **No large see-through cards in the middle of the scene.** A big halo
  card behind the logo froze it; the portal glow, far behind and screen-facing, is fine.
- **pycgfx's fixes** (found by
  [daaaaavia/CBC-for-YANBF](https://github.com/daaaaavia/CBC-for-YANBF), MIT):
  each mesh names its node's bone, the bone lookup tree is rebuilt after
  pycgfx reorders the bones of BLEND materials, and BLEND materials are marked
  translucent. Applied in `to_cgfx.py` at run time.
- **Keep the normals.** The materials are unlit, but a model without normals
  crashed it.
- **Not too much geometry.** Smoothing Giratina (4x the triangles) crashed it,
  even with a small banner overall. The largest scene that worked was about
  431 KB; this one is 349 KB.
- **It spins every banner** one turn per 10 seconds (faster when you blow into
  the microphone). Like Pokémon Y's `y_autorotate` bone, Giratina sits under an
  `autorotate` bone that turns the other way at the same rate, with its pivot
  on the spin axis, so it stays still, and turns in place when you blow. The
  portal and the logo are screen-facing billboards (like Pokémon Y's logo) at
  the root, so they never turn or drift, and they sit outside the circle
  Giratina sweeps when it spins.
- **Only the part between its bars is drawn** (about y 40 to 212 of the
  240-pixel screen): the wings stay inside it over the whole loop.
- **The logo is drawn at 1 texel per screen pixel** (224 px wide), dithered
  to the 3DS's RGBA4 colours, so it stays sharp.
- **Skinning is fine** when each vertex follows one joint and each part uses
  at most 7 joints (as Pokémon Y's banner does).
- **The HOME Menu remembers a title's banner.** To see a new one, install the
  title under a new ID, or delete it and install it again.
