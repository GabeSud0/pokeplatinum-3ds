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

/* Rows at a time: a picture changed in some bands of 4 rows, only those
 * done again over the texture of the picture before, comes out as the
 * whole new picture done at once (the edge column and, when the last band
 * is among them, the row below too). */
static void test_double_rows(void)
{
    enum { W = 20, H = 16, TW = 64, TEX = TW * 48 };
    static uint16_t a[W * H], b[W * H], table[32768];
    static uint32_t a32[W * H], b32[W * H];
    static uint16_t got[TEX] __attribute__((aligned(8))), want[TEX] __attribute__((aligned(8)));
    static uint16_t gotT[TEX] __attribute__((aligned(8))), wantT[TEX] __attribute__((aligned(8)));
    static uint32_t got32[TEX], want32[TEX];
    int i, trial, band;

    for (i = 0; i < 32768; i++) {
        table[i] = (uint16_t)(i * 40503u >> 3);
    }
    for (trial = 0; trial < 40; trial++) {
        int changed[H / 4];

        for (i = 0; i < W * H; i++) {
            a[i] = b[i] = (uint16_t)Rnd();
            a32[i] = b32[i] = Rnd() * 3u;
        }
        memset(got, 0x11, sizeof(got)); memset(want, 0x11, sizeof(want));
        memset(gotT, 0x22, sizeof(gotT)); memset(wantT, 0x22, sizeof(wantT));
        memset(got32, 0x33, sizeof(got32)); memset(want32, 0x33, sizeof(want32));
        NitroPort_Double2xTiled16(a, W, H, got, TW);
        NitroPort_Double2xTiled16Table(a, W, H, gotT, TW, table);
        NitroPort_Double2xTiled32(a32, W, H, got32, TW);
        for (band = 0; band < H / 4; band++) {
            changed[band] = (Rnd() & 3) == 0 || (trial == 1 && band == H / 4 - 1);
            if (changed[band]) {
                for (i = band * 4 * W; i < (band + 1) * 4 * W; i++) {
                    b[i] = (uint16_t)Rnd();
                    b32[i] = Rnd() * 5u;
                }
            }
        }
        for (band = 0; band < H / 4; band++) {
            if (changed[band]) {
                NitroPort_Double2xTiled16Rows(b, W, H, band * 4, band * 4 + 4, got, TW);
                NitroPort_Double2xTiled16TableRows(b, W, H, band * 4, band * 4 + 4, gotT, TW, table);
                NitroPort_Double2xTiled32Rows(b32, W, H, band * 4, band * 4 + 4, got32, TW);
            }
        }
        NitroPort_Double2xTiled16(b, W, H, want, TW);
        NitroPort_Double2xTiled16Table(b, W, H, wantT, TW, table);
        NitroPort_Double2xTiled32(b32, W, H, want32, TW);
        CHECK(memcmp(got, want, sizeof(got)) == 0);
        CHECK(memcmp(gotT, wantT, sizeof(gotT)) == 0);
        CHECK(memcmp(got32, want32, sizeof(got32)) == 0);
    }
}

/* Publishing what changed, as the compositor does: two sources of
 * pictures (the engines) published in turn into three sets of textures,
 * each picture changed in a few bands (or none, or all), its over layer
 * coming and going, its width changing now and then. After each publish,
 * the set's textures are those of the whole picture done at once. */
