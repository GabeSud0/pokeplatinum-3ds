/*
 * Tests for the software 2D renderer (src/port_render2d.c).
 *
 * Each test sets the display up with the SDK's own functions (GX_*, G2_*),
 * as a game does, then draws and checks pixels. Tiles, maps and palettes
 * are made up here: no game data. Everything the SDK copies by DMA is
 * static: its DMA registers hold 32-bit addresses, and on a 64-bit PC the
 * stack is above 4 GB.
 */

#include <nitro.h>
#include <nitro_port.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* tests/render2d_reference.c: the renderer before it was made faster. */
void Ref_RenderScreens(u16 *top, u16 *bottom);

extern unsigned char NitroPort_IoReg[];

static int sChecks;
static int sFailures;

#define CHECK(cond)                                                          \
    do {                                                                     \
        sChecks++;                                                           \
        if (!(cond)) {                                                       \
            sFailures++;                                                     \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);           \
        }                                                                    \
    } while (0)

#define CHECK_PX(buf, x, y, want)                                            \
    do {                                                                     \
        u16 got_ = (buf)[(y) * 256 + (x)];                                   \
        sChecks++;                                                           \
        if (got_ != (u16)(want)) {                                           \
            sFailures++;                                                     \
            printf("FAIL %s:%d: pixel (%d,%d) = %#06x, expected %#06x\n",     \
                   __FILE__, __LINE__, (x), (y), got_, (unsigned)(want));    \
        }                                                                    \
    } while (0)

#define RGB(r, g, b) ((u16)((r) | ((g) << 5) | ((b) << 10)))

#define RED RGB(31, 0, 0)
#define GREEN RGB(0, 31, 0)
#define BLUE RGB(0, 0, 31)
#define GREY RGB(10, 10, 10)
#define WHITE RGB(31, 31, 31)

static u16 sTop[256 * 192];
static u16 sBottom[256 * 192];

/* A clean engine A: BG mode 0, nothing visible, banks A for BG, B for OBJ. */
static void Reset(void)
{
    GX_Init();
    GX_SetPower(GX_POWER_ALL);
    GX_SetDispSelect(GX_DISP_SELECT_MAIN_SUB);
    GX_SetBankForBG(GX_VRAM_BG_128_A);
    GX_SetBankForOBJ(GX_VRAM_OBJ_128_B);
    GX_SetBankForSubBG(GX_VRAM_SUB_BG_128_C);
    GX_SetGraphicsMode(GX_DISPMODE_GRAPHICS, GX_BGMODE_0, GX_BG0_AS_2D);
    GXS_SetGraphicsMode(GX_BGMODE_0);
    GX_SetVisiblePlane(0);
    GXS_SetVisiblePlane(0);
    G2_BlendNone();
    GX_SetMasterBrightness(0);
    MI_CpuClearFast(NitroPort_BgVram, 0x20000);
    MI_CpuClearFast(NitroPort_ObjVram, 0x20000);
    MI_CpuClearFast(NitroPort_DbBgVram, 0x20000);
    MI_CpuClearFast(NitroPort_Oam, 0x800);
    MI_CpuClearFast(NitroPort_Pltt, 0x800);
    GX_DispOn();
    GXS_DispOn();
}

/* One 4bpp tile filled with colour index `index`, with index 2 in its top
 * left pixel. */
static void MakeTile4(u8 *tile, int index)
{
    int i;

    for (i = 0; i < 32; i++) {
        tile[i] = (u8)(index | (index << 4));
    }
    tile[0] = (u8)((tile[0] & 0xF0) | 2);
}

static void test_backdrop_and_power(void)
{
    static u16 backdrop = GREY;

    Reset();
    GX_LoadBGPltt(&backdrop, 0, 2);
    NitroPort_Render2D(0, sTop);
    CHECK_PX(sTop, 0, 0, GREY);
    CHECK_PX(sTop, 255, 191, GREY);

    GX_DispOff();
    NitroPort_Render2D(0, sTop);
    CHECK_PX(sTop, 10, 10, WHITE);      /* display off shows white */
    GX_DispOn();

    GX_SetMasterBrightness(-16);        /* fully faded to black */
    NitroPort_Render2D(0, sTop);
    CHECK_PX(sTop, 10, 10, 0);
    GX_SetMasterBrightness(16);         /* fully faded to white */
    NitroPort_Render2D(0, sTop);
    CHECK_PX(sTop, 10, 10, WHITE);
    GX_SetMasterBrightness(0);
}

