"""The WideArt editor's workings, apart from its window (wideart.py): the
captures the game makes, the pictures it reads, and what the editor does
to a layer. See docs/WIDE_ART.md.

A capture (the game: L + R + X) is a folder wide/captures/NNNN with
screen.png (the whole upper screen, 320 x 192), bg<N>.png (each BG alone,
transparent where it has no pixels) and info.txt (each BG's key, scroll
and priority). A picture the game reads is wide/<key>_<hofs>_<vofs>.png
(the side bands only) or ..._full.png (the whole layer).
"""

from __future__ import annotations

import os
from dataclasses import dataclass, field
from pathlib import Path

from PIL import Image

W, H = 320, 192
DS_X0, DS_X1 = 32, 288          # the DS's picture on the canvas: x 32-287
BANDS = ((0, DS_X0), (DS_X1, W))


def load_rgba(path: str | os.PathLike) -> Image.Image:
    """A picture, its file closed again (on Windows an open file can't be
    replaced or deleted)."""
    with Image.open(path) as im:
        return im.convert("RGBA")


# --------------------------------------------------------------- folders

def find_art_dir(folder: str | os.PathLike) -> Path | None:
    """The art folder ("wide") from whatever the person picked: the SD
    card, Azahar's sdmc folder, 3ds/pokeplatinum, the wide folder itself,
    or a capture folder in it."""
    p = Path(folder)
    candidates = [p, p / "wide", p / "3ds" / "pokeplatinum" / "wide",
                  p / "sdmc" / "3ds" / "pokeplatinum" / "wide",
                  p.parent.parent if p.parent.name == "captures" else p]
    for c in candidates:
        if (c / "captures").is_dir():
            return c
    if p.name == "captures" and p.is_dir():
        return p.parent
    return None


def default_folders() -> list[Path]:
    """Where to look first: Azahar's SD folder on Windows, then removable
    drives' 3ds folders."""
    out = []
    appdata = os.environ.get("APPDATA")
    if appdata:
        out.append(Path(appdata) / "Azahar" / "sdmc")
        out.append(Path(appdata) / "Citra" / "sdmc")
    if os.name == "nt":
        for letter in "DEFGHIJKLMNOPQRSTUVWXYZ":
            out.append(Path(f"{letter}:\\"))
    return out


# --------------------------------------------------------------- captures

@dataclass
class Layer:
    bg: int
    prio: int
    key: str
    hofs: int
    vofs: int
    file: str

    def art_name(self, full: bool) -> str:
        return f"{self.key}_{self.hofs}_{self.vofs}{'_full' if full else ''}.png"


@dataclass
class Capture:
    path: Path
    layers: list[Layer] = field(default_factory=list)
    backdrop: tuple[int, int, int] = (0, 0, 0)
    engine: str = "A"
    has3d: bool = False

    @property
    def name(self) -> str:
        return self.path.name

    def screen(self) -> Image.Image:
        return load_rgba(self.path / "screen.png")

    def layer_image(self, layer: Layer) -> Image.Image:
        return load_rgba(self.path / layer.file)

    def layer(self, bg: int) -> Layer | None:
        for layer in self.layers:
            if layer.bg == bg:
                return layer
        return None


def rgb555(value: int) -> tuple[int, int, int]:
    r, g, b = value & 31, (value >> 5) & 31, (value >> 10) & 31
    return ((r << 3) | (r >> 2), (g << 3) | (g >> 2), (b << 3) | (b >> 2))


def read_capture(path: str | os.PathLike) -> Capture:
    """A capture folder's info.txt."""
    path = Path(path)
    cap = Capture(path)
    for line in (path / "info.txt").read_text(encoding="utf-8").splitlines():
        words = line.split()
        if not words or words[0].startswith("#"):
            continue
        if words[0] == "bg" and len(words) >= 12:
            f = dict(zip(words[2::2], words[3::2]))
            cap.layers.append(Layer(bg=int(words[1]), prio=int(f["prio"]), key=f["key"].lower(),
                                    hofs=int(f["hofs"]), vofs=int(f["vofs"]), file=f["file"]))
        elif words[0] == "backdrop" and len(words) == 2:
            cap.backdrop = rgb555(int(words[1], 16))
        elif words[0] == "engine" and len(words) == 2:
            cap.engine = words[1]
        elif words[0] == "has3d" and len(words) == 2:
            cap.has3d = words[1] == "1"
    return cap


