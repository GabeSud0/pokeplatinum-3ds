/*
 * A software stand-in for the 3DS GPU path, for checking it on a PC.
 *
 * It draws a scene the way port_gx3d_pica.c asks the PICA to: the same
 * batches (NitroPort_Gx3dBuildBatches), the same tiled textures
 * (NitroPort_Gx3dDecodeTextureTiled) read back the way the PICA addresses
 * them, the same combiner modes, alpha test, blending and depth rules.
 * Comparing its picture with the PC reference rasterizer's shows mistakes
 * in that translation without a 3DS. It doesn't model the PICA's exact
 * rasterization rules or precision, so expect small differences on edges.
 */
#include <nitro_port_gx3d.h>
#include <nitro_port_gx3d_texture.h>
#include <nitro_port_gx3d_vertex.h>
#include <nitro_port_shadow.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

extern unsigned char NitroPort_IoReg[];

/* The picture's size: the DS's, or a bigger view's at the DS's pixel size
 * (NitroPort_Gx3dSimulateGpuSized). */
#define MAX_W 512
#define MAX_H 256
static int sW = NITRO_GX3D_WIDTH, sH = NITRO_GX3D_HEIGHT;
#define W sW
#define H sH

typedef struct { float r, g, b, a; } Color;

static unsigned wrap(int c, unsigned size, int repeat, int mirror)
{
    if (!repeat) return c < 0 ? 0 : c >= (int)size ? size - 1 : (unsigned)c;
    if (mirror) {
        int range = (int)size * 2, p = c % range;
        if (p < 0) p += range;
        return p >= (int)size ? (unsigned)(range - 1 - p) : (unsigned)p;
    }
    c %= (int)size;
    return c < 0 ? (unsigned)(c + (int)size) : (unsigned)c;
}

/* The PICA's texel fetch: nearest, wrap, then row (height - 1 - t) of the
 * 8x8 Morton-tiled image. */
static Color fetch(const uint32_t *tiled, uint32_t param, float u, float v)
{
    unsigned w = NitroPort_Gx3dTexWidth(param), h = NitroPort_Gx3dTexHeight(param);
    unsigned x = wrap((int)floorf(u * (float)w), w, (param >> 16) & 1, (param >> 18) & 1);
    unsigned t = wrap((int)floorf(v * (float)h), h, (param >> 17) & 1, (param >> 19) & 1);
    unsigned y = h - 1 - t, bit, morton = 0;
    uint32_t word;
    Color c;
    for (bit = 0; bit < 3; bit++) {
        morton |= ((x >> bit) & 1u) << (2 * bit);
        morton |= ((y >> bit) & 1u) << (2 * bit + 1);
    }
    word = tiled[(y / 8u) * (w / 8u) * 64u + (x / 8u) * 64u + morton];
    c.r = (float)(word >> 24) / 255.0f;
    c.g = (float)((word >> 16) & 255u) / 255.0f;
    c.b = (float)((word >> 8) & 255u) / 255.0f;
    c.a = (float)(word & 255u) / 255.0f;
    return c;
}

static float edge(float ax, float ay, float bx, float by, float px, float py)
{
    return (px - ax) * (by - ay) - (py - ay) * (bx - ax);
}

/* Sun shadows, as port_gx3d_pica.c draws them: a 512x512 map of the
 * opaque polygons' nearest distance from the sun (the PICA's shadow mode;
 * cut-outs by their texture's alpha), looked up 2x2 filtered (as the PICA's
 * shadow texture) for each opaque pixel, which is lit times
 * shade + (1 - shade) * the lookup. */
#define SHADOW_SIZE 512
static int sShadows, sGrade, sSunFixed;
static NitroShadowSun sFixedSun = { 55.0f, 32.0f };
static float sShadowMap[SHADOW_SIZE * SHADOW_SIZE];
static float sLit[MAX_W * MAX_H];   /* (the opaque pixels' light, for the mask) */
/* (as port_gx3d_pica.c: room for the cards' copies turned to the sun, and
 * the batches that are the game's painted shadows, left out) */
#define SHADOW_CARD_TRIANGLES 512u
#define SHADOW_CARD_BATCHES 256u
static uint8_t sDecal[NITRO_GX3D_MAX_TRIANGLES];
static uint32_t sGradeTable[32768];

/* The sun shadows as SUN SHADOWS: ON draws them (port_gx3d_pica.c,
 * NitroPort_PicaGroundShadows): look 2. */
static int sGround;
void NitroPort_Gx3dSimulateLook(int shadows, int grade, int fixedSun, float elevation, float azimuth)
{
    sGround = shadows == 2;
    if (sGround) shadows = 0;
    NitroPort_SetGroundShadows(sGround);
    sShadows = shadows;
    sGrade = grade;
    sSunFixed = fixedSun;
    sFixedSun.elevation = elevation;
    sFixedSun.azimuth = azimuth;
    NitroPort_SetSunShadows(shadows);
    NitroPort_SetShadowReach(shadows);
    if (sGround) {
        /* (the game makes the land's trees' shadows for this sun from its
         * next scene on) */
        NitroShadowSun sun = sFixedSun;
        float strength = 1.0f;
        time_t now = time(NULL);
        struct tm local;
        if (!fixedSun && now != (time_t)-1 && localtime_r(&now, &local))
            NitroPort_ShadowSunAt(local.tm_hour, local.tm_min, &sun, &strength);
        NitroPort_SetSunNow(&sun, strength);
    }
}

static float row4(const float r[4], const float *p) { return r[0] * p[0] + r[1] * p[1] + r[2] * p[3] + r[3]; }

/* m (w x h) at (u, v) in texels, bilinear, clamped at the edges */
static float bilinear(const float *m, int w, int h, float u, float v)
{
    float cx = u - 0.5f, cy = v - 0.5f, fx, fy;
    int x0 = (int)floorf(cx), y0 = (int)floorf(cy), x1, y1;
    fx = cx - (float)x0;
    fy = cy - (float)y0;
    x1 = x0 + 1; y1 = y0 + 1;
    x0 = x0 < 0 ? 0 : x0 >= w ? w - 1 : x0; x1 = x1 < 0 ? 0 : x1 >= w ? w - 1 : x1;
    y0 = y0 < 0 ? 0 : y0 >= h ? h - 1 : y0; y1 = y1 < 0 ? 0 : y1 >= h ? h - 1 : y1;
    return (m[y0 * w + x0] * (1.0f - fx) + m[y0 * w + x1] * fx) * (1.0f - fy) +
           (m[y1 * w + x0] * (1.0f - fx) + m[y1 * w + x1] * fx) * fy;
}

