/*
 * The 3DS remaster's lower screen as a menu: see
 * include/nitro_port_lowermenu.h. The picture is drawn here, on the CPU,
 * whenever it changes (on, off, the button the cursor or a finger is on):
 * the frame (its panel, the column of buttons and the two squares, after
 * the layout the project's owner sent, 2026-10-23), the buttons' names in
 * a small font of its own (the game's is too wide: POKéDEX is 42 pixels
 * in it, a button's name has 38), and the icons: the start menu's own, read
 * from the ROM (graphic/menu_gra.narc), and two drawn for the menu (MAP,
 * POKéTCH). The DS's lower screen goes in the panel when the screen is put
 * together (the compositor, or the PC runner's mshot); or the panel shows
 * the Town Map: a picture of its own (port_lowermap.c), drawn by the map's
 * drawer (a thread of the platform's, off the game's) and put over the
 * panel the same way. The panel fades to black and back as the game
 * changes what it shows (the platforms draw the black over it).
 */
#include <nitro_port_lowermenu.h>
#include <nitro_port_fs.h>
#include <nitro_port.h>
#include "port_platform.h"
#include "port_lowermap.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define W NITRO_LOWERMENU_W
#define H NITRO_LOWERMENU_H

/* ---------------------------------------------------------------- state */

static volatile int sOn;
static volatile int sTall;          /* (NitroPort_LowerMenuSetTall) */
static int sPanelTouch = 1;         /* (NitroPort_LowerMenuSetPanelTouch) */
static volatile int sHoldUpper;     /* (NitroPort_LowerMenuHoldUpper) */
static volatile uint32_t sLetGoAt;  /* (let go now: the clock then; NitroPort_LowerMenuUpperLetGoShows) */
static volatile uint32_t sFieldSerial;   /* (NitroPort_LowerMenuForgetField) */
static volatile int sAvailable;     /* (NitroPort_LowerMenuSetAvailable) */
static int sSelected = -1;          /* the game's cursor */
static int sHeld = -1;              /* the button a finger is on */
static int sFemale;
/* The touch began: -1 none, 0 on the frame, 1 in the panel, 2 before the
 * menu came on (nobody's until it's let go: a finger still down from the
 * screen before isn't a press), 3 in the panel showing the map (the menu's:
 * letting go there is a press, NITRO_LOWERMENU_PANEL_PRESS). */
static int sTouchIn = -1;
static int sTouchButton = -1;       /* (the button it began on) */
static volatile int sPress = -1;    /* a press, not taken yet */
static uint32_t sFrames;            /* (the menu's frames: NitroPort_LowerMenuTouch calls) */
static uint32_t sPressFrame;        /* (the frame it was made in) */
static int sPressDouble;            /* (it was the second of two on that button: NitroPort_LowerMenuPressDouble) */
static int sLastPress = -1;         /* (the press before, and its frame) */
static uint32_t sLastPressFrame;
static uint32_t sEnabled = 0xFFFFFFFFu;
static volatile int sPanelShows;    /* (NitroPort_LowerMenuSetPanel) */
static volatile int sKeepShown;     /* (NitroPort_LowerMenuKeepShown) */
static uint32_t (*sClock)(void);    /* (NitroPort_LowerMenuSetClock) */

/* The panel put together (NitroPort_LowerMenuSetPanelRuns: the game's
 * thread; the platforms' copies under sRunLock) and its tabs. */
static NitroLowerPanelRun sRuns[NITRO_LOWERMENU_RUNS_MAX];
static int sRunCount;
static volatile uint32_t sRunSerial;
static volatile int sRunLock;
static volatile int sRunsUpper;     /* (NitroPort_LowerMenuPanelShowsUpper) */
static int sLastDsX, sLastDsY;      /* (a drag's last place on the lower screen's rows) */
static int sTouchTab = -1;          /* (the tab a touch began on) */
static int sTouchTabOn;             /* (still on it) */
static int sTabCount, sTabActive;
static char sTabLabels[NITRO_LOWERMENU_TABS_MAX][16];
static NitroLowerTabStyle sTabStyle;

static uint32_t sPicture[W * H];    /* the shared picture (under sLock) */
static uint32_t sDraw[W * H];       /* (drawn here, then copied over) */
static int sDirtyY0, sDirtyY1 = H;  /* the shared picture's rows changed, not taken yet (under sLock) */
static volatile uint32_t sSerial;
static volatile int sLock;

/* The panel's fade (under sFadeLock): from sFadeFrom at sFadeStart to
 * sFadeTo (0-16), at 16 levels a NITRO_LOWERMENU_FADE_FRAMES; fading in to the
 * map, waiting for the map given by then (sFadeWaitSub) since sFadeAsked. */
static int sFadeFrom, sFadeTo;
static uint32_t sFadeStart, sFadeAsked, sFadeWaitSub;
static volatile int sFadeLock;
static int sPanelNext = -1;  /* (NitroPort_LowerMenuFadeToPanel: shown once the panel's black; under sFadeLock) */
#define FADE_WAIT_MAX 30   /* (half a second at most) */
/* Fading in to (or clearing at once on) the DS's lower screen: not till the
 * platform has drawn a picture of it made since (sDsWait: the clock then,
 * 0 none), so an app popping in never shows a frame from before it was
 * whole; the platform's newest one, NitroPort_LowerMenuPanelShown (sDsShown;
 * sDsKnown once it has said). */
static uint32_t sDsWait;
static int sDsPop;   /* (the wait's for clearing at once: black meanwhile) */
static volatile uint32_t sDsShown;
static volatile int sDsKnown;
#define FADE_DS_HOLD 4     /* (the DS's lower screen shown: its picture drawn first, these frames black) */

/* The map: what the game gives (its plain calls into sPend, handed over
 * into sSub under sMapLock, sSubSerial counting them), what the map's drawer
 * took last (sWork), and the map's picture for the platforms (sMapPic,
 * under sMapPicLock). The texts as the game's windows hold them (4 bits a
 * pixel), made pixels by the drawer. */
typedef struct MapText {
    int shown;
    short x, y, wTiles, hTiles;
    uint16_t palette[16];
    uint8_t tiles[32 * 24 * 32];
} MapText;
#define MAP_TEXT_MAX_H(which) ((which) == 0 ? 8 : 24)   /* (in tiles: 64 and 192 rows) */
static NitroLowerMap sPend;
static MapText sPendText[2];
static int sPendTextNew[2];
static int sPendWaiting;   /* (given, not handed over yet: the drawer had the lock) */
/* (the texts given, kept: the game's thread's) */
static struct {
    int used;
    uint32_t key, last;
    MapText text;
} sKeptText[2][NITRO_LOWERMENU_TEXTS_KEPT];
static uint32_t sKeptClock;
static NitroLowerMap sSub;
static MapText sSubText[2];
static uint32_t sSubTextSerial[2], sSubSerial;
static volatile int sMapLock;
static NitroLowerMap sWork;
static uint32_t sWorkTextSerial[2], sWorkSub;
static uint16_t sWorkName[256 * 64], sWorkDesc[256 * 192];
static volatile uint32_t sMapFrame;          /* (the menu's frames, for the animations) */
static void (*sMapWake)(void);               /* (NitroPort_LowerMenuSetMapWorker) */
static uint32_t sMapPic[NITRO_LOWERMENU_PANEL_W * NITRO_LOWERMENU_PANEL_H];
static int sMapPicY0 = NITRO_LOWERMENU_PANEL_H, sMapPicY1;
static volatile uint32_t sMapPicSerial, sMapDrawnSub;
static volatile int sMapPicReady, sMapPicLock;

/* (spinning a little, then sleeping: the game's thread and the display
 * thread share a core, the display thread at a lower priority; an endless
 * spin by the game's thread would never let it finish and let go, as in
 * port_gx3d_mailbox.c) */
static void spin_lock(volatile int *l)
{
    unsigned spins = 0;

    while (__atomic_test_and_set(l, __ATOMIC_ACQUIRE)) {
        if (++spins >= 64u) {
            NitroPortPlatform_Sleep(50);
            spins = 0;
        }
    }
}

static void spin_unlock(volatile int *l)
{
    __atomic_clear(l, __ATOMIC_RELEASE);
}

static void lock(void)
{
    spin_lock(&sLock);
}

static void unlock(void)
{
    spin_unlock(&sLock);
}

/* ------------------------------------------------------------ the frame */

#define RGB(r, g, b) (((uint32_t)(r) << 24) | ((uint32_t)(g) << 16) | ((uint32_t)(b) << 8) | 0xFFu)

static const uint32_t BG = RGB(200, 208, 211), EDGE_HI = RGB(252, 253, 253), NAVY = RGB(7, 33, 54),
                      WHITE = RGB(246, 247, 250), PANEL = RGB(185, 199, 217), COL = RGB(77, 100, 114),
                      COLDK = RGB(60, 80, 94), DIV_L1 = RGB(216, 223, 226), DIV_L2 = RGB(230, 234, 237),
                      BTN = RGB(244, 247, 254), BTN_TOP = RGB(252, 253, 255), BTN_SHD = RGB(189, 201, 222),
                      BTN_DIV = RGB(164, 176, 182), CELL = RGB(232, 237, 246), RED_HI = RGB(246, 93, 77),
                      RED = RGB(212, 59, 60), TEXT = RGB(18, 26, 38), SEL = RGB(46, 208, 155),
                      SEL_SHD = RGB(24, 153, 115), SEL_DIV = RGB(26, 108, 95), SEL_CELL = RGB(40, 190, 142),
                      SEL_TXT = RGB(255, 255, 255), SEL_TXT_O = RGB(0, 84, 64), TEXT_OFF = RGB(150, 160, 170);

/* The column: the buttons (outline x 250-316, 22 rows each, 23 apart from
 * row 3), each with its icon's little square (x 251-274), a divider (275)
 * and its name (x 277-314), a red tab at its right edge; the two squares
 * under them. */
