/*
 * The 2D renderer as it was before it was made faster (PR #51's version),
 * kept for test_render: the faster one must draw exactly the same pixels.
 * Its own names, so both can be linked into the test.
 */
#define NitroPort_Render2D Ref_Render2D
#define NitroPort_EngineAOnTop Ref_EngineAOnTop
#define NitroPort_RenderScreens Ref_RenderScreens
#include <nitro.h>
#include <nitro_port.h>
#include <string.h>

#define W 256
#define H 192

/* ---------------------------------------------------------------- memory */

extern unsigned char NitroPort_IoReg[];

static u16 Rd16(const u8 *p, u32 off)
{
    return (u16)(p[off] | (p[off + 1] << 8));
}

static u32 Rd32(const u8 *p, u32 off)
{
    return (u32)p[off] | ((u32)p[off + 1] << 8) | ((u32)p[off + 2] << 16) | ((u32)p[off + 3] << 24);
}

#define IO16(off) Rd16(NitroPort_IoReg, (off))
#define IO32(off) Rd32(NitroPort_IoReg, (off))

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
} Engine;

/* One layer's pixels on the current line. */
typedef struct Line {
    u16 color[W];
    u8 opaque[W];
} Line;

/* The OBJ layer's pixels on the current line. */
typedef struct ObjLine {
    u16 color[W];
    u8 opaque[W];
    u8 prio[W];
    u8 semi[W];            /* semi-transparent: blends with what's below */
    u8 alpha[W];           /* bitmap OBJ alpha 1-16 (16 = opaque) */
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

static u16 PlttColor(const u8 *pltt, int index)
{
    return Rd16(pltt, (u32)index * 2) & 0x7FFF;
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

static void DrawTextBg(const Engine *e, int bg, u16 bgcnt, u32 dispcnt, int y, Line *out)
{
    static const int sizes[4][2] = { { 256, 256 }, { 512, 256 }, { 256, 512 }, { 512, 512 } };
    int mapW = sizes[bgcnt >> 14][0];
    int mapH = sizes[bgcnt >> 14][1];
    int hofs;
    const u8 *io = e->io;
    u32 charBase = CharBase(e, bgcnt, dispcnt);
    u32 screenBase = ScreenBase(e, bgcnt, dispcnt);
    BOOL bpp8 = (bgcnt & 0x80) != 0;
    const u8 *ext = (bpp8 && (dispcnt & 0x40000000)) ? BgExtSlot(e, bg, bgcnt) : NULL;
    int ty, x;

    hofs = Rd16(io, 0x10 + bg * 4) & 0x1FF;
    ty = (y + (Rd16(io, 0x12 + bg * 4) & 0x1FF)) & (mapH - 1);

    for (x = 0; x < W; x++) {
        int tx = (x + hofs) & (mapW - 1);
        u32 block = (u32)((tx >> 8) + ((ty >> 8) * (mapW >> 8)));
        u32 entryAddr = screenBase + block * 0x800 + (u32)(((ty & 255) >> 3) * 32 + ((tx & 255) >> 3)) * 2;
        u16 entry = Rd16(e->bgVram, entryAddr & e->bgMask);
        int tile = entry & 0x3FF;
        int px = tx & 7;
        int py = ty & 7;
        int index;

        if (entry & 0x400) {
            px = 7 - px;
        }
        if (entry & 0x800) {
            py = 7 - py;
        }
        if (bpp8) {
            index = e->bgVram[(charBase + (u32)tile * 64 + (u32)py * 8 + (u32)px) & e->bgMask];
            if (index == 0) {
                out->opaque[x] = 0;
                continue;
            }
            out->color[x] = ext != NULL ? PlttColor(ext, ((entry >> 12) << 8) | index)
                                        : PlttColor(e->bgPltt, index);
        } else {
            u8 pair = e->bgVram[(charBase + (u32)tile * 32 + (u32)py * 4 + (u32)(px >> 1)) & e->bgMask];

            index = (px & 1) ? (pair >> 4) : (pair & 0xF);
            if (index == 0) {
                out->opaque[x] = 0;
                continue;
            }
            out->color[x] = PlttColor(e->bgPltt, ((entry >> 12) << 4) | index);
        }
        out->opaque[x] = 1;
    }
}

/* Affine and extended BGs: the texture coordinate of each pixel comes from
 * the reference point and the 2x2 matrix (8 fractional bits). */
static void DrawAffineBg(const Engine *e, int bg, int kind, u16 bgcnt, u32 dispcnt, int y, Line *out)
{
    const u8 *io = e->io;
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
        int index;

        out->opaque[x] = 0;
        if (wrap) {
            tx &= mapW - 1;
            ty &= mapH - 1;
        } else if (tx < 0 || ty < 0 || tx >= mapW || ty >= mapH) {
            continue;
        }

        if (mode == 3) {
            u16 c = Rd16(e->bgVram, (screenBase + ((u32)ty * (u32)mapW + (u32)tx) * 2) & e->bgMask);

            if (c & 0x8000) {
                out->color[x] = c & 0x7FFF;
                out->opaque[x] = 1;
            }
            continue;
        }
        if (mode == 2) {
            index = e->bgVram[(screenBase + (u32)ty * (u32)mapW + (u32)tx) & e->bgMask];
            if (index != 0) {
                out->color[x] = PlttColor(e->bgPltt, index);
                out->opaque[x] = 1;
            }
            continue;
        }
        if (mode == 0) {
            int tile = e->bgVram[(screenBase + (u32)(ty >> 3) * (u32)(mapW >> 3) + (u32)(tx >> 3)) & e->bgMask];

            index = e->bgVram[(charBase + (u32)tile * 64 + (u32)(ty & 7) * 8 + (u32)(tx & 7)) & e->bgMask];
            if (index != 0) {
                out->color[x] = PlttColor(e->bgPltt, index);
                out->opaque[x] = 1;
            }
            continue;
        }
        {
            u16 entry = Rd16(e->bgVram, (screenBase + ((u32)(ty >> 3) * (u32)(mapW >> 3) + (u32)(tx >> 3)) * 2) & e->bgMask);
            int px = tx & 7;
            int py = ty & 7;

            if (entry & 0x400) {
                px = 7 - px;
            }
            if (entry & 0x800) {
                py = 7 - py;
            }
            index = e->bgVram[(charBase + (u32)(entry & 0x3FF) * 64 + (u32)py * 8 + (u32)px) & e->bgMask];
            if (index != 0) {
                out->color[x] = ext != NULL ? PlttColor(ext, ((entry >> 12) << 8) | index)
                                            : PlttColor(e->bgPltt, index);
                out->opaque[x] = 1;
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

static void DrawObjs(const Engine *e, u32 dispcnt, int y, ObjLine *out)
{
    int i;

    memset(out->opaque, 0, sizeof(out->opaque));
    memset(out->window, 0, sizeof(out->window));
    if (!(dispcnt & 0x1000)) {
        return;
    }

    for (i = 0; i < 128; i++) {
        const u8 *o = e->oam + i * 8;
        u16 a0 = Rd16(o, 0), a1 = Rd16(o, 2), a2 = Rd16(o, 4);
        BOOL affine = (a0 & 0x100) != 0;
        int shape = (a0 >> 14) & 3;
        int mode = (a0 >> 10) & 3;
        int w, h, boxW, boxH, oy, ox, ly, sx;
        s32 pa = 256, pb = 0, pc = 0, pd = 256;
        BOOL bpp8 = (a0 & 0x2000) != 0;
        int tile = a2 & 0x3FF;
        int prio = (a2 >> 10) & 3;
        int pal = a2 >> 12;

        if (!affine && (a0 & 0x200)) {
            continue;                       /* hidden */
        }
        if (shape == 3) {
            continue;
        }
        w = sObjSize[shape][(a1 >> 14) & 3][0];
        h = sObjSize[shape][(a1 >> 14) & 3][1];
        boxW = w;
        boxH = h;
        if (affine) {
            int p = (a1 >> 9) & 0x1F;

            pa = (s16)Rd16(e->oam, (u32)p * 32 + 6);
            pb = (s16)Rd16(e->oam, (u32)p * 32 + 14);
            pc = (s16)Rd16(e->oam, (u32)p * 32 + 22);
            pd = (s16)Rd16(e->oam, (u32)p * 32 + 30);
            if (a0 & 0x200) {               /* double size */
                boxW *= 2;
                boxH *= 2;
            }
        }
        oy = a0 & 0xFF;
        ly = (y - oy) & 0xFF;
        if (ly >= boxH) {
            continue;
        }
        ox = a1 & 0x1FF;
        if (ox >= 256) {
            ox -= 512;
        }

        for (sx = 0; sx < boxW; sx++) {
            int x = ox + sx;
            int tx, tyy, color = -1;
            u16 rgb = 0;
            int alpha = 16;

            if (x < 0 || x >= W) {
                continue;
            }
            if (out->opaque[x] && mode != 2) {
                continue;                   /* a lower-numbered OBJ is in front */
            }
            if (affine) {
                s32 dx = sx - boxW / 2;
                s32 dy = ly - boxH / 2;

                tx = ((pa * dx + pb * dy) >> 8) + w / 2;
                tyy = ((pc * dx + pd * dy) >> 8) + h / 2;
                if (tx < 0 || tyy < 0 || tx >= w || tyy >= h) {
                    continue;
                }
            } else {
                tx = (a1 & 0x1000) ? w - 1 - sx : sx;
                tyy = (a1 & 0x2000) ? h - 1 - ly : ly;
            }

            if (mode == 3) {                /* bitmap OBJ: 15-bit colour */
                u32 addr;

                if (dispcnt & 0x40) {       /* 1D */
                    addr = (u32)tile * ((dispcnt & 0x400000) ? 256u : 128u) + ((u32)tyy * (u32)w + (u32)tx) * 2;
                } else if (dispcnt & 0x20) {    /* 2D, 256 dots wide */
                    addr = (u32)(tile & 0x1F) * 0x10 + (u32)(tile & ~0x1F) * 0x80 + ((u32)tyy * 256 + (u32)tx) * 2;
                } else {                    /* 2D, 128 dots wide */
                    addr = (u32)(tile & 0x0F) * 0x10 + (u32)(tile & ~0x0F) * 0x80 + ((u32)tyy * 128 + (u32)tx) * 2;
                }
                rgb = Rd16(e->objVram, addr & e->objMask);
                if (!(rgb & 0x8000) || pal == 0) {
                    continue;
                }
                rgb &= 0x7FFF;
                alpha = pal + 1;
                color = 1;
            } else {
                u32 addr;
                int index;
                int tileX = tx >> 3, tileY = tyy >> 3;

                if (dispcnt & 0x10) {       /* 1D character mapping */
                    u32 unit = 32u << ((dispcnt >> 20) & 3);
                    u32 n = (u32)(tileY * (w >> 3) + tileX);

                    addr = (u32)tile * unit + n * (bpp8 ? 64u : 32u);
                } else {                    /* 2D: 32 characters per row */
                    addr = (u32)tile * 32 + (u32)tileY * 1024 + (u32)tileX * (bpp8 ? 64u : 32u);
                }
                if (bpp8) {
                    index = e->objVram[(addr + (u32)(tyy & 7) * 8 + (u32)(tx & 7)) & e->objMask];
                    if (index == 0) {
                        continue;
                    }
                    rgb = (e->objExt != NULL && (dispcnt & 0x80000000))
                              ? PlttColor(e->objExt, (pal << 8) | index)
                              : PlttColor(e->objPltt, index);
                } else {
                    u8 pair = e->objVram[(addr + (u32)(tyy & 7) * 4 + (u32)((tx & 7) >> 1)) & e->objMask];

                    index = (tx & 1) ? (pair >> 4) : (pair & 0xF);
                    if (index == 0) {
                        continue;
                    }
                    rgb = PlttColor(e->objPltt, (pal << 4) | index);
                }
                color = 1;
            }

            if (color < 0) {
                continue;
            }
            if (mode == 2) {
                out->window[x] = 1;
                continue;
            }
            out->color[x] = rgb;
            out->opaque[x] = 1;
            out->prio[x] = (u8)prio;
            out->semi[x] = mode == 1;
            out->alpha[x] = (u8)alpha;
        }
    }
}

/* ------------------------------------------------------------ the frame */

static u16 Blend(u16 a, u16 b, int eva, int evb)
{
    int r = ((a & 31) * eva + (b & 31) * evb) >> 4;
    int g = (((a >> 5) & 31) * eva + ((b >> 5) & 31) * evb) >> 4;
    int bl = (((a >> 10) & 31) * eva + ((b >> 10) & 31) * evb) >> 4;

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

static void DrawEngine(Engine *e, u16 *out)
{
    static Line bgLine[4];
    static ObjLine objLine;
    const u8 *io = e->io;
    u32 dispcnt = Rd32(io, 0);
    u16 bgcnt[4];
    int kind[4];
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
    int y, x, bg;

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
    for (bg = 0; bg < 4; bg++) {
        bgcnt[bg] = Rd16(io, 8 + bg * 2);
        kind[bg] = (dispcnt & (0x100u << bg)) ? BgKind(e, bg, dispcnt) : BG_NONE;
        if (kind[bg] == BG_3D || kind[bg] == BG_LARGE) {
            kind[bg] = BG_NONE;   /* not drawn yet (see the top of this file) */
        }
    }

    for (y = 0; y < H; y++) {
        u16 *row = out + y * W;
        BOOL inWin0V = (dispcnt & 0x2000) && InRange(y, win0v >> 8, win0v & 0xFF);
        BOOL inWin1V = (dispcnt & 0x4000) && InRange(y, win1v >> 8, win1v & 0xFF);

        for (bg = 0; bg < 4; bg++) {
            if (kind[bg] == BG_TEXT) {
                DrawTextBg(e, bg, bgcnt[bg], dispcnt, y, &bgLine[bg]);
            } else if (kind[bg] == BG_AFFINE || kind[bg] == BG_EXTENDED) {
                DrawAffineBg(e, bg, kind[bg], bgcnt[bg], dispcnt, y, &bgLine[bg]);
            }
        }
        DrawObjs(e, dispcnt, y, &objLine);

        for (x = 0; x < W; x++) {
            /* Which layers and effects this pixel shows (the windows). */
            u8 enable = 0x3F;
            int top = 5, second = 5;          /* 0-3 BG, 4 OBJ, 5 backdrop */
            u16 topColor = backdrop, secondColor = backdrop;
            int topPrio = 4, secondPrio = 4;
            int p;
            u16 c;

            if (anyWindow) {
                if (inWin0V && InRange(x, win0h >> 8, win0h & 0xFF)) {
                    enable = winin & 0x3F;
                } else if (inWin1V && InRange(x, win1h >> 8, win1h & 0xFF)) {
                    enable = (winin >> 8) & 0x3F;
                } else if ((dispcnt & 0x8000) && objLine.window[x]) {
                    enable = (winout >> 8) & 0x3F;
                } else {
                    enable = winout & 0x3F;
                }
            }

            /* The two front-most visible layers: by priority, OBJ before
             * a BG of the same priority, then BG0 before BG1 ... */
            for (p = 0; p < 4; p++) {
                if ((enable & 0x10) && objLine.opaque[x] && objLine.prio[x] == p) {
                    if (top == 5) {
                        top = 4;
                        topColor = objLine.color[x];
                        topPrio = p;
                    } else if (second == 5) {
                        second = 4;
                        secondColor = objLine.color[x];
                        secondPrio = p;
                    }
                }
                for (bg = 0; bg < 4; bg++) {
                    if (kind[bg] == BG_NONE || !(enable & (1 << bg)) ||
                        (bgcnt[bg] & 3) != p || !bgLine[bg].opaque[x]) {
                        continue;
                    }
                    if (top == 5) {
                        top = bg;
                        topColor = bgLine[bg].color[x];
                        topPrio = p;
                    } else if (second == 5) {
                        second = bg;
                        secondColor = bgLine[bg].color[x];
                        secondPrio = p;
                    }
                }
            }
            (void)topPrio;
            (void)secondPrio;

            c = topColor;
            if (top == 4 && (objLine.semi[x] || objLine.alpha[x] < 16) &&
                (bldcnt & (0x100 << second))) {
                /* Semi-transparent and bitmap OBJs blend with what's below,
                 * whatever the effect setting. */
                if (objLine.alpha[x] < 16) {
                    c = Blend(topColor, secondColor, objLine.alpha[x], 16 - objLine.alpha[x]);
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