static void test_publish_doubled(void)
{
    enum { TW = 1024, TEXEL_ROWS = 2 * NITRO_PUBLISH_H + 8, H = NITRO_PUBLISH_H, MAXW = NITRO_PUBLISH_MAX_W };
    static NitroPublishShadow shadows[2];
    static NitroPublishTarget targets[3];
    static uint16_t table[32768], under[2][MAXW * H], over[2][MAXW * H];
    static uint8_t alpha[2][MAXW * H], alpha16[17];
    static uint32_t premul[MAXW * H];
    uint16_t *underTex[3], *wantUnder;
    uint32_t *overTex[3], *wantOver;
    int width[2] = { 320, 320 }, i, step, ok = 1, okOver = 1, okAny = 1, partial = 0;

    for (i = 0; i < 32768; i++) {
        table[i] = (uint16_t)(i * 2654435761u >> 9);
    }
    for (i = 0; i <= 16; i++) {
        alpha16[i] = (uint8_t)(i >= 16 ? 255 : i * 255 / 16);
    }
    for (i = 0; i < 3; i++) {
        underTex[i] = malloc((size_t)TW * TEXEL_ROWS * 2);
        overTex[i] = malloc((size_t)TW * TEXEL_ROWS * 4);
        memset(underTex[i], 0, (size_t)TW * TEXEL_ROWS * 2);
        memset(overTex[i], 0, (size_t)TW * TEXEL_ROWS * 4);
    }
    wantUnder = malloc((size_t)TW * TEXEL_ROWS * 2);
    wantOver = malloc((size_t)TW * TEXEL_ROWS * 4);
    for (i = 0; i < MAXW * H; i++) {
        under[0][i] = (uint16_t)Rnd(); under[1][i] = (uint16_t)Rnd();
        over[0][i] = (uint16_t)(Rnd() & 0x8000 ? 0 : Rnd()); over[1][i] = 0;
        alpha[0][i] = (uint8_t)(Rnd() % 17); alpha[1][i] = 0;
    }
    for (step = 0; step < 300; step++) {
        int e = (Rnd() % 5) == 0, t = step % 3, w, hasOver = (Rnd() % 7) != 0, band, x, y, any;
        int r = (int)(Rnd() % 10);

        if (r == 0) {
            width[e] = width[e] == 320 ? 256 : 320;       /* the canvas narrower or wider */
        }
        w = width[e];
        for (band = 0; band < H / 4; band++) {
            int what = (int)(Rnd() % 16);

            if (r == 1 || what == 0) {                     /* the under picture */
                for (i = band * 4 * w; i < (band + 1) * 4 * w; i++) under[e][i] = (uint16_t)Rnd();
            }
            if (r == 2 || what == 1) {                     /* the over picture */
                for (i = band * 4 * w; i < (band + 1) * 4 * w; i++) {
                    over[e][i] = (uint16_t)(Rnd() & 1 ? 0x8000 | Rnd() : 0);
                    alpha[e][i] = (uint8_t)(Rnd() % 17);
                }
            }
            if (r == 3 || what == 2) {                     /* nothing in front, there */
                for (i = band * 4 * w; i < (band + 1) * 4 * w; i++) over[e][i] = 0;
            }
            if (what == 3) {                               /* only its alpha */
                alpha[e][band * 4 * w + (int)(Rnd() % (unsigned)(4 * w))] ^= 1;
            }
        }
        any = NitroPort_PublishDoubled(&shadows[e], &targets[t], under[e], over[e], alpha[e], hasOver, w, H,
                                       underTex[t], overTex[t], TW, table, alpha16, NULL);
        partial += step > 3 && targets[t].heldBy == &shadows[e];
        /* The whole picture at once, compared where the picture is (and
         * the column right of it, the row below it). */
        NitroPort_Double2xTiled16Table(under[e], w, H, wantUnder, TW, table);
        for (y = 0; y <= 2 * H; y++) {
            for (x = 0; x <= 2 * w; x++) {
                unsigned k = NitroPort_TiledIndex((unsigned)x, (unsigned)y, TW);
                ok &= underTex[t][k] == wantUnder[k];
            }
        }
        if (hasOver) {
            uint32_t want = NitroPort_PremultiplyOver(over[e], alpha[e], alpha16, premul, 0, w * H);

            okAny &= (want != 0) == (any != 0);
            NitroPort_Double2xTiled32(premul, w, H, wantOver, TW);
            for (y = 0; y <= 2 * H; y++) {
                for (x = 0; x <= 2 * w; x++) {
                    unsigned k = NitroPort_TiledIndex((unsigned)x, (unsigned)y, TW);
                    okOver &= overTex[t][k] == wantOver[k];
                }
            }
        } else {
            okAny &= any == 0;
        }
    }
    CHECK(ok);
    CHECK(okOver);
    CHECK(okAny);
    CHECK(partial > 100);   /* (most publishes were partial ones) */
    for (i = 0; i < 3; i++) {
        free(underTex[i]);
        free(overTex[i]);
    }
    free(wantUnder);
    free(wantOver);
}

/* A tall picture (the lower screen as a tall panel: 256 x 248) published
 * after the DS's 192 rows and before: each time the whole picture done at
 * once (rows past 192 too), its height changing counting as all of it
 * changed; and a picture it can't hold not published. */
