/*
 * Sun shadows: where the sun is for a scene, and the shadow map's matrices.
 * See include/nitro_port_shadow.h.
 */
#include <nitro_port_shadow.h>
#include <nitro_port_gx3d_vertex.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct { float x, y, z; } V3;

static V3 v3(float x, float y, float z) { V3 r = { x, y, z }; return r; }
static V3 add(V3 a, V3 b) { return v3(a.x + b.x, a.y + b.y, a.z + b.z); }
static V3 sub(V3 a, V3 b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
static V3 scale(V3 a, float k) { return v3(a.x * k, a.y * k, a.z * k); }
static float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static V3 cross(V3 a, V3 b) { return v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
static float length(V3 a) { return sqrtf(dot(a, a)); }
static V3 normalize(V3 a)
{
    float l = length(a);
    return l > 0.0f ? scale(a, 1.0f / l) : a;
}

/* The camera's space from (x, y, w, 1): rows for x, y, z. */
typedef struct { float m[3][4]; } Back;

static V3 back(const Back *b, const float *p)
{
    float q[4] = { p[0], p[1], p[3], 1.0f };
    V3 r;
    r.x = b->m[0][0] * q[0] + b->m[0][1] * q[1] + b->m[0][2] * q[2] + b->m[0][3];
    r.y = b->m[1][0] * q[0] + b->m[1][1] * q[1] + b->m[1][2] * q[2] + b->m[1][3];
    r.z = b->m[2][0] * q[0] + b->m[2][1] * q[1] + b->m[2][2] * q[2] + b->m[2][3];
    return r;
}

/* A row (on x, y, w, 1) giving dot(axis, camera space) * k + c. */
static void row(float *out, const Back *b, V3 axis, float k, float c)
{
    int j;
    for (j = 0; j < 4; j++) {
        out[j] = (axis.x * b->m[0][j] + axis.y * b->m[1][j] + axis.z * b->m[2][j]) * k;
    }
    out[3] += c;
}

/* The ground: normals binned (each axis in 1/16ths), the bin with the most
 * area wins; its normals' area-weighted mean. */
#define BINS 64
typedef struct { int key[3]; float area; V3 sum; int used; } Bin;

/* The camera's space back from the projection (p: it as floats). 0 if it
 * isn't a camera's perspective: w from z alone (p[3] = p[7] = p[15] = 0,
 * p[11] != 0), x and y not mixed. */
static int camera_back(const int32_t projection[16], unsigned screenWidth, unsigned screenHeight, float p[16],
                       Back *b)
{
    const float k = 1.0f / 4096.0f;
    int j;
    for (j = 0; j < 16; j++) p[j] = (float)projection[j] * k;
    if (p[3] != 0.0f || p[7] != 0.0f || p[15] != 0.0f || p[11] == 0.0f) return 0;
    if (p[0] == 0.0f || p[5] == 0.0f || p[1] != 0.0f || p[4] != 0.0f) return 0;
    {
        /* z = w / p23; x = (x' * wide - z p20 - p30) / p00; y likewise */
        float wide = (float)(screenWidth ? screenWidth : 256u) / 256.0f;
        float tall = (float)(screenHeight ? screenHeight : 192u) / 192.0f;   /* (the field at the DS's pixel size) */
        memset(b, 0, sizeof(*b));
        b->m[2][2] = 1.0f / p[11];
        b->m[0][0] = wide / p[0];
        b->m[0][2] = -p[8] / (p[11] * p[0]);
        b->m[0][3] = -p[12] / p[0];
        b->m[1][1] = tall / p[5];
        b->m[1][2] = -p[9] / (p[11] * p[5]);
        b->m[1][3] = -p[13] / p[5];
    }
    return 1;
}

/* Where the sun's light goes, over ground with the normal up: along the
 * ground, the screen's right (x) and towards the camera (t). */
static V3 sun_light(V3 up, const NitroShadowSun *sun, float *elevation)
{
    V3 x = normalize(sub(v3(1.0f, 0.0f, 0.0f), scale(up, up.x)));
    V3 t = cross(x, up);
    float e = sun->elevation * 3.14159265f / 180.0f, a = sun->azimuth * 3.14159265f / 180.0f;
    if (t.z < 0.0f) t = scale(t, -1.0f);
    if (elevation) *elevation = e;
    return normalize(add(scale(up, -sinf(e)), scale(add(scale(x, cosf(a)), scale(t, sinf(a))), cosf(e))));
}

/* (what both setups leave for the shadows laid on the ground) */
static void setup_camera(NitroShadowSetup *out, const float p[16], const Back *b, V3 up, V3 d)
{
    int j;
    out->up[0] = up.x; out->up[1] = up.y; out->up[2] = up.z;
    out->light[0] = d.x; out->light[1] = d.y; out->light[2] = d.z;
    memcpy(out->back, b->m, sizeof(out->back));
    /* clip z = camera x p02 + y p12 + z p22 + p32 (the DS's rows), the
     * camera's space through back */
    for (j = 0; j < 4; j++) out->zclip[j] = p[2] * b->m[0][j] + p[6] * b->m[1][j] + p[10] * b->m[2][j];
    out->zclip[3] += p[14];
}

int NitroPort_ShadowSetup(const float *positions, size_t stride, size_t count,
                          const int32_t projection[16], unsigned screenWidth, unsigned screenHeight,
                          const NitroShadowSun *sun, unsigned mapSize, NitroShadowSetup *out)
{
    float p[16];
    Back b;
    Bin bins[BINS];
    float total = 0.0f, lo1 = 1e30f, hi1 = -1e30f, lo2 = 1e30f, hi2 = -1e30f, lod = 1e30f, hid = -1e30f;
    float extent, texel, o1, o2, range, e;
    V3 up, d, b1, b2;
    size_t i;
    int j, best = -1;
    const unsigned char *base = (const unsigned char *)positions;

    memset(out, 0, sizeof(*out));
    if (count < 3 || mapSize == 0) return 0;
    if (!camera_back(projection, screenWidth, screenHeight, p, &b)) return 0;

    /* the ground */
    memset(bins, 0, sizeof(bins));
    for (i = 0; i + 2 < count; i += 3) {
        V3 a0 = back(&b, (const float *)(base + i * stride));
        V3 a1 = back(&b, (const float *)(base + (i + 1) * stride));
        V3 a2 = back(&b, (const float *)(base + (i + 2) * stride));
        V3 n = cross(sub(a1, a0), sub(a2, a0));
        float area = length(n);
        int key[3], h, s;
        if (!(area > 0.0f)) continue;
        n = scale(n, 1.0f / area);
        if (n.y < 0.0f || (n.y == 0.0f && n.z < 0.0f)) n = scale(n, -1.0f);
        if (fabsf(n.z) > 0.97f) continue;   /* (a card facing the camera) */
        total += area;
        key[0] = (int)floorf(n.x * 16.0f + 0.5f);
        key[1] = (int)floorf(n.y * 16.0f + 0.5f);
        key[2] = (int)floorf(n.z * 16.0f + 0.5f);
        h = ((key[0] * 73 + key[1] * 151 + key[2] * 37) % BINS + BINS) % BINS;
        for (s = 0; s < BINS; s++, h = (h + 1) % BINS) {
            Bin *bin = &bins[h];
            if (!bin->used) {
                bin->used = 1;
                memcpy(bin->key, key, sizeof(key));
            }
            if (!memcmp(bin->key, key, sizeof(key))) {
                bin->area += area;
                bin->sum = add(bin->sum, scale(n, area));
                break;
            }
        }
    }
    for (j = 0; j < BINS; j++) {
        if (bins[j].used && (best < 0 || bins[j].area > bins[best].area)) best = j;
    }
    /* Most of the scene, facing up towards the camera (not a wall) */
    if (best < 0 || bins[best].area < total * 0.3f) return 0;
    up = normalize(bins[best].sum);
    if (up.y < 0.2f) return 0;

    /* along the ground: the screen's right, and towards the camera */
    d = sun_light(up, sun, &e);
    b1 = normalize(cross(d, up));
    b2 = cross(d, b1);

    /* everything in the scene, seen from the sun */
    for (i = 0; i < count; i++) {
        V3 q = back(&b, (const float *)(base + i * stride));
        float c1 = dot(q, b1), c2 = dot(q, b2), cd = dot(q, d);
        if (c1 < lo1) lo1 = c1;
        if (c1 > hi1) hi1 = c1;
        if (c2 < lo2) lo2 = c2;
        if (c2 > hi2) hi2 = c2;
        if (cd < lod) lod = cd;
        if (cd > hid) hid = cd;
    }
    extent = hi1 - lo1 > hi2 - lo2 ? hi1 - lo1 : hi2 - lo2;
    if (!(extent > 0.0f)) return 0;
    /* The map's size in steps of 1/8 of a power of two, and its corner on
     * whole texels: as the camera moves, a texel stays where it was in the
     * world (the shadows' edges don't crawl). */
    {
        float step = exp2f(floorf(log2f(extent))) / 8.0f;
        extent = ceilf(extent * 1.02f / step) * step;
    }
    texel = extent / (float)mapSize;
    o1 = floorf((lo1 + hi1 - extent) * 0.5f / texel) * texel;
    o2 = floorf((lo2 + hi2 - extent) * 0.5f / texel) * texel;
    range = (hid - lod) * 1.02f + texel * 4.0f;
    lod -= texel * 2.0f;

    /* the rows */
    row(out->receiver[0], &b, b1, 1.0f / extent, -o1 / extent);
    row(out->receiver[1], &b, b2, 1.0f / extent, -o2 / extent);
    memset(out->receiver[2], 0, sizeof(out->receiver[2]));
    row(out->receiver[3], &b, d, 1.0f / range, -lod / range);
    for (j = 0; j < 4; j++) {
        out->caster[0][j] = out->receiver[0][j] * 2.0f;
        out->caster[1][j] = out->receiver[1][j] * 2.0f;
        out->caster[2][j] = -out->receiver[3][j];
        out->caster[3][j] = 0.0f;
    }
    out->caster[0][3] -= 1.0f;
    out->caster[1][3] -= 1.0f;
    out->caster[3][3] = 1.0f;
    /* The ground's distance from the sun changes by texel / tan(elevation)
     * over a texel: two texels of that, and a half of one. */
    out->bias = texel * (2.0f / tanf(e > 0.1f ? e : 0.1f) + 0.5f) / range;
    out->extent = extent;
    setup_camera(out, p, &b, up, d);
    return 1;
}

int NitroPort_ShadowGroundSetup(const NitroGx3dGpuVertex *verts, const NitroGx3dBatch *batches, size_t batchCount,
                                const int32_t projection[16], unsigned screenWidth, unsigned screenHeight,
                                const NitroShadowSun *sun, NitroShadowSetup *out)
{
    float p[16];
    Back b;
    V3 sum = v3(0.0f, 0.0f, 0.0f), up;
    size_t bi, i;

    memset(out, 0, sizeof(*out));
    if (!camera_back(projection, screenWidth, screenHeight, p, &b)) return 0;
    /* the ground: the round shadows lie on it (each triangle's normal,
     * as long as twice its area) */
    for (bi = 0; bi < batchCount; bi++) {
        const NitroGx3dBatch *batch = &batches[bi];
        if (!NITRO_GX3D_TAG_IS_ROUND(batch->tag)) continue;
        for (i = batch->first; i + 2 < (size_t)batch->first + batch->count; i += 3) {
            V3 a0 = back(&b, verts[i].position), a1 = back(&b, verts[i + 1].position);
            V3 a2 = back(&b, verts[i + 2].position), n = cross(sub(a1, a0), sub(a2, a0));
            if (n.y < 0.0f || (n.y == 0.0f && n.z < 0.0f)) n = scale(n, -1.0f);
            sum = add(sum, n);
        }
    }
    if (!(length(sum) > 1e-6f)) return 0;
    up = normalize(sum);
    if (!(up.y >= 0.2f)) return 0;
    setup_camera(out, p, &b, up, sun_light(up, sun, NULL));
    return 1;
}

void NitroPort_ShadowReach(const float *positions, size_t stride, size_t count, NitroShadowSetup *setup)
{
    const unsigned char *base = (const unsigned char *)positions;
    float least = 0.0f, k;
    size_t i;
    int j, c;
    for (i = 0; i + 2 < count; i += 3) {
        float lo[2] = { 1e30f, 1e30f }, hi[2] = { -1e30f, -1e30f }, d = 1e30f;
        for (c = 0; c < 3; c++) {
            const float *q = (const float *)(base + (i + (size_t)c) * stride);
            float in[4] = { q[0], q[1], q[3], 1.0f }, u[2], dist = 0.0f;
            u[0] = u[1] = 0.0f;
            for (j = 0; j < 4; j++) {
                u[0] += setup->receiver[0][j] * in[j];
                u[1] += setup->receiver[1][j] * in[j];
                dist += setup->receiver[3][j] * in[j];
            }
            for (j = 0; j < 2; j++) {
                if (u[j] < lo[j]) lo[j] = u[j];
                if (u[j] > hi[j]) hi[j] = u[j];
            }
            if (dist < d) d = dist;
        }
        if (hi[0] < 0.0f || lo[0] > 1.0f || hi[1] < 0.0f || lo[1] > 1.0f) continue;
        if (d < least) least = d;
    }
    if (!(least < 0.0f)) return;
    /* the distance d becomes (d - least) / (1 - least) */
    k = 1.0f / (1.0f - least);
    setup->receiver[3][3] -= least;
    for (j = 0; j < 4; j++) {
        setup->receiver[3][j] *= k;
        setup->caster[2][j] = -setup->receiver[3][j];
    }
    setup->bias *= k;
}

void NitroPort_ShadowRoles(const float *positions, uint8_t *alphas, size_t stride, size_t count,
                           const NitroShadowSetup *setup)
{
    const unsigned char *base = (const unsigned char *)positions;
    V3 up = v3(setup->up[0], setup->up[1], setup->up[2]);
    Back b;
    size_t i;
    memcpy(b.m, setup->back, sizeof(b.m));
    for (i = 0; i + 2 < count; i += 3) {
        V3 a0 = back(&b, (const float *)(base + i * stride));
        V3 a1 = back(&b, (const float *)(base + (i + 1) * stride));
        V3 a2 = back(&b, (const float *)(base + (i + 2) * stride));
        V3 n = normalize(cross(sub(a1, a0), sub(a2, a0)));
        float facing = fabsf(dot(n, up));
        uint8_t role = facing > 0.92f ? NITRO_SHADOW_RECEIVE_ONLY
                       : facing > 0.6f ? NITRO_SHADOW_CAST_RECEIVE : NITRO_SHADOW_CAST_ONLY;
        int k;
        for (k = 0; k < 3; k++) {
            uint8_t *a = alphas + (i + (size_t)k) * stride;
            if (*a == 255u) *a = role;
        }
    }
}

/* The clip space's (x, y, w) of a camera-space point: back's inverse. */
static int unback(const Back *b, V3 c, float out[3])
{
    /* c = L (x, y, w) + t: L the 3x3 of back's first three columns */
    float a[3][3], det, r[3];
    int i, j;
    for (i = 0; i < 3; i++)
        for (j = 0; j < 3; j++) a[i][j] = b->m[i][j];
    r[0] = c.x - b->m[0][3];
    r[1] = c.y - b->m[1][3];
    r[2] = c.z - b->m[2][3];
    det = a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) -
          a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
          a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
    if (fabsf(det) < 1e-12f) return 0;
    /* Cramer's rule */
    for (i = 0; i < 3; i++) {
        float m[3][3];
        memcpy(m, a, sizeof(m));
        for (j = 0; j < 3; j++) m[j][i] = r[j];
        out[i] = (m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) -
                  m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
                  m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0])) / det;
    }
    return 1;
}

