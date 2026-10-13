/*
 * The DS's two 2D graphics engines, drawn in software.
 *
 * The game sets up the display through registers, palettes, OAM and VRAM,
 * all of which are ordinary memory in the port (port_ioreg.c,
 * port_memory.c). Once a frame this reads that state and draws what each
 * engine would show: 256x192 pixels in the DS's 15-bit colour (bits 0-4 red,
 * 5-9 green, 10-14 blue).
 *
 * What it draws, per engine:
 *   - the BG modes 0-5: text BGs (4 and 8 bits per pixel, flips, extended
 *     palettes), affine BGs, and the extended affine BGs (16-bit map
 *     entries, 256-colour and direct-colour bitmaps), with scrolling and
 *     affine transforms;
 *   - OBJs (sprites): normal and affine (double size), 4 and 8 bits per
 *     pixel with extended palettes, 1D and 2D character mapping, bitmap
 *     OBJs, semi-transparent OBJs and OBJ windows;
 *   - windows 0, 1 and OBJ, colour effects (alpha blending, brightness up
 *     and down) and the master brightness the game fades the screen with;
 *   - engine A's display modes: off, normal, and VRAM display.
 * Not yet: DS 3D on the 3DS GPU (PC shows captured triangles), mosaic, the large
 * bitmap BG of mode 6, the main-memory display mode, display capture, and
 * anything changed during the frame (H-blank effects): each frame is drawn
 * from the state at its end.
 *
 * VRAM: each CPU view (engine A BG, A OBJ, B BG, B OBJ, LCDC) is its own
 * block in the port (mmap_global.h), so BG and OBJ graphics are read where
 * the game wrote them. Extended palettes are loaded through the LCDC view;
 * the VRAM bank registers say which bank holds them, and they're read from
 * that bank's LCDC area.
 *
 * Speed: this runs on the 3DS's CPU for both screens every frame, so it
 * works a line at a time and a tile at a time: a text BG reads each map
 * entry and each tile row once for the pixels it covers, each line only
 * looks at the OBJs on it (sorted out once a frame), the windows are worked
 * out once a line, and a pixel's two front-most layers are found walking the
 * BGs in priority order. A layer's pixel is its colour with bit 15 set, or 0
 * where it's transparent. tests/render2d_reference.c is the plain version
 * this was made from; test_render checks that both draw the same.
 *
 * The two engines can be drawn at the same time (from two threads): each
 * has its own working memory. One engine must not be drawn by two threads
 * at once.
 */

#include <nitro.h>
#include <nitro_port.h>
#include <nitro_port_gx3d.h>
#include <string.h>

#define W 256
#define H 192
#define OPAQUE 0x8000

/* ---------------------------------------------------------------- memory */

extern unsigned char NitroPort_IoReg[];

/* The blocks the renderer reads are aligned, and everything it reads as 16
 * or 32 bits is at an aligned offset in them (the masks keep it so). */
typedef u16 __attribute__((may_alias)) u16a;
typedef u32 __attribute__((may_alias)) u32a;

static inline u16 Rd16(const u8 *p, u32 off)
{
    return *(const u16a *)(p + off);
}

static inline u32 Rd32(const u8 *p, u32 off)
{
    return *(const u32a *)(p + off);
}

#define IO16(off) Rd16(NitroPort_IoReg, (off))

/* LCDC addresses of the VRAM banks, relative to the LCDC view. */
static const u32 sBankLcdc[9] = {
    0x00000, 0x20000, 0x40000, 0x60000, 0x80000, 0x90000, 0x94000, 0x98000, 0xA0000
};

static u8 BankCnt(int bank)
{
    /* VRAMCNT_A-G at 0x240-0x246, H at 0x248, I at 0x249 */
    return NitroPort_IoReg[bank < 7 ? 0x240 + bank : 0x241 + bank];
}

/* ----------------------------------------------------------- one engine */

typedef struct Engine {
    const u8 *io;          /* the engine's registers (A: +0, B: +0x1000) */
    const u8 *bgPltt;      /* 0x200 bytes */
    const u8 *objPltt;
    const u8 *oam;         /* 0x400 bytes */
    const u8 *bgVram;
    u32 bgMask;
    const u8 *objVram;
    u32 objMask;
    const u8 *bgExt[4];    /* BG extended palette slots (8 KB each) or NULL */
    const u8 *objExt;      /* OBJ extended palette (8 KB) or NULL */
    BOOL isA;
    /* The palettes as pixels (colour | OPAQUE), 0 for the transparent
     * colour: index 0 of each 16 colours (4 bits per pixel), or of all 256
     * (8 bits per pixel). */
    u16 bgPal4[256];
    u16 bgPal8[256];
    u16 objPal4[256];
    u16 objPal8[256];
} Engine;

/* The OBJ layer's pixels on the current line. */
typedef struct ObjLine {
    u8 prios;              /* bit p: some pixel has priority p */
    short x0, x1;          /* where the line's OBJs are */
    u16 color[W];          /* colour | OPAQUE, or 0 */
    u8 prio[W];
    u8 alpha[W];           /* bitmap OBJ alpha 1-16 (16 = opaque) */
    u8 semi[W];            /* semi-transparent: blends with what's below */
    u8 window[W];          /* inside the OBJ window */
} ObjLine;

static void FindExtPalettes(Engine *e)
{
    int bank, i;

    for (i = 0; i < 4; i++) {
        e->bgExt[i] = NULL;
    }
    e->objExt = NULL;

    for (bank = 0; bank < 9; bank++) {
        u8 cnt = BankCnt(bank);
        int mst = cnt & 7;
        int ofs = (cnt >> 3) & 3;
        const u8 *lcdc = NitroPort_LcdcVram + sBankLcdc[bank];

        if (!(cnt & 0x80)) {
            continue;
        }
        if (e->isA) {
            if (mst == 4 && bank == 4) {                    /* E: slots 0-3 */
                for (i = 0; i < 4; i++) {
                    e->bgExt[i] = lcdc + i * 0x2000;
                }
            } else if (mst == 4 && (bank == 5 || bank == 6)) {   /* F/G: 2 slots */
                int first = (ofs & 1) * 2;

                e->bgExt[first] = lcdc;
                e->bgExt[first + 1] = lcdc + 0x2000;
            } else if (mst == 5 && (bank == 5 || bank == 6)) {   /* F/G: OBJ */
                e->objExt = lcdc;
            }
        } else {
            if (mst == 2 && bank == 7) {                    /* H: slots 0-3 */
                for (i = 0; i < 4; i++) {
                    e->bgExt[i] = lcdc + i * 0x2000;
                }
            } else if (mst == 3 && bank == 8) {             /* I: OBJ */
                e->objExt = lcdc;
            }
        }
    }
}

static inline u16 PlttColor(const u8 *pltt, u32 index)
{
    return Rd16(pltt, index * 2) & 0x7FFF;
}

