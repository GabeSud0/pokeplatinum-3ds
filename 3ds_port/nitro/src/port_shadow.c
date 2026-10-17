/*
 * Sun shadows: where the sun is for a scene, and the shadow map's matrices.
 * See include/nitro_port_shadow.h.
 */
#include <nitro_port_shadow.h>
#include <math.h>
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

int NitroPort_ShadowSetup(const float *positions, size_t stride, size_t count,
                          const int32_t projection[16], unsigned screenWidth,
                          const NitroShadowSun *sun, unsigned mapSize, NitroShadowSetup *out)
{
    const float k = 1.0f / 4096.0f;
    float p[16];
    Back b;
    Bin bins[BINS];
    float total = 0.0f, lo1 = 1e30f, hi1 = -1e30f, lo2 = 1e30f, hi2 = -1e30f, lod = 1e30f, hid = -1e30f;
    float extent, texel, o1, o2, range, e, a;
    V3 up, x, t, d, b1, b2;
    size_t i;
    int j, best = -1;
    const unsigned char *base = (const unsigned char *)positions;

    memset(out, 0, sizeof(*out));
    if (count < 3 || mapSize == 0) return 0;
    for (j = 0; j < 16; j++) p[j] = (float)projection[j] * k;
    /* A camera's perspective: w from z alone (p[3] = p[7] = p[15] = 0,
     * p[11] != 0), x and y not mixed. */
    if (p[3] != 0.0f || p[7] != 0.0f || p[15] != 0.0f || p[11] == 0.0f) return 0;
    if (p[0] == 0.0f || p[5] == 0.0f || p[1] != 0.0f || p[4] != 0.0f) return 0;
    {
        /* z = w / p23; x = (x' * wide - z p20 - p30) / p00; y likewise */
        float wide = (float)(screenWidth ? screenWidth : 256u) / 256.0f;
        memset(&b, 0, sizeof(b));
        b.m[2][2] = 1.0f / p[11];
        b.m[0][0] = wide / p[0];
        b.m[0][2] = -p[8] / (p[11] * p[0]);
        b.m[0][3] = -p[12] / p[0];
        b.m[1][1] = 1.0f / p[5];
        b.m[1][2] = -p[9] / (p[11] * p[5]);
        b.m[1][3] = -p[13] / p[5];
    }

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
    x = normalize(sub(v3(1.0f, 0.0f, 0.0f), scale(up, up.x)));
    t = cross(x, up);
    if (t.z < 0.0f) t = scale(t, -1.0f);
    e = sun->elevation * 3.14159265f / 180.0f;
    a = sun->azimuth * 3.14159265f / 180.0f;
    d = normalize(add(scale(up, -sinf(e)), scale(add(scale(x, cosf(a)), scale(t, sinf(a))), cosf(e))));
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
    out->up[0] = up.x; out->up[1] = up.y; out->up[2] = up.z;
    out->light[0] = d.x; out->light[1] = d.y; out->light[2] = d.z;
    out->extent = extent;
    memcpy(out->back, b.m, sizeof(out->back));
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
 * between 9:30 and 12:30, high, its shadows short. Never lower than 30
 * degrees: the shadows at most about twice as long as what casts them (a
 * real low sun's ran across the whole screen). */
static const float sSunWay[][3] = {
    { 5.0f, 30.0f, 170.0f },
    { 8.0f, 42.0f, 158.0f },
    { 9.5f, 52.0f, 150.0f },
    { 12.5f, 65.0f, 45.0f },
    { 16.0f, 55.0f, 30.0f },
    { 19.0f, 40.0f, 14.0f },
    { 20.0f, 32.0f, 8.0f },
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
