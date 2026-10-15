/*
 * Tests for the pixel-art upscaler and texture tiler (src/port_upscale.c),
 * against plain definitions: the tiled index worked out texel by texel,
 * and Scale2x by its rule.
 */
#include <nitro_port_upscale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int sChecks, sFailures;
#define CHECK(cond) do { sChecks++; if (!(cond)) { sFailures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static uint32_t sSeed = 12345;
static uint32_t Rnd(void) { sSeed = sSeed * 1103515245u + 12345u; return sSeed >> 8; }

/* Texel (x, y) of a tiled texture, by the definition in the header. */
static unsigned Index(unsigned x, unsigned y, unsigned width)
{
    unsigned tile = (y / 8) * (width / 8) + x / 8, tx = x & 7, ty = y & 7;
    unsigned m = (tx & 1) | ((ty & 1) << 1) | ((tx & 2) << 1) | ((ty & 2) << 2) | ((tx & 4) << 2) | ((ty & 4) << 3);
    return tile * 64 + m;
}

static void test_tiled_index(void)
{
    /* The first tile's Morton order: (0,0) 0, (1,0) 1, (0,1) 2, (1,1) 3,
     * (2,0) 4, ... (7,7) 63; the second tile starts at 64. */
    CHECK(NitroPort_TiledIndex(0, 0, 512) == 0);
    CHECK(NitroPort_TiledIndex(1, 0, 512) == 1);
    CHECK(NitroPort_TiledIndex(0, 1, 512) == 2);
    CHECK(NitroPort_TiledIndex(1, 1, 512) == 3);
    CHECK(NitroPort_TiledIndex(2, 0, 512) == 4);
    CHECK(NitroPort_TiledIndex(0, 2, 512) == 8);
    CHECK(NitroPort_TiledIndex(4, 0, 512) == 16);
    CHECK(NitroPort_TiledIndex(0, 4, 512) == 32);
    CHECK(NitroPort_TiledIndex(7, 7, 512) == 63);
    CHECK(NitroPort_TiledIndex(8, 0, 512) == 64);
    CHECK(NitroPort_TiledIndex(0, 8, 512) == 64 * 64);
    CHECK(NitroPort_TiledIndex(0, 8, 256) == 64 * 32);
}

static void test_tile(void)
{
    enum { W = 32, H = 16, TW = 64 };
    static uint16_t src[W * H], dst[TW * H];
    static uint32_t src32[W * H], dst32[TW * H];
    int x, y, ok = 1;

    for (y = 0; y < H; y++) {
        for (x = 0; x < W; x++) {
            src[y * W + x] = (uint16_t)(y * W + x + 1);
            src32[y * W + x] = (uint32_t)(y * W + x + 1) * 0x01010101u;
        }
    }
    memset(dst, 0, sizeof(dst));
    memset(dst32, 0, sizeof(dst32));
    NitroPort_Tile16(src, W, H, dst, TW);
    NitroPort_Tile32(src32, W, H, dst32, TW);
    for (y = 0; y < H; y++) {
        for (x = 0; x < W; x++) {
            ok &= dst[Index(x, y, TW)] == src[y * W + x];
            ok &= dst32[Index(x, y, TW)] == src32[y * W + x];
        }
    }
    CHECK(ok);
    /* Tiles beyond the picture's width aren't touched. */
    CHECK(dst[Index(W, 0, TW)] == 0);
    CHECK(dst32[Index(W, 0, TW)] == 0);
}

/* Scale2x by its rule, output pixel (ox, oy). */
static uint32_t Scale2xRef(const uint32_t *src, int w, int h, int ox, int oy)
{
    int x = ox / 2, y = oy / 2;
    uint32_t it = src[y * w + x];
    uint32_t u = src[(y > 0 ? y - 1 : 0) * w + x], d = src[(y < h - 1 ? y + 1 : h - 1) * w + x];
    uint32_t l = src[y * w + (x > 0 ? x - 1 : 0)], r = src[y * w + (x < w - 1 ? x + 1 : w - 1)];

    if (!(ox & 1) && !(oy & 1)) return (u == l && u != r && l != d) ? u : it;
    if ((ox & 1) && !(oy & 1)) return (u == r && u != l && r != d) ? r : it;
    if (!(ox & 1) && (oy & 1)) return (d == l && d != r && l != u) ? l : it;
    return (d == r && d != l && r != u) ? r : it;
}