static void shadow_map(const NitroGx3dGpuVertex *verts, const NitroGx3dBatch *batches, size_t nb,
                       const NitroGx3dTexBanks *banks, uint32_t *tiled, const NitroShadowSetup *setup)
{
    size_t b, i;
    for (i = 0; i < SHADOW_SIZE * SHADOW_SIZE; i++) sShadowMap[i] = 1.0f;
    for (b = 0; b < nb; b++) {
        const NitroGx3dBatch *batch = &batches[b];
        int textured = batch->texture_param != 0;
        uint32_t k;
        if (batch->translucent) continue;
        if (textured) NitroPort_Gx3dDecodeTextureTiled(banks, batch->texture_param, batch->palette_base, tiled);
        for (k = batch->first; k + 2 < batch->first + batch->count; k += 3) {
            const NitroGx3dGpuVertex *v = &verts[k];
            float mx[3], my[3], md[3], area;
            /* (lying flat, or a card cast by its copy turned to the sun: doesn't cast) */
            if (v[0].color[3] == NITRO_SHADOW_RECEIVE_ONLY || v[0].color[3] == NITRO_SHADOW_NEITHER) continue;
            int j, x0, x1, y0, y1, x, y;
            for (j = 0; j < 3; j++) {
                mx[j] = (row4(setup->caster[0], v[j].position) + 1.0f) * 0.5f * SHADOW_SIZE;
                my[j] = (row4(setup->caster[1], v[j].position) + 1.0f) * 0.5f * SHADOW_SIZE;
                md[j] = -row4(setup->caster[2], v[j].position);
            }
            area = edge(mx[0], my[0], mx[1], my[1], mx[2], my[2]);
            if (fabsf(area) < 1e-6f) continue;
            x0 = (int)floorf(fminf(mx[0], fminf(mx[1], mx[2])));
            x1 = (int)ceilf(fmaxf(mx[0], fmaxf(mx[1], mx[2])));
            y0 = (int)floorf(fminf(my[0], fminf(my[1], my[2])));
            y1 = (int)ceilf(fmaxf(my[0], fmaxf(my[1], my[2])));
            if (x0 < 0) x0 = 0;
            if (y0 < 0) y0 = 0;
            if (x1 > SHADOW_SIZE - 1) x1 = SHADOW_SIZE - 1;
            if (y1 > SHADOW_SIZE - 1) y1 = SHADOW_SIZE - 1;
            for (y = y0; y <= y1; y++) {
                for (x = x0; x <= x1; x++) {
                    float px = x + 0.5f, py = y + 0.5f;
                    float l0 = edge(mx[1], my[1], mx[2], my[2], px, py) / area;
                    float l1 = edge(mx[2], my[2], mx[0], my[0], px, py) / area;
                    float l2 = 1.0f - l0 - l1, d;
                    if (l0 < -1e-6f || l1 < -1e-6f || l2 < -1e-6f) continue;
                    if (textured) {
                        /* (the map is a parallel view: texture coordinates linear in it) */
                        Color t = fetch(tiled, batch->texture_param,
                                        l0 * v[0].uv[0] + l1 * v[1].uv[0] + l2 * v[2].uv[0],
                                        l0 * v[0].uv[1] + l1 * v[1].uv[1] + l2 * v[2].uv[1]);
                        if (!(t.a * 255.0f + 0.5f >= 1.0f)) continue;
                    }
                    d = l0 * md[0] + l1 * md[1] + l2 * md[2];
                    if (d < sShadowMap[y * SHADOW_SIZE + x]) sShadowMap[y * SHADOW_SIZE + x] = d;
                }
            }
        }
    }
}

/* The light over the opaque pixels, as port_gx3d_pica.c puts it: a mask at
 * half size (each texel the mean of its 2x2), blurred (four bilinear taps
 * half a texel to the diagonals), read back bilinear; each pixel times
 * shade + (1 - shade) * it. */
static void apply_mask(Color *color, const float shade[3])
{
    const int MW = W / 2, MH = H / 2;
    static float mask[(MAX_W / 2) * (MAX_H / 2)], blurred[(MAX_W / 2) * (MAX_H / 2)];
    int x, y, k;
#define TAP(m, u, v) bilinear(m, MW, MH, u, v)
    for (y = 0; y < MH; y++)
        for (x = 0; x < MW; x++)
            mask[y * MW + x] = (sLit[(2 * y) * W + 2 * x] + sLit[(2 * y) * W + 2 * x + 1] +
                                sLit[(2 * y + 1) * W + 2 * x] + sLit[(2 * y + 1) * W + 2 * x + 1]) * 0.25f;
    for (y = 0; y < MH; y++)
        for (x = 0; x < MW; x++) {
            float s = 0.0f;
            for (k = 0; k < 4; k++) s += TAP(mask, x + 0.5f + ((k & 1) ? 0.5f : -0.5f), y + 0.5f + ((k & 2) ? 0.5f : -0.5f));
            blurred[y * MW + x] = s * 0.25f;
        }
    for (y = 0; y < H; y++)
        for (x = 0; x < W; x++) {
            float m = TAP(blurred, (x + 0.5f) * 0.5f, (y + 0.5f) * 0.5f);
            Color *c = &color[y * W + x];
            c->r *= shade[0] + (1.0f - shade[0]) * m;
            c->g *= shade[1] + (1.0f - shade[1]) * m;
            c->b *= shade[2] + (1.0f - shade[2]) * m;
        }
#undef TAP
}

/* 1 lit .. 0 in shadow, for a distance d at map place (u, v) */
static float shadow_lookup(float u, float v, float d)
{
    float cx = u * SHADOW_SIZE - 0.5f, cy = v * SHADOW_SIZE - 0.5f;
    int ix = (int)floorf(cx), iy = (int)floorf(cy), j;
    float fx = cx - (float)ix, fy = cy - (float)iy, s[4];
    for (j = 0; j < 4; j++) {
        int x = ix + (j & 1), y = iy + (j >> 1);
        s[j] = (x < 0 || y < 0 || x >= SHADOW_SIZE || y >= SHADOW_SIZE) ? 1.0f
               : sShadowMap[y * SHADOW_SIZE + x] > d ? 1.0f : 0.0f;
    }
    return (s[0] * (1.0f - fx) + s[1] * fx) * (1.0f - fy) + (s[2] * (1.0f - fx) + s[3] * fx) * fy;
}

