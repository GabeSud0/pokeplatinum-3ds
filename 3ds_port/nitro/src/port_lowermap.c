/*
 * The Town Map in the 3DS remaster's lower menu's panel
 * (nitro_port_lowermenu.h: NitroPort_LowerMenuSetMap; port_lowermap.h).
 *
 * The canvas is 256 x 247 DS pixels, made as the Town Map app makes its
 * screens (the decompilation's src/applications/town_map/), from its own
 * pictures in graphic/tmap_gra.narc:
 *
 * - rows 0-191: its upper screen. BG3 the region's background (the sea and
 *   the blue bars, the name's bar), BG2 the land and the routes over it,
 *   blended (G2_SetBlendAlpha(BG2, BG3, 28, 4): BG2 at 16/16, what's behind
 *   at 4/16); the hidden places shown copied into both as the Town Map does
 *   (ShowHiddenLocation), and the arrow of the Fly screen's bar taken out as
 *   it does for the Town Map item; the sprites (OBJ priority 2: in front of
 *   BG2 and BG3) as the game places them: the cursor (its two frames by
 *   turns), the player's head, one of the dots of the places been through
 *   before at a time, the towns' blocks; over everything, the name's window
 *   (BG1).
 * - rows 192-246: the guide page's rows 61-115 (the description's first
 *   lines): its zoomed-in map (BG3 its background, BG2 over it, 512 x 512,
 *   at the scroll the game gives) and the description's window over it.
 *
 * Checked against the Town Map app's screens in the PC runner: the same
 * pixels (2026-10-24).
 *
 * The panel's picture is the canvas at 15/16 (240 x 232) as the screens
 * draw the DS's lower screen in the panel: each canvas pixel 2 x 2 texels,
 * then bilinear. Only what changes is drawn again: the upper screen's
 * background only when the hidden places shown change; the zoomed-in band
 * when its scroll or the description does; the sprites' rectangles where
 * they were and where they are; the name's when it changes; the animated
 * sprites' for a frame of theirs. The panel's rows drawn again are told
 * (NitroPort_LowerMapTakeDirty). It's drawn by the map's drawer
 * (port_lowermenu.c: a thread of the platform's, off the game's, or the
 * game's own); the pictures are read on the game's thread
 * (NitroPort_LowerMapLoad: the data pack is the game's thread's).
 */

#include <nitro.h>
#include <nitro_port.h>
#include <nitro_port_fs.h>
#include <nitro_port_lowermenu.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "port_lowermap.h"

#define CW 256
#define CH NITRO_LOWERMENU_CANVAS_H
#define TOP_H 192
#define BAND_ROW0 61   /* the guide page's row at the canvas's row 192 */
#define PW NITRO_LOWERMENU_PANEL_W
#define PH NITRO_LOWERMENU_PANEL_H

/* graphic/tmap_gra.narc's members (res/graphics/town_map's order) */
enum {
    M_TOP_PAL = 0, M_ZOOM_PAL = 1, M_OBJ_PAL = 2,
    M_CURSOR = 7, M_PLAYER = 10, M_DOT = 13, M_BLOCKS = 16,
    M_TOP_TILES = 19, M_ZOOM_TILES = 20,
    M_TOP_BG = 22, M_HIDDEN_TOP = 23, M_TOP_MAP = 24, M_ZOOM_BG = 25, M_ZOOM_MAP = 26, M_HIDDEN_ZOOM = 29,
};

/* The Town Map's frames: the cursor's two (34 each at speed 2), the dots'
 * (the next one every 26). */
#define CURSOR_FRAMES 17
#define DOT_FRAMES 26

/* (the hidden places: ShowHiddenLocation's table) */
enum { HIDDEN_FULLMOON, HIDDEN_NEWMOON, HIDDEN_SPRING_PATH, HIDDEN_SEABREAK_PATH, HIDDEN_COUNT };
typedef struct { uint8_t srcX, srcY, destX, destY, w, h; } Rect;
static const Rect sHidden[HIDDEN_COUNT][4] = {
    /* the upper screen's BG3, its BG2, the zoomed-in map's BG3, its BG2 */
    [HIDDEN_FULLMOON] = { { 5, 0, 3, 1, 2, 3 }, { 5, 3, 3, 2, 2, 2 }, { 8, 0, 3, 16, 4, 4 }, { 8, 4, 3, 16, 4, 4 } },
    [HIDDEN_NEWMOON] = { { 0, 0, 6, 1, 2, 3 }, { 0, 3, 6, 2, 2, 2 }, { 0, 0, 8, 16, 4, 4 }, { 0, 4, 8, 16, 4, 4 } },
    [HIDDEN_SPRING_PATH] = { { 0, 0, 0, 0, 0, 0 }, { 4, 5, 22, 13, 3, 3 }, { 0, 0, 0, 0, 0, 0 }, { 8, 8, 48, 42, 5, 6 } },
    [HIDDEN_SEABREAK_PATH] = { { 2, 0, 26, 0, 3, 3 }, { 2, 3, 27, 0, 2, 9 }, { 4, 0, 57, 12, 4, 4 }, { 4, 4, 57, 12, 4, 19 } },
};

typedef struct Tiles {
    const uint8_t *data;
    uint32_t count;   /* 8 x 8 tiles, 4 bits a pixel */
} Tiles;