static void MakePalettes(Engine *e)
{
    u32 i;

    for (i = 0; i < 256; i++) {
        u16 bg = PlttColor(e->bgPltt, i) | OPAQUE;
        u16 obj = PlttColor(e->objPltt, i) | OPAQUE;

        e->bgPal4[i] = (i & 15) ? bg : 0;
        e->bgPal8[i] = i ? bg : 0;
        e->objPal4[i] = (i & 15) ? obj : 0;
        e->objPal8[i] = i ? obj : 0;
    }
}

/* ----------------------------------------------------------------- BGs */

enum { BG_NONE, BG_TEXT, BG_AFFINE, BG_EXTENDED, BG_3D, BG_LARGE };

static int BgKind(const Engine *e, int bg, u32 dispcnt)
{
    int mode = dispcnt & 7;

    if (bg == 0 && e->isA && (dispcnt & 8)) {
        return BG_3D;
    }
    switch (mode) {
    case 0:
        return BG_TEXT;
    case 1:
        return bg < 3 ? BG_TEXT : BG_AFFINE;
    case 2:
        return bg < 2 ? BG_TEXT : BG_AFFINE;
    case 3:
        return bg < 3 ? BG_TEXT : BG_EXTENDED;
    case 4:
        return bg < 2 ? BG_TEXT : (bg == 2 ? BG_AFFINE : BG_EXTENDED);
    case 5:
        return bg < 2 ? BG_TEXT : BG_EXTENDED;
    case 6:
        return bg == 2 && e->isA ? BG_LARGE : BG_NONE;
    default:
        return BG_NONE;
    }
}

static u32 CharBase(const Engine *e, u16 bgcnt, u32 dispcnt)
{
    u32 base = ((bgcnt >> 2) & 0xF) * 0x4000;

    if (e->isA) {
        base += ((dispcnt >> 24) & 7) * 0x10000;
    }
    return base;
}

static u32 ScreenBase(const Engine *e, u16 bgcnt, u32 dispcnt)
{
    u32 base = ((bgcnt >> 8) & 0x1F) * 0x800;

    if (e->isA) {
        base += ((dispcnt >> 27) & 7) * 0x10000;
    }
    return base;
}

/* The extended palette slot a BG uses: its own number, or BG0/BG1 can
 * take slot 2/3 (BGxCNT bit 13). */
static const u8 *BgExtSlot(const Engine *e, int bg, u16 bgcnt)
{
    int slot = bg;

    if (bg < 2 && (bgcnt & 0x2000)) {
        slot += 2;
    }
    return e->bgExt[slot];
}

/* ---------------------------------------------------------- the line */

/*
 * Where a layer's pixels go. Two ways:
 *   - T_PLAIN: every pixel is written (colour | OPAQUE, or 0), to a line of
 *     the layer's own;
 *   - otherwise into the screen's line, front layer first: a pixel only
 *     goes where no layer in front has put one (and with T_WINDOW only where
 *     the windows show the layer; with T_LAYER, which layer it was is noted
 *     for the colour effects).
 */
enum { T_PLAIN = 1, T_WINDOW = 2, T_LAYER = 4 };

typedef struct Target {
    u16 *row;
    u8 *layerOf;           /* T_LAYER */
    const u8 *enable;      /* T_WINDOW: the layers each pixel shows */
    u8 layer;
    u8 bit;                /* 1 << layer */
    int filled;            /* pixels of the line that have their layer */
} Target;

/* One pixel into the line. A macro over the drawing function's own copies
 * of the target (row, enable, layerOf, layer, bit, filled), which the
 * compiler can keep in registers: through the Target it would have to
 * reload them after every pixel written. */
#define PUT(x_, c_, flags_)                                                  \
    do {                                                                     \
        int px_ = (x_);                                                      \
        u16 pc_ = (c_);                                                      \
                                                                             \
        if ((flags_) & T_PLAIN) {                                            \
            row[px_] = pc_;                                                  \
        } else if (pc_ && !(row[px_] & OPAQUE) &&                            \
                   (!((flags_) & T_WINDOW) || (enable[px_] & bit))) {        \
            row[px_] = pc_;                                                  \
            if ((flags_) & T_LAYER) {                                        \
                layerOf[px_] = layer;                                        \
            }                                                                \
            filled++;                                                        \
        }                                                                    \
    } while (0)

#define TARGET_LOCALS(t)                                                     \
    u16 *row = (t)->row;                                                     \
    u8 *layerOf = (t)->layerOf;                                              \
    const u8 *enable = (t)->enable;                                          \
    u8 layer = (t)->layer;                                                   \
    u8 bit = (t)->bit;                                                       \
    int filled = (t)->filled

/* A text BG's line, a tile (or the part of one on screen) at a time. */
static inline __attribute__((always_inline)) void DrawTextBgT(const Engine *e, int bg, u16 bgcnt, u32 dispcnt,
                                                              int y, Target *t, int flags)
{
    static const int sizes[4][2] = { { 256, 256 }, { 512, 256 }, { 256, 512 }, { 512, 512 } };
    int mapW = sizes[bgcnt >> 14][0];
    int mapH = sizes[bgcnt >> 14][1];
    const u8 *io = e->io;
    const u8 *vram = e->bgVram;
    u32 mask = e->bgMask;
    u32 charBase = CharBase(e, bgcnt, dispcnt);
    u32 screenBase = ScreenBase(e, bgcnt, dispcnt);
    BOOL bpp8 = (bgcnt & 0x80) != 0;
    const u8 *ext = (bpp8 && (dispcnt & 0x40000000)) ? BgExtSlot(e, bg, bgcnt) : NULL;
    int hofs = Rd16(io, 0x10 + bg * 4) & 0x1FF;
    int ty = (y + (Rd16(io, 0x12 + bg * 4) & 0x1FF)) & (mapH - 1);
    /* The map row this line is in: its address for the left and the right
     * 256-pixel half of the map. */
    u32 rowAddr = screenBase + (u32)((ty >> 8) * (mapW >> 8)) * 0x800 + (u32)((ty & 255) >> 3) * 64;
    int x = 0;
    TARGET_LOCALS(t);

    while (x < W) {
        int tx = (x + hofs) & (mapW - 1);
        int px0 = tx & 7;
        int n = 8 - px0;
        u32 entryAddr = rowAddr + (u32)(tx >> 8) * 0x800 + (u32)((tx & 255) >> 3) * 2;
        u32 entry = Rd16(vram, entryAddr & mask);
        u32 tile = entry & 0x3FF;
        int py = (entry & 0x800) ? 7 - (ty & 7) : (ty & 7);
        int flip = (entry & 0x400) ? 7 : 0;   /* pixel px of the tile is at px ^ flip */
        int i;

        if (n > W - x) {
            n = W - x;
        }
        if (bpp8) {
            const u8 *chars = vram + ((charBase + tile * 64 + (u32)py * 8) & mask);

            if (ext != NULL) {
                const u8 *pltt = ext + ((entry >> 12) << 9);

                for (i = 0; i < n; i++) {
                    u32 index = chars[(px0 + i) ^ flip];

                    PUT(x + i, index ? (u16)(PlttColor(pltt, index) | OPAQUE) : 0, flags);
                }
            } else {
                for (i = 0; i < n; i++) {
                    PUT(x + i, e->bgPal8[chars[(px0 + i) ^ flip]], flags);
                }
            }
        } else {
            u32 bits = Rd32(vram, (charBase + tile * 32 + (u32)py * 4) & mask);
            const u16 *pal = e->bgPal4 + ((entry >> 12) << 4);

            if (bits == 0) {
                /* all transparent */
                if (flags & T_PLAIN) {
                    memset(row + x, 0, (size_t)n * 2);
                }
            } else if (n == 8) {
                if (flip) {
                    /* the pixels right to left: the nibbles high to low */
                    bits = ((bits >> 4) & 0x0F0F0F0Fu) | ((bits & 0x0F0F0F0Fu) << 4);
                    bits = (bits >> 24) | ((bits >> 8) & 0xFF00u) | ((bits << 8) & 0xFF0000u) | (bits << 24);
                }
                PUT(x + 0, pal[bits & 15], flags);
                PUT(x + 1, pal[(bits >> 4) & 15], flags);
                PUT(x + 2, pal[(bits >> 8) & 15], flags);
                PUT(x + 3, pal[(bits >> 12) & 15], flags);
                PUT(x + 4, pal[(bits >> 16) & 15], flags);
                PUT(x + 5, pal[(bits >> 20) & 15], flags);
                PUT(x + 6, pal[(bits >> 24) & 15], flags);
                PUT(x + 7, pal[bits >> 28], flags);
            } else {
                for (i = 0; i < n; i++) {
                    PUT(x + i, pal[(bits >> (((px0 + i) ^ flip) * 4)) & 0xF], flags);
                }
            }
        }
        x += n;
    }
    t->filled = filled;
}

