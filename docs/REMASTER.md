# The remaster display

On a New 3DS the port shows the game the way a 3DS game would show it,
rather than as a DS game in the middle of the screen. This is a display
path only: the game's own code, its 2D graphics and its 3D models are
untouched (except two small patches that tell the port where the game's
windows are and move the battle's health boxes; see "The wider screen").

What it does:

- **Bigger screens.** The DS's pictures are drawn 1.25 times bigger: the
  lower screen's 256x192 fills its 320x240 exactly; the upper screen is
  320 DS pixels wide (400x240), laid out for the wider screen (below).
- **Sharper 3D.** The 3D is drawn by the GPU straight into the screen at
  the screen's own resolution, not at the DS's 256x192 and then enlarged.
  The outlines the game draws round its 3D (the DS's edge marking) and its
  fog are drawn by the GPU too.
- **The wider screen.** On the upper screen the 3D fills all 400 pixels:
  the geometry engine widens the game's camera by a quarter (what was on
  screen stays where it was; the sides show more of the world), and the
  game's own visibility tests are given the wider view, so it draws what
  the sides show. The 2D is drawn on a canvas 320 pixels wide, nothing
  stretched (see "The wider screen").
- **Depth from the 3D slider.** The upper screen is drawn twice, the 3D
  shifted the opposite way for each eye, by how far each vertex is from the
  scene's middle distance (worked out per scene). 2D stays at the screen's
  depth; flat (orthographic) 3D too.
- **The pixels as they are.** By default each DS pixel becomes 2x2 texels
  and the GPU's bilinear filter scales the rest of the way: every pixel the
  same size, only its borders a texel soft (no blur, no uneven pixels).
  Scale2x and the others are settings. The 3D's textures stay the DS's
  (nearest texel) unless `filter3d` says otherwise: made like the 2D (each
  texel 2x2, then bilinear) the GPU took two to three times as long, the
  overworld lagged, and polygons' edges showed lines on the opening's
  mountains (October 2026).
- **Even pacing.** Each 3D scene is shown a fixed number of V-blanks after
  the game made it, with the 2D the DS would have shown it with (see
  "Timing").

## How it works

`3ds_port/game/app/port_compose_pica.c` puts each screen together on the
GPU, on the 3D thread, which becomes the display thread. The 2D renderer
draws an engine's picture in two layers (`NitroPort_Render2DSplit`,
`port_render2d.c`): what's behind the 3D (the picture without BG0) and the
layers in front of it, with each pixel's alpha over the 3D (a text box that
blends with the 3D, a semi-transparent sprite). The drawing threads draw
those as before (only when the 2D changed: the 3D no longer makes them
draw) and publish them into the textures of the screen the engine is on
(`port_upscale.c`; three sets a screen, so one can be drawn from, one be
the newest and one be written). A screen's frame is then: the layer
behind; the 3D's rear plane (fogged when the game says so); the polygons
(`port_gx3d_pica.c`, `NitroPort_PicaDrawScene`) with edge marking and fog;
a brightness effect on the 3D if the game has one; the layers in front;
the master brightness. A screen isn't drawn again until the V-blank after
its last frame was shown (its other framebuffer is on the LCD until then).

Edge marking on the GPU (the DS outlines a pixel of an opaque polygon when
a neighbouring pixel belongs to another polygon and is further away): for
each of the four directions the opaque polygons are drawn again, moved a
pixel, into a depth/stencil buffer of that direction's own (cleared by a
memory fill ahead of the frame; so it holds what's visible at each pixel's
neighbour), then in place with that buffer as the depth test: where
they're nearer than the neighbour and the neighbour's ID differs, they get
their edge colour. Polygons whose texture has see-through texels keep it
in those passes: a tree's cut-out texels are no part of its shape (drawn
plain, the trees had their rectangles outlined). The others are drawn
there without their texture (only their shape counts), and a texture is
bound only when it changes: citro3d clears the GPU's texture cache with
every bind.