/* A triangle faces the camera: a card (as the ground's search leaves out). */
static int card(V3 a0, V3 a1, V3 a2, V3 up, V3 *normal)
{
    V3 n = cross(sub(a1, a0), sub(a2, a0));
    float l = length(n);
    if (!(l > 0.0f)) return 0;
    n = scale(n, 1.0f / l);
    *normal = n;
    return fabsf(n.z) > 0.97f && fabsf(dot(n, up)) < 0.95f;
}

static int same_point(const float *a, const float *b)
{
    return a[0] == b[0] && a[1] == b[1] && a[3] == b[3];
}

size_t NitroPort_ShadowCards(NitroGx3dGpuVertex *verts, const NitroGx3dBatch *batches,
                             size_t batchCount, size_t first, NitroGx3dGpuVertex *out,
                             size_t capacity, NitroGx3dBatch *outBatches, size_t maxBatches,
                             size_t *outBatchCount, const NitroShadowSetup *setup)
{
    Back b;
    V3 up = v3(setup->up[0], setup->up[1], setup->up[2]);
    V3 light = v3(setup->light[0], setup->light[1], setup->light[2]);
    V3 lh = sub(light, scale(up, dot(light, up)));
    size_t written = 0, nb = 0, bi;
    if (outBatchCount) *outBatchCount = 0;
    if (!(length(lh) > 1e-4f)) return 0;   /* (the sun straight above: cards cast as they are) */
    lh = normalize(lh);
    memcpy(b.m, setup->back, sizeof(b.m));
    for (bi = 0; bi < batchCount; bi++) {
        const NitroGx3dBatch *batch = &batches[bi];
        size_t k = batch->first, end = batch->first + batch->count;
        if (batch->translucent) continue;
        while (k + 2 < end) {
            V3 n, c, across, along, turned;
            size_t g = k, gEnd, i;
            int count = 0;
            if (verts[k].color[3] == NITRO_SHADOW_RECEIVE_ONLY ||
                !card(back(&b, verts[k].position), back(&b, verts[k + 1].position),
                      back(&b, verts[k + 2].position), up, &n)) {
                k += 3;
                continue;
            }
            /* the card: this triangle, and the next if it shares an edge
             * with it (a quad's two halves) */
            gEnd = k + 3;
            while (gEnd + 2 < end && gEnd - g < 6) {   /* (a quad at most: trees side by side share corners) */
                V3 n2;
                int shared = 0;
                size_t p, q;
                if (!card(back(&b, verts[gEnd].position), back(&b, verts[gEnd + 1].position),
                          back(&b, verts[gEnd + 2].position), up, &n2))
                    break;
                for (p = gEnd; p < gEnd + 3; p++) {
                    int found = 0;
                    for (q = g; q < gEnd && !found; q++)
                        found = same_point(verts[p].position, verts[q].position);
                    shared += found;
                }
                if (shared < 2) break;   /* (a quad's other half shares an edge) */
                gEnd += 3;
            }
            k = gEnd;
            if (written + (gEnd - g) > capacity) break;
            /* the middle of its foot (its lowest corners), and the turn
             * about the upright through it: its facing (along the ground)
             * onto the sun's way, the nearer way; its foot stays where it
             * stands */
            {
                float hmin = 1e30f, hmax = -1e30f;
                for (i = g; i < gEnd; i++) {
                    float h = dot(back(&b, verts[i].position), up);
                    if (h < hmin) hmin = h;
                    if (h > hmax) hmax = h;
                }
                c = v3(0.0f, 0.0f, 0.0f);
                for (i = g; i < gEnd; i++) {
                    size_t j;
                    int dup = 0;
                    V3 q = back(&b, verts[i].position);
                    if (dot(q, up) > hmin + (hmax - hmin) * 0.1f) continue;
                    for (j = g; j < i && !dup; j++) dup = same_point(verts[j].position, verts[i].position);
                    if (dup) continue;
                    c = add(c, q);
                    count++;
                }
                if (!count) continue;
                c = scale(c, 1.0f / (float)count);
            }
            /* its axes: across (along the ground), and up its face
             * (leaning back, towards the camera's view); the copy stands
             * upright, as tall as the card is long, across the sun's way */
            across = cross(n, up);
            if (!(length(across) > 1e-4f)) continue;
            across = normalize(across);
            along = normalize(cross(across, n));
            if (dot(along, up) < 0.0f) along = scale(along, -1.0f);
            turned = normalize(cross(up, lh));
            if (dot(turned, across) < 0.0f) turned = scale(turned, -1.0f);
            if (nb == 0 || outBatches[nb - 1].texture_param != batch->texture_param ||
                outBatches[nb - 1].palette_base != batch->palette_base ||
                outBatches[nb - 1].polygon_attr != batch->polygon_attr ||
                outBatches[nb - 1].first + outBatches[nb - 1].count != first + written) {
                if (nb >= maxBatches) break;
                outBatches[nb] = *batch;
                outBatches[nb].first = (uint32_t)(first + written);
                outBatches[nb].count = 0;
                outBatches[nb].translucent = 0;
                nb++;
            }
            for (i = g; i < gEnd; i++) {
                NitroGx3dGpuVertex *dst = &out[written++];
                V3 r = sub(back(&b, verts[i].position), c), rr;
                float xyw[3];
                rr = add(scale(turned, dot(r, across)), scale(up, dot(r, along)));
                *dst = verts[i];
                if (unback(&b, add(c, rr), xyw)) {
                    dst->position[0] = xyw[0];
                    dst->position[1] = xyw[1];
                    dst->position[3] = xyw[2];
                }
                dst->color[3] = NITRO_SHADOW_CAST_ONLY;
                verts[i].color[3] = NITRO_SHADOW_NEITHER;
            }
            outBatches[nb - 1].count += (uint32_t)(gEnd - g);
        }
    }
    if (outBatchCount) *outBatchCount = nb;
    return written;
}