static void test_text_bg(void)
{
    static u8 tiles[3 * 32];
    static u16 map[32 * 32];
    static u16 pltt[16] = { GREY, RED, GREEN, BLUE };
    int i;

    Reset();
    MakeTile4(tiles + 0, 0);            /* tile 0: transparent but pixel (0,0) */
    MakeTile4(tiles + 32, 1);           /* tile 1: red, green top-left */
    MakeTile4(tiles + 64, 3);           /* tile 2: blue, green top-left */
    for (i = 0; i < 32 * 32; i++) {
        map[i] = 0;
    }
    map[0] = 1;                         /* (0,0): tile 1 */
    map[1] = 1 | 0x400;                 /* (8,0): tile 1, flipped horizontally */
    map[32] = 2 | 0x800;                /* (0,8): tile 2, flipped vertically */
    map[33] = 1 | (1 << 12);            /* (8,8): tile 1, palette 1 */

    G2_SetBG0Control(GX_BG_SCRSIZE_TEXT_256x256, GX_BG_COLORMODE_16, GX_BG_SCRBASE_0x0000,
                     GX_BG_CHARBASE_0x04000, GX_BG_EXTPLTT_01);
    GX_LoadBG0Char(tiles, 0, sizeof(tiles));
    GX_LoadBG0Scr(map, 0, sizeof(map));
    GX_LoadBGPltt(pltt, 0, sizeof(pltt));
    {
        static u16 pal1[16] = { 0, WHITE, WHITE, WHITE };

        GX_LoadBGPltt(pal1, 32, sizeof(pal1));
    }
    GX_SetVisiblePlane(GX_PLANEMASK_BG0);
    NitroPort_Render2D(0, sTop);

    CHECK_PX(sTop, 0, 0, GREEN);        /* tile 1's top-left */
    CHECK_PX(sTop, 3, 3, RED);
    CHECK_PX(sTop, 15, 0, GREEN);       /* flipped: top-left is now top-right */
    CHECK_PX(sTop, 8, 0, RED);
    CHECK_PX(sTop, 0, 15, GREEN);       /* flipped vertically: bottom-left */
    CHECK_PX(sTop, 0, 8, BLUE);
    CHECK_PX(sTop, 12, 12, WHITE);      /* palette 1 */
    CHECK_PX(sTop, 1, 17, GREY);        /* tile 0: transparent, backdrop */
    CHECK_PX(sTop, 16, 16, GREEN);      /* tile 0's one opaque pixel (index 2) */

    G2_SetBG0Offset(8, 0);              /* scroll left by 8: (8,0) moves to (0,0) */
    NitroPort_Render2D(0, sTop);
    CHECK_PX(sTop, 7, 0, GREEN);
    CHECK_PX(sTop, 0, 0, RED);
    G2_SetBG0Offset(0, 0);

    /* Engine B shows the same through its own registers and VRAM, and
     * the screens swap with GX_SetDispSelect. */
    G2S_SetBG0Control(GX_BG_SCRSIZE_TEXT_256x256, GX_BG_COLORMODE_16, GX_BG_SCRBASE_0x0000,
                      GX_BG_CHARBASE_0x04000, GX_BG_EXTPLTT_01);
    GXS_LoadBG0Char(tiles, 0, sizeof(tiles));
    GXS_LoadBG0Scr(map, 0, sizeof(map));
    GXS_LoadBGPltt(pltt, 0, sizeof(pltt));
    GXS_SetVisiblePlane(GX_PLANEMASK_BG0);
    NitroPort_RenderScreens(sTop, sBottom);
    CHECK(NitroPort_EngineAOnTop());
    CHECK_PX(sBottom, 3, 3, RED);
    CHECK_PX(sBottom, 12, 12, 0);       /* engine B's palette 1 is empty: black */
}

