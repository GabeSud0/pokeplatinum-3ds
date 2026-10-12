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
#include <math.h>
#include <stdlib.h>
#include <string.h>

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

void NitroPort_Gx3dSimulateGpu(const NitroGx3dScene *scene, uint16_t *out555)
{
    static NitroGx3dGpuVertex verts[NITRO_GX3D_PICA_MAX_VERTICES];
    static NitroGx3dBatch batches[NITRO_GX3D_MAX_TRIANGLES];
    static uint32_t tiled[1024 * 1024];
    static Color color[W * H];
    static float depth[W * H];
    NitroGx3dTexBanks banks;
    size_t nb = 0, b, i;
    unsigned dropped;
    uint32_t clear = (uint32_t)NitroPort_IoReg[0x350] | ((uint32_t)NitroPort_IoReg[0x351] << 8) |
                     ((uint32_t)NitroPort_IoReg[0x352] << 16) | ((uint32_t)NitroPort_IoReg[0x353] << 24);
    int blend = (NitroPort_IoReg[0x60] & 8u) != 0;
    Color cc;

    cc.r = (float)(((clear & 31u) << 3) | ((clear & 31u) >> 2)) / 255.0f;
    cc.g = (float)((((clear >> 5) & 31u) << 3) | (((clear >> 5) & 31u) >> 2)) / 255.0f;
    cc.b = (float)((((clear >> 10) & 31u) << 3) | (((clear >> 10) & 31u) >> 2)) / 255.0f;
    cc.a = (float)((((clear >> 16) & 31u) << 3) | (((clear >> 16) & 31u) >> 2)) / 255.0f;
    if (cc.a == 0.0f) cc.r = cc.g = cc.b = 0.0f;   /* as port_gx3d_pica.c */
    for (i = 0; i < W * H; i++) {
        color[i] = cc;
        depth[i] = 1.0f;
    }
    NitroPort_Gx3dMapTextureBanks(&banks);
    NitroPort_Gx3dBuildBatches(scene, verts, NITRO_GX3D_PICA_MAX_VERTICES, batches,
                               NITRO_GX3D_MAX_TRIANGLES, &nb, &dropped);
    for (b = 0; b < nb; b++) {
        const NitroGx3dBatch *batch = &batches[b];
        unsigned mode = (batch->polygon_attr >> 4) & 3u;
        int textured = batch->texture_param != 0;
        int equal = (batch->polygon_attr >> 14) & 1;
        int bit11 = (batch->polygon_attr >> 11) & 1;
        /* The draws port_gx3d_pica.c makes for this batch: alpha test
         * (0: alpha > 0, 1: alpha > 254, 2: alpha < 255), blending, depth
         * writes. */
        struct { int test, blend, writeDepth; } passes[2];
        int np = 0, p;
        uint32_t k;
        if (!batch->translucent) {
            passes[np].test = 0; passes[np].blend = 0; passes[np++].writeDepth = 1;
        } else {
            if (((batch->polygon_attr >> 16) & 31u) == 31u) {
                passes[np].test = 1; passes[np].blend = 0; passes[np++].writeDepth = 1;
                passes[np].test = blend ? 2 : 0;
            } else {
                passes[np].test = 0;
            }
            passes[np].blend = blend; passes[np++].writeDepth = bit11;
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
                    z = l0 * sz[0] + l1 * sz[1] + l2 * sz[2];
                    if (equal ? !(z <= depth[idx]) : !(z < depth[idx])) continue;
                    pw = l0 * iw[0] + l1 * iw[1] + l2 * iw[2];
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
                    if (passes[p].blend) {
                        Color d = color[idx];
                        out.r = out.r * out.a + d.r * (1.0f - out.a);
                        out.g = out.g * out.a + d.g * (1.0f - out.a);
                        out.b = out.b * out.a + d.b * (1.0f - out.a);
                        out.a = out.a + d.a * (1.0f - out.a);
                    }
                    color[idx] = out;
                    if (passes[p].writeDepth) depth[idx] = z;
                }
            }
        }
    }
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
}
