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
 * the Town Map, drawn here too (port_lowermap.c).
 */
#include <nitro_port_lowermenu.h>
#include <nitro_port_fs.h>
#include <nitro_port.h>
#include "port_platform.h"
#include "port_lowermap.h"

#include <stdlib.h>
#include <string.h>

#define W NITRO_LOWERMENU_W
#define H NITRO_LOWERMENU_H

/* ---------------------------------------------------------------- state */

static volatile int sOn;
static volatile int sTall;          /* (NitroPort_LowerMenuSetTall) */
static int sPanelTouch = 1;         /* (NitroPort_LowerMenuSetPanelTouch) */
static volatile int sHoldUpper;     /* (NitroPort_LowerMenuHoldUpper) */
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
static uint32_t sEnabled = 0xFFFFFFFFu;
static int sPanelShows;             /* (NitroPort_LowerMenuSetPanel) */
static int sPictureHasPanel;        /* (the shared picture's panel drawn in it: under sLock) */

static uint32_t sPicture[W * H];    /* the shared picture (under sLock) */
static uint32_t sDraw[W * H];       /* (drawn here, then copied over) */
static volatile uint32_t sSerial;
static volatile int sLock;

/* (spinning a little, then sleeping: the game's thread and the display
 * thread share a core, the display thread at a lower priority; an endless
 * spin by the game's thread would never let it finish and let go, as in
 * port_gx3d_mailbox.c) */
static void lock(void)
{
    unsigned spins = 0;

    while (__atomic_test_and_set(&sLock, __ATOMIC_ACQUIRE)) {
        if (++spins >= 64u) {
            NitroPortPlatform_Sleep(50);
            spins = 0;
        }
    }
}