static void DrawTextBg(const Engine *e, int bg, u16 bgcnt, u32 dispcnt, int y, Target *t, int flags)
{
    switch (flags) {
    case T_PLAIN:
        DrawTextBgT(e, bg, bgcnt, dispcnt, y, t, T_PLAIN);
        break;
    case 0:
        DrawTextBgT(e, bg, bgcnt, dispcnt, y, t, 0);
        break;
    case T_WINDOW:
        DrawTextBgT(e, bg, bgcnt, dispcnt, y, t, T_WINDOW);
        break;
    case T_LAYER:
        DrawTextBgT(e, bg, bgcnt, dispcnt, y, t, T_LAYER);
        break;
    default:
        DrawTextBgT(e, bg, bgcnt, dispcnt, y, t, T_WINDOW | T_LAYER);
        break;
    }
}

/* A layer's own line (T_PLAIN) into the screen's line, from x0 to x1; for
 * the OBJ line, only the pixels of priority prio. */
static inline __attribute__((always_inline)) void MergeT(Target *t, const u16 *line, const u8 *prioOf,
                                                         int prio, int x0, int x1, int flags)
{
    int x;
    TARGET_LOCALS(t);

    for (x = x0; x < x1; x++) {
        if (prioOf == NULL || prioOf[x] == prio) {
            PUT(x, line[x], flags);
        }
    }
    t->filled = filled;
}

static void Merge(Target *t, const u16 *line, const u8 *prioOf, int prio, int x0, int x1, int flags)
{
    switch (flags) {
    case 0:
        MergeT(t, line, prioOf, prio, x0, x1, 0);
        break;
    case T_WINDOW:
        MergeT(t, line, prioOf, prio, x0, x1, T_WINDOW);
        break;
    case T_LAYER:
        MergeT(t, line, prioOf, prio, x0, x1, T_LAYER);
        break;
    default:
        MergeT(t, line, prioOf, prio, x0, x1, T_WINDOW | T_LAYER);
        break;
    }
}

/* Affine and extended BGs: the texture coordinate of each pixel comes from
 * the reference point and the 2x2 matrix (8 fractional bits). */
static void DrawAffineBg(const Engine *e, int bg, int kind, u16 bgcnt, u32 dispcnt, int y, u16 *out)
{
    const u8 *io = e->io;
    const u8 *vram = e->bgVram;
    u32 mask = e->bgMask;
    u32 base = bg == 2 ? 0x20 : 0x30;
    s32 pa = (s16)Rd16(io, base + 0);
    s32 pb = (s16)Rd16(io, base + 2);
    s32 pc = (s16)Rd16(io, base + 4);
    s32 pd = (s16)Rd16(io, base + 6);
    s32 refX = (s32)(Rd32(io, base + 8) << 4) >> 4;    /* 28-bit signed */
    s32 refY = (s32)(Rd32(io, base + 12) << 4) >> 4;
    s32 fx = refX + pb * y;
    s32 fy = refY + pd * y;
    BOOL wrap = (bgcnt & 0x2000) != 0;
    int size = bgcnt >> 14;
    u32 charBase = CharBase(e, bgcnt, dispcnt);
    u32 screenBase = ScreenBase(e, bgcnt, dispcnt);
    int mode;   /* 0: affine (8-bit map), 1: extended tiles, 2: 256-colour bitmap, 3: direct colour */
    int mapW, mapH, x;
    const u8 *ext = NULL;

    if (kind == BG_AFFINE) {
        mode = 0;
        mapW = mapH = 128 << size;
    } else if (!(bgcnt & 0x80)) {
        mode = 1;
        mapW = mapH = 128 << size;
        if (dispcnt & 0x40000000) {
            ext = BgExtSlot(e, bg, bgcnt);
        }
    } else {
        static const int bmp[4][2] = { { 128, 128 }, { 256, 256 }, { 512, 256 }, { 512, 512 } };

        mode = (bgcnt & 0x4) ? 3 : 2;
        mapW = bmp[size][0];
        mapH = bmp[size][1];
        screenBase = ((bgcnt >> 8) & 0x1F) * 0x4000;   /* bitmaps: 16 KB units */
    }

    for (x = 0; x < W; x++, fx += pa, fy += pc) {
        s32 tx = fx >> 8;
        s32 ty = fy >> 8;
        u32 index;

        out[x] = 0;
        if (wrap) {
            tx &= mapW - 1;
            ty &= mapH - 1;
        } else if (tx < 0 || ty < 0 || tx >= mapW || ty >= mapH) {
            continue;
        }

        switch (mode) {
        case 3: {
            u16 c = Rd16(vram, (screenBase + ((u32)ty * (u32)mapW + (u32)tx) * 2) & mask);

            if (c & 0x8000) {
                out[x] = c;   /* colour | OPAQUE */
            }
            break;
        }
        case 2:
            out[x] = e->bgPal8[vram[(screenBase + (u32)ty * (u32)mapW + (u32)tx) & mask]];
            break;
        case 0: {
            u32 tile = vram[(screenBase + (u32)(ty >> 3) * (u32)(mapW >> 3) + (u32)(tx >> 3)) & mask];

            out[x] = e->bgPal8[vram[(charBase + tile * 64 + (u32)(ty & 7) * 8 + (u32)(tx & 7)) & mask]];
            break;
        }
        default: {
            u32 entry = Rd16(vram, (screenBase + ((u32)(ty >> 3) * (u32)(mapW >> 3) + (u32)(tx >> 3)) * 2) & mask);
            int px = tx & 7;
            int py = ty & 7;

            if (entry & 0x400) {
                px = 7 - px;
            }
            if (entry & 0x800) {
                py = 7 - py;
            }
            index = vram[(charBase + (entry & 0x3FF) * 64 + (u32)py * 8 + (u32)px) & mask];
            if (index != 0) {
                out[x] = (ext != NULL ? PlttColor(ext, ((entry >> 12) << 8) | index)
                                      : PlttColor(e->bgPltt, index)) | OPAQUE;
            }
            break;
        }
        }
    }
}

