/*
 * Pixel-art upscaling (Scale2x) and PICA texture tiling: see
 * include/nitro_port_upscale.h. Written for the 3DS's CPU: a tile at a
 * time, each input pixel read once per use, no per-pixel index arithmetic
 * (the Morton offsets within a tile are a table).
 */
#include <nitro_port_upscale.h>
#include <string.h>

/* The Morton offset of texel (x, y) within an 8x8 tile. */
static const uint8_t sMorton[8][8] = {
    { 0, 1, 4, 5, 16, 17, 20, 21 },
    { 2, 3, 6, 7, 18, 19, 22, 23 },
    { 8, 9, 12, 13, 24, 25, 28, 29 },
    { 10, 11, 14, 15, 26, 27, 30, 31 },
    { 32, 33, 36, 37, 48, 49, 52, 53 },
    { 34, 35, 38, 39, 50, 51, 54, 55 },
    { 40, 41, 44, 45, 56, 57, 60, 61 },
    { 42, 43, 46, 47, 58, 59, 62, 63 },
};

/* Scale2x: the 2x2 of input pixel (i, j) of a 4x4 block is at Morton
 * offsets 4 * m + 0..3 (top left, top right, bottom left, bottom right),
 * where m is (i, j)'s Morton index in the 4x4: the pair's low bits are
 * the output's lowest x and y bits. */
static const uint8_t sMorton4[4][4] = {
    { 0, 1, 4, 5 },
    { 2, 3, 6, 7 },
    { 8, 9, 12, 13 },
    { 10, 11, 14, 15 },
};

#define DEFINE_TILE(NAME, T)                                                   \
    void NAME(const T *src, int w, int h, T *dst, int dstWidth)                \
    {                                                                          \
        int tilesPerRow = dstWidth / 8, tx, ty, x, y;                          \
        for (ty = 0; ty < h / 8; ty++) {                                       \
            for (tx = 0; tx < w / 8; tx++) {                                   \
                T *tile = dst + (ty * tilesPerRow + tx) * 64;                  \
                const T *s = src + (ty * 8) * w + tx * 8;                      \
                for (y = 0; y < 8; y++) {                                      \
                    const uint8_t *m = sMorton[y];                             \
                    for (x = 0; x < 8; x++) {                                  \
                        tile[m[x]] = s[x];                                     \
                    }                                                          \
                    s += w;                                                    \
                }                                                              \
            }                                                                  \
        }                                                                      \
    }

DEFINE_TILE(NitroPort_Tile16, uint16_t)
DEFINE_TILE(NitroPort_Tile32, uint32_t)

/* One 4x4 block of input pixels into one 8x8 tile. up/down: the rows
 * above and below the block's (the edge row again at the picture's edge);
 * the columns beyond the block's sides are read from the row itself
 * (clamped at the picture's edge). */
#define DEFINE_SCALE2X(NAME, T)                                                \
    static void NAME##Block(const T *rows[6], int x0, int w, T *tile)          \
    {                                                                          \
        int i, j;                                                              \
        for (j = 0; j < 4; j++) {                                              \
            const T *up = rows[j], *row = rows[j + 1], *down = rows[j + 2];    \
            const uint8_t *m = sMorton4[j];                                    \
            for (i = 0; i < 4; i++) {                                          \
                int x = x0 + i;                                                \
                T it = row[x];                                                 \
                T u = up[x], d = down[x];                                      \
                T l = row[x > 0 ? x - 1 : 0], r = row[x < w - 1 ? x + 1 : w - 1]; \
                T *out = tile + m[i] * 4;                                      \
                out[0] = (u == l && u != r && l != d) ? u : it;                \
                out[1] = (u == r && u != l && r != d) ? r : it;                \
                out[2] = (d == l && d != r && l != u) ? l : it;                \
                out[3] = (d == r && d != l && r != u) ? r : it;                \
            }                                                                  \
        }                                                                      \
    }                                                                          \
    void NAME(const T *src, int w, int h, T *dst, int dstWidth)                \
    {                                                                          \
        int tilesPerRow = dstWidth / 8, bx, by, k;                             \
        for (by = 0; by < h / 4; by++) {                                       \
            const T *rows[6];                                                  \
            for (k = 0; k < 6; k++) {                                          \
                int y = by * 4 + k - 1;                                        \
                rows[k] = src + (y < 0 ? 0 : y >= h ? h - 1 : y) * w;          \
            }                                                                  \
            for (bx = 0; bx < w / 4; bx++) {                                   \
                NAME##Block(rows, bx * 4, w, dst + (by * tilesPerRow + bx) * 64); \
            }                                                                  \
        }                                                                      \
    }