#define BTN_X0 250
#define BTN_X1 316
#define BTN_Y(i) (3 + 23 * (i))
#define BTN_H 22
#define ICON_X0 251
#define ICON_X1 274
#define DIVIDER_X 275
#define LABEL_X0 277
#define LABEL_X1 314
static const int sSquares[2][4] = { { 250, 197, 282, 229 }, { 285, 197, 317, 229 } };

static void put(int x, int y, uint32_t c)
{
    if (x >= 0 && x < W && y >= 0 && y < H) {
        sDraw[y * W + x] = c;
    }
}

static void rect(int x0, int y0, int x1, int y1, uint32_t c)
{
    int x, y;

    for (y = y0; y <= y1; y++) {
        for (x = x0; x <= x1; x++) {
            put(x, y, c);
        }
    }
}

/* A box: a 1-pixel outline (its corners left out), its fill, a highlight
 * row under its top and shading rows over its bottom. */
static void box(int x0, int y0, int x1, int y1, uint32_t outline, uint32_t fill, uint32_t top, int hasTop,
                uint32_t shade, int shadeRows)
{
    int x, y, r;

    for (x = x0 + 1; x < x1; x++) {
        put(x, y0, outline);
        put(x, y1, outline);
    }
    for (y = y0 + 1; y < y1; y++) {
        put(x0, y, outline);
        put(x1, y, outline);
    }
    rect(x0 + 1, y0 + 1, x1 - 1, y1 - 1, fill);
    if (hasTop) {
        for (x = x0 + 1; x < x1; x++) {
            put(x, y0 + 1, top);
        }
    }
    for (r = 0; r < shadeRows; r++) {
        for (x = x0 + 1; x < x1; x++) {
            put(x, y1 - 1 - r, shade);
        }
    }
}

/* The names' font: capitals 9 rows tall, 4 wide (M, T, V 5; I 3); 'e' is
 * a small e with its accent (POKéMON). */
typedef struct Glyph {
    char ch;
    const char *rows[9];
} Glyph;

static const Glyph sGlyphs[] = {
    { 'A', { ".##.", "#..#", "#..#", "#..#", "####", "#..#", "#..#", "#..#", "#..#" } },
    { 'B', { "###.", "#..#", "#..#", "#..#", "###.", "#..#", "#..#", "#..#", "###." } },
    { 'C', { ".##.", "#..#", "#...", "#...", "#...", "#...", "#...", "#..#", ".##." } },
    { 'D', { "###.", "#..#", "#..#", "#..#", "#..#", "#..#", "#..#", "#..#", "###." } },
    { 'E', { "####", "#...", "#...", "#...", "###.", "#...", "#...", "#...", "####" } },
    { 'e', { "..#.", ".#..", "....", "....", ".##.", "#..#", "####", "#...", ".###" } },
    { 'G', { ".##.", "#..#", "#...", "#...", "#.##", "#..#", "#..#", "#..#", ".###" } },
    { 'H', { "#..#", "#..#", "#..#", "#..#", "####", "#..#", "#..#", "#..#", "#..#" } },
    { 'I', { "###", ".#.", ".#.", ".#.", ".#.", ".#.", ".#.", ".#.", "###" } },
    { 'K', { "#..#", "#..#", "#.#.", "#.#.", "##..", "#.#.", "#.#.", "#..#", "#..#" } },
    { 'M', { "#...#", "##.##", "#.#.#", "#.#.#", "#...#", "#...#", "#...#", "#...#", "#...#" } },
    { 'N', { "#..#", "#..#", "##.#", "##.#", "#.##", "#.##", "#..#", "#..#", "#..#" } },
    { 'O', { ".##.", "#..#", "#..#", "#..#", "#..#", "#..#", "#..#", "#..#", ".##." } },
    { 'P', { "###.", "#..#", "#..#", "#..#", "###.", "#...", "#...", "#...", "#..." } },
    { 'R', { "###.", "#..#", "#..#", "#..#", "###.", "#.#.", "#..#", "#..#", "#..#" } },
    { 'S', { ".##.", "#..#", "#...", "#...", ".##.", "...#", "...#", "#..#", ".##." } },
    { 'T', { "#####", "..#..", "..#..", "..#..", "..#..", "..#..", "..#..", "..#..", "..#.." } },
    { 'V', { "#...#", "#...#", "#...#", "#...#", "#...#", ".#.#.", ".#.#.", "..#..", "..#.." } },
    { 'X', { "#..#", "#..#", "#..#", ".##.", ".##.", ".##.", "#..#", "#..#", "#..#" } },
};
#define GLYPH_H 9

static const Glyph *glyph(char ch)
{
    size_t i;

    for (i = 0; i < sizeof(sGlyphs) / sizeof(sGlyphs[0]); i++) {
        if (sGlyphs[i].ch == ch) {
            return &sGlyphs[i];
        }
    }
    return NULL;
}

static int text_width(const char *s)
{
    int w = 0, n = 0;

    for (; *s; s++, n++) {
        const Glyph *g = glyph(*s);
        w += g ? (int)strlen(g->rows[0]) : 4;
    }
    return n ? w + n - 1 : 0;
}

static void text(int x, int y, const char *s, uint32_t c)
{
    for (; *s; s++) {
        const Glyph *g = glyph(*s);
        int gx, gy, gw = 4;

        if (g) {
            gw = (int)strlen(g->rows[0]);
            for (gy = 0; gy < GLYPH_H; gy++) {
                for (gx = 0; gx < gw; gx++) {
                    if (g->rows[gy][gx] == '#') {
                        put(x + gx, y + gy, c);
                    }
                }
            }
        }
        x += gw + 1;
    }
}

static void text_outlined(int x, int y, const char *s, uint32_t c, uint32_t outline)
{
    text(x - 1, y, s, outline);
    text(x + 1, y, s, outline);
    text(x, y - 1, s, outline);
    text(x, y + 1, s, outline);
    text(x, y, s, c);
}

static const char *const sNames[8] = { "MAP", "POKeMON", "BAG", "TRAINER", "POKeDEX", "POKeTCH", "SAVE", "OPTIONS" };

/* ------------------------------------------------------------ the icons */

/* An icon: its pixels (0: clear), cropped to what's drawn. */
typedef struct Icon {
    int w, h;
    uint32_t px[32 * 32];
} Icon;

/* The start menu's ten icons (0 Pokédex, 1 Pokémon, 2 bag, 3 trainer card,
 * 4 save, 5 options, ..., 9 her bag), in their coloured palette; and the
 * two drawn for the menu. */
static Icon sIcons[10];
static int sIconsRead;   /* 0 not yet, 1 read, -1 couldn't be */
static Icon sMapIcon, sPoketchIcon;

/* MAP: a map folded in three, sea and land, a route and a red place mark,
 * in the start menu icons' style. 21 x 17. */
static const uint32_t sMapIconPalette[] = { 0x4A525AFFu, 0xFFFFF6FFu, 0xCDDEE6FFu, 0x78C8FFFFu, 0x1870D0FFu,
                                            0x90E468FFu, 0x288838FFu, 0x309CF0FFu, 0xE83828FFu, 0x40B840FFu,
                                            0xDEB440FFu, 0xFFE260FFu };
static const char *const sMapIconRows[] = {
    "......aaa..........aa", "..aaaabccaaaa...aaaba", "aabbbbdeeccccaaabbbba", "abdddddeeeeeecbbdddba",
    "abddddfggeeeeeddffdba", "abhhfffggeeeeedfiffba", "abhfffjggkkkkefilffba", "abjjjjjggkgeeklljjhba",
    "abjjjjlkkkgeeehjhhhba", "abjllljggkgeeehhhhhba", "abjjjjjggggeeehhhhhba", "abjjjjjggggeeehhhhhba",
    "abhhjjheeeeeeehhhhhba", "abhhhhbcceeeeehhhhhba", "abbbbbaaaccccehhbbbaa", "abaaaa...aaaacbbaaa..",
    "aa...........aaa.....",
};
/* POKéTCH: the watch from the front, a chunky red case lit from the top
 * left, a plain green screen in a dark rim, its button at the side. 17 x 18. */
static const uint32_t sPoketchIconPalette[] = { 0x4A525AFFu, 0xFFFFF6FFu, 0xD6E2ECFFu, 0xA4B2C8FFu, 0xFF927CFFu,
                                                0xEE5042FFu, 0xD43C38FFu, 0xB0282EFFu, 0x343844FFu, 0xB0E8A0FFu,
                                                0x82C678FFu, 0xECFCE4FFu, 0xC4CAD6FFu, 0x788092FFu, 0x801A26FFu };
static const char *const sPoketchIconRows[] = {
    "...aaaaaa........", "..abbbbbca.......", "..accccccdaa.....", ".aeeeeeeeeeeaa...", "aeefffffffffgha..",
    "aefiiiiiiiifghha.", "aefijjjjjkifghha.", "aefijlljjkifghmma", "aefijljjjkifghnna", "aefijjjjjkifghha.",
    "aefikkkkkkifghha.", "aefiiiiiiiifghha.", "aeffffffffffghha.", ".aggggggggggfhha.", ".aoooooooooooooa.",
    "..abbbbbcdaaaaa..", "..accccccda......", "...aaaaaaa.......",
};

static void icon_from_rows(Icon *icon, const char *const *rows, int h, const uint32_t *palette)
{
    int x, y;

    icon->w = (int)strlen(rows[0]);
    icon->h = h;
    memset(icon->px, 0, sizeof(icon->px));
    for (y = 0; y < h; y++) {
        for (x = 0; x < icon->w; x++) {
            char c = rows[y][x];
            icon->px[y * 32 + x] = c == '.' ? 0 : palette[c - 'a'];
        }
    }
}

static uint32_t rd16(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}

static uint32_t rd32(const uint8_t *p)
{
    return rd16(p) | (rd16(p + 2) << 16);
}

static uint32_t rgb555(uint32_t c)
{
    uint32_t r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;

    return RGB((r << 3) | (r >> 2), (g << 3) | (g >> 2), (b << 3) | (b >> 2));
}

