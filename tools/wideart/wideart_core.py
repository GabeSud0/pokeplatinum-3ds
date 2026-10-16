"""The WideArt editor's workings, apart from its window (wideart.py): the
captures the game makes, the pictures it reads, and what the editor does
to a layer. See docs/WIDE_ART.md.

A capture (the game: L + R + X) is a folder wide/captures/NNNN with
screen.png (the whole upper screen, 320 x 192), bg<N>.png (each BG alone,
transparent where it has no pixels), obj<G>.png (each group of sprites:
the OBJs of one priority and palette) and info.txt (each layer's key,
scroll or position, and priority). A picture the game reads is
wide/<key>_<hofs>_<vofs>.png (the side bands only) or ..._full.png (the
whole layer). A capture also has the pictures the screen's layout draws
itself (picture_<name>.png: the party's panel, the boxes' divider...),
each on the canvas where it's drawn, unturned; their files are
wide/<name>.png, saved back as the box round what's drawn, with how far
that is from the layout's place (the PNG's oFFs chunk) when it's moved.

The editor draws a layer as a stack of drawing layers of its own (added,
duplicated, moved, hidden), kept in wide/editor/<key>/ (layers.json and a
PNG each); saving flattens them into the one picture the game reads.
"""

from __future__ import annotations

import colorsys
import io
import json
import os
import struct
import zlib
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
    position (where its picture is anchored). Or a picture the screen's
    layout draws (kind "picture": key its name, drawn with BG bg; hofs,
    vofs: the layout's place for it (its file's top-left, unmoved); what's
    drawn in it was w x h, dx, dy from there, turned angle degrees; group:
    its place in the capture's list)."""
    bg: int
    prio: int
    key: str
    hofs: int
    vofs: int
    file: str
    kind: str = "bg"
    group: int = 0
    count: int = 0
    w: int = 0
    h: int = 0
    angle: int = 0
    dx: int = 0
    dy: int = 0

    def art_name(self, full: bool, loop: bool = False) -> str:
        if self.kind == "picture":
            return f"{self.key}.png"
        return f"{self.key}_{self.hofs}_{self.vofs}{'_full' if full else ''}{'_loop' if loop else ''}.png"

    @property
    def id(self) -> str:
        if self.kind == "picture":
            return f"pic_{self.key}"
        return f"obj{self.group}" if self.kind == "obj" else f"bg{self.bg}"

    @property
    def label(self) -> str:
        if self.kind == "obj":
            pal = "8-bit" if self.group & 16 else f"palette {self.group & 15}"
            return f"Sprites, {pal}  (priority {self.prio}, {self.count} OBJs)"
        if self.kind == "picture":
            return f"Picture: {self.key}  ({self.w} x {self.h}, with BG{self.bg})"
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


def pixels(img: Image.Image) -> list:
    """A picture's pixels, row after row (getdata goes in Pillow 14)."""
    get = getattr(img, "get_flattened_data", None)
    return list(get() if get is not None else img.getdata())


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
        elif words[0] == "picture" and len(words) >= 18:
            # (x, y: where what's drawn in it was; dx, dy: how far that is from the layout's place)
            f = dict(zip(words[2::2], words[3::2]))
            dx, dy = int(f.get("dx", 0)), int(f.get("dy", 0))
            cap.layers.append(Layer(bg=int(f["bg"]), prio=int(f["prio"]), key=words[1], hofs=int(f["x"]) - dx,
                                    vofs=int(f["y"]) - dy, file=f["file"], kind="picture",
                                    group=sum(1 for l in cap.layers if l.kind == "picture"),
                                    w=int(f["w"]), h=int(f["h"]), angle=int(f.get("angle", 0)), dx=dx, dy=dy))
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


@dataclass
class ArtRef:
    """A picture the game reads (port_wideart.c's names): its file, where
    the layer was when it was made, whether it's the whole layer and
    whether its bands go round with the layer; and (found for a capture)
    how far to move it to line up with the capture: the layer has moved
    by (-dx, -dy) since."""
    path: Path
    hofs: int
    vofs: int
    full: bool
    loop: bool
    dx: int = 0
    dy: int = 0


