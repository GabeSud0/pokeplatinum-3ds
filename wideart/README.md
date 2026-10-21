# Wide art built into the port

The pictures in this folder are built into the program (`tools/embed_wideart.py`,
at the build) and shown on the remaster's wider upper screen, so that a player
needs no `wide` folder of their own. See [docs/WIDE_ART.md](../docs/WIDE_ART.md),
"Sharing your pictures".

They hold none of the game's art. Where a pixel of a picture was the game's
own (the DS's part of a screen, or a band the game's map goes on into), it's
a marker (alpha 64), and the port draws the player's own game's pixel there;
one that was the game's own moved along (the opening's Pokémon) says how far
(red and blue, green), and the game's pixel from that far back is drawn.
Only the hand-drawn pixels are in the files. The PC runner writes them this
way into the `share` folder beside your `wide` folder as you play the
screens they're for (`tools/wideart/share_from_capture.py` from a capture);
copy them here.

`<name>.png` (the pictures a screen's layout draws: the party's panel and
Poké Ball, the boxes' divider...) are hand-drawn whole and go in as they are,
with their `oFFs` chunk: where the editor moved one from its layout's place
(the boxes' divider, 3 left and 4 down). A copy re-saved without it is drawn
at the layout's place instead, offset (2026-10-21). Other chunks (an image
editor's metadata) can go.