void NitroPort_ShadowDepthRow(const NitroShadowSetup *setup, int wbuffer, float depthA, float depthK,
                              float out[4])
{
    int j;
    if (wbuffer) {
        /* d = w 4096 / 16777215; z' = -(d - lo) k */
        out[0] = out[1] = 0.0f;
        out[2] = -depthK * 4096.0f / 16777215.0f;
        out[3] = depthA * depthK;
        return;
    }
    /* d = (z / w + 1) / 2; z' = -(near + (d - lo) k) w = -k d w + (lo k - near) w,
     * and depthA depthK = lo k - near: z' = -k/2 z + (depthA depthK - k/2) w */
    for (j = 0; j < 4; j++) out[j] = -0.5f * depthK * setup->zclip[j];
    out[2] += depthA * depthK - 0.5f * depthK;
}

/* Where up the card (0 its foot .. 1 its top) the picture's feet are:
 * footRow's first opaque row from the foot's side, over the card's
 * texels. 0 if not known. */
static float card_feet(const NitroGx3dGpuVertex *verts, size_t g, size_t gEnd, const float *alongOf,
                       float aMin, float aMax, uint32_t param, uint32_t palette, NitroShadowFootRow footRow,
                       void *ctx)
{
    float tw = (float)(8u << ((param >> 20) & 7u)), th = (float)(8u << ((param >> 23) & 7u));
    float sLo = 1e30f, sHi = -1e30f, tFoot = 0.0f, tTop = 0.0f, f;
    int nFoot = 0, nTop = 0, row, t0, t1;
    size_t i;
    if (!footRow || !(aMax - aMin > 0.0f)) return 0.0f;
    for (i = g; i < gEnd; i++) {
        float s = verts[i].uv[0] * tw, t = verts[i].uv[1] * th, a = (alongOf[i - g] - aMin) / (aMax - aMin);
        if (s < sLo) sLo = s;
        if (s > sHi) sHi = s;
        if (a < 0.1f) { tFoot += t; nFoot++; }
        if (a > 0.9f) { tTop += t; nTop++; }
    }
    if (!nFoot || !nTop) return 0.0f;
    tFoot /= (float)nFoot;
    tTop /= (float)nTop;
    if (!(fabsf(tTop - tFoot) >= 1.0f) || !(sHi - sLo >= 1.0f)) return 0.0f;
    /* the rows' texels: from the foot's edge (the row just inside it) to
     * the top's */
    if (tTop > tFoot) {
        t0 = (int)floorf(tFoot + 0.5f);
        t1 = (int)floorf(tTop - 0.5f);
    } else {
        t0 = (int)floorf(tFoot - 0.5f);
        t1 = (int)floorf(tTop + 0.5f);
    }
    row = footRow(ctx, param, palette, (int)floorf(sLo + 0.5f), (int)floorf(sHi + 0.5f), t0, t1);
    if (row < 0) return 0.0f;
    /* the row's edge on the foot's side */
    f = tTop > tFoot ? ((float)row - tFoot) / (tTop - tFoot) : (tFoot - (float)(row + 1)) / (tFoot - tTop);
    return f < 0.0f ? 0.0f : f > 0.9f ? 0.9f : f;
}