def parse_art_name(name: str) -> tuple[str, int, int, bool, bool] | None:
    """A picture's file name: key, hofs, vofs, full, loop; None if it isn't one."""
    import re
    m = re.fullmatch(r"([0-9a-fA-F]{16})_(\d+)_(\d+)(_full)?(_loop)?\.(png|PNG)", name)
    if not m or int(m.group(2)) > 511 or int(m.group(3)) > 511:
        return None
    return m.group(1).lower(), int(m.group(2)), int(m.group(3)), bool(m.group(4)), bool(m.group(5))


def art_files(art_dir: str | os.PathLike, key: str) -> list[ArtRef]:
    out = []
    for p in sorted(Path(art_dir).glob(f"{key}_*_*.png")):
        parsed = parse_art_name(p.name)
        if parsed and parsed[0] == key:
            out.append(ArtRef(p, parsed[1], parsed[2], parsed[3], parsed[4]))
    return out


def _wrapped(d: int, period: int) -> int:
    d %= period
    return d - period if d >= period // 2 else d


def find_art(art_dir: str | os.PathLike, layer: Layer) -> ArtRef | None:
    """The picture for a capture's layer, as the game would choose it: the
    one made where the layer is (the whole one if there are both), else the
    nearest less than a screen away (a sprite group's: the nearest). A
    layout's picture: its file."""
    if layer.kind == "picture":
        path = Path(art_dir) / layer.art_name(True)
        return ArtRef(path, layer.hofs, layer.vofs, True, False) if path.is_file() else None
    best, best_dist = None, None
    vperiod = 256 if layer.kind == "obj" else 512
    for ref in art_files(art_dir, layer.key):
        ref.dx = _wrapped(ref.hofs - layer.hofs, 512)
        ref.dy = _wrapped(ref.vofs - layer.vofs, vperiod)
        if layer.kind != "obj" and (abs(ref.dx) >= 256 or abs(ref.dy) >= 192):
            continue
        dist = abs(ref.dx) + abs(ref.dy)
        if best is None or dist < best_dist or (dist == best_dist and ref.full and not best.full):
            best, best_dist = ref, dist
    return best


ART_REST = 8   # port_render2d.c: a picture's bands show while the layer is within 8 px sideways of its place


def moved_art(art_dir: str | os.PathLike, cap: Capture, layer: Layer) -> tuple[Image.Image, ArtRef] | None:
    """A whole-layer picture for where a capture's BG is, made from one of
    its pictures made at another place: that picture moved by as much as
    the layer has (nothing stretched or mirrored, what goes past the
    picture's edge left out), used only if it lines up exactly with what the
    DS shows of the layer there. None if the layer already has a picture
    there, or none of its pictures lines up (the map may be 256 or 512
    wide: both are tried)."""
    if layer.kind != "bg":
        return None
    refs = art_files(art_dir, layer.key)
    for ref in refs:
        if abs(_wrapped(ref.hofs - layer.hofs, 512)) <= ART_REST and ref.vofs == layer.vofs:
            return None
    shown = cap.layer_image(layer).crop((DS_X0, 0, DS_X1, H))
    shown_px = pixels(shown)
    if not any(p[3] for p in shown_px):
        return None
    for ref in sorted(refs, key=lambda r: not r.full):
        if not ref.full:
            continue        # (a bands-only picture has no middle to line up)
        pic = load_rgba(ref.path)
        tried = set()
        for hp in (512, 256, 1024):
            for vp in (512, 256, 1024):
                dx, dy = _wrapped(ref.hofs - layer.hofs, hp), _wrapped(ref.vofs - layer.vofs, vp)
                if (dx, dy) in tried:
                    continue
                tried.add((dx, dy))
                moved = Image.new("RGBA", (W, H))
                moved.paste(pic, (dx, dy))
                mid = pixels(moved.crop((DS_X0, 0, DS_X1, H)))
                if all((a[3] == 0 and b[3] == 0) or (a[3] and b[3] and a[:3] == b[:3])
                       for a, b in zip(mid, shown_px)):
                    ref.dx, ref.dy = dx, dy
                    return moved, ref
    return None