typedef struct Screen {
    const uint8_t *data;   /* its entries (16 bits each, unaligned) */
    int w, h;              /* in tiles */
} Screen;

static struct {
    int state;                         /* 0 not read, 1 read, -1 couldn't be */
    uint8_t *narc;
    Tiles top, zoom, cursor, player, dot, blocks;
    uint16_t topPal[16][16], zoomPal[16][16], objPal[16][16];
    Screen topBg, topMap, hiddenTop, zoomBg, zoomMap, hiddenZoom;
} G;

static int sReady;
static NitroLowerMap sMap;
static uint16_t sName[256 * 64], sDesc[256 * 192];   /* (the map's text, copied) */
static uint16_t sTopBg[32 * 32], sTopMap[32 * 32];   /* (the upper screen's BGs, as the game's buffers) */
static uint16_t sZoomBg[64 * 64], sZoomMap[64 * 64]; /* (the zoomed-in map's, 512 x 512: four 32 x 32 blocks) */
static uint16_t sBack[CW * CH];   /* the canvas without the sprites and the name */
static uint16_t sCanvas[CW * CH];
static uint32_t sPanel[PW * PH];
static uint32_t sFrame;
static int sCursorFrame, sDotShown = -1;
static int sDirtyY0 = PH, sDirtyY1 = 0;   /* the panel's rows drawn again, not told yet */

/* ------------------------------------------------------------- reading */

static uint32_t rd16(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}

static uint32_t rd32(const uint8_t *p)
{
    return rd16(p) | (rd16(p + 2) << 16);
}

const uint8_t *NitroPort_NarcMember(const uint8_t *narc, uint32_t size, int index, uint32_t *memberSize)
{
    uint32_t p, fatSize, count, gmif, start, end;

    /* (every check by subtraction: the sizes are the file's, not to be
     * trusted, and a sum could wrap past the check) */
    if (narc == NULL || size < 16 || memcmp(narc, "NARC", 4) != 0) {
        return NULL;
    }
    p = rd16(narc + 12);
    if (p > size - 12 || memcmp(narc + p, "BTAF", 4) != 0) {
        return NULL;
    }
    fatSize = rd32(narc + p + 4);
    count = rd16(narc + p + 8);
    if (index < 0 || (uint32_t)index >= count || 8u * count > size - p - 12) {
        return NULL;
    }
    start = rd32(narc + p + 12 + 8 * (uint32_t)index);
    end = rd32(narc + p + 16 + 8 * (uint32_t)index);
    if (fatSize > size - p) {
        return NULL;
    }
    p += fatSize;   /* the file names */
    if (p > size - 8 || memcmp(narc + p, "BTNF", 4) != 0 || rd32(narc + p + 4) > size - p) {
        return NULL;
    }
    p += rd32(narc + p + 4);
    if (p > size - 8 || memcmp(narc + p, "GMIF", 4) != 0) {
        return NULL;
    }
    gmif = p + 8;
    if (end < start || end > size - gmif) {
        return NULL;
    }
    *memberSize = end - start;
    return narc + gmif + start;
}

const uint8_t *NitroPort_NitroBlock(const uint8_t *file, uint32_t size, const char *magic, uint32_t *blockSize)
{
    uint32_t p, n, i;

    if (file == NULL || size < 16) {
        return NULL;
    }
    p = rd16(file + 12);
    n = rd16(file + 14);
    for (i = 0; i < n && p <= size - 8; i++) {
        uint32_t len = rd32(file + p + 4);

        if (memcmp(file + p, magic, 4) == 0) {
            *blockSize = len < 8 || len > size - p ? size - p - 8 : len - 8;
            return file + p + 8;
        }
        if (len < 8 || len > size - p) {
            break;
        }
        p += len;
    }
    return NULL;
}

static const uint8_t *member(uint32_t narcSize, int index, uint32_t *size)
{
    return NitroPort_NarcMember(G.narc, narcSize, index, size);
}

/* An NCGR's tiles (4 bits a pixel). */
static int read_tiles(uint32_t narcSize, int index, Tiles *t)
{
    uint32_t size, blockSize;
    const uint8_t *f = member(narcSize, index, &size), *b;

    b = f ? NitroPort_NitroBlock(f, size, "RAHC", &blockSize) : NULL;
    if (b == NULL || blockSize < 24) {
        return 0;
    }
    {
        /* RAHC: tiles high, wide (u16 each), depth, mapping, mode, data
         * size, data offset (from these fields) */
        uint32_t dataSize = rd32(b + 16), dataOffset = rd32(b + 20);

        if (rd32(b + 4) != 3 || dataOffset > blockSize || dataSize > blockSize - dataOffset || dataSize < 32) {
            return 0;
        }
        t->data = b + dataOffset;
        t->count = dataSize / 32;
    }
    return 1;
}

