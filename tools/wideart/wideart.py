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
        self.grab_set()
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
        self.zoom = tk.IntVar(value=int(self.settings.get("zoom", 3)))
        self.tool = tk.StringVar(value="pencil")
        self.brush_size = tk.IntVar(value=int(self.settings.get("brush", 1)))
        self.lock_middle = tk.BooleanVar(value=True)
        self.show_others = tk.BooleanVar(value=True)
        self.see_through = tk.BooleanVar(value=True)
        self.show_grid = tk.BooleanVar(value=False)
        self.side = tk.StringVar(value="both")
        self.layer_var = tk.StringVar(value="")
        self.selection: tuple[int, int, int, int] | None = None
        self.floating: Image.Image | None = None      # pasted, following the mouse
        self.clipboard: Image.Image | None = None
        self.drag_start: tuple[int, int] | None = None
        self.moving: dict | None = None               # a move under way (the Move tool)
        self.mouse: tuple[int, int] | None = None
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
        filem.add_command(label="Export layer as PNG...", command=self.export_png)
        filem.add_command(label="Import PNG as a new drawing layer...", command=self.import_png)
        filem.add_command(label="Open saved picture in another program", command=self.open_outside)
        filem.add_command(label="Reload saved picture", command=self.reload_saved)
        filem.add_separator()
        filem.add_command(label="Quit", command=self.on_close)
        menu.add_cascade(label="File", menu=filem)
        editm = tk.Menu(menu, tearoff=False)
        editm.add_command(label="Undo", command=self.do_undo, accelerator="Ctrl+Z")
        editm.add_command(label="Redo", command=self.do_redo, accelerator="Ctrl+Y")
        editm.add_separator()
        editm.add_command(label="Copy selection", command=self.copy, accelerator="Ctrl+C")
        editm.add_command(label="Paste", command=self.paste, accelerator="Ctrl+V")
        editm.add_command(label="Paste as a new layer", command=self.paste_as_layer, accelerator="Ctrl+Shift+V")
        editm.add_command(label="Flip pasted (horizontal)", command=self.flip_floating, accelerator="H")
        editm.add_command(label="Clear selection", command=self.clear_selection, accelerator="Delete")
        editm.add_command(label="Select all", command=self.select_all, accelerator="Ctrl+A")
        editm.add_command(label="Deselect", command=self.cancel, accelerator="Esc")
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
        for z in (1, 2, 3, 4, 5, 6):
            viewm.add_radiobutton(label=f"Zoom {z}x", variable=self.zoom, value=z, command=self.redraw)
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
        self.canvas.bind("<Control-MouseWheel>", self.on_zoom_wheel)

        self.status_var = tk.StringVar()
        ttk.Label(self, textvariable=self.status_var, anchor="w", relief="sunken").pack(fill="x", side="bottom")
        self.update_color_box()
        self.set_size(self.brush_size.get())

    def _bind_keys(self):
        for seq, fn in (("<Control-o>", self.choose_folder), ("<F5>", self.refresh), ("<Control-s>", self.save),
                        ("<Control-S>", self.save_all), ("<Control-z>", self.do_undo), ("<Control-y>", self.do_redo),
                        ("<Control-c>", self.copy), ("<Control-v>", self.paste), ("<Control-V>", self.paste_as_layer),
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
            made = sum(1 for l in cap.layers if core.existing_art(self.art_dir, l.key))
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
        the picture saved for it (made elsewhere), else of the capture."""
        if self.art_dir is not None:
            project = core.load_project(self.art_dir, layer.key)
            if project is not None:
                return project
            saved = core.existing_art(self.art_dir, layer.key)
            if saved:
                return [core.Paint("Layer 1", core.load_rgba(saved))], 0
        return [core.Paint("Layer 1", self.originals[layer.id].copy())], 0

    def select_capture(self, force: bool = False):
        sel = self.cap_list.curselection()
        if not sel:
            return
        cap = self.captures[sel[0]]
        if self.cap is not None and cap.path == self.cap.path and not force:
            return
        if not self.confirm_discard():
            return
        previous = self.lid if self.cap is not None and cap.path == self.cap.path else None
        self.cap = cap
        self.originals = {l.id: cap.layer_image(l) for l in cap.layers}
        self.stacks, self.active, self.middle, self.flat = {}, {}, {}, {}
        for l in cap.layers:
            self.stacks[l.id], self.active[l.id] = self.load_stack(l)
            self.flat[l.id] = core.flatten(self.stacks[l.id])
            self.middle[l.id] = self.flat[l.id]
        self.dirty = set()
        self.undo, self.redo = [], []
        self.selection = None
        self.floating = None
        self.moving = None
        thumb = cap.screen().resize((160, 96), Image.NEAREST)
        self.thumb_photo = ImageTk.PhotoImage(thumb)
        self.thumb.configure(image=self.thumb_photo)
        for w in self.layer_frame.winfo_children():
            w.destroy()
        for l in reversed(core.draw_order(cap.layers)):
            saved = core.existing_art(self.art_dir, l.key) if self.art_dir else None
            ttk.Radiobutton(self.layer_frame, text=l.label + ("  - drawn" if saved else ""),
                            variable=self.layer_var, value=l.id, command=self.select_layer).pack(anchor="w")
        if cap.layers:
            # the backmost layer: usually the background to draw
            self.layer_var.set(previous if previous in self.stacks else core.draw_order(cap.layers)[0].id)
            self.select_layer()
        self.update_swatches()

    def select_layer(self):
        if self.lid is not None and self.lid in self.stacks:
            self.flat[self.lid] = core.flatten(self.stacks[self.lid])
        self.lid = self.layer_var.get() or None
        self.selection = None
        self.floating = None
        layer = self.layer()
        if layer is not None:
            saved = core.existing_art(self.art_dir, layer.key) if self.art_dir else None
            if saved is not None and saved.stem.endswith("_full"):
                self.lock_middle.set(False)   # (its middle was drawn: don't put the game's back)
            what = "position" if layer.kind == "obj" else "scroll"
            self.layer_info.configure(text=f"{layer.label}\nkey {layer.key}\n{what} {layer.hofs}, {layer.vofs}\n"
                                           + (f"saved: {saved.name}" if saved else "not drawn yet"))
        self.update_paint_list()
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
    def allowed(self, x: int, y: int) -> bool:
        if not (0 <= x < core.W and 0 <= y < core.H):
            return False
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
        self._swap(self.undo, self.redo)

    def do_redo(self):
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

    def on_press(self, event):
        p = self.paint()
        if p is None:
            return
        x, y = self.to_pixel(event)
        tool = self.tool.get()
        if self.floating is not None:
            self.stamp_floating(x, y)
            return
        if not p.visible and tool in ("pencil", "eraser", "fill", "move"):
            self.status("That drawing layer is hidden: show it first (Show/Hide).")
            return
        if tool in ("pencil", "eraser"):
            self.push_undo()
            self.stamp(x, y, (0, 0, 0, 0) if tool == "eraser" else self.color)
            self.drag_start = (x, y)
            self.dirty.add(self.lid)
        elif tool == "fill":
            if self.allowed(x, y):
                self.push_undo()
                p.set_view(core.flood_fill(p.view(), x, y, self.color, self.allowed))
                self.dirty.add(self.lid)
        elif tool == "pick":
            self.pick_at(x, y)
        elif tool == "select":
            self.drag_start = (x, y)
            self.selection = None
        elif tool == "move":
            self.start_move(x, y)
        self.redraw()

    def on_drag(self, event):
        x, y = self.to_pixel(event)
        self.mouse = (x, y)
        tool = self.tool.get()
        if self.moving is not None:
            self.drag_move(x, y)
            return
        if self.drag_start is None:
            return
        if tool in ("pencil", "eraser"):
            self.line(*self.drag_start, x, y, (0, 0, 0, 0) if tool == "eraser" else self.color)
            self.drag_start = (x, y)
        elif tool == "select":
            x0, y0 = self.drag_start
            self.selection = (max(0, min(x0, x)), max(0, min(y0, y)),
                              min(core.W, max(x0, x) + 1), min(core.H, max(y0, y) + 1))
        self.redraw()
        self.show_cursor(x, y)

    def on_release(self, event):
        self.drag_start = None
        if self.moving is not None:
            self.end_move()

    def on_motion(self, event):
        x, y = self.to_pixel(event)
        self.mouse = (x, y)
        if self.floating is not None:
            self.redraw()
        self.show_cursor(x, y)
        if 0 <= x < core.W and 0 <= y < core.H and self.stack() is not None:
            p = self.current_flat().getpixel((x, y))
            where = "left band" if x < core.DS_X0 else "right band" if x >= core.DS_X1 else f"DS x {x - core.DS_X0}"
            colour = core.hex_color(p) if p[3] else "transparent"
            self.status(f"x {x}, y {y} ({where})   {colour}")

    def show_cursor(self, x: int, y: int):
        """The brush's outline under the mouse."""
        self.canvas.delete("cursor")
        if self.tool.get() not in ("pencil", "eraser") or self.floating is not None:
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

    def on_wheel(self, event):
        self.canvas.yview_scroll(-1 if event.delta > 0 else 1, "units")

    def on_zoom_wheel(self, event):
        self.set_zoom(self.zoom.get() + (1 if event.delta > 0 else -1))

    def set_zoom(self, z: int):
        self.zoom.set(max(1, min(8, z)))
        self.settings["zoom"] = self.zoom.get()
        save_settings(self.settings)
        self.redraw()

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

    # -------------------------------------------------------------- move
    def start_move(self, x: int, y: int):
        p = self.paint()
        self.push_undo()
        sel = self.selection
        if sel is not None and sel[0] <= x < sel[2] and sel[1] <= y < sel[3]:
            # lift the selected pixels (those that may change) off the layer
            view = p.view()
            lifted = Image.new("RGBA", (sel[2] - sel[0], sel[3] - sel[1]), (0, 0, 0, 0))
            src, dst = view.load(), lifted.load()
            for yy in range(sel[1], sel[3]):
                for xx in range(sel[0], sel[2]):
                    if self.allowed(xx, yy):
                        dst[xx - sel[0], yy - sel[1]] = src[xx, yy]
                        src[xx, yy] = (0, 0, 0, 0)
            p.set_view(view)
            self.moving = {"what": "selection", "start": (x, y), "image": lifted, "origin": sel[:2],
                           "at": sel[:2]}
        else:
            self.selection = None
            self.moving = {"what": "layer", "start": (x, y), "origin": (p.x, p.y)}
        self.dirty.add(self.lid)

    def drag_move(self, x: int, y: int):
        m = self.moving
        dx, dy = x - m["start"][0], y - m["start"][1]
        if m["what"] == "layer":
            p = self.paint()
            p.x, p.y = m["origin"][0] + dx, m["origin"][1] + dy
            self.status(f"Moved {dx:+d}, {dy:+d}")
        else:
            m["at"] = (m["origin"][0] + dx, m["origin"][1] + dy)
            w, h = m["image"].size
            self.selection = (m["at"][0], m["at"][1], m["at"][0] + w, m["at"][1] + h)
        self.redraw()

    def end_move(self):
        m, self.moving = self.moving, None
        if m["what"] == "selection":
            self.put_down(m["image"], *m["at"])
            w, h = m["image"].size
            ax, ay = m["at"]
            self.selection = (max(0, ax), max(0, ay), min(core.W, ax + w), min(core.H, ay + h))
            if self.selection[0] >= self.selection[2] or self.selection[1] >= self.selection[3]:
                self.selection = None
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
        """The arrow keys, with the Move tool: the selection, or the drawing layer."""
        p = self.paint()
        if self.tool.get() != "move" or p is None or self.moving is not None:
            return
        sel = self.selection
        if sel is not None:
            cx, cy = sel[0], sel[1]
            self.start_move(cx, cy)
            self.drag_move(cx + dx, cy + dy)
            self.end_move()
        else:
            self.push_undo()
            p.x += dx
            p.y += dy
            self.dirty.add(self.lid)
            self.redraw()

    # --------------------------------------------------------- selection
    def select_all(self):
        self.selection = (0, 0, core.W, core.H)
        self.redraw()

    def copy(self):
        p = self.paint()
        if p is None or self.selection is None:
            return
        self.clipboard = p.view().crop(self.selection)
        self.status(f"Copied {self.clipboard.width} x {self.clipboard.height} pixels.")

    def paste(self):
        if self.clipboard is None or self.paint() is None:
            return
        self.floating = self.clipboard.copy()
        self.status("Pasted: move it and click to put it down (H flips it, Esc cancels).")
        self.redraw()

    def paste_as_layer(self):
        if self.clipboard is None or self.stack() is None:
            return
        x, y = self.selection[:2] if self.selection else (0, 0)
        self.add_paint(core.Paint(self.new_name("Pasted"), self.clipboard.copy(), x, y))
        self.set_tool("move")
        self.status("Pasted as a new drawing layer: drag it into place with the Move tool.")

    def flip_floating(self):
        if self.floating is not None:
            self.floating = self.floating.transpose(Image.FLIP_LEFT_RIGHT)
            self.redraw()
        elif self.moving is None and self.selection is not None and self.paint() is not None:
            # flip the selected pixels where they are
            self.push_undo()
            p = self.paint()
            region = p.view().crop(self.selection).transpose(Image.FLIP_LEFT_RIGHT)
            view = p.view()
            for yy in range(self.selection[1], self.selection[3]):
                for xx in range(self.selection[0], self.selection[2]):
                    if self.allowed(xx, yy):
                        view.putpixel((xx, yy), region.getpixel((xx - self.selection[0], yy - self.selection[1])))
            p.set_view(view)
            self.dirty.add(self.lid)
            self.redraw()

    def stamp_floating(self, x: int, y: int):
        self.push_undo()
        self.put_down(self.floating, x, y)
        self.dirty.add(self.lid)
        self.redraw()

    def clear_selection(self):
        p = self.paint()
        if self.selection is None or p is None:
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
        self.floating = None
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
        return core.lock_middle(flat, self.middle[lid]) if self.lock_middle.get() else flat

    def save_layer(self, lid: str) -> Path:
        layer = self.cap.layer(lid)
        path = core.save_art(self.art_dir, layer, self.picture(lid), self.originals[lid])
        core.save_project(self.art_dir, layer.key, self.stacks[lid], self.active[lid])
        self.dirty.discard(lid)
        return path

    def save(self):
        if self.stack() is None or self.art_dir is None:
            return
        path = self.save_layer(self.lid)
        self.status(f"Saved {path.name}" + ("  (the whole layer)" if path.stem.endswith("_full") else
                                            "  (the side bands)") + ". In the game: L + R + A takes it in.")
        self.refresh_layer_labels()

    def save_all(self):
        if self.cap is None or self.art_dir is None:
            return
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
            saved = core.existing_art(self.art_dir, l.key) if self.art_dir else None
            ttk.Radiobutton(self.layer_frame, text=l.label + ("  - drawn" if saved else ""),
                            variable=self.layer_var, value=l.id, command=self.select_layer).pack(anchor="w")
        index = self.cap_list.curselection()
        if index:
            made = sum(1 for l in self.cap.layers if core.existing_art(self.art_dir, l.key))
            self.cap_list.delete(index[0])
            self.cap_list.insert(index[0], f"{self.cap.name}   {len(self.cap.layers)} layers" +
                                 (f", {made} drawn" if made else ""))
            self.cap_list.selection_set(index[0])
        layer = self.layer()
        if layer is not None:
            saved = core.existing_art(self.art_dir, layer.key)
            what = "position" if layer.kind == "obj" else "scroll"
            self.layer_info.configure(text=f"{layer.label}\nkey {layer.key}\n{what} {layer.hofs}, {layer.vofs}\n"
                                           + (f"saved: {saved.name}" if saved else "not drawn yet"))
        self.redraw()

    def revert(self):
        if self.stack() is None:
            return
        self.push_undo()
        self.stacks[self.lid] = [core.Paint("Layer 1", self.originals[self.lid].copy())]
        self.active[self.lid] = 0
        self.middle[self.lid] = self.originals[self.lid]
        self.changed()

    def delete_saved(self):
        layer = self.layer()
        if layer is None or self.art_dir is None:
            return
        if not core.existing_art(self.art_dir, layer.key):
            self.status("Nothing saved for this layer.")
            return
        if messagebox.askyesno(APP, f"Delete the saved picture for {layer.label}? The game goes back to "
                                    "filling its bands by itself."):
            core.delete_art(self.art_dir, layer)
            core.delete_project(self.art_dir, layer.key)
            self.stacks[self.lid] = [core.Paint("Layer 1", self.originals[self.lid].copy())]
            self.active[self.lid] = 0
            self.middle[self.lid] = self.originals[self.lid]
            self.dirty.discard(self.lid)
            self.update_paint_list()
            self.refresh_layer_labels()

    def export_png(self):
        if self.stack() is None:
            return
        path = filedialog.asksaveasfilename(defaultextension=".png", filetypes=[("PNG", "*.png")],
                                            initialfile=f"{self.lid}.png")
        if path:
            self.picture(self.lid).save(path)
            self.status(f"Exported {path} (320 x 192, all the drawing layers in one)")

    def import_png(self):
        if self.stack() is None:
            return
        path = filedialog.askopenfilename(filetypes=[("Pictures", "*.png *.gif *.bmp"), ("All", "*.*")])
        if path:
            img = core.load_rgba(path)
            self.add_paint(core.Paint(Path(path).stem, img))
            if img.size != (core.W, core.H):
                self.set_tool("move")
                self.status(f"Imported {img.width} x {img.height}: drag it into place with the Move tool.")

    def open_outside(self):
        layer = self.layer()
        if layer is None or self.art_dir is None:
            return
        if self.lid in self.dirty or not core.existing_art(self.art_dir, layer.key):
            self.save()
        saved = core.existing_art(self.art_dir, layer.key)
        if saved:
            open_externally(saved)
            self.status(f"Opened {saved.name}: save it there, then File > Reload saved picture.")

    def reload_saved(self):
        layer = self.layer()
        if layer is None or self.art_dir is None:
            return
        saved = core.existing_art(self.art_dir, layer.key)
        if saved:
            self.push_undo()
            self.stacks[self.lid] = [core.Paint("Layer 1", core.load_rgba(saved))]
            self.active[self.lid] = 0
            self.changed()

    def confirm_discard(self) -> bool:
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
        """The layer being drawn as it shows now (a selection being moved
        included)."""
        img = self.picture(self.lid)
        if self.moving is not None and self.moving["what"] == "selection":
            lifted = self.moving["image"]
            ax, ay = self.moving["at"]
            img = img.copy()
            layer = Image.new("RGBA", (core.W, core.H), (0, 0, 0, 0))
            layer.paste(lifted, (ax, ay))
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
        if self.floating is not None and self.mouse is not None:
            img = img.copy()
            layer = Image.new("RGBA", (core.W, core.H), (0, 0, 0, 0))
            layer.paste(self.floating, self.mouse)
            img.alpha_composite(layer)
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
            x0, y0, x1, y1 = self.selection
            self.canvas.create_rectangle(x0 * z, y0 * z, x1 * z, y1 * z, outline="#00ffff", dash=(3, 2))
        p = self.paint()
        if self.tool.get() == "move" and p is not None and self.selection is None:
            # the drawing layer's extent
            self.canvas.create_rectangle(p.x * z, p.y * z, (p.x + p.image.width) * z, (p.y + p.image.height) * z,
                                         outline="#ff66ff", dash=(2, 4))
        self.canvas.configure(scrollregion=(0, 0, core.W * z, core.H * z))
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
            "Move (V): drag the drawing layer, or drag a selection's pixels; arrow keys nudge.\n"
            "[ and ]: brush size. Right-click: pick a colour.\n\n"
            "4. Save. In the game, L + R + A takes the new pictures in (or restart it)."))