def existing_art(art_dir: str | os.PathLike, key: str, layer: Layer | None = None) -> Path | None:
    """The picture already made for a layer (the whole one if both): for
    a capture's layer, the one find_art chooses; else any of the key's."""
    if layer is not None:
        ref = find_art(art_dir, layer)
        return ref.path if ref else None
    found = art_files(art_dir, key)
    full = [r for r in found if r.full]
    return (full or found or [None])[0].path if (full or found) else None


def draw_order(layers: list[Layer]) -> list[Layer]:
    """Back to front: priority 3 first; of one priority, the BGs behind the
    sprites, the higher BG number further back (the DS's order); a
    layout's picture just in front of its BG (the later ones in front)."""
    def key(l: Layer):
        if l.kind == "obj":
            return (-l.prio, True, -l.group)
        if l.kind == "picture":
            return (-l.prio, False, -l.bg + 0.5 + l.group * 0.001)
        return (-l.prio, False, -l.bg)
    return sorted(layers, key=key)


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


def save_art(art_dir: str | os.PathLike, layer: Layer, img: Image.Image, original: Image.Image,
             loop: bool = False, replace: Path | None = None) -> Path:
    """The picture where the game finds it, made where the capture's layer
    is: the bands only, or (the middle changed) the whole layer; loop: its
    bands go round with the layer (a strip). The layer's other files made
    at the same place go, and replace (the picture it was drawn from, made
    a little elsewhere). Pictures of the layer at other places (another
    part of its map) stay."""
    full = middle_changed(img, original)
    art_dir = Path(art_dir)
    path = art_dir / layer.art_name(full, loop)
    for ref in art_files(art_dir, layer.key):
        if ref.path != path and ((ref.hofs, ref.vofs) == (layer.hofs, layer.vofs) or ref.path == replace):
            ref.path.unlink()
    clean = Image.new("RGBA", (W, H))
    clean.putdata([snap(p) for p in pixels(img.convert("RGBA"))])
    clean.save(path)
    return path


def png_offset(path: str | os.PathLike) -> tuple[int, int]:
    """A PNG's oFFs chunk (in pixels: where its top-left goes; the editor
    writes it for a layout's picture moved from its place), else 0, 0. As
    port_wideart.c reads it: before the pixels."""
    try:
        data = Path(path).read_bytes()
    except OSError:
        return 0, 0
    at = 8
    while at + 12 <= len(data):
        n, kind = struct.unpack(">I4s", data[at:at + 8])
        if kind in (b"IDAT", b"IEND") or at + 12 + n > len(data):
            break
        if kind == b"oFFs" and n == 9:
            x, y, unit = struct.unpack(">iiB", data[at + 8:at + 17])
            if unit == 0 and -W < x < W and -H < y < H:
                return x, y
            break
        at += 12 + n
    return 0, 0


def save_with_offset(img: Image.Image, path: str | os.PathLike, offset: tuple[int, int]):
    """A PNG with an oFFs chunk after its header (unless the offset is 0,
    0; Pillow leaves that chunk out, so it's put in here)."""
    out = io.BytesIO()
    img.save(out, "PNG")
    data = out.getvalue()
    if offset != (0, 0):
        body = b"oFFs" + struct.pack(">iiB", offset[0], offset[1], 0)
        chunk = struct.pack(">I", 9) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)
        data = data[:33] + chunk + data[33:]      # (the signature, then IHDR: 8 + 25 bytes)
    Path(path).write_bytes(data)