/* An NCLR's palettes, into the slots its PMCP block names (else in turn). */
static int read_palettes(uint32_t narcSize, int index, uint16_t pal[16][16])
{
    uint32_t size, blockSize, pmcpSize = 0;
    const uint8_t *f = member(narcSize, index, &size), *b, *pmcp;
    uint32_t colors, offset, n, i, k;

    b = f ? NitroPort_NitroBlock(f, size, "TTLP", &blockSize) : NULL;
    if (b == NULL || blockSize < 16) {
        return 0;
    }
    colors = rd32(b + 8) / 2;
    offset = rd32(b + 12);
    if (offset > blockSize || colors > (blockSize - offset) / 2) {
        return 0;
    }
    pmcp = NitroPort_NitroBlock(f, size, "PMCP", &pmcpSize);
    n = colors / 16;
    for (i = 0; i < n && i < 16; i++) {
        uint32_t slot = i;

        if (pmcp != NULL && pmcpSize >= 8) {
            uint32_t ids = rd16(pmcp), idOffset = rd32(pmcp + 4);

            if (i >= ids || idOffset > pmcpSize || 2 * (i + 1) > pmcpSize - idOffset) {
                break;
            }
            slot = rd16(pmcp + idOffset + 2 * i);
            if (slot >= 16) {
                continue;
            }
        }
        for (k = 0; k < 16; k++) {
            pal[slot][k] = (uint16_t)(rd16(b + offset + 2 * (16 * i + k)) & 0x7FFF);
        }
    }
    return n > 0;
}

/* An NSCR's entries. */
static int read_screen(uint32_t narcSize, int index, Screen *s)
{
    uint32_t size, blockSize;
    const uint8_t *f = member(narcSize, index, &size), *b;

    b = f ? NitroPort_NitroBlock(f, size, "NRCS", &blockSize) : NULL;
    if (b == NULL || blockSize < 12) {
        return 0;
    }
    /* NRCS: width, height (pixels, u16 each), format, data size, data */
    s->w = (int)rd16(b) / 8;
    s->h = (int)rd16(b + 2) / 8;
    if (s->w <= 0 || s->h <= 0 || rd32(b + 8) > blockSize - 12 || (uint32_t)(s->w * s->h * 2) > rd32(b + 8)) {
        return 0;
    }
    s->data = b + 12;
    return 1;
}

static uint16_t entry(const Screen *s, int i)
{
    return (uint16_t)rd16(s->data + 2 * i);
}

static void load(void)
{
    FSFile file;
    uint32_t size;
    int ok;

    G.state = -1;
    if (!NitroPort_IsPakOpen()) {
        NitroPort_Log("[lowermap] no data pack open: no map");
        return;
    }
    FS_InitFile(&file);
    if (!FS_OpenFile(&file, "graphic/tmap_gra.narc")) {
        NitroPort_Log("[lowermap] can't open graphic/tmap_gra.narc: no map");
        return;
    }
    size = FS_GetLength(&file);
    G.narc = size > 0 && size < (1u << 20) ? malloc(size) : NULL;
    if (G.narc == NULL || FS_ReadFile(&file, G.narc, (s32)size) != (s32)size) {
        FS_CloseFile(&file);
        free(G.narc);
        G.narc = NULL;
        NitroPort_Log("[lowermap] can't read graphic/tmap_gra.narc: no map");
        return;
    }
    FS_CloseFile(&file);
    ok = read_tiles(size, M_TOP_TILES, &G.top) && read_tiles(size, M_ZOOM_TILES, &G.zoom) &&
         read_tiles(size, M_CURSOR, &G.cursor) && read_tiles(size, M_PLAYER, &G.player) &&
         read_tiles(size, M_DOT, &G.dot) && read_tiles(size, M_BLOCKS, &G.blocks) &&
         read_palettes(size, M_TOP_PAL, G.topPal) && read_palettes(size, M_ZOOM_PAL, G.zoomPal) &&
         read_palettes(size, M_OBJ_PAL, G.objPal) && read_screen(size, M_TOP_BG, &G.topBg) &&
         read_screen(size, M_TOP_MAP, &G.topMap) && read_screen(size, M_HIDDEN_TOP, &G.hiddenTop) &&
         read_screen(size, M_ZOOM_BG, &G.zoomBg) && read_screen(size, M_ZOOM_MAP, &G.zoomMap) &&
         read_screen(size, M_HIDDEN_ZOOM, &G.hiddenZoom) && G.topBg.w <= 32 && G.topBg.h <= 32 &&
         G.topMap.w <= 32 && G.topMap.h <= 32 && G.zoomBg.w == 64 && G.zoomBg.h == 64 && G.zoomMap.w == 64 &&
         G.zoomMap.h == 64 && G.cursor.count >= 8 && G.player.count >= 8 && G.dot.count >= 1 && G.blocks.count >= 28;
    if (!ok) {
        free(G.narc);
        G.narc = NULL;
        NitroPort_Log("[lowermap] tmap_gra.narc not as expected: no map");
        return;
    }
    G.state = 1;
}

/* ---------------------------------------------------- the tilemaps made */

/* (bg_window.c's CalcTilemapIndexFromCoords for a 256 x 256 or a 512 x 512
 * BG, and CalcTilemapIndexFromCoordsWidthHeight: a rectangle of 32 x 32
 * blocks) */
static int index512(int x, int y)
{
    return ((x >> 5) + (y >> 5) * 2) * 1024 + (y & 31) * 32 + (x & 31);
}

static int index_wh(int x, int y, int width, int height)
{
    int adjustedWidth = width - 32, adjustedHeight = height - 32, i = 0;

    switch ((x / 32 ? 1 : 0) + (y / 32 ? 2 : 0)) {
    case 0:
        i = adjustedWidth >= 0 ? y * 32 + x : y * width + x;
        break;
    case 1:
        i = (adjustedHeight >= 0 ? 1024 : 32 * height) + y * adjustedWidth + (x & 31);
        break;
    case 2:
        i = width * 32 + (adjustedWidth >= 0 ? (y & 31) * 32 + x : (y & 31) * width + x);
        break;
    default:
        i = width * 32 + adjustedHeight * 32 + (y & 31) * adjustedWidth + (x & 31);
        break;
    }
    return i;
}