def self_test() -> int:
    """(CI) Open a made-up capture; fill a layer's bands, draw on a new
    drawing layer and move it, save: 0 if the picture is where the game
    reads it and the drawing layers open again."""
    import tempfile

    with tempfile.TemporaryDirectory() as tmp:
        art = Path(tmp) / "wide"
        core.make_sample_capture(art, sprites=True)
        global SETTINGS
        SETTINGS = Path(tmp) / "settings.json"
        app = Editor(str(art))
        app.withdraw()
        app.update()
        ok = app.cap is not None and app.lid == "bg3" and app.cap.layer("obj102") is not None
        if ok:
            app.fill_bands("mirror")
            app.new_layer()
            app.set_size(3)
            app.stamp(10, 10, (255, 0, 0, 255))
            app.paint().x += 2            # (the Move tool's doing)
            app.save()
            saved = core.existing_art(art, "6d5a3d55615351bf")
            ok = saved is not None and saved.name == "6d5a3d55615351bf_8_300.png"
            pic = core.load_rgba(saved)
            ok = ok and pic.getpixel((31, 5))[0] == core.snap((32, 7, 9))[0]
            ok = ok and pic.getpixel((12, 10))[:3] == (255, 0, 0) and pic.getpixel((13, 11))[:3] == (255, 0, 0)
            ok = ok and pic.getpixel((100, 50)) == core.snap(app.originals["bg3"].getpixel((100, 50)))
            project = core.load_project(art, "6d5a3d55615351bf")
            ok = ok and project is not None and len(project[0]) == 2 and project[0][1].x == 2
            # the sprites' layer: its own picture
            app.layer_var.set("obj102")
            app.select_layer()
            app.fill_bands("extend")
            app.save()
            ok = ok and core.existing_art(art, "10d372806e45f5c2") is not None
            # the Move tool: the red dot's pixels dragged 10 down
            app.layer_var.set("bg3")
            app.select_layer()
            app.set_tool("move")
            app.selection = (11, 9, 14, 12)
            app.start_move(12, 10)
            app.drag_move(12, 20)
            app.end_move()
            app.save()
            pic = core.load_rgba(core.existing_art(art, "6d5a3d55615351bf"))
            ok = ok and pic.getpixel((12, 20))[:3] == (255, 0, 0) and pic.getpixel((12, 10))[:3] != (255, 0, 0)
        app.destroy()
    return 0 if ok else 1


def main():
    if "--selftest" in sys.argv:
        sys.exit(self_test())
    folder = sys.argv[1] if len(sys.argv) > 1 else None
    app = Editor(folder)
    app.mainloop()


if __name__ == "__main__":
    main()
