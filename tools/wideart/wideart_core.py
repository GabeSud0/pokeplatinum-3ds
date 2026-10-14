"""The WideArt editor's workings, apart from its window (wideart.py): the
captures the game makes, the pictures it reads, and what the editor does
to a layer. See docs/WIDE_ART.md.

A capture (the game: L + R + X) is a folder wide/captures/NNNN with
screen.png (the whole upper screen, 320 x 192), bg<N>.png (each BG alone,
transparent where it has no pixels), obj<G>.png (each group of sprites:
the OBJs of one priority and palette) and info.txt (each layer's key,
scroll or position, and priority). A picture the game reads is
wide/<key>_<hofs>_<vofs>.png (the side bands only) or ..._full.png (the
whole layer).

The editor draws a layer as a stack of drawing layers of its own (added,
duplicated, moved, hidden), kept in wide/editor/<key>/ (layers.json and a
PNG each); saving flattens them into the one picture the game reads.
"""

from __future__ import annotations

import colorsys
import json
import os
from dataclasses import dataclass, field
from pathlib import Path

from PIL import Image, ImageChops

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
    """One of the game's layers in a capture: a BG (kind "bg", bg its
    number) or a group of sprites (kind "obj", group: priority << 5 |
    palette, or 16 for 8-bit ones). hofs, vofs: a BG's scroll, a group's
    position (where its picture is anchored)."""
    bg: int
    prio: int
    key: str
    hofs: int
    vofs: int
    file: str
    kind: str = "bg"
    group: int = 0
    count: int = 0

    def art_name(self, full: bool) -> str:
        return f"{self.key}_{self.hofs}_{self.vofs}{'_full' if full else ''}.png"

    @property
    def id(self) -> str:
        return f"obj{self.group}" if self.kind == "obj" else f"bg{self.bg}"

    @property
    def label(self) -> str:
        if self.kind == "obj":
            pal = "8-bit" if self.group & 16 else f"palette {self.group & 15}"
            return f"Sprites, {pal}  (priority {self.prio}, {self.count} OBJs)"
        return f"BG{self.bg}  (priority {self.prio})"


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

    def layer(self, ident: int | str) -> Layer | None:
        """A layer by id ("bg3", "obj102") or BG number."""
        for layer in self.layers:
            if layer.id == ident or (isinstance(ident, int) and layer.kind == "bg" and layer.bg == ident):
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
        elif words[0] == "obj" and len(words) >= 14:
            f = dict(zip(words[2::2], words[3::2]))
            cap.layers.append(Layer(bg=-1, prio=int(f["prio"]), key=f["key"].lower(), hofs=int(f["x"]),
                                    vofs=int(f["y"]), file=f["file"], kind="obj", group=int(words[1]),
                                    count=int(f.get("count", 0))))
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
    """Back to front: priority 3 first; of one priority, the BGs behind the
    sprites, the higher BG number further back (the DS's order)."""
    return sorted(layers, key=lambda l: (-l.prio, l.kind == "obj", -l.bg if l.kind == "bg" else -l.group))


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


def composite(cap: Capture, images: dict, only: str | int | None = None,
              see_through: str | int | None = None, opacity: float = 0.35,
              hidden: set[str] | frozenset = frozenset()) -> Image.Image:
    """The layers (images by layer id, or a BG's by its number) over the backdrop, back to front
    (blending left out: an approximation of the game's screen). only: that
    layer alone. see_through: the layers in front of that one drawn faintly
    (opacity), so what's drawn on it shows through them. hidden: layers
    left out."""
    def match(layer: Layer, ident) -> bool:
        return layer.id == ident or (isinstance(ident, int) and layer.kind == "bg" and layer.bg == ident)

    out = Image.new("RGBA", (W, H), cap.backdrop + (255,))
    in_front = False
    for layer in draw_order(cap.layers):
        if (only is not None and not match(layer, only)) or layer.id in hidden:
            continue
        img = images.get(layer.id)
        if img is None and layer.kind == "bg":
            img = images.get(layer.bg)   # (by BG number)
        if img is None:
            continue
        if in_front:
            faint = img.copy()
            faint.putalpha(img.getchannel("A").point(lambda a: int(a * opacity)))
            img = faint
        out.alpha_composite(img)
        if see_through is not None and match(layer, see_through):
            in_front = True
    return out


