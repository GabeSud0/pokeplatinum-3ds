/*
 * Sun shadows for the 3D (remaster.txt: shadows on): where the sun is for a
 * scene, and the matrices the GPU needs for a shadow map. Plain C on the
 * scene's vertices, shared by the 3DS program and the PC tests.
 *
 * The scene's vertices are the DS's clip space (as the GPU path has them:
 * x and y in -w..w, x scaled for a wider view). With the scene's projection
 * (NitroGx3dScene.projection) they go back to the camera's own space, where
 * the sun is put: the ground is found as the plane most of the scene lies
 * in (cards facing the camera, a character or a tree, left out), and the
 * sun shines down onto it at an elevation, from an azimuth along the
 * ground (0: from the screen's left; 90: from the screen's top). Fixed to
 * the camera, so it doesn't move as the camera follows the player.
 *
 * The shadow map looks along the sun's direction over everything in the
 * scene: each vertex's place in it (u, v, 0..1) and its distance along the
 * sun's direction (0 nearest the sun .. 1), both as matrices of 4 rows
 * applied to (x, y, w, 1) of a vertex (its clip z isn't used: the
 * camera's distance is w).
 */
#ifndef NITRO_PORT_SHADOW_H
#define NITRO_PORT_SHADOW_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* What the light is in the shadow (each channel; the sun's 1), slightly
 * blue as a clear sky's shade: 0x94 0x99 0xB3 of 255. */
#define NITRO_SHADOW_SHADE_R 0x94u
#define NITRO_SHADOW_SHADE_G 0x99u
#define NITRO_SHADOW_SHADE_B 0xB3u
/* How far the shadow map's lookup takes the setup's bias (of the ground's
 * own slope): half again, for the tilted walls. */
#define NITRO_SHADOW_BIAS_SCALE 1.5f

typedef struct NitroShadowSun {
    float elevation;   /* degrees above the ground */
    float azimuth;     /* degrees: 0 the sun at the screen's left, 90 at its top */
} NitroShadowSun;

typedef struct NitroShadowSetup {
    /* The shadow map pass: rows giving clip x, y (-1..1 over the map), z
     * (-distance, -1..0) and w (1). */
    float caster[4][4];
    /* The scene's pass: rows giving the shadow map's u, v (0..1), 0, and
     * the distance (0..1): texture coordinate 0's s, t and w. */
    float receiver[4][4];
    /* How much nearer the sun a texel must be to shadow (in the distance's
     * 0..1): the ground's own slope over a texel or two, so it doesn't
     * shadow itself. */
    float bias;
    /* In the camera's space: the ground's normal, the sun's light's
     * direction (where it goes), and the map's width in it. */
    float up[3], light[3];
    float extent;
    /* the camera's space from (x, y, w, 1): rows for x, y, z */
    float back[3][4];
} NitroShadowSetup;

/* What a triangle does in the sun (NitroPort_ShadowRoles), kept in its
 * vertices' alpha (opaque polygons' is 255 otherwise): */
#define NITRO_SHADOW_CAST_RECEIVE 255u   /* a slope: a roof */
#define NITRO_SHADOW_CAST_ONLY    254u   /* upright: a wall, a character, a tree */
#define NITRO_SHADOW_RECEIVE_ONLY 253u   /* flat: the ground, what lies on it */
#define NITRO_SHADOW_NEITHER      252u   /* a card turned to the sun for its shadow (NitroPort_ShadowCards) */

/* The setup for a scene, from its triangles' vertices (count of them, three
 * a triangle; each x, y, z, w floats, stride bytes apart) and projection
 * (the DS's 4x4, 20.12, rows as the DS keeps them: clip = position x P),
 * the view's width and height (NitroGx3dScene.screen_width, screen_height:
 * 256 and 192, or more: x was scaled by 256 / the width, y by 192 / the
 * height), the sun and the map's size in texels. 0 (no
 * shadows) if the projection isn't a camera's perspective or no ground
 * stands out (a battle, a menu). */
int NitroPort_ShadowSetup(const float *positions, size_t stride, size_t count,
                          const int32_t projection[16], unsigned screenWidth, unsigned screenHeight,
                          const NitroShadowSun *sun, unsigned mapSize, NitroShadowSetup *out);

/* A setup's distances reaching further towards the sun, for triangles that
 * only cast (the scene's shadow_triangles: outside the view, or facing
 * away from it; as for NitroPort_ShadowSetup, three vertices a triangle):
 * any over the map nearer the sun than the scene's nearest would be cut
 * off otherwise. The map's place and size stay as they are (what shadows
 * the view is over it, whatever casts it). */