/* Bg_CopyToTilemapRect onto a 256 x 256 BG (the source flat). */
static void copy_flat(uint16_t *dest, const Screen *src, const Rect *r)
{
    int i, j;

    for (i = 0; i < r->h; i++) {
        if (r->destY + i >= 32 || r->srcY + i >= src->h) {
            break;
        }
        for (j = 0; j < r->w; j++) {
            if (r->destX + j >= 32 || r->srcX + j >= src->w) {
                break;
            }
            dest[(r->destY + i) * 32 + r->destX + j] = entry(src, (r->srcY + i) * src->w + r->srcX + j);
        }
    }
}

/* Bg_CopyRectToTilemapRect onto a 512 x 512 BG (the source in blocks). */
static void copy_rect512(uint16_t *dest, const Screen *src, const Rect *r)
{
    int i, j;

    for (i = 0; i < r->h; i++) {
        if (r->destY + i >= 64 || r->srcY + i >= src->h) {
            break;
        }
        for (j = 0; j < r->w; j++) {
            int k;

            if (r->destX + j >= 64 || r->srcX + j >= src->w) {
                break;
            }
            k = index_wh(r->srcX + j, r->srcY + i, src->w, src->h);
            if (k >= 0 && k < src->w * src->h) {
                dest[index512(r->destX + j, r->destY + i)] = entry(src, k);
            }
        }
    }
}

static void make_tilemaps(void)
{
    int i, k;

    /* (TownMap's LoadMapGraphics: the upper screen's two, 32 x 24, the rows
     * below them clear) */
    memset(sTopBg, 0, sizeof(sTopBg));
    memset(sTopMap, 0, sizeof(sTopMap));
    for (i = 0; i < G.topBg.w * G.topBg.h; i++) {
        sTopBg[(i / G.topBg.w) * 32 + i % G.topBg.w] = entry(&G.topBg, i);
    }
    for (i = 0; i < G.topMap.w * G.topMap.h; i++) {
        sTopMap[(i / G.topMap.w) * 32 + i % G.topMap.w] = entry(&G.topMap, i);
    }
    /* (not the Fly screen: the arrow between its text and the place's name
     * taken out, BG3's tiles at (16, 21) copied from (0, 21)) */
    {
        static const Rect arrow = { 0, 21, 16, 21, 2, 3 };

        copy_flat(sTopBg, &G.topBg, &arrow);
    }
    for (i = 0; i < 64 * 64; i++) {
        sZoomBg[i] = entry(&G.zoomBg, i);
        sZoomMap[i] = entry(&G.zoomMap, i);
    }
    for (k = 0; k < HIDDEN_COUNT; k++) {
        if (!((sMap.hidden >> k) & 1)) {
            continue;
        }
        copy_flat(sTopMap, &G.hiddenTop, &sHidden[k][1]);
        copy_rect512(sZoomMap, &G.hiddenZoom, &sHidden[k][3]);
        if (k != HIDDEN_SPRING_PATH) {
            copy_flat(sTopBg, &G.hiddenTop, &sHidden[k][0]);
            copy_rect512(sZoomBg, &G.hiddenZoom, &sHidden[k][2]);
        }
    }
}

/* ------------------------------------------------------ the canvas drawn */

/* A row of a text BG: n pixels from (mx, my) to the right, each its colour
 * index | palette << 4, or 0 (nothing), a tile's entry read once for its 8.
 * wrap: the 512 x 512 zoomed-in map's (its blocks, x wrapping round); else
 * mapW tiles wide. */
static void bg_row(const uint16_t *map, int mapW, const Tiles *t, int mx, int my, int n, uint8_t *out, int wrap)
{
    int py = my & 7, i = 0;

    while (i < n) {
        int tx = mx >> 3, px = mx & 7;
        uint16_t e = wrap ? map[index512(tx, my >> 3)] : map[(my >> 3) * mapW + tx];
        uint32_t tile = e & 0x3FFu;
        int pal = (e >> 12) << 4, hflip = (e & 0x400) != 0;
        const uint8_t *row = tile < t->count ? t->data + tile * 32 + ((e & 0x800) ? 7 - py : py) * 4 : NULL;

        for (; px < 8 && i < n; px++, i++) {
            int sx = hflip ? 7 - px : px, v;

            if (row == NULL) {
                out[i] = 0;
                continue;
            }
            v = row[sx >> 1];
            v = (sx & 1) ? v >> 4 : v & 15;
            out[i] = v ? (uint8_t)(v | pal) : 0;
        }
        mx = wrap ? (mx + 8 - (mx & 7)) & 511 : mx + 8 - (mx & 7);
    }
}

static uint16_t blend(uint16_t a, uint16_t b)
{
    /* BG2 at 16/16 (the game's 28, as the DS takes it), what's behind at 4/16 */
    uint16_t c = 0;
    int k;

    for (k = 0; k < 15; k += 5) {
        int v = (((a >> k) & 31) * 16 + ((b >> k) & 31) * 4) / 16;

        c |= (uint16_t)((v > 31 ? 31 : v) << k);
    }
    return c;
}