static void test_scale2x(void)
{
    enum { W = 32, H = 24, TW = 64 };
    static uint32_t src32[W * H], dst32[TW * 2 * H];
    static uint16_t src16[W * H], dst16[TW * 2 * H];
    int round, x, y, ok = 1, corners = 0;

    for (round = 0; round < 20; round++) {
        /* Few colours, so edges and diagonals are common. */
        for (y = 0; y < H; y++) {
            for (x = 0; x < W; x++) {
                uint32_t c = round < 10 ? Rnd() % 3 : (uint32_t)((x / 3 + y / 2) % 2) * 0x7FFF;
                src32[y * W + x] = c * 0x11u;
                src16[y * W + x] = (uint16_t)c;
            }
        }
        NitroPort_Scale2xTiled32(src32, W, H, dst32, TW);
        NitroPort_Scale2xTiled16(src16, W, H, dst16, TW);
        for (y = 0; y < 2 * H; y++) {
            for (x = 0; x < 2 * W; x++) {
                uint32_t want = Scale2xRef(src32, W, H, x, y);

                ok &= dst32[Index(x, y, TW)] == want;
                ok &= dst16[Index(x, y, TW)] == (uint16_t)(want / 0x11u);
                corners += want != src32[(y / 2) * W + x / 2];
            }
        }
    }
    CHECK(ok);
    CHECK(corners > 0);   /* (the rule did something) */

    /* A lone diagonal step: the corners fill in, and nothing else moves. */
    memset(src32, 0, sizeof(src32));
    for (y = 0; y < H; y++) {
        for (x = 0; x < W; x++) {
            if (x + y >= 20) src32[y * W + x] = 1;
        }
    }
    NitroPort_Scale2xTiled32(src32, W, H, dst32, TW);
    /* Pixel (19, 1) is 1 (19 + 1 = 20): its top-left corner has 0 above
     * and 0 to its left, which are equal, and its right (1) and below (1)
     * differ from them: the corner becomes 0. */
    CHECK(dst32[Index(38, 2, TW)] == 0);
    CHECK(dst32[Index(39, 2, TW)] == 1);
    CHECK(dst32[Index(38, 3, TW)] == 1);
    CHECK(dst32[Index(39, 3, TW)] == 1);
    /* Pixel (20, 0): 1, with 0 to its left and (clamped) itself above. */
    CHECK(dst32[Index(40, 0, TW)] == 1);
    CHECK(dst32[Index(40, 1, TW)] == 1);
}

static void test_double(void)
{
    enum { W = 20, H = 12, TW = 64 };
    static uint16_t src[W * H], dst[TW * 32];
    static uint32_t src32[W * H], dst32[TW * 32];
    int x, y, ok = 1;

    for (y = 0; y < H; y++) {
        for (x = 0; x < W; x++) {
            src[y * W + x] = (uint16_t)Rnd();
            src32[y * W + x] = Rnd() * 7u;
        }
    }
    NitroPort_Double2xTiled16(src, W, H, dst, TW);
    NitroPort_Double2xTiled32(src32, W, H, dst32, TW);
    for (y = 0; y <= 2 * H; y++) {
        for (x = 0; x <= 2 * W; x++) {
            int sx = x / 2 < W ? x / 2 : W - 1, sy = y / 2 < H ? y / 2 : H - 1;

            ok &= dst[Index(x, y, TW)] == src[sy * W + sx];
            ok &= dst32[Index(x, y, TW)] == src32[sy * W + sx];
        }
    }
    CHECK(ok);
}

/* The one-pass version with a colour table: the same as converting first,
 * then doubling. */
static void test_double_table(void)
{
    enum { W = 20, H = 12, TW = 64 };
    static uint16_t src[W * H], conv[W * H], table[32768];
    static uint16_t dst[TW * 32] __attribute__((aligned(8))), ref[TW * 32] __attribute__((aligned(8)));
    int i;

    for (i = 0; i < 32768; i++) {
        table[i] = (uint16_t)(i * 2654435761u >> 7);
    }
    for (i = 0; i < W * H; i++) {
        src[i] = (uint16_t)Rnd();          /* (bit 15 set or not: ignored) */
        conv[i] = table[src[i] & 0x7FFF];
    }
    memset(dst, 0x55, sizeof(dst));
    memset(ref, 0x55, sizeof(ref));
    NitroPort_Double2xTiled16(conv, W, H, ref, TW);
    NitroPort_Double2xTiled16Table(src, W, H, dst, TW, table);
    CHECK(memcmp(dst, ref, sizeof(dst)) == 0);
}

int main(void)
{
    test_tiled_index();
    test_tile();
    test_scale2x();
    test_double();
    test_double_table();
    printf("upscaler and tiler: %d checks, %d failures\n", sChecks, sFailures);
    return sFailures ? 1 : 0;
}