def list_captures(art_dir: str | os.PathLike) -> list[Capture]:
    """The art folder's captures, newest first."""
    root = Path(art_dir) / "captures"
    caps = []
    if root.is_dir():
        for p in sorted(root.iterdir(), reverse=True):
            if (p / "info.txt").is_file() and (p / "screen.png").is_file():
                try:
                    caps.append(read_capture(p))
                except (OSError, ValueError, KeyError):
                    pass
    return caps


def existing_art(art_dir: str | os.PathLike, key: str) -> Path | None:
    """The picture already made for a layer (the whole one if both)."""
    found = sorted(Path(art_dir).glob(f"{key}_*_*.png"))
    full = [p for p in found if p.stem.endswith("_full")]
    return (full or found or [None])[0]


def draw_order(layers: list[Layer]) -> list[Layer]:
    """Back to front: priority 3 first; of one priority, the higher BG
    number is further back (the DS's order)."""
    return sorted(layers, key=lambda l: (-l.prio, -l.bg))


# ---------------------------------------------------------------- editing

def snap(color: tuple[int, ...]) -> tuple[int, int, int, int]:
    """A colour as the DS (and the game) can show it: 5 bits a channel."""
    r, g, b = color[:3]
    a = color[3] if len(color) > 3 else 255
    q = lambda v: ((v >> 3) << 3) | (v >> 5)
    return (q(r), q(g), q(b), 255 if a >= 128 else 0)


def fill_bands(img: Image.Image, how: str, side: str = "both") -> Image.Image:
    """The side bands worked out from the DS's picture, as a start:
    "mirror" (folded over at the edge), "extend" (the edge column
    repeated), "repeat" (the picture's other side, as if it wrapped),
    "clear" (transparent). side: "both", "left" or "right"."""
    out = img.copy()
    src = img.load()
    dst = out.load()
    for y in range(H):
        for x in range(W):
            left = x < DS_X0
            right = x >= DS_X1
            if not (left or right) or (side == "left" and not left) or (side == "right" and not right):
                continue
            if how == "mirror":
                sx = 2 * DS_X0 - 1 - x if left else 2 * DS_X1 - 1 - x
            elif how == "extend":
                sx = DS_X0 if left else DS_X1 - 1
            elif how == "repeat":
                sx = x + 256 if left else x - 256
            elif how == "clear":
                dst[x, y] = (0, 0, 0, 0)
                continue
            else:
                raise ValueError(how)
            dst[x, y] = src[sx, y]
    return out


def flood_fill(img: Image.Image, x: int, y: int, color: tuple[int, int, int, int],
               allowed=None) -> Image.Image:
    """The area of x, y's colour (4-connected) in color. allowed(x, y):
    whether a pixel may change (the middle locked)."""
    out = img.copy()
    px = out.load()
    target = px[x, y]
    if target == color:
        return out
    stack = [(x, y)]
    seen = set()
    while stack:
        cx, cy = stack.pop()
        if (cx, cy) in seen or not (0 <= cx < W and 0 <= cy < H):
            continue
        seen.add((cx, cy))
        if px[cx, cy] != target or (allowed and not allowed(cx, cy)):
            continue
        px[cx, cy] = color
        stack += [(cx + 1, cy), (cx - 1, cy), (cx, cy + 1), (cx, cy - 1)]
    return out


def middle_changed(img: Image.Image, original: Image.Image) -> bool:
    """Whether the DS's part of the layer differs from the capture's."""
    box = (DS_X0, 0, DS_X1, H)
    return img.crop(box).tobytes() != original.crop(box).tobytes()


