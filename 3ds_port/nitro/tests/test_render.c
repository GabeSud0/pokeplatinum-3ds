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
#include <nitro_port_gx3d.h>
#include <nitro_port_wide.h>
#include <nitro_port_wideart.h>
#include <unistd.h>
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

/* A screen whose registers, palettes, OAM and VRAM didn't change isn't
 * drawn again; any change to them draws it. */
static void test_unchanged_screen(void)
{
    static u16 backdrop = GREY, other = WHITE;
    static u16 tile[16] = { 0x1111 };

    Reset();
    GX_LoadBGPltt(&backdrop, 0, 2);
    CHECK(NitroPort_Render2DIfChanged(0, sTop) == 1);    /* first: drawn */
    CHECK_PX(sTop, 0, 0, GREY);
    CHECK(NitroPort_Render2DIfChanged(0, sTop) == 0);    /* nothing changed */
    GX_LoadBGPltt(&other, 0, 2);                          /* a palette */
    CHECK(NitroPort_Render2DIfChanged(0, sTop) == 1);
    CHECK_PX(sTop, 0, 0, WHITE);
    CHECK(NitroPort_Render2DIfChanged(0, sTop) == 0);
    GX_LoadBG0Char(tile, 0x400, sizeof(tile));            /* VRAM */
    CHECK(NitroPort_Render2DIfChanged(0, sTop) == 1);
    G2_SetBG0Offset(3, 0);                                /* a register */
    CHECK(NitroPort_Render2DIfChanged(0, sTop) == 1);
    CHECK(NitroPort_Render2DIfChanged(0, sTop) == 0);
    CHECK(NitroPort_Render2DIfChanged(1, sBottom) == 1); /* engine B: its own */
    CHECK(NitroPort_Render2DIfChanged(1, sBottom) == 0);
    CHECK(NitroPort_Render2DIfChanged(0, sTop) == 0);
}

/* While the 3D keeps changing, the 3D screen is drawn for each new 3D
 * image only; a few calls after the last one, changes are looked for as
 * before (and the screen drawn once, to be sure it's up to date). */