static void test_8bpp_ext_palette(void)
{
    static u8 tile[64];
    static u16 map[32 * 32];
    static u16 ext[256 * 2];            /* slot 0: palettes 0 and 1 */
    static u16 normal[256];
    int i;

    Reset();
    for (i = 0; i < 64; i++) {
        tile[i] = 5;
    }
    for (i = 0; i < 256; i++) {
        normal[i] = RED;
    }
    for (i = 0; i < 256; i++) {
        ext[i] = GREEN;                 /* ext palette 0 */
        ext[256 + i] = BLUE;            /* ext palette 1 */
    }
    map[0] = 0;                         /* palette 0 */
    map[1] = 0 | (1 << 12);             /* palette 1 */

    G2_SetBG0Control(GX_BG_SCRSIZE_TEXT_256x256, GX_BG_COLORMODE_256, GX_BG_SCRBASE_0x0000,
                     GX_BG_CHARBASE_0x04000, GX_BG_EXTPLTT_01);
    GX_LoadBG0Char(tile, 0, sizeof(tile));
    GX_LoadBG0Scr(map, 0, sizeof(map));
    GX_LoadBGPltt(normal, 0, sizeof(normal));
    GX_SetVisiblePlane(GX_PLANEMASK_BG0);

    NitroPort_Render2D(0, sTop);        /* no extended palettes: the normal one */
    CHECK_PX(sTop, 1, 1, RED);
    CHECK_PX(sTop, 9, 1, RED);

    GX_SetBankForBGExtPltt(GX_VRAM_BGEXTPLTT_0123_E);
    GX_BeginLoadBGExtPltt();
    GX_LoadBGExtPltt(ext, 0, sizeof(ext));
    GX_EndLoadBGExtPltt();
    NitroPort_Render2D(0, sTop);
    CHECK_PX(sTop, 1, 1, GREEN);
    CHECK_PX(sTop, 9, 1, BLUE);
}

static void test_objs(void)
{
    static u8 tiles[2 * 32];
    static u16 map[32 * 32];
    static u16 bgPltt[16] = { GREY, RED };
    static u16 objPltt[16] = { 0, BLUE, GREEN };
    static GXOamAttr oam[128];
    int i;

    Reset();
    GX_SetOBJVRamModeChar(GX_OBJVRAMMODE_CHAR_1D_32K);
    MakeTile4(tiles, 1);                /* OBJ tile 0: index 1 (blue) */
    MakeTile4(tiles + 32, 2);           /* OBJ tile 1: index 2 (green) */
    GX_LoadOBJ(tiles, 0, sizeof(tiles));
    GX_LoadOBJPltt(objPltt, 0, sizeof(objPltt));

    /* a BG behind, all red, priority 1 */
    for (i = 0; i < 32 * 32; i++) {
        map[i] = 1;
    }
    G2_SetBG0Control(GX_BG_SCRSIZE_TEXT_256x256, GX_BG_COLORMODE_16, GX_BG_SCRBASE_0x0000,
                     GX_BG_CHARBASE_0x04000, GX_BG_EXTPLTT_01);
    {
        static u8 bgTiles[64];

        /* (after BG0's control: the load goes to its character base) */
        MakeTile4(bgTiles + 32, 1);
        GX_LoadBG0Char(bgTiles, 0, sizeof(bgTiles));
    }
    G2_SetBG0Priority(1);
    GX_LoadBG0Scr(map, 0, sizeof(map));
    GX_LoadBGPltt(bgPltt, 0, sizeof(bgPltt));

    MI_CpuFillFast(oam, 0x0200, sizeof(oam));   /* all hidden */
    /* OBJ 0: 16x8 at (40,30), tiles 0-1, priority 0 (in front of the BG) */
    G2_SetOBJAttr(&oam[0], 40, 30, 0, GX_OAM_MODE_NORMAL, FALSE, GX_OAM_EFFECT_NONE,
                  GX_OAM_SHAPE_16x8, GX_OAM_COLORMODE_16, 0, 0, 0);   /* priority 0 */
    /* OBJ 1: the same, flipped horizontally, at (40,50), priority 2 (behind) */
    G2_SetOBJAttr(&oam[1], 40, 50, 2, GX_OAM_MODE_NORMAL, FALSE, GX_OAM_EFFECT_FLIP_H,
                  GX_OAM_SHAPE_16x8, GX_OAM_COLORMODE_16, 0, 0, 0);   /* priority 2 */
    /* OBJ 2: at x = 500 (= -12), so only its right 4 pixels show */
    G2_SetOBJAttr(&oam[2], 500, 70, 0, GX_OAM_MODE_NORMAL, FALSE, GX_OAM_EFFECT_NONE,
                  GX_OAM_SHAPE_16x8, GX_OAM_COLORMODE_16, 0, 0, 0);
    DC_FlushRange(oam, sizeof(oam));
    GX_LoadOAM(oam, 0, sizeof(oam));
    GX_SetVisiblePlane(GX_PLANEMASK_BG0 | GX_PLANEMASK_OBJ);
    NitroPort_Render2D(0, sTop);

    CHECK_PX(sTop, 42, 32, BLUE);       /* tile 0 */
    CHECK_PX(sTop, 50, 32, GREEN);      /* tile 1 */
    CHECK_PX(sTop, 40, 30, GREEN);      /* tile 0's top-left index 2 */
    CHECK_PX(sTop, 60, 32, RED);        /* past the OBJ: the BG */
    CHECK_PX(sTop, 42, 52, RED);        /* OBJ 1 is behind the BG */
    CHECK_PX(sTop, 2, 72, GREEN);       /* OBJ 2's tile 1, from x = -12 */

    G2_SetBG0Priority(3);               /* now OBJ 1 is in front, and flipped */
    NitroPort_Render2D(0, sTop);
    CHECK_PX(sTop, 42, 52, GREEN);      /* flipped: tile 1 on the left */
    CHECK_PX(sTop, 50, 52, BLUE);
    CHECK_PX(sTop, 55, 50, GREEN);      /* tile 0's top-left, mirrored */
}

