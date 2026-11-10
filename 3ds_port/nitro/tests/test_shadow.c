/*
 * Tests for the sun shadows' setup (src/port_shadow.c), on a made-up scene
 * drawn as the game would: a camera looking down at ground with a post on
 * it, through Platinum's field projection, its x for a 320-wide view.
 */
#include <nitro_port_shadow.h>
#include <nitro_port_gx3d_vertex.h>
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
    ok = NitroPort_ShadowSetup(sV[0].pos, sizeof(Vtx), (size_t)sCount, sProj, WIDTH, 192, &sun, 512, &s);
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
    CHECK(NitroPort_ShadowSetup(sV[0].pos, sizeof(Vtx), (size_t)sCount, sProj, WIDTH, 192, &sun, 512, &s));
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
    CHECK(!NitroPort_ShadowSetup(sV[0].pos, sizeof(Vtx), (size_t)sCount, sProj, WIDTH, 192, &sun, 512, &s));
    build(1, 0);
    CHECK(!NitroPort_ShadowSetup(sV[0].pos, sizeof(Vtx), (size_t)sCount, ortho, WIDTH, 192, &sun, 512, &s));
    CHECK(!NitroPort_ShadowSetup(sV[0].pos, sizeof(Vtx), 0, sProj, WIDTH, 192, &sun, 512, &s));
}

/* As the camera moves a little, the map stays the same size (its size
 * goes in steps: the shadows' texels don't change size every frame). */
