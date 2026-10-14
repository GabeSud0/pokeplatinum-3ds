/*
 * Pixel-art upscaling (Scale2x) and PICA texture tiling: see
 * include/nitro_port_upscale.h. Written for the 3DS's CPU: a tile at a
 * time, each input pixel read once per use, no per-pixel index arithmetic
 * (the Morton offsets within a tile are a table).
 */
#include <nitro_port_upscale.h>

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
