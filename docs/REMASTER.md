# The remaster display

On a New 3DS the port shows the game the way a 3DS game would show it,
rather than as a DS game in the middle of the screen. This is a display
path only: the game's own code, its 2D graphics and its 3D models are
untouched.

What it does:

- **Bigger screens.** The DS's 256x192 pictures are drawn at 320x240 on both
  screens (1.25 times the size); the lower screen is filled exactly, the
  upper one at full height.
- **Sharper 3D.** The 3D is drawn by the GPU straight into the screen at
  the screen's own resolution, not at the DS's 256x192 and then enlarged.
  The outlines the game draws round its 3D (the DS's edge marking) are
  drawn by the GPU too, at that resolution.
- **Wide view.** On the upper screen the 3D fills all 400 pixels: the
  geometry engine widens the game's camera by a quarter (what was on
  screen stays where it was; the sides show more of the world), and the
  game's own visibility tests are given the wider view, so it draws what
  the sides show. The 2D layers (text boxes, menus) keep the DS's layout,
  centred; the picture behind the 3D continues its edge colour into the
  side bands.
- **Depth from the 3D slider.** The upper screen is drawn twice, the 3D
  shifted the opposite way for each eye, by how far each vertex is from the
  scene's middle distance (worked out per scene). 2D stays at the screen's
  depth; flat (orthographic) 3D too.
- **A pixel-art filter.** The 2D layers are upscaled with Scale2x before
  the GPU scales them the rest of the way with bilinear filtering: slopes
  and curves in sprites and text come out as slopes and curves rather than
  steps, without blurring.
- **60 frames a second on the display.** The GPU composes a new frame
  whenever anything changed: the 2D at 60, the 3D as often as the game
  makes a new scene (30 a second in the field: the game's own pace; see
  below).

## How it works

`3ds_port/game/app/port_compose_pica.c` puts each screen together on the
GPU, on the 3D thread, which becomes the display thread. The 2D renderer
draws an engine's picture in two layers (`NitroPort_Render2DSplit`,
`port_render2d.c`): what's behind the 3D (the picture without BG0) and the
layers in front of it, with each pixel's alpha over the 3D (a text box that
blends with the 3D, a semi-transparent sprite). The drawing threads draw
those as before (only when the 2D changed: the 3D no longer makes them
draw) and upscale them into textures (`port_upscale.c`, three per engine so
one can be drawn from, one be the newest and one be written). A screen's
frame is then: the layer behind, over the whole screen; the 3D's rear
plane; the polygons (`port_gx3d_pica.c`, `NitroPort_PicaDrawScene`) with
edge marking; a brightness effect on the 3D if the game has one; the
layers in front; the master brightness. A screen isn't drawn again until
the V-blank after its last frame was shown (its other framebuffer is on the
LCD until then).

Edge marking on the GPU (the DS outlines a pixel of an opaque polygon when
a neighbouring pixel belongs to another polygon and is further away): for
each of the four directions the opaque polygons are drawn again, moved a
pixel, into a depth/stencil buffer of their own (so it holds what's visible
at each pixel's neighbour), then in place with that buffer as the depth
test: where they're nearer than the neighbour and the neighbour's ID
differs, they get their edge colour.

The vertex program (`gx3d_passthrough.v.pica`) does the view (where the
DS's screen goes on the 3DS's turned framebuffer) and the stereoscopic
shift: `x += strength * max(w - convergence, -w)`, for perspective
vertices (w > 1) only.

What's approximate, compared with the DS's compositing: a 2D layer in
front of the 3D blends with the 3D by its alpha (eva / 16) where the DS
blends by eva and evb separately; a 3D first target of alpha blending
(BLDCNT bit 0 with the alpha effect) isn't blended with the layer behind;
a brightness effect on the 3D applies to the whole 3D area regardless of
windows. `tests/test_render.c` (`test_split_layers`) checks the two layers
put together give the same picture as the renderer draws in one, where the
DS's result is exactly reproducible.

## Settings

`sdmc:/3ds/pokeplatinum/remaster.txt`, a setting a line, `name value`:

| line | meaning |
|------|---------|
| `remaster off` | the classic display (the DS's pictures in the middle of the screens). The default on an Old 3DS. |
| `wide off` | the 3D at the DS's width (320 of the 400 pixels) |
| `filter off` | no Scale2x: the 2D bilinear-filtered from 256x192 |
| `stereo off` / `stereo 0.03` | no depth from the slider / its strength at full slider, in screen widths (default 0.02) |
| `convergence 400` | the distance (the DS's w) that's at the screen's depth, instead of each scene's middle |
| `edges off` | no outlines (edge marking) |
| `flip on` | the 2D layers upside down (if a GPU's texture rows come out the other way round) |

The log says what's on (`[compose] screens drawn by the GPU: ...`) and,
every two seconds, `[perf] display: N frames/s composed by the GPU, X ms of
GPU each; 2D layers published N/s, upscaled in X ms each`.

## 60 frames a second in the 3D

The game runs its field at 30 frames a second: its main loop waits for two
V-blanks, and everything in the field (walking, the camera, animations)
moves in those steps. Showing the field at 60 would mean either changing
the game's own timing (every movement and animation speed), or drawing a
frame of the port's own in between (the camera moved halfway, by running
the scene's commands again with a matrix in between), which costs a second
geometry pass per frame. Neither is in this display path; the second
depends on how fast the geometry thread runs on the real hardware, which
the `[perf] geometry` line measures.