void NitroPort_ShadowReach(const float *positions, size_t stride, size_t count, NitroShadowSetup *setup);

/* Each opaque triangle's part in the sun shadows, into its vertices'
 * alpha (count vertices, three a triangle, positions and alphas stride
 * bytes apart; only alphas of 255 are changed). Only what faces up
 * receives shadows: the models are made for the camera's one view, and on
 * walls, characters and trees (cards) their own shadows made odd shapes.
 * What lies flat doesn't cast: a doormat or a path's edge just above the
 * ground cast a stray shape beside it; a flat roof isn't missed (its
 * walls' shadows have its outline). */
void NitroPort_ShadowRoles(const float *positions, uint8_t *alphas, size_t stride, size_t count,
                           const NitroShadowSetup *setup);

/* Characters, signs, the trees on routes: cards facing the camera. From the
 * side, the sun sees them edge on: their shadows were slivers, or nothing.
 * Each card (its triangles in a row sharing corners) gets a copy turned
 * about its upright middle to face the sun, for the shadow map only, as
 * NITRO_SHADOW_CAST_ONLY triangles (out, capacity vertices; outBatches,
 * maxBatches, each batch's first counting from first, where out is in the
 * vertex buffer); the card itself becomes NITRO_SHADOW_NEITHER (its alpha
 * in verts). Only opaque batches' (batches, batchCount: the scene's, over
 * verts). Returns the vertices written. After NitroPort_ShadowRoles. */
struct NitroGx3dGpuVertex;
struct NitroGx3dBatch;
size_t NitroPort_ShadowCards(struct NitroGx3dGpuVertex *verts, const struct NitroGx3dBatch *batches,
                             size_t batchCount, size_t first, struct NitroGx3dGpuVertex *out,
                             size_t capacity, struct NitroGx3dBatch *outBatches, size_t maxBatches,
                             size_t *outBatchCount, const NitroShadowSetup *setup);

/* Whether a translucent triangle is one of the game's own painted shadows
 * (lying flat, see-through, dark: the patches under buildings and on the
 * ground beside them), left out with the sun shadows on: the two together
 * were two shadows. tri: its three vertices (x, y, z, w floats, stride
 * bytes apart); polygonAlpha 0-31; texels: its texture decoded (RGBA8,
 * red in the top byte, alpha the bottom; NULL: untextured, colour the
 * vertices' RGB555); count texels. */
int NitroPort_ShadowDecal(const float *tri, size_t stride, unsigned polygonAlpha, int translucentFormat,
                          const uint32_t *texels, size_t count, uint16_t color,
                          const NitroShadowSetup *setup);

/* The look over the day, after Platinum's own times of day (rtc.c):
 * morning 4:00-9:59, day 10:00-16:59, evening 17:00-19:59, night 20:00-3:59. */
enum { NITRO_LOOK_MORNING, NITRO_LOOK_DAY, NITRO_LOOK_EVENING, NITRO_LOOK_NIGHT, NITRO_LOOK_PERIODS };
int NitroPort_LookPeriod(int hour);

/* The sun at a time of day (hour 0-23, minute 0-59), and how strong its
 * shadows are (0 none .. 1). It rises on the screen's right at 5:00 (long
 * shadows to the left), is high at the top left at midday (short shadows
 * to the bottom right) and sets on the left at 20:00 (long shadows to the
 * right); its shadows fade in after sunrise and out before sunset. At night
 * the moon, high at the top left, its shadows a third as strong, fading in
 * after 20:00 and out by 5:00. */
void NitroPort_ShadowSunAt(int hour, int minute, NitroShadowSun *sun, float *strength);

/* A period's colour grade (white balance, saturation, a gamma), for the
 * 3D's textures: table[c] for each of the DS's colours (RGB555), RGBA8
 * with alpha 0 (NitroPort_Gx3dSetGrade). Morning golden, day warm,
 * evening orange, night cool. */
void NitroPort_LookGradeTable(int period, uint32_t table[32768]);

/* Whether the sun shadows are on (the GPU path sets it): the game leaves
 * out its round shadow model under objects then (patch 0027). Any thread. */
void NitroPort_SetSunShadows(int on);
int NitroPort_SunShadows(void);

#ifdef __cplusplus
}
#endif

#endif
