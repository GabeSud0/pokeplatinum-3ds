#!/usr/bin/env python3
"""WideArt: draw the remaster's wide upper screen by hand.

The game (L + R + X on the 3DS or in Azahar) captures the upper screen into
3ds/pokeplatinum/wide/captures; this opens those captures, lets you paint
each layer's side bands (or the whole layer), and saves the pictures where
the game reads them (3ds/pokeplatinum/wide). See docs/WIDE_ART.md.

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
from tkinter import colorchooser, filedialog, messagebox, ttk

from PIL import Image, ImageDraw, ImageTk

sys.path.insert(0, str(Path(__file__).resolve().parent))
import wideart_core as core  # noqa: E402

APP = "WideArt"
SETTINGS = Path(os.environ.get("APPDATA") or Path.home() / ".config") / "WideArt" / "settings.json"
UNDO_LIMIT = 60
TOOLS = (("pencil", "Pencil (B)"), ("eraser", "Eraser (E)"), ("fill", "Fill (G)"),
         ("pick", "Pick colour (I)"), ("select", "Select (M)"))


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


class Editor(tk.Tk):
    def __init__(self, folder: str | None = None):
        super().__init__()
        self.title(APP)
        self.geometry("1360x780")
        self.minsize(1000, 640)
        self.settings = load_settings()
        self.art_dir: Path | None = None
        self.captures: list[core.Capture] = []
        self.cap: core.Capture | None = None
        self.originals: dict[int, Image.Image] = {}   # each layer as captured
        self.images: dict[int, Image.Image] = {}      # each layer as being drawn
        self.dirty: set[int] = set()
        self.bg: int | None = None                    # the layer being drawn
        self.undo: list[tuple[int, Image.Image]] = []
        self.redo: list[tuple[int, Image.Image]] = []
        self.color = (248, 248, 248, 255)
        self.zoom = tk.IntVar(value=int(self.settings.get("zoom", 3)))
        self.tool = tk.StringVar(value="pencil")
        self.lock_middle = tk.BooleanVar(value=True)
        self.show_others = tk.BooleanVar(value=True)
        self.see_through = tk.BooleanVar(value=True)
        self.show_grid = tk.BooleanVar(value=False)
        self.side = tk.StringVar(value="both")
        self.selection: tuple[int, int, int, int] | None = None
        self.floating: Image.Image | None = None      # pasted, following the mouse
        self.clipboard: Image.Image | None = None
        self.stroke_saved = False
        self.drag_start: tuple[int, int] | None = None
        self.mouse: tuple[int, int] | None = None
        self.photo = None
        self.swatch_photos: list = []
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
        filem.add_command(label="Import layer from PNG...", command=self.import_png)
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
        editm.add_command(label="Flip pasted (horizontal)", command=self.flip_floating, accelerator="H")
        editm.add_command(label="Clear selection", command=self.clear_selection, accelerator="Delete")
        menu.add_cascade(label="Edit", menu=editm)
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

        # left: captures and layers
        left = ttk.Frame(main, width=230)
        left.pack(side="left", fill="y", padx=6, pady=6)
        ttk.Label(left, text="Captures (newest first)").pack(anchor="w")
        self.cap_list = tk.Listbox(left, height=14, exportselection=False, width=30)
        self.cap_list.pack(fill="x")
        self.cap_list.bind("<<ListboxSelect>>", lambda e: self.select_capture())
        self.thumb = ttk.Label(left)
        self.thumb.pack(pady=6)
        ttk.Label(left, text="Layers (front to back)").pack(anchor="w")
        self.layer_frame = ttk.Frame(left)
        self.layer_frame.pack(fill="x")
        self.layer_var = tk.IntVar(value=-1)
        ttk.Separator(left).pack(fill="x", pady=8)
        ttk.Checkbutton(left, text="Lock the DS's picture (middle)", variable=self.lock_middle,
                        command=self.redraw).pack(anchor="w")
        ttk.Checkbutton(left, text="Show the other layers", variable=self.show_others,
                        command=self.redraw).pack(anchor="w")
        ttk.Checkbutton(left, text="Layers in front: see-through", variable=self.see_through,
                        command=self.redraw).pack(anchor="w")

        # right: tools, colours, band fills, saving
        right = ttk.Frame(main, width=250)
        right.pack(side="right", fill="y", padx=6, pady=6)
        ttk.Label(right, text="Tools").pack(anchor="w")
        tools = ttk.Frame(right)
        tools.pack(fill="x")
        for i, (name, label) in enumerate(TOOLS):
            ttk.Radiobutton(tools, text=label, variable=self.tool, value=name,
                            command=self.redraw).grid(row=i // 2, column=i % 2, sticky="w")
        ttk.Separator(right).pack(fill="x", pady=8)
        ttk.Label(right, text="Colour").pack(anchor="w")
        crow = ttk.Frame(right)
        crow.pack(fill="x")
        self.color_box = tk.Canvas(crow, width=40, height=24, highlightthickness=1, highlightbackground="#888")
        self.color_box.pack(side="left")
        ttk.Button(crow, text="Choose...", command=self.choose_color).pack(side="left", padx=6)
        ttk.Label(right, text="This screen's colours:").pack(anchor="w", pady=(6, 0))
        self.swatches = tk.Canvas(right, width=232, height=148, highlightthickness=0)
        self.swatches.pack()
        self.swatches.bind("<Button-1>", self.pick_swatch)
        ttk.Separator(right).pack(fill="x", pady=8)
        ttk.Label(right, text="Start the side bands from the picture:").pack(anchor="w")
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
        ttk.Separator(right).pack(fill="x", pady=8)
        ttk.Button(right, text="Save layer (Ctrl+S)", command=self.save).pack(fill="x", pady=2)
        ttk.Button(right, text="Revert to the capture", command=self.revert).pack(fill="x", pady=2)
        ttk.Button(right, text="Delete saved picture", command=self.delete_saved).pack(fill="x", pady=2)
        ttk.Button(right, text="3DS preview (P)", command=self.preview).pack(fill="x", pady=2)
        self.layer_info = ttk.Label(right, text="", wraplength=240, justify="left")
        self.layer_info.pack(anchor="w", pady=8)

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
        self.canvas.bind("<Button-3>", self.on_right_click)
        self.canvas.bind("<MouseWheel>", self.on_wheel)
        self.canvas.bind("<Control-MouseWheel>", self.on_zoom_wheel)

        self.status_var = tk.StringVar()
        ttk.Label(self, textvariable=self.status_var, anchor="w", relief="sunken").pack(fill="x", side="bottom")
        self.update_color_box()

    def _bind_keys(self):
        for seq, fn in (("<Control-o>", self.choose_folder), ("<F5>", self.refresh), ("<Control-s>", self.save),
                        ("<Control-S>", self.save_all), ("<Control-z>", self.do_undo), ("<Control-y>", self.do_redo),
                        ("<Control-c>", self.copy), ("<Control-v>", self.paste), ("<Delete>", self.clear_selection),
                        ("<Escape>", self.cancel)):
            self.bind(seq, lambda e, f=fn: f())
        for key, value in (("b", "pencil"), ("e", "eraser"), ("g", "fill"), ("i", "pick"), ("m", "select")):
            self.bind(key, lambda e, v=value: self.set_tool(v))
        self.bind("h", lambda e: self.flip_floating())
        self.bind("p", lambda e: self.preview())
        self.bind("<plus>", lambda e: self.set_zoom(self.zoom.get() + 1))
        self.bind("<equal>", lambda e: self.set_zoom(self.zoom.get() + 1))
        self.bind("<minus>", lambda e: self.set_zoom(self.zoom.get() - 1))

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

    def select_capture(self, force: bool = False):
        sel = self.cap_list.curselection()
        if not sel:
            return
        cap = self.captures[sel[0]]
        if self.cap is not None and cap.path == self.cap.path and not force:
            return
        if not self.confirm_discard():
            return
        self.cap = cap
        self.originals = {l.bg: cap.layer_image(l) for l in cap.layers}
        self.images = {}
        for l in cap.layers:
            saved = core.existing_art(self.art_dir, l.key) if self.art_dir else None
            self.images[l.bg] = core.load_rgba(saved) if saved else self.originals[l.bg].copy()
        self.dirty = set()
        self.undo, self.redo = [], []
        self.selection = None
        self.floating = None
        thumb = cap.screen().resize((160, 96), Image.NEAREST)
        self.thumb_photo = ImageTk.PhotoImage(thumb)
        self.thumb.configure(image=self.thumb_photo)
        for w in self.layer_frame.winfo_children():
            w.destroy()
        for l in reversed(core.draw_order(cap.layers)):
            saved = core.existing_art(self.art_dir, l.key) if self.art_dir else None
            label = f"BG{l.bg}  (priority {l.prio})" + ("  - drawn" if saved else "")
            ttk.Radiobutton(self.layer_frame, text=label, variable=self.layer_var, value=l.bg,
                            command=self.select_layer).pack(anchor="w")
        if cap.layers:
            # the backmost layer: usually the background to draw
            self.layer_var.set(core.draw_order(cap.layers)[0].bg)
            self.select_layer()
        self.update_swatches()

    def select_layer(self):
        self.bg = self.layer_var.get()
        layer = self.cap.layer(self.bg) if self.cap else None
        if layer is not None:
            saved = core.existing_art(self.art_dir, layer.key)
            self.layer_info.configure(text=f"BG{layer.bg}, key {layer.key}\nscroll {layer.hofs}, {layer.vofs}\n"
                                           + (f"saved: {saved.name}" if saved else "not drawn yet"))
        self.redraw()

    # ---------------------------------------------------------- drawing
    def current(self) -> Image.Image | None:
        return self.images.get(self.bg) if self.bg is not None else None

    def allowed(self, x: int, y: int) -> bool:
        if not (0 <= x < core.W and 0 <= y < core.H):
            return False
        return not (self.lock_middle.get() and core.DS_X0 <= x < core.DS_X1)

    def push_undo(self):
        img = self.current()
        if img is None:
            return
        self.undo.append((self.bg, img.copy()))
        del self.undo[:-UNDO_LIMIT]
        self.redo.clear()

    def do_undo(self):
        if not self.undo:
            return
        bg, img = self.undo.pop()
        self.redo.append((bg, self.images[bg].copy()))
        self.images[bg] = img
        self.dirty.add(bg)
        self.redraw()

    def do_redo(self):
        if not self.redo:
            return
        bg, img = self.redo.pop()
        self.undo.append((bg, self.images[bg].copy()))
        self.images[bg] = img
        self.dirty.add(bg)
        self.redraw()

    def to_pixel(self, event) -> tuple[int, int]:
        z = self.zoom.get()
        return int(self.canvas.canvasx(event.x) // z), int(self.canvas.canvasy(event.y) // z)

    def plot(self, x: int, y: int, color):
        img = self.current()
        if img is None or not self.allowed(x, y):
            return
        img.putpixel((x, y), color)

    def line(self, x0, y0, x1, y1, color):
        dx, dy = abs(x1 - x0), -abs(y1 - y0)
        sx, sy = (1 if x0 < x1 else -1), (1 if y0 < y1 else -1)
        err = dx + dy
        while True:
            self.plot(x0, y0, color)
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
        if self.current() is None:
            return
        x, y = self.to_pixel(event)
        tool = self.tool.get()
        if self.floating is not None:
            self.stamp_floating(x, y)
            return
        if tool in ("pencil", "eraser"):
            self.push_undo()
            color = (0, 0, 0, 0) if tool == "eraser" else self.color
            self.plot(x, y, color)
            self.drag_start = (x, y)
            self.dirty.add(self.bg)
        elif tool == "fill":
            if 0 <= x < core.W and 0 <= y < core.H and self.allowed(x, y):
                self.push_undo()
                self.images[self.bg] = core.flood_fill(self.current(), x, y, self.color, self.allowed)
                self.dirty.add(self.bg)
        elif tool == "pick":
            self.pick_at(x, y)
        elif tool == "select":
            self.drag_start = (x, y)
            self.selection = None
        self.redraw()

    def on_drag(self, event):
        x, y = self.to_pixel(event)
        self.mouse = (x, y)
        tool = self.tool.get()
        if self.drag_start is None:
            return
        if tool in ("pencil", "eraser"):
            color = (0, 0, 0, 0) if tool == "eraser" else self.color
            self.line(*self.drag_start, x, y, color)
            self.drag_start = (x, y)
        elif tool == "select":
            x0, y0 = self.drag_start
            self.selection = (max(0, min(x0, x)), max(0, min(y0, y)),
                              min(core.W, max(x0, x) + 1), min(core.H, max(y0, y) + 1))
        self.redraw()

    def on_release(self, event):
        self.drag_start = None

    def on_motion(self, event):
        x, y = self.to_pixel(event)
        self.mouse = (x, y)
        if self.floating is not None:
            self.redraw()
        if 0 <= x < core.W and 0 <= y < core.H and self.current() is not None:
            p = self.current().getpixel((x, y))
            where = "left band" if x < core.DS_X0 else "right band" if x >= core.DS_X1 else f"DS x {x - core.DS_X0}"
            colour = f"#{p[0]:02x}{p[1]:02x}{p[2]:02x}" if p[3] else "transparent"
            self.status(f"x {x}, y {y} ({where})   {colour}")

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

    def set_tool(self, tool: str):
        self.tool.set(tool)
        self.redraw()

    def pick_at(self, x: int, y: int):
        if not (0 <= x < core.W and 0 <= y < core.H) or self.current() is None:
            return
        p = self.current().getpixel((x, y))
        if p[3] < 128:
            p = self.composite(faint=False).getpixel((x, y))
        self.color = core.snap(p)
        self.update_color_box()
        if self.tool.get() == "pick":
            self.tool.set("pencil")

    # --------------------------------------------------------- selection
    def copy(self):
        if self.current() is None or self.selection is None:
            return
        self.clipboard = self.current().crop(self.selection)
        self.status(f"Copied {self.clipboard.width} x {self.clipboard.height} pixels.")

    def paste(self):
        if self.clipboard is None or self.current() is None:
            return
        self.floating = self.clipboard.copy()
        self.status("Pasted: move it and click to put it down (H flips it, Esc cancels).")
        self.redraw()

    def flip_floating(self):
        if self.floating is not None:
            self.floating = self.floating.transpose(Image.FLIP_LEFT_RIGHT)
            self.redraw()

    def stamp_floating(self, x: int, y: int):
        self.push_undo()
        fw, fh = self.floating.size
        for fy in range(fh):
            for fx in range(fw):
                p = self.floating.getpixel((fx, fy))
                if p[3] >= 128:
                    self.plot(x + fx, y + fy, p)
        self.dirty.add(self.bg)
        self.redraw()

    def clear_selection(self):
        if self.selection is None or self.current() is None:
            return
        self.push_undo()
        x0, y0, x1, y1 = self.selection
        for y in range(y0, y1):
            for x in range(x0, x1):
                self.plot(x, y, (0, 0, 0, 0))
        self.dirty.add(self.bg)
        self.redraw()

    def cancel(self):
        self.floating = None
        self.selection = None
        self.redraw()

    # ------------------------------------------------------------ colours
    def choose_color(self):
        rgb, _ = colorchooser.askcolor(color="#%02x%02x%02x" % self.color[:3], title="Colour")
        if rgb:
            self.color = core.snap(tuple(int(v) for v in rgb) + (255,))
            self.update_color_box()

    def update_color_box(self):
        self.color_box.delete("all")
        self.color_box.create_rectangle(0, 0, 40, 24, fill="#%02x%02x%02x" % self.color[:3], outline="")

    def update_swatches(self):
        self.swatches.delete("all")
        self.swatch_colors = core.palette(self.cap.screen(), 64) if self.cap else []
        for i, c in enumerate(self.swatch_colors):
            x, y = (i % 8) * 29, (i // 8) * 18
            self.swatches.create_rectangle(x, y, x + 27, y + 16, fill="#%02x%02x%02x" % c[:3], outline="#555")

    def pick_swatch(self, event):
        i = (event.y // 18) * 8 + event.x // 29
        if 0 <= i < len(getattr(self, "swatch_colors", [])):
            self.color = self.swatch_colors[i]
            self.update_color_box()
            if self.tool.get() in ("eraser", "pick"):
                self.tool.set("pencil")

    # ------------------------------------------------------------ bands
    def fill_bands(self, how: str):
        if self.current() is None:
            return
        self.push_undo()
        self.images[self.bg] = core.fill_bands(self.current(), how, self.side.get())
        self.dirty.add(self.bg)
        self.redraw()

    # ------------------------------------------------------------ files
    def save(self):
        if self.current() is None or self.art_dir is None:
            return
        layer = self.cap.layer(self.bg)
        path = core.save_art(self.art_dir, layer, self.current(), self.originals[self.bg])
        self.dirty.discard(self.bg)
        self.status(f"Saved {path.name}" + ("  (the whole layer)" if path.stem.endswith("_full") else
                                            "  (the side bands)") + ". In the game: L + R + A takes it in.")
        self.select_layer()
        self.refresh_layer_labels()

    def save_all(self):
        if self.cap is None or self.art_dir is None:
            return
        for bg in sorted(self.dirty):
            layer = self.cap.layer(bg)
            core.save_art(self.art_dir, layer, self.images[bg], self.originals[bg])
        n = len(self.dirty)
        self.dirty.clear()
        self.status(f"Saved {n} layers.")
        self.refresh_layer_labels()

    def refresh_layer_labels(self):
        index = self.cap_list.curselection()
        current = self.bg
        self.refresh()
        if index:
            self.cap_list.selection_set(index[0])
        if current is not None:
            self.layer_var.set(current)
            self.select_layer()

    def revert(self):
        if self.current() is None:
            return
        self.push_undo()
        self.images[self.bg] = self.originals[self.bg].copy()
        self.dirty.add(self.bg)
        self.redraw()

    def delete_saved(self):
        if self.current() is None or self.art_dir is None:
            return
        layer = self.cap.layer(self.bg)
        if not core.existing_art(self.art_dir, layer.key):
            self.status("Nothing saved for this layer.")
            return
        if messagebox.askyesno(APP, f"Delete the saved picture for BG{layer.bg}? The game goes back to "
                                    "filling its bands by itself."):
            core.delete_art(self.art_dir, layer)
            self.images[self.bg] = self.originals[self.bg].copy()
            self.dirty.discard(self.bg)
            self.refresh_layer_labels()

    def export_png(self):
        if self.current() is None:
            return
        path = filedialog.asksaveasfilename(defaultextension=".png", filetypes=[("PNG", "*.png")],
                                            initialfile=f"bg{self.bg}.png")
        if path:
            self.current().save(path)
            self.status(f"Exported {path} (320 x 192; import it again when done)")

    def import_png(self):
        if self.current() is None:
            return
        path = filedialog.askopenfilename(filetypes=[("PNG", "*.png")])
        if path:
            self.take_in(Path(path))

    def take_in(self, path: Path):
        img = core.load_rgba(path)
        if img.size != (core.W, core.H):
            messagebox.showerror(APP, f"That picture is {img.width} x {img.height}; it must be "
                                      f"{core.W} x {core.H}.")
            return
        self.push_undo()
        self.images[self.bg] = img
        self.dirty.add(self.bg)
        self.redraw()

    def open_outside(self):
        if self.current() is None or self.art_dir is None:
            return
        layer = self.cap.layer(self.bg)
        if self.bg in self.dirty or not core.existing_art(self.art_dir, layer.key):
            self.save()
        saved = core.existing_art(self.art_dir, layer.key)
        if saved:
            open_externally(saved)
            self.status(f"Opened {saved.name}: save it there, then File > Reload saved picture.")

    def reload_saved(self):
        if self.current() is None or self.art_dir is None:
            return
        saved = core.existing_art(self.art_dir, self.cap.layer(self.bg).key)
        if saved:
            self.take_in(saved)
            self.dirty.discard(self.bg)

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
        if self.confirm_discard():
            self.destroy()

    # ---------------------------------------------------------- showing
    def composite(self, faint: bool = True) -> Image.Image:
        return core.composite(self.cap, self.images, None if self.show_others.get() else self.bg,
                              self.bg if faint and self.see_through.get() else None)

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
        if self.cap is None or self.current() is None:
            return
        z = self.zoom.get()
        img = self.composite()
        if self.floating is not None and self.mouse is not None:
            img = img.copy()
            img.alpha_composite(self.floating, dest=(max(0, self.mouse[0]), max(0, self.mouse[1])))
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
        self.canvas.configure(scrollregion=(0, 0, core.W * z, core.H * z))
        marks = "*" if self.bg in self.dirty else ""
        self.title(f"{APP} - {self.cap.name} BG{self.bg}{marks}")

    def status(self, text: str):
        self.status_var.set(text)

    def show_help(self):
        messagebox.showinfo(APP, (
            "1. In the game, on the screen to draw, press L + R + X: it saves a capture to "
            "3ds/pokeplatinum/wide/captures on the SD card (Azahar: its sdmc folder).\n\n"
            "2. Open that folder here (the SD card or sdmc folder is fine) and pick the capture.\n\n"
            "3. Pick a layer (the background is usually the backmost) and draw the side bands: the 32 "
            "pixels left and right of the yellow lines. Start from Mirror / Extend edge / Repeat if you "
            "like, then fix it by hand. Unlock the middle to change the DS's picture too.\n\n"
            "4. Save. In the game, L + R + A takes the new pictures in (or restart it).\n\n"
            "Right-click picks a colour. Transparent pixels show what's behind the layer."))


def self_test() -> int:
    """(CI) Open a made-up capture, fill a layer's bands, save: 0 if the
    picture is where the game reads it."""
    import tempfile

    with tempfile.TemporaryDirectory() as tmp:
        art = Path(tmp) / "wide"
        core.make_sample_capture(art)
        global SETTINGS
        SETTINGS = Path(tmp) / "settings.json"
        app = Editor(str(art))
        app.withdraw()
        app.update()
        ok = app.cap is not None and app.bg == 3
        if ok:
            app.fill_bands("mirror")
            app.save()
            saved = core.existing_art(art, "6d5a3d55615351bf")
            ok = saved is not None and saved.name == "6d5a3d55615351bf_8_300.png"
            ok = ok and core.load_rgba(saved).getpixel((31, 5))[0] == core.snap((32, 7, 9))[0]
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