/* ---- SUN SHADOWS: ON, as port_gx3d_pica.c draws them (draw_ground_shadows,
 * draw_once_batches): after the opaque polygons, each shadow multiplying
 * what's there, a pixel darkened once, in its passes. ---- */
#define G_CARD_BATCHES (2048u + SHADOW_CARD_BATCHES)
#define G_MASKS 32
static NitroGx3dBatch sGTag[NITRO_GX3D_MAX_TAGGED_TRIANGLES];
static uint8_t sGRoundUsed[NITRO_GX3D_MAX_TAGGED_TRIANGLES];
static NitroGx3dBatch sGCards[G_CARD_BATCHES];
static size_t sGCardCount;
static int sGReady, sGOnce;
static unsigned sGShadeBatches;
static float sGShade[3], sGTree[3];   /* (what a full shadow takes; a tree's) */
static uint8_t sOnce[MAX_W * MAX_H];
static struct { int used; uint32_t param, palette; uint32_t *tiled; } sGMasks[G_MASKS];

enum { G_GROUND, G_PROP, G_MASK, G_BLOB, G_CHAR, G_RIM };


static Color fetch_linear(const uint32_t *tiled, uint32_t param, float u, float v)
{
    unsigned w = NitroPort_Gx3dTexWidth(param), h = NitroPort_Gx3dTexHeight(param);
    float x = u * (float)w - 0.5f, y = v * (float)h - 0.5f, fx, fy;
    int x0 = (int)floorf(x), y0 = (int)floorf(y), k;
    Color c[4], out;
    fx = x - (float)x0;
    fy = y - (float)y0;
    for (k = 0; k < 4; k++)
        c[k] = fetch(tiled, param, ((float)(x0 + (k & 1)) + 0.5f) / (float)w, ((float)(y0 + (k >> 1)) + 0.5f) / (float)h);
#define LERP2(f) ((c[0].f * (1.0f - fx) + c[1].f * fx) * (1.0f - fy) + (c[2].f * (1.0f - fx) + c[3].f * fx) * fy)
    out.r = LERP2(r); out.g = LERP2(g); out.b = LERP2(b); out.a = LERP2(a);
#undef LERP2
    return out;
}

/* A tree texture's softened outline (port_gx3d_pica.c, get_mask), tiled. */
static const uint32_t *g_mask(const NitroGx3dTexBanks *banks, uint32_t param, uint32_t palette)
{
    unsigned w = NitroPort_Gx3dTexWidth(param), h = NitroPort_Gx3dTexHeight(param);
    uint32_t *tiled, *lin, *out;
    int i, slot = -1;
    for (i = 0; i < G_MASKS; i++) {
        if (sGMasks[i].used && sGMasks[i].param == param && sGMasks[i].palette == palette) return sGMasks[i].tiled;
        if (!sGMasks[i].used && slot < 0) slot = i;
    }
    if (slot < 0) return NULL;
    tiled = malloc((size_t)w * h * 4u);
    lin = malloc((size_t)w * h * 4u);
    out = malloc((size_t)w * h * 4u);
    if (!tiled || !lin || !out) { free(tiled); free(lin); free(out); return NULL; }
    NitroPort_Gx3dDecodeTextureTiled(banks, param, palette, tiled);
    NitroPort_Gx3dUntileRGBA8(tiled, w, h, lin);
    if (!NitroPort_ShadowTreeMask(lin, w, h,
                                  !(param & (1u << 16)) ? NITRO_SHADOW_WRAP_CLAMP : (param & (1u << 18)) ? NITRO_SHADOW_WRAP_MIRROR : NITRO_SHADOW_WRAP_REPEAT,
                                  !(param & (1u << 17)) ? NITRO_SHADOW_WRAP_CLAMP : (param & (1u << 19)) ? NITRO_SHADOW_WRAP_MIRROR : NITRO_SHADOW_WRAP_REPEAT,
                                  out)) {
        free(tiled); free(lin); free(out);
        return NULL;
    }
    NitroPort_Gx3dTileRGBA8(out, w, h, tiled);
    free(lin); free(out);
    sGMasks[slot].used = 1;
    sGMasks[slot].param = param;
    sGMasks[slot].palette = palette;
    sGMasks[slot].tiled = tiled;
    return tiled;
}
static void g_masks_free(void)
{
    int i;
    for (i = 0; i < G_MASKS; i++) { free(sGMasks[i].tiled); sGMasks[i].tiled = NULL; sGMasks[i].used = 0; }
}

static float g_mask_at(void *ctx, uint32_t param, uint32_t palette, float u, float v)
{
    const uint32_t *mask = g_mask(ctx, param, palette);
    return mask ? fetch_linear(mask, param, u, v).a : 1.0f;
}

static int g_foot_row(void *ctx, uint32_t param, uint32_t palette, int s0, int s1, int tFrom, int tTo)
{
    const NitroGx3dTexBanks *banks = ctx;
    int t, s, sEnd = s1 - s0 > 128 ? s0 + 128 : s1, step = tTo >= tFrom ? 1 : -1;
    for (t = tFrom;; t += step) {
        for (s = s0; s < sEnd; s++)
            if (NitroPort_Gx3dSampleTexel(banks, param, palette, s, t).alpha) return t;
        if (t == tTo) break;
    }
    return -1;
}

/* One shadow batch drawn: depth LEQUAL (no writes), the kind's colour and
 * alpha, alpha >= test, once (or not), the frame times the colour. */
