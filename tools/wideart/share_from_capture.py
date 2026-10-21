#!/usr/bin/env python3
"""A wide art picture shared from a capture of its screen, as the PC runner
does it on the screen itself (NitroPort_Render2DShareArt, docs/WIDE_ART.md
"Sharing your pictures"): for a screen the runner can't get to.

    python tools/wideart/share_from_capture.py PICTURE CAPTURE [OUT]

PICTURE: the picture (wide/<key>_<hofs>_<vofs>[_full|_loop].png). CAPTURE: a
capture folder (wide/captures/NNNN) of the screen it's for, made where the
picture was made (the same scroll); its info.txt names the layer of the
picture's key: a BG's bg<N>.png (as the port drew it, its bands included)
and bg<N>_map.png (its map going on past the DS's edges), or a sprite
group's obj<G>.png. OUT: the file written (wide/share/<picture's name> if
left out).

The picture's pixels the same as the layer's where they are are made the
game's own (alpha 64; red 0, green 128, blue 1); those the same as the
layer's some way off, by the move most of the picture's pixels at edges
agree on (three rounds), the game's own moved that far (red and blue the
move + 256, low byte then high; green 128 + the move down); the rest, yours,
kept. A sprite group's picture: its sprites' own pixels where they are left
out (the sprites show themselves), moved sideways only. A picture of the
bands only: nothing in the DS's part.
"""
import os
import re
import sys

from PIL import Image

W, H = 320, 192
DS_X0, DS_X1 = 32, 288
MX, MY = 256, 16          # a move: sideways up to 255, up or down 16
SAMPLES, VOTES_LEAST = 512, 16


def quantized(p):
    return (p[0] >> 3, p[1] >> 3, p[2] >> 3)


def share(picture, references, sideways, bands_only):
    pic = Image.open(picture).convert('RGBA').load()
    refs = [Image.open(r).convert('RGBA').load() for r in references]
    ref = {}
    for y in range(H):
        for x in range(W):
            for r in refs:
                if r[x, y][3] >= 128:
                    ref[(x, y)] = quantized(r[x, y])
                    break
    orig = [[pic[x, y] if pic[x, y][3] >= 128 else None for x in range(W)] for y in range(H)]
    state = [['mine' if orig[y][x] else None for x in range(W)] for y in range(H)]

    def edge(x, y):
        c = orig[y][x]
        return c is not None and ((x > 0 and orig[y][x - 1] != c) or (y > 0 and orig[y - 1][x] != c))

    def mark(dx, dy):
        n = 0
        for y in range(H):
            for x in range(W):
                if state[y][x] == 'mine' and ref.get((x - dx, y - dy)) == quantized(orig[y][x]):
                    state[y][x] = (dx, dy)
                    n += 1
        return n

    places = {}
    for (x, y), c in ref.items():
        places.setdefault(c, []).append((x, y))

    def best_move():
        samples = [(x, y) for y in range(H) for x in range(W) if state[y][x] == 'mine' and edge(x, y)]
        samples = samples[::len(samples) // SAMPLES + 1]
        votes = {}
        for (x, y) in samples:
            for (rx, ry) in places.get(quantized(orig[y][x]), []):
                ox, oy = x - rx, y - ry
                if -MX < ox < MX and (oy == 0 if sideways else -MY <= oy < MY):
                    votes[(ox, oy)] = votes.get((ox, oy), 0) + 1
        best, bdx, bdy = 0, 0, 0
        for (ox, oy), v in votes.items():
            if (ox, oy) != (0, 0) and (v > best or (v == best and abs(ox) + abs(oy) < abs(bdx) + abs(bdy))):
                best, bdx, bdy = v, ox, oy
        return (bdx, bdy) if best >= VOTES_LEAST and best * 4 >= len(samples) else None

    if not sideways:
        mark(0, 0)
    for _ in range(3):
        move = best_move()
        if move is None or mark(*move) == 0:
            break
    if sideways:
        mark(0, 0)   # (the sprites' own, where they are: nothing to add)
    out = Image.new('RGBA', (W, H), (0, 0, 0, 0))
    op = out.load()
    game = moved = mine = 0
    for y in range(H):
        for x in range(W):
            s = state[y][x]
            if s is None or (bands_only and DS_X0 <= x < DS_X1) or (sideways and s == (0, 0)):
                continue
            if s == 'mine':
                c = orig[y][x]
                op[x, y] = (c[0], c[1], c[2], 255)
                mine += 1
            else:
                dx, dy = s
                op[x, y] = ((dx + 256) & 255, 128 + dy, (dx + 256) >> 8, 64)
                game += 1
                moved += (dx, dy) != (0, 0)
    return out, game, moved, mine


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 2
    picture, capture = argv[1], argv[2]
    name = os.path.basename(picture)
    m = re.match(r'^([0-9a-f]{16})_(-?\d+)_(-?\d+)((?:_full)?(?:_loop)?)\.png$', name)
    if m is None:
        print(f"{name}: not a picture of a layer (<key>_<hofs>_<vofs>[_full|_loop].png)")
        return 1
    key, full = m.group(1), '_full' in m.group(4)
    out = argv[3] if len(argv) > 3 else os.path.join(os.path.dirname(picture), 'share', name)
    references, sideways = [], False
    with open(os.path.join(capture, 'info.txt')) as f:
        for line in f:
            words = line.split()
            if len(words) > 5 and words[0] == 'bg' and words[5] == key:
                references = [os.path.join(capture, f'bg{words[1]}.png'), os.path.join(capture, f'bg{words[1]}_map.png')]
            elif len(words) > 5 and words[0] == 'obj' and words[5] == key:
                references, sideways = [os.path.join(capture, f'obj{words[1]}.png')], True
    references = [r for r in references if os.path.exists(r)]
    if not references:
        print(f"{capture}: no layer of key {key} in its info.txt")
        return 1
    image, game, moved, mine = share(picture, references, sideways, not full)
    os.makedirs(os.path.dirname(out) or '.', exist_ok=True)
    image.save(out, optimize=True)
    print(f"{out}: {game} of its pixels the game's own (left out; {moved} of them moved), {mine} yours")
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