/* The start menu's icons, from graphic/menu_gra.narc: icons_NCGR (member
 * 6: ten 32 x 32 sprites, 4 bits a pixel, 16 tiles each one after another)
 * in menu_NCLR's (member 5) palette 1, the coloured one. */
static void read_icons(void)
{
    FSFile file;
    uint8_t *narc = NULL;
    uint32_t size = 0, ncgrSize, nclrSize, charSize, palSize;
    const uint8_t *ncgr, *nclr, *chars, *pal;
    int k;

    sIconsRead = -1;
    if (!NitroPort_IsPakOpen()) {
        NitroPort_Log("[lowermenu] no data pack open: no icons");
        return;
    }
    FS_InitFile(&file);
    if (!FS_OpenFile(&file, "graphic/menu_gra.narc")) {
        NitroPort_Log("[lowermenu] can't open graphic/menu_gra.narc: no icons");
        return;
    }
    size = FS_GetLength(&file);
    narc = size > 0 && size < (1u << 20) ? malloc(size) : NULL;
    if (narc == NULL || FS_ReadFile(&file, narc, (s32)size) != (s32)size) {
        FS_CloseFile(&file);
        free(narc);
        NitroPort_Log("[lowermenu] can't read graphic/menu_gra.narc: no icons");
        return;
    }
    FS_CloseFile(&file);
    ncgr = NitroPort_NarcMember(narc, size, 6, &ncgrSize);
    nclr = NitroPort_NarcMember(narc, size, 5, &nclrSize);
    chars = ncgr ? NitroPort_NitroBlock(ncgr, ncgrSize, "RAHC", &charSize) : NULL;
    pal = nclr ? NitroPort_NitroBlock(nclr, nclrSize, "TTLP", &palSize) : NULL;
    if (chars == NULL || pal == NULL || charSize < 24 || palSize < 16) {
        free(narc);
        NitroPort_Log("[lowermenu] menu_gra.narc's icons not as expected: no icons");
        return;
    }
    {
        /* RAHC: tiles high, wide (u16 each), depth, mapping, mode, data
         * size, data offset (from after the block's header, as these
         * fields) */
        uint32_t dataSize = rd32(chars + 16), dataOffset = rd32(chars + 20);
        /* TTLP: depth, unused, data size, data offset (the same) */
        uint32_t colors = rd32(pal + 8) / 2, colorOffset = rd32(pal + 12);
        const uint8_t *tiles = chars + dataOffset, *c555 = pal + colorOffset;
        uint32_t palette[16];
        int i;

        if (dataOffset > charSize || dataSize > charSize - dataOffset || dataSize < 10u * 16u * 32u || colors < 32 ||
            colorOffset > palSize || palSize - colorOffset < 64) {
            free(narc);
            NitroPort_Log("[lowermenu] menu_gra.narc's icons not as expected: no icons");
            return;
        }
        for (i = 0; i < 16; i++) {
            palette[i] = rgb555(rd16(c555 + 2 * (16 + i)));
        }
        for (k = 0; k < 10; k++) {
            Icon *icon = &sIcons[k];
            uint32_t full[32 * 32];
            int t, x, y, x0 = 32, y0 = 32, x1 = -1, y1 = -1;

            for (t = 0; t < 16; t++) {
                const uint8_t *tile = tiles + (k * 16 + t) * 32;
                for (y = 0; y < 8; y++) {
                    for (x = 0; x < 8; x++) {
                        int b = tile[y * 4 + x / 2], v = (x & 1) ? b >> 4 : b & 15;
                        int X = (t % 4) * 8 + x, Y = (t / 4) * 8 + y;

                        full[Y * 32 + X] = v ? palette[v] : 0;
                        if (v) {
                            x0 = X < x0 ? X : x0;
                            y0 = Y < y0 ? Y : y0;
                            x1 = X > x1 ? X : x1;
                            y1 = Y > y1 ? Y : y1;
                        }
                    }
                }
            }
            memset(icon->px, 0, sizeof(icon->px));
            icon->w = x1 >= x0 ? x1 - x0 + 1 : 0;
            icon->h = y1 >= y0 ? y1 - y0 + 1 : 0;
            for (y = 0; y < icon->h; y++) {
                for (x = 0; x < icon->w; x++) {
                    icon->px[y * 32 + x] = full[(y0 + y) * 32 + x0 + x];
                }
            }
        }
    }
    free(narc);
    sIconsRead = 1;
}

/* An icon nine tenths of its size (the bag's and the options' icons, clear
 * of their little square's edges): each pixel the average of what it
 * covers, alpha too. */
static void shrink(const Icon *in, Icon *out)
{
    int w = (in->w * 9 + 5) / 10, h = (in->h * 9 + 5) / 10, x, y;
    float sx = (float)in->w / (float)w, sy = (float)in->h / (float)h;

    memset(out->px, 0, sizeof(out->px));
    out->w = w;
    out->h = h;
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            float fx0 = x * sx, fx1 = (x + 1) * sx, fy0 = y * sy, fy1 = (y + 1) * sy;
            float acc[4] = { 0, 0, 0, 0 }, area = 0;
            int ix, iy;

            for (iy = (int)fy0; iy < (int)(fy1 + 0.999f) && iy < in->h; iy++) {
                float wy = (iy + 1 < fy1 ? iy + 1 : fy1) - (iy > fy0 ? iy : fy0);
                for (ix = (int)fx0; ix < (int)(fx1 + 0.999f) && ix < in->w; ix++) {
                    float wx = (ix + 1 < fx1 ? ix + 1 : fx1) - (ix > fx0 ? ix : fx0), wgt = wx * wy;
                    uint32_t p = in->px[iy * 32 + ix];
                    float a = (float)(p & 0xFF);

                    acc[0] += (float)(p >> 24) * a * wgt;
                    acc[1] += (float)((p >> 16) & 0xFF) * a * wgt;
                    acc[2] += (float)((p >> 8) & 0xFF) * a * wgt;
                    acc[3] += a * wgt;
                    area += wgt;
                }
            }
            if (acc[3] > 0.0f && area > 0.0f) {
                uint32_t a = (uint32_t)(acc[3] / area + 0.5f);
                out->px[y * 32 + x] = RGB(acc[0] / acc[3] + 0.5f, acc[1] / acc[3] + 0.5f, acc[2] / acc[3] + 0.5f) &
                                      (0xFFFFFF00u | a);
                if (a == 0) {
                    out->px[y * 32 + x] = 0;
                }
            }
        }
    }
}

/* An icon over the picture, in the middle of a box (its alpha blended,
 * times opacity / 255: a button that can't be used now, faded). */
static void paste(const Icon *icon, int x0, int y0, int x1, int y1, uint32_t opacity)
{
    int x = x0 + (x1 - x0 + 1 - icon->w) / 2, y = y0 + (y1 - y0 + 1 - icon->h) / 2, i, j;

    for (j = 0; j < icon->h; j++) {
        for (i = 0; i < icon->w; i++) {
            uint32_t p = icon->px[j * 32 + i], a = (p & 0xFF) * opacity / 255;
            int X = x + i, Y = y + j;

            if (a == 0 || X < 0 || X >= W || Y < 0 || Y >= H) {
                continue;
            }
            if (a == 255) {
                sDraw[Y * W + X] = p;
            } else {
                uint32_t q = sDraw[Y * W + X], c, out = 0xFF;
                int s;

                for (s = 24; s >= 8; s -= 8) {
                    c = (((p >> s) & 0xFF) * a + ((q >> s) & 0xFF) * (255 - a) + 127) / 255;
                    out |= c << s;
                }
                sDraw[Y * W + X] = out;
            }
        }
    }
}

/* ------------------------------------------------------------ drawing */

/* The panel shows the map (asked to, and the Town Map's pictures could be
 * read: the map's drawer draws it). */
static int map_shown(void)
{
    return sPanelShows == NITRO_LOWERMENU_PANEL_MAP && NitroPort_LowerMapLoaded();
}

/* The picture drawn (on, or kept on while the game swaps what's under it). */
static int drawn(void)
{
    return sOn || sKeepShown;
}

static void draw_tabs(void);