static void test_publish_tall(void)
{
    enum { TW = 512, H = NITRO_PUBLISH_MAX_H, TEXEL_ROWS = 2 * H + 8, W = 256 };
    static NitroPublishShadow shadow;
    static NitroPublishTarget target;
    static uint16_t table[32768], under[W * H];
    static uint8_t alpha16[17];
    uint16_t *tex = malloc((size_t)TW * TEXEL_ROWS * 2), *want = malloc((size_t)TW * TEXEL_ROWS * 2);
    int i, step, x, y, ok = 1;

    for (i = 0; i < 32768; i++) {
        table[i] = (uint16_t)(i * 2654435761u >> 9);
    }
    for (i = 0; i <= 16; i++) {
        alpha16[i] = (uint8_t)(i >= 16 ? 255 : i * 255 / 16);
    }
    memset(tex, 0, (size_t)TW * TEXEL_ROWS * 2);
    for (step = 0; step < 6; step++) {
        int h = (step == 2 || step == 3) ? NITRO_PUBLISH_H : H;   /* tall, tall, 192, 192, tall, tall */

        for (i = 0; i < W * h; i++) {
            if (step % 2 == 0 || (Rnd() % 8) == 0) {
                under[i] = (uint16_t)Rnd();
            }
        }
        (void)NitroPort_PublishDoubled(&shadow, &target, under, NULL, NULL, 0, W, h, tex, NULL, TW, table, alpha16,
                                       NULL);
        NitroPort_Double2xTiled16Table(under, W, h, want, TW, table);
        for (y = 0; y < 2 * h; y++) {
            for (x = 0; x < 2 * W; x++) {
                unsigned k = NitroPort_TiledIndex((unsigned)x, (unsigned)y, TW);
                ok &= tex[k] == want[k];
            }
        }
    }
    CHECK(ok);
    /* Taller than it holds, or not a multiple of 4: nothing. */
    tex[0] ^= 1;
    CHECK(NitroPort_PublishDoubled(&shadow, &target, under, NULL, NULL, 0, W, H + 4, tex, NULL, TW, table, alpha16,
                                   NULL) == 0);
    CHECK(NitroPort_PublishDoubled(&shadow, &target, under, NULL, NULL, 0, W, 246, tex, NULL, TW, table, alpha16,
                                   NULL) == 0);
    CHECK(NitroPort_PublishDoubled(&shadow, &target, under, NULL, NULL, 0, 320, H, tex, NULL, TW, table, alpha16,
                                   NULL) == 0);
    {
        unsigned k = NitroPort_TiledIndex(0, 0, TW);

        CHECK(tex[k] != want[k]);   /* (left as it was) */
    }
    free(tex);
    free(want);
}

/* The same with the targets taking RGBA5551 (allow16): the over layer
 * mostly opaque or clear (a text box), now and then partly see-through
 * somewhere (a fade). After each publish the over texture is RGBA5551
 * exactly when no drawn pixel of the picture is partly see-through, and
 * either way it's the whole picture done at once in that format; the
 * RGBA5551 texels are the RGBA8 ones' colours to 5 bits. */