static void g_draw(const NitroGx3dGpuVertex *verts, const NitroGx3dBatch *batch, int kind, unsigned test, int once,
                   const NitroGx3dTexBanks *banks, uint32_t *tiled, Color *color, const float *depth, int wbuffer)
{
    const uint32_t *tex = NULL;
    uint32_t k;
    if (batch->texture_param && (kind == G_GROUND || kind == G_PROP || kind == G_CHAR)) {
        NitroPort_Gx3dDecodeTextureTiled(banks, batch->texture_param, batch->palette_base, tiled);
        tex = tiled;
    } else if (kind == G_MASK) {
        tex = g_mask(banks, batch->texture_param, batch->palette_base);
        if (!tex) return;
    } else if (kind != G_GROUND && kind != G_RIM && kind != G_BLOB) {
        return;
    }
    for (k = batch->first; k + 2 < batch->first + batch->count; k += 3) {
        const NitroGx3dGpuVertex *v = &verts[k];
        float sx[3], sy[3], sz[3], iw[3], area;
        int j, x0, x1, y0, y1, x, y;
        for (j = 0; j < 3; j++) {
            float w = v[j].position[3];
            iw[j] = w != 0.0f ? 1.0f / w : 0.0f;
            sx[j] = (v[j].position[0] * iw[j] + 1.0f) * (float)W * 0.5f;
            sy[j] = (1.0f - v[j].position[1] * iw[j]) * (float)H * 0.5f;
            sz[j] = -v[j].position[2] * iw[j];
        }
        area = edge(sx[0], sy[0], sx[1], sy[1], sx[2], sy[2]);
        if (fabsf(area) < 1e-6f) continue;
        x0 = (int)floorf(fminf(sx[0], fminf(sx[1], sx[2])));
        x1 = (int)ceilf(fmaxf(sx[0], fmaxf(sx[1], sx[2])));
        y0 = (int)floorf(fminf(sy[0], fminf(sy[1], sy[2])));
        y1 = (int)ceilf(fmaxf(sy[0], fmaxf(sy[1], sy[2])));
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > W - 1) x1 = W - 1;
        if (y1 > H - 1) y1 = H - 1;
        for (y = y0; y <= y1; y++)
        for (x = x0; x <= x1; x++) {
            float px = x + 0.5f, py = y + 0.5f;
            float l0 = edge(sx[1], sy[1], sx[2], sy[2], px, py) / area;
            float l1 = edge(sx[2], sy[2], sx[0], sy[0], px, py) / area;
            float l2 = 1.0f - l0 - l1, z, pw, a = 1.0f, rgb[3];
            float pr[4], uv[2];
            unsigned idx = (unsigned)y * W + (unsigned)x, a8, c;
            if (l0 < -1e-6f || l1 < -1e-6f || l2 < -1e-6f) continue;
            /* (a pixel darkened once by a building's shadow (bit 1) and once
             * by the rest's (bit 2); a full one (a building's, or the
             * rest's core) both: port_gx3d_pica.c, once_stencil) */
            if (once && (sOnce[idx] & (once == 3 ? 3 : once))) continue;
            z = l0 * sz[0] + l1 * sz[1] + l2 * sz[2];
            pw = l0 * iw[0] + l1 * iw[1] + l2 * iw[2];
            if (pw == 0.0f) continue;
            if (wbuffer) z = z / pw;
            if (!(z <= depth[idx])) continue;
            for (c = 0; c < 4; c++)
                pr[c] = (l0 * iw[0] * v[0].color[c] + l1 * iw[1] * v[1].color[c] + l2 * iw[2] * v[2].color[c]) / pw / 255.0f;
            for (c = 0; c < 2; c++)
                uv[c] = (l0 * iw[0] * v[0].uv[c] + l1 * iw[1] * v[1].uv[c] + l2 * iw[2] * v[2].uv[c]) / pw;
            switch (kind) {
            case G_GROUND:
            case G_RIM:
                a = tex ? fetch_linear(tex, batch->texture_param, uv[0], uv[1]).a : pr[3];
                /* (a building's soft edge in the bands: how much of it, its grey) */
                if (kind == G_RIM) a *= pr[0];
                for (c = 0; c < 3; c++) rgb[c] = 1.0f - sGShade[c] * pr[c];
                break;
            case G_PROP:
                a = fetch_linear(tex, batch->texture_param, uv[0], uv[1]).a;
                for (c = 0; c < 3; c++) rgb[c] = pr[c];
                break;
            case G_MASK:
                a = fetch_linear(tex, batch->texture_param, uv[0], uv[1]).a;
                for (c = 0; c < 3; c++) rgb[c] = 1.0f - sGTree[c] * a;
                break;
            case G_BLOB: {
                float d = 4.0f * ((pr[0] - 0.5f) * (pr[0] - 0.5f) + (pr[1] - 0.5f) * (pr[1] - 0.5f) +
                                  (pr[2] - 0.5f) * (pr[2] - 0.5f));
                d = d > 1.0f ? 1.0f : d;
                a = 4.0f * (1.0f - d);
                a = a > 1.0f ? 1.0f : a;
                for (c = 0; c < 3; c++) rgb[c] = 1.0f - a * sGTree[c];
                break;
            }
            default:
                a = fetch_linear(tex, batch->texture_param, uv[0], uv[1]).a;
                for (c = 0; c < 3; c++) rgb[c] = pr[c] * a + (1.0f - a);
                break;
            }
            a8 = (unsigned)(a * 255.0f + 0.5f);
            if (test ? a8 < test : !(a8 > 0)) continue;
            if (once) sOnce[idx] |= (uint8_t)(once == 1 && kind == G_GROUND ? 3 : once);
            color[idx].r *= rgb[0] < 0.0f ? 0.0f : rgb[0];
            color[idx].g *= rgb[1] < 0.0f ? 0.0f : rgb[1];
            color[idx].b *= rgb[2] < 0.0f ? 0.0f : rgb[2];
        }
    }
}

static void ground_shadows(const NitroGx3dGpuVertex *verts, const NitroGx3dBatch *batches, size_t nb,
                           const NitroGx3dTexBanks *banks, uint32_t *tiled, Color *color, const float *depth,
                           int wbuffer)
{
    static const unsigned tests[4] = { 128, 250, 128, 8 };
    size_t b;
    int pass;
    if (!sGOnce) {
        for (b = 0; b < sGCardCount && sGReady; b++)
            if (!sGCards[b].tag) g_draw(verts, &sGCards[b], G_CHAR, 0, 0, banks, tiled, color, depth, wbuffer);
        return;
    }
    memset(sOnce, 0, sizeof(sOnce));
    for (pass = 0; pass < 4; pass++) {
        /* (the rest's: in the core band both bits, else its own) */
        const int rest = pass == 1 ? 3 : 2;
        for (b = 0; b < nb && sGShadeBatches; b++) {
            const NitroGx3dBatch *batch = &batches[b];
            uint32_t tag = batch->tag;
            if (batch->translucent) continue;
            if (pass == 0 ? tag != NITRO_GX3D_TAG_GROUND :
                (tag != NITRO_GX3D_TAG_SHADE && tag != NITRO_GX3D_TAG_TRUNK && tag != NITRO_GX3D_TAG_RIM)) continue;
            g_draw(verts, batch, pass == 0 ? G_GROUND : tag == NITRO_GX3D_TAG_RIM ? G_RIM :
                   tag == NITRO_GX3D_TAG_SHADE && batch->texture_param ? G_MASK : G_BLOB,
                   tests[pass], pass == 0 || tag == NITRO_GX3D_TAG_RIM ? 1 : rest, banks, tiled, color, depth, wbuffer);
        }
        if (pass == 2)
            for (b = 0; b < sGCardCount && sGReady; b++)
                if (sGCards[b].tag == NITRO_SHADOW_ONCE)
                    g_draw(verts, &sGCards[b], G_PROP, tests[pass], rest, banks, tiled, color, depth, wbuffer);
        if (pass >= 1 && pass <= 3)
            for (b = 0; b < sGCardCount && sGReady; b++)
                if (!sGCards[b].tag)
                    g_draw(verts, &sGCards[b], G_CHAR, tests[pass], rest, banks, tiled, color, depth, wbuffer);
    }
}

