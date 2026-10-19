/* Host-testable Stage 2 vertex construction, no libctru dependencies. */
#ifndef NITRO_PORT_GX3D_VERTEX_H
#define NITRO_PORT_GX3D_VERTEX_H
#include <nitro_port_gx3d.h>
#include <stddef.h>
#include <stdint.h>
#define NITRO_GX3D_PICA_MAX_VERTICES (3u * NITRO_GX3D_MAX_TRIANGLES)
typedef struct NitroGx3dGpuVertex {
    float position[4];          /* clip-space x,y,z,w; perspective preserved */
    uint8_t color[4];           /* RGBA8 from RGB555, alpha is opaque for PR1 */
    float uv[2];                /* normalized by original DS texture size */
} NitroGx3dGpuVertex;
/* Only Stage 2 PR1's untextured, fully-opaque triangles. Polygon textures,
 * transparency, sorting and combiner modes belong to subsequent PRs.
 * Returns number of GPU vertices (always divisible by 3), capped by capacity.
 * A maximum Stage 1 scene contains 4096 post-clipping triangles, requiring
 * 12288 triangle-list vertices. A smaller caller-supplied capacity is safe;
 * the count-dropped helper reports how many opaque triangles did not fit. */
/* 0 near .. 1 far; the PICA backend uses GPU_LESS with depth cleared
 * to maximum, so smaller returned values must win the depth test.
 * W-buffer uses the same documented monotonic approximation as PR1. */
float NitroPort_Gx3dDepthValue(const NitroGx3dVertex *vertex,
                              uint32_t swap_param);
/* The clip-space z to give the GPU for a vertex (see port_gx3d_vertex.c). */
float NitroPort_Gx3dClipZ(const NitroGx3dVertex *vertex, uint32_t swap_param);
size_t NitroPort_Gx3dCountDroppedTriangles(const NitroGx3dScene *scene,
                                            size_t capacity);
size_t NitroPort_Gx3dBuildOpaqueVertices(const NitroGx3dScene *scene,
                                         NitroGx3dGpuVertex *out,
                                         size_t capacity);

/* Stage 2 PR2: every visible triangle, grouped into draw batches that share
 * GPU state. As in the PC reference rasterizer, opaque triangles come first
 * and translucent ones (polygon alpha 1-30, or a texture format that can
 * have see-through texels) after them, each pass in submission order;
 * consecutive triangles with the same state share a batch. Alpha 0
 * (wireframe) polygons are skipped, as the reference does for now.
 * Vertex alpha carries the polygon alpha. */
typedef struct NitroGx3dBatch {
    uint32_t first, count;        /* vertices */
    uint32_t texture_param;       /* 0 when untextured */
    uint32_t palette_base;        /* 0 when untextured or direct colour */
    uint32_t polygon_attr;        /* mode (bits 4-5), depth bits 11/14, fog (15), alpha, ID (24-29) */
    uint8_t translucent;
} NitroGx3dBatch;

/* A triangle needs the translucent pass. */
int NitroPort_Gx3dTriangleTranslucent(const NitroGx3dTriangle *tri);
/* The scene's triangles in the DS's drawing order (indices into
 * scene->triangles); scratch: as many entries, for sorting. */
size_t NitroPort_Gx3dDrawOrder(const NitroGx3dScene *scene, uint16_t *order, uint16_t *scratch);
/* How far to move a screen-space triangle's corners (pixels) so its edges
 * sit a tenth of a pixel further out (see port_gx3d_vertex.c). */
void NitroPort_Gx3dExpandTriangle(const float sx[3], const float sy[3], float dx[3], float dy[3]);

/* Returns the number of vertices written; *batch_count gets the batches.
 * Triangles that don't fit in either array are counted in *dropped. */
size_t NitroPort_Gx3dBuildBatches(const NitroGx3dScene *scene,
                                  NitroGx3dGpuVertex *out, size_t capacity,
                                  NitroGx3dBatch *batches, size_t max_batches,
                                  size_t *batch_count, unsigned *dropped);

/* The scene's shadow-only triangles (shadow_triangles) for the sun
 * shadows' map, as NitroPort_Gx3dBuildBatches has the opaque ones: into
 * out (capacity vertices) and batches (max_batches), batches' first
 * counting from first (where out is in the vertex buffer). Their vertices
 * aren't to be drawn on the screen. Returns the vertices written. */
size_t NitroPort_Gx3dBuildShadowBatches(const NitroGx3dScene *scene, size_t first,
                                        NitroGx3dGpuVertex *out, size_t capacity,
                                        NitroGx3dBatch *batches, size_t max_batches,
                                        size_t *batch_count);

