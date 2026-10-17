/*
 * Tests for the sun shadows' setup (src/port_shadow.c), on a made-up scene
 * drawn as the game would: a camera looking down at ground with a post on
 * it, through Platinum's field projection, its x for a 320-wide view.
 */
#include <nitro_port_shadow.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static int sChecks, sFailures;
#define CHECK(cond) do { sChecks++; if (!(cond)) { sFailures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define NEAR(a, b, eps) (fabs((double)(a) - (double)(b)) <= (eps))

/* The field's projection (the PC runner's log, Jubilife): rows as the DS's */
static const int32_t sProj[16] = {
    21628, 0, 0, 0,
    0, 28836, 0, 0,
    0, 0, -5734, -4096,
    0, 0, -1474560, 0,
};
#define WIDTH 320u
#define PITCH 0.95   /* radians the camera looks down */

typedef struct { float pos[4]; float pad[3]; } Vtx;   /* (as the GPU's vertices: other fields after) */

/* a world point (y up) through the camera (above and behind the origin,
 * looking down at it) and the projection, into a vertex's x, y, z, w */
static void project(double wx, double wy, double wz, float *out, double *viewOut)
{
    double c = cos(PITCH), s = sin(PITCH);
    double px = wx, py = wy - 300.0 * s, pz = wz - 300.0 * c;   /* (the camera 300 away) */
    double v[3], clip[4];
    int j;
    v[0] = px;                 /* the screen's right */
    v[1] = py * c - pz * s;    /* the screen's up */
    v[2] = py * s + pz * c;    /* towards the camera */
    for (j = 0; j < 4; j++) {
        clip[j] = (v[0] * sProj[0 * 4 + j] + v[1] * sProj[1 * 4 + j] + v[2] * sProj[2 * 4 + j]) / 4096.0 +
                  sProj[3 * 4 + j] / 4096.0;
    }
    out[0] = (float)(clip[0] * 256.0 / WIDTH);
    out[1] = (float)clip[1];
    out[2] = (float)clip[2];
    out[3] = (float)clip[3];
    if (viewOut) memcpy(viewOut, v, sizeof(v));
}

static float apply(const float r[4], const float *p) { return r[0] * p[0] + r[1] * p[1] + r[2] * p[3] + r[3]; }

static int sCount;
static Vtx sV[600];
static void tri(const double a[3], const double b[3], const double c[3])
{
    project(a[0], a[1], a[2], sV[sCount++].pos, NULL);
    project(b[0], b[1], b[2], sV[sCount++].pos, NULL);
    project(c[0], c[1], c[2], sV[sCount++].pos, NULL);
}
static void quad(const double a[3], const double b[3], const double c[3], const double d[3])
{
    tri(a, b, c);
    tri(a, c, d);
}

static void build(int withGround, int cardsOnly)
{
    int i, j;
    sCount = 0;
    if (withGround) {
        for (i = -4; i < 4; i++) {
            for (j = -3; j < 3; j++) {
                double a[3] = { i * 20.0, 0, j * 20.0 }, b[3] = { i * 20.0 + 20, 0, j * 20.0 };
                double c[3] = { i * 20.0 + 20, 0, j * 20.0 + 20 }, d[3] = { i * 20.0, 0, j * 20.0 + 20 };
                quad(a, b, c, d);
            }
        }
    }
    if (!cardsOnly) {
        /* a post 4 wide, 30 high, at the origin: its four sides */
        double p[8][3] = { { -2, 0, -2 }, { 2, 0, -2 }, { 2, 0, 2 }, { -2, 0, 2 },
                           { -2, 30, -2 }, { 2, 30, -2 }, { 2, 30, 2 }, { -2, 30, 2 } };
        quad(p[0], p[1], p[5], p[4]);
        quad(p[1], p[2], p[6], p[5]);
        quad(p[2], p[3], p[7], p[6]);
        quad(p[3], p[0], p[4], p[7]);
    } else {
        /* cards facing the camera: only those */
        for (i = 0; i < 6; i++) {
            float q[4][4];
            double c = cos(PITCH), s = sin(PITCH);
            int k;
            for (k = 0; k < 4; k++) {
                double u = (k == 1 || k == 2) ? 8.0 : -8.0, v = k >= 2 ? 16.0 : 0.0;
                project(i * 20.0 - 50 + u, v * c, -v * s, q[k], NULL);   /* (along the screen's up) */
            }
            memcpy(sV[sCount++].pos, q[0], 16); memcpy(sV[sCount++].pos, q[1], 16); memcpy(sV[sCount++].pos, q[2], 16);
            memcpy(sV[sCount++].pos, q[0], 16); memcpy(sV[sCount++].pos, q[2], 16); memcpy(sV[sCount++].pos, q[3], 16);
        }
    }
}