def picture_paint(art_dir: str | os.PathLike | None, cap: Capture, layer: Layer) -> "Paint":
    """A layout's picture as a drawing layer where it's drawn: its file
    (all of it, past the canvas too) at the layout's place, moved by the
    file's offset; else what the capture has of it."""
    path = Path(art_dir) / layer.art_name(True) if art_dir is not None else None
    if path is not None and path.is_file():
        dx, dy = png_offset(path)
        return Paint("Layer 1", load_rgba(path), layer.hofs + dx, layer.vofs + dy)
    return Paint("Layer 1", cap.layer_image(layer))


def stack_bbox(stack: list["Paint"]) -> tuple[int, int, int, int] | None:
    """Where the drawing layers' visible pixels are (canvas coordinates,
    past its edges too): x0, y0, x1, y1; None if nowhere."""
    box = None
    for p in stack:
        if not p.visible:
            continue
        b = p.image.getchannel("A").point(lambda a: 255 if a >= 128 else 0).getbbox()
        if b is None:
            continue
        b = (b[0] + p.x, b[1] + p.y, b[2] + p.x, b[3] + p.y)
        box = b if box is None else (min(box[0], b[0]), min(box[1], b[1]), max(box[2], b[2]), max(box[3], b[3]))
    return box


def flatten_box(stack: list["Paint"], box: tuple[int, int, int, int]) -> Image.Image:
    """The drawing layers' visible pixels in a box of the canvas (its
    coordinates, past its edges too)."""
    out = Image.new("RGBA", (box[2] - box[0], box[3] - box[1]), (0, 0, 0, 0))
    for p in stack:
        if p.visible:
            layer = Image.new("RGBA", out.size, (0, 0, 0, 0))
            layer.paste(p.image, (p.x - box[0], p.y - box[1]))
            out.alpha_composite(layer)
    return out


def save_picture(art_dir: str | os.PathLike, layer: Layer, stack: list["Paint"]) -> tuple[Path, tuple[int, int]]:
    """A layout's picture saved to its file: the box round what's drawn
    (wherever it was moved or drawn on: past the canvas's edges too), and
    how far that box is from the layout's place, in the file (oFFs) when
    it isn't there: the game draws it there, and turns it about the
    middle of what's drawn. Nothing drawn: an empty picture (the game
    draws nothing). Also that offset."""
    path = Path(art_dir) / layer.art_name(True)
    drawn = stack_bbox(stack)
    if drawn is None:
        Image.new("RGBA", (1, 1)).save(path)
        return path, (0, 0)
    img = flatten_box(stack, drawn)
    clean = Image.new("RGBA", img.size)
    clean.putdata([snap(p) for p in pixels(img)])
    offset = (drawn[0] - layer.hofs, drawn[1] - layer.vofs)
    save_with_offset(clean, path, offset)
    return path, offset


def delete_art(art_dir: str | os.PathLike, layer: Layer) -> int:
    """The layer's picture (find_art's) and any others made at the same place."""
    if layer.kind == "picture":
        path = Path(art_dir) / layer.art_name(True)
        if path.is_file():
            path.unlink()
            return 1
        return 0
    n = 0
    found = find_art(art_dir, layer)
    for ref in art_files(art_dir, layer.key):
        if (ref.hofs, ref.vofs) == (layer.hofs, layer.vofs) or (found and ref.path == found.path):
            ref.path.unlink()
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


def make_sample_capture(art_dir: str | os.PathLike, number: int = 1, sprites: bool = False,
                        picture: bool = False) -> Path:
    """A capture like the game's (two layers: a gradient behind, a white
    bar in front; sprites: and a green bar of sprites; picture: and a
    layout's picture, party_ball, 8 x 6, half past the canvas's left edge,
    with BG3, its file in the folder), for the self-test and the tests."""
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
    if picture:
        ball = Image.new("RGBA", (8, 6), (200, 40, 60, 255))
        ball.paste((240, 240, 240, 255), (0, 3, 8, 6))      # (red over white)
        ball.save(Path(art_dir) / "party_ball.png")
        on_canvas = Image.new("RGBA", (W, H), (0, 0, 0, 0))
        on_canvas.paste(ball, (-4, 10))
        on_canvas.save(cap / "picture_party_ball.png")
        layers.append(Layer(3, 3, "party_ball", -4, 10, "picture_party_ball.png", "picture", 0, 0, 8, 6))
        images["pic_party_ball"] = on_canvas
        lines += "picture party_ball bg 3 prio 3 x -4 y 10 w 8 h 6 angle 0 file picture_party_ball.png\n"
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