def save_art(art_dir: str | os.PathLike, layer: Layer, img: Image.Image, original: Image.Image) -> Path:
    """The picture where the game finds it: the bands only, or (the middle
    changed) the whole layer. The other file for that layer goes."""
    full = middle_changed(img, original)
    art_dir = Path(art_dir)
    path = art_dir / layer.art_name(full)
    for old in art_dir.glob(f"{layer.key}_*_*.png"):
        if old != path:
            old.unlink()
    clean = Image.new("RGBA", (W, H))
    clean.putdata([snap(p) for p in img.convert("RGBA").getdata()])
    clean.save(path)
    return path


def delete_art(art_dir: str | os.PathLike, layer: Layer) -> int:
    n = 0
    for old in Path(art_dir).glob(f"{layer.key}_*_*.png"):
        old.unlink()
        n += 1
    return n


def palette(img: Image.Image, limit: int = 64) -> list[tuple[int, int, int, int]]:
    """A picture's colours, the commonest first (opaque ones)."""
    colors = img.convert("RGBA").getcolors(W * H) or []
    colors = [(n, c) for n, c in colors if c[3] >= 128]
    colors.sort(key=lambda nc: -nc[0])
    return [snap(c) for _, c in colors[:limit]]


def composite(cap: Capture, images: dict[int, Image.Image], only: int | None = None,
              see_through: int | None = None, opacity: float = 0.35) -> Image.Image:
    """The layers over the backdrop, back to front (blending left out: an
    approximation of the game's screen). only: that BG alone. see_through:
    the layers in front of that BG drawn faintly (opacity), so what's drawn
    on it shows through them."""
    out = Image.new("RGBA", (W, H), cap.backdrop + (255,))
    in_front = False
    for layer in draw_order(cap.layers):
        if only is not None and layer.bg != only:
            continue
        img = images.get(layer.bg)
        if img is None:
            continue
        if in_front:
            faint = img.copy()
            faint.putalpha(img.getchannel("A").point(lambda a: int(a * opacity)))
            img = faint
        out.alpha_composite(img)
        if see_through is not None and layer.bg == see_through:
            in_front = True
    return out


def make_sample_capture(art_dir: str | os.PathLike, number: int = 1) -> Path:
    """A capture like the game's (two layers: a gradient behind, a white
    bar in front), for the self-test and the tests."""
    cap = Path(art_dir) / "captures" / f"{number:04d}"
    cap.mkdir(parents=True)
    bg3 = Image.new("RGBA", (W, H))
    bg3.putdata([(x % 256, 7, 9, 255) for y in range(H) for x in range(W)])
    bg3.save(cap / "bg3.png")
    bg1 = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    bg1.paste((255, 255, 255, 255), (DS_X0, 80, DS_X1, 112))
    bg1.save(cap / "bg1.png")
    composite(Capture(cap, [Layer(1, 0, "", 0, 0, "bg1.png"), Layer(3, 3, "", 0, 0, "bg3.png")]),
              {1: bg1, 3: bg3}).save(cap / "screen.png")
    (cap / "info.txt").write_text(
        "# pokeplatinum-3ds wide art capture (docs/WIDE_ART.md)\n"
        "size 320 192\nds 32 256\nengine A\nhas3d 0\nbackdrop 001f\n"
        "bg 1 prio 0 key c348e56dbb30d48a hofs 0 vofs 0 file bg1.png\n"
        "bg 3 prio 3 key 6D5A3D55615351BF hofs 8 vofs 300 file bg3.png\n", encoding="utf-8")
    return cap


def preview_3ds(img: Image.Image, scale: int = 2) -> Image.Image:
    """As the 3DS shows it: 1.25 times the DS's size (400 x 240, the
    remaster's sharp filter: each pixel doubled, then smoothly to size),
    then scale times bigger for looking at."""
    doubled = img.convert("RGB").resize((W * 2, H * 2), Image.NEAREST)
    shown = doubled.resize((400, 240), Image.BILINEAR)
    return shown.resize((400 * scale, 240 * scale), Image.NEAREST)