static unsigned sLastSprites, sLastShadows, sLastLaid, sLastProps, sLastPropsLaid;

void NitroPort_ShadowGroundStats(unsigned *sprites, unsigned *shadows, unsigned *laid)
{
    if (sprites) *sprites = sLastSprites;
    if (shadows) *shadows = sLastShadows;
    if (laid) *laid = sLastLaid;
}

void NitroPort_ShadowPropStats(unsigned *props, unsigned *laid)
{
    if (props) *props = sLastProps;
    if (laid) *laid = sLastPropsLaid;
}

/* The clip space's (x, y, w) of camera-space points: back's inverse, made
 * once for all of a scene's (unback works it out again each time). */
typedef struct { float m[3][3], t[3]; } Unback;
static int unback_make(const Back *b, Unback *u)
{
    const float (*a)[4] = b->m;
    float det = a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) -
                a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
                a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
    float inv;
    if (!(fabsf(det) >= 1e-12f)) return 0;
    inv = 1.0f / det;
    u->m[0][0] = (a[1][1] * a[2][2] - a[1][2] * a[2][1]) * inv;
    u->m[0][1] = (a[0][2] * a[2][1] - a[0][1] * a[2][2]) * inv;
    u->m[0][2] = (a[0][1] * a[1][2] - a[0][2] * a[1][1]) * inv;
    u->m[1][0] = (a[1][2] * a[2][0] - a[1][0] * a[2][2]) * inv;
    u->m[1][1] = (a[0][0] * a[2][2] - a[0][2] * a[2][0]) * inv;
    u->m[1][2] = (a[0][2] * a[1][0] - a[0][0] * a[1][2]) * inv;
    u->m[2][0] = (a[1][0] * a[2][1] - a[1][1] * a[2][0]) * inv;
    u->m[2][1] = (a[0][1] * a[2][0] - a[0][0] * a[2][1]) * inv;
    u->m[2][2] = (a[0][0] * a[1][1] - a[0][1] * a[1][0]) * inv;
    u->t[0] = a[0][3];
    u->t[1] = a[1][3];
    u->t[2] = a[2][3];
    return 1;
}
static void unback_point(const Unback *u, V3 c, float out[3])
{
    float r0 = c.x - u->t[0], r1 = c.y - u->t[1], r2 = c.z - u->t[2];
    out[0] = u->m[0][0] * r0 + u->m[0][1] * r1 + u->m[0][2] * r2;
    out[1] = u->m[1][0] * r0 + u->m[1][1] * r1 + u->m[1][2] * r2;
    out[2] = u->m[2][0] * r0 + u->m[2][1] * r1 + u->m[2][2] * r2;
}

/* A prop for its shadow: its batches (a run of one mark), its vertices,
 * where it stands and how tall its picture is. */
#define GROUND_PROPS 256
#define PROP_VERTS 1536    /* (512 triangles: more is no small prop) */
typedef struct {
    size_t b0, b1;         /* its batches: [b0, b1) */
    V3 base;
    float top;
} Prop;
static Prop sProps[GROUND_PROPS];

#define GROUND_VERTS 24    /* (a sprite's: two triangles, more when the view's edge cuts them) */
#define GROUND_ROUNDS 64   /* (round shadows looked at a scene) */

/* (a vertex of a shadow: v's own, where xyw puts it, the colour given) */
static void lay_vertex(NitroGx3dGpuVertex *dst, const NitroGx3dGpuVertex *v, const float xyw[3], const float depth[4],
                       const uint8_t col[3])
{
    *dst = *v;
    dst->position[0] = xyw[0];
    dst->position[1] = xyw[1];
    dst->position[3] = xyw[2];
    dst->position[2] = depth[0] * xyw[0] + depth[1] * xyw[1] + depth[2] * xyw[2] + depth[3];
    dst->color[0] = col[0];
    dst->color[1] = col[1];
    dst->color[2] = col[2];
    dst->color[3] = 255;
}
/* (the shade at j of the way up a picture, 0-1: DARK times as dark, fading
 * to TIP of it at the top) */
static void shade_at(const float shade[3], float j, uint8_t col[3])
{
    float f;
    int c3;
    j = j < 0.0f ? 0.0f : j > 1.0f ? 1.0f : j;
    f = 1.0f - (1.0f - NITRO_SHADOW_GROUND_TIP) * j;
    for (c3 = 0; c3 < 3; c3++) {
        float full = 1.0f - (1.0f - shade[c3]) * NITRO_SHADOW_GROUND_DARK * f;
        if (!(full > 0.0f)) full = 0.0f;
        col[c3] = (uint8_t)(full * 255.0f + 0.5f);
    }
}