static void draw(void)
{
    int i, x, y;
    static Icon small[2];
    const Icon *icons[8];

    /* The background, its light top and left edges. */
    rect(0, 0, W - 1, H - 1, BG);
    for (x = 0; x < 246; x++) {
        put(x, 0, EDGE_HI);
    }
    for (y = 0; y < H; y++) {
        put(0, y, EDGE_HI);
    }
    /* The panel: navy outline, white inside it. */
    box(1, 2, 244, 237, NAVY, PANEL, 0, 0, 0, 0);
    for (x = 2; x < 244; x++) {
        put(x, 3, WHITE);
        put(x, 236, WHITE);
    }
    for (y = 3; y < 237; y++) {
        put(2, y, WHITE);
        put(243, y, WHITE);
    }
    /* The column. */
    for (y = 0; y < H; y++) {
        put(246, y, DIV_L1);
        put(247, y, NAVY);
        put(248, y, DIV_L2);
        put(319, y, NAVY);
    }
    rect(249, 0, 318, H - 1, COL);
    for (x = 249; x < 319; x++) {
        put(x, 188, NAVY);
        put(x, 189, COLDK);
    }
    /* The icons, by button. */
    icons[NITRO_LOWERMENU_MAP] = &sMapIcon;
    icons[NITRO_LOWERMENU_POKETCH] = &sPoketchIcon;
    if (sIconsRead == 1) {
        icons[NITRO_LOWERMENU_POKEMON] = &sIcons[1];
        icons[NITRO_LOWERMENU_TRAINER] = &sIcons[3];
        icons[NITRO_LOWERMENU_POKEDEX] = &sIcons[0];
        icons[NITRO_LOWERMENU_SAVE] = &sIcons[4];
        shrink(&sIcons[sFemale ? 9 : 2], &small[0]);
        shrink(&sIcons[5], &small[1]);
        icons[NITRO_LOWERMENU_BAG] = &small[0];
        icons[NITRO_LOWERMENU_OPTIONS] = &small[1];
    } else {
        icons[NITRO_LOWERMENU_POKEMON] = icons[NITRO_LOWERMENU_BAG] = icons[NITRO_LOWERMENU_TRAINER] =
            icons[NITRO_LOWERMENU_POKEDEX] = icons[NITRO_LOWERMENU_SAVE] = icons[NITRO_LOWERMENU_OPTIONS] = NULL;
    }
    for (i = 0; i < 8; i++) {
        int y0 = BTN_Y(i), sel = (i == sSelected || i == sHeld), on = (sEnabled >> i) & 1;
        char name[8];
        int tx, ty, k;

        box(BTN_X0, y0, BTN_X1, y0 + BTN_H - 1, NAVY, sel ? SEL : BTN, BTN_TOP, !sel, sel ? SEL_SHD : BTN_SHD, 2);
        rect(ICON_X0, y0 + 1, ICON_X1, y0 + 20, sel ? SEL_CELL : CELL);
        for (y = y0 + 1; y < y0 + 21; y++) {
            put(DIVIDER_X, y, sel ? SEL_DIV : BTN_DIV);
        }
        for (y = y0 + 3; y < y0 + 19; y++) {   /* the red tab */
            put(317, y, RED_HI);
            put(318, y, RED);
        }
        for (k = 0; sNames[i][k] && k < 7; k++) {
            name[k] = sNames[i][k];
        }
        name[k] = '\0';
        tx = LABEL_X0 + (LABEL_X1 - LABEL_X0 + 1 - text_width(name) + 1) / 2;
        ty = y0 + 1 + (18 - GLYPH_H + 1) / 2;   /* in the 18 rows over the shading */
        if (sel) {
            text_outlined(tx, ty, name, SEL_TXT, SEL_TXT_O);
        } else {
            text(tx, ty, name, on ? TEXT : TEXT_OFF);
        }
        if (icons[i] != NULL) {
            paste(icons[i], ICON_X0, y0 + 1, ICON_X1, y0 + 20, on || sel ? 255 : 96);
        }
    }
    for (i = 0; i < 2; i++) {
        const int *s = sSquares[i];
        int sel = (NITRO_LOWERMENU_ITEM1 + i == sSelected || NITRO_LOWERMENU_ITEM1 + i == sHeld);

        box(s[0], s[1], s[2], s[3], NAVY, sel ? SEL : BTN, BTN_TOP, !sel, sel ? SEL_SHD : BTN_SHD, 2);
    }
    draw_tabs();
}

/* ------------------------------------------------------------ the tabs */

/* The canvas's row a panel pixel's row shows (as NitroPort_LowerMenuPanelAt). */
static int canvas_row(int y)
{
    return (2 * (y - NITRO_LOWERMENU_PANEL_Y0) + 1) * NITRO_LOWERMENU_CANVAS_H / (2 * NITRO_LOWERMENU_PANEL_H);
}

/* The first tabs' run's panel rows (-1: none). */
static int tab_rows(int *y0, int *y1)
{
    int i, y;

    for (i = 0; i < sRunCount; i++) {
        if (sRuns[i].from == NITRO_LOWERMENU_FROM_TABS && sRuns[i].rows > 0) {
            *y0 = *y1 = -1;
            for (y = NITRO_LOWERMENU_PANEL_Y0; y < NITRO_LOWERMENU_PANEL_Y0 + NITRO_LOWERMENU_PANEL_H; y++) {
                int c = canvas_row(y);

                if (c >= sRuns[i].y && c < sRuns[i].y + sRuns[i].rows) {
                    *y0 = *y0 < 0 ? y : *y0;
                    *y1 = y;
                }
            }
            return *y0 >= 0 ? 0 : -1;
        }
    }
    return -1;
}

/* Tab k's columns (the panel's 240, 4 apart). */
static void tab_columns(int k, int *x0, int *x1)
{
    int w = (NITRO_LOWERMENU_PANEL_W - 4 * (sTabCount - 1)) / sTabCount;

    *x0 = NITRO_LOWERMENU_PANEL_X0 + k * (w + 4);
    *x1 = k == sTabCount - 1 ? NITRO_LOWERMENU_PANEL_X0 + NITRO_LOWERMENU_PANEL_W - 1 : *x0 + w - 1;
}

/* The tab under a panel pixel (-1: none; its rows above the top line). */
static int tab_at(int x, int y)
{
    int y0, y1, k, x0, x1;

    if (sTabCount <= 0 || tab_rows(&y0, &y1) < 0 || y < y0 || y > y1) {
        return -1;
    }
    for (k = 0; k < sTabCount; k++) {
        tab_columns(k, &x0, &x1);
        if (x >= x0 && x <= x1) {
            return k;
        }
    }
    return -1;
}

static uint32_t rgb888(uint32_t c)
{
    return (c & 0xFFFFFFu) << 8 | 0xFFu;
}

/* A small Poké Ball: its ring, its band and its button. */
static const char *const sTabBall[9] = {
    "...###...", ".##...##.", ".#.....#.", "#..###..#", "####.####",
    "#..###..#", ".#.....#.", ".##...##.", "...###...",
};

static void tab_ball(int x, int y, uint32_t c)
{
    int i, j;

    for (j = 0; j < 9; j++) {
        for (i = 0; i < 9; i++) {
            if (sTabBall[j][i] == '#') {
                put(x + i, y + j, c);
                put(x + i + 1, y + j, c);   /* (bold, as the case's own lettering) */
            }
        }
    }
}

/* The tabs' lettering: the names' font, bold (each column twice), a
 * pixel more between the letters. */
static int text_bold_width(const char *s)
{
    int w = 0, n = 0;

    for (; *s; s++, n++) {
        const Glyph *g = glyph(*s);
        w += (g ? (int)strlen(g->rows[0]) : 4) + 1;
    }
    return n ? w + 2 * (n - 1) : 0;
}

static void text_bold(int x, int y, const char *s, uint32_t c)
{
    for (; *s; s++) {
        const Glyph *g = glyph(*s);
        int gx, gy, gw = 4;

        if (g) {
            gw = (int)strlen(g->rows[0]);
            for (gy = 0; gy < GLYPH_H; gy++) {
                for (gx = 0; gx < gw; gx++) {
                    if (g->rows[gy][gx] == '#') {
                        put(x + gx, y + gy, c);
                        put(x + gx + 1, y + gy, c);
                    }
                }
            }
        }
        x += gw + 3;
    }
}

static void draw_tabs(void)
{
    /* (each row of a tab's rounded top corner: how far in it begins) */
    static const int corner[] = { 5, 3, 2, 1, 1 };
    const int nCorner = (int)(sizeof(corner) / sizeof(corner[0]));
    int y0, y1, top, line, k, x, y;
    uint32_t back = rgb888(sTabStyle.back), edge = rgb888(sTabStyle.edge), fill = rgb888(sTabStyle.fill);

    if (sTabCount <= 0 || tab_rows(&y0, &y1) < 0) {
        return;
    }
    top = y0 + 2;          /* the tabs' top line */
    line = y1 - 5;         /* the top line of what they're the tabs of */
    if (line - top < 13) {
        top = line - 13 < y0 ? y0 : line - 13;
    }
    rect(NITRO_LOWERMENU_PANEL_X0, y0, NITRO_LOWERMENU_PANEL_X0 + NITRO_LOWERMENU_PANEL_W - 1, line - 1, back);
    /* What's under them: its fill, its edge at the sides; its top line. */
    rect(NITRO_LOWERMENU_PANEL_X0, line, NITRO_LOWERMENU_PANEL_X0 + NITRO_LOWERMENU_PANEL_W - 1, y1, fill);
    for (y = line; y <= y1; y++) {
        put(NITRO_LOWERMENU_PANEL_X0, y, edge);
        put(NITRO_LOWERMENU_PANEL_X0 + NITRO_LOWERMENU_PANEL_W - 1, y, edge);
    }
    for (x = NITRO_LOWERMENU_PANEL_X0; x < NITRO_LOWERMENU_PANEL_X0 + NITRO_LOWERMENU_PANEL_W; x++) {
        put(x, line, edge);
    }
    for (k = 0; k < sTabCount; k++) {
        int x0, x1, on = k == sTabActive, w, lx, ly;
        uint32_t f = on ? fill : rgb888(sTabStyle.fillOff);
        uint32_t ink = rgb888(on ? sTabStyle.ink : sTabStyle.inkShadow);
        uint32_t shadow = rgb888(on ? sTabStyle.inkShadow : sTabStyle.inkOff);

        tab_columns(k, &x0, &x1);
        /* Its fill and edges down to the line, the top corners rounded. */
        for (y = top; y < line; y++) {
            int in = y - top < nCorner ? corner[y - top] : 0;

            for (x = x0 + in; x <= x1 - in; x++) {
                put(x, y, f);
            }
            put(x0 + in, y, edge);
            put(x1 - in, y, edge);
            if (y > top) {
                /* (the edge joined to the row above's) */
                int above = y - 1 - top < nCorner ? corner[y - 1 - top] : 0, i;

                for (i = in + 1; i < above; i++) {
                    put(x0 + i, y, edge);
                    put(x1 - i, y, edge);
                }
            } else {
                for (x = x0 + in; x <= x1 - in; x++) {
                    put(x, y, edge);
                }
            }
        }
        /* The open one opens into what's under it: no line under it. */
        if (on) {
            for (x = x0 + 1; x < x1; x++) {
                put(x, line, f);
            }
        }
        /* Its ball and label, in the middle of its fill. */
        w = 10 + 4 + text_bold_width(sTabLabels[k]);
        lx = x0 + (x1 - x0 + 1 - w) / 2;
        ly = top + 1 + (line - top - 1 - GLYPH_H) / 2;
        tab_ball(lx + 1, ly + 1, shadow);
        tab_ball(lx, ly, ink);
        text_bold(lx + 14 + 1, ly + 1, sTabLabels[k], shadow);
        text_bold(lx + 14, ly, sTabLabels[k], ink);
    }
}

