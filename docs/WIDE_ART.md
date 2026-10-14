# Wide art: drawing the wide screen by hand

The remaster's upper screen is wider than the DS's: 32 DS pixels more on
each side. By itself the port fills those bands with rules (an edge colour
going on, a picture mirrored, nothing; see [REMASTER.md](REMASTER.md)).
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
3. **Pick a layer and draw.** A screen is made of up to four layers (the
   DS's BGs); the list shows them front to back. Usually the one to draw is
   the background, the backmost. The yellow lines are the DS's picture's
   edges: the bands outside them are yours. To start, **Mirror**, **Extend
   edge**, **Repeat** or **Clear** fills the bands from the picture; then
   fix them by hand. The middle is locked so it can't be changed by
   accident; untick "Lock the DS's picture" to change it too.
4. **Save** (Ctrl+S). The picture goes into `3ds/pokeplatinum/wide`, named
   after the layer.
5. **In the game, L + R + A** takes the new pictures in (or restart it).
   From then on, whenever that layer shows, your bands show with it.

## The editor

| | |
|---|---|
| Pencil (B), Eraser (E) | one pixel; the eraser makes it transparent (the layers behind show) |
| Fill (G) | the area of one colour |
| Pick colour (I), right-click | the colour under the cursor |
| Select (M), Ctrl+C, Ctrl+V | copy a rectangle; the pasted copy follows the mouse until you click; H flips it |
| Delete | clears the selection |
| Ctrl+Z, Ctrl+Y | undo, redo |
| +, -, Ctrl+wheel | zoom |
| P | the 3DS preview: the screen at the 3DS's size, as its filter shows it |
| This screen's colours | the colours the screen uses: drawing with them keeps the game's look |
| Layers in front: see-through | the layers in front of the one you're drawing, faint, so you can see it |
| File > Open saved picture in another program | edit it in Aseprite, Photoshop...; then File > Reload saved picture |
| File > Export / Import layer | the layer as a 320 x 192 PNG, to draw elsewhere |

Colours are saved as the DS shows them (5 bits a channel), and pixels are
either solid or transparent (no half-transparent ones).

The editor's own preview puts the layers on top of each other without the
game's blending effects (some screens blend two layers, the title screen
for one); the game shows the real thing.

## What the game does with it

A picture belongs to a layer by its **key**: a fingerprint of that layer's
art (its map, the tiles it uses, their colours), not of where the game put
it in memory or how it's scrolled. So:

- it shows whenever that layer shows, on any visit to that screen, whatever
  the other layers do (text printing, a cursor moving);
- if the layer scrolls, the picture moves with it (by the scroll since the
  capture); past the picture's edge, the bands are empty;
- if the layer's own art changes (text printed into it, a palette fading),
  the key changes and the port's own rules come back until it's the same
  art again. Text boxes and menus are usually on layers of their own, so
  the background layer keeps its key.

Files, in `3ds/pokeplatinum/wide`:

- `<key>_<hofs>_<vofs>.png`: the bands only (the middle stays the game's
  own: anything the game animates there keeps moving);
- `<key>_<hofs>_<vofs>_full.png`: the whole layer (the editor saves this
  one when you change the middle);
- `captures/NNNN/`: `screen.png`, `bg0.png`-`bg3.png`, `info.txt` (each
  layer's key, scroll and priority).

320 x 192 pixels, the DS's picture at x 32-287. The pictures are made from
the game's art: like the data pack, they stay on your SD card and aren't
shared or committed.

## The code

- `3ds_port/nitro/src/port_wideart.c`: the folder, reading the pictures
  (stb_image, `3ds_port/nitro/third_party`), captures.
- `3ds_port/nitro/src/port_render2d.c`: a layer's key (`BgKey`), the picture
  in its bands (`LookUpArt`, `SEG_ART`), one layer alone
  (`NitroPort_Render2DLayer`).
- `3ds_port/game/app/main_3ds.c`: L + R + X, L + R + A; the PC runner:
  `--wide-art DIR`, control commands `capture DIR`, `wideart DIR`.
- `tools/wideart/`: the editor (Python, Tk, Pillow); CI builds
  `WideArt.exe` with PyInstaller (the `wideart-editor` job's artifact).
  `python tools/wideart/wideart.py` runs it from the source.