/* ---------------------------------------------------------------- OBJs */

static const u8 sObjSize[3][4][2] = {
    { { 8, 8 }, { 16, 16 }, { 32, 32 }, { 64, 64 } },     /* square */
    { { 16, 8 }, { 32, 8 }, { 32, 16 }, { 64, 32 } },     /* wide */
    { { 8, 16 }, { 8, 32 }, { 16, 32 }, { 32, 64 } },     /* tall */
};

/* An OBJ that shows, as OAM describes it, worked out once a frame. */
typedef struct Obj {
    s16 ox;                /* left edge, -256..255 */
    u8 oy;
    u8 w, h;
    u16 boxW, boxH;        /* doubled for double-size affine OBJs */
    u8 affine, hflip, vflip, bpp8;
    u8 mode;               /* 0 normal, 1 semi-transparent, 2 window, 3 bitmap */
    u8 prio;
    u8 pal;
    u16 tile;
    s32 pa, pb, pc, pd;
} Obj;

/* This frame's OBJs, and which of them are on each line (in OAM order). */
typedef struct ObjFrame {
    Obj obj[128];
    u8 count;
    u8 blends;             /* some are semi-transparent or bitmap OBJs */
    u8 onLine[H][128];
    u8 lineCount[H];
} ObjFrame;

static void SortObjs(const Engine *e, ObjFrame *f)
{
    int i, y;

    f->count = 0;
    f->blends = 0;
    memset(f->lineCount, 0, sizeof(f->lineCount));
    for (i = 0; i < 128; i++) {
        const u8 *o = e->oam + i * 8;
        u16 a0 = Rd16(o, 0), a1 = Rd16(o, 2), a2 = Rd16(o, 4);
        int shape = (a0 >> 14) & 3;
        Obj *obj = &f->obj[f->count];

        if (!(a0 & 0x100) && (a0 & 0x200)) {
            continue;                       /* hidden */
        }
        if (shape == 3) {
            continue;
        }
        obj->affine = (a0 & 0x100) != 0;
        obj->w = sObjSize[shape][(a1 >> 14) & 3][0];
        obj->h = sObjSize[shape][(a1 >> 14) & 3][1];
        obj->boxW = obj->w;
        obj->boxH = obj->h;
        obj->pa = 256;
        obj->pb = 0;
        obj->pc = 0;
        obj->pd = 256;
        if (obj->affine) {
            int p = (a1 >> 9) & 0x1F;

            obj->pa = (s16)Rd16(e->oam, (u32)p * 32 + 6);
            obj->pb = (s16)Rd16(e->oam, (u32)p * 32 + 14);
            obj->pc = (s16)Rd16(e->oam, (u32)p * 32 + 22);
            obj->pd = (s16)Rd16(e->oam, (u32)p * 32 + 30);
            if (a0 & 0x200) {               /* double size */
                obj->boxW *= 2;
                obj->boxH *= 2;
            }
        }
        obj->oy = (u8)(a0 & 0xFF);
        obj->ox = (s16)(a1 & 0x1FF);
        if (obj->ox >= 256) {
            obj->ox -= 512;
        }
        obj->hflip = !obj->affine && (a1 & 0x1000) != 0;
        obj->vflip = !obj->affine && (a1 & 0x2000) != 0;
        obj->bpp8 = (a0 & 0x2000) != 0;
        obj->mode = (a0 >> 10) & 3;
        obj->tile = a2 & 0x3FF;
        obj->prio = (a2 >> 10) & 3;
        obj->pal = a2 >> 12;
        if (obj->ox >= W || obj->ox + obj->boxW <= 0) {
            continue;                       /* off the sides */
        }

        /* The lines it's on: (y - oy) & 255 < boxH. */
        for (y = 0; y < H; y++) {
            if (((y - obj->oy) & 0xFF) < obj->boxH) {
                f->onLine[y][f->lineCount[y]++] = f->count;
            }
        }
        if (obj->mode == 1 || obj->mode == 3) {
            f->blends = 1;
        }
        f->count++;
    }
}

/* The address of an OBJ's pixel row in its graphics: a tile's row of 8
 * pixels (tx a multiple of 8), or for bitmap OBJs the pixel itself. */
static u32 ObjCharAddr(const Obj *obj, u32 dispcnt, int tx, int ty)
{
    if (obj->mode == 3) {
        if (dispcnt & 0x40) {       /* 1D */
            return (u32)obj->tile * ((dispcnt & 0x400000) ? 256u : 128u) + ((u32)ty * obj->w + (u32)tx) * 2;
        } else if (dispcnt & 0x20) {    /* 2D, 256 dots wide */
            return (u32)(obj->tile & 0x1F) * 0x10 + (u32)(obj->tile & ~0x1F) * 0x80 + ((u32)ty * 256 + (u32)tx) * 2;
        } else {                    /* 2D, 128 dots wide */
            return (u32)(obj->tile & 0x0F) * 0x10 + (u32)(obj->tile & ~0x0F) * 0x80 + ((u32)ty * 128 + (u32)tx) * 2;
        }
    } else {
        u32 addr;
        int tileX = tx >> 3, tileY = ty >> 3;

        if (dispcnt & 0x10) {       /* 1D character mapping */
            u32 unit = 32u << ((dispcnt >> 20) & 3);
            u32 n = (u32)(tileY * (obj->w >> 3) + tileX);

            addr = (u32)obj->tile * unit + n * (obj->bpp8 ? 64u : 32u);
        } else {                    /* 2D: 32 characters per row */
            addr = (u32)obj->tile * 32 + (u32)tileY * 1024 + (u32)tileX * (obj->bpp8 ? 64u : 32u);
        }
        return addr + (u32)(ty & 7) * (obj->bpp8 ? 8u : 4u);
    }
}

/* One pixel of an OBJ's graphics, at (tx, ty) in it: its colour | OPAQUE
 * and *alpha, or 0 where it's transparent. */