/* The scene's sun shadows made (port_gx3d_pica.c's prepare): the
 * characters standing in the trees' darkened, the characters' and props'
 * laid on the ground after the scene's vertices, the round shadows under
 * them left out. */
static void ground_prepare(const NitroGx3dScene *scene, NitroGx3dGpuVertex *verts, size_t nv, size_t room,
                           NitroGx3dBatch *batches, size_t nb, const NitroGx3dTexBanks *banks,
                           const NitroShadowSun *sun, float strength, const float shade[3], int wbuffer)
{
    size_t b, i, marked = 0;
    int c;
    sGReady = 0;
    sGCardCount = 0;
    sGShadeBatches = 0;
    sGOnce = 0;
    g_masks_free();
    NitroPort_SetSunNow(sun, strength);
    if (!(strength >= 0.02f)) return;
    for (c = 0; c < 3; c++) {
        /* (as port_gx3d_pica.c: the shade as bytes) */
        float shadeC = floorf(shade[c] * 255.0f + 0.5f) / 255.0f, f = (1.0f - shadeC) * NITRO_SHADOW_GROUND_DARK;
        f = f > 0.0f ? (f > 1.0f ? 1.0f : f) : 0.0f;
        sGShade[c] = floorf(f * 255.0f + 0.5f) / 255.0f;
        sGTree[c] = floorf(f * NITRO_SHADOW_TREE_BLOB * 255.0f + 0.5f) / 255.0f;
    }
    for (b = 0; b < nb; b++)
        if (NITRO_GX3D_TAG_IS_SHADE(batches[b].tag)) sGShadeBatches++;
    if (sGShadeBatches) {
        float f[3];
        for (c = 0; c < 3; c++) {
            float shadeC = floorf(shade[c] * 255.0f + 0.5f) / 255.0f;
            f[c] = (1.0f - shadeC) * NITRO_SHADOW_GROUND_DARK;
            f[c] = f[c] > 0.0f ? (f[c] > 1.0f ? 1.0f : f[c]) : 0.0f;
        }
        NitroPort_ShadowDarkenSprites(verts, batches, nb, f, g_mask_at, (void *)banks);
    }
    for (i = 0; i < scene->tagged_count && !marked; i++) {
        uint32_t tag = scene->tagged_triangles[i].tag;
        marked = NITRO_GX3D_TAG_IS_ROUND(tag) || NITRO_GX3D_TAG_IS_PROP(tag);
    }
    if (marked) {
        NitroShadowSetup setup;
        size_t ntb = 0, nt, n = 0, nc = 0;
        float depthRow[4], cardShade[3];
        int ready;
        nt = NitroPort_Gx3dBuildTaggedBatches(scene, nv, verts + nv, room, sGTag, NITRO_GX3D_MAX_TAGGED_TRIANGLES, &ntb);
        ready = NitroPort_ShadowGroundSetup(verts, sGTag, ntb, scene->projection, scene->screen_width,
                                            scene->screen_height, sun, &setup);
        if (!ready)
            ready = NitroPort_ShadowSetup(verts[0].position, sizeof(verts[0]), nv, scene->projection,
                                          scene->screen_width, scene->screen_height, sun, SHADOW_SIZE, &setup);
        if (ready) {
            /* (the depths as the scene's: no remap here) */
            NitroPort_ShadowDepthRow(&setup, wbuffer, 0.0f, 1.0f, depthRow);
            for (c = 0; c < 3; c++) cardShade[c] = floorf(shade[c] * 255.0f + 0.5f) / 255.0f;
            memset(sGRoundUsed, 0, ntb);
            n = NitroPort_ShadowGroundCards(verts, sGTag, ntb, nv + nt, verts + nv + nt, room - nt, sGCards,
                                            G_CARD_BATCHES, &nc, &setup, depthRow, sGRoundUsed, cardShade,
                                            g_foot_row, (void *)banks);
        }
        if (n && nc) {
            size_t t;
            for (t = 0; t < ntb; t++) {
                if (!sGRoundUsed[t]) continue;
                for (b = 0; b < nb; b++) {
                    NitroGx3dGpuVertex *rv = verts + batches[b].first;
                    uint32_t j;
                    if (batches[b].tag != sGTag[t].tag) continue;
                    for (j = 1; j < batches[b].count; j++) memcpy(rv[j].position, rv[0].position, sizeof(rv[0].position));
                }
            }
            sGCardCount = nc;
            sGReady = 1;
        }
    }
    sGOnce = sGShadeBatches != 0;
    for (b = 0; b < sGCardCount && sGReady; b++) sGOnce |= sGCards[b].tag == NITRO_SHADOW_ONCE;
}

static void simulate(const NitroGx3dScene *scene, uint16_t *out555);

void NitroPort_Gx3dSimulateGpu(const NitroGx3dScene *scene, uint16_t *out555)
{
    sW = NITRO_GX3D_WIDTH;
    sH = NITRO_GX3D_HEIGHT;
    simulate(scene, out555);
}

void NitroPort_Gx3dSimulateGpuSized(const NitroGx3dScene *scene, uint16_t *out555, int width, int height)
{
    sW = width > 0 && width <= MAX_W ? width : NITRO_GX3D_WIDTH;
    sH = height > 0 && height <= MAX_H ? height : NITRO_GX3D_HEIGHT;
    simulate(scene, out555);
    sW = NITRO_GX3D_WIDTH;
    sH = NITRO_GX3D_HEIGHT;
}