static void test_blend_and_window(void)
{
    static u8 tiles[64];
    static u16 map[32 * 32];
    static u16 pltt[16] = { 0, RGB(16, 0, 0), RGB(0, 16, 0) };
    int i;

    Reset();
    MakeTile4(tiles + 32, 1);
    for (i = 0; i < 32 * 32; i++) {
        map[i] = 1;
    }
    G2_SetBG0Control(GX_BG_SCRSIZE_TEXT_256x256, GX_BG_COLORMODE_16, GX_BG_SCRBASE_0x0000,
                     GX_BG_CHARBASE_0x04000, GX_BG_EXTPLTT_01);
    GX_LoadBG0Char(tiles, 0, sizeof(tiles));
    GX_LoadBG0Scr(map, 0, sizeof(map));
    GX_LoadBGPltt(pltt, 0, sizeof(pltt));
    GX_SetVisiblePlane(GX_PLANEMASK_BG0);

    /* BG0 (red 16) darkened by half */
    G2_SetBlendBrightness(GX_BLEND_PLANEMASK_BG0, -8);
    NitroPort_Render2D(0, sTop);
    CHECK_PX(sTop, 5, 5, RGB(8, 0, 0));
    G2_BlendNone();

    /* a window: BG0 shows only inside (16,16)-(32,32) */
    G2_SetWnd0Position(16, 16, 32, 32);
    G2_SetWnd0InsidePlane(GX_WND_PLANEMASK_BG0, FALSE);
    G2_SetWndOutsidePlane(GX_WND_PLANEMASK_NONE, FALSE);
    GX_SetVisibleWnd(GX_WNDMASK_W0);
    NitroPort_Render2D(0, sTop);
    CHECK_PX(sTop, 20, 20, RGB(16, 0, 0));
    CHECK_PX(sTop, 40, 20, 0);          /* outside: the backdrop (colour 0, black) */
    CHECK_PX(sTop, 20, 32, 0);          /* the bottom edge is exclusive */
    GX_SetVisibleWnd(GX_WNDMASK_NONE);
}

static void test_bitmap_bg(void)
{
    static u16 bitmap[256 * 256];

    Reset();
    GX_SetGraphicsMode(GX_DISPMODE_GRAPHICS, GX_BGMODE_5, GX_BG0_AS_2D);
    bitmap[0] = RED | 0x8000;
    bitmap[1] = GREEN;                  /* alpha bit clear: transparent */
    bitmap[256 * 2 + 3] = BLUE | 0x8000;
    G2_SetBG3ControlDCBmp(GX_BG_SCRSIZE_DCBMP_256x256, GX_BG_AREAOVER_XLU, GX_BG_BMPSCRBASE_0x00000);
    GX_LoadBG3Bmp(bitmap, 0, sizeof(bitmap));
    {
        MtxFx22 identity;

        MTX_Identity22(&identity);
        G2_SetBG3Affine(&identity, 0, 0, 0, 0);
    }
    GX_SetVisiblePlane(GX_PLANEMASK_BG3);
    NitroPort_Render2D(0, sTop);
    CHECK_PX(sTop, 0, 0, RED);
    CHECK_PX(sTop, 1, 0, 0);            /* transparent: backdrop */
    CHECK_PX(sTop, 3, 2, BLUE);
}