static u16 ObjPixel(const Engine *e, const Obj *obj, u32 dispcnt, int tx, int ty, int *alpha)
{
    u32 addr;

    *alpha = 16;
    if (obj->mode == 3) {                /* bitmap OBJ: 15-bit colour */
        u16 rgb = Rd16(e->objVram, ObjCharAddr(obj, dispcnt, tx, ty) & e->objMask);

        if (!(rgb & 0x8000) || obj->pal == 0) {
            return 0;
        }
        *alpha = obj->pal + 1;
        return rgb;
    }
    addr = ObjCharAddr(obj, dispcnt, tx & ~7, ty);
    if (obj->bpp8) {
        u32 index = e->objVram[(addr + (u32)(tx & 7)) & e->objMask];

        if (index == 0) {
            return 0;
        }
        return ((e->objExt != NULL && (dispcnt & 0x80000000))
                    ? PlttColor(e->objExt, ((u32)obj->pal << 8) | index)
                    : PlttColor(e->objPltt, index)) | OPAQUE;
    } else {
        u8 pair = e->objVram[(addr + (u32)((tx & 7) >> 1)) & e->objMask];
        u32 index = (tx & 1) ? (pair >> 4) : (pair & 0xF);

        if (index == 0) {
            return 0;
        }
        return PlttColor(e->objPltt, ((u32)obj->pal << 4) | index) | OPAQUE;
    }
}

/* Put one OBJ pixel on the line (lower-numbered OBJs, drawn first, stay in
 * front; the window OBJs only mark the OBJ window). */
static inline void PutObjPixel(const Obj *obj, ObjLine *out, int x, u16 rgb, int alpha)
{
    if (obj->mode == 2) {
        out->window[x] = 1;
        return;
    }
    out->color[x] = rgb | OPAQUE;
    out->prio[x] = obj->prio;
    out->prios |= (u8)(1 << obj->prio);
    out->semi[x] = obj->mode == 1;
    out->alpha[x] = (u8)alpha;
}

/* The OBJ layer's line; returns whether any OBJ is on it. */
static BOOL DrawObjs(const Engine *e, const ObjFrame *f, u32 dispcnt, int y, ObjLine *out)
{
    int k;

    if (!(dispcnt & 0x1000) || f->lineCount[y] == 0) {
        return FALSE;
    }
    memset(out->color, 0, sizeof(out->color));
    memset(out->window, 0, sizeof(out->window));
    out->prios = 0;
    out->x0 = W;
    out->x1 = 0;

    for (k = 0; k < f->lineCount[y]; k++) {
        const Obj *obj = &f->obj[f->onLine[y][k]];
        int ly = (y - obj->oy) & 0xFF;
        int sx = obj->ox < 0 ? -obj->ox : 0;
        int end = obj->ox + obj->boxW > W ? W - obj->ox : obj->boxW;

        if (obj->ox + sx < out->x0) {
            out->x0 = (short)(obj->ox + sx);
        }
        if (obj->ox + end > out->x1) {
            out->x1 = (short)(obj->ox + end);
        }

        if (obj->affine) {
            s32 dy = ly - obj->boxH / 2;
            s32 dx = sx - obj->boxW / 2;
            s32 fx = obj->pa * dx + obj->pb * dy;
            s32 fy = obj->pc * dx + obj->pd * dy;

            for (; sx < end; sx++, fx += obj->pa, fy += obj->pc) {
                int x = obj->ox + sx;
                int tx = (fx >> 8) + obj->w / 2;
                int ty = (fy >> 8) + obj->h / 2;
                int alpha;
                u16 rgb;

                if ((out->color[x] & OPAQUE) && obj->mode != 2) {
                    continue;
                }
                if (tx < 0 || ty < 0 || tx >= obj->w || ty >= obj->h) {
                    continue;
                }
                rgb = ObjPixel(e, obj, dispcnt, tx, ty, &alpha);
                if (rgb) {
                    PutObjPixel(obj, out, x, rgb, alpha);
                }
            }
        } else if (obj->mode == 3) {
            int ty = obj->vflip ? obj->h - 1 - ly : ly;

            for (; sx < end; sx++) {
                int x = obj->ox + sx;
                int tx = obj->hflip ? obj->w - 1 - sx : sx;
                int alpha;
                u16 rgb;

                if (out->color[x] & OPAQUE) {
                    continue;
                }
                rgb = ObjPixel(e, obj, dispcnt, tx, ty, &alpha);
                if (rgb) {
                    PutObjPixel(obj, out, x, rgb, alpha);
                }
            }
        } else {
            /* A row of tiles: each tile's row of 8 pixels read once. */
            int ty = obj->vflip ? obj->h - 1 - ly : ly;
            const u8 *vram = e->objVram;
            u32 mask = e->objMask;
            const u8 *ext = obj->bpp8 && e->objExt != NULL && (dispcnt & 0x80000000)
                                ? e->objExt + ((u32)obj->pal << 9) : NULL;
            const u16 *pal = obj->bpp8 ? e->objPal8 : e->objPal4 + ((u32)obj->pal << 4);
            while (sx < end) {
                int tx = obj->hflip ? obj->w - 1 - sx : sx;
                u32 addr = ObjCharAddr(obj, dispcnt, tx & ~7, ty);
                /* the pixels of this tile on the line, left to right */
                int n = obj->hflip ? (tx & 7) + 1 : 8 - (tx & 7);
                int step = obj->hflip ? -1 : 1;
                int i;

                if (n > end - sx) {
                    n = end - sx;
                }
                for (i = 0; i < n; i++, tx += step) {
                    int x = obj->ox + sx + i;
                    u32 index;

                    if ((out->color[x] & OPAQUE) && obj->mode != 2) {
                        continue;
                    }
                    if (obj->bpp8) {
                        index = vram[(addr + (u32)(tx & 7)) & mask];
                    } else {
                        u8 pair = vram[(addr + (u32)((tx & 7) >> 1)) & mask];

                        index = (tx & 1) ? (pair >> 4) : (pair & 0xF);
                    }
                    if (index != 0) {
                        PutObjPixel(obj, out, x, ext != NULL ? PlttColor(ext, index) : pal[index], 16);
                    }
                }
                sx += n;
            }
        }
    }
    return TRUE;
}

/* ------------------------------------------------------------ the frame */

static u16 Blend(u16 a, u16 b, int eva, int evb)
{
    /* The three parts side by side in one word (red at bit 0, blue at 10,
     * green at 21), so two multiplications do all six; each sum is under
     * 1024 and can't run into the next part. */
    u32 pa = (a & 0x7C1Fu) | ((u32)(a & 0x03E0u) << 16);
    u32 pb = (b & 0x7C1Fu) | ((u32)(b & 0x03E0u) << 16);
    u32 sum = pa * (u32)eva + pb * (u32)evb;
    int r = (sum & 0x3FF) >> 4;
    int bl = ((sum >> 10) & 0x3FF) >> 4;
    int g = ((sum >> 21) & 0x3FF) >> 4;

    if (r > 31) {
        r = 31;
    }
    if (g > 31) {
        g = 31;
    }
    if (bl > 31) {
        bl = 31;
    }
    return (u16)(r | (g << 5) | (bl << 10));
}

