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

/* PC only, for checking the GPU path without a 3DS: draws the scene in
 * software the way port_gx3d_pica.c asks the PICA to (port_gx3d_gpusim.c),
 * into RGB555|coverage pixels like the reference rasterizer's. */
void NitroPort_Gx3dSimulateGpu(const NitroGx3dScene *scene, uint16_t *out555);
/* The remaster's look in the simulation, as port_gx3d_pica.c draws it
 * (remaster.txt: shadows, grade, sun; NitroPort_PicaLook): sun shadows,
 * the colour grade, the sun fixed (degrees) or by the clock. */
void NitroPort_Gx3dSimulateLook(int shadows, int grade, int fixedSun, float elevation, float azimuth);
#endif
