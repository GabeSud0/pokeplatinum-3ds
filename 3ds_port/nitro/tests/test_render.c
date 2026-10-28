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

/* DISP3DCNT's acknowledge bits (the game writes them every frame) don't
 * count as a change; its other bits do. */
static void test_3d_acknowledge_unchanged(void)
{
    static u16 backdrop = GREY;

    Reset();
    GX_LoadBGPltt(&backdrop, 0, 2);
    CHECK(NitroPort_Render2DIfChanged(0, sTop) == 1);
    CHECK(NitroPort_Render2DIfChanged(0, sTop) == 0);
    reg_G3X_DISP3DCNT = (u16)(reg_G3X_DISP3DCNT | REG_G3X_DISP3DCNT_GO_MASK | REG_G3X_DISP3DCNT_RO_MASK);
    CHECK(NitroPort_Render2DIfChanged(0, sTop) == 0);
    reg_G3X_DISP3DCNT = (u16)(reg_G3X_DISP3DCNT & ~(REG_G3X_DISP3DCNT_GO_MASK | REG_G3X_DISP3DCNT_RO_MASK));
    CHECK(NitroPort_Render2DIfChanged(0, sTop) == 0);
    reg_G3X_DISP3DCNT = (u16)(reg_G3X_DISP3DCNT ^ 0x0080);   /* fog on or off */
    CHECK(NitroPort_Render2DIfChanged(0, sTop) == 1);
    reg_G3X_DISP3DCNT = (u16)(reg_G3X_DISP3DCNT ^ 0x0080);
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

/* An affine OBJ drawn smoothly (NitroPort_Render2DSmoothObjs: the bag in
 * the lower menu's panel, 1.25 times as big): 8x8, its left half blue and
 * its right half red, shown twice as big. As the DS draws it, each pixel
 * one texel's colour; smoothly, the pixels between two texels blended,
 * the edge's pixels (mostly in the OBJ) its colour. Only that engine. */
static void test_smooth_objs(void)
{
    static u8 tile[32];
    static u16 objPltt[16] = { 0, BLUE, RED };
    static GXOamAttr oam[128];
    MtxFx22 twice = { FX32_ONE / 2, 0, 0, FX32_ONE / 2 };
    u16 c;
    int i;

    Reset();
    GX_SetOBJVRamModeChar(GX_OBJVRAMMODE_CHAR_1D_32K);
    for (i = 0; i < 8; i++) {
        tile[i * 4 + 0] = 0x11;   /* x 0-3: index 1 (blue) */
        tile[i * 4 + 1] = 0x11;
        tile[i * 4 + 2] = 0x22;   /* x 4-7: index 2 (red) */
        tile[i * 4 + 3] = 0x22;
    }
    GX_LoadOBJ(tile, 0, sizeof(tile));
    GX_LoadOBJPltt(objPltt, 0, sizeof(objPltt));
    MI_CpuFillFast(oam, 0x0200, sizeof(oam));   /* all hidden */
    /* double size: its 16x16 box at (40, 40), the OBJ filling it */
    G2_SetOBJAttr(&oam[0], 40, 40, 0, GX_OAM_MODE_NORMAL, FALSE, GX_OAM_EFFECT_AFFINE_DOUBLE,
                  GX_OAM_SHAPE_8x8, GX_OAM_COLORMODE_16, 0, 0, 0);
    G2_SetOBJEffect(&oam[0], GX_OAM_EFFECT_AFFINE_DOUBLE, 0);
    G2_SetOBJAffine((GXOamAffine *)&oam[0], &twice);
    DC_FlushRange(oam, sizeof(oam));
    GX_LoadOAM(oam, 0, sizeof(oam));
    GX_SetVisiblePlane(GX_PLANEMASK_OBJ);

    /* as the DS: texel 3 blue (x 46-47), texel 4 red (48-49) */
    NitroPort_Render2D(0, sTop);
    CHECK_PX(sTop, 47, 47, BLUE);
    CHECK_PX(sTop, 48, 47, RED);
    CHECK_PX(sTop, 40, 47, BLUE);
    CHECK_PX(sTop, 55, 47, RED);

    /* smoothly: x 47 three quarters blue, x 48 three quarters red; the
     * edges (three quarters in the OBJ) their colour */
    NitroPort_Render2DSmoothObjs(0, 1);
    NitroPort_Render2D(0, sTop);
    c = sTop[47 * 256 + 47];
    CHECK((c & 31) >= 7 && (c & 31) <= 9 && ((c >> 10) & 31) >= 22 && ((c >> 10) & 31) <= 24);
    c = sTop[47 * 256 + 48];
    CHECK((c & 31) >= 22 && (c & 31) <= 24 && ((c >> 10) & 31) >= 7 && ((c >> 10) & 31) <= 9);
    CHECK_PX(sTop, 40, 47, BLUE);
    CHECK_PX(sTop, 55, 47, RED);
    CHECK_PX(sTop, 44, 40, BLUE);
    CHECK_PX(sTop, 44, 55, BLUE);
    /* the other engine as the DS draws it */
    NitroPort_Render2DSmoothObjs(1, 0);
    NitroPort_Render2D(0, sTop);
    c = sTop[47 * 256 + 47];
    CHECK(c != BLUE && c != RED);
    NitroPort_Render2DSmoothObjs(0, 0);
    NitroPort_Render2D(0, sTop);
    CHECK_PX(sTop, 47, 47, BLUE);
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

    /* (all of it: the DS's width, the current registers, not a snapshot) */
    memset(&split, 0, sizeof(split));
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
         * the game's windows is a picture: where its edge isn't plain, it
         * used to be mirrored at the DS's edge; now nothing is. Tiles 0 and 1 of row
         * 0: red, blue (each tile's top left pixel blue: MakeTile4); the
         * rest empty. Line 0's edge isn't plain (that pixel), line 5's is. */
        for (i = 0; i < 32 * 32; i++) {
            map[i] = (i == 0) ? 1 : (i == 1) ? 2 : 0;
        }
        GX_LoadBG1Scr(map, 0, sizeof(map));
        NitroPort_WideForcePictures(0);          /* the UI */
        NitroPort_Render2DSplit(0, wide, &split);
        CHECK(wide[0 * 320 + 32] == BLUE);
        /* Line 0 carries on the plain lines next to it (lines 1-7: red).
         * Changed October 2026, as asked (a menu's panels reaching the
         * edge left black blocks beside them); was GREY: nothing beside a
         * line whose edge isn't plain. */
        CHECK(wide[0 * 320 + 31] == RED);
        CHECK(wide[5 * 320 + 31] == RED);        /* (a plain edge still goes on) */
        CHECK(wide[20 * 320 + 31] == GREY);      /* (nothing at the edge: nothing beside it) */
        NitroPort_WideForcePictures(1);
        NitroPort_Render2DSplit(0, wide, &split);
        CHECK(wide[0 * 320 + 32] == BLUE);
        CHECK(wide[0 * 320 + 33] == RED);
        CHECK(wide[0 * 320 + 40] == BLUE);
        /* Not mirrored any more (October 2026: nothing mirrored or
         * stretched; was the DS's 0, 7, 8 at x -1, -8, -9): nothing */
        CHECK(wide[0 * 320 + 31] == GREY);
        CHECK(wide[0 * 320 + 24] == GREY);
        CHECK(wide[0 * 320 + 16] == GREY);
        CHECK(wide[0 * 320 + 0] == GREY);
        /* Line 5 is plain for 8 pixels only (tile 0; tile 1's pixel 0 is
         * blue): on a picture screen that's part of the picture, not a plain
         * edge, so it doesn't go on either. (Was: it went on, as on the UI's
         * screens; that drew the opening strip's ovals out into lines,
         * October 2026.) */
        CHECK(wide[5 * 320 + 31] == GREY);
        CHECK(wide[5 * 320 + 0] == GREY);
        /* plain for a band's width (tiles 0-3 red): it goes on */
        for (i = 0; i < 32 * 32; i++) {
            map[i] = (i < 4) ? 1 : (i == 4) ? 2 : 0;
        }
        GX_LoadBG1Scr(map, 0, sizeof(map));
        NitroPort_Render2DSplit(0, wide, &split);
        CHECK(wide[5 * 320 + 31] == RED);
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
/* kind: NITRO_WIDE_ART_FULL, NITRO_WIDE_ART_LOOP */
static void WriteArtKind(const char *dir, uint64_t key, int hofs, int vofs, int kind, const u16 *pixels)
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
    snprintf(path, sizeof(path), "%s/%016llx_%d_%d%s%s.png", dir, (unsigned long long)key, hofs, vofs,
             (kind & NITRO_WIDE_ART_FULL) ? "_full" : "", (kind & NITRO_WIDE_ART_LOOP) ? "_loop" : "");
    CHECK(NitroPort_WideArtWritePng(path, rgba, 320, 192) == 0);
}

/* A PNG's oFFs chunk (x, y pixels: where its top-left goes) put in after
 * its header, as the editor writes it. */
static void AddPngOffset(const char *path, int x, int y)
{
    static uint8_t bytes[64 * 1024];
    uint8_t chunk[21];
    uint32_t crc = 0xFFFFFFFFu;
    size_t n;
    int i, k;
    FILE *f = fopen(path, "rb");

    CHECK(f != NULL);
    if (f == NULL) {
        return;
    }
    n = fread(bytes, 1, sizeof(bytes), f);
    fclose(f);
    CHECK(n > 33 && n < sizeof(bytes));
    chunk[0] = chunk[1] = chunk[2] = 0;
    chunk[3] = 9;
    memcpy(chunk + 4, "oFFs", 4);
    for (i = 0; i < 4; i++) {
        chunk[8 + i] = (uint8_t)((uint32_t)x >> (24 - 8 * i));
        chunk[12 + i] = (uint8_t)((uint32_t)y >> (24 - 8 * i));
    }
    chunk[16] = 0;
    for (i = 4; i < 17; i++) {
        crc ^= chunk[i];
        for (k = 0; k < 8; k++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
        }
    }
    crc ^= 0xFFFFFFFFu;
    for (i = 0; i < 4; i++) {
        chunk[17 + i] = (uint8_t)(crc >> (24 - 8 * i));
    }
    f = fopen(path, "wb");
    CHECK(f != NULL);
    if (f == NULL) {
        return;
    }
    fwrite(bytes, 1, 33, f);   /* (the signature and IHDR) */
    fwrite(chunk, 1, sizeof(chunk), f);
    fwrite(bytes + 33, 1, n - 33, f);
    fclose(f);
}

static void WriteArt(const char *dir, uint64_t key, int hofs, int vofs, int full, const u16 *pixels)
{
    WriteArtKind(dir, key, hofs, vofs, full ? NITRO_WIDE_ART_FULL : 0, pixels);
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
    CHECK(NitroPort_WideArtParseName("0123456789abcdef_0_0_full_loop.png", &key, &hofs, &vofs, &full) &&
          full == (NITRO_WIDE_ART_FULL | NITRO_WIDE_ART_LOOP));
    CHECK(NitroPort_WideArtParseName("0123456789abcdef_0_0_loop.png", &key, &hofs, &vofs, &full) &&
          full == NITRO_WIDE_ART_LOOP);
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
    /* Past the picture's right: the map goes round (256 pixels: a text
     * BG wraps), and so does the picture: canvas x 315 is the picture's
     * x 323, the map's place of its x 67, green. (Was: nothing, the
     * backdrop; that cut off the intro's scrolling strips.) */
    CHECK(wide[5 * 320 + 315] == RGB(0, 31, 0));
    /* ... and scrolled back 8: the left band's x 0 is the picture's -8,
     * the same place of the map as its 248, green */
    G2_SetBG1Offset(512 - 8, 0);
    NitroPort_Render2DSplit(0, wide, &split);
    CHECK(wide[5 * 320 + 0] == RGB(0, 31, 0));
    CHECK(wide[5 * 320 + 20] == RED);            /* (the picture's 12, 8 on) */
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

/* Built-in pictures (NitroPort_WideArtBuiltIn): none, but in the test
 * below. */
static const NitroWideArtFile *sTestBuiltIn;
static int sTestBuiltInCount;

const NitroWideArtFile *NitroPort_WideArtBuiltIn(int *count)
{
    *count = sTestBuiltInCount;
    return sTestBuiltIn;
}

/* Wide art without the game's art: a picture shared as its screen shows
 * (NitroPort_Render2DShareArt) has the BG's own pixels where it had them
 * (NITRO_WIDE_ART_GAME), its own elsewhere; drawn, the BG's own pixels go
 * back there (from the map where the picture was made, also when the BG
 * has scrolled since); built into the program it's the same, and a file of
 * that name in the folder takes its place. */
static void test_wide_art_shared(void)
{
    static u16 wide[320 * 192], over[320 * 192], art[320 * 192], layerPx[320 * 192];
    static u8 overAlpha[320 * 192];
    static u8 tiles[96];
    static u16 map[32 * 32];
    static u16 pltt[16] = { GREY, RED, BLUE };
    char dir[] = "/tmp/wideshareXXXXXX", share[600], name[64], path[700];
    NitroRender2DSplit split;
    const u16 *got;
    uint64_t key;
    int hofs, vofs, prio, full, i, x, y, game = 0, mine = 0, wrong = 0;

    /* BG1: red (tile 1) on the map's left half, nothing on its right */
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
    NitroPort_SetWideScreen(32);
    memset(&split, 0, sizeof(split));
    split.width = 320;
    split.origin = 32;
    memset(split.clip, 1, sizeof(split.clip));
    CHECK(NitroPort_Render2DLayer(0, 1, &split, layerPx, &key, &hofs, &vofs, &prio));
    /* a whole picture made at scroll 0: the BG's own in the middle (its
     * tiles' red, a blue pixel at each one's top left; one red pixel
     * painted green), blue bands; transparent where the BG isn't (the
     * map's right half). The game's own pixels in it: those the same as
     * the BG's as the port draws it there (the middle's), or as its map
     * goes on past the DS's edges (the right band's blue pixels over the
     * tiles' blue corners, the map's left half again; nothing in the left
     * band: the right half, empty). */
    for (y = 0; y < 192; y++) {
        for (x = 0; x < 320; x++) {
            u16 c = (x < 32 || x >= 288) ? (BLUE | 0x8000) : layerPx[y * 320 + x];

            art[y * 320 + x] = c;
            game += (c & 0x8000) && layerPx[y * 320 + x] == c;
        }
    }
    game--;
    art[20 * 320 + 50] = (RGB(0, 31, 0) | 0x8000);
    {
        int expected = game + 4 * 24, moved = 0;

        game = 0;
    CHECK(mkdtemp(dir) != NULL);
    WriteArtKind(dir, key, 0, 0, NITRO_WIDE_ART_FULL, art);
    snprintf(name, sizeof(name), "%016llx_0_0_full.png", (unsigned long long)key);
    CHECK(NitroPort_WideArtLoad(dir) == 1);
    CHECK(NitroPort_WideArtUnshared() == 1);
    /* shared as the screen shows */
    CHECK(NitroPort_Render2DShareArt(0) == 1);
    CHECK(NitroPort_WideArtUnshared() == 0);
    CHECK(NitroPort_Render2DShareArt(0) == 0);   /* (once) */
    snprintf(share, sizeof(share), "%s/share", dir);
    snprintf(path, sizeof(path), "%s/%s", share, name);
    CHECK(access(path, F_OK) == 0);
    /* what it holds: the map's red left out, the rest kept */
    CHECK(NitroPort_WideArtLoad(share) == 1);
    got = NitroPort_WideArtFind(key, &hofs, &vofs, &full);
    CHECK(got != NULL);
    for (i = 0; got != NULL && i < 320 * 192; i++) {
        if (NitroPort_WideArtIsGame(got[i])) {
            game++;
            moved += got[i] != NITRO_WIDE_ART_GAME;
            wrong += !(art[i] & 0x8000);
        } else {
            mine += (got[i] & 0x8000) != 0;
            wrong += got[i] != art[i];
        }
    }
    CHECK(wrong == 0);
    CHECK(game == expected && game > 100 * 192);
    CHECK(moved == 0);   /* (nothing of it the BG's moved: its own art isn't) */
    CHECK(game + mine == 128 * 192 + 64 * 192);   /* (every opaque pixel one or the other) */
    }
    /* drawn from the shared picture: the same as from the whole one */
    split.over = over;
    split.overAlpha = overAlpha;
    NitroPort_Render2DSplit(0, wide, &split);
    CHECK(wide[5 * 320 + 40] == RED);          /* the game's own, back */
    CHECK(wide[20 * 320 + 50] == RGB(0, 31, 0));   /* the green kept */
    CHECK(wide[5 * 320 + 3] == BLUE);
    CHECK(wide[5 * 320 + 200] == GREY);        /* (transparent: the backdrop) */
    /* scrolled 8 right since: the picture moves 8 left with it, the red
     * from the map where it was made */
    G2_SetBG1Offset(8, 0);
    NitroPort_Render2DSplit(0, wide, &split);
    CHECK(wide[5 * 320 + 32] == RED);          /* (the picture's x 40) */
    CHECK(wide[20 * 320 + 42] == RGB(0, 31, 0));
    CHECK(wide[5 * 320 + 151] == RED);         /* (its 159, the last red) */
    CHECK(wide[5 * 320 + 152] == GREY);
    G2_SetBG1Offset(0, 0);
    /* built in: the same */
    {
        static unsigned char bytes[400000];
        static NitroWideArtFile file;
        FILE *f = fopen(path, "rb");
        size_t n = f != NULL ? fread(bytes, 1, sizeof(bytes), f) : 0;

        if (f != NULL) {
            fclose(f);
        }
        CHECK(n > 0 && n < sizeof(bytes));
        file.name = name;
        file.data = bytes;
        file.size = (unsigned)n;
        sTestBuiltIn = &file;
        sTestBuiltInCount = 1;
        CHECK(NitroPort_WideArtLoad("/nonexistent") == 1);
        CHECK(NitroPort_WideArtUnshared() == 0);   /* (built in: nothing to share) */
        NitroPort_Render2DSplit(0, wide, &split);
        CHECK(wide[5 * 320 + 40] == RED);
        CHECK(wide[20 * 320 + 50] == RGB(0, 31, 0));
        CHECK(wide[5 * 320 + 3] == BLUE);
        /* the folder's of the same name in its place (and to share) */
        art[20 * 320 + 50] = BLUE | 0x8000;
        WriteArtKind(dir, key, 0, 0, NITRO_WIDE_ART_FULL, art);
        CHECK(NitroPort_WideArtLoad(dir) == 1);
        CHECK(NitroPort_WideArtUnshared() == 1);
        NitroPort_Render2DSplit(0, wide, &split);
        CHECK(wide[20 * 320 + 50] == BLUE);
        sTestBuiltIn = NULL;
        sTestBuiltInCount = 0;
    }
    /* A picture of the BG moved along (the editor's Move tool: the
     * opening's Pokemon moved so that they come in from the wide screen's
     * edge), made at scroll 16: its pixels the BG's own from 24 further
     * left (the green one its own). Shared, they're the game's own, moved
     * (the plain red matching where it is, the rest from 24 (or 8, 16: the
     * tiles repeat) further left); drawn, the same picture. */
    {
        static u16 art2[320 * 192];
        uint64_t key2;
        const u16 *got2;
        static u16 unclippedPx[320 * 192];
        int opaque = 0, moved = 0, expected = 0;

        NitroPort_WideArtLoad("/nonexistent");   /* (the BG's own, without the first picture) */
        G2_SetBG1Offset(16, 0);
        CHECK(NitroPort_Render2DLayer(0, 1, &split, layerPx, &key2, &hofs, &vofs, &prio));
        CHECK(key2 == key);
        for (y = 0; y < 192; y++) {
            for (x = 0; x < 320; x++) {
                u16 c = (x < 32 || x >= 288) ? (BLUE | 0x8000) : x >= 32 + 24 ? layerPx[y * 320 + x - 24] : 0;

                art2[y * 320 + x] = c;
                opaque += (c & 0x8000) != 0;
            }
        }
        art2[20 * 320 + 50] = (RGB(0, 31, 0) | 0x8000);
        opaque++;
        /* (its own: the green, and the bands' blue but where the map, going
         * on past the DS's edges, has its tiles' blue corners) */
        {
            NitroRender2DSplit unclipped = split;

            unclipped.clip[1] = 0;
            CHECK(NitroPort_Render2DLayer(0, 1, &unclipped, unclippedPx, &key2, &hofs, &vofs, &prio));
            for (i = 0; i < 320 * 192; i++) {
                if ((i % 320 < 32 || i % 320 >= 288) && unclippedPx[i] != art2[i]) {
                    expected++;
                }
            }
            expected++;
        }
        WriteArtKind(dir, key, 16, 0, NITRO_WIDE_ART_FULL, art2);
        CHECK(NitroPort_WideArtLoad(dir) == 2);
        CHECK(NitroPort_WideArtUnshared() == 2);   /* (the first, changed above, again) */
        CHECK(NitroPort_Render2DShareArt(0) == 2);
        CHECK(NitroPort_WideArtLoad(share) == 2);
        got2 = NitroPort_WideArtFindNear(key, 16, 0, 256, 256, 256, 192, &hofs, &vofs, &full);
        CHECK(got2 != NULL && hofs == 16);
        game = mine = moved = wrong = 0;
        for (i = 0; got2 != NULL && i < 320 * 192; i++) {
            if (NitroPort_WideArtIsGame(got2[i])) {
                game++;
                moved += got2[i] != NITRO_WIDE_ART_GAME;
                wrong += !(art2[i] & 0x8000) || NitroPort_WideArtGameDx(got2[i]) < 0 ||
                         NitroPort_WideArtGameDx(got2[i]) > 24 || NitroPort_WideArtGameDy(got2[i]) != 0;
            } else {
                mine += (got2[i] & 0x8000) != 0;
                wrong += got2[i] != art2[i];
            }
        }
        CHECK(wrong == 0);
        CHECK(moved == 24 * 192);   /* (the stretch past the map's red: moved) */
        CHECK(mine == expected && game + mine == opaque);
        NitroPort_Render2DSplit(0, wide, &split);
        for (y = 0; y < 192; y++) {
            for (x = 32; x < 288; x++) {
                u16 c = art2[y * 320 + x];

                wrong += (c & 0x8000) ? wide[y * 320 + x] != (c & 0x7FFF) : wide[y * 320 + x] != GREY;
            }
        }
        CHECK(wrong == 0);
        CHECK(wide[5 * 320 + 160] == RED);   /* (the map's red ends at 143 here: moved) */
        G2_SetBG1Offset(0, 0);
    }
    NitroPort_SetWideScreen(0);
    NitroPort_WideArtLoad("/nonexistent");
    CHECK(NitroPort_WideArtCount() == 0);
    {
        char cmd[600];

        snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
        CHECK(system(cmd) == 0);
    }
}

/* A shared picture of a BG the port splits (a plain middle, decorated
 * sides: the halves to the screen's edges, the middle widened): its
 * pixels the game's own are told from the BG's own layout, as the BG
 * is drawn with a picture (the plain one, its map going on past the DS's
 * edges), not the split (the split moves the sides: the game's own pixel
 * drawn back there would be another's). */
static void test_wide_art_split_bg(void)
{
    static u16 wide[320 * 192], over[320 * 192], art[320 * 192];
    static u8 overAlpha[320 * 192];
    static u8 tiles[3 * 32];
    static u16 map[32 * 32];
    static u16 pltt[16] = { GREY, RED, BLUE, GREEN };
    char dir[] = "/tmp/widesplitXXXXXX", share[600];
    NitroRender2DSplit split;
    uint64_t key;
    const u16 *got;
    int hofs, vofs, full, prio, i, x, y, game = 0, mine = 0, wrong = 0;

    Reset();
    /* tile 1: red, blue at its top left and at the left of its row 4 (so
     * that the sides aren't plain on the lines DecideSplits looks at);
     * tile 2: green, plain; the map's columns 12-19 (the DS's x 96-159)
     * green, the rest tile 1 */
    MakeTile4(tiles + 32, 1);
    tiles[32 + 16] = (u8)((1 << 4) | 2);
    MakeTile4(tiles + 64, 3);
    tiles[64] = (u8)((3 << 4) | 3);
    for (i = 0; i < 32 * 32; i++) {
        int col = i % 32;

        map[i] = (u16)(col >= 12 && col < 20 ? 2 : 1);
    }
    G2_SetBG1Control(GX_BG_SCRSIZE_TEXT_256x256, GX_BG_COLORMODE_16, GX_BG_SCRBASE_0x0000,
                     GX_BG_CHARBASE_0x04000, GX_BG_EXTPLTT_01);
    GX_LoadBG1Char(tiles, 0, sizeof(tiles));
    GX_LoadBG1Scr(map, 0, sizeof(map));
    GX_LoadBGPltt(pltt, 0, sizeof(pltt));
    GX_SetVisiblePlane(GX_PLANEMASK_BG1);
    NitroPort_SetWideScreen(32);
    memset(&split, 0, sizeof(split));
    split.width = 320;
    split.origin = 32;
    memset(split.clip, 1, sizeof(split.clip));
    split.over = over;
    split.overAlpha = overAlpha;
    /* without a picture: split (the left half at the left edge: its green
     * from x 96; the right half's green ends at x 223) */
    NitroPort_Render2DSplit(0, wide, &split);
    CHECK(wide[20 * 320 + 100] == GREEN);
    CHECK(wide[20 * 320 + 201] == GREEN);
    /* a whole picture of the plain layout (the map at the DS's x 0-255,
     * nothing in the bands), one pixel its own */
    for (y = 0; y < 192; y++) {
        for (x = 32; x < 288; x++) {
            int mx = x - 32, col = mx / 8;
            u16 c = col >= 12 && col < 20 ? GREEN
                    : (mx % 8 == 0 && (y % 8 == 0 || y % 8 == 4)) ? BLUE
                                                                   : RED;

            art[y * 320 + x] = (u16)(c | 0x8000);
        }
    }
    art[20 * 320 + 150] = (u16)(GREY | 0x8000);
    CHECK(NitroPort_Render2DLayer(0, 1, &split, over, &key, &hofs, &vofs, &prio));
    CHECK(mkdtemp(dir) != NULL);
    WriteArtKind(dir, key, 0, 0, NITRO_WIDE_ART_FULL, art);
    CHECK(NitroPort_WideArtLoad(dir) == 1);
    CHECK(NitroPort_Render2DShareArt(0) == 1);
    snprintf(share, sizeof(share), "%s/share", dir);
    CHECK(NitroPort_WideArtLoad(share) == 1);
    got = NitroPort_WideArtFind(key, &hofs, &vofs, &full);
    CHECK(got != NULL);
    for (i = 0; got != NULL && i < 320 * 192; i++) {
        if (NitroPort_WideArtIsGame(got[i])) {
            game++;
            wrong += got[i] != NITRO_WIDE_ART_GAME;
        } else if (got[i] & 0x8000) {
            mine++;
        }
    }
    CHECK(wrong == 0 && game == 256 * 192 - 1 && mine == 1);   /* (told from the plain layout: all of it) */
    /* drawn: the picture as made (the plain layout), the grey pixel */
    NitroPort_Render2DSplit(0, wide, &split);
    wrong = 0;
    for (y = 0; y < 192; y++) {
        for (x = 32; x < 288; x++) {
            wrong += wide[y * 320 + x] != (art[y * 320 + x] & 0x7FFF);
        }
    }
    CHECK(wrong == 0);
    CHECK(wide[20 * 320 + 100] == RED);   /* (not split: the map's x 68) */
    CHECK(wide[20 * 320 + 201] == RED);
    CHECK(wide[20 * 320 + 150] == GREY);
    NitroPort_SetWideScreen(0);
    NitroPort_WideArtLoad("/nonexistent");
    {
        char cmd[600];

        snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
        CHECK(system(cmd) == 0);
    }
}

/* An H-blank effect (NitroPort_LineRegs): each line drawn with its own
 * registers, the BGs' scroll and the windows; the 3D's scroll on each line
 * for the compositor (scroll3dLine), only when the lines differ; the
 * snapshot taken with them. */
static void test_line_regs(void)
{
    static u8 tiles[64];
    static u16 map[32 * 32];
    static u16 pltt[16] = { GREY, RED, BLUE };
    static u16 under[256 * 192], over[256 * 192];
    static u8 overAlpha[256 * 192];
    NitroRender2DSplit split;
    int i, y;

    Reset();
    MakeTile4(tiles + 32, 1);
    for (i = 0; i < 32 * 32; i++) {
        map[i] = (i % 32) < 16 ? 1 : 0;   /* red on the map's x 0-127 */
    }
    G2_SetBG1Control(GX_BG_SCRSIZE_TEXT_256x256, GX_BG_COLORMODE_16, GX_BG_SCRBASE_0x0000,
                     GX_BG_CHARBASE_0x04000, GX_BG_EXTPLTT_01);
    GX_LoadBG1Char(tiles, 0, sizeof(tiles));
    GX_LoadBG1Scr(map, 0, sizeof(map));
    GX_LoadBGPltt(pltt, 0, sizeof(pltt));
    GX_SetVisiblePlane(GX_PLANEMASK_BG1);
    /* odd lines scrolled 8 left; line 100 with window 0 (x 0-49) showing
     * BG1, nothing outside it */
    for (y = 0; y < 192; y++) {
        memcpy(NitroPort_LineRegs.regs[0][y], NitroPort_IoReg, NITRO_LINE_REGS);
        memcpy(NitroPort_LineRegs.regs[1][y], NitroPort_IoReg + 0x1000, NITRO_LINE_REGS);
        if (y & 1) {
            NitroPort_LineRegs.regs[0][y][0x14] = 8;
        }
    }
    {
        u8 *l = NitroPort_LineRegs.regs[0][100];
        u32 dispcnt = (u32)l[0] | ((u32)l[1] << 8) | ((u32)l[2] << 16) | ((u32)l[3] << 24);

        dispcnt |= 0x2000;   /* window 0 */
        l[0] = (u8)dispcnt;
        l[1] = (u8)(dispcnt >> 8);
        l[0x40] = 50;        /* WIN0H: 0-50 */
        l[0x41] = 0;
        l[0x44] = 192;       /* WIN0V: 0-192 */
        l[0x45] = 0;
        l[0x48] = 0x02;      /* in: BG1 */
        l[0x4A] = 0x00;      /* out: nothing */
    }
    NitroPort_LineRegs.active = 1;
    NitroPort_Render2D(0, sTop);
    CHECK(sTop[0 * 256 + 124] == RED);    /* (the map's x 124) */
    CHECK(sTop[1 * 256 + 124] == GREY);   /* (its x 132: nothing, the backdrop) */
    CHECK(sTop[1 * 256 + 116] == RED);
    CHECK(sTop[2 * 256 + 124] == RED);
    CHECK(sTop[100 * 256 + 40] == RED);   /* (in window 0) */
    CHECK(sTop[100 * 256 + 60] == GREY);  /* (out of it) */
    CHECK(sTop[102 * 256 + 60] == RED);   /* (no window on the lines either side) */
    /* the snapshot has them too; and without them, the frame's own */
    NitroPort_Render2DTakeState(0);
    NitroPort_LineRegs.active = 0;
    memset(&split, 0, sizeof(split));
    split.over = over;
    split.overAlpha = overAlpha;
    split.snapshot = 1;
    NitroPort_Render2DSplit(0, under, &split);
    CHECK(under[1 * 256 + 124] == GREY);
    CHECK(under[100 * 256 + 60] == GREY);
    CHECK(!split.scroll3dLines);   /* (no 3D) */
    split.snapshot = 0;
    NitroPort_Render2DSplit(0, under, &split);
    CHECK(under[1 * 256 + 124] == RED);
    CHECK(under[100 * 256 + 60] == RED);
    /* the 3D's scroll on each line (BG0, the 3D): lines 40-59 slid 16 left */
    GX_SetGraphicsMode(GX_DISPMODE_GRAPHICS, GX_BGMODE_0, GX_BG0_AS_3D);
    GX_SetVisiblePlane(GX_PLANEMASK_BG0 | GX_PLANEMASK_BG1);
    for (y = 0; y < 192; y++) {
        memcpy(NitroPort_LineRegs.regs[0][y], NitroPort_IoReg, NITRO_LINE_REGS);
        memcpy(NitroPort_LineRegs.regs[1][y], NitroPort_IoReg + 0x1000, NITRO_LINE_REGS);
        if (y >= 40 && y < 60) {
            NitroPort_LineRegs.regs[0][y][0x10] = 16;
        }
    }
    NitroPort_LineRegs.active = 1;
    memset(&split, 0, sizeof(split));
    split.over = over;
    split.overAlpha = overAlpha;
    NitroPort_Render2DSplit(0, under, &split);
    CHECK(split.has3d);
    CHECK(split.scroll3dLines);
    CHECK(split.scroll3d == 0 && split.scroll3dLine[39] == 0 && split.scroll3dLine[40] == 16 &&
          split.scroll3dLine[59] == 16 && split.scroll3dLine[60] == 0);
    NitroPort_LineRegs.active = 0;
    NitroPort_Render2DSplit(0, under, &split);
    CHECK(!split.scroll3dLines);
    GX_SetVisiblePlane(0);
}

/* Wide art for a group of OBJs (a sprite background): its key doesn't
 * change with where the OBJs are or where their tiles are, the picture
 * goes into the OBJ layer where no OBJ is, at the group's priority, and
 * moves with the group. */
static void test_wide_art_objs(void)
{
    static u16 wide[320 * 192], over[320 * 192], art[320 * 192], layerPx[320 * 192];
    static u8 overAlpha[320 * 192];
    static u8 tiles[4 * 32];
    static u16 objPltt[16 * 16];
    static GXOamAttr oam[128];
    char dir[] = "/tmp/wideartobjXXXXXX";
    NitroRender2DSplit split;
    NitroObjGroup groups[8], groups2[8];
    int n, i, x, g3 = -1;

    Reset();
    GX_SetOBJVRamModeChar(GX_OBJVRAMMODE_CHAR_1D_32K);
    MakeTile4(tiles, 1);
    MakeTile4(tiles + 32, 2);
    GX_LoadOBJ(tiles, 0, sizeof(tiles));
    objPltt[1] = BLUE;
    objPltt[2] = GREEN;
    objPltt[5 * 16 + 1] = RED;
    GX_LoadOBJPltt(objPltt, 0, sizeof(objPltt));
    MI_CpuFillFast(oam, 0x0200, sizeof(oam));
    /* the group behind: two 8x8 OBJs of palette 0, priority 3 */
    G2_SetOBJAttr(&oam[0], 60, 40, 3, GX_OAM_MODE_NORMAL, FALSE, GX_OAM_EFFECT_NONE,
                  GX_OAM_SHAPE_8x8, GX_OAM_COLORMODE_16, 0, 0, 0);
    G2_SetOBJAttr(&oam[1], 100, 40, 3, GX_OAM_MODE_NORMAL, FALSE, GX_OAM_EFFECT_NONE,
                  GX_OAM_SHAPE_8x8, GX_OAM_COLORMODE_16, 1, 0, 0);
    /* another: palette 5, priority 0 */
    G2_SetOBJAttr(&oam[2], 10, 100, 0, GX_OAM_MODE_NORMAL, FALSE, GX_OAM_EFFECT_NONE,
                  GX_OAM_SHAPE_8x8, GX_OAM_COLORMODE_16, 0, 5, 0);
    DC_FlushRange(oam, sizeof(oam));
    GX_LoadOAM(oam, 0, sizeof(oam));
    GX_SetVisiblePlane(GX_PLANEMASK_OBJ);
    memset(&split, 0, sizeof(split));
    split.width = 320;
    split.origin = 32;
    memset(split.clip, 1, sizeof(split.clip));

    n = NitroPort_Render2DObjGroups(0, &split, groups, 8);
    CHECK(n == 2);
    for (i = 0; i < n; i++) {
        if (groups[i].group == (3 << 5)) {
            g3 = i;
        }
    }
    CHECK(g3 >= 0 && groups[g3].prio == 3 && groups[g3].count == 2);
    CHECK(NitroPort_Render2DObjLayer(0, 3 << 5, &split, layerPx));
    CHECK(layerPx[42 * 320 + 32 + 62] == (BLUE | 0x8000));      /* OBJ 0 */
    CHECK(layerPx[102 * 320 + 32 + 12] == 0);                     /* (the other group's) */
    CHECK(!NitroPort_Render2DObjLayer(0, 3 << 5 | 7, &split, layerPx));
    /* moved, in the other order, its tiles elsewhere: the same key */
    GX_LoadOBJ(tiles, 64, sizeof(tiles));
    G2_SetOBJAttr(&oam[0], 108, 50, 3, GX_OAM_MODE_NORMAL, FALSE, GX_OAM_EFFECT_NONE,
                  GX_OAM_SHAPE_8x8, GX_OAM_COLORMODE_16, 3, 0, 0);
    G2_SetOBJAttr(&oam[1], 68, 50, 3, GX_OAM_MODE_NORMAL, FALSE, GX_OAM_EFFECT_NONE,
                  GX_OAM_SHAPE_8x8, GX_OAM_COLORMODE_16, 2, 0, 0);
    DC_FlushRange(oam, sizeof(oam));
    GX_LoadOAM(oam, 0, sizeof(oam));
    CHECK(NitroPort_Render2DObjGroups(0, &split, groups2, 8) == 2);
    for (i = 0; i < 2; i++) {
        if (groups2[i].group == (3 << 5)) {
            CHECK(groups2[i].key == groups[g3].key);
            CHECK(groups2[i].x - groups[g3].x == 8 && groups2[i].y - groups[g3].y == 10);
        }
    }
    /* a different colour: a different key */
    objPltt[2] = RED;
    GX_LoadOBJPltt(objPltt, 0, sizeof(objPltt));
    CHECK(NitroPort_Render2DObjGroups(0, &split, groups2, 8) == 2);
    for (i = 0; i < 2; i++) {
        if (groups2[i].group == (3 << 5)) {
            CHECK(groups2[i].key != groups[g3].key);
        }
    }
    objPltt[2] = GREEN;
    GX_LoadOBJPltt(objPltt, 0, sizeof(objPltt));
    /* back where they were captured */
    G2_SetOBJAttr(&oam[0], 100, 40, 3, GX_OAM_MODE_NORMAL, FALSE, GX_OAM_EFFECT_NONE,
                  GX_OAM_SHAPE_8x8, GX_OAM_COLORMODE_16, 3, 0, 0);
    G2_SetOBJAttr(&oam[1], 60, 40, 3, GX_OAM_MODE_NORMAL, FALSE, GX_OAM_EFFECT_NONE,
                  GX_OAM_SHAPE_8x8, GX_OAM_COLORMODE_16, 2, 0, 0);
    DC_FlushRange(oam, sizeof(oam));
    GX_LoadOAM(oam, 0, sizeof(oam));

    /* its picture: yellow all over (bands only), a BG of priority 1 in
     * front of priority 3 hides it, the backdrop doesn't */
    CHECK(mkdtemp(dir) != NULL);
    for (i = 0; i < 320 * 192; i++) {
        art[i] = RGB(31, 31, 0) | 0x8000;
    }
    art[5 * 320 + 0] = RGB(31, 0, 31) | 0x8000;          /* (a mark, to see it move) */
    WriteArt(dir, groups[g3].key, groups[g3].x, groups[g3].y, 0, art);
    CHECK(NitroPort_WideArtLoad(dir) == 1);
    split.over = over;
    split.overAlpha = overAlpha;
    NitroPort_Render2DSplit(0, wide, &split);
    CHECK(wide[5 * 320 + 0] == RGB(31, 0, 31));          /* the bands */
    CHECK(wide[5 * 320 + 3] == RGB(31, 31, 0));
    CHECK(wide[5 * 320 + 300] == RGB(31, 31, 0));
    CHECK(wide[5 * 320 + 100] != RGB(31, 31, 0));        /* not the middle */
    CHECK(wide[102 * 320 + 32 + 12] == RED);             /* the other group's OBJ */
    /* the group moved 8 right: so did the picture; its left 8 columns are
     * its x -8 to -1, a screen's width on (248-255): a strip of sprites
     * that scrolls round goes on. (Was: empty, which cut the intro's
     * strips short of the screen's edge.) */
    G2_SetOBJAttr(&oam[0], 108, 40, 3, GX_OAM_MODE_NORMAL, FALSE, GX_OAM_EFFECT_NONE,
                  GX_OAM_SHAPE_8x8, GX_OAM_COLORMODE_16, 3, 0, 0);
    G2_SetOBJAttr(&oam[1], 68, 40, 3, GX_OAM_MODE_NORMAL, FALSE, GX_OAM_EFFECT_NONE,
                  GX_OAM_SHAPE_8x8, GX_OAM_COLORMODE_16, 2, 0, 0);
    DC_FlushRange(oam, sizeof(oam));
    GX_LoadOAM(oam, 0, sizeof(oam));
    NitroPort_Render2DSplit(0, wide, &split);
    CHECK(wide[5 * 320 + 8] == RGB(31, 0, 31));
    CHECK(wide[5 * 320 + 3] == RGB(31, 31, 0));
    CHECK(wide[5 * 320 + 10] == RGB(31, 31, 0));
    /* the capture: an obj line for each group, and its picture */
    {
        char made[512], path[600], line[256];
        FILE *f;
        int found = 0;

        NitroPort_SetWideScreen(32);
        CHECK(NitroPort_WideArtCapture(dir, made, sizeof(made)) == 0);
        snprintf(path, sizeof(path), "%s/info.txt", made);
        f = fopen(path, "r");
        CHECK(f != NULL);
        while (f != NULL && fgets(line, sizeof(line), f) != NULL) {
            unsigned long long k;
            int g;

            if (sscanf(line, "obj %d prio 3 key %llx", &g, &k) == 2 && g == (3 << 5) && k == groups[g3].key) {
                found = 1;
            }
        }
        if (f != NULL) {
            fclose(f);
        }
        CHECK(found);
        snprintf(path, sizeof(path), "%s/obj%d.png", made, 3 << 5);
        CHECK(access(path, F_OK) == 0);
        NitroPort_SetWideScreen(0);
    }
    NitroPort_WideArtLoad("/nonexistent");
    {
        char cmd[600];

        snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
        CHECK(system(cmd) == 0);
    }
    (void)x;
}

/* Where a layer's pictures show: the one made nearest the layer's scroll,
 * not one of another part of its map; what was drawn in a picture's bands
 * never over the DS's part. */
static void test_wide_art_places(void)
{
    static u16 wide[320 * 192], over[320 * 192], art[320 * 192];
    static u8 overAlpha[320 * 192];
    static u8 tiles[96];
    static u16 map[32 * 64];
    static u16 pltt[16] = { GREY, RED, BLUE };
    char dir[] = "/tmp/wideartplXXXXXX";
    NitroRender2DSplit split;
    uint64_t key;
    int hofs, vofs, prio, i, x, y;
    const u16 SKY = RGB(0, 0, 31) | 0x8000, STRIP = RGB(0, 31, 0) | 0x8000, MADE = RGB(31, 0, 31) | 0x8000;
    static u16 layerPx[320 * 192];

    /* BG1, 256x512: red tiles (the sky) in the map's top half, blue (the
     * strip) in the bottom */
    Reset();
    MakeTile4(tiles + 32, 1);
    MakeTile4(tiles + 64, 2);
    for (i = 0; i < 32 * 64; i++) {
        map[i] = i < 32 * 32 ? 1 : 2;
    }
    G2_SetBG1Control(GX_BG_SCRSIZE_TEXT_256x512, GX_BG_COLORMODE_16, GX_BG_SCRBASE_0x0000,
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

    /* two pictures of it: the sky's (bands only) at vofs 0, the strip's at
     * 256; (going round with it: _loop, so their bands show wherever) */
    CHECK(mkdtemp(dir) != NULL);
    for (i = 0; i < 320 * 192; i++) {
        art[i] = SKY;
    }
    WriteArtKind(dir, key, 0, 0, NITRO_WIDE_ART_LOOP, art);
    for (i = 0; i < 320 * 192; i++) {
        art[i] = STRIP;
    }
    WriteArtKind(dir, key, 0, 256, NITRO_WIDE_ART_LOOP, art);
    CHECK(NitroPort_WideArtLoad(dir) == 2);
    split.over = over;
    split.overAlpha = overAlpha;
    NitroPort_Render2DSplit(0, wide, &split);
    CHECK(wide[5 * 320 + 3] == (SKY & 0x7FFF));               /* at 0: the sky's */
    G2_SetBG1Offset(0, 256);
    NitroPort_Render2DSplit(0, wide, &split);
    CHECK(wide[5 * 320 + 3] == (STRIP & 0x7FFF));             /* at 256: the strip's */
    G2_SetBG1Offset(0, 100);
    NitroPort_Render2DSplit(0, wide, &split);
    CHECK(wide[5 * 320 + 3] == (SKY & 0x7FFF));               /* nearer the sky's */
    G2_SetBG1Offset(0, 200);
    NitroPort_Render2DSplit(0, wide, &split);
    CHECK(wide[100 * 320 + 3] == (STRIP & 0x7FFF));           /* nearer the strip's (line 100: map line 300) */
    /* the strip's picture of its bands only (its middle clear), moved round
     * with the strip so far that its middle comes in beside the DS's
     * screen: there the BG's own pixels, what the middle was (the
     * opening's blue strip, 2026-10-23: nothing there before) */
    for (y = 0; y < 192; y++) {
        for (x = 0; x < 320; x++) {
            art[y * 320 + x] = (x >= 32 && x < 288) ? 0 : STRIP;
        }
    }
    WriteArtKind(dir, key, 0, 256, NITRO_WIDE_ART_LOOP, art);
    CHECK(NitroPort_WideArtLoad(dir) == 2);
    G2_SetBG1Offset(8, 256);
    NitroPort_Render2DSplit(0, wide, &split);
    CHECK(wide[100 * 320 + 3] == (STRIP & 0x7FFF));           /* its band, moved 8 */
    CHECK(wide[100 * 320 + 26] == BLUE);                      /* its middle come in: the BG's own */
    G2_SetBG1Offset(100, 256);
    NitroPort_Render2DSplit(0, wide, &split);
    CHECK(wide[100 * 320 + 3] == BLUE && wide[100 * 320 + 300] == BLUE);
    for (i = 0; i < 320 * 192; i++) {
        art[i] = STRIP;
    }
    G2_SetBG1Offset(0, 200);
    /* not going round (the usual): its bands only where it was made */
    {
        char path[600];

        snprintf(path, sizeof(path), "%s/%016llx_0_256_loop.png", dir, (unsigned long long)key);
        remove(path);
        snprintf(path, sizeof(path), "%s/%016llx_0_0_loop.png", dir, (unsigned long long)key);
        remove(path);
    }
    WriteArt(dir, key, 0, 256, 0, art);
    CHECK(NitroPort_WideArtLoad(dir) == 1);
    NitroPort_Render2DSplit(0, wide, &split);
    CHECK(wide[100 * 320 + 3] == (STRIP & 0x7FFF));           /* 56 down: there (up and down doesn't matter) */
    G2_SetBG1Offset(40, 256);
    NitroPort_Render2DSplit(0, wide, &split);
    CHECK(wide[100 * 320 + 3] == GREY);                       /* 40 sideways: none */
    G2_SetBG1Offset(4, 252);
    NitroPort_Render2DSplit(0, wide, &split);
    CHECK(wide[100 * 320 + 3] == (STRIP & 0x7FFF));           /* 4 sideways: there */

    /* only one picture, a screen or more away: not used (the port's own
     * band, here the plain edge going on) */
    WriteArtKind(dir, key, 0, 0, NITRO_WIDE_ART_LOOP, art);   /* (the strip colour at 0) */
    {
        char path[600];

        snprintf(path, sizeof(path), "%s/%016llx_0_256.png", dir, (unsigned long long)key);
        remove(path);
    }
    CHECK(NitroPort_WideArtLoad(dir) == 1);
    G2_SetBG1Offset(0, 256);
    NitroPort_Render2DSplit(0, wide, &split);
    CHECK(wide[5 * 320 + 3] == BLUE);
    G2_SetBG1Offset(0, 0);

    /* a whole picture: its own middle (purple), green bands; scrolled 20
     * right, its right band doesn't come over the DS's part: the BG's own
     * there */
    for (y = 0; y < 192; y++) {
        for (x = 0; x < 320; x++) {
            art[y * 320 + x] = (x >= 32 && x < 288) ? MADE : STRIP;
        }
    }
    WriteArt(dir, key, 0, 0, 1, art);
    {
        char path[600];

        snprintf(path, sizeof(path), "%s/%016llx_0_0_loop.png", dir, (unsigned long long)key);
        remove(path);
    }
    CHECK(NitroPort_WideArtLoad(dir) == 1);
    NitroPort_Render2DSplit(0, wide, &split);
    CHECK(wide[5 * 320 + 100] == (MADE & 0x7FFF));
    CHECK(wide[5 * 320 + 300] == (STRIP & 0x7FFF));
    /* Scrolled 20 right (October 2026: a whole picture is the whole
     * layer, moved with it, so that a Pokemon sliding in isn't cut at the
     * DS's edge; was: the picture's middle only, the map's own beside it,
     * nothing in the bands): its right band 20 to the left, over the DS's
     * part; past its right edge the map's own. */
    G2_SetBG1Offset(20, 0);
    NitroPort_Render2DSplit(0, wide, &split);
    CHECK(wide[5 * 320 + 100] == (MADE & 0x7FFF));            /* the picture's middle, moved */
    CHECK(wide[5 * 320 + 280] == (STRIP & 0x7FFF));           /* its band */
    CHECK(wide[5 * 320 + 300] == RED);                        /* past it: the map's own */
    CHECK(wide[5 * 320 + 12] == (MADE & 0x7FFF));
    CHECK(wide[5 * 320 + 5] == (STRIP & 0x7FFF));             /* its left band, 20 on */
    G2_SetBG1Offset(4, 0);
    NitroPort_Render2DSplit(0, wide, &split);
    CHECK(wide[5 * 320 + 300] == (STRIP & 0x7FFF));           /* 4 away: the bands */
    G2_SetBG1Offset(0, 0);

    NitroPort_WideArtLoad("/nonexistent");
    {
        char cmd[600];

        snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
        CHECK(system(cmd) == 0);
    }
}

/* A whole picture of a layer that slides in (the opening's Pokemon): the
 * whole layer from the picture, moved with it, never cut at the DS's edge;
 * its slide widened so that it comes in from the wide screen's edge, and
 * what's waiting beyond the DS's edge stays hidden. */
static void test_wide_art_sliding(void)
{
    static u16 wide[320 * 192], over[320 * 192], art[320 * 192], layerPx[320 * 192];
    static u8 overAlpha[320 * 192];
    static u8 tiles[96];
    static u16 map[64 * 32];
    static u16 pltt[16] = { GREY, RED, BLUE };
    const u16 MON = RGB(31, 20, 0) | 0x8000;
    char dir[] = "/tmp/wideartslXXXXXX";
    NitroRender2DSplit split;
    uint64_t key;
    int hofs, vofs, prio, i, x, y, left, right;

    /* the widening itself: a picture whose opaque columns are 150-199 */
    CHECK(NitroPort_WideArtMovedOffset(0, 150, 200) == 0);
    CHECK(NitroPort_WideArtMovedOffset(50, 150, 200) == 50);      /* well inside: as far */
    CHECK(NitroPort_WideArtMovedOffset(106, 150, 200) == 106);
    CHECK(NitroPort_WideArtMovedOffset(122, 150, 200) == 138);    /* the last 32: twice as far */
    CHECK(NitroPort_WideArtMovedOffset(138, 150, 200) == 170);    /* at the DS's edge: at the wide one's */
    CHECK(NitroPort_WideArtMovedOffset(152, 150, 200) == 184);    /* beyond: 32 more */
    CHECK(NitroPort_WideArtMovedOffset(-50, 150, 200) == -50);    /* going left: the same */
    CHECK(NitroPort_WideArtMovedOffset(-168, 150, 200) == -200);  /* right edge at the DS's: at the screen's */
    for (i = 0; i <= 260; i++) {                                  /* never back, never cut */
        int a = NitroPort_WideArtMovedOffset(i, 150, 200), b = NitroPort_WideArtMovedOffset(i + 1, 150, 200);

        CHECK(b >= a);
        CHECK((150 + i < 288) || (150 + a >= 320));               /* hidden on the DS: hidden */
        CHECK(-NitroPort_WideArtMovedOffset(-i, 150, 200) >= i);
    }

    /* BG1, 512x256, a tile far off (map x 400) so it has art */
    Reset();
    MakeTile4(tiles + 32, 1);
    for (i = 0; i < 64 * 32; i++) {
        map[i] = 0;
    }
    map[32 * 32 + 18] = 1;   /* (the right 256x256 screen block, column 18) */
    G2_SetBG1Control(GX_BG_SCRSIZE_TEXT_512x256, GX_BG_COLORMODE_16, GX_BG_SCRBASE_0x0000,
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

    /* its whole picture at scroll 0: a Pokemon at columns 150-199, its
     * right edge marked blue */
    CHECK(mkdtemp(dir) != NULL);
    for (y = 0; y < 192; y++) {
        for (x = 0; x < 320; x++) {
            art[y * 320 + x] = (x >= 150 && x < 200) ? (x == 199 ? (BLUE | 0x8000) : MON) : 0;
        }
    }
    WriteArt(dir, key, 0, 0, 1, art);
    CHECK(NitroPort_WideArtLoad(dir) == 1);
    split.over = over;
    split.overAlpha = overAlpha;
    NitroPort_Render2DSplit(0, wide, &split);
    CHECK(wide[5 * 320 + 150] == (MON & 0x7FFF));
    CHECK(NitroPort_WideArtExtent(NitroPort_WideArtFindNear(key, 0, 0, 512, 256, 256, 192, &hofs, &vofs, &i),
                                  &left, &right) && left == 150 && right == 200);
    /* waiting beyond the DS's edge (152 to the right: its columns 302-351,
     * on the DS hidden): hidden on the wide screen too */
    G2_SetBG1Offset(512 - 152, 0);
    NitroPort_Render2DSplit(0, wide, &split);
    for (x = 0; x < 320; x++) {
        CHECK(wide[5 * 320 + x] == GREY);
    }
    /* coming in, 122 to the right (the DS shows its columns 272-287): 138
     * on the wide screen, whole up to its edge */
    G2_SetBG1Offset(512 - 122, 0);
    NitroPort_Render2DSplit(0, wide, &split);
    CHECK(wide[5 * 320 + 287] == GREY);
    CHECK(wide[5 * 320 + 288] == (MON & 0x7FFF));
    CHECK(wide[5 * 320 + 319] == (MON & 0x7FFF));
    /* 50 to the right: where the DS has it, whole (its blue right edge at
     * 249, inside), the BG's own past the picture */
    G2_SetBG1Offset(512 - 50, 0);
    NitroPort_Render2DSplit(0, wide, &split);
    CHECK(wide[5 * 320 + 199] == GREY);
    CHECK(wide[5 * 320 + 200] == (MON & 0x7FFF));
    CHECK(wide[5 * 320 + 249] == BLUE);
    CHECK(wide[5 * 320 + 250] == GREY);
    /* 100 to the right: its edge in the right band, not cut at the DS's */
    G2_SetBG1Offset(512 - 100, 0);
    NitroPort_Render2DSplit(0, wide, &split);
    CHECK(wide[5 * 320 + 290] == (MON & 0x7FFF));
    CHECK(wide[5 * 320 + 299] == BLUE);
    G2_SetBG1Offset(0, 0);

    /* Two pictures of it, each cut by the wide screen's right edge (the
     * opening's Torterra): at 0, columns 200-319; at 452 (60 to the right),
     * green, 260-319. At 464 the nearer one, moved 12 left, would show its
     * cut edge at 308: the one at 0 instead, moved 48 right. */
    for (y = 0; y < 192; y++) {
        for (x = 0; x < 320; x++) {
            art[y * 320 + x] = x >= 200 ? MON : 0;
        }
    }
    WriteArt(dir, key, 0, 0, 1, art);
    for (y = 0; y < 192; y++) {
        for (x = 0; x < 320; x++) {
            art[y * 320 + x] = x >= 260 ? (GREEN | 0x8000) : 0;
        }
    }
    WriteArt(dir, key, 452, 0, 1, art);
    CHECK(NitroPort_WideArtLoad(dir) == 2);
    G2_SetBG1Offset(456, 0);
    NitroPort_Render2DSplit(0, wide, &split);
    CHECK(wide[5 * 320 + 312] == GREEN);                      /* 4 from 452: that one */
    G2_SetBG1Offset(464, 0);
    NitroPort_Render2DSplit(0, wide, &split);
    CHECK(wide[5 * 320 + 312] == (MON & 0x7FFF));             /* the one at 0, not cut */
    CHECK(wide[5 * 320 + 247] == GREY);
    CHECK(wide[5 * 320 + 248] == (MON & 0x7FFF));
    G2_SetBG1Offset(0, 0);

    NitroPort_WideArtLoad("/nonexistent");
    {
        char cmd[600];

        snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
        CHECK(system(cmd) == 0);
    }
}

/* A window over the screen from 0 to 255 (the DS's register can't say
 * 256: its last column is left out) covers the wider screen's bands too. */
static void test_wide_window_edges(void)
{
    static u16 wide[320 * 192], over[320 * 192];
    static u8 overAlpha[320 * 192];
    static u8 tiles[64];
    static u16 map[32 * 32];
    static u16 pltt[16] = { GREY, RED };
    NitroRender2DSplit split;
    int i;

    Reset();
    MakeTile4(tiles + 32, 1);
    for (i = 0; i < 32 * 32; i++) {
        map[i] = 1;
    }
    G2_SetBG1Control(GX_BG_SCRSIZE_TEXT_256x256, GX_BG_COLORMODE_16, GX_BG_SCRBASE_0x0000,
                     GX_BG_CHARBASE_0x04000, GX_BG_EXTPLTT_01);
    GX_LoadBG1Char(tiles, 0, sizeof(tiles));
    GX_LoadBG1Scr(map, 0, sizeof(map));
    GX_LoadBGPltt(pltt, 0, sizeof(pltt));
    GX_SetVisiblePlane(GX_PLANEMASK_BG1);
    G2_SetWnd0Position(0, 32, 255, 160);
    G2_SetWnd0InsidePlane(GX_WND_PLANEMASK_BG1, FALSE);
    G2_SetWndOutsidePlane(GX_WND_PLANEMASK_NONE, FALSE);
    GX_SetVisibleWnd(GX_WNDMASK_W0);
    memset(&split, 0, sizeof(split));
    split.width = 320;
    split.origin = 32;
    memset(split.clip, 0, sizeof(split.clip));   /* (the map goes on: a full-screen picture) */
    split.over = over;
    split.overAlpha = overAlpha;
    NitroPort_Render2DSplit(0, wide, &split);
    CHECK(wide[100 * 320 + 3] == RED);            /* the left band */
    CHECK(wide[100 * 320 + 32 + 255] == RED);     /* the DS's last column */
    CHECK(wide[100 * 320 + 310] == RED);          /* the right band */
    CHECK(wide[10 * 320 + 310] == GREY);          /* (above the window: outside it) */
    /* the DS's own screen: its last column outside the window, as on the DS */
    NitroPort_Render2D(0, sTop);
    CHECK(sTop[100 * 256 + 255] == GREY);
    CHECK(sTop[100 * 256 + 254] == RED);
    /* the remaster's lower screen (the DS's width, drawn bigger): the last
     * column inside, no black strip down its right edge */
    memset(&split, 0, sizeof(split));
    split.width = 256;
    split.origin = 0;
    memset(split.clip, 1, sizeof(split.clip));
    split.over = over;
    split.overAlpha = overAlpha;
    NitroPort_Render2DSplit(0, wide, &split);
    CHECK(wide[100 * 256 + 255] == GREY);   /* (without windowsToEdge: as the DS) */
    split.windowsToEdge = 1;
    NitroPort_Render2DSplit(0, wide, &split);
    CHECK(wide[100 * 256 + 255] == RED);
    CHECK(wide[100 * 256 + 0] == RED);
    CHECK(wide[10 * 256 + 255] == GREY);
    /* An empty window (x 0-0: a circle wipe's lines past its circle) covers
     * nothing on the wider canvas either, the bands included (they showed
     * the Town Map's side bars while its circle closed); a window across
     * the middle leaves the bands outside. */
    memset(&split, 0, sizeof(split));
    split.width = 320;
    split.origin = 32;
    split.windowsToEdge = 1;
    split.over = over;
    split.overAlpha = overAlpha;
    G2_SetWnd0Position(0, 32, 0, 160);
    NitroPort_Render2DSplit(0, wide, &split);
    CHECK(wide[100 * 320 + 3] == GREY);
    CHECK(wide[100 * 320 + 32 + 100] == GREY);
    CHECK(wide[100 * 320 + 310] == GREY);
    G2_SetWnd0Position(100, 32, 156, 160);
    NitroPort_Render2DSplit(0, wide, &split);
    CHECK(wide[100 * 320 + 3] == GREY);
    CHECK(wide[100 * 320 + 32 + 120] == RED);
    CHECK(wide[100 * 320 + 310] == GREY);
    GX_SetVisibleWnd(GX_WNDMASK_NONE);
}

/* A BG whose map goes on past the DS's edges (the battle's background,
 * clip off): a 512-wide map, its picture then the same picture mirrored
 * (tiles flipped sideways), but for a few tiles of a picture of its own
 * just left of the DS's edge (the player's platform's left part, which
 * the game draws there). Beside the DS: those tiles as they are; where
 * the map only has the mirror, the map's pixel next to the DS's edge
 * repeated (the background's own edge), not the DS's edge pixel (the
 * platform's, drawn out into lines, 2026-10-16), nothing mirrored. */
static void test_wide_map_past_edges(void)
{
    static u16 wide[320 * 192], over[320 * 192];
    static u8 overAlpha[320 * 192], tiles[4 * 32];
    static u16 map[64 * 32];
    static u16 pltt[16] = { GREY, RED, BLUE, GREEN, WHITE };
    NitroRender2DSplit split;
    int i, x, y;

    Reset();
    /* tile 1 red, tile 2 blue, tile 3 green on its left half and white on
     * its right (so a flip shows) */
    memset(tiles, 0, sizeof(tiles));
    memset(tiles + 32, 0x11, 32);
    memset(tiles + 64, 0x22, 32);
    for (i = 0; i < 32; i++) {
        tiles[96 + i] = (u8)((i & 3) < 2 ? 0x33 : 0x44);
    }
    for (y = 0; y < 32; y++) {
        for (x = 0; x < 32; x++) {
            /* the picture: its first column blue (a platform at the DS's edge) */
            map[y * 32 + x] = (u16)(x == 0 ? 2 : 1);
            /* the mirror (the right 256x256 block): red, flipped */
            map[32 * 32 + y * 32 + x] = (u16)(1 | 0x400);
        }
    }
    for (y = 8; y < 16; y++) {
        for (x = 28; x < 32; x++) {
            map[32 * 32 + y * 32 + x] = 3;   /* (the platform's own, map x 480-511) */
        }
    }
    G2_SetBG1Control(GX_BG_SCRSIZE_TEXT_512x256, GX_BG_COLORMODE_16, GX_BG_SCRBASE_0x0000,
                     GX_BG_CHARBASE_0x04000, GX_BG_EXTPLTT_01);
    GX_LoadBG1Char(tiles, 0, sizeof(tiles));
    GX_LoadBG1Scr(map, 0, sizeof(map));
    GX_LoadBGPltt(pltt, 0, sizeof(pltt));
    GX_SetVisiblePlane(GX_PLANEMASK_BG1);
    G2_SetBG1Offset(0, 0);
    memset(&split, 0, sizeof(split));
    split.width = 320;
    split.origin = 32;
    memset(split.clip, 0, sizeof(split.clip));
    split.over = over;
    split.overAlpha = overAlpha;
    NitroPort_Render2DSplit(0, wide, &split);
    CHECK(wide[10 * 320 + 32] == BLUE && wide[10 * 320 + 40] == RED);   /* (the DS's) */
    CHECK(wide[10 * 320 + 0] == RED && wide[10 * 320 + 31] == RED);     /* the mirror: the map's next pixel */
    CHECK(wide[70 * 320 + 0] == GREEN && wide[70 * 320 + 4] == WHITE);  /* its own: as it is */
    CHECK(wide[70 * 320 + 24] == GREEN && wide[70 * 320 + 31] == WHITE);
    CHECK(wide[10 * 320 + 288] == RED && wide[10 * 320 + 319] == RED);  /* the right: the mirror */
    /* scrolled 8 to the right (the slide): the same, moved */
    G2_SetBG1Offset(8, 0);
    NitroPort_Render2DSplit(0, wide, &split);
    CHECK(wide[70 * 320 + 0] == GREEN && wide[70 * 320 + 23] == WHITE);  /* (map x 488-511) */
    CHECK(wide[70 * 320 + 24] == BLUE && wide[70 * 320 + 31] == BLUE);   /* (map x 0-7) */
    CHECK(wide[70 * 320 + 32] == RED);
    G2_SetBG1Offset(0, 0);
}

/* One 4bpp tile all colour index `index`. */
/* (the remaster's compositor) A repeating BG it draws itself
 * (NitroRender2DSplit.gpuBgs: the field's weather): left out of the
 * picture when it's alpha-blended alone onto the 3D right in front of it,
 * its scroll then no reason to draw the picture again; its whole map as a
 * picture (NitroPort_Render2DBgPicture). Not with an OBJ between it and
 * the 3D. */
static void test_gpu_bg(void)
{
    static u8 tiles[2 * 32];
    static u16 map[32 * 32];
    static u16 pltt[16] = { GREY, RED, GREEN, BLUE };
    static uint32_t rgba[256 * 192];
    static u16 under[256 * 192], over[256 * 192], picture[256 * 256];
    static u8 overAlpha[256 * 192];
    NitroRender2DSplit split;
    int i, any;

    Reset();
    MakeTile4(tiles + 32, 1);           /* tile 1: red, green top-left */
    memset(map, 0, sizeof(map));
    map[0] = 1;
    map[1] = 1 | 0x400;                 /* (8,0): flipped horizontally */
    map[32 * 31 + 31] = 1;              /* the map's last tile */
    GX_SetGraphicsMode(GX_DISPMODE_GRAPHICS, GX_BGMODE_0, GX_BG0_AS_3D);
    G2_SetBG2ControlText(GX_BG_SCRSIZE_TEXT_256x256, GX_BG_COLORMODE_16, GX_BG_SCRBASE_0x0800,
                         GX_BG_CHARBASE_0x04000);
    GX_LoadBG2Char(tiles, 0, sizeof(tiles));
    GX_LoadBG2Scr(map, 0, sizeof(map));
    GX_LoadBGPltt(pltt, 0, sizeof(pltt));
    G2_SetBG0Priority(2);
    G2_SetBG2Priority(1);
    GX_SetVisiblePlane(GX_PLANEMASK_BG0 | GX_PLANEMASK_BG2);
    G2_SetBlendAlpha(GX_BLEND_PLANEMASK_BG2, GX_BLEND_PLANEMASK_BG0, 6, 10);
    for (i = 0; i < 256 * 192; i++) {
        rgba[i] = 0x404040FFu;
    }
    NitroPort_Gx3dPublishRGBA8(rgba, 256 * 192);

    memset(&split, 0, sizeof(split));
    split.over = over;
    split.overAlpha = overAlpha;
    NitroPort_Render2DSplit(0, under, &split);
    CHECK(split.has3d);
    CHECK(split.gpuBgLeftOut == 0);     /* not asked for: drawn */
    CHECK(over[3 * 256 + 3] & 0x8000);

    split.gpuBgs = 1 << 2;
    G2_SetBG2Offset(5, 7);
    NitroPort_Render2DSplit(0, under, &split);
    CHECK(split.gpuBgLeftOut == 1 << 2);
    CHECK(split.gpuBgEva == 6 && split.gpuBgEvb == 10);
    CHECK(split.gpuBgHofs == 5 && split.gpuBgVofs == 7);
    for (i = 0, any = 0; i < 256 * 192; i++) {
        any |= over[i] & 0x8000;
    }
    CHECK(any == 0);                    /* nothing of it in front of the 3D */

    /* its picture: the whole map, RGBA5551, 0 where it's clear */
    CHECK(NitroPort_Render2DBgPicture(2, 0, picture));
    CHECK(picture[0] == (u16)((0 << 11) | (31 << 6) | (0 << 1) | 1));        /* green top-left */
    CHECK(picture[3 * 256 + 3] == (u16)((31 << 11) | 1));                     /* red */
    CHECK(picture[15] == (u16)((0 << 11) | (31 << 6) | 1));                   /* flipped: green at the right */
    CHECK(picture[20 * 256 + 20] == 0);                                      /* clear */
    CHECK(picture[255 * 256 + 255] == (u16)((31 << 11) | 1));                 /* the map's last tile */

    /* its scroll alone: not drawn again; anything else: drawn (the first
     * two times: drawn, then the VRAM noted) */
    NitroPort_Render2DSplitIfChanged(0, under, &split);
    NitroPort_Render2DSplitIfChanged(0, under, &split);
    CHECK(NitroPort_Render2DSplitIfChanged(0, under, &split) == 0);
    G2_SetBG2Offset(9, 12);
    CHECK(NitroPort_Render2DSplitIfChanged(0, under, &split) == 0);
    G2_SetBG2Offset(10, 12);
    CHECK(NitroPort_Render2DSplitIfChanged(0, under, &split) == 0);
    split.gpuBgs = 0;
    CHECK(NitroPort_Render2DSplitIfChanged(0, under, &split) == 1);
    G2_SetBG2Offset(11, 12);
    CHECK(NitroPort_Render2DSplitIfChanged(0, under, &split) == 1);
    split.gpuBgs = 1 << 2;

    /* an OBJ between it and the 3D (priority 2): drawn as before */
    {
        u16 obj[4] = { 0, 0, (u16)(2 << 10), 0 };

        memcpy(NitroPort_Oam, obj, sizeof(obj));
        GX_SetVisiblePlane(GX_PLANEMASK_BG0 | GX_PLANEMASK_BG2 | GX_PLANEMASK_OBJ);
        NitroPort_Render2DSplit(0, under, &split);
        CHECK(split.gpuBgLeftOut == 0);
        obj[2] = (u16)(1 << 10);        /* in front of it: fine */
        memcpy(NitroPort_Oam, obj, sizeof(obj));
        NitroPort_Render2DSplit(0, under, &split);
        CHECK(split.gpuBgLeftOut == 1 << 2);
        obj[0] = (u16)(1 << 10);        /* in front of it, semi-transparent: drawn as before */
        memcpy(NitroPort_Oam, obj, sizeof(obj));
        NitroPort_Render2DSplit(0, under, &split);
        CHECK(split.gpuBgLeftOut == 0);
        MI_CpuClearFast(NitroPort_Oam, 0x800);
    }
    /* blended with something else too, or behind the 3D: drawn */
    G2_SetBlendAlpha(GX_BLEND_PLANEMASK_BG2 | GX_BLEND_PLANEMASK_BG1, GX_BLEND_PLANEMASK_BG0, 6, 10);
    NitroPort_Render2DSplit(0, under, &split);
    CHECK(split.gpuBgLeftOut == 0);
    G2_SetBlendAlpha(GX_BLEND_PLANEMASK_BG2, GX_BLEND_PLANEMASK_BG0, 6, 10);
    G2_SetBG2Priority(3);
    NitroPort_Render2DSplit(0, under, &split);
    CHECK(split.gpuBgLeftOut == 0);
}

/* The field at the DS's pixel size (NitroRender2DSplit.top): a 400 x 240
 * picture, the DS's in its middle (line 0 at 24, x 0 at 72). A clipped
 * BG's map past the DS's lines isn't shown (its leftover tiles), but a
 * line one colour across goes on; the OBJs go on above and below as on a
 * taller screen; an unclipped BG's map goes on; the 3D on every line. */
/* A battle's start stretched over the wider screen (NitroRender2DSplit.
 * stretch): the DS's layout, each OBJ 4/5 as wide round its middle. */
static void test_stretch_squeeze(void)
{
    static u8 objTile[4 * 32];
    static u16 objPltt[16] = { 0, BLUE };
    static GXOamAttr oam[128];
    static uint32_t rgba[256 * 192];
    static u16 under[256 * 192], over[256 * 192];
    static u8 overAlpha[256 * 192];
    NitroRender2DSplit split;
    int i, x, first = -1, last = -1;

    Reset();
    memset(objTile, 0x11, sizeof(objTile));   /* a 16 x 16 OBJ, all blue */
    GX_SetGraphicsMode(GX_DISPMODE_GRAPHICS, GX_BGMODE_0, GX_BG0_AS_3D);
    G2_SetBG0Priority(3);
    GX_SetOBJVRamModeChar(GX_OBJVRAMMODE_CHAR_1D_32K);
    GX_LoadOBJ(objTile, 0, sizeof(objTile));
    GX_LoadOBJPltt(objPltt, 0, sizeof(objPltt));
    MI_CpuFillFast(oam, 0x0200, sizeof(oam));
    G2_SetOBJAttr(&oam[0], 100, 50, 0, GX_OAM_MODE_NORMAL, FALSE, GX_OAM_EFFECT_NONE, GX_OAM_SHAPE_16x16,
                  GX_OAM_COLORMODE_16, 0, 0, 0);
    DC_FlushRange(oam, sizeof(oam));
    GX_LoadOAM(oam, 0, sizeof(oam));
    GX_SetVisiblePlane(GX_PLANEMASK_BG0 | GX_PLANEMASK_OBJ);
    for (i = 0; i < 256 * 192; i++) {
        rgba[i] = 0x404040FFu;
    }
    NitroPort_Gx3dPublishRGBA8(rgba, 256 * 192);

    memset(&split, 0, sizeof(split));
    split.over = over;
    split.overAlpha = overAlpha;
    split.stretch = 1;
    NitroPort_Render2DSplit(0, under, &split);
    CHECK(split.has3d);
    for (x = 0; x < 256; x++) {
        if (over[58 * 256 + x] & 0x8000) {
            if (first < 0) {
                first = x;
            }
            last = x;
        }
    }
    /* 16 wide at x 100, now 13 round its middle (108): 102-114 */
    CHECK(first == 102 && last == 114);
    /* its height as it was */
    CHECK((over[50 * 256 + 108] & 0x8000) && (over[65 * 256 + 108] & 0x8000) && !(over[66 * 256 + 108] & 0x8000));
    /* without: 100-115 */
    split.stretch = 0;
    NitroPort_Render2DSplit(0, under, &split);
    CHECK((over[58 * 256 + 100] & 0x8000) && (over[58 * 256 + 115] & 0x8000) && !(over[58 * 256 + 116] & 0x8000));

    /* two OBJs side by side, one picture (x 100-131): narrower round its
     * middle (116) as one, no gap between them: 104-128 */
    G2_SetOBJAttr(&oam[1], 116, 50, 0, GX_OAM_MODE_NORMAL, FALSE, GX_OAM_EFFECT_NONE, GX_OAM_SHAPE_16x16,
                  GX_OAM_COLORMODE_16, 0, 0, 0);
    DC_FlushRange(oam, sizeof(oam));
    GX_LoadOAM(oam, 0, sizeof(oam));
    split.stretch = 1;
    NitroPort_Render2DSplit(0, under, &split);
    first = last = -1;
    for (x = 0; x < 256; x++) {
        if (over[58 * 256 + x] & 0x8000) {
            if (first < 0) {
                first = x;
            }
            last = x;
        } else if (first >= 0 && last == x - 1) {
            break;   /* (the end of the first run) */
        }
    }
    CHECK(first == 104 && last == 128);
}

static void test_tall_field(void)
{
    enum { TW = 400, TH = 240, TOP = 24, OX = 72 };
    static u8 tiles[3 * 32], objTile[32];
    static u16 map1[32 * 32], map2[32 * 32], pltt[16] = { GREY, RED, GREEN, BLUE };
    static u16 objPltt[16] = { 0, BLUE };
    static GXOamAttr oam[128];
    static uint32_t rgba[256 * 192];
    static u16 under[TW * TH], over[TW * TH];
    static u8 overAlpha[TW * TH];
    NitroRender2DSplit split;
    int i;

#define TALL_PX(buf, x, y) ((buf)[(y) * TW + (x)] & 0x7FFF)
    Reset();
    for (i = 0; i < 32; i++) {
        tiles[32 + i] = 0x11;   /* tile 1: red */
        tiles[64 + i] = 0x22;   /* tile 2: green */
        objTile[i] = 0x11;      /* blue */
    }
    memset(map1, 0, sizeof(map1));
    map1[0] = 1;                 /* the DS's line 0: red at x 0-7 */
    map1[32 * 31] = 2;           /* map row 31 (above the DS's line 0): leftover green */
    memset(map2, 0, sizeof(map2));
    for (i = 0; i < 32; i++) {
        map2[32 * 23 + i] = 2;   /* BG2's last DS line's row: green across */
    }
    GX_SetGraphicsMode(GX_DISPMODE_GRAPHICS, GX_BGMODE_0, GX_BG0_AS_3D);
    G2_SetBG1Control(GX_BG_SCRSIZE_TEXT_256x256, GX_BG_COLORMODE_16, GX_BG_SCRBASE_0x0800,
                     GX_BG_CHARBASE_0x04000, GX_BG_EXTPLTT_01);
    G2_SetBG2ControlText(GX_BG_SCRSIZE_TEXT_256x256, GX_BG_COLORMODE_16, GX_BG_SCRBASE_0x1000,
                         GX_BG_CHARBASE_0x04000);
    GX_LoadBG1Char(tiles, 0, sizeof(tiles));
    GX_LoadBG1Scr(map1, 0, sizeof(map1));
    GX_LoadBG2Scr(map2, 0, sizeof(map2));
    GX_LoadBGPltt(pltt, 0, sizeof(pltt));
    G2_SetBG0Priority(3);
    G2_SetBG1Priority(0);
    G2_SetBG2Priority(1);
    GX_SetOBJVRamModeChar(GX_OBJVRAMMODE_CHAR_1D_32K);
    GX_LoadOBJ(objTile, 0, sizeof(objTile));
    GX_LoadOBJPltt(objPltt, 0, sizeof(objPltt));
    MI_CpuFillFast(oam, 0x0200, sizeof(oam));   /* all hidden */
    /* an OBJ 6 lines above the DS's screen (y 250), at x 100 */
    G2_SetOBJAttr(&oam[0], 100, 250, 0, GX_OAM_MODE_NORMAL, FALSE, GX_OAM_EFFECT_NONE, GX_OAM_SHAPE_8x8,
                  GX_OAM_COLORMODE_16, 0, 0, 0);
    DC_FlushRange(oam, sizeof(oam));
    GX_LoadOAM(oam, 0, sizeof(oam));
    GX_SetVisiblePlane(GX_PLANEMASK_BG0 | GX_PLANEMASK_BG1 | GX_PLANEMASK_BG2 | GX_PLANEMASK_OBJ);
    for (i = 0; i < 256 * 192; i++) {
        rgba[i] = 0x404040FFu;
    }
    NitroPort_Gx3dPublishRGBA8(rgba, 256 * 192);

    memset(&split, 0, sizeof(split));
    split.over = over;
    split.overAlpha = overAlpha;
    split.width = TW;
    split.origin = OX;
    split.height = TH;
    split.top = TOP;
    split.clip[0] = split.clip[1] = split.clip[2] = split.clip[3] = 1;
    NitroPort_Render2DSplit(0, under, &split);
    CHECK(split.has3d);
    /* BG1 (in front of the 3D): its line 0 at the picture's line 24 */
    CHECK(TALL_PX(over, OX, TOP) == RED && (over[TOP * TW + OX] & 0x8000));
    CHECK(TALL_PX(over, OX + 7, TOP + 7) == RED);
    /* its map row 31 (the DS's line -8..-1) not shown: nothing in front there */
    CHECK(!(over[(TOP - 4) * TW + OX] & 0x8000));
    /* the OBJ at the DS's line -6 (the picture's 18), x 100 (172) */
    CHECK(TALL_PX(over, OX + 100, TOP - 6) == BLUE && (over[(TOP - 6) * TW + OX + 100] & 0x8000));
    CHECK(TALL_PX(over, OX + 100, TOP + 1) == BLUE);
    CHECK(!(over[(TOP - 7) * TW + OX + 100] & 0x8000));
    /* BG2 (in front of the 3D): its last DS line green across, so on below
     * it to the picture's bottom (the DS's line 191: the picture's 215) */
    CHECK(TALL_PX(over, OX + 50, TOP + 191) == GREEN);
    CHECK(TALL_PX(over, OX + 50, TH - 1) == GREEN && (over[(TH - 1) * TW + OX + 50] & 0x8000));
    /* and its first line clear: nothing above it */
    CHECK(!(over[2 * TW + OX + 50] & 0x8000));
    /* the 3D on every line: nothing in front of it at the picture's top
     * left, the 3D under it there (under: nothing drawn, the backdrop) */
    CHECK(!(over[0] & 0x8000));

    /* a 32-tall OBJ below the DS's screen (y 210, its last 8 lines drawn):
     * its lines past 256 don't come round to the picture's top (the DS
     * would, its screen not that tall) */
    {
        static u8 tile12[13 * 32];

        memset(tile12, 0, sizeof(tile12));
        memset(tile12 + 12 * 32, 0x11, 32);   /* tile 12: the 32x32's bottom-left */
        GX_LoadOBJ(tile12, 0, sizeof(tile12));
        G2_SetOBJAttr(&oam[1], 200, 210, 0, GX_OAM_MODE_NORMAL, FALSE, GX_OAM_EFFECT_NONE, GX_OAM_SHAPE_32x32,
                      GX_OAM_COLORMODE_16, 0, 0, 0);
        DC_FlushRange(oam, sizeof(oam));
        GX_LoadOAM(oam, 0, sizeof(oam));
        NitroPort_Render2DSplit(0, under, &split);
        for (i = 0; i < 12; i++) {
            CHECK(!(over[i * TW + OX + 200] & 0x8000));
        }
        GX_LoadOBJ(objTile, 0, sizeof(objTile));
    }

    /* unclipped, BG1's map goes on above: its row 31 at the picture's 16-23 */
    split.clip[1] = 0;
    NitroPort_Render2DSplit(0, under, &split);
    CHECK(TALL_PX(over, OX, TOP - 4) == GREEN && (over[(TOP - 4) * TW + OX] & 0x8000));
#undef TALL_PX
}

static void SolidTile4(u8 *tile, int index)
{
    memset(tile, index | (index << 4), 32);
}

/* The game's framed windows on the wider canvas (NitroPort_WindowFramed):
 * a menu by the DS's right edge goes to the screen's right edge, its
 * sprites with it; one by the left edge to the left edge; one in the
 * middle stays; a text box across the DS's screen has its frame widened
 * to the screen's edges, the inside where the DS has it. And on the UI's
 * screens, a panel reaching the DS's edge has the background's stripes
 * carried on beside it. */
static void test_wide_framed_windows(void)
{
    static u16 wide[320 * 192], over[320 * 192];
    static u8 overAlpha[320 * 192];
    static u8 tiles[8 * 32], objTile[32];
    static u16 map[32 * 32];
    static u16 pltt[16] = { GREY, RED, BLUE, WHITE, GREEN };
    static u16 objPltt[16] = { 0, BLUE };
    static GXOamAttr oam[128];
    static struct { void *config; u8 layer, left, top, width, height; } menu = { NULL, 1, 26, 3, 4, 3 },
        leftMenu = { NULL, 1, 1, 10, 4, 2 }, middle = { NULL, 1, 12, 10, 6, 2 },
        box = { NULL, 1, 2, 19, 27, 4 };
    NitroRender2DSplit split;
    int i, x, y;

    Reset();
    memset(&split, 0, sizeof(split));
    split.width = 320;
    split.origin = 32;
    memset(split.clip, 1, sizeof(split.clip));
    split.anchorWindows = 1;
    split.over = over;
    split.overAlpha = overAlpha;
    NitroPort_WideForcePictures(0);
    SolidTile4(tiles + 1 * 32, 1);   /* red */
    SolidTile4(tiles + 2 * 32, 2);   /* blue */
    SolidTile4(tiles + 3 * 32, 3);   /* white */
    SolidTile4(tiles + 4 * 32, 4);   /* green */
    G2_SetBG1Control(GX_BG_SCRSIZE_TEXT_256x256, GX_BG_COLORMODE_16, GX_BG_SCRBASE_0x0000,
                     GX_BG_CHARBASE_0x04000, GX_BG_EXTPLTT_01);
    GX_LoadBG1Char(tiles, 0, sizeof(tiles));
    GX_LoadBGPltt(pltt, 0, sizeof(pltt));
    /* the menu with its frame: tiles 25-30, rows 2-6, red */
    memset(map, 0, sizeof(map));
    for (y = 2; y <= 6; y++) {
        for (x = 25; x <= 30; x++) {
            map[y * 32 + x] = 1;
        }
    }
    GX_LoadBG1Scr(map, 0, sizeof(map));
    /* a sprite over it (DS 208, 24) and one elsewhere (100, 24) */
    GX_SetOBJVRamModeChar(GX_OBJVRAMMODE_CHAR_1D_32K);
    SolidTile4(objTile, 1);
    GX_LoadOBJ(objTile, 0, sizeof(objTile));
    GX_LoadOBJPltt(objPltt, 0, sizeof(objPltt));
    MI_CpuFillFast(oam, 0x0200, sizeof(oam));
    G2_SetOBJAttr(&oam[0], 208, 24, 0, GX_OAM_MODE_NORMAL, FALSE, GX_OAM_EFFECT_NONE, GX_OAM_SHAPE_8x8,
                  GX_OAM_COLORMODE_16, 0, 0, 0);
    G2_SetOBJAttr(&oam[1], 100, 24, 0, GX_OAM_MODE_NORMAL, FALSE, GX_OAM_EFFECT_NONE, GX_OAM_SHAPE_8x8,
                  GX_OAM_COLORMODE_16, 0, 0, 0);
    DC_FlushRange(oam, sizeof(oam));
    GX_LoadOAM(oam, 0, sizeof(oam));
    GX_SetVisiblePlane(GX_PLANEMASK_BG1 | GX_PLANEMASK_OBJ);
    NitroPort_WindowAdded(&menu, 1, 26, 3, 4, 3);
    NitroPort_Render2DSplit(0, wide, &split);
    CHECK(wide[20 * 320 + 240] == RED);           /* no frame told: where the DS has it */
    NitroPort_WindowFramed(&menu, 1);
    NitroPort_Render2DSplit(0, wide, &split);
    CHECK(wide[20 * 320 + 264] == RED);           /* DS x 200-247 at the screen's right: 264-311 */
    CHECK(wide[20 * 320 + 311] == RED);
    CHECK(wide[20 * 320 + 240] == GREY);          /* where it was: the backdrop */
    CHECK(wide[20 * 320 + 315] == GREY);          /* (as far from the edge as from the DS's) */
    CHECK(wide[60 * 320 + 270] == GREY);          /* below it: nothing moved */
    CHECK(wide[26 * 320 + 274] == BLUE);          /* its sprite with it: 208 + 32 + 32 */
    CHECK(wide[26 * 320 + 242] == GREY);          /* (not where it was) */
    CHECK(wide[26 * 320 + 134] == BLUE);          /* the other sprite where it was: 100 + 32 */
    /* Drawn from the display state taken (the 3DS's drawing threads):
     * the windows as they were then too. The game takes the menu's frame
     * off before its tiles leave the VRAM (at its next V-blank): the menu
     * stays where it was until the state is taken again. */
    NitroPort_Render2DTakeState(0);
    NitroPort_WindowFramed(&menu, 0);
    split.snapshot = 1;
    NitroPort_Render2DSplit(0, wide, &split);
    CHECK(wide[20 * 320 + 264] == RED && wide[20 * 320 + 240] == GREY);
    CHECK(wide[26 * 320 + 274] == BLUE);
    NitroPort_Render2DTakeState(0);
    NitroPort_Render2DSplit(0, wide, &split);
    CHECK(wide[20 * 320 + 240] == RED && wide[20 * 320 + 300] == GREY);
    split.snapshot = 0;
    NitroPort_Render2DSplit(0, wide, &split);     /* the frame taken off: back */
    CHECK(wide[20 * 320 + 240] == RED);
    CHECK(wide[26 * 320 + 242] == BLUE);
    NitroPort_WindowRemoved(&menu);
    GX_SetVisiblePlane(GX_PLANEMASK_BG1);

    /* By the left edge (tiles 0-5 with its frame): to the screen's left
     * edge, as far from it as from the DS's. In the middle: stays. */
    memset(map, 0, sizeof(map));
    for (y = 9; y <= 12; y++) {
        for (x = 0; x <= 5; x++) {
            map[y * 32 + x] = 1;
        }
    }
    for (y = 9; y <= 12; y++) {
        for (x = 11; x <= 18; x++) {
            map[y * 32 + x] = 4;
        }
    }
    GX_LoadBG1Scr(map, 0, sizeof(map));
    NitroPort_WindowAdded(&leftMenu, 1, 1, 10, 4, 2);
    NitroPort_WindowFramed(&leftMenu, 1);
    NitroPort_WindowAdded(&middle, 1, 12, 10, 6, 2);
    NitroPort_WindowFramed(&middle, 1);
    NitroPort_Render2DSplit(0, wide, &split);
    CHECK(wide[80 * 320 + 0] == RED);             /* DS x 0-47 at 0-47 */
    CHECK(wide[80 * 320 + 47] == RED);
    CHECK(wide[80 * 320 + 60] == GREY);           /* where it was (32-79), past it */
    CHECK(wide[80 * 320 + 32 + 88] == GREEN);     /* the middle one where the DS has it */
    CHECK(wide[80 * 320 + 32 + 151] == GREEN);
    NitroPort_WindowRemoved(&leftMenu);
    NitroPort_WindowRemoved(&middle);

    /* A text box (tiles 2-28, rows 19-22; its frame: 2 tiles left, 3
     * right, a row above and below): the frame's left part (blue, white)
     * at the screen's left, its right part (white, blue, blue) at the
     * right, the inside (white, a red mark at its start) a pixel inside
     * the frame's left part (the room on the text's right, 2026-10-16),
     * and between, the frame's inner columns (white) drawn on. */
    memset(map, 0, sizeof(map));
    for (y = 18; y <= 23; y++) {
        for (x = 0; x < 32; x++) {
            map[y * 32 + x] = (x == 0 || x >= 30) ? 2 : (x == 2 && y == 20) ? 1 : 3;
        }
    }
    GX_LoadBG1Scr(map, 0, sizeof(map));
    NitroPort_WindowAdded(&box, 1, 2, 19, 27, 4);
    NitroPort_WindowFramed(&box, 2);
    NitroPort_Render2DSplit(0, wide, &split);
    y = 164;
    CHECK(wide[y * 320 + 3] == BLUE);             /* the frame's outer tile at the edge */
    CHECK(wide[y * 320 + 12] == WHITE);
    CHECK(wide[y * 320 + 16] == WHITE);           /* drawn on */
    CHECK(wide[y * 320 + 17] == RED);             /* the inside: DS x 16 at 17 */
    CHECK(wide[y * 320 + 24] == RED);
    CHECK(wide[y * 320 + 30] == WHITE);           /* (the inside, white) */
    CHECK(wide[y * 320 + 50] == WHITE);           /* where it was: DS x 16 at 48 (changed: was red, 2026-10-16) */
    CHECK(wide[y * 320 + 280] == WHITE);          /* drawn on */
    CHECK(wide[y * 320 + 299] == WHITE);          /* the right part: DS 232-255 at 296-319 */
    CHECK(wide[y * 320 + 310] == BLUE);
    CHECK(wide[y * 320 + 319] == BLUE);
    CHECK(wide[140 * 320 + 3] == GREY);           /* above the box: nothing */
    /* A frame whose right part starts with its own drawing (red, as the
     * frames with a coloured right end have it): between, the inside's
     * last column drawn on, white, not the red. */
    for (y = 18; y <= 23; y++) {
        map[y * 32 + 29] = 1;
    }
    GX_LoadBG1Scr(map, 0, sizeof(map));
    NitroPort_Render2DSplit(0, wide, &split);
    CHECK(wide[164 * 320 + 280] == WHITE);        /* drawn on: the inside's */
    CHECK(wide[164 * 320 + 295] == WHITE);
    CHECK(wide[164 * 320 + 299] == RED);          /* the right part's first tile: DS 232-239 at 296-303 */
    NitroPort_WindowRemoved(&box);

    /* The UI's band beside a panel: a background of stripes (lines red,
     * blue in turn) with a panel reaching the DS's left edge on lines
     * 40-79 (its first 4 pixels green, then white: not plain). Beside the
     * panel's lines the stripes go on. */
    {
        static u8 stripe[32], panel[32];

        for (i = 0; i < 32; i++) {
            stripe[i] = (u8)(((i / 4) & 1) ? 0x22 : 0x11);   /* rows: red, blue, red... */
            panel[i] = (u8)((i % 4) < 2 ? 0x44 : 0x33);      /* each row: green x4, white x4 */
        }
        memcpy(tiles + 5 * 32, stripe, 32);
        memcpy(tiles + 6 * 32, panel, 32);
        GX_LoadBG1Char(tiles, 0, sizeof(tiles));
        for (i = 0; i < 32 * 32; i++) {
            map[i] = ((i / 32) >= 5 && (i / 32) < 10 && (i % 32) < 4) ? 6 : 5;
        }
        GX_LoadBG1Scr(map, 0, sizeof(map));
        NitroPort_Render2DSplit(0, wide, &split);
        CHECK(wide[31 * 320 + 10] == BLUE);       /* a plain line (31: odd, blue) goes on */
        CHECK(wide[32 + 50 * 320] == GREEN);      /* the panel where the DS has it */
        CHECK(wide[50 * 320 + 10] == RED);        /* beside it, the stripes: line 50 red */
        CHECK(wide[51 * 320 + 10] == BLUE);       /* line 51 blue */
        CHECK(wide[79 * 320 + 31] == BLUE);
        NitroPort_WideForcePictures(1);           /* (the pictures' screens: not) */
        NitroPort_Render2DSplit(0, wide, &split);
        CHECK(wide[50 * 320 + 10] == GREY);
    }
    NitroPort_WideForcePictures(-1);
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

/* A screen laid out anew (NitroPort_WideSplitScreen, the trainer card):
 * its parts moved apart, the body's column drawn on between them, the
 * text's not; on an affine BG, through its matrix (the card turning over
 * stays laid out as it turns), its hand-drawn picture as well. */
static void test_wide_splits(void)
{
    static u16 wide[320 * 192], over[320 * 192], art[320 * 192], layerPx[320 * 192];
    static u8 overAlpha[320 * 192];
    static u8 tiles8[4 * 64], tiles4[4 * 32];
    static u8 amap[32 * 32];
    static u16 map[32 * 32];
    static u16 pltt[256];
    /* the body (BG2) split at 152, 31 each way, its column drawn on
     * between, on lines 0-95; the text (BG1) the same, nothing between */
    static const short body[][7] = { { 1 << 2, 0, 96, 152, -31, 31, 1 } };
    static const short text[][7] = { { 1 << 1, 0, 96, 152, -31, 31, 0 }, { 0, 0, 96, 0, 0, 0, 0 } };
    static const short textFill[][7] = { { 1 << 1, 0, 96, 152, -31, 31, 1 } };
    NitroWideSplit got[NITRO_WIDE_MAX_SPLITS];
    char dir[] = "/tmp/widesplitXXXXXX";
    NitroRender2DSplit split;
    MtxFx22 m;
    uint64_t key;
    int hofs, vofs, prio, i, x, y;

    /* what the game says is what the renderer gets: rows without BGs left out */
    NitroPort_WideSplitScreen(0, &text[0][0], 2);
    CHECK(NitroPort_WideSplits(0, got) == 1 && got[0].bgs == 2 && got[0].top == 0 && got[0].bottom == 96 &&
          got[0].x == 152 && got[0].left == -31 && got[0].right == 31 && got[0].fill == 0);
    CHECK(NitroPort_WideSplits(1, got) == 0);   /* the other engine's: as the DS has it */
    NitroPort_WideSplitScreen(0, NULL, 0);
    CHECK(NitroPort_WideSplits(0, got) == 0);

    Reset();
    GX_SetGraphicsMode(GX_DISPMODE_GRAPHICS, GX_BGMODE_2, GX_BG0_AS_2D);
    memset(&split, 0, sizeof(split));
    split.width = 320;
    split.origin = 32;
    memset(split.clip, 1, sizeof(split.clip));
    split.anchorWindows = 1;
    split.over = over;
    split.overAlpha = overAlpha;
    NitroPort_WideForcePictures(0);
    memset(pltt, 0, sizeof(pltt));
    pltt[0] = GREY;
    pltt[1] = RED;
    pltt[2] = BLUE;
    pltt[3] = WHITE;
    pltt[4] = GREEN;
    GX_LoadBGPltt(pltt, 0, sizeof(pltt));
    /* BG2, affine 256x256: red columns 0-18 (x 0-151), white 19 (152-159),
     * blue 20-31 (160-255); a green tile at column 1 */
    memset(tiles8, 0, sizeof(tiles8));
    for (i = 0; i < 64; i++) {
        tiles8[1 * 64 + i] = 1;
        tiles8[2 * 64 + i] = 2;
        tiles8[3 * 64 + i] = 3;
    }
    for (i = 0; i < 32 * 32; i++) {
        x = i % 32;
        amap[i] = (u8)(x < 19 ? 1 : x == 19 ? 3 : 2);
    }
    G2_SetBG2ControlAffine(GX_BG_SCRSIZE_AFFINE_256x256, GX_BG_AREAOVER_XLU, GX_BG_SCRBASE_0x0000,
                           GX_BG_CHARBASE_0x04000);
    GX_LoadBG2Char(tiles8, 0, sizeof(tiles8));
    GX_LoadBG2Scr(amap, 0, sizeof(amap));
    MTX_Identity22(&m);
    G2_SetBG2Affine(&m, 128, 96, 0, 0);
    GX_SetVisiblePlane(GX_PLANEMASK_BG2);
    NitroPort_WideSplitScreen(0, &body[0][0], 1);
    NitroPort_Render2DSplit(0, wide, &split);
    y = 40;
    CHECK(wide[y * 320 + 0] == GREY);     /* DS x -1: nothing (from beyond the DS's screen) */
    CHECK(wide[y * 320 + 1] == RED);      /* DS x 0, 31 to the left */
    CHECK(wide[y * 320 + 152] == RED);    /* DS x 151 */
    CHECK(wide[y * 320 + 153] == WHITE);  /* between: column 152 drawn on */
    CHECK(wide[y * 320 + 214] == WHITE);
    CHECK(wide[y * 320 + 222] == WHITE);  /* DS x 159, 31 to the right */
    CHECK(wide[y * 320 + 223] == BLUE);   /* DS x 160 */
    CHECK(wide[y * 320 + 318] == BLUE);   /* DS x 255 */
    CHECK(wide[y * 320 + 319] == GREY);
    y = 120;                               /* (below the split: where the DS has it, nothing beside) */
    CHECK(wide[y * 320 + 31] == GREY && wide[y * 320 + 32] == RED);
    CHECK(wide[y * 320 + 287] == BLUE && wide[y * 320 + 288] == GREY);
    /* Turning over: half as wide about the DS's middle (x 128), the card
     * laid out as it was, narrowed: DS x -31..286 of it on 48.5..207 */
    MTX_Scale22(&m, FX32_ONE * 2, FX32_ONE);
    G2_SetBG2Affine(&m, 128, 96, 0, 0);
    NitroPort_Render2DSplit(0, wide, &split);
    y = 40;
    CHECK(wide[y * 320 + 32 + 48] == GREY);
    CHECK(wide[y * 320 + 32 + 49] == RED);
    CHECK(wide[y * 320 + 32 + 128] == WHITE);   /* its widened middle */
    CHECK(wide[y * 320 + 32 + 207] == BLUE);
    CHECK(wide[y * 320 + 32 + 208] == GREY);
    /* its hand-drawn picture (made at rest, across the canvas), through the
     * matrix as well: green, its left column blue */
    MTX_Identity22(&m);
    G2_SetBG2Affine(&m, 128, 96, 0, 0);
    CHECK(NitroPort_Render2DLayer(0, 2, &split, layerPx, &key, &hofs, &vofs, &prio));
    CHECK(mkdtemp(dir) != NULL);
    for (i = 0; i < 320 * 192; i++) {
        art[i] = (u16)((i % 320 == 0 ? BLUE : GREEN) | 0x8000);
    }
    WriteArt(dir, key, 0, 0, 1, art);
    CHECK(NitroPort_WideArtLoad(dir) == 1);
    NitroPort_Render2DSplit(0, wide, &split);
    CHECK(wide[40 * 320 + 0] == BLUE && wide[40 * 320 + 200] == GREEN && wide[40 * 320 + 319] == GREEN);
    MTX_Scale22(&m, FX32_ONE * 2, FX32_ONE);
    G2_SetBG2Affine(&m, 128, 96, 0, 0);
    NitroPort_Render2DSplit(0, wide, &split);
    CHECK(wide[40 * 320 + 32 + 47] == GREY);    /* (the picture's x -2: none) */
    CHECK(wide[40 * 320 + 32 + 48] == BLUE);    /* its left column, where the card's left edge went */
    CHECK(wide[40 * 320 + 32 + 128] == GREEN);
    CHECK(wide[40 * 320 + 32 + 208] == GREY);
    NitroPort_WideArtLoad("/nonexistent");
    MTX_Identity22(&m);
    G2_SetBG2Affine(&m, 128, 96, 0, 0);
    /* as the DS has it again */
    NitroPort_WideSplitScreen(0, NULL, 0);
    NitroPort_Render2DSplit(0, wide, &split);
    CHECK(wide[40 * 320 + 153] == RED && wide[40 * 320 + 184] == WHITE && wide[40 * 320 + 287] == BLUE);

    /* BG1, a text BG: the same columns (4 bits a pixel); green at column 1 */
    GX_SetGraphicsMode(GX_DISPMODE_GRAPHICS, GX_BGMODE_0, GX_BG0_AS_2D);
    memset(tiles4, 0, sizeof(tiles4));
    SolidTile4(tiles4 + 1 * 32, 1);
    SolidTile4(tiles4 + 2 * 32, 2);
    SolidTile4(tiles4 + 3 * 32, 3);
    for (i = 0; i < 32 * 32; i++) {
        x = i % 32;
        map[i] = (u16)(x < 19 ? 1 : x == 19 ? 3 : 2);
    }
    for (y = 0; y < 32; y++) {
        map[y * 32 + 1] = 0;   /* column 1 (x 8-15): nothing */
    }
    G2_SetBG1Control(GX_BG_SCRSIZE_TEXT_256x256, GX_BG_COLORMODE_16, GX_BG_SCRBASE_0x0800,
                     GX_BG_CHARBASE_0x08000, GX_BG_EXTPLTT_01);
    GX_LoadBG1Char(tiles4, 0, sizeof(tiles4));
    GX_LoadBG1Scr(map, 0, sizeof(map));
    GX_SetVisiblePlane(GX_PLANEMASK_BG1);
    NitroPort_WideSplitScreen(0, &text[0][0], 2);
    NitroPort_Render2DSplit(0, wide, &split);
    y = 40;
    CHECK(wide[y * 320 + 0] == GREY);     /* not the map's other side (it wraps) */
    CHECK(wide[y * 320 + 1] == RED);
    CHECK(wide[y * 320 + 9] == GREY);     /* its column 1 */
    CHECK(wide[y * 320 + 152] == RED);
    CHECK(wide[y * 320 + 153] == GREY);   /* between: nothing */
    CHECK(wide[y * 320 + 214] == GREY);
    CHECK(wide[y * 320 + 215] == WHITE);
    CHECK(wide[y * 320 + 223] == BLUE);
    CHECK(wide[y * 320 + 318] == BLUE);
    CHECK(wide[y * 320 + 319] == GREY);
    y = 120;
    CHECK(wide[y * 320 + 31] == GREY && wide[y * 320 + 32] == RED && wide[y * 320 + 287] == BLUE &&
          wide[y * 320 + 288] == GREY);
    /* scrolled 8 to the left: its map's columns go with it (the split is
     * of the map: column 152 still) */
    G2_SetBG1Offset(8, 0);
    NitroPort_Render2DSplit(0, wide, &split);
    y = 40;
    CHECK(wide[y * 320 + 1] == GREY);     /* map x 8: column 1, nothing */
    CHECK(wide[y * 320 + 9] == RED);      /* map x 16 */
    CHECK(wide[y * 320 + 144] == RED);    /* map x 151 */
    CHECK(wide[y * 320 + 145] == GREY);
    CHECK(wide[y * 320 + 206] == GREY);
    CHECK(wide[y * 320 + 207] == WHITE);  /* map x 152 */
    CHECK(wide[y * 320 + 310] == BLUE);   /* map x 255 */
    CHECK(wide[y * 320 + 311] == RED);    /* map x 256: round to 0, as on the DS's screen (its x 248) */
    CHECK(wide[y * 320 + 319] == GREY);   /* the DS's x 256: beyond its screen */
    G2_SetBG1Offset(0, 0);
    /* its column drawn on between */
    NitroPort_WideSplitScreen(0, &textFill[0][0], 1);
    NitroPort_Render2DSplit(0, wide, &split);
    CHECK(wide[40 * 320 + 153] == WHITE && wide[40 * 320 + 214] == WHITE);
    NitroPort_WideSplitScreen(0, NULL, 0);
    NitroPort_Render2DSplit(0, wide, &split);
    CHECK(wide[40 * 320 + 152] == RED && wide[40 * 320 + 184] == WHITE && wide[40 * 320 + 223] == BLUE);
    NitroPort_WideForcePictures(-1);
}

/* A screen laid out in parts (NitroPort_WideScreenParts): the DS's picture
 * where it is but for the columns MOVEs take, each part drawn over it in
 * turn (a later one over an earlier), only what the parts put there beside
 * the DS's screen; columns of the BG's picture, or of the DS's screen
 * (NITRO_WIDE_SCREEN_COLUMNS) as it scrolls; the sprites moved (and
 * stretched back to where they were), the 3D moved; a moved BG's
 * hand-drawn picture left out. */
static void test_wide_parts(void)
{
    static u16 wide[320 * 192], over[320 * 192], art[320 * 192], layerPx[320 * 192];
    static u8 overAlpha[320 * 192];
    static u8 tiles4[4 * 32], objTiles[32];
    static u16 map[32 * 32];
    static u16 pltt[16], objPltt[16] = { 0, BLUE, GREEN };
    static GXOamAttr oam[128];
    /* (kind, bgs, top, bottom, x0, x1, dx, col) */
    static const short bad[][8] = {
        { 0, 2, 0, 96, 0, 64, 5, 0 },                                     /* no such kind */
        { NITRO_WIDE_MOVE, 0, 0, 96, 0, 64, 5, 0 },                       /* no BGs */
        { NITRO_WIDE_FILL, 2, 96, 96, 0, 64, 0, 3 },                      /* no lines */
        { NITRO_WIDE_OBJS, 0, 0, 96, 64, 64, 5, 0 },                      /* no columns */
        { NITRO_WIDE_MOVE, 2 | NITRO_WIDE_SCREEN_COLUMNS, 0, 96, 0, 64, -32, 0 },
        { NITRO_WIDE_SHIFT3D, 0, 0, 0, 0, 0, -17, 0 },                    /* (no lines or columns: kept) */
    };
    /* BG1: columns 0-63 to the left edge, 200-255 to the right edge, the
     * white column (152) drawn on across first (under them) */
    static const short apart[][8] = {
        { NITRO_WIDE_FILL, 2, 0, 96, -32, 288, 0, 152 },
        { NITRO_WIDE_MOVE, 2, 0, 96, 0, 64, -32, 0 },
        { NITRO_WIDE_MOVE, 2, 0, 96, 200, 256, 32, 0 },
    };
    static const short apartBare[][8] = {
        { NITRO_WIDE_MOVE, 2, 0, 96, 0, 64, -32, 0 },
        { NITRO_WIDE_MOVE, 2, 0, 96, 200, 256, 32, 0 },
    };
    /* the white column (152-159) over the red moved right of it, and the
     * other way round */
    static const short whiteOver[][8] = {
        { NITRO_WIDE_MOVE, 2, 0, 96, 0, 128, 16, 0 },
        { NITRO_WIDE_MOVE, 2, 0, 96, 152, 160, -40, 0 },
    };
    static const short redOver[][8] = {
        { NITRO_WIDE_MOVE, 2, 0, 96, 152, 160, -40, 0 },
        { NITRO_WIDE_MOVE, 2, 0, 96, 0, 128, 16, 0 },
    };
    /* the DS's screen's columns 144-151, 100 to the right; beside the left
     * edge, the picture's white column */
    static const short onScreen[][8] = {
        { NITRO_WIDE_FILL, 2 | NITRO_WIDE_SCREEN_COLUMNS, 0, 96, -32, 0, 0, 152 },
        { NITRO_WIDE_MOVE, 2 | NITRO_WIDE_SCREEN_COLUMNS, 0, 96, 144, 152, 100, 0 },
    };
    static const short ofPicture[][8] = {
        { NITRO_WIDE_MOVE, 2, 0, 96, 144, 152, 100, 0 },
    };
    static const short objs[][8] = {
        { NITRO_WIDE_OBJS, 0, 0, 96, 64, 128, 50, 0 },
        { NITRO_WIDE_SHIFT3D, 0, 0, 0, 0, 0, -17, 0 },
    };
    static const short objsStretch[][8] = {
        { NITRO_WIDE_OBJS, 0, 0, 96, 64, 128, 50, 1 },
    };
    NitroWidePart got[NITRO_WIDE_MAX_PARTS];
    char dir[] = "/tmp/widepartsXXXXXX";
    NitroRender2DSplit split;
    uint64_t key;
    int hofs, vofs, prio, i, x, y, n;

    /* what the game says is what the renderer gets: rows that say nothing
     * left out */
    NitroPort_WideScreenParts(0, &bad[0][0], (int)(sizeof(bad) / sizeof(bad[0])));
    n = NitroPort_WideParts(0, got);
    CHECK(n == 2);
    CHECK(got[0].kind == NITRO_WIDE_MOVE && got[0].bgs == (2 | NITRO_WIDE_SCREEN_COLUMNS) && got[0].top == 0 &&
          got[0].bottom == 96 && got[0].x0 == 0 && got[0].x1 == 64 && got[0].dx == -32);
    CHECK(got[1].kind == NITRO_WIDE_SHIFT3D && got[1].dx == -17);
    CHECK(NitroPort_WideParts(1, got) == 0);   /* the other engine's: as the DS has it */
    NitroPort_WideScreenParts(0, NULL, 0);
    CHECK(NitroPort_WideParts(0, got) == 0);

    Reset();
    memset(&split, 0, sizeof(split));
    split.width = 320;
    split.origin = 32;
    memset(split.clip, 1, sizeof(split.clip));
    split.anchorWindows = 1;
    split.over = over;
    split.overAlpha = overAlpha;
    NitroPort_WideForcePictures(0);
    memset(pltt, 0, sizeof(pltt));
    pltt[0] = GREY;
    pltt[1] = RED;
    pltt[2] = BLUE;
    pltt[3] = WHITE;
    GX_LoadBGPltt(pltt, 0, sizeof(pltt));
    /* BG1, a text BG: red columns 0-18 (x 0-151), white 19 (152-159), blue
     * 20-31 (160-255); column 1 (x 8-15) empty */
    memset(tiles4, 0, sizeof(tiles4));
    SolidTile4(tiles4 + 1 * 32, 1);
    SolidTile4(tiles4 + 2 * 32, 2);
    SolidTile4(tiles4 + 3 * 32, 3);
    for (i = 0; i < 32 * 32; i++) {
        x = i % 32;
        map[i] = (u16)(x == 1 ? 0 : x < 19 ? 1 : x == 19 ? 3 : 2);
    }
    G2_SetBG1Control(GX_BG_SCRSIZE_TEXT_256x256, GX_BG_COLORMODE_16, GX_BG_SCRBASE_0x0800,
                     GX_BG_CHARBASE_0x08000, GX_BG_EXTPLTT_01);
    GX_LoadBG1Char(tiles4, 0, sizeof(tiles4));
    GX_LoadBG1Scr(map, 0, sizeof(map));
    GX_SetVisiblePlane(GX_PLANEMASK_BG1);

    NitroPort_WideScreenParts(0, &apart[0][0], 3);
    NitroPort_Render2DSplit(0, wide, &split);
    y = 40;
    CHECK(wide[y * 320 + 0] == RED);      /* DS x 0, 32 to the left */
    CHECK(wide[y * 320 + 8] == GREY);     /* DS x 8 (nothing there): nothing (not the fill: a
                                           * part has its columns, what's clear in it clear) */
    CHECK(wide[y * 320 + 63] == RED);     /* DS x 63 */
    CHECK(wide[y * 320 + 64] == WHITE);   /* the fill (over the DS's picture where it was) */
    CHECK(wide[y * 320 + 150] == WHITE);
    CHECK(wide[y * 320 + 263] == WHITE);
    CHECK(wide[y * 320 + 264] == BLUE);   /* DS x 200, 32 to the right */
    CHECK(wide[y * 320 + 319] == BLUE);   /* DS x 255 */
    y = 120;                               /* (no parts: where the DS has it, nothing beside) */
    CHECK(wide[y * 320 + 31] == GREY && wide[y * 320 + 32] == RED && wide[y * 320 + 287] == BLUE &&
          wide[y * 320 + 288] == GREY);
    /* without the fill: the DS's picture where it was, but for what moved */
    NitroPort_WideScreenParts(0, &apartBare[0][0], 2);
    NitroPort_Render2DSplit(0, wide, &split);
    y = 40;
    CHECK(wide[y * 320 + 8] == GREY);     /* DS x 8: nothing */
    CHECK(wide[y * 320 + 63] == RED);
    CHECK(wide[y * 320 + 64] == GREY);    /* DS x 32-63: gone to the left */
    CHECK(wide[y * 320 + 95] == GREY);
    CHECK(wide[y * 320 + 96] == RED);     /* DS x 64, where it is */
    CHECK(wide[y * 320 + 184] == WHITE);
    CHECK(wide[y * 320 + 231] == BLUE);   /* DS x 199 */
    CHECK(wide[y * 320 + 232] == GREY);   /* DS x 200-231: gone to the right */
    CHECK(wide[y * 320 + 263] == GREY);
    CHECK(wide[y * 320 + 264] == BLUE);

    /* a later part over an earlier one */
    NitroPort_WideScreenParts(0, &whiteOver[0][0], 2);
    NitroPort_Render2DSplit(0, wide, &split);
    CHECK(wide[40 * 320 + 143] == RED);   /* DS x 95, 16 to the right */
    CHECK(wide[40 * 320 + 144] == WHITE); /* DS x 152, 40 to the left: over the red */
    CHECK(wide[40 * 320 + 151] == WHITE);
    CHECK(wide[40 * 320 + 152] == RED);   /* DS x 104 */
    CHECK(wide[40 * 320 + 175] == RED);   /* DS x 127 */
    CHECK(wide[40 * 320 + 176] == RED);   /* DS x 144-151, where they are */
    CHECK(wide[40 * 320 + 184] == GREY);  /* DS x 152-159: moved */
    CHECK(wide[40 * 320 + 192] == BLUE);  /* DS x 160 */
    NitroPort_WideScreenParts(0, &redOver[0][0], 2);
    NitroPort_Render2DSplit(0, wide, &split);
    CHECK(wide[40 * 320 + 144] == RED && wide[40 * 320 + 151] == RED);

    /* columns of the DS's screen, scrolled 8 to the left: what is at its
     * x 144-151 (the white column) moves; of the picture, its x 144-151
     * (red, at the DS's x 136-143) */
    G2_SetBG1Offset(8, 0);
    NitroPort_WideScreenParts(0, &onScreen[0][0], 2);
    NitroPort_Render2DSplit(0, wide, &split);
    y = 40;
    CHECK(wide[y * 320 + 0] == WHITE && wide[y * 320 + 31] == WHITE);   /* the picture's column 152 */
    CHECK(wide[y * 320 + 175] == RED);    /* the DS's x 143 */
    CHECK(wide[y * 320 + 176] == GREY);   /* x 144-151: moved */
    CHECK(wide[y * 320 + 183] == GREY);
    CHECK(wide[y * 320 + 184] == BLUE);   /* x 152 (the picture's 160) */
    CHECK(wide[y * 320 + 276] == WHITE && wide[y * 320 + 283] == WHITE);
    NitroPort_WideScreenParts(0, &ofPicture[0][0], 1);
    NitroPort_Render2DSplit(0, wide, &split);
    CHECK(wide[y * 320 + 31] == GREY);    /* (nothing beside but the part) */
    CHECK(wide[y * 320 + 167] == RED);    /* the DS's x 135 */
    CHECK(wide[y * 320 + 168] == GREY);   /* x 136-143: moved */
    CHECK(wide[y * 320 + 176] == WHITE);  /* x 144: where it is */
    CHECK(wide[y * 320 + 268] == RED && wide[y * 320 + 275] == RED);
    /* the map going on beside the DS's screen on lines 0-95 (SHOW), as it's
     * scrolled (8: the canvas's x 0 is the map's -24, round to 232): blue,
     * then the red of its start; on the right its x 264 on, round to 8: the
     * empty column, then red. Its other lines: nothing beside. */
    {
        static const short show[][8] = {
            { NITRO_WIDE_SHOW, 2, 0, 96, 0, 1, 0, 0 },
        };

        NitroPort_WideScreenParts(0, &show[0][0], 1);
        CHECK(NitroPort_WideParts(0, got) == 1 && got[0].kind == NITRO_WIDE_SHOW);
        NitroPort_Render2DSplit(0, wide, &split);
        CHECK(wide[y * 320 + 0] == BLUE && wide[y * 320 + 23] == BLUE);
        CHECK(wide[y * 320 + 24] == RED && wide[y * 320 + 31] == RED);
        CHECK(wide[y * 320 + 32] == GREY);    /* the DS's x 0: the map's 8, the empty column */
        CHECK(wide[y * 320 + 40] == RED);
        CHECK(wide[y * 320 + 176] == WHITE);  /* the DS's x 144: the map's 152, white */
        CHECK(wide[y * 320 + 288] == GREY && wide[y * 320 + 295] == GREY);
        CHECK(wide[y * 320 + 296] == RED && wide[y * 320 + 319] == RED);
        CHECK(wide[120 * 320 + 0] == GREY && wide[120 * 320 + 319] == GREY);
    }
    G2_SetBG1Offset(0, 0);

    /* a moved BG shows its own pixels, not a hand-drawn picture of it made
     * elsewhere (8 to the right: the parts' columns are another picture's
     * there) */
    NitroPort_WideScreenParts(0, NULL, 0);
    CHECK(NitroPort_Render2DLayer(0, 1, &split, layerPx, &key, &hofs, &vofs, &prio));
    CHECK(mkdtemp(dir) != NULL);
    for (i = 0; i < 320 * 192; i++) {
        art[i] = (u16)(GREEN | 0x8000);
    }
    WriteArt(dir, key, 8, 0, 1, art);
    CHECK(NitroPort_WideArtLoad(dir) == 1);
    NitroPort_Render2DSplit(0, wide, &split);
    CHECK(wide[40 * 320 + 100] == GREEN);
    NitroPort_WideScreenParts(0, &apart[0][0], 3);
    NitroPort_Render2DSplit(0, wide, &split);
    CHECK(wide[40 * 320 + 0] == RED && wide[40 * 320 + 100] == WHITE && wide[120 * 320 + 100] == RED);
    /* one made right where it is (a capture of the screen as its parts lay
     * it out, drawn on) is the layer, all of it; one of its bands only: the
     * bands, and the parts over the DS's part */
    {
        char made[512];

        WriteArt(dir, key, 0, 0, 1, art);
        CHECK(NitroPort_WideArtLoad(dir) == 2);
        NitroPort_Render2DSplit(0, wide, &split);
        CHECK(wide[40 * 320 + 0] == GREEN && wide[40 * 320 + 100] == GREEN && wide[120 * 320 + 100] == GREEN);
        /* NO_ART: that BG without its picture, the parts as they are */
        {
            static const short noArt[][8] = {
                { NITRO_WIDE_NO_ART, 2, 0, 1, 0, 1, 0, 0 },
            };
            short withNoArt[4][8];

            memcpy(withNoArt, apart, sizeof(apart));
            memcpy(withNoArt[3], noArt[0], sizeof(noArt[0]));
            NitroPort_WideScreenParts(0, &withNoArt[0][0], 4);
            NitroPort_Render2DSplit(0, wide, &split);
            CHECK(wide[40 * 320 + 0] == RED && wide[40 * 320 + 100] == WHITE && wide[120 * 320 + 100] == RED);
            NitroPort_WideScreenParts(0, &apart[0][0], 3);
        }
        snprintf(made, sizeof(made), "%s/%016llx_0_0_full.png", dir, (unsigned long long)key);
        CHECK(remove(made) == 0);
        WriteArt(dir, key, 0, 0, 0, art);
        CHECK(NitroPort_WideArtLoad(dir) == 2);
        NitroPort_Render2DSplit(0, wide, &split);
        CHECK(wide[40 * 320 + 0] == GREEN && wide[40 * 320 + 319] == GREEN);   /* (the bands) */
        CHECK(wide[40 * 320 + 32] == RED && wide[40 * 320 + 63] == RED);       /* (the parts: moved, */
        CHECK(wide[40 * 320 + 64] == WHITE && wide[40 * 320 + 263] == WHITE);  /* the fill) */
        CHECK(wide[120 * 320 + 100] == RED);
        CHECK(wide[120 * 320 + 0] == GREEN);
        snprintf(made, sizeof(made), "%s/%016llx_0_0.png", dir, (unsigned long long)key);
        CHECK(remove(made) == 0);
        snprintf(made, sizeof(made), "%s/%016llx_8_0_full.png", dir, (unsigned long long)key);
        CHECK(remove(made) == 0);
        NitroPort_WideArtLoad("/nonexistent");
        CHECK(rmdir(dir) == 0);
    }

    /* colours shown clear (CLEAR: a panel's background drawn into its
     * edges): BG1's red (palette 0, index 1), all over it; the white and
     * the blue as they are; a palette without that bit as it is */
    {
        static const short clear[][8] = {
            { NITRO_WIDE_CLEAR, 2, 0, 0, 0, 0, 0, 1 << 1 },
        };
        static const short clearOther[][8] = {
            { NITRO_WIDE_CLEAR, 2, 0, 0, 0, 0, 1, 1 << 1 },   /* (palette 1: not BG1's) */
        };

        NitroPort_WideScreenParts(0, &clear[0][0], 1);
        NitroPort_Render2DSplit(0, wide, &split);
        CHECK(wide[40 * 320 + 72] == GREY && wide[120 * 320 + 32] == GREY);   /* (the backdrop) */
        CHECK(wide[40 * 320 + 184] == WHITE && wide[40 * 320 + 232] == BLUE);
        NitroPort_WideScreenParts(0, &clearOther[0][0], 1);
        NitroPort_Render2DSplit(0, wide, &split);
        CHECK(wide[40 * 320 + 72] == RED);
        NitroPort_WideScreenParts(0, NULL, 0);
    }

    /* a picture drawn for the layout (NITRO_WIDE_PICTURE, a file in the
     * folder): in front of its BG's pixels, where it's put, turned about
     * its middle; nothing while its file isn't there */
    {
        /* 4 x 2: red red blue blue / green green white white */
        static const u8 rgba[8 * 4] = {
            255, 0, 0, 255, 255, 0, 0, 255, 0, 0, 255, 255, 0, 0, 255, 255,
            0, 255, 0, 255, 0, 255, 0, 255, 255, 255, 255, 255, 255, 255, 255, 255,
        };
        static const short picture[][8] = {
            { NITRO_WIDE_PICTURE, 2, 40, 41, 100, 101, 0, NITRO_WIDE_PICTURE_PC_BOXES_BALL },
        };
        static const short turned[][8] = {
            { NITRO_WIDE_PICTURE, 2, 40, 41, 100, 101, 90, NITRO_WIDE_PICTURE_PC_BOXES_BALL },
        };
        char path[512], dir2[] = "/tmp/widepicturesXXXXXX";
        int w, h, dx, dy;

        NitroPort_WideArtLoad("/nonexistent");
        CHECK(mkdtemp(dir2) != NULL);
        NitroPort_WideScreenParts(0, &picture[0][0], 1);
        NitroPort_Render2DSplit(0, wide, &split);
        CHECK(wide[40 * 320 + 132] == RED);    /* (no file: BG1, the DS's x 100) */
        snprintf(path, sizeof(path), "%s/%s.png", dir2, NitroPort_WidePictureName(NITRO_WIDE_PICTURE_PC_BOXES_BALL));
        CHECK(NitroPort_WideArtWritePng(path, rgba, 4, 2) == 0);
        CHECK(NitroPort_WideArtLoad(dir2) == 0);   /* (no layer's pictures) */
        CHECK(NitroPort_WideArtPicture(NITRO_WIDE_PICTURE_PC_BOXES_BALL, &w, &h, &dx, &dy) != NULL && w == 4 &&
              h == 2 && dx == 0 && dy == 0);
        CHECK(NitroPort_WideArtPicture(NITRO_WIDE_PICTURE_PARTY_PANEL, &w, &h, &dx, &dy) == NULL);
        NitroPort_Render2DSplit(0, wide, &split);
        CHECK(wide[40 * 320 + 131] == RED);    /* (BG1) */
        CHECK(wide[40 * 320 + 132] == RED && wide[40 * 320 + 134] == BLUE && wide[40 * 320 + 135] == BLUE);
        CHECK(wide[41 * 320 + 132] == GREEN && wide[41 * 320 + 135] == WHITE);
        CHECK(wide[40 * 320 + 136] == RED && wide[42 * 320 + 132] == RED);   /* (BG1 again) */
        /* (what a capture lists of it: which, with which BG, where, how big, how far turned) */
        {
            NitroWidePictureShown shown[4];

            CHECK(NitroPort_Render2DPictures(0, &split, shown, 4) == 1);
            CHECK(shown[0].picture == NITRO_WIDE_PICTURE_PC_BOXES_BALL && shown[0].bg == 1 && shown[0].x == 132 &&
                  shown[0].y == 40 && shown[0].w == 4 && shown[0].h == 2 && shown[0].angle == 0);
        }
        /* turned a quarter clockwise: its left column on top */
        NitroPort_WideScreenParts(0, &turned[0][0], 1);
        NitroPort_Render2DSplit(0, wide, &split);
        CHECK(wide[39 * 320 + 133] == GREEN && wide[39 * 320 + 134] == RED);
        CHECK(wide[40 * 320 + 133] == GREEN && wide[40 * 320 + 134] == RED);
        CHECK(wide[41 * 320 + 133] == WHITE && wide[41 * 320 + 134] == BLUE);
        CHECK(wide[42 * 320 + 133] == WHITE && wide[42 * 320 + 134] == BLUE);
        CHECK(wide[40 * 320 + 132] == RED && wide[40 * 320 + 135] == RED);   /* (BG1 beside it) */
        /* the same layout again: nothing new to draw */
        {
            uint32_t serial = NitroPort_WideWindowsSerial();

            NitroPort_WideScreenParts(0, &turned[0][0], 1);
            CHECK(NitroPort_WideWindowsSerial() == serial);
            NitroPort_WideScreenParts(0, &picture[0][0], 1);
            CHECK(NitroPort_WideWindowsSerial() != serial);
        }
        /* drawn 2 right and 1 down in its file (a clear margin left and
         * above): drawn there, turned about the middle of what's drawn */
        {
            static u8 margin[6 * 3 * 4];

            memset(margin, 0, sizeof(margin));
            for (y = 0; y < 2; y++) {
                memcpy(margin + ((y + 1) * 6 + 2) * 4, rgba + y * 4 * 4, 4 * 4);
            }
            CHECK(NitroPort_WideArtWritePng(path, margin, 6, 3) == 0);
            NitroPort_WideArtLoad(dir2);
            CHECK(NitroPort_WideArtPicture(NITRO_WIDE_PICTURE_PC_BOXES_BALL, &w, &h, &dx, &dy) != NULL && w == 4 &&
                  h == 2 && dx == 2 && dy == 1);
            NitroPort_WideScreenParts(0, &picture[0][0], 1);
            NitroPort_Render2DSplit(0, wide, &split);
            CHECK(wide[41 * 320 + 136] == BLUE && wide[41 * 320 + 137] == BLUE);
            CHECK(wide[42 * 320 + 134] == GREEN && wide[42 * 320 + 137] == WHITE);
            CHECK(wide[40 * 320 + 134] == RED && wide[43 * 320 + 134] == RED);   /* (BG1) */
            NitroPort_WideScreenParts(0, &turned[0][0], 1);
            NitroPort_Render2DSplit(0, wide, &split);
            CHECK(wide[40 * 320 + 135] == GREEN && wide[41 * 320 + 135] == GREEN);
            CHECK(wide[42 * 320 + 135] == WHITE && wide[43 * 320 + 136] == BLUE);
        }
        /* its file's offset (the editor's, for a picture moved there): 3
         * left, 5 down */
        {
            NitroWidePictureShown shown[4];

            CHECK(NitroPort_WideArtWritePng(path, rgba, 4, 2) == 0);
            AddPngOffset(path, -3, 5);
            NitroPort_WideArtLoad(dir2);
            CHECK(NitroPort_WideArtPicture(NITRO_WIDE_PICTURE_PC_BOXES_BALL, &w, &h, &dx, &dy) != NULL && w == 4 &&
                  h == 2 && dx == -3 && dy == 5);
            NitroPort_WideScreenParts(0, &picture[0][0], 1);
            NitroPort_Render2DSplit(0, wide, &split);
            CHECK(wide[45 * 320 + 131] == BLUE && wide[46 * 320 + 129] == GREEN && wide[46 * 320 + 132] == WHITE);
            CHECK(wide[40 * 320 + 134] == RED);   /* (BG1 where it was) */
            CHECK(NitroPort_Render2DPictures(0, &split, shown, 4) == 1);
            CHECK(shown[0].x == 129 && shown[0].y == 45 && shown[0].w == 4 && shown[0].h == 2);
        }
        NitroPort_WideScreenParts(0, NULL, 0);
        remove(path);
        rmdir(dir2);
    }
    NitroPort_WideArtLoad("/nonexistent");

    /* sprites: the one whose middle is in x 64-127, lines 0-95, 50 to the
     * right; the others where they are */
    GX_SetOBJVRamModeChar(GX_OBJVRAMMODE_CHAR_1D_32K);
    MakeTile4(objTiles, 1);              /* blue, its top-left pixel green */
    GX_LoadOBJ(objTiles, 0, sizeof(objTiles));
    GX_LoadOBJPltt(objPltt, 0, sizeof(objPltt));
    MI_CpuFillFast(oam, 0x0200, sizeof(oam));
    G2_SetOBJAttr(&oam[0], 100, 40, 0, GX_OAM_MODE_NORMAL, FALSE, GX_OAM_EFFECT_NONE, GX_OAM_SHAPE_8x8,
                  GX_OAM_COLORMODE_16, 0, 0, 0);
    G2_SetOBJAttr(&oam[1], 20, 40, 0, GX_OAM_MODE_NORMAL, FALSE, GX_OAM_EFFECT_NONE, GX_OAM_SHAPE_8x8,
                  GX_OAM_COLORMODE_16, 0, 0, 0);
    G2_SetOBJAttr(&oam[2], 100, 120, 0, GX_OAM_MODE_NORMAL, FALSE, GX_OAM_EFFECT_NONE, GX_OAM_SHAPE_8x8,
                  GX_OAM_COLORMODE_16, 0, 0, 0);
    DC_FlushRange(oam, sizeof(oam));
    GX_LoadOAM(oam, 0, sizeof(oam));
    GX_SetVisiblePlane(GX_PLANEMASK_OBJ);
    NitroPort_WideScreenParts(0, &objs[0][0], 2);
    NitroPort_Render2DSplit(0, wide, &split);
    y = 43;
    CHECK(wide[y * 320 + 135] == GREY);   /* where it was */
    CHECK(wide[y * 320 + 181] == GREY);
    CHECK(wide[y * 320 + 182] == BLUE);   /* 50 to the right */
    CHECK(wide[y * 320 + 189] == BLUE);
    CHECK(wide[y * 320 + 55] == BLUE);    /* (its middle not in the part's columns) */
    CHECK(wide[123 * 320 + 135] == BLUE); /* (nor in its lines) */
    /* stretched back to where it was: its first column drawn on */
    NitroPort_WideScreenParts(0, &objsStretch[0][0], 1);
    NitroPort_Render2DSplit(0, wide, &split);
    CHECK(wide[43 * 320 + 131] == GREY);
    CHECK(wide[43 * 320 + 132] == BLUE && wide[43 * 320 + 160] == BLUE && wide[43 * 320 + 189] == BLUE);
    CHECK(wide[40 * 320 + 132] == GREEN && wide[40 * 320 + 181] == GREEN && wide[40 * 320 + 182] == GREEN);
    CHECK(wide[40 * 320 + 183] == BLUE);
    CHECK(wide[43 * 320 + 52] == BLUE && wide[43 * 320 + 51] == GREY);   /* (not stretched: not moved) */
    NitroPort_WideScreenParts(0, NULL, 0);
    NitroPort_Render2DSplit(0, wide, &split);
    CHECK(wide[43 * 320 + 135] == BLUE && wide[43 * 320 + 185] == GREY);
    /* the 3D 17 to the left (the compositor moves it left by scroll3d,
     * with BG0's own scroll) */
    GX_SetGraphicsMode(GX_DISPMODE_GRAPHICS, GX_BGMODE_0, GX_BG0_AS_3D);
    GX_SetVisiblePlane(GX_PLANEMASK_BG0);
    G2_SetBG0Offset(3, 0);
    NitroPort_WideScreenParts(0, &objs[0][0], 2);
    NitroPort_Render2DSplit(0, wide, &split);
    CHECK(split.has3d && split.scroll3d == 20);
    NitroPort_WideScreenParts(0, NULL, 0);
    NitroPort_Render2DSplit(0, wide, &split);
    CHECK(split.has3d && split.scroll3d == 3);
    NitroPort_WideForcePictures(-1);
}

/* A tall picture (NitroRender2DSplit.height: the remaster's lower screen
 * as a tall panel): the lines past the DS's 192 go on with the BG's map
 * and the OBJs placed there, as a taller screen would show them; without
 * it, nothing past line 191 is written. */
static void test_tall_picture(void)
{
    static u8 tiles[3 * 32];
    static u16 map[32 * 32];
    static u16 bgPltt[16] = { GREY, RED, GREEN, BLUE };
    static u16 objPltt[16] = { 0, GREEN };
    static u8 objTiles[2 * 32];
    static GXOamAttr oam[128];
    static u16 under[256 * 256];
    NitroRender2DSplit split;
    int i;

    Reset();
    MakeTile4(tiles + 32, 1);           /* tile 1: red (green top-left) */
    MakeTile4(tiles + 64, 3);           /* tile 2: blue (green top-left) */
    for (i = 0; i < 32 * 32; i++) {
        map[i] = i < 24 * 32 ? 1 : 2;   /* the map's rows 0-23 red, 24-31 blue */
    }
    G2S_SetBG0Control(GX_BG_SCRSIZE_TEXT_256x256, GX_BG_COLORMODE_16, GX_BG_SCRBASE_0x0000,
                      GX_BG_CHARBASE_0x04000, GX_BG_EXTPLTT_01);
    GXS_LoadBG0Char(tiles, 0, sizeof(tiles));
    GXS_LoadBG0Scr(map, 0, sizeof(map));
    GXS_LoadBGPltt(bgPltt, 0, sizeof(bgPltt));
    GXS_SetOBJVRamModeChar(GX_OBJVRAMMODE_CHAR_1D_32K);
    MakeTile4(objTiles, 1);
    MakeTile4(objTiles + 32, 1);
    GXS_LoadOBJ(objTiles, 0, sizeof(objTiles));
    GXS_LoadOBJPltt(objPltt, 0, sizeof(objPltt));
    MI_CpuFillFast(oam, 0x0200, sizeof(oam));   /* all hidden */
    /* a 16x8 OBJ at (40, 200): past the DS's screen */
    G2_SetOBJAttr(&oam[0], 40, 200, 0, GX_OAM_MODE_NORMAL, FALSE, GX_OAM_EFFECT_NONE,
                  GX_OAM_SHAPE_16x8, GX_OAM_COLORMODE_16, 0, 0, 0);
    DC_FlushRange(oam, sizeof(oam));
    GXS_LoadOAM(oam, 0, sizeof(oam));
    GXS_SetVisiblePlane(GX_PLANEMASK_BG0 | GX_PLANEMASK_OBJ);

    /* The DS's screen: line 191 the last written. */
    for (i = 0; i < 256 * 256; i++) {
        under[i] = 0x1234;
    }
    memset(&split, 0, sizeof(split));
    NitroPort_Render2DSplit(1, under, &split);
    CHECK_PX(under, 3, 100, RED);
    CHECK_PX(under, 3, 191, RED);
    CHECK_PX(under, 3, 192, 0x1234);

    /* Tall: 248 lines, the map's rows 24-30 and the OBJ below line 191. */
    memset(&split, 0, sizeof(split));
    split.height = 248;
    NitroPort_Render2DSplit(1, under, &split);
    CHECK_PX(under, 3, 100, RED);
    CHECK_PX(under, 3, 191, RED);
    CHECK_PX(under, 3, 195, BLUE);      /* the map's row 24 */
    CHECK_PX(under, 0, 192, GREEN);     /* (its tile's top-left) */
    CHECK_PX(under, 42, 202, GREEN);    /* the OBJ */
    CHECK_PX(under, 60, 202, BLUE);
    CHECK_PX(under, 3, 247, BLUE);
    CHECK_PX(under, 3, 248, 0x1234);    /* nothing past it */

    /* Scrolled down 16: line 180 is the map's row 196, blue. */
    G2S_SetBG0Offset(0, 16);
    NitroPort_Render2DSplit(1, under, &split);
    CHECK_PX(under, 3, 170, RED);
    CHECK_PX(under, 3, 180, BLUE);
    G2S_SetBG0Offset(0, 0);

    /* Too tall, or wider than the DS's: the DS's 192. */
    for (i = 0; i < 256 * 256; i++) {
        under[i] = 0x1234;
    }
    split.height = 300;
    NitroPort_Render2DSplit(1, under, &split);
    CHECK_PX(under, 3, 191, RED);
    CHECK_PX(under, 3, 192, 0x1234);
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
    test_3d_acknowledge_unchanged();
    test_screen_follows_3d();
    test_text_bg();
    test_8bpp_ext_palette();
    test_objs();
    test_smooth_objs();
    test_blend_and_window();
    test_3d_blends_with_layer_behind();
    test_bitmap_bg();
    test_affine_bg();
    test_obj_8bpp_ext();
    test_matches_reference();
    test_split_layers();
    test_wide_canvas();
    test_wide_art();
    test_wide_art_shared();
    test_wide_art_split_bg();
    test_line_regs();
    test_wide_art_objs();
    test_wide_art_places();
    test_wide_art_sliding();
    test_wide_window_edges();
    test_wide_map_past_edges();
    test_gpu_bg();
    test_tall_field();
    test_stretch_squeeze();
    test_wide_framed_windows();
    test_wide_splits();
    test_wide_parts();
    test_tall_picture();
    test_engines_on_two_threads();

    printf("2D renderer: %d checks, %d failed\n", sChecks, sFailures);
    return sFailures == 0 ? 0 : 1;
}
