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
 * now: the player, the follower, the people. Each stands on the game's
 * round shadow (drawn as ever: ov5_021F134C.c); a card facing the camera
 * standing on one (a sprite) gets a shadow laid on the ground from it, and
 * the round one goes (the caller's: markerUsed). Without one (a tree, a
 * sign; or no sun: the round ones stay) nothing.
 *
 * The shadow, as the Emerald remaster's lie: the picture on the ground,
 * starting at its feet, its rows slid towards the screen's lower right
 * (NITRO_SHADOW_GROUND_ANGLE below the horizontal, whichever way the
 * camera looks), NITRO_SHADOW_GROUND_LENGTH times the sprite's height
 * long on the screen; across the sprite stays across, so a sprite seen
 * from the back or the front casts the same shape. Soft: drawn
 * NITRO_SHADOW_GROUND_TAPS times, each a little apart on the ground and
 * each a TAPS-th root of the shade (where they all meet, the shade; the
 * edges between), and fading to NITRO_SHADOW_GROUND_TIP of it at the
 * head. Pulled towards the camera along its ray (where it is on the
 * screen unchanged) by NITRO_SHADOW_GROUND_PULL of its distance: in front
 * of the ground at any distance.
 *
 * Where the shadow starts: the marker's middle (on the ground, under the
 * feet). Which row of the picture is its feet: footRow (if not NULL) gives
 * the first texel row from tFrom towards tTo (tTo included) with any
 * opaque texel in columns s0..s1 (s1 excluded), or -1 (texels as the DS
 * samples them: repeat and flip as the parameter has them); the clear rows
 * below the feet fall behind the start.
 *
 * Cards: opaque batches' (only), triangles in a row sharing corners, up to
 * NITRO_SHADOW_GROUND_CARD_MAX long and wide, with a texture that has see-
 * through texels (a palette's colour 0 transparent, or direct colour).
 * A card and a marker go together if each is the other's nearest (the
 * card's foot's middle to the marker's: within NITRO_SHADOW_GROUND_REACH,
 * or 0.4 of the card's width if more; a taller card nearer by a tenth of
 * its height, so a character beats the grass at its feet).
 *
 * depth: the row on (x, y, w, 1) giving a vertex's z as the scene's
 * vertices have theirs (NitroPort_ShadowDepthRow). shade: the full shade
 * (0-1, red, green, blue), into the copies' vertex colours (each tap's,
 * with the fade): drawn as mix(white, vertex colour, texture alpha),
 * multiplying what's there. out, capacity vertices; outBatches,
 * maxBatches (each one first counting from first); markerUsed (as many as
 * markers, or NULL): 1 for each marker given a shadow (the others
 * untouched). Returns the vertices written. verts are only read. */
#define NITRO_SHADOW_GROUND_ANGLE 28.0f    /* (degrees: the Emerald remaster's Littleroot) */
#define NITRO_SHADOW_GROUND_LENGTH 1.3f
#define NITRO_SHADOW_GROUND_TAPS 5
#define NITRO_SHADOW_GROUND_BLUR 0.06f     /* (each tap's distance: the sprite's height times it, 0.75-3) */
#define NITRO_SHADOW_GROUND_TIP 0.35f
#define NITRO_SHADOW_GROUND_PULL 0.01f
#define NITRO_SHADOW_GROUND_REACH 10.0f    /* (5/8 of a tile) */
#define NITRO_SHADOW_GROUND_CARDS 128      /* (cards looked at a scene, at most) */

/* The last NitroPort_ShadowGroundCards' cards looked at, and how near the
 * nearest of them was to a marker (its foot's middle to the marker's
 * middle; -1 if no card or no marker). For the log. */
void NitroPort_ShadowGroundStats(unsigned *cards, float *nearest);

/* The game's round shadows: the batches flagged (flagged[b] nonzero: the
 * caller's NitroPort_ShadowDecal verdicts) split into markers, each a run
 * of their triangles sharing corners: its vertices' middle (camera space)
 * and how far its farthest is. More than NITRO_SHADOW_MARKER_TRIANGLES
 * triangles, or wider than NITRO_SHADOW_MARKER_RADIUS: a painted patch
 * (beside a building), not a marker. Returns the markers (out, max). */
#define NITRO_SHADOW_MARKER_TRIANGLES 8
#define NITRO_SHADOW_MARKER_RADIUS 12.0f   /* (3/4 of a tile) */
typedef struct NitroShadowMarker {
    float ground[3];          /* (camera space) */
    float radius;
    uint32_t first, count;    /* its vertices in verts */
} NitroShadowMarker;
size_t NitroPort_ShadowMarkers(const struct NitroGx3dGpuVertex *verts, const struct NitroGx3dBatch *batches,
                               size_t batchCount, const uint8_t *flagged, const NitroShadowSetup *setup,
                               NitroShadowMarker *out, size_t max);
#define NITRO_SHADOW_GROUND_CARD_MAX 96.0f   /* (6 tiles: past it, not a sprite) */
typedef int (*NitroShadowFootRow)(void *ctx, uint32_t textureParam, uint32_t paletteBase, int s0, int s1,
                                  int tFrom, int tTo);
/* Whether a texture (TEXIMAGE_PARAM) has see-through texels: a sprite's. */
int NitroPort_ShadowSpriteTexture(uint32_t textureParam);
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
                                   const NitroShadowMarker *markers, size_t markerCount, uint8_t *markerUsed,
                                   const float shade[3], NitroShadowFootRow footRow, void *footCtx);

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

#ifdef __cplusplus
}
#endif

#endif
