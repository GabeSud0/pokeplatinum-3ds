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
  In the field the lower screen is a menu instead, with the DS's lower
  screen in a panel (see "The lower screen as a menu").
- **Sharper 3D.** The 3D is drawn by the GPU straight into the screen at
  the screen's own resolution, not at the DS's 256x192 and then enlarged
  (at twice it and scaled down, in fact: no jagged edges). Its fog is
  drawn by the GPU too, and the outlines the game draws round its 3D (the
  DS's edge marking) can be (`edges on`): off by default since 2026-10-20,
  on the bigger screen they get in the picture's way.
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
  Scale2x and the others are settings. The layers in front of the 3D (a
  text box) are 16 bits a texel (RGBA5551: the DS's colours exactly)
  while none of them is partly see-through, 32 when some is (a fade):
  with 32 a text box's quad took the GPU 2-3.7 ms of a frame (2026-10-20,
  since PR #135). The 3D's textures stay the DS's
  (nearest texel) unless `filter3d` says otherwise: made like the 2D (each
  texel 2x2, then bilinear) the GPU took two to three times as long, the
  overworld lagged, and polygons' edges showed lines on the opening's
  mountains (October 2026). `filter3d xbrz` makes them 2x with xBRZ
  instead: edges and curves smooth, small details kept, at 16 bits a texel
  where the DS's format allows and nearest (half sharp's memory and GPU
  reads, no lines), each texture worked out a little each frame (2 ms) and
  shown 2x2 until it's done; one that keeps changing (an animation, a
  fade) waits until it's been still for a second. Faint patterns (grass
  tufts a shade off the grass) are left as they are: xBRZ smooths them
  too, into strokes half a texel wide that mostly fall between the 3DS's
  pixels, and the grass came out flat.
