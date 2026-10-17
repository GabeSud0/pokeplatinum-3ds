/*
 * Tests for xBRZ at 2x (src/port_xbrz.c): its output for a few pictures
 * against the original xBRZ's (the hashes below are of PPSSPP's
 * ext/xbrz/xbrz.cpp's output for the same pictures, its ScalerCfg's
 * defaults, at 2x), and its wrapping, two passes and rows against itself.
 */
#include <nitro_port_xbrz.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int sChecks, sFailures;
#define CHECK(cond) do { sChecks++; if (!(cond)) { sFailures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static uint32_t sSeed;
static uint32_t Rnd(void) { sSeed = sSeed * 1103515245u + 12345u; return sSeed >> 8; }

/* RGBA8, red in the top byte; the DS's colours (5 bits a channel, widened) */
static uint32_t Ds(unsigned r, unsigned g, unsigned b, unsigned a)
{
#define W8(c) (((c) << 3) | ((c) >> 2))
    return (W8(r) << 24) | (W8(g) << 16) | (W8(b) << 8) | a;
#undef W8
}

static uint32_t Fnv(const uint32_t *p, size_t n)
{
    uint32_t h = 2166136261u;
    size_t i;
    for (i = 0; i < n; i++) {
        unsigned k;
        for (k = 0; k < 4; k++) h = (h ^ ((p[i] >> (8 * k)) & 0xffu)) * 16777619u;
    }
    return h;
}

/* The test pictures (made the same way for the original's hashes). */
static void Picture(int which, uint32_t *p, unsigned w, unsigned h)
{
    uint32_t c[6];
    unsigned x, y;
    c[0] = Ds(31, 31, 31, 255);
    c[1] = Ds(4, 6, 10, 255);
    c[2] = Ds(28, 10, 4, 255);
    c[3] = Ds(6, 20, 8, 255);
    c[4] = Ds(30, 26, 6, 255);
    c[5] = 0;   /* see-through */
    sSeed = 1234u + (uint32_t)which;
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            int dx = (int)x * 2 - (int)w, dy = (int)y * 2 - (int)h;
            unsigned k;
            switch (which) {
            case 0: /* a diagonal line and a step, two colours */
                k = (x == y || x == y + 1 || (x > w / 2 && y > h / 2 && x + y > w + 3)) ? 1u : 0u;
                break;
            case 1: /* a ring with an outline on see-through, a dot inside */
                k = dx * dx + dy * dy < (int)(w * w) * 3 / 4 ? (dx * dx + dy * dy < (int)(w * w) / 2 ? 2u : 1u) : 5u;
                if (x == w / 2 && y == h / 2) k = 4;
                break;
            default: /* blocks of colours with noise, as a texture's */
                k = ((x / 3 + y / 2) % 4u) + (Rnd() % 7u == 0 ? 1u : 0u);
                if (k > 4) k = 3;
                break;
            }
            p[y * w + x] = c[k];
        }
    }
}

static void test_original(void)
{
    /* hashes of the original xBRZ's 2x output, picture by picture */
    static const struct { int which; unsigned w, h; uint32_t hash; } cases[] = {
        { 0, 16, 16, 0xc3febceeu }, { 1, 24, 24, 0x1359854du }, { 2, 32, 16, 0xd5933544u },
    };
    unsigned i;
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        unsigned w = cases[i].w, h = cases[i].h;
        uint32_t *src = malloc(w * h * 4), *out = malloc(w * h * 16);
        uint8_t *info = malloc(w * h);
        Picture(cases[i].which, src, w, h);
        NitroPort_Xbrz2x(src, w, h, NITRO_XBRZ_CLAMP, NITRO_XBRZ_CLAMP, 0, info, out);
        if (getenv("XBRZ_DUMP")) {
            char name[64];
            FILE *f;
            snprintf(name, sizeof(name), "xbrz_case%u.raw", i);
            f = fopen(name, "wb");
            if (f) { fwrite(src, 4, w * h, f); fclose(f); }
            printf("case %u: %ux%u hash %08lx\n", i, w, h, (unsigned long)Fnv(out, (size_t)w * h * 4));
        }
        CHECK(Fnv(out, (size_t)w * h * 4) == cases[i].hash);
        free(src); free(out); free(info);
    }
}

static void test_flat(void)
{
    enum { W = 8, H = 8 };
    uint32_t src[W * H], out[W * H * 4];
    uint8_t info[W * H];
    unsigned i;
    for (i = 0; i < W * H; i++) src[i] = Ds(10, 20, 30, 255);
    NitroPort_Xbrz2x(src, W, H, NITRO_XBRZ_REPEAT, NITRO_XBRZ_CLAMP, 0, info, out);
    for (i = 0; i < W * H * 4; i++) CHECK(out[i] == src[0]);
}