static void simulate(const NitroGx3dScene *scene, uint16_t *out555)
{
    /* (after the scene's, the sun shadows' own, as port_gx3d_pica.c) */
    static NitroGx3dGpuVertex verts[NITRO_GX3D_PICA_MAX_VERTICES + 3 * NITRO_GX3D_MAX_SHADOW_TRIANGLES +
                                    3 * SHADOW_CARD_TRIANGLES];
    static uint32_t decalTexels[4096];
    static NitroGx3dBatch batches[NITRO_GX3D_MAX_TRIANGLES + 2048 + SHADOW_CARD_BATCHES];
    static uint32_t tiled[1024 * 1024];
    static Color color[MAX_W * MAX_H];
    static float depth[MAX_W * MAX_H];
    /* As port_gx3d_pica.c keeps it: translucent polygon ID + 64, 0xFF:
     * none; then the opaque polygon IDs for edge marking. */
    static uint8_t stencil[MAX_W * MAX_H];
    static uint32_t depthId[MAX_W * MAX_H];
    static uint8_t alpha[MAX_W * MAX_H];
    NitroGx3dTexBanks banks;
    NitroGx3dEdgeSetup edges;
    NitroShadowSetup shadow;
    NitroShadowSun sun = sFixedSun;
    float strength = 1.0f, shade[3];
    int idPass, phase, shadowed = 0, masked = 0;
    size_t nb = 0, b, i, nv;
    unsigned dropped;
    uint32_t clear = (uint32_t)NitroPort_IoReg[0x350] | ((uint32_t)NitroPort_IoReg[0x351] << 8) |
                     ((uint32_t)NitroPort_IoReg[0x352] << 16) | ((uint32_t)NitroPort_IoReg[0x353] << 24);
    int blend = (NitroPort_IoReg[0x60] & 8u) != 0;
    int wbuffer = (scene->swap_param & 2u) != 0;
    Color cc;

    cc.r = (float)(((clear & 31u) << 3) | ((clear & 31u) >> 2)) / 255.0f;
    cc.g = (float)((((clear >> 5) & 31u) << 3) | (((clear >> 5) & 31u) >> 2)) / 255.0f;
    cc.b = (float)((((clear >> 10) & 31u) << 3) | (((clear >> 10) & 31u) >> 2)) / 255.0f;
    cc.a = (float)((((clear >> 16) & 31u) << 3) | (((clear >> 16) & 31u) >> 2)) / 255.0f;
    if (cc.a == 0.0f) cc.r = cc.g = cc.b = 0.0f;   /* as port_gx3d_pica.c */
    for (i = 0; i < W * H; i++) {
        color[i] = cc;
        depth[i] = 1.0f;
        stencil[i] = 0xFF;
    }
    NitroPort_Gx3dMapTextureBanks(&banks);
    nv = NitroPort_Gx3dBuildBatches(scene, verts, NITRO_GX3D_PICA_MAX_VERTICES, batches,
                                    NITRO_GX3D_MAX_TRIANGLES, &nb, &dropped);
    /* the look at this time of day, as port_gx3d_pica.c's look_now */
    {
        time_t now = time(NULL);
        struct tm local;
        int hour = 12, minute = 0;
        if (now != (time_t)-1 && localtime_r(&now, &local)) { hour = local.tm_hour; minute = local.tm_min; }
        if (!sSunFixed) NitroPort_ShadowSunAt(hour, minute, &sun, &strength);
        if (sGrade) {
            NitroPort_LookGradeTable(NitroPort_LookPeriod(hour), sGradeTable);
            NitroPort_Gx3dSetGrade(sGradeTable);
        }
        shade[0] = 1.0f - (1.0f - NITRO_SHADOW_SHADE_R / 255.0f) * strength;
        shade[1] = 1.0f - (1.0f - NITRO_SHADOW_SHADE_G / 255.0f) * strength;
        shade[2] = 1.0f - (1.0f - NITRO_SHADOW_SHADE_B / 255.0f) * strength;
    }
    if (sGround && nv)
        ground_prepare(scene, verts, nv, 3 * NITRO_GX3D_MAX_SHADOW_TRIANGLES + 3 * SHADOW_CARD_TRIANGLES, batches, nb,
                       &banks, &sun, strength, shade, wbuffer);
    else
        sGReady = 0, sGShadeBatches = 0;
    if (sShadows && nv && strength >= 0.02f &&
        NitroPort_ShadowSetup(verts[0].position, sizeof(verts[0]), nv, scene->projection, scene->screen_width, scene->screen_height,
                              &sun, SHADOW_SIZE, &shadow)) {
        size_t ns = 0, n;
        shadowed = 1;
        n = NitroPort_Gx3dBuildShadowBatches(scene, nv, verts + nv, 3 * NITRO_GX3D_MAX_SHADOW_TRIANGLES,
                                             batches + nb, 2048, &ns);
        NitroPort_ShadowRoles(verts[0].position, &verts[0].color[3], sizeof(verts[0]), nv + n, &shadow);
        {
            /* the cards' copies turned to the sun, after the shadows' own */
            size_t nc = 0;
            n += NitroPort_ShadowCards(verts, batches, nb, nv + n, verts + nv + n,
                                       3 * NITRO_GX3D_MAX_SHADOW_TRIANGLES + 3 * SHADOW_CARD_TRIANGLES - n,
                                       batches + nb + ns, SHADOW_CARD_BATCHES, &nc, &shadow);
            ns += nc;
        }
        NitroPort_ShadowReach(verts[nv].position, sizeof(verts[0]), n, &shadow);
        /* the game's own painted shadows, left out */
        for (b = 0; b < nb; b++) {
            const NitroGx3dBatch *batch = &batches[b];
            unsigned fmt = (batch->texture_param >> 26) & 7u;
            sDecal[b] = 0;
            if (!batch->translucent || !batch->count) continue;
            if (fmt && NitroPort_Gx3dTexWidth(batch->texture_param) * NitroPort_Gx3dTexHeight(batch->texture_param) > 4096u) continue;
            if (fmt) NitroPort_Gx3dDecodeTexture(&banks, batch->texture_param, batch->palette_base, decalTexels);
            sDecal[b] = (uint8_t)NitroPort_ShadowDecal(verts[batch->first].position, sizeof(verts[0]),
                                                       (batch->polygon_attr >> 16) & 31u, fmt == 1u || fmt == 6u,
                                                       fmt ? decalTexels : NULL,
                                                       fmt ? NitroPort_Gx3dTexWidth(batch->texture_param) *
                                                             NitroPort_Gx3dTexHeight(batch->texture_param) : 0,
                                                       0x7FFF, &shadow);
        }
        shadow_map(verts, batches, nb + ns, &banks, tiled, &shadow);
        for (i = 0; i < W * H; i++) sLit[i] = 1.0f;
    }
    NitroPort_Gx3dEdgeSetup(&edges);
    edges.clearDepth = 0xFFFFFFu;   /* the target is cleared to the farthest */
    /* Pass 1 draws; with edge marking, pass 2 writes the opaque pixels'
     * polygon IDs to the stencil where they're in front (port_gx3d_pica.c). */
    /* (as port_gx3d_pica.c: the opaque polygons, the opaque texels of the
     * see-through ones; the sun shadows; then the see-through polygons) */
    for (idPass = 0; idPass < (edges.enabled ? 2 : 1); idPass++)
    for (phase = 0; phase < (idPass ? 1 : 2); phase++)
    for (b = 0; b <= nb; b++) {
        const NitroGx3dBatch *batch = &batches[b < nb ? b : 0];
        /* the sun shadows' light, after the opaque polygons (as the 3DS) */
        if (!idPass && phase && shadowed && !masked) {
            apply_mask(color, shade);
            masked = 1;
        }
        if (!idPass && phase && sGround && (sGReady || sGShadeBatches) && !masked) {
            ground_shadows(verts, batches, nb, &banks, tiled, color, depth, wbuffer);
            masked = 1;
        }
        if (b == nb) break;
        if (shadowed && sDecal[b]) continue;   /* (one of the game's painted shadows) */
        if (NITRO_GX3D_TAG_IS_SHADE(batch->tag)) continue;   /* (drawn as shadows, above, or not at all) */
        {
        unsigned mode = (batch->polygon_attr >> 4) & 3u;
        int textured = batch->texture_param != 0;
        int equal = (batch->polygon_attr >> 14) & 1;
        int bit11 = (batch->polygon_attr >> 11) & 1;
        /* The draws port_gx3d_pica.c makes for this batch: alpha test
         * (0: alpha > 0, 1: alpha > 254, 2: alpha < 255), blending, depth
         * writes. */
        struct { int test, blend, writeDepth, translucent; } passes[2];
        unsigned id = (batch->polygon_attr >> 24) & 63u;
        int np = 0, p;
        uint32_t k;
        /* The stencil buffer keeps the polygon ID of each pixel's last
         * translucent draw (0xFF after an opaque one), as port_gx3d_pica.c
         * sets it up. */
        if (!batch->translucent) {
            passes[np].test = 0; passes[np].blend = 0; passes[np].translucent = 0;
            passes[np++].writeDepth = 1;
        } else {
            if (((batch->polygon_attr >> 16) & 31u) == 31u) {
                passes[np].test = 1; passes[np].blend = 0; passes[np].translucent = 0;
                passes[np++].writeDepth = 1;
                passes[np].test = 0;
            } else {
                passes[np].test = 0;
            }
            passes[np].translucent = 1;
            passes[np].blend = blend; passes[np++].writeDepth = bit11;
        }
        if (idPass) {
            if (batch->translucent && ((batch->polygon_attr >> 16) & 31u) != 31u) continue;
            passes[0].test = batch->translucent ? 1 : 0;
            passes[0].blend = 0;
            passes[0].writeDepth = 0;
            passes[0].translucent = 0;
            np = 1;
        }
        if (textured)
            NitroPort_Gx3dDecodeTextureTiled(&banks, batch->texture_param, batch->palette_base, tiled);
        for (p = 0; p < np; p++)
        for (k = batch->first; k + 2 < batch->first + batch->count; k += 3) {
            if (!idPass && passes[p].translucent != phase) break;
            const NitroGx3dGpuVertex *v = &verts[k];
            float sx[3], sy[3], sz[3], iw[3], area;
            int j, x0, x1, y0, y1, x, y;
            for (j = 0; j < 3; j++) {
                float w = v[j].position[3];
                iw[j] = w != 0.0f ? 1.0f / w : 0.0f;
                sx[j] = (v[j].position[0] * iw[j] + 1.0f) * (float)W * 0.5f;
                sy[j] = (1.0f - v[j].position[1] * iw[j]) * (float)H * 0.5f;
                sz[j] = -v[j].position[2] * iw[j];
            }
            area = edge(sx[0], sy[0], sx[1], sy[1], sx[2], sy[2]);
            if (fabsf(area) < 1e-6f) continue;
            x0 = (int)floorf(fminf(sx[0], fminf(sx[1], sx[2])));
            x1 = (int)ceilf(fmaxf(sx[0], fmaxf(sx[1], sx[2])));
            y0 = (int)floorf(fminf(sy[0], fminf(sy[1], sy[2])));
            y1 = (int)ceilf(fmaxf(sy[0], fmaxf(sy[1], sy[2])));
            if (x0 < 0) x0 = 0;
            if (y0 < 0) y0 = 0;
            if (x1 > W - 1) x1 = W - 1;
            if (y1 > H - 1) y1 = H - 1;
            for (y = y0; y <= y1; y++) {
                for (x = x0; x <= x1; x++) {
                    float px = x + 0.5f, py = y + 0.5f;
                    float l0 = edge(sx[1], sy[1], sx[2], sy[2], px, py) / area;
                    float l1 = edge(sx[2], sy[2], sx[0], sy[0], px, py) / area;
                    float l2 = 1.0f - l0 - l1;
                    float z, pw, u, tv;
                    Color prim, tex, out;
                    unsigned idx = (unsigned)y * W + (unsigned)x;
                    if (l0 < -1e-6f || l1 < -1e-6f || l2 < -1e-6f) continue;
                    if (passes[p].translucent && stencil[idx] == (64u | id)) continue;
                    z = l0 * sz[0] + l1 * sz[1] + l2 * sz[2];
                    pw = l0 * iw[0] + l1 * iw[1] + l2 * iw[2];
                    /* The PICA's W-buffer mode, which W-buffered scenes
                     * use: z / W as interpolated times the
                     * perspective-correct W. */
                    if (wbuffer)
                        z = pw != 0.0f ? z / pw : 1.0f;
                    if (idPass ? z != depth[idx] : equal ? !(z <= depth[idx]) : !(z < depth[idx])) continue;
                    if (pw == 0.0f) continue;
                    /* Perspective-correct, as the PICA interpolates. */
                    prim.r = (l0 * iw[0] * v[0].color[0] + l1 * iw[1] * v[1].color[0] + l2 * iw[2] * v[2].color[0]) / pw / 255.0f;
                    prim.g = (l0 * iw[0] * v[0].color[1] + l1 * iw[1] * v[1].color[1] + l2 * iw[2] * v[2].color[1]) / pw / 255.0f;
                    prim.b = (l0 * iw[0] * v[0].color[2] + l1 * iw[1] * v[1].color[2] + l2 * iw[2] * v[2].color[2]) / pw / 255.0f;
                    prim.a = (l0 * iw[0] * v[0].color[3] + l1 * iw[1] * v[1].color[3] + l2 * iw[2] * v[2].color[3]) / pw / 255.0f;
                    if (!textured) {
                        out = prim;
                    } else {
                        u = (l0 * iw[0] * v[0].uv[0] + l1 * iw[1] * v[1].uv[0] + l2 * iw[2] * v[2].uv[0]) / pw;
                        tv = (l0 * iw[0] * v[0].uv[1] + l1 * iw[1] * v[1].uv[1] + l2 * iw[2] * v[2].uv[1]) / pw;
                        tex = fetch(tiled, batch->texture_param, u, tv);
                        if (mode == 1) {
                            out.r = tex.r * tex.a + prim.r * (1.0f - tex.a);
                            out.g = tex.g * tex.a + prim.g * (1.0f - tex.a);
                            out.b = tex.b * tex.a + prim.b * (1.0f - tex.a);
                            out.a = prim.a;
                        } else {
                            out.r = tex.r * prim.r;
                            out.g = tex.g * prim.g;
                            out.b = tex.b * prim.b;
                            out.a = tex.a * prim.a;
                        }
                    }
                    /* (a tree's layers and a building's walls lit by the
                     * sun: their colour twice, port_gx3d_pica.c) */
                    if (batch->tag == NITRO_GX3D_TAG_CANOPY || NITRO_GX3D_TAG_IS_WALLS(batch->tag)) {
                        out.r = fminf(out.r * 2.0f, 1.0f);
                        out.g = fminf(out.g * 2.0f, 1.0f);
                        out.b = fminf(out.b * 2.0f, 1.0f);
                    }
                    /* Alpha test, on 8-bit alpha as the PICA does it. */
                    if (sGround && textured && batch->tag == NITRO_GX3D_TAG_STUMP && passes[p].test == 0) {
                        /* (a tree's stump without its patch: stump_combiner) */
                        float st = fminf((1.0f - tex.g) + tex.r, 1.0f) * tex.a;
                        if (!((unsigned)(st * 255.0f + 0.5f) > 250u)) continue;
                    } else {
                        unsigned a8 = (unsigned)(out.a * 255.0f + 0.5f);
                        if (passes[p].test == 0 ? !(a8 > 0) : passes[p].test == 1 ? !(a8 > 254) : !(a8 < 255))
                            continue;
                    }
                    if (idPass) {
                        stencil[idx] = (uint8_t)id;
                        continue;
                    }
                    /* sun shadows: on the opaque polygons' pass */
                    if (shadowed && !batch->translucent) {
                        float su = 0.0f, sv = 0.0f, sd = 0.0f, lit;
                        for (j = 0; j < 3; j++) {
                            float wgt = (j == 0 ? l0 : j == 1 ? l1 : l2) * iw[j] / pw;
                            su += wgt * row4(shadow.receiver[0], v[j].position);
                            sv += wgt * row4(shadow.receiver[1], v[j].position);
                            sd += wgt * row4(shadow.receiver[3], v[j].position);
                        }
                        lit = (v[0].color[3] == NITRO_SHADOW_CAST_ONLY ||
                               v[0].color[3] == NITRO_SHADOW_NEITHER) ? 1.0f   /* (upright, a card: doesn't receive) */
                              : shadow_lookup(su, sv, sd - shadow.bias * NITRO_SHADOW_BIAS_SCALE);
                        sLit[idx] = lit;   /* (put on with the mask, apply_mask) */
                    }
                    if (passes[p].blend) {
                        Color d = color[idx];
                        out.r = out.r * out.a + d.r * (1.0f - out.a);
                        out.g = out.g * out.a + d.g * (1.0f - out.a);
                        out.b = out.b * out.a + d.b * (1.0f - out.a);
                        out.a = out.a + d.a * (1.0f - out.a);
                    }
                    color[idx] = out;
                    if (passes[p].writeDepth) depth[idx] = z;
                    stencil[idx] = passes[p].translucent ? (uint8_t)(64u | id) : 0xFF;
                }
            }
        }
        }
    }
    NitroPort_Gx3dSetGrade(NULL);
    for (i = 0; i < W * H; i++) {
        /* Un-premultiply partly transparent pixels, as publishing does. */
        if (color[i].a > 0.0f && color[i].a < 1.0f) {
            color[i].r = fminf(color[i].r / color[i].a, 1.0f);
            color[i].g = fminf(color[i].g / color[i].a, 1.0f);
            color[i].b = fminf(color[i].b / color[i].a, 1.0f);
        }
    }
    for (i = 0; i < W * H; i++) {
        unsigned r = (unsigned)(color[i].r * 255.0f + 0.5f) >> 3;
        unsigned g = (unsigned)(color[i].g * 255.0f + 0.5f) >> 3;
        unsigned bl = (unsigned)(color[i].b * 255.0f + 0.5f) >> 3;
        out555[i] = (uint16_t)(r | (g << 5) | (bl << 10) | (color[i].a > 0.0f ? 0x8000u : 0u));
    }
    /* (NitroPort_Gx3dMarkEdges outlines a DS-sized image: at a view's own
     * size, the 3DS's GPU draws the outlines, not shown here; taken as
     * 256 wide, a wider picture's rows came out in black bands) */
    if (edges.enabled && W == NITRO_GX3D_WIDTH && H == NITRO_GX3D_HEIGHT) {
        for (i = 0; i < W * H; i++) {
            float z = depth[i] < 0.0f ? 0.0f : depth[i] > 1.0f ? 1.0f : depth[i];
            depthId[i] = (uint32_t)(z * 16777215.0f) | ((uint32_t)stencil[i] << 24);
            alpha[i] = (uint8_t)((unsigned)(color[i].a * 255.0f + 0.5f) >> 3);
        }
        NitroPort_Gx3dMarkEdges(out555, alpha, depthId, &edges, NULL);
    }
}
