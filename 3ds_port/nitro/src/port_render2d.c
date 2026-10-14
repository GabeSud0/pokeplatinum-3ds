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
#include <nitro_port_wide.h>
#include <stddef.h>
#include <string.h>

#define W 256
#define H 192
/* The widest picture drawn (the 3DS remaster's upper screen: the DS's 256
 * pixels and 32 more each side; NitroRender2DSplit's canvas). */
#define WMAX 320
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
    /* The picture: width pixels, the DS's x = 0 at canvas x origin (256 and
     * 0 but for the remaster's wider canvas; see NitroRender2DSplit). */
    int width, origin;
    /* Wider canvas: each BG's pixels only within the DS's 256 (clip), or
     * wrapping round its map; and the game's windows moved to the screen's
     * edges (anchorWindows). */
    u8 clip[4];
    u8 anchorWindows;
    /* (worked out each frame, DecideSplits) a text BG that's a picture with
     * a plain middle and decorated sides: its halves go to the screen's
     * edges, the middle widened with its plain colour. */
    u8 split[4];
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
    u16 color[WMAX];       /* colour | OPAQUE, or 0 */
    u8 prio[WMAX];
    u8 alpha[WMAX];        /* bitmap OBJ alpha 1-16 (16 = opaque) */
    u8 semi[WMAX];         /* semi-transparent: blends with what's below */
    u8 window[WMAX];       /* inside the OBJ window */
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

/* A run of a BG's line on the canvas: x0..x1, showing map x = x + off
 * (wrapping round the map). */
typedef struct Segment {
    short x0, x1, off;
    short fixed;   /* >= 0: every pixel of the run is the map's pixel at x = fixed */
} Segment;
#define MAX_SEGMENTS 24

static const int sTextSizes[4][2] = { { 256, 256 }, { 512, 256 }, { 256, 512 }, { 512, 512 } };

/* One pixel of a text BG's map (map x, map row ty): colour | OPAQUE, or 0. */
static u16 TextBgPixel(const Engine *e, int bg, u16 bgcnt, u32 dispcnt, int mapx, int ty)
{
    int mapW = sTextSizes[bgcnt >> 14][0];
    const u8 *vram = e->bgVram;
    u32 mask = e->bgMask;
    u32 screenBase = ScreenBase(e, bgcnt, dispcnt);
    u32 charBase = CharBase(e, bgcnt, dispcnt);
    int tx = mapx & (mapW - 1);
    u32 rowAddr = screenBase + (u32)((ty >> 8) * (mapW >> 8)) * 0x800 + (u32)((ty & 255) >> 3) * 64;
    u32 entry = Rd16(vram, (rowAddr + (u32)(tx >> 8) * 0x800 + (u32)((tx & 255) >> 3) * 2) & mask);
    u32 tile = entry & 0x3FF;
    int py = (entry & 0x800) ? 7 - (ty & 7) : (ty & 7);
    int px = (entry & 0x400) ? 7 - (tx & 7) : (tx & 7);

    if (bgcnt & 0x80) {
        u32 index = vram[(charBase + tile * 64 + (u32)py * 8 + (u32)px) & mask];
        const u8 *ext = (dispcnt & 0x40000000) ? BgExtSlot(e, bg, bgcnt) : NULL;

        if (ext != NULL) {
            return index ? (u16)(PlttColor(ext + ((entry >> 12) << 9), index) | OPAQUE) : 0;
        }
        return e->bgPal8[index];
    } else {
        u8 pair = vram[(charBase + tile * 32 + (u32)py * 4 + (u32)(px >> 1)) & mask];

        return e->bgPal4[((entry >> 12) << 4) | ((px & 1) ? (pair >> 4) : (pair & 15))];
    }
}

/* Whether the DS's pixels x0..x0+n-1 of a text BG's line (map row ty, its
 * scroll hofs) are all one opaque colour. */
static BOOL TextBgPlain(const Engine *e, int bg, u16 bgcnt, u32 dispcnt, int hofs, int ty, int x0, int n)
{
    u16 first = TextBgPixel(e, bg, bgcnt, dispcnt, x0 + hofs, ty);
    int i;

    if (!(first & OPAQUE)) {
        return FALSE;
    }
    for (i = 1; i < n; i++) {
        if (TextBgPixel(e, bg, bgcnt, dispcnt, x0 + i + hofs, ty) != first) {
            return FALSE;
        }
    }
    return TRUE;
}

/* (wider canvas) Which text BGs are pictures to split (Engine.split): a
 * plain middle (the DS's x 96-159) on every line, and sides that aren't
 * plain on a quarter of them or more, the intro's bars, say. */
static void DecideSplits(Engine *e, const int *kind, const u16 *bgcnt, u32 dispcnt)
{
    int bg;

    for (bg = 0; bg < 4; bg++) {
        int y, lines = 0, decorated = 0, mapH, hofs, vofs;

        e->split[bg] = 0;
        if (kind[bg] != BG_TEXT || !e->clip[bg]) {
            continue;
        }
        mapH = sTextSizes[bgcnt[bg] >> 14][1];
        hofs = Rd16(e->io, 0x10 + bg * 4) & 0x1FF;
        vofs = Rd16(e->io, 0x12 + bg * 4) & 0x1FF;
        for (y = 4; y < H; y += 8) {
            int ty = (y + vofs) & (mapH - 1);

            if (!TextBgPlain(e, bg, bgcnt[bg], dispcnt, hofs, ty, 96, 64)) {
                break;
            }
            lines++;
            if (!TextBgPlain(e, bg, bgcnt[bg], dispcnt, hofs, ty, 0, 8) ||
                !TextBgPlain(e, bg, bgcnt[bg], dispcnt, hofs, ty, 248, 8)) {
                decorated++;
            }
        }
        e->split[bg] = lines == H / 8 && decorated * 4 >= lines;
    }
}