DEFINE_SCALE2X(NitroPort_Scale2xTiled16, uint16_t)
DEFINE_SCALE2X(NitroPort_Scale2xTiled32, uint32_t)

/* Nearest 2x: source pixel (x, y) is the 2x2 block at (2x, 2y), which in a
 * tile's Morton order is four texels in a row (the lowest bits are x's and
 * y's first): a 4x4 block of source pixels fills one 8x8 tile, the pixel
 * (i, j) of the block at texels 4 m .. 4 m + 3, m its index in sMorton4.
 * Rows y0 .. y1 of the source (a band of whole tile rows), with the
 * column right of the picture for those rows and, when the band is the
 * last, the row below it and their corner. */
#define DEFINE_DOUBLE(NAME, T)                                                 \
    void NAME##Rows(const T *src, int w, int h, int y0, int y1, T *dst, int dstWidth) \
    {                                                                          \
        int tilesPerRow = dstWidth / 8, bx, by, x, y;                          \
        for (by = y0 / 4; by < y1 / 4; by++) {                                 \
            const T *s = src + (by * 4) * w;                                   \
            T *tiles = dst + (by * tilesPerRow) * 64;                          \
            for (bx = 0; bx < w / 4; bx++, s += 4, tiles += 64) {              \
                int j;                                                         \
                for (j = 0; j < 4; j++) {                                      \
                    const T *row = s + j * w;                                  \
                    const uint8_t *m = sMorton4[j];                            \
                    T *o;                                                      \
                    o = tiles + m[0] * 4; o[0] = o[1] = o[2] = o[3] = row[0];  \
                    o = tiles + m[1] * 4; o[0] = o[1] = o[2] = o[3] = row[1];  \
                    o = tiles + m[2] * 4; o[0] = o[1] = o[2] = o[3] = row[2];  \
                    o = tiles + m[3] * 4; o[0] = o[1] = o[2] = o[3] = row[3];  \
                }                                                              \
            }                                                                  \
        }                                                                      \
        /* the edges repeated: the column right of the picture, the row     \
         * below it, and their corner */                                     \
        for (y = 2 * y0; y < 2 * y1 && 2 * w < dstWidth; y++) {                \
            dst[NitroPort_TiledIndex((unsigned)(2 * w), (unsigned)y, (unsigned)dstWidth)] = \
                dst[NitroPort_TiledIndex((unsigned)(2 * w - 1), (unsigned)y, (unsigned)dstWidth)]; \
        }                                                                      \
        for (x = 0; y1 == h && x <= 2 * w && x < dstWidth; x++) {              \
            dst[NitroPort_TiledIndex((unsigned)x, (unsigned)(2 * h), (unsigned)dstWidth)] = \
                dst[NitroPort_TiledIndex((unsigned)x, (unsigned)(2 * h - 1), (unsigned)dstWidth)]; \
        }                                                                      \
    }                                                                          \
    void NAME(const T *src, int w, int h, T *dst, int dstWidth)                \
    {                                                                          \
        NAME##Rows(src, w, h, 0, h, dst, dstWidth);                            \
    }

DEFINE_DOUBLE(NitroPort_Double2xTiled16, uint16_t)
DEFINE_DOUBLE(NitroPort_Double2xTiled32, uint32_t)

void NitroPort_Double2xTiled16TableRows(const uint16_t *src, int w, int h, int y0, int y1, uint16_t *dst,
                                        int dstWidth, const uint16_t *table)
{
    typedef uint64_t __attribute__((may_alias, aligned(8))) u64a;
    int tilesPerRow = dstWidth / 8, bx, by, x, y;

    for (by = y0 / 4; by < y1 / 4; by++) {
        const uint16_t *s = src + (by * 4) * w;
        uint16_t *tiles = dst + (by * tilesPerRow) * 64;

        for (bx = 0; bx < w / 4; bx++, s += 4, tiles += 64) {
            int j;

            for (j = 0; j < 4; j++) {
                const uint16_t *row = s + j * w;
                const uint8_t *m = sMorton4[j];
                int i;

                for (i = 0; i < 4; i++) {
                    uint64_t t = table[row[i] & 0x7FFF];

                    t |= t << 16;
                    *(u64a *)(tiles + m[i] * 4) = t | (t << 32);
                }
            }
        }
    }
    /* the edges repeated, as NitroPort_Double2xTiled16Rows */
    for (y = 2 * y0; y < 2 * y1 && 2 * w < dstWidth; y++) {
        dst[NitroPort_TiledIndex((unsigned)(2 * w), (unsigned)y, (unsigned)dstWidth)] =
            dst[NitroPort_TiledIndex((unsigned)(2 * w - 1), (unsigned)y, (unsigned)dstWidth)];
    }
    for (x = 0; y1 == h && x <= 2 * w && x < dstWidth; x++) {
        dst[NitroPort_TiledIndex((unsigned)x, (unsigned)(2 * h), (unsigned)dstWidth)] =
            dst[NitroPort_TiledIndex((unsigned)x, (unsigned)(2 * h - 1), (unsigned)dstWidth)];
    }
}