static u16 Brighten(u16 c, int evy)
{
    int r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;

    r += ((31 - r) * evy) >> 4;
    g += ((31 - g) * evy) >> 4;
    b += ((31 - b) * evy) >> 4;
    return (u16)(r | (g << 5) | (b << 10));
}

static u16 Darken(u16 c, int evy)
{
    int r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;

    r -= (r * evy) >> 4;
    g -= (g * evy) >> 4;
    b -= (b * evy) >> 4;
    return (u16)(r | (g << 5) | (b << 10));
}

static BOOL InRange(int v, int lo, int hi)
{
    /* hi is exclusive; lo > hi wraps around the screen edge */
    return lo <= hi ? (v >= lo && v < hi) : (v >= lo || v < hi);
}

/* Set enable[x] = value for the x in a window's horizontal range. */
static void FillRange(u8 *enable, int lo, int hi, u8 value)
{
    if (lo <= hi) {
        if (hi > lo) {
            memset(enable + lo, value, (size_t)(hi - lo));
        }
    } else {
        memset(enable + lo, value, (size_t)(W - lo));   /* lo <= 255 */
        memset(enable, value, (size_t)hi);
    }
}

/* The layers of a line, front to back: the OBJs of priority 0, the BGs of
 * priority 0 (by number), the OBJs of priority 1, and so on. */
#define SLOT_OBJ 4

/* What drawing one engine works with, line by line. */
typedef struct EngineWork {
    u16 bgLine[W];
    ObjLine objLine;
    ObjFrame objFrame;
    u8 enableLine[W];
    /* Per pixel of the line, when blending needs two layers: the two
     * front-most layers (0-3 BG, 4 OBJ), their colours, how many found. */
    u16 color1[W], color2[W];
    u8 layer1[W], layer2[W], found[W];
    u8 alpha3d[W];   /* BG0's 3D alpha (0-31) on this line */
} EngineWork;

static EngineWork sWork[2];

/* BG0 acquires the newest finished image on both platforms. PC fills it
 * with the Stage 1 software rasterizer, 3DS with the PICA off-screen worker.
 * A held image is never overwritten by its producer during DrawEngine. */