size_t NitroPort_ShadowGroundCards(const NitroGx3dGpuVertex *verts, const NitroGx3dBatch *batches,
                                   size_t batchCount, size_t first, NitroGx3dGpuVertex *out,
                                   size_t capacity, NitroGx3dBatch *outBatches, size_t maxBatches,
                                   size_t *outBatchCount, const NitroShadowSetup *setup, const float depth[4],
                                   uint8_t *roundUsed, const float shade[3], NitroShadowFootRow footRow,
                                   void *footCtx)
{
    Back b;
    Unback ub;
    V3 up = v3(setup->up[0], setup->up[1], setup->up[2]), right, toward, way;
    V3 light = v3(setup->light[0], setup->light[1], setup->light[2]), lh;
    float s, stretch, down, cr, ct;
    const float side = sinf(NITRO_SHADOW_GROUND_SIDE * 3.14159265f / 180.0f);
    size_t written = 0, nb = 0, bi, rounds[GROUND_ROUNDS], nr = 0, ri, props = 0;
    if (outBatchCount) *outBatchCount = 0;
    sLastSprites = sLastShadows = sLastLaid = sLastProps = sLastPropsLaid = 0;
    /* the marked batches: the round shadows (each mark once), the props */
    for (bi = 0; bi < batchCount; bi++) {
        uint32_t tag = batches[bi].tag;
        if (!tag || !batches[bi].count) continue;
        if (NITRO_GX3D_TAG_IS_PROP(tag)) {
            props++;
            continue;
        }
        if (NITRO_GX3D_TAG_IS_DRAW(tag)) continue;
        if (!(tag & 1u)) {
            sLastSprites++;
            continue;
        }
        for (ri = 0; ri < nr && batches[rounds[ri]].tag != tag; ri++) {
        }
        if (ri == nr && nr < GROUND_ROUNDS) rounds[nr++] = bi;
    }
    sLastShadows = (unsigned)nr;
    if (!nr && !props) return 0;
    memcpy(b.m, setup->back, sizeof(b.m));
    if (!unback_make(&b, &ub)) return 0;
    /* The ground's two ways: the screen's right (the camera's x on the
     * ground) and down the screen (towards the camera), foreshortened
     * there by s. */
    right = sub(v3(1.0f, 0.0f, 0.0f), scale(up, up.x));
    if (!(length(right) > 1e-3f)) return 0;
    right = normalize(right);
    toward = normalize(cross(right, up));
    s = -toward.y;
    if (s < 0.35f) s = 0.35f;
    if (s > 1.0f) s = 1.0f;
    /* The way the shadows lie: the sun's light along the ground (the
     * setup's: by the clock), but never nearer the sprite's own line
     * (across: the screen's right) than NITRO_SHADOW_GROUND_SIDE, and
     * never up the screen: beside it a sprite's shadow is a sliver, and
     * behind it it's hidden. Its length: the sprite's height times the
     * sun's cotangent (and NITRO_SHADOW_GROUND_LENGTH). */
    down = -dot(light, up);
    lh = sub(light, scale(up, dot(light, up)));
    if (!(down > 0.05f) || !(length(lh) > 1e-4f)) return 0;
    stretch = NITRO_SHADOW_GROUND_LENGTH * length(lh) / down;
    lh = normalize(lh);
    cr = dot(lh, right);
    ct = dot(lh, toward);
    if (ct < side) {
        ct = side;
        cr = (cr < 0.0f ? -1.0f : 1.0f) * sqrtf(1.0f - side * side);
    }
    way = normalize(add(scale(right, cr), scale(toward, ct)));

    for (ri = 0; ri < nr; ri++) {
        uint32_t tag = batches[rounds[ri]].tag;
        const NitroGx3dBatch *batch = NULL;
        V3 anchor = v3(0.0f, 0.0f, 0.0f), q[GROUND_VERTS], n = v3(0.0f, 0.0f, 0.0f), across, along;
        float alongOf[GROUND_VERTS], aMin = 1e30f, aMax = -1e30f, uMin = 1e30f, uMax = -1e30f, uMid;
        float feet, top;
        size_t g, gEnd, i, count = 0;
        /* its sprite: the batch with the mark less its bit 0, opaque */
        for (bi = 0; bi < batchCount; bi++) {
            if (batches[bi].tag == (tag & ~1u) && batches[bi].count >= 3 && batches[bi].count <= GROUND_VERTS &&
                !batches[bi].translucent && batches[bi].texture_param) {
                batch = &batches[bi];
                break;
            }
        }
        if (!batch) continue;
        /* where it starts: the round shadow's middle (all its vertices':
         * a quad's two triangles have its corners evenly) */
        for (bi = rounds[ri]; bi < batchCount; bi++) {
            if (batches[bi].tag != tag) continue;
            for (i = batches[bi].first; i < batches[bi].first + batches[bi].count; i++) {
                anchor = add(anchor, back(&b, verts[i].position));
                count++;
            }
        }
        anchor = scale(anchor, 1.0f / (float)count);
        /* the sprite's frame: across it (on the ground: it stands facing
         * the camera; to the screen's right) and up it */
        g = batch->first;
        gEnd = g + batch->count;
        for (i = g; i < gEnd; i++) q[i - g] = back(&b, verts[i].position);
        for (i = 0; i + 2 < gEnd - g && !(length(n) > 1e-6f); i += 3) n = cross(sub(q[i + 1], q[i]), sub(q[i + 2], q[i]));
        if (!(length(n) > 1e-6f)) continue;
        n = normalize(n);
        across = cross(n, up);
        if (!(length(across) > 1e-3f)) continue;   /* (lying flat: no sprite) */
        across = normalize(across);
        if (across.x < 0.0f) across = scale(across, -1.0f);
        along = normalize(cross(across, n));
        if (dot(along, up) < 0.0f) along = scale(along, -1.0f);
        for (i = 0; i < gEnd - g; i++) {
            float u = dot(q[i], across), a = dot(q[i], along);
            alongOf[i] = a;
            if (a < aMin) aMin = a;
            if (a > aMax) aMax = a;
            if (u < uMin) uMin = u;
            if (u > uMax) uMax = u;
        }
        /* (a sprite: not a sliver, not a whole backdrop) */
        if (!(aMax - aMin > 0.5f) || aMax - aMin > NITRO_SHADOW_GROUND_CARD_MAX ||
            uMax - uMin > NITRO_SHADOW_GROUND_CARD_MAX)
            continue;
        uMid = 0.5f * (uMin + uMax);
        if (written + (gEnd - g) > capacity) break;
        feet = card_feet(verts, g, gEnd, alongOf, aMin, aMax, batch->texture_param, batch->palette_base, footRow,
                         footCtx) * (aMax - aMin);
        top = aMax - aMin - feet;
        if (nb == 0 || outBatches[nb - 1].texture_param != batch->texture_param ||
            outBatches[nb - 1].palette_base != batch->palette_base ||
            outBatches[nb - 1].polygon_attr != batch->polygon_attr ||
            outBatches[nb - 1].first + outBatches[nb - 1].count != first + written) {
            if (nb >= maxBatches) break;
            outBatches[nb] = *batch;
            outBatches[nb].first = (uint32_t)(first + written);
            outBatches[nb].count = 0;
            outBatches[nb].translucent = 0;
            outBatches[nb].tag = 0;
            nb++;
        }
        for (i = 0; i < gEnd - g; i++) {
            V3 spot;
            float h = alongOf[i] - aMin - feet, xyw[3];
            uint8_t col[3];
            /* across stays across, about the sprite's middle; up the
             * sprite from its feet becomes along the shadows' way (the
             * clear rows below the feet behind the start) */
            spot = add(add(anchor, scale(across, dot(q[i], across) - uMid)), scale(way, h * stretch));
            unback_point(&ub, scale(spot, 1.0f - NITRO_SHADOW_GROUND_PULL), xyw);
            /* the colour: the shade, fading to the head */
            shade_at(shade, top > 0.0f ? h / top : 0.0f, col);
            lay_vertex(&out[written++], &verts[g + i], xyw, depth, col);
        }
        outBatches[nb - 1].count += (uint32_t)(gEnd - g);
        /* its round shadow: left out (the caller's) */
        if (roundUsed) {
            for (bi = rounds[ri]; bi < batchCount; bi++)
                if (batches[bi].tag == tag) roundUsed[bi] = 1;
        }
        sLastLaid++;
    }

    /* The props (signs, lamp posts, boards: NITRO_GX3D_TAG_PROP marks, each
     * run of one mark a prop): the prop's picture as the camera has it,
     * laid on the ground from where it stands. Where: the middle of its
     * lowest vertices (the ground it's on). Each vertex: across by its
     * place across the screen from there, along the shadows' way by its
     * height up the screen from there (as a sprite's rows are). */
    if (props) {
        const V3 pictureUp = v3(0.0f, 1.0f, 0.0f);
        size_t np = 0, pi, nb0 = nb;

        /* the props */
        for (bi = 0; bi < batchCount && np < GROUND_PROPS;) {
            uint32_t tag = batches[bi].tag;
            size_t bj = bi + 1, i, g, gEnd, low = 0, drawn = 0;
            float hMin = 1e30f, hMax = -1e30f, top = -1e30f;
            V3 base;
            if (!NITRO_GX3D_TAG_IS_PROP(tag)) {
                bi++;
                continue;
            }
            while (bj < batchCount && batches[bj].tag == tag &&
                   batches[bj].first == batches[bj - 1].first + batches[bj - 1].count)
                bj++;
            g = batches[bi].first;
            gEnd = batches[bj - 1].first + batches[bj - 1].count;
            sLastProps++;
            for (i = bi; i < bj; i++)
                if (!batches[i].translucent && batches[i].texture_param) drawn += batches[i].count;
            if (gEnd - g >= 3 && gEnd - g <= PROP_VERTS && drawn) {
                for (i = g; i < gEnd; i++) {
                    float h = dot(back(&b, verts[i].position), up);
                    if (h < hMin) hMin = h;
                    if (h > hMax) hMax = h;
                }
                {
                    /* (the middle of their span across and in depth: a
                     * corner two triangles share isn't counted twice) */
                    float rLo = 1e30f, rHi = -1e30f, tLo = 1e30f, tHi = -1e30f, hSum = 0.0f;
                    for (i = g; i < gEnd; i++) {
                        V3 q = back(&b, verts[i].position);
                        float h = dot(q, up), cr2 = dot(q, right), ct2 = dot(q, toward);
                        if (h > hMin + 1.5f) continue;
                        if (cr2 < rLo) rLo = cr2;
                        if (cr2 > rHi) rHi = cr2;
                        if (ct2 < tLo) tLo = ct2;
                        if (ct2 > tHi) tHi = ct2;
                        hSum += h;
                        low++;
                    }
                    base = add(add(scale(right, 0.5f * (rLo + rHi)), scale(toward, 0.5f * (tLo + tHi))),
                               scale(up, hSum / (float)low));
                }
                for (i = g; i < gEnd; i++) {
                    float h = dot(sub(back(&b, verts[i].position), base), pictureUp);
                    if (h > top) top = h;
                }
                /* (standing up, off the ground it's on: a mat or a flower
                 * bed lying flat casts none) */
                if (hMax - hMin >= NITRO_SHADOW_PROP_MIN && top > 1.0f && top <= NITRO_SHADOW_GROUND_CARD_MAX) {
                    sProps[np].b0 = bi;
                    sProps[np].b1 = bj;
                    sProps[np].base = base;
                    sProps[np].top = top;
                    np++;
                }
            }
            bi = bj;
        }

        /* their shadows, in batches of their own (NITRO_SHADOW_ONCE: the
         * renderer darkens each pixel once under them, the props' own
         * layers and each other's overlapping); one that doesn't fit
         * whole isn't laid at all */
        for (pi = 0; pi < np; pi++) {
            const Prop *pr = &sProps[pi];
            size_t at = written, nbAt = nb;
            int fits = 1;
            for (bi = pr->b0; bi < pr->b1 && fits; bi++) {
                const NitroGx3dBatch *batch = &batches[bi];
                size_t i = batch->first, end = (size_t)batch->first + batch->count;
                if (batch->translucent || !batch->texture_param) continue;
                for (; i + 2 < end && fits; i += 3) {
                    V3 q[3];
                    int k;
                    for (k = 0; k < 3; k++) q[k] = back(&b, verts[i + (size_t)k].position);
                    if (written + 3 > capacity) {
                        fits = 0;
                        break;
                    }
                    if (nb == nb0 || outBatches[nb - 1].texture_param != batch->texture_param ||
                        outBatches[nb - 1].palette_base != batch->palette_base ||
                        outBatches[nb - 1].first + outBatches[nb - 1].count != first + written) {
                        if (nb >= maxBatches) {
                            fits = 0;
                            break;
                        }
                        outBatches[nb] = *batch;
                        outBatches[nb].first = (uint32_t)(first + written);
                        outBatches[nb].count = 0;
                        outBatches[nb].translucent = 0;
                        outBatches[nb].tag = NITRO_SHADOW_ONCE;
                        nb++;
                    }
                    for (k = 0; k < 3; k++) {
                        V3 rel = sub(q[k], pr->base), spot;
                        float h = dot(rel, pictureUp), xyw[3];
                        uint8_t col[3];
                        spot = add(add(pr->base, scale(right, dot(rel, right))), scale(way, h * stretch));
                        unback_point(&ub, scale(spot, 1.0f - NITRO_SHADOW_GROUND_PULL), xyw);
                        shade_at(shade, h / pr->top, col);
                        lay_vertex(&out[written++], &verts[i + (size_t)k], xyw, depth, col);
                    }
                    outBatches[nb - 1].count += 3;
                }
            }
            if (!fits) {
                /* (what was laid of it taken back: the batches begun for
                 * it, and its vertices off the one before them) */
                if (nbAt > nb0 && nbAt <= nb && outBatches[nbAt - 1].first + outBatches[nbAt - 1].count > first + at)
                    outBatches[nbAt - 1].count = (uint32_t)(first + at - outBatches[nbAt - 1].first);
                nb = nbAt;
                written = at;
                break;
            }
            sLastPropsLaid++;
        }
    }
    if (outBatchCount) *outBatchCount = nb;
    return written;
}

