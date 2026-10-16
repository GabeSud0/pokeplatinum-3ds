# Wide art: drawing the wide screen by hand

The remaster's upper screen is wider than the DS's: 32 DS pixels more on
each side. By itself the port fills those bands with rules (an edge colour
going on, nothing; never a picture mirrored or stretched: see
[REMASTER.md](REMASTER.md)).
Where a rule doesn't look right, you can draw the bands yourself, screen by
screen, and the game shows your drawing instead.

## How to

1. **Capture the screen.** In the game (on the 3DS, or in Azahar), on the
   screen you want to draw, press **L + R + X**. The game saves a capture
   into `3ds/pokeplatinum/wide/captures/0001` (then `0002`, ...) on the SD
   card (Azahar: its `sdmc` folder). Capture as many screens as you like.
   (While L and R are held, A, X, Y and SELECT go to the port, not the game.)
2. **Open WideArt.** It's `WideArt.exe` (no installing). The first time,
   File > Open folder and pick the SD card, Azahar's `sdmc` folder, or
   `3ds\pokeplatinum\wide`; it remembers. The captures are listed newest
   first.
3. **Pick a layer and draw.** A screen is made of the game's layers: up to
   four BGs and the sprites (OBJs), the sprites in groups (those of one
   priority and palette: a scene's sprite background, like the strip
   behind the Pokemon in the opening, is usually one group, "Sprites,
   palette 6"). The list shows them front to back. Usually the one to draw
   is the background, the backmost. The yellow lines are the DS's
   picture's edges: the bands outside them are yours. To start, **Mirror**,
   **Extend edge**, **Repeat** or **Clear** fills the bands from the
   picture; then fix them by hand. The middle is locked so it can't be
   changed by accident; untick "Lock the DS's picture" to change it too.
4. **Save** (Ctrl+S). The picture goes into `3ds/pokeplatinum/wide`, named
   after the layer.
5. **In the game, L + R + A** takes the new pictures in (or restart it).
   From then on, whenever that layer shows, your bands show with it.

## The editor

| | |
|---|---|
| Brush (B), Eraser (E) | round, 1 to 32 pixels across (the slider, or **[** and **]**); the eraser makes pixels transparent (the layers behind show). **Shift + click**: a straight line from where the last stroke ended |
| Fill (G) | the area of one colour |
| Pick colour (I), right-click | the colour under the cursor |
| Select (M), Ctrl+A, Ctrl+D | drag a rectangle; then **drag inside it to cut and move those pixels** (hold Alt to move a copy, leaving them where they were). They float (an orange outline) until you put them down: Enter, a click outside, or another tool; Esc puts them back where they were. While floating: drag them again, the arrow keys move by 1 pixel (Shift: 8), H flips them, Delete throws them away |
| Ctrl+T, R, Shift+R, H, Shift+H | scale and rotate the selection (by % and degrees, as you watch; 90° turns, flips; sharp pixels unless "Smooth"); turn it 90° right / left; flip it left-right / upside down. It stays floating, to drag into place |
| Ctrl+X, Ctrl+C, Ctrl+V | cut, copy, paste the selection; the paste floats where the copy came from (or in the selection), ready to drag into place. Ctrl+Shift+V pastes onto a new drawing layer |
| Move (V) | drag the drawing layer (nothing selected), or the selection's pixels as Select does; the arrow keys move by 1 pixel, Shift+arrows by 8 |
| Colour picker (click the colour) | a hue bar and a saturation/brightness square, the colour by H S B, R G B or hex; "in the game" is the colour as the DS shows it |
| Recent, This screen's colours | the colours you used last; the colours the screen uses (drawing with them keeps the game's look) |
| Ctrl+Z; Ctrl+Y or Ctrl+Shift+Z | undo; redo |
| Alt + wheel (or Ctrl + wheel), +, - | zoom (1x to 12x), the pixel under the mouse staying under it |
| Middle button drag (with or without Alt) | drag the whole picture round the window; Home puts it back in the middle |
| P | the 3DS preview: the screen at the 3DS's size, as its filter shows it |
| Layers in front: see-through | the layers in front of the one you're drawing, faint, so you can see it |
| File > Open saved picture in another program | edit it in Aseprite, Photoshop...; then File > Reload saved picture |
| File > Import picture (Ctrl+I), Export layer (Ctrl+Shift+E); the buttons under Save | to work on a picture in another program (Photoshop...) and bring it back: Export writes the layer as a PNG (320 x 192; a layout's picture at its own size), or (File) the drawing layer alone or the whole screen; Import puts a PNG or a PSD (Photoshop's, as one picture) in as a new drawing layer: a 320 x 192 one where it is, a layout's picture's own size at its place, anything else at the view's top-left, the Move tool chosen to drag it into place; Merge (Ctrl+E) puts it into the layer under it |

