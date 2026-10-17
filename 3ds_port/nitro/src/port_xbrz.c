/*
 * xBRZ at 2x: see include/nitro_port_xbrz.h.
 *
 * Based on xBRZ by Zenju (zenju AT gmx DOT de), part of the HqMAME project,
 * distributed under the GNU General Public License; this C version (2x
 * only, two passes, wrapping, this port's pixel layout) is under the same
 * licence as the rest of this port (GPL 3). The rules, thresholds and
 * colour distance are the original's (ScalerCfg's defaults: luminance
 * weight 1, equal colour tolerance 30, dominant direction 3.6, steep
 * direction 2.2), including its distance table's precision (each channel's
 * difference halved), so with CLAMP wrapping the output is the original's.
 */
#include <nitro_port_xbrz.h>
#include <math.h>

enum { BLEND_NONE = 0, BLEND_NORMAL = 1, BLEND_DOMINANT = 2 };

#define EQUAL_COLOR_TOLERANCE 30.0
#define DOMINANT_DIRECTION_THRESHOLD 3.6
#define STEEP_DIRECTION_THRESHOLD 2.2

static inline unsigned red(uint32_t p) { return p >> 24; }
static inline unsigned green(uint32_t p) { return (p >> 16) & 0xffu; }
static inline unsigned blue(uint32_t p) { return (p >> 8) & 0xffu; }
static inline unsigned alpha(uint32_t p) { return p & 0xffu; }

/* The original's YCbCr distance (ITU-R BT.2020, no division by 255), from
 * its table: each channel's difference d as (d + 255) / 2 * 2 - 255. */
static float dist_ycbcr(int rd, int gd, int bd)
{
    const double k_b = 0.0593, k_r = 0.2627, k_g = 1 - k_b - k_r;
    const double scale_b = 0.5 / (1 - k_b), scale_r = 0.5 / (1 - k_r);
    double r = (double)((rd + 255) / 2 * 2 - 255);
    double g = (double)((gd + 255) / 2 * 2 - 255);
    double b = (double)((bd + 255) / 2 * 2 - 255);
    double y = k_r * r + k_g * g + k_b * b;
    double c_b = scale_b * (b - y), c_r = scale_r * (r - y);
    return (float)sqrt(y * y + c_b * c_b + c_r * c_r);
}

/* The colour distance with alpha (the original's ColorDistanceARGB). A
 * texture has few colours (a palette's), so the same pairs come up again
 * and again: the last result for each of 4096 slots is kept (the 3DS's
 * square roots in double are slow). One thread at a time. */
static struct { uint32_t p1, p2; double d; } sDistCache[4096];
static int sDistCacheReady;

static double dist_uncached(uint32_t p1, uint32_t p2)
{
    double a1 = alpha(p1) / 255.0, a2 = alpha(p2) / 255.0;
    double d = dist_ycbcr((int)red(p1) - (int)red(p2), (int)green(p1) - (int)green(p2),
                          (int)blue(p1) - (int)blue(p2));
    return a1 < a2 ? a1 * d + 255.0 * (a2 - a1) : a2 * d + 255.0 * (a1 - a2);
}

static double dist(uint32_t p1, uint32_t p2)
{
    uint32_t key = p1 * 0x9E3779B1u ^ (p2 + 0x7F4A7C15u) * 0x85EBCA77u;
    unsigned slot = (key ^ (key >> 15)) & 4095u;
    if (!sDistCacheReady) {
        unsigned i;
        /* (a slot's pair must not match before it's been filled: 0, 1 is
         * worked out, as every slot is filled with it) */
        double d01 = dist_uncached(0u, 1u);
        for (i = 0; i < 4096u; i++) {
            sDistCache[i].p1 = 0u;
            sDistCache[i].p2 = 1u;
            sDistCache[i].d = d01;
        }
        sDistCacheReady = 1;
    }
    if (sDistCache[slot].p1 != p1 || sDistCache[slot].p2 != p2) {
        sDistCache[slot].p1 = p1;
        sDistCache[slot].p2 = p2;
        sDistCache[slot].d = dist_uncached(p1, p2);
    }
    return sDistCache[slot].d;
}

static inline int eq(uint32_t p1, uint32_t p2) { return dist(p1, p2) < EQUAL_COLOR_TOLERANCE; }

/* front over back, weighted m / n by their alphas (no alpha blending: the
 * original's gradientARGB) */
static uint32_t gradient(uint32_t front, uint32_t back, unsigned m, unsigned n)
{
    unsigned wf = alpha(front) * m, wb = alpha(back) * (n - m), ws = wf + wb;
    if (ws == 0) return 0;
#define MIX(c) ((c(front) * wf + c(back) * wb) / ws)
    return (MIX(red) << 24) | (MIX(green) << 16) | (MIX(blue) << 8) | (ws / n);
#undef MIX
}