/* Drawn again and handed over. */
static void redraw(void)
{
    if (sMapIcon.w == 0) {
        icon_from_rows(&sMapIcon, sMapIconRows, (int)(sizeof(sMapIconRows) / sizeof(sMapIconRows[0])), sMapIconPalette);
        icon_from_rows(&sPoketchIcon, sPoketchIconRows, (int)(sizeof(sPoketchIconRows) / sizeof(sPoketchIconRows[0])),
                       sPoketchIconPalette);
    }
    draw();
    lock();
    memcpy(sPicture, sDraw, sizeof(sPicture));
    sDirtyY0 = 0;
    sDirtyY1 = H;
    __atomic_add_fetch(&sSerial, 1u, __ATOMIC_RELEASE);
    unlock();
}

/* ------------------------------------------------------------ the API */

void NitroPort_LowerMenuSetAvailable(int available)
{
    __atomic_store_n(&sAvailable, available ? 1 : 0, __ATOMIC_RELEASE);
}

int NitroPort_LowerMenuAvailable(void)
{
    return __atomic_load_n(&sAvailable, __ATOMIC_ACQUIRE);
}

void NitroPort_LowerMenuSetPanelTouch(int on)
{
    sPanelTouch = on ? 1 : 0;
}

void NitroPort_LowerMenuHoldUpper(int hold)
{
    hold = hold == NITRO_LOWERMENU_LET_GO_NOW || hold == NITRO_LOWERMENU_HOLD_FIELD ? hold : hold ? 1 : 0;
    if (hold != __atomic_load_n(&sHoldUpper, __ATOMIC_ACQUIRE)) {
        if (hold == NITRO_LOWERMENU_LET_GO_NOW) {
            __atomic_store_n(&sLetGoAt, sClock ? sClock() : 0u, __ATOMIC_RELEASE);
        }
        __atomic_store_n(&sHoldUpper, hold, __ATOMIC_RELEASE);
        NitroPort_Log("[lowermenu] upper screen %s", hold == NITRO_LOWERMENU_HOLD_FIELD ? "held, the field's picture"
                                                     : hold > 0                         ? "held"
                                                     : hold < 0                         ? "let go now"
                                                                                        : "let go");
    }
}

int NitroPort_LowerMenuUpperHeld(void)
{
    return __atomic_load_n(&sHoldUpper, __ATOMIC_ACQUIRE);
}

void NitroPort_LowerMenuForgetField(void)
{
    __atomic_add_fetch(&sFieldSerial, 1u, __ATOMIC_RELEASE);
}

uint32_t NitroPort_LowerMenuFieldSerial(void)
{
    return __atomic_load_n(&sFieldSerial, __ATOMIC_ACQUIRE);
}

int NitroPort_LowerMenuUpperLetGoShows(uint32_t stamp)
{
    uint32_t at = __atomic_load_n(&sLetGoAt, __ATOMIC_ACQUIRE);

    if (sClock == NULL) {
        return 1;
    }
    return (int32_t)(stamp - at) >= 0 || (int32_t)(sClock() - at) >= FADE_WAIT_MAX;
}

int NitroPort_LowerMenuBrightness(unsigned masterBright)
{
    int mode = (int)(masterBright >> 14) & 3, f = (int)(masterBright & 31);

    f = f > 16 ? 16 : f;
    return mode == 1 ? f : mode == 2 ? -f : 0;
}

void NitroPort_LowerMenuShow(int on)
{
    on = on && __atomic_load_n(&sAvailable, __ATOMIC_ACQUIRE) ? 1 : 0;
    if (on && sIconsRead == 0) {
        read_icons();
    }
    if (on != sOn) {
        /* (kept shown meanwhile, the menu never went: its touches and press
         * go on) */
        if (!sKeepShown) {
            sTouchIn = on ? 2 : -1;   /* (a finger still down: nobody's until let go) */
            sHeld = -1;
            sPress = -1;
            sLastPress = -1;          /* (no double tap across the time it was off) */
        }
        if (on) {
            redraw();
        }
        __atomic_store_n(&sOn, on, __ATOMIC_RELEASE);
        if (!on) {
            __atomic_store_n(&sTall, 0, __ATOMIC_RELEASE);   /* (each screen says so again) */
            sPanelTouch = 1;
            if (sTabCount > 0) {
                sTabCount = 0;
                if (sKeepShown) {
                    redraw();
                }
            }
            NitroPort_LowerMenuSetPanelRuns(NULL, 0);
            __atomic_add_fetch(&sSerial, 1u, __ATOMIC_RELEASE);
        }
        NitroPort_Log("[lowermenu] %s", on ? "on" : "off");
    }
}

void NitroPort_LowerMenuSetTall(int tall)
{
    tall = tall ? 1 : 0;
    if (tall != __atomic_load_n(&sTall, __ATOMIC_ACQUIRE)) {
        __atomic_store_n(&sTall, tall, __ATOMIC_RELEASE);
        __atomic_add_fetch(&sSerial, 1u, __ATOMIC_RELEASE);
    }
}

int NitroPort_LowerMenuTall(void)
{
    return __atomic_load_n(&sTall, __ATOMIC_ACQUIRE);
}

int NitroPort_LowerMenuOn(void)
{
    return __atomic_load_n(&sOn, __ATOMIC_ACQUIRE) && __atomic_load_n(&sAvailable, __ATOMIC_ACQUIRE);
}

void NitroPort_LowerMenuSelect(int button)
{
    if (button < -1 || button >= NITRO_LOWERMENU_COUNT) {
        button = -1;
    }
    if (button != sSelected) {
        sSelected = button;
        if (drawn()) {
            redraw();
        }
    }
}

void NitroPort_LowerMenuSetEnabled(uint32_t mask)
{
    if (mask != sEnabled) {
        sEnabled = mask;
        if (drawn()) {
            redraw();
        }
    }
}

void NitroPort_LowerMenuSetFemale(int female)
{
    female = female ? 1 : 0;
    if (female != sFemale) {
        sFemale = female;
        if (drawn()) {
            redraw();
        }
    }
}

const uint32_t *NitroPort_LowerMenuLockFrame(uint32_t *serial)
{
    lock();
    if (serial) {
        *serial = sSerial;
    }
    return sPicture;
}

void NitroPort_LowerMenuTakeDirtyRows(int *y0, int *y1)
{
    *y0 = sDirtyY0;
    *y1 = sDirtyY1;
    sDirtyY0 = H;
    sDirtyY1 = 0;
}

/* (under sFadeLock, or the game's thread with no change on its way) */
static void show_panel(int shows)
{
    if (shows != sPanelShows) {
        __atomic_store_n(&sPanelShows, shows, __ATOMIC_RELEASE);
        __atomic_add_fetch(&sSerial, 1u, __ATOMIC_RELEASE);   /* (the platforms draw the panel) */
    }
}

void NitroPort_LowerMenuSetPanel(int shows)
{
    shows = shows == NITRO_LOWERMENU_PANEL_MAP ? shows : NITRO_LOWERMENU_PANEL_DS;
    spin_lock(&sFadeLock);
    if (sPanelNext >= 0) {
        sPanelNext = shows;   /* (a change on its way: shown once the panel's black) */
    } else {
        show_panel(shows);
    }
    spin_unlock(&sFadeLock);
}

int NitroPort_LowerMenuPanel(void)
{
    return map_shown() ? NITRO_LOWERMENU_PANEL_MAP : NITRO_LOWERMENU_PANEL_DS;
}

/* ----------------------------------------------------------- the fade */

void NitroPort_LowerMenuSetClock(uint32_t (*now)(void))
{
    sClock = now;
}

/* How dark the panel is at the time now (under sFadeLock; a fade that
 * starts later: as it starts). */
static int fade_level(uint32_t now)
{
    int span = sFadeTo > sFadeFrom ? sFadeTo - sFadeFrom : sFadeFrom - sFadeTo;
    int32_t frames = span * NITRO_LOWERMENU_FADE_FRAMES / 16, t = (int32_t)(now - sFadeStart);

    if (sClock == NULL || frames == 0 || t >= frames) {
        return sFadeTo;
    }
    if (t <= 0) {
        return sFadeFrom;
    }
    return sFadeFrom + (sFadeTo - sFadeFrom) * t / frames;
}

/* (under sFadeLock) A fade from how dark it is now to `to`, beginning in
 * `wait` frames. Fading in to the map: once it's drawn as given by now. */
static void fade_to(int to, uint32_t now, uint32_t wait)
{
    /* (from how dark it is now: black while an app's still to pop in) */
    sFadeFrom = sDsWait != 0 && sDsPop ? 16 : fade_level(now);
    sFadeTo = to;
    sFadeStart = now + wait;
    sFadeAsked = now;
    sFadeWaitSub = to ? 0 : __atomic_load_n(&sSubSerial, __ATOMIC_ACQUIRE);
    sDsWait = to ? 0 : now;
    sDsPop = 0;
}

void NitroPort_LowerMenuFadePanel(int black)
{
    uint32_t now;

    spin_lock(&sFadeLock);
    now = sClock ? sClock() : 0;   /* (read under the lock: no fade asked for after it) */
    if (black == NITRO_LOWERMENU_CLEAR_NOW) {
        sFadeFrom = sFadeTo = 0;
        sFadeWaitSub = 0;
        sFadeAsked = now;
        sDsWait = now;     /* (black till there's a picture of it from now on: see PanelFade) */
        sDsPop = 1;
        sPanelNext = -1;   /* (a change on its way: not made; the game shows what it wants) */
    } else if (black) {
        sPanelNext = -1;   /* (black to stay so: a change on its way not made) */
        if (sFadeTo != 16) {
            fade_to(16, now, 0);
        }
    } else if (sPanelNext < 0 && sFadeTo != 0) {
        /* (with a change on its way, it fades in itself) */
        fade_to(0, now, 0);
    }
    spin_unlock(&sFadeLock);
}