def make_sample_capture(art_dir: str | os.PathLike, number: int = 1, sprites: bool = False) -> Path:
    """A capture like the game's (two layers: a gradient behind, a white
    bar in front; sprites: and a green bar of sprites), for the self-test
    and the tests."""
    cap = Path(art_dir) / "captures" / f"{number:04d}"
    cap.mkdir(parents=True)
    bg3 = Image.new("RGBA", (W, H))
    bg3.putdata([(x % 256, 7, 9, 255) for y in range(H) for x in range(W)])
    bg3.save(cap / "bg3.png")
    bg1 = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    bg1.paste((255, 255, 255, 255), (DS_X0, 80, DS_X1, 112))
    bg1.save(cap / "bg1.png")
    layers = [Layer(1, 0, "", 0, 0, "bg1.png"), Layer(3, 3, "", 0, 0, "bg3.png")]
    images = {"bg1": bg1, "bg3": bg3}
    lines = ("bg 1 prio 0 key c348e56dbb30d48a hofs 0 vofs 0 file bg1.png\n"
             "bg 3 prio 3 key 6D5A3D55615351BF hofs 8 vofs 300 file bg3.png\n")
    if sprites:
        obj = Image.new("RGBA", (W, H), (0, 0, 0, 0))
        obj.paste((0, 200, 0, 255), (DS_X0, 120, DS_X1, 140))
        obj.save(cap / "obj102.png")
        layers.append(Layer(-1, 3, "", 0, 0, "obj102.png", "obj", 102))
        images["obj102"] = obj
        lines += "obj 102 prio 3 key 10d372806e45f5c2 x 368 y 64 count 72 file obj102.png\n"
    composite(Capture(cap, layers), images).save(cap / "screen.png")
    (cap / "info.txt").write_text(
        "# pokeplatinum-3ds wide art capture (docs/WIDE_ART.md)\n"
        "size 320 192\nds 32 256\nengine A\nhas3d 0\nbackdrop 001f\n" + lines, encoding="utf-8")
    return cap


def preview_3ds(img: Image.Image, scale: int = 2) -> Image.Image:
    """As the 3DS shows it: 1.25 times the DS's size (400 x 240, the
    remaster's sharp filter: each pixel doubled, then smoothly to size),
    then scale times bigger for looking at."""
    doubled = img.convert("RGB").resize((W * 2, H * 2), Image.NEAREST)
    shown = doubled.resize((400, 240), Image.BILINEAR)
    return shown.resize((400 * scale, 240 * scale), Image.NEAREST)


# ----------------------------------------------------------- drawing layers

@dataclass
class Paint:
    """A drawing layer: its pixels and where they are on the 320 x 192
    canvas (x, y: the image's top-left; it may reach past the canvas, so
    moving it loses nothing)."""
    name: str
    image: Image.Image
    x: int = 0
    y: int = 0
    visible: bool = True

    def copy(self, name: str | None = None) -> "Paint":
        return Paint(name or self.name, self.image.copy(), self.x, self.y, self.visible)

    def contains(self, cx: int, cy: int) -> bool:
        return self.x <= cx < self.x + self.image.width and self.y <= cy < self.y + self.image.height

    def grow(self, x0: int = 0, y0: int = 0, x1: int = W, y1: int = H) -> None:
        """Make the image cover the box (canvas coordinates) as well."""
        nx0, ny0 = min(self.x, x0), min(self.y, y0)
        nx1, ny1 = max(self.x + self.image.width, x1), max(self.y + self.image.height, y1)
        if (nx0, ny0, nx1, ny1) == (self.x, self.y, self.x + self.image.width, self.y + self.image.height):
            return
        img = Image.new("RGBA", (nx1 - nx0, ny1 - ny0), (0, 0, 0, 0))
        img.paste(self.image, (self.x - nx0, self.y - ny0))
        self.image, self.x, self.y = img, nx0, ny0

    def view(self) -> Image.Image:
        """Its pixels on the canvas (W x H)."""
        out = Image.new("RGBA", (W, H), (0, 0, 0, 0))
        out.paste(self.image, (self.x, self.y))
        return out

    def set_view(self, img: Image.Image) -> None:
        """Its pixels on the canvas replaced by img's (W x H); the rest kept."""
        self.grow()
        self.image.paste(img, (-self.x, -self.y))

    def plot(self, cx: int, cy: int, color) -> None:
        if not self.contains(cx, cy):
            self.grow()
        self.image.putpixel((cx - self.x, cy - self.y), tuple(color))


def flatten(stack: list[Paint]) -> Image.Image:
    """The drawing layers' visible pixels on the canvas, bottom (first) to
    top."""
    out = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    for p in stack:
        if p.visible:
            out.alpha_composite(p.view())
    return out


def lock_middle(img: Image.Image, original: Image.Image) -> Image.Image:
    """img with the DS's part put back as the capture had it."""
    out = img.copy()
    out.paste(original.crop((DS_X0, 0, DS_X1, H)), (DS_X0, 0))
    return out


def project_dir(art_dir: str | os.PathLike, key: str) -> Path:
    return Path(art_dir) / "editor" / key