static void DrawEngine(Engine *e, u16 *out)
{
    /* Each engine has its own (they may be drawn at the same time, on two
     * CPU cores). */
    EngineWork *work = &sWork[e->isA ? 0 : 1];
    u16 *bgLine = work->bgLine;
    ObjLine *objLine = &work->objLine;
    ObjFrame *objFrame = &work->objFrame;
    u8 *enableLine = work->enableLine;
    u16 *color1 = work->color1, *color2 = work->color2;
    u8 *layer1 = work->layer1, *layer2 = work->layer2, *found = work->found;
    const u8 *io = e->io;
    u32 dispcnt = Rd32(io, 0);
    u16 bgcnt[4];
    int kind[4];
    const u16 *gx3dPixels = NULL;
    const u8 *gx3dAlpha = NULL;
    u8 *alpha3d = work->alpha3d;   /* BG0's 3D alpha on the line being drawn */
    int gx3dTranslucent = 0;   /* the 3D image has pixels of alpha 1-30 */
    u16 bldcnt = Rd16(io, 0x50);
    u16 bldalpha = Rd16(io, 0x52);
    int eva = bldalpha & 31, evb = (bldalpha >> 8) & 31;
    int evy = Rd16(io, 0x54) & 31;
    int effect = (bldcnt >> 6) & 3;
    u16 win0h = Rd16(io, 0x40), win1h = Rd16(io, 0x42);
    u16 win0v = Rd16(io, 0x44), win1v = Rd16(io, 0x46);
    u16 winin = Rd16(io, 0x48), winout = Rd16(io, 0x4A);
    BOOL anyWindow = (dispcnt & 0xE000) != 0;
    u16 backdrop = PlttColor(e->bgPltt, 0);
    BOOL blend2;   /* some pixels may blend two layers */
    int flags;     /* how layers go into the line, when only the front one matters */
    int y, x, bg, p;

    if (eva > 16) {
        eva = 16;
    }
    if (evb > 16) {
        evb = 16;
    }
    if (evy > 16) {
        evy = 16;
    }
    FindExtPalettes(e);
    MakePalettes(e);
    for (bg = 0; bg < 4; bg++) {
        bgcnt[bg] = Rd16(io, 8 + bg * 2);
        kind[bg] = (dispcnt & (0x100u << bg)) ? BgKind(e, bg, dispcnt) : BG_NONE;
        if (kind[bg] == BG_LARGE) {
            kind[bg] = BG_NONE;   /* large-bitmap mode still unsupported */
        }
    }
    if (e->isA && kind[0] == BG_3D) {
        gx3dPixels = NitroPort_Gx3dAcquireImage();
        gx3dAlpha = gx3dPixels != NULL ? NitroPort_Gx3dImageAlpha(gx3dPixels) : NULL;
        gx3dTranslucent = gx3dAlpha != NULL && NitroPort_Gx3dImageTranslucent(gx3dPixels);
    }
    objFrame->blends = 0;
    if (dispcnt & 0x1000) {
        SortObjs(e, objFrame);
    }
    if (!anyWindow) {
        memset(enableLine, 0x3F, W);
    }
    /* Blending two layers needs second targets, and alpha blending or OBJs
     * that blend by themselves; otherwise each pixel shows its front-most
     * layer, perhaps brightened or darkened. */
    blend2 = (bldcnt & 0x3F00) && (effect == 1 || objFrame->blends || gx3dTranslucent);
    flags = (anyWindow ? T_WINDOW : 0) | (effect >= 2 ? T_LAYER : 0);

    for (y = 0; y < H; y++) {
        u16 *row = out + y * W;
        BOOL objs = DrawObjs(e, objFrame, dispcnt, y, objLine);
        Target t;
        int done = 0;   /* (blend2) pixels that have both their layers */

        /* Which layers and effects each pixel shows (the windows): window
         * 0 over window 1 over the OBJ window over the outside. */
        if (anyWindow) {
            memset(enableLine, winout & 0x3F, W);
            if ((dispcnt & 0x8000) && objs) {
                for (x = objLine->x0; x < objLine->x1; x++) {
                    if (objLine->window[x]) {
                        enableLine[x] = (winout >> 8) & 0x3F;
                    }
                }
            }
            if ((dispcnt & 0x4000) && InRange(y, win1v >> 8, win1v & 0xFF)) {
                FillRange(enableLine, win1h >> 8, win1h & 0xFF, (winin >> 8) & 0x3F);
            }
            if ((dispcnt & 0x2000) && InRange(y, win0v >> 8, win0v & 0xFF)) {
                FillRange(enableLine, win0h >> 8, win0h & 0xFF, winin & 0x3F);
            }
        }

        t.row = blend2 ? bgLine : row;
        t.layerOf = layer1;
        t.enable = enableLine;
        t.filled = 0;
        if (blend2) {
            memset(found, 0, W);
        } else {
            memset(row, 0, W * sizeof(u16));
        }

        /* The layers front to back (the OBJs of priority 0, the BGs of
         * priority 0 by number, the OBJs of priority 1, ...), until every
         * pixel has what it needs: the layers behind aren't even drawn. */
        for (p = 0; p < 4; p++) {
            int slot;

            for (slot = -1; slot < 4; slot++) {
                const u16 *px;
                const u8 *prioOf = NULL;
                int layer, x0 = 0, x1 = W;

                if (blend2 ? done >= W : t.filled >= W) {
                    break;
                }
                if (slot < 0) {
                    if (!objs || !(objLine->prios & (1 << p))) {
                        continue;
                    }
                    layer = SLOT_OBJ;
                    px = objLine->color;
                    prioOf = objLine->prio;
                    x0 = objLine->x0;
                    x1 = objLine->x1;
                } else {
                    bg = slot;
                    if (kind[bg] == BG_NONE || (bgcnt[bg] & 3) != p) {
                        continue;
                    }
                    layer = bg;
                    px = bgLine;
                    t.layer = (u8)layer;
                    t.bit = (u8)(1 << layer);
                    if (kind[bg] == BG_TEXT && !blend2) {
                        /* straight into the line */
                        DrawTextBg(e, bg, bgcnt[bg], dispcnt, y, &t, flags);
                        continue;
                    }
                    if (kind[bg] == BG_TEXT) {
                        DrawTextBg(e, bg, bgcnt[bg], dispcnt, y, &t, T_PLAIN);
                    } else if (kind[bg] == BG_3D) {
                        int scroll = Rd16(io, 0x10) & 0x1FF;
                        if (scroll == 0 && gx3dPixels != NULL && gx3dAlpha != NULL) {
                            /* The image's own line, no copy. */
                            px = gx3dPixels + y * W;
                            alpha3d = (u8 *)(gx3dAlpha + y * W);
                        } else {
                            alpha3d = work->alpha3d;
                            for (x = 0; x < W; ++x) {
                                int sx = (x + scroll) & 0x1FF;
                                bgLine[x] = (sx < W && gx3dPixels != NULL)
                                                ? gx3dPixels[y * W + sx] : 0;
                                alpha3d[x] = (sx < W && gx3dAlpha != NULL)
                                                ? gx3dAlpha[y * W + sx] : 31;
                            }
                        }
                    } else {
                        DrawAffineBg(e, bg, kind[bg], bgcnt[bg], dispcnt, y, bgLine);
                    }
                }
                t.layer = (u8)layer;
                t.bit = (u8)(1 << layer);

                if (!blend2) {
                    Merge(&t, px, prioOf, p, x0, x1, flags);
                    continue;
                }
                for (x = x0; x < x1; x++) {
                    u16 c = px[x];
                    int n = found[x];

                    if (!(c & OPAQUE) || n >= 2 || !(enableLine[x] & t.bit) ||
                        (prioOf != NULL && prioOf[x] != p)) {
                        continue;
                    }
                    if (n == 0) {
                        color1[x] = c & 0x7FFF;
                        layer1[x] = (u8)layer;
                    } else {
                        color2[x] = c & 0x7FFF;
                        layer2[x] = (u8)layer;
                        done++;
                    }
                    found[x] = (u8)(n + 1);
                }
            }
        }

        if (!blend2) {
            if (!(flags & T_LAYER)) {
                if (((uintptr_t)row & 3) == 0) {
                    /* Two pixels at a time: a pixel keeps its colour if
                     * drawn, else takes the backdrop. */
                    u32a *pair = (u32a *)row;
                    u32 back2 = (u32)backdrop * 0x10001u;

                    for (x = 0; x < W / 2; x++) {
                        u32 c = pair[x];
                        u32 drawn = (c >> 15) & 0x10001u;

                        pair[x] = (c & (drawn * 0x7FFFu)) | (back2 & ((drawn ^ 0x10001u) * 0xFFFFu));
                    }
                } else {
                    for (x = 0; x < W; x++) {
                        u16 c = row[x];

                        row[x] = (c & OPAQUE) ? (u16)(c & 0x7FFF) : backdrop;
                    }
                }
            } else {
                /* brighten or darken the layers chosen */
                for (x = 0; x < W; x++) {
                    u16 c = row[x];
                    int top = (c & OPAQUE) ? layer1[x] : 5;

                    c = (c & OPAQUE) ? (u16)(c & 0x7FFF) : backdrop;
                    if ((enableLine[x] & 0x20) && (bldcnt & (1 << top))) {
                        c = effect == 2 ? Brighten(c, evy) : Darken(c, evy);
                    }
                    row[x] = c;
                }
            }
            continue;
        }
        for (x = 0; x < W; x++) {
            u8 enable = enableLine[x];
            int n = found[x];
            int top = n > 0 ? layer1[x] : 5;          /* 5: the backdrop */
            int second = n > 1 ? layer2[x] : 5;
            u16 topColor = n > 0 ? color1[x] : backdrop;
            u16 secondColor = n > 1 ? color2[x] : backdrop;
            u16 c = topColor;

            if (top == 0 && kind[0] == BG_3D && alpha3d[x] < 31 &&
                (bldcnt & (0x100 << second))) {
                /* A partly transparent 3D pixel blends with what's below
                 * by its own alpha, whatever the effect setting. */
                int a = (alpha3d[x] * 16 + 15) / 31;
                c = Blend(topColor, secondColor, a, 16 - a);
            } else if (top == SLOT_OBJ && (objLine->semi[x] || objLine->alpha[x] < 16) &&
                (bldcnt & (0x100 << second))) {
                /* Semi-transparent and bitmap OBJs blend with what's below,
                 * whatever the effect setting. */
                if (objLine->alpha[x] < 16) {
                    c = Blend(topColor, secondColor, objLine->alpha[x], 16 - objLine->alpha[x]);
                } else {
                    c = Blend(topColor, secondColor, eva, evb);
                }
            } else if ((enable & 0x20) && (bldcnt & (1 << top))) {
                if (effect == 1 && (bldcnt & (0x100 << second))) {
                    c = Blend(topColor, secondColor, eva, evb);
                } else if (effect == 2) {
                    c = Brighten(topColor, evy);
                } else if (effect == 3) {
                    c = Darken(topColor, evy);
                }
            }
            row[x] = c;
        }
    }
    /* Release only after every scanline has been composited. */
    if(gx3dPixels)NitroPort_Gx3dReleaseImage(gx3dPixels);
}