void NitroPort_LowerMenuFadeToPanel(int shows)
{
    uint32_t now;

    shows = shows == NITRO_LOWERMENU_PANEL_MAP ? shows : NITRO_LOWERMENU_PANEL_DS;
    spin_lock(&sFadeLock);
    now = sClock ? sClock() : 0;
    if (shows != (sPanelNext >= 0 ? sPanelNext : sPanelShows)) {
        if (sClock == NULL) {
            show_panel(shows);   /* (no clock: at once) */
            sPanelNext = -1;
        } else {
            sPanelNext = shows;
            if (sFadeTo != 16) {
                fade_to(16, now, 0);
            }
        }
    }
    spin_unlock(&sFadeLock);
}

int NitroPort_LowerMenuPanelFade(void)
{
    uint32_t now;
    int level;

    spin_lock(&sFadeLock);
    now = sClock ? sClock() : 0;   /* (read under the lock: never before a fade asked for) */
    /* A change on its way, the panel black: made, and faded back in (the DS's
     * lower screen a few frames on: its picture drawn first). On whichever
     * thread asks first: the display's every frame, whatever the game's
     * doing. */
    if (sPanelNext >= 0 && fade_level(now) >= 16) {
        int shows = sPanelNext, span = 16 - sFadeFrom;
        /* (from when it went black, however late it's asked) */
        uint32_t black = sClock != NULL ? sFadeStart + (uint32_t)(span * NITRO_LOWERMENU_FADE_FRAMES / 16) : now;

        if ((int32_t)(now - black) < 0) {
            black = now;
        }
        sPanelNext = -1;
        show_panel(shows);
        fade_to(0, black, shows == NITRO_LOWERMENU_PANEL_DS ? FADE_DS_HOLD : 0);
        sFadeAsked = now;
    }
    if (sFadeTo == 0 && map_shown()) {
        if (!__atomic_load_n(&sMapPicReady, __ATOMIC_ACQUIRE)) {
            /* (no map drawn yet: black, faded in once it is) */
            sFadeFrom = 16;
            sFadeStart = now;
        } else if (sFadeWaitSub != 0) {
            /* (fading in to the map: not begun till it's drawn as it was
             * given when the fade was asked for, half a second at most) */
            if ((int32_t)(__atomic_load_n(&sMapDrawnSub, __ATOMIC_ACQUIRE) - sFadeWaitSub) < 0 &&
                sClock != NULL && (int32_t)(now - sFadeAsked) < FADE_WAIT_MAX) {
                sFadeStart = now;
            } else {
                sFadeWaitSub = 0;
            }
        }
    }
    if (sDsWait != 0 && map_shown()) {
        sDsWait = 0;   /* (the map shown instead: its own wait) */
    } else if (sFadeTo == 0 && sDsWait != 0) {
        /* (fading in to the DS's lower screen: the fade not begun (cleared
         * at once: black) till the platform's drawn a picture of it made
         * since; half a second at most) */
        if (__atomic_load_n(&sDsKnown, __ATOMIC_ACQUIRE) && sClock != NULL &&
            (int32_t)(__atomic_load_n(&sDsShown, __ATOMIC_ACQUIRE) - sDsWait) < 0 &&
            (int32_t)(now - sFadeAsked) < FADE_WAIT_MAX) {
            sFadeStart = now;
        } else {
            sDsWait = 0;
        }
    }
    level = sDsWait != 0 && sDsPop ? 16 : fade_level(now);
    if (sClock == NULL && sFadeTo == 0 && map_shown() && !__atomic_load_n(&sMapPicReady, __ATOMIC_ACQUIRE)) {
        level = 16;
    }
    spin_unlock(&sFadeLock);
    return level;
}

void NitroPort_LowerMenuPanelShown(uint32_t stamp)
{
    __atomic_store_n(&sDsShown, stamp, __ATOMIC_RELEASE);
    __atomic_store_n(&sDsKnown, 1, __ATOMIC_RELEASE);
}

void NitroPort_LowerMenuKeepShown(int on)
{
    on = on ? 1 : 0;
    if (on != sKeepShown) {
        __atomic_store_n(&sKeepShown, on, __ATOMIC_RELEASE);
        if (on && !sOn) {
            redraw();   /* (as the game has it now) */
        } else {
            __atomic_add_fetch(&sSerial, 1u, __ATOMIC_RELEASE);
        }
        NitroPort_Log("[lowermenu] %s", on ? "kept shown" : "not kept shown");
    }
}

int NitroPort_LowerMenuKeptShown(void)
{
    return __atomic_load_n(&sKeepShown, __ATOMIC_ACQUIRE);
}

/* ------------------------------------------------- the panel put together */

static int runs_equal(const NitroLowerPanelRun *a, const NitroLowerPanelRun *b, int n)
{
    int i;

    for (i = 0; i < n; i++) {
        if (a[i].y != b[i].y || a[i].rows != b[i].rows || a[i].from != b[i].from || a[i].srcY != b[i].srcY ||
            a[i].step != b[i].step || a[i].sides != b[i].sides || a[i].sidesIn != b[i].sidesIn) {
            return 0;
        }
    }
    return 1;
}

void NitroPort_LowerMenuSetPanelRuns(const NitroLowerPanelRun *runs, int count)
{
    int hadTabs, i, n = 0;
    NitroLowerPanelRun next[NITRO_LOWERMENU_RUNS_MAX];

    count = runs == NULL || count < 0 ? 0 : count > NITRO_LOWERMENU_RUNS_MAX ? NITRO_LOWERMENU_RUNS_MAX : count;
    for (i = 0; i < count; i++) {
        NitroLowerPanelRun r = runs[i];

        /* (kept in the canvas; the step one of the three) */
        if (r.y < 0) {
            r.rows += r.y;
            r.srcY += (short)(-r.y * r.step);
            r.y = 0;
        }
        if (r.y + r.rows > NITRO_LOWERMENU_CANVAS_H) {
            r.rows = (short)(NITRO_LOWERMENU_CANVAS_H - r.y);
        }
        r.step = r.step > 0 ? 1 : r.step < 0 ? -1 : 0;
        if (r.rows > 0 && r.from >= NITRO_LOWERMENU_FROM_LOWER && r.from <= NITRO_LOWERMENU_FROM_TABS) {
            next[n++] = r;
        }
    }
    if (n == sRunCount && runs_equal(next, sRuns, n)) {
        return;
    }
    hadTabs = tab_rows(&i, &i) == 0;
    spin_lock(&sRunLock);
    memcpy(sRuns, next, sizeof(next[0]) * (size_t)n);
    sRunCount = n;
    for (i = 0; i < n && next[i].from != NITRO_LOWERMENU_FROM_UPPER; i++) {
    }
    __atomic_store_n(&sRunsUpper, i < n, __ATOMIC_RELEASE);
    __atomic_add_fetch(&sRunSerial, 1u, __ATOMIC_RELEASE);
    spin_unlock(&sRunLock);
    /* (the tabs drawn where they now are) */
    if ((hadTabs || tab_rows(&i, &i) == 0) && sTabCount > 0 && drawn()) {
        redraw();
    } else {
        __atomic_add_fetch(&sSerial, 1u, __ATOMIC_RELEASE);
    }
}

int NitroPort_LowerMenuPanelShowsUpper(void)
{
    return __atomic_load_n(&sRunsUpper, __ATOMIC_ACQUIRE);
}

int NitroPort_LowerMenuPanelRuns(NitroLowerPanelRun *out, uint32_t *serial)
{
    int n;

    spin_lock(&sRunLock);
    n = sRunCount;
    if (out != NULL) {
        memcpy(out, sRuns, sizeof(sRuns[0]) * (size_t)n);
    }
    if (serial != NULL) {
        *serial = sRunSerial;
    }
    spin_unlock(&sRunLock);
    return n;
}

int NitroPort_LowerMenuRunRow(const NitroLowerPanelRun *runs, int count, int canvasY, int *srcY)
{
    int i;

    for (i = 0; i < count; i++) {
        if (canvasY >= runs[i].y && canvasY < runs[i].y + runs[i].rows) {
            if (srcY != NULL) {
                *srcY = runs[i].srcY + (canvasY - runs[i].y) * runs[i].step;
            }
            return runs[i].from;
        }
    }
    return -1;
}

void NitroPort_LowerMenuSetTabs(int count, const char *const *labels, int active, const NitroLowerTabStyle *style)
{
    int k, changed;

    count = labels == NULL || style == NULL || count < 0 ? 0 : count > NITRO_LOWERMENU_TABS_MAX ? NITRO_LOWERMENU_TABS_MAX : count;
    changed = count != sTabCount || active != sTabActive || (count > 0 && memcmp(style, &sTabStyle, sizeof(sTabStyle)) != 0);
    for (k = 0; k < count; k++) {
        const char *l = labels[k] ? labels[k] : "";

        if (strncmp(l, sTabLabels[k], sizeof(sTabLabels[k]) - 1) != 0) {
            strncpy(sTabLabels[k], l, sizeof(sTabLabels[k]) - 1);
            sTabLabels[k][sizeof(sTabLabels[k]) - 1] = '\0';
            changed = 1;
        }
    }
    if (!changed) {
        return;
    }
    sTabCount = count;
    sTabActive = active;
    if (style != NULL) {
        sTabStyle = *style;
    }
    if (drawn()) {
        redraw();
    }
}

/* ------------------------------------------------------------ the map */

/* The panel's rows the map's drawer drew again, into the map's picture for
 * the platforms. */
static void publish_map(void)
{
    const uint32_t *panel = NitroPort_LowerMapPanel();
    int y0, y1;

    NitroPort_LowerMapTakeDirty(&y0, &y1);
    if (y0 >= y1) {
        return;
    }
    spin_lock(&sMapPicLock);
    memcpy(sMapPic + y0 * NITRO_LOWERMENU_PANEL_W, panel + y0 * NITRO_LOWERMENU_PANEL_W,
           (size_t)(y1 - y0) * NITRO_LOWERMENU_PANEL_W * sizeof(uint32_t));
    sMapPicY0 = y0 < sMapPicY0 ? y0 : sMapPicY0;
    sMapPicY1 = y1 > sMapPicY1 ? y1 : sMapPicY1;
    __atomic_store_n(&sMapPicReady, 1, __ATOMIC_RELEASE);
    __atomic_add_fetch(&sMapPicSerial, 1u, __ATOMIC_RELEASE);
    spin_unlock(&sMapPicLock);
}