def save_project(art_dir: str | os.PathLike, key: str, stack: list[Paint], active: int = 0) -> Path:
    """The drawing layers, to open again as they were."""
    d = project_dir(art_dir, key)
    d.mkdir(parents=True, exist_ok=True)
    for old in d.glob("*.png"):
        old.unlink()
    entries = []
    for i, p in enumerate(stack):
        name = f"{i:02d}.png"
        p.image.save(d / name)
        entries.append({"name": p.name, "file": name, "x": p.x, "y": p.y, "visible": p.visible})
    art = existing_art(art_dir, key)
    data = {"version": 1, "layers": entries, "active": active,
            "art": art.name if art else None, "art_mtime": art.stat().st_mtime if art else None}
    (d / "layers.json").write_text(json.dumps(data, indent=1), encoding="utf-8")
    return d


def load_project(art_dir: str | os.PathLike, key: str) -> tuple[list[Paint], int] | None:
    """The drawing layers saved for a layer, unless the picture the game
    reads was changed since (in another program): None then, or if none."""
    d = project_dir(art_dir, key)
    try:
        data = json.loads((d / "layers.json").read_text(encoding="utf-8"))
        art = existing_art(art_dir, key)
        if art is not None and (data.get("art") != art.name or data.get("art_mtime") != art.stat().st_mtime):
            return None
        stack = [Paint(e["name"], load_rgba(d / e["file"]), int(e["x"]), int(e["y"]), bool(e["visible"]))
                 for e in data["layers"]]
    except (OSError, ValueError, KeyError, TypeError):
        return None
    if not stack:
        return None
    return stack, max(0, min(int(data.get("active", 0)), len(stack) - 1))


def delete_project(art_dir: str | os.PathLike, key: str) -> None:
    d = project_dir(art_dir, key)
    if d.is_dir():
        for f in d.iterdir():
            f.unlink()
        d.rmdir()


def brush(size: int) -> list[tuple[int, int]]:
    """The pixels of a round brush size pixels across, round the pixel
    under the cursor (1: that pixel, 2: a 2 x 2 square)."""
    size = max(1, size)
    if size <= 2:
        return [(dx, dy) for dy in range(size) for dx in range(size)]
    c = (size - 1) / 2
    r2 = (size / 2) ** 2 + 0.25
    off = (size - 1) // 2
    return [(dx - off, dy - off) for dy in range(size) for dx in range(size)
            if (dx - c) ** 2 + (dy - c) ** 2 <= r2]


def shift_band_fill(img: Image.Image, source: Image.Image, how: str, side: str = "both") -> Image.Image:
    """img with its bands filled from source's DS part (fill_bands); the
    middle and the bands not chosen kept."""
    filled = fill_bands(source, how, side)
    out = img.copy()
    for (x0, x1), name in ((BANDS[0], "left"), (BANDS[1], "right")):
        if side in ("both", name):
            out.paste(filled.crop((x0, 0, x1, H)), (x0, 0))
    return out


# ------------------------------------------------------------------ colours

def hsv_to_rgb(h: float, s: float, v: float) -> tuple[int, int, int]:
    """h 0-360, s and v 0-1."""
    r, g, b = colorsys.hsv_to_rgb((h % 360) / 360, s, v)
    return (round(r * 255), round(g * 255), round(b * 255))


def rgb_to_hsv(rgb: tuple[int, ...]) -> tuple[float, float, float]:
    h, s, v = colorsys.rgb_to_hsv(*(c / 255 for c in rgb[:3]))
    return (h * 360, s, v)


def hex_color(rgb: tuple[int, ...]) -> str:
    return "#%02x%02x%02x" % tuple(rgb[:3])


def parse_hex(text: str) -> tuple[int, int, int] | None:
    t = text.strip().lstrip("#")
    if len(t) == 3:
        t = "".join(c * 2 for c in t)
    if len(t) != 6:
        return None
    try:
        return (int(t[0:2], 16), int(t[2:4], 16), int(t[4:6], 16))
    except ValueError:
        return None


def sv_square(hue: float, size: int = 256) -> Image.Image:
    """The colour picker's square for a hue: saturation left to right,
    brightness top to bottom."""
    pure = hsv_to_rgb(hue, 1, 1)
    row = Image.new("RGB", (size, 1))
    row.putdata([tuple(round(255 + (p - 255) * x / (size - 1)) for p in pure) for x in range(size)])
    sat = row.resize((size, size), Image.NEAREST)
    col = Image.new("L", (1, size))
    col.putdata([255 - round(255 * y / (size - 1)) for y in range(size)])
    dark = col.resize((size, size), Image.NEAREST)
    return ImageChops.multiply(sat, Image.merge("RGB", (dark, dark, dark)))


def hue_bar(width: int = 24, height: int = 256) -> Image.Image:
    """The hues, 0 at the top to 360 at the bottom."""
    col = Image.new("RGB", (1, height))
    col.putdata([hsv_to_rgb(360 * y / (height - 1), 1, 1) for y in range(height)])
    return col.resize((width, height), Image.NEAREST)