/* Neighbour i (maybe outside 0..n-1) of an axis, by its wrapping. */
static inline unsigned wrap(int i, unsigned n, int mode)
{
    int size = (int)n;
    if (i >= 0 && i < size) return (unsigned)i;
    if (mode == NITRO_XBRZ_REPEAT) {
        i %= size;
        return (unsigned)(i < 0 ? i + size : i);
    }
    if (mode == NITRO_XBRZ_MIRROR) {
        int p = i % (2 * size);
        if (p < 0) p += 2 * size;
        return (unsigned)(p < size ? p : 2 * size - 1 - p);
    }
    return i < 0 ? 0u : n - 1u;
}

/* One byte a pixel: its four corners' blends, 2 bits each. */
#define TOP_L(b) ((b) & 3u)
#define TOP_R(b) (((b) >> 2) & 3u)
#define BOTTOM_R(b) (((b) >> 4) & 3u)
#define BOTTOM_L(b) (((b) >> 6) & 3u)

void NitroPort_Xbrz2xCorners(const uint32_t *src, unsigned w, unsigned h, int wrapS, int wrapT,
                             int keepSubtle, uint8_t *info, unsigned y0, unsigned y1)
{
    unsigned x, y;
    for (y = y0; y < y1 && y < h; y++) {
        const uint32_t *r0 = src + wrap((int)y - 1, h, wrapT) * w, *r1 = src + y * w;
        const uint32_t *r2 = src + wrap((int)y + 1, h, wrapT) * w, *r3 = src + wrap((int)y + 2, h, wrapT) * w;
        /* the corner's lower pixels' row; none past the edge unless repeating */
        int down = y + 1 < h ? (int)y + 1 : wrapT == NITRO_XBRZ_REPEAT ? 0 : -1;
        for (x = 0; x < w; x++) {
            unsigned xm = wrap((int)x - 1, w, wrapS), xp = wrap((int)x + 1, w, wrapS), xq = wrap((int)x + 2, w, wrapS);
            int right = x + 1 < w ? (int)x + 1 : wrapS == NITRO_XBRZ_REPEAT ? 0 : -1;
            /* the 4x4 around the corner between f, g, j and k:
             *   a b c d
             *   e f g h
             *   i j k l
             *   m n o p */
            uint32_t b = r0[x], c = r0[xp];
            uint32_t e = r1[xm], f = r1[x], g = r1[xp], hh = r1[xq];
            uint32_t i = r2[xm], j = r2[x], k = r2[xp], l = r2[xq];
            uint32_t n = r3[x], o = r3[xp];
            unsigned bf = 0, bg = 0, bj = 0, bk = 0;
            double jg, fk;
            if ((f == g && j == k) || (f == j && g == k)) continue;
            if (keepSubtle && eq(f, g) && eq(f, j) && eq(f, k) && eq(g, j) && eq(g, k) && eq(j, k)) continue;
            jg = dist(i, f) + dist(f, c) + dist(n, k) + dist(k, hh) + 4.0 * dist(j, g);
            fk = dist(e, j) + dist(j, o) + dist(b, g) + dist(g, l) + 4.0 * dist(f, k);
            if (jg < fk) {
                unsigned t = DOMINANT_DIRECTION_THRESHOLD * jg < fk ? BLEND_DOMINANT : BLEND_NORMAL;
                if (f != g && f != j) bf = t;
                if (k != j && k != g) bk = t;
            } else if (fk < jg) {
                unsigned t = DOMINANT_DIRECTION_THRESHOLD * fk < jg ? BLEND_DOMINANT : BLEND_NORMAL;
                if (j != f && j != k) bj = t;
                if (g != f && g != k) bg = t;
            }
            info[y * w + x] |= (uint8_t)(bf << 4);                                         /* f: bottom right */
            if (right >= 0) info[y * w + (unsigned)right] |= (uint8_t)(bg << 6);           /* g: bottom left */
            if (down >= 0) info[(unsigned)down * w + x] |= (uint8_t)(bj << 2);             /* j: top right */
            if (down >= 0 && right >= 0) info[(unsigned)down * w + (unsigned)right] |= (uint8_t)bk;   /* k: top left */
        }
    }
}

/* The 3x3 around a pixel, a to i (e the pixel), as the four rotations
 * see it (the original's get_x<rotation>), and the 2x2 output's places. */
static const uint8_t sRotKernel[4][9] = {
    { 0, 1, 2, 3, 4, 5, 6, 7, 8 },
    { 6, 3, 0, 7, 4, 1, 8, 5, 2 },
    { 8, 7, 6, 5, 4, 3, 2, 1, 0 },
    { 2, 5, 8, 1, 4, 7, 0, 3, 6 },
};