static void test_affine_bg(void)
{
    static u8 tiles[2 * 64];
    static u8 map[16 * 16];             /* 128x128: 16x16 tiles, 8-bit entries */
    static u16 pltt[256];
    MtxFx22 m;
    int i;

    Reset();
    GX_SetGraphicsMode(GX_DISPMODE_GRAPHICS, GX_BGMODE_1, GX_BG0_AS_2D);
    for (i = 0; i < 64; i++) {
        tiles[64 + i] = 7;              /* tile 1: colour 7 */
    }
    for (i = 0; i < 16 * 16; i++) {
        map[i] = 0;
    }
    map[0] = 1;                         /* top-left tile */
    pltt[0] = GREY;
    pltt[7] = RED;
    G2_SetBG3ControlAffine(GX_BG_SCRSIZE_AFFINE_128x128, GX_BG_AREAOVER_XLU,
                           GX_BG_SCRBASE_0x0000, GX_BG_CHARBASE_0x04000);
    GX_LoadBG3Char(tiles, 0, sizeof(tiles));
    GX_LoadBG3Scr(map, 0, sizeof(map));
    GX_LoadBGPltt(pltt, 0, sizeof(pltt));
    GX_SetVisiblePlane(GX_PLANEMASK_BG3);

    MTX_Identity22(&m);
    G2_SetBG3Affine(&m, 0, 0, 0, 0);
    NitroPort_Render2D(0, sTop);
    CHECK_PX(sTop, 3, 3, RED);
    CHECK_PX(sTop, 9, 3, GREY);         /* tile 0: colour 0, transparent */
    CHECK_PX(sTop, 130, 3, GREY);       /* outside 128x128, no wrap: nothing */

    /* scaled by 2 (the matrix halves the step): the tile covers 16x16 */
    MTX_Scale22(&m, FX32_ONE / 2, FX32_ONE / 2);
    G2_SetBG3Affine(&m, 0, 0, 0, 0);
    NitroPort_Render2D(0, sTop);
    CHECK_PX(sTop, 14, 14, RED);
    CHECK_PX(sTop, 17, 3, GREY);

    /* wrapping: the 128x128 map repeats */
    MTX_Identity22(&m);
    G2_SetBG3Affine(&m, 0, 0, 0, 0);
    G2_SetBG3ControlAffine(GX_BG_SCRSIZE_AFFINE_128x128, GX_BG_AREAOVER_REPEAT,
                           GX_BG_SCRBASE_0x0000, GX_BG_CHARBASE_0x04000);
    NitroPort_Render2D(0, sTop);
    CHECK_PX(sTop, 131, 3, RED);
}

static void test_obj_8bpp_ext(void)
{
    static u8 tile[64];
    static u16 ext[256 * 3];            /* OBJ extended palettes 0-2 */
    static GXOamAttr oam[128];
    int i;

    Reset();
    GX_SetOBJVRamModeChar(GX_OBJVRAMMODE_CHAR_1D_32K);
    for (i = 0; i < 64; i++) {
        tile[i] = 9;
    }
    for (i = 0; i < 256; i++) {
        ext[512 + i] = BLUE;            /* palette 2 */
    }
    GX_LoadOBJ(tile, 0, sizeof(tile));
    GX_SetBankForOBJExtPltt(GX_VRAM_OBJEXTPLTT_0_F);
    GX_BeginLoadOBJExtPltt();
    GX_LoadOBJExtPltt(ext, 0, sizeof(ext));
    GX_EndLoadOBJExtPltt();
    MI_CpuFillFast(oam, 0x0200, sizeof(oam));
    G2_SetOBJAttr(&oam[0], 100, 100, 0, GX_OAM_MODE_NORMAL, FALSE, GX_OAM_EFFECT_NONE,
                  GX_OAM_SHAPE_8x8, GX_OAM_COLORMODE_256, 0, 2, 0);
    GX_LoadOAM(oam, 0, sizeof(oam));
    GX_SetVisiblePlane(GX_PLANEMASK_OBJ);
    NitroPort_Render2D(0, sTop);
    CHECK_PX(sTop, 103, 103, BLUE);
    CHECK_PX(sTop, 108, 103, 0);
}