static void MasterBrightness(u16 *out, u16 reg)
{
    int mode = reg >> 14;
    int factor = reg & 31;
    int i;

    if (factor > 16) {
        factor = 16;
    }
    if (factor == 0 || (mode != 1 && mode != 2)) {
        return;
    }
    if (factor == 16) {
        /* all the way: white or black */
        for (i = 0; i < W * H; i++) {
            out[i] = mode == 1 ? 0x7FFF : 0;
        }
        return;
    }
    for (i = 0; i < W * H; i++) {
        out[i] = mode == 1 ? Brighten(out[i], factor) : Darken(out[i], factor);
    }
}

static void Fill(u16 *out, u16 color)
{
    int i;

    for (i = 0; i < W * H; i++) {
        out[i] = color;
    }
}

void NitroPort_Render2D(int engine, u16 *out)
{
    Engine e;
    u32 dispcnt;
    u16 powcnt = IO16(0x304);

    memset(&e, 0, sizeof(e));
    e.isA = engine == 0;
    e.io = NitroPort_IoReg + (e.isA ? 0 : 0x1000);
    e.bgPltt = NitroPort_Pltt + (e.isA ? 0 : 0x400);
    e.objPltt = e.bgPltt + 0x200;
    e.oam = NitroPort_Oam + (e.isA ? 0 : 0x400);
    e.bgVram = e.isA ? NitroPort_BgVram : NitroPort_DbBgVram;
    e.bgMask = e.isA ? 0x7FFFF : 0x1FFFF;
    e.objVram = e.isA ? NitroPort_ObjVram : NitroPort_DbObjVram;
    e.objMask = e.isA ? 0x3FFFF : 0x1FFFF;
    dispcnt = Rd32(e.io, 0);

    /* The engine powered off: nothing on that screen. */
    if (!(powcnt & (e.isA ? 0x0002 : 0x0200))) {
        Fill(out, 0);
        return;
    }
    if (dispcnt & 0x80) {                  /* forced blank: white */
        Fill(out, 0x7FFF);
        return;
    }
    switch ((dispcnt >> 16) & (e.isA ? 3 : 1)) {
    case 0:                                /* display off: white */
        Fill(out, 0x7FFF);
        return;
    case 1:
        DrawEngine(&e, out);
        break;
    case 2: {                              /* VRAM display: a bank in LCDC */
        const u8 *bank = NitroPort_LcdcVram + ((dispcnt >> 18) & 3) * 0x20000;
        int i;

        for (i = 0; i < W * H; i++) {
            out[i] = Rd16(bank, (u32)i * 2) & 0x7FFF;
        }
        break;
    }
    default:
        NitroPort_Unimplemented("display mode: main memory display");
        Fill(out, 0);
        return;
    }
    MasterBrightness(out, Rd16(e.io, 0x6C));
}

BOOL NitroPort_EngineAOnTop(void)
{
    /* POWCNT1 bit 15: engine A's picture goes to the upper screen. */
    return (IO16(0x304) & 0x8000) != 0;
}

void NitroPort_RenderScreens(u16 *top, u16 *bottom)
{
    BOOL aOnTop = NitroPort_EngineAOnTop();

    NitroPort_Render2D(0, aOnTop ? top : bottom);
    NitroPort_Render2D(1, aOnTop ? bottom : top);
}

/* ------------------------------------------------- unchanged screens */

/* A 32-bit FNV-1a hash of words, two lanes at a time. */
static u32 HashWords(const u8 *p, u32 bytes, u32 h)
{
    const u32a *w = (const u32a *)p;
    u32 n = bytes / 8, h2 = h ^ 0x9E3779B9u, i;

    for (i = 0; i < n; i++) {
        h = (h ^ w[2 * i]) * 16777619u;
        h2 = (h2 ^ w[2 * i + 1]) * 16777619u;
    }
    return (h ^ (h2 * 31u)) * 16777619u;
}

/* Everything an engine's picture comes from: its registers, palettes, OAM,
 * VRAM (as the CPU sees it, and the LCDC banks it may use as extended
 * palettes or for VRAM display), the VRAM bank settings and POWCNT1. */
static u32 EngineSignature(int engine)
{
    BOOL isA = engine == 0;
    const u8 *io = NitroPort_IoReg + (isA ? 0 : 0x1000);
    u32 h = 2166136261u;

    h = HashWords(io, 0x70, h);
    h = HashWords(NitroPort_IoReg + 0x240, 0x10, h);   /* VRAMCNT */
    h = (h ^ IO16(0x304)) * 16777619u;                  /* POWCNT1 */
    h = HashWords(NitroPort_Pltt + (isA ? 0 : 0x400), 0x400, h);
    h = HashWords(NitroPort_Oam + (isA ? 0 : 0x400), 0x400, h);
    if (isA) {
        h = HashWords(NitroPort_BgVram, 0x80000, h);
        h = HashWords(NitroPort_ObjVram, 0x40000, h);
        h = HashWords(NitroPort_LcdcVram + 0x80000, 0x18000, h);   /* E, F, G */
        if (((Rd32(io, 0) >> 16) & 3) == 2) {                       /* VRAM display */
            h = HashWords(NitroPort_LcdcVram + ((Rd32(io, 0) >> 18) & 3) * 0x20000, 0x20000, h);
        }
    } else {
        h = HashWords(NitroPort_DbBgVram, 0x20000, h);
        h = HashWords(NitroPort_DbObjVram, 0x20000, h);
        h = HashWords(NitroPort_LcdcVram + 0x98000, 0xC000, h);    /* H, I */
    }
    return h;
}

/* NitroPort_Render2D, unless nothing it draws from changed since the last
 * time for this engine (then out is left as it is, and 0 returned). Working
 * that out reads the engine's VRAM once: far less than drawing it. A 3D
 * layer counts as changed whenever a new 3D image is out. While the 3D
 * keeps changing, the screen is drawn for each new 3D image only, nothing
 * hashed: in step with the 3D (a moving camera moves evenly), and no
 * drawing in between that would hold the next 3D image up. */
#define QUIET_3D_CALLS 4   /* calls without a new 3D image: the 3D has stopped */
int NitroPort_Render2DIfChanged(int engine, u16 *out)
{
    static struct { u32 signature; u32 images; int valid; int following; } last[2];
    int e = engine ? 1 : 0;
    u32 images = NitroPort_Gx3dImageCount();
    u32 dispcnt = Rd32(NitroPort_IoReg, 0);
    BOOL uses3D = e == 0 && (dispcnt & 0x0108) == 0x0108;   /* BG0 on, as 3D */
    u32 signature;

    if (uses3D && images != last[e].images) {
        /* New 3D: draw, and check the rest again once the 3D stops. */
        last[e].images = images;
        last[e].valid = 0;
        last[e].following = QUIET_3D_CALLS;
        NitroPort_Render2D(engine, out);
        return 1;
    }
    if (uses3D && last[e].following > 0) {
        last[e].following--;
        return 0;
    }
    signature = EngineSignature(e);
    if (last[e].valid && signature == last[e].signature) {
        return 0;
    }
    last[e].signature = signature;
    last[e].images = images;
    last[e].valid = 1;
    NitroPort_Render2D(engine, out);
    return 1;
}
