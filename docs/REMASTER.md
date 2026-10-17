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
  mountains (October 2026). `filter3d xbrz` makes them 2x with xBRZ
  instead: edges and curves smooth, small details kept, at 16 bits a texel
  where the DS's format allows and nearest (half sharp's memory and GPU
  reads, no lines), each texture worked out a little each frame (2 ms) and
  shown 2x2 until it's done; one that keeps changing (an animation, a
  fade) waits until it's been still for a second. Faint patterns (grass
  tufts a shade off the grass) are left as they are: xBRZ smooths them
  too, into strokes half a texel wide that mostly fall between the 3DS's
  pixels, and the grass came out flat.
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

Each 3D scene carries the V-blank count when the game finished it (its
SWAP), and each published 2D picture the count when the drawers read the
game's display state. A frame pairs the 3D with the newest 2D no newer
than the scene + 1 (the DS shows a scene the frame after its 2D was set),
and is shown `lag` V-blanks after the scene was made: the 3D comes at the
game's own even pace (every second V-blank in the field), not whenever a
scene happens to be done. `lag` starts at 2 and adapts (1 to 4): up when a
frame would be late, back down after 600 frames in a row with a V-blank to
spare, and twice as long before the next try each time a lowered lag
doesn't hold. (Counting the frames with room since the last late one, it
went down every 20-26 seconds in the field, a frame came late, and it went
back up: a hitch every 26 seconds.)

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
N% in stereo; 3D textures converted N (X ms each); 2D published N/s, X ms
each; 3D shown N V-blanks after the game made it, N late; GPU command list
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