/* The canvas's background: the upper screen's rows (they change only with
 * the hidden places shown), then the zoomed-in band's (with its scroll and
 * the description). */
static void draw_back_top(void)
{
    uint8_t rowA[CW], rowB[CW];
    int x, y;

    for (y = 0; y < TOP_H; y++) {
        bg_row(sTopMap, 32, &G.top, 0, y, CW, rowA, 0);
        bg_row(sTopBg, 32, &G.top, 0, y, CW, rowB, 0);
        for (x = 0; x < CW; x++) {
            int a = rowA[x], b = rowB[x];
            uint16_t c;

            if (a && b) {
                c = blend(G.topPal[a >> 4][a & 15], G.topPal[b >> 4][b & 15]);
            } else if (a) {
                c = G.topPal[a >> 4][a & 15];
            } else if (b) {
                c = G.topPal[b >> 4][b & 15];
            } else {
                c = G.topPal[0][0];
            }
            sBack[y * CW + x] = c;
        }
    }
}

static void draw_back_band(void)
{
    uint8_t rowA[CW], rowB[CW];
    int x, y;

    for (y = TOP_H; y < CH; y++) {
        int gy = y - TOP_H + BAND_ROW0, my = (gy + sMap.zoomY) & 511;

        bg_row(sZoomMap, 64, &G.zoom, sMap.zoomX & 511, my, CW, rowA, 1);
        bg_row(sZoomBg, 64, &G.zoom, sMap.zoomX & 511, my, CW, rowB, 1);
        for (x = 0; x < CW; x++) {
            int a = rowA[x], b = a ? 0 : rowB[x];
            uint16_t c = a ? G.zoomPal[a >> 4][a & 15] : b ? G.zoomPal[b >> 4][b & 15] : G.zoomPal[0][0];

            /* the description's window over it */
            if (sMap.desc != NULL) {
                int dx = x - sMap.descX, dy = gy - sMap.descY;

                if (dx >= 0 && dx < sMap.descW && dy >= 0 && dy < sMap.descH && (sDesc[dy * sMap.descW + dx] & 0x8000)) {
                    c = sDesc[dy * sMap.descW + dx] & 0x7FFF;
                }
            }
            sBack[y * CW + x] = c;
        }
    }
}

/* A sprite's pixels within a rectangle of the canvas (x0-x1, y0-y1, the
 * last ones in it): its middle at (cx, cy), size x size, its tiles from
 * first (one after another, 1D), its palette. */
static void draw_sprite(const Tiles *t, uint32_t first, int size, int cx, int cy, int palette, int x0, int y0, int x1, int y1)
{
    int left = cx - size / 2, top = cy - size / 2, x, y;

    for (y = top < y0 ? y0 : top; y < top + size && y <= y1 && y < TOP_H; y++) {
        for (x = left < x0 ? x0 : left; x < left + size && x <= x1 && x < CW; x++) {
            int sx = x - left, sy = y - top;
            uint32_t tile = first + (uint32_t)((sy >> 3) * (size >> 3) + (sx >> 3));
            int v;

            if (x < 0 || y < 0 || tile >= t->count) {
                continue;
            }
            v = t->data[tile * 32 + (sy & 7) * 4 + (sx & 7) / 2];
            v = (sx & 1) ? v >> 4 : v & 15;
            if (v) {
                sCanvas[y * CW + x] = G.objPal[palette & 15][v];
            }
        }
    }
}

static void sprite_of(const NitroLowerMapSprite *s, const Tiles **t, uint32_t *first)
{
    switch (s->kind) {
    case NITRO_LOWERMAP_CURSOR:
        *t = &G.cursor;
        *first = (uint32_t)sCursorFrame * 4u;
        break;
    case NITRO_LOWERMAP_PLAYER:
        *t = &G.player;
        *first = (s->frame ? 1u : 0u) * 4u;
        break;
    default:
        *t = &G.blocks;
        *first = (uint32_t)(s->frame > 6 ? 6 : s->frame) * 4u;
        break;
    }
}

/* The canvas's rectangle (x0-x1, y0-y1, the last ones in it) drawn again:
 * its background, the sprites (the blocks, the dot shown, the cursor and
 * the head: as the Town Map's OAM has them, the front one last), the name
 * over everything. */