/* Output (row i, column j) of the 2x2 as rotation r sees it. */
static inline uint32_t *at(uint32_t *out, unsigned pitch, int r, unsigned i, unsigned j)
{
    unsigned oi = i, oj = j;
    switch (r) {
    case 1: oi = 1 - j; oj = i; break;
    case 2: oi = 1 - i; oj = 1 - j; break;
    case 3: oi = j; oj = 1 - i; break;
    }
    return out + oi * pitch + oj;
}

static void blend_pixel(const uint32_t *ker, uint32_t *out, unsigned pitch, unsigned info, int r)
{
    const uint8_t *m = sRotKernel[r];
    uint32_t b = ker[m[1]], c = ker[m[2]], d = ker[m[3]], e = ker[m[4]];
    uint32_t f = ker[m[5]], g = ker[m[6]], hh = ker[m[7]], i = ker[m[8]];
    unsigned blend = (r == 0 ? info : ((info << (2 * r)) | (info >> (8 - 2 * r)))) & 0xffu;
    uint32_t px, *p;
    int line;

    if (BOTTOM_R(blend) < BLEND_NORMAL) return;
    if (BOTTOM_R(blend) >= BLEND_DOMINANT) {
        line = 1;
    } else if (TOP_R(blend) != BLEND_NONE && !eq(e, g)) {
        line = 0;   /* (no second blend beside it: lone pixels, eyes) */
    } else if (BOTTOM_L(blend) != BLEND_NONE && !eq(e, c)) {
        line = 0;
    } else if (!eq(e, i) && eq(g, hh) && eq(hh, i) && eq(i, f) && eq(f, c)) {
        line = 0;   /* (an L shape: the corner only) */
    } else {
        line = 1;
    }
    px = dist(e, f) <= dist(e, hh) ? f : hh;
    if (line) {
        double fg = dist(f, g), hc = dist(hh, c);
        int shallow = STEEP_DIRECTION_THRESHOLD * fg <= hc && e != g && d != g;
        int steep = STEEP_DIRECTION_THRESHOLD * hc <= fg && e != c && b != c;
        if (shallow && steep) {
            p = at(out, pitch, r, 1, 0); *p = gradient(px, *p, 1, 4);
            p = at(out, pitch, r, 0, 1); *p = gradient(px, *p, 1, 4);
            p = at(out, pitch, r, 1, 1); *p = gradient(px, *p, 5, 6);
        } else if (shallow) {
            p = at(out, pitch, r, 1, 0); *p = gradient(px, *p, 1, 4);
            p = at(out, pitch, r, 1, 1); *p = gradient(px, *p, 3, 4);
        } else if (steep) {
            p = at(out, pitch, r, 0, 1); *p = gradient(px, *p, 1, 4);
            p = at(out, pitch, r, 1, 1); *p = gradient(px, *p, 3, 4);
        } else {
            p = at(out, pitch, r, 1, 1); *p = gradient(px, *p, 1, 2);
        }
    } else {
        p = at(out, pitch, r, 1, 1); *p = gradient(px, *p, 21, 100);   /* (a round corner) */
    }
}

void NitroPort_Xbrz2xBlend(const uint32_t *src, unsigned w, unsigned h, int wrapS, int wrapT,
                           const uint8_t *info, uint32_t *out, unsigned y0, unsigned y1)
{
    unsigned x, y, pitch = w * 2u;
    for (y = y0; y < y1 && y < h; y++) {
        const uint32_t *r0 = src + wrap((int)y - 1, h, wrapT) * w, *r1 = src + y * w;
        const uint32_t *r2 = src + wrap((int)y + 1, h, wrapT) * w;
        uint32_t *o = out + y * 2u * pitch;
        for (x = 0; x < w; x++, o += 2) {
            unsigned b = info[y * w + x];
            o[0] = o[1] = o[pitch] = o[pitch + 1] = r1[x];
            if (b) {
                unsigned xm = wrap((int)x - 1, w, wrapS), xp = wrap((int)x + 1, w, wrapS);
                uint32_t ker[9];
                int r;
                ker[0] = r0[xm]; ker[1] = r0[x]; ker[2] = r0[xp];
                ker[3] = r1[xm]; ker[4] = r1[x]; ker[5] = r1[xp];
                ker[6] = r2[xm]; ker[7] = r2[x]; ker[8] = r2[xp];
                for (r = 0; r < 4; r++) blend_pixel(ker, o, pitch, b, r);
            }
        }
    }
}

void NitroPort_Xbrz2x(const uint32_t *src, unsigned w, unsigned h, int wrapS, int wrapT,
                      int keepSubtle, uint8_t *info, uint32_t *out)
{
    unsigned i;
    for (i = 0; i < w * h; i++) info[i] = 0;
    NitroPort_Xbrz2xCorners(src, w, h, wrapS, wrapT, keepSubtle, info, 0, h);
    NitroPort_Xbrz2xBlend(src, w, h, wrapS, wrapT, info, out, 0, h);
}
