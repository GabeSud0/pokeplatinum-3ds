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
 *   - H-blank effects (NitroPort_LineRegs: the battles' start, a wavy
 *     screen): each line's BG scroll and windows as the game set them for
 *     it, and the 3D's scroll on each line for the compositor
 *     (NitroRender2DSplit.scroll3dLine); the rest of the registers (the
 *     colour effects, the affine BGs' matrices, OAM) as they are at the
 *     frame's end.
 * Not yet: DS 3D on the 3DS GPU (PC shows captured triangles), mosaic, the large
 * bitmap BG of mode 6, the main-memory display mode and display capture
 * (the legendary Pokemon's motion blur).
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
#include <nitro_port_wideart.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define W 256
#define H 192
#define HMAX NITRO_RENDER2D_MAX_H   /* (a tall picture: NitroRender2DSplit.height) */
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

static u8 BankCnt(const u8 *io, int bank)
{
    /* VRAMCNT_A-G at 0x240-0x246, H at 0x248, I at 0x249 */
    return io[bank < 7 ? 0x240 + bank : 0x241 + bank];
}

/* The display state the drawing threads draw from (NitroPort_Render2DTake
 * State): the registers, palettes and OAM as they were when the game
 * handed the frame over, so that what the game does meanwhile (its next
 * frame's sprites, scroll, colours) doesn't get mixed into the picture.
 * The VRAM is read as it is (too big to copy each frame; the game changes
 * it in its V-blank work, and the drawers are waited for before that). */
typedef struct State2D {
    u8 io[0x2000];
    u8 pltt[0x800];
    u8 oam[0x800];
    /* (an H-blank effect: NitroPort_LineRegs) the engine's registers on
     * each line, when they differ */
    int lines;
    u8 lineRegs[H][NITRO_LINE_REGS];
    /* (the wider screen) the windows moved or widened on each BG
     * (NitroPort_WideWindows), as they were: the game takes a menu's frame
     * off before the menu's tiles leave the VRAM (at its next V-blank), so
     * read live the menu showed a frame where the DS has it, 32 pixels to
     * the left of where it had been (2026-10-23); and the screen's layout
     * (NitroPort_WideSplits, NitroPort_WideParts), which the game also sets
     * before the new screen's tiles are in */
    NitroWideWindow wins[4][NITRO_WIDE_MAX_WINDOWS];
    int nwins[4];
    NitroWideSplit splits[NITRO_WIDE_MAX_SPLITS];
    int nsplits;
    NitroWidePart parts[NITRO_WIDE_MAX_PARTS];
    int nparts;
    uint32_t winSerial;
} State2D;
static State2D sState[2];

/* ----------------------------------------------------------- one engine */

#define OBJ_MOVES_MAX 32   /* sprite moves: the layout's, then the moved windows' */
#define WIDE_PICS_MAX 16   /* pictures a layout draws (NITRO_WIDE_PICTURE) */

typedef struct Engine {
    const u8 *ioAll;       /* all the registers (live, or a State2D's) */
    const u8 *io;          /* the engine's registers (A: +0, B: +0x1000) */
    /* (an H-blank effect) its first NITRO_LINE_REGS bytes on each line, or
     * NULL: io's on every line (NitroPort_LineRegs) */
    const u8 (*lines)[NITRO_LINE_REGS];
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
    /* The picture's lines: H, or up to HMAX (NitroRender2DSplit.height). */
    int height;
    /* Wider canvas: each BG's pixels only within the DS's 256 (clip), or
     * wrapping round its map; the windows the game anchors moved to the
     * screen's edges (anchorWindows); and whether this is one of the
     * screens made of pictures (NitroPort_WidePictures: the opening, the
     * title...), where a plain edge goes on only where it's plain for a
     * band's width (PlainRun): nothing of a picture is ever stretched (an
     * oval of the opening's strip touching the edge was drawn out into a
     * line, 2026-10-14). Nothing is mirrored anywhere. */
    u8 clip[4];
    u8 anchorWindows;
    const State2D *taken;  /* (drawn from the display state taken: its windows) */
    u8 pictures;
    /* (wider canvas, worked out each frame) a BG's hand-drawn picture
     * (nitro_port_wideart.h): its pixels, how far the BG has scrolled
     * since it was made, and whether it's the whole picture (else the
     * bands beside the DS's only). */
    const u16 *art[4];
    const u8 *artRows[4];  /* (its rows with anything: NitroPort_WideArtRows; NULL, not known) */
    short artDx[4], artDy[4];
    short artH[4], artV[4];  /* (a text BG's) the scroll its picture was made at */
    u8 artFull[4];
    u8 artBands[4];        /* its bands drawn (else nothing there: the layer is elsewhere) */
    /* (a picture of the bands only of a strip that scrolls round: _loop)
     * where its bands, moved with the layer, bring in its middle, the
     * BG's own pixels go on (they're what the middle was) */
    u8 artLoopBands[4];
    /* A whole picture of a layer that has moved away from where it was made
     * (a Pokemon sliding in): the whole layer from the picture, moved
     * (artDx, artDy: MovedOffset's, the slide made as wide as the screen);
     * artExact: moved as far as the layer (else further), so past the
     * picture's edges the BG's own pixels go on. */
    u8 artMoved[4];
    u8 artExact[4];
    /* (worked out each frame, DecideSplits) a text BG that's a picture with
     * a plain middle and decorated sides: its halves go to the screen's
     * edges, the middle widened with its plain colour. */
    u8 split[4];
    /* (worked out each frame, DecideBandFills: the UI's screens) what goes
     * beside the DS's picture on each line of a text BG, left and right:
     * colour | OPAQUE, or 0 nothing. bandFills[bg]: worked out (else the
     * edges are looked at line by line, as before). */
    u8 bandFills[4];
    u16 bandFill[4][2][H];
    /* (worked out each frame) the windows moved to the screen's edges, on
     * the DS's screen (pixels), and how far: the game's sprites over them
     * go with them (a menu's icons). */
    struct { short x0, y0, x1, y1, shift, stretch; } objMove[OBJ_MOVES_MAX];
    int nObjMove;
    /* (wider canvas, the game's: NitroPort_WideSplitScreen,
     * NitroPort_WideScreenParts) a screen laid out anew in parts: the BGs'
     * MOVE and FILL parts (the splits made into parts first), the BGs they
     * are of (a bit each), those the game moves (they show their own
     * pixels, not a hand-drawn picture), whether sprites are moved (they
     * show themselves, not their pictures), and how far the 3D moves. */
    NitroWidePart parts[NITRO_WIDE_MAX_PARTS + 3 * NITRO_WIDE_MAX_SPLITS];
    int nParts;
    u8 partBgs, movedBgs, objsMoved;
    short shift3d;
    /* (NITRO_WIDE_PICTURE parts) the pictures drawn with the BGs: their
     * pixels, size, top-left on the canvas, turn (cos, sin: 16.16), the
     * canvas rectangle they may cover, and the BG; the BGs that have one. */
    struct {
        const u16 *px;
        short w, h, x, y, x0, y0, x1, y1;
        s32 c, s;
        u8 bg, id;
        short angle;
    } pics[WIDE_PICS_MAX];
    int nPics;
    u8 picBgs;
    /* The palettes as pixels (colour | OPAQUE), 0 for the transparent
     * colour: index 0 of each 16 colours (4 bits per pixel), or of all 256
     * (8 bits per pixel). */
    u16 bgPal4[256];
    /* (NITRO_WIDE_CLEAR parts) the BGs (a bit each) with colours shown
     * clear, which (per palette, a bit each index), and their palettes so */
    u8 clearBgs;
    u16 clearMask[16];
    u16 bgPal4Clear[256];
    u16 bgPal8[256];
    u16 objPal4[256];
    u16 objPal8[256];
} Engine;

/* The windows moved or widened on a BG (NitroPort_WideWindows): as they
 * were when the display state was taken, when drawn from that (as the
 * VRAM's tiles were then too), else as they are. How many. */
static int EngineWindows(const Engine *e, int bg, NitroWideWindow *out)
{
    if (e->taken != NULL) {
        memcpy(out, e->taken->wins[bg], (size_t)e->taken->nwins[bg] * sizeof(*out));
        return e->taken->nwins[bg];
    }
    return NitroPort_WideWindows(e->isA ? 0 : 1, bg, out, NITRO_WIDE_MAX_WINDOWS);
}

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
        u8 cnt = BankCnt(e->ioAll, bank);
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
    if (e->clearBgs) {
        for (i = 0; i < 256; i++) {
            e->bgPal4Clear[i] = ((e->clearMask[i >> 4] >> (i & 15)) & 1) ? 0 : e->bgPal4[i];
        }
    }
}

/* A 16-colour BG's palettes (NITRO_WIDE_CLEAR: some colours clear). */
static inline const u16 *BgPal4(const Engine *e, int bg)
{
    return ((e->clearBgs >> bg) & 1) ? e->bgPal4Clear : e->bgPal4;
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

/* Whether PUT would leave pixel x as it is whatever the colour: a layer in
 * front has it, or the window hides this layer there (not for a layer's
 * own line, T_PLAIN). A pixel worked out per pixel (a hand-drawn picture's)
 * isn't worked out then: on the opening's strips, two of three BGs' picture
 * pixels were behind another's (2026-10-21). */
#define COVERED(x_, flags_)                                                  \
    (!((flags_) & T_PLAIN) &&                                                \
     ((row[(x_)] & OPAQUE) || (((flags_) & T_WINDOW) && !(enable[(x_)] & bit))))

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
    u16 color;     /* (SEG_COLOR) */
} Segment;
#define SEG_COLOR (-7)     /* every pixel this colour (Segment.color) */
#define SEG_ART (-3)       /* the BG's hand-drawn picture (Engine.art), in the bands */
#define SEG_ART_MID (-4)   /* the same over the DS's part: the picture where it has it, else the BG's own */
#define SEG_CLEAR (-5)     /* nothing */
#define SEG_ART_MOVED (-6) /* the whole layer from its picture, moved (Engine.artMoved) */
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

        return BgPal4(e, bg)[((entry >> 12) << 4) | ((px & 1) ? (pair >> 4) : (pair & 15))];
    }
}

/* A pixel of a run of a BG's line as the run draws it (not a hand-drawn
 * picture's): the map's, its colour, or nothing. */
static u16 SegmentPixel(const Engine *e, int bg, u16 bgcnt, u32 dispcnt, const Segment *s, int x, int ty, int mapW)
{
    if (s->fixed >= 0) {
        return TextBgPixel(e, bg, bgcnt, dispcnt, s->fixed, ty);
    }
    if (s->fixed == -1) {
        return TextBgPixel(e, bg, bgcnt, dispcnt, (x + s->off) & (mapW - 1), ty);
    }
    return s->fixed == SEG_COLOR ? s->color : 0;
}

static int TextBgSegments(const Engine *e, int bg, u16 bgcnt, u32 dispcnt, int y, int ty, int hofs, int mapW,
                          Segment *segs, int ownOnly);

/* A text BG's own runs on one row of the canvas, without its hand-drawn
 * picture (TextBgSegments, ownOnly), kept from one pixel to the next:
 * y < 0 till a row's are worked out. */
typedef struct OwnRow {
    Segment segs[MAX_SEGMENTS];
    int n, y;
} OwnRow;
static inline void OwnRowStart(OwnRow *own)
{
    own->n = 0;
    own->y = -1;
}

/* A text BG's own pixel at canvas x, y (any: past the canvas too), the BG
 * drawn as it is, without its hand-drawn picture, at the scroll hofs,
 * vofs: its runs' on the canvas (TextBgSegments, ownOnly: the windows
 * moved and the parts as the port lays it out, the map going on past the
 * DS's edges; where a window was, nothing), the map's on the rows past
 * the canvas. The same as a capture's bg<N>_map.png. */
static u16 OwnTextPixel(const Engine *e, int bg, u16 bgcnt, u32 dispcnt, int hofs, int vofs, int mapW, int mapH,
                        int x, int y, OwnRow *own)
{
    int ty = (y + vofs) & (mapH - 1), k;

    if (y < 0 || y >= H) {
        return TextBgPixel(e, bg, bgcnt, dispcnt, (x - e->origin + hofs) & (mapW - 1), ty);
    }
    if (own->y != y) {
        own->n = TextBgSegments(e, bg, bgcnt, dispcnt, y, ty, hofs, mapW, own->segs, 1);
        own->y = y;
    }
    for (k = 0; k < own->n; k++) {
        if (x >= own->segs[k].x0 && x < own->segs[k].x1) {
            return SegmentPixel(e, bg, bgcnt, dispcnt, &own->segs[k], x, ty, mapW);
        }
    }
    return 0;
}

/* A text BG's hand-drawn picture's pixel (ax, ay: on the canvas, the
 * picture's row ay): its own, or for NITRO_WIDE_ART_GAME (a picture that
 * leaves the game's art out: nitro_port_wideart.h) the BG's own pixel
 * where the picture had it, as far back as the picture moved it: the BG
 * drawn as it is, without the picture, at the scroll the picture was made
 * at (artH, artV: OwnTextPixel), so a BG scrolled or slid since shows the
 * same art (its key) there. */
static inline __attribute__((always_inline)) u16 ArtTextPixel(const Engine *e, int bg, u16 bgcnt, u32 dispcnt, int ax,
                                                              int ay, int mapW, int mapH, OwnRow *own)
{
    u16 c = e->art[bg][ay * NITRO_WIDE_ART_W + ax];

    if (!NitroPort_WideArtIsGame(c)) {
        return c;
    }
    return OwnTextPixel(e, bg, bgcnt, dispcnt, e->artH[bg], e->artV[bg], mapW, mapH,
                        ax - NitroPort_WideArtGameDx(c), ay - NitroPort_WideArtGameDy(c), own);
}

/* Whether a text BG's map has its tile at map x mapx (row ty) flipped
 * sideways: a mirror of another (the battle background's map is its
 * picture, then the same picture mirrored, for its slide-in). */
static BOOL TextBgTileMirrored(const Engine *e, u16 bgcnt, u32 dispcnt, int mapx, int ty)
{
    int mapW = sTextSizes[bgcnt >> 14][0];
    u32 screenBase = ScreenBase(e, bgcnt, dispcnt);
    int tx = mapx & (mapW - 1);
    u32 rowAddr = screenBase + (u32)((ty >> 8) * (mapW >> 8)) * 0x800 + (u32)((ty & 255) >> 3) * 64;
    u32 entry = Rd16(e->bgVram, (rowAddr + (u32)(tx >> 8) * 0x800 + (u32)((tx & 255) >> 3) * 2) & e->bgMask);

    return (entry & 0x400) != 0;
}

/* How much of a line at the DS's edge must be one colour for that colour
 * to go on beside it: a band's width (32) on the screens made of pictures,
 * where anything shorter is part of the picture (the opening strip's ovals
 * and streaks were drawn out into lines), 8 elsewhere (a window's frame, a
 * fade's black). */