void NitroPort_LowerMenuSetMap(const NitroLowerMap *map)
{
    if (NitroPort_LowerMapLoad() && NitroPort_LowerMapSet(map)) {
        publish_map();
    }
}

int NitroPort_LowerMenuMapAvailable(void)
{
    return NitroPort_LowerMapLoad();
}

void NitroPort_LowerMenuMapBegin(void)
{
    /* (the texts kept: the last ones given, new till they're handed over) */
    memset(&sPend, 0, sizeof(sPend));
}

void NitroPort_LowerMenuMapSprite(int kind, int x, int y, int frame, int palette)
{
    if (sPend.sprites < NITRO_LOWERMAP_MAX_SPRITES) {
        NitroLowerMapSprite *s = &sPend.sprite[sPend.sprites++];

        s->kind = (uint8_t)kind;
        s->x = (short)x;
        s->y = (short)y;
        s->frame = (uint8_t)frame;
        s->palette = (uint8_t)palette;
    }
}

void NitroPort_LowerMenuMapDot(int x, int y)
{
    if (sPend.dots < NITRO_LOWERMAP_MAX_DOTS) {
        sPend.dot[sPend.dots].x = (short)x;
        sPend.dot[sPend.dots].y = (short)y;
        sPend.dots++;
    }
}

void NitroPort_LowerMenuMapLayout(int hidden, int zoomX, int zoomY)
{
    sPend.hidden = (uint16_t)hidden;
    sPend.zoomX = (short)zoomX;
    sPend.zoomY = (short)zoomY;
}

void NitroPort_LowerMenuMapText(int which, int x, int y, int wTiles, int hTiles, const void *tiles,
                                const uint16_t *palette, uint32_t key)
{
    MapText *t;
    int k, slot = 0;

    if (which < 0 || which > 1) {
        return;
    }
    t = &sPendText[which];
    sPendTextNew[which] = 1;
    t->shown = tiles != NULL && palette != NULL && wTiles > 0 && hTiles > 0 && wTiles <= 32 &&
               hTiles <= MAP_TEXT_MAX_H(which);
    if (!t->shown) {
        return;
    }
    t->x = (short)x;
    t->y = (short)y;
    t->wTiles = (short)wTiles;
    t->hTiles = (short)hTiles;
    memcpy(t->palette, palette, sizeof(t->palette));
    memcpy(t->tiles, tiles, (size_t)wTiles * (size_t)hTiles * 32u);
    /* (kept: in the slot with that key, else an empty one, else the one used
     * longest ago) */
    for (k = 0; k < NITRO_LOWERMENU_TEXTS_KEPT; k++) {
        if (sKeptText[which][k].used && sKeptText[which][k].key == key) {
            slot = k;
            break;
        }
        if (!sKeptText[which][k].used || (sKeptText[which][slot].used && sKeptText[which][k].last < sKeptText[which][slot].last)) {
            slot = k;
        }
    }
    sKeptText[which][slot].used = 1;
    sKeptText[which][slot].key = key;
    sKeptText[which][slot].last = ++sKeptClock;
    memcpy(&sKeptText[which][slot].text, t, offsetof(MapText, tiles) + (size_t)wTiles * (size_t)hTiles * 32u);
}

int NitroPort_LowerMenuMapTextKept(int which, uint32_t key)
{
    int k;

    if (which < 0 || which > 1) {
        return 0;
    }
    for (k = 0; k < NITRO_LOWERMENU_TEXTS_KEPT; k++) {
        if (sKeptText[which][k].used && sKeptText[which][k].key == key) {
            const MapText *from = &sKeptText[which][k].text;

            sKeptText[which][k].last = ++sKeptClock;
            memcpy(&sPendText[which], from, offsetof(MapText, tiles) + (size_t)from->wTiles * (size_t)from->hTiles * 32u);
            sPendTextNew[which] = 1;
            return 1;
        }
    }
    return 0;
}

void NitroPort_LowerMenuMapNoText(int which)
{
    if (which >= 0 && which <= 1) {
        sPendText[which].shown = 0;
        sPendTextNew[which] = 1;
    }
}

/* The map's drawer wanted: woken, or (no worker) drawing now. */
static void want_map_work(void)
{
    if (sMapWake != NULL) {
        sMapWake();
    } else {
        NitroPort_LowerMenuMapWork();
    }
}

/* What the game gave handed over to the map's drawer, if its lock's free:
 * the game's thread never waits for the drawer (on another core, at a low
 * priority, it may be held up while it has the lock); else again next frame
 * (NitroPort_LowerMenuTouch). */
static void hand_over(void)
{
    int k;

    if (!sPendWaiting || __atomic_test_and_set(&sMapLock, __ATOMIC_ACQUIRE)) {
        return;
    }
    sPendWaiting = 0;
    sSub = sPend;
    for (k = 0; k < 2; k++) {
        if (sPendTextNew[k]) {
            const MapText *from = &sPendText[k];
            MapText *to = &sSubText[k];

            /* (as much as it holds) */
            memcpy(to, from, offsetof(MapText, tiles));
            if (from->shown) {
                memcpy(to->tiles, from->tiles, (size_t)from->wTiles * (size_t)from->hTiles * 32u);
            }
            sSubTextSerial[k]++;
            sPendTextNew[k] = 0;
        }
    }
    __atomic_add_fetch(&sSubSerial, 1u, __ATOMIC_RELEASE);
    spin_unlock(&sMapLock);
    want_map_work();
}

void NitroPort_LowerMenuMapEnd(void)
{
    if (!NitroPort_LowerMapLoad()) {
        return;   /* (no Town Map's pictures: no map) */
    }
    sPendWaiting = 1;
    hand_over();
}

void NitroPort_LowerMenuSetMapWorker(void (*wake)(void))
{
    sMapWake = wake;
}

/* A text's window as pixels (NitroLowerMap's: 0x8000 | its colour, or 0). */
static void text_pixels(const MapText *t, uint16_t *out)
{
    int w = t->wTiles * 8, h = t->hTiles * 8, x, y;

    for (y = 0; y < h; y++) {
        const uint8_t *row = t->tiles + (size_t)(y >> 3) * (size_t)t->wTiles * 32u + (size_t)(y & 7) * 4u;

        for (x = 0; x < w; x += 2) {
            int b = row[(x >> 3) * 32 + ((x & 7) >> 1)], lo = b & 15, hi = b >> 4;

            out[y * w + x] = lo ? (uint16_t)(0x8000u | (t->palette[lo] & 0x7FFFu)) : 0;
            out[y * w + x + 1] = hi ? (uint16_t)(0x8000u | (t->palette[hi] & 0x7FFFu)) : 0;
        }
    }
}

int NitroPort_LowerMenuMapWork(void)
{
    static MapText text[2];   /* (the drawer's) */
    int take, fresh[2] = { 0, 0 }, k, drew;
    uint32_t sub, before = __atomic_load_n(&sMapPicSerial, __ATOMIC_ACQUIRE);

    spin_lock(&sMapLock);
    sub = sSubSerial;
    take = sub != sWorkSub;
    if (take) {
        NitroLowerMap keep = sWork;   /* (the texts' places, as last made) */

        sWork = sSub;
        sWork.name = keep.name;
        sWork.nameX = keep.nameX;
        sWork.nameY = keep.nameY;
        sWork.nameW = keep.nameW;
        sWork.nameH = keep.nameH;
        sWork.desc = keep.desc;
        sWork.descX = keep.descX;
        sWork.descY = keep.descY;
        sWork.descW = keep.descW;
        sWork.descH = keep.descH;
        for (k = 0; k < 2; k++) {
            if (sSubTextSerial[k] != sWorkTextSerial[k]) {
                memcpy(&text[k], &sSubText[k], offsetof(MapText, tiles));
                if (sSubText[k].shown) {
                    memcpy(text[k].tiles, sSubText[k].tiles, (size_t)sSubText[k].wTiles * (size_t)sSubText[k].hTiles * 32u);
                }
                sWorkTextSerial[k] = sSubTextSerial[k];
                fresh[k] = 1;
            }
        }
        sWorkSub = sub;
    }
    spin_unlock(&sMapLock);
    /* (the animations' frame first: a whole map drawn at it) */
    NitroPort_LowerMapSetFrame(__atomic_load_n(&sMapFrame, __ATOMIC_ACQUIRE));
    if (take) {
        for (k = 0; k < 2; k++) {
            uint16_t *pixels = k == 0 ? sWorkName : sWorkDesc;

            if (!fresh[k]) {
                continue;
            }
            if (!text[k].shown) {
                if (k == 0) {
                    sWork.name = NULL;
                } else {
                    sWork.desc = NULL;
                }
                continue;
            }
            text_pixels(&text[k], pixels);
            if (k == 0) {
                sWork.name = pixels;
                sWork.nameX = text[k].x;
                sWork.nameY = text[k].y;
                sWork.nameW = (short)(text[k].wTiles * 8);
                sWork.nameH = (short)(text[k].hTiles * 8);
            } else {
                sWork.desc = pixels;
                sWork.descX = text[k].x;
                sWork.descY = text[k].y;
                sWork.descW = (short)(text[k].wTiles * 8);
                sWork.descH = (short)(text[k].hTiles * 8);
            }
        }
        NitroPort_LowerMapSet(&sWork);
    }
    publish_map();
    if (take) {
        __atomic_store_n(&sMapDrawnSub, sub, __ATOMIC_RELEASE);
    }
    drew = __atomic_load_n(&sMapPicSerial, __ATOMIC_ACQUIRE) != before;
    return drew;
}