/* A tile repeated 3x3 times, scaled with CLAMP: the middle tile's output
 * is the tile's own scaled with REPEAT (and the same with mirrored copies
 * and MIRROR): what's beyond its edges is what wrapping says is there. */
static void test_wrap(int mode)
{
    enum { W = 16, H = 8 };
    uint32_t tile[W * H], big[9 * W * H], out[4 * W * H], bigOut[36 * W * H];
    uint8_t info[W * H], bigInfo[9 * W * H];
    unsigned x, y, same = 1;
    Picture(2, tile, W, H);
    for (y = 0; y < 3 * H; y++) {
        for (x = 0; x < 3 * W; x++) {
            unsigned tx = x % W, ty = y % H;
            if (mode == NITRO_XBRZ_MIRROR) {
                if ((x / W) != 1) tx = W - 1 - tx;
                if ((y / H) != 1) ty = H - 1 - ty;
            }
            big[y * 3 * W + x] = tile[ty * W + tx];
        }
    }
    NitroPort_Xbrz2x(tile, W, H, mode, mode, 0, info, out);
    NitroPort_Xbrz2x(big, 3 * W, 3 * H, NITRO_XBRZ_CLAMP, NITRO_XBRZ_CLAMP, 0, bigInfo, bigOut);
    for (y = 0; y < 2 * H; y++) {
        for (x = 0; x < 2 * W; x++) {
            if (out[y * 2 * W + x] != bigOut[(y + 2 * H) * 6 * W + x + 2 * W]) same = 0;
        }
    }
    CHECK(same);
}

/* keepSubtle: a faint pattern (colours xBRZ counts as equal) stays as it
 * is, each pixel 2x2; a strong edge beside it is still smoothed. */
static void test_subtle(void)
{
    enum { W = 16, H = 16 };
    uint32_t src[W * H], out[4 * W * H], plain[4 * W * H];
    uint8_t info[W * H];
    unsigned x, y, same = 1, smoothed = 0;
    uint32_t grass = Ds(10, 31, 18, 255), tuft = Ds(9, 29, 19, 255), line = Ds(2, 4, 2, 255);
    for (y = 0; y < H; y++) {
        for (x = 0; x < W; x++) {
            uint32_t c = grass;
            if (y < 8 && (x + y) % 5 == 0) c = tuft;          /* faint diagonal tufts above */
            if (y >= 8 && x >= y) c = line;                    /* a strong diagonal edge below */
            src[y * W + x] = c;
        }
    }
    NitroPort_Xbrz2x(src, W, H, NITRO_XBRZ_REPEAT, NITRO_XBRZ_REPEAT, 1, info, out);
    NitroPort_Xbrz2x(src, W, H, NITRO_XBRZ_REPEAT, NITRO_XBRZ_REPEAT, 0, info, plain);
    for (y = 0; y < 2 * 6; y++) {   /* (rows 0-5: away from the edge and the wrap) */
        for (x = 0; x < 2 * W; x++) {
            if (out[y * 2 * W + x] != src[(y / 2) * W + x / 2]) same = 0;
        }
    }
    for (y = 2 * 10; y < 2 * 14; y++) {
        for (x = 0; x < 2 * W; x++) {
            uint32_t o = out[y * 2 * W + x];
            if (o != grass && o != line) smoothed = 1;
        }
    }
    CHECK(same);
    CHECK(smoothed);
    CHECK(memcmp(out, plain, sizeof(out)) != 0);   /* (the original did blend the tufts) */
}

/* The passes a few rows at a time: the same output. */
static void test_rows(void)
{
    enum { W = 24, H = 24 };
    uint32_t src[W * H], whole[4 * W * H], parts[4 * W * H];
    uint8_t info[W * H], info2[W * H];
    unsigned y;
    Picture(1, src, W, H);
    NitroPort_Xbrz2x(src, W, H, NITRO_XBRZ_REPEAT, NITRO_XBRZ_MIRROR, 1, info, whole);
    memset(info2, 0, sizeof(info2));
    memset(parts, 0, sizeof(parts));
    for (y = 0; y < H; y += 5) NitroPort_Xbrz2xCorners(src, W, H, NITRO_XBRZ_REPEAT, NITRO_XBRZ_MIRROR, 1, info2, y, y + 5);
    for (y = 0; y < H; y += 7) NitroPort_Xbrz2xBlend(src, W, H, NITRO_XBRZ_REPEAT, NITRO_XBRZ_MIRROR, info2, parts, y, y + 7);
    CHECK(memcmp(info, info2, sizeof(info)) == 0);
    CHECK(memcmp(whole, parts, sizeof(whole)) == 0);
}

int main(void)
{
    test_original();
    test_flat();
    test_wrap(NITRO_XBRZ_REPEAT);
    test_wrap(NITRO_XBRZ_MIRROR);
    test_subtle();
    test_rows();
    printf("xbrz: %d checks, %d failures\n", sChecks, sFailures);
    return sFailures != 0;
}