- **The look: sun shadows and a colour grade** (`shadows on`, `grade on`). **The sun shadows are
  parked on the 3DS for now** (see "Sun shadows: parked" below): `shadows
  on` leaves them off there; the PC runner still draws them. Every opaque object casts
  a real shadow from the sun, from its own shape: the scene's opaque
  polygons are drawn once more from the sun into a 512x512 shadow map (the
  PICA's shadow mode: each texel keeps the nearest distance; cut-outs by
  their alpha; `gx3d_shadow_cast.v.pica`). On the 3DS the opaque scene
  is drawn normally, then drawn again with the shadow lookup on texture
  unit 0 and each polygon's cut-out on unit 1
  (`gx3d_shadow_recv.v.pica`). This pass multiplies the existing colour
  by the light, only where its depth equals the visible surface's; stencil
  makes sure overlapping coplanar polygons shade a pixel only once.
  It preserves depth and destination alpha. This is the hardware baseline:
  a nearest-filtered map, no half-size masks, blur quads or mask depth
  buffer. Edges are intentionally sharper while hardware stability is
  tested; the PC `gpusim` still has the earlier mask/blur implementation
  and does not validate the PICA command stream. Only what faces up receives
  (the ground, roofs): on walls,
  characters and trees, made for the camera's one view, their own shadows
  made odd shapes; and what lies flat doesn't cast (a doormat or a path's
  edge cast a stray shape beside it); each triangle's part is kept in its
  vertices' alpha (`NitroPort_ShadowRoles`). What the camera doesn't
  draw still casts: the triangles facing away from it (a building's back)
  and those outside the view's sides, up to one and a half views beyond
  them (8192 at most), are kept for the map only
  (`NitroGx3dScene.shadow_triangles`, made when the shadows are first
  on), and the game's box tests pass what's up to half a view beyond, so a
  building's shadow isn't cut where its back or its top is out of view.
  Characters, signs and the routes' trees are cards facing the camera: the
  sun saw them edge on, their shadows slivers. Each card gets a copy
  turned to face the sun and standing upright, its foot where it stands,
  for the map only (`NitroPort_ShadowCards`; the card itself then neither
  casts nor receives). The game's own painted shadows (see-through dark
  patches lying under and beside buildings) are left out with the sun
  shadows on (`NitroPort_ShadowDecal`): with them there were two shadows. The
  sun is found for each scene in the camera's own space
  (`nitro/src/port_shadow.c`): the vertices go back through the scene's
  projection (`NitroGx3dScene.projection`), the ground is the plane most of
  the scene lies in (cards facing the camera, characters and trees, left
  out), and the sun shines onto it. A scene without a ground (a battle, a
  menu) gets none. The sun follows the clock (the game's: the 3DS's): it
  rises on the right at 5:00 (long shadows to the left), is high at the top
  left at midday (short ones to the bottom right) and sets on the left at
  20:00 (long ones to the right); at night the moon, high at the top left,
  shadows a third as strong. Never lower than 40 degrees: a shadow at most
  about 1.2 times as long as what casts it. It stays at the sides and the top left: from
  the top, the light comes towards the camera and the shadows fall under
  the characters and trees (cards facing the camera), which shade each
  other in stripes; from the bottom, the shadows hide behind what casts
  them. The game's own round shadow under objects is left out with them
  (patch 0027): two shadows looked strange. The grade goes on the 3D's
  textures as they're decoded (white balance, saturation, a gamma, a table
  of the DS's 32768 colours): golden in the morning, warm by day, orange in
  the evening, cool at night, after the game's times of day; the textures
  are decoded again when the time of day changes. `sun 55,32` fixes the
  sun (degrees above the ground; from the screen's left, 0, towards its
  top, 90) instead of the clock. The PC runner's `look` command shows it
  in `gpusim`.
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
A layer behind that's all one colour (in the field, often only the
backdrop) is painted by the frame's clear instead of a quad the size of
the screen (1.3-2 ms of the GPU's an eye); the `[perf] display:` line
says how often.

Edge marking on the GPU (`edges on`; off by default since 2026-10-20, with
the slider up and down: on the bigger screen the outlines get in the
picture's way, and with the slider down they took 8-9 ms of a frame's GPU
time, so it was mostly at 30 frames a second; the smooth 3D below has that
time now). The DS outlines a pixel of an opaque polygon when
a neighbouring pixel belongs to another polygon and is further away: for
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

Those eight passes an eye were 70% of the GPU's time on a field frame
(the `[gpu]` lines, Jubilife, October 2026), and most polygons can't get
an outline anywhere: one gets it only where a neighbouring pixel shows
another polygon ID, or the rear plane (where nothing is drawn: also past
the edges of the DS's screen, where the scene stops). So each pass draws
only the triangles it needs (`NitroPort_Gx3dPruneEdges`,
`port_gx3d_prune.c`, worked out on the CPU once a scene and slider
setting; index lists the GPU reads):

- the outlines drawn: the triangles within two pixels of one of another
  polygon ID, and those (of an ID other than the rear plane's) within two
  pixels of a pixel no solid triangle surely covers (each eye's pixels, a
  bit each; surely: its centre inside by a sixteenth of a pixel, which the
  tenth of a DS pixel every edge is moved out by keeps shared edges
  covered);
- the moved passes: those, and the triangles within two pixels of one;
- along the borders, four zones a few pixels wide past where the scene
  stops (the slider moves it): there the triangles touching a zone are
  drawn with the scissor test on it, rather than all of a big ground
  triangle drawn for an outline at its end.

The picture is the same as with every triangle in every pass:
tests/test_gx3d.c draws both through a model of the passes (the vertex
program's shift, the view matrix, the framebuffer turned a quarter turn,
the scissor) for town and route scenes, slider down and up, the screen
shaking, the lower screen, and compares every pixel; so did scenes from
Jubilife and Route 202 run the same way on a PC. There the outlines'
passes draw about 90% (moved) and 67% (outlines) of what they did in
Jubilife, and 54% and 14% on Route 202. It costs the CPU about 0.1-0.2 ms
a scene on a PC, but on a New 3DS 5-10 ms (`[perf] outlines:`, 2026-10-19),
and in play the moved passes still drew 85-100% of the polygons (D 20-90%):
the GPU saved 1-2 ms a frame, the CPU lost more. So it's off (`edges
pruned` turns it on).

What a moved pass leaves in its buffer is the screen's own depth/stencil
buffer moved a pixel, once the opaque polygons write their polygon ID as
the stencil (they do, with the screen's buffer cleared to the rear plane's
ID). The framebuffer is turned a quarter turn, its lines (240 pixels) run
down the screen, so the neighbours to the left and right are a line away:
the GPU's transfer engine untiles the screen's buffer into a linear one
with a spare line either side (the rear plane: past the screen's edges, as
on the DS) and tiles it back into those two buffers from a line on and a
line back (`NitroPort_PicaEdgeCopies`). Three transfers an eye instead of
the scene drawn twice. Up and down are 4 bytes away, and the engine takes
8-byte addresses: the up one is drawn, and the down one is the up one two
pixels on (8 bytes: untiled, tiled back from two pixels on;
`derive_down_buffer`, since PR #135). Each line's last two pixels come
from the next line's first two, so the outline the DS draws along the
3D's top edge comes a row lower; nothing else changes. Not with the sun shadows
(they use the stencil). `edges drawn` draws all four, as before. On a
New 3DS (2026-10-19) the two drawn passes took half the four's time, and
the copies 1.4 ms an eye through a linear buffer in the linear heap: it's
in VRAM now (the linear heap if there's no room).

With the 3D slider up there were no outlines for a while (PR #128; on
by default again from PR #130 to #135, drawn for each eye after the
smooth 3D, as with the slider down, below; `edges3d on` now).
Drawn for each eye they took 8-13 ms an eye of a 25-43 ms stereo frame
(2026-10-19), for about 3% of the picture (buildings' silhouettes, signs,
a line round the DS's screen), and in 3D the depth sets shapes apart
anyway. Two directions tested instead of four, from copies a knight's move
away (PR #127), saved two thirds of that but looked wrong on a 3DS:
outlines where there was nothing, and too thick on some buildings
(2026-10-20).

With the smooth 3D and outlines in each eye (PR #130) a stereo frame took
the GPU 28-35 ms in towns and 40-51 ms on busier routes (19-21 frames a
second there), 9-17 ms without the outlines (2026-10-20). So with the
slider up they're drawn while they fit (`auto_outlines`,
port_compose_pica.c): off after 3 stereo frames in a row over 33 ms (a
frame at 30 a second), on again after 45 in a row (1.5 s) that would take
under 29 ms with them: the frame without them times what they cost the
last time they went off or on (the frames either side of that much the
same scene; about 3 times). Each switch is a `[compose]` log line, and
`[perf] display` says how many of the stereo frames had them.

That time goes to a smoother 3D instead (`smooth3d`, on by default): the
upper screen is drawn at twice the resolution each way,
480 x 800 (the framebuffer's way round), and the transfer engine averages
each 2x2 pixels into one as it copies the eye into its LCD framebuffer
(citro3d's anti-aliasing: the output's size given as the input's), so
polygons' edges come out smooth, not jagged. One framebuffer serves both
eyes: the left eye drawn, copied out, the framebuffer cleared, then the
right one. Its depth/stencil is the edge buffers' VRAM (one block now:
4 x 240 x 400 x 4 bytes, exactly 480 x 800 D24S8), idle with the slider up;
its colour 1.5 MB more VRAM, made room for by the classic display's
render target, now made only if the classic display is used. Since PR
#135 the upper screen's framebuffers (the eyes' and the smooth 3D's) are
RGB565, as its LCD framebuffers are: half the bytes to clear, draw and
copy (and half the smooth 3D's VRAM), the same picture on the LCD but for
the blends' and averages' rounding at 5-6 bits; nothing reads their alpha
(every blend is by the source's alpha or a constant). Every pixel
is drawn four times over: the 3D, and the 2D in front of and behind it.
The 2D in front of the 3D (a text box) is drawn only over the rows with
anything in them (with the slider down too). On a New 3DS (2026-10-20)
the opaque polygons took 3.3-5.5 ms an eye drawn 2x2 (1.1-2.1 at the
screen's resolution), a stereo frame 9-14 ms of GPU in Jubilife and on
Route 202: less than the outlines had taken.

With outlines (`edges on`: the slider down; `edges3d on` too, each eye
in turn, the 2x2 framebuffer cleared between them) the frame is split. The picture
behind and the opaque polygons are drawn 2x2 as above, then scaled down
into the screen's own framebuffer (the eye's) (through a linear copy in the smooth
depth's memory, which is done with by then: two transfers); there the
opaque polygons are drawn again for their depth and IDs alone (no
colour), the edge buffers filled only then (that memory is theirs), and
the outlines, the translucent polygons and the 2D in front go on at the
screen's resolution. Every polygon edge is smooth, the outlines one pixel
wide as before (their outer side softened by the smooth pixels next to
them). `smooth3d stereo` keeps the smooth 3D to the slider up. A frame
with the slider up and outlines took about 2 x 12.5 ms of the GPU's by
the measures so far (2026-10-20): the frames in flight (Timing) are what
let it keep 30 frames a second.

The 3D's textures are RGBA5551 where that's exact (every DS format but
A3I5 and A5I3, sampled nearest, as `filter3d 0` has it: the DS's colours
are 5 bits a channel, those formats' texels opaque or not there), RGBA8
otherwise. A frame draws the opaque
polygons nine times with outlines on, those with see-through texels
textured each time: in tall grass and among trees the GPU took 22-30 ms a
frame of the 33 (2026-10-15), and RGBA8 textures were twice the memory to
read.

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

The field at the DS's pixel size sees 24 lines more above the DS's top
row, further away than the DS ever shows, and the fog (made for the DS's
view) went on thickening there: the mountains and buildings at the top of
the screen washed out to the fog's colour, as if cut off (Route 206's
Cycling Road, 2026-11-01). With a view taller than the DS's, the fog is
held at the DS's top row's: the farthest depth within its 192 rows (the
scene's vertices there, and where their triangles' edges cross the rows'
top and bottom: `ds_rows_farthest`), the table's entries beyond it as at
it, the rear plane no thicker. Logged now and then: `[compose] the taller
view's fog held at the DS's top row's`.

A scene is drawn with the 3D registers the game had at its SWAP_BUFFERS
(DISP3DCNT and 0x320-0x3BF: the clear colour, the fog, the outlines'
colours; noted on the game's thread with the swap:
`NitroGx3dScene.disp3dcnt`, `regs3d`), not as they are when it's drawn, a
few V-blanks later.

The vertex program (`gx3d_passthrough.v.pica`) does the view (where the
DS's screen goes on the 3DS's turned framebuffer) and the stereoscopic
shift: `x += strength * max(w - convergence, -w)`, for perspective
vertices (w > 1) only. The convergence (the depth that's at the screen)
is the middle of where most of a scene's perspective vertices are: the
geometric mean of the 5th and the 95th percentile of their w, so a few
polygons near the camera or far away don't move it; and the eyes follow
it a tenth of the way (in octaves) each new scene, not at once, and not
again for the in-between scenes. Taken from the nearest and the farthest
vertex, each scene, it jumped when something came on screen, and every
polygon's shift with it: the geometry flickered with the slider up.

The GPU's command list is 1 MB; a draw that wouldn't fit is left out
rather than overflow it (an overflow stopped the GPU thread on the real
3DS). The `[perf] display` line says how full it got.

Publishing a 2D picture (the drawing threads): the 15-bit pixels to
RGB565, each doubled into the tiled texture (a 4x4 block of pixels is one
8x8 tile), and the layers in front to RGBA8 with the alpha multiplied in.
With the sharp filter (the default) only what changed is done again
(`NitroPort_PublishDoubled`): each engine's last picture is kept, and each
band of 4 rows (a row of the texture's tiles) remembers the publish it
last changed in; a set of textures gets the bands changed since it was
last written, and only those are flushed. Most changes are small (a
location's name sliding in, a text box, a cursor): done whole, engine A's
picture took ~10 ms a frame on the core the 3D's geometry shares, and
frames came late while a name slid in. The other filters publish the
whole picture, the layers in front only when there's anything there.

What counts as a change of the picture (the drawers draw again only
then) leaves out DISP3DCNT's two acknowledge bits: the game writes them
every frame, alternating, and the field's upper screen was drawn and
published again every other frame with nothing on it changed.

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

### H-blank effects: the screen changed line by line

Some of the game's effects change the display's registers while the DS
draws the screen, between lines (an H-blank interrupt handler, or a DMA
that writes a register at every line): the start of a battle slices the
screen into bands that slide apart (the grass), splits it (a trainer),
waves it (water, caves); some moves wave the battle's background. The
port draws a whole frame at once, so it goes through the frame's lines
first (`NitroPort_HBlankLinesOnce`, port_os.c, on the game's thread before
the display state is taken; once a V-blank: the game arms its H-blank
DMAs again only in its V-blank interrupt): before each line it runs that
H-blank's DMAs and interrupt handler, with the V-count the game reads for
it and DISPSTAT's H-blank flag set (some handlers check it), and keeps
each line's registers (`NitroPort_LineRegs`: each engine's first 0x70
bytes, but DISPSTAT and the V-count). The 2D renderer draws each line with
its own BG scroll, windows and affine matrices, and gives the compositor
the 3D's scroll on each line (`scroll3dLine`). The compositor draws the
3D a run of lines at a time, each run moved by its scroll and drawn
inside its rows alone (the viewport of its rows, the view moved to match,
so the GPU clips the polygons to the run: `PicaView.band`; and the
scissor, set again by each of the scene's passes: `PicaView.scissor`; the
scissor alone still had every run's polygons gone through everywhere, 62
ms of the GPU a frame at a battle's start), its rear plane and its
brightness tint too:
at most 24 runs (12 an eye with the slider up), and no more than the GPU's
command list has room for (each run is the scene's commands again); more
lines are put together by an odd number of runs (lines sliding apart by
turns stay apart by turns). Frames with runs aren't counted by the 60
frames a second steps. Each run draws only the scene's triangles that can
reach its rows (`NitroPort_Gx3dTriangleRows`, once a scene: each
triangle's rows as -y / w of its corners; `NitroPort_Gx3dBandRanges`:
each batch's parts with them, the run 2 rows more each side; a corner not
in front of the camera, every run): the GPU clipped the others away, but
went through all of them in every run, the whole scene's vertices and
state 24 times a frame. The grass's slices took the GPU 17.1 ms a frame
and the display thread 14.3 ms building it (a log of 2026-11-04: 40
frames a second at the wild Pokémon's battle start, less with the slider
up); on the PC's scenes of it each run now has about a tenth of the
triangles and a quarter of the batches. `test_band_ranges`: a run's
pixels (depth and polygon ID, drawn as the GPU puts them) the same as
with every triangle, the slider up or down, the 3D moved past the edge.
That still left each run about 0.3 ms of the GPU beyond its pixels (the
next log: 15.1 ms a frame, the slices moving 15 to 30 times a second, 10
or 18 lines high instead of 2). So where it can, the compositor draws the
3D once an eye into a texture of its own (`draw_slices`: 256 x 512, the
screen's 240 x 400 at its corner, the GPU's framebuffer there too, the
viewport only those: `PicaView.viewWidth`), unscrolled, as the screen
would have it (its rear plane, its polygons), then every one of the DS's
runs from it at its own scroll, a textured quad each, in one draw, and
the 3D's tint over the same quads: the grass's 96 slices of 2 lines
exactly (each moved by whole pixels, so every pixel takes one texel's
middle). What's behind the 3D: with a rear plane that hides it, or all
one colour (the texture filled with it), the texture is all there is;
else, the texture RGBA8 (filled with alpha 0) and no rear plane (the
field's: its clear colour's alpha is 0), the 3D's empty pixels are left
out of the quads (the alpha test) and the screen's own picture shows
there. Only on the upper screen, the 3D inside it, no outlines; else the
runs as above. The first log (2026-11-04) had a battle's start with
what's behind the 3D not one colour: the stretched picture's (the log
now says where, once: `[compose] a battle's start: behind the 3D`). It's taken even where the command list had room
for only one run (that run would have drawn the effect unsliced). With
the slider up the right eye goes the left eye's way. The
log says the way each time it changes (`[compose] an H-blank effect's 3D
(96 runs of lines) drawn from its texture`, or why not), 24 times at
most. The 3D is at the screen's own pixels then (the
2x2 smooth 3D's texels doubled) for the moment of the effect. The texture
is made the first time it's needed, when VRAM has room for it (RGBA8, 1
MB with its depth/stencil; else RGB565, 768 KB, only for the first two
cases) and 512 KB more (the outlines' copies come later), its quads in linear
memory of their own (126 KB); the log says which (`[compose] an H-blank
effect's 3D drawn once into a texture`, or `no room for the H-blank
effects' texture`: the runs as above).
The game moves the slices 30 times a second (once a game step). At 60
frames a second they move each frame, halfway in the in-between frames, as
the 3D's camera does; their black edges go with them. Those edges are the
game's windows (window 0 the part a slice moved one way shows, window 1 the
part of one moved the other way it hides), so they're left out of the
picture for the compositor to draw (`NitroRender2DSplit.gpuSpans`): on a
battle's start stretched over the screen, with the 3D's lines scrolled
apart, where every line's windows show everything (the colour effects too)
or nothing, in one part at most, no OBJ window or circle, something is
hidden, and nothing is in front of the 3D (a trainer's battle start slides
two halves with the halves of a Poké Ball, sprites the picture draws: the
windows stay in the picture then, all of it at 30 as before) (`gpuSpansOn`,
each line's shown part `spanLo` to `spanHi`). The
picture's `over` then shows nothing where the windows hide the 3D, rather
than what's behind it, and the compositor draws the backdrop there after
the layers in front (`draw_window_edges`), on the lines the 3D was drawn
with. The game's thread notes each frame's lines (each line's 3D scroll and
shown part, `NitroPort_Render2DLineSpansNow`, while a battle's start plays:
`NitroPort_ComposeNoteLines`), found by the scene's frame as the BGs'
scrolls are; the slices from the texture and their edges are those of the
frame shown, and in an in-between frame halfway from the frame before's
(`NitroPort_LineSpansHalfway`: each line's scroll the shorter way round,
its scroll finer than a pixel, more than 128 pixels after's; a part's
edges halfway, a part shrinking away to nothing going towards the side it
touched; a side hidden in one frame only no further than the line's 3D,
moved halfway, leaves uncovered: the grass's slices change sides as they
start going apart, from 3 pixels one way to 29 the other, and the 4
columns hidden on the left before would have covered 2 of the 3D moved 13
the other way). A new scene without an in-between one gets an in-between
frame of its own when the slices moved (`lines_moved`: slices in both
frames, as the sprites). The lines are noted only while the upper
screen's picture is a battle's start stretched.
Drawn a run at a time (no texture, the outlines), the slices and their
edges are the picture's, as before. The PC runner's `pshot` draws the
edges as the 3DS does, the same pictures as with them in `over`;
`test_gpu_spans` has the grass's lines as the game sets them.
The colour effects and OAM are the frame's (no
effect of the game's changes those by line). Without an H-blank interrupt
or DMA nothing of this runs; in the field the game keeps its H-blank
interrupt on, so its handler runs 193 times a frame there (with nothing to
do: the lines don't differ, and nothing is drawn differently). Without the
remaster (the drawers read the game's live state) the lines are gone
through only while no drawer reads them.

Before this (2026-10-21) the effects showed as the frame's last line had
the registers: the whole screen jumped, then went black.

Not yet: display capture (the legendary Pokémon's motion blur at the
start of their battles), the main-memory display mode.

## The wider screen

The upper screen's 2D is drawn on a canvas 320 DS pixels wide, the DS's
x = 0 at 32 (`NitroRender2DSplit.width`/`origin`, laid out by
`NitroPort_WideLayout` in `port_wide.c`). The rules, so that nothing is
stretched:

- **Hand-drawn bands win.** A layer (a BG, or a group of sprites) with a
  picture drawn for it shows that picture in its bands (or all over): see
  [WIDE_ART.md](WIDE_ART.md).
- **The DS's layout stays, but for the edges.** The game's windows,
  menus and sprites keep their places, but for the windows that go to the
  screen's edges (below), which take their sprites with them (moved apart,
  a menu's icons and its window came apart). Nothing is stretched.
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
- **The UI's backgrounds go on beside the DS's screen.** Where a line of
  a BG has something at the DS's edge that isn't plain (a menu's panel
  reaching it), the plain lines next to it are carried on through it, in
  their pattern (every 1 to 8 lines: the party list's stripes go on
  behind its panels). Where a BG has nothing at the edge, nothing goes
  beside it (the backdrop, or the 3D). (Before October 2026, nothing went
  beside a line that wasn't plain, and the party list, the summary and
  the bag had black blocks in the bands.)
- **Pictures with plain middles and decorated sides** (the intro's red
  bars) are split: each half goes to its screen edge and the plain middle
  widens.
- **The DS's windows** (its hardware windows, WIN0/1) at the DS's edges go
  on to the screen's: the intro's letterbox covers the bands too. A window
  from 0 to 255 counts as reaching the right edge (the register can't say
  256, so it leaves the DS's last column out; the bands went black beside
  the intro's city). An empty window (0 to 0: the circle wipes' lines
  past their circle) covers nothing, the bands included (the Town Map's
  side bars showed in them while its circle closed). Where a circle is
  wider than the DS's screen, the game's window stops at its edges, so
  for those few lines the bands are inside it.
- **The battle background** is a 512-pixel map (the trainer's slide-in
  scrolls across it): the picture, then the same picture mirrored. After
  the intro the game draws the platforms (the terrain's sprites) into it
  (`BattleSystem_BakeSpritesToBackground`), and puts the player's
  platform's left part just left of the picture, where the map goes on
  past the DS's left edge. Beside the DS's screen the map's own pixels show
  where it has a picture of its own (that platform part, the same for
  every terrain), and where it only has the mirror, the map's pixel next
  to the DS's edge goes on: the background's own edge, from before the
  platforms (the DS's edge pixel is a platform's where one reaches the
  edge: the Elite Four's, on the right). Until 2026-10-16 the bands
  repeated the DS's edge pixel, and the player's platform was drawn out
  into lines.
- **The place name sign** goes to the screen's left edge while it shows:
  the game tells the port (`patches/pokeplatinum/0014`,
  `NitroPort_WindowAnchor`; patch 0012 tells it where the windows are).
- **Menus by the DS's edge go to the screen's edge.** The game tells the
  port which windows have a frame drawn round them (patch 0015,
  `NitroPort_WindowFramed`): a menu's (a tile all round) or a text box's
  (2 tiles left, 3 right). A framed window within 2 tiles of the DS's left
  or right edge goes to the wider screen's edge, as far from it as it was
  from the DS's (the field menu, YES/NO, the save's summary, a shop's
  list, the party's menus); one in the middle, or across the screen,
  stays. The sprites over a moved window go with it (the field menu's
  icons).
- **The battle's text box** has its frame's edges clear and the ground
  under it (patch 0040, 2026-10-25): the game fills a frame's clear pixels
  with its colour 10 in battles (black in 19 of the 20 frames), and the
  battle's background has nothing under the box (rows 20-23), so a black
  box showed behind it, wider for the wider box. Now the clear pixels stay
  clear and the background's last row of pictures (the ground, row 19) is
  carried on down through rows 20-23.
- **Text boxes span the screen.** A text box across the DS's screen has
  its frame's left part at the screen's left edge and its right part at
  the right edge, each line of the frame's inner columns drawn on between
  them; the text starts a pixel inside the frame's left part (2026-10-16,
  from a drawing of the PC's text box: the room the wider screen gives is
  used, the text at the left as on the DS). Its lines stay as the game
  broke them. What's drawn on right of the text is the window's own last
  column (its top and bottom borders and the box's inside), whatever the
  frame (2026-10-25): it was the frame's right part's first column, which
  in many of the 20 frames is the frame's own drawing, a red or dark band
  across the box (the battle's text box after the frame was changed).
  On a line of text that reaches that last column it's the inside's plain
  colour instead, from the nearest column left of the letters that has it
  (the colour of the nearest line above or below with nothing on it in
  the window's last 24 columns, 16 lines at most: `WidenLinePlain`):
  Camper Jacob's "Hey, how about you and me battle before" on Route 205
  ends in the last column, and its e was drawn on to the box's right as a
  grey line (2026-11-06). On the PC that box now has nothing past the e.
  In `test_render.c`: `test_wide_framed_windows` (a letter's column there,
  and a line red all along the last 24 columns: white drawn on).
- **Screens redrawn for the wider screen.** For a screen with a reference
  drawn for the wider screen, the game lays the screen out in parts that
  move apart (`NitroPort_WideSplitScreen`): on given lines of given BGs,
  what's left of a column moves one way and what's right of it the other,
  with that column drawn on between them (a card's body, widened) or
  nothing (the text over it). Only the DS's 256 columns move, nothing from
  beyond them. On an affine BG the parts are laid out on the BG's own
  picture, before its matrix, so a card that turns over keeps its layout
  as it turns; its hand-drawn picture, if there is one, goes through the
  matrix too. The trainer card (patch 0016, 2026-10-15): its body is 31
  pixels wider each side; the fields and their values sit at its left
  edge and the photo at its right; TIME, ADVENTURE STARTED and the back's
  rows have their labels at the left and their values at the right; the
  signature stays in the middle of its widened box.
- **Screens laid out in parts** (`NitroPort_WideScreenParts`, patches
  0017-0022, 2026-10-16). The splits are a case of these. An app gives
  the port its screen as a list of parts, when the screen changes, and
  none when it ends (`include/port_wide_layout.h` in the decompilation):
  a BG's columns moved (MOVE), a BG's column drawn on where others would
  be (FILL), the sprites in a rectangle moved (OBJS; stretched: their edge
  column drawn back to where they were, a cursor as wide as its row), the
  3D moved (SHIFT3D). On a BG's line with parts, the DS's picture shows
  where it is but for the columns MOVEs take, and each part is drawn over
  it in turn (a column shows the last part over it); beside the DS's
  screen, only what parts put there. Lines and columns are the BG's
  picture's (they go with its scroll), or the DS's screen's (a box's
  wallpapers sliding past behind a fixed edge). A moved BG shows its own
  pixels, not a hand-drawn picture of it, and moved sprites themselves;
  a screen's static backgrounds keep their pictures (the Pokédex's pages).
  But a text BG's picture made right where it is (a capture of the screen
  as its parts lay it out, drawn on in the editor: the boxes' panel's
  edges, cleaned, 2026-10-16) is that layer: all of it (a whole-layer
  picture), or its bands with the parts between.
  From references drawn for the wider screen:
  - the summary: the left column (name, picture, item) toward the left
    edge, each page's panel widened to the right edge with its values
    there, the move details, the memo and contest pages, the exit page;
  - the party: the panels 32 to the right, the background widened left;
  - the bag: the bag and its pockets left, the list widened to the right
    edge (counts at the edge, the cursor as wide as its row), the
    description box widened across, its text by the item's icon;
  - the boxes: the panel of the Pokémon looked at at the left edge, the
    box at the right edge (its party tray, icons, hand and buttons with
    it), the wallpaper's edge between; the boxes to jump to and an item's
    description across the middle; the message box widened like a text
    box, the menus at the right edge;
  - the Pokédex's list: its label and picture left, its rows and scroll
    bar right, the counts apart.
  - the Pokédex's page after a catch (patch 0040, 2026-10-25), in the
    battle: its maps are 256 wide, so they came round beside the DS's
    screen (the entry's picture frame and text box over again). Now as the
    info page's picture has it: the banner on to the edges, the
    background's pattern beside the DS's screen, the text box's ends at the
    screen's edges and its middle drawn on between.
  - the Pokédex's cry page (patch 0040, 2026-10-25): the wave from edge to
    edge. The DS draws it a few columns a frame into a 256-wide map it
    scrolls, the oldest columns coming round next to the newest at its
    right (a glitch at the wave's end); here it's drawn into a 512-wide
    window of its own, the newest columns at the wider screen's right edge,
    the map shown on beside the DS's screen (SHOW parts, below); the rest
    of that map is a clear tile. Its band's lower edge is 3 lines lower, at
    the screen's bottom (the DS has 3 lines of the page's background under
    it, which looked like a mistake there), drawn on the wave's layer (shown
    beside the DS's screen and faded with the page), so the background
    layer's hand-drawn picture still fits it.
  - the shop's list (patch 0026, 2026-10-17): its box 31 pixels wider
    to the right, the prices at its right edge, the cursor as wide as its
    row, the scroll arrows at its middle; the green panel below across the
    screen (the money's and the quantity's windows go by the screen's
    edges, as windows do). The list's text layer changes with every item
    (its description), so a picture of it can't stand for the layout.
  A layout can also draw pictures of its own (PICTURE parts, patches 0023
  and 0024: files in the wide art folder, see [WIDE_ART.md](WIDE_ART.md)),
  with a BG, in front of its pixels, anywhere on the wider screen (moved
  from there in the editor: the file says how far), turned about the
  middle of what's drawn in them: the boxes' divider and its Poké Ball, which turns
  once round as the boxes slide; the party's panel at the left edge, its
  Poké Ball turning a little as the cursor moves, its lights changing now
  and then. A layout sent again unchanged (an animation's still frames)
  draws nothing again. A SHOW part (2026-10-25) shows a BG's lines across
  the whole wider screen, its map going on beside the DS's screen as it
  scrolls (a picture wider than the DS's screen in a map wider than it:
  the cry page's wave).
- **The battle's health boxes** go to the screen's edges, the player's
  right and the opponent's left (`patches/pokeplatinum/0013`, by
  `NitroPort_WideScreenExtra`). A level up's stats (a framed menu by the
  DS's right edge) go to the screen's right edge, and since 2026-10-25 the
  box above them with the Pokémon's icon, name and level with them
  (`patches/pokeplatinum/0040`; it stayed where the DS has it).
- **Fixes from a playthrough** (patch 0042, 2026-10-26):
  - a text box taken off and put back between messages (Professor
    Rowan's opening speech) keeps its widened frame while it's away
    (`port_wide.c`: the box is remembered till another window is put over
    it, its BG is set up anew or freed, or the screen changes), so it
    doesn't narrow and widen with every
    message; the icon at its top right that says which screen to look at
    (`Text_RenderScreenIndicator`, `NitroPort_WindowIndicator`) goes to the
    widened box's right edge, where the DS has it in its box;
  - the Pokédex's registration page: the Pokémon's box (and the 3D
    Pokémon in it) 24 pixels to the left, the title 24 to the right;
  - the evolution's background: a 512-wide map whose right half came
    round beside the DS's screen; its edge columns carried on instead,
    and since 2026-10-26 hand-drawn bands (`wideart/6570e913eaa0988b_0_0.png`:
    while its palette is the evolution's teal; fading in and in the white
    flash, the edge columns). After a battle the battle's overlay is
    still loaded: its BG3 is left unclipped (its own map beside the DS's
    screen) only without a screen laid out in parts, so the evolution's
    picture is looked up (`NitroPort_WideLayout`);
  - the egg hatching (patch 0063, 2026-11-01): its background is the
    evolution's (a 256-wide map), with the evolution's hand-drawn bands
    (`wideart/49fbb4dbe2cd6dcb_0_0.png`, a copy of
    `6570e913eaa0988b_0_0.png`, while its palette is the teal) and else
    its edge columns carried on; its black bars above and below the egg
    (sprites across the DS's screen, sliding off as it hatches) reach the
    wider screen's edges (`WIDE_OBJS_STRETCH`: the ones at the DS's sides
    moved out, their edge column drawn back), over the widened text box
    (beside the DS's bars it showed, broken off);
  - the battle's trainers slide off (the player after throwing the ball,
    the opponent) till they're past the wider screen's edges, not the
    DS's (they vanished in the sides); since 2026-10-26 (patch 0043) till
    all of the sprite is (it reaches 60 pixels on from its position: the
    player went with 20 pixels of it still in the left band); past the
    DS's -40 the player's Poké Ball is left to its arc as on the DS (patch
    0052: the trainer's hand took it off the screen for the 4 frames it
    hangs at the top, and it vanished);
  - the HM cut-in: its band of Poké Balls (BG3, a 256-pixel map) repeats
    to the screen's edges (a repeating BG of the field's,
    `NitroPort_RepeatingBg`, as the weather's; a SHOW part until
    2026-10-30, but a screen laid out in parts takes the field out of
    PIXEL PERFECT, and the field zoomed in for the cut-in and back out)
    instead of a stripe of its plain lines; the Pokémon waits beyond the
    wider screen's right edge and slides off past its left one (it sat
    still in the right side); its wind (the white streaks) blows across
    the wider screen too (`NitroPort_SetFieldCells` while it plays: in the
    field the cells beside the DS's screen are otherwise left out, as the
    menus park sprites there; each streak shown left of the screen past
    the right band, as the DS's 9-bit sprite x wraps); its band of rings
    goes on as drawn for the wider screen (2026-10-30: three more rings
    each side, fading in two darker shades), made from the game's own band
    as the cut-in starts (its middle ring's shape stamped behind the
    band's rings; BG3's map made 512 wide, the sides in its second half;
    `CutIn_PortWideBand`, patch 0058); BG3 hidden for one of the game's
    frames before the band's VRAM is written as it starts, and again
    before it's put back as it ends (patch 0059: the picture of the frame
    before is drawn while the game goes on, the VRAM read as it is, and
    the band showed in it white and red, the text box's palette, no
    window: 2026-10-30, most often with A speeding the text up); its
    sprites drawn by the compositor at 60 frames a second (see GPU
    sprites, below);
  - the gym leaders' battle intro (patch 0058): the leader's picture, its
    art ending at the DS screen's right edge, slides to the wider screen's
    right edge, not the DS's (it looked cut off, the band going on beside
    it); the name stays by the VS;
  - the people the scripts bring on and send off (`scrcmd.c`): the
    scripts put them just past the DS's edge, where they popped up,
    vanished or walked through things in the wider screen's sides. One
    added beside the DS's picture but not beyond the wider one's is put
    beyond it, 12 tiles from the middle (the 2 tiles more each side, the
    sprite's width, the perspective), and its first steps in take it the
    extra tiles; one whose walk out ends off the DS's picture (9 tiles or
    more from the middle) walks the extra tiles out before the script
    takes it away; one walking straight out through something the DS's
    picture didn't show (the ground's collisions, ledges, cliffs; not the
    other people, who walk out together) goes round it (outwards first,
    else up, else down: Roark leaving the mine after the boulder). When
    the script ends (or the next starts), the ones it didn't take away go
    back where it put them. Only walks east and west change (the screen
    isn't taller). Since 2026-10-26: one whose script turns it or waits
    before its first steps (Professor Rowan, Dawn, Looker) is moved out
    before it's drawn, so it isn't seen standing where the script put it
    first; the way round something also goes round the people standing
    there (not the ones the script sets walking with it: its
    ApplyMovement commands before the WaitMovement). Since 2026-11-03
    (patch 0072): nor the player's follower Pokémon or its Poké Ball,
    which the DS doesn't have (behind the player in Mt. Coronet as Cyrus
    left, it had his walk left as the DS has it: he vanished in the middle
    of the wider screen; now he walks on toward the exit and out of
    sight). Since patch 0089 a way round that runs outwards, then up or
    down with something in the way beside it every step, out past the
    picture's top or bottom, turns a tile sooner and walks a tile in from
    it, if that's clear: Cyrus went 11 west to the cave's west wall and
    north along it (2026-11-05: "just move him one space to the right so
    he walks in the middle"); on the PC he now goes north at x 5, between
    the wall and the pool, not 4. Since patch 0091 that also goes for a
    way round that turns before its last run outwards: Roark leaving
    Oreburgh Mine's B2F after his Rock Smash went east to a crate, a step
    north round it, 2 east to the mine's wall and north along it; on the
    PC he now goes 1 east past the crate and north up the middle of the
    passage, x 28, not 29 (asked for with a photo of the spot, 2026-11-06).
    And a way round that ends past the top or bottom goes on until the one
    walking is out of the camera's sight there (`ScrCmd_PortWidePastEdge`:
    `ScrCmd_PortTallRows`' feet and shadow above the taller field's top, its
    head below its bottom), when there's a way that far, else as far as
    before: Roark was taken away 9 rows above the middle with his feet on
    the screen; on the PC he now walks a row further, to 28,19, and off;
  - Route 201's Pokémon Breeder (who stands just past the DS's left
    edge, on the way Rowan and Dawn come in from Twinleaf Town) stands 5
    tiles further right (`map_header_data.c`, `sPortWideMoves`): in the
    wider screen's edge he was cut in half, and they walked through him;
  - the Pokédex's registration page (2026-10-26): its left edge carries
    the page's border on, not the arc round the footprint again; the
    entry's text is wrapped again across a box as wide as the screen's
    (up to 240 pixels: `InfoMain_PortRewrap`, at the spaces; a word the
    DS split at its hyphen joined again) instead of
    the DS's lines squeezed in its middle;
  - the Pokédex's page after a catch (patch 0043): its background shows
    no hand-drawn picture (`WIDE_NO_ART`): as its header blinks, the art
    is at times the info page's, whose built-in picture is laid out as that
    page is, and the page flashed between the two layouts;
  - selling in a Poké Mart with the lower screen's menu: the store stays
    on the upper screen (it was black while the bag was up);
- **OBJs** (sprites) placed past the DS's edges show.
- **The 3D** is drawn wide (the camera widened) only while it's on the
  upper screen; the intro's flyover is on the lower screen, at the DS's
  width.

### The field at the DS's pixel size (PIXEL PERFECT)

An option on the settings page (on by default, `pixel_field` in
`options.txt`): in the field the upper screen's 400 x 240 pixels are each
one of the DS's, nothing scaled, so the pixel art stays exact; the view
shows more of the world instead of making it bigger (2026-10-27). In the
field only (`NitroPort_PixelField`: overlay5 loaded, not a battle, not a
screen laid out in parts or made of pictures); everywhere else the wider
screen's 320 x 192 at 1.25 times, as above.

- **The 3D**: the view 400 x 240 (`NitroPort_Gx3dSetView`): clip x scaled
  by 256 / 400 and y by 192 / 240 (with the DS's full-size viewport), so
  the game's own box tests pass what that shows. Each scene says its size
  (`screen_width`, `screen_height`); the compositor draws a scene at a
  pixel a DS pixel at the screen's height (1.25 for 192 lines, 1 for 240),
  whatever the 2D's picture is.
- **The 2D**: a 400 x 240 picture, the DS's in its middle (x 0 at 72, line
  0 at 24: `NitroRender2DSplit.top`). The renderer draws the lines above
  and below as the DS would were its screen taller: the BGs' maps (VOFS
  less 24), the OBJs (y plus 24, mod 256: one placed just above the DS's
  screen shows above it), the H-blank effects' lines (the first's above,
  the last's below), the windows (one from the DS's top or to its bottom
  from the picture's). A clipped BG's map past the DS's lines isn't shown
  (the field's text BG has leftover tiles there) unless its first or last
  line is one colour across (a fade, Flash's dark): that line goes on. The
  weather's BG (unclipped) goes on as its map does. No hand-drawn wide art
  here (it's for the 320).
- **The compositor**: that picture at a pixel a pixel (`ComposeLayers.top`,
  `PIC_SCALE`), the layers in front of the 3D on all 240 lines; the
  weather's BG drawn by the GPU from line -24 of its map.
- **The sprites a texel a pixel** (`port_gx3d_vertex.c`,
  `sprite_texel_a_pixel`): the people, the Pokémon and the item balls are
  billboards, quads facing the camera, and its perspective still makes
  them a little bigger or smaller than their texels (a person near the
  screen's bottom 32 texels over 35.7 pixels) and puts them a fraction of
  a pixel off: the DS's nearest-texel drawing doubled or dropped a texel
  here and there, and the smooth 3D (drawn 2x2 and averaged) blended the
  texels' edges into the pixels beside them, blurring the pixel art
  (2026-10-29). Here a textured triangle with its three corners at one
  depth, upright and within 0.9-1.2 of a pixel a texel, is laid out at
  a texel a pixel, the texture's bottom middle (the feet) on the nearest
  pixel corner, its edges not moved out: each texel whole pixels, sharp
  with the smooth 3D or without; the 3D (buildings, signs) as smooth as
  before. From 0.75 to 0.9 and 1.2 to 1.4 its size eases from its own to
  a texel a pixel (`sprite_scale`): with 0.85 the line, the people and
  berry trees under Route 206's bridge, further from the camera than the
  road's (about 0.85; the road's own at the screen's top are 0.89),
  jumped by a seventh between the two sizes as they went up and down the
  screen (2026-11-02). One scaled on purpose (a Pokémon growing out of
  its ball, below 0.75) is left as it is; the 1.25x field too (a texel 1.25 pixels there), and
  with an orthographic camera (the interiors: Pokémon Centers, shops,
  Eterna's gym) every polygon is at one depth and about a texel a pixel,
  the room's own floor, fences and paths as much as its people: each laid
  out on its own nearest pixel, Gardenia's gym came apart (2026-10-30).
  There only the people's are: lit, and the whole of their texture once
  (`sprite_whole`; the rooms' own polygons are never lit, in every
  interior looked at: the gym, a Pokémon Center, the Mart, the Cycle Shop,
  houses). Each
  is on the nearest pixel corner while it's still on the screen or moves
  a whole number of pixels a frame (the player, whom the camera follows;
  the people on the player's row). One moving by a fraction of a pixel
  a frame goes on the nearest half pixel that way instead (and a 64th
  on, so drawn as it is no pixel's middle falls on a texel's edge): the
  camera's perspective moves each row of the ground by its own amount
  (running in Eterna: 1.94 pixels a frame at the screen's top, 2 at the
  player's row, 2.2 at the bottom), and on whole pixels a person there
  fell behind its ground and caught up a pixel at a time, about 10 times
  a second near the bottom (the jiggle, seen in a video at 120 frames a
  second, 2026-10-30). On half pixels it moves with its ground; drawn
  2x2 its texels are still whole pixels there, and scaled down a person
  is sharp or half a pixel soft that way only, while it moves; and on the
  half pixel only while it's within a sixth of a pixel of it, the nearest
  whole one the rest (`soft_place`): soft a third of the time as it goes
  by, not half, and a third of a pixel off its ground at most (asked a
  little more subtle, 2026-10-30). Which way
  each moves is the in-between scenes' finding (`port_gx3d.c`,
  `soft_of`: half its move since the scene before, off a whole number by
  more than 0.02 of a pixel), kept with its triangles
  (`NitroGx3dTriangle.soft`) in the scene and its in-between scene. (All
  of them on their exact fraction, every way, was tried first and taken
  back, 2026-10-29: everything soft, still or not.) The flatter camera
  that keeps every row moving alike (sharp and locked, but the buildings
  standing straight) was shown and not chosen (2026-10-30).
- **The game** reads `NitroPort_WideScreenExtra` as 72 there: the people the
  scripts bring on from the sides come from beyond the 400 pixels.
- **The game's windows** go to the picture's top or bottom
  (`DecideTallWindows`): those wholly in the DS's lower half 24 lines down
  (a text box at the screen's bottom, YES/NO with it), those wholly in its
  upper half 24 up (the place name sign at the top), the OBJs over them
  with them; the lines they leave are drawn from nowhere.
- **Past a forest's edge** (patch 0060): by Eterna Forest's east edge
  (its way out to Route 205) the 400 pixels went past the last of its
  maps, where there's no map, and showed black. With the player in the
  half of a map by its matrix's left or right edge, the maps of that
  column are drawn again past it, mirrored left to right about the edge,
  props too (`Port_RenderMirroredEdges` in land_data.c,
  `MapPropManager_RenderMirrored`): the trees by the edge go on past it.
  A mirrored model's faces turn the other way, so its materials' culled
  side is swapped while it's drawn (`Port_SwapModelCulling`). Only for the
  matrices in `sPortMirroredEdgeMatrices` (Eterna Forest's, 7): past a
  town's edge, mirrored houses would look wrong.
- **Maps out of view, and the scenery's seams** (patch 0068): a loaded
  map wholly out of the view (the box test, as the game's props have it:
  it passes what the wider view shows and what the sun shadows reach) isn't
  sent to the 3D; its polygons were all thrown away, but its vertices were
  worked out first (at the top of the Cycling Road, Eterna City's south
  part behind the camera: a third of the scene's vertices). And the
  overworld's scenery maps (the map header "everywhere", the mountains
  beside a route) sit at their own height in the matrix, nothing joining
  their edges where one is higher than the map beside it: a slit down the
  border, the backdrop's white through it, which the DS's narrower view
  never reaches from the player's ways (the Cycling Road's left: its
  mountains looked cut). Such a map is drawn again below itself, from the
  lowest map beside it up, 2 tiles apart (16 polygons each), the copies
  filling the slit as a wall would (`Port_RenderSceneryMapsLowered`).
- **The maps beside the loaded ones** (patch 0083): the game keeps 4 maps
  loaded, 2 by 2 round the player (theirs and the ones beside the quarter
  of it they're in), always enough on the DS. The pixel field sees about
  18 tiles each side at its top corners (a map is 32), so standing in the
  middle of a map the far corner reached the map beside, not loaded: the
  clear colour showed (Veilstone City at 687,632, a photo of 2026-11-05: a
  black square at the right edge, gone a step east). That map (beside the
  loaded ones, in their rows, on the side they leave out) is read too once
  its strip by them is in the view (`Port_RenderBesideMaps`): its model with
  its lists cut to those 3 tiles (`NitroPort_Gx3dTrimList`), and its props
  without their animations (`MapPropManager_PortLoadStill`), each
  box-tested as the game's. Its own NARC (the lazy loader reads the game's
  in steps), 2 kept, let go with the loaded maps; a map the game has
  loaded and read is the game's alone. The strip is a fifth to two fifths
  of a map's lists (big polygons cross it), sent only while it's in the
  view: in a map's middle 4 columns. On the PC with the 3D's clear colour
  made magenta (local `clearmag`): Veilstone's square gone, the pixels
  filled as the map shows them a step later; walking across Veilstone's
  middle and back, no hole, each map read once; with and without it in
  the same build, other towns differ only where people walked.
  Since patch 0084, while a map of the loaded ones' own column at that
  side is still being read: walking across a map's middle the loaded maps
  move a column on, the one now loaded is read over several frames, and
  till then nothing of it was drawn (the square for a moment as the player
  ran east towards Veilstone's spot, 2026-11-05). Its strip as it was
  beside them, still kept, is drawn till it's read; one not kept isn't
  read for this (after a warp all four are being read). On the PC, running
  east across Veilstone's middle from 681,632, a shot every 2 frames: 3
  shots with 107-209 black pixels at the right edge before, none after.
- **Route 215's cliffs down to the forest** (patch 0082): the route's maps
  are at height 0, the forest's scenery map south of them at 4. Below the
  ledges the route's cliffs come down to its south edge (z 608) and stop
  there, and the pixel field's 24 lines below the DS's saw past them: a
  black strip between the cliff and the forest's trees (a photo of
  2026-11-05, standing at 606,600). The cliffs are drawn on down to the
  trees (`Port_RenderEdgeAboveScenery`, only for the maps in
  `sPortEdgeAboveScenery`: elsewhere a copy under a map could show through
  its water): the route map's strip by its south edge (its last 2 tiles,
  its shapes' lists cut to it with `NitroPort_Gx3dTrimList` as the mirrored
  copies' are, made once and kept till the maps are let go) drawn again 2
  tiles lower and an eighth of a tile north, behind the map's own cliff
  face, and 4 tiles lower and a quarter north for the few dots still seen
  between the trees' tops (one copy 3 tiles down showed its top's edge, a
  step in a cliff's dark side). Each is sent only while the strip's own box
  is in the view (about an eighth of a map's lists). On the PC with the
  rain off, at 606,600, 600,604, 590,600, 620,604 and 632,602: the pictures
  with and without the copies differ only in pixels that were black. (A
  first try drew the forest's map again behind and below the route's edge:
  it filled the strip with a forest where the cliff should go on, and was
  replaced.) Since patch 0084 also Route 205's south end and Valley
  Windworks beside it, their scenery maps south of them 5 and 9 tiles up
  (a black band under the cliff at 222,663 in a photo of 2026-11-05): a
  copy 2 tiles lower more for each 2 tiles the scenery is higher than
  Route 215's forest (altitudes are half tiles: 3 copies and 5; Route
  215's 2 as before). On the PC at 222,663, 221,664 and 218,664 the black
  band gone (the black pixels left are the player's outline).
- **Route 203's south edge** (patches 0093, 0095, 0096): south of the route's
  east map the scenery is a tile up, and the pixel field's bottom lines saw
  a black band past the route's edge, the ridge cut short above it (a
  photo of 2026-11-06 at 230,762): the scenery's own forest there has no
  ground between its rows. Patch 0093 put the route in
  `sPortEdgeAboveScenery`: its strip drawn again lower showed the trees'
  feet in the band, a light stripe across the forest, and the ridge running
  on over the trees ("even more broken", 2026-11-06). Since patch 0095 the
  strip (its last 2 tiles, its trees' tops with them, the model's: the
  route has no props) is drawn again 2, 4 and 6 tiles further south
  (`Port_RenderForestOnSouth`, `sPortForestOnSouth`), a row of trees each,
  the forest's own spacing: the band filled with trees, the ridge going on
  behind them, each copy an eighth of a tile lower (under the route's own
  ground where its big polygons reach back). Only west of x 248: east of it
  the strip has the end of the rocky plateau's cliff, which drawn again
  made steps down it (the black under the plateau's corner left there is
  seen only from inside the trees). By the route's west map the scenery's
  forest there overlapped the route's last trees and the copies, trees in
  each other with a light line between (a photo of 2026-11-06 at
  228,763): since patch 0096 the copies go over both of the route's maps
  west of the plateau (x 192-248), 4 of them (8 tiles), and the scenery
  map south of the route isn't drawn under them
  (`Port_SceneryForestOver`): only its columns past them, the polygons
  reaching into those (`Port_RenderSceneryPart`, its copies under itself
  not at all). Tried on the PC and left: the props
  (none), the strip mirrored about the edge (the trees, seen from behind,
  stumps), only the polygons starting in the last 2 rows (the trees' tops
  start further north: stumps), copies a tile apart (rows too close). PC
  shots at 228,763, 236,764, 230,762, 226,761, 216,764, 204,763, 210,762
  and 220,763, and Jubilife's east side (188,764, 186,760, 190,766): the
  forest goes on, no stripe, no trees in each other.
- **Route 203's north edge** (patch 0093): north of the route the scenery
  is higher too (a tile up over its west map, 4 over its east), and the
  pixel field's top lines see past the route's last trees into the gap
  under the scenery, which the scenery's copies under itself
  (`Port_RenderSceneryMapsLowered`) filled with their flat ground: a green
  edge along the screen's top, the ridge ending in it (a photo of
  2026-11-06 at 230,745). The route's strip by its north edge (its first 3
  tiles, its shapes' lists cut to them, and its props there: the trees) is
  drawn again 3 and 6 tiles further north (`Port_RenderEdgeBelowScenery`,
  for the maps in `sPortEdgeBelowScenery`): the forest and the ridge go on.
  The strip's big polygons reach back into the route's own rows, so the
  copies are an eighth of a tile lower each (the further one lower still)
  and stay under the route's ground there; the scenery's copy at the
  route's height is half a tile lower (`PORT_NORTH_SCENERY_DROP`), under
  them past the edge. Each copy is sent only while its box is in the view
  (on the PC at 230,745, 170 more triangles of about 1000). PC shots at
  230,745, 226,744, 214,744, 240,744 and 250,746: only the top lines
  differ; the thin line left at 230,745's very top is the route's own
  shadow at the foot of its next trees, whose tops are just above the
  screen.
- **Route 206's river past the scenery** (patch 0069): beside the Cycling
  Road the wider field sees into the scenery map next to it, a flat ledge
  at its own height with the route's river and ledges stopping at the
  border. With the player in the half of the route's map by that border,
  the map is drawn again past it, mirrored about it, and the scenery map
  isn't (`Port_RenderMirroredBesideScenery`, `sPortMirroredBesideScenery`:
  Route 206 only): the river and ledges go on, level. Not the whole map:
  a copy of its model with its shapes' display lists cut to the strip by
  the border (`NitroPort_Gx3dTrimList`, the polygons kept drawn the same),
  made once for 4, 8, 12, 16 and 24 tiles; each scene the box test of the
  strips past the border picks the narrowest with all the camera sees
  (the whole map again was a fifth more work for the 3D, nearly all of it
  off the screen; this is 3%). In the copy the river's texture, one to a
  tile, ran the other way, its waves facing back at the border: water
  materials (named `sea_...`) have their S coordinates turned round again
  through the texture matrix (`Port_MirroredWaterShape`, a shape
  callback). Since patch 0088 also Valley Windworks, whose river runs east
  along its north edge and stopped against the scenery map's flat ledge
  (a photo of 2026-11-05), drawn there without its props
  (`sPortMirroredGroundOnly`: the windmills by the border not drawn
  again): the river and its banks go on east, and so do its plateau's
  terraces, where the ledge was flat (PC shots before and after at
  250,648, 246,652, 250,660).
- **Route 206 under the bridge, past the scenery** (patch 0070): there a
  ridge and the grass either side of it cross both of the map's borders,
  and mirrored, the ridge's half and its mounds and trees showed twice,
  the same either side of the border. In those rows (`sPortMovedRows`:
  tiles 617-661 of z on the left, 624-661 on the right) the map goes on
  as its own other side, moved over instead, the two borders' halves of
  the ridge meeting; the river, its ledges and the plateau north of the
  ridge stay mirrored. The copy's lists are cut by rows as well
  (`NitroPort_Gx3dTrimListRows`: each polygon by where it starts, so the
  two ways share none), a second copy has the moved rows (cut to the
  strip by the other border, its culling as it is), and the props go by
  the same rows (`MapPropManager_RenderRows`). The rows were picked a side
  at a time, on the PC, where no gap opens where the two ways meet past
  the border (a cliff ending in the open, the white behind showing); on
  the right one row of the mirror is kept under the moved ones' first.
- **The people of the maps beside** (patch 0050, also with the 320-wide
  field): the DS makes a map's people as the player walks into it and drops
  those of the map left, all out of its screen; on the wider one they
  popped in and out (the berry trees and people at Floaroma's edge). Now
  the people of the maps beside the player's are there ahead
  (`MAP_OBJ_STATUS_PORT_NEIGHBOUR`): `FieldMap_PortNeighbours` brings those
  within 20 tiles left or right (17 before patch 0085: the field at the
  DS's pixel size sees about 18 at its top corners, and the people there
  came in in sight), 13 up, 11 down (one a frame, read from
  their map's events, while there's room for their pictures and 12 places
  left for the game's own), and drops those 4 tiles further out; walking
  into a map, the map left's in reach stay beside, and the new map's are
  taken as the game's own (`MapObjectMan_AddMapObjectFromHeader`). They walk
  and are in the way, but can't be talked to, don't see the player, aren't
  found by a script's local ID and aren't saved: as on the DS, where they
  weren't there yet. A map's own flags (cleared walking into it) count as
  clear for its people seen from beside; one hidden by a flag set since is
  dropped. Taken as the game's own only as their event has them (a Vs.
  Seeker's spin, a script's hiding: made again from it); not the invisible
  ones (nothing to see). Their pictures' places are given back as they go,
  and walking into a map the people beside are given up (the furthest
  first) while the new map's pictures wouldn't fit. Nor someone with the
  same picture at the same place as one of the player's map's own events
  (patch 0084): someone at the edge between two maps is in both maps'
  events so as to show from either (Route 204's two youngsters by the
  Ravaged Path and its sign, in the north's and the south's), and brought
  from beside as well they showed twice, side by side as they turned
  (photos of 2026-11-05; on the PC at 173,703 and 174,712, two pairs
  before, one each after).
  - The textures nobody uses are freed (`ov5_021EDA38`) as the people
    beside go, and as the follower or its ball does, where the DS does it
    only as a map loads. A berry patch said which texture it uses from its
    growth stage as its movement last saw it, and one at the far end of
    the wider field (Route 206's, by the Cycling Road) hadn't moved yet:
    its textures were freed while its picture showed them, and the game
    crashed once the memory was used again (patch 0066,
    `BerryPatchGraphics_GetCurrentGraphicsResourceID`: a patch showing a
    picture says that picture's texture).
  - A berry patch beside (patch 0077) is brought by the picture its berry
    shows now (`BerryPatchGraphics_PortPictureOf`, from its patch in the
    save): at once if that's in already or there's none, else with room for
    it, places left for the pictures the player's map's own people may
    still need (its events' not in now: the ones its scripts bring on) and
    two more (`MapObjectGfx_PortHasRoomForBerry`). It kept room for any
    berry's picture before (six places free), and walking up Route 209 into
    Solaceon Town there were five: the town's four berry trees by its south
    entrance popped in at the zone change, in sight at the top of the
    taller field (2026-11-05).
  - The player's counterpart (patch 0055): her graphics are a variable's
    (`OBJ_EVENT_GFX_VAR_0`), set by her map's script as the player walks
    in, so she was never brought and popped in at the zone change (Dawn
    outside the lab in Sandgem Town, 4 tiles away). On the overworld only
    she has one, each map setting Dawn for a boy and Lucas for a girl, so a
    neighbour's `VAR_0` is that; where the scripts move or hide her in some
    of the story's states she's placed the same way
    (`sPortCounterpartPlaces`: Sandgem, Route 202, Jubilife, Veilstone,
    Canalave). The other variables' (only indoors) aren't brought.
  - What a map hid as the player left it (patch 0062): walking into a map
    clears the local flags, among them Cut's on the trees it cut. A map's
    local flags are remembered as the player leaves it, till they walk
    back in: what they hid isn't brought from beside, and walking back in,
    what they hid within reach of the screen stays hidden (its flag set
    again before the map's people are made, `FieldMap_PortKeepHiddenInSight`);
    further out it comes back as on the DS, unseen. The two Cut trees on
    Eterna Forest's east side (the forest's zone, flags 32 and 33) came
    back in sight as soon as the player stepped onto Route 205, and again
    walking back, 9 tiles away (2026-11-01). The flags' bytes start at the
    one with `MAP_LOCAL_FLAGS_START`'s bit (flag 1 is bit 1), as
    `FieldSystem_ClearLocalFlags` clears them.
  - Brought during a map change too (patch 0074): none are brought during
    a field task (a script, a menu, a battle's start), and walking out of
    a door or a gate is one. Out of the gate between Route 208 and
    Hearthome City, Route 208's arrow sign beside it, at the screen's left
    edge in the city, came in 17 frames after the gate's wipe had shown
    that part of the screen (a video of 2026-11-04). They're now also
    brought during a task that has taken the player into another map
    (the map with no task running last isn't the player's) with no script
    in any of the field's tasks (`FieldSystem_PortRunningScript`): as its
    screen fades in, as the map's own people are made as it loads. On the
    PC the sign is there as the wipe's last step shows its columns.
- **A battle's start** (patch 0051): the encounter effects are made for the
  DS's screen (windows and H-blank slices across its columns and lines,
  wipes drawn into a 256 x 192 BG). While one plays (`NitroPort_SetFieldEffect`)
  the upper screen's 2D is laid out as the DS's and stretched over the
  whole screen by the compositor (`NitroRender2DSplit.stretch`), the 3D at
  the screen's own size, its H-blank scrolls stretched too: the slices and
  wipes reach the screen's edges. The sprites (the Poké Balls, the Galactic
  logo) are drawn 4/5 as wide round their middles (a sprite of several OBJs
  touching, round its whole middle), so stretched 1.5625 across and 1.25
  down they keep their shape. Not the cut-ins with a trainer's picture
  (the leaders, the Elite Four, the Champion) nor the legendaries' (the
  camera only). The field's weather BG the compositor draws itself
  (`ComposeLayers.gpuBg`: Eterna Forest's patches of light, say) is
  drawn as the field had it, a pixel a pixel with the DS's screen in the
  middle, not stretched with the picture: it grew half as big again as
  each battle started (2026-11-05). (Not yet the dark of a cave's Flash
  circle, which is the picture's own BG: it's still stretched.)
  Their end (2026-11-01): the master brightness to black and the effect's
  BG cleared in the same frame (the Galactic boss's). The drawer takes the
  registers as the frame is handed over but reads the VRAM as it is when
  it draws; a frame later, the BG cleared and the brightness not yet,
  the field showed whole for a frame. A picture drawn with the master
  brightness at full black or white now but not in the registers taken
  isn't published (`RenderScreen`): the one before stays, and the next
  frame's has the brightness.
- **The circle fades** (patch 0054, also with the 320-wide screen): the
  screen closing round the middle (walking into Eterna Forest) is a window
  the game shapes line by line for the DS's 256 x 192. It was cut flat at
  the DS's sides, the lines it covered whole reached the screen's edges as
  a band, and the taller field's lines above and below the DS's had its
  first and last line's. Each step the game now tells the port the circle
  (`NitroPort_SetWindowCircle`: middle and radius), and the 2D renderer
  draws it across the whole canvas; off when the fade ends or another
  starts. Test: `test_window_circle`.
- **A walk out up or down** (patch 0053): a script's walk straight out up
  or down that meets something in the way beyond the DS's picture (more
  than 5 rows from the middle) goes to a door within the walk's reach
  instead (on the walk's way first, else west, else east): Dawn leaving
  the lab after the Pokédex walked 10 steps south beside the door, into
  the wall, where the DS's screen had ended. With no door near, the
  script's walk as it is.
- **A walk on or off past the DS's top or bottom** (patch 0076): the
  scripts add the ones who walk on from above or below just past the DS's
  top or bottom, and take away the ones who walk off up or down just past
  it; the taller field showed them pop in at its top and vanish there (the
  rival in Solaceon Town, added 8 tiles above the player, half of him in
  sight, and taken away the same way as he ran off). As beside the wider
  picture: one added there is put out of the taller picture (if the tiles
  between are clear) and walks them in with its first walk; one whose walk
  straight up or down ends there walks the tiles out (if clear) before the
  script takes it away. Whether a sprite shows comes from the camera's
  projection (`ScrCmd_PortTallRows`: its feet at its tile's middle, 32
  units tall, 4 rows' margin), at most 4 tiles more. One just added goes
  back where the script put it if it doesn't walk in first or the camera
  moves (the player or a free camera set walking, `MoveCamera`). On the
  PC the rival comes in from beyond the top and runs off past it (3 tiles
  more each way). Since patch 0086 (photos and notes of 2026-11-05): a walk
  whose last steps go up or down after other steps is made longer as well
  (Dawn leaving Sandgem Town after the tour: a step south, 5 west, 10
  north; Dawn leaving Jubilife after the Galactic battle; Looker's way out
  of Jubilife), and where the way on isn't clear the walker goes round,
  after the script's walk (that way first, else east, else west, to out of
  the taller picture or beside the wider one: Dawn's 10 steps out of
  Sandgem end under a row of trees, and she stopped at the top and was
  taken away in sight; on the PC she now goes a step east and on north,
  out of sight). A walk blocked by a wall where the DS's picture ended,
  with a door in reach (Dawn leaving the lab beside the player), first
  tries a step that way, across to the door's column and straight on to
  it, down the middle of the room (the search for a way to the door as
  before if that isn't clear). `SetPosition` forgets what was done to a
  walker's walk, so Looker, put in the road for later after his walk, is
  left there when the script ends. Since patch 0092 (2026-11-06) a walk
  counts as ending past the DS's top or bottom when the middle of the one
  walking is past that edge (`ScrCmd_PortTallPastDs`: more of them out of
  the DS's picture than on it), not only when they're out of it wholly:
  Rowan and Dawn leaving Jubilife after the Galactic battle walk 9 south
  and end just past the DS's bottom (out of it wholly, with the rows'
  margins, they weren't), so their walk wasn't made longer and they were
  taken away at the taller screen's bottom, in sight. On the PC (the tag
  battle skipped with a local debug build) they now walk 3 tiles more, to
  175,752, off the screen; Dawn leaving Sandgem still ends at 184,833.
- The weather's sprites (patch 0080, 2026-11-05): the rain's drops, the
  snow's flakes, the sand live on the DS's screen with 64 pixels round it
  (they come on there, go off one side and on at the other:
  `ov5_021D630C`), and the weather's sprite surface showed only those on
  the DS's 256 x 192, so on the taller field the weather was in the DS's
  part of it alone. With the screen wider or taller
  (`NitroPort_EngineABeside`; 0 and 0 on the DS's, all as before): the
  surface shows its cells on the whole picture
  (`RenderOam_PortInitSurfaceBeside`); they go round the wider screen with
  64 pixels round it (`ov5_PortPlaceWide`); each comes on where the game
  puts it on the DS's screen stretched onto the wider one, or as far
  beside it as the game has it beside the DS's (its first place after
  `ov5_021D6F00`); more come on for the wider screen's area, width or
  height, as each weather brings them on all over, along the top or down
  the side (`ov5_PortWeatherCount`; the pool of 48 as before); and they're
  taken away as far below or above it as below or above the DS's. On the
  PC on Route 215 (`dbgweather`, local), every sprite weather over the
  whole 400 x 240, as many to a part of it as on the DS's (rain about 35
  OAM entries, heavy rain 61 at most, of 124); the BG weathers (clouds,
  fog, Flash's dark) already were.

## The soft reset

L + R + START + SELECT (the game's own soft reset, or its restart after a
link error: `OS_ResetSystem`) closes the program as from the HOME menu (the
save's file written) and has the system start it again
(`aptSetChainloaderToSelf`: the HOME menu jumps back to the title), so the
game starts from its opening with nothing left over. Started from the
Homebrew Launcher (a `.3dsx`) there's nothing to jump back to: it closes to
the launcher. The PC runner starts itself again with the same arguments
(`execv`), the control directory carrying on.

## A catch with the party full

A Pokémon caught with six in the party goes to the PC, as on the DS. In
the battle, after the Pokédex's page and before the nickname
(`patches/pokeplatinum/0042`, `battle_script.c`), the game asks "Your
party is full. Add X to your party?"; YES opens the battle's party screen
(its SHIFT reads SEND TO PC, and any of them can be picked: the one in
battle, a fainted one, an egg) to pick the one that goes to the PC in its
place, then "X was added to the party and Y was sent to the PC." and the
nickname as ever; the box message is left out (and from the naming
screen). NO, or the party screen left with its back button: the PC, as
ever. One holding Mail or with a Ball Capsule can't be picked (the PC
takes neither: "Y is holding Mail. It can't be sent to the PC.", and the
party screen again). The caught one goes to the PC as ever and the
battle notes where (box, place, its personality value); the two change
places once the battle's party is back in the save (`encounter.c`,
`UpdateFieldSystemFromDTO`): the battle's end writes its battlers back
over their party slots, so not before. The caught one takes the picked
one's slot, healed (as from the PC). Not in the Safari Zone, Pal Park or
the catching tutorial. The texts are English (the game has none of its
own for it).

## The lower screen as a menu

In the field the lower screen is the port's menu, drawn at the 3DS's
resolution (`port_lowermenu.c`, `include/nitro_port_lowermenu.h`): on the
left a panel with the DS's lower screen in it (the Pokétch, or the Poké
Ball before the player has one), at 15/16 of its size; on the right a
column of eight buttons (MAP, POKéMON, BAG, TRAINER, POKéDEX, POKéTCH,
SAVE, OPTIONS), each with its icon and name, over two squares for the
registered items. Everywhere else (battles, the apps that take both
screens) the lower screen is the DS's, filling the screen as before.

- **On and off:** the Pokétch's handlers in the field
  (`patches/pokeplatinum/0031`): the menu is on while the Pokétch is the
  field's lower screen. Only in the remaster display (the compositor says
  it can show it: `NitroPort_LowerMenuSetAvailable`, with the memory for
  its picture), and only without the 3D on the lower screen. In the
  classic display the game is the DS's.
- **The start menu** (`patches/pokeplatinum/0032`): while the menu is on,
  it is the start menu. X opens it as before, but its window is never
  drawn and its icons are hidden: the cursor is on the lower screen's
  buttons (up and down over the ones that can be used, A, B or X to
  leave). The start menu's task still runs everything, so the apps open
  and come back as on the DS (the bag's and the party's uses go through
  it). POKéMON, BAG, TRAINER, POKéDEX, SAVE and OPTIONS are its entries;
  MAP (with the Town Map in the bag, where the bag can be used) puts the
  map in the panel (A on it again, with the map there, opens the Town Map
  app); POKéTCH puts the Pokétch in the panel. The buttons that
  can't be used now are dimmed (the field says which every frame while
  the player could open the menu). Where the menu has an entry with no
  button (RETIRE in the Great Marsh and the Pal Park, CHAT in the Union
  Room), it's the DS's start menu on the upper screen.
- **Quick access** (`patches/pokeplatinum/0036`): a tap on a button does
  what it says at once, with the press's sound (the menu's opening sound
  is X's): the menu opens straight on it (its cursor on it from the
  start). Leaving what the menu opened (B, or the bag's button again; the
  party, the trainer card, the Pokédex, the options, the Town Map, the
  save's question answered no) leaves the menu too, back to walking,
  whether a tap or X opened it. A double tap on MAP
  (`NitroPort_LowerMenuPressDouble`: the second press on a button within
  half a second) opens the Town Map app; one tap only shows the map. A tap
  on the map does nothing.
- **The panel's tabs:** the map or the Pokétch, as chosen last (MAP,
  POKéTCH; the map again once the bag's closed).
- **The panel's fade** (`NitroPort_LowerMenuFadePanel`,
  `NitroPort_LowerMenuPanelFade`): whenever what the panel shows changes,
  it fades to black and back, the frame and the buttons as they are: the
  same look for every change, and the loading in between hidden. A whole
  fade is 8 of the platform's frames (`NITRO_LOWERMENU_FADE_FRAMES`, its
  V-blanks: `NitroPort_LowerMenuSetClock`), from wherever a fade was; an
  app pops in at once (`NITRO_LOWERMENU_CLEAR_NOW`). Fading in to the map,
  the fade waits (black, half a second at most) till the map's drawn as the
  game last gave it, and the map's panel is black till there's a map drawn
  at all. Fading in to the DS's lower screen, or an app popping in, the
  panel stays black till the platform has drawn a picture of it made since
  (`NitroPort_LowerMenuPanelShown`; half a second at most; the 3DS's
  drawers say how recent each screen's newest picture is, changed or not:
  `NitroPort_Compose2DCurrent`): the 3DS draws
  the game's pictures a frame or more late, and its newest one could still
  be from before the app was whole (a glitch for a frame, 2026-10-25). The
  platforms draw black over the panel by as much (the
  compositor a black quad, the PC runner its pixels darkened). While the
  panel's dark its touches are nobody's. Tabs
  (`NitroPort_LowerMenuFadeToPanel`): the panel fades to black, the port
  shows the other tab once it's black, whatever the game's running then (a
  tab chosen again on the way just changes where it's going), and fades
  back in (the Pokétch 4 frames after it's shown: its picture drawn
  first). A fade back to black, or an app popping in, calls the change
  off.
- **Kept shown** (`NitroPort_LowerMenuKeepShown`): while the game swaps the
  field and the panel's apps (the bag, the party, the summary, the trainer
  case), the menu's
  kept on (drawn with its panel black, or the map, while the game turns it
  off and on; its buttons drawn and pressed as usual, the panel's touches
  nobody's). An app that finds it so when it starts came behind the
  panel's black, and pops in.
- **3D in the panel:** an app shown in the panel with its 3D (the summary's
  Pokémon, the party's form change) has it drawn there: the DS's 256 x 192
  at the panel's place for the DS's rows (on a tall screen its first 192
  lines), clipped to the panel, the layers in front of it over it
  (`draw_menu_3d` in the compositor, `CanvasWith3D` in the PC runner).
- **The picture:** drawn on the CPU when it changes (on, off, the cursor
  or a finger on a button), handed to the compositor under a lock with a
  serial and the rows that changed (`NitroPort_LowerMenuTakeDirtyRows`);
  the compositor puts it in a texture (RGB565, a texel a pixel, two taken
  by turns, each brought up to date with only the rows it's behind on) and
  draws the panel over it: the DS's lower screen, or the map's own picture
  (the same way, textures of its own). The frame and the names
  are the port's (the names in a small font of its own: the game's is too
  wide for a button, POKéDEX is 42 pixels in it); the icons are the start
  menu's own, read from the ROM (`graphic/menu_gra.narc`: `icons_NCGR` in
  `menu_NCLR`'s coloured palette; the bag's for a girl is hers), and two
  drawn for the menu (MAP and POKéTCH).
- **The panel:** a canvas 256 x 247 DS pixels, drawn 240 x 232 at (3, 4):
  the DS's 192 rows at its rows 27-218, its top row carried on above them
  and its bottom row below (on the Pokétch, its strap goes on up and down).
  Three quads from the DS's picture's texture.
- **Tall:** a screen made for it (the game's `NitroPort_LowerMenuSetTall`)
  has the lower screen's engine drawn 248 lines tall
  (`NitroRender2DSplit.height`): its BGs' maps and its OBJs go on below
  the DS's 192 rows as they would were its screen taller, and the panel
  shows the picture's first 247 rows. The H-blank effects' lines past 191
  are line 191's; never with the 3D (its lines past 191 black). The bag
  and the party are laid out for it (below).
- **The map** (`port_lowermap.c`; the game's side in
  `patches/pokeplatinum/0033`): once the player has the Town Map, the panel
  shows it (the Pokétch, or the Poké Ball before it, goes on underneath,
  running; MAP and POKéTCH choose). It's the Town Map item's upper screen
  as the Town Map app makes it (its map, the hidden places found, the
  towns' blocks, the cursor and the player's head, the places been through
  before blinking one at a time, the place's name in its bar) and under it,
  the canvas's last 55 rows, its guide page's rows 61-115: the place's
  description over the zoomed-in map. The port draws it from the Town
  Map's own pictures (`graphic/tmap_gra.narc`) as the game lays it out:
  where the sprites are, which hidden places show, the zoomed-in map's
  scroll, and the name and the description as the game prints them (its
  font, the signpost's colours), told again while the map is chosen when
  the player's square or place changes, or the field starts again. The
  same pixels as the Town Map app's (checked in the PC runner,
  2026-10-24). A double tap on MAP opens the Town Map app; so does MAP if
  the port can't show the map (no Town Map pictures).
- **The map without a stutter:** walking with the map out costs the
  game's thread next to nothing (a stutter, 2026-10-24, twice: the game's
  thread drew the map and the whole menu, then only what changed; still a
  hitch on the 3DS, its core 0 nearly full).
  - The game tells the port cheaply: the Town Map's context worked out
    each time with nothing read (the description flags' file read once and
    kept: `town_map/context.c`), its blocks and the texts' palettes read
    once; the name and the description printed only when they change, one
    a frame while the panel's shown (both at once behind its black), and
    only if the port hasn't kept them (`NitroPort_LowerMenuMapTextKept`: the last 8 of each, by the
    game's key: going back and forth between two places prints nothing);
    the windows' tiles handed over as they are (4 bits a pixel), the port
    making them pixels. On the PC runner a new square is about 0.03 ms of
    the game's time, a new place's texts about 0.06 and 0.1 ms on two
    frames (they were 1 ms in one, the drawing with them).
  - The port draws it off the game's thread: what the game hands over
    (`NitroPort_LowerMenuMapEnd`; with the drawer still taking the last
    one, handed over a moment later, nobody waiting) and the sprites'
    animations go to the map's drawer (`NitroPort_LowerMenuSetMapWorker`, `MapWork`), on the 3DS
    a thread at the lowest priority on a New 3DS's second core (core 0 on
    an Old 3DS); it draws only what changed (the upper screen's background
    only for other hidden places, the zoomed-in band when its scroll or the
    description changes, the sprites where they were and are, the name's
    window, the cursor's frames, the dots) at 15/16 as the screens draw the
    DS's (each pixel 2 x 2, then bilinear), and hands over only those rows
    (`NitroPort_LowerMenuLockMap`, `TakeMapDirtyRows`); the compositor
    takes them only if the drawer isn't handing some over
    (`NitroPort_LowerMenuTryLockMap`: else the texture it has, till the
    next frame). The Town Map's pictures are read on the game's thread
    (the data pack is its), once.
  - The DS's lower screen (the Pokétch underneath) isn't drawn on the 3DS
    while the panel shows the map: nothing of it shows (drawn whole again
    once it does). Its brightness changing, it's drawn (the DS's fades
    darken the menu with it: `NitroPort_Render2DMasterBright`).
- **The bag in the panel** (`patches/pokeplatinum/0035`): opened while
  the platform can show the menu (`NitroPort_LowerMenuAvailable`), the bag
  puts its main screen (the list, the pockets, the description) on the
  lower screen (the DS's screen swap) and lays it out tall, as its approved
  mock-up has it: the list's box 7 tile rows longer (its middle row again)
  with 3 more lines of items (12), and below it, 7 rows lower, the
  pockets' panel, the description bar, and the windows and sprites that
  go with them (the item's menu, the messages, the counts, the item's
  icon); above the pockets, the bag 1.25 times as big, drawn smoothly
  (`NitroPort_Render2DSmoothObjs`: its texels blended), its shadow two
  tiles wider. Its BG2 (the list) has its tiles in a block of their own
  (the list's window is taller, the other windows' tiles stay where they
  were). The sprite system's main screen goes on to line 255 then
  (`SpriteUtil_PortTallMainScreen`: the sub screen's sprites from 256).
  The bag's touch screen isn't shown, so the panel's touches are nobody's
  (`NitroPort_LowerMenuSetPanelTouch`). With `--classic-menu` (or no menu)
  the bag is as before, across the wider upper screen.
- **The bag in and out** (`patches/pokeplatinum/0035`, `0036`): BAG fades
  the panel to black, the menu kept on (BAG lit) and the upper screen
  keeping the field, and the field goes and the bag comes behind it; the
  bag pops in two frames after it's whole. B (or CLOSE BAG, or BAG again)
  fades the panel to black again, the field comes back behind it, and
  three frames after the field's been drawn the panel fades in on the map
  (the Pokétch for POKéTCH, or without the Town Map), the menu closed.
  While the item list waits, the menu's other buttons work (with its
  sound): BAG puts the bag away; POKéMON and TRAINER switch to the party
  and the trainer case in the panel (below); POKéDEX and OPTIONS open
  straight from the bag
  (the panel black, then both screens black, as the DS's fade out leaves
  them, and the app's own fade in; the bag makes them black while they're
  still kept, a frame before it goes, because the 3DS shows the game's
  pictures a frame late); SAVE, MAP and POKéTCH are acted on back in the
  field. A tap while the
  bag's busy (an item's menu, a message, moving an item) is nobody's.
  An item used on a Pokémon (a Potion, a TM, an evolution stone, the
  Gracidea, a Berry away from a soft-soil patch), given from the item's
  menu, or given from the party's (and B there) goes the same way, the
  party coming behind the black panel (below); anything else (the
  Bicycle, a Berry's tag, a mail to write for a Pokémon holding nothing:
  the start menu tells the bag which one, `gStartMenuPortGiveSlot`) is the
  DS's fade out. The
  bag's fades out are put off to its last state, which decides
  (`BagPort_LeavesInPanel`). (The bag hands the start menu the button
  tapped, `gStartMenuPortAppPress`: the menu forgets its own press when it
  goes off.) With `--classic-menu` the bag's the DS's.
- **The party in the panel** (`patches/pokeplatinum/0037`): opened while
  the platform can show the menu, the party puts its main screen (the
  panels, the message, CANCEL) on the lower screen and lays it out tall,
  as its approved mock-up has it (the same pixels, checked in the PC
  runner with `cshot`, 2026-10-25; but for the cursor's top-right corner,
  whole now). Each row of panels 16 pixels lower than the one above it
  (the first 16, the second 32, the third 48), in tiles: the panels, their
  windows, their sprites and the cursor's places (`PARTY_PORT_TALL_SLOT_DY`);
  what was under them 56 lower, at the bottom: the message's windows,
  CANCEL (and CONFIRM), a Pokémon's menu, YES/NO
  (`PARTY_PORT_TALL_BOTTOM_DY`). The background (BG3) is drawn anew when
  the party opens, from its own tiles, by the rules the mock-up was made
  with: the stripes from 7, 7 (the plain border round them), the panels'
  rows of the DS's background each moved with its row, the teal bands
  between the columns run on along the DS's diagonal from one panel to
  the next (20 wide), the dashed line beside each right panel, the
  message's rows 56 lower; its tiles go after the party's own, the same
  ones once. The second left panel loses its top edge (tiles without it,
  kept on that row as panels are drawn and switched:
  `PartyPortTall_FixEdges`; an empty panel there too). A form change
  (Giratina's Griseous Orb, Shaymin's Gracidea) has its 3D sparkles in the
  panel, at the icon moved with its row (48 pixels are 1.953125 of the
  particles' units). The touch screen (the Poké Balls) isn't
  shown, so the panel's touches are nobody's; the upper screen keeps the
  field. With `--classic-menu` the party is as before, across the wider
  upper screen.
- **The party in and out** (`0037`): as the bag (above): POKéMON fades the
  panel to black, the party comes behind it and pops in; B (or CANCEL, or
  POKéMON again) fades it to black, and the map fades in once the field's
  back. While the cursor waits on the panels the menu's other buttons
  work: BAG and TRAINER switch to the bag and the trainer case in the
  panel (and POKéMON in them back to the party), POKéDEX and OPTIONS open
  straight from it, SAVE,
  MAP and POKéTCH are acted on in the field; a tap while it's busy (a
  Pokémon's menu, a message, a switch), before it's shown, or in a party
  not the menu's (a script's) is nobody's (the bag's the same). The
  summary (also to choose a move to forget), the bag to give an item, and
  back to the bag go the same way, behind the black panel
  (`PartyPort_LeavesInPanel`); a field move, the mail or an evolution is
  the DS's fade out, and what comes next fades in.
- **The summary in the panel** (`0037`): from the party in the panel, the
  summary comes behind the black panel and pops in: its main screen (the
  pages, the Pokémon, 3D) on the lower screen at the DS's size (not tall,
  none of the wider upper screen's layouts), the upper screen keeping the
  field; its touch screen (the buttons) isn't shown, the panel's touches
  nobody's (the pad does what they do), the menu's taps nobody's. Left, it
  goes the same way and the party comes behind it. From anywhere else
  (the PC's boxes...) it's the DS's.
- **The panel put together** (`NitroPort_LowerMenuSetPanelRuns`): an app
  whose two DS screens both go in the panel has the canvas made of runs of
  rows: each the DS's lower screen's rows, the upper screen's (its DS 256
  columns), or the port's tabs; a row after another, upside down, or one
  row again (a plain row stretched); the canvas's outer two columns each
  side painted a colour where the layout wants an edge the rows haven't
  (`sides`, `sidesIn`; painted at the panel's outer two pixel columns).
  Rows no run covers are as usual. Touches on the lower screen's rows are
  the game's, at the row they show (dragged onto another's rows: where the
  drag last was on the lower screen's); on the upper screen's, nobody's.
  The upper screen is drawn at the DS's width then
  (`NitroPort_LowerMenuPanelShowsUpper`: no wider layout). The compositor
  draws a quad a run from the lower screen's texture or the upper screen's
  (its newest picture held for the frame, the panel drawn again when it
  changes; the panel's fresh-picture wait goes by the older of the two),
  the upper screen's master brightness over its rows; the PC runner puts
  the canvas together the same way (`cshot` writes it, 256 x 247, the
  tabs' rows magenta). Gone with the menu (`NitroPort_LowerMenuShow(0)`).
- **The panel's tabs** (`NitroPort_LowerMenuSetTabs`): folder tabs over
  the runs' tab rows, drawn by the port in its picture at the 3DS's
  pixels: side by side 4 pixels apart, their top corners rounded, each a
  small Poké Ball and its label in the port's font made bold, the open one
  opening into what's under it (its top line runs on under the others), in
  the colours the app gives. A tap let go on the one it began on is a
  press (`NITRO_LOWERMENU_TAB_PRESS(tab)`), also while the panel's other
  touches are nobody's; the panel's fade darkens them with it.
- **The trainer case in the panel** (`patches/pokeplatinum/0038`): TRAINER
  (or switching from the bag or the party) brings the trainer case behind
  the panel's black, as the bag; it pops in, the upper screen keeping the
  field. Its two screens are in the panel by turns, as two tabs on the case
  (the approved mock-ups; the card's tab the same pixels in the PC runner
  but the tabs' lettering, 2026-10-25): TRAINER CARD, the card above the
  case's clasp; BADGES, the badge case above it. Put together from runs:
  the card's tab the card (the upper screen's rows 7-183; at rows 7-8 the
  case's corner left out, its edge painted on), the clasp and the case's
  rounded bottom (the lower screen's rows 152-191, the edge painted on
  where the lower screen's case has none); the badges' tab the lower
  screen's rows 0-191 in one piece (the clasp where the card's tab has it).
  There the lid is shaped for the panel (`TrainerCasePort_ShapeLid`): its
  sides and rounded bottom corners moved in 6 pixels, its GYM LEADERS
  heading with them (as far from the side as on the DS), so it covers the
  badge case beneath by 2 pixels each side (not 8 wider than it, as on the
  DS's screen), and its bottom edge upside down over its plain top rows as
  its top edge (made as a picture of its rows and cut into tiles again,
  those changed in the slots its tilemap no longer uses; the case's own
  rows round its top made plain). It shrinks towards that top as on the DS,
  flipping up from the badge case's top; open, its strip up there is left
  out (laid out so once it's up, and with it again as it comes down). The
  case's body above them is a plain row of it from below the card (the
  upper screen's row 188: no sprite there, nobody's to touch; a lower
  screen's row or a mirrored one would show the clasp's tap circles). The
  tabs in the case's colours, their labels the port's. A tab tapped, or Left / Right, shows it; the clasp in
  the card's tab shows the badges and opens the case; A turns the card in
  its tab; B, TRAINER, or another of the menu's buttons leaves as the bag
  does (BAG and POKéMON switch in the panel, POKéDEX and OPTIONS open
  straight from it, MAP, POKéTCH and SAVE back in the field). Without the
  menu, or opened another way, it's the DS's, across the wider upper
  screen. Leaving the panel it sets the screens back as they were, as the
  bag and the party do (the Pokédex, opened straight from it, doesn't set
  them, and came up on the wrong screens: 2026-10-25).
- **The panel's buttons** (`NitroPort_LowerMenuSetPanelButtons`): up to
  four rectangles on the canvas the port takes touches on itself, for a
  page with buttons below the DS's 192 rows (the game's touch stops at row
  191). A touch that begins on one is the port's (pressed while it's still
  on it, `NitroPort_LowerMenuPanelButtonHeld`); let go on it, a press
  (`NITRO_LOWERMENU_BUTTON_PRESS(button)`). Not while the panel is dark or
  the map is out; gone with the menu.
- **The save in the panel** (`patches/pokeplatinum/0039`): SAVE (tapped,
  or A on it after X) with the Pokétch up fades the panel to black, ends
  the Pokétch (`FieldSystem_EndBottomScreen`; its dot artist keeps its
  picture as it goes) and draws a save page on the DS's lower screen,
  tall, with SAVE lit (the approved mock-ups): Platinum's message box at
  the top; a report card in the window frame (the trainer's picture in a
  pale box, the name, IDNo., the place in red, POKéDEX, TIME, the eight
  badges at half size with those not earned as pale outlines, the party's
  icons on shadows); YES and NO buttons in the party's CANCEL style, the
  one chosen ringed orange. The save's own script (common script 5) runs
  as it is: while the page is up, its message box opens on the page
  (`SavePanel_AddMessageWindow`), its YES/NO is the page's buttons
  (`SavePanel_StartYesNo`, `ScriptContext_WaitForSavePanelYesNo`: a tap,
  A, B for NO, the D-pad to choose) and the DS's save info window isn't
  shown. The page is pictures put at any pixel on three layers (each cut
  into tiles, alike ones shared): the badges from the badge case's sprites
  (their mounting rim peeled off, each 2 x 2 pixels the commonest colour,
  a tie the darker; the outline the darkest colour round the edge), the
  icons, the trainer from the trainer card's. The sub engine has banks H
  and I for its BG (48 KB) while it's up, as before after. When the
  script ends the panel fades to black, the Pokétch starts again
  (`FieldSystem_InitBottomScreen`; the panel no longer tall, its touches
  the Pokétch's again) and the map or the Pokétch fades in as the panel
  was. SAVE tapped in the bag, the party or the trainer case: the field
  comes back behind the panel's black and the page comes straight in it
  (no map in between). Taps made before a question are not its answer, and
  menu buttons tapped during the save aren't acted on after it. Without
  the menu, or with the Pokétch not up, it's the DS's save. A new game
  saves over the old one (`patches/pokeplatinum/0042`): the DS refused
  until the old save was erased from the title screen (and warned about
  it when NEW GAME was chosen); the first save of the new game writes
  over it, the old save there until then, and starts the Hall of Fame
  anew as erasing did (`SaveDataExtra_Init`). Nothing asks first: saving
  is the choice.
- **The two squares** (`patches/pokeplatinum/0041`): the registered items'
  icons (the game's own, read from `itemtool/itemdata/item_icon.narc` by
  the port when the item changes: `NitroPort_LowerMenuSetItem`), the first
  with a small red Y (the approved mock-up). The first is the item on Y
  (the save's registered item); the second the one that was on Y before:
  REGISTER in the bag puts an item on Y and moves the one that was there
  to the second square, DESELECT takes it off either square. The second is
  the port's, not the save's (`item2` in `options.txt`, saved at once), so
  saves stay the DS's; shown only while the bag has it (another
  adventure's isn't). The bag marks Y's item as before (its "Y SET"), not
  the second's. A tap on a square uses its item as Y does
  (`RegisteredItem_PortUse`: the same checks and messages; the Bicycle,
  the rods, the Journal, the Vs. Recorder...); with the menu open (X) the
  cursor goes onto them (Up / Down round the column and the squares, Left
  / Right between the two) and A uses it; tapped in the bag, the party, the
  trainer case or the options page, those close and it's used back in the
  field. Dimmed where Y does nothing (the Battle Tower's salon, the Pal
  Park). Without the menu, the bag's REGISTER is the DS's.
- **The panel at the field's start** (`StartMenu_PortFieldStarts`, from the
  Pokétch's start): the tab chosen (the map once they have the Town Map)
  from the field's first frame, its name and description printed at once,
  not the Pokétch till the player can first walk; the game's first field
  on the tab the options' START WITH says (the map or the Pokétch).
- **The options in the panel** (`patches/pokeplatinum/0040`,
  `options_panel.c`): OPTIONS (tapped, A on it after X, or tapped in the
  bag, the party or the trainer case, which go through the field behind the
  panel's black as SAVE does) with the Pokétch up fades the panel to black,
  ends the Pokétch and draws an options page on the DS's lower screen,
  tall, OPTIONS lit (the approved mock-ups, the same pixels in the PC
  runner but for the cards added since): tabs (SETTINGS, ENHANCEMENTS,
  CHEATS), then cards in two columns, each its name and its value in red
  between arrows, the cursor Platinum's options screen's red frame.
  SETTINGS: the game's six (into its save, as its options screen sets
  them; the sound and the buttons at once), 60 FPS, SMOOTH 3D and START
  WITH; ENHANCEMENTS and CHEATS (below); CHEATS's GIVE ITEMS a page of
  its own (a pocket, an item, how many; no key items). The FRAME card is
  drawn as a message box in the frame chosen, a preview (the frame's
  tiles stretched to the card, in the sub BG's palette 1: the frame's
  colours and the few the card's text and cursor need). Closed and opened
  again, the page comes back on the tab and the cards it was on (the start
  menu keeps them). Drawn into a window off the screen as one picture (the cards,
  the game's text on them, their edges and the cursor over it) and cut
  into tiles, alike ones once (48 KB of the sub engine's VRAM, BG0 alone),
  drawn whole again on any change. The D-pad only moves the cursor (Up /
  Down in its column, round again; Left / Right to the other column's card
  on the same row, its last if it's shorter: 2026-10-25, it used to change
  values), A changes the card (or does it), L / R the tabs, B or X close
  (B on GIVE ITEMS: back to CHEATS); taps: the tabs, a card's left half its left arrow and its
  right half its right one (the port's panel buttons, now up to 32).
  Another of the menu's buttons closes the page and is acted on (BAG,
  POKéMON, TRAINER and SAVE straight on from the black panel; MAP,
  POKéTCH, POKéDEX once it's faded in). Closed, the Pokétch comes back and
  the port's settings are saved. Without the menu it's the DS's options
  screen. The field isn't paused while the page is up (patch 0057: the
  start menu pauses everyone's movement, the page lets them go), so
  people walk about and changes are seen as they're made (the follower
  coming out or going in); the player stays still, every button the
  page's.
- **The enhancements and cheats** (`port_options.h` in the
  decompilation: the port's settings, read where the game decides):
  - SPEED (1x, 1.5x, 2x, 3x, 4x; the 3DS): that many game frames to each
    one shown, the others not drawn nor waited for (the music keeps its
    own clock, the audio thread's). Each frame shown waits till the
    threads below the game on its core (the display thread, the lower
    screen's drawer: they only run while the game waits) are done with
    the one before, and for the V-blank: without that, at 4x the lower
    screen stopped being drawn again and the screens stayed black after a
    battle (2026-10-25). And the game waits at the end of the frame after
    a shown one till every drawer is done reading its display state, the
    one on the game's core too (at 1x it reads while the game waits for
    the V-blank; faster, it went on reading through the frames not shown,
    their changes in its picture: a battle's lower screen garbled at 4x).
    A battle holds it at 1x while it waits for the player
    (`NitroPort_SpeedHold`): its menus on the lower screen (from when one
    is drawn till a move or a target is chosen or the panel goes blank),
    its bag and its party (not a battle video's STOP button: playback
    doesn't wait for the player); the battle's animations and text go at
    the speed set. The start menu holds it at 1x too, and what it opens
    (the bag, the party, the Pokédex, the options...), till the player
    walks again (patch 0042): only the walking and what happens in the
    field go faster. A frame shown is drawn after the game's next V-blank
    work, whichever halt it was shown on: in the field (30 frames a
    second, two halts a frame) at 4x every fourth halt fell on the one
    without V-blank work, so nothing was drawn at all, the screens left
    as they were while the game went on (black rooms, no text boxes, the
    battles' health bars and animations stopped; 2026-10-26).
    Faster than 1x a frame composed after one that took long (7 ms, 14)
    is composed 2 (3) V-blanks after it, not the next one
    (`ComposeWaits`, main_3ds.c): the display thread is on the game's
    core, below it, and at 4x the game makes a 3D scene for every
    V-blank; outdoors each frame composed was 9-10 ms of building there,
    which with the game's 4 frames a V-blank was more than a V-blank, so
    the game waited for it and went between 4x and about 2x as the scenes
    did (82 to 240 frames a second in a log of 2026-11-05). Indoors,
    quick, still every V-blank; at 1x as before.
    SPEED above 1x turns 60 FPS off (when it's on), 1x on again
    unless the player has set 60 FPS since (`fps60_held` in
    `options.txt`). Every start is at 1x (since 2026-10-26): SPEED is
    for now, not kept, and 60 FPS comes back on with it if SPEED had
    turned it off. The PC runner isn't paced, so it doesn't show.
  - EXP ON CATCH: after "Gotcha!" the catch stops while the battle's
    script pays out experience for the caught Pokémon as for a defeated
    one (its messages, levels and moves, as after a battle), then goes on
    to the Pokédex and the nickname. Not in the catching tutorial, the Pal
    Park or a battle with no experience.
  - PARTY EXP SHARE: every Pokémon able to (not fainted, not an egg) gets
    experience. First those that battled, their share each, as without it
    (each its own message, its gauge, its level up); then the rest of the
    party as much as each of those got (before their own boosts: a Lucky
    Egg, traded), under one message, "Your party gained ... Exp. Points!"
    (the port's text: the game has none), with their levels up as usual
    and their effort values, as later games' Exp. Share (a held Exp. Share
    adds nothing then). When none of those that battled is still up (an
    Explosion, Destiny Bond...), the rest get the whole amount each.
  - TRADE EVO AT 40: a Pokémon that evolves by trade (with or without its
    item) evolves by level from 40 instead.
  - HMs WITHOUT MOVES: a field move whose HM is in the bag is the party's
    first Pokémon's (not an egg), whatever it knows (its badge still
    needed): the scripts' checks (Cut, Surf, Strength, Rock Smash...) and
    the party menu's Fly and Defog (Flash is a TM in Platinum).
  - AUTO-RUN: running unless B is held (once the player has the running
    shoes). The Underground too (patch 0065, `comm_player_manager.c`):
    the speed it moves the player at is sent to its server, and that was
    B's alone, so with AUTO-RUN the player's picture ran a tile and stood
    for the rest of a walk's tile. And each player's picture is chosen
    from its speed with B standing for "runs" (`CommPlayer_MoveClient`):
    `PlayerAvatar_IsRunButtonHeld` took that as the button and AUTO-RUN
    turned it round (a run's speed under a walking picture), so it's
    turned round for it too.
  - The Underground's answers (patch 0067): the player is moved by the
    comm system's server (the 3DS itself, the wireless having nobody
    around), and their picture followed the place it sent back through
    the comm system's buffers, 4 frames later; turning stood them still 4
    frames, at the slower turn, and a turn while walking stopped them.
    With nobody else there, the place is put on the picture as the server
    sends it (the same packets coming round after are left alone), a turn
    while walking takes its step at once, and a turn from standing is the
    field's, its step 2 frames after. From the press to the screen moving:
    10 frames to 6; turning, 21 to 13 (the field's 11).
  - FOLLOWER (patch 0056, `port_follower.c`; off by default): the party's
    first Pokémon (not an egg, not fainted) walks behind the player, on
    HeartGold and SoulSilver's sprites when the data pack has them
    (`builder/followers.py` downloads them from pret/pokeheartgold at a
    pinned commit, checks each file and lays each out as Platinum's
    walking sprites: 16 textures, 4 a direction; shiny ones with their
    palettes swapped), else on Platinum's own for the 24 it has (the
    Amity Square Pokémon, the starters' lines, Machop, Croagunk, Shaymin,
    Regigigas). A map object of its own (`MAP_OBJ_STATUS_PORT_FOLLOWER`,
    local ID 0xF3), moving with the partner trainers' code under its own
    movement type (scripts never take it for the partner): walked
    through, kept across zone changes, never saved, not talked to. Its
    graphics IDs (0x4000 + 2 x sprite + shiny) are answered by the
    overworld sprites' lookups as Pikachu's (Regigigas's for 64 x 64),
    their texture an archive member past the archive's own (`narc.c`).
    Out while the player walks (in its ball on the bike, surfing, during
    field moves, while the player hands the party to a Pokémon Center's
    nurse for healing (the player's picture holding out the Poké Balls),
    with Cheryl and the other partners or Amity Square's
    Pokémon, in the Underground, the Union Room and maps with features of
    their own; a 64 x 64 one indoors; and, since patch 0085, till the
    player walks out of their house after the first battle with the
    rival, `VAR_PLAYER_HOUSE_STATE` 5: on Route 201 the Pokémon just
    chosen came out between the battles and in the cutscenes there and at
    home), behind the player or, where that
    tile isn't free (out of a door), on the player's tile hidden till they
    step off it (the tile behind checked as the player's step would be:
    no water, ledge or ramp). After a battle (patch 0087: its start noted
    by `EncounterEffect_Start`, `PortFollower_BattleStarts`) it's out at
    once as the field comes back, at its size, without its ball, behind
    the player, even while the field's task or the trainer's script runs;
    where that tile isn't free, out of its ball once the player steps off
    theirs (before, it came out of its ball a moment after the field was
    back, or after the trainer's words: 2026-11-05). As a trainer sees the
    player it stays out (patch 0090, 2026-11-06): the trainer's script
    starts on the step that brought the player into its sight, and that
    step was taken for the game moving the player, so it went into its
    ball before every trainer's battle (33 times in a log). The player's
    tile as a script starts, and through the rest of a step under way then
    (16 frames at most), is where they are; a script's own walk of the
    player still sends it in, as does someone coming to its tile (the
    trainer walking up from behind). On the PC, Route 202's Youngster
    Tristan (his flag cleared): stepping east into his sight it stays out
    beside the player as he comes over (before: "in its ball (the game
    moves the player)"). That script's lock pauses every map object's
    movement, and the follower had set off on its own step after the
    player in the same frame: its tile was the one beside the player, but
    its picture stayed on the tile it set off from, two from the player,
    while the trainer came over (Route 204's Aroma Lady Taylor, a photo of
    2026-11-06). Since patch 0094 a step it's paused in is finished first,
    as the game does for the partner who walks with the player
    (`ScrCmd_LockAll`): unpaused till it stands, then paused again while
    the player is (`[follower] stopped by the game in a step: the step
    finished first`). On the PC, Taylor's flag cleared, walking east along
    z 694 into her sight: it stands beside the player as she comes over
    (before: a tile further back). Up or down stairs or an escalator and into
    a cave or a forest it goes with the player, as HeartGold's does
    (patch 0064: during a warp's way in or out, the task
    `sub_02056BDC`/`sub_02056C18` start, `FieldSystem_PortWarpKind`): out
    at once on the other side, without its ball, behind the player or on
    their tile (the stairs') shown once they've stepped off it and stand,
    or it sets off after them. Through a building's door (in by it,
    transition 1, or out onto it) it goes into its ball and comes out of
    it, as before (Gabriel's choice, 2026-11-01). (It went into its ball
    on the stairs too, and came out only once the player stood still:
    walking on up a building's stairs it stayed in, 2026-11-01.) Going
    into its ball it waits up to 20 frames for room for its ball's
    picture (handing the party to the nurse the player's own picture
    changes in that frame, and it went in with no ball). Back in its ball whenever the game moves
    the player (a script's walk; out again when the player is theirs
    again: scenes where it stays out may come later), when someone walks
    onto its tile in a script (a person coming up behind the player: the
    two stood in each other, 2026-10-29). Down a ledge it jumps after the
    player (patch 0078, 2026-11-05): its way is the player's tiles as
    they go, the ledge's left out (`PortFollower_NoteTrail`; the follow
    code took it to the tile the player left, the ledge's own halfway
    through their jump), so as they jump it walks to where they jumped
    from, and as they go on it jumps down after them, 2 tiles in their
    step's time (8 frames walking, 4 running: `PortFollower_JumpFrames`),
    behind them as before. Where it can't (not by that tile as they jump,
    or a 3 tiles' jump) it goes into its ball while they jump: out again
    where they stop, or, the player walking on, behind
    them as they go, growing out of nothing without its ball (which would
    be left behind), not waiting for them to stop. Its graphics IDs from 0x4000, past
    the berry trees' (0x1000) and OBJ_EVENT_GFX_INVISIBLE (0x2000). Its
    Poké Ball: coming out it grows from nothing (its picture made at no
    size, so it never shows whole first), going in it shrinks away where
    it stands, 6 frames each, with the battle's send-out and recall
    sounds. The ball itself is seen (the item ball's picture, a map
    object of its own: local ID 0xF4, not saved, not found by scripts,
    walked through; no shadow; on the Pokémon's own spot, its ground
    line the player's): coming out it drops in and opens; going
    in the Pokémon shrinks into it and it stays a moment. It's the ball
    the Pokémon was caught in (`MON_DATA_POKEBALL`), closed or open, as
    the battles draw it: `builder/ball_sprites.py` makes each of the 17
    kinds from the ROM's battle ball sprites (`pl_batt_obj.narc`: the
    closed cell and the open one) two rows and columns smaller, the
    overworld item ball's size: carved, not resampled, the rows and
    columns most like their neighbours taken (the columns in mirrored
    pairs), so the band, the button and the markings stay (resampled to
    3/4 they went); the closed ball's outline then drawn again as a
    circle 12 across, its inside carved to fit within it (carved whole,
    the outline went diamond-like), into a port file of the data pack
    (`0x10000001`, with the follower sprites); without it, the item ball.
    Open and closed are one sprite, open facing north: the ball opens and
    closes by turning, the same object, its texture already loaded. (Made
    again as it opened or closed, a new object, it was missing for two
    scenes while its texture loaded, drawn once with the texture not there
    yet, a block of noise, and paired by the 60 FPS in-between scenes with
    the player beside it: a flicker to the side, 2026-10-29.) As it opens, the battle's
    send-out burst for its kind (`ball_particle.narc`, a particle system of
    its own drawn after the billboards with the field's camera, its sizes
    and speeds 3.25 times: the battle's camera is a few units away, the
    field's units about a pixel), only when the field heap and the 3D VRAM
    have room for it; it goes along with the Pokémon (its emitters and
    their particles moved as it moves) if the player walks on as it
    comes out, from where the Pokémon is as its ball opens (patch 0059:
    on the player's tile the ball opens once the player has stepped off,
    the Pokémon already walking after them, and the burst from the ball
    went along a tile behind it, 2026-10-30). The Pokémon has
    no shadow while it's smaller than itself. Out and standing it steps
    in place, as HeartGold's do (the walk at a slow walk's pace). Talked
    to (A facing it, out): it turns to the player, its cry, a bubble over
    it (a heart, a note, "...", "!", "?": the builder's own drawings, in
    the ball file) and, happy, a hop; then a word, the port's text
    (`PortFollower_TryTalk`, from `field_control.c`): unwell with a status
    condition; else half the time a line for the weather (rain, snow,
    dust, fog; a Water type loves the rain, an Ice type the snow), the
    place (a cave, a Pokémon Center, outdoors) or the time (night: sleepy,
    or livelier for a Ghost or Dark type; morning); else its friendship
    ("... is overjoyed to be walking with you!", "... looks very happy.",
    "... is happy to see you.", "... is keeping an eye on you."). Now and
    then it has found something (friendship 100 or more, 1 talk in 5, 3 a
    day while the game runs, room in the bag): a common item 70% of the
    time (Potion, Antidote, Parlyz Heal, Poké Ball, Oran or Pecha Berry),
    less common 25% (Super Potion, Great Ball, Ether, Leppa Berry,
    TinyMushroom, Honey), rare 5% (Ultra Ball, Revive, Rare Candy, Nugget,
    Stardust), with the item fanfare. The sounds are in the battle's sound bank, which the field never
    loads: the field's sound effects load them too (`sound.c`), the sound
    heap 128 KB larger for their 84 KB of waves (`sound_system.h`; the
    DS's heap had 74 KB left there). The log says when it comes out, why
    it goes back, and what it waits for when it can't come out ("no map
    object free", "no sprites", "no room for its picture yet"), and when
    the first Pokémon has no sprite (a data pack made without the
    follower sprites: only Platinum's 24). The map objects this code adds
    (the Pokémon, its ball, its bubble) start at the height of the one
    they're made for (patch 0073): at height 0, as a new map object is,
    the ground's height taken nearest it put them on the lower ground
    under a bridge (Route 208's wooden ones): the bubble showed at the
    Pokémon's feet, 2026-11-03.
  - ALWAYS CATCH (every ball catches), INSTANT VICTORY (the player's
    attacks knock the opponent out, Endure and a Focus Band or Sash no
    help; a Substitute still takes the hit), FAST EGGS (the day care's chance at
    an egg every step rather than every 256; an egg in the party hatches
    in a couple of steps), INFINITE MONEY (as much as can be had,
    whatever's spent; the save's own amount stays), WILD ENCOUNTERS
    (normal, off, 1/4, 1/2, 2x; where there are none, none), SHINY ODDS (1/8192 as the game has it,
    1/4096, 1/1024, 1/256, 1/64, always).
- **The port's own settings** (`port_options.c`,
  `nitro_port_options.h`): 60 FPS, smooth 3D, the start tab, the
  enhancements and the cheats, a whole number each, kept in `options.txt`
  beside the save (`name value` a line, only those chosen on the page:
  the others follow their defaults; written to a new file put in the old
  one's place, the new one read if the power went off between). The 3DS takes `remaster.txt`'s `fps60` and `smooth3d`
  as their defaults, the file's over them, and applies a change at once:
  60 FPS from the next frame, smooth 3D from the compositor's next (its
  buffers now made whatever the setting, so it can be turned on). The PC
  runner keeps the file beside its save.
- **The apps' hand-over:** the start menu opens the panel's apps
  (`StartMenu_PortAppIsMenus`: the field's own app, the start menu's task
  the field's; not a script's, a registered item's, or an app's own like
  the Poffin Case's party) and knows, when one's gone, whether it went behind
  the panel's black (`StartMenu_PortAppGone`: the menu still kept): then
  the next one pops in, or the field comes and the panel fades in;
  else what comes next fades in as on the DS. Walking again with the menu
  still kept, or the upper screen held (an app gone a way not foreseen),
  they're let go (`StartMenu_PortUpdateButtons`).
- **The upper screen held** (`NitroPort_LowerMenuHoldUpper`): while an app
  is in the panel, the upper screen keeps the field. Let go (0), the
  game's upper picture shows again once it's as bright as what was kept:
  back in the field after a fade, at the end of its fade in (seamless);
  kept black (the bag opened after a fade), at once. Let go now
  (`NITRO_LOWERMENU_LET_GO_NOW`), it shows the game's picture from the next
  frame however bright (back from the bag behind the panel's black): the
  first one made since (`NitroPort_LowerMenuUpperLetGoShows`; half a
  second at most), not the app's hidden screen from before it. Kept
  showing the field's picture (`NITRO_LOWERMENU_HOLD_FIELD`, the start
  menu's panel apps): the platforms copy the upper screen's picture as it
  begins to be kept at full brightness (the field, a panel app opening
  from it) and put it back when an app in the panel comes after one that
  took both screens (the bag after an evolution, the party after the
  mail), whose fade out left black there; forgotten once the player walks
  again (`NitroPort_LowerMenuForgetField`). A copy kept already isn't
  copied over when the field's picture is asked for again: the bag after
  an evolution started being kept at full brightness, the evolution's
  screen gone dark in its palette fade (not a brightness fade) on the
  LCD, and that was kept as the field and shown frozen above the bag
  (2026-10-26). The 3DS puts the copy on the LCD itself (its own framebuffer, once nothing of the upper screen's is
  waiting for its V-blank).
  (A soft reset starts the program again: nothing's kept.) The 3DS
  doesn't draw a screen while it's
  kept (what's on its LCD stays); drawn again regardless (back from the
  HOME menu), it's black. The PC runner's `rshot` and `mshot` show what's
  kept.
- **The OAM entries not used** (`patches/NitroSystem/0005`): NitroSystem's
  OAM manager parks the entries not used in a frame at line 192, just
  under the DS's screen. Tall, line 192 shows: switching the bag's pockets
  left stray pieces of sprites there (red lines, scrambled icons). They're
  hidden outright now (the entry's disable bit), as they always were on
  the DS.
- **Touch:** a touch that begins in the panel is the game's, at the DS's
  pixel there (dragged out of the panel, the nearest pixel of it); one that
  begins on a button that can be used shows it pressed, and letting go on
  it is a press. The start menu takes it; with the menu closed, the field
  opens it straight on that button (`StartMenu_PortTapOpens`; MAP and
  POKéTCH only change the panel, the menu staying closed). With the map in
  the panel, the panel's touches are the menu's (a tap a press,
  `NITRO_LOWERMENU_PANEL_PRESS`), not the Pokétch's; while it's dark,
  nobody's.
  A press not taken within half a second is dropped (one made while a
  script ran isn't used long after).
- **The DS's fades** cover the whole screen, the menu too (the panel's own
  only the panel).
- **The PC runner:** `mshot PATH` writes the lower screen as the 3DS shows
  it (320 x 240), `cshot PATH` the panel's picture at the DS's pixels (256 x
  248 when it's tall, 256 x 247 put together), `mtouch X Y` touches it there. It has the menu unless
  started with `--classic-menu`. The map's drawn on the game's thread as
  it's handed over (the same pictures every run), or with `--map-thread`
  on a thread of its own as on the 3DS.

## Timing

The display thread builds a frame while the GPU draws the one before
(since PR #130). citro3d's frames don't allow it: C3D_FrameBegin waits for
the GPU before a frame's commands may start, into its one command list,
and the queue of GPU operations (fills, command lists, transfers) runs
only after C3D_FrameEnd. So the remaster has frames of its own
(`NitroPort_PicaFrameOpen`): two command lists used in turn, cut with
citro3d's own `C3Di_SplitFrame`, and two GX queues of its own, bound in
turn while a frame is built (its operations recorded, not run) and run
once the frame before is done (`flight_finish`; 128 entries, not
citro3d's 32). What the frame being drawn reads stays as it is until then:
its 2D sets (busy till the GPU's done with them: a publish that finds no
set free has the drawer draw it all again the next time), its quads (two
halves of one buffer), its vertices (two buffers, as before), its
textures and the outlines' lists (one it drew isn't changed or freed
before the GPU's done: `NitroPort_PicaGpuWait`). A frame goes on the LCD
at its V-blank once the GPU's done with it (the queue's callback counts
frames done); the lag (above) counts the V-blanks the GPU's last frame
took. Should a frame's queue be stopped from outside (the HOME menu
coming), it's run on after a fifth of a second. The log's
`[perf] display` line has the time the display thread still waited for
the GPU.

Each 3D scene carries the V-blank count when the game finished it (its
SWAP), and each published 2D picture the count when the drawers read the
game's display state. A frame pairs the 3D with the newest 2D no newer
than the scene + 1 (the DS shows a scene the frame after its 2D was set),
and is shown `lag` V-blanks after the scene was made: the 3D comes at the
game's own even pace (every second V-blank in the field), not whenever a
scene happens to be done. `lag` starts at 2 and adapts (1 to 4): up when a
frame would be late, back down after 600 frames in a row with a V-blank to
spare, and twice as long before the next try each time a lowered lag
doesn't hold (up to 16 times as long: 5 minutes). (Counting the frames
with room since the last late one, it went down every 20-26 seconds in the
field, a frame came late, and it went back up: a hitch every 26 seconds.)
With 60 frames a second (below) an in-between scene comes a V-blank before
each scene: one late for its V-blank (it would show at its scene's, and be
skipped for it: a step of 30 a second) raises the lag as a late scene
does, and the lag is lowered only after frames with two V-blanks to spare,
so that the in-between scenes are still on time with it lowered. The
`[perf] display` line counts the late in-between scenes apart.

A new scene is drawn once engine A's drawer is done with the 2D of the
frame after it, or it's known there's none (the game hands the drawers a
frame every other V-blank, or the drawer was busy and left that one out).
A scene still waiting when the next one comes is drawn then, with the 2D
there is, so every scene shows. Drawn as soon as it came, it went out with
the 2D before and was drawn again when that came: the 3D drawn twice, and
if the 2D was late, a frame of the wrong picture (the title's Giratina
showed, bright, for a frame before its fade in from black). Two versions
that went wrong: waiting up to a few V-blanks, where engine A's drawer
takes more than a frame (the opening's Pokemon on green, the battles:
16-25 ms) each scene was overtaken by the next before its 2D came, and
only 2 to 9 of 30 a second were shown, the 3D's screen standing still;
cut at the V-blank after the 2D's frame, where the drawer takes about a
frame (the forest, 20 ms) a scene went out with that 2D or the one before
by turns, and the forest's clouds (2D) jumped back and forth over the
trees.

Engine A's drawer is the slower one where there are pictures: the 2D at
320 pixels, and each BG's key worked out every frame to look its picture
up. The key (FNV-1a, a 64-bit multiply a byte of the BG's map, tiles and
palettes) is worked out again only when a quicker hash of the same words
changes; a picture is drawn along a line without a division a pixel (the
ARM11 has no divide instruction: tens of thousands a frame in the
opening); the pictures are read at the start (`[wide art] N read ahead in
X ms`), not as each first shows.

The Underground has the field on the lower screen (engine A, the 3D,
there). With the pixel-perfect field option it's at the DS's pixel size
there too (`NitroPort_PixelFieldLower`, `NitroPort_WideLayoutLower`): the
3D's view 320 x 240, the 2D's picture 320 x 240 with the DS's in its
middle, a pixel a pixel on the lower screen's 320 x 240, more of the
tunnels around the player, the text box at the bottom across the
screen. The lower screen's layer textures are made for 2 texels a pixel
of the DS's 256; a picture too wide for that is published at a texel a
pixel (`LayerSet.texScale`), which is what a pixel a pixel needs anyway
(no room in the linear heap for textures twice as wide). With the display
made smaller for the memory (the 2D a texel a pixel, `NitroPort_
ComposeInit`) the lower screen's textures were the DS's 256 wide: the
Underground's 320 didn't fit, its picture was never published, and its
lower screen stayed black. That smaller display is for the 64 MB memory
mode (issue #188), but it's taken when less than `COMPOSE_TEXTURE_ROOM`
of linear memory is left for the 3D's textures, and from the place name
sign's textures on (192 KB, 2026-11-04) the New 3DS had 4069 KB left, 27
under the 4 MB it was then: every log of its since says "the display made
smaller for the memory". The room is 3 MB now (the 3D's texture cache
lets older textures go when there's no memory; the New 3DS's logs before
the sign had 4253 KB left after the title screen, 2571 at the fewest
through a run), the lower screen's textures are at least 320 wide on the
wider field whatever the display (`alloc_sets`' `leastW`: 512 already at
2 texels a pixel), a picture too big for its screen's textures says so in
the log once (`[compose] a W x H picture too big ...`), and a picture
published a texel a pixel has its right column and bottom row repeated
past it (`repeat_edges16/32`), as the doubled ones have: drawn bilinear,
its edge pixels were mixed with what a taller picture left there (the
lower screen as the tall menu, 248 rows), a line of another colour along
the bottom. `NitroPort_
PixelField` is now the upper screen's only (engine A on top). The
wireless signal icon, at the DS screen's top right, is moved with engine
A's picture (`NitroPort_EngineABeside`, patch 0065) to the 3DS screen's
corner. The sprites over a window moved to the screen's edge (the
Underground's menu: its icons and cursor) go with it: the window's place
is taken round the BG map's 512 as the BG's drawing has it, since a
pixel-perfect field's BGs are scrolled by its top (the window came out
hundreds of lines away from the sprites over it, and they stayed behind).

The drawing threads draw from the display state as it was when the game
handed them the frame: its registers, palettes and OAM are copied then
(`NitroPort_Render2DTakeState`, a few KB), so the game's next frame (its
sprites, scroll, colours) can't get mixed into a picture that takes long
to draw (sprites showed in the wrong places for a frame). The BGs' maps
are copied with them (`TakeMaps`, the screen blocks the BGs on show: 2-8
KB each): the game writes them in its next frame's work too, before that
frame's V-blank (a window it opens is copied to the VRAM at once,
`Window_CopyToVRAM`), and drawn with the windows, registers and palettes
taken before it, a YES/NO box showed for a frame where the DS has it, its
frame black, then snapped to the wider screen's edge; a text box showed
first in the middle with its frame black (2026-11-01). The tiles those
maps show are taken with them (each once), and a bitmap BG's picture: the
options page is drawn whole again when anything on it changes, its alike
tiles shared, so a tile's number goes to another picture each time, and
its new tiles showed through the map taken for a frame, mixed up. The
rest of the VRAM (the OBJs', the extended palettes) is read as it is:
before its next V-blank work the game waits for a drawer still reading
it. Working out whether a screen changed compares the small parts
first, and the VRAM only when they're the same: since 2026-10-30 only the
VRAM the screen shows (`EngineShownVramSignature`): for each BG on, its
whole map, the tiles the map uses and their palettes; the OBJs' VRAM while
an OBJ is shown; the VRAM display's bank. All of it (864 KB for engine A)
was read each frame nothing changed, about 15 ms of every 33 on the 3D's
core, half of engine A's drawer's time, and the 3D's own VRAM was in it
(bank E, its textures' palettes, in the field): the upper screen's 2D was
drawn again 5-11 times a second in the towns with nothing of it changed.
All of it still with an H-blank effect or a bitmap BG. When the small
parts change, the VRAM it shows is noted with the picture (when that's
quick: no OBJ shown, no bitmap), not on the next frame with a second
drawing. The `[perf] game` line says why each drawer drew (`upper 29.9/s
(6.0/s new: 4.0 for registers, palettes or sprites, 2.0 for VRAM), 16.2
ms, 4.6 of it handing over (core 2)`: handing over is the remaster's
upscaling into the compositor's textures, `ShowScreen`), and for engine A
the parts of its time a frame: finding out whether the picture changed,
drawing it, and of that the HM cut-in's sprites
(`NitroPort_Render2DTimes`; the cut-in's pictures took 37-77 ms a frame
under Route 206's bridge, log 31a6ea6d, 15-32 in Eterna before).

Where things run on a New 3DS: the game, the display thread and engine B's
drawer on core 0 (the game first); engine A's drawer and the geometry on
core 2, the geometry first (it has a scene to finish every 2 V-blanks or
it's left out; with the drawer first, the upper screen's 2D changing every
frame, text or the HM cut-in, left out 6-10 scenes a second and the field
dropped to 48-52 frames a second, 2026-10-30), but for the drawer's
reading of the game's display state (and drawing from it), which the game
waits for before its next frame's display work: behind the geometry (25
ms at a stretch where the maps change) the game waited for it and stepped
unevenly, Eterna to Route 205 (2026-10-30). Handing the picture over (the
upscaling) is below the geometry. While a battle's start plays in the
field (`NitroPort_FieldEffect`: the Poké Ball splitting in two and the
like) the field stands still, but its 3D was made again every scene, on
the core that draws the effect's 2D: on the Cycling Road the geometry
took 21-29 ms a scene and engine A's drawer got 8-15 pictures a second
(50-87 ms each), the effect at a third of its speed (2026-11-01). Then a
scene the same as the one before, word for word (the 3D registers too,
not its time; hashed on the game's thread as the commands go into the
ring), has the next left out, as one the thread can't keep up with: the
one before stays on the screen. If it moved after all, it's there a scene
later. Not outside such an effect: the first step after standing still
would be a scene late. The scene that stays stands for the frames of those
left out (`NitroPort_Gx3dSameAs`: the newest one's time and SWAP_BUFFERS
count; the display thread gives the scene on the screen those): the 2D of
their frames goes with it, and their brightness and scrolls. Before that
(2026-11-04) the screen kept the 2D of the frame the scene was made in
while the field stood still: the battle start's first flash to black
stuck dark for three times as long and the second was missed (videos
against a DS's), and the slices stood still between the camera's steps.
The next in-between scene starts from the newest of them too. To the
compositor that's a new 3D frame (the scene's SWAP_BUFFERS count changed:
`Screen.shownSwap3d`), shown at its time once its 2D and its note are there,
each in turn after the scene's own frame; only where the scene on the
screen is the one before those left out (not one left out for the
thread's time). The `[perf] geometry` line counts them (`left out/s
(n as the same)`), and a `[perf] geometry's work` line says where its time
goes: ending scenes (the SWAP: sorting, matching, publishing), and the
vertices a scene sends, of them those in lists wholly out of the view (not
worked out), and polygons; and a `[perf] shapes' lists` line, what the
game's side leaves out (below) and the time it takes.

Where the geometry's time goes was measured on recordings of the Cycling
Road ridden end to end, replayed as the 3DS has it (`gx3d_replay --view
400 240 --interp --shadows`: the pixel field's view, the in-between
scenes, the sun shadows; callgrind, 2026-11-01): placing vertices on the
screen, decoding the commands, the in-between scenes (a third), and
transforming and lighting. On the 3DS (log fcde80ee, 2026-11-02) ending
scenes was 10-16 ms of the road's 25-29 ms a scene. Leaving out each
shape's polygons out of the view on the game's side, every scene (cutting
its display lists in groups by their bounding boxes), cost the PC 0.1 ms
a frame and the 3DS 4 ms: the game's 2-3 ms a frame became 6-7, and the
display, on its core, dropped to 48-56 frames a second; not done. Now:
with the thread behind, ending a scene a V-blank or more after the game
did, it made no in-between scene for it (`interp_mid`; the frame between
shows the scene twice) rather than have the scene after the next left
out (a jump). That left out 10-13 a second while the HM cut-in or a
battle's start played (log 24186035: the 2D's drawing, 30-60 ms a
picture on the same core, held the thread up a V-blank or two, and it
caught up): now only with the thread a whole scene behind, the game
having ended the next one already. (For a day they were left out a
V-blank late too while the 2D's drawing shared the thread's core, a
picture drawn there in the last 4 V-blanks, to get the HM cut-in's
pictures out sooner: 6-11 a second were left out through the cut-in's
text and start and as the place names' signs slid in, the field
stuttering, log 6fb7c4a4, against 0-0.5 before, log 31a6ea6d. Taken out
again, 2026-11-03.) The `[perf] geometry's work` line has the in-between
scenes' share of ending scenes (on the road, 2026-11-02: 9-15 ms of the
25-30 a scene, nearly all of ending scenes), of that the matching of
their polygons with the scene before's, and how many it left out (on
the road, 2026-11-02: matching was 3-9 ms of a 8-15 ms in-between scene;
standing, under 1). Matching reads what it compares of each record (its
kind, its corners' count, in front of the camera, its corners on the
screen) from tables of their own, 8 and 32 bytes a record, rather than
the 160-byte records (`RecTag`, `RecPos`): the window's search looked at
up to 129 records for a polygon, a cache line or two each (in a 3DS-like
cache, 28% fewer misses in the in-between scenes; the same pairs, bit
for bit). A `[perf] in-between matching` line has its parts: the order's
pairs, the scene's motion, each by where it is, one each, and how many
polygons the window searched for: on the road (3DS log 10c1917f) each by
where it is was 3-7 ms of it, 400-500 of a scene's 700-900 polygons
searched for, 60,000 places looked at. Polygons going out of the view at
one side and coming in at the other shift the order by a few, and every
pair after it was the order's neighbour of its kind; and the ground's
rows move their own ways in the perspective, a pixel or two off the
scene's motion. Before the window's search now: the place one off the
order as the last pair was, and where the pair before moved (when it's
within MATCH_LOCAL, 2 pixels, of the scene's motion), each a pixel at
most from where it puts it (`match_fits`). The road's recordings: a
quarter of the searches; 47 of 1.2 million pairs different, 36 of them a
pair where there was none, none further than 0.3 pixels. The in-between
scene's polygons wholly inside the view in both scenes (most of them;
`PolyRecord.screenOk`, the same viewport) aren't clipped and projected
again (`replay_polygon`: three divisions a vertex, the clipping's
copies): halfway in clip space is inside the view too, and its place on
the screen is between the two scenes' (the corners matching has, the
depths `to_screen` gave) weighed by their w (`x = (X0 + X1) / (W0 +
W1)`, the same for y and depth; one division a vertex,
`replay_blended`). The road's recordings, against the redraw: the same
triangles; x and y the same but for 0.3% of them, those 1/256 pixel
off; depth at most 3 of 16 million off. In callgrind, the in-between scenes' work 22%
less. Those near the screen's edges,
behind the camera, or with no pair are redrawn as before. On the 3DS (log
24186035, the road's median against 33073953): the in-between's own part
6.2 → 4.7 ms a scene, the geometry 23.9 → 20.0. Lighting keeps
64 normals' colours, forgotten together when the vector matrix, the
lights, the material or the polygon's lights change (it was the last
normal's, forgotten at every matrix command: 12% less lighting work); the
sun shadows' triangles take their vertices' screen positions with the
drawn ones' (`screen_of`). With the sun shadows on, the thread passed
over lists out of the view that they keep (beside it, within their
reach: a building just off the screen lost its shadow on the 3DS, not on
one core), and in-between scenes matched polygons out of the view only
then: now both as on one core, and the same with the shadows on or off. Core 1 is the system's; a
program gets 30% of it, and there the
geometry took 18-30 ms a scene and left scenes out (the intro's flyover
jumped), and engine B's screen drew 14-25 frames a second
(`geometry.txt`: `core 1` puts both back there).

Showing is apart from drawing. The display thread draws a frame, has the
GPU put it into one of the screen's own LCD framebuffers (the upper
screen's seven, the lower's five, since PR #138; fewer if the linear heap
hasn't room, three at least), and queues it with the
V-blank it's for; a V-blank thread (`main_3ds.c`) counts the V-blanks and
puts each frame on the LCD at its one (`GSPGPU_SetBufferSwap`; of the
frames whose V-blank has come and the GPU has drawn, the newest, the
older skipped). The display thread never waits for a V-blank: it goes on
to the next scene. (The first version waited inside the frame, and the
scenes made during the wait were never drawn: 10 to 14 frames a second of
a game making 30, and the waiting grew the lag.) A screen whose
framebuffers are all taken (one on the LCD, the one before it still
scanned out till the swap takes, the frames waiting) isn't drawn again
until one has shown: at 60 frames a second a frame is built 1-2 V-blanks
after its scene and shown up to 5 after, so 4-5 wait at once (with three
framebuffers and the lag at 4, only the in-between scenes got one and the
scenes were skipped: 45 frames a second of which 15 scenes, and the lag
never came down, 2026-10-20). A scene that didn't get a framebuffer (or
its 2D) is drawn before the next scene takes its place, and kept while
only one newer scene waits.

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
| `edges on` | the outlines (the DS's edge marking round the 3D) with the slider down (default off, 2026-10-20: on the bigger screen they get in the picture's way, and they took 8-9 ms of a slider-down frame) |
| `edges drawn` | the same, their left and right moved passes drawn, as before they were copied (for comparing: the same picture, slower) |
| `edges pruned` | the same, each outline pass drawing only the polygons it needs (worked out on the CPU each scene; the same picture). Slower on a 3DS (see below) |
| `edges3d on` | (with `edges on`) the outlines with the 3D slider up too, while they fit in a frame (default off: drawn for each eye they cost 7-16 ms of the GPU's an eye; see How it works) |
| `fps60 off` | the field at the game's 30 frames a second (default on: an in-between scene shown between each two, while frames fit, in the game itself: not the opening, the title, the cutscenes; see 60 frames a second) |
| `smooth3d off` / `smooth3d stereo` | the upper screen drawn at its own resolution / so only with the 3D slider down (default on: twice it and scaled down, no jagged edges, slider up or down; see How it works) |
| `filter3d sharp` / `filter3d smooth` | the 3D's textures each texel 2x2 then bilinear / bilinear (default `nearest`: the DS's; the others cost the GPU two to three times as much) |
| `shadows on` | sun shadows on the 3D (and no round shadow under objects), the sun by the time of day. **Parked on the 3DS** (they hang its GPU; see below): off there until fixed |
| `shadowdebug on` | debug only (for the sun shadows' hunt, with them on again): wait for GPU completion before/after the sun map, after the opaque scene, and after direct shadow receiving. Slower; the watchdog can identify the outstanding pass. Default off |
| `grade on` | the colour grade on the 3D's textures, by the time of day |
| `sun 55,32` | the sun fixed: degrees above the ground, and from the screen's left (0) towards its top (90); `sun clock` (the default) by the time of day |
| `filter3d xbrz` / `filter3d xbrz-smooth` | the 3D's textures 2x by xBRZ, nearest / bilinear (smooth: no shimmer while the picture moves, but sharp's cost and lines may come back). On trial: the [perf] line's 3D times say what it costs |
| `flip on` | the 2D layers upside down (if a GPU's texture rows come out the other way round) |

A frame that took long (more than 34 ms of the game's own, a scene's
time at 30 a second: the field missed a scene) is in the log with where
its time went: `[stall] frame N: X ms in the game (file reads: n, KB, ms,
of them from the SD card n, KB; waiting for the geometry thread: ms)`, or
`X ms in the port between frames (...)`.

The data pack is read through blocks kept in memory (`3ds_port/pak`, 8 KB
each: the game's reads are mostly small, an archive's header, a file
table, a few glyphs, and each read of the SD card costs about 1 ms). 4 MB
of them on an Old 3DS, 16 MB on a New 3DS (`pak_set_cache_blocks`), and
before the game starts the field's small files are read into them
(`NitroPort_PakWarmField`, about 1.4 MB: the font, the place name's frame,
the zones' events, encounters and scripts, the people's list, the field
effects, the weather...): going between a route and a town the game makes
120-240 small reads in a frame, and the field's streaming (its map
blocks, buildings, music) pushed those files out of 4 MB (2026-10-30).
The log file is written by a thread of its own, at the lowest priority, a
moment after the lines come: written on the thread that logged, each line
cost the SD card's time, and the `[perf]` lines held the game up 35 ms
every two seconds. A crash, a stop with a message and closing write what's
waiting at once.

The save is kept in memory (read once when the game first opens it, one
request to the SD card); each change the game makes is written to the file
before the game is told it's done, as the DS's flash had it. Loading a save
at Continue took 1.8 s, the save read 256 bytes at a time. The game writes
the save a page (256 bytes) at a time too: a write's pages go into the file
together, with its last one (one write to the SD card for each block of the
save and each footer, not 512), so a save no longer holds the game up 3.3 s
on the 3DS (2026-10-16). What the game waits for is still the whole write
in the file; a write cut short leaves the file as it was (the save's other
copy whole). The HOME menu, sleep and closing write anything waiting. The
log says `[save] N KB into the file in N ms` for each write of 4 KB or
more.

The log says what's on (`[compose] screens drawn by the GPU: ...`) and,
every two seconds, `[perf] display: N frames/s drawn (N with a new 3D
scene, of N scenes/s; N scenes not shown), each X ms building + X ms GPU;
N% in stereo, N% drawn 2x2, N% 2x1; 3D textures converted N (X ms each); 2D published N/s, X ms
each; 3D shown N V-blanks after the game made it, N late (N in-between); GPU command list
N% full at most, N draws cut`. In the field the frames with a new 3D
scene should match the scenes made (30 a second), none not shown;
building + GPU is what a frame costs the display thread, and must stay
under 33 ms for that.

A hitch is in the log with where its time went: `[hitch] a 3D frame N
V-blanks late (lag now N): made at V-blank S, reached the display thread
N V-blanks later, drawn from N after; X ms preparing, X ms building (N
textures converted: X ms), X ms GPU`, or `[hitch] a frame took the
display thread X ms: ...` (over two V-blanks: the scenes made meanwhile
may never show). At most one every 15 frames drawn.

Where the GPU's time goes: one frame with a 3D scene every 150 (about
five seconds) is cut into parts, and each part timed by the GPU's own
"done" interrupt: `[gpu] upper screen, left eye (stereo, outlines on):
the 2D behind X ms; the opaque polygons X ms; outlines, the shifted
passes X ms; outlines, the outlines drawn X ms; the translucent
polygons X ms; the 2D in front X ms;` (a line an eye, and the lower
screen's), then `[gpu] the frame: the clears X ms, the rest X ms, the
transfers (the outlines' copies, to the screens) X ms; X ms in all`.
Nothing changes on the other frames; the cuts are the ones the second
eye's outlines already use. (The shifted passes are the up one since the
left, right and down ones are copied.)

## 60 frames a second in the 3D

(In the game itself: the field, battles, menus over them. The screens made
of pictures, the opening and the title, the start of the game, the
credits and the cutscenes (`NitroPort_WidePictures`), stay at the game's
30, as made: their 2D animations flickered black and jittered between the
in-between frames, 2026-10-21.)

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

The geometry side is in (`NitroPort_Gx3dSetInterpolation`, port_gx3d.c;
off by default, at no cost): with it on, each polygon that isn't wholly
outside the view is kept as the game drew it (its vertices in clip space,
before clipping and culling), and at each SWAP_BUFFERS the new scene's
polygons are paired with the scene before's. First in order (the game
draws the same things in the same order), which gives the scene's motion
(the middle of the pairs' moves: most of a scene moves with the camera);
then each polygon again with the polygon of its kind (its attributes but
the alpha, its texture's format and size but not its address, its vertex
count) nearest to where it is less that motion, within 64 polygons of
where the order says, if nearer than 16 DS pixels plus one and a half
times the scene's motion: a map's many tiles of one kind are told apart
by where they are. One each: a polygon of the scene before paired with
two of the new scene's goes to the nearer (where the motion puts it, or
where it was: what the camera follows), the other has none (a figure
new beside one of its kind, the follower's Poké Ball beside the player,
took the player's pair and was drawn halfway to him: 2026-10-29). Each paired polygon goes halfway between its two
places, clipped and culled as any other; one with no pair is where the
scene's motion puts it halfway (half the motion back from where the new
scene has it: mostly polygons coming into the view at its edges), and
one of the scene before's that the motion takes out of the view goes
halfway out (half the motion on). Where the new scene has them, the
polygons coming in were half a step ahead of the rest for a frame, and
those going out were gone half a step early: the edge of a roof coming
in at the top flickered (2026-10-21). Too few pairs (under half: a cut, a new map) and
there's no in-between scene. In the PC runner: `interp 1`, then `midsim
PATH` draws the newest in-between scene as gpusim draws a scene. Walking
and running in Jubilife and on Route 202, the in-between scenes are
halfway (the scene 2 to 4 pixels on from the one before, the in-between
1 to 2), with nothing out of place.

GPU sprites (2026-10-30): the game moves its sprites 30 times a second
in the field, as it does everything, and the HM cut-in is nearly all
sprites (its wind's streaks, 24 pixels a step; its Pokémon sliding up to
64): at 30 next to the field's 60 it looked choppy. While the cut-in
plays (`NitroPort_SetFieldCells`, `NitroPort_GpuSprites`), engine A's
sprites are drawn by the compositor itself when they're all in front of
everything (`NitroRender2DSplit.gpuObjs`: normal ones, none behind a BG,
shown inside and outside the windows, no colour effect on them, no
H-blank effect): left out of the picture, their pictures in an atlas
(256 x 128, each picture once: `gpuObjAtlas`, the set's `objTex`), what
each sprite shows and where in it (`NitroGpuObj`). Where they are comes
from the game's thread, each frame it hands over
(`NitroPort_Render2DObjPlaces` into `NitroPort_ComposeNoteObjs`, as the
weather BG's scrolls): with a scene, the frame the DS shows it in; in an
in-between scene, halfway from the frame before
(`NitroPort_GpuObjsBetween`: each found again as the nearest of its kind,
each the other's nearest; one going off or just come on moved half as
far as most of its kind). Their places don't count as a change: the
picture isn't drawn again as they move, only when what they show does.
In the PC runner the picture draws them where it had them.

What they show changing (2026-11-03): the picture has none of them in it,
so when nothing else it's drawn from changed (its registers, the BGs'
palette, its maps and tiles: hashed without the OAM and the sprites'
palette and VRAM; with a bitmap BG or an H-blank effect besides the
windows' it's always drawn whole), only the sprites are made again, their places and the
atlas, and the layers are left as they were (`DrawChanged`, `sPicture`;
the picture drawn as far as the sprites, `sObjsOnly`). Drawn into the same
layers otherwise since (whole, when no set was free to hand it over in),
it's drawn whole the next time. The HM cut-in changes its sprites every
game step and nothing else, its band's lines being the compositor's: its
picture was drawn whole each time, 12-25 ms on the geometry's core on
Route 206 (10-14 pictures a second, the in-between scenes giving way,
50-55 frames a second: log c090eb75). On the PC, through a real Cut:
after the band's first picture, every one of them its sprites alone (77
of 78), and each frame the same as drawn whole (layers, sprites, atlas).
The `[perf]` game line says how many a second were the sprites only. In
`test_render.c`: `test_gpu_objs`. Their pixels (the atlas, made 512 x 256
texels and flushed for each picture) are made again for a set only when
they changed, and only the rows in use (`atlas_rows`, `atlas_key`): through
the cut-in its sprites mostly move, their pictures the same.

Their in-between frames without an in-between scene (2026-11-03): the
compositor's sprites are halfway in an in-between frame, but with the 3D
behind (its core shared with the 2D's drawing) its in-between scenes are
left out, 4-11 a second through the HM cut-in on Route 206 (log
8532faaf), and those frames were the frame before shown again: the
cut-in's wind and Pokémon not smooth, though the field stands still
behind them. The in-between frame for the 2D alone (`NitroPort_ComposeFrame`'s
mid 2, the scene's own 3D, as for a fade or a band: `fades_changed`) is
now drawn when the sprites moved too (`objs_moved`), them halfway from the
last scene shown's frame (`draw_gpu_objs` by `frame_before`).

The weather's drops (2026-11-05, patch 0079): the rain's drops are
sprites the game moves every step, so the upper screen's picture was drawn
whole for each, about 9 ms on the geometry's core on Route 215: 15 pictures
a second at most (each taking ~60 ms to show), the rain going at 15 and the
field held up as it began (log da31452d). While a weather with sprites
plays (`ov5_021D676C` starts it, `ov5_021D69B8` and `ov5_021D68B8` end it:
`NitroPort_SetWeatherSprites`), `NitroPort_GpuSprites` is 2 and its
sprites are drawn by the compositor as the cut-in's are, with two
differences in `GpuObjsOf`: a BG showing nothing doesn't count as in front
of them (the message box's BG3, closed, `BgTextBlank`), nor the place name
sign the compositor draws (`signBg`): each drop behind the sign then, and
the sign drawn over them (`ComposeLayers.gpuObjsUnderSign`: step 5b before
5a). With a text box up they're in the picture, as before. They don't go
with the windows (`Engine.objsStay`: the sign's window moved to the tall
field's top, the drops over it went too), and a refusal (`sGpuObjsRefused`)
is tried again each picture, as the rain goes on for minutes.

So that they fall at 60 frames a second, where the game moves them 30
times: the drops live a step or two and fall 20 to 118 pixels a step, no
sprite found again as the cut-in's are. So the game tells each one's move:
the weather's sprite list (`SpriteList_PortNoteMoves`) notes in
`SpriteList_Update` each sprite's place against where it was drawn in the
list's step before, `RenderOam_PortSetMove` ties it to the main screen's
OAM entries it adds, handed over with OAM (`RenderOam_Transfer`:
`NitroPort_SetOamMoves`, then `NitroGpuObj.moved` with the places). In an
in-between frame each is drawn half its move back, one just come on half
the middle move of its kind's, the frame before's not drawn
(`NitroPort_GpuObjsBetween`). On the PC, in the rain on Route 215: every
drop left out and told of, each told move the place it had in the frame
before (`[raindbg]`, local).

The place name sign in the rain (2026-11-05): the sign is drawn by the
compositor only with no sprite shown (`GpuSignOf`), and that's worked out
both for the picture and for each frame's notes
(`NitroPort_Render2DSignNow`). As the rain began the drops came and went,
so the sign was drawn in the picture in one frame and by the compositor in
the next, out of step with each other. Now a weather's sprites behind the
sign don't keep it in the picture (`gpuObjs` 2, the sprite's priority
below the BG's): the sign stays the compositor's, over the drops.

The rain's cost, second round (2026-11-05, log ced77ee5): with the drops
the compositor's, the upper screen's picture was still made again about
28 times a second in the rain (its sprites alone, `sObjsOnly`), and
handed over each time: ~30 ms a picture on the geometry's core, 10-15 ms
of it handing over, next to the geometry's 20-28 ms a scene on Route 215.
At four times the speed the game made 80-100 frames a second in the rain
and 150-216 without it, and with a text box up only 7-10 pictures a
second came through (each drawn whole, below). The change check hashed
the sprites' OAM entry by entry, their places taken out: the drops come
and go every step, OAM's entries taken in turn, so it changed every step.
Now it's what they show, as a set (`GpuObjKey`, sorted, each once:
`AddObjKey`, `ObjKeysHash`), and while all that show are of those the last
picture left out (`sObjKeysMade`, made by `MakeGpuObjs`; not kept by a
picture that didn't leave them out), it's that picture's: each kind of
drop isn't shown every step. The compositor draws each where the game had
it in the frame shown and finds its picture by what it shows, so the
picture needn't be made again for it. On the PC (the 3DS's check run each
other frame, local `drawcount`), standing in Route 215's rain for 600
frames: 300 pictures before, 0 now (116 with the set alone); walking: 80,
0. In `test_render.c`: `test_gpu_objs` (entries swapped, some hidden: no
change; a picture the last didn't have: drawn).

A text box in the rain (2026-11-05): the box's BG is in front of the 3D
and of the drops (priority 0; the drops 1, the 3D 1), so the drops were
put back in the picture and it was drawn whole for every step they took
(on the PC, the Cut tree's question up in Eterna's rain: 150 pictures in
300 frames; with the cost above, 7-10 a second on the 3DS, the text
coming slowly). Now a weather's sprites behind every BG in front of the 3D
and in front of the 3D can be left out too (`GpuObjsOf`, `gpuObjs` 2,
without the sign), when none of those BGs is alpha blended (the DS would
blend it with the drops): they're drawn by the compositor before the
layer in front of the 3D (`NitroRender2DSplit.gpuObjsUnder`: 1 + the
priority they're behind, as `ComposeLayers.gpuObjsUnder`; draw_screen's
step 5, the others at 5b), which hides them where it isn't clear, as the
DS has them. The PC's picture draws them so too (`spritesUnder`). The
cut-in's own (`gpuObjs` 1) are as before. On the PC, the same box: 0
pictures in 300 frames, the drops over the field and under the box. In
`test_render.c`: `test_text_box_objs`.

The rain's cost, third round (2026-11-05, log ac25aa6b): standing in the
rain the picture wasn't made again any more, but finding that out took the
upper screen's drawer 1.8 ms each time on the geometry's core (0.3 without
the rain), above the geometry, up to 60 times a second at four times the
speed, where the geometry is what holds the game back: four times the
speed went slower in the rain. With any sprite shown the check hashed all
of the OBJs' VRAM, 256 KB. Now only the tiles the sprites shown use, each
once, where they are and what's in them (`ShownVramSignature`, worked out
from each sprite's key, `GpuObjKey`, as `ObjCharAddr` finds its tiles); all
of it still with a bitmap OBJ. While all the sprites shown are of those the
last picture left out (`sObjKeysMade`), the tiles of all of those, so the
drops coming and going don't change it. Quick, it's noted as the picture's
drawn, as the BGs' are, so a picture with sprites isn't drawn again the
next time to be sure. On the PC, standing in Route 215's rain: 0 pictures
in 300 checks as before, each check 0.025 ms (0.063 before); Cut in
Eterna's rain all through, 63 pictures in 270 checks (76 before). Each check that found
no change compared with the picture drawn whole (local `dcverify`): no
difference the old check didn't have too. In `test_render.c`:
`test_obj_vram_check_tiles` (a tile no sprite shows: not a change; one
that shows, 2D mapping); now only with the compositor's sprites (below).

That round also moved the VRAM's note after the picture's drawing (so the
sprites it had just left out would count). The game goes on writing the
VRAM on its own core while the drawer draws: what it wrote after the
picture read the VRAM but before the note was in the note and not in the
picture, so it was never drawn (a battle's bars' tiles are written every
frame as they move). The note is taken before the drawing again, as it
always was: a write after it is a change the next time. (The
Underground's black lower screen, reported with it, was the smaller
display's instead: see the Underground above.) The PC runner never showed it (its check runs
between the game's frames). In `test_render.c`:
`test_vram_written_while_drawing` (a tile written at the drawing's last
clock call: drawn again the next time; it fails with the note after).

The bars still moved in jumps after that (a video of 2026-11-05: a wild
Bibarel's HP bar held for 0.25 to 0.8 seconds at a time and then jumped,
four or five steps in a 2-second drain, while the "It's not very
effective..." printed straight after it came a few letters every 2
V-blanks, as it should). The text is a BG, in the state taken as the frame
is handed over; the bars are sprite tiles, in the OBJ VRAM the check
hashes. So the tiles-only check is now used only where it was made for:
with the sprites drawn by the compositor (`gpuObjs`: the rain's drops, the
HM cut-in's). Everywhere else (a battle, the menus, the Underground) the
whole OBJ VRAM is hashed again, as before the rain's round: not worked
out when something else changed too (a picture with sprites is drawn
again the next time, to be sure), so a screen whose registers change
every frame, a battle's lower screen, doesn't hash it each time
(`ShownVramSignature`'s `objTilesOnly`). It's hashed 32
bytes at a time (`ObjVramParts`), each part's hash kept: a change only in
parts no sprite shown has its tiles in (what the tiles alone would have
missed) is counted in the log's `[perf] game` line ("for VRAM (N outside
the shown sprites' tiles)"), and in a battle, when that happens 4 checks
running (a bar's tiles written again and again, not the sprites loaded at
its start), the first 6 times are logged with the parts that changed and
where the sprites shown have their tiles (`[2d] engine A's OBJ VRAM
changed only where no sprite shown has its tiles`). On the PC the bar's
tiles are in the sprites' (no such line through a drain), so the next 3DS
log says which it was. In `test_render.c`: `test_obj_vram_check` (with
the sprites in the picture: any of the OBJ VRAM a change, counted when it's
outside the sprites' tiles; a sprite moved drawn twice, as before) and
`test_obj_vram_check_tiles` (with the compositor's sprites: a tile no
sprite shows not a change).

Cut in a heavy scene (the same log): at the top of Route 215 in the rain,
the geometry busy 80% of the time, the upper screen's drawer took 55-60 ms
to hand over each picture, below the geometry: the cut-in's steps came 3
to 7 times a second. A picture that's its sprites alone made again (the
cut-in's, every step: `NitroPort_Render2DLastSpritesOnly`) is now handed
over at the reading's priority, above the geometry: its layers are as they
were, little more than a memcmp of each and the sprites' atlas (`DrawerMain`,
`sSpritesOnlyDrawn`). A picture drawn whole still goes below the geometry.
And at its end the band opened all the way again for its last frames: the
frame noted with no band, the cut-in over, but the picture older, its band
wider (the drawer behind), so the band was drawn with the picture's lines.
Now, with the band drawn lately and narrower than the picture's, a frame
noted with none closes it where it was closing (`bg_band_rows`,
`frame_band_kind`); not noted, or noted unknown, as before.

The cut-in's band opening and closing (2026-11-01): a window, window 0
alone across the DS's screen, its lines (WIN0V) 5 to 13 further each side
a step, every other frame; inside it BG3 alone of the BGs (the band, a
512 x 256 map), outside it everything else. Drawn in the picture, it
opened and closed 30 times a second. Now the compositor shows it
(`NitroRender2DSplit.gpuCircle` 3, `gpuCircleOn` 3, `GpuBgRowsOf`): when
that BG is alone in front of the 3D (the other BGs behind it, the sprites
behind it or drawn by the compositor) and there's no H-blank effect, the
picture is drawn as outside the window everywhere with that BG too, so
the layer in front of the 3D is that BG alone; the compositor fills the
band's lines with the backdrop and shows that layer in them only, the
lines as the game had them in the frame shown, halfway in the in-between
frames (`bg_band_rows`, noted as `NitroPort_Render2DCircleNow`'s 3). Its
lines don't count as a change. Test: `test_bg_rows`. In the PC runner
(and on a 3DS without the compositor's BGs) the window is in the picture.
As it ends the game holds the band 16 lines tall for 4 steps (its rings,
then its BG off: the band the backdrop alone, `GpuBgRowsOf`'s 4, still
the compositor's) before it goes; after the steady close that looked like
a stop (a video of 2026-11-01). Held after closing, the band goes on
closing at its last speed (2 lines a frame each side), never opening again
while the game holds it: closed in 4 frames, by the V-blank (`sBgBand`). The
compositor's sprites (the cut-in's wind and Pokémon) are cut to the band's
lines while it closes, and while it's closed further than the game's
(`sObjClip`): the game still shows them for two steps, and over the field
the dots and the Pokémon's parts showed for a few frames after it closed.
Then the game deletes its sprites, puts BG3 back (cleared, its own size
again) and a game frame later takes the window away; the DS shows the
last of the sprites in the band's 16 lines till then. With the
compositor's sprites ended as they were deleted, those frames' pictures
had them drawn in, not a band the compositor draws: the band jumped back
open to 16 lines (the DS's), its close not seen through (2026-11-03,
noticed once the cut-in's pictures came quicker). Patch 0071 keeps them
the compositor's till the window goes (`CutIn_PortFieldCellsOff`).
In Eterna Forest (2026-11-03) the band was never the compositor's: the
3D is behind the forest's fog there (BG2, blended onto it), and the field
message box's BG (BG1, empty once the box has closed) is in front of the
3D too. So the band was in the picture, drawn whole at every step of its
opening and closing and every move of the fog, and the fog was the 2D's
in whole pixels, not the compositor's. Now a BG the compositor draws
itself (the fog) can be left out with the band's window on, when it shows
outside the band only (`GpuBgOutsideBandOf`: window 0 alone across the
screen, the BG outside it with the colour effect, not inside; judged by
the layers outside), and the band is the compositor's with it (skipped by
`GpuBgRowsOf`; the band's lines are filled over it, as inside them the DS
shows the band's BG alone); and a text BG showing nothing (its map all one
tile, that tile blank: `BgTextBlank`) doesn't count as in front of the 3D.
In `test_render.c`: `test_bg_rows`. On the PC through a real Cut at the
Old Chateau's tree: the band the compositor's from its first frame.
Before the band opens, its stripe (2 lines) slides on from the right, 64
of the DS's columns a step, every other frame (WIN0H 254-255, 190-255,
126-255, 62-255, then 0-255): not across the screen, so drawn in the
picture, whole each step, and in the forest the fog the 2D's then (the
squares on it before the cut-in, a test of 2026-11-03). The band's
columns are now the compositor's too: `GpuBgRowsOf` takes any WIN0H that
doesn't wrap round, `gpuCircleX` to `gpuCircleRight` on the canvas (to
its edge from the DS's, as `FillRange` has a window), noted as
`NitroPort_Render2DCircleNow`'s x (the two halves), halfway in the
in-between frames, the band's backdrop and BG drawn in them only
(`bg_band_rows`). And the frame the window comes on in, showing nothing
yet (lines 192 to 192) with the message box still up, keeps the fog the
compositor's (`GpuBgEmptyWindowOf`: everything as outside the window).

The band in the rain (2026-11-05): the drops are sprites behind the
band's BG (priority 1 to its 0), so `GpuObjsOf` said the sprites couldn't
be left out, while `GpuBgRowsOf` had already taken the band as the
compositor's on the promise that they would be (`GpuBandObjsOut`). So the
sprites were in the picture, in the layer in front of the 3D, and the
compositor shows that layer in the band's lines only: over the field the
drops were gone, and in the band they were drawn into the band's layer,
where the DS's band hides them (a report of 2026-11-05: the drops went
dark through the cut-in). Now the band's BG doesn't count as in front of
the sprites (`GpuObjsOf`'s `bandBg`, the BG `GpuBgRowsOf` found:
`Engine.rowsBg`), so they're left out with the band, and those behind its
BG (`NitroRender2DSplit.gpuObjsUnder`: 1 + its priority) are drawn by
the compositor before the band (over the field and the band's backdrop,
under its BG, which hides them where it isn't clear, as the DS has them;
not cut as the band closes), the cut-in's after it, as before. And the
band is the compositor's with the sprites drawn by it only when they can
be left out with it (`GpuBandRowsOf`, for the picture, its change check
and `NitroPort_Render2DCircleNow` alike); else the band and the sprites
are the picture's, as the DS has them. Without rain the cut-in is as it
was (on the PC through a real Cut in Eterna City: the band's and sprites'
choices the same frame by frame, old and new). In `test_render.c`:
`test_band_objs`.

Eterna Forest's light finer than a pixel (patch 0061, 2026-10-31): the
game keeps the clouds' place finer than a pixel as they follow the camera,
and sways them slowly (a table, a pixel every 8 frames); the register has
whole pixels, 1, 2 or 3 a step as the player walks, and halved in the
in-between frames to whole pixels again, the light moved 1, 1, 0, 1... a
frame against the ground's steady 2. The game hands the place over
(`NitroPort_SetBgFineScroll`, the sway eased between the table's entries);
it's noted with the frame (`NitroPort_Render2DBgOffsetsFine`: within 3
pixels of the register's, set in the frame before, else the register's)
and drawn there, halfway between two (as floats) in the in-between frames:
about 1.9 to 2.1 pixels a step, changing slowly. Halfway goes the shorter
way round 256 (the picture repeats every 256: a register gone from 202 to
459 is a pixel on).

The place name sign (2026-11-03): it slides in and out by its BG's
vertical scroll (BG3, 4 lines a game step, every other frame: 38 to 0,
then back), and every step drew the picture whole and handed it over
again: 30 steps a second at most, and on the 3D's core, the in-between
scenes giving way (the field stuttered as a route's or a city's sign came
and went). Now the compositor draws it (`NitroRender2DSplit.gpuSigns`,
`GpuSignOf`): when that BG is in front of everything (every other BG and
the 3D behind it, no sprite on the picture, no windows, no H-blank effect,
no colour effect on it as a first target), all it shows is in at most 7
rows of tiles of its 256 x 256 map's upper half (`BgTextRowsShown`: a band
at the screen's top, not a text box), no other copy of them comes on the
picture as it scrolls round its map, and on the tall field they're in the
BG's one window, moved up to the picture's top at the scroll 0 and now
(`GpuSignTallMove`), the
picture is drawn without it (`gpuSignLeftOut`) and the band of its lines
is drawn on its own as it would be at the scroll 0
(`NitroPort_Render2DSignPicture`: the canvas's width, the anchored sign at
its left edge; made again only when its key changes: the BG's map, tiles,
colours, its other scroll, the canvas and the windows). The compositor
draws that over the picture moved up by the BG's scroll in the frame shown
(the frame's noted scrolls, as the weather's), halfway in the in-between
frames (2 lines a frame), in an in-between frame of its own when only it
moved (`sign_moved`, as the GPU sprites), a texel a pixel (the set's
`signTex`, 512 x 64: 64 KB a set; at 2x2 texels a pixel, as the over
layer, the three sets would take 768 KB of the linear heap): on the field
at the DS's pixel size only (`NitroPort_PixelField`, the default), drawn
a pixel a pixel (BG3 alone, the sign's); elsewhere the picture draws it,
as before. As the picture isn't drawn again while the sign slides, it may
be of a frame before or after the one shown: each frame the game hands
over notes the sign then with its scrolls (`NitroPort_Render2DSignNow`:
the same test on the live state, on the last picture's canvas, and what
its band shows, hashed: `SignContentOf`), and the compositor draws it only
in a frame with the same sign up (the game puts it up scrolled 38, the
frame before having none, and takes it away cleared, scrolled 0: drawn
from the picture's own scroll, or the frame's 0, it showed whole for a
frame). A review of the first version found it took a frame's scroll only
within 8 lines of the picture's own, which is stale while it slides: on
the 3DS the sign would have stayed 38 up, 2 lines shown. Its vertical scroll
isn't a change once a picture has left it out (`sSignDrawn`): the picture
isn't drawn again as it slides. In `test_render.c`: `test_gpu_sign` (on
the DS's canvas, the wider one and the tall field's, each scroll of the
slide the same, pixel for pixel, as the picture drawn with it). The PC
runner draws it over its picture the same way.

Which frame goes with a scene (2026-10-30): the weather BG's scrolls, the
GPU sprites' places, the circle fade and the master brightness are noted
each frame the game hands over, with the count of SWAP_BUFFERS the game has
made then (`NitroPort_Gx3dSwapsMade`); a scene carries its own
(`NitroGx3dScene.swap_seq`, an in-between scene the one before's too) and
takes the last noted with it. By the V-blank clock alone (the first frame
1-4 V-blanks after the scene's stamp), the frame handed over in the same
V-blank as the scene was made was left out now and then (the game's frames
don't keep step with the 3DS's V-blanks) and the next step's taken: Eterna
Forest's light went back and forth about a pixel at 30 a second as the
player walked. The clock still where nothing was noted with the count.

A scene and its in-between scene wait for their frame to be noted
(`frame_note_due`, 2026-10-31): the game notes a frame in its V-blank work,
a V-blank after the scene's SWAP_BUFFERS, while the display thread has the
scene as soon as the 3D's made it, and drew the in-between scene at once.
That had no note of its own frame yet and took the frame before's (or the
picture's own, older still): videos at 120 frames a second showed Eterna
Forest's light shaking every other frame, the HM cut-in's streaks jumping
back a whole step now and then, and the circle closing into the forest
opening up again in its in-between frames. (Not when a later frame's
noted already: that scene's frame was left out, there's nothing to wait
for.) A GPU sprite whose picture isn't in the 2D's atlas yet (a frame of
its animation newer than the 2D drawn) is drawn with the nearest of its
size there instead of left out: the cut-in's trainer went out for a frame
as it swung.

The circle fades (walking into Eterna Forest) are drawn by the compositor
too (`NitroRender2DSplit.gpuCircle`): when the circle's window is the only
one, nothing shows outside it and all that's shown shows inside it, the
picture is drawn as inside it everywhere (with the inside's colour
effects), and a ring of 96 pieces in the backdrop's colour covers the rest,
its radius halfway in the in-between frames; the picture isn't drawn again
as it closes. Where a frame's note has no circle (the game's last step of
the fade turns it off, its window still on), the frame before's is drawn,
else the last one noted: never the picture's own, which is steps behind
(the picture isn't drawn again as the circle closes): the in-between frame
after the last step went back to a circle twice the size for a frame (the
PC runner's local sim60, 2026-10-31). The screen opening as a band of lines (walking out of a
building: the window empty, each line's WINOUT showing all or nothing, the
band 32 lines taller every other frame) goes the same way
(`GpuRowsOf`, `gpuCircleOn` 2): the picture drawn whole, the backdrop above
and below the band drawn by the compositor, its edges halfway in the
in-between frames (16 lines a frame). Opening, the band's edges are
driven by the V-blank, not the game's steps (`band_ease`): right after the
map changes the game makes 19 to 26 steps a second, 2 or 3 V-blanks each
(a log of 2026-10-31: the upper 2D's drawer and the city's geometry on the
same core), and a band eased once a step jumped (16 lines on, then
nothing for a V-blank). Each edge goes on every V-blank at about the game's
speed (6 lines), trailing the game's edge by about 20 lines (a step and a
bit), a little faster when further behind, slower when nearer, never past
it; a step that comes late doesn't stop it. While it moves the upper
screen is drawn again each V-blank (`band_moving`: the scene shown and
nothing waiting for its V-blank, so it never shows before a frame still
to come). It starts so from a band that's narrow (less than half the
picture); a wider one, or one closing, is as the game has it. A frame
noted with no band, the window off as it ends, shows the picture whole
(drawn so already), the band's last lines eased out to the edges, and so
does a picture without the band while it's open still (`draw_band_tail`):
before, the 2D's drawer still on the band's picture, the band stood at its
last noted lines (40 to 200 of 240) for a sixth of a second, the outside
framed in black, then the frame gone at once. A band is noted from its first
frame: on the 3DS the frame's lines (`NitroPort_HBlankLinesOnce`) are
taken each V-blank before the frame's notes, not only when a drawer takes
the frame, and `NitroPort_Render2DCircleNow` finds a band on the canvas
engine A was last drawn on for the compositor, whether that picture had a
band or not (`sGpuBandCanvas`). Before, with the upper drawer busy every
other frame after a map change, the band's first frames were noted as
none, and its picture, when it came, was taken for a band that was over:
the outside showed whole at full brightness (the place name's sign in it)
for five frames, then the band from two thirds open (a video of
2026-10-31; the PC runner's sim60 with the notes taken the 3DS's way and
the lines every other frame shows the same). A band's picture with no band
drawn lately and none noted yet starts from the picture's own lines. A
band within a second of a circle drawn is the circle fade's end (going
into Eterna Forest: the circle closed, an empty band, 120-120 of 240), drawn
as the picture has it, all black, not eased as an opening band: eased, a
strip of the forest showed across the screen for the fade's last frame;
drawn as the circle last drawn instead, the circle stood still at its end
(a screenshot of 2026-10-31 and a log of 2026-11-01). Until 2026-10-31 neither was ever
left to the compositor with an H-blank effect: a line's copy of the
registers has DISPSTAT and VCOUNT at 0, the frame's the V-blank's, and the
lines were taken for differing in more than the windows (`LineLikeFrame`
leaves those two out); the circle into Eterna Forest stayed in the picture
at 30 a second, and walking out of a building showed the outside whole
for a moment, then the band at 30. Engine A's master brightness (the fades to white or black)
is halfway in them too, in 32nds; as noted only with a picture of that
frame or newer (`picture_of_frame`), the picture's own with an older one
(the 2D's drawer behind): walking out of a building, the outside showed
whole for a moment at full brightness before the opening band, the
picture from before the band with the band's frames' brightness. Strictly
(a picture of that frame or newer, no V-blank's slack) and only when the
picture has the frame's circle or band (a circle's or band's picture is
drawn whole, another frame's isn't): with a V-blank's slack, the picture
from just before the band got the band's halfway brightness, a dim flash
of the whole outside (a video of 2026-10-31).

The screen shown from one side (2026-11-04, leaving the gate between
Route 208 and Hearthome City: window 0 over columns 0 to 212, 170, 127,
85, 42 of every line, every other frame, nothing inside it and all outside
it into the city; all inside and nothing outside it, 42 to 212, onto the
route) is a band too (`GpuRowsOf`: each line's part inside the window and
outside it showing all or nothing, the same columns of every line from an
edge, the 3D on; `gpuCircleX` its columns, left | right << 16, 0 for all of
them, `GpuRowsColumns`), drawn by the compositor beside its columns (and
halfway in the in-between frames, the last step's too: `draw_band`,
`band_columns_halfway`). Its columns go across the canvas as they do
across the DS's 256 (400 / 256 a column): with the DS's columns moved to
the canvas as the 2D draws a window (`FillRange`), the wipe went 42
columns a step, then 114 at once as the window went off, and the place
name sign, at the canvas's left edge, came with them all at once (a video
of 2026-11-04). Before, it was drawn in the picture, whole each step: 30 a
second at most, and the frames noted without a band, so the city showed
whole at full brightness for a frame before the wipe (a picture from before
it with the wipe's first frame's brightness). The sign slides in under it
(`GpuSignOf`'s `bandShown`: with a band the compositor draws and that shows
the sign's BG, the window on, the picture drawn with none then).

In the compositor a band of columns is on every line, so its lines aren't
eased (`band_ease` is for a band of lines opening from the middle): the
frame before the wipe's first step is black (the master brightness) with
no band noted, or an empty one, and eased from there, each step's columns
would have opened from the middle of the screen up and down. Halfway, an
empty band or a black frame before it (`frame_black`) counts as none of
its columns, from the edge they're from: its first step halfway too. Its
last step is halfway with the picture already without the band
(`draw_columns_tail`, from the frames' notes alone, so both eyes alike) or
with it still; a frame noted with the band, the picture already without it
(ahead), gets the frame's columns there too, not the whole screen. A
picture with the band before the frame's note of it is drawn with its own
columns, not whole, and all black where the frame's noted black (the
picture's first step ahead of the black frame before it). A band with
columns, once drawn in a frame, is drawn the same for the other eye
(`sColsDrawn`): drawing it changes the band's state, which took the other
eye another way (a review of 2026-11-04). The sign goes to the compositor
under a band of columns only: under a building door's band of lines it
stays in the picture, as before. Simulated on the PC (sim60, the
compositor's rules copied, each in-between frame with the picture before
or the new one), the wipe goes 33 of the canvas's columns every frame each
way, 65 at once at its first step only when the in-between frame has the
picture from before the band (black there).

An in-between frame for the 2D's fades alone (`NitroPort_ComposeFrame`'s
mid 2, `fades_changed`): with no in-between scene (a map's first scenes
have none: walking out of a building, the band opened 30 times a second,
each step held two frames or more), the scene's own 3D is drawn a V-blank
before it with the brightness and the circle or band halfway from the
last scene shown's frame, when one of them changed.

With an H-blank effect the 2D's change check reads all of the engine's
VRAM (864 KB for engine A, about 15 ms on the 3D's core); not when the
lines differ in the windows alone, a circle or band the compositor draws
(`ShownVramSignature`'s linesOnlyWindows): the BGs are as the frame's
registers have them on every line. A log of 2026-10-31: for 2 seconds
after each walk out of a building the game made 24 scenes a second instead
of 30, the upper screen's drawer at 15 ms a frame through the band, the
game's steps held for it (some 3 frames, the band with them).

On the 3DS (`fps60`, on by default since PR #132): each mailbox slot has
an in-between scene of its own, made before the slot's scene is
published (`NitroPort_Gx3dMidOf`: as stable as the scene while it's
acquired; the newest one before, `sMid`, was rewritten under the reader).
The display thread draws a new scene's in-between scene first, shown a
V-blank before the scene (the scene's lag less one; `ComposeFrame`'s
`mid`: it doesn't count as the scene shown, and is prepared apart from
it), then the scene, both built while the GPU draws the frame before (see
Timing): a frame each V-blank, the game's 30 steps and 30 in between. The
in-between scene goes with the 2D the DS showed halfway, no newer than its
scene's own V-blank (`pick_set`'s `mid`: the scene before's, there
already), and doesn't wait for its scene's 2D as the scene does. Waiting
for it (until 2026-10-30), it went out with its scene or not at all while
the upper screen's 2D took long (a sign sliding in, a text box, the 2D
drawn again for nothing in the towns): an in-between frame left out for
each new 2D picture, 5-15 a second, and the 3D stepped at 30 by turns
(the people in Eterna seemed to shake as the player walked). A
frame then has 16.7 ms of the GPU, not 33. On a New 3DS (PR #132's build,
2026-10-20, the field): with the slider up (smooth, no outlines) 9-11 ms
a frame, 14-16.5 in the busiest places; with it down (smooth and
outlines) 14-15.5, 18-20 with a text box up, so slider down was mostly at
30. The 3D's smooth drawing was 3.3-5.8 ms an eye of it, the outlines
8-9 ms (the shifted passes 2.4-2.9, the outlines drawn 4.7-5.8, the
depth again 1.1).

60 frames a second come first (`auto_fps60`, since PR #135): while
frames don't fit (4 in a row over 16 ms) the upper screen's 3D is drawn
more cheaply, a step at a time: 2x2, then 2x1 (two samples down the
screen, one across: the framebuffer's lines, which the transfer engine
halves on their own; half the 2x2's work), then as it is; only after
that are the in-between scenes left out (30 a second, drawn 2x2 again: it
fits in 33 ms). A step back up after 60 frames in a row that would be
under 14.5 ms the next way up (the frame's time times what that step
costs, measured each time a step is taken), 120, 240 or 480 after each
step down, till they've stayed put for 10 s. The slider down and up each
have their own steps (the outlines; each eye). A `[compose]` line says
each step (`60 frames a second, slider down: the 3D drawn 2x1 for now`),
and `[perf] display` counts the in-between frames and those drawn 2x2 and
2x1. Matching the polygons costs the geometry core time per scene: in the
starter's choice (its many polygons) it was 21 ms a scene on the 3DS, so
some in-between scenes were left out there and the animation stuttered.
Since 2026-10-26 a polygon whose counterpart in the next scene is the
one in the same place in the list, and near enough to be certain, is
taken without the search round it; the search stops comparing a polygon
once it's further than the best so far; and the median that sets
"near enough" is found without sorting (half the cost there, on the PC). The outlines with the slider up are off by default for it
(`edges3d on`: while they fit).

The scene's motion (2026-10-27): the order's pairs give it only while the
order holds. A map's block coming into the view puts its tiles among the
rest, all of one kind, and every pair after it was a tile with its
neighbour: on Route 205's lawn at a run the in-between scene had its
tiles halfway to the wrong ones (holes, the stairs stretched). Now the
motion is the one most of a sample of 64 polygons fits (each with one of
its kind in the scene before within 2 DS pixels of where it puts it): the
order's or the scene before's (the camera's pace is steady; forgotten
after a cut, a change of view or the in-between scenes off), none only
without either (a floor moving a tile a scene, the bike's top gear, has
every tile where its neighbour was: none fitted it all). And
while the scene itself stays still, a polygon takes the order's pair when
that's no more than 3 DS pixels further than the nearest of its kind: the
starters' Poké Balls wobbling in the briefcase had their faces paired
with the next face or the next ball, and the in-between scenes showed
grey blocks. In `test_gx3d.c`: `test_interpolation_order_shift`,
`test_interpolation_tile_a_scene`, `test_interpolation_swaying`. The PC runner's `midview FILE` draws the
in-between scene at the view's size (as `viewsim`).

The same polygon (2026-11-06): as the balls jump out of the briefcase,
spinning, the camera closing in, each face of a ball was nearer another
face's place in the scene before than its own, and was paired with it
(or with the case's): the in-between scenes showed grey specks on the
balls' white and the case's handle torn (a video of 2026-11-06; on the PC
with the local `dbgstarter`, every in-between scene of the case opening
and the balls landing, the same). The order's pair is the same polygon of
the same model when its texture coordinates are the same at every corner
(`same_st`); another part of its kind mostly has its own (a ball's other
faces), while a map's repeated tiles share theirs and tell nothing. So
the order's pair is taken when it's the same polygon and the nearest of
its kind isn't, as far as twice the usual reach (`ORDER_SAME_FAR`); of two
polygons paired with one of the scene before's, the one that's the same
polygon as it keeps it when only one is (two copies of a model, by where
they are, as before); and the camera's move (`static_rematch`) doesn't
give such a polygon another that isn't the same too. On the PC the
briefcase's in-between scenes came out as clean as with the order's pairs
alone. Geometry recordings of the Cycling Road, Eterna at a run and Route
203 (`gx3d_replay --hashes --interp --view 400 240`): every scene and
in-between scene the same as before but the dozen as the follower's ball
bursts open (its sparks, many of one kind), as good as before; and in
Jubilife a man by the road, whose in-between pictures had his body dark
and muddled, now drawn as himself. `test_interpolation_fast_model`.

The camera's move (2026-11-03): riding the Cycling Road, the in-between
frames had white lines along the rocks' ledges (video IMG_2955). The
ground near the screen's bottom moves further than the scene's motion
(the perspective: 6 DS pixels a scene against 4), so a ledge's steps, a
row of one kind every two pixels, were paired a step off, sure as the
order's pairs are, and met their neighbours a pixel apart. But the
field's camera keeps its angle: everything that stays where it is in the
world moves by the same 4 numbers in clip space between two scenes, the
far and the near alike. So after the matching, the middle of the pairs'
moves in clip space (`clip_motion`); when at least half the pairs fit it
on every corner (on the Cycling Road they fit it to a clip unit or two:
the tolerance is w / 16384, 1/128 of a pixel), each polygon whose pair
doesn't fit it takes the one of its kind that does (`static_rematch`),
and one with none is put halfway as the camera moved, not as the screen
moved. What the camera follows (the player and his shadow: where its
pair was on the screen, as far from the camera) keeps its pair: the
player's shadow is 4 quads, each the size of the bike's move in a scene,
and the quad below fitted the move to the one above it exactly. The
camera still, or turning or zooming (the pairs don't fit one move), the
matching is as it was. On five recordings of the Cycling Road the pixels
left uncovered in the in-between scenes that both scenes either side
covered went from 30,451 to 1,348 (to none on three; what's left on the
other two was there the same before, at a map's edge), and 1-8% of the
in-between scenes differ at all. In
`test_gx3d.c`: `test_interpolation_camera_move`. (An earlier try that
welded shared corners and re-matched by rows, 56abffc, was undone with the
HM cut-in change of the same commit: log b1334782 had the geometry thread
held up 140-275 ms at a time while the cut-in's pictures were handed over
above its priority.)

## Known

- In the Azahar emulator, the field's 3D sometimes flashes black for a
  frame. Not seen on a real New 3DS, nor in the PC build. If it shows on a
  3DS, `edges off` in remaster.txt says whether it's the edge marking.

### Sun shadows: parked

Since PR #114 the sun shadows hang a 3DS's GPU on their first frame (the
screens freeze or go black, the sound goes on, and the HOME menu can't
draw either: only holding POWER gets out). PRs #116 to #119 narrowed it
down with `shadowdebug on`: the shadow map pass and the opaque scene come
back; the receiving pass doesn't, with the receivers' vertex program in
any form, over the scene or into a mask of its own, and even exactly as
the #113 build drew it (which worked on a 3DS then). So the cause is
something PR #114 or #115 changed elsewhere, not the pass itself.

Until it's found, `shadows on` leaves the sun shadows off on the 3DS (a
`[remaster] sun shadows are parked` line in the log) and the field's round
shadows stay; the colour grade works as set. The code stays:
`SUN_SHADOWS_PARKED 0` in `port_gx3d_pica.c` turns them back on.

The way back: a build of the #113 commit (3e8c953) on the 3DS first, to
see that it still draws them today; then #114's and #115's changes again
one at a time, each in a normal build that can be played.

`shadowdebug on` (with the shadows on again) still waits for the GPU
between the shadow passes, so the two-second watchdog names the pass the
GPU didn't come back from; it's slower, and only for that hunt.