/* ------------------------------------------------------------------------
 * The faster renderer against the old one: random displays (every BG mode,
 * BG kind, OBJ kind, window and effect, random graphics), both must draw
 * exactly the same pixels.
 */
static u32 sSeed = 2024;

static u32 Rnd(void)
{
    sSeed = sSeed * 1103515245u + 12345u;
    return sSeed >> 8;
}

/* Graphics with plenty of transparent (0) pixels and some blank tiles. */
static void RandomGraphics(u8 *p, u32 size)
{
    u32 i;
    int sparse = Rnd() % 3;

    for (i = 0; i < size; i += 4) {
        u32 v = (Rnd() << 8) ^ Rnd();

        if (sparse == 1) {
            v &= (Rnd() | Rnd() << 16) & 0x0F0F0F0F ? 0xFFFFFFFF : 0x0F0F0F0F;
        } else if (sparse == 2 && Rnd() % 4 == 0) {
            v = 0;
        }
        memcpy(p + i, &v, 4);
    }
}

static void Put16(u32 off, u16 v)
{
    NitroPort_IoReg[off] = (u8)v;
    NitroPort_IoReg[off + 1] = (u8)(v >> 8);
}

static void Put32(u32 off, u32 v)
{
    Put16(off, (u16)v);
    Put16(off + 2, (u16)(v >> 16));
}

static void RandomEngine(u32 io)
{
    u32 dispcnt = (Rnd() << 16) ^ Rnd();
    int i;

    dispcnt &= ~0x80u;                     /* forced blank: rarely */
    if (Rnd() % 16 == 0) {
        dispcnt |= 0x80;
    }
    dispcnt = (dispcnt & ~0x30000u) | ((Rnd() % 10 == 0 ? Rnd() % 4 : 1) << 16);
    if (Rnd() % 2) {
        dispcnt &= ~0xE000u;               /* half without windows */
    }
    Put32(io, dispcnt);
    for (i = 0; i < 4; i++) {
        Put16(io + 8 + i * 2, (u16)Rnd());
        Put16(io + 0x10 + i * 4, (u16)Rnd());
        Put16(io + 0x12 + i * 4, (u16)Rnd());
    }
    for (i = 0; i < 2; i++) {               /* BG2/BG3 affine */
        u32 b = 0x20 + i * 0x10;
        BOOL plain = Rnd() % 2;

        Put16(io + b + 0, plain ? 256 : (u16)Rnd());
        Put16(io + b + 2, plain ? 0 : (u16)Rnd());
        Put16(io + b + 4, plain ? 0 : (u16)Rnd());
        Put16(io + b + 6, plain ? 256 : (u16)Rnd());
        Put32(io + b + 8, (Rnd() << 8) ^ Rnd());
        Put32(io + b + 12, (Rnd() << 8) ^ Rnd());
    }
    for (i = 0x40; i < 0x4C; i += 2) {      /* windows */
        Put16(io + i, (u16)Rnd());
    }
    Put16(io + 0x50, Rnd() % 3 == 0 ? 0 : (u16)Rnd());   /* effects */
    Put16(io + 0x52, (u16)Rnd());
    Put16(io + 0x54, (u16)Rnd());
    Put16(io + 0x6C, Rnd() % 3 == 0 ? (u16)Rnd() : 0);   /* master brightness */
}

static void RandomOam(u8 *oam)
{
    int i;

    for (i = 0; i < 128; i++) {
        u16 a0 = (u16)Rnd(), a1 = (u16)Rnd(), a2 = (u16)Rnd();

        if (Rnd() % 3 == 0) {
            a0 = (u16)((a0 & ~0x300) | 0x200);   /* hidden */
        }
        if (Rnd() % 2) {
            a0 &= ~0x100;                         /* mostly not affine */
        }
        oam[i * 8 + 0] = (u8)a0;
        oam[i * 8 + 1] = (u8)(a0 >> 8);
        oam[i * 8 + 2] = (u8)a1;
        oam[i * 8 + 3] = (u8)(a1 >> 8);
        oam[i * 8 + 4] = (u8)a2;
        oam[i * 8 + 5] = (u8)(a2 >> 8);
        oam[i * 8 + 6] = (u8)Rnd();               /* affine parameters */
        oam[i * 8 + 7] = (u8)Rnd();
    }
}

