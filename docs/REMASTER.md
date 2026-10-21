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
- **The look: sun shadows and a colour grade** (`shadows on`, `grade on`;
  L + R + B turns both on or off while playing). **The sun shadows are
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

With the 3D slider up there were no outlines for a while (PR #128;
`edges3d off` still leaves them out; on by default again since PR #130,
drawn for each eye after the smooth 3D, as with the slider down, below).
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

With outlines (the slider down; up too since PR #130, each eye in turn,
the 2x2 framebuffer cleared between them) the frame is split. The picture
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
otherwise, as `docs/3D_STAGE2.md` planned. A frame draws the opaque
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

The vertex program (`gx3d_passthrough.v.pica`) does the view (where the
DS's screen goes on the 3DS's turned framebuffer) and the stereoscopic
shift: `x += strength * max(w - convergence, -w)`, for perspective
vertices (w > 1) only.

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
  the intro's city).
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
- **Text boxes span the screen.** A text box across the DS's screen has
  its frame's left part at the screen's left edge and its right part at
  the right edge, each line of the frame's inner columns drawn on between
  them; the text starts a pixel inside the frame's left part (2026-10-16,
  from a drawing of the PC's text box: the room the wider screen gives is
  used, the text at the left as on the DS). Its lines stay as the game
  broke them.
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
  draws nothing again.
- **The battle's health boxes** go to the screen's edges, the player's
  right and the opponent's left (`patches/pokeplatinum/0013`, by
  `NitroPort_WideScreenExtra`).
- **OBJs** (sprites) placed past the DS's edges show.
- **The 3D** is drawn wide (the camera widened) only while it's on the
  upper screen; the intro's flyover is on the lower screen, at the DS's
  width.

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
screen's seven, the lower's three, since PR #138), and queues it with the
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
| `edges off` | no outlines (edge marking) |
| `edges drawn` | the outlines' left and right moved passes drawn, as before they were copied (for comparing: the same picture, slower) |
| `edges pruned` | each outline pass draws only the polygons it needs (worked out on the CPU each scene; the same picture). Slower on a 3DS (see below): off by default |
| `edges3d on` | the outlines with the 3D slider up too, while they fit in a frame (default off since 60 frames a second: drawn for each eye they cost 7-16 ms of the GPU's an eye; see How it works) |
| `fps60 off` | the field at the game's 30 frames a second (default on: an in-between scene shown between each two, while frames fit; see 60 frames a second) |
| `smooth3d off` / `smooth3d stereo` | the upper screen drawn at its own resolution / so only with the 3D slider down (default on: twice it and scaled down, no jagged edges, the outlines kept with the slider down; see How it works) |
| `filter3d sharp` / `filter3d smooth` | the 3D's textures each texel 2x2 then bilinear / bilinear (default `nearest`: the DS's; the others cost the GPU two to three times as much) |
| `shadows on` | sun shadows on the 3D (and no round shadow under objects), the sun by the time of day. L + R + B: the look on or off. **Parked on the 3DS** (they hang its GPU; see below): off there until fixed |
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
new scene has it. Too few pairs (under half: a cut, a new map) and
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
2x1. The outlines with the slider up are off by default for it
(`edges3d on`: while they fit).

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
