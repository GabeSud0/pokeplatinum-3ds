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
    /* the DS's clip z from (x, y, w, 1) (the scene's projection's z column
     * through back): a new vertex's depth, before the port's remap */
    float zclip[4];
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

struct NitroGx3dGpuVertex;
struct NitroGx3dBatch;
/* The same for the sun shadows laid on the ground alone
 * (NitroPort_ShadowGroundCards), from the marked polygons' batches
 * (NitroPort_Gx3dBuildTaggedBatches) instead of the whole scene: the
 * ground's normal is the round shadows' (they lie on it: every batch with
 * a round shadow's mark, its triangles' normals weighed by their area), so
 * none of the scene's thousands of vertices is gone through (the whole
 * setup took most of the 5 ms a scene the shadows cost). Only up, light,
 * back and zclip are set. 0 with no round shadow among them (the whole
 * setup then), or as above. */
int NitroPort_ShadowGroundSetup(const struct NitroGx3dGpuVertex *verts, const struct NitroGx3dBatch *batches,
                                size_t batchCount, const int32_t projection[16], unsigned screenWidth,
                                unsigned screenHeight, const NitroShadowSun *sun, NitroShadowSetup *out);

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

/* The sun shadows (the options page's SUN SHADOWS: ON), characters' for
 * now: the player, the follower, the people. The game marks its polygons
 * (NitroPort_Gx3dTag; NitroGx3dBatch.tag): each field sprite with a mark of
 * its own (even), and a character's round shadow (ov5_021F134C.c, drawn as
 * ever) with its sprite's mark | 1. A round shadow whose sprite is in the
 * scene (an opaque batch of up to 24 vertices with that mark) gets a sun
 * shadow laid on the ground from it, and the round one goes (the
 * caller's: roundUsed). Nothing is guessed from shapes or places. A sprite
 * with no round shadow (a tree, a sign, the grass) gets none; with no sun
 * (the caller's) the round ones stay.
 *
 * The shadow, as the Emerald remaster's lie: the picture on the ground,
 * starting at its feet, its rows slid along the ground the way the sun's
 * light goes (the setup's sun: by the clock, NitroPort_ShadowSunAt; to
 * the left in the morning, down the screen about midday, to the right in
 * the evening), the sprite's height times the sun's cotangent
 * (x NITRO_SHADOW_GROUND_LENGTH) long: short at midday, 0.6 of the
 * sprite; 1.5 times it early and late. Never nearer the sprite's own line
 * than NITRO_SHADOW_GROUND_SIDE (it would be a sliver), never up the
 * screen. Across the sprite stays across (about its middle), so a sprite
 * seen from the back or the front casts the same shape. Drawn once: one
 * copy, its edge as soft as the texture's own smooth sampling makes it
 * (nine copies a step apart were a blur, but nine multiplies, each
 * rounded to the upper screen's 5-6-5 bits: red and blue by 8/255 a step,
 * green by 4, and the shadow came out purple here and blue there), and
 * fading to NITRO_SHADOW_GROUND_TIP of it at the head. The shade: the one
 * given, its darkening NITRO_SHADOW_GROUND_DARK
 * times as much. Pulled towards the camera along its ray (where it is on the
 * screen unchanged) by NITRO_SHADOW_GROUND_PULL of its distance: in front
 * of the ground, behind the sprite's own feet.
 *
 * The batches given are the marked polygons' whole triangles
 * (NitroPort_Gx3dBuildTaggedBatches: not clipped at the view's sides, kept
 * beside it), so a character at the screen's edge or just off it casts
 * the same shadow as in its middle.
 *
 * Where the shadow starts: the round shadow's middle (on the ground, under
 * the feet, as the game puts it: through a jump too). Which row of the
 * picture is its feet: footRow (if not NULL) gives the first texel row
 * from tFrom towards tTo (tTo included) with any opaque texel in columns
 * s0..s1 (s1 excluded), or -1 (texels as the DS samples them: repeat and
 * flip as the parameter has them); the clear rows below the feet fall
 * behind the start.
 *
 * depth: the row on (x, y, w, 1) giving a vertex's z as the scene's
 * vertices have theirs (NitroPort_ShadowDepthRow). shade: the full shade
 * (0-1, red, green, blue), into the vertices' colours (with the fade):
 * drawn as mix(white, vertex colour, texture alpha), multiplying what's
 * there. out, capacity vertices; outBatches,
 * maxBatches (each one first counting from first); roundUsed (as many as
 * batches, or NULL): 1 for each round shadow's batch whose character got a
 * sun shadow (the others untouched). Returns the vertices written. verts
 * are only read. */