static void draw_rect(int x0, int y0, int x1, int y1)
{
    int x, y, i;

    x0 = x0 < 0 ? 0 : x0;
    y0 = y0 < 0 ? 0 : y0;
    x1 = x1 >= CW ? CW - 1 : x1;
    y1 = y1 >= CH ? CH - 1 : y1;
    if (x0 > x1 || y0 > y1) {
        return;
    }
    for (y = y0; y <= y1; y++) {
        memcpy(sCanvas + y * CW + x0, sBack + y * CW + x0, (size_t)(x1 - x0 + 1) * 2);
    }
    if (y0 < TOP_H) {
        for (i = sMap.sprites - 1; i >= 0; i--) {
            const Tiles *t;
            uint32_t first;

            if (sMap.sprite[i].kind == NITRO_LOWERMAP_BLOCK) {
                sprite_of(&sMap.sprite[i], &t, &first);
                draw_sprite(t, first, 16, sMap.sprite[i].x, sMap.sprite[i].y, sMap.sprite[i].palette, x0, y0, x1, y1);
            }
        }
        if (sDotShown >= 0 && sDotShown < sMap.dots) {
            draw_sprite(&G.dot, 0, 8, sMap.dot[sDotShown].x, sMap.dot[sDotShown].y, 2, x0, y0, x1, y1);
        }
        for (i = sMap.sprites - 1; i >= 0; i--) {
            const Tiles *t;
            uint32_t first;

            if (sMap.sprite[i].kind != NITRO_LOWERMAP_BLOCK) {
                sprite_of(&sMap.sprite[i], &t, &first);
                draw_sprite(t, first, 16, sMap.sprite[i].x, sMap.sprite[i].y, sMap.sprite[i].palette, x0, y0, x1, y1);
            }
        }
        if (sMap.name != NULL) {
            for (y = y0; y <= y1 && y < TOP_H; y++) {
                int ny = y - sMap.nameY;

                if (ny < 0 || ny >= sMap.nameH) {
                    continue;
                }
                for (x = x0; x <= x1; x++) {
                    int nx = x - sMap.nameX;

                    if (nx >= 0 && nx < sMap.nameW && (sName[ny * sMap.nameW + nx] & 0x8000)) {
                        sCanvas[y * CW + x] = sName[ny * sMap.nameW + nx] & 0x7FFF;
                    }
                }
            }
        }
    }
}

/* ---------------------------------------------------- the panel's picture */

/* For each panel column (row): the canvas's two pixels its texel pair
 * comes from, and the second one's weight (0-256), as the GPU samples the
 * canvas doubled (each pixel 2 x 2 texels) at 15/16. */
static struct { short c0, c1; short w; } sCols[PW], sRows[PH];
static int sTables;

static void make_tables(void)
{
    int i;

    for (i = 0; i < PW + PH; i++) {
        int across = i < PW, n = across ? PW : PH, size = across ? CW : CH, k = across ? i : i - PW;
        /* the texel coordinate of the pixel's middle */
        int64_t t256 = ((int64_t)(2 * k + 1) * 2 * size * 256) / (2 * n) - 128;
        int t0 = (int)(t256 >> 8), w = (int)(t256 & 255);
        int c0 = t0 >> 1, c1 = (t0 + 1) >> 1;

        c0 = c0 < 0 ? 0 : c0 >= size ? size - 1 : c0;
        c1 = c1 < 0 ? 0 : c1 >= size ? size - 1 : c1;
        if (across) {
            sCols[k].c0 = (short)c0;
            sCols[k].c1 = (short)c1;
            sCols[k].w = (short)w;
        } else {
            sRows[k].c0 = (short)c0;
            sRows[k].c1 = (short)c1;
            sRows[k].w = (short)w;
        }
    }
    sTables = 1;
}

/* An RGB555 pixel's channels widened to 8 bits: r | g << 8 | b << 16. */
static uint32_t widen(uint16_t a)
{
    uint32_t r = a & 31, g = (a >> 5) & 31, b = (a >> 10) & 31;

    return ((r << 3) | (r >> 2)) | (((g << 3) | (g >> 2)) << 8) | (((b << 3) | (b >> 2)) << 16);
}

/* Two widened pixels mixed, b at w / 256, each channel rounded down: r and
 * b in one multiply, g in another (each channel's sum fits its 16 bits). */
static uint32_t mix(uint32_t a, uint32_t b, uint32_t w)
{
    uint32_t rb = (((a & 0x00FF00FFu) * (256u - w) + (b & 0x00FF00FFu) * w) >> 8) & 0x00FF00FFu;
    uint32_t g = (((a & 0x0000FF00u) * (256u - w) + (b & 0x0000FF00u) * w) >> 8) & 0x0000FF00u;

    return rb | g;
}

/* Two canvas rows widened (columns cx0-cx1), the last ones asked for. */
static uint32_t sWide[2][CW];
static int sWideRow[2] = { -1, -1 }, sWideX0[2], sWideX1[2];

/* Canvas row `row` widened over columns cx0-cx1; `keep` (the other row the
 * caller wants now) not thrown out for it. */
static const uint32_t *widened_row(int row, int keep, int cx0, int cx1)
{
    int k, x;

    for (k = 0; k < 2; k++) {
        if (sWideRow[k] == row && sWideX0[k] <= cx0 && sWideX1[k] >= cx1) {
            return sWide[k];
        }
    }
    k = sWideRow[0] == keep && keep != row ? 1 : 0;
    for (x = cx0; x <= cx1; x++) {
        sWide[k][x] = widen(sCanvas[row * CW + x]);
    }
    sWideRow[k] = row;
    sWideX0[k] = cx0;
    sWideX1[k] = cx1;
    return sWide[k];
}