static void test_post_shadow(void)
{
    NitroShadowSun sun = { 55.0f, 30.0f };
    NitroShadowSetup s;
    double c = cos(PITCH), sn = sin(PITCH);
    float top[4], ground[4], foot[4];
    double tx, tz;
    int ok;
    build(1, 0);
    ok = NitroPort_ShadowSetup(sV[0].pos, sizeof(Vtx), (size_t)sCount, sProj, WIDTH, &sun, 512, &s);
    CHECK(ok);
    if (!ok) return;
    /* the ground's normal in the camera's space: world up, turned */
    CHECK(NEAR(s.up[0], 0.0, 1e-3));
    CHECK(NEAR(s.up[1], c, 1e-3));
    CHECK(NEAR(s.up[2], sn, 1e-3));
    /* the light goes down, to the screen's right and down the screen */
    CHECK(s.light[0] > 0.0f);
    CHECK(s.light[0] * s.up[0] + s.light[1] * s.up[1] + s.light[2] * s.up[2] < -0.8f);
    /* The post's top corner and where its shadow falls (along the light,
     * down to the ground) are at the same place in the map, the top
     * nearer the sun; the post's foot isn't in that shadow. */
    {
        /* the light in the world: the camera's space turned back */
        double lx = s.light[0], ly = s.light[1] * c + s.light[2] * sn, lz = -s.light[1] * sn + s.light[2] * c;
        double k = 30.0 / -ly;
        tx = 2.0 + lx * k;
        tz = 2.0 + lz * k;
        CHECK(ly < 0.0);
    }
    project(2, 30, 2, top, NULL);
    project(tx, 0, tz, ground, NULL);
    project(2, 0, 2, foot, NULL);
    CHECK(NEAR(apply(s.receiver[0], top), apply(s.receiver[0], ground), 1e-3));
    CHECK(NEAR(apply(s.receiver[1], top), apply(s.receiver[1], ground), 1e-3));
    CHECK(apply(s.receiver[3], top) + s.bias < apply(s.receiver[3], ground));
    CHECK(fabs(apply(s.receiver[0], foot) - apply(s.receiver[0], ground)) > 0.01 ||
          fabs(apply(s.receiver[1], foot) - apply(s.receiver[1], ground)) > 0.01);
    /* everything inside the map, distances inside 0..1 */
    {
        int i, inside = 1;
        for (i = 0; i < sCount; i++) {
            float u = apply(s.receiver[0], sV[i].pos), v = apply(s.receiver[1], sV[i].pos), d = apply(s.receiver[3], sV[i].pos);
            if (u < 0 || u > 1 || v < 0 || v > 1 || d < 0 || d > 1) inside = 0;
        }
        CHECK(inside);
    }
    /* the shadow map pass: clip -1..1 for 0..1, z the -distance, w 1 */
    CHECK(NEAR(apply(s.caster[0], top), apply(s.receiver[0], top) * 2 - 1, 1e-4));
    CHECK(NEAR(apply(s.caster[1], top), apply(s.receiver[1], top) * 2 - 1, 1e-4));
    CHECK(NEAR(apply(s.caster[2], top), -apply(s.receiver[3], top), 1e-4));
    CHECK(NEAR(apply(s.caster[3], top), 1.0, 1e-6));
    /* a ground bias of a few texels, not more */
    CHECK(s.bias > 0.0f && s.bias < 0.05f);
}

/* the sun from the screen's top: the shadow goes down the screen */
static void test_azimuth(void)
{
    NitroShadowSun sun = { 50.0f, 90.0f };
    NitroShadowSetup s;
    build(1, 0);
    CHECK(NitroPort_ShadowSetup(sV[0].pos, sizeof(Vtx), (size_t)sCount, sProj, WIDTH, &sun, 512, &s));
    CHECK(NEAR(s.light[0], 0.0, 1e-3));
    /* along the ground towards the camera: down the screen */
    CHECK(-s.light[1] * sin(PITCH) + s.light[2] * cos(PITCH) > 0.3);
}