static void test_stable(void)
{
    NitroShadowSun sun = { 55.0f, 30.0f };
    NitroShadowSetup s0, s1;
    int i;
    build(1, 0);
    NitroPort_ShadowSetup(sV[0].pos, sizeof(Vtx), (size_t)sCount, sProj, WIDTH, 192, &sun, 512, &s0);
    for (i = 0; i < sCount; i++) sV[i].pos[0] -= (float)(3.3 * sProj[0] / 4096.0 * 256.0 / WIDTH);
    NitroPort_ShadowSetup(sV[0].pos, sizeof(Vtx), (size_t)sCount, sProj, WIDTH, 192, &sun, 512, &s1);
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
    /* and the shadow map's reach, apart from it */
    NitroPort_SetShadowReach(1);
    CHECK(NitroPort_ShadowReachOn() && !NitroPort_SunShadows());
    NitroPort_SetShadowReach(0);
    CHECK(!NitroPort_ShadowReachOn());
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
    CHECK(NitroPort_ShadowSetup(sV[0].pos, sizeof(Vtx), (size_t)sCount, sProj, WIDTH, 192, &sun, 512, &s));
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

/* Cards facing the camera (a character): a copy turned to the sun, upright,
 * its foot where it stands; the card itself neither casts nor receives.
 * And the game's own painted shadows: flat, see-through, dark. */
static void test_cards_decals(void)
{
    static NitroGx3dGpuVertex v[600], out[64];
    NitroGx3dBatch batch, outBatches[4];
    NitroShadowSetup s;
    NitroShadowSun side = { 40.0f, 0.0f }, top = { 40.0f, 90.0f };
    size_t nb = 0, w, i;
    int n, ground, k;
    double c = cos(PITCH), sn = sin(PITCH), corner[4][3];
    static uint32_t dark[16], light[16];
    build(1, 0);
    ground = 8 * 6 * 6;
    for (i = 0; i < (size_t)ground; i++) {
        memcpy(v[i].position, sV[i].pos, sizeof(v[i].position));
        v[i].color[3] = 255;
    }
    /* a card 16 wide and 16 long, leaning back to face the camera */
    for (k = 0; k < 4; k++) {
        double u = (k == 1 || k == 2) ? 8.0 : -8.0, along = k >= 2 ? 16.0 : 0.0;
        corner[k][0] = u; corner[k][1] = along * c; corner[k][2] = -along * sn;
    }
    {
        static const int order[6] = { 0, 1, 2, 0, 2, 3 };
        for (k = 0; k < 6; k++) {
            project(corner[order[k]][0], corner[order[k]][1], corner[order[k]][2], v[ground + k].position, NULL);
            v[ground + k].color[3] = 255;
        }
    }
    n = ground + 6;
    memset(&batch, 0, sizeof(batch));
    batch.count = (uint32_t)n;
    CHECK(NitroPort_ShadowSetup(v[0].position, sizeof(v[0]), (size_t)n, sProj, WIDTH, 192, &side, 512, &s));
    NitroPort_ShadowRoles(v[0].position, &v[0].color[3], sizeof(v[0]), (size_t)n, &s);
    w = NitroPort_ShadowCards(v, &batch, 1, (size_t)n, out, 64, outBatches, 4, &nb, &s);
    CHECK(w == 6 && nb == 1 && outBatches[0].first == (uint32_t)n && outBatches[0].count == 6);
    for (k = 0; k < 6; k++) {
        CHECK(v[ground + k].color[3] == NITRO_SHADOW_NEITHER);
        CHECK(out[k].color[3] == NITRO_SHADOW_CAST_ONLY);
    }
    for (i = 0; i < (size_t)ground; i++) CHECK(v[i].color[3] == NITRO_SHADOW_RECEIVE_ONLY);
    /* from the side: turned a quarter, its foot's corners now one behind
     * the other (x 0: the middle of its foot), as far either way */
    CHECK(fabs(out[0].position[0]) < 0.5 && fabs(out[1].position[0]) < 0.5);
    CHECK(fabs(out[0].position[1] + out[1].position[1]) < 0.5 && fabs(out[0].position[1]) > 10.0);
    /* from the top (straight towards the camera): facing it already, only
     * stood upright: its foot as it was, its top lower on the screen */
    for (k = 0; k < 6; k++) v[ground + k].color[3] = 255;
    CHECK(NitroPort_ShadowSetup(v[0].position, sizeof(v[0]), (size_t)n, sProj, WIDTH, 192, &top, 512, &s));
    w = NitroPort_ShadowCards(v, &batch, 1, (size_t)n, out, 64, outBatches, 4, &nb, &s);
    CHECK(w == 6);
    CHECK(NEAR(out[0].position[0], v[ground].position[0], 0.1) && NEAR(out[0].position[1], v[ground].position[1], 0.1));
    CHECK(out[2].position[1] < v[ground + 2].position[1] - 1.0f);
    /* no room: nothing */
    for (k = 0; k < 6; k++) v[ground + k].color[3] = 255;
    CHECK(NitroPort_ShadowCards(v, &batch, 1, (size_t)n, out, 3, outBatches, 4, &nb, &s) == 0);
    /* a translucent batch's cards are left alone */
    batch.translucent = 1;
    CHECK(NitroPort_ShadowCards(v, &batch, 1, (size_t)n, out, 64, outBatches, 4, &nb, &s) == 0);

    /* painted shadows */
    for (k = 0; k < 16; k++) { dark[k] = 0x101010FFu; light[k] = 0xF0F0F0FFu; }
    dark[3] = 0xFFFFFF00u;   /* (a see-through texel: doesn't count) */
    CHECK(NitroPort_ShadowDecal(v[0].position, sizeof(v[0]), 9, 0, dark, 16, 0x7FFF, &s));
    CHECK(!NitroPort_ShadowDecal(v[0].position, sizeof(v[0]), 9, 0, light, 16, 0x7FFF, &s));
    CHECK(!NitroPort_ShadowDecal(v[0].position, sizeof(v[0]), 31, 0, dark, 16, 0x7FFF, &s));   /* opaque */
    CHECK(NitroPort_ShadowDecal(v[0].position, sizeof(v[0]), 31, 1, dark, 16, 0x7FFF, &s));    /* A3I5 */
    CHECK(NitroPort_ShadowDecal(v[0].position, sizeof(v[0]), 12, 0, NULL, 0, 0x0421, &s));     /* untextured, dark */
    CHECK(!NitroPort_ShadowDecal(v[0].position, sizeof(v[0]), 12, 0, NULL, 0, 0x7FFF, &s));
    CHECK(!NitroPort_ShadowDecal(v[ground].position, sizeof(v[0]), 9, 0, dark, 16, 0x7FFF, &s)); /* upright */
    /* its two halves: the shape alone, then the texture alone */
    CHECK(NitroPort_ShadowDecalShape(v[0].position, sizeof(v[0]), 9, 0, &s));
    CHECK(!NitroPort_ShadowDecalShape(v[0].position, sizeof(v[0]), 31, 0, &s));          /* opaque */
    CHECK(NitroPort_ShadowDecalShape(v[0].position, sizeof(v[0]), 31, 1, &s));           /* A3I5 */
    CHECK(!NitroPort_ShadowDecalShape(v[ground].position, sizeof(v[0]), 9, 0, &s));      /* upright */
    CHECK(NitroPort_ShadowDecalDark(dark, 16, 0x7FFF));
    CHECK(!NitroPort_ShadowDecalDark(light, 16, 0x7FFF));
    CHECK(NitroPort_ShadowDecalDark(NULL, 0, 0x0421));
    CHECK(!NitroPort_ShadowDecalDark(NULL, 0, 0x7FFF));
}

/* camera space from a vertex, through a setup's back rows */
static void back_of(const NitroShadowSetup *s, const float *p, double out[3])
{
    int r;
    for (r = 0; r < 3; r++) out[r] = s->back[r][0] * p[0] + s->back[r][1] * p[1] + s->back[r][2] * p[3] + s->back[r][3];
}
static double dot3(const double a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

#define SPRITE_PARAM ((3u << 26) | (1u << 29) | (2u << 20) | (2u << 23))
static int sFeetAsked;
/* (the card's texture: its bottom 8 rows of 32 clear, the rows from the
 * foot's side t 31 down to t 0) */
static int fake_feet(void *ctx, uint32_t param, uint32_t palette, int s0, int s1, int tFrom, int tTo)
{
    (void)ctx; (void)palette;
    sFeetAsked = param == SPRITE_PARAM && s0 == 0 && s1 == 32 && tFrom == 31 && tTo == 0;
    return 23;
}

/* a card 16 wide and tall long, leaning back to face the camera, its foot
 * at (x, 0, z), the foot at t 32; into v at k */
static void stand_card(NitroGx3dGpuVertex *v, int at, double x, double z, double tall)
{
    static const int order[6] = { 0, 1, 2, 0, 2, 3 };
    static const float uvs[4][2] = { { 0, 1 }, { 1, 1 }, { 1, 0 }, { 0, 0 } };
    double c = cos(PITCH), sn = sin(PITCH), corner[4][3];
    int k;
    for (k = 0; k < 4; k++) {
        double u = (k == 1 || k == 2) ? 8.0 : -8.0, along = k >= 2 ? tall : 0.0;
        corner[k][0] = x + u; corner[k][1] = along * c; corner[k][2] = z - along * sn;
    }
    for (k = 0; k < 6; k++) {
        project(corner[order[k]][0], corner[order[k]][1], corner[order[k]][2], v[at + k].position, NULL);
        v[at + k].color[3] = 255;
        v[at + k].uv[0] = uvs[order[k]][0];
        v[at + k].uv[1] = uvs[order[k]][1];
    }
}
/* a quad lying on the ground (y), hw by hd about (x, z), into v at k */
static void lay_quad(NitroGx3dGpuVertex *v, int at, double x, double y, double z, double hw, double hd)
{
    static const int order[6] = { 0, 1, 2, 0, 2, 3 };
    double corner[4][3] = { { x - hw, y, z + hd }, { x + hw, y, z + hd }, { x + hw, y, z - hd }, { x - hw, y, z - hd } };
    int k;
    for (k = 0; k < 6; k++) {
        project(corner[order[k]][0], corner[order[k]][1], corner[order[k]][2], v[at + k].position, NULL);
        v[at + k].color[3] = 128;
    }
}
/* a camera-space point on the screen (up to a scale): x and y over its
 * distance */
static void on_screen(const double q[3], double out[2])
{
    out[0] = q[0] / -q[2];
    out[1] = q[1] / -q[2];
}

/* The sun shadows: a character's sprite and its round shadow go together
 * by the game's marks alone (the sprite's mark, the shadow's that | 1),
 * wherever they are; the sprite laid on the ground from the round shadow's
 * middle, its rows slid to the screen's lower right, soft, fading, pulled
 * towards the camera; its z by the depth row given (here the DS's clip z
 * itself). None for a sprite without a round shadow, a round shadow
 * without its sprite, or unmarked ones however near. */
static void test_ground_cards(void)
{
    static NitroGx3dGpuVertex v[700], out[256];
    NitroGx3dBatch batches[8], outBatches[4];
    NitroShadowSetup s;
    NitroShadowSun side = { 40.0f, 0.0f };
    uint8_t used[8];
    const float shade[3] = { 0.58f, 0.60f, 0.70f };
    const int T = NITRO_SHADOW_GROUND_TAPS;
    const double keep = 1.0 - NITRO_SHADOW_GROUND_PULL;
    const uint32_t HERO = 0x2000u, SIGN = 0x3000u, GONE = 0x4000u;
    size_t nb = 0, w, i;
    int n, ground, hero, round0, round1, round2, tuft, sign, k;
    unsigned sprites = 9, shadows = 9, laid = 9;
    double worst = 0.0, mk[3];
    build(1, 0);
    ground = 8 * 6 * 6;
    for (i = 0; i < (size_t)ground; i++) {
        memcpy(v[i].position, sV[i].pos, sizeof(v[i].position));
        v[i].color[3] = 255;
    }
    hero = ground;
    stand_card(v, hero, 0.0, 6.0, 16.0);                   /* (the character: 6 nearer than its round shadow) */
    round0 = hero + 6;
    lay_quad(v, round0, -0.5, 0.1, 1.0, 6.0, 4.0);         /* (its round shadow) */
    round1 = round0 + 6;
    lay_quad(v, round1, -48.0, 0.1, 0.0, 6.0, 4.0);        /* (the round shadow of someone not in the scene) */
    round2 = round1 + 6;
    lay_quad(v, round2, 48.0, 0.1, 0.0, 6.0, 4.0);         /* (an unmarked one, under the sign) */
    tuft = round2 + 6;
    stand_card(v, tuft, 0.0, 1.0, 8.0);                    /* (grass at the character's feet: unmarked) */
    sign = tuft + 6;
    stand_card(v, sign, 48.0, 0.0, 16.0);                  /* (a sign: marked, no round shadow of its own) */
    n = sign + 6;
    memset(batches, 0, sizeof(batches));
    batches[0].count = (uint32_t)ground;
    batches[1].first = (uint32_t)hero;
    batches[1].count = 6;
    batches[1].texture_param = SPRITE_PARAM;   /* (16 colours, colour 0 clear, 32 x 32) */
    batches[1].tag = HERO;
    for (k = 0; k < 3; k++) {
        static const uint32_t tags[3] = { 0x2001u, 0x4001u, 0 };
        batches[2 + k].first = (uint32_t)(round0 + 6 * k);
        batches[2 + k].count = 6;
        batches[2 + k].texture_param = (1u << 26) | (1u << 20) | (1u << 23);
        batches[2 + k].translucent = 1;
        batches[2 + k].tag = tags[k];
    }
    batches[5].first = (uint32_t)tuft;
    batches[5].count = 6;
    batches[5].texture_param = SPRITE_PARAM;
    batches[5].palette_base = 0x100;
    batches[6].first = (uint32_t)sign;
    batches[6].count = 6;
    batches[6].texture_param = SPRITE_PARAM;
    batches[6].palette_base = 0x200;
    batches[6].tag = SIGN;
    (void)GONE;
    CHECK(NitroPort_ShadowSetup(v[0].position, sizeof(v[0]), (size_t)ground, sProj, WIDTH, 192, &side, 512, &s));
    /* the depth row: every vertex's own clip z from its x, y, w */
    for (i = 0; i < (size_t)n; i++) {
        double z = s.zclip[0] * v[i].position[0] + s.zclip[1] * v[i].position[1] + s.zclip[2] * v[i].position[3] +
                   s.zclip[3];
        double e = fabs(z - v[i].position[2]) / (fabs(v[i].position[2]) + 1.0);
        if (e > worst) worst = e;
    }
    CHECK(worst < 1e-4);
    /* the GPU path's z, as the port makes it: the DS's depth from clip z
     * and w (port_gx3d.c), then normalize_depth's remap (port_gx3d_pica.c),
     * against NitroPort_ShadowDepthRow on x, y, w alone */
    {
        const float nearD = 1.0f / 1024.0f, lo = 0.31f, kz = 3.7f;
        float row[4], rowW[4];
        double worstZ = 0.0, worstW = 0.0;
        NitroPort_ShadowDepthRow(&s, 0, lo - nearD / kz, kz, row);
        NitroPort_ShadowDepthRow(&s, 1, 0.002f, 900.0f, rowW);
        for (i = 0; i < (size_t)n; i++) {
            const float *q = v[i].position;   /* (x, y, the DS's clip z, w) */
            double nz = q[2] / q[3], d = (nz + 1.0) * 8388607.5 / 16777215.0;
            double zPath = -(nearD + (d - lo) * kz) * q[3];
            double dW = q[3] * 4096.0 / 16777215.0, wPath = -(dW - 0.002) * 900.0;
            double zRow = row[0] * q[0] + row[1] * q[1] + row[2] * q[3] + row[3];
            double wRow = rowW[0] * q[0] + rowW[1] * q[1] + rowW[2] * q[3] + rowW[3];
            if (fabs(zRow - zPath) / (fabs(zPath) + 1e-3) > worstZ) worstZ = fabs(zRow - zPath) / (fabs(zPath) + 1e-3);
            if (fabs(wRow - wPath) / (fabs(wPath) + 1e-3) > worstW) worstW = fabs(wRow - wPath) / (fabs(wPath) + 1e-3);
        }
        CHECK(worstZ < 1e-3);
        CHECK(worstW < 1e-4);
    }
    /* the round shadow's middle (camera space) */
    {
        double q[3];
        static const int corner[4] = { 0, 1, 2, 5 };
        mk[0] = mk[1] = mk[2] = 0.0;
        for (k = 0; k < 4; k++) {
            int j;
            back_of(&s, v[round0 + corner[k]].position, q);
            for (j = 0; j < 3; j++) mk[j] += q[j] * 0.25;
        }
    }

    /* the shadow: the character's alone (its texture), TAPS copies; its
     * round shadow's batch to leave out, no other */
    memset(used, 0, sizeof(used));
    w = NitroPort_ShadowGroundCards(v, batches, 7, (size_t)n, out, 256, outBatches, 4, &nb, &s, s.zclip, used, shade,
                                    NULL, NULL);
    CHECK(w == (size_t)(6 * T) && nb == 1 && outBatches[0].first == (uint32_t)n && outBatches[0].count == (uint32_t)w);
    CHECK(outBatches[0].texture_param == SPRITE_PARAM && outBatches[0].palette_base == 0 && !outBatches[0].translucent);
    CHECK(outBatches[0].tag == 0);
    CHECK(used[2] == 1 && !used[0] && !used[1] && !used[3] && !used[4] && !used[5] && !used[6]);
    NitroPort_ShadowGroundStats(&sprites, &shadows, &laid);
    CHECK(sprites == 2 && shadows == 2 && laid == 1);
    /* the card itself untouched (verts are only read) */
    for (k = 0; k < 6; k++) CHECK(v[hero + k].color[3] == 255);
    {
        double q[3], p0[3], p1[3], p2[3], a[2], b2[2], hM = mk[0] * s.up[0] + mk[1] * s.up[1] + mk[2] * s.up[2];
        double d = 0.0;
        /* every copy: on the round shadow's ground once the pull's undone;
         * its z by the row given */
        for (i = 0; i < w; i++) {
            back_of(&s, out[i].position, q);
            for (k = 0; k < 3; k++) q[k] /= keep;
            CHECK(NEAR(dot3(q, s.up), hM, 0.05));
            CHECK(NEAR(out[i].position[2], s.zclip[0] * out[i].position[0] + s.zclip[1] * out[i].position[1] +
                                                s.zclip[2] * out[i].position[3] + s.zclip[3], 1e-3));
        }
        /* the middle tap: the foot's corners (0, 1) 16 apart, their middle
         * the round shadow's (not under the card, 6 away) */
        back_of(&s, out[0].position, p0);
        back_of(&s, out[1].position, p1);
        back_of(&s, out[2].position, p2);
        for (k = 0; k < 3; k++) { p0[k] /= keep; p1[k] /= keep; p2[k] /= keep; }
        for (k = 0; k < 3; k++) d += (p1[k] - p0[k]) * (p1[k] - p0[k]);
        CHECK(NEAR(sqrt(d), 16.0, 0.1));
        for (k = 0; k < 3; k++) CHECK(NEAR((p0[k] + p1[k]) * 0.5, mk[k], 0.05));
        /* the head (corner 2) from the foot (1): along the ground the
         * way the sun's light goes, the card's height times the sun's
         * cotangent long. This sun is at the screen's left (its light to
         * the right, along the sprite's own line): the shadow's way is
         * kept NITRO_SHADOW_GROUND_SIDE off that line, towards the camera */
        {
            double right[3], toward[3], n = 0.0, want, side = NITRO_SHADOW_GROUND_SIDE * 3.14159265 / 180.0;
            right[0] = 1.0 - s.up[0] * s.up[0]; right[1] = -s.up[1] * s.up[0]; right[2] = -s.up[2] * s.up[0];
            for (k = 0; k < 3; k++) n += right[k] * right[k];
            for (k = 0; k < 3; k++) right[k] /= sqrt(n);
            toward[0] = right[1] * s.up[2] - right[2] * s.up[1];
            toward[1] = right[2] * s.up[0] - right[0] * s.up[2];
            toward[2] = right[0] * s.up[1] - right[1] * s.up[0];
            want = 16.0 * NITRO_SHADOW_GROUND_LENGTH / tan(40.0 * 3.14159265 / 180.0);
            for (k = 0; k < 3; k++)
                CHECK(NEAR(p2[k] - p1[k], (right[k] * cos(side) + toward[k] * sin(side)) * want, 0.1));
            /* (down the screen and to its right) */
            on_screen(p1, a);
            on_screen(p2, b2);
            CHECK(b2[0] > a[0] && b2[1] < a[1]);
        }
        /* pulled along the ray: where it is on the screen unchanged */
        back_of(&s, out[0].position, q);
        on_screen(q, a);
        on_screen(p0, b2);
        CHECK(NEAR(a[0], b2[0], 1e-5) && NEAR(a[1], b2[1], 1e-5));
        CHECK(NEAR(q[2] / p0[2], keep, 1e-4));
        /* the other taps: the same corner, a little apart (the blur's
         * radius, 0.5 to 2.25), still on the ground */
        for (k = 1; k < T; k++) {
            double t[3], dd = 0.0;
            int j;
            back_of(&s, out[6 * k].position, t);
            for (j = 0; j < 3; j++) { t[j] /= keep; dd += (t[j] - p0[j]) * (t[j] - p0[j]); }
            CHECK(sqrt(dd) > 0.45 && sqrt(dd) < 2.25 / 0.35 + 0.01);
        }
    }
    /* the colours: each tap the TAPS-th root of the shade at the feet, of
     * TIP of it at the head */
    for (k = 0; k < 3; k++) {
        double atFeet = pow(shade[k], 1.0 / T), atHead = pow(1.0 - (1.0 - shade[k]) * NITRO_SHADOW_GROUND_TIP, 1.0 / T);
        CHECK(NEAR(out[0].color[k], atFeet * 255.0, 1.0) && NEAR(out[6 * (T - 1)].color[k], atFeet * 255.0, 1.0));
        CHECK(NEAR(out[2].color[k], atHead * 255.0, 1.0));
    }
    /* the picture's feet a quarter up the card (its bottom 8 of 32 rows
     * clear): a quarter along the shadow is the round shadow's middle */
    {
        double a[3], bq[3], cq[3], d[3];
        w = NitroPort_ShadowGroundCards(v, batches, 7, (size_t)n, out, 256, outBatches, 4, &nb, &s, s.zclip, NULL,
                                        shade, fake_feet, NULL);
        CHECK(w == (size_t)(6 * T) && sFeetAsked);
        back_of(&s, out[0].position, a);
        back_of(&s, out[1].position, bq);
        back_of(&s, out[2].position, cq);
        back_of(&s, out[5].position, d);
        for (k = 0; k < 3; k++) CHECK(NEAR(((a[k] + bq[k]) * 0.375 + (cq[k] + d[k]) * 0.125) / keep, mk[k], 0.05));
    }
    /* the sun over the top of the screen, 60 degrees up: the shadow
     * straight down the screen, 16 cot(60) long; in the morning, on the
     * right (150 degrees round: just at the limit), to the lower left */
    {
        static const NitroShadowSun top = { 60.0f, 90.0f }, morning = { 50.0f, 150.0f };
        NitroShadowSetup s2;
        double p1[3], p2[3], a[2], b2[2], len = 0.0;
        CHECK(NitroPort_ShadowSetup(v[0].position, sizeof(v[0]), (size_t)ground, sProj, WIDTH, 192, &top, 512, &s2));
        w = NitroPort_ShadowGroundCards(v, batches, 7, (size_t)n, out, 256, outBatches, 4, &nb, &s2, s2.zclip, NULL,
                                        shade, NULL, NULL);
        CHECK(w == (size_t)(6 * T));
        back_of(&s2, out[1].position, p1);
        back_of(&s2, out[2].position, p2);
        for (k = 0; k < 3; k++) len += (p2[k] - p1[k]) * (p2[k] - p1[k]) / (keep * keep);
        CHECK(NEAR(sqrt(len), 16.0 * NITRO_SHADOW_GROUND_LENGTH / tan(60.0 * 3.14159265 / 180.0), 0.1));
        on_screen(p1, a);
        on_screen(p2, b2);
        CHECK(NEAR(b2[0], a[0], 1e-3) && b2[1] < a[1]);
        CHECK(NitroPort_ShadowSetup(v[0].position, sizeof(v[0]), (size_t)ground, sProj, WIDTH, 192, &morning, 512, &s2));
        w = NitroPort_ShadowGroundCards(v, batches, 7, (size_t)n, out, 256, outBatches, 4, &nb, &s2, s2.zclip, NULL,
                                        shade, NULL, NULL);
        CHECK(w == (size_t)(6 * T));
        back_of(&s2, out[1].position, p1);
        back_of(&s2, out[2].position, p2);
        on_screen(p1, a);
        on_screen(p2, b2);
        CHECK(b2[0] < a[0] && b2[1] < a[1]);
    }
    /* the same from the two triangles in two batches of the round shadow
     * (its middle still the quad's) */
    {
        NitroGx3dBatch split[8];
        double p0[3], p1[3];
        memcpy(split, batches, sizeof(split));
        split[2].count = 3;
        split[7] = split[2];
        split[7].first = (uint32_t)round0 + 3;
        memset(used, 0, sizeof(used));
        w = NitroPort_ShadowGroundCards(v, split, 8, (size_t)n, out, 256, outBatches, 4, &nb, &s, s.zclip, used, shade,
                                        NULL, NULL);
        CHECK(w == (size_t)(6 * T) && used[2] == 1 && used[7] == 1);
        back_of(&s, out[0].position, p0);
        back_of(&s, out[1].position, p1);
        for (k = 0; k < 3; k++) CHECK(NEAR((p0[k] + p1[k]) * 0.5 / keep, mk[k], 0.05));
    }
    /* none: no room; the character's batch translucent (fading) or gone;
     * no marks at all */
    CHECK(NitroPort_ShadowGroundCards(v, batches, 7, (size_t)n, out, 6 * T - 1, outBatches, 4, &nb, &s, s.zclip, NULL,
                                      shade, NULL, NULL) == 0);
    batches[1].translucent = 1;
    memset(used, 0, sizeof(used));
    CHECK(NitroPort_ShadowGroundCards(v, batches, 7, (size_t)n, out, 256, outBatches, 4, &nb, &s, s.zclip, used, shade,
                                      NULL, NULL) == 0 && nb == 0 && !used[2]);
    batches[1].translucent = 0;
    batches[1].tag = 0;
    CHECK(NitroPort_ShadowGroundCards(v, batches, 7, (size_t)n, out, 256, outBatches, 4, &nb, &s, s.zclip, NULL, shade,
                                      NULL, NULL) == 0);
    batches[1].tag = HERO;
    batches[2].tag = 0;
    batches[3].tag = 0;
    CHECK(NitroPort_ShadowGroundCards(v, batches, 7, (size_t)n, out, 256, outBatches, 4, &nb, &s, s.zclip, NULL, shade,
                                      NULL, NULL) == 0);
    NitroPort_ShadowGroundStats(&sprites, &shadows, &laid);
    CHECK(sprites == 2 && shadows == 0 && laid == 0);
}

int main(void)
{
    test_ground_cards();
    test_cards_decals();
    test_roles_reach();
    test_day();
    test_post_shadow();
    test_azimuth();
    test_none();
    test_stable();
    printf("sun shadows: %d checks, %d failures\n", sChecks, sFailures);
    return sFailures != 0;
}
