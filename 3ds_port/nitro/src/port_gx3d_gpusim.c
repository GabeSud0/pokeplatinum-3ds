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

#define W NITRO_GX3D_WIDTH
#define H NITRO_GX3D_HEIGHT

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
static float sLit[W * H];   /* (the opaque pixels' light, for the mask) */
static uint32_t sGradeTable[32768];

void NitroPort_Gx3dSimulateLook(int shadows, int grade, int fixedSun, float elevation, float azimuth)
{
    sShadows = shadows;
    sGrade = grade;
    sSunFixed = fixedSun;
    sFixedSun.elevation = elevation;
    sFixedSun.azimuth = azimuth;
    NitroPort_SetSunShadows(shadows);
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
            if (v[0].color[3] == NITRO_SHADOW_RECEIVE_ONLY) continue;   /* (lying flat: doesn't cast) */
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
    enum { MW = W / 2, MH = H / 2 };
    static float mask[MW * MH], blurred[MW * MH];
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

void NitroPort_Gx3dSimulateGpu(const NitroGx3dScene *scene, uint16_t *out555)
{
    /* (after the scene's, the sun shadows' own, as port_gx3d_pica.c) */
    static NitroGx3dGpuVertex verts[NITRO_GX3D_PICA_MAX_VERTICES + 3 * NITRO_GX3D_MAX_SHADOW_TRIANGLES];
    static NitroGx3dBatch batches[NITRO_GX3D_MAX_TRIANGLES + 256];
    static uint32_t tiled[1024 * 1024];
    static Color color[W * H];
    static float depth[W * H];
    /* As port_gx3d_pica.c keeps it: translucent polygon ID + 64, 0xFF:
     * none; then the opaque polygon IDs for edge marking. */
    static uint8_t stencil[W * H];
    static uint32_t depthId[W * H];
    static uint8_t alpha[W * H];
    NitroGx3dTexBanks banks;
    NitroGx3dEdgeSetup edges;
    NitroShadowSetup shadow;
    NitroShadowSun sun = sFixedSun;
    float strength = 1.0f, shade[3];
    int idPass, shadowed = 0, masked = 0;
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
    if (sShadows && nv && strength >= 0.02f &&
        NitroPort_ShadowSetup(verts[0].position, sizeof(verts[0]), nv, scene->projection, scene->screen_width,
                              &sun, SHADOW_SIZE, &shadow)) {
        size_t ns = 0, n;
        shadowed = 1;
        n = NitroPort_Gx3dBuildShadowBatches(scene, nv, verts + nv, 3 * NITRO_GX3D_MAX_SHADOW_TRIANGLES,
                                             batches + nb, 256, &ns);
        NitroPort_ShadowReach(verts[nv].position, sizeof(verts[0]), n, &shadow);
        NitroPort_ShadowRoles(verts[0].position, &verts[0].color[3], sizeof(verts[0]), nv + n, &shadow);
        shadow_map(verts, batches, nb + ns, &banks, tiled, &shadow);
        for (i = 0; i < W * H; i++) sLit[i] = 1.0f;
    }
    NitroPort_Gx3dEdgeSetup(&edges);
    edges.clearDepth = 0xFFFFFFu;   /* the target is cleared to the farthest */
    /* Pass 1 draws; with edge marking, pass 2 writes the opaque pixels'
     * polygon IDs to the stencil where they're in front (port_gx3d_pica.c). */
    for (idPass = 0; idPass < (edges.enabled ? 2 : 1); idPass++)
    for (b = 0; b <= nb; b++) {
        const NitroGx3dBatch *batch = &batches[b < nb ? b : 0];
        /* the sun shadows' light, after the opaque polygons (as the 3DS) */
        if (!idPass && shadowed && !masked && (b == nb || batch->translucent)) {
            apply_mask(color, shade);
            masked = 1;
        }
        if (b == nb) break;
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
            const NitroGx3dGpuVertex *v = &verts[k];
            float sx[3], sy[3], sz[3], iw[3], area;
            int j, x0, x1, y0, y1, x, y;
            for (j = 0; j < 3; j++) {
                float w = v[j].position[3];
                iw[j] = w != 0.0f ? 1.0f / w : 0.0f;
                sx[j] = (v[j].position[0] * iw[j] + 1.0f) * 128.0f;
                sy[j] = (1.0f - v[j].position[1] * iw[j]) * 96.0f;
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
                    /* Alpha test, on 8-bit alpha as the PICA does it. */
                    {
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
                        lit = v[0].color[3] == NITRO_SHADOW_CAST_ONLY ? 1.0f   /* (upright: doesn't receive) */
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
    if (edges.enabled) {
        for (i = 0; i < W * H; i++) {
            float z = depth[i] < 0.0f ? 0.0f : depth[i] > 1.0f ? 1.0f : depth[i];
            depthId[i] = (uint32_t)(z * 16777215.0f) | ((uint32_t)stencil[i] << 24);
            alpha[i] = (uint8_t)((unsigned)(color[i].a * 255.0f + 0.5f) >> 3);
        }
        NitroPort_Gx3dMarkEdges(out555, alpha, depthId, &edges, NULL);
    }
}