**Drawing layers.** Each of the game's layers can have as many drawing
layers of your own as you like, as in a paint program (the list under the
game's layers, top first): **New** (Ctrl+Shift+N), **Duplicate** (Ctrl+J),
**Delete**, **Show/Hide**, **Up**/**Down** (Ctrl+] / Ctrl+[), **Merge** down
(Ctrl+E), **Rename** (F2, or double-click); Layer > Flatten all. Edit >
Paste as a new layer (Ctrl+Shift+V) puts the copy on a layer of its own.
Moving a drawing layer loses nothing that goes past the screen's edge: move
it back and it's there. Saving puts the visible drawing layers together
into the one picture the game reads, and keeps them as they are
(`wide/editor/<key>_<hofs>_<vofs>/`) for next time. (If the saved picture
was changed in another program since, the editor opens that instead.)

**Scrolls round all the time (a strip).** Tick it for a layer that keeps
scrolling (the opening's blue strip): its bands then move with it all the
time. Otherwise a layer's bands show only while it's where you drew it
(see below).

**A screen's own pictures.** The pictures a screen's layout draws itself
(the party's panel, its Poké Ball and lights, the boxes' divider: see
"Pictures a screen's layout draws" below) are in its captures too, as
layers "Picture: <name>", each just in front of the BG it's drawn with,
unturned (a ball captured mid-turn shows upright). Draw on them like any
layer (nothing is locked for them), move them anywhere (the Move tool) and
save: the editor writes the file (`wide/<name>.png`) as the box round
what's drawn, wherever it is, what's past the screen's edge too (the
party's ball, half off the screen). Moved from the layout's place, the
file also says how far (its PNG `oFFs` chunk): the game draws it there,
and turns it about the middle of what's drawn. (A picture saved straight
from another program loses that and goes back to its place: bring it in
with File > Import instead.) A light's colours are a picture each: a
capture has the ones that were lit then.

**Screens laid out in parts.** On the screens the port lays out anew (the
party, the boxes, the bag, the summary...: see "Pictures a screen's
layout draws" below), the game moves the layers itself. A picture of one
of those layers made right where it is in a capture (drawn on: the boxes'
panel's edges cleaned) is used as it is: the whole layer, or its bands
with the game's layout between. Captured where the layer is somewhere
else (a box's wallpaper mid-slide), the game's own layout shows.

Colours are saved as the DS shows them (5 bits a channel), and pixels are
either solid or transparent (no half-transparent ones).

The editor's own preview puts the layers on top of each other without the
game's blending effects (some screens blend two layers, the title screen
for one), nor the 3D (a capture doesn't have it); the game shows the real
thing.

## What the game does with it

A picture belongs to a layer by its **key**: a fingerprint of that layer's
art (a BG's map, the tiles it uses, their colours; a sprite group's
tiles and colours, not how many sprites show them or how they're cut), not of where the game put it in memory or how
it's scrolled. So:

- it shows whenever that layer shows, on any visit to that screen, whatever
  the other layers do (text printing, a cursor moving);
- the picture shows **where you put it**, and moves with the layer when
  the layer scrolls (a BG) or moves (a sprite group):
  - **the side bands only** (`<key>_<h>_<v>.png`): they show while the
    layer is across from where it was when you captured it (give or take
    8 pixels sideways; up and down doesn't matter, so a layer that bobs
    keeps its bands), else nothing beyond the DS's edges;
  - **the whole layer** (`_full`): away from its place the whole layer is
    the picture, moved with it, so what slides in (the opening's Pokemon)
    is whole, never cut at the DS's edge. The slide is made as wide as the
    screen: in the last 32 pixels before something is whole on the DS's
    screen it moves twice as far, so it comes in from the wide screen's
    edge, and what waits beyond the DS's edge to come in waits beyond the
    wide screen's too (no Pokemon's hand showing before it comes in).
    Past the picture's own edges the layer's map goes on. If the layer
    has a picture made further along, the one whose own edge wouldn't come
    into view is used (the opening's Torterra, cut by the wide screen's
    edge in both of his);
  - **scrolls round all the time** (`_loop`, the editor's "Scrolls round
    all the time"): its bands move with it everywhere, and past the
    picture's edge they go round as the layer's map does (the opening's
    blue strip);
- one layer can have **several pictures**, one for each place it was
  captured at: the opening's sky and its blue strip are the same layer (one
  tall map, the sky above, the strip below), each with its own picture.
  The game uses the one made nearest where the layer is, if that's less
  than a screen away; capturing the layer a few pixels from a picture's
  place opens that picture, lined up, and saving it replaces it;
- if the layer's own art changes (text printed into it, a palette fading,
  a sprite animating), the key changes and the port's own rules come back
  until it's the same art again. Text boxes and menus are usually on layers
  of their own, so the background layer keeps its key.

A sprite group's picture goes where the group has no sprite, at the
group's priority (so the layers in front of the sprites stay in front).

**Placing a picture.** The port's own rules only carry a plain edge
colour on, and on the screens made of pictures (the opening, the title)
only where the DS's 32 outermost pixels of a line are one colour: nothing
of a picture is drawn out. Where a picture reaches the DS's edge, draw the layer as it
should be: move it (Move tool) so that what the DS cut at its edge is cut
at the wide screen's edge, and draw the strips on to the edges. Saved with
the middle changed, the whole layer is yours, at the place you put it.
(A capture's `bg<N>_map.png` is the layer with its map going on past the
DS's edges, where the map has more: a start for the bands.)

**A layer that stops at another place.** A layer that stops somewhere else
for a while (the opening's Pokemon, 60 pixels further right before the
lightning) needs a picture there too. Capture it there, then

    python tools/wideart/fill_places.py WIDE_FOLDER CAPTURE --write

makes it from the layer's whole-layer pictures at its other places: moved
by as much as the layer moved (nothing stretched or mirrored), and only if
it lines up exactly with what the DS shows there. (Without CAPTURE: all
the captures; without `--write` it only lists them.) Don't capture where
a layer waits beside the DS's screen before it comes in: there it should
show nothing.

Files, in `3ds/pokeplatinum/wide`:

- `<key>_<hofs>_<vofs>.png`: the bands only (the middle stays the game's
  own: anything the game animates there keeps moving); for a sprite group
  hofs and vofs are where the group was (OAM's x and y of one of its
  sprites);
- `<key>_<hofs>_<vofs>_full.png`: the whole layer (the editor saves this
  one when you change the middle);
- `..._loop.png` (either kind): its bands go round with the layer;
- `captures/NNNN/`: `screen.png`, `bg0.png`-`bg3.png` (and
  `bg<N>_map.png`, its map past the DS's edges), `obj<G>.png` (each
  sprite group, G = priority x 32 + palette, or + 16 for 8-bit sprites),
  `picture_<name>.png` (each picture the screen's layout draws, unturned,
  where it's drawn), `info.txt` (each layer's key, scroll or position, and
  priority; each picture's BG, where what's drawn in it is (x, y), its
  size, turn and how far it is from the layout's place (dx, dy));
- `<name>.png`: a picture a screen's layout draws (the table below);
- `editor/<key>_<hofs>_<vofs>/`: the editor's drawing layers for a picture
  (`layers.json` and a PNG each; `editor/<key>/` before; a layout's
  picture's: `editor/<name>_<x>_<y>/`). The game doesn't read it.

320 x 192 pixels, the DS's picture at x 32-287. The pictures are made from
the game's art: like the data pack, they stay on your SD card and aren't
shared or committed.

### Pictures a screen's layout draws

Some screens laid out for the wider screen in code (see
[REMASTER.md](REMASTER.md), "Screens laid out in parts") also draw
pictures of their own, by name, from the same folder. Any size up to
320 x 192; transparent where alpha < 128. Without a file, that part isn't
drawn and the screen looks as it did. The game places them, turns them
and swaps them; only what's drawn in a file counts (a clear margin is left
out). Redraw them in the editor, or in any paint program and bring them
in with File > Import (a file's top-left goes at the place below unless
the editor moved it):

| File | Where | What the game does with it |
| --- | --- | --- |
| `pc_boxes_divider.png` (first drawn 40 x 192) | the boxes: between the Pokémon's panel and the box, behind the panel's edge (its place: x 88, y 0) | |
| `pc_boxes_ball.png` (23 x 23) | over the divider's round socket (x 102, y 75) | turns once round as the boxes slide |
| `party_panel.png` (64 x 158) | the party: at the left edge (x 0, y 0) | |
| `party_ball.png` (70 x 70) | the panel's socket (x -20, y 46: its middle at x 15, y 81) | turns a little as the cursor moves |
| `party_light_off`, `_red`, `_blue`, `_green`, `_amber`, `_white` `.png` (5 x 5) | the panel's two rows of lights (x 49, y 9, 15, 21 and 131, 137, 143) | each light changes colour now and then |

(Places on the 320 x 192 screen, where the file's top-left goes.)

A turned picture turns about the middle of what's drawn in it: draw a
round one with nothing else in the file (an odd size has a middle pixel).

## The code

- `3ds_port/nitro/src/port_wideart.c`: the folder, reading the pictures
  (stb_image, `3ds_port/nitro/third_party`), captures; the layouts'
  pictures (`NitroPort_WideArtPicture`; their names:
  `NitroPort_WidePictureName`, `port_wide.c`), drawn by `PicturesOnLine`
  in `port_render2d.c`.
- `3ds_port/nitro/src/port_render2d.c`: a BG's key (`BgKey`), the picture
  in its bands (`LookUpArt`, `SEG_ART`), one layer alone
  (`NitroPort_Render2DLayer`); the sprite groups (`ObjGroups`, keyed by
  the tiles they use), their pictures in the OBJ line (`LookUpObjArt`, `DrawObjs`),
  one group alone (`NitroPort_Render2DObjLayer`).
- `3ds_port/game/app/main_3ds.c`: L + R + X, L + R + A; the PC runner:
  `--wide-art DIR`, control commands `capture DIR`, `wideart DIR`, `oam`.
- `tools/wideart/`: the editor (Python, Tk, Pillow); CI builds
  `WideArt.exe` with PyInstaller (the `wideart-editor` job's artifact).
  `python tools/wideart/wideart.py` runs it from the source.