void NitroPort_Double2xTiled16Table(const uint16_t *src, int w, int h, uint16_t *dst, int dstWidth,
                                    const uint16_t *table)
{
    NitroPort_Double2xTiled16TableRows(src, w, h, 0, h, dst, dstWidth, table);
}

/* ------------------------------------------------- publishing what changed */

static uint32_t Widen5(uint32_t v)
{
    return (v << 3) | (v >> 2);
}

uint32_t NitroPort_PremultiplyOver(const uint16_t *over, const uint8_t *alpha, const uint8_t alpha16[17],
                                   uint32_t *out, int i, int j)
{
    uint32_t any = 0;

    for (; i < j; i++) {
        uint16_t c = over[i];
        uint32_t a, r, g, b;

        if (!(c & 0x8000)) {
            out[i] = 0;
            continue;
        }
        a = alpha16[alpha[i] > 16 ? 16 : alpha[i]];
        /* (x / 255, rounded: (x + 128 + ((x + 128) >> 8)) >> 8) */
        r = Widen5(c & 31) * a + 128u;
        g = Widen5((c >> 5) & 31) * a + 128u;
        b = Widen5((c >> 10) & 31) * a + 128u;
        r = (r + (r >> 8)) >> 8;
        g = (g + (g >> 8)) >> 8;
        b = (b + (b >> 8)) >> 8;
        out[i] = (r << 24) | (g << 16) | (b << 8) | a;
        any |= a;
    }
    return any;
}

int NitroPort_OverPartial(const uint16_t *over, const uint8_t *alpha, const uint8_t alpha16[17], int i, int j)
{
    for (; i < j; i++) {
        if (over[i] & 0x8000) {
            uint32_t a = alpha16[alpha[i] > 16 ? 16 : alpha[i]];

            if (a != 0 && a != 255) {
                return 1;
            }
        }
    }
    return 0;
}

void NitroPort_PackOver5551(const uint32_t *premultiplied, uint16_t *out, int i, int j)
{
    for (; i < j; i++) {
        uint32_t p = premultiplied[i];

        out[i] = (p & 0xFFu) ? (uint16_t)(((p >> 27) << 11) | (((p >> 19) & 31u) << 6) | (((p >> 11) & 31u) << 1) | 1u)
                             : 0;
    }
}