def project_dir(art_dir: str | os.PathLike, key: str, hofs: int | None = None, vofs: int | None = None) -> Path:
    """Where a picture's drawing layers are kept: one for each place the
    layer has a picture (editor/<key>_<hofs>_<vofs>); editor/<key> was
    the one for all, before that."""
    name = key if hofs is None else f"{key}_{hofs}_{vofs}"
    return Path(art_dir) / "editor" / name


def save_project(art_dir: str | os.PathLike, key: str, stack: list[Paint], active: int = 0,
                 hofs: int | None = None, vofs: int | None = None, art: Path | None = None) -> Path:
    """The drawing layers, to open again as they were; art: the picture
    saved from them (by default the key's)."""
    d = project_dir(art_dir, key, hofs, vofs)
    d.mkdir(parents=True, exist_ok=True)
    for old in d.glob("*.png"):
        old.unlink()
    entries = []
    for i, p in enumerate(stack):
        name = f"{i:02d}.png"
        p.image.save(d / name)
        entries.append({"name": p.name, "file": name, "x": p.x, "y": p.y, "visible": p.visible})
    if art is None:
        art = existing_art(art_dir, key)
    data = {"version": 1, "layers": entries, "active": active,
            "art": art.name if art else None, "art_mtime": art.stat().st_mtime if art else None}
    (d / "layers.json").write_text(json.dumps(data, indent=1), encoding="utf-8")
    return d


def load_project(art_dir: str | os.PathLike, key: str, hofs: int | None = None, vofs: int | None = None,
                 art: Path | None = None) -> tuple[list[Paint], int] | None:
    """The drawing layers saved for a picture (art: that picture; by
    default the key's), unless the picture was changed since (in another
    program): None then, or if none."""
    if art is None:
        art = existing_art(art_dir, key)
    dirs = [project_dir(art_dir, key, hofs, vofs)] if hofs is not None else []
    dirs.append(project_dir(art_dir, key))
    for d in dirs:
        try:
            data = json.loads((d / "layers.json").read_text(encoding="utf-8"))
            if art is not None and (data.get("art") != art.name or data.get("art_mtime") != art.stat().st_mtime):
                continue
            stack = [Paint(e["name"], load_rgba(d / e["file"]), int(e["x"]), int(e["y"]), bool(e["visible"]))
                     for e in data["layers"]]
        except (OSError, ValueError, KeyError, TypeError):
            continue
        if stack:
            return stack, max(0, min(int(data.get("active", 0)), len(stack) - 1))
    return None


def delete_project(art_dir: str | os.PathLike, key: str, hofs: int | None = None, vofs: int | None = None) -> None:
    for d in ([project_dir(art_dir, key, hofs, vofs)] if hofs is not None else []) + [project_dir(art_dir, key)]:
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


def transform(img: Image.Image, sx: float = 1.0, sy: float = 1.0, angle: float = 0.0,
              smooth: bool = False) -> Image.Image:
    """A piece scaled (sx, sy: 1 is its size) and turned (angle degrees,
    anticlockwise), grown to fit; nearest pixels (sharp, as pixel art
    wants) unless smooth. Right angles are exact."""
    w = max(1, round(img.width * sx))
    h = max(1, round(img.height * sy))
    resample = Image.BILINEAR if smooth else Image.NEAREST
    out = img.resize((w, h), resample) if (w, h) != img.size else img.copy()
    turn = angle % 360
    if turn in (90, 180, 270):
        return out.transpose({90: Image.ROTATE_90, 180: Image.ROTATE_180, 270: Image.ROTATE_270}[int(turn)])
    if turn:
        out = out.rotate(turn, resample=resample, expand=True)
    return out