Fog: the DS fogs a pixel by its depth, through a 32-entry density table
starting at FOG_OFFSET, each entry 0x400 >> FOG_SHIFT depth units on
(DISP3DCNT). The PICA's fog takes a 128-entry table over its own depth, so
each scene's depth is put back into the DS's units (the vertex depth is
the DS's z or w, normalised per scene, linear) and the table is worked out
from the DS's (`setup_fog`, `fog_density`). Polygons fog where their
attribute's fog bit is set; the rear plane where the clear colour's is.
The intro's forest uses it (white fog, a linear table). The table goes
to the GPU once per scene's draw (citro3d sends all 128 entries whenever
it's bound; it was bound for each batch).

The vertex program (`gx3d_passthrough.v.pica`) does the view (where the
DS's screen goes on the 3DS's turned framebuffer) and the stereoscopic
shift: `x += strength * max(w - convergence, -w)`, for perspective
vertices (w > 1) only.

The GPU's command list is 1 MB; a draw that wouldn't fit is left out
rather than overflow it (an overflow stopped the GPU thread on the real
3DS). The `[perf] display` line says how full it got.

Publishing a 2D picture (the drawing threads): the 15-bit pixels to
RGB565, each doubled into the tiled texture (a 4x4 block of pixels is one
8x8 tile), and the layers in front to RGBA8 with the alpha multiplied in,
only when there's anything in front (often there isn't); the texture's
used rows flushed, not all of it.

What's approximate, compared with the DS's compositing: a 2D layer in
front of the 3D blends with the 3D by its alpha (eva / 16) where the DS
blends by eva and evb separately; a 3D first target of alpha blending
(BLDCNT bit 0 with the alpha effect) isn't blended with the layer behind;
a brightness effect on the 3D applies to the whole 3D area regardless of
windows; the fog's alpha-only mode isn't drawn. `tests/test_render.c`
(`test_split_layers`, `test_wide_canvas`) checks the two layers put
together give the same picture as the renderer draws in one, where the
DS's result is exactly reproducible, and that the wider canvas's middle is
the DS's picture.

## The wider screen

The upper screen's 2D is drawn on a canvas 320 DS pixels wide, the DS's
x = 0 at 32 (`NitroRender2DSplit.width`/`origin`, laid out by
`NitroPort_WideLayout` in `port_wide.c`). The rules, so that nothing is
stretched:

- **Hand-drawn bands win.** A layer (a BG, or a group of sprites) with a
  picture drawn for it shows that picture in its bands (or all over): see
  [WIDE_ART.md](WIDE_ART.md).
- **The DS's layout stays.** The game's windows, menus and sprites keep
  their places (moved apart, they came apart: a menu's icons are sprites
  placed to match its window). Nothing is stretched.
- **A plain edge goes on.** Where a BG's line is one colour at its edge (a
  picture's plain border, the black of a fade, a window's frame) that
  colour goes on to the screen's edge: for 8 pixels at the edge, or on the
  screens made of pictures (`NitroPort_WidePictures`) for 32, a band's
  width, so that nothing of a picture is drawn out (the opening strip's
  ovals were, into lines).
- **Nothing is mirrored.** A picture that reaches the DS's edge (the
  intro's scenery, the title's torn border) isn't folded over into the
  bands any more: those screens have pictures drawn for them (see
  [WIDE_ART.md](WIDE_ART.md)); without one, its bands stay empty.
- **The UI shows nothing beside the DS's screen** (the backdrop, or the
  3D): a menu's background stays a menu's background.
- **Pictures with plain middles and decorated sides** (the intro's red
  bars) are split: each half goes to its screen edge and the plain middle
  widens.
- **The DS's windows** (its hardware windows, WIN0/1) at the DS's edges go
  on to the screen's: the intro's letterbox covers the bands too. A window
  from 0 to 255 counts as reaching the right edge (the register can't say
  256, so it leaves the DS's last column out; the bands went black beside
  the intro's city).
- **The battle background** is a picture wider than the DS's screen (a
  512-pixel map: the trainer's slide-in scrolls across it), so beyond the
  DS's edges it shows its own art.
- **The place name sign** goes to the screen's left edge while it shows:
  the game tells the port (`patches/pokeplatinum/0014`,
  `NitroPort_WindowAnchor`; patch 0012 tells it where the windows are).
- **The battle's health boxes** go to the screen's edges, the player's
  right and the opponent's left (`patches/pokeplatinum/0013`, by
  `NitroPort_WideScreenExtra`).
- **OBJs** (sprites) placed past the DS's edges show.
- **The 3D** is drawn wide (the camera widened) only while it's on the
  upper screen; the intro's flyover is on the lower screen, at the DS's
  width.

## Timing

Each 3D scene carries the V-blank count when the game finished it (its
SWAP), and each published 2D picture the count when the drawers read the
game's display state. A frame pairs the 3D with the newest 2D no newer
than the scene + 1 (the DS shows a scene the frame after its 2D was set),
and is shown `lag` V-blanks after the scene was made: the 3D comes at the
game's own even pace (every second V-blank in the field), not whenever a
scene happens to be done. `lag` starts at 2 and adapts (1 to 4): up when a
frame would be late, back down after ten seconds with room to spare.