static void test_screen_follows_3d(void)
{
    static uint32_t rgba[256 * 192];
    static u16 backdrop = GREY, other = WHITE;
    int i;

    Reset();
    GX_SetGraphicsMode(GX_DISPMODE_GRAPHICS, GX_BGMODE_0, GX_BG0_AS_3D);
    GX_SetVisiblePlane(GX_PLANEMASK_BG0);
    GX_LoadBGPltt(&backdrop, 0, 2);
    NitroPort_Gx3dPublishRGBA8(rgba, 256 * 192);
    CHECK(NitroPort_Render2DIfChanged(0, sTop) == 1);    /* a new 3D image */
    GX_LoadBGPltt(&other, 0, 2);
    CHECK(NitroPort_Render2DIfChanged(0, sTop) == 0);    /* 2D only: waits */
    NitroPort_Gx3dPublishRGBA8(rgba, 256 * 192);
    CHECK(NitroPort_Render2DIfChanged(0, sTop) == 1);    /* with the next 3D */
    CHECK_PX(sTop, 0, 0, WHITE);
    for (i = 0; i < 4; i++) {
        CHECK(NitroPort_Render2DIfChanged(0, sTop) == 0);
    }
    CHECK(NitroPort_Render2DIfChanged(0, sTop) == 1);    /* the 3D stopped */
    CHECK(NitroPort_Render2DIfChanged(0, sTop) == 0);
    GX_LoadBGPltt(&backdrop, 0, 2);
    CHECK(NitroPort_Render2DIfChanged(0, sTop) == 1);    /* changes seen again */
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

/* Two layers blending, the ways the reference comparison can't check: a
 * partly transparent 3D pixel over a BG, where the 3D covers the whole line
 * (the BG behind is only drawn for that pixel), and a first target with
 * nothing but the backdrop behind it. */
static void test_3d_blends_with_layer_behind(void)
{
    static uint32_t rgba[256 * 192];
    static u8 tiles[64];
    static u16 map[32 * 32];
    static u16 pltt[16] = { GREY, BLUE, BLUE };
    int i;

    Reset();
    MakeTile4(tiles + 32, 1);
    for (i = 0; i < 32 * 32; i++) {
        map[i] = 1;
    }
    for (i = 0; i < 256 * 192; i++) {
        rgba[i] = 0xF80000FFu;          /* opaque red (0xRRGGBBAA) */
    }
    rgba[5 * 256 + 10] = 0x00800080u;   /* green at alpha 128, premultiplied */
    GX_SetGraphicsMode(GX_DISPMODE_GRAPHICS, GX_BGMODE_0, GX_BG0_AS_3D);
    G2_SetBG1Control(GX_BG_SCRSIZE_TEXT_256x256, GX_BG_COLORMODE_16, GX_BG_SCRBASE_0x0000,
                     GX_BG_CHARBASE_0x04000, GX_BG_EXTPLTT_01);
    GX_LoadBG1Char(tiles, 0, sizeof(tiles));
    GX_LoadBG1Scr(map, 0, sizeof(map));
    GX_LoadBGPltt(pltt, 0, sizeof(pltt));
    G2_SetBG0Priority(0);
    G2_SetBG1Priority(1);
    GX_SetVisiblePlane(GX_PLANEMASK_BG0 | GX_PLANEMASK_BG1);
    NitroPort_Gx3dPublishRGBA8(rgba, 256 * 192);

    /* Alpha blending with no first target, BG1 as the second: only the
     * partly transparent 3D pixel blends, by its own alpha (16 of 31:
     * half and half). */
    G2_SetBlendAlpha(GX_BLEND_PLANEMASK_NONE, GX_BLEND_PLANEMASK_BG1, 16, 0);
    NitroPort_Render2D(0, sTop);
    CHECK_PX(sTop, 10, 5, RGB(0, 15, 15));
    CHECK_PX(sTop, 11, 5, RED);
    CHECK_PX(sTop, 10, 6, RED);

    /* BG1 not a second target: the 3D pixel as it is. */
    G2_SetBlendAlpha(GX_BLEND_PLANEMASK_NONE, GX_BLEND_PLANEMASK_BG2, 16, 0);
    NitroPort_Render2D(0, sTop);
    CHECK_PX(sTop, 10, 5, GREEN);

    /* BG1 alone, a first target with only the backdrop as the second. */
    GX_SetVisiblePlane(GX_PLANEMASK_BG1);
    G2_SetBlendAlpha(GX_BLEND_PLANEMASK_BG1, GX_BLEND_PLANEMASK_BD, 8, 8);
    NitroPort_Render2D(0, sTop);
    CHECK_PX(sTop, 20, 20, RGB(5, 5, 20));   /* (blue + grey) / 2 */
    G2_BlendNone();

    /* An empty 3D image again, for the tests after this one. */
    memset(rgba, 0, sizeof(rgba));
    NitroPort_Gx3dPublishRGBA8(rgba, 256 * 192);
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

static u32 Rd32Test(u32 off)
{
    u32 v;

    memcpy(&v, NitroPort_IoReg + off, 4);
    return v;
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
    if (Rnd() % 4 == 0) {
        /* Alpha blending with no first target, as battles set it. */
        Put16(io + 0x50, (u16)(((Rnd() & 0x3F) << 8) | 0x40));
    }
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

/* The picture in two layers (NitroPort_Render2DSplit), with the 3D put
 * between them here the way the 3DS's compositor does: the same picture as
 * drawing it in one, where the compositor's blending is the DS's: every
 * layer a second target of the 3D's alpha, eva + evb = 16, effects
 * allowed everywhere by the windows, no master brightness (that's the
 * compositor's too). */
static u16 Blend16(u16 a, u16 b, int ea, int eb)
{
    int r = ((a & 31) * ea + (b & 31) * eb) >> 4;
    int g = (((a >> 5) & 31) * ea + ((b >> 5) & 31) * eb) >> 4;
    int bl = (((a >> 10) & 31) * ea + ((b >> 10) & 31) * eb) >> 4;

    return RGB(r > 31 ? 31 : r, g > 31 ? 31 : g, bl > 31 ? 31 : bl);
}

static u16 Bright(u16 c, int evy, int up)
{
    int r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;

    if (up) {
        r += ((31 - r) * evy) >> 4;
        g += ((31 - g) * evy) >> 4;
        b += ((31 - b) * evy) >> 4;
    } else {
        r -= (r * evy) >> 4;
        g -= (g * evy) >> 4;
        b -= (b * evy) >> 4;
    }
    return RGB(r, g, b);
}

static void test_split_layers(void)
{
    static uint32_t rgba[256 * 192];
    static u16 under[256 * 192], over[256 * 192], composed[256 * 192];
    static u8 overAlpha[256 * 192];
    NitroRender2DSplit split;
    int n, i, differ = 0, with3d = 0, blended = 0;

    split.over = over;
    split.overAlpha = overAlpha;
    for (n = 0; n < 300 && differ < 5; n++) {
        u16 bldcnt, win;

        RandomDisplay();
        /* Engine A: mode 0 with BG0 as the 3D, the display mode normal,
         * all BGs and OBJs. */
        Put32(0, 0x10011F18u | (Rnd() % 2 ? 0x40000000u : 0) | (Rnd() % 2 ? 0xE000u : 0));
        Put16(0x6C, 0);
        bldcnt = (u16)((Rnd() & 0x3F) | 0x3F00 | ((Rnd() % 4) << 6));
        if (n % 2) {
            bldcnt &= ~1u;   /* half the time the 3D isn't a first target */
        }
        Put16(0x50, bldcnt);
        i = (int)(Rnd() % 17);
        Put16(0x52, (u16)(i | ((16 - i) << 8)));   /* eva + evb = 16 */
        Put16(0x54, (u16)(Rnd() % 17));
        for (win = 0x48; win <= 0x4A; win += 2) {
            Put16(win, (u16)(Rnd() | 0x2121));   /* effects on, and the 3D shown, everywhere */
        }
        if (Rnd() % 3 == 0) {
            Put16(0x10, (u16)(Rnd() % 300));    /* the 3D scrolled */
        } else {
            Put16(0x10, 0);
        }
        /* A 3D image: opaque, see-through, or (rarely) a whole picture. */
        for (i = 0; i < 256 * 192; i++) {
            u32 c = Rnd();

            rgba[i] = (Rnd() % 3 == 0) ? 0 : ((c & 0xF8F8F800u) | 0xFFu);
        }
        NitroPort_Gx3dPublishRGBA8(rgba, 256 * 192);

        NitroPort_Render2D(0, sTop);
        NitroPort_Render2DSplit(0, under, &split);
        CHECK(split.has3d);
        with3d++;
        /* Composed: under, the 3D over it (moved left by the scroll, see-
         * through where its alpha is 0), then the layers in front. Where
         * the 3D is a first target of the effect and covers the pixel,
         * the compositor's result is an approximation (the DS blends or
         * brightens the 3D pixel before anything in front of it blends
         * with it, and doesn't blend the layer behind it with its own
         * second target): those pixels aren't compared. */
        for (i = 0; i < 256 * 192; i++) {
            int x = i % 256, y = i / 256, sx = (x + split.scroll3d) & 0x1FF;
            u16 c = under[i];
            const u16 *image3d = NitroPort_Gx3dAcquireImage();

            if (sx < 256 && (image3d[y * 256 + sx] & 0x8000)) {
                c = image3d[y * 256 + sx] & 0x7FFF;
                if (split.tint3d != 0) {
                    NitroPort_Gx3dReleaseImage(image3d);
                    composed[i] = sTop[i];   /* (not compared) */
                    continue;
                }
                if (split.tint3d == 1) {
                    c = Blend16(c, under[i], split.eva, split.evb);
                } else if (split.tint3d == 2) {
                    c = Bright(c, split.evy, 1);
                } else if (split.tint3d == 3) {
                    c = Bright(c, split.evy, 0);
                }
            }
            NitroPort_Gx3dReleaseImage(image3d);
            if (over[i] & 0x8000) {
                if (overAlpha[i] < 16) {
                    if (((bldcnt >> 6) & 3) != 0 && (bldcnt & 0x3E)) {
                        /* (a BG behind the 3D may be a first target: the
                         * compositor blends with it as under shows it,
                         * already blended or brightened; the DS with the
                         * BG's own colour) */
                        composed[i] = sTop[i];
                        continue;
                    }
                    c = Blend16(over[i] & 0x7FFF, c, overAlpha[i], 16 - overAlpha[i]);
                    blended++;
                } else {
                    c = over[i] & 0x7FFF;
                }
            }
            composed[i] = c;
        }
        if (memcmp(composed, sTop, sizeof(sTop)) != 0) {
            for (i = 0; i < 256 * 192; i++) {
                if (composed[i] != sTop[i]) {
                    {
                        const u16 *im = NitroPort_Gx3dAcquireImage();
                        int sx = ((i % 256) + split.scroll3d) & 0x1FF;
                        u16 px3 = sx < 256 ? im[(i / 256) * 256 + sx] : 0;
                        NitroPort_Gx3dReleaseImage(im);
                        printf("split display %d: first difference at (%d,%d): composed %#06x, drawn %#06x (over %#06x alpha %d, under %#06x, 3D %#06x scroll %d; BLDCNT %#06x BLDALPHA %#06x DISPCNT %#010x)\n",
                               n, i % 256, i / 256, composed[i], sTop[i], over[i], overAlpha[i], under[i], px3, split.scroll3d, bldcnt, NitroPort_IoReg[0x52] | (NitroPort_IoReg[0x53] << 8), *(u32 *)NitroPort_IoReg);
                    }
                    break;
                }
            }
            differ++;
        }
    }
    CHECK(differ == 0);
    CHECK(with3d == n);
    CHECK(blended > 0);   /* (the layers in front blended with the 3D somewhere) */

    /* A window that hides the 3D: there, the layers in front show what's
     * behind the 3D instead (and a pixel that would blend with the 3D
     * blends with that). */
    RandomDisplay();
    Put32(0, 0x10011F18u | 0x2000u);   /* window 0 on */
    Put16(0x6C, 0);
    Put16(0x50, 0x3F00);               /* no effect */
    Put16(0x40, 0x1080);               /* window 0: x 16-128 */
    Put16(0x44, 0x1080);               /* y 16-128 */
    Put16(0x48, 0x3E);                 /* inside: everything but BG0 */
    Put16(0x4A, 0x3F);                 /* outside: everything */
    Put16(0x10, 0);                    /* the 3D not scrolled */
    Put16(0x52, 0x0808);               /* eva + evb = 16 */
    for (i = 0; i < 256 * 192; i++) {
        rgba[i] = 0xF80000FFu;         /* an opaque red 3D image */
    }
    NitroPort_Gx3dPublishRGBA8(rgba, 256 * 192);
    NitroPort_Render2D(0, sTop);
    NitroPort_Render2DSplit(0, under, &split);
    CHECK(split.has3d);
    differ = 0;
    for (i = 0; i < 256 * 192; i++) {
        int x = i % 256, y = i / 256;
        u16 c = RED;

        if (over[i] & 0x8000) {
            c = overAlpha[i] < 16 ? Blend16(over[i] & 0x7FFF, RED, overAlpha[i], 16 - overAlpha[i])
                                  : (u16)(over[i] & 0x7FFF);
        }
        if (x >= 16 && x < 128 && y >= 16 && y < 128) {
            differ += !(over[i] & 0x8000) || overAlpha[i] != 16;   /* the 3D hidden: covered */
        }
        differ += c != sTop[i];
    }
    CHECK(differ == 0);

    /* Without a 3D layer the whole picture is under, as drawn in one. */
    RandomDisplay();
    Put32(0, 0x10011F10u);
    Put16(0x6C, 0);
    NitroPort_Render2D(0, sTop);
    NitroPort_Render2DSplit(0, under, &split);
    CHECK(!split.has3d);
    CHECK(memcmp(under, sTop, sizeof(sTop)) == 0);
    /* The master brightness is for the compositor. */
    Put16(0x6C, 0x4008);
    NitroPort_Render2DSplit(0, under, &split);
    CHECK(split.masterBright == 0x4008);
    CHECK(memcmp(under, sTop, sizeof(sTop)) == 0);
    memset(rgba, 0, sizeof(rgba));
    NitroPort_Gx3dPublishRGBA8(rgba, 256 * 192);
}

/* The wider canvas (NitroRender2DSplit's width and origin): the DS's
 * picture in the middle, the same as drawn at the DS's width, with only the
 * backdrop or OBJs beyond it; and a window of the game's moved to the edge
 * nearer it. */
static void test_wide_canvas(void)
{
    static u16 wide[320 * 192], over[320 * 192];
    static u8 overAlpha[320 * 192];
    NitroRender2DSplit split;
    int n, differ = 0;

    memset(&split, 0, sizeof(split));
    split.over = over;
    split.overAlpha = overAlpha;
    split.width = 320;
    split.origin = 32;
    memset(split.clip, 1, sizeof(split.clip));
    for (n = 0; n < 200 && differ < 5; n++) {
        int y, x;

        RandomDisplay();
        /* engine A, BG0 2D (not the 3D) */
        Put32(0, (Rd32Test(0) & ~0x8u));
        Put16(0x6C, 0);
        NitroPort_Render2D(0, sTop);
        NitroPort_Render2DSplit(0, wide, &split);
        for (y = 0; y < 192; y++) {
            for (x = 0; x < 256; x++) {
                if (wide[y * 320 + 32 + x] != sTop[y * 256 + x]) {
                    if (differ < 5) {
                        printf("wide canvas %d: (%d,%d) %#06x, at the DS's width %#06x\n", n, x, y,
                               wide[y * 320 + 32 + x], sTop[y * 256 + x]);
                    }
                    differ++;
                    y = 192;
                    break;
                }
            }
        }
    }
    CHECK(differ == 0);

    /* A window of the game's on BG1, at the left of the screen: tiles 0-7 of
     * rows 0-3 (the DS's x 0-63, y 0-31). Anchored to the left, on the wider
     * canvas it goes to the left edge (x 0-63), and where it was (32-95)
     * shows what's behind BG1 (the backdrop). Not anchored, it stays where
     * the DS has it. */
    {
        static u8 tiles[96];
        static u16 map[32 * 32];
        static u16 pltt[16] = { GREY, RED, BLUE };
        static struct { void *config; u8 layer, left, top, width, height; } window = { NULL, 1, 0, 0, 8, 4 };
        int i;

        Reset();
        MakeTile4(tiles + 32, 1);
        MakeTile4(tiles + 64, 2);
        for (i = 0; i < 32 * 32; i++) {
            map[i] = ((i % 32) < 8 && (i / 32) < 4) ? 1 : 0;   /* the window's tiles red */
        }
        G2_SetBG1Control(GX_BG_SCRSIZE_TEXT_256x256, GX_BG_COLORMODE_16, GX_BG_SCRBASE_0x0000,
                         GX_BG_CHARBASE_0x04000, GX_BG_EXTPLTT_01);
        GX_LoadBG1Char(tiles, 0, sizeof(tiles));
        GX_LoadBG1Scr(map, 0, sizeof(map));
        GX_LoadBGPltt(pltt, 0, sizeof(pltt));
        GX_SetVisiblePlane(GX_PLANEMASK_BG1);
        NitroPort_WindowAdded(&window, 1, 0, 0, 8, 4);
        split.anchorWindows = 1;
        NitroPort_Render2DSplit(0, wide, &split);
        CHECK(wide[5 * 320 + 5] == RED);          /* not anchored: where the DS has it... */
        CHECK(wide[5 * 320 + 32] == RED);
        CHECK(wide[5 * 320 + 95] == RED);
        CHECK(wide[5 * 320 + 96] == GREY);
        /* (...and its plain red edge goes on into the band) */
        NitroPort_WindowAnchor(&window, -1, 0);
        NitroPort_Render2DSplit(0, wide, &split);
        CHECK(wide[5 * 320 + 5] == RED);          /* moved to the left edge */
        CHECK(wide[5 * 320 + 63] == RED);
        CHECK(wide[5 * 320 + 64] == GREY);        /* where it was: the backdrop */
        CHECK(wide[5 * 320 + 95] == GREY);
        CHECK(wide[40 * 320 + 5] == GREY);        /* below the window: the band */
        /* Only its first 4 tiles anchored: they go to the edge; the rest
         * (tiles 4-7, red too) stay where the DS has them. */
        NitroPort_WindowAnchor(&window, -1, 4);
        NitroPort_Render2DSplit(0, wide, &split);
        CHECK(wide[5 * 320 + 5] == RED);          /* tiles 0-3 at the edge: x 0-31 */
        CHECK(wide[5 * 320 + 31] == RED);
        CHECK(wide[5 * 320 + 32] == GREY);        /* where tiles 0-3 were */
        CHECK(wide[5 * 320 + 63] == GREY);
        CHECK(wide[5 * 320 + 64] == RED);         /* tiles 4-7, where they were */
        CHECK(wide[5 * 320 + 95] == RED);
        CHECK(wide[5 * 320 + 96] == GREY);
        NitroPort_WindowRemoved(&window);
        /* On the picture screens (the opening, the title), a BG with none of
         * the game's windows is a picture: where its edge isn't plain, it's
         * mirrored at the DS's edge. Tiles 0 and 1 of row
         * 0: red, blue (each tile's top left pixel blue: MakeTile4); the
         * rest empty. Line 0's edge isn't plain (that pixel), line 5's is. */
        for (i = 0; i < 32 * 32; i++) {
            map[i] = (i == 0) ? 1 : (i == 1) ? 2 : 0;
        }
        GX_LoadBG1Scr(map, 0, sizeof(map));
        NitroPort_WideForcePictures(0);          /* the UI: nothing beside the picture */
        NitroPort_Render2DSplit(0, wide, &split);
        CHECK(wide[0 * 320 + 32] == BLUE);
        CHECK(wide[0 * 320 + 31] == GREY);
        CHECK(wide[5 * 320 + 31] == RED);        /* (a plain edge still goes on) */
        NitroPort_WideForcePictures(1);
        NitroPort_Render2DSplit(0, wide, &split);
        CHECK(wide[0 * 320 + 32] == BLUE);
        CHECK(wide[0 * 320 + 33] == RED);
        CHECK(wide[0 * 320 + 40] == BLUE);
        CHECK(wide[0 * 320 + 31] == BLUE);        /* mirrored: x -1 is the DS's 0 */
        CHECK(wide[0 * 320 + 30] == RED);
        CHECK(wide[0 * 320 + 24] == RED);         /* x -8 is the DS's 7 */
        CHECK(wide[0 * 320 + 23] == BLUE);        /* x -9 is the DS's 8 */
        CHECK(wide[0 * 320 + 16] == BLUE);
        CHECK(wide[0 * 320 + 15] == GREY);        /* x -17 is the DS's 16: empty */
        CHECK(wide[0 * 320 + 0] == GREY);
        CHECK(wide[5 * 320 + 31] == RED);         /* line 5: a plain edge goes on */
        CHECK(wide[5 * 320 + 0] == RED);
        /* A picture that stops short of the edge (the title's logo) isn't
         * mirrored: tile 0 empty, tile 1 red. */
        for (i = 0; i < 32 * 32; i++) {
            map[i] = (i == 1) ? 1 : 0;
        }
        GX_LoadBG1Scr(map, 0, sizeof(map));
        NitroPort_Render2DSplit(0, wide, &split);
        CHECK(wide[0 * 320 + 40] == BLUE);
        CHECK(wide[0 * 320 + 41] == RED);
        CHECK(wide[0 * 320 + 31] == GREY);
        CHECK(wide[0 * 320 + 23] == GREY);
        NitroPort_WideForcePictures(-1);
        split.anchorWindows = 0;
    }
}

/* Hand-drawn pictures (nitro_port_wideart.h): a BG's key, a picture made
 * for it, loaded and drawn in the bands (or all of it), moved with the
 * scroll; another BG's art isn't this one's. */
static void WriteArt(const char *dir, uint64_t key, int hofs, int vofs, int full, const u16 *pixels)
{
    static uint8_t rgba[320 * 192 * 4];
    char path[512];
    int i;

    for (i = 0; i < 320 * 192; i++) {
        u16 c = pixels[i];

        rgba[i * 4 + 0] = (uint8_t)((c & 31) << 3);
        rgba[i * 4 + 1] = (uint8_t)(((c >> 5) & 31) << 3);
        rgba[i * 4 + 2] = (uint8_t)(((c >> 10) & 31) << 3);
        rgba[i * 4 + 3] = (c & 0x8000) ? 255 : 0;
    }
    snprintf(path, sizeof(path), "%s/%016llx_%d_%d%s.png", dir, (unsigned long long)key, hofs, vofs,
             full ? "_full" : "");
    CHECK(NitroPort_WideArtWritePng(path, rgba, 320, 192) == 0);
}

static void test_wide_art(void)
{
    static u16 wide[320 * 192], over[320 * 192], art[320 * 192], layerPx[320 * 192];
    static u8 overAlpha[320 * 192];
    static u8 tiles[96];
    static u16 map[32 * 32];
    static u16 pltt[16] = { GREY, RED, BLUE };
    char dir[] = "/tmp/wideartXXXXXX";
    NitroRender2DSplit split;
    uint64_t key, key2;
    int hofs, vofs, prio, full, i, x;

    /* file names */
    CHECK(NitroPort_WideArtParseName("0123456789abcdef_12_3.png", &key, &hofs, &vofs, &full) &&
          key == 0x0123456789abcdefull && hofs == 12 && vofs == 3 && !full);
    CHECK(NitroPort_WideArtParseName("0123456789ABCDEF_0_0_full.png", &key, &hofs, &vofs, &full) && full);
    CHECK(!NitroPort_WideArtParseName("0123456789abcdef_0_0.bmp", &key, &hofs, &vofs, &full));
    CHECK(!NitroPort_WideArtParseName("0123456789abcde_0_0.png", &key, &hofs, &vofs, &full));
    CHECK(!NitroPort_WideArtParseName("screen.png", &key, &hofs, &vofs, &full));

    /* BG1: tile 1 (red) over the map's left half, empty on the right */
    Reset();
    MakeTile4(tiles + 32, 1);
    for (i = 0; i < 32 * 32; i++) {
        map[i] = (i % 32) < 16 ? 1 : 0;
    }
    G2_SetBG1Control(GX_BG_SCRSIZE_TEXT_256x256, GX_BG_COLORMODE_16, GX_BG_SCRBASE_0x0000,
                     GX_BG_CHARBASE_0x04000, GX_BG_EXTPLTT_01);
    GX_LoadBG1Char(tiles, 0, sizeof(tiles));
    GX_LoadBG1Scr(map, 0, sizeof(map));
    GX_LoadBGPltt(pltt, 0, sizeof(pltt));
    GX_SetVisiblePlane(GX_PLANEMASK_BG1);
    memset(&split, 0, sizeof(split));
    split.width = 320;
    split.origin = 32;
    memset(split.clip, 1, sizeof(split.clip));
    CHECK(NitroPort_Render2DLayer(0, 1, &split, layerPx, &key, &hofs, &vofs, &prio));
    CHECK(key != 0 && hofs == 0 && vofs == 0);
    CHECK(layerPx[5 * 320 + 40] == (RED | 0x8000));
    CHECK(!NitroPort_Render2DLayer(0, 2, &split, layerPx, &key2, &hofs, &vofs, &prio));   /* BG2 off */
    /* the same art elsewhere in VRAM: the same key */
    G2_SetBG1Control(GX_BG_SCRSIZE_TEXT_256x256, GX_BG_COLORMODE_16, GX_BG_SCRBASE_0x0800,
                     GX_BG_CHARBASE_0x08000, GX_BG_EXTPLTT_01);
    GX_LoadBG1Char(tiles, 0, sizeof(tiles));
    GX_LoadBG1Scr(map, 0, sizeof(map));
    CHECK(NitroPort_Render2DLayer(0, 1, &split, layerPx, &key2, &hofs, &vofs, &prio) && key2 == key);
    /* different art: a different key */
    map[0] = 2;
    GX_LoadBG1Scr(map, 0, sizeof(map));
    CHECK(NitroPort_Render2DLayer(0, 1, &split, layerPx, &key2, &hofs, &vofs, &prio) && key2 != key);
    map[0] = 1;
    GX_LoadBG1Scr(map, 0, sizeof(map));

    /* a picture for it: blue bands, a green middle (shows only as _full) */
    CHECK(mkdtemp(dir) != NULL);
    for (i = 0; i < 320 * 192; i++) {
        x = i % 320;
        art[i] = (x < 32 || x >= 288) ? (BLUE | 0x8000) : (RGB(0, 31, 0) | 0x8000);
    }
    art[10 * 320 + 3] = 0;   /* a transparent pixel */
    WriteArt(dir, key, 0, 0, 0, art);
    CHECK(NitroPort_WideArtLoad(dir) == 1);
    split.over = over;
    split.overAlpha = overAlpha;
    NitroPort_Render2DSplit(0, wide, &split);
    CHECK(wide[5 * 320 + 0] == BLUE);            /* the bands: the picture */
    CHECK(wide[5 * 320 + 319] == BLUE);
    CHECK(wide[10 * 320 + 3] == GREY);           /* (transparent: the backdrop) */
    CHECK(wide[5 * 320 + 40] == RED);            /* the middle: the game's own */
    CHECK(wide[5 * 320 + 200] == GREY);
    /* scrolled 8 pixels right since: the picture moves 8 left with it */
    art[5 * 320 + 12] = RED | 0x8000;
    WriteArt(dir, key, 0, 0, 0, art);
    NitroPort_WideArtLoad(dir);
    G2_SetBG1Offset(8, 0);
    NitroPort_Render2DSplit(0, wide, &split);
    CHECK(wide[5 * 320 + 4] == RED);
    CHECK(wide[5 * 320 + 12] == BLUE);
    CHECK(wide[5 * 320 + 315] == GREY);          /* past the picture's right: nothing */
    G2_SetBG1Offset(0, 0);
    /* the whole picture: the middle too */
    WriteArt(dir, key, 0, 0, 1, art);
    NitroPort_WideArtLoad(dir);
    NitroPort_Render2DSplit(0, wide, &split);
    CHECK(wide[5 * 320 + 40] == RGB(0, 31, 0));
    CHECK(wide[5 * 320 + 0] == BLUE);
    /* the DS's own screen (no wider canvas): no art */
    NitroPort_Render2D(0, sTop);
    CHECK(sTop[5 * 256 + 8] == RED);
    /* a capture of the screen: the folder and its files */
    {
        char made[512], path[600];
        FILE *f;
        char line[256];
        int found = 0;

        NitroPort_SetWideScreen(32);
        CHECK(NitroPort_WideArtCapture(dir, made, sizeof(made)) == 0);
        snprintf(path, sizeof(path), "%s/info.txt", made);
        f = fopen(path, "r");
        CHECK(f != NULL);
        while (f != NULL && fgets(line, sizeof(line), f) != NULL) {
            unsigned long long k;
            int b;

            if (sscanf(line, "bg %d prio %*d key %llx", &b, &k) == 2 && b == 1 && k == key) {
                found = 1;
            }
        }
        if (f != NULL) {
            fclose(f);
        }
        CHECK(found);
        snprintf(path, sizeof(path), "%s/bg1.png", made);
        CHECK(access(path, F_OK) == 0);
        snprintf(path, sizeof(path), "%s/screen.png", made);
        CHECK(access(path, F_OK) == 0);
        NitroPort_SetWideScreen(0);
    }
    NitroPort_WideArtLoad("/nonexistent");
    CHECK(NitroPort_WideArtCount() == 0);
    {
        char cmd[600];

        snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
        CHECK(system(cmd) == 0);
    }
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
    test_unchanged_screen();
    test_screen_follows_3d();
    test_text_bg();
    test_8bpp_ext_palette();
    test_objs();
    test_blend_and_window();
    test_3d_blends_with_layer_behind();
    test_bitmap_bg();
    test_affine_bg();
    test_obj_8bpp_ext();
    test_matches_reference();
    test_split_layers();
    test_wide_canvas();
    test_wide_art();
    test_engines_on_two_threads();

    printf("2D renderer: %d checks, %d failed\n", sChecks, sFailures);
    return sFailures == 0 ? 0 : 1;
}