static void unlock(void)
{
    __atomic_clear(&sLock, __ATOMIC_RELEASE);
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

/* The panel shows the map (asked to, and it has one). */
static int map_shown(void)
{
    return sPanelShows == NITRO_LOWERMENU_PANEL_MAP && NitroPort_LowerMapReady();
}

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
    if (map_shown()) {
        const uint32_t *panel = NitroPort_LowerMapPanel();

        for (y = 0; y < NITRO_LOWERMENU_PANEL_H; y++) {
            memcpy(sDraw + (NITRO_LOWERMENU_PANEL_Y0 + y) * W + NITRO_LOWERMENU_PANEL_X0, panel + y * NITRO_LOWERMENU_PANEL_W,
                   NITRO_LOWERMENU_PANEL_W * sizeof(uint32_t));
        }
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
    sPictureHasPanel = map_shown();
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
    hold = hold ? 1 : 0;
    if (hold != __atomic_load_n(&sHoldUpper, __ATOMIC_ACQUIRE)) {
        __atomic_store_n(&sHoldUpper, hold, __ATOMIC_RELEASE);
        NitroPort_Log("[lowermenu] upper screen %s", hold ? "held" : "let go");
    }
}

int NitroPort_LowerMenuUpperHeld(void)
{
    return __atomic_load_n(&sHoldUpper, __ATOMIC_ACQUIRE);
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
        sTouchIn = on ? 2 : -1;   /* (a finger still down: nobody's until let go) */
        sHeld = -1;
        sPress = -1;
        if (on) {
            redraw();
        }
        __atomic_store_n(&sOn, on, __ATOMIC_RELEASE);
        if (!on) {
            __atomic_store_n(&sTall, 0, __ATOMIC_RELEASE);   /* (each screen says so again) */
            sPanelTouch = 1;
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
        if (sOn) {
            redraw();
        }
    }
}

void NitroPort_LowerMenuSetEnabled(uint32_t mask)
{
    if (mask != sEnabled) {
        sEnabled = mask;
        if (sOn) {
            redraw();
        }
    }
}

void NitroPort_LowerMenuSetFemale(int female)
{
    female = female ? 1 : 0;
    if (female != sFemale) {
        sFemale = female;
        if (sOn) {
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

int NitroPort_LowerMenuFrameHasPanel(void)
{
    return sPictureHasPanel;
}

void NitroPort_LowerMenuSetPanel(int shows)
{
    shows = shows == NITRO_LOWERMENU_PANEL_MAP ? NITRO_LOWERMENU_PANEL_MAP : NITRO_LOWERMENU_PANEL_DS;
    if (shows != sPanelShows) {
        int before = map_shown();

        sPanelShows = shows;
        if (sOn && map_shown() != before) {
            redraw();
        }
    }
}

int NitroPort_LowerMenuPanel(void)
{
    return map_shown() ? NITRO_LOWERMENU_PANEL_MAP : NITRO_LOWERMENU_PANEL_DS;
}

void NitroPort_LowerMenuSetMap(const NitroLowerMap *map)
{
    if (NitroPort_LowerMapSet(map) && sOn && map_shown()) {
        redraw();
    }
}

static NitroLowerMap sPending;
static uint16_t sPendingName[256 * 64], sPendingDesc[256 * 192];

void NitroPort_LowerMenuMapBegin(void)
{
    /* (the text kept: the last one given, until new text or none) */
    NitroLowerMap text = sPending;

    memset(&sPending, 0, sizeof(sPending));
    sPending.name = text.name;
    sPending.nameX = text.nameX;
    sPending.nameY = text.nameY;
    sPending.nameW = text.nameW;
    sPending.nameH = text.nameH;
    sPending.desc = text.desc;
    sPending.descX = text.descX;
    sPending.descY = text.descY;
    sPending.descW = text.descW;
    sPending.descH = text.descH;
}

void NitroPort_LowerMenuMapSprite(int kind, int x, int y, int frame, int palette)
{
    if (sPending.sprites < NITRO_LOWERMAP_MAX_SPRITES) {
        NitroLowerMapSprite *s = &sPending.sprite[sPending.sprites++];

        s->kind = (uint8_t)kind;
        s->x = (short)x;
        s->y = (short)y;
        s->frame = (uint8_t)frame;
        s->palette = (uint8_t)palette;
    }
}

void NitroPort_LowerMenuMapDot(int x, int y)
{
    if (sPending.dots < NITRO_LOWERMAP_MAX_DOTS) {
        sPending.dot[sPending.dots].x = (short)x;
        sPending.dot[sPending.dots].y = (short)y;
        sPending.dots++;
    }
}

void NitroPort_LowerMenuMapLayout(int hidden, int zoomX, int zoomY)
{
    sPending.hidden = (uint16_t)hidden;
    sPending.zoomX = (short)zoomX;
    sPending.zoomY = (short)zoomY;
}

uint16_t *NitroPort_LowerMenuMapText(int which, int x, int y, int w, int h)
{
    if (w <= 0 || h <= 0 || w > 256 || h > (which == 0 ? 64 : 192)) {
        return NULL;
    }
    if (which == 0) {
        memset(sPendingName, 0, (size_t)w * (size_t)h * 2);
        sPending.name = sPendingName;
        sPending.nameX = (short)x;
        sPending.nameY = (short)y;
        sPending.nameW = (short)w;
        sPending.nameH = (short)h;
        return sPendingName;
    }
    memset(sPendingDesc, 0, (size_t)w * (size_t)h * 2);
    sPending.desc = sPendingDesc;
    sPending.descX = (short)x;
    sPending.descY = (short)y;
    sPending.descW = (short)w;
    sPending.descH = (short)h;
    return sPendingDesc;
}

void NitroPort_LowerMenuMapNoText(int which)
{
    if (which == 0) {
        sPending.name = NULL;
    } else {
        sPending.desc = NULL;
    }
}

void NitroPort_LowerMenuMapEnd(void)
{
    NitroPort_LowerMenuSetMap(&sPending);
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
    cy = (2 * (y - NITRO_LOWERMENU_PANEL_Y0) + 1) * NITRO_LOWERMENU_CANVAS_H / (2 * NITRO_LOWERMENU_PANEL_H);
    if (!NitroPort_LowerMenuTall()) {
        cy -= NITRO_LOWERMENU_DS_ROW0;
    }
    if (dsX) {
        *dsX = cx;
    }
    if (dsY) {
        *dsY = cy < 0 ? 0 : cy > 191 ? 191 : cy;
    }
    return 1;
}

int NitroPort_LowerMenuTouch(int down, int x, int y, int *dsX, int *dsY)
{
    int b, held = sHeld;

    if (!sOn) {
        sTouchIn = -1;
        return 0;
    }
    sFrames++;
    /* (the map's animations: a frame) */
    if (NitroPort_LowerMapTick() && map_shown()) {
        redraw();
    }
    if (!down) {
        if (sTouchIn == 0 && sTouchButton >= 0 && sHeld == sTouchButton) {
            sPressFrame = sFrames;
            __atomic_store_n(&sPress, sTouchButton, __ATOMIC_RELEASE);
        } else if (sTouchIn == 3 && map_shown()) {
            /* (let go: still in the panel, or dragged off it, a tap) */
            sPressFrame = sFrames;
            __atomic_store_n(&sPress, NITRO_LOWERMENU_PANEL_PRESS, __ATOMIC_RELEASE);
        }
        sTouchIn = -1;
        sTouchButton = -1;
        held = -1;
    } else if (sTouchIn == 2) {
        return 0;   /* (down since before the menu came on) */
    } else if (sTouchIn < 0) {
        /* A new touch: the panel's (the game's, all the way; or, the map
         * shown, the menu's), or the frame's. */
        if (NitroPort_LowerMenuPanelAt(x, y, NULL, NULL)) {
            /* (an app whose touch screen isn't shown: nobody's till let go) */
            sTouchIn = map_shown() ? 3 : sPanelTouch ? 1 : 2;
        } else {
            sTouchIn = 0;
            sTouchButton = NitroPort_LowerMenuButtonAt(x, y);
            if (sTouchButton >= 0 && !((sEnabled >> sTouchButton) & 1)) {
                sTouchButton = -1;   /* (one that can't be used now: nothing) */
            }
            held = sTouchButton;
        }
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

int NitroPort_LowerMenuTakePress(void)
{
    int press = NitroPort_LowerMenuPressPending();

    sPress = -1;
    return press;
}