static void RandomDisplay(void)
{
    int i;

    Put16(0x304, (u16)(0x0203 | (Rnd() % 2 ? 0x8000 : 0) | (Rnd() % 16 == 0 ? 0 : 0)));
    for (i = 0; i < 10; i++) {              /* VRAMCNT A-I (0x247 is WRAM) */
        if (i != 7) {
            NitroPort_IoReg[0x240 + i] = (u8)(Rnd() % 3 == 0 ? Rnd() : 0);
        }
    }
    RandomEngine(0);
    RandomEngine(0x1000);
    RandomGraphics(NitroPort_Pltt, 0x800);
    RandomOam(NitroPort_Oam);
    RandomOam(NitroPort_Oam + 0x400);
    RandomGraphics(NitroPort_BgVram, 0x80000);
    RandomGraphics(NitroPort_ObjVram, 0x40000);
    RandomGraphics(NitroPort_DbBgVram, 0x20000);
    RandomGraphics(NitroPort_DbObjVram, 0x20000);
    RandomGraphics(NitroPort_LcdcVram, 0xA4000);
}

static void test_matches_reference(void)
{
    static u16 refTop[256 * 192], refBottom[256 * 192];
    int n, differ = 0;

    for (n = 0; n < 400 && differ < 5; n++) {
        RandomDisplay();
        if (n % 4 == 0) {
            /* Often: the usual case, mode 0 with all BGs and OBJs. */
            Put32(0, 0x10011F10u | (Rnd() % 2 ? 0x40000000u : 0));
        }
        NitroPort_RenderScreens(sTop, sBottom);
        Ref_RenderScreens(refTop, refBottom);
        if (memcmp(sTop, refTop, sizeof(sTop)) != 0 ||
            memcmp(sBottom, refBottom, sizeof(sBottom)) != 0) {
            int i;

            for (i = 0; i < 256 * 192; i++) {
                if (sTop[i] != refTop[i] || sBottom[i] != refBottom[i]) {
                    printf("display %d: first difference at (%d,%d): %#06x/%#06x, "
                           "expected %#06x/%#06x\n", n, i % 256, i / 256,
                           sTop[i], sBottom[i], refTop[i], refBottom[i]);
                    break;
                }
            }
            differ++;
        }
    }
    CHECK(differ == 0);
}

/* The two engines drawn at the same time on two threads (as the 3DS
 * program does on two CPU cores), many times over: the same pixels. */
static u16 sThreadOut[2][256 * 192];

static void *DrawEngineThread(void *arg)
{
    int engine = (int)(intptr_t)arg;
    int i;

    for (i = 0; i < 20; i++) {
        NitroPort_Render2D(engine, sThreadOut[engine]);
    }
    return NULL;
}

static void test_engines_on_two_threads(void)
{
    static u16 refA[256 * 192], refB[256 * 192];
    int n, differ = 0;

    for (n = 0; n < 20; n++) {
        pthread_t a, b;

        RandomDisplay();
        Put32(0, 0x10011F10u);
        Put32(0x1000, 0x10011F10u);
        Put16(0x304, 0x0203);   /* engine A on the lower screen: B is the top */
        Ref_RenderScreens(refB, refA);
        pthread_create(&a, NULL, DrawEngineThread, (void *)(intptr_t)0);
        pthread_create(&b, NULL, DrawEngineThread, (void *)(intptr_t)1);
        pthread_join(a, NULL);
        pthread_join(b, NULL);
        if (memcmp(sThreadOut[0], refA, sizeof(refA)) != 0 ||
            memcmp(sThreadOut[1], refB, sizeof(refB)) != 0) {
            differ++;
        }
    }
    CHECK(differ == 0);
}

/* test_render --bench: how long a busy frame (both screens, four BGs, many
 * OBJs, blending) takes, with the old renderer and the new one. */