#define NITRO_SHADOW_GROUND_SIDE 30.0f     /* (degrees, on the ground) */
#define NITRO_SHADOW_GROUND_LENGTH 1.25f    /* (a little longer than the sun alone makes them) */
#define NITRO_SHADOW_GROUND_DARK 1.1f      /* (how much darker than the shade given: its darkening times this) */
#define NITRO_SHADOW_GROUND_TIP 0.35f
#define NITRO_SHADOW_GROUND_PULL 0.004f

/* The last NitroPort_ShadowGroundCards': the marked sprites' batches, the
 * round shadows (marks), and the sun shadows laid. For the log. */
void NitroPort_ShadowGroundStats(unsigned *sprites, unsigned *shadows, unsigned *laid);

/* The props' sun shadows (trees, signs, lamp posts), in the same call:
 * each run of batches with one NITRO_GX3D_TAG_PROP mark is a prop (the
 * game marks the small standing ones). Its shadow is its picture as the
 * camera has it, laid on the ground as a sprite's: from the middle of its
 * lowest vertices (within 1.5 of the lowest: where it stands), each vertex
 * across by its place across the screen from there and along the shadows'
 * way by its height up the screen from there, times the same stretch;
 * fading to the tip at its top. Its opaque, textured batches only; none
 * for a prop standing under NITRO_SHADOW_PROP_MIN above its ground (lying
 * flat) or with more than 512 triangles.
 *
 * Drawn once too, in batches of their own after the characters', their
 * tag NITRO_SHADOW_ONCE (the characters' batches' tag is 0): the renderer
 * draws those darkening each pixel once (a prop's layers overlap, and
 * props each other: the shade, however many layers), where a texel is
 * more than half opaque.
 * The last call's props and how many got a shadow: */
#define NITRO_SHADOW_ONCE 1u
/* The characters standing in the land's trees' shadows (2026-11-10, from
 * a preview: "people darken in shade"): a sprite stays as bright as in
 * the sun on darkened ground otherwise. Each marked sprite's batch (an
 * even mark that isn't a prop's or a draw-only one) whose foot (the
 * middle of its lowest edge on the screen, NITRO_SHADOW_FOOT_UP of its
 * height up) is inside a triangle of a NITRO_GX3D_TAG_SHADE batch has its
 * vertices' colours darkened: by `take` (what a full shadow takes from
 * each channel, 0 to 1) times the triangle's grey there (the fade; the
 * darkest of the triangles it's in), no less than NITRO_SHADOW_FOOT_MIN
 * of it. A tree's layers' (NITRO_GX3D_TAG_SHADE with a texture) by
 * their outline there instead: maskAt (if not NULL) gives the softened
 * outline's alpha (0 to 1, NitroPort_ShadowTreeMask's) of the batch's
 * texture at a texture coordinate; under NITRO_SHADOW_FOOT_IN of it, not in
 * that shadow. (First the corners alone: a layer's quad reaches well past
 * its outline, and a character beside a tree's shadow was darkened, on a
 * 3DS 2026-11-11: "Im close to a tree shadow, not under it and I still
 * get darkened".) Returns how many sprites were darkened. */
#define NITRO_SHADOW_FOOT_IN 0.3f
typedef float (*NitroShadowMaskAt)(void *ctx, uint32_t textureParam, uint32_t paletteBase, float u, float v);
/* (how dark a tree's shadow is, of the characters' darkening at their feet) */
#define NITRO_SHADOW_TREE_BLOB 0.85f
/* A tree's shadow's outline from its texture (2026-11-11, chosen from a
 * preview, "softness medium": the tree's own picture laid on the ground,
 * its leaves' pixel steps and thin tips rounded off, the edge soft): the
 * texels' alpha (rgba8, rows of `width`, alpha in the low byte) blurred
 * (a Gaussian of NITRO_SHADOW_TREE_SOFT texels, about a unit of the
 * trees: NITRO_SHADOW_TREE_REACH either way), then 0 below
 * NITRO_SHADOW_TREE_FROM, all of it above NITRO_SHADOW_TREE_TO, smoothly
 * between: a blur cut at its middle rounds the outline (a spike or a gap
 * a texel wide goes) and the cut's width is the soft edge. Beyond the
 * texture's edges as the GPU samples it: wrapS, wrapT one of
 * NITRO_SHADOW_WRAP_*. Out (the same size): white, the alpha the mask.
 * 0 if there's no memory for it. */
#define NITRO_SHADOW_TREE_SOFT 1.4f
#define NITRO_SHADOW_TREE_REACH 4
#define NITRO_SHADOW_TREE_FROM 0.22f
#define NITRO_SHADOW_TREE_TO 0.78f
#define NITRO_SHADOW_WRAP_CLAMP 0
#define NITRO_SHADOW_WRAP_REPEAT 1
#define NITRO_SHADOW_WRAP_MIRROR 2
int NitroPort_ShadowTreeMask(const uint32_t *rgba8, unsigned width, unsigned height, int wrapS, int wrapT,
                             uint32_t *out);