/* The panel's pixels over the canvas's rectangle (x0-x1, y0-y1) drawn again. */
static void scale_rect(int x0, int y0, int x1, int y1)
{
    int x, y, ya = 0, yb = PH - 1, xa = 0, xb = PW - 1, cx0, cx1;

    sWideRow[0] = sWideRow[1] = -1;   /* (the canvas may have changed since) */

    if (!sTables) {
        make_tables();
    }
    /* (the panel's rows and columns over it: the tables go up with them) */
    while (ya < PH && sRows[ya].c1 < y0) {
        ya++;
    }
    while (yb >= ya && sRows[yb].c0 > y1) {
        yb--;
    }
    while (xa < PW && sCols[xa].c1 < x0) {
        xa++;
    }
    while (xb >= xa && sCols[xb].c0 > x1) {
        xb--;
    }
    if (ya > yb || xa > xb) {
        return;
    }
    /* (the canvas's rows widened once each, over the columns used: the
     * next panel row mostly uses the same two) */
    cx0 = sCols[xa].c0;
    cx1 = sCols[xb].c1;
    for (y = ya; y <= yb; y++) {
        const uint32_t *r0 = widened_row(sRows[y].c0, sRows[y].c1, cx0, cx1);
        const uint32_t *r1 = widened_row(sRows[y].c1, sRows[y].c0, cx0, cx1);
        uint32_t wy = (uint32_t)sRows[y].w, *out = sPanel + y * PW;

        for (x = xa; x <= xb; x++) {
            uint32_t wx = (uint32_t)sCols[x].w, c0 = (uint32_t)sCols[x].c0, c1 = (uint32_t)sCols[x].c1;
            uint32_t top = mix(r0[c0], r0[c1], wx), bottom = mix(r1[c0], r1[c1], wx);
            /* (the rows mixed, rounded to nearest) */
            uint32_t rb = ((top & 0x00FF00FFu) * (256u - wy) + (bottom & 0x00FF00FFu) * wy + 0x00800080u) >> 8 & 0x00FF00FFu;
            uint32_t g = ((top & 0x0000FF00u) * (256u - wy) + (bottom & 0x0000FF00u) * wy + 0x00008000u) >> 8 & 0x0000FF00u;
            uint32_t c = rb | g;

            /* (r << 24 | g << 16 | b << 8 | 255: c has r in its low byte) */
            out[x] = ((c & 255u) << 24) | (((c >> 8) & 255u) << 16) | (((c >> 16) & 255u) << 8) | 255u;
        }
    }
    sDirtyY0 = ya < sDirtyY0 ? ya : sDirtyY0;
    sDirtyY1 = yb + 1 > sDirtyY1 ? yb + 1 : sDirtyY1;
}

/* A rectangle of the canvas drawn again, and the panel over it. */
static void redraw_rect(int x0, int y0, int x1, int y1)
{
    draw_rect(x0, y0, x1, y1);
    scale_rect(x0, y0, x1, y1);
}

/* ------------------------------------------------------------------ API */

static void sprite_box(const NitroLowerMapSprite *s, int *x0, int *y0, int *x1, int *y1)
{
    *x0 = s->x - 8;
    *y0 = s->y - 8;
    *x1 = s->x + 7;
    *y1 = s->y + 7;
}

static void update_animations(int *changed)
{
    int cursor = (int)((sFrame / CURSOR_FRAMES) & 1u), dot = -1, i;

    if (sMap.dots > 0) {
        /* (the Town Map's: the last one first, then each one before it in
         * turn, every DOT_FRAMES) */
        dot = (int)((uint32_t)(sMap.dots - 1) - (sFrame / DOT_FRAMES) % (uint32_t)sMap.dots);
    }
    if (cursor != sCursorFrame) {
        sCursorFrame = cursor;
        for (i = 0; i < sMap.sprites; i++) {
            if (sMap.sprite[i].kind == NITRO_LOWERMAP_CURSOR) {
                int x0, y0, x1, y1;

                sprite_box(&sMap.sprite[i], &x0, &y0, &x1, &y1);
                draw_rect(x0, y0, x1, y1);
                scale_rect(x0, y0, x1, y1);
                *changed = 1;
            }
        }
    }
    if (dot != sDotShown) {
        int old = sDotShown;

        sDotShown = dot;
        if (old >= 0 && old < sMap.dots) {
            draw_rect(sMap.dot[old].x - 4, sMap.dot[old].y - 4, sMap.dot[old].x + 3, sMap.dot[old].y + 3);
            scale_rect(sMap.dot[old].x - 4, sMap.dot[old].y - 4, sMap.dot[old].x + 3, sMap.dot[old].y + 3);
        }
        if (dot >= 0) {
            draw_rect(sMap.dot[dot].x - 4, sMap.dot[dot].y - 4, sMap.dot[dot].x + 3, sMap.dot[dot].y + 3);
            scale_rect(sMap.dot[dot].x - 4, sMap.dot[dot].y - 4, sMap.dot[dot].x + 3, sMap.dot[dot].y + 3);
        }
        *changed = 1;
    }
}

/* Whether a text's window changes: shown or not, where, its pixels (the
 * old ones in buf). */
static int text_changes(const uint16_t *was, int wasX, int wasY, int wasW, int wasH, const uint16_t *now, int x, int y,
                        int w, int h, int fits)
{
    if ((was != NULL) != (now != NULL && fits)) {
        return 1;
    }
    if (was == NULL) {
        return 0;
    }
    return wasX != x || wasY != y || wasW != w || wasH != h || memcmp(was, now, (size_t)w * (size_t)h * 2) != 0;
}

static int sprite_same(const NitroLowerMapSprite *a, const NitroLowerMapSprite *b)
{
    return a->kind == b->kind && a->x == b->x && a->y == b->y && a->frame == b->frame && a->palette == b->palette;
}

int NitroPort_LowerMapLoad(void)
{
    if (G.state == 0) {
        load();
    }
    return G.state > 0;
}

int NitroPort_LowerMapLoaded(void)
{
    return G.state > 0;
}