int NitroPort_ShadowDecalShape(const float *tri, size_t stride, unsigned polygonAlpha, int translucentFormat,
                               const NitroShadowSetup *setup)
{
    const unsigned char *base = (const unsigned char *)tri;
    Back b;
    V3 a0, a1, a2, n, up = v3(setup->up[0], setup->up[1], setup->up[2]);
    float l;
    /* see-through: the polygon's alpha, or a format with alpha of its own */
    if (!(polygonAlpha > 0 && polygonAlpha < 31) && !translucentFormat) return 0;
    memcpy(b.m, setup->back, sizeof(b.m));
    a0 = back(&b, (const float *)base);
    a1 = back(&b, (const float *)(base + stride));
    a2 = back(&b, (const float *)(base + 2 * stride));
    n = cross(sub(a1, a0), sub(a2, a0));
    l = length(n);
    return l > 0.0f && fabsf(dot(n, up)) / l >= 0.9f;   /* lying flat */
}

int NitroPort_ShadowDecalDark(const uint32_t *texels, size_t count, uint16_t color)
{
    float luma = 0.0f, weight = 0.0f;
    size_t i;
    if (texels) {
        for (i = 0; i < count; i++) {
            uint32_t p = texels[i];
            float a = (float)(p & 255u) / 255.0f;
            float y = 0.30f * (float)(p >> 24) + 0.59f * (float)((p >> 16) & 255u) +
                      0.11f * (float)((p >> 8) & 255u);
            luma += y * a;
            weight += a;
        }
        if (!(weight > 0.0f)) return 0;
        luma /= weight;
    } else {
        luma = 0.30f * (float)((color & 31u) * 255u / 31u) + 0.59f * (float)(((color >> 5) & 31u) * 255u / 31u) +
               0.11f * (float)(((color >> 10) & 31u) * 255u / 31u);
    }
    return luma < 72.0f;   /* dark: a shadow, not glass or water */
}

int NitroPort_ShadowDecal(const float *tri, size_t stride, unsigned polygonAlpha, int translucentFormat,
                          const uint32_t *texels, size_t count, uint16_t color,
                          const NitroShadowSetup *setup)
{
    return NitroPort_ShadowDecalShape(tri, stride, polygonAlpha, translucentFormat, setup) &&
           NitroPort_ShadowDecalDark(texels, count, color);
}

/* ------------------------------------------------- the look over the day */
int NitroPort_LookPeriod(int hour)
{
    if (hour >= 4 && hour < 10) return NITRO_LOOK_MORNING;
    if (hour >= 10 && hour < 17) return NITRO_LOOK_DAY;
    if (hour >= 17 && hour < 20) return NITRO_LOOK_EVENING;
    return NITRO_LOOK_NIGHT;
}

/* 0 at a, 1 at b (either way round), smooth between */
static float ramp(float t, float a, float b)
{
    float x = (t - a) / (b - a);
    if (x <= 0.0f) return 0.0f;
    if (x >= 1.0f) return 1.0f;
    return x * x * (3.0f - 2.0f * x);
}

/* The sun's way over the day: (hour, elevation, azimuth). It stays at the
 * sides and the top left: shadows to the left in the morning, to the
 * bottom right at midday, to the right in the evening, where they're seen
 * beside what casts them. (From the top, the light comes towards the
 * camera: the shadows fall under the characters and trees, which are cards
 * facing the camera, and the cards shade each other in stripes; from the
 * bottom, the shadows hide behind what casts them.) It crosses the top
 * between 9:30 and 12:30, high, its shadows short. Never lower than 40
 * degrees: the shadows at most about 1.2 times as long as what casts them
 * (a real low sun's ran across the whole screen; at 30 degrees, with the
 * characters' and trees' cards standing upright for their shadows,
 * NitroPort_ShadowCards, a row of trees darkened a whole route). */
