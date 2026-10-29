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
frames a second steps. The colour effects and OAM are the frame's (no
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
  - the battle's trainers slide off (the player after throwing the ball,
    the opponent) till they're past the wider screen's edges, not the
    DS's (they vanished in the sides); since 2026-10-26 (patch 0043) till
    all of the sprite is (it reaches 60 pixels on from its position: the
    player went with 20 pixels of it still in the left band); past the
    DS's -40 the player's Poké Ball is left to its arc as on the DS (patch
    0052: the trainer's hand took it off the screen for the 4 frames it
    hangs at the top, and it vanished);
  - the HM cut-in: its band of Poké Balls (BG3, a 256-pixel map) repeats
    to the screen's edges (a SHOW part) instead of a stripe of its plain
    lines; the Pokémon waits beyond the wider screen's right edge and
    slides off past its left one (it sat still in the right side);
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
    ApplyMovement commands before the WaitMovement);
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
- **The game** reads `NitroPort_WideScreenExtra` as 72 there: the people the
  scripts bring on from the sides come from beyond the 400 pixels.
- **The game's windows** go to the picture's top or bottom
  (`DecideTallWindows`): those wholly in the DS's lower half 24 lines down
  (a text box at the screen's bottom, YES/NO with it), those wholly in its
  upper half 24 up (the place name sign at the top), the OBJs over them
  with them; the lines they leave are drawn from nowhere.
- **The people of the maps beside** (patch 0050, also with the 320-wide
  field): the DS makes a map's people as the player walks into it and drops
  those of the map left, all out of its screen; on the wider one they
  popped in and out (the berry trees and people at Floaroma's edge). Now
  the people of the maps beside the player's are there ahead
  (`MAP_OBJ_STATUS_PORT_NEIGHBOUR`): `FieldMap_PortNeighbours` brings those
  within 17 tiles left or right, 13 up, 11 down (one a frame, read from
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
  first) while the new map's pictures wouldn't fit.
  - The player's counterpart (patch 0055): her graphics are a variable's
    (`OBJ_EVENT_GFX_VAR_0`), set by her map's script as the player walks
    in, so she was never brought and popped in at the zone change (Dawn
    outside the lab in Sandgem Town, 4 tiles away). On the overworld only
    she has one, each map setting Dawn for a boy and Lucas for a girl, so a
    neighbour's `VAR_0` is that; where the scripts move or hide her in some
    of the story's states she's placed the same way
    (`sPortCounterpartPlaces`: Sandgem, Route 202, Jubilife, Veilstone,
    Canalave). The other variables' (only indoors) aren't brought.
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
  camera only).
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
- Still to do: the people scripts bring on from above or below (they show
  the last few tiles of their walk in); weather sprites (rain, snow) only
  on the DS's lines.

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
  screen.
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
    shoes).
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

A frame that took long is in the log with where its time went:
`[stall] frame N: X ms in the game (file reads: n, KB, ms; waiting for the
geometry thread: ms)`, or `X ms in the port between frames (...)`.
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
by where they are. Each paired polygon goes halfway between its two
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

On the 3DS (`fps60`, on by default since PR #132): each mailbox slot has
an in-between scene of its own, made before the slot's scene is
published (`NitroPort_Gx3dMidOf`: as stable as the scene while it's
acquired; the newest one before, `sMid`, was rewritten under the reader).
The display thread draws a new scene's in-between scene first, shown a
V-blank before the scene (the scene's lag less one; `ComposeFrame`'s
`mid`: it doesn't count as the scene shown, and is prepared apart from
it), then the scene, both built while the GPU draws the frame before (see
Timing): a frame each V-blank, the game's 30 steps and 30 in between. A
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