int NitroPort_LowerMapSet(const NitroLowerMap *map)
{
    NitroLowerMap old;
    int changed = 0, full, band, nameChanges, descChanges, dotsChange, i, n;

    if (G.state <= 0 || map == NULL) {
        return 0;   /* (not read: NitroPort_LowerMapLoad, on the game's thread) */
    }
    old = sMap;
    nameChanges = text_changes(old.name, old.nameX, old.nameY, old.nameW, old.nameH, map->name, map->nameX, map->nameY,
                               map->nameW, map->nameH, map->nameW > 0 && map->nameH > 0 && map->nameW <= 256 && map->nameH <= 64);
    descChanges = text_changes(old.desc, old.descX, old.descY, old.descW, old.descH, map->desc, map->descX, map->descY,
                               map->descW, map->descH, map->descW > 0 && map->descH > 0 && map->descW <= 256 && map->descH <= 192);
    sMap = *map;
    sMap.sprites = sMap.sprites < 0 ? 0 : sMap.sprites > NITRO_LOWERMAP_MAX_SPRITES ? NITRO_LOWERMAP_MAX_SPRITES : sMap.sprites;
    sMap.dots = sMap.dots < 0 ? 0 : sMap.dots > NITRO_LOWERMAP_MAX_DOTS ? NITRO_LOWERMAP_MAX_DOTS : sMap.dots;
    /* (the text copied, if it fits: the name's window up to 256 x 64, the
     * description's up to 256 x 192) */
    sMap.name = NULL;
    if (map->name != NULL && map->nameW > 0 && map->nameH > 0 && map->nameW <= 256 && map->nameH <= 64) {
        memcpy(sName, map->name, (size_t)map->nameW * (size_t)map->nameH * 2);
        sMap.name = sName;
    }
    sMap.desc = NULL;
    if (map->desc != NULL && map->descW > 0 && map->descH > 0 && map->descW <= 256 && map->descH <= 192) {
        memcpy(sDesc, map->desc, (size_t)map->descW * (size_t)map->descH * 2);
        sMap.desc = sDesc;
    }
    full = !sReady || sMap.hidden != old.hidden;
    band = full || descChanges || sMap.zoomX != old.zoomX || sMap.zoomY != old.zoomY;
    dotsChange = sMap.dots != old.dots || memcmp(sMap.dot, old.dot, (size_t)sMap.dots * sizeof(sMap.dot[0])) != 0;
    if (full) {
        make_tilemaps();
        draw_back_top();
    }
    if (band) {
        draw_back_band();
    }
    if (full) {
        sCursorFrame = (int)((sFrame / CURSOR_FRAMES) & 1u);
        sDotShown = -1;
        update_animations(&changed);
        redraw_rect(0, 0, CW - 1, CH - 1);
        sReady = 1;
        return 1;
    }
    /* Only what changed: the dot shown, chosen again when the dots change
     * (first: the boxes below would draw it at its new place otherwise);
     * the band; each sprite where it was and where it is; the name's
     * window, before and after. */
    if (dotsChange) {
        int was = sDotShown;

        sDotShown = -1;
        if (was >= 0 && was < old.dots) {
            redraw_rect(old.dot[was].x - 4, old.dot[was].y - 4, old.dot[was].x + 3, old.dot[was].y + 3);
        }
    }
    if (band) {
        redraw_rect(0, TOP_H, CW - 1, CH - 1);
    }
    n = sMap.sprites > old.sprites ? sMap.sprites : old.sprites;
    for (i = 0; i < n; i++) {
        int x0, y0, x1, y1;

        if (i < sMap.sprites && i < old.sprites && sprite_same(&sMap.sprite[i], &old.sprite[i])) {
            continue;
        }
        if (i < old.sprites) {
            sprite_box(&old.sprite[i], &x0, &y0, &x1, &y1);
            redraw_rect(x0, y0, x1, y1);
        }
        if (i < sMap.sprites) {
            sprite_box(&sMap.sprite[i], &x0, &y0, &x1, &y1);
            redraw_rect(x0, y0, x1, y1);
        }
    }
    if (nameChanges) {
        if (old.name != NULL) {
            redraw_rect(old.nameX, old.nameY, old.nameX + old.nameW - 1, old.nameY + old.nameH - 1);
        }
        if (sMap.name != NULL) {
            redraw_rect(sMap.nameX, sMap.nameY, sMap.nameX + sMap.nameW - 1, sMap.nameY + sMap.nameH - 1);
        }
    }
    update_animations(&changed);
    return 1;
}

void NitroPort_LowerMapTakeDirty(int *y0, int *y1)
{
    *y0 = sDirtyY0;
    *y1 = sDirtyY1;
    sDirtyY0 = PH;
    sDirtyY1 = 0;
}

int NitroPort_LowerMapSetFrame(uint32_t frame)
{
    int changed = 0;

    sFrame = frame;
    if (sReady) {
        update_animations(&changed);
    }
    return changed;
}

int NitroPort_LowerMapFrameChanges(uint32_t frame)
{
    return frame % CURSOR_FRAMES == 0 || frame % DOT_FRAMES == 0;
}

const uint32_t *NitroPort_LowerMapPanel(void)
{
    return sPanel;
}

const uint16_t *NitroPort_LowerMapCanvas(void)
{
    return sCanvas;
}