#define NITRO_SHADOW_FOOT_UP 0.08f
#define NITRO_SHADOW_FOOT_MIN 0.5f
unsigned NitroPort_ShadowDarkenSprites(struct NitroGx3dGpuVertex *verts, const struct NitroGx3dBatch *batches,
                                       size_t count, const float take[3], NitroShadowMaskAt maskAt, void *maskCtx);
#define NITRO_SHADOW_PROP_MIN 5.0f
void NitroPort_ShadowPropStats(unsigned *props, unsigned *laid);

/* The sun now, from the renderer to the game: the land's trees' shadows
 * are polygons the game sends (NitroPort_Gx3dTreeLists), made for the way
 * the light goes. NitroPort_SetSunNow: each scene the renderer looks at
 * the clock (strength under 0.02: no shadows now). NitroPort_SunWayNow: 0
 * with no sun set or the sun shadows off; else way[0], way[1] the way the
 * light goes along the ground (to the screen's right, and towards the
 * camera: never under sin NITRO_SHADOW_GROUND_SIDE) and way[2] a shadow's
 * length for a height of 1 (the sun's cotangent times
 * NITRO_SHADOW_GROUND_LENGTH), fx32 in steps of 1/256: the same way and
 * length as NitroPort_ShadowGroundCards lays the characters' along. */
void NitroPort_SetSunNow(const NitroShadowSun *sun, float strength);
int NitroPort_SunWayNow(int32_t way[3]);

#define NITRO_SHADOW_GROUND_CARD_MAX 96.0f   /* (6 tiles: past it, not a sprite) */
typedef int (*NitroShadowFootRow)(void *ctx, uint32_t textureParam, uint32_t paletteBase, int s0, int s1,
                                  int tFrom, int tTo);
/* The row on (x, y, w, 1) giving a new vertex's z as the GPU path's
 * vertices have theirs after the depth remap, for NitroPort_ShadowGroundCards.
 * The path's z isn't the DS's clip z: Z-buffer scenes, the DS's depth
 * (clip z / w + 1) / 2 (port_gx3d.c: (nz + 1) * 8388607.5 of 16777215)
 * times -w; W-buffer, -(w 20.12 / 16777215). The GPU path's remap
 * (normalize_depth) then gives z' = -(near + (d - lo) k) w, or -(d - lo) k,
 * with depthA = lo - near / k (W-buffer: lo) and depthK = k. */
void NitroPort_ShadowDepthRow(const NitroShadowSetup *setup, int wbuffer, float depthA, float depthK,
                              float out[4]);
size_t NitroPort_ShadowGroundCards(const struct NitroGx3dGpuVertex *verts, const struct NitroGx3dBatch *batches,
                                   size_t batchCount, size_t first, struct NitroGx3dGpuVertex *out,
                                   size_t capacity, struct NitroGx3dBatch *outBatches, size_t maxBatches,
                                   size_t *outBatchCount, const NitroShadowSetup *setup, const float depth[4],
                                   uint8_t *roundUsed, const float shade[3], NitroShadowFootRow footRow,
                                   void *footCtx);

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
/* Its two halves (NitroPort_ShadowDecal is the one and the other): the
 * triangle see-through and lying flat, cheap; then its texture (or colour)
 * dark, which needs the texture decoded. Decode only for a triangle that
 * passes the first; the second is the texture's alone, the same for every
 * batch with that texture and palette. */
int NitroPort_ShadowDecalShape(const float *tri, size_t stride, unsigned polygonAlpha, int translucentFormat,
                               const NitroShadowSetup *setup);
int NitroPort_ShadowDecalDark(const uint32_t *texels, size_t count, uint16_t color);

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
/* Whether the shadow map reaches past the view's sides (the GPU path sets
 * it with the full shadow map, SUN SHADOWS: FULL): the geometry's box
 * tests then pass objects just outside the view, for their shadows. Not
 * with the sprites' shadows alone: they need nothing off the screen. */
void NitroPort_SetShadowReach(int on);
int NitroPort_ShadowReachOn(void);
/* Whether the sun shadows (SUN SHADOWS: ON) are being drawn: the game's
 * marks on its polygons are only taken then (NitroPort_Gx3dTag: with them
 * off nothing is marked, kept or tested any differently). Set by the
 * renderer; read on the game's thread. */
void NitroPort_SetGroundShadows(int on);
int NitroPort_GroundShadowsOn(void);

#ifdef __cplusplus
}
#endif

#endif