const uint32_t *NitroPort_LowerMenuLockMap(uint32_t *serial)
{
    spin_lock(&sMapPicLock);
    if (serial) {
        *serial = sMapPicSerial;
    }
    return sMapPicReady ? sMapPic : NULL;
}

int NitroPort_LowerMenuTryLockMap(const uint32_t **map, uint32_t *serial)
{
    if (__atomic_test_and_set(&sMapPicLock, __ATOMIC_ACQUIRE)) {
        return 0;
    }
    if (serial) {
        *serial = sMapPicSerial;
    }
    *map = sMapPicReady ? sMapPic : NULL;
    return 1;
}

void NitroPort_LowerMenuTakeMapDirtyRows(int *y0, int *y1)
{
    *y0 = sMapPicY0;
    *y1 = sMapPicY1;
    sMapPicY0 = NITRO_LOWERMENU_PANEL_H;
    sMapPicY1 = 0;
}

void NitroPort_LowerMenuUnlockMap(void)
{
    spin_unlock(&sMapPicLock);
}

uint32_t NitroPort_LowerMenuMapSerial(void)
{
    return __atomic_load_n(&sMapPicSerial, __ATOMIC_ACQUIRE);
}

void NitroPort_LowerMenuUnlockFrame(void)
{
    unlock();
}

uint32_t NitroPort_LowerMenuSerial(void)
{
    return __atomic_load_n(&sSerial, __ATOMIC_ACQUIRE);
}

int NitroPort_LowerMenuButtonAt(int x, int y)
{
    int i;

    if (x >= BTN_X0 && x <= BTN_X1) {
        for (i = 0; i < 8; i++) {
            if (y >= BTN_Y(i) && y < BTN_Y(i) + BTN_H) {
                return i;
            }
        }
    }
    for (i = 0; i < 2; i++) {
        const int *s = sSquares[i];

        if (x >= s[0] && x <= s[2] && y >= s[1] && y <= s[3]) {
            return NITRO_LOWERMENU_ITEM1 + i;
        }
    }
    return -1;
}

int NitroPort_LowerMenuPanelAt(int x, int y, int *dsX, int *dsY)
{
    int cx, cy;

    if (x < NITRO_LOWERMENU_PANEL_X0 || x >= NITRO_LOWERMENU_PANEL_X0 + NITRO_LOWERMENU_PANEL_W ||
        y < NITRO_LOWERMENU_PANEL_Y0 || y >= NITRO_LOWERMENU_PANEL_Y0 + NITRO_LOWERMENU_PANEL_H) {
        return 0;
    }
    /* The canvas's pixel under the screen pixel's middle, then the DS's
     * (the canvas rows outside the DS's 192 to its top or bottom row; tall,
     * the canvas's row is the DS's). */
    cx = (2 * (x - NITRO_LOWERMENU_PANEL_X0) + 1) * NITRO_LOWERMENU_CANVAS_W / (2 * NITRO_LOWERMENU_PANEL_W);
    cy = canvas_row(y);
    if (sRunCount > 0) {
        /* (put together: the lower screen's row there; on another's, where
         * the drag last was on the lower screen's) */
        int src, from = NitroPort_LowerMenuRunRow(sRuns, sRunCount, cy, &src);

        if (from == NITRO_LOWERMENU_FROM_LOWER) {
            cy = src;
        } else if (from >= 0) {
            cx = sLastDsX;
            cy = sLastDsY;
        } else if (!NitroPort_LowerMenuTall()) {
            cy -= NITRO_LOWERMENU_DS_ROW0;
        }
    } else if (!NitroPort_LowerMenuTall()) {
        cy -= NITRO_LOWERMENU_DS_ROW0;
    }
    cy = cy < 0 ? 0 : cy > 191 ? 191 : cy;
    sLastDsX = cx;
    sLastDsY = cy;
    if (dsX) {
        *dsX = cx;
    }
    if (dsY) {
        *dsY = cy;
    }
    return 1;
}

int NitroPort_LowerMenuTouch(int down, int x, int y, int *dsX, int *dsY)
{
    int b, held = sHeld;

    sFrames++;
    hand_over();   /* (a map the drawer's lock held up) */
    if (!sOn && !sKeepShown) {
        sTouchIn = -1;
        return 0;
    }
    /* (the map's animations: drawn when they change, the map shown) */
    if (NitroPort_LowerMapFrameChanges(__atomic_add_fetch(&sMapFrame, 1u, __ATOMIC_ACQ_REL)) && map_shown() &&
        __atomic_load_n(&sMapPicReady, __ATOMIC_ACQUIRE)) {
        want_map_work();
    }
    if (!down) {
        if (sTouchIn == 0 && sTouchButton >= 0 && sHeld == sTouchButton) {
            /* (the second press on a button within NITRO_LOWERMENU_DOUBLE_FRAMES
             * of the first: a double tap) */
            sPressDouble = sTouchButton == sLastPress && sFrames - sLastPressFrame <= NITRO_LOWERMENU_DOUBLE_FRAMES;
            sLastPress = sPressDouble ? -1 : sTouchButton;   /* (a third starts again) */
            sLastPressFrame = sFrames;
            sPressFrame = sFrames;
            __atomic_store_n(&sPress, sTouchButton, __ATOMIC_RELEASE);
        } else if (sTouchIn == 4 && sTouchTab >= 0 && sTouchTabOn) {
            /* (a tab: let go on it) */
            sPressDouble = 0;
            sLastPress = -1;
            sPressFrame = sFrames;
            __atomic_store_n(&sPress, NITRO_LOWERMENU_TAB_PRESS(sTouchTab), __ATOMIC_RELEASE);
        } else if (sTouchIn == 3 && map_shown()) {
            /* (let go: still in the panel, or dragged off it, a tap) */
            sPressDouble = 0;
            sLastPress = -1;
            sPressFrame = sFrames;
            __atomic_store_n(&sPress, NITRO_LOWERMENU_PANEL_PRESS, __ATOMIC_RELEASE);
        }
        sTouchIn = -1;
        sTouchButton = -1;
        sTouchTab = -1;
        held = -1;
    } else if (sTouchIn == 2) {
        return 0;   /* (down since before the menu came on) */
    } else if (sTouchIn < 0) {
        /* A new touch: the panel's (the game's, all the way; or, the map
         * shown, the menu's), or the frame's. */
        if (NitroPort_LowerMenuPanelAt(x, y, NULL, NULL)) {
            /* (an app whose touch screen isn't shown, or the menu kept
             * shown while the game swaps what's under it: nobody's till let
             * go) */
            sTouchIn = !sOn || NitroPort_LowerMenuPanelFade() > 0 ? 2 : map_shown() ? 3 : sPanelTouch ? 1 : 2;
            if (sTouchIn == 1 && sRunCount > 0 && !map_shown()) {
                /* (put together: the lower screen's rows the game's, the
                 * tabs' the tabs', the upper screen's nobody's) */
                int from = NitroPort_LowerMenuRunRow(sRuns, sRunCount, canvas_row(y), NULL);

                sTouchTab = from == NITRO_LOWERMENU_FROM_TABS ? tab_at(x, y) : -1;
                sTouchTabOn = 1;
                sTouchIn = from == NITRO_LOWERMENU_FROM_TABS ? (sTouchTab >= 0 ? 4 : 2)
                           : from == NITRO_LOWERMENU_FROM_UPPER ? 2
                                                                : 1;
            } else if (sTouchIn == 2 && sRunCount > 0 && sOn && NitroPort_LowerMenuPanelFade() <= 0 && !map_shown() &&
                       NitroPort_LowerMenuRunRow(sRuns, sRunCount, canvas_row(y), NULL) == NITRO_LOWERMENU_FROM_TABS) {
                /* (the tabs work while the game's touches don't) */
                sTouchTab = tab_at(x, y);
                sTouchTabOn = 1;
                sTouchIn = sTouchTab >= 0 ? 4 : 2;
            }
        } else {
            sTouchIn = 0;
            sTouchButton = NitroPort_LowerMenuButtonAt(x, y);
            if (sTouchButton >= 0 && !((sEnabled >> sTouchButton) & 1)) {
                sTouchButton = -1;   /* (one that can't be used now: nothing) */
            }
            held = sTouchButton;
        }
    } else if (sTouchIn == 4) {
        sTouchTabOn = tab_at(x, y) == sTouchTab;
    } else if (sTouchIn == 0) {
        /* Still on the button it began on: pressed; off it: not. */
        b = NitroPort_LowerMenuButtonAt(x, y);
        held = b == sTouchButton ? b : -1;
    }
    if (held != sHeld) {
        sHeld = held;
        redraw();
    }
    if (down && sTouchIn == 1) {
        /* (dragged out of the panel: its nearest pixel) */
        int px = x < NITRO_LOWERMENU_PANEL_X0 ? NITRO_LOWERMENU_PANEL_X0
                 : x >= NITRO_LOWERMENU_PANEL_X0 + NITRO_LOWERMENU_PANEL_W
                     ? NITRO_LOWERMENU_PANEL_X0 + NITRO_LOWERMENU_PANEL_W - 1
                     : x;
        int py = y < NITRO_LOWERMENU_PANEL_Y0 ? NITRO_LOWERMENU_PANEL_Y0
                 : y >= NITRO_LOWERMENU_PANEL_Y0 + NITRO_LOWERMENU_PANEL_H
                     ? NITRO_LOWERMENU_PANEL_Y0 + NITRO_LOWERMENU_PANEL_H - 1
                     : y;

        return NitroPort_LowerMenuPanelAt(px, py, dsX, dsY);
    }
    return 0;
}

int NitroPort_LowerMenuPressPending(void)
{
    if (sPress >= 0 && sFrames - sPressFrame > NITRO_LOWERMENU_PRESS_FRAMES) {
        sPress = -1;   /* (too old) */
    }
    return sPress;
}

int NitroPort_LowerMenuPressDouble(void)
{
    return sPressDouble;
}

int NitroPort_LowerMenuTakePress(void)
{
    int press = NitroPort_LowerMenuPressPending();

    sPress = -1;
    return press;
}
