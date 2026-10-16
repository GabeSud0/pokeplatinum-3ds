#!/usr/bin/env python3
"""WideArt: draw the remaster's wide upper screen by hand.

The game (L + R + X on the 3DS or in Azahar) captures the upper screen into
3ds/pokeplatinum/wide/captures; this opens those captures, lets you paint
each layer's side bands (or the whole layer), in drawing layers of your own,
and saves the pictures where and as the game reads them
(3ds/pokeplatinum/wide). See docs/WIDE_ART.md.

    python tools/wideart/wideart.py [FOLDER]

(The Windows .exe is this, packed by CI: WideArt.exe.)
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
import tkinter as tk
from pathlib import Path
from tkinter import filedialog, messagebox, simpledialog, ttk

from PIL import Image, ImageDraw, ImageTk

sys.path.insert(0, str(Path(__file__).resolve().parent))
import wideart_core as core  # noqa: E402

APP = "WideArt"
SETTINGS = Path(os.environ.get("APPDATA") or Path.home() / ".config") / "WideArt" / "settings.json"
UNDO_LIMIT = 40
RECENT = 16
TOOLS = (("pencil", "Brush (B)"), ("eraser", "Eraser (E)"), ("fill", "Fill (G)"),
         ("pick", "Pick colour (I)"), ("select", "Select (M)"), ("move", "Move (V)"))


def load_settings() -> dict:
    try:
        return json.loads(SETTINGS.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {}


def save_settings(data: dict) -> None:
    try:
        SETTINGS.parent.mkdir(parents=True, exist_ok=True)
        SETTINGS.write_text(json.dumps(data, indent=1), encoding="utf-8")
    except OSError:
        pass


def open_externally(path: Path) -> None:
    if os.name == "nt":
        os.startfile(str(path))  # type: ignore[attr-defined]
    elif sys.platform == "darwin":
        subprocess.Popen(["open", str(path)])
    else:
        subprocess.Popen(["xdg-open", str(path)])


# ------------------------------------------------------------ colour picker

class ColorPicker(tk.Toplevel):
    """A colour picker like a paint program's: the hue bar, and saturation
    and brightness in the square; the colour by its numbers too. The
    colour comes out as the DS shows it (5 bits a channel). result: the
    colour, or None if cancelled."""

    SIZE = 256

    def __init__(self, master, color, recent: list[tuple[int, int, int, int]]):
        super().__init__(master)
        self.title("Colour")
        self.resizable(False, False)
        self.transient(master)
        self.result = None
        self.old = core.snap(color)
        self.h, self.s, self.v = core.rgb_to_hsv(self.old)
        self.updating = False
        self.vars = {k: tk.StringVar() for k in ("h", "s", "b", "r", "g", "bl", "hex")}
        self.ds_text = tk.StringVar()

        body = ttk.Frame(self, padding=8)
        body.pack()
        self.square = tk.Canvas(body, width=self.SIZE, height=self.SIZE, highlightthickness=1,
                                highlightbackground="#666", cursor="crosshair")
        self.square.grid(row=0, column=0, rowspan=12)
        self.bar = tk.Canvas(body, width=24, height=self.SIZE, highlightthickness=1,
                             highlightbackground="#666", cursor="sb_v_double_arrow")
        self.bar.grid(row=0, column=1, rowspan=12, padx=(6, 10))
        self.bar_photo = ImageTk.PhotoImage(core.hue_bar(24, self.SIZE))
        self.bar.create_image(0, 0, anchor="nw", image=self.bar_photo)
        for widget, fn in ((self.square, self.on_square), (self.bar, self.on_bar)):
            widget.bind("<Button-1>", fn)
            widget.bind("<B1-Motion>", fn)

        side = ttk.Frame(body)
        side.grid(row=0, column=2, rowspan=12, sticky="n")
        ttk.Label(side, text="new").grid(row=0, column=0, sticky="w")
        self.preview = tk.Canvas(side, width=72, height=64, highlightthickness=1, highlightbackground="#666")
        self.preview.grid(row=1, column=0, columnspan=3, sticky="w")
        self.preview.bind("<Button-1>", lambda e: self.set_rgb(self.old) if e.y >= 32 else None)
        ttk.Label(side, text="current").grid(row=2, column=0, sticky="w")
        fields = (("H", "h", "\N{DEGREE SIGN}"), ("S", "s", "%"), ("B", "b", "%"),
                  ("R", "r", ""), ("G", "g", ""), ("B", "bl", ""))
        for i, (label, key, unit) in enumerate(fields):
            row = 3 + i + (1 if i >= 3 else 0)
            ttk.Label(side, text=label).grid(row=row, column=0, sticky="e", pady=1)
            e = ttk.Entry(side, textvariable=self.vars[key], width=6)
            e.grid(row=row, column=1, sticky="w", padx=4)
            e.bind("<Return>", lambda ev, k=key: self.from_field(k))
            e.bind("<FocusOut>", lambda ev, k=key: self.from_field(k))
            ttk.Label(side, text=unit).grid(row=row, column=2, sticky="w")
        ttk.Separator(side).grid(row=6, column=0, columnspan=3, sticky="ew", pady=4)
        ttk.Label(side, text="#").grid(row=10, column=0, sticky="e")
        e = ttk.Entry(side, textvariable=self.vars["hex"], width=8)
        e.grid(row=10, column=1, columnspan=2, sticky="w", padx=4)
        e.bind("<Return>", lambda ev: self.from_field("hex"))
        e.bind("<FocusOut>", lambda ev: self.from_field("hex"))
        ttk.Label(side, textvariable=self.ds_text, foreground="#555").grid(row=11, column=0, columnspan=3,
                                                                          sticky="w", pady=(4, 0))

        if recent:
            rec = ttk.Frame(body)
            rec.grid(row=12, column=0, columnspan=3, sticky="w", pady=(8, 0))
            ttk.Label(rec, text="Recent:").pack(side="left")
            self.recent = tk.Canvas(rec, width=18 * RECENT, height=16, highlightthickness=0)
            self.recent.pack(side="left", padx=4)
            for i, c in enumerate(recent[:RECENT]):
                self.recent.create_rectangle(i * 18, 0, i * 18 + 16, 15, fill=core.hex_color(c), outline="#555")
            self.recent_colors = recent[:RECENT]
            self.recent.bind("<Button-1>", self.on_recent)
        buttons = ttk.Frame(body)
        buttons.grid(row=13, column=0, columnspan=3, sticky="e", pady=(10, 0))
        ttk.Button(buttons, text="OK", command=self.ok).pack(side="left", padx=4)
        ttk.Button(buttons, text="Cancel", command=self.destroy).pack(side="left")
        self.bind("<Return>", lambda e: self.ok())
        self.bind("<Escape>", lambda e: self.destroy())
        self.square_hue = None
        self.show()
        try:
            self.grab_set()
        except tk.TclError:   # (not shown yet on some systems: fine without)
            pass
        self.focus_set()

    # the colour as it would show in the game
    def rgb(self) -> tuple[int, int, int, int]:
        return core.snap(core.hsv_to_rgb(self.h, self.s, self.v) + (255,))

    def on_square(self, event):
        n = self.SIZE - 1
        self.s = max(0, min(n, event.x)) / n
        self.v = 1 - max(0, min(n, event.y)) / n
        self.show()

    def on_bar(self, event):
        self.h = 360 * max(0, min(self.SIZE - 1, event.y)) / (self.SIZE - 1)
        self.show()

    def on_recent(self, event):
        i = event.x // 18
        if 0 <= i < len(self.recent_colors):
            self.set_rgb(self.recent_colors[i])

    def set_rgb(self, rgb):
        self.h, self.s, self.v = core.rgb_to_hsv(rgb)
        self.show()

    def from_field(self, key: str):
        if self.updating:
            return
        try:
            if key == "hex":
                rgb = core.parse_hex(self.vars["hex"].get())
                if rgb is None:
                    raise ValueError
                self.set_rgb(rgb)
                return
            value = float(self.vars[key].get())
        except ValueError:
            self.show()
            return
        if key in ("h", "s", "b"):
            if key == "h":
                self.h = value % 360
            elif key == "s":
                self.s = max(0.0, min(100.0, value)) / 100
            else:
                self.v = max(0.0, min(100.0, value)) / 100
            self.show()
        else:
            rgb = list(core.hsv_to_rgb(self.h, self.s, self.v))
            rgb[("r", "g", "bl").index(key)] = int(max(0, min(255, value)))
            self.set_rgb(tuple(rgb))

    def show(self):
        n = self.SIZE - 1
        if self.square_hue != round(self.h, 1):
            self.square_hue = round(self.h, 1)
            self.square_photo = ImageTk.PhotoImage(core.sv_square(self.h, self.SIZE))
        self.square.delete("all")
        self.square.create_image(0, 0, anchor="nw", image=self.square_photo)
        x, y = self.s * n, (1 - self.v) * n
        ring = "#000000" if self.v > 0.6 and self.s < 0.5 else "#ffffff"
        self.square.create_oval(x - 6, y - 6, x + 6, y + 6, outline=ring, width=2)
        self.bar.delete("mark")
        hy = self.h / 360 * n
        self.bar.create_polygon(0, hy - 5, 7, hy, 0, hy + 5, fill="#000", tags="mark")
        self.bar.create_polygon(24, hy - 5, 17, hy, 24, hy + 5, fill="#000", tags="mark")
        rgb = self.rgb()
        self.preview.delete("all")
        self.preview.create_rectangle(0, 0, 72, 32, fill=core.hex_color(rgb), outline="")
        self.preview.create_rectangle(0, 32, 72, 64, fill=core.hex_color(self.old), outline="")
        self.updating = True
        raw = core.hsv_to_rgb(self.h, self.s, self.v)
        for key, value in (("h", round(self.h)), ("s", round(self.s * 100)), ("b", round(self.v * 100)),
                           ("r", raw[0]), ("g", raw[1]), ("bl", raw[2])):
            self.vars[key].set(str(value))
        self.vars["hex"].set(core.hex_color(raw)[1:])
        self.ds_text.set("in the game: %s\n(DS: %d, %d, %d of 31)" % (core.hex_color(rgb), rgb[0] >> 3,
                                                                    rgb[1] >> 3, rgb[2] >> 3))
        self.updating = False

    def ok(self):
        self.result = self.rgb()
        self.destroy()


# --------------------------------------------------------- scale / rotate

class TransformDialog(tk.Toplevel):
    """Scale and rotate the floating piece, shown on the canvas as the
    numbers change. OK keeps it (still floating, to drag into place);
    Cancel puts it back as it was."""

    def __init__(self, editor: "Editor"):
        super().__init__(editor)
        self.title("Scale / rotate")
        self.resizable(False, False)
        self.transient(editor)
        self.editor = editor
        f = editor.float
        self.base = f["image"].copy()       # (flipped by Flip H / V; scaled and turned from)
        self.original = f["image"]
        self.base_at = f["at"]
        self.center = (f["at"][0] + self.base.width / 2, f["at"][1] + self.base.height / 2)
        self.width_pct = tk.DoubleVar(value=100)
        self.height_pct = tk.DoubleVar(value=100)
        self.angle = tk.DoubleVar(value=0)
        self.keep_ratio = tk.BooleanVar(value=True)
        self.smooth = tk.BooleanVar(value=False)
        self.busy = False

        body = ttk.Frame(self, padding=10)
        body.pack()
        ttk.Label(body, text=f"The piece: {self.base.width} x {self.base.height} pixels").grid(
            row=0, column=0, columnspan=4, sticky="w", pady=(0, 6))
        ttk.Label(body, text="Width %").grid(row=1, column=0, sticky="e")
        ttk.Spinbox(body, from_=5, to=800, increment=5, textvariable=self.width_pct, width=7,
                    command=lambda: self.changed("w")).grid(row=1, column=1, sticky="w", padx=4)
        ttk.Label(body, text="Height %").grid(row=2, column=0, sticky="e")
        ttk.Spinbox(body, from_=5, to=800, increment=5, textvariable=self.height_pct, width=7,
                    command=lambda: self.changed("h")).grid(row=2, column=1, sticky="w", padx=4)
        ttk.Checkbutton(body, text="Keep the proportions", variable=self.keep_ratio).grid(
            row=1, column=2, columnspan=2, sticky="w")
        sizes = ttk.Frame(body)
        sizes.grid(row=2, column=2, columnspan=2, sticky="w")
        for pct in (50, 100, 200):
            ttk.Button(sizes, text=f"{pct}%", width=5, command=lambda v=pct: self.set_scale(v)).pack(side="left")
        ttk.Label(body, text="Rotate").grid(row=3, column=0, sticky="e", pady=(8, 0))
        ttk.Scale(body, from_=-180, to=180, variable=self.angle, orient="horizontal", length=220,
                  command=lambda v: self.changed("a")).grid(row=3, column=1, columnspan=2, sticky="ew",
                                                            pady=(8, 0))
        ttk.Spinbox(body, from_=-180, to=180, increment=1, textvariable=self.angle, width=6,
                    command=lambda: self.changed("a")).grid(row=3, column=3, sticky="w", pady=(8, 0))
        turns = ttk.Frame(body)
        turns.grid(row=4, column=0, columnspan=4, pady=6)
        for label, fn in (("90\N{DEGREE SIGN} left", lambda: self.turn(90)),
                          ("90\N{DEGREE SIGN} right", lambda: self.turn(-90)),
                          ("180\N{DEGREE SIGN}", lambda: self.turn(180)),
                          ("Flip H", lambda: self.flip(Image.FLIP_LEFT_RIGHT)),
                          ("Flip V", lambda: self.flip(Image.FLIP_TOP_BOTTOM))):
            ttk.Button(turns, text=label, command=fn).pack(side="left", padx=2)
        ttk.Checkbutton(body, text="Smooth (blends colours; off keeps the pixels sharp)", variable=self.smooth,
                        command=self.apply).grid(row=5, column=0, columnspan=4, sticky="w")
        buttons = ttk.Frame(body)
        buttons.grid(row=6, column=0, columnspan=4, sticky="e", pady=(10, 0))
        ttk.Button(buttons, text="OK", command=self.ok).pack(side="left", padx=4)
        ttk.Button(buttons, text="Cancel", command=self.cancel).pack(side="left")
        for var, which in ((self.width_pct, "w"), (self.height_pct, "h")):
            var.trace_add("write", lambda *a, k=which: self.changed(k))
        self.bind("<Return>", lambda e: self.ok())
        self.bind("<Escape>", lambda e: self.cancel())
        self.protocol("WM_DELETE_WINDOW", self.cancel)
        try:
            self.grab_set()
        except tk.TclError:   # (not shown yet on some systems: fine without)
            pass

    def number(self, var: tk.DoubleVar, default: float) -> float:
        try:
            return float(var.get())
        except (tk.TclError, ValueError):
            return default

    def set_scale(self, pct: float):
        self.busy = True
        self.width_pct.set(pct)
        self.height_pct.set(pct)
        self.busy = False
        self.apply()

    def changed(self, which: str):
        if self.busy:
            return
        if self.keep_ratio.get() and which in ("w", "h"):
            self.busy = True
            if which == "w":
                self.height_pct.set(self.number(self.width_pct, 100))
            else:
                self.width_pct.set(self.number(self.height_pct, 100))
            self.busy = False
        self.apply()

    def turn(self, degrees: float):
        a = (self.number(self.angle, 0) + degrees + 180) % 360 - 180
        self.angle.set(round(a, 1))
        self.apply()

    def flip(self, how):
        self.base = self.base.transpose(how)
        self.apply()

    def apply(self):
        sx = max(1.0, self.number(self.width_pct, 100)) / 100
        sy = max(1.0, self.number(self.height_pct, 100)) / 100
        img = core.transform(self.base, sx, sy, self.number(self.angle, 0), self.smooth.get())
        if self.smooth.get():
            # (pixels are solid or transparent in the game)
            img.putalpha(img.getchannel("A").point(lambda a: 255 if a >= 128 else 0))
        cx, cy = self.center
        self.editor.float["image"] = img
        self.editor.float["at"] = (round(cx - img.width / 2), round(cy - img.height / 2))
        self.editor.sync_selection()
        self.editor.redraw()

    def ok(self):
        self.apply()
        self.destroy()
        self.editor.status("Scaled / rotated: drag it into place, Enter puts it down.")

    def cancel(self):
        self.editor.float["image"] = self.original
        self.editor.float["at"] = self.base_at
        self.editor.sync_selection()
        self.editor.redraw()
        self.destroy()


# ------------------------------------------------------------------ editor

class Editor(tk.Tk):
    def __init__(self, folder: str | None = None):
        super().__init__()
        self.title(APP)
        self.geometry("1400x820")
        self.minsize(1100, 680)
        self.settings = load_settings()
        self.art_dir: Path | None = None
        self.captures: list[core.Capture] = []
        self.cap: core.Capture | None = None
        self.originals: dict[str, Image.Image] = {}     # each game layer as captured
        self.stacks: dict[str, list[core.Paint]] = {}    # each one's drawing layers, bottom first
        self.active: dict[str, int] = {}                 # the drawing layer being drawn on
        self.middle: dict[str, Image.Image] = {}         # the DS's part, as locked
        self.flat: dict[str, Image.Image] = {}           # the others, flattened (for showing)
        self.dirty: set[str] = set()
        self.lid: str | None = None                      # the game layer being drawn
        self.undo: list[tuple[str, list[core.Paint], int]] = []
        self.redo: list[tuple[str, list[core.Paint], int]] = []
        self.color = (248, 248, 248, 255)
        self.recent: list[tuple[int, int, int, int]] = []
        for h in self.settings.get("recent", []):
            rgb = core.parse_hex(h)
            if rgb:
                self.recent.append(core.snap(rgb + (255,)))
        self.zoom = tk.IntVar(value=max(1, min(12, int(self.settings.get("zoom", 3)))))
        self._zoom_was = self.zoom.get()                 # (the zoom the view is laid out for)
        self.tool = tk.StringVar(value="pencil")
        self.brush_size = tk.IntVar(value=int(self.settings.get("brush", 1)))
        self.lock_middle = tk.BooleanVar(value=True)
        self.show_others = tk.BooleanVar(value=True)
        self.see_through = tk.BooleanVar(value=True)
        self.show_grid = tk.BooleanVar(value=False)
        self.side = tk.StringVar(value="both")
        self.layer_var = tk.StringVar(value="")
        self.loop_var = tk.BooleanVar(value=False)
        self.refs: dict[str, core.ArtRef | None] = {}   # each layer's saved picture
        self.loops: dict[str, bool] = {}                 # its bands go round with it
        self.selection: tuple[int, int, int, int] | None = None
        self.float: dict | None = None                # lifted or pasted pixels: image, at (x, y)
        self.clipboard: Image.Image | None = None
        self.drag_start: tuple[int, int] | None = None
        self.drag: dict | None = None                 # a move under way (a drawing layer, a floating piece)
        self.clip_at = (0, 0)                         # where the copy was copied from
        self.mouse: tuple[int, int] | None = None
        self.last_point: tuple[int, int] | None = None   # where the last brush stroke ended (Shift+click: a line from it)
        self.panning = False
        self.centred = False                          # the view put on the picture (after a capture opens)
        self.photo = None
        self._build()
        self._bind_keys()
        self.protocol("WM_DELETE_WINDOW", self.on_close)
        start = folder or self.settings.get("folder")
        if start and core.find_art_dir(start):
            self.open_folder(start)
        else:
            for f in core.default_folders():
                if f.exists() and core.find_art_dir(f):
                    self.open_folder(str(f))
                    break
        if self.art_dir is None:
            self.status("Open the folder with your captures: File > Open folder (the SD card, "
                        "Azahar's sdmc folder, or 3ds\\pokeplatinum\\wide).")

    # ------------------------------------------------------------- layout
    def _build(self):
        menu = tk.Menu(self)
        filem = tk.Menu(menu, tearoff=False)
        filem.add_command(label="Open folder...", command=self.choose_folder, accelerator="Ctrl+O")
        filem.add_command(label="Refresh captures", command=self.refresh, accelerator="F5")
        filem.add_separator()
        filem.add_command(label="Save layer", command=self.save, accelerator="Ctrl+S")
        filem.add_command(label="Save all layers", command=self.save_all, accelerator="Ctrl+Shift+S")
        filem.add_separator()
        filem.add_command(label="Import a picture (PNG, PSD...) as a new drawing layer...", command=self.import_png,
                          accelerator="Ctrl+I")
        filem.add_command(label="Export this layer as PNG...", command=self.export_png, accelerator="Ctrl+Shift+E")
        filem.add_command(label="Export the drawing layer as PNG...", command=self.export_paint)
        filem.add_command(label="Export the whole screen as PNG...", command=self.export_screen)
        filem.add_command(label="Open saved picture in another program", command=self.open_outside)
        filem.add_command(label="Reload saved picture", command=self.reload_saved)
        filem.add_separator()
        filem.add_command(label="Quit", command=self.on_close)
        menu.add_cascade(label="File", menu=filem)
        editm = tk.Menu(menu, tearoff=False)
        editm.add_command(label="Undo", command=self.do_undo, accelerator="Ctrl+Z")
        editm.add_command(label="Redo", command=self.do_redo, accelerator="Ctrl+Y / Ctrl+Shift+Z")
        editm.add_separator()
        editm.add_command(label="Cut", command=self.cut, accelerator="Ctrl+X")
        editm.add_command(label="Copy", command=self.copy, accelerator="Ctrl+C")
        editm.add_command(label="Paste", command=self.paste, accelerator="Ctrl+V")
        editm.add_command(label="Paste as a new layer", command=self.paste_as_layer, accelerator="Ctrl+Shift+V")
        editm.add_command(label="Scale / rotate selection...", command=self.transform_dialog, accelerator="Ctrl+T")
        editm.add_command(label="Rotate selection 90\N{DEGREE SIGN} right", command=self.rotate_right, accelerator="R")
        editm.add_command(label="Rotate selection 90\N{DEGREE SIGN} left", command=self.rotate_left,
                          accelerator="Shift+R")
        editm.add_command(label="Flip selection (horizontal)", command=self.flip_floating, accelerator="H")
        editm.add_command(label="Flip selection (vertical)", command=self.flip_vertical, accelerator="Shift+H")
        editm.add_command(label="Delete selection", command=self.clear_selection, accelerator="Delete")
        editm.add_command(label="Put down (finish moving)", command=self.commit, accelerator="Enter")
        editm.add_separator()
        editm.add_command(label="Select all", command=self.select_all, accelerator="Ctrl+A")
        editm.add_command(label="Deselect", command=self.deselect, accelerator="Ctrl+D")
        menu.add_cascade(label="Edit", menu=editm)
        layerm = tk.Menu(menu, tearoff=False)
        layerm.add_command(label="New drawing layer", command=self.new_layer, accelerator="Ctrl+Shift+N")
        layerm.add_command(label="Duplicate drawing layer", command=self.duplicate_layer, accelerator="Ctrl+J")
        layerm.add_command(label="Delete drawing layer", command=self.delete_layer)
        layerm.add_command(label="Rename drawing layer...", command=self.rename_layer, accelerator="F2")
        layerm.add_command(label="Show / hide drawing layer", command=self.toggle_layer)
        layerm.add_separator()
        layerm.add_command(label="Move layer up", command=lambda: self.move_layer(1), accelerator="Ctrl+]")
        layerm.add_command(label="Move layer down", command=lambda: self.move_layer(-1), accelerator="Ctrl+[")
        layerm.add_command(label="Merge down", command=self.merge_down, accelerator="Ctrl+E")
        layerm.add_command(label="Flatten all", command=self.flatten_all)
        menu.add_cascade(label="Layer", menu=layerm)
        viewm = tk.Menu(menu, tearoff=False)
        viewm.add_command(label="3DS preview", command=self.preview, accelerator="P")
        viewm.add_checkbutton(label="Show the other layers", variable=self.show_others, command=self.redraw)
        viewm.add_checkbutton(label="Pixel grid", variable=self.show_grid, command=self.redraw)
        viewm.add_command(label="Put the screen in the middle", command=self.centre_view, accelerator="Home")
        for z in (1, 2, 3, 4, 5, 6, 8, 10, 12):
            viewm.add_radiobutton(label=f"Zoom {z}x", variable=self.zoom, value=z, command=lambda: self.set_zoom(self.zoom.get()))
        menu.add_cascade(label="View", menu=viewm)
        helpm = tk.Menu(menu, tearoff=False)
        helpm.add_command(label="How it works", command=self.show_help)
        menu.add_cascade(label="Help", menu=helpm)
        self.config(menu=menu)

        main = ttk.Frame(self)
        main.pack(fill="both", expand=True)

        # left: captures, the game's layers, the drawing layers
        left = ttk.Frame(main, width=260)
        left.pack(side="left", fill="y", padx=6, pady=6)
        ttk.Label(left, text="Captures (newest first)").pack(anchor="w")
        self.cap_list = tk.Listbox(left, height=8, exportselection=False, width=34)
        self.cap_list.pack(fill="x")
        self.cap_list.bind("<<ListboxSelect>>", lambda e: self.select_capture())
        self.thumb = ttk.Label(left)
        self.thumb.pack(pady=4)
        ttk.Label(left, text="The game's layers (front to back)").pack(anchor="w")
        self.layer_frame = ttk.Frame(left)
        self.layer_frame.pack(fill="x")
        ttk.Separator(left).pack(fill="x", pady=6)
        ttk.Label(left, text="Drawing layers (top first; saved as one)").pack(anchor="w")
        self.paint_list = tk.Listbox(left, height=6, exportselection=False, width=34, activestyle="none")
        self.paint_list.pack(fill="x")
        self.paint_list.bind("<<ListboxSelect>>", lambda e: self.select_paint())
        self.paint_list.bind("<Double-Button-1>", lambda e: self.rename_layer())
        pbtn = ttk.Frame(left)
        pbtn.pack(fill="x", pady=2)
        for i, (label, fn) in enumerate((("New", self.new_layer), ("Duplicate", self.duplicate_layer),
                                         ("Delete", self.delete_layer), ("Show/Hide", self.toggle_layer),
                                         ("Up", lambda: self.move_layer(1)), ("Down", lambda: self.move_layer(-1)),
                                         ("Merge", self.merge_down), ("Rename", self.rename_layer))):
            ttk.Button(pbtn, text=label, command=fn, width=8).grid(row=i // 4, column=i % 4, sticky="ew", padx=1, pady=1)
        ttk.Separator(left).pack(fill="x", pady=6)
        ttk.Checkbutton(left, text="Lock the DS's picture (middle)", variable=self.lock_middle,
                        command=self.on_lock).pack(anchor="w")
        ttk.Checkbutton(left, text="Scrolls round all the time (a strip):\nits bands move with it",
                        variable=self.loop_var, command=self.on_loop).pack(anchor="w")
        ttk.Checkbutton(left, text="Show the other layers", variable=self.show_others,
                        command=self.redraw).pack(anchor="w")
        ttk.Checkbutton(left, text="Layers in front: see-through", variable=self.see_through,
                        command=self.redraw).pack(anchor="w")

        # right: tools, brush, colours, band fills, saving
        right = ttk.Frame(main, width=260)
        right.pack(side="right", fill="y", padx=6, pady=6)
        ttk.Label(right, text="Tools").pack(anchor="w")
        tools = ttk.Frame(right)
        tools.pack(fill="x")
        for i, (name, label) in enumerate(TOOLS):
            ttk.Radiobutton(tools, text=label, variable=self.tool, value=name,
                            command=self.on_tool).grid(row=i // 2, column=i % 2, sticky="w")
        brow = ttk.Frame(right)
        brow.pack(fill="x", pady=(6, 0))
        ttk.Label(brow, text="Brush / eraser size").pack(side="left")
        self.size_label = ttk.Label(brow, width=4)
        self.size_label.pack(side="right")
        ttk.Scale(right, from_=1, to=32, orient="horizontal", variable=self.brush_size,
                  command=lambda v: self.set_size(round(float(v)))).pack(fill="x")
        ttk.Label(right, text="[ and ] change it", foreground="#666").pack(anchor="w")
        ttk.Separator(right).pack(fill="x", pady=6)
        ttk.Label(right, text="Colour (click it to choose)").pack(anchor="w")
        crow = ttk.Frame(right)
        crow.pack(fill="x")
        self.color_box = tk.Canvas(crow, width=48, height=28, highlightthickness=1, highlightbackground="#888",
                                   cursor="hand2")
        self.color_box.pack(side="left")
        self.color_box.bind("<Button-1>", lambda e: self.choose_color())
        ttk.Button(crow, text="Colour picker...", command=self.choose_color).pack(side="left", padx=6)
        ttk.Label(right, text="Recent:").pack(anchor="w", pady=(4, 0))
        self.recent_box = tk.Canvas(right, width=232, height=30, highlightthickness=0)
        self.recent_box.pack()
        self.recent_box.bind("<Button-1>", self.pick_recent)
        ttk.Label(right, text="This screen's colours:").pack(anchor="w")
        self.swatches = tk.Canvas(right, width=232, height=148, highlightthickness=0)
        self.swatches.pack()
        self.swatches.bind("<Button-1>", self.pick_swatch)
        ttk.Separator(right).pack(fill="x", pady=6)
        ttk.Label(right, text="Fill the side bands from the picture:").pack(anchor="w")
        srow = ttk.Frame(right)
        srow.pack(fill="x")
        for value, label in (("both", "Both sides"), ("left", "Left"), ("right", "Right")):
            ttk.Radiobutton(srow, text=label, variable=self.side, value=value).pack(side="left")
        frow = ttk.Frame(right)
        frow.pack(fill="x", pady=4)
        for i, (how, label) in enumerate((("mirror", "Mirror"), ("extend", "Extend edge"),
                                          ("repeat", "Repeat"), ("clear", "Clear"))):
            ttk.Button(frow, text=label, command=lambda h=how: self.fill_bands(h)).grid(
                row=i // 2, column=i % 2, sticky="ew", padx=2, pady=2)
        frow.columnconfigure(0, weight=1)
        frow.columnconfigure(1, weight=1)
        ttk.Separator(right).pack(fill="x", pady=6)
        ttk.Button(right, text="Save layer (Ctrl+S)", command=self.save).pack(fill="x", pady=2)
        irow = ttk.Frame(right)
        irow.pack(fill="x")
        ttk.Button(irow, text="Import picture...", command=self.import_png).pack(side="left", fill="x", expand=True,
                                                                              padx=(0, 1), pady=2)
        ttk.Button(irow, text="Export layer...", command=self.export_png).pack(side="left", fill="x", expand=True,
                                                                            padx=(1, 0), pady=2)
        ttk.Button(right, text="Revert to the capture", command=self.revert).pack(fill="x", pady=2)
        ttk.Button(right, text="Delete saved picture", command=self.delete_saved).pack(fill="x", pady=2)
        ttk.Button(right, text="3DS preview (P)", command=self.preview).pack(fill="x", pady=2)
        self.layer_info = ttk.Label(right, text="", wraplength=250, justify="left")
        self.layer_info.pack(anchor="w", pady=6)

        # middle: the canvas
        mid = ttk.Frame(main)
        mid.pack(side="left", fill="both", expand=True, pady=6)
        self.canvas = tk.Canvas(mid, background="#2b2b2b", highlightthickness=0, cursor="crosshair")
        xs = ttk.Scrollbar(mid, orient="horizontal", command=self.canvas.xview)
        ys = ttk.Scrollbar(mid, orient="vertical", command=self.canvas.yview)
        self.canvas.configure(xscrollcommand=xs.set, yscrollcommand=ys.set)
        ys.pack(side="right", fill="y")
        xs.pack(side="bottom", fill="x")
        self.canvas.pack(fill="both", expand=True)
        self.canvas.bind("<ButtonPress-1>", self.on_press)
        self.canvas.bind("<B1-Motion>", self.on_drag)
        self.canvas.bind("<ButtonRelease-1>", self.on_release)
        self.canvas.bind("<Motion>", self.on_motion)
        self.canvas.bind("<Leave>", lambda e: self.canvas.delete("cursor"))
        self.canvas.bind("<Button-3>", self.on_right_click)
        self.canvas.bind("<MouseWheel>", self.on_wheel)
        self.canvas.bind("<Shift-MouseWheel>", self.on_hwheel)
        self.canvas.bind("<Control-MouseWheel>", self.on_zoom_wheel)
        self.canvas.bind("<Alt-MouseWheel>", self.on_zoom_wheel)
        # (X11's wheel: buttons 4 and 5)
        for button, delta in (("4", 120), ("5", -120)):
            self.canvas.bind(f"<Button-{button}>", lambda e, d=delta: self.on_wheel(self._wheel(e, d)))
            self.canvas.bind(f"<Control-Button-{button}>", lambda e, d=delta: self.on_zoom_wheel(self._wheel(e, d)))
            self.canvas.bind(f"<Alt-Button-{button}>", lambda e, d=delta: self.on_zoom_wheel(self._wheel(e, d)))
        # dragging the whole picture round: the middle button (with Alt, or without)
        self.canvas.bind("<ButtonPress-2>", self.on_pan_start)
        self.canvas.bind("<B2-Motion>", self.on_pan)
        self.canvas.bind("<ButtonRelease-2>", self.on_pan_end)
        self.canvas.bind("<Configure>", lambda e: self.redraw() if not self.centred and self.cap else None)

        self.status_var = tk.StringVar()
        ttk.Label(self, textvariable=self.status_var, anchor="w", relief="sunken").pack(fill="x", side="bottom")
        self.update_color_box()
        self.set_size(self.brush_size.get())

    def _bind_keys(self):
        for seq, fn in (("<Control-o>", self.choose_folder), ("<F5>", self.refresh), ("<Control-s>", self.save),
                        ("<Control-S>", self.save_all), ("<Control-z>", self.do_undo), ("<Control-y>", self.do_redo),
                        ("<Control-Z>", self.do_redo), ("<Control-i>", self.import_png),
                        ("<Control-E>", self.export_png), ("<Home>", self.centre_view),
                        ("<Control-c>", self.copy), ("<Control-v>", self.paste), ("<Control-V>", self.paste_as_layer),
                        ("<Control-x>", self.cut), ("<Control-d>", self.deselect), ("<Return>", self.commit),
                        ("<KP_Enter>", self.commit),
                        ("<Delete>", self.clear_selection), ("<Escape>", self.cancel), ("<Control-a>", self.select_all),
                        ("<Control-N>", self.new_layer), ("<Control-j>", self.duplicate_layer),
                        ("<Control-e>", self.merge_down), ("<F2>", self.rename_layer),
                        ("<Control-bracketright>", lambda: self.move_layer(1)),
                        ("<Control-bracketleft>", lambda: self.move_layer(-1))):
            self.bind(seq, lambda e, f=fn: f())
        for key, value in (("b", "pencil"), ("e", "eraser"), ("g", "fill"), ("i", "pick"), ("m", "select"),
                           ("v", "move")):
            self.bind(key, lambda e, v=value: self.set_tool(v))
        self.bind("h", lambda e: self.flip_floating())
        self.bind("H", lambda e: self.flip_vertical())
        self.bind("r", lambda e: self.rotate_right())
        self.bind("R", lambda e: self.rotate_left())
        self.bind("<Control-t>", lambda e: self.transform_dialog())
        self.bind("p", lambda e: self.preview())
        self.bind("<bracketleft>", lambda e: self.set_size(self.brush_size.get() - 1))
        self.bind("<bracketright>", lambda e: self.set_size(self.brush_size.get() + 1))
        self.bind("<plus>", lambda e: self.set_zoom(self.zoom.get() + 1))
        self.bind("<equal>", lambda e: self.set_zoom(self.zoom.get() + 1))
        self.bind("<minus>", lambda e: self.set_zoom(self.zoom.get() - 1))
        for key, dx, dy in (("Left", -1, 0), ("Right", 1, 0), ("Up", 0, -1), ("Down", 0, 1)):
            self.bind(f"<{key}>", lambda e, a=dx, b=dy: self.nudge(a, b))
            self.bind(f"<Shift-{key}>", lambda e, a=dx, b=dy: self.nudge(a * 8, b * 8))

    # ------------------------------------------------------------ folders
    def choose_folder(self):
        folder = filedialog.askdirectory(title="The SD card, Azahar's sdmc folder, or 3ds/pokeplatinum/wide")
        if folder:
            self.open_folder(folder)

    def open_folder(self, folder: str):
        art = core.find_art_dir(folder)
        if art is None:
            messagebox.showinfo(APP, "No captures there yet.\n\nIn the game, press L + R + X on the screen you "
                                     "want to draw; it makes 3ds/pokeplatinum/wide/captures on the SD card. Then "
                                     "open the SD card (or Azahar's sdmc folder) here.")
            return
        if not self.confirm_discard():
            return
        self.art_dir = art
        self.settings["folder"] = str(folder)
        save_settings(self.settings)
        self.refresh()

    def refresh(self):
        if self.art_dir is None:
            return
        current = self.cap.name if self.cap else None
        self.captures = core.list_captures(self.art_dir)
        self.cap_list.delete(0, "end")
        for cap in self.captures:
            made = sum(1 for l in cap.layers if core.find_art(self.art_dir, l))
            self.cap_list.insert("end", f"{cap.name}   {len(cap.layers)} layers" + (f", {made} drawn" if made else ""))
        self.title(f"{APP} - {self.art_dir}")
        self.status(f"{len(self.captures)} captures in {self.art_dir}")
        if self.captures:
            index = next((i for i, c in enumerate(self.captures) if c.name == current), 0)
            self.cap_list.selection_clear(0, "end")
            self.cap_list.selection_set(index)
            self.select_capture(force=True)

    def load_stack(self, layer: core.Layer) -> tuple[list[core.Paint], int]:
        """A layer's drawing layers: as last saved here, else one layer of
        the picture saved for it (made elsewhere), else of the capture. A
        picture made with the layer a little elsewhere is moved to line up
        with this capture (the game moves it the same way)."""
        ref = core.find_art(self.art_dir, layer) if self.art_dir is not None else None
        self.refs[layer.id] = ref
        self.loops[layer.id] = bool(ref and ref.loop)
        if layer.kind == "picture":
            # a layout's picture: its file, where the game draws it
            if ref is not None:
                project = core.load_project(self.art_dir, layer.key, layer.hofs, layer.vofs, ref.path)
                if project is not None:
                    return project
            return [core.picture_paint(self.art_dir, self.cap, layer)], 0
        if ref is not None:
            project = core.load_project(self.art_dir, layer.key, ref.hofs, ref.vofs, ref.path)
            if project is not None:
                stack, active = project
                for p in stack:
                    p.x += ref.dx
                    p.y += ref.dy
                return stack, active
            return [core.Paint("Layer 1", core.load_rgba(ref.path), ref.dx, ref.dy)], 0
        return [core.Paint("Layer 1", self.originals[layer.id].copy())], 0

    def select_capture(self, force: bool = False):
        sel = self.cap_list.curselection()
        if not sel:
            return
        cap = self.captures[sel[0]]
        if self.cap is not None and cap.path == self.cap.path and not force:
            return
        self.commit()
        if not self.confirm_discard():
            return
        previous = self.lid if self.cap is not None and cap.path == self.cap.path else None
        self.cap = cap
        self.originals = {l.id: cap.layer_image(l) for l in cap.layers}
        self.stacks, self.active, self.middle, self.flat = {}, {}, {}, {}
        self.refs, self.loops = {}, {}
        for l in cap.layers:
            self.stacks[l.id], self.active[l.id] = self.load_stack(l)
            self.flat[l.id] = core.flatten(self.stacks[l.id])
            self.middle[l.id] = self.flat[l.id]
        self.dirty = set()
        self.undo, self.redo = [], []
        self.selection = None
        self.float = None
        self.drag = None
        self.last_point = None
        self.centred = False
        thumb = cap.screen().resize((160, 96), Image.NEAREST)
        self.thumb_photo = ImageTk.PhotoImage(thumb)
        self.thumb.configure(image=self.thumb_photo)
        for w in self.layer_frame.winfo_children():
            w.destroy()
        for l in reversed(core.draw_order(cap.layers)):
            saved = self.refs.get(l.id)
            ttk.Radiobutton(self.layer_frame, text=l.label + ("  - drawn" if saved and l.kind != "picture" else ""),
                            variable=self.layer_var, value=l.id, command=self.select_layer).pack(anchor="w")
        if cap.layers:
            # the backmost layer: usually the background to draw
            first = next((l for l in core.draw_order(cap.layers) if l.kind != "picture"), core.draw_order(cap.layers)[0])
            self.layer_var.set(previous if previous in self.stacks else first.id)
            self.select_layer()
        self.update_swatches()

    def select_layer(self):
        self.commit()
        if self.lid is not None and self.lid in self.stacks:
            self.flat[self.lid] = core.flatten(self.stacks[self.lid])
        self.lid = self.layer_var.get() or None
        self.selection = None
        layer = self.layer()
        if layer is not None:
            ref = self.refs.get(self.lid)
            if ref is not None and ref.full and layer.kind != "picture":
                self.lock_middle.set(False)   # (its middle was drawn: don't put the game's back)
            self.loop_var.set(self.loops.get(self.lid, False))
            self.update_layer_info()
        self.update_paint_list()
        self.redraw()

    def update_layer_info(self):
        layer = self.layer()
        if layer is None:
            return
        ref = self.refs.get(self.lid)
        if layer.kind == "picture":
            turned = f", turned {layer.angle}\N{DEGREE SIGN} when captured" if layer.angle else ""
            saved = f"file: {ref.path.name}" if ref else "no file (it isn't drawn)"
            self.layer_info.configure(text=f"{layer.label}\nthe game draws it with its top-left at x {layer.hofs}, "
                                           f"y {layer.vofs}{turned}\n{saved}\nSaving writes the file from that "
                                           "corner: as big as it is, or bigger if drawn past its right or bottom "
                                           "edge.")
            return
        what = "position" if layer.kind == "obj" else "scroll"
        if ref is None:
            saved = "not drawn yet"
        elif ref.dx or ref.dy:
            saved = f"saved: {ref.path.name}\n(made {abs(ref.dx)}, {abs(ref.dy)} px away: lined up)"
        else:
            saved = f"saved: {ref.path.name}"
        self.layer_info.configure(text=f"{layer.label}\nkey {layer.key}\n{what} {layer.hofs}, {layer.vofs}\n{saved}")

    def on_loop(self):
        if self.lid is not None:
            self.loops[self.lid] = self.loop_var.get()
            self.dirty.add(self.lid)
            self.redraw()

    def layer(self) -> core.Layer | None:
        return self.cap.layer(self.lid) if self.cap is not None and self.lid else None

    # ------------------------------------------------------ drawing layers
    def stack(self) -> list[core.Paint] | None:
        return self.stacks.get(self.lid) if self.lid else None

    def paint(self) -> core.Paint | None:
        stack = self.stack()
        return stack[self.active[self.lid]] if stack else None

    def update_paint_list(self):
        self.paint_list.delete(0, "end")
        stack = self.stack()
        if not stack:
            return
        for p in reversed(stack):
            self.paint_list.insert("end", p.name + ("" if p.visible else "   (hidden)"))
        index = len(stack) - 1 - self.active[self.lid]
        self.paint_list.selection_clear(0, "end")
        self.paint_list.selection_set(index)
        self.paint_list.see(index)

    def select_paint(self):
        sel = self.paint_list.curselection()
        stack = self.stack()
        if sel and stack:
            self.commit()
            self.active[self.lid] = len(stack) - 1 - sel[0]
            self.redraw()

    def changed(self):
        self.dirty.add(self.lid)
        self.update_paint_list()
        self.redraw()

    def new_name(self, base: str = "Layer") -> str:
        names = {p.name for p in self.stack() or []}
        n = 1
        while f"{base} {n}" in names:
            n += 1
        return f"{base} {n}"

    def add_paint(self, paint: core.Paint):
        stack = self.stack()
        if stack is None:
            return
        self.push_undo()
        stack.insert(self.active[self.lid] + 1, paint)
        self.active[self.lid] += 1
        self.changed()

    def new_layer(self):
        if self.stack() is not None:
            self.add_paint(core.Paint(self.new_name(), Image.new("RGBA", (core.W, core.H), (0, 0, 0, 0))))

    def duplicate_layer(self):
        p = self.paint()
        if p is not None:
            self.add_paint(p.copy(p.name + " copy"))

    def delete_layer(self):
        stack = self.stack()
        if not stack:
            return
        if len(stack) == 1:
            self.status("A layer needs one drawing layer at least.")
            return
        self.push_undo()
        del stack[self.active[self.lid]]
        self.active[self.lid] = max(0, self.active[self.lid] - 1)
        self.changed()

    def toggle_layer(self):
        p = self.paint()
        if p is not None:
            self.push_undo()
            p.visible = not p.visible
            self.changed()

    def rename_layer(self):
        p = self.paint()
        if p is None:
            return
        name = simpledialog.askstring(APP, "The drawing layer's name:", initialvalue=p.name, parent=self)
        if name:
            p.name = name.strip() or p.name
            self.update_paint_list()

    def move_layer(self, step: int):
        stack = self.stack()
        if not stack:
            return
        i = self.active[self.lid]
        j = i + step
        if not 0 <= j < len(stack):
            return
        self.push_undo()
        stack[i], stack[j] = stack[j], stack[i]
        self.active[self.lid] = j
        self.changed()

    def merge_down(self):
        stack = self.stack()
        if not stack:
            return
        i = self.active[self.lid]
        if i == 0:
            self.status("Nothing below it to merge into.")
            return
        self.push_undo()
        top, below = stack[i], stack[i - 1]
        x0, y0 = min(top.x, below.x), min(top.y, below.y)
        x1 = max(top.x + top.image.width, below.x + below.image.width)
        y1 = max(top.y + top.image.height, below.y + below.image.height)
        img = Image.new("RGBA", (x1 - x0, y1 - y0), (0, 0, 0, 0))
        if below.visible:
            img.alpha_composite(below.image, (below.x - x0, below.y - y0))
        if top.visible:
            img.alpha_composite(top.image, (top.x - x0, top.y - y0))
        stack[i - 1] = core.Paint(below.name, img, x0, y0, True)
        del stack[i]
        self.active[self.lid] = i - 1
        self.changed()

    def flatten_all(self):
        stack = self.stack()
        if not stack or len(stack) == 1:
            return
        self.push_undo()
        self.stacks[self.lid] = [core.Paint(stack[0].name, core.flatten(stack))]
        self.active[self.lid] = 0
        self.changed()

    # ---------------------------------------------------------- drawing
    def is_picture(self, lid: str | None = None) -> bool:
        """Whether a layer is a layout's picture (drawn anywhere: the DS's
        picture isn't locked for it)."""
        layer = self.cap.layer(lid or self.lid) if self.cap is not None and (lid or self.lid) else None
        return layer is not None and layer.kind == "picture"

    def allowed(self, x: int, y: int) -> bool:
        if not (0 <= x < core.W and 0 <= y < core.H):
            return False
        if self.is_picture():
            return True
        return not (self.lock_middle.get() and core.DS_X0 <= x < core.DS_X1)

    def on_lock(self):
        if self.lock_middle.get() and self.lid:
            # what the middle is now is what stays
            for lid, stack in self.stacks.items():
                self.middle[lid] = core.flatten(stack)
        self.redraw()

    def push_undo(self):
        stack = self.stack()
        if stack is None:
            return
        self.commit()   # (a floating piece goes down before anything else changes)
        self.undo.append((self.lid, [p.copy() for p in stack], self.active[self.lid]))
        del self.undo[:-UNDO_LIMIT]
        self.redo.clear()

    def _swap(self, src: list, dst: list):
        if not src:
            return
        lid, stack, active = src.pop()
        dst.append((lid, [p.copy() for p in self.stacks[lid]], self.active[lid]))
        self.stacks[lid] = stack
        self.active[lid] = active
        if lid != self.lid:
            self.layer_var.set(lid)
            self.select_layer()
        self.dirty.add(lid)
        self.update_paint_list()
        self.redraw()

    def do_undo(self):
        if self.float is not None:
            self.cancel_float()
            return
        self._swap(self.undo, self.redo)

    def do_redo(self):
        self.commit()
        self._swap(self.redo, self.undo)

    def to_pixel(self, event) -> tuple[int, int]:
        z = self.zoom.get()
        return int(self.canvas.canvasx(event.x) // z), int(self.canvas.canvasy(event.y) // z)

    def stamp(self, x: int, y: int, color):
        p = self.paint()
        if p is None:
            return
        offsets = core.brush(self.brush_size.get())
        for dx, dy in offsets:
            if self.allowed(x + dx, y + dy):
                p.plot(x + dx, y + dy, color)

    def line(self, x0, y0, x1, y1, color):
        dx, dy = abs(x1 - x0), -abs(y1 - y0)
        sx, sy = (1 if x0 < x1 else -1), (1 if y0 < y1 else -1)
        err = dx + dy
        while True:
            self.stamp(x0, y0, color)
            if (x0, y0) == (x1, y1):
                break
            e2 = 2 * err
            if e2 >= dy:
                err += dy
                x0 += sx
            if e2 <= dx:
                err += dx
                y0 += sy

    # The mouse. Select (and Move) with a selection: dragging inside it
    # lifts its pixels off the drawing layer (cut; with Alt, a copy) into a
    # floating piece that follows the mouse; it stays floating (drag it
    # again, arrow keys, H flips it) until it's put down: Enter, a click
    # outside it, another tool. Esc takes it back. A paste is a floating
    # piece too.
    def on_press(self, event):
        alt = bool(event.state & (0x20000 if os.name == "nt" else 0x0008))
        shift = bool(event.state & 0x0001)
        self.press(*self.to_pixel(event), alt=alt, shift=shift)

    def on_drag(self, event):
        self.drag_to(*self.to_pixel(event))

    def on_release(self, event):
        self.release()

    def in_selection(self, x: int, y: int) -> bool:
        s = self.selection
        return s is not None and s[0] <= x < s[2] and s[1] <= y < s[3]

    def press(self, x: int, y: int, alt: bool = False, shift: bool = False):
        p = self.paint()
        if p is None:
            return
        tool = self.tool.get()
        self.mouse = (x, y)
        if tool in ("select", "move"):
            if self.float is not None and (tool == "move" or self.in_selection(x, y)):
                self.drag = {"what": "float", "start": (x, y), "origin": self.float["at"]}
                return
            self.commit()
            if self.in_selection(x, y) and p.visible:
                self.lift(copy=alt)
                self.drag = {"what": "float", "start": (x, y), "origin": self.float["at"]}
                self.redraw()
                return
            if tool == "move":
                if not p.visible:
                    self.status("That drawing layer is hidden: show it first (Show/Hide).")
                    return
                self.push_undo()
                self.selection = None
                self.drag = {"what": "layer", "start": (x, y), "origin": (p.x, p.y)}
                self.dirty.add(self.lid)
                return
            # a new selection
            self.drag_start = (x, y)
            self.selection = None
            self.redraw()
            return
        self.commit()
        if not p.visible and tool in ("pencil", "eraser", "fill"):
            self.status("That drawing layer is hidden: show it first (Show/Hide).")
            return
        if tool in ("pencil", "eraser"):
            self.push_undo()
            color = (0, 0, 0, 0) if tool == "eraser" else self.color
            if shift and self.last_point is not None:
                # Shift + click: a straight line from where the last stroke ended
                self.line(*self.last_point, x, y, color)
            else:
                self.stamp(x, y, color)
            self.drag_start = (x, y)
            self.last_point = (x, y)
            self.dirty.add(self.lid)
        elif tool == "fill":
            if self.allowed(x, y):
                self.push_undo()
                p.set_view(core.flood_fill(p.view(), x, y, self.color, self.allowed))
                self.dirty.add(self.lid)
        elif tool == "pick":
            self.pick_at(x, y)
        self.redraw()

    def drag_to(self, x: int, y: int):
        self.mouse = (x, y)
        tool = self.tool.get()
        if self.drag is not None:
            d = self.drag
            dx, dy = x - d["start"][0], y - d["start"][1]
            if d["what"] == "layer":
                p = self.paint()
                p.x, p.y = d["origin"][0] + dx, d["origin"][1] + dy
            else:
                self.float["at"] = (d["origin"][0] + dx, d["origin"][1] + dy)
                self.sync_selection()
            locked = d["what"] == "float" and self.float.get("locked")
            self.status(f"Moved {dx:+d}, {dy:+d}" + ("  (the DS's picture is locked: only the bands' pixels move)"
                                                     if locked else ""))
            self.redraw()
            return
        if self.drag_start is None:
            return
        if tool in ("pencil", "eraser"):
            self.line(*self.drag_start, x, y, (0, 0, 0, 0) if tool == "eraser" else self.color)
            self.drag_start = (x, y)
            self.last_point = (x, y)
        elif tool == "select":
            x0, y0 = self.drag_start
            self.selection = (max(0, min(x0, x)), max(0, min(y0, y)),
                              min(core.W, max(x0, x) + 1), min(core.H, max(y0, y) + 1))
            w, h = self.selection[2] - self.selection[0], self.selection[3] - self.selection[1]
            self.status(f"Selection {w} x {h}: drag inside it to move it (Alt: a copy), Ctrl+C copies, "
                        "Ctrl+X cuts")
        self.redraw()
        self.show_cursor(x, y)

    def release(self):
        self.drag_start = None
        if self.drag is not None:
            self.drag = None
            self.redraw()

    def on_motion(self, event):
        x, y = self.to_pixel(event)
        self.mouse = (x, y)
        self.show_cursor(x, y)
        tool = self.tool.get()
        inside = (tool == "select" and self.in_selection(x, y)) or (tool == "move")
        want = "fleur" if inside else "crosshair"
        if self.canvas.cget("cursor") != want:
            self.canvas.configure(cursor=want)
        if 0 <= x < core.W and 0 <= y < core.H and self.stack() is not None:
            p = self.current_flat().getpixel((x, y))
            where = "left band" if x < core.DS_X0 else "right band" if x >= core.DS_X1 else f"DS x {x - core.DS_X0}"
            colour = core.hex_color(p) if p[3] else "transparent"
            self.status(f"x {x}, y {y} ({where})   {colour}")

    def show_cursor(self, x: int, y: int):
        """The brush's outline under the mouse."""
        self.canvas.delete("cursor")
        if self.tool.get() not in ("pencil", "eraser"):
            return
        z = self.zoom.get()
        n = self.brush_size.get()
        off = (n - 1) // 2 if n > 2 else 0
        x0, y0 = (x - off) * z, (y - off) * z
        shape = self.canvas.create_oval if n > 2 else self.canvas.create_rectangle
        shape(x0, y0, x0 + n * z, y0 + n * z, outline="#ffffff", tags="cursor")
        shape(x0 - 1, y0 - 1, x0 + n * z + 1, y0 + n * z + 1, outline="#000000", tags="cursor")

    def on_right_click(self, event):
        x, y = self.to_pixel(event)
        self.pick_at(x, y)

    @staticmethod
    def _wheel(event, delta: int):
        event.delta = delta
        return event

    def on_wheel(self, event):
        self.canvas.yview_scroll(-1 if event.delta > 0 else 1, "units")

    def on_hwheel(self, event):
        self.canvas.xview_scroll(-1 if event.delta > 0 else 1, "units")

    def on_zoom_wheel(self, event):
        """Ctrl or Alt + the wheel: zoom, the pixel under the mouse staying
        under it."""
        self.set_zoom(self.zoom.get() + (1 if event.delta > 0 else -1), at=(event.x, event.y))

    # (the picture can be dragged anywhere: the scrolling region goes well
    # past each of its edges)
    MARGIN = 2400

    def _region(self) -> tuple[int, int, int, int]:
        z = self.zoom.get()
        m = self.MARGIN
        return (-m, -m, core.W * z + m, core.H * z + m)

    def _view_at(self, left: float, top: float):
        """The view's top-left at canvas coordinates left, top."""
        x0, y0, x1, y1 = self._region()
        self.canvas.xview_moveto((left - x0) / (x1 - x0))
        self.canvas.yview_moveto((top - y0) / (y1 - y0))

    def centre_view(self):
        """The screen in the middle of the window."""
        z = self.zoom.get()
        self.canvas.configure(scrollregion=self._region())
        self._view_at((core.W * z - self.canvas.winfo_width()) / 2, (core.H * z - self.canvas.winfo_height()) / 2)
        self.centred = True

    def set_zoom(self, z: int, at: tuple[int, int] | None = None):
        """Zoom z (1-12), about the window's point at (the mouse; its middle
        if none)."""
        old = self._zoom_was
        new = max(1, min(12, z))
        if at is None:
            at = (self.canvas.winfo_width() / 2, self.canvas.winfo_height() / 2)
        # the picture's point under at, before
        px = self.canvas.canvasx(at[0]) / old
        py = self.canvas.canvasy(at[1]) / old
        self.zoom.set(new)
        self._zoom_was = new
        self.settings["zoom"] = new
        save_settings(self.settings)
        self.canvas.configure(scrollregion=self._region())
        self._view_at(px * new - at[0], py * new - at[1])
        self.redraw()

    def on_pan_start(self, event):
        self.panning = True
        self.canvas.scan_mark(event.x, event.y)
        self.canvas.configure(cursor="fleur")

    def on_pan(self, event):
        if self.panning:
            self.canvas.scan_dragto(event.x, event.y, gain=1)

    def on_pan_end(self, event):
        self.panning = False
        self.on_tool()

    def set_size(self, n: int):
        n = max(1, min(32, int(n)))
        if self.brush_size.get() != n:
            self.brush_size.set(n)
        self.size_label.configure(text=f"{n} px")
        self.settings["brush"] = n
        if self.mouse is not None:
            self.show_cursor(*self.mouse)

    def set_tool(self, tool: str):
        self.tool.set(tool)
        self.on_tool()

    def on_tool(self):
        if self.tool.get() not in ("select", "move"):
            self.commit()
        self.canvas.configure(cursor="fleur" if self.tool.get() == "move" else "crosshair")
        self.redraw()

    def pick_at(self, x: int, y: int):
        if not (0 <= x < core.W and 0 <= y < core.H) or self.stack() is None:
            return
        p = self.current_flat().getpixel((x, y))
        if p[3] < 128:
            p = self.composite(faint=False).getpixel((x, y))
        self.set_color(core.snap(p))
        if self.tool.get() == "pick":
            self.tool.set("pencil")
            self.on_tool()

    # ---------------------------------------------------- floating pieces
    def sync_selection(self):
        """The selection round the floating piece."""
        ax, ay = self.float["at"]
        w, h = self.float["image"].size
        self.selection = (ax, ay, ax + w, ay + h)

    def lift(self, copy: bool = False):
        """The selection's pixels (those that may change) off the drawing
        layer into a floating piece: cut, or (copy) copied."""
        p = self.paint()
        x0, y0, x1, y1 = self.selection
        x0, y0, x1, y1 = max(0, x0), max(0, y0), min(core.W, x1), min(core.H, y1)
        if x0 >= x1 or y0 >= y1:
            return
        self.push_undo()
        view = p.view()
        piece = Image.new("RGBA", (x1 - x0, y1 - y0), (0, 0, 0, 0))
        src, dst = view.load(), piece.load()
        locked = 0
        for yy in range(y0, y1):
            for xx in range(x0, x1):
                if self.allowed(xx, yy):
                    dst[xx - x0, yy - y0] = src[xx, yy]
                    if not copy:
                        src[xx, yy] = (0, 0, 0, 0)
                else:
                    locked += 1
        if not copy:
            p.set_view(view)
        self.locked_lift = bool(locked)
        if locked:
            self.status("Only the pixels in the bands move: the DS's picture is locked (untick 'Lock the "
                        "DS's picture' to move those too).")
        self.float = {"image": piece, "at": (x0, y0), "from": (x0, y0, x1, y1), "locked": self.locked_lift}
        self.sync_selection()
        self.dirty.add(self.lid)

    def commit(self):
        """The floating piece put down where it is (its solid pixels, where
        they may go); the selection stays round it."""
        if self.float is None:
            return
        f, self.float = self.float, None
        self.drag = None
        if self.paint() is not None:
            self.put_down(f["image"], *f["at"])
            self.dirty.add(self.lid)
        if self.selection is not None:
            x0, y0, x1, y1 = self.selection
            self.selection = (max(0, x0), max(0, y0), min(core.W, x1), min(core.H, y1))
            if self.selection[0] >= self.selection[2] or self.selection[1] >= self.selection[3]:
                self.selection = None
        self.redraw()

    def cancel_float(self):
        """The floating piece taken back: the drawing layer as it was before
        it was lifted (or pasted); the selection where it was lifted from."""
        self.selection = self.float.get("from") if self.float is not None else None
        self.float = None
        self.drag = None
        if self.undo:
            self._swap(self.undo, self.redo)
            self.redo.pop()
        self.redraw()

    def put_down(self, img: Image.Image, x: int, y: int):
        """img's solid pixels onto the drawing layer at x, y (where they may go)."""
        p = self.paint()
        src = img.load()
        for fy in range(img.height):
            for fx in range(img.width):
                c = src[fx, fy]
                if c[3] >= 128 and self.allowed(x + fx, y + fy):
                    p.plot(x + fx, y + fy, c)

    def nudge(self, dx: int, dy: int):
        """The arrow keys, with Select or Move: the selection's pixels (or
        the floating piece), or with Move and nothing selected the drawing
        layer."""
        p = self.paint()
        tool = self.tool.get()
        if tool not in ("select", "move") or p is None or self.drag is not None:
            return
        if self.float is None and self.selection is not None:
            self.lift()
        if self.float is not None:
            ax, ay = self.float["at"]
            self.float["at"] = (ax + dx, ay + dy)
            self.sync_selection()
            self.redraw()
        elif tool == "move":
            self.push_undo()
            p.x += dx
            p.y += dy
            self.dirty.add(self.lid)
            self.redraw()

    # --------------------------------------------------------- selection
    def select_all(self):
        self.commit()
        self.selection = (0, 0, core.W, core.H)
        self.redraw()

    def deselect(self):
        self.commit()
        self.selection = None
        self.redraw()

    def copy(self):
        p = self.paint()
        if p is None:
            return
        if self.float is not None:
            self.clipboard = self.float["image"].copy()
            self.clip_at = self.float["at"]
        elif self.selection is not None:
            sel = self.selection
            self.clipboard = p.view().crop(sel)
            self.clip_at = sel[:2]
        else:
            self.status("Select something first (Select tool, M).")
            return
        self.status(f"Copied {self.clipboard.width} x {self.clipboard.height} pixels: Ctrl+V pastes them.")

    def cut(self):
        p = self.paint()
        if p is None:
            return
        if self.float is None:
            if self.selection is None:
                self.status("Select something first (Select tool, M).")
                return
            self.lift()
        self.clipboard = self.float["image"].copy()
        self.clip_at = self.float["at"]
        self.float = None
        self.selection = None
        self.status(f"Cut {self.clipboard.width} x {self.clipboard.height} pixels: Ctrl+V pastes them.")
        self.redraw()

    def paste(self):
        if self.clipboard is None or self.paint() is None:
            return
        self.commit()
        self.push_undo()
        at = self.selection[:2] if self.selection else getattr(self, "clip_at", (0, 0))
        self.float = {"image": self.clipboard.copy(), "at": at}
        self.sync_selection()
        if self.tool.get() not in ("select", "move"):
            self.tool.set("select")
            self.on_tool()
        self.dirty.add(self.lid)
        self.status("Pasted: drag it into place (arrow keys nudge, H flips it); Enter or a click outside puts "
                    "it down, Esc takes it back.")
        self.redraw()

    def paste_as_layer(self):
        if self.clipboard is None or self.stack() is None:
            return
        self.commit()
        x, y = self.selection[:2] if self.selection else getattr(self, "clip_at", (0, 0))
        self.add_paint(core.Paint(self.new_name("Pasted"), self.clipboard.copy(), x, y))
        self.selection = None
        self.set_tool("move")
        self.status("Pasted as a new drawing layer: drag it into place with the Move tool.")

    def flip_floating(self):
        """H: the floating piece (or the selection's pixels) flipped."""
        if self.float is None and self.selection is not None and self.paint() is not None:
            self.lift()
        if self.float is not None:
            self.float["image"] = self.float["image"].transpose(Image.FLIP_LEFT_RIGHT)
            self.redraw()

    def ensure_float(self) -> bool:
        """A floating piece to change: the selection's pixels lifted, if
        they aren't yet."""
        if self.float is None and self.selection is not None and self.paint() is not None:
            self.lift()
        if self.float is None:
            self.status("Select something first (Select tool, M).")
            return False
        return True

    def transform_dialog(self):
        """Ctrl+T: scale and rotate the selection."""
        if not self.ensure_float():
            return
        if self.tool.get() not in ("select", "move"):
            self.tool.set("select")
            self.on_tool()
        self.wait_window(TransformDialog(self))

    def rotate_right(self):
        if self.ensure_float():
            self.turn_float(Image.ROTATE_270)

    def rotate_left(self):
        if self.ensure_float():
            self.turn_float(Image.ROTATE_90)

    def flip_vertical(self):
        if self.ensure_float():
            self.turn_float(Image.FLIP_TOP_BOTTOM)

    def turn_float(self, how):
        """The floating piece turned or flipped about its middle."""
        img = self.float["image"]
        ax, ay = self.float["at"]
        cx, cy = ax + img.width / 2, ay + img.height / 2
        img = img.transpose(how)
        self.float["image"] = img
        self.float["at"] = (round(cx - img.width / 2), round(cy - img.height / 2))
        self.sync_selection()
        self.redraw()

    def clear_selection(self):
        """Delete: the selection's pixels (or the floating piece) gone."""
        p = self.paint()
        if p is None:
            return
        if self.float is not None:
            self.float = None
            self.selection = None
            self.dirty.add(self.lid)
            self.redraw()
            return
        if self.selection is None:
            return
        self.push_undo()
        view = p.view()
        x0, y0, x1, y1 = self.selection
        for y in range(y0, y1):
            for x in range(x0, x1):
                if self.allowed(x, y):
                    view.putpixel((x, y), (0, 0, 0, 0))
        p.set_view(view)
        self.dirty.add(self.lid)
        self.redraw()

    def cancel(self):
        """Esc: a floating piece taken back, else the selection dropped."""
        if self.float is not None:
            self.cancel_float()
            return
        self.selection = None
        self.redraw()

    # ------------------------------------------------------------ colours
    def set_color(self, color):
        self.color = core.snap(color)
        if self.color in self.recent:
            self.recent.remove(self.color)
        self.recent.insert(0, self.color)
        del self.recent[RECENT:]
        self.settings["recent"] = [core.hex_color(c) for c in self.recent]
        save_settings(self.settings)
        self.update_color_box()
        if self.tool.get() in ("eraser", "pick"):
            self.tool.set("pencil")
            self.on_tool()

    def choose_color(self):
        picker = ColorPicker(self, self.color, self.recent)
        self.wait_window(picker)
        if picker.result:
            self.set_color(picker.result)

    def update_color_box(self):
        self.color_box.delete("all")
        self.color_box.create_rectangle(0, 0, 48, 28, fill=core.hex_color(self.color), outline="")
        self.recent_box.delete("all")
        for i, c in enumerate(self.recent):
            x, y = (i % 8) * 29, (i // 8) * 15
            self.recent_box.create_rectangle(x, y, x + 27, y + 13, fill=core.hex_color(c), outline="#555")

    def pick_recent(self, event):
        i = (event.y // 15) * 8 + event.x // 29
        if 0 <= i < len(self.recent):
            self.set_color(self.recent[i])

    def update_swatches(self):
        self.swatches.delete("all")
        self.swatch_colors = core.palette(self.cap.screen(), 64) if self.cap else []
        for i, c in enumerate(self.swatch_colors):
            x, y = (i % 8) * 29, (i // 8) * 18
            self.swatches.create_rectangle(x, y, x + 27, y + 16, fill=core.hex_color(c), outline="#555")

    def pick_swatch(self, event):
        i = (event.y // 18) * 8 + event.x // 29
        if 0 <= i < len(getattr(self, "swatch_colors", [])):
            self.set_color(self.swatch_colors[i])

    # ------------------------------------------------------------ bands
    def fill_bands(self, how: str):
        p = self.paint()
        if p is None:
            return
        self.push_undo()
        # from what shows of this layer (all its drawing layers), into the one being drawn on
        p.set_view(core.shift_band_fill(p.view(), self.current_flat(), how, self.side.get()))
        if not p.visible:
            p.visible = True
            self.update_paint_list()
        self.dirty.add(self.lid)
        self.redraw()

    # ------------------------------------------------------------ files
    def picture(self, lid: str) -> Image.Image:
        """What the game is to show for a layer: its drawing layers flattened
        (the DS's part as locked, if it is)."""
        flat = core.flatten(self.stacks[lid])
        if self.is_picture(lid):
            return flat
        return core.lock_middle(flat, self.middle[lid]) if self.lock_middle.get() else flat

    def save_layer(self, lid: str) -> Path:
        layer = self.cap.layer(lid)
        if layer.kind == "picture":
            path, lost = core.save_picture(self.art_dir, layer, self.stacks[lid])
            core.save_project(self.art_dir, layer.key, self.stacks[lid], self.active[lid], layer.hofs, layer.vofs,
                              path)
            self.refs[lid] = core.ArtRef(path, layer.hofs, layer.vofs, True, False)
            self.dirty.discard(lid)
            if lost:
                self.after(10, lambda: self.status(
                    f"Saved {path.name}; what's drawn left of or above its top-left corner was left out "
                    "(the game draws the picture from that corner)."))
            return path
        old = self.refs.get(lid)
        path = core.save_art(self.art_dir, layer, self.picture(lid), self.originals[lid],
                             loop=self.loops.get(lid, False), replace=old.path if old else None)
        core.save_project(self.art_dir, layer.key, self.stacks[lid], self.active[lid], layer.hofs, layer.vofs, path)
        parsed = core.parse_art_name(path.name)
        self.refs[lid] = core.ArtRef(path, layer.hofs, layer.vofs, parsed[3], parsed[4])
        self.dirty.discard(lid)
        return path

    def save(self):
        if self.stack() is None or self.art_dir is None:
            return
        self.commit()
        path = self.save_layer(self.lid)
        if self.is_picture():
            with Image.open(path) as im:
                size = im.size
            self.status(f"Saved {path.name} ({size[0]} x {size[1]}). In the game: L + R + A takes it in.")
        else:
            self.status(f"Saved {path.name}" + ("  (the whole layer)" if path.stem.endswith("_full") else
                                                "  (the side bands)") + ". In the game: L + R + A takes it in.")
        self.refresh_layer_labels()

    def save_all(self):
        if self.cap is None or self.art_dir is None:
            return
        self.commit()
        n = len(self.dirty)
        for lid in sorted(self.dirty):
            self.save_layer(lid)
        self.status(f"Saved {n} layers.")
        self.refresh_layer_labels()

    def refresh_layer_labels(self):
        """(after saving) the lists again, the same layer chosen; the
        drawing kept as it is."""
        if self.cap is None:
            return
        for w in self.layer_frame.winfo_children():
            w.destroy()
        for l in reversed(core.draw_order(self.cap.layers)):
            saved = self.refs.get(l.id)
            ttk.Radiobutton(self.layer_frame, text=l.label + ("  - drawn" if saved and l.kind != "picture" else ""),
                            variable=self.layer_var, value=l.id, command=self.select_layer).pack(anchor="w")
        index = self.cap_list.curselection()
        if index:
            made = sum(1 for l in self.cap.layers if self.refs.get(l.id))
            self.cap_list.delete(index[0])
            self.cap_list.insert(index[0], f"{self.cap.name}   {len(self.cap.layers)} layers" +
                                 (f", {made} drawn" if made else ""))
            self.cap_list.selection_set(index[0])
        self.update_layer_info()
        self.redraw()

    def revert(self):
        if self.stack() is None:
            return
        self.push_undo()
        if self.is_picture():
            # (its file as it is: the capture has only what was on the canvas)
            self.stacks[self.lid] = [core.picture_paint(self.art_dir, self.cap, self.layer())]
            self.active[self.lid] = 0
            self.changed()
            return
        self.stacks[self.lid] = [core.Paint("Layer 1", self.originals[self.lid].copy())]
        self.active[self.lid] = 0
        self.middle[self.lid] = self.originals[self.lid]
        self.changed()

    def delete_saved(self):
        layer = self.layer()
        if layer is None or self.art_dir is None:
            return
        ref = self.refs.get(self.lid)
        if ref is None:
            self.status("Nothing saved for this layer.")
            return
        question = (f"Delete {ref.path.name}? The game stops drawing that picture." if layer.kind == "picture" else
                    f"Delete the saved picture for {layer.label}? The game goes back to filling its bands by "
                    "itself.")
        if messagebox.askyesno(APP, question):
            core.delete_art(self.art_dir, layer)
            core.delete_project(self.art_dir, layer.key, ref.hofs, ref.vofs)
            self.refs[self.lid] = None
            self.stacks[self.lid] = [core.Paint("Layer 1", self.originals[self.lid].copy())]
            self.active[self.lid] = 0
            self.middle[self.lid] = self.originals[self.lid]
            self.dirty.discard(self.lid)
            self.update_paint_list()
            self.refresh_layer_labels()

    def export_png(self, path: str | None = None):
        """This layer as a PNG to work on elsewhere (Photoshop...): the whole
        canvas (320 x 192, the bands at the sides), or a layout's picture at
        its own size. Import it back (File > Import) to put it in."""
        if self.stack() is None:
            return
        self.commit()
        layer = self.layer()
        picture = layer is not None and layer.kind == "picture"
        if path is None:
            path = filedialog.asksaveasfilename(defaultextension=".png", filetypes=[("PNG", "*.png")],
                                                initialfile=f"{layer.key if picture else self.lid}.png")
        if path:
            if picture:
                box = (layer.hofs, layer.vofs, layer.hofs + layer.w, layer.vofs + layer.h)
                drawn = core.stack_bbox(self.stack())
                if drawn:
                    box = (box[0], box[1], max(box[2], drawn[2]), max(box[3], drawn[3]))
                img = core.flatten_box(self.stack(), box)
            else:
                img = self.picture(self.lid)
            img.save(path)
            self.status(f"Exported {path} ({img.width} x {img.height}, all the drawing layers in one). "
                        "Edit it, then File > Import to bring it back.")

    def export_paint(self):
        """The drawing layer being drawn on, as it is (its own size)."""
        p = self.paint()
        if p is None:
            return
        self.commit()
        path = filedialog.asksaveasfilename(defaultextension=".png", filetypes=[("PNG", "*.png")],
                                            initialfile=f"{p.name}.png")
        if path:
            p.image.save(path)
            self.status(f"Exported {path} ({p.image.width} x {p.image.height}, at x {p.x}, y {p.y} on the canvas)")

    def export_screen(self):
        """The whole screen as it shows here (every layer)."""
        if self.cap is None:
            return
        self.commit()
        path = filedialog.asksaveasfilename(defaultextension=".png", filetypes=[("PNG", "*.png")],
                                            initialfile=f"screen_{self.cap.name}.png")
        if path:
            self.composite(faint=False).save(path)
            self.status(f"Exported {path} (320 x 192, every layer)")

    def import_png(self, path: str | None = None):
        """A picture made elsewhere (Photoshop: PSD or PNG...) as a new
        drawing layer of this layer, to put where it goes: a whole-canvas
        one (320 x 192) where it is; a layout's picture's own size at its
        place; anything else at the top-left of the view, the Move tool
        chosen to drag it."""
        if self.stack() is None:
            return
        if path is None:
            path = filedialog.askopenfilename(filetypes=[("Pictures", "*.png *.psd *.gif *.bmp *.jpg *.jpeg *.tga"),
                                                         ("All", "*.*")])
        if not path:
            return
        try:
            img = core.load_rgba(path)
        except (OSError, ValueError) as e:
            messagebox.showerror(APP, f"Can't read {path}:\n{e}")
            return
        layer = self.layer()
        x = y = 0
        if layer is not None and layer.kind == "picture" and img.width != core.W:
            x, y = layer.hofs, layer.vofs
        elif img.size != (core.W, core.H):
            z = self.zoom.get()
            x = max(0, min(core.W - 1, int(self.canvas.canvasx(0) // z)))
            y = max(0, min(core.H - 1, int(self.canvas.canvasy(0) // z)))
        self.add_paint(core.Paint(Path(path).stem, img, x, y))
        if (x, y) != (0, 0) or img.size != (core.W, core.H):
            self.set_tool("move")
            self.status(f"Imported {Path(path).name} ({img.width} x {img.height}) at x {x}, y {y}: drag it into "
                        "place with the Move tool; Merge (Ctrl+E) puts it into the layer under it.")
        else:
            self.status(f"Imported {Path(path).name} as a new drawing layer.")

    def open_outside(self):
        layer = self.layer()
        if layer is None or self.art_dir is None:
            return
        if self.lid in self.dirty or self.refs.get(self.lid) is None:
            self.save()
        ref = self.refs.get(self.lid)
        saved = ref.path if ref else None
        if saved:
            open_externally(saved)
            self.status(f"Opened {saved.name}: save it there, then File > Reload saved picture.")

    def reload_saved(self):
        layer = self.layer()
        if layer is None or self.art_dir is None:
            return
        ref = self.refs.get(self.lid)
        if ref is not None and ref.path.exists():
            self.push_undo()
            if layer.kind == "picture":
                self.stacks[self.lid] = [core.picture_paint(self.art_dir, self.cap, layer)]
            else:
                self.stacks[self.lid] = [core.Paint("Layer 1", core.load_rgba(ref.path), ref.dx, ref.dy)]
            self.active[self.lid] = 0
            self.changed()

    def confirm_discard(self) -> bool:
        self.commit()
        if not self.dirty:
            return True
        answer = messagebox.askyesnocancel(APP, "Save the layers you changed first?")
        if answer is None:
            return False
        if answer:
            self.save_all()
        self.dirty.clear()
        return True

    def on_close(self):
        save_settings(self.settings)
        if self.confirm_discard():
            self.destroy()

    # ---------------------------------------------------------- showing
    def current_flat(self) -> Image.Image:
        """The layer being drawn as it shows now (a floating piece
        included)."""
        img = self.picture(self.lid)
        if self.float is not None:
            img = img.copy()
            layer = Image.new("RGBA", (core.W, core.H), (0, 0, 0, 0))
            layer.paste(self.float["image"], self.float["at"])
            img.alpha_composite(layer)
        return img

    def composite(self, faint: bool = True) -> Image.Image:
        images = dict(self.flat)
        images[self.lid] = self.current_flat()
        return core.composite(self.cap, images, None if self.show_others.get() else self.lid,
                              self.lid if faint and self.see_through.get() else None)

    def preview(self):
        if self.cap is None:
            return
        win = tk.Toplevel(self)
        win.title("3DS preview (the upper screen, twice its size)")
        img = core.preview_3ds(self.composite(faint=False), 2)
        photo = ImageTk.PhotoImage(img)
        label = ttk.Label(win, image=photo)
        label.image = photo
        label.pack()

    def redraw(self):
        self.canvas.delete("all")
        if self.cap is None or self.stack() is None:
            return
        z = self.zoom.get()
        img = self.composite()
        big = img.resize((core.W * z, core.H * z), Image.NEAREST)
        draw = ImageDraw.Draw(big)
        if self.lock_middle.get():
            # the locked middle: a light veil
            veil = Image.new("RGBA", (256 * z, core.H * z), (0, 0, 0, 60))
            big.alpha_composite(veil, dest=(core.DS_X0 * z, 0))
        if self.show_grid.get() and z >= 3:
            for x in range(0, core.W * z, z):
                draw.line([(x, 0), (x, core.H * z)], fill=(0, 0, 0, 40))
            for y in range(0, core.H * z, z):
                draw.line([(0, y), (core.W * z, y)], fill=(0, 0, 0, 40))
        self.photo = ImageTk.PhotoImage(big)
        self.canvas.create_image(0, 0, anchor="nw", image=self.photo)
        # the DS's picture's edges
        for x in (core.DS_X0, core.DS_X1):
            self.canvas.create_line(x * z, 0, x * z, core.H * z, fill="#ffcc00", dash=(4, 3))
        if self.selection:
            # (a floating piece: orange, not put down yet)
            x0, y0, x1, y1 = self.selection
            color = "#ff9900" if self.float is not None else "#00ffff"
            self.canvas.create_rectangle(x0 * z - 1, y0 * z - 1, x1 * z, y1 * z, outline="#000000")
            self.canvas.create_rectangle(x0 * z - 1, y0 * z - 1, x1 * z, y1 * z, outline=color, dash=(4, 3))
        p = self.paint()
        if self.tool.get() == "move" and p is not None and self.selection is None:
            # the drawing layer's extent
            self.canvas.create_rectangle(p.x * z, p.y * z, (p.x + p.image.width) * z, (p.y + p.image.height) * z,
                                         outline="#ff66ff", dash=(2, 4))
        self.canvas.configure(scrollregion=self._region())
        if not self.centred and self.canvas.winfo_width() > 1:
            self.centre_view()
        if self.mouse is not None:
            self.show_cursor(*self.mouse)
        marks = "*" if self.lid in self.dirty else ""
        layer = self.layer()
        name = layer.label.split("  (")[0] if layer else ""
        self.title(f"{APP} - {self.cap.name} {name}{marks}" + (f"  [{p.name}]" if p else ""))

    def status(self, text: str):
        self.status_var.set(text)

    def show_help(self):
        messagebox.showinfo(APP, (
            "1. In the game, on the screen to draw, press L + R + X: it saves a capture to "
            "3ds/pokeplatinum/wide/captures on the SD card (Azahar: its sdmc folder).\n\n"
            "2. Open that folder here (the SD card or sdmc folder is fine) and pick the capture.\n\n"
            "3. Pick one of the game's layers (the background is usually the backmost; sprite "
            "backgrounds are the 'Sprites' layers) and draw the side bands: the 32 pixels left and right "
            "of the yellow lines. Start from Mirror / Extend edge / Repeat if you like, then fix it by "
            "hand. Unlock the middle to change the DS's picture too.\n\n"
            "Drawing layers: add, duplicate, hide and reorder layers of your own (Layer menu); saving "
            "puts them together into the one picture the game reads, and keeps them to open again.\n\n"
            "Select (M): drag a rectangle, then drag inside it to cut and move those pixels (hold Alt "
            "to move a copy). They float until you press Enter or click outside; Esc puts them back. "
            "Ctrl+X / Ctrl+C / Ctrl+V cut, copy and paste; the paste floats too, ready to drag. Arrow "
            "keys nudge, H / Shift+H flip, R / Shift+R turn by 90 degrees, Ctrl+T scales and rotates.\n"
            "Move (V): drag the drawing layer (nothing selected), or the selection.\n"
            "[ and ]: brush size. Right-click: pick a colour. Shift + click: a straight line from where the last "
            "stroke ended.\n"
            "Alt (or Ctrl) + wheel: zoom where the mouse is. Middle button (with or without Alt): drag the "
            "picture round. Home: put it back in the middle.\n"
            "Ctrl+Z / Ctrl+Y (or Ctrl+Shift+Z): undo / redo.\n\n"
            "Pictures of a screen's own (the party's panel, the boxes' divider...) are layers of the capture "
            "too ('Picture: ...'): draw on them and save; the game reads them back from their files.\n"
            "File > Import / Export: a picture to or from another program (PNG; Photoshop's PSD to import).\n\n"
            "4. Save. In the game, L + R + A takes the new pictures in (or restart it)."))


def self_test(log_path: str | None = None) -> int:
    """(CI) Open a made-up capture; fill a layer's bands, draw on a new
    drawing layer and move it, save: 0 if the picture is where the game
    reads it and the drawing layers open again. Each check's result goes
    to wideart_selftest.log (the exe has no console to print to)."""
    import gc
    import shutil
    import tempfile
    import traceback

    log = Path(log_path or os.environ.get("WIDEART_SELFTEST_LOG") or "wideart_selftest.log")
    results: list[str] = []
    failed: list[str] = []

    def check(name: str, cond) -> bool:
        good = bool(cond)
        results.append(("ok    " if good else "FAILED ") + name)
        if not good:
            failed.append(name)
        return good

    tmp = tempfile.mkdtemp(prefix="wideart_selftest_")
    app = None
    try:
        global SETTINGS
        art = Path(tmp) / "wide"
        core.make_sample_capture(art, sprites=True, picture=True)
        SETTINGS = Path(tmp) / "settings.json"
        app = Editor(str(art))
        app.withdraw()
        app.update()
        if check("the capture opens, on BG3, with the sprites' layer",
                 app.cap is not None and app.lid == "bg3" and app.cap.layer("obj102") is not None):
            app.fill_bands("mirror")
            app.new_layer()
            app.set_size(3)
            app.stamp(10, 10, (255, 0, 0, 255))
            app.paint().x += 2            # (the Move tool's doing)
            app.save()
            saved = core.existing_art(art, "6d5a3d55615351bf")
            check("saved where the game reads it", saved is not None and saved.name == "6d5a3d55615351bf_8_300.png")
            pic = core.load_rgba(saved)
            check("the band mirrored", pic.getpixel((31, 5))[0] == core.snap((32, 7, 9))[0])
            check("the drawing layer moved 2 right",
                  pic.getpixel((12, 10))[:3] == (255, 0, 0) and pic.getpixel((13, 11))[:3] == (255, 0, 0))
            check("the middle kept", pic.getpixel((100, 50)) == core.snap(app.originals["bg3"].getpixel((100, 50))))
            project = core.load_project(art, "6d5a3d55615351bf", 8, 300)
            check("the drawing layers saved",
                  project is not None and len(project[0]) == 2 and project[0][1].x == 2)
            # the sprites' layer: its own picture
            app.layer_var.set("obj102")
            app.select_layer()
            app.fill_bands("extend")
            app.save()
            check("the sprites' layer saved", core.existing_art(art, "10d372806e45f5c2") is not None)
            # the Move tool: the red dot's pixels dragged 10 down
            app.layer_var.set("bg3")
            app.select_layer()
            app.set_tool("select")
            app.press(11, 9)                 # a selection round it
            app.drag_to(13, 11)
            app.release()
            check("a selection", app.selection == (11, 9, 14, 12))
            app.press(12, 10)                # dragged from inside it: cut and moved
            app.drag_to(12, 20)
            app.release()
            app.commit()
            app.save()
            pic = core.load_rgba(core.existing_art(art, "6d5a3d55615351bf"))
            check("the selection moved",
                  pic.getpixel((12, 20))[:3] == (255, 0, 0) and pic.getpixel((12, 10))[:3] != (255, 0, 0))
            # Alt: a copy moved; copy and paste
            app.press(12, 20, alt=True)
            app.drag_to(12, 40)
            app.release()
            app.commit()
            app.selection = (11, 39, 14, 42)
            app.copy()
            app.paste()
            app.press(12, 40)
            app.drag_to(12, 60)
            app.release()
            app.commit()
            app.save()
            pic = core.load_rgba(core.existing_art(art, "6d5a3d55615351bf"))
            check("Alt + drag and copy / paste copied it",
                  all(pic.getpixel((12, y))[:3] == (255, 0, 0) for y in (20, 40, 60)))
            # Shift + click: a straight line from where the last stroke ended; undo, redo
            app.set_tool("pencil")
            app.set_size(1)
            app.color = (255, 0, 0, 255)
            app.press(2, 100)                # (in the left band: the middle is locked)
            app.release()
            app.press(12, 100, shift=True)
            app.release()
            line = lambda: all(app.paint().view().getpixel((x, 100))[:3] == (255, 0, 0) for x in range(2, 13))
            check("Shift + click drew a line", line())
            app.do_undo()
            check("undo took the line away", not line())
            app.do_redo()
            check("redo put it back", line())
            # a layout's picture: a layer of the capture, saved back to its file from its corner (what's past
            # the canvas's edge kept), bigger when drawn past its right edge
            check("the capture has the layout's picture", app.cap.layer("pic_party_ball") is not None)
            app.layer_var.set("pic_party_ball")
            app.select_layer()
            check("the picture isn't locked", app.allowed(100, 100))
            app.stamp(5, 12, (0, 0, 255, 255))         # (its x 9)
            app.stamp(8, 12, (0, 0, 255, 255))         # (past its right edge: x 12)
            app.save()
            ball = core.load_rgba(art / "party_ball.png")
            check("the picture saved, grown to the right", ball.size == (13, 6))
            check("what's off the canvas kept", ball.getpixel((0, 0))[:3] == core.snap((200, 40, 60))[:3])
            check("the drawing in it",
                  ball.getpixel((9, 2))[:3] == (0, 0, 255) and ball.getpixel((12, 2))[:3] == (0, 0, 255))
            check("the rest of it kept", ball.getpixel((7, 4))[:3] == core.snap((240, 240, 240))[:3])
            # export it and import it back
            out = Path(tmp) / "exported.png"
            app.export_png(str(out))
            check("exported at its own size", core.load_rgba(out).size == (13, 6))
            n = len(app.stack())
            app.import_png(str(out))
            check("imported back where the picture is",
                  len(app.stack()) == n + 1 and (app.paint().x, app.paint().y) == (-4, 10))
            # zooming about the mouse: the pixel under it stays
            app.set_zoom(3)
            app.update()
            before = app.canvas.canvasx(50) / 3
            app.set_zoom(6, at=(50, 50))
            after = app.canvas.canvasx(50) / 6
            check(f"zoomed about the mouse (pixel {before:.2f} -> {after:.2f})", abs(after - before) < 1)
    except Exception:
        failed.append("error")
        results.append("ERROR\n" + traceback.format_exc())
    finally:
        if app is not None:
            try:
                app.destroy()
            except Exception:
                pass
        app = None
        gc.collect()
        try:
            shutil.rmtree(tmp)
            check("no file left open", True)
        except OSError as e:
            check(f"no file left open ({e})", False)
        try:
            log.write_text("\n".join(results) + "\n", encoding="utf-8")
        except OSError:
            pass
    return 0 if not failed else 1


def main():
    if "--selftest" in sys.argv:
        sys.exit(self_test())
    folder = sys.argv[1] if len(sys.argv) > 1 else None
    app = Editor(folder)
    app.mainloop()


if __name__ == "__main__":
    main()