int NitroPort_PublishDoubled(NitroPublishShadow *sh, NitroPublishTarget *t, const uint16_t *under,
                             const uint16_t *over, const uint8_t *overAlpha, int hasOver, int w, int h,
                             uint16_t *underTex, uint32_t *overTex, int texW, const uint16_t *table,
                             const uint8_t alpha16[17], void (*flush)(void *data, uint32_t bytes))
{
    const int bands = h / 4;
    /* (the layers in front of the 3D: the DS's lines only, a tall picture's
     * too: past them there's no 3D) */
    const int overBands = (h > 192 ? 192 : h) / 4;
    const size_t band = (size_t)w * 4u;
    const uint32_t tileRow = (uint32_t)texW * 8u;   /* texels in a band's row of tiles (doubled) */
    uint32_t gen = ++sh->gen, any = 0;
    int b, from, known = t->heldBy == sh && t->heldWidth == w && t->heldHeight == h, packed = 0, sameKind;

    if (h <= 0 || h > NITRO_PUBLISH_MAX_H || (h & 3) || (size_t)w * (size_t)h > NITRO_PUBLISH_MAX_PIXELS) {
        return 0;   /* (not a picture it can hold) */
    }
    /* What changed since the last picture. */
    if (!sh->underValid || sh->width != w || sh->height != h) {
        memcpy(sh->under, under, band * bands * 2u);
        for (b = 0; b < bands; b++) {
            sh->underChanged[b] = gen;
        }
        sh->underValid = 1;
        sh->overValid = 0;
        sh->width = w;
        sh->height = h;
    } else {
        for (b = 0; b < bands; b++) {
            if (memcmp(under + band * b, sh->under + band * b, band * 2u) != 0) {
                memcpy(sh->under + band * b, under + band * b, band * 2u);
                sh->underChanged[b] = gen;
            }
        }
    }
    /* The under texture brought up to date: each run of bands changed
     * since it was written, done again (and the row below the picture
     * with the last band). */
    for (b = 0, from = -1; b <= bands; b++) {
        int redo = b < bands && (!known || t->underGen == 0 || sh->underChanged[b] > t->underGen);

        if (redo && from < 0) {
            from = b;
        } else if (!redo && from >= 0) {
            uint32_t rows = (uint32_t)(b - from) + (b == bands ? 1u : 0u);

            NitroPort_Double2xTiled16TableRows(under, w, h, from * 4, b * 4, underTex, texW, table);
            if (flush) {
                flush(underTex + (uint32_t)from * tileRow, rows * tileRow * 2u);
            }
            from = -1;
        }
    }
    t->underGen = gen;
    t->heldBy = sh;
    t->heldWidth = w;
    t->heldHeight = h;
    if (!hasOver) {
        /* (not drawn, nor kept: all of it again when there is one; the
         * over texture holds what it did, maybe another source's) */
        sh->overValid = 0;
        t->overGen = 0;
        return 0;
    }
    if (!sh->overValid) {
        memcpy(sh->over, over, band * overBands * 2u);
        memcpy(sh->alpha, overAlpha, band * overBands);
        for (b = 0; b < overBands; b++) {
            sh->overChanged[b] = gen;
        }
        sh->overValid = 1;
    } else {
        for (b = 0; b < overBands; b++) {
            if (memcmp(over + band * b, sh->over + band * b, band * 2u) != 0 ||
                memcmp(overAlpha + band * b, sh->alpha + band * b, band) != 0) {
                memcpy(sh->over + band * b, over + band * b, band * 2u);
                memcpy(sh->alpha + band * b, overAlpha + band * b, band);
                sh->overChanged[b] = gen;
            }
        }
    }
    /* The bands changed converted now: whether anything's in them, and
     * whether any of it is partly see-through (then RGBA8, else RGBA5551
     * if the target takes it). */
    for (b = 0; b < overBands; b++) {
        if (sh->overChanged[b] == gen) {
            sh->overAny[b] = NitroPort_PremultiplyOver(over, overAlpha, alpha16, sh->premultiplied,
                                                       (int)(band * b), (int)(band * (b + 1))) != 0;
            sh->overPartial[b] = sh->overAny[b] &&
                                 NitroPort_OverPartial(over, overAlpha, alpha16, (int)(band * b), (int)(band * (b + 1)));
        }
    }
    if (t->allow16) {
        packed = 1;
        for (b = 0; b < overBands; b++) {
            if (sh->overPartial[b]) {
                packed = 0;
                break;
            }
        }
    }
    sameKind = t->overGen == 0 || t->over16 == packed;   /* (else all of it again, the other way) */
    for (b = 0, from = -1; b <= overBands; b++) {
        int redo = b < overBands && (!known || !sameKind || t->overGen == 0 || sh->overChanged[b] > t->overGen);

        if (redo && from < 0) {
            from = b;
        } else if (!redo && from >= 0) {
            uint32_t rows = (uint32_t)(b - from) + (b == overBands ? 1u : 0u);
            int k;

            for (k = from; k < b; k++) {
                if (sh->overChanged[k] != gen) {
                    /* (changed before, but not in this target's textures) */
                    (void)NitroPort_PremultiplyOver(over, overAlpha, alpha16, sh->premultiplied,
                                                    (int)(band * k), (int)(band * (k + 1)));
                }
            }
            if (packed) {
                uint16_t *overTex16 = (uint16_t *)(void *)overTex;

                NitroPort_PackOver5551(sh->premultiplied, sh->packed, (int)(band * from), (int)(band * b));
                NitroPort_Double2xTiled16Rows(sh->packed, w, h, from * 4, b * 4, overTex16, texW);
                if (flush) {
                    flush(overTex16 + (uint32_t)from * tileRow, rows * tileRow * 2u);
                }
            } else {
                NitroPort_Double2xTiled32Rows(sh->premultiplied, w, h, from * 4, b * 4, overTex, texW);
                if (flush) {
                    flush(overTex + (uint32_t)from * tileRow, rows * tileRow * 4u);
                }
            }
            from = -1;
        }
    }
    for (b = 0; b < overBands; b++) {
        any |= sh->overAny[b];
    }
    t->overGen = gen;
    t->over16 = packed;
    return any != 0;
}