/* The runs of a text BG's line (y, its map row ty) on a wider canvas: the
 * DS's 256 pixels (or the whole canvas, wrapping round the map, without
 * clip), with the game's windows on the BG moved to the nearer edge of the
 * screen (NitroPort_WideWindows): a window in the left part of the screen
 * to its left edge, one in the right part to its right edge; one across
 * the middle (a text box) stays. Returns how many. */
static int TextBgSegments(const Engine *e, int bg, u16 bgcnt, u32 dispcnt, int ty, int hofs, int mapW,
                          Segment *segs)
{
    int origin = e->origin, width = e->width, n = 0, i, k;
    NitroWideWindow wins[NITRO_WIDE_MAX_WINDOWS];
    int nwins = 0;
    struct { int x0, x1, shift, off; } moved[NITRO_WIDE_MAX_WINDOWS];
    int nmoved = 0;

    if (e->split[bg]) {
        /* The halves to the edges, the plain middle widened. */
        segs[0].x0 = 0;
        segs[0].x1 = 128;
        segs[0].off = (short)hofs;
        segs[0].fixed = -1;
        segs[1].x0 = (short)(width - 128);
        segs[1].x1 = (short)width;
        segs[1].off = (short)(hofs - (width - 256));
        segs[1].fixed = -1;
        segs[2].x0 = 128;
        segs[2].x1 = (short)(width - 128);
        segs[2].off = 0;
        segs[2].fixed = (short)((128 + hofs) & (mapW - 1));
        return 3;
    }
    if (e->anchorWindows) {
        nwins = NitroPort_WideWindows(e->isA ? 0 : 1, bg, wins, NITRO_WIDE_MAX_WINDOWS);
    }
    for (i = 0; i < nwins; i++) {
        int top = wins[i].top * 8, bottom = (wins[i].top + wins[i].height) * 8;
        int a, b, shift;

        if (ty < top || ty >= bottom) {
            continue;
        }
        /* Where it is on the DS's screen (left of its first column). */
        a = ((wins[i].left * 8 - hofs) % mapW + mapW) % mapW;
        if (a >= 256) {
            a -= mapW;   /* (partly left of the screen) */
        }
        b = a + wins[i].width * 8;
        if (b <= 0 || a >= 256) {
            continue;
        }
        if (a < 128 - 8 && b <= 128 + 8 && b - a < 160) {
            shift = -origin;                 /* to the left edge */
        } else if (a >= 128 - 8 && b > 128 + 8 && b - a < 160) {
            shift = width - 256 - origin;    /* to the right edge */
        } else {
            continue;                        /* across the middle: stays */
        }
        moved[nmoved].x0 = a + origin;
        moved[nmoved].x1 = b + origin;
        moved[nmoved].shift = shift;
        moved[nmoved].off = hofs - origin - shift;
        nmoved++;
    }
    /* The BG's own pixels, less where the windows were and where they went;
     * beside them, its edge's colour when its edge is plain on this line
     * (a picture's border, say: widened without anything stretched). */
    {
        int bx0 = e->clip[bg] ? origin : 0, bx1 = e->clip[bg] ? origin + 256 : width;
        int cuts[2 * NITRO_WIDE_MAX_WINDOWS][2], ncuts = 0, x;
        int fillLeft = e->clip[bg] && origin > 0 && TextBgPlain(e, bg, bgcnt, dispcnt, hofs, ty, 0, 8);
        int fillRight = e->clip[bg] && origin + 256 < width &&
                        TextBgPlain(e, bg, bgcnt, dispcnt, hofs, ty, 248, 8);

        if (fillLeft) {
            bx0 = 0;
        }
        if (fillRight) {
            bx1 = width;
        }

        for (i = 0; i < nmoved; i++) {
            cuts[ncuts][0] = moved[i].x0;
            cuts[ncuts][1] = moved[i].x1;
            ncuts++;
            cuts[ncuts][0] = moved[i].x0 + moved[i].shift;
            cuts[ncuts][1] = moved[i].x1 + moved[i].shift;
            ncuts++;
        }
        x = bx0;
        while (x < bx1 && n < MAX_SEGMENTS - nmoved) {
            int next = bx1, skip = -1;

            /* the nearest cut at or after x */
            for (k = 0; k < ncuts; k++) {
                if (cuts[k][1] <= x) {
                    continue;
                }
                if (cuts[k][0] <= x) {
                    skip = skip < 0 || cuts[k][1] > skip ? cuts[k][1] : skip;
                } else if (cuts[k][0] < next) {
                    next = cuts[k][0];
                }
            }
            if (skip >= 0) {
                x = skip;
                continue;
            }
            /* (split at the DS's edges: the bands repeat its edge pixel) */
            if (x < origin && next > origin) {
                next = origin;
            } else if (x < origin + 256 && next > origin + 256) {
                next = origin + 256;
            }
            segs[n].x0 = (short)x;
            segs[n].x1 = (short)next;
            segs[n].off = (short)(hofs - origin);
            segs[n].fixed = x < origin ? (short)(hofs & (mapW - 1))
                          : x >= origin + 256 ? (short)((255 + hofs) & (mapW - 1)) : (short)-1;
            n++;
            x = next;
        }
    }
    for (i = 0; i < nmoved; i++) {
        int x0 = moved[i].x0 + moved[i].shift, x1 = moved[i].x1 + moved[i].shift;

        if (x0 < 0) {
            x0 = 0;
        }
        if (x1 > width) {
            x1 = width;
        }
        if (x0 < x1) {
            segs[n].x0 = (short)x0;
            segs[n].x1 = (short)x1;
            segs[n].off = (short)moved[i].off;
            segs[n].fixed = -1;
            n++;
        }
    }
    return n;
}

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
    Segment segs[MAX_SEGMENTS];
    int nsegs, seg;
    TARGET_LOCALS(t);

    if (e->width == W && e->origin == 0 && !e->anchorWindows) {
        /* the DS's own screen: one run */
        segs[0].x0 = 0;
        segs[0].x1 = W;
        segs[0].off = (short)hofs;
        segs[0].fixed = -1;
        nsegs = 1;
    } else {
        nsegs = TextBgSegments(e, bg, bgcnt, dispcnt, ty, hofs, mapW, segs);
        if (flags & T_PLAIN) {
            memset(row, 0, (size_t)e->width * 2);
        }
    }
    for (seg = 0; seg < nsegs; seg++) {
    int x = segs[seg].x0, xe = segs[seg].x1, off = segs[seg].off;

    if (segs[seg].fixed >= 0) {
        /* one pixel of the map, repeated */
        u16 c = TextBgPixel(e, bg, bgcnt, dispcnt, segs[seg].fixed, ty);

        for (; x < xe; x++) {
            PUT(x, c, flags);
        }
        continue;
    }
    while (x < xe) {
        int tx = (x + off) & (mapW - 1);
        int px0 = tx & 7;
        int n = 8 - px0;
        u32 entryAddr = rowAddr + (u32)(tx >> 8) * 0x800 + (u32)((tx & 255) >> 3) * 2;
        u32 entry = Rd16(vram, entryAddr & mask);
        u32 tile = entry & 0x3FF;
        int py = (entry & 0x800) ? 7 - (ty & 7) : (ty & 7);
        int flip = (entry & 0x400) ? 7 : 0;   /* pixel px of the tile is at px ^ flip */
        int i;

        if (n > xe - x) {
            n = xe - x;
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
        /* (a 3D pixel of alpha 0 can have a colour, but no OPAQUE) */
        if ((line[x] & OPAQUE) && (prioOf == NULL || prioOf[x] == prio)) {
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
    /* (a wider canvas: the DS's x = 0 at e->origin; without clip, the BG
     * goes on past the DS's edges as its matrix takes it) */
    int xs = e->clip[bg] ? e->origin : 0, xe = e->clip[bg] ? e->origin + W : e->width;
    s32 fx = refX + pb * y + pa * (xs - e->origin);
    s32 fy = refY + pd * y + pc * (xs - e->origin);
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

    if (xs > 0 || xe < e->width) {
        memset(out, 0, (size_t)e->width * 2);
    }
    for (x = xs; x < xe; x++, fx += pa, fy += pc) {
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
    /* (wider canvas) beside the DS's screen, the edge's colour when the
     * edge is plain on this line */
    if (xs > 0 && xe - xs >= 8) {
        int i;
        BOOL plain = (out[xs] & OPAQUE) != 0;

        for (i = 1; i < 8 && plain; i++) {
            plain = out[xs + i] == out[xs];
        }
        if (plain) {
            for (i = 0; i < xs; i++) {
                out[i] = out[xs];
            }
        }
    }
    if (xe < e->width && xe - xs >= 8) {
        int i;
        BOOL plain = (out[xe - 1] & OPAQUE) != 0;

        for (i = 2; i <= 8 && plain; i++) {
            plain = out[xe - i] == out[xe - 1];
        }
        if (plain) {
            for (i = xe; i < e->width; i++) {
                out[i] = out[xe - 1];
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
        /* (x 256-511 is left of the screen; on a wider canvas the first of
         * those are right of the DS's screen, where the canvas goes on) */
        if (obj->ox >= 256 + (e->width - W - e->origin)) {
            obj->ox -= 512;
        }
        obj->ox += e->origin;
        obj->hflip = !obj->affine && (a1 & 0x1000) != 0;
        obj->vflip = !obj->affine && (a1 & 0x2000) != 0;
        obj->bpp8 = (a0 & 0x2000) != 0;
        obj->mode = (a0 >> 10) & 3;
        obj->tile = a2 & 0x3FF;
        obj->prio = (a2 >> 10) & 3;
        obj->pal = a2 >> 12;
        if (obj->ox >= e->width || obj->ox + obj->boxW <= 0) {
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
    memset(out->color, 0, (size_t)e->width * sizeof(out->color[0]));
    memset(out->window, 0, (size_t)e->width);
    out->prios = 0;
    out->x0 = (short)e->width;
    out->x1 = 0;

    for (k = 0; k < f->lineCount[y]; k++) {
        const Obj *obj = &f->obj[f->onLine[y][k]];
        int ly = (y - obj->oy) & 0xFF;
        int sx = obj->ox < 0 ? -obj->ox : 0;
        int end = obj->ox + obj->boxW > e->width ? e->width - obj->ox : obj->boxW;

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

/* Blend() when eva + evb <= 16 (always for a 3D or bitmap OBJ pixel's own
 * alpha, a and 16 - a): no part can go over 31, so nothing to clamp, and
 * the parts come out of the sum with a shift and two masks. */
static inline u16 BlendIn16(u16 a, u16 b, u32 eva, u32 evb)
{
    u32 pa = (a & 0x7C1Fu) | ((u32)(a & 0x03E0u) << 16);
    u32 pb = (b & 0x7C1Fu) | ((u32)(b & 0x03E0u) << 16);
    u32 sum = (pa * eva + pb * evb) >> 4;

    return (u16)((sum & 0x7C1Fu) | ((sum >> 16) & 0x03E0u));
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

/* Set enable[x] = value for the x in a window's horizontal range (the DS's
 * x, at origin on the canvas; beyond the DS's screen is outside). */
static void FillRange(u8 *enable, int lo, int hi, u8 value, int origin)
{
    if (lo <= hi) {
        if (hi > lo) {
            memset(enable + origin + lo, value, (size_t)(hi - lo));
        }
    } else {
        memset(enable + origin + lo, value, (size_t)(W - lo));   /* lo <= 255 */
        memset(enable + origin, value, (size_t)hi);
    }
}

/* The layers of a line, front to back: the OBJs of priority 0, the BGs of
 * priority 0 (by number), the OBJs of priority 1, and so on. */
#define SLOT_OBJ 4

/* What drawing one engine works with, line by line. */
typedef struct EngineWork {
    u16 lineOf[4][WMAX];  /* each BG's own line, when it isn't drawn into the screen's */
    ObjLine objLine;
    ObjFrame objFrame;
    u8 enableLine[WMAX];
    u8 layer1[WMAX];      /* each pixel's front layer (0-3 BG, 4 OBJ), for the effects */
    u8 alpha3d[WMAX];     /* BG0's 3D alpha (0-31) on this line, when scrolled */
} EngineWork;

static EngineWork sWork[2];

/* A layer of the line being drawn, in the order the layers are in front of
 * each other: its own line (NULL: not drawn yet) and, for the OBJ layer,
 * the priority its pixels must have to be in this place. */
typedef struct LineLayer {
    const u16 *px;
    const u8 *prioOf;
    short x0, x1;
    u8 layer, p;
} LineLayer;

/* A BG's line in its own buffer (the 3D: the image's own line, unless
 * scrolled), and for the 3D its alpha. */
static const u16 *LayerLine(const Engine *e, EngineWork *work, int bg, int kind, u16 bgcnt,
                            u32 dispcnt, int y, const u16 *gx3dPixels, const u8 *gx3dAlpha,
                            const u8 **alpha3d)
{
    u16 *line = work->lineOf[bg];
    int x;

    if (kind == BG_TEXT) {
        Target t;

        t.row = line;
        t.layerOf = NULL;
        t.enable = NULL;
        t.layer = (u8)bg;
        t.bit = (u8)(1 << bg);
        t.filled = 0;
        DrawTextBg(e, bg, bgcnt, dispcnt, y, &t, T_PLAIN);
    } else if (kind == BG_3D) {
        int scroll = Rd16(e->io, 0x10) & 0x1FF;

        if (scroll == 0 && gx3dPixels != NULL && gx3dAlpha != NULL) {
            /* The image's own line, no copy. */
            *alpha3d = gx3dAlpha + y * W;
            return gx3dPixels + y * W;
        }
        for (x = 0; x < W; ++x) {
            int sx = (x + scroll) & 0x1FF;

            line[x] = (sx < W && gx3dPixels != NULL) ? gx3dPixels[y * W + sx] : 0;
            work->alpha3d[x] = (sx < W && gx3dAlpha != NULL) ? gx3dAlpha[y * W + sx] : 31;
        }
        *alpha3d = work->alpha3d;
    } else {
        DrawAffineBg(e, bg, kind, bgcnt, dispcnt, y, line);
    }
    return line;
}

/* What a line that may blend two layers needs (DrawEngine's, for
 * BlendLine). */
typedef struct BlendLineState {
    const Engine *e;
    EngineWork *work;
    const int *kind;
    const u16 *bgcnt;
    u32 dispcnt;
    int y;
    const u16 *gx3dPixels;
    const u8 *gx3dAlpha;
    const u8 **alpha3d;
    LineLayer *list;
    int nlist;
    const s8 *entryOf;
    const ObjLine *objLine;
    u32 special;
    u32 bldcnt;
    u16 backdrop;
    int effect, eva, evb, evy;
    /* (split) each pixel's alpha over the 3D, 0-16: see DrawEngine. */
    u8 *alphaOut;
    int width;
} BlendLineState;

/* Each pixel of a line whose front layers are drawn (row, layer1): one that
 * blends with what's behind it (a first target of alpha blending, a partly
 * transparent 3D pixel, a semi-transparent OBJ) takes the next layer that
 * covers it, or the backdrop. windows: some window is on (enableLine says
 * which layers and effects each pixel shows); without, all of them. */
static inline __attribute__((always_inline)) void BlendLineT(const BlendLineState *b, u16 *row,
                                                             const u8 *enableLine, const u8 *layer1,
                                                             int windows, int split)
{
    u8 *alphaOut = b->alphaOut;
    const ObjLine *objLine = b->objLine;
    const int nlist = b->nlist;
    const u32 special = b->special;
    const u32 bldcnt = b->bldcnt;
    const u32 seconds = (bldcnt >> 8) & 0x3F;   /* the second targets, by layer */
    /* The first targets of alpha blending (when the windows let them). */
    const u32 firsts = b->effect == 1 ? bldcnt & 0x3F : 0;
    const u16 backdrop = b->backdrop;
    const int effect = b->effect;
    const u32 eva = (u32)b->eva, evb = (u32)b->evb;
    const int evy = b->evy;
    const BOOL in16 = eva + evb <= 16;   /* alpha blending can't go over 31 */
    const BOOL has3d = b->kind[0] == BG_3D;
    const u8 *alpha3dLine = *b->alpha3d;
    /* The line's layers, copied where the compiler can keep them: the
     * pixels written don't change them (as far as it can tell). */
    const u16 *px[8];
    const u8 *prioOf[8];
    int x0[8], x1[8];
    u8 layerOf[8], prio[8], entryOf[8];
    int x, k;

    for (k = 0; k < nlist; k++) {
        px[k] = b->list[k].px;
        prioOf[k] = b->list[k].prioOf;
        x0[k] = b->list[k].x0;
        x1[k] = b->list[k].x1;
        layerOf[k] = b->list[k].layer;
        prio[k] = b->list[k].p;
    }
    memcpy(entryOf, b->entryOf, sizeof(entryOf));
    for (x = 0; x < b->width; x++) {
        u32 c = row[x];
        u32 enable = windows ? enableLine[x] : 0x3F;
        int top = 5, second = 5;          /* 5: the backdrop */
        u16 topColor = backdrop, secondColor = backdrop;
        BOOL alpha3d = FALSE, objAlpha = FALSE;

        if (split) {
            if (!(c & OPAQUE)) {
                continue;   /* (the 3D, or what's behind it) */
            }
            alphaOut[x] = 16;
        }
        if (c & OPAQUE) {
            top = layer1[x];
            topColor = (u16)(c & 0x7FFF);
            if (!((special >> top) & 1)) {
                /* (most pixels) shown as they are */
                row[x] = split ? (u16)c : topColor;
                continue;
            }
            alpha3d = top == 0 && has3d && alpha3dLine[x] < 31;
            objAlpha = top == SLOT_OBJ && (objLine->semi[x] || objLine->alpha[x] < 16);
            if (alpha3d || objAlpha || (((firsts >> top) & 1) && (enable & 0x20))) {
                /* The layers behind the front one, in order. */
                for (k = entryOf[top == SLOT_OBJ ? 4 + objLine->prio[x] : top] + 1; k < nlist; k++) {
                    u16 lc;

                    if (x < x0[k] || x >= x1[k] || (windows && !((enable >> layerOf[k]) & 1))) {
                        continue;
                    }
                    if (px[k] == NULL) {
                        px[k] = b->list[k].px =
                            LayerLine(b->e, b->work, layerOf[k], b->kind[layerOf[k]],
                                      b->bgcnt[layerOf[k]], b->dispcnt, b->y, b->gx3dPixels,
                                      b->gx3dAlpha, b->alpha3d);
                    }
                    lc = px[k][x];
                    if (!(lc & OPAQUE) || (prioOf[k] != NULL && prioOf[k][x] != prio[k])) {
                        continue;
                    }
                    second = layerOf[k];
                    secondColor = lc & 0x7FFF;
                    break;
                }
            }
        }
        if (split && second == 5) {
            /* (the layers in front of the 3D) nothing in front of the 3D
             * behind this pixel: the 3D is, and BLDCNT's second-target
             * bit for BG0 says whether the pixel blends with it. Blended
             * by the compositor, by the alpha noted here. */
            if (objAlpha && (seconds & 1)) {
                u32 a = objLine->alpha[x];

                alphaOut[x] = (u8)(a < 16 ? a : eva);
                c = topColor;
            } else if ((enable & 0x20) && (bldcnt & (1u << top))) {
                if (effect == 1 && (seconds & 1)) {
                    alphaOut[x] = (u8)eva;
                    c = topColor;
                } else if (effect == 2) {
                    c = Brighten(topColor, evy);
                } else if (effect == 3) {
                    c = Darken(topColor, evy);
                } else {
                    c = topColor;
                }
            } else {
                c = topColor;
            }
            row[x] = (u16)(c | OPAQUE);
            continue;
        }
        if (alpha3d && ((seconds >> second) & 1)) {
            /* A partly transparent 3D pixel blends with what's below by
             * its own alpha, whatever the effect setting. */
            u32 a = ((u32)alpha3dLine[x] * 16 + 15) / 31;

            c = BlendIn16(topColor, secondColor, a, 16 - a);
        } else if (objAlpha && ((seconds >> second) & 1)) {
            /* Semi-transparent and bitmap OBJs blend with what's below,
             * whatever the effect setting. */
            u32 a = objLine->alpha[x];

            if (a < 16) {
                c = BlendIn16(topColor, secondColor, a, 16 - a);
            } else {
                c = in16 ? BlendIn16(topColor, secondColor, eva, evb)
                         : Blend(topColor, secondColor, (int)eva, (int)evb);
            }
        } else if ((enable & 0x20) && (bldcnt & (1u << top))) {
            if (effect == 1 && ((seconds >> second) & 1)) {
                c = in16 ? BlendIn16(topColor, secondColor, eva, evb)
                         : Blend(topColor, secondColor, (int)eva, (int)evb);
            } else if (effect == 2) {
                c = Brighten(topColor, evy);
            } else if (effect == 3) {
                c = Darken(topColor, evy);
            } else {
                c = topColor;
            }
        } else {
            c = topColor;
        }
        row[x] = (u16)(split ? c | OPAQUE : c);
    }
}

static void BlendLine(const BlendLineState *b, u16 *row, const u8 *enableLine, const u8 *layer1,
                      BOOL windows)
{
    if (windows) {
        BlendLineT(b, row, enableLine, layer1, 1, 0);
    } else {
        BlendLineT(b, row, enableLine, layer1, 0, 0);
    }
}

/* (split) The layers in front of the 3D: each drawn pixel keeps its OPAQUE
 * bit, the others stay 0, and b->alphaOut gets each drawn pixel's alpha
 * over the 3D (16: opaque). The line's layers (b->list) are those in
 * front of the 3D only. */
static void BlendLineOver(const BlendLineState *b, u16 *row, const u8 *enableLine, const u8 *layer1,
                          BOOL windows)
{
    if (windows) {
        BlendLineT(b, row, enableLine, layer1, 1, 1);
    } else {
        BlendLineT(b, row, enableLine, layer1, 0, 1);
    }
}

/* BG0 acquires the newest finished image on both platforms. PC fills it
 * with the Stage 1 software rasterizer, 3DS with the PICA off-screen worker.
 * A held image is never overwritten by its producer during DrawEngine. */

/* (split) The layers in front of the 3D, from the line as drawn so far
 * (row, with each pixel's layer in layer1): into split->over and
 * split->overAlpha, blended among themselves or marked to blend with the
 * 3D (BlendLineOver), or brightened or darkened. */
static void SnapshotOver(NitroRender2DSplit *split, BlendLineState *blendState, const u16 *row,
                         int y, int nlist, BOOL blend2, int flags, int effect, int evy,
                         u16 bldcnt, const u8 *enableLine, const u8 *layer1)
{
    int w = blendState->width;
    u16 *over = split->over + y * w;
    u8 *alpha = split->overAlpha + y * w;
    int x;

    memcpy(over, row, (size_t)w * sizeof(u16));
    if (blend2) {
        blendState->y = y;
        blendState->nlist = nlist;
        blendState->alphaOut = alpha;
        BlendLineOver(blendState, over, enableLine, layer1, (flags & T_WINDOW) != 0);
        return;
    }
    for (x = 0; x < w; x++) {
        u16 c = over[x];

        alpha[x] = 16;
        if ((c & OPAQUE) && effect >= 2 && (enableLine[x] & 0x20) && (bldcnt & (1 << layer1[x]))) {
            over[x] = (u16)((effect == 2 ? Brighten(c & 0x7FFF, evy) : Darken(c & 0x7FFF, evy)) | OPAQUE);
        }
    }
}

/* Drawn in two layers for a compositor that puts the 3D in between
 * (split != NULL): out gets what's behind the 3D (the picture without BG0),
 * split->over the layers in front of it (OPAQUE where they cover it, 0
 * elsewhere) with each pixel's alpha over the 3D in split->overAlpha. Where
 * a window hides the 3D, over shows what's behind it. Brightness effects
 * on the 3D itself and the master brightness are left to the compositor
 * (split->tint3d, split->masterBright). */
static inline __attribute__((always_inline)) void DrawEngineT(Engine *e, u16 *out, NitroRender2DSplit *split,
                                                              int fixedWidth)
{
    /* Each engine has its own (they may be drawn at the same time, on two
     * CPU cores). */
    EngineWork *work = &sWork[e->isA ? 0 : 1];
    ObjLine *objLine = &work->objLine;
    ObjFrame *objFrame = &work->objFrame;
    u8 *enableLine = work->enableLine;
    u8 *layer1 = work->layer1;
    LineLayer list[8];   /* the line's layers, front to back */
    int nlist;
    s8 entryOf[5 + 3];   /* (blend2) each BG's place in the list; the OBJs' at 4 + priority */
    const u8 *io = e->io;
    u32 dispcnt = Rd32(io, 0);
    u16 bgcnt[4];
    int kind[4];
    const u16 *gx3dPixels = NULL;
    const u8 *gx3dAlpha = NULL;
    const u8 *alpha3d = work->alpha3d;   /* BG0's 3D alpha on the line being drawn */
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
    u32 special;   /* (blend2) the layers whose pixels may not just show as they are */
    int flags;     /* how layers go into the line, when only the front one matters */
    BlendLineState blendState;
    /* (the DS's own width known to the compiler: its loops as before) */
    const int width = fixedWidth ? fixedWidth : e->width;
    BOOL snapped;  /* (split) this line's layers in front of the 3D are noted */
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
    if (fixedWidth == 0) {
        DecideSplits(e, kind, bgcnt, dispcnt);
    }
    if (split != NULL) {
        split->has3d = e->isA && kind[0] == BG_3D;
        split->prio3d = bgcnt[0] & 3;
        split->scroll3d = split->has3d ? Rd16(io, 0x10) & 0x1FF : 0;
        split->tint3d = (split->has3d && effect >= 1 && (bldcnt & 1)) ? effect : 0;
        split->evy = evy;
        split->eva = eva;
        split->evb = evb;
        split->blend3d = (bldcnt >> 8) & 0x3F;
        split->backdrop = backdrop;
        if (split->has3d) {
            /* Drawn without BG0: the layers in front of it are noted as
             * the line reaches it. */
            kind[0] = BG_NONE;
        }
    } else if (e->isA && kind[0] == BG_3D) {
        gx3dPixels = NitroPort_Gx3dAcquireImage();
        gx3dAlpha = gx3dPixels != NULL ? NitroPort_Gx3dImageAlpha(gx3dPixels) : NULL;
        gx3dTranslucent = gx3dAlpha != NULL && NitroPort_Gx3dImageTranslucent(gx3dPixels);
    }
    objFrame->blends = 0;
    if (dispcnt & 0x1000) {
        SortObjs(e, objFrame);
    }
    if (!anyWindow) {
        memset(enableLine, 0x3F, (size_t)width);
    }
    /* Blending two layers needs second targets, and alpha blending of some
     * first target that's on screen, or OBJs or 3D pixels that blend by
     * themselves; otherwise each pixel shows its front-most layer, perhaps
     * brightened or darkened. (Battles set alpha blending with no first
     * target on the upper screen, and with BG1 on the touch screen while
     * BG1 is off: no pixel blends.) */
    {
        u32 shown = 0x20;   /* the backdrop */

        for (bg = 0; bg < 4; bg++) {
            if (kind[bg] != BG_NONE) {
                shown |= 1u << bg;
            }
        }
        if (dispcnt & 0x1000) {
            shown |= 0x10;
        }
        blend2 = (bldcnt & 0x3F00) &&
                 ((effect == 1 && (bldcnt & shown)) || objFrame->blends || gx3dTranslucent);
    }
    special = (effect != 0 ? (bldcnt & 0x1Fu) : 0) | (gx3dTranslucent ? 0x01u : 0) |
              (objFrame->blends ? 0x10u : 0);
    flags = (anyWindow ? T_WINDOW : 0) | (effect >= 2 ? T_LAYER : 0);
    if (split != NULL && split->has3d) {
        flags |= T_LAYER;   /* (the layers in front of the 3D: which is which) */
    }
    blendState.alphaOut = NULL;
    blendState.width = width;   /* (split: each line's, in SnapshotOver) */
    blendState.e = e;
    blendState.work = work;
    blendState.kind = kind;
    blendState.bgcnt = bgcnt;
    blendState.dispcnt = dispcnt;
    blendState.gx3dPixels = gx3dPixels;
    blendState.gx3dAlpha = gx3dAlpha;
    blendState.alpha3d = &alpha3d;
    blendState.list = list;
    blendState.entryOf = entryOf;
    blendState.objLine = objLine;
    blendState.special = special;
    blendState.bldcnt = bldcnt;
    blendState.backdrop = backdrop;
    blendState.effect = effect;
    blendState.eva = eva;
    blendState.evb = evb;
    blendState.evy = evy;

    for (y = 0; y < H; y++) {
        u16 *row = out + y * width;
        BOOL objs = DrawObjs(e, objFrame, dispcnt, y, objLine);
        Target t;

        /* Which layers and effects each pixel shows (the windows): window
         * 0 over window 1 over the OBJ window over the outside. */
        if (anyWindow) {
            memset(enableLine, winout & 0x3F, (size_t)width);
            if ((dispcnt & 0x8000) && objs) {
                for (x = objLine->x0; x < objLine->x1; x++) {
                    if (objLine->window[x]) {
                        enableLine[x] = (winout >> 8) & 0x3F;
                    }
                }
            }
            if ((dispcnt & 0x4000) && InRange(y, win1v >> 8, win1v & 0xFF)) {
                FillRange(enableLine, win1h >> 8, win1h & 0xFF, (winin >> 8) & 0x3F, e->origin);
            }
            if ((dispcnt & 0x2000) && InRange(y, win0v >> 8, win0v & 0xFF)) {
                FillRange(enableLine, win0h >> 8, win0h & 0xFF, winin & 0x3F, e->origin);
            }
        }

        t.row = row;
        t.layerOf = layer1;
        t.enable = enableLine;
        t.filled = 0;
        memset(row, 0, (size_t)width * sizeof(u16));
        nlist = 0;
        snapped = FALSE;

        /* The layers front to back (the OBJs of priority 0, the BGs of
         * priority 0 by number, the OBJs of priority 1, ...), until every
         * pixel has its front layer: the layers behind aren't even drawn
         * (when two layers blend, only if a pixel needs the one behind). */
        for (p = 0; p < 4; p++) {
            int slot;

            for (slot = -1; slot < 4; slot++) {
                const u16 *px = NULL;
                const u8 *prioOf = NULL;
                int layer, x0 = 0, x1 = width;

                if (slot == 0 && split != NULL && split->has3d && p == split->prio3d) {
                    /* The 3D's place among the layers: what's drawn so far
                     * is in front of it. */
                    SnapshotOver(split, &blendState, row, y, nlist, blend2, flags, effect,
                                 evy, bldcnt, enableLine, layer1);
                    snapped = TRUE;
                    /* What's behind the 3D is drawn without them. */
                    memset(row, 0, (size_t)width * sizeof(u16));
                    t.filled = 0;
                    nlist = 0;
                }
                if (slot < 0) {
                    if (!objs || !(objLine->prios & (1 << p))) {
                        continue;
                    }
                    layer = SLOT_OBJ;
                    prioOf = objLine->prio;
                    x0 = objLine->x0;
                    x1 = objLine->x1;
                } else {
                    bg = slot;
                    if (kind[bg] == BG_NONE || (bgcnt[bg] & 3) != p) {
                        continue;
                    }
                    layer = bg;
                }
                if (blend2) {
                    /* Noted, for the pixels that need the layer behind
                     * their front one: its own line (px) is drawn then. */
                    entryOf[layer == SLOT_OBJ ? 4 + p : layer] = (s8)nlist;
                    list[nlist].layer = (u8)layer;
                    list[nlist].p = (u8)p;
                    list[nlist].px = layer == SLOT_OBJ ? objLine->color : NULL;
                    list[nlist].prioOf = prioOf;
                    list[nlist].x0 = (short)x0;
                    list[nlist].x1 = (short)x1;
                    nlist++;
                }
                if (t.filled >= width) {
                    if (!blend2) {
                        break;
                    }
                    continue;
                }
                t.layer = (u8)layer;
                t.bit = (u8)(1 << layer);
                if (layer == SLOT_OBJ) {
                    px = objLine->color;
                } else if (kind[bg] == BG_TEXT) {
                    /* straight into the line */
                    DrawTextBg(e, bg, bgcnt[bg], dispcnt, y, &t, blend2 ? flags | T_LAYER : flags);
                    continue;
                } else {
                    px = LayerLine(e, work, bg, kind[bg], bgcnt[bg], dispcnt, y,
                                   gx3dPixels, gx3dAlpha, &alpha3d);
                    if (blend2) {
                        list[nlist - 1].px = px;
                    }
                }
                Merge(&t, px, prioOf, p, x0, x1, blend2 ? flags | T_LAYER : flags);
            }
        }

        if (split != NULL && split->has3d && !snapped) {
            /* (the line was full before the 3D's place: everything drawn
             * is in front of it, and nothing behind it was drawn) */
            SnapshotOver(split, &blendState, row, y, nlist, blend2, flags, effect, evy, bldcnt,
                         enableLine, layer1);
            memset(row, 0, (size_t)width * sizeof(u16));
            nlist = 0;
        }
        if (!blend2) {
            if (!(flags & T_LAYER)) {
                if (((uintptr_t)row & 3) == 0) {
                    /* Two pixels at a time: a pixel keeps its colour if
                     * drawn, else takes the backdrop. */
                    u32a *pair = (u32a *)row;
                    u32 back2 = (u32)backdrop * 0x10001u;

                    for (x = 0; x < width / 2; x++) {
                        u32 c = pair[x];
                        u32 drawn = (c >> 15) & 0x10001u;

                        pair[x] = (c & (drawn * 0x7FFFu)) | (back2 & ((drawn ^ 0x10001u) * 0xFFFFu));
                    }
                } else {
                    for (x = 0; x < width; x++) {
                        u16 c = row[x];

                        row[x] = (c & OPAQUE) ? (u16)(c & 0x7FFF) : backdrop;
                    }
                }
            } else {
                /* brighten or darken the layers chosen */
                for (x = 0; x < width; x++) {
                    u16 c = row[x];
                    int top = (c & OPAQUE) ? layer1[x] : 5;

                    c = (c & OPAQUE) ? (u16)(c & 0x7FFF) : backdrop;
                    if ((enableLine[x] & 0x20) && (bldcnt & (1 << top))) {
                        c = effect == 2 ? Brighten(c, evy) : Darken(c, evy);
                    }
                    row[x] = c;
                }
            }
        } else {
            /* Each pixel has its front layer; some blend with the one behind. */
            blendState.y = y;
            blendState.nlist = nlist;
            BlendLine(&blendState, row, enableLine, layer1, anyWindow);
        }
        if (split != NULL && split->has3d && anyWindow) {
            /* Where a window hides the 3D, what's behind it shows. */
            u16 *over = split->over + y * width;
            u8 *alpha = split->overAlpha + y * width;

            for (x = 0; x < width; x++) {
                if (enableLine[x] & 1) {
                    continue;
                }
                if (!(over[x] & OPAQUE)) {
                    over[x] = (u16)(row[x] | OPAQUE);
                } else if (alpha[x] < 16) {
                    /* (a pixel that would blend with the 3D blends with
                     * what's behind it instead) */
                    over[x] = (u16)(BlendIn16(over[x] & 0x7FFF, row[x], alpha[x], 16 - alpha[x]) | OPAQUE);
                }
                alpha[x] = 16;
            }
        }
    }
    /* Release only after every scanline has been composited. */
    if(gx3dPixels)NitroPort_Gx3dReleaseImage(gx3dPixels);
}

static void DrawEngine(Engine *e, u16 *out, NitroRender2DSplit *split)
{
    if (e->width == W) {
        DrawEngineT(e, out, split, W);
    } else {
        DrawEngineT(e, out, split, 0);
    }
}

static void MasterBrightness(u16 *out, u16 reg, int n)
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
        for (i = 0; i < n; i++) {
            out[i] = mode == 1 ? 0x7FFF : 0;
        }
        return;
    }
    for (i = 0; i < n; i++) {
        out[i] = mode == 1 ? Brighten(out[i], factor) : Darken(out[i], factor);
    }
}

static void Fill(u16 *out, u16 color, int n)
{
    int i;

    for (i = 0; i < n; i++) {
        out[i] = color;
    }
}

static void Render(int engine, u16 *out, NitroRender2DSplit *split)
{
    Engine e;
    u32 dispcnt;
    u16 powcnt = IO16(0x304);

    if (split != NULL) {
        memset(split, 0, offsetof(NitroRender2DSplit, over));
        split->masterBright = IO16((engine == 0 ? 0 : 0x1000) + 0x6C);
    }
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
    e.width = W;
    if (split != NULL && split->width > W && split->width <= WMAX) {
        int bg;

        e.width = split->width;
        e.origin = split->origin;
        for (bg = 0; bg < 4; bg++) {
            e.clip[bg] = split->clip[bg];
        }
        e.anchorWindows = (u8)split->anchorWindows;
    }
    dispcnt = Rd32(e.io, 0);

    /* The engine powered off: nothing on that screen. */
    if (!(powcnt & (e.isA ? 0x0002 : 0x0200))) {
        Fill(out, 0, e.width * H);
        return;
    }
    if (dispcnt & 0x80) {                  /* forced blank: white */
        Fill(out, 0x7FFF, e.width * H);
        return;
    }
    switch ((dispcnt >> 16) & (e.isA ? 3 : 1)) {
    case 0:                                /* display off: white */
        Fill(out, 0x7FFF, e.width * H);
        return;
    case 1:
        DrawEngine(&e, out, split);
        break;
    case 2: {                              /* VRAM display: a bank in LCDC */
        const u8 *bank = NitroPort_LcdcVram + ((dispcnt >> 18) & 3) * 0x20000;
        int i;

        Fill(out, 0, e.width * H);
        for (i = 0; i < W * H; i++) {
            out[(i / W) * e.width + e.origin + i % W] = Rd16(bank, (u32)i * 2) & 0x7FFF;
        }
        break;
    }
    default:
        NitroPort_Unimplemented("display mode: main memory display");
        Fill(out, 0, e.width * H);
        return;
    }
    if (split == NULL) {
        MasterBrightness(out, Rd16(e.io, 0x6C), e.width * H);
    }
}

void NitroPort_Render2D(int engine, u16 *out)
{
    Render(engine, out, NULL);
}

void NitroPort_Render2DSplit(int engine, u16 *under, NitroRender2DSplit *split)
{
    Render(engine, under, split);
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
int NitroPort_Render2DSplitIfChanged(int engine, u16 *under, NitroRender2DSplit *split)
{
    /* (its own record: the 3D doesn't come into these layers) */
    static struct { u32 signature; int valid; } last[2];
    int e = engine ? 1 : 0;
    u32 signature = EngineSignature(e);

    /* the canvas, and where the windows are */
    signature = (signature ^ (u32)split->width ^ ((u32)split->origin << 9) ^
                 ((u32)split->anchorWindows << 18) ^ ((u32)split->clip[0] << 20) ^
                 ((u32)split->clip[1] << 21) ^ ((u32)split->clip[2] << 22) ^
                 ((u32)split->clip[3] << 23)) * 16777619u;
    if (split->anchorWindows) {
        signature = (signature ^ NitroPort_WideWindowsSerial()) * 16777619u;
    }
    if (last[e].valid && signature == last[e].signature) {
        return 0;
    }
    last[e].signature = signature;
    last[e].valid = 1;
    Render(engine, under, split);
    return 1;
}

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