/* No ground (cards only), and a flat projection: no shadows. */
static void test_none(void)
{
    NitroShadowSun sun = { 55.0f, 30.0f };
    NitroShadowSetup s;
    int32_t ortho[16] = { 4096, 0, 0, 0, 0, 4096, 0, 0, 0, 0, 4096, 0, 0, 0, 0, 4096 };
    build(0, 1);
    CHECK(!NitroPort_ShadowSetup(sV[0].pos, sizeof(Vtx), (size_t)sCount, sProj, WIDTH, &sun, 512, &s));
    build(1, 0);
    CHECK(!NitroPort_ShadowSetup(sV[0].pos, sizeof(Vtx), (size_t)sCount, ortho, WIDTH, &sun, 512, &s));
    CHECK(!NitroPort_ShadowSetup(sV[0].pos, sizeof(Vtx), 0, sProj, WIDTH, &sun, 512, &s));
}

/* As the camera moves a little, the map stays the same size (its size
 * goes in steps: the shadows' texels don't change size every frame). */
static void test_stable(void)
{
    NitroShadowSun sun = { 55.0f, 30.0f };
    NitroShadowSetup s0, s1;
    int i;
    build(1, 0);
    NitroPort_ShadowSetup(sV[0].pos, sizeof(Vtx), (size_t)sCount, sProj, WIDTH, &sun, 512, &s0);
    for (i = 0; i < sCount; i++) sV[i].pos[0] -= (float)(3.3 * sProj[0] / 4096.0 * 256.0 / WIDTH);
    NitroPort_ShadowSetup(sV[0].pos, sizeof(Vtx), (size_t)sCount, sProj, WIDTH, &sun, 512, &s1);
    CHECK(NEAR(s0.extent, s1.extent, 1e-3));
}

/* The sun over the day, the periods, the grades. */
static void test_day(void)
{
    NitroShadowSun m, n, e, night;
    float sm, sn, se, sx, s0;
    static uint32_t table[32768];
    uint32_t white, grey;
    CHECK(NitroPort_LookPeriod(3) == NITRO_LOOK_NIGHT);
    CHECK(NitroPort_LookPeriod(4) == NITRO_LOOK_MORNING);
    CHECK(NitroPort_LookPeriod(10) == NITRO_LOOK_DAY);
    CHECK(NitroPort_LookPeriod(17) == NITRO_LOOK_EVENING);
    CHECK(NitroPort_LookPeriod(20) == NITRO_LOOK_NIGHT);
    NitroPort_ShadowSunAt(7, 0, &m, &sm);
    NitroPort_ShadowSunAt(12, 30, &n, &sn);
    NitroPort_ShadowSunAt(18, 30, &e, &se);
    NitroPort_ShadowSunAt(23, 0, &night, &sx);
    CHECK(m.azimuth > 135.0f);                      /* morning: from the right */
    CHECK(n.azimuth > 30.0f && n.azimuth < 60.0f);  /* midday: the top left */
    CHECK(e.azimuth < 25.0f);                       /* evening: from the left */
    CHECK(n.elevation > m.elevation && n.elevation > e.elevation);
    CHECK(NEAR(sm, 1.0, 1e-6) && NEAR(sn, 1.0, 1e-6) && NEAR(se, 1.0, 1e-6));
    CHECK(NEAR(sx, 0.35, 1e-6));                    /* the moon */
    NitroPort_ShadowSunAt(20, 0, &night, &s0);
    CHECK(s0 < 0.02f);                              /* dusk: neither */
    NitroPort_ShadowSunAt(4, 59, &night, &s0);
    CHECK(s0 < 0.05f);
    /* grades: white stays bright; warm by day, cool at night */
    NitroPort_LookGradeTable(NITRO_LOOK_DAY, table);
    white = table[0x7fff];
    grey = table[16 | (16 << 5) | (16 << 10)];
    CHECK((white >> 24) >= 0xF0u && (white & 0xFFu) == 0);
    CHECK((grey >> 24) > ((grey >> 8) & 0xFFu));     /* red over blue */
    NitroPort_LookGradeTable(NITRO_LOOK_NIGHT, table);
    grey = table[16 | (16 << 5) | (16 << 10)];
    CHECK(((grey >> 8) & 0xFFu) > (grey >> 24));     /* blue over red */
    /* the flag the game reads */
    NitroPort_SetSunShadows(1);
    CHECK(NitroPort_SunShadows());
    NitroPort_SetSunShadows(0);
    CHECK(!NitroPort_SunShadows());
}