static inline int PlainRun(const Engine *e)
{
    return e->pictures ? 32 : 8;
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
        if (kind[bg] != BG_TEXT || !e->clip[bg] || ((e->partBgs >> bg) & 1)) {
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

/* (wider canvas, each frame) Engine.bandFill: on each line of a text BG,
 * beside the DS's picture, the colour of its edge where that's plain
 * (PlainRun); and on the UI's screens, on lines where the BG has something
 * at its edge that isn't plain (a menu's panel reaching it), the plain
 * lines next to them carried on through them: the pattern of those just
 * above (or below), every 1 to 8 lines (a background's stripes go on
 * behind the panel). Beside a party list's panels and a
 * summary's boxes, the bands were black (nothing there: the backdrop),
 * 2026-10-15. */
static void DecideBandFills(Engine *e, const int *kind, const u16 *bgcnt, u32 dispcnt)
{
    int bg, side;

    for (bg = 0; bg < 4; bg++) {
        int mapH, hofs, vofs, y, run = PlainRun(e);
        u16 own[2][H];
        u8 edge[2][H];   /* the BG has something at its edge, not plain */

        e->bandFills[bg] = 0;
        if (e->width <= W || kind[bg] != BG_TEXT || !e->clip[bg] || e->art[bg] != NULL || e->split[bg] ||
            ((e->partBgs >> bg) & 1)) {
            continue;
        }
        mapH = sTextSizes[bgcnt[bg] >> 14][1];
        hofs = Rd16(e->io, 0x10 + bg * 4) & 0x1FF;
        vofs = Rd16(e->io, 0x12 + bg * 4) & 0x1FF;
        e->bandFills[bg] = 1;
        for (side = 0; side < 2; side++) {
            int x0 = side ? W - run : 0;

            for (y = 0; y < H; y++) {
                int ty = (y + vofs) & (mapH - 1);

                own[side][y] = TextBgPlain(e, bg, bgcnt[bg], dispcnt, hofs, ty, x0, run)
                                   ? TextBgPixel(e, bg, bgcnt[bg], dispcnt, x0 + hofs + (side ? run - 1 : 0), ty)
                                   : 0;
                edge[side][y] = !own[side][y] &&
                                (TextBgPixel(e, bg, bgcnt[bg], dispcnt, (side ? W - 1 : 0) + hofs, ty) & OPAQUE);
                e->bandFill[bg][side][y] = own[side][y];
            }
        }
        if (e->pictures) {
            continue;   /* (the pictures' screens: plain edges only) */
        }
        for (side = 0; side < 2; side++) {
            y = 0;
            while (y < H) {
                int a, b, k, p, up, down, src, dir, len;

                if (!edge[side][y]) {
                    y++;
                    continue;
                }
                /* a run of such lines, a .. b - 1 */
                for (a = y, b = y; b < H && edge[side][b]; b++) {
                }
                y = b;
                /* the plain lines next to it, above and below (as many as
                 * there are in a row, 16 at most) */
                for (up = 0; up < 16 && a - 1 - up >= 0 && own[side][a - 1 - up]; up++) {
                }
                for (down = 0; down < 16 && b + down < H && own[side][b + down]; down++) {
                }
                /* carried on from above (from below if there are fewer
                 * than two plain lines above): the pattern of the plain
                 * lines next to the run (every 1 to 8 lines: the shortest
                 * that repeats over the nearest twice its length, 4 lines
                 * at least), repeated through the run */
                if (up == 0 && down == 0) {
                    continue;
                }
                dir = up >= 2 || down == 0 ? -1 : 1;
                len = dir < 0 ? up : down;
                src = dir < 0 ? a - 1 : b;   /* the plain line next to the run */
                for (p = 1; p < 8 && p < len; p++) {
                    int ok = 1, span = 2 * p > 4 ? 2 * p : 4;

                    if (span > len) {
                        span = len;
                    }
                    for (k = 0; k + p < span && ok; k++) {
                        ok = own[side][src + dir * k] == own[side][src + dir * (k + p)];
                    }
                    if (ok) {
                        break;
                    }
                }
                if (p > len) {
                    p = len;
                }
                for (k = a; k < b; k++) {
                    /* line k: as far into the run as it is, a whole number
                     * of patterns back into the plain lines */
                    int into = dir < 0 ? k - a : b - 1 - k;   /* 0: next to them */
                    int back = (into % p);

                    e->bandFill[bg][side][k] = own[side][src + dir * (p - 1 - back)];
                }
            }
        }
    }
}

/* sin(degrees) in 16.16 (without the maths library: a few terms of its
 * series, on 0-90 degrees). */
static s32 SinFx(int degrees)
{
    int d = degrees % 360, sign = 1;
    float x, x2, v;

    if (d < 0) {
        d += 360;
    }
    if (d >= 180) {
        d -= 180;
        sign = -1;
    }
    if (d > 90) {
        d = 180 - d;
    }
    x = (float)d * 0.017453292f;
    x2 = x * x;
    v = x * (1.0f - x2 / 6.0f * (1.0f - x2 / 20.0f * (1.0f - x2 / 42.0f * (1.0f - x2 / 72.0f))));
    return sign * (s32)(v * 65536.0f + 0.5f);
}

/* (wider canvas) A picture a layout draws (NITRO_WIDE_PICTURE): where, with
 * which BG, turned how far; left out if its file isn't there. */
static void SetUpPicture(Engine *e, const NitroWidePart *p)
{
    int w, h, dx, dy, bg, r;
    const u16 *px;

    if (e->nPics >= WIDE_PICS_MAX || (px = NitroPort_WideArtPicture(p->col, &w, &h, &dx, &dy)) == NULL) {
        return;
    }
    for (bg = 0; bg < 4 && !((p->bgs >> bg) & 1); bg++) {
    }
    if (bg == 4) {
        return;
    }
    e->pics[e->nPics].px = px;
    e->pics[e->nPics].w = (short)w;
    e->pics[e->nPics].h = (short)h;
    e->pics[e->nPics].x = (short)(e->origin + p->x0 + dx);   /* (moved from its place in the editor) */
    e->pics[e->nPics].y = (short)(p->top + dy);
    e->pics[e->nPics].bg = (u8)bg;
    e->pics[e->nPics].id = (u8)p->col;
    e->pics[e->nPics].angle = p->dx;
    e->pics[e->nPics].s = SinFx(p->dx);
    e->pics[e->nPics].c = SinFx(p->dx + 90);
    if (p->dx % 360 == 0) {
        e->pics[e->nPics].c = 0x10000;
        e->pics[e->nPics].s = 0;
        e->pics[e->nPics].x0 = e->pics[e->nPics].x;
        e->pics[e->nPics].y0 = e->pics[e->nPics].y;
        e->pics[e->nPics].x1 = (short)(e->pics[e->nPics].x + w);
        e->pics[e->nPics].y1 = (short)(e->pics[e->nPics].y + h);
    } else {
        /* (turned: within its middle's distance from its corners, at most
         * 3/4 of its longer side) */
        r = (w > h ? w : h) * 3 / 4 + 1;
        e->pics[e->nPics].x0 = (short)(e->pics[e->nPics].x + w / 2 - r);
        e->pics[e->nPics].y0 = (short)(e->pics[e->nPics].y + h / 2 - r);
        e->pics[e->nPics].x1 = (short)(e->pics[e->nPics].x + (w + 1) / 2 + r);
        e->pics[e->nPics].y1 = (short)(e->pics[e->nPics].y + (h + 1) / 2 + r);
    }
    e->picBgs |= (u8)(1 << bg);
    e->nPics++;
}

/* The pictures drawn with BG bg on line y (NITRO_WIDE_PICTURE): over line,
 * where they have a pixel (later ones over earlier ones), the columns they
 * may cover in *x0..*x1 (cleared first when clear). Returns whether any is
 * on the line. */
static BOOL PicturesOnLine(const Engine *e, int bg, int y, u16 *line, BOOL clear, int *x0, int *x1)
{
    int i, x, a = e->width, b = 0;

    for (i = 0; i < e->nPics; i++) {
        if (e->pics[i].bg == bg && y >= e->pics[i].y0 && y < e->pics[i].y1) {
            if (e->pics[i].x0 < a) {
                a = e->pics[i].x0;
            }
            if (e->pics[i].x1 > b) {
                b = e->pics[i].x1;
            }
        }
    }
    if (a < 0) {
        a = 0;
    }
    if (b > e->width) {
        b = e->width;
    }
    if (a >= b) {
        return FALSE;
    }
    if (clear) {
        memset(line + a, 0, (size_t)(b - a) * sizeof(u16));
    }
    for (i = 0; i < e->nPics; i++) {
        const u16 *px = e->pics[i].px;
        int w = e->pics[i].w, h = e->pics[i].h, px0 = e->pics[i].x0, px1 = e->pics[i].x1;

        if (e->pics[i].bg != bg || y < e->pics[i].y0 || y >= e->pics[i].y1) {
            continue;
        }
        if (px0 < 0) {
            px0 = 0;
        }
        if (px1 > e->width) {
            px1 = e->width;
        }
        if (e->pics[i].s == 0 && e->pics[i].c == 0x10000) {
            const u16 *row = px + (y - e->pics[i].y) * w - e->pics[i].x;

            for (x = px0; x < px1; x++) {
                if (row[x]) {
                    line[x] = row[x];
                }
            }
        } else {
            /* Turned about its middle: each pixel from the picture's pixel
             * there turned back (16.16). */
            s32 c = e->pics[i].c, s = e->pics[i].s;
            s32 cx = ((s32)e->pics[i].x << 16) + (w << 15), cy = ((s32)e->pics[i].y << 16) + (h << 15);
            s32 dy = ((s32)y << 16) + 0x8000 - cy;

            for (x = px0; x < px1; x++) {
                s32 dx = ((s32)x << 16) + 0x8000 - cx;
                s32 u = (s32)(((s64)c * dx + (s64)s * dy) >> 16) + (w << 15);
                s32 v = (s32)(((s64)c * dy - (s64)s * dx) >> 16) + (h << 15);
                int sx = u >> 16, sy = v >> 16;

                if (u >= 0 && v >= 0 && sx < w && sy < h && px[sy * w + sx]) {
                    line[x] = px[sy * w + sx];
                }
            }
        }
    }
    *x0 = a;
    *x1 = b;
    return TRUE;
}

/* (wider canvas: a screen laid out anew, NitroPort_WideSplitScreen and
 * NitroPort_WideScreenParts) A run of a line of a BG with parts: canvas
 * x c0..c1-1 showing the BG's column c - origin - shift (+ its scroll), or
 * its column col (col >= 0). */
typedef struct PartRun {
    short c0, c1, shift, col;
} PartRun;
#define PART_RUNS_MAX 24
#define PART_LINE_MAX 48   /* parts on one line of a BG (more: left out) */

/* A part's column x as a column of the BG's picture: the DS screen's
 * column x (NITRO_WIDE_SCREEN_COLUMNS) is the picture's x + hofs. */
static int PartX(const NitroWidePart *p, int x, int hofs)
{
    return (p->bgs & NITRO_WIDE_SCREEN_COLUMNS) && x > -0x7FFF && x < 0x7FFF ? x + hofs : x;
}

/* The runs of line ty of BG bg (its own lines and columns; hofs: a text
 * BG's scroll, 0 for an affine BG's picture), left to right: how many; -1
 * if no part is on the line (where the DS has it, nothing beside). The
 * DS's picture where it is, but for the columns the MOVEs take (only the
 * DS's columns, hofs..hofs + 255); over it each part in turn. */
static int PartRuns(const Engine *e, int bg, int ty, int hofs, PartRun *runs)
{
    struct { short c0, c1, prec, shift, col; } iv[2 * PART_LINE_MAX + 1];
    short cut[2 * (2 * PART_LINE_MAX + 1)];
    int niv = 0, ncut = 0, i, k, n = 0, origin = e->origin, width = e->width;

    for (i = 0; i < e->nParts && niv < PART_LINE_MAX; i++) {
        const NitroWidePart *p = &e->parts[i];
        int c0, c1, px0 = PartX(p, p->x0, hofs), px1 = PartX(p, p->x1, hofs);

        if (!((p->bgs >> bg) & 1) || ty < p->top || ty >= p->bottom) {
            continue;
        }
        if (p->kind == NITRO_WIDE_MOVE) {
            int x0 = px0 > hofs ? px0 : hofs, x1 = px1 < hofs + W ? px1 : hofs + W;

            c0 = origin + x0 - hofs + p->dx;
            c1 = origin + x1 - hofs + p->dx;
            iv[niv].shift = p->dx;
            iv[niv].col = -1;
        } else if (p->kind == NITRO_WIDE_SHOW) {
            /* (the map going on beside the DS's screen: the whole canvas) */
            c0 = 0;
            c1 = width;
            iv[niv].shift = 0;
            iv[niv].col = -1;
        } else {
            c0 = origin + px0 - hofs;
            c1 = origin + px1 - hofs;
            iv[niv].shift = 0;
            iv[niv].col = p->col;
        }
        iv[niv].prec = i;
        if (c0 < 0) {
            c0 = 0;
        }
        if (c1 > width) {
            c1 = width;
        }
        if (c0 >= c1) {
            iv[niv].c0 = iv[niv].c1 = 0;
            niv++;   /* (on the line, but off the canvas) */
            continue;
        }
        iv[niv].c0 = c0;
        iv[niv].c1 = c1;
        cut[ncut++] = c0;
        cut[ncut++] = c1;
        niv++;
    }
    if (niv == 0) {
        return -1;
    }
    /* Under them, the DS's picture where it is, but for what the MOVEs
     * took from it (their columns aren't shown twice). */
    {
        int src[PART_LINE_MAX][2], nsrc = 0, from = hofs, j;

        for (i = 0; i < e->nParts && nsrc < PART_LINE_MAX; i++) {
            const NitroWidePart *p = &e->parts[i];
            int px0 = PartX(p, p->x0, hofs), px1 = PartX(p, p->x1, hofs);
            int x0 = px0 > hofs ? px0 : hofs, x1 = px1 < hofs + W ? px1 : hofs + W;

            if (p->kind != NITRO_WIDE_MOVE || !((p->bgs >> bg) & 1) || ty < p->top || ty >= p->bottom || x0 >= x1) {
                continue;
            }
            /* (in order of their starts) */
            for (j = nsrc; j > 0 && src[j - 1][0] > x0; j--) {
                src[j][0] = src[j - 1][0];
                src[j][1] = src[j - 1][1];
            }
            src[j][0] = x0;
            src[j][1] = x1;
            nsrc++;
        }
        for (j = 0; j <= nsrc; j++) {
            int to = j < nsrc ? src[j][0] : hofs + W;

            if (to > from) {
                int c0 = origin + from - hofs, c1 = origin + to - hofs;

                if (c0 < 0) {
                    c0 = 0;
                }
                if (c1 > width) {
                    c1 = width;
                }
                if (c0 < c1) {
                    iv[niv].c0 = c0;
                    iv[niv].c1 = c1;
                    iv[niv].prec = -1;
                    iv[niv].shift = 0;
                    iv[niv].col = -1;
                    cut[ncut++] = c0;
                    cut[ncut++] = c1;
                    niv++;
                }
            }
            if (j < nsrc && src[j][1] > from) {
                from = src[j][1];
            }
        }
    }
    /* the places where what shows may change, in order */
    for (i = 1; i < ncut; i++) {
        int v = cut[i];

        for (k = i; k > 0 && cut[k - 1] > v; k--) {
            cut[k] = cut[k - 1];
        }
        cut[k] = v;
    }
    for (i = 0; i + 1 < ncut; i++) {
        int c0 = cut[i], c1 = cut[i + 1], best = -1;

        if (c0 == c1) {
            continue;
        }
        for (k = 0; k < niv; k++) {
            if (iv[k].c0 < iv[k].c1 && iv[k].c0 <= c0 && iv[k].c1 >= c1 && (best < 0 || iv[k].prec > iv[best].prec)) {
                best = k;
            }
        }
        if (best < 0) {
            continue;
        }
        if (n > 0 && runs[n - 1].c1 == c0 && runs[n - 1].shift == iv[best].shift &&
            runs[n - 1].col == iv[best].col) {
            runs[n - 1].c1 = (short)c1;
            continue;
        }
        if (n == PART_RUNS_MAX) {
            break;
        }
        runs[n].c0 = (short)c0;
        runs[n].c1 = (short)c1;
        runs[n].shift = (short)iv[best].shift;
        runs[n].col = (short)iv[best].col;
        n++;
    }
    return n;
}

/* A text BG with parts: its line's runs as segments (a line without parts
 * where the DS has it). */
static int PartSegments(const Engine *e, int bg, int ty, int hofs, int mapW, Segment *segs)
{
    PartRun runs[PART_RUNS_MAX];
    int n = PartRuns(e, bg, ty, hofs, runs), i;

    if (n < 0) {
        segs[0].x0 = (short)e->origin;
        segs[0].x1 = (short)(e->origin + W);
        segs[0].off = (short)(hofs - e->origin);
        segs[0].fixed = -1;
        return 1;
    }
    for (i = 0; i < n && i < MAX_SEGMENTS; i++) {
        segs[i].x0 = runs[i].c0;
        segs[i].x1 = runs[i].c1;
        segs[i].off = (short)(hofs - e->origin - runs[i].shift);
        segs[i].fixed = (short)(runs[i].col >= 0 ? (runs[i].col & (mapW - 1)) : -1);
    }
    return i;
}

/* The runs of a text BG's line (y, its map row ty) on a wider canvas: the
 * DS's 256 pixels (or the whole canvas, wrapping round the map, without
 * clip), with the windows the game anchors (NitroPort_WideWindows: the
 * place name sign) moved to the screen's edge. Beside the DS's picture (with
 * clip): its edge colour when the edge is plain on this line (PlainRun),
 * else nothing. A BG with parts: its runs (PartSegments). Returns how
 * many. */
static int TextBgSegments(const Engine *e, int bg, u16 bgcnt, u32 dispcnt, int y, int ty, int hofs, int mapW,
                          Segment *segs, int ownOnly)
{
    int origin = e->origin, width = e->width, n = 0, i, k;
    NitroWideWindow wins[NITRO_WIDE_MAX_WINDOWS];
    int nwins = 0;
    struct { int x0, x1, shift, off; } moved[NITRO_WIDE_MAX_WINDOWS];
    int nmoved = 0;

    /* (ownOnly: the BG's own runs, as if it had no hand-drawn picture: for
     * the picture's pixels that are the game's own, ArtTextPixel) */
    if (e->art[bg] != NULL && !ownOnly) {
        /* A hand-drawn picture: the bands (or all of it) from that. */
        if (e->artMoved[bg]) {
            segs[0].x0 = 0;
            segs[0].x1 = (short)width;
            segs[0].off = (short)(hofs - origin);
            segs[0].fixed = SEG_ART_MOVED;
            return 1;
        }
        if (origin > 0) {
            segs[n].x0 = 0;
            segs[n].x1 = (short)origin;
            segs[n].off = 0;
            segs[n].fixed = e->artBands[bg] ? SEG_ART : SEG_CLEAR;
            n++;
        }
        if (!e->artFull[bg] && ((e->partBgs >> bg) & 1)) {
            /* (a BG laid out in parts, its bands drawn: the parts over the
             * DS's part) */
            Segment mid[MAX_SEGMENTS];
            int m = PartSegments(e, bg, ty, hofs, mapW, mid);

            for (i = 0; i < m && n < MAX_SEGMENTS - 1; i++) {
                int a = mid[i].x0 > origin ? mid[i].x0 : origin;
                int b = mid[i].x1 < origin + 256 ? mid[i].x1 : origin + 256;

                if (a < b) {
                    segs[n] = mid[i];
                    segs[n].x0 = (short)a;
                    segs[n].x1 = (short)b;
                    n++;
                }
            }
        } else {
            segs[n].x0 = (short)origin;
            segs[n].x1 = (short)(origin + 256);
            segs[n].off = (short)(hofs - origin);
            segs[n].fixed = e->artFull[bg] ? SEG_ART_MID : -1;
            n++;
        }
        if (origin + 256 < width) {
            segs[n].x0 = (short)(origin + 256);
            segs[n].x1 = (short)width;
            segs[n].off = 0;
            segs[n].fixed = e->artBands[bg] ? SEG_ART : SEG_CLEAR;
            n++;
        }
        return n;
    }
    if ((e->partBgs >> bg) & 1) {
        return PartSegments(e, bg, ty, hofs, mapW, segs);
    }
    if (e->split[bg] && !ownOnly) {
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
        nwins = EngineWindows(e, bg, wins);
    }
    for (i = 0; i < nwins; i++) {
        int top = wins[i].top * 8, bottom = (wins[i].top + wins[i].height) * 8;
        int a, b, shift;

        if (ty < top || ty >= bottom) {
            continue;
        }
        if (wins[i].widen) {
            /* A text box across the DS's screen: its frame's left part at
             * the screen's left edge, its right part at the right edge, the
             * window (its text) a pixel inside the frame's left part (the
             * room the wider box has is on the right of the text, as drawn
             * for it, 2026-10-16), and between, each line of the frame's
             * inner columns drawn on (its top and bottom borders, the box's
             * inside): on the right, the window's last column (its top and
             * bottom borders and the inside, whatever the frame: not the
             * right part's first column, which is the frame's own drawing
             * in many of the 20 frames, a red or a dark band drawn on). */
            int fl = wins[i].frameLeft * 8, fr = wins[i].frameRight * 8, ind = 0;
            int m0 = wins[i].left * 8, m1 = (wins[i].left + wins[i].width) * 8;
            int in0 = origin + m0 - hofs + fl, in1 = origin + m1 - hofs - fr, move;

            if (in0 < fl || in1 > width - fr || in0 > in1) {
                continue;   /* (not across the DS's screen as it is scrolled) */
            }
            move = fl + 1 - in0;
            if (move > 0) {
                move = 0;
            }
            in0 += move;
            in1 += move;
            /* (the game's screen indicator in the window's last 3 tiles,
             * its first 32 lines: with the frame's right part, the plain
             * inside drawn on from the column before it) */
            if (wins[i].indicator && ty >= top + 8 && ty < top + 8 + 32 && in1 - 24 >= in0) {
                ind = 24;
            }
            n = 0;
            segs[n].x0 = 0;
            segs[n].x1 = (short)fl;
            segs[n].off = (short)m0;
            segs[n].fixed = -1;
            n++;
            if (fl < in0) {
                segs[n].x0 = (short)fl;
                segs[n].x1 = (short)in0;
                segs[n].off = 0;
                segs[n].fixed = (short)((m0 + fl - 1) & (mapW - 1));
                n++;
            }
            segs[n].x0 = (short)in0;
            segs[n].x1 = (short)(in1 - ind);
            segs[n].off = (short)(hofs - origin - move);
            segs[n].fixed = -1;
            n++;
            if (in1 < width - fr) {
                segs[n].x0 = (short)(in1 - ind);
                segs[n].x1 = (short)(width - fr - ind);
                segs[n].off = 0;
                segs[n].fixed = (short)((m1 - fr - ind - 1) & (mapW - 1));
                n++;
            }
            segs[n].x0 = (short)(width - fr - ind);
            segs[n].x1 = (short)width;
            segs[n].off = (short)(m1 - width);
            segs[n].fixed = -1;
            n++;
            return n;
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
        if (wins[i].anchor < 0) {
            shift = -origin;                 /* to the left edge */
        } else {
            shift = width - 256 - origin;    /* to the right edge */
        }
        moved[nmoved].x0 = a + origin;
        moved[nmoved].x1 = b + origin;
        moved[nmoved].shift = shift;
        moved[nmoved].off = hofs - origin - shift;
        nmoved++;
    }
    /* The BG's own pixels, less where the windows were and where they went;
     * beside them, its edge's colour when its edge is plain on this line
     * (a picture's border, say: widened without anything stretched).
     * (ownOnly: as if without clip, its map going on past the DS's edges
     * and no colour beside it, nor split: the BG's own pixels as a capture's
     * bg<N>_map.png has them, what a picture's pixels are told from.) */
    {
        int clip = e->clip[bg] && !ownOnly;
        int bx0 = clip ? origin : 0, bx1 = clip ? origin + 256 : width;
        int cuts[2 * NITRO_WIDE_MAX_WINDOWS][2], ncuts = 0, x, run = PlainRun(e);
        /* (the colour beside it: worked out for the frame, DecideBandFills,
         * or looked at here) */
        u16 bandL = e->bandFills[bg] ? e->bandFill[bg][0][y] : 0, bandR = e->bandFills[bg] ? e->bandFill[bg][1][y] : 0;
        int fillLeft = clip && origin > 0 &&
                       (e->bandFills[bg] ? bandL != 0 : TextBgPlain(e, bg, bgcnt, dispcnt, hofs, ty, 0, run));
        int fillRight = clip && origin + 256 < width &&
                        (e->bandFills[bg] ? bandR != 0 : TextBgPlain(e, bg, bgcnt, dispcnt, hofs, ty, 256 - run, run));

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
            if (!clip && (x < origin || x >= origin + 256)) {
                /* A BG whose map goes on past the DS's edges (the battle's
                 * background): there, tile by tile, the map's own pixels
                 * where it has a picture of its own (the player's
                 * platform's left part, which the game puts there; it was
                 * drawn out from the DS's edge into lines, every terrain's,
                 * 2026-10-16), and where the map only has the picture
                 * mirrored, the map's pixel next to the DS's edge
                 * repeated (nothing mirrored): that's the background's own
                 * edge, as it was before the game drew the platforms into
                 * it (the DS's edge pixel is the platform's, where a
                 * platform reaches the edge: the Elite Four's). */
                short edge = (short)((x < origin ? hofs - 1 : 256 + hofs) & (mapW - 1));

                while (x < next && n < MAX_SEGMENTS - nmoved) {
                    int mx = x + hofs - origin, end = x + 8 - (mx & 7);
                    short fixed = TextBgTileMirrored(e, bgcnt, dispcnt, mx, ty) ? edge : -1;

                    if (end > next) {
                        end = next;
                    }
                    if (n > 0 && segs[n - 1].x1 == x && segs[n - 1].fixed == fixed) {
                        segs[n - 1].x1 = (short)end;
                    } else {
                        segs[n].x0 = (short)x;
                        segs[n].x1 = (short)end;
                        segs[n].off = (short)(hofs - origin);
                        segs[n].fixed = fixed;
                        n++;
                    }
                    x = end;
                }
                continue;
            }
            segs[n].x0 = (short)x;
            segs[n].x1 = (short)next;
            segs[n].off = (short)(hofs - origin);
            if (x < origin && e->bandFills[bg] && clip) {
                segs[n].fixed = SEG_COLOR;
                segs[n].color = bandL;
            } else if (x >= origin + 256 && e->bandFills[bg] && clip) {
                segs[n].fixed = SEG_COLOR;
                segs[n].color = bandR;
            } else if (x < origin) {
                segs[n].fixed = (short)(hofs & (mapW - 1));
            } else if (x >= origin + 256) {
                segs[n].fixed = (short)((255 + hofs) & (mapW - 1));
            } else {
                segs[n].fixed = -1;
            }
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

/* A place in a hand-drawn picture (size pixels across) where the BG's map
 * wraps every period pixels: a place past the picture's edge is the same
 * place of the map a period on, if that's in the picture. -1 if none. */
static inline int ArtWrap(int a, int size, int period)
{
    if (a >= 0 && a < size) {
        return a;
    }
    a %= period;
    if (a < 0) {
        a += period;
    }
    return a < size ? a : -1;
}

/* ArtWrap of a, a + 1, a + 2, ... along a line: the first worked out, the
 * rest stepped. (A division a pixel past a picture's edge: the 3DS's ARM11
 * has no divide instruction, and the opening's pictures did tens of
 * thousands a frame.) */
typedef struct ArtRun {
    int a, m, size, period;
} ArtRun;

static inline void ArtRunStart(ArtRun *r, int a, int size, int period)
{
    r->a = a;
    r->m = a % period;
    if (r->m < 0) {
        r->m += period;
    }
    r->size = size;
    r->period = period;
}

static inline int ArtRunNext(ArtRun *r)
{
    int v = (unsigned)r->a < (unsigned)r->size ? r->a : r->m < r->size ? r->m : -1;

    r->a++;
    if (++r->m == r->period) {
        r->m = 0;
    }
    return v;
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
    OwnRow own;   /* (the BG's runs where its picture was made, for ArtTextPixel) */
    int nsegs, seg;
    TARGET_LOCALS(t);

    OwnRowStart(&own);

    if (e->width == W && e->origin == 0 && !e->anchorWindows) {
        /* the DS's own screen: one run */
        segs[0].x0 = 0;
        segs[0].x1 = W;
        segs[0].off = (short)hofs;
        segs[0].fixed = -1;
        nsegs = 1;
    } else {
        nsegs = TextBgSegments(e, bg, bgcnt, dispcnt, y, ty, hofs, mapW, segs, 0);
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
    if (segs[seg].fixed == SEG_ART) {
        /* the hand-drawn picture, moved by the scroll since it was made;
         * past its edge, round the map as the BG itself goes (a scrolling
         * strip goes on). A strip's picture of its bands only, moved so far
         * that its middle comes in: there the BG's own pixels, what the
         * middle was (the opening's blue strip: else nothing beside the
         * DS's screen but where its bands had come round, 2026-10-23). */
        const u16 *art = e->art[bg];
        int ay = ArtWrap(y + e->artDy[bg], H, mapH);
        int loop = e->artLoopBands[bg];
        ArtRun run;

        if (ay < 0 || (e->artRows[bg] != NULL && !e->artRows[bg][ay])) {
            continue;   /* (nothing on this row of it: nothing to put) */
        }
        ArtRunStart(&run, x + e->artDx[bg], NITRO_WIDE_ART_W, mapW);
        (void)art;
        for (; x < xe; x++) {
            int ax = ArtRunNext(&run);

            if (COVERED(x, flags)) {
                continue;
            }
            if (loop && ax >= e->origin && ax < e->origin + W) {
                PUT(x, TextBgPixel(e, bg, bgcnt, dispcnt, (x + hofs - e->origin) & (mapW - 1), ty), flags);
                continue;
            }
            PUT(x,
                (ay >= 0 && ax >= 0)
                    ? ArtTextPixel(e, bg, bgcnt, dispcnt, ax, ay, mapW, mapH, &own)
                    : 0,
                flags);
        }
        continue;
    }
    if (segs[seg].fixed == SEG_CLEAR) {
        for (; x < xe; x++) {
            PUT(x, 0, flags);
        }
        continue;
    }
    if (segs[seg].fixed == SEG_COLOR) {
        u16 c = segs[seg].color;

        for (; x < xe; x++) {
            PUT(x, c, flags);
        }
        continue;
    }
    if (segs[seg].fixed == SEG_ART_MID) {
        /* Over the DS's part, only what the picture has of the DS's part
         * (its middle when it was made): what was drawn in its bands is
         * made up, and scrolled in here it would show where the DS shows
         * the map (the opening's trees, drawn on in the bands, came in
         * when their layer scrolled away). Elsewhere the BG's own. */
        const u16 *art = e->art[bg];
        int ay = ArtWrap(y + e->artDy[bg], H, mapH);
        ArtRun run;

        int lo = ay >= 0 ? e->origin : 0, hi = ay >= 0 ? e->origin + W : 0;
        /* (nothing on this row of it: nothing where it's the picture's) */
        BOOL empty = ay >= 0 && e->artRows[bg] != NULL && !e->artRows[bg][ay];
        int dx = e->artDx[bg];

        if (empty && x + dx >= 0 && xe - 1 + dx < NITRO_WIDE_ART_W) {
            /* (an empty row of it, not going round: the picture's part,
             * x lo - dx .. hi - dx, has nothing; the rest is the BG's own) */
            int m0 = lo - dx, m1 = hi - dx;

            for (; x < xe; x++) {
                if (x >= m0 && x < m1) {
                    x = m1 - 1;
                    continue;
                }
                if (!COVERED(x, flags)) {
                    PUT(x, TextBgPixel(e, bg, bgcnt, dispcnt, (x + off) & (mapW - 1), ty), flags);
                }
            }
            continue;
        }
        ArtRunStart(&run, x + dx, NITRO_WIDE_ART_W, mapW);
        (void)art;
        for (; x < xe; x++) {
            int ax = ArtRunNext(&run);

            if (COVERED(x, flags)) {
                continue;
            }
            if (ax >= lo && ax < hi) {
                if (!empty) {
                    PUT(x, ArtTextPixel(e, bg, bgcnt, dispcnt, ax, ay, mapW, mapH, &own), flags);
                }
            } else {
                PUT(x, TextBgPixel(e, bg, bgcnt, dispcnt, (x + off) & (mapW - 1), ty), flags);
            }
        }
        continue;
    }
    if (segs[seg].fixed == SEG_ART_MOVED) {
        /* the whole layer from its picture, moved; past the picture's
         * edges the BG's own pixels (when moved as far as the BG: the
         * picture is of the map there), else nothing */
        const u16 *art = e->art[bg];
        int ay = ArtWrap(y + e->artDy[bg], H, mapH);

        BOOL empty = ay >= 0 && e->artRows[bg] != NULL && !e->artRows[bg][ay];

        for (; x < xe; x++) {
            int ax = x + e->artDx[bg];

            (void)art;
            if (COVERED(x, flags)) {
                continue;
            }
            if (ay >= 0 && ax >= 0 && ax < NITRO_WIDE_ART_W) {
                if (!empty) {
                    PUT(x, ArtTextPixel(e, bg, bgcnt, dispcnt, ax, ay, mapW, mapH, &own), flags);
                }
            } else if (e->artExact[bg]) {
                PUT(x, TextBgPixel(e, bg, bgcnt, dispcnt, (x + off) & (mapW - 1), ty), flags);
            } else {
                PUT(x, 0, flags);
            }
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
            } else if (n == 8) {
                /* a whole tile's row: its 8 bytes as two words (the
                 * battles' backgrounds) */
                u32 lo = Rd32(chars, 0), hi = Rd32(chars, 4);
                const u16 *pal8 = e->bgPal8;

                if ((lo | hi) == 0) {
                    if (flags & T_PLAIN) {
                        memset(row + x, 0, 16);
                    }
                } else if (!flip) {
                    PUT(x + 0, pal8[lo & 255], flags);
                    PUT(x + 1, pal8[(lo >> 8) & 255], flags);
                    PUT(x + 2, pal8[(lo >> 16) & 255], flags);
                    PUT(x + 3, pal8[lo >> 24], flags);
                    PUT(x + 4, pal8[hi & 255], flags);
                    PUT(x + 5, pal8[(hi >> 8) & 255], flags);
                    PUT(x + 6, pal8[(hi >> 16) & 255], flags);
                    PUT(x + 7, pal8[hi >> 24], flags);
                } else {
                    PUT(x + 0, pal8[hi >> 24], flags);
                    PUT(x + 1, pal8[(hi >> 16) & 255], flags);
                    PUT(x + 2, pal8[(hi >> 8) & 255], flags);
                    PUT(x + 3, pal8[hi & 255], flags);
                    PUT(x + 4, pal8[lo >> 24], flags);
                    PUT(x + 5, pal8[(lo >> 16) & 255], flags);
                    PUT(x + 6, pal8[(lo >> 8) & 255], flags);
                    PUT(x + 7, pal8[lo & 255], flags);
                }
            } else {
                for (i = 0; i < n; i++) {
                    PUT(x + i, e->bgPal8[chars[(px0 + i) ^ flip]], flags);
                }
            }
        } else {
            u32 bits = Rd32(vram, (charBase + tile * 32 + (u32)py * 4) & mask);
            const u16 *pal = BgPal4(e, bg) + ((entry >> 12) << 4);

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
/* An affine (or extended, or bitmap) BG's pixel at its map's (tx, ty), as
 * its kind (mode: 0 affine, 1 extended tiles, 2 256-colour bitmap, 3
 * direct colour) has it: colour | OPAQUE, or 0 (off its map, unwrapped). */
static inline u16 AffineSample(const Engine *e, int mode, const u8 *ext, u32 charBase, u32 screenBase, int mapW,
                               int mapH, BOOL wrap, s32 tx, s32 ty)
{
    const u8 *vram = e->bgVram;
    u32 mask = e->bgMask;

    if (wrap) {
        tx &= mapW - 1;
        ty &= mapH - 1;
    } else if (tx < 0 || ty < 0 || tx >= mapW || ty >= mapH) {
        return 0;
    }
    switch (mode) {
    case 3: {
        u16 c = Rd16(vram, (screenBase + ((u32)ty * (u32)mapW + (u32)tx) * 2) & mask);

        return (c & 0x8000) ? c : 0;   /* colour | OPAQUE */
    }
    case 2:
        return e->bgPal8[vram[(screenBase + (u32)ty * (u32)mapW + (u32)tx) & mask]];
    case 0: {
        u32 tile = vram[(screenBase + (u32)(ty >> 3) * (u32)(mapW >> 3) + (u32)(tx >> 3)) & mask];

        return e->bgPal8[vram[(charBase + tile * 64 + (u32)(ty & 7) * 8 + (u32)(tx & 7)) & mask]];
    }
    default: {
        u32 entry = Rd16(vram, (screenBase + ((u32)(ty >> 3) * (u32)(mapW >> 3) + (u32)(tx >> 3)) * 2) & mask);
        int px = tx & 7;
        int py = ty & 7;
        u32 index;

        if (entry & 0x400) {
            px = 7 - px;
        }
        if (entry & 0x800) {
            py = 7 - py;
        }
        index = vram[(charBase + (entry & 0x3FF) * 64 + (u32)py * 8 + (u32)px) & mask];
        if (index == 0) {
            return 0;
        }
        return (u16)((ext != NULL ? PlttColor(ext, ((entry >> 12) << 8) | index) : PlttColor(e->bgPltt, index)) |
                     OPAQUE);
    }
    }
}

static void DrawAffineBg(const Engine *e, int bg, int kind, u16 bgcnt, u32 dispcnt, int y, u16 *out)
{
    const u8 *io = e->io;
    u32 base = bg == 2 ? 0x20 : 0x30;
    s32 pa = (s16)Rd16(io, base + 0);
    s32 pb = (s16)Rd16(io, base + 2);
    s32 pc = (s16)Rd16(io, base + 4);
    s32 pd = (s16)Rd16(io, base + 6);
    s32 refX = (s32)(Rd32(io, base + 8) << 4) >> 4;    /* 28-bit signed */
    s32 refY = (s32)(Rd32(io, base + 12) << 4) >> 4;
    /* (a wider canvas: the DS's x = 0 at e->origin; without clip, the BG
     * goes on past the DS's edges as its matrix takes it; split, across
     * the canvas: its parts, moved, wherever they are) */
    int splitHere = (e->partBgs >> bg) & 1;
    int xs = e->clip[bg] && !splitHere ? e->origin : 0, xe = e->clip[bg] && !splitHere ? e->origin + W : e->width;
    PartRun runs[PART_RUNS_MAX];
    int nruns = -1, run = 0;
    s32 splitTy = -0x7FFFFFFF;
    const u16 *art = splitHere ? e->art[bg] : NULL;
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

        out[x] = 0;
        if (splitHere) {
            /* Where the matrix takes this pixel is a place on the split
             * picture (laid out across the canvas: x -origin.. at rest);
             * from that, the BG's own pixel it shows (or its hand-drawn
             * picture's, made at rest). */
            if (art != NULL && (e->artFull[bg] || tx < 0 || tx >= W)) {
                s32 ax = tx + e->origin;
                u16 c = 0;

                if (ax >= 0 && ax < NITRO_WIDE_ART_W && ty >= 0 && ty < H) {
                    c = art[ty * NITRO_WIDE_ART_W + ax];
                }
                if (!NitroPort_WideArtIsGame(c)) {
                    out[x] = c;
                    continue;
                }
                /* (the game's own there, as far back as the picture moved
                 * it: as the split draws it) */
                tx -= NitroPort_WideArtGameDx(c);
                ty -= NitroPort_WideArtGameDy(c);
            }
            if (ty != splitTy) {
                nruns = PartRuns(e, bg, ty, 0, runs);
                splitTy = ty;
                run = 0;
            }
            if (nruns < 0) {
                if (tx < 0 || tx >= W) {
                    continue;   /* (a line without parts: the DS's part only) */
                }
            } else {
                /* the run this place is in (the place goes on from the last
                 * pixel's, mostly: look from that run) */
                s32 c = tx + e->origin;

                if (run >= nruns || c < runs[run].c0 || c >= runs[run].c1) {
                    for (run = 0; run < nruns && !(c >= runs[run].c0 && c < runs[run].c1); run++) {
                    }
                    if (run == nruns) {
                        continue;
                    }
                }
                tx = runs[run].col >= 0 ? runs[run].col : tx - runs[run].shift;
                if (tx < 0 || tx >= W) {
                    continue;
                }
            }
        }
        out[x] = AffineSample(e, mode, ext, charBase, screenBase, mapW, mapH, wrap, tx, ty);
    }
    /* (wider canvas) beside the DS's screen, the edge's colour when the
     * edge is plain on this line (PlainRun) */
    if (xs > 0 && xe - xs >= PlainRun(e)) {
        int i, run = PlainRun(e);
        BOOL plain = (out[xs] & OPAQUE) != 0;

        for (i = 1; i < run && plain; i++) {
            plain = out[xs + i] == out[xs];
        }
        if (plain) {
            for (i = 0; i < xs; i++) {
                out[i] = out[xs];
            }
        }
    }
    if (xe < e->width && xe - xs >= PlainRun(e)) {
        int i, run = PlainRun(e);
        BOOL plain = (out[xe - 1] & OPAQUE) != 0;

        for (i = 2; i <= run && plain; i++) {
            plain = out[xe - i] == out[xe - 1];
        }
        if (plain) {
            for (i = xe; i < e->width; i++) {
                out[i] = out[xe - 1];
            }
        }
    }
    /* a hand-drawn picture: the bands (or all of it) from that (a split
     * BG's, above: where its matrix takes it) */
    if (e->art[bg] != NULL && !splitHere) {
        const u16 *line = e->art[bg] + y * NITRO_WIDE_ART_W;
        int i;

        for (i = 0; i < e->width && i < NITRO_WIDE_ART_W; i++) {
            if (e->artFull[bg] || i < e->origin || i >= e->origin + W) {
                u16 c = line[i];

                if (NitroPort_WideArtIsGame(c)) {
                    /* the game's own, as far back as the picture moved it:
                     * the BG's pixel there (drawn above over the DS's part
                     * when it's there; beside it, where the matrix takes
                     * it past the DS's edge) */
                    int gx = i - NitroPort_WideArtGameDx(c), gy = y - NitroPort_WideArtGameDy(c);

                    if (gx == i && gy == y && i >= xs && i < xe) {
                        continue;
                    }
                    c = AffineSample(e, mode, ext, charBase, screenBase, mapW, mapH, wrap,
                                     (refX + pb * gy + pa * (gx - e->origin)) >> 8,
                                     (refY + pd * gy + pc * (gx - e->origin)) >> 8);
                }
                out[i] = c;
            }
        }
    }
}

/* ------------------------------------------------------- hand-drawn art */

/* FNV-1a, 64 bits. */
#define KEY_START 0xcbf29ce484222325ull
static inline u64 KeyAdd(u64 h, u32 v)
{
    int i;

    for (i = 0; i < 4; i++) {
        h = (h ^ (v & 0xFF)) * 0x100000001b3ull;
        v >>= 8;
    }
    return h;
}

/* A quicker hash of the same words (two 32-bit lanes, a word at a time):
 * BgKeyCached's check that nothing BgKey reads changed. */
static inline u64 QuickAdd(u64 h, u32 v)
{
    u32 a = (u32)h, b = (u32)(h >> 32);

    a = (a ^ v) * 0x9E3779B1u;
    a = (a << 15) | (a >> 17);
    b = (b + v) * 0x85EBCA77u;
    b = ((b << 13) | (b >> 19)) ^ a;
    return ((u64)b << 32) | a;
}

/* A BG's key (nitro_port_wideart.h): its kind and size, its whole map, the
 * tiles the map uses (each once, by number) and the palettes they're in.
 * Not where its map and tiles are in VRAM, nor its scroll: the same art is
 * the same key wherever the game put it. 0 for a BG with none (a bitmap).
 * quick: QuickAdd's hash of the same words instead (not a key). */
static inline __attribute__((always_inline)) u64 BgKeyT(const Engine *e, int bg, int kind, u16 bgcnt,
                                                        u32 dispcnt, int quick)
{
    const u8 *vram = e->bgVram;
    u32 mask = e->bgMask, charBase = CharBase(e, bgcnt, dispcnt), screenBase = ScreenBase(e, bgcnt, dispcnt);
    u32 used[1024 / 32];
    u32 banks = 0, entries, i;
    int bpp8, wide16;   /* 8-bit tiles; 16-bit map entries */
    u64 h = KEY_START;
#define ADD(h, v) (quick ? QuickAdd(h, v) : KeyAdd(h, v))

    if (kind == BG_TEXT) {
        bpp8 = (bgcnt & 0x80) != 0;
        wide16 = 1;
        entries = (u32)(sTextSizes[bgcnt >> 14][0] / 8) * (u32)(sTextSizes[bgcnt >> 14][1] / 8);
    } else if (kind == BG_AFFINE || (kind == BG_EXTENDED && !(bgcnt & 0x80))) {
        bpp8 = 1;
        wide16 = kind == BG_EXTENDED;
        entries = (u32)((128 << (bgcnt >> 14)) / 8) * (u32)((128 << (bgcnt >> 14)) / 8);
    } else {
        return 0;   /* a bitmap, the 3D */
    }
    h = ADD(h, (u32)kind | ((u32)(bgcnt & 0xC080) << 8));
    memset(used, 0, sizeof(used));
    /* the map (a text BG's in its 2 KB blocks, in order: the same bytes) */
    for (i = 0; i < entries; i++) {
        u32 entry = wide16 ? Rd16(vram, (screenBase + i * 2) & mask) : vram[(screenBase + i) & mask];
        u32 tile = wide16 ? entry & 0x3FF : entry;

        h = ADD(h, entry);
        used[tile >> 5] |= 1u << (tile & 31);
        if (wide16) {
            banks |= 1u << (entry >> 12);
        }
    }
    /* the tiles used */
    for (i = 0; i < 1024; i++) {
        u32 size = bpp8 ? 64 : 32, k;

        if (!(used[i >> 5] & (1u << (i & 31)))) {
            continue;
        }
        for (k = 0; k < size; k += 4) {
            h = ADD(h, Rd32(vram, (charBase + i * size + k) & mask));
        }
    }
    /* their palettes */
    if (!bpp8) {
        for (i = 0; i < 16; i++) {
            if (banks & (1u << i)) {
                u32 k;

                for (k = 0; k < 16; k++) {
                    h = ADD(h, PlttColor(e->bgPltt, i * 16 + k));
                }
            }
        }
    } else {
        const u8 *ext = (dispcnt & 0x40000000) && wide16 ? BgExtSlot(e, bg, bgcnt) : NULL;

        if (ext != NULL) {
            for (i = 0; i < 16; i++) {
                if (banks & (1u << i)) {
                    u32 k;

                    for (k = 0; k < 256; k++) {
                        h = ADD(h, PlttColor(ext + (i << 9), k));
                    }
                }
            }
        } else {
            for (i = 0; i < 256; i++) {
                h = ADD(h, PlttColor(e->bgPltt, i));
            }
        }
    }
#undef ADD
    return h ? h : 1;
}


static u64 BgKey(const Engine *e, int bg, int kind, u16 bgcnt, u32 dispcnt)
{
    return BgKeyT(e, bg, kind, bgcnt, dispcnt, 0);
}

/* BgKey for drawing, worked out again only when what it reads changed
 * (QuickAdd's hash of it differs): FNV-1a's 64-bit multiply a byte took a
 * sixth of drawing the opening's screens with pictures, every frame. Each
 * engine's BGs, under a lock (an engine is drawn by one thread at a time,
 * but the PC's tools may draw one besides). */
static struct {
    u64 quick, key;
    u8 kind, valid;
    u16 bgcnt;
} sKeyCache[2][4];
static volatile int sKeyCacheLock[2];

static u64 BgKeyCached(const Engine *e, int bg, int kind, u16 bgcnt, u32 dispcnt)
{
    int eng = e->isA ? 0 : 1;
    u64 quick = BgKeyT(e, bg, kind, bgcnt, dispcnt, 1), key;

    while (__atomic_test_and_set(&sKeyCacheLock[eng], __ATOMIC_ACQUIRE)) {
    }
    if (sKeyCache[eng][bg].valid && sKeyCache[eng][bg].quick == quick && sKeyCache[eng][bg].kind == (u8)kind &&
        sKeyCache[eng][bg].bgcnt == bgcnt) {
        key = sKeyCache[eng][bg].key;
        __atomic_clear(&sKeyCacheLock[eng], __ATOMIC_RELEASE);
        return key;
    }
    __atomic_clear(&sKeyCacheLock[eng], __ATOMIC_RELEASE);
    key = BgKey(e, bg, kind, bgcnt, dispcnt);
    while (__atomic_test_and_set(&sKeyCacheLock[eng], __ATOMIC_ACQUIRE)) {
    }
    sKeyCache[eng][bg].quick = quick;
    sKeyCache[eng][bg].key = key;
    sKeyCache[eng][bg].kind = (u8)kind;
    sKeyCache[eng][bg].bgcnt = bgcnt;
    sKeyCache[eng][bg].valid = 1;
    __atomic_clear(&sKeyCacheLock[eng], __ATOMIC_RELEASE);
    return key;
}

/* (wider canvas) each BG's hand-drawn picture, if there's one made where
 * the BG is now: of a text BG's pictures, the one made at the scroll
 * nearest its scroll now, less than a screen away (a picture of what's on
 * screen, not of another part of the map). */
#define ART_REST 8   /* pixels from where a picture was made that are still there */
static void LookUpArt(Engine *e, const int *kind, const u16 *bgcnt, u32 dispcnt)
{
    int bg;

    if (e->width != NITRO_WIDE_ART_W || NitroPort_WideArtCount() == 0) {
        return;
    }
    for (bg = 0; bg < 4; bg++) {
        u64 key;
        int hofs, vofs, full, moved = (e->movedBgs >> bg) & 1;
        const u16 *art;

        if (kind[bg] == BG_NONE || kind[bg] == BG_3D || !e->clip[bg] || (moved && kind[bg] != BG_TEXT)) {
            continue;   /* (a BG the screen's parts move: its own pixels) */
        }
        key = BgKeyCached(e, bg, kind[bg], bgcnt[bg], dispcnt);
        if (key == 0) {
            continue;
        }
        if (kind[bg] == BG_TEXT) {
            int mapW = sTextSizes[bgcnt[bg] >> 14][0], mapH = sTextSizes[bgcnt[bg] >> 14][1];
            int curH = Rd16(e->io, 0x10 + bg * 4) & 0x1FF, curV = Rd16(e->io, 0x12 + bg * 4) & 0x1FF;
            int dx, dy;

            if (moved) {
                /* A BG the screen's parts move: a picture made right where
                 * it is (a capture of the screen as the parts lay it out,
                 * drawn on: the boxes' panel's edges, 2026-10-16), else its
                 * own pixels as the parts move them (scrolled elsewhere, the
                 * parts' columns are another picture's). */
                art = NitroPort_WideArtFindNear(key, curH, curV, mapW, mapH, 1, 1, &hofs, &vofs, &full);
                if (art == NULL) {
                    continue;
                }
                e->artDx[bg] = e->artDy[bg] = 0;
                e->artH[bg] = (short)hofs;
                e->artV[bg] = (short)vofs;
                e->artBands[bg] = 1;
                e->art[bg] = art;
                e->artRows[bg] = NitroPort_WideArtRows(art);
                e->artFull[bg] = (u8)(full & NITRO_WIDE_ART_FULL);
                e->artLoopBands[bg] = 0;
                continue;
            }
            art = NitroPort_WideArtFindNear(key, curH, curV, mapW, mapH, W, H, &hofs, &vofs, &full);
            if (art == NULL) {
                continue;
            }
            e->artH[bg] = (short)hofs;
            e->artV[bg] = (short)vofs;
            /* how far it has scrolled since, the shorter way round its map */
            dx = ((curH - hofs) % mapW + mapW) % mapW;
            dy = ((curV - vofs) % mapH + mapH) % mapH;
            e->artDx[bg] = (short)(dx >= mapW / 2 ? dx - mapW : dx);
            e->artDy[bg] = (short)(dy >= mapH / 2 ? dy - mapH : dy);
            /* Its bands while it's across where the picture was made (a
             * few pixels either way), or always for a strip that scrolls
             * round (_loop). Up and down doesn't matter (the running
             * scene's city bobs 13-22 pixels from its picture's place).
             * Elsewhere sideways, a whole picture is the whole layer, moved
             * with it: a Pokemon sliding in shows whole, not cut at the
             * DS's edge (2026-10-14), its slide widened so that it comes
             * in from the wide screen's edge: what the DS hides beyond its
             * edge, waiting to come in, stays hidden (Chimchar's hand
             * showed beside the DS's screen before he came in). A picture
             * of the bands only shows nothing there. */
            e->artBands[bg] = (u8)((full & NITRO_WIDE_ART_LOOP) ||
                                   (e->artDx[bg] >= -ART_REST && e->artDx[bg] <= ART_REST));
            if (!e->artBands[bg] && (full & NITRO_WIDE_ART_FULL)) {
                int left, right, moved = -e->artDx[bg], widened = moved;

                if (NitroPort_WideArtExtent(art, &left, &right)) {
                    widened = NitroPort_WideArtMovedOffset(moved, left, right);
                }
                e->artMoved[bg] = 1;
                e->artExact[bg] = (u8)(widened == moved);
                e->artDx[bg] = (short)-widened;
            }
        } else {
            art = NitroPort_WideArtFindNear(key, 0, 0, 512, 512, 0, 0, &hofs, &vofs, &full);
            if (art == NULL) {
                continue;
            }
        }
        e->art[bg] = art;
        e->artRows[bg] = NitroPort_WideArtRows(art);
        e->artFull[bg] = (u8)(full & NITRO_WIDE_ART_FULL);
        e->artLoopBands[bg] = (u8)(kind[bg] == BG_TEXT && (full & NITRO_WIDE_ART_LOOP) && !(full & NITRO_WIDE_ART_FULL));
        if (kind[bg] != BG_TEXT) {
            e->artBands[bg] = 1;
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
    s16 stretch;           /* (moved by a screen's part) its edge column drawn on over this many pixels back
                            * to where it was: its first column left of it (> 0), its last right of it (< 0) */
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

/* (wider canvas) a group of OBJs' hand-drawn picture (see ObjGroups):
 * drawn into the OBJ layer where no OBJ is, at the group's priority. */
typedef struct ObjArt {
    const u16 *art;
    const u8 *rows;        /* (its rows with anything: NitroPort_WideArtRows) */
    short dx, dy;          /* how far the group has moved since its capture */
    u8 prio, full;
} ObjArt;
#define OBJ_ART_MAX 4

/* This frame's OBJs, and which of them are on each line (in OAM order). */
typedef struct ObjFrame {
    Obj obj[128];
    u8 count;
    u8 blends;             /* some are semi-transparent or bitmap OBJs */
    u8 artCount;
    ObjArt art[OBJ_ART_MAX];
    u8 onLine[HMAX][128];
    u8 lineCount[HMAX];
} ObjFrame;

/* OAM entry i as an Obj (its left edge on the canvas): FALSE if it's
 * hidden (or of the shape that doesn't exist). */
static BOOL ReadObj(const Engine *e, int i, Obj *obj)
{
    const u8 *o = e->oam + i * 8;
    u16 a0 = Rd16(o, 0), a1 = Rd16(o, 2), a2 = Rd16(o, 4);
    int shape = (a0 >> 14) & 3;

    if (!(a0 & 0x100) && (a0 & 0x200)) {
        return FALSE;                       /* hidden */
    }
    if (shape == 3) {
        return FALSE;
    }
    obj->affine = (a0 & 0x100) != 0;
    obj->stretch = 0;
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
    if (e->nObjMove) {
        /* (over a window moved to an edge: with it) */
        int cx = obj->ox + obj->boxW / 2, cy = (obj->oy + obj->boxH / 2) & 255, k;

        for (k = 0; k < e->nObjMove; k++) {
            if (cx >= e->objMove[k].x0 && cx < e->objMove[k].x1 && cy >= e->objMove[k].y0 &&
                cy < e->objMove[k].y1) {
                obj->ox += e->objMove[k].shift;
                if (e->objMove[k].stretch && !obj->affine && obj->mode != 2) {
                    obj->stretch = e->objMove[k].shift;
                }
                break;
            }
        }
    }
    obj->ox += e->origin;
    obj->hflip = !obj->affine && (a1 & 0x1000) != 0;
    obj->vflip = !obj->affine && (a1 & 0x2000) != 0;
    obj->bpp8 = (a0 & 0x2000) != 0;
    obj->mode = (a0 >> 10) & 3;
    obj->tile = a2 & 0x3FF;
    obj->prio = (a2 >> 10) & 3;
    obj->pal = a2 >> 12;
    return TRUE;
}

static void SortObjs(const Engine *e, ObjFrame *f)
{
    int i, y;

    f->count = 0;
    f->blends = 0;
    f->artCount = 0;
    memset(f->lineCount, 0, sizeof(f->lineCount));
    for (i = 0; i < 128; i++) {
        Obj *obj = &f->obj[f->count];

        if (!ReadObj(e, i, obj)) {
            continue;
        }
        if (obj->ox >= e->width || obj->ox + obj->boxW <= 0) {
            continue;                       /* off the sides */
        }

        /* The lines it's on: (y - oy) & 255 < boxH. */
        for (y = 0; y < e->height; y++) {
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

/* ---------------------------------------------- OBJ groups' hand-drawn art */

/* The OBJs are put in groups for wide art (nitro_port_wideart.h): those of
 * one priority and one palette (the 8-bit ones of a priority together); a
 * scene's sprite background (the intro's strip behind the Pokemon) is
 * usually one, the sprites in front of it others. The window and bitmap
 * OBJs aren't in any. */
static inline int ObjGroupOf(const Obj *obj)
{
    return (obj->prio << 5) | (obj->bpp8 ? 16 : obj->pal);
}

/* A fingerprint of an OBJ's first tile (its pixels): which of a group's
 * OBJs its position is taken from. */
static u64 ObjFirstTile(const Engine *e, const Obj *obj, u32 dispcnt)
{
    u32 addr = ObjCharAddr(obj, dispcnt, 0, 0) & e->objMask & ~3u, k;
    u64 h = KEY_START;

    for (k = 0; k < (obj->bpp8 ? 64u : 32u); k += 4) {
        h = (h ^ Rd32(e->objVram, (addr + k) & e->objMask)) * 0x100000001b3ull;
    }
    return h;
}

/* The OBJ groups shown (all of OAM's, on screen or not): each one's key and
 * where it is. The key is a fingerprint of the tiles its OBJs use (each
 * once, in VRAM's order: their pixels, not where they are) and its
 * palette: so not of how many OBJs show them, nor their shapes or places
 * (a strip of sprites scrolling round, its sprites cut differently as they
 * go, keeps its key). Where it is: the position (OAM's x 0-511, y 0-255)
 * of the OBJ whose first tile has the smallest fingerprint, the first of
 * those. */
static int ObjGroups(const Engine *e, u32 dispcnt, NitroObjGroup *out, int max)
{
    struct {
        u64 anchor;
        u16 count, x, y;
    } g[128];
    s8 groupOf[128];
    u32 used[0x40000 / 32 / 32];   /* the 32-byte tile units used, of OBJ VRAM */
    int i, k, n = 0;

    memset(g, 0, sizeof(g));
    for (i = 0; i < 128; i++) {
        Obj obj;
        u64 h;

        groupOf[i] = -1;
        if (!ReadObj(e, i, &obj) || obj.mode >= 2) {
            continue;
        }
        k = ObjGroupOf(&obj);
        groupOf[i] = (s8)k;
        h = ObjFirstTile(e, &obj, dispcnt);
        if (g[k].count == 0 || h < g[k].anchor) {
            g[k].anchor = h;
            g[k].x = Rd16(e->oam, (u32)i * 8 + 2) & 0x1FF;
            g[k].y = Rd16(e->oam, (u32)i * 8) & 0xFF;
        }
        g[k].count++;
    }
    for (k = 0; k < 128 && n < max; k++) {
        u64 key;
        u32 units = (e->objMask + 1) / 32, u, c;

        if (g[k].count == 0) {
            continue;
        }
        /* the tiles its OBJs use */
        memset(used, 0, sizeof(used));
        for (i = 0; i < 128; i++) {
            Obj obj;
            int tx, ty;

            if (groupOf[i] != k || !ReadObj(e, i, &obj)) {
                continue;
            }
            for (ty = 0; ty < obj.h; ty += 8) {
                for (tx = 0; tx < obj.w; tx += 8) {
                    u = (ObjCharAddr(&obj, dispcnt, tx, ty) & e->objMask) >> 5;
                    used[u >> 5] |= 1u << (u & 31);
                    if (obj.bpp8 && u + 1 < units) {
                        used[(u + 1) >> 5] |= 1u << ((u + 1) & 31);
                    }
                }
            }
        }
        key = KeyAdd(KEY_START, 0x4F420000u | (u32)(k >> 5));   /* "OB", the priority */
        for (u = 0; u < units; u++) {
            if (used[u >> 5] == 0) {
                u += 31;
                continue;
            }
            if (used[u >> 5] & (1u << (u & 31))) {
                for (c = 0; c < 32; c += 4) {
                    key = (key ^ Rd32(e->objVram, u * 32 + c)) * 0x100000001b3ull;
                }
            }
        }
        /* its colours */
        if (!(k & 16)) {
            for (c = 0; c < 16; c++) {
                key = (key ^ PlttColor(e->objPltt, ((u32)(k & 15) << 4) | c)) * 0x100000001b3ull;
            }
        } else {
            for (c = 0; c < 256; c++) {
                key = (key ^ PlttColor(e->objPltt, c)) * 0x100000001b3ull;
            }
        }
        out[n].key = key ? key : 1;
        out[n].group = k;
        out[n].prio = k >> 5;
        out[n].x = g[k].x;
        out[n].y = g[k].y;
        out[n].count = g[k].count;
        n++;
    }
    return n;
}

/* (wider canvas) the OBJ groups' hand-drawn pictures, if there are any (all
 * groups', or only's: -1 for all). */
static void LookUpObjArt(const Engine *e, ObjFrame *f, u32 dispcnt, int only)
{
    NitroObjGroup groups[32];
    int n, i;

    f->artCount = 0;
    if (e->width != NITRO_WIDE_ART_W || NitroPort_WideArtCount() == 0 || !(dispcnt & 0x1000) || e->objsMoved) {
        return;   /* (sprites the screen's parts move: themselves) */
    }
    n = ObjGroups(e, dispcnt, groups, 32);
    for (i = 0; i < n && f->artCount < OBJ_ART_MAX; i++) {
        int x, y, full, dx, dy;
        const u16 *art;
        ObjArt *a;

        if (only >= 0 && groups[i].group != only) {
            continue;
        }
        art = NitroPort_WideArtFindNear(groups[i].key, groups[i].x, groups[i].y, 512, 256, 0, 0, &x, &y, &full);
        if (art == NULL) {
            continue;
        }
        dx = (groups[i].x - x + 512) & 511;
        dy = (groups[i].y - y + 256) & 255;
        a = &f->art[f->artCount++];
        a->art = art;
        a->rows = NitroPort_WideArtRows(art);
        a->dx = (short)(dx >= 256 ? dx - 512 : dx);
        a->dy = (short)(dy >= 128 ? dy - 256 : dy);
        a->prio = (u8)groups[i].prio;
        a->full = (u8)(full & NITRO_WIDE_ART_FULL);
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

/* (NitroPort_Render2DSmoothObjs) */
static volatile int sSmoothObjs[2];

void NitroPort_Render2DSmoothObjs(int engine, int on)
{
    __atomic_store_n(&sSmoothObjs[engine ? 1 : 0], on ? 1 : 0, __ATOMIC_RELEASE);
}

/* An affine OBJ's pixel drawn smoothly: (u, v) where its middle falls in
 * the OBJ's texels, in 256ths, from the top-left texel's middle; the four
 * texels round it blended by how near each is, the clear ones left out (a
 * pixel mostly clear: clear). 0: clear. */
static u16 SmoothObjPixel(const Engine *e, const Obj *obj, u32 dispcnt, s32 u, s32 v, int *alpha)
{
    int tx = u >> 8, ty = v >> 8, ax = u & 255, ay = v & 255, k, weights = 0, a = 16, best = 0;
    u32 r = 0, g = 0, b = 0;

    for (k = 0; k < 4; k++) {
        int px = tx + (k & 1), py = ty + (k >> 1);
        int weight = ((k & 1) ? ax : 256 - ax) * ((k >> 1) ? ay : 256 - ay);
        int texelAlpha;
        u16 c;

        if (weight == 0 || px < 0 || py < 0 || px >= obj->w || py >= obj->h) {
            continue;
        }
        c = ObjPixel(e, obj, dispcnt, px, py, &texelAlpha);
        if (c == 0) {
            continue;
        }
        if (weight > best) {
            best = weight;   /* (a bitmap OBJ's alpha: the nearest shown texel's) */
            a = texelAlpha;
        }
        weights += weight;
        r += (u32)(c & 31) * (u32)weight;
        g += (u32)((c >> 5) & 31) * (u32)weight;
        b += (u32)((c >> 10) & 31) * (u32)weight;
    }
    *alpha = a;
    if (weights * 2 < 256 * 256) {
        return 0;
    }
    r = (r + (u32)weights / 2) / (u32)weights;
    g = (g + (u32)weights / 2) / (u32)weights;
    b = (b + (u32)weights / 2) / (u32)weights;
    return (u16)(r | (g << 5) | (b << 10) | OPAQUE);
}

/* The OBJ layer's line; returns whether any OBJ is on it. */
static BOOL DrawObjs(const Engine *e, const ObjFrame *f, u32 dispcnt, int y, ObjLine *out)
{
    int smooth = __atomic_load_n(&sSmoothObjs[e->isA ? 0 : 1], __ATOMIC_ACQUIRE);

    u16 sprites[WMAX];
    int k;

    if (!(dispcnt & 0x1000) || (f->lineCount[y] == 0 && f->artCount == 0)) {
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

        if (obj->affine && smooth && obj->mode != 2) {
            /* (NitroPort_Render2DSmoothObjs: the pixel's middle mapped,
             * the texels round it blended) */
            s32 dy = ly - obj->boxH / 2;
            s32 dx = sx - obj->boxW / 2;
            s32 fx = obj->pa * dx + obj->pb * dy + (obj->pa + obj->pb) / 2 + (obj->w << 7) - 128;
            s32 fy = obj->pc * dx + obj->pd * dy + (obj->pc + obj->pd) / 2 + (obj->h << 7) - 128;

            for (; sx < end; sx++, fx += obj->pa, fy += obj->pc) {
                int x = obj->ox + sx;
                int alpha;
                u16 rgb;

                if ((out->color[x] & OPAQUE) && obj->mode != 2) {
                    continue;
                }
                if (fx < -256 || fy < -256 || fx >= obj->w << 8 || fy >= obj->h << 8) {
                    continue;
                }
                rgb = SmoothObjPixel(e, obj, dispcnt, fx, fy, &alpha);
                if (rgb) {
                    PutObjPixel(obj, out, x, rgb, alpha);
                }
            }
        } else if (obj->affine) {
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
        if (obj->stretch) {
            /* moved by a screen's part, stretched: its edge column drawn
             * on back to where it was (a cursor made of two halves,
             * widened with its row) */
            int ty = obj->vflip ? obj->h - 1 - ly : ly;
            int edge = obj->stretch > 0 ? 0 : obj->w - 1;
            int tx = obj->hflip ? obj->w - 1 - edge : edge;
            int x0 = obj->stretch > 0 ? obj->ox - obj->stretch : obj->ox + obj->w;
            int x1 = obj->stretch > 0 ? obj->ox : obj->ox + obj->w - obj->stretch;
            int alpha, x;
            u16 rgb = ObjPixel(e, obj, dispcnt, tx, ty, &alpha);

            if (x0 < 0) {
                x0 = 0;
            }
            if (x1 > e->width) {
                x1 = e->width;
            }
            for (x = x0; rgb && x < x1; x++) {
                if (!(out->color[x] & OPAQUE)) {
                    PutObjPixel(obj, out, x, rgb, alpha);
                }
            }
            if (x0 < out->x0) {
                out->x0 = (short)x0;
            }
            if (x1 > out->x1) {
                out->x1 = (short)x1;
            }
        }
    }
    if (f->artCount > 0) {
        /* (the sprites' own pixels, before any picture goes on: a picture's
         * pixel that's a sprite's moved along is drawn from them) */
        memcpy(sprites, out->color, (size_t)e->width * sizeof(u16));
    }
    for (k = 0; k < f->artCount; k++) {
        /* a group's picture where no OBJ is (in the bands beside the DS's
         * screen, or all over) */
        const ObjArt *a = &f->art[k];
        int ay = ArtWrap(y - a->dy, H, 256), x;
        const u16 *row;
        ArtRun run;

        if (ay < 0 || (a->rows != NULL && !a->rows[ay])) {
            continue;
        }
        row = a->art + ay * NITRO_WIDE_ART_W;
        ArtRunStart(&run, -a->dx, NITRO_WIDE_ART_W, 256);
        for (x = 0; x < e->width; x++) {
            /* (past the picture's edge: a screen's width on, as a strip of
             * sprites that scrolls round goes on) */
            int ax = ArtRunNext(&run);
            u16 c;

            if (ax < 0 || (out->color[x] & OPAQUE) ||
                (!a->full && x >= e->origin && x < e->origin + W)) {
                continue;
            }
            c = row[ax];
            if (NitroPort_WideArtIsGame(c)) {
                /* the sprites' own pixel that far back on the row (the
                 * picture had it there, moved) */
                int gx = x - NitroPort_WideArtGameDx(c);

                c = (gx >= 0 && gx < e->width && gx != x) ? sprites[gx] : 0;
            }
            if (!(c & 0x8000)) {
                continue;
            }
            out->color[x] = c | OPAQUE;
            out->prio[x] = a->prio;
            out->prios |= (u8)(1 << a->prio);
            out->semi[x] = 0;
            out->alpha[x] = 16;
            if (x < out->x0) {
                out->x0 = (short)x;
            }
            if (x + 1 > out->x1) {
                out->x1 = (short)(x + 1);
            }
        }
    }
    return out->x0 < out->x1 || f->lineCount[y] > 0;
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
/* The DS's windows are rectangles of its screen, x lo to hi (hi = 0: its
 * right edge). On a wider canvas (origin > 0, width > 256) a window at
 * the DS's left or right edge goes on to the canvas's (the intro's
 * letterbox, a window over the whole screen: the bands beside the DS's
 * picture are in it too). */
static void FillRange(u8 *enable, int lo, int hi, u8 value, int origin, int width, BOOL toEdge)
{
    /* An empty window (lo = hi: the circle wipes' lines past the circle are
     * 0-0) covers nothing, the bands beside the DS's picture included: a
     * window from x 0 reaches the left edge only when it covers x 0. (The
     * Town Map's circle closing left its blue side bars showing in the
     * bands, 2026-10-24.) */
    if (lo == hi) {
        return;
    }
    /* (the wider canvas, or toEdge: NitroRender2DSplit.windowsToEdge) A
     * window a pixel short of an edge is at it: games cover the screen
     * with 0-255 (the right end is exclusive, and 256 doesn't fit in the
     * register), leaving the DS's last column out. On the wider screen
     * that column and the band beyond it went black (the intro's city); on
     * the remaster's lower screen, drawn 1.25 times bigger, a black strip
     * down its right edge (the intro's girl running, 2026-10-21). The DS's
     * own picture keeps the DS's column. */
    if ((toEdge || origin > 0) && lo < hi) {
        if (hi == 255) {
            hi = W;
        }
        if (lo == 1) {
            lo = 0;
        }
    }
    if (origin > 0 && lo < hi) {
        memset(enable + origin + lo, value, (size_t)(hi - lo));
        if (lo == 0) {
            memset(enable, value, (size_t)origin);
        }
        if (hi == W && origin + W < width) {
            memset(enable + origin + W, value, (size_t)(width - origin - W));
        }
        return;
    }
    if (lo <= hi) {
        if (hi > lo) {
            memset(enable + origin + lo, value, (size_t)(hi - lo));
        }
        if (lo == 0 && origin > 0) {
            memset(enable, value, (size_t)origin);
        }
        if (hi == 0 && origin + W < width) {
            memset(enable + origin + W, value, (size_t)(width - origin - W));
        }
    } else {
        memset(enable + origin + lo, value, (size_t)(W - lo));   /* lo <= 255 */
        memset(enable + origin, value, (size_t)hi);
        if (origin + W < width) {
            memset(enable + origin + W, value, (size_t)(width - origin - W));
        }
        if (hi == 0 && origin > 0) {
            memset(enable, value, (size_t)origin);
        }
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
    u16 picLine[WMAX];    /* the pictures drawn with a BG on this line (PicturesOnLine) */
    u8 lineIo[0x2000];    /* (an H-blank effect) the registers as they are on the line being drawn */
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
    if ((e->picBgs >> bg) & 1) {
        int x0, x1;

        (void)PicturesOnLine(e, bg, y, line, FALSE, &x0, &x1);
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
    if (e->isA && kind[0] == BG_3D && split == NULL) {
        e->height = H;   /* (a tall picture with the 3D: only in two layers, the 3D on the DS's lines) */
    }
    if (fixedWidth == 0) {
        DecideSplits(e, kind, bgcnt, dispcnt);
        LookUpArt(e, kind, bgcnt, dispcnt);
        DecideBandFills(e, kind, bgcnt, dispcnt);
    }
    if (split != NULL) {
        split->has3d = e->isA && kind[0] == BG_3D;
        split->prio3d = bgcnt[0] & 3;
        split->scroll3d = split->has3d ? (Rd16(io, 0x10) - e->shift3d) & 0x1FF : 0;
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
    objFrame->artCount = 0;
    if (dispcnt & 0x1000) {
        SortObjs(e, objFrame);
        if (fixedWidth == 0) {
            LookUpObjArt(e, objFrame, dispcnt, -1);
        }
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

    /* (an H-blank effect) Each line's registers: the BGs' scroll, the
     * windows (where they are, which ones show, what's in and out of them)
     * and the 3D's scroll, as the game set them for it. The colour effects
     * and the rest are the frame's. */
    if (e->lines != NULL) {
        u8 *lineIo = work->lineIo;
        int off = e->isA ? 0 : 0x1000;

        memcpy(lineIo, e->ioAll, sizeof(work->lineIo));
        e->ioAll = lineIo;
        e->io = lineIo + off;
    }
    if (split != NULL) {
        split->scroll3dLines = 0;
    }

    for (y = 0; y < e->height; y++) {
        u16 *row = out + y * width;
        BOOL objs;
        Target t;

        if (e->lines != NULL) {
            u8 *lineRegs = (u8 *)e->io;
            u32 lineDispcnt;

            /* (a tall picture's lines past the DS's: its last line's) */
            memcpy(lineRegs, e->lines[y < H ? y : H - 1], NITRO_LINE_REGS);
            lineDispcnt = Rd32(lineRegs, 0);
            dispcnt = (dispcnt & ~0xE000u) | (lineDispcnt & 0xE000u);
            win0h = Rd16(lineRegs, 0x40);
            win1h = Rd16(lineRegs, 0x42);
            win0v = Rd16(lineRegs, 0x44);
            win1v = Rd16(lineRegs, 0x46);
            winin = Rd16(lineRegs, 0x48);
            winout = Rd16(lineRegs, 0x4A);
            anyWindow = (dispcnt & 0xE000) != 0;
            flags = (flags & ~T_WINDOW) | (anyWindow ? T_WINDOW : 0);
            if (!anyWindow) {
                memset(enableLine, 0x3F, (size_t)width);
            }
            if (split != NULL && split->has3d && y < H) {
                int scroll = (Rd16(lineRegs, 0x10) - e->shift3d) & 0x1FF;

                split->scroll3dLine[y] = (short)scroll;
                split->scroll3dLines |= scroll != split->scroll3d;
            }
        }
        objs = DrawObjs(e, objFrame, dispcnt, y, objLine);

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
                FillRange(enableLine, win1h >> 8, win1h & 0xFF, (winin >> 8) & 0x3F, e->origin, width, split != NULL && split->windowsToEdge);
            }
            if ((dispcnt & 0x2000) && InRange(y, win0v >> 8, win0v & 0xFF)) {
                FillRange(enableLine, win0h >> 8, win0h & 0xFF, winin & 0x3F, e->origin, width, split != NULL && split->windowsToEdge);
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

                if (slot == 0 && split != NULL && split->has3d && p == split->prio3d && y < H) {
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
                    /* straight into the line (the pictures drawn with it
                     * first: in front of it) */
                    int px0, px1;

                    if (((e->picBgs >> bg) & 1) &&
                        PicturesOnLine(e, bg, y, work->picLine, TRUE, &px0, &px1)) {
                        Merge(&t, work->picLine, NULL, p, px0, px1, blend2 ? flags | T_LAYER : flags);
                    }
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

        if (split != NULL && split->has3d && !snapped && y < H) {
            /* (the line was full before the 3D's place: everything drawn
             * is in front of it, and nothing behind it was drawn; a tall
             * picture's lines past the DS's have no 3D: all of it behind) */
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
        if (split != NULL && split->has3d && anyWindow && y < H) {
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

/* (wider canvas) The screen's parts (port_wide.c): the splits made into
 * parts (the part left of the split, the part right of it, the column
 * drawn on between), then the game's parts; its sprite moves first in
 * line, and the 3D's. */
static void SetUpParts(Engine *e)
{
    NitroWideSplit splits[NITRO_WIDE_MAX_SPLITS];
    NitroWidePart game[NITRO_WIDE_MAX_PARTS];
    int eng = e->isA ? 0 : 1, ns, ng, i;

    if (e->taken != NULL) {
        /* (as they were when the display state was taken) */
        ns = e->taken->nsplits;
        ng = e->taken->nparts;
        memcpy(splits, e->taken->splits, (size_t)ns * sizeof(*splits));
        memcpy(game, e->taken->parts, (size_t)ng * sizeof(*game));
    } else {
        ns = NitroPort_WideSplits(eng, splits);
        ng = NitroPort_WideParts(eng, game);
    }

    for (i = 0; i < ns; i++) {
        const NitroWideSplit *sp = &splits[i];
        NitroWidePart *p = &e->parts[e->nParts];

        /* (the gap's column first: the parts drawn over it, as before
         * when they overlap) */
        memset(p, 0, 3 * sizeof(*p));
        p[0].kind = p[1].kind = NITRO_WIDE_MOVE;
        p[0].bgs = p[1].bgs = p[2].bgs = sp->bgs;
        p[0].top = p[1].top = p[2].top = (s16)sp->top;
        p[0].bottom = p[1].bottom = p[2].bottom = (s16)sp->bottom;
        p[0].x0 = -0x7FFF;   /* (from the DS's first column, and to its last: PartRuns) */
        p[0].x1 = p[1].x0 = sp->x;
        p[1].x1 = 0x7FFF;
        p[0].dx = sp->left;
        p[1].dx = sp->right;
        e->nParts += 2;
        if (sp->fill && sp->left < sp->right) {
            /* the gap's column under the two parts */
            p[2] = p[1];
            p[1] = p[0];
            p[0].kind = NITRO_WIDE_FILL;
            p[0].x0 = (s16)(sp->x + sp->left);
            p[0].x1 = (s16)(sp->x + sp->right);
            p[0].dx = 0;
            p[0].col = sp->x;
            e->nParts++;
        }
        e->partBgs |= sp->bgs;
    }
    for (i = 0; i < ng; i++) {
        const NitroWidePart *p = &game[i];

        switch (p->kind) {
        case NITRO_WIDE_MOVE:
        case NITRO_WIDE_FILL:
        case NITRO_WIDE_SHOW:
            e->parts[e->nParts++] = *p;
            e->partBgs |= p->bgs & 0x0F;
            if (p->kind == NITRO_WIDE_MOVE) {
                e->movedBgs |= p->bgs & 0x0F;
            }
            break;
        case NITRO_WIDE_OBJS:
            if (e->nObjMove < OBJ_MOVES_MAX) {
                e->objMove[e->nObjMove].x0 = p->x0;
                e->objMove[e->nObjMove].x1 = p->x1;
                e->objMove[e->nObjMove].y0 = p->top;
                e->objMove[e->nObjMove].y1 = p->bottom;
                e->objMove[e->nObjMove].shift = p->dx;
                e->objMove[e->nObjMove].stretch = (short)(p->col != 0);
                e->nObjMove++;
                e->objsMoved = 1;
            }
            break;
        case NITRO_WIDE_SHIFT3D:
            e->shift3d = (short)(e->shift3d + p->dx);
            break;
        case NITRO_WIDE_PICTURE:
            SetUpPicture(e, p);
            break;
        case NITRO_WIDE_CLEAR:
            e->clearBgs |= (u8)(p->bgs & 0x0F);
            e->clearMask[p->dx & 15] |= (u16)p->col;
            break;
        }
    }
}

/* An engine's registers and memory, and the canvas (split's: width,
 * origin, clip; NULL: the DS's). */
static void SetUpEngine(Engine *e, int engine, const NitroRender2DSplit *split)
{
    BOOL taken = split != NULL && split->snapshot;
    const State2D *st = &sState[engine ? 1 : 0];

    memset(e, 0, sizeof(*e));
    e->isA = engine == 0;
    e->taken = taken ? st : NULL;
    e->ioAll = taken ? st->io : NitroPort_IoReg;
    e->io = e->ioAll + (e->isA ? 0 : 0x1000);
    e->lines = taken ? (st->lines ? (const u8(*)[NITRO_LINE_REGS])st->lineRegs : NULL)
                     : (NitroPort_LineRegs.active ? (const u8(*)[NITRO_LINE_REGS])NitroPort_LineRegs.regs[e->isA ? 0 : 1]
                                                  : NULL);
    e->bgPltt = (taken ? st->pltt : NitroPort_Pltt) + (e->isA ? 0 : 0x400);
    e->objPltt = e->bgPltt + 0x200;
    e->oam = (taken ? st->oam : NitroPort_Oam) + (e->isA ? 0 : 0x400);
    e->bgVram = e->isA ? NitroPort_BgVram : NitroPort_DbBgVram;
    e->bgMask = e->isA ? 0x7FFFF : 0x1FFFF;
    e->objVram = e->isA ? NitroPort_ObjVram : NitroPort_DbObjVram;
    e->objMask = e->isA ? 0x3FFFF : 0x1FFFF;
    e->width = W;
    e->height = H;
    if (split != NULL && split->height > H && split->height <= HMAX && split->width <= W) {
        e->height = split->height;   /* (a tall picture: the DS's width only) */
    }
    if (split != NULL && split->width > W && split->width <= WMAX) {
        int bg;

        e->width = split->width;
        e->origin = split->origin;
        for (bg = 0; bg < 4; bg++) {
            e->clip[bg] = split->clip[bg];
        }
        e->anchorWindows = (u8)split->anchorWindows;
        e->pictures = (u8)(NitroPort_WidePictures() != 0);
        SetUpParts(e);
        if (e->anchorWindows) {
            /* The windows moved to an edge, on the DS's screen: the
             * sprites over them go with them. */
            for (bg = 0; bg < 4 && e->nObjMove < OBJ_MOVES_MAX; bg++) {
                NitroWideWindow wins[NITRO_WIDE_MAX_WINDOWS];
                int n = EngineWindows(e, bg, wins), i;
                int hofs = Rd16(e->io, 0x10 + bg * 4) & 0x1FF, vofs = Rd16(e->io, 0x12 + bg * 4) & 0x1FF;

                for (i = 0; i < n && e->nObjMove < OBJ_MOVES_MAX; i++) {
                    if (wins[i].anchor == 0) {
                        continue;
                    }
                    e->objMove[e->nObjMove].x0 = (short)(wins[i].left * 8 - hofs);
                    e->objMove[e->nObjMove].x1 = (short)((wins[i].left + wins[i].width) * 8 - hofs);
                    e->objMove[e->nObjMove].y0 = (short)(wins[i].top * 8 - vofs);
                    e->objMove[e->nObjMove].y1 = (short)((wins[i].top + wins[i].height) * 8 - vofs);
                    e->objMove[e->nObjMove].shift =
                        (short)(wins[i].anchor < 0 ? -e->origin : e->width - W - e->origin);
                    e->objMove[e->nObjMove].stretch = 0;
                    e->nObjMove++;
                }
            }
        }
    }
}

static void Render(int engine, u16 *out, NitroRender2DSplit *split)
{
    Engine e;
    u32 dispcnt;
    u16 powcnt;

    SetUpEngine(&e, engine, split);
    if (split != NULL) {
        memset(split, 0, offsetof(NitroRender2DSplit, over));
        split->masterBright = Rd16(e.io, 0x6C);
    }
    powcnt = Rd16(e.ioAll, 0x304);
    dispcnt = Rd32(e.io, 0);

    /* The engine powered off: nothing on that screen. */
    if (!(powcnt & (e.isA ? 0x0002 : 0x0200))) {
        Fill(out, 0, e.width * e.height);
        return;
    }
    if (dispcnt & 0x80) {                  /* forced blank: white */
        Fill(out, 0x7FFF, e.width * e.height);
        return;
    }
    switch ((dispcnt >> 16) & (e.isA ? 3 : 1)) {
    case 0:                                /* display off: white */
        Fill(out, 0x7FFF, e.width * e.height);
        return;
    case 1: {
        int lines = e.height;

        DrawEngine(&e, out, split);
        if (e.height < lines) {
            /* (a tall picture asked for with the 3D on: the DS's lines
             * drawn, the rest black) */
            Fill(out + e.width * e.height, 0, e.width * (lines - e.height));
        }
        break;
    }
    case 2: {                              /* VRAM display: a bank in LCDC */
        const u8 *bank = NitroPort_LcdcVram + ((dispcnt >> 18) & 3) * 0x20000;
        int i;

        Fill(out, 0, e.width * e.height);
        for (i = 0; i < W * H; i++) {
            out[(i / W) * e.width + e.origin + i % W] = Rd16(bank, (u32)i * 2) & 0x7FFF;
        }
        break;
    }
    default:
        NitroPort_Unimplemented("display mode: main memory display");
        Fill(out, 0, e.width * e.height);
        return;
    }
    if (split == NULL) {
        MasterBrightness(out, Rd16(e.io, 0x6C), e.width * e.height);
    }
}

int NitroPort_Render2DLayer(int engine, int bg, const NitroRender2DSplit *layout, u16 *out, uint64_t *key, int *hofs,
                            int *vofs, int *prio)
{
    Engine e;
    u32 dispcnt;
    u16 bgcnt[4], line[WMAX];
    int kind[4], b, y;

    SetUpEngine(&e, engine, layout);
    dispcnt = Rd32(e.io, 0);
    if (((dispcnt >> 16) & 3) != 1 || (dispcnt & 0x80) || bg < 0 || bg > 3) {
        return 0;
    }
    FindExtPalettes(&e);
    MakePalettes(&e);
    for (b = 0; b < 4; b++) {
        bgcnt[b] = Rd16(e.io, 8 + b * 2);
        kind[b] = (dispcnt & (0x100u << b)) ? BgKind(&e, b, dispcnt) : BG_NONE;
        if (kind[b] == BG_LARGE) {
            kind[b] = BG_NONE;
        }
    }
    if (kind[bg] == BG_NONE || kind[bg] == BG_3D) {
        return 0;
    }
    if (e.width > W) {
        DecideSplits(&e, kind, bgcnt, dispcnt);
        LookUpArt(&e, kind, bgcnt, dispcnt);
        DecideBandFills(&e, kind, bgcnt, dispcnt);
    }
    *key = BgKey(&e, bg, kind[bg], bgcnt[bg], dispcnt);
    *hofs = kind[bg] == BG_TEXT ? Rd16(e.io, 0x10 + bg * 4) & 0x1FF : 0;
    *vofs = kind[bg] == BG_TEXT ? Rd16(e.io, 0x12 + bg * 4) & 0x1FF : 0;
    *prio = bgcnt[bg] & 3;
    for (y = 0; y < H; y++) {
        if (kind[bg] == BG_TEXT) {
            Target t;

            t.row = line;
            t.layerOf = NULL;
            t.enable = NULL;
            t.layer = (u8)bg;
            t.bit = (u8)(1 << bg);
            t.filled = 0;
            DrawTextBg(&e, bg, bgcnt[bg], dispcnt, y, &t, T_PLAIN);
        } else {
            DrawAffineBg(&e, bg, kind[bg], bgcnt[bg], dispcnt, y, line);
        }
        memcpy(out + y * e.width, line, (size_t)e.width * sizeof(u16));
    }
    return 1;
}

/* (capturing: the game's thread, between frames) the OBJ frame and line
 * of one group alone; the drawing threads have their own. */
static ObjFrame sCaptureObjFrame;
static ObjLine sCaptureObjLine;

/* SetUpEngine for one of the capture's single layers: 0 if the engine
 * shows no normal picture. */
static BOOL SetUpLayer(Engine *e, int engine, const NitroRender2DSplit *layout, u32 *dispcnt)
{
    SetUpEngine(e, engine, layout);
    *dispcnt = Rd32(e->io, 0);
    if (((*dispcnt >> 16) & 3) != 1 || (*dispcnt & 0x80)) {
        return FALSE;
    }
    FindExtPalettes(e);
    MakePalettes(e);
    return TRUE;
}

int NitroPort_Render2DObjGroups(int engine, const NitroRender2DSplit *layout, NitroObjGroup *groups, int max)
{
    Engine e;
    u32 dispcnt;

    if (!SetUpLayer(&e, engine, layout, &dispcnt) || !(dispcnt & 0x1000)) {
        return 0;
    }
    return ObjGroups(&e, dispcnt, groups, max);
}

int NitroPort_Render2DPictures(int engine, const NitroRender2DSplit *layout, NitroWidePictureShown *out, int max)
{
    Engine e;
    u32 dispcnt;
    int i, n = 0;

    if (!SetUpLayer(&e, engine, layout, &dispcnt)) {
        return 0;
    }
    for (i = 0; i < e.nPics && n < max; i++) {
        out[n].picture = e.pics[i].id;
        out[n].bg = e.pics[i].bg;
        out[n].prio = Rd16(e.io, 8 + e.pics[i].bg * 2) & 3;
        out[n].x = e.pics[i].x;
        out[n].y = e.pics[i].y;
        out[n].w = e.pics[i].w;
        out[n].h = e.pics[i].h;
        out[n].angle = e.pics[i].angle;
        n++;
    }
    return n;
}

int NitroPort_Render2DObjLayer(int engine, int group, const NitroRender2DSplit *layout, u16 *out)
{
    Engine e;
    u32 dispcnt;
    ObjFrame *f = &sCaptureObjFrame;
    ObjLine *line = &sCaptureObjLine;
    int y, x, k, any = 0;

    if (!SetUpLayer(&e, engine, layout, &dispcnt) || !(dispcnt & 0x1000)) {
        return 0;
    }
    SortObjs(&e, f);
    /* only the group's OBJs on each line */
    for (y = 0; y < H; y++) {
        int n = 0;

        for (k = 0; k < f->lineCount[y]; k++) {
            const Obj *obj = &f->obj[f->onLine[y][k]];

            if (obj->mode < 2 && ObjGroupOf(obj) == group) {
                f->onLine[y][n++] = f->onLine[y][k];
            }
        }
        f->lineCount[y] = (u8)n;
    }
    LookUpObjArt(&e, f, dispcnt, group);
    for (y = 0; y < H; y++) {
        u16 *row = out + y * e.width;

        if (!DrawObjs(&e, f, dispcnt, y, line)) {
            memset(row, 0, (size_t)e.width * sizeof(u16));
            continue;
        }
        for (x = 0; x < e.width; x++) {
            row[x] = (line->color[x] & OPAQUE) ? line->color[x] : 0;
            any |= row[x] != 0;
        }
    }
    return any;   /* (a group all off the screen: nothing to draw) */
}

/* ------------------------------------------------- sharing wide art */

/* The BG's own pixels over the canvas and a margin past its edges
 * (SHARE_MX, SHARE_MY), at the scroll a picture was made at (OwnTextPixel;
 * an affine BG's where its matrix takes it now): what the picture's
 * pixels are compared with, where they are and where they were moved
 * from (NITRO_WIDE_ART_MOVE_MAX either way, and a little up or down;
 * 832 x 224 pixels, 364 KB). */
#define SHARE_MX (NITRO_WIDE_ART_MOVE_MAX + 1)
#define SHARE_MY NITRO_WIDE_ART_MOVE_UP_MAX
#define SHARE_RW (NITRO_WIDE_ART_W + 2 * SHARE_MX)
#define SHARE_RH (H + 2 * SHARE_MY)
#define SHARE_SAMPLES 512
#define SHARE_VOTES_LEAST 16
static u16 *sShareRef;   /* (got the first time something is shared: the 3DS program never does) */
static u16 (*sShareVotes)[2 * SHARE_MX];
#define SHARE_REF(x, y) sShareRef[((y) + SHARE_MY) * SHARE_RW + (x) + SHARE_MX]

/* A picture's pixels that are the BG's own dx right and dy down of where
 * the BG has them: made NITRO_WIDE_ART_GAME, moved that far. How many. */
static int ShareMark(u16 *px, int dx, int dy)
{
    int x, y, n = 0;

    for (y = 0; y < H; y++) {
        int ry = y - dy;

        if (ry < -SHARE_MY || ry >= H + SHARE_MY) {
            continue;
        }
        for (x = 0; x < NITRO_WIDE_ART_W; x++) {
            u16 *c = &px[y * NITRO_WIDE_ART_W + x], g;
            int rx = x - dx;

            if (!(*c & OPAQUE) || rx < -SHARE_MX || rx >= NITRO_WIDE_ART_W + SHARE_MX) {
                continue;
            }
            g = SHARE_REF(rx, ry);
            if ((g & OPAQUE) && (g & 0x7FFF) == (*c & 0x7FFF)) {
                *c = NitroPort_WideArtGameAt(dx, dy);
                n++;
            }
        }
    }
    return n;
}

/* Whether a picture's pixel is at an edge (in the picture as made, orig:
 * not as its pixels are left out): opaque, and not the same as the one
 * left of it or the one above (the pixels a move can be told from: in a
 * plain stretch every pixel is the same as the BG's over a whole plain
 * stretch of it, wherever that is). */
static BOOL ShareEdgePixel(const u16 *px, int x, int y)
{
    u16 c = px[y * NITRO_WIDE_ART_W + x];

    return (c & OPAQUE) && ((x > 0 && px[y * NITRO_WIDE_ART_W + x - 1] != c) ||
                            (y > 0 && px[(y - 1) * NITRO_WIDE_ART_W + x] != c));
}

/* The move most of a picture's pixels (those left: not yet the game's
 * own) match the BG's own at, from a sample of those at edges (sideways
 * only, for a group of sprites: its picture's row is drawn from the
 * sprites on that row): how many votes it got, 0 when fewer than a
 * quarter of the sample agree (the sample's own art, matching the BG's
 * here and there by chance). */
static int ShareBestMove(const u16 *px, const u16 *orig, int sideways, int *dx, int *dy)
{
    u16 (*votes)[2 * SHARE_MX] = sShareVotes;
    int x, y, edges = 0, samples = 0, step, i = 0, best = 0, ox, oy;
    int oy0 = sideways ? 0 : -SHARE_MY, oy1 = sideways ? 1 : SHARE_MY;

    for (y = 0; y < H; y++) {
        for (x = 0; x < NITRO_WIDE_ART_W; x++) {
            edges += (px[y * NITRO_WIDE_ART_W + x] & OPAQUE) && ShareEdgePixel(orig, x, y);
        }
    }
    step = edges / SHARE_SAMPLES + 1;
    memset(votes, 0, sizeof(u16) * 2 * SHARE_MY * 2 * SHARE_MX);
    for (y = 0; y < H; y++) {
        for (x = 0; x < NITRO_WIDE_ART_W; x++) {
            u16 c = px[y * NITRO_WIDE_ART_W + x];

            if (!(c & OPAQUE) || !ShareEdgePixel(orig, x, y) || i++ % step != 0) {
                continue;
            }
            samples++;
            for (oy = oy0; oy < oy1; oy++) {
                int ry = y - oy;

                if (ry < -SHARE_MY || ry >= H + SHARE_MY) {
                    continue;
                }
                for (ox = -NITRO_WIDE_ART_MOVE_MAX; ox <= NITRO_WIDE_ART_MOVE_MAX; ox++) {
                    int rx = x - ox;

                    if (rx >= -SHARE_MX && rx < NITRO_WIDE_ART_W + SHARE_MX && SHARE_REF(rx, ry) == c) {
                        votes[oy + SHARE_MY][ox + SHARE_MX]++;
                    }
                }
            }
        }
    }
    /* (the shortest of the moves most agree on: a tile pattern that
     * repeats agrees on every move by the pattern's size) */
    *dx = *dy = 0;
    for (oy = oy0; oy < oy1; oy++) {
        for (ox = -NITRO_WIDE_ART_MOVE_MAX; ox <= NITRO_WIDE_ART_MOVE_MAX; ox++) {
            int v = votes[oy + SHARE_MY][ox + SHARE_MX];

            if ((ox != 0 || oy != 0) && (v > best || (v == best && (ox < 0 ? -ox : ox) + (oy < 0 ? -oy : oy) < (*dx < 0 ? -*dx : *dx) + (*dy < 0 ? -*dy : *dy)))) {
                best = v;
                *dx = ox;
                *dy = oy;
            }
        }
    }
    return best >= SHARE_VOTES_LEAST && best * 4 >= samples ? best : 0;
}

/* A picture of a BG with the BG's own pixels left out: those the same as
 * the BG's, where the picture was made, made NITRO_WIDE_ART_GAME (the
 * renderer puts the BG's own back there: ArtTextPixel, AffineSample),
 * where they are or moved from where the BG has them (the move most of
 * them match at, up to three times over: a Pokemon of the opening's moved
 * along, the map's beyond the DS's edge copied into a band); a picture of
 * the bands only has nothing in the DS's part (it isn't shown there). 0 if
 * the BG isn't where the picture can be told from (a BG laid out in parts,
 * or split, away from where its picture was made). */
static BOOL StripBgArt(const Engine *e, int bg, int kind, u16 bgcnt, u32 dispcnt, int hofs, int vofs, int full,
                       const u16 *orig, u16 *px)
{
    int x, y, curH = Rd16(e->io, 0x10 + bg * 4) & 0x1FF, curV = Rd16(e->io, 0x12 + bg * 4) & 0x1FF, round;
    BOOL parts = (e->partBgs >> bg) & 1;

    if (kind == BG_TEXT) {
        int mapW = sTextSizes[bgcnt >> 14][0], mapH = sTextSizes[bgcnt >> 14][1];
        OwnRow own;

        OwnRowStart(&own);
        /* (the BG as it's drawn where the picture was made: a BG laid out
         * in parts only when that's where it is) */
        if (parts && (curH != hofs || curV != vofs)) {
            return FALSE;
        }
        for (y = -SHARE_MY; y < H + SHARE_MY; y++) {
            for (x = -SHARE_MX; x < NITRO_WIDE_ART_W + SHARE_MX; x++) {
                SHARE_REF(x, y) = OwnTextPixel(e, bg, bgcnt, dispcnt, hofs, vofs, mapW, mapH, x, y, &own);
            }
        }
    } else {
        const u8 *io = e->io;
        u32 base = bg == 2 ? 0x20 : 0x30;
        s32 pa = (s16)Rd16(io, base + 0), pb = (s16)Rd16(io, base + 2);
        s32 pc = (s16)Rd16(io, base + 4), pd = (s16)Rd16(io, base + 6);
        s32 refX = (s32)(Rd32(io, base + 8) << 4) >> 4, refY = (s32)(Rd32(io, base + 12) << 4) >> 4;
        BOOL wrap = (bgcnt & 0x2000) != 0;
        int size = bgcnt >> 14, mode, mapW, mapH;
        u32 charBase = CharBase(e, bgcnt, dispcnt), screenBase = ScreenBase(e, bgcnt, dispcnt);
        const u8 *ext = NULL;

        if (parts) {
            return FALSE;   /* (split: where its matrix takes it, its parts) */
        }
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
            screenBase = ((bgcnt >> 8) & 0x1F) * 0x4000;
        }
        /* (the matrix as it is now: an affine BG's picture is used
         * wherever it's scrolled) */
        for (y = -SHARE_MY; y < H + SHARE_MY; y++) {
            for (x = -SHARE_MX; x < NITRO_WIDE_ART_W + SHARE_MX; x++) {
                SHARE_REF(x, y) = AffineSample(e, mode, ext, charBase, screenBase, mapW, mapH, wrap,
                                               (refX + pb * y + pa * (x - e->origin)) >> 8,
                                               (refY + pd * y + pc * (x - e->origin)) >> 8);
            }
        }
    }
    ShareMark(px, 0, 0);
    for (round = 0; round < 3; round++) {
        int dx, dy;

        if (ShareBestMove(px, orig, 0, &dx, &dy) == 0 || ShareMark(px, dx, dy) == 0) {
            break;
        }
    }
    if (!(full & NITRO_WIDE_ART_FULL)) {
        for (y = 0; y < H; y++) {
            memset(px + y * NITRO_WIDE_ART_W + e->origin, 0, W * sizeof(u16));
        }
    }
    return TRUE;
}

int NitroPort_Render2DShareArt(int engine)
{
    static u16 px[NITRO_WIDE_ART_W * H], layer[NITRO_WIDE_ART_W * H];
    static u8 oam[0x400];
    NitroRender2DSplit split;
    Engine e;
    u32 dispcnt;
    u16 bgcnt[4];
    int kind[4], b, shared = 0;
    char name[64];

    if (NitroPort_WideArtUnshared() == 0) {
        return 0;
    }
    if (sShareRef == NULL) {
        sShareRef = malloc(sizeof(u16) * SHARE_RW * SHARE_RH);
        sShareVotes = malloc(sizeof(u16) * 2 * SHARE_MY * 2 * SHARE_MX);
        if (sShareRef == NULL || sShareVotes == NULL) {
            NitroPort_Log("[wide art] no memory to share the pictures with");
            free(sShareRef);
            free(sShareVotes);
            sShareRef = NULL;
            sShareVotes = NULL;
            return 0;
        }
    }
    memset(&split, 0, sizeof(split));
    NitroPort_WideLayout(engine, &split);
    if (split.width != NITRO_WIDE_ART_W || !SetUpLayer(&e, engine, &split, &dispcnt)) {
        return 0;
    }
    for (b = 0; b < 4; b++) {
        bgcnt[b] = Rd16(e.io, 8 + b * 2);
        kind[b] = (dispcnt & (0x100u << b)) ? BgKind(&e, b, dispcnt) : BG_NONE;
    }
    for (b = 0; b < 4; b++) {
        const u16 *pixels;
        uint64_t key;
        int hofs, vofs, full, i = 0;

        if (kind[b] == BG_NONE || kind[b] == BG_3D || kind[b] == BG_LARGE) {
            continue;
        }
        key = BgKey(&e, b, kind[b], bgcnt[b], dispcnt);
        while (NitroPort_WideArtToShare(key, i, &pixels, &hofs, &vofs, &full, name, sizeof(name))) {
            memcpy(px, pixels, sizeof(px));
            if (StripBgArt(&e, b, kind[b], bgcnt[b], dispcnt, hofs, vofs, full, pixels, px) &&
                NitroPort_WideArtShareWrite(name, px) == 0) {
                shared++;
            } else {
                i++;   /* (not now: another time, where the BG can be told from) */
            }
        }
    }
    if (dispcnt & 0x1000) {
        /* The OBJ groups': their sprites' own pixels left out (a group's
         * picture only goes where none of its OBJs is anyway), and those
         * moved along made NITRO_WIDE_ART_GAME (drawn from the sprites on
         * the row: DrawObjs). */
        NitroObjGroup groups[32];
        int count = ObjGroups(&e, dispcnt, groups, 32), g;

        for (g = 0; g < count; g++) {
            const u16 *pixels;
            int hofs, vofs, full, i = 0, x, y, drawn = -1, drawnDx = 0, drawnDy = 0;

            while (NitroPort_WideArtToShare(groups[g].key, i, &pixels, &hofs, &vofs, &full, name, sizeof(name))) {
                /* (how far the group has moved since the picture was
                 * made, the shorter way round OAM's 512 x 256: as
                 * LookUpObjArt has it) */
                int dx = (groups[g].x - hofs + 512) & 511, dy = (groups[g].y - vofs + 256) & 255;

                dx = dx >= 256 ? dx - 512 : dx;
                dy = dy >= 128 ? dy - 256 : dy;

                memcpy(px, pixels, sizeof(px));
                if (drawn < 0 || dx != drawnDx || dy != drawnDy) {
                    /* the group alone, without its picture, back where
                     * the picture was made (its OBJs moved back by as far
                     * as the group has moved since: a group that slides
                     * is mostly off the canvas where it is now) */
                    ObjFrame *f = &sCaptureObjFrame;
                    ObjLine *line = &sCaptureObjLine;
                    Engine there = e;
                    int k;

                    memcpy(oam, e.oam, sizeof(oam));
                    for (k = 0; k < 128; k++) {
                        u8 *o = oam + k * 8;
                        u16 a0 = Rd16(o, 0), a1 = Rd16(o, 2), a2 = Rd16(o, 4);
                        int mode = (a0 >> 10) & 3, prio = (a2 >> 10) & 3, pal = (a2 >> 12) & 15;
                        int group = (prio << 5) | ((a0 & 0x2000) ? 16 : pal);

                        if (mode >= 2 || group != groups[g].group) {
                            continue;
                        }
                        a0 = (u16)((a0 & ~0xFFu) | ((a0 - dy) & 0xFF));
                        a1 = (u16)((a1 & ~0x1FFu) | ((a1 - dx) & 0x1FF));
                        o[0] = (u8)a0;
                        o[1] = (u8)(a0 >> 8);
                        o[2] = (u8)a1;
                        o[3] = (u8)(a1 >> 8);
                    }
                    there.oam = oam;
                    SortObjs(&there, f);
                    for (y = 0; y < H; y++) {
                        int n = 0;

                        for (k = 0; k < f->lineCount[y]; k++) {
                            const Obj *obj = &f->obj[f->onLine[y][k]];

                            if (obj->mode < 2 && ObjGroupOf(obj) == groups[g].group) {
                                f->onLine[y][n++] = f->onLine[y][k];
                            }
                        }
                        f->lineCount[y] = (u8)n;
                    }
                    f->artCount = 0;
                    drawn = 0;
                    drawnDx = dx;
                    drawnDy = dy;
                    for (y = 0; y < H; y++) {
                        u16 *row = layer + y * NITRO_WIDE_ART_W;

                        if (!DrawObjs(&there, f, dispcnt, y, line)) {
                            memset(row, 0, NITRO_WIDE_ART_W * sizeof(u16));
                            continue;
                        }
                        for (x = 0; x < NITRO_WIDE_ART_W; x++) {
                            row[x] = (line->color[x] & OPAQUE) ? line->color[x] : 0;
                            drawn |= row[x] != 0;
                        }
                    }
                }
                if (drawn > 0) {
                    /* The sprites' own pixels moved along (a strip of
                     * sprites that goes round, copied on into a band):
                     * the game's own, drawn from the sprites that far
                     * back on the row, wherever the strip is (first: a
                     * pixel the sprites cover now they may not cover
                     * elsewhere). */
                    int round, dx2, dy2;

                    memset(sShareRef, 0, sizeof(u16) * SHARE_RW * SHARE_RH);
                    for (y = 0; y < H; y++) {
                        for (x = 0; x < NITRO_WIDE_ART_W; x++) {
                            SHARE_REF(x, y) = layer[y * NITRO_WIDE_ART_W + x];
                        }
                    }
                    for (round = 0; round < 3; round++) {
                        if (ShareBestMove(px, pixels, 1, &dx2, &dy2) == 0 || ShareMark(px, dx2, dy2) == 0) {
                            break;
                        }
                    }
                }
                for (y = 0; y < H; y++) {
                    for (x = 0; x < NITRO_WIDE_ART_W; x++) {
                        u16 *c = &px[y * NITRO_WIDE_ART_W + x], g2 = layer[y * NITRO_WIDE_ART_W + x];

                        if (!(full & NITRO_WIDE_ART_FULL) && x >= e.origin && x < e.origin + W) {
                            *c = 0;
                        } else if (drawn > 0 && (*c & OPAQUE) && (g2 & OPAQUE) && (g2 & 0x7FFF) == (*c & 0x7FFF)) {
                            *c = 0;   /* (the sprites' own, where they are: nothing to add) */
                        }
                    }
                }
                if (NitroPort_WideArtShareWrite(name, px) == 0) {
                    shared++;
                } else {
                    i++;
                }
            }
        }
    }
    return shared;
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
void NitroPort_Render2DTakeState(int engine)
{
    State2D *st = &sState[engine ? 1 : 0];

    memcpy(st->io, NitroPort_IoReg, sizeof(st->io));
    memcpy(st->pltt, NitroPort_Pltt, sizeof(st->pltt));
    memcpy(st->oam, NitroPort_Oam, sizeof(st->oam));
    st->lines = NitroPort_LineRegs.active;
    if (st->lines) {
        memcpy(st->lineRegs, NitroPort_LineRegs.regs[engine ? 1 : 0], sizeof(st->lineRegs));
    }
    {
        int bg;

        st->winSerial = NitroPort_WideWindowsSerial();
        for (bg = 0; bg < 4; bg++) {
            st->nwins[bg] = NitroPort_WideWindows(engine ? 1 : 0, bg, st->wins[bg], NITRO_WIDE_MAX_WINDOWS);
        }
        st->nsplits = NitroPort_WideSplits(engine ? 1 : 0, st->splits);
        st->nparts = NitroPort_WideParts(engine ? 1 : 0, st->parts);
    }
}

/* What an engine's picture is drawn from, in two parts: the small ones
 * (registers, palettes, OAM) and its VRAM (the most part: 864 KB for
 * engine A, read in full to work it out). */
static u32 EngineSmallSignature(int engine, int snapshot)
{
    BOOL isA = engine == 0;
    const State2D *st = &sState[isA ? 0 : 1];
    const u8 *ioAll = snapshot ? st->io : NitroPort_IoReg;
    const u8 *io = ioAll + (isA ? 0 : 0x1000);
    u32 regs[0x70 / 4];
    u32 h = 2166136261u;

    /* Not DISP3DCNT's two acknowledge bits (12, 13: the 3D's colour
     * buffer underflow and polygon RAM overflow): the game writes them
     * every frame, alternating, and they change nothing drawn. Counted,
     * engine A's picture was drawn and upscaled again every other frame
     * of the field with nothing on it changed (about 15 times a second, 9
     * ms each on the 3D's core: 2026-10-15). */
    memcpy(regs, io, sizeof(regs));
    if (isA) {
        regs[0x60 / 4] &= ~0x3000u;
    }
    h = HashWords((const u8 *)regs, 0x70, h);
    h = HashWords(ioAll + 0x240, 0x10, h);              /* VRAMCNT */
    h = (h ^ Rd16(ioAll, 0x304)) * 16777619u;           /* POWCNT1 */
    h = HashWords((snapshot ? st->pltt : NitroPort_Pltt) + (isA ? 0 : 0x400), 0x400, h);
    h = HashWords((snapshot ? st->oam : NitroPort_Oam) + (isA ? 0 : 0x400), 0x400, h);
    /* (an H-blank effect: each line's registers, but those two bits) */
    if (snapshot ? st->lines : NitroPort_LineRegs.active) {
        const u8(*lines)[NITRO_LINE_REGS] = snapshot ? st->lineRegs : NitroPort_LineRegs.regs[isA ? 0 : 1];
        int y;

        h ^= 0x5A5A5A5Au;
        for (y = 0; y < H; y++) {
            memcpy(regs, lines[y], sizeof(regs));
            if (isA) {
                regs[0x60 / 4] &= ~0x3000u;
            }
            h = HashWords((const u8 *)regs, 0x70, h);
        }
    }
    return h;
}

static u32 EngineVramSignature(int engine, const u8 *io)
{
    u32 h = 2166136261u;

    if (engine == 0) {
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

static u32 EngineSignature(int engine)
{
    return (EngineSmallSignature(engine, 0) * 31u) ^
           EngineVramSignature(engine, NitroPort_IoReg + (engine ? 0x1000 : 0));
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
    static struct { u32 small, vram; int valid, vramValid; } last[2];
    int e = engine ? 1 : 0;
    u32 small = EngineSmallSignature(e, split->snapshot), vram;

    /* the canvas, and where the windows are */
    small = (small ^ (u32)split->width ^ ((u32)split->origin << 9) ^
             ((u32)split->anchorWindows << 18) ^ ((u32)(split->windowsToEdge != 0) << 19) ^ ((u32)split->clip[0] << 20) ^
             ((u32)split->clip[1] << 21) ^ ((u32)split->clip[2] << 22) ^
             ((u32)split->clip[3] << 23)) * 16777619u;
    small = (small ^ (u32)split->height) * 16777619u;   /* (a tall picture) */
    small = (small ^ (u32)__atomic_load_n(&sSmoothObjs[e], __ATOMIC_ACQUIRE)) * 16777619u;
    if (split->width > W) {
        /* (the windows decide the edges' look, and where the anchored ones
         * are; hand-drawn pictures loaded again) */
        small = (small ^ (split->snapshot ? sState[e].winSerial : NitroPort_WideWindowsSerial())) * 16777619u;
        small = (small ^ NitroPort_WideArtSerial()) * 16777619u;
    }
    /* The small parts changed (a scroll, a sprite, a colour: most frames of
     * anything moving): drawn, without reading the VRAM through to find
     * out about that as well. Only when they're the same is the VRAM
     * compared (once it's known: the first time after a change it's just
     * noted, and drawn). */
    if (!last[e].valid || small != last[e].small) {
        last[e].small = small;
        last[e].valid = 1;
        last[e].vramValid = 0;
        Render(engine, under, split);
        return 1;
    }
    vram = EngineVramSignature(e, (split->snapshot ? sState[e].io : NitroPort_IoReg) + (e ? 0x1000 : 0));
    if (last[e].vramValid && vram == last[e].vram) {
        return 0;
    }
    last[e].vram = vram;
    last[e].vramValid = 1;
    Render(engine, under, split);
    return 1;
}

unsigned NitroPort_Render2DMasterBright(int engine, int snapshot)
{
    const u8 *io = (snapshot ? sState[engine ? 1 : 0].io : NitroPort_IoReg) + (engine ? 0x1000 : 0);

    return Rd16(io, 0x6C);
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