/* Edge marking made cheaper (port_gx3d_prune.c). The GPU's outline
 * passes (port_gx3d_pica.c, mark_edges) draw the scene's opaque polygons 8
 * times an eye; most of them can't get an outline anywhere: a polygon gets
 * one only where a neighbouring pixel shows another polygon ID, or the
 * rear plane (nothing drawn there: also past the edges of the DS's screen,
 * where the scene stops). These are the triangles each pass needs, as
 * index lists:
 *  - D: those that may get an outline anywhere (pass B: the outlines
 *    drawn);
 *  - A: D and those next to one (pass A: the scene moved a pixel);
 *  - four zones along the borders (left, right, top, bottom, a few pixels
 *    wide; see NitroGx3dPruneResult.zone) for the many triangles that touch
 *    the DS's screen's edges, where the rear plane shows past them: ZONE_B
 *    those D leaves out that may get an outline there, ZONE_A those A leaves
 *    out that touch the zone, both drawn with the scissor test on the zone
 *    (NitroPort_Gx3dPruneScissor).
 * The same pixels come out as with every triangle drawn in every pass;
 * tests/test_gx3d.c checks that against a model of the passes. */
enum {
    NITRO_GX3D_PRUNE_A,
    NITRO_GX3D_PRUNE_D,
    NITRO_GX3D_PRUNE_ZONE_A,                                  /* + zone */
    NITRO_GX3D_PRUNE_ZONE_B = NITRO_GX3D_PRUNE_ZONE_A + 4,   /* + zone */
    NITRO_GX3D_PRUNE_LISTS = NITRO_GX3D_PRUNE_ZONE_B + 4
};
/* How a batch takes part in the outline passes. */
enum {
    NITRO_GX3D_PRUNE_OUT,     /* not at all (translucent, alpha under 31) */
    NITRO_GX3D_PRUNE_SHAPE,   /* drawn, but may leave pixels out (the alpha test) */
    NITRO_GX3D_PRUNE_SOLID    /* drawn, every pixel it covers */
};
typedef struct NitroGx3dPruneView {
    float x0, y0, x1, y1;   /* where the DS's screen goes, in the screen's pixels */
    int width, height;      /* the screen's size */
    int eyes;               /* 1, or 2 (the lists serve both eyes) */
    float stereo[2];        /* each eye's shift, as the vertex program has it */
    float convergence;
    unsigned clear_id;      /* the rear plane's polygon ID */
} NitroGx3dPruneView;
typedef struct NitroGx3dPruneList {
    uint32_t offset[NITRO_GX3D_PRUNE_LISTS];   /* into the indices, 8-aligned */
    uint16_t count[NITRO_GX3D_PRUNE_LISTS];    /* indices (3 a triangle) */
} NitroGx3dPruneList;
typedef struct NitroGx3dPruneResult {
    int zone[4][4];         /* each zone: x0, y0, x1, y1 (screen pixels, x1 and y1 past it); empty
                             * when x0 >= x1 or y0 >= y1 */
    size_t indices;         /* used */
    unsigned triangles, a, d, zone_a, zone_b;   /* (counts, for the log) */
} NitroGx3dPruneResult;
/* vertices: the scene's (NitroPort_Gx3dBuildBatches); roles: a
 * NITRO_GX3D_PRUNE_ role a batch; lists: one a batch, its index lists into
 * indices (vertex numbers, as the vertex buffer has them; capacity of
 * them). Returns 1, or 0 when they don't fit or memory runs out (draw
 * everything then). */
int NitroPort_Gx3dPruneEdges(const NitroGx3dGpuVertex *vertices, const NitroGx3dBatch *batches,
                             size_t batch_count, const uint8_t *roles, const NitroGx3dPruneView *view,
                             uint16_t *indices, size_t capacity, NitroGx3dPruneList *lists,
                             NitroGx3dPruneResult *result);
/* The working memory it keeps between scenes, freed. */
void NitroPort_Gx3dPruneFree(void);
/* A zone (screen pixels) as C3D_SetScissor's left, top, right, bottom on
 * the screen's render target (240 x width, turned a quarter turn; see
 * NitroPort_PicaViewMatrix). */
void NitroPort_Gx3dPruneScissor(const int zone[4], int width, int height, unsigned out[4]);

/* PC only, for checking the GPU path without a 3DS: draws the scene in
 * software the way port_gx3d_pica.c asks the PICA to (port_gx3d_gpusim.c),
 * into RGB555|coverage pixels like the reference rasterizer's. */
void NitroPort_Gx3dSimulateGpu(const NitroGx3dScene *scene, uint16_t *out555);
/* The remaster's look in the simulation, as port_gx3d_pica.c draws it
 * (remaster.txt: shadows, grade, sun; NitroPort_PicaLook): sun shadows,
 * the colour grade, the sun fixed (degrees) or by the clock. */
void NitroPort_Gx3dSimulateLook(int shadows, int grade, int fixedSun, float elevation, float azimuth);
#endif