static const float sSunWay[][3] = {
    { 5.0f, 40.0f, 170.0f },
    { 8.0f, 46.0f, 158.0f },
    { 9.5f, 52.0f, 150.0f },
    { 12.5f, 65.0f, 45.0f },
    { 16.0f, 55.0f, 30.0f },
    { 19.0f, 46.0f, 14.0f },
    { 20.0f, 40.0f, 8.0f },
};

void NitroPort_ShadowSunAt(int hour, int minute, NitroShadowSun *sun, float *strength)
{
    float t = (float)hour + (float)minute / 60.0f;
    if (t >= 5.0f && t <= 20.0f) {
        unsigned i = 0, n = sizeof(sSunWay) / sizeof(sSunWay[0]);
        float k;
        while (i + 2 < n && t > sSunWay[i + 1][0]) i++;
        k = (t - sSunWay[i][0]) / (sSunWay[i + 1][0] - sSunWay[i][0]);
        if (k < 0.0f) k = 0.0f;
        if (k > 1.0f) k = 1.0f;
        sun->elevation = sSunWay[i][1] + (sSunWay[i + 1][1] - sSunWay[i][1]) * k;
        sun->azimuth = sSunWay[i][2] + (sSunWay[i + 1][2] - sSunWay[i][2]) * k;
        *strength = ramp(t, 5.0f, 6.0f) * ramp(t, 20.0f, 19.5f);
    } else {
        float n = t < 12.0f ? t + 24.0f : t;   /* (20:00 .. 29:00, past midnight) */
        sun->elevation = 60.0f;
        sun->azimuth = 60.0f;
        *strength = 0.35f * ramp(n, 20.0f, 20.75f) * ramp(n, 29.0f, 28.25f);
    }
}

/* The sun as the renderer has it now, for the game's side (the land's
 * trees' shadows are made there, as polygons): the way its light goes
 * along the ground and the shadows' length for a height, as
 * NitroPort_ShadowGroundCards has them, in 1/256ths (a step is a quarter
 * of a degree or less: the shadows made again a few times a minute at
 * most). Set by the renderer's thread, read by the game's: each a word. */
static volatile int sGroundShadows;
static volatile int32_t sSunWayNow[3];
static volatile int sSunWaySet;

void NitroPort_SetSunNow(const NitroShadowSun *sun, float strength)
{
    const float side = sinf(NITRO_SHADOW_GROUND_SIDE * 3.14159265f / 180.0f);
    float a, e, cr, ct, stretch;
    if (!sun || !(strength >= 0.02f)) {
        sSunWaySet = 0;
        return;
    }
    a = sun->azimuth * 3.14159265f / 180.0f;
    e = sun->elevation * 3.14159265f / 180.0f;
    cr = cosf(a);
    ct = sinf(a);
    if (ct < side) {
        ct = side;
        cr = (cr < 0.0f ? -1.0f : 1.0f) * sqrtf(1.0f - side * side);
    }
    if (!(sinf(e) > 0.05f)) {
        sSunWaySet = 0;
        return;
    }
    stretch = NITRO_SHADOW_GROUND_LENGTH * cosf(e) / sinf(e);
    sSunWayNow[0] = (int32_t)floorf(cr * 256.0f + 0.5f) * 16;
    sSunWayNow[1] = (int32_t)floorf(ct * 256.0f + 0.5f) * 16;
    sSunWayNow[2] = (int32_t)floorf(stretch * 256.0f + 0.5f) * 16;
    sSunWaySet = 1;
}

int NitroPort_SunWayNow(int32_t way[3])
{
    if (!sSunWaySet || !sGroundShadows) return 0;
    way[0] = sSunWayNow[0];
    way[1] = sSunWayNow[1];
    way[2] = sSunWayNow[2];
    return 1;
}

void NitroPort_LookGradeTable(int period, uint32_t table[32768])
{
    /* white balance, saturation, gamma */
    static const float grades[NITRO_LOOK_PERIODS][5] = {
        { 1.00f, 0.95f, 0.86f, 1.08f, 1.08f },   /* morning: golden */
        { 1.00f, 0.92f, 0.80f, 1.10f, 1.15f },   /* day: warm */
        { 1.00f, 0.90f, 0.76f, 1.08f, 1.10f },   /* evening: orange (the game's own light is already) */
        { 0.88f, 0.94f, 1.00f, 0.92f, 1.08f },   /* night: cool */
    };
    const float *g = grades[period >= 0 && period < NITRO_LOOK_PERIODS ? period : NITRO_LOOK_DAY];
    uint8_t curve[256];
    unsigned c;
    for (c = 0; c < 256; c++) {
        float v = powf((float)c / 255.0f, g[4]) * 255.0f + 0.5f;
        curve[c] = (uint8_t)(v > 255.0f ? 255.0f : v);
    }
    for (c = 0; c < 32768u; c++) {
        float rgb[3], l;
        unsigned k, out[3];
        for (k = 0; k < 3; k++) {
            unsigned v = (c >> (5 * k)) & 31u;
            rgb[k] = (float)((v << 3) | (v >> 2)) / 255.0f * g[k];
        }
        l = rgb[0] * 0.299f + rgb[1] * 0.587f + rgb[2] * 0.114f;
        for (k = 0; k < 3; k++) {
            float v = l + (rgb[k] - l) * g[3];
            v = v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
            out[k] = curve[(unsigned)(v * 255.0f + 0.5f)];
        }
        table[c] = (out[0] << 24) | (out[1] << 16) | (out[2] << 8);
    }
}

static volatile int sSunShadows;

void NitroPort_SetSunShadows(int on)
{
    sSunShadows = on;
}

int NitroPort_SunShadows(void)
{
    return sSunShadows;
}


void NitroPort_SetGroundShadows(int on)
{
    sGroundShadows = on;
}

int NitroPort_GroundShadowsOn(void)
{
    return sGroundShadows;
}

static volatile int sShadowReach;

void NitroPort_SetShadowReach(int on)
{
    sShadowReach = on;
}

int NitroPort_ShadowReachOn(void)
{
    return sShadowReach;
}