static double Now(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

static void BenchScene(int busy)
{
    int i, k;

    sSeed = 7;
    RandomDisplay();
    Put16(0x304, 0x8203);
    NitroPort_IoReg[0x244] = 0;            /* no extended palettes */
    for (i = 0; i < 2; i++) {
        u32 io = i * 0x1000;
        u8 *oam = NitroPort_Oam + i * 0x400;

        Put16(io + 8, 0x0000);
        Put16(io + 10, 0x0101);
        Put16(io + 12, 0x0202);
        Put16(io + 14, 0x0303);
        Put16(io + 0x6C, 0);
        if (busy) {
            /* mode 0, BG0-3 of random graphics, OBJ 1D, everything blended */
            Put32(io, 0x10011F10u);
            Put16(io + 0x50, 0x3F41);
            continue;
        }
        /* As the game's screens mostly are: an opaque background (BG3),
         * a half-empty layer over it (BG2), a mostly empty text window (BG1),
         * twenty 32x32 OBJs, no effects. */
        Put32(io, 0x00011E10u);
        Put16(io + 0x50, 0);
        Put16(io + 14, 0x1C03);            /* BG3: map at 0xE000, chars at 0 */
        Put16(io + 12, 0x1A06);            /* BG2: map at 0xD000, chars at 0x4000 */
        Put16(io + 10, 0x1809);            /* BG1: map at 0xC000, chars at 0x8000 */
        {
            u8 *vram = i == 0 ? NitroPort_BgVram : NitroPort_DbBgVram;

            for (k = 0; k < 0x4000; k++) {
                vram[k] |= 0x11;                       /* BG3's chars: no 0s */
                if ((k / 32) % 2) {
                    vram[0x4000 + k] = 0;              /* BG2: every other tile empty */
                }
                vram[0x8000 + k] = (k / 32) % 8 ? 0 : vram[0x8000 + k];  /* BG1: 1 in 8 */
            }
            for (k = 0; k < 0x800; k += 2) {           /* maps: tiles 0-511, no flips */
                u16 t = (u16)((k / 2) & 0x1FF);

                memcpy(vram + 0xE000 + k, &t, 2);
                memcpy(vram + 0xD000 + k, &t, 2);
                memcpy(vram + 0xC000 + k, &t, 2);
            }
        }
        for (k = 0; k < 128; k++) {
            u16 a0 = k < 20 ? (u16)(0x0000 | ((k * 9) % 180)) : 0x0200;  /* square, 4 bpp */
            u16 a1 = (u16)(0x8000 | ((k * 37) % 240));                   /* 32x32 */
            u16 a2 = (u16)(k * 16);

            memcpy(oam + k * 8, &a0, 2);
            memcpy(oam + k * 8 + 2, &a1, 2);
            memcpy(oam + k * 8 + 4, &a2, 2);
        }
    }
}

static void Bench(int only)
{
    static u16 top[256 * 192], bottom[256 * 192];
    int scene, which, i;

    for (scene = 0; scene < 2; scene++) {
        if (only >= 0 && scene != only) {
            continue;
        }
        BenchScene(scene);
        for (which = 0; which < 2; which++) {
            double start = Now();

            for (i = 0; i < 120; i++) {
                if (which == 0) {
                    Ref_RenderScreens(top, bottom);
                } else {
                    NitroPort_RenderScreens(top, bottom);
                }
            }
            printf("%s scene, %s renderer: %.2f ms a frame\n", scene ? "busy" : "typical",
                   which == 0 ? "old" : "new", (Now() - start) * 1000.0 / 120);
        }
    }
}

int main(int argc, char **argv)
{
    OS_Init();
    if (argc > 1 && strcmp(argv[1], "--bench") == 0) {
        Bench(argc > 2 ? atoi(argv[2]) : -1);
        return 0;
    }

    test_backdrop_and_power();
    test_text_bg();
    test_8bpp_ext_palette();
    test_objs();
    test_blend_and_window();
    test_bitmap_bg();
    test_affine_bg();
    test_obj_8bpp_ext();
    test_matches_reference();
    test_engines_on_two_threads();

    printf("2D renderer: %d checks, %d failed\n", sChecks, sFailures);
    return sFailures == 0 ? 0 : 1;
}