/* What casts and receives; triangles that only cast reaching further
 * towards the sun */
static void test_roles_reach(void)
{
    NitroShadowSun sun = { 55.0f, 30.0f };
    NitroShadowSetup s, before;
    typedef struct { float pos[4]; uint8_t color[4]; float uv[2]; } GVtx;
    static GVtx g[700];
    float hi[3][4], ground[4], post[4];
    int i, n, groundCount, found = -1;
    double roof[3][3] = { { -10, 40, 20 }, { 10, 40, 20 }, { 0, 50, 30 } };   /* tilted 45 degrees */
    build(1, 0);
    groundCount = 8 * 6 * 6;
    CHECK(NitroPort_ShadowSetup(sV[0].pos, sizeof(Vtx), (size_t)sCount, sProj, WIDTH, &sun, 512, &s));
    n = sCount;
    tri(roof[0], roof[1], roof[2]);
    for (i = 0; i < sCount; i++) {
        memcpy(g[i].pos, sV[i].pos, sizeof(g[i].pos));
        g[i].color[3] = 255;
    }
    g[sCount] = g[0];                 /* a translucent ground triangle: left as it is */
    g[sCount + 1] = g[1];
    g[sCount + 2] = g[2];
    for (i = 0; i < 3; i++) g[sCount + i].color[3] = 128;
    NitroPort_ShadowRoles(g[0].pos, &g[0].color[3], sizeof(GVtx), (size_t)sCount + 3, &s);
    for (i = 0; i < groundCount; i++) {
        if (g[i].color[3] != NITRO_SHADOW_RECEIVE_ONLY) break;
    }
    CHECK(i == groundCount);                                   /* the ground: flat */
    for (i = groundCount; i < n; i++) {
        if (g[i].color[3] != NITRO_SHADOW_CAST_ONLY) break;
    }
    CHECK(i == n);                                             /* the post's sides: upright */
    CHECK(g[n].color[3] == NITRO_SHADOW_CAST_RECEIVE && g[n + 2].color[3] == NITRO_SHADOW_CAST_RECEIVE);
    CHECK(g[sCount].color[3] == 128);

    /* a point high over the post (outside the view: only casting), over the
     * map but nearer the sun than anything in the scene */
    for (i = 40; i <= 400 && found < 0; i += 10) {
        float u, v;
        project(0.0, (double)i, 0.0, hi[0], NULL);
        u = apply(s.receiver[0], hi[0]);
        v = apply(s.receiver[1], hi[0]);
        if (u > 0.05f && u < 0.95f && v > 0.05f && v < 0.95f && apply(s.receiver[3], hi[0]) < -0.05f) found = i;
    }
    CHECK(found > 0);
    project(-1.0, (double)found, 0.0, hi[1], NULL);
    project(1.0, (double)found, 1.0, hi[2], NULL);
    project(10.0, 0.0, 10.0, ground, NULL);
    project(2.0, 30.0, 2.0, post, NULL);
    before = s;
    NitroPort_ShadowReach(hi[0], sizeof(hi[0]), 3, &s);
    CHECK(apply(s.receiver[3], hi[0]) > -1e-4f);
    CHECK(apply(s.receiver[3], ground) < 1.0f + 1e-4f);
    CHECK(apply(s.receiver[3], post) + s.bias < apply(s.receiver[3], ground));
    CHECK(s.bias < before.bias);   /* (the same in the scene: the distances longer) */
    CHECK(!memcmp(s.receiver[0], before.receiver[0], sizeof(s.receiver[0])));   /* the map stays where it is */
    for (i = 0; i < 4; i++) CHECK(s.caster[2][i] == -s.receiver[3][i]);
    /* far off the map: nothing changes */
    s = before;
    project(2000.0, 300.0, 0.0, hi[0], NULL);
    project(2001.0, 300.0, 0.0, hi[1], NULL);
    project(2000.0, 301.0, 1.0, hi[2], NULL);
    NitroPort_ShadowReach(hi[0], sizeof(hi[0]), 3, &s);
    CHECK(!memcmp(&s, &before, sizeof(s)));
}

int main(void)
{
    test_roles_reach();
    test_day();
    test_post_shadow();
    test_azimuth();
    test_none();
    test_stable();
    printf("sun shadows: %d checks, %d failures\n", sChecks, sFailures);
    return sFailures != 0;
}