A new scene is drawn once engine A's drawer is done with the 2D of the
frame after it (or it's known there's none: the game hands the drawers a
frame every other V-blank, or the drawer was busy and left that one out;
at most a few V-blanks). Drawn as soon as it came, it went out with the
2D before and was drawn again when that came: the 3D drawn twice, and if
the 2D was late, a frame of the wrong picture (the title's Giratina showed,
bright, for a frame before its fade in from black).

The drawing threads draw from the display state as it was when the game
handed them the frame: its registers, palettes and OAM are copied then
(`NitroPort_Render2DTakeState`, a few KB), so the game's next frame (its
sprites, scroll, colours) can't get mixed into a picture that takes long
to draw (sprites showed in the wrong places for a frame). The VRAM is read
as it is: before its next V-blank work the game waits for a drawer still
reading it. Working out whether a screen changed compares the small parts
first, and the VRAM (864 KB for engine A) only when they're the same.

Where things run on a New 3DS: the game, the display thread and engine B's
drawer on core 0 (the game first); engine A's drawer and the geometry on
core 2. Core 1 is the system's; a program gets 30% of it, and there the
geometry took 18-30 ms a scene and left scenes out (the intro's flyover
jumped), and engine B's screen drew 14-25 frames a second
(`geometry.txt`: `core 1` puts both back there).

Showing is apart from drawing. The display thread draws a frame, has the
GPU put it into one of three LCD framebuffers of the screen's own, and
queues it with the V-blank it's for; a V-blank thread (`main_3ds.c`)
counts the V-blanks and puts each frame on the LCD at its one
(`GSPGPU_SetBufferSwap`). The display thread never waits for a V-blank: it
goes on to the next scene. (The first version waited inside the frame, and
the scenes made during the wait were never drawn: 10 to 14 frames a second
of a game making 30, and the waiting grew the lag.) A screen with two
frames waiting isn't drawn again until one has shown.

## Settings

`sdmc:/3ds/pokeplatinum/remaster.txt`, a setting a line, `name value`:

| line | meaning |
|------|---------|
| `remaster off` | the classic display (the DS's pictures in the middle of the screens). The default on an Old 3DS. |
| `wide off` | the upper screen at the DS's width (320 of the 400 pixels), 2D and 3D |
| `filter sharp` | (the default) each pixel 2x2, then bilinear: the pixels as they are |
| `filter nearest` | nearest-neighbour: crisp, but pixels 1 or 2 screen pixels wide by turns |
| `filter smooth` | bilinear from the DS's pixels: soft |
| `filter scale2x` | Scale2x, then bilinear: diagonals rounded off |
| `stereo off` / `stereo 0.03` | no depth from the slider / its strength at full slider, in screen widths (default 0.02) |
| `convergence 400` | the distance (the DS's w) that's at the screen's depth, instead of each scene's middle |
| `edges off` | no outlines (edge marking) |
| `filter3d sharp` / `filter3d smooth` | the 3D's textures each texel 2x2 then bilinear / bilinear (default `nearest`: the DS's; the others cost the GPU two to three times as much) |
| `flip on` | the 2D layers upside down (if a GPU's texture rows come out the other way round) |

The log says what's on (`[compose] screens drawn by the GPU: ...`) and,
every two seconds, `[perf] display: N frames/s drawn (N with a new 3D
scene, of N scenes/s), each X ms building + X ms GPU; 2D published N/s, X
ms each; 3D shown N V-blanks after the game made it, N late; GPU command
list N% full at most, N draws cut`. In the field the frames with a new 3D
scene should match the scenes made (30 a second); building + GPU is what
a frame costs the display thread, and must stay under 33 ms for that.

## 60 frames a second in the 3D

The game runs its field at 30 frames a second: its main loop waits for two
V-blanks, and everything in the field (walking, the camera, animations)
moves in those steps. The display shows each of those steps evenly (see
"Timing"). Showing the field at a true 60 means drawing a frame of the
port's own in between each pair: the camera and every moving model
halfway, from the two scenes either side (matching each polygon of one
scene to the same polygon of the next, which the DS's command stream
doesn't say, so it has to be worked out from the display lists the game
sends). That's the next step for the display; it costs a frame of delay
(the in-between needs the scene after it).

## Known

- In the Azahar emulator, the field's 3D sometimes flashes black for a
  frame. Not seen on a real New 3DS, nor in the PC build. If it shows on a
  3DS, `edges off` in remaster.txt says whether it's the edge marking.