/* (see the header) */
static int mask_index(int i,int n,int wrap)
{
    if(i>=0 && i<n)return i;
    if(wrap==NITRO_SHADOW_WRAP_REPEAT){i%=n;return i<0?i+n:i;}
    if(wrap==NITRO_SHADOW_WRAP_MIRROR){
        const int p=2*n;
        i%=p;if(i<0)i+=p;
        return i<n?i:p-1-i;
    }
    return i<0?0:n-1;
}
int NitroPort_ShadowTreeMask(const uint32_t *rgba8,unsigned width,unsigned height,int wrapS,int wrapT,
                             uint32_t *out)
{
    enum { R=NITRO_SHADOW_TREE_REACH };
    float kernel[2*R+1],sum=0.0f,*a,*b;
    const size_t n=(size_t)width*height;
    unsigned x,y;
    int k;
    if(!rgba8 || !out || !width || !height)return 0;
    a=malloc(n*sizeof(*a));
    b=malloc(n*sizeof(*b));
    if(!a || !b){free(a);free(b);return 0;}
    for(k=-R;k<=R;k++){
        kernel[k+R]=expf(-(float)(k*k)/(2.0f*NITRO_SHADOW_TREE_SOFT*NITRO_SHADOW_TREE_SOFT));
        sum+=kernel[k+R];
    }
    for(k=0;k<2*R+1;k++)kernel[k]/=sum;
    for(y=0;y<n/width;y++)for(x=0;x<width;x++)a[y*width+x]=(float)(rgba8[y*width+x]&0xFFu)*(1.0f/255.0f);
    /* across, then down */
    for(y=0;y<height;y++)for(x=0;x<width;x++){
        float v=0.0f;
        for(k=-R;k<=R;k++)v+=kernel[k+R]*a[y*width+(unsigned)mask_index((int)x+k,(int)width,wrapS)];
        b[y*width+x]=v;
    }
    for(y=0;y<height;y++)for(x=0;x<width;x++){
        float v=0.0f,t;
        unsigned m;
        for(k=-R;k<=R;k++)v+=kernel[k+R]*b[(unsigned)mask_index((int)y+k,(int)height,wrapT)*width+x];
        t=(v-NITRO_SHADOW_TREE_FROM)/(NITRO_SHADOW_TREE_TO-NITRO_SHADOW_TREE_FROM);
        t=t<0.0f?0.0f:t>1.0f?1.0f:t;
        m=(unsigned)(t*t*(3.0f-2.0f*t)*255.0f+0.5f);
        out[y*width+x]=0xFFFFFF00u|m;
    }
    free(a);free(b);
    return 1;
}
/* (see the header) */
#define DARKEN_VERTS 1536u
unsigned NitroPort_ShadowDarkenSprites(NitroGx3dGpuVertex *verts,const NitroGx3dBatch *batches,size_t count,
                                       const float take[3],NitroShadowMaskAt maskAt,void *maskCtx)
{
    static float ndc[DARKEN_VERTS][7];   /* (x, y on the screen, the colour: red the grey, or a trunk's strip's three; the texture coordinate) */
    static uint8_t oval[DARKEN_VERTS/3u];
    static const NitroGx3dBatch *of[DARKEN_VERTS/3u];   /* (a tree's layers': its batch, for its outline) */
    size_t b,nt=0,t;
    unsigned done=0;
    if(!verts || !batches || !take)return 0;
    /* the shadows' triangles, on the screen */
    for(b=0;b<count;b++){
        const NitroGx3dBatch *batch=&batches[b];
        uint32_t i;
        if(!NITRO_GX3D_TAG_IS_SHADE(batch->tag))continue;
        for(i=0;i+3u<=batch->count && nt<DARKEN_VERTS/3u;i+=3u){
            unsigned k,ok=1;
            for(k=0;k<3u;k++){
                const NitroGx3dGpuVertex *v=&verts[batch->first+i+k];
                float *o=ndc[nt*3u+k];
                if(!(v->position[3]>0.0f)){ok=0;break;}
                o[0]=v->position[0]/v->position[3];
                o[1]=v->position[1]/v->position[3];
                o[2]=(float)v->color[0]*(1.0f/255.0f);
                o[3]=(float)v->color[1]*(1.0f/255.0f);
                o[4]=(float)v->color[2]*(1.0f/255.0f);
                o[5]=v->uv[0];
                o[6]=v->uv[1];
            }
            /* (a tree's trunk's shadow is a strip in its quad, by its
             * colours: port_gx3d_pica.c, blob_batch; its layers', red 31, a
             * tree's shade: NITRO_SHADOW_TREE_BLOB of a full one) */
            if(ok){
                oval[nt]=(uint8_t)(batch->tag==NITRO_GX3D_TAG_TRUNK?1u:batch->tag==NITRO_GX3D_TAG_SHADE?2u:0u);
                of[nt]=batch;
                nt++;
            }
        }
    }
    if(!nt)return 0;
    /* each marked sprite's batch alone; each prop's batches (its mark's)
     * together, from the foot of them all (2026-11-12: the props, a
     * bench, a sign, a lamp post, stayed as bright as in the sun in a
     * shadow, "Some shadows dont affect objects which breaks realism") */
    for(b=0;b<count;b++){
        const NitroGx3dBatch *batch=&batches[b];
        float lo[2]={0,0},hi[2]={0,0},px,py,dark=0.0f;
        size_t c,e;
        uint32_t i;
        int ok=batch->count>=3u,prop=NITRO_GX3D_TAG_IS_PROP(batch->tag),first=1;
        if(!batch->tag || (batch->tag&1u) || ((batch->tag&0xC0000000u) && !prop))continue;
        if(prop){
            /* (once a prop: at its first batch) */
            for(c=0;c<b && batches[c].tag!=batch->tag;c++){}
            if(c<b)continue;
        }
        for(e=b;e<count && ok;e++){
            const NitroGx3dBatch *part=&batches[e];
            if(e!=b && (!prop || part->tag!=batch->tag))continue;
            for(i=0;i<part->count;i++){
                const NitroGx3dGpuVertex *v=&verts[part->first+i];
                float x,y;
                if(!(v->position[3]>0.0f)){ok=0;break;}
                x=v->position[0]/v->position[3];y=v->position[1]/v->position[3];
                if(first || x<lo[0])lo[0]=x;
                if(first || x>hi[0])hi[0]=x;
                if(first || y<lo[1])lo[1]=y;
                if(first || y>hi[1])hi[1]=y;
                first=0;
            }
            if(!prop)break;
        }
        if(!ok || first)continue;
        px=(lo[0]+hi[0])*0.5f;
        py=lo[1]+(hi[1]-lo[1])*NITRO_SHADOW_FOOT_UP;
        for(t=0;t<nt;t++){
            const float *a=ndc[t*3u],*c1=ndc[t*3u+1u],*c2=ndc[t*3u+2u];
            float area,w0,w1,w2,g;
            if((px<a[0] && px<c1[0] && px<c2[0]) || (px>a[0] && px>c1[0] && px>c2[0]) ||
               (py<a[1] && py<c1[1] && py<c2[1]) || (py>a[1] && py>c1[1] && py>c2[1]))
                continue;
            area=(c1[0]-a[0])*(c2[1]-a[1])-(c1[1]-a[1])*(c2[0]-a[0]);
            if(area>-1e-9f && area<1e-9f)continue;
            w1=((px-a[0])*(c2[1]-a[1])-(py-a[1])*(c2[0]-a[0]))/area;
            w2=((c1[0]-a[0])*(py-a[1])-(c1[1]-a[1])*(px-a[0]))/area;
            w0=1.0f-w1-w2;
            if(w0<0.0f || w1<0.0f || w2<0.0f)continue;
            g=w0*a[2]+w1*c1[2]+w2*c2[2];
            if(oval[t]==2u){
                if(maskAt && of[t]->texture_param){
                    /* (the outline at the feet) */
                    const float u=w0*a[5]+w1*c1[5]+w2*c2[5],tv=w0*a[6]+w1*c1[6]+w2*c2[6];
                    g=maskAt(maskCtx,of[t]->texture_param,of[t]->palette_base,u,tv);
                    if(!(g>=NITRO_SHADOW_FOOT_IN))continue;
                }
                g*=NITRO_SHADOW_TREE_BLOB;
            }else if(oval[t]){
                const float gg=w0*a[3]+w1*c1[3]+w2*c2[3]-0.5f,bb=w0*a[4]+w1*c1[4]+w2*c2[4]-0.5f,rr=g-0.5f;
                g=4.0f*(1.0f-4.0f*(rr*rr+gg*gg+bb*bb));
                if(g>1.0f)g=1.0f;
                if(!(g>0.0f))continue;
                g*=NITRO_SHADOW_TREE_BLOB;
            }
            if(g>dark)dark=g;
        }
        if(!(dark>0.0f))continue;
        if(dark<NITRO_SHADOW_FOOT_MIN)dark=NITRO_SHADOW_FOOT_MIN;
        if(dark>1.0f)dark=1.0f;
        for(e=b;e<count;e++){
            const NitroGx3dBatch *part=&batches[e];
            if(e!=b && (!prop || part->tag!=batch->tag))continue;
            for(i=0;i<part->count;i++){
                NitroGx3dGpuVertex *v=&verts[part->first+i];
                unsigned k;
                for(k=0;k<3u;k++){
                    float m=1.0f-take[k]*dark;
                    if(m<0.0f)m=0.0f;
                    v->color[k]=(uint8_t)((float)v->color[k]*m+0.5f);
                }
            }
            if(!prop)break;
        }
        done++;
    }
    return done;
}