static void test_publish_doubled16(void)
{
    enum { TW = 1024, TEXEL_ROWS = 2 * NITRO_PUBLISH_H + 8, H = NITRO_PUBLISH_H, MAXW = NITRO_PUBLISH_MAX_W };
    static NitroPublishShadow shadows[2];
    static NitroPublishTarget targets[3];
    static uint16_t table[32768], under[2][MAXW * H], over[2][MAXW * H], packed[MAXW * H];
    static uint8_t alpha[2][MAXW * H], alpha16[17];
    static uint32_t premul[MAXW * H];
    uint32_t *overTex[3], *wantOver;
    uint16_t *underTex[3], *wantOver16;
    int width[2] = { 320, 320 }, i, step, okOver = 1, okKind = 1, okAny = 1, okExact = 1, as16 = 0, as32 = 0;

    for (i = 0; i < 32768; i++) {
        table[i] = (uint16_t)(i * 2654435761u >> 9);
    }
    for (i = 0; i <= 16; i++) {
        alpha16[i] = (uint8_t)(i >= 16 ? 255 : i * 255 / 16);
    }
    for (i = 0; i < 3; i++) {
        underTex[i] = malloc((size_t)TW * TEXEL_ROWS * 2);
        overTex[i] = malloc((size_t)TW * TEXEL_ROWS * 4);
        memset(underTex[i], 0, (size_t)TW * TEXEL_ROWS * 2);
        memset(overTex[i], 0, (size_t)TW * TEXEL_ROWS * 4);
        targets[i].allow16 = 1;
    }
    wantOver = malloc((size_t)TW * TEXEL_ROWS * 4);
    wantOver16 = malloc((size_t)TW * TEXEL_ROWS * 2);
    for (i = 0; i < MAXW * H; i++) {
        under[0][i] = (uint16_t)Rnd(); under[1][i] = (uint16_t)Rnd();
        over[0][i] = (uint16_t)(Rnd() & 1 ? 0x8000 | Rnd() : 0); over[1][i] = 0;
        alpha[0][i] = (uint8_t)(Rnd() & 1 ? 16 : 0); alpha[1][i] = 16;
    }
    for (step = 0; step < 300; step++) {
        int e = (Rnd() % 5) == 0, t = step % 3, w, hasOver = (Rnd() % 7) != 0, band, x, y, any, partial;
        int r = (int)(Rnd() % 10);

        if (r == 0) {
            width[e] = width[e] == 320 ? 256 : 320;
        }
        w = width[e];
        for (band = 0; band < H / 4; band++) {
            int what = (int)(Rnd() % 16);

            if (r == 1 || what == 0) {                     /* the over picture, opaque or clear */
                for (i = band * 4 * w; i < (band + 1) * 4 * w; i++) {
                    over[e][i] = (uint16_t)(Rnd() & 1 ? 0x8000 | Rnd() : 0);
                    alpha[e][i] = (uint8_t)(Rnd() & 1 ? 16 : 0);
                }
            }
        }
        if (Rnd() % 4 == 0) {                              /* partly see-through, somewhere */
            i = (int)(Rnd() % (unsigned)(w * H));
            over[e][i] |= 0x8000;
            alpha[e][i] = (uint8_t)(1 + Rnd() % 15);
        }
        if (Rnd() % 3 == 0) {                              /* and opaque again */
            for (i = 0; i < MAXW * H; i++) {
                if (alpha[e][i] != 0 && alpha[e][i] != 16) alpha[e][i] = 16;
            }
        }
        any = NitroPort_PublishDoubled(&shadows[e], &targets[t], under[e], over[e], alpha[e], hasOver, w, H,
                                       underTex[t], overTex[t], TW, table, alpha16, NULL);
        if (!hasOver) {
            okAny &= any == 0;
            continue;
        }
        partial = NitroPort_OverPartial(over[e], alpha[e], alpha16, 0, w * H);
        okKind &= targets[t].over16 == !partial;
        okAny &= (NitroPort_PremultiplyOver(over[e], alpha[e], alpha16, premul, 0, w * H) != 0) == (any != 0);
        if (targets[t].over16) {
            const uint16_t *got = (const uint16_t *)(const void *)overTex[t];

            as16++;
            NitroPort_PackOver5551(premul, packed, 0, w * H);
            for (i = 0; i < w * H; i++) {
                uint32_t p = premul[i], q = packed[i];

                /* (5 bits each, widened back: the RGBA8 texel again) */
                okExact &= (p & 0xFFu) == 0 ? q == 0 && p == 0
                                            : ((((q >> 11) & 31u) << 3 | ((q >> 11) & 31u) >> 2) == (p >> 24) &&
                                               (((q >> 6) & 31u) << 3 | ((q >> 6) & 31u) >> 2) == ((p >> 16) & 0xFFu) &&
                                               (((q >> 1) & 31u) << 3 | ((q >> 1) & 31u) >> 2) == ((p >> 8) & 0xFFu) &&
                                               (q & 1u) == 1u);
            }
            NitroPort_Double2xTiled16(packed, w, H, wantOver16, TW);
            for (y = 0; y <= 2 * H; y++) {
                for (x = 0; x <= 2 * w; x++) {
                    unsigned k = NitroPort_TiledIndex((unsigned)x, (unsigned)y, TW);
                    okOver &= got[k] == wantOver16[k];
                }
            }
        } else {
            as32++;
            NitroPort_Double2xTiled32(premul, w, H, wantOver, TW);
            for (y = 0; y <= 2 * H; y++) {
                for (x = 0; x <= 2 * w; x++) {
                    unsigned k = NitroPort_TiledIndex((unsigned)x, (unsigned)y, TW);
                    okOver &= overTex[t][k] == wantOver[k];
                }
            }
        }
    }
    CHECK(okOver);
    CHECK(okKind);
    CHECK(okAny);
    CHECK(okExact);
    CHECK(as16 > 30);   /* (both kinds came up, and went back and forth) */
    CHECK(as32 > 30);
    for (i = 0; i < 3; i++) {
        free(underTex[i]);
        free(overTex[i]);
    }
    free(wantOver);
    free(wantOver16);
}

int main(void)
{
    test_tiled_index();
    test_tile();
    test_scale2x();
    test_double();
    test_double_table();
    test_double_rows();
    test_publish_doubled();
    test_publish_doubled16();
    test_publish_tall();
    printf("upscaler and tiler: %d checks, %d failures\n", sChecks, sFailures);
    return sFailures ? 1 : 0;
}
