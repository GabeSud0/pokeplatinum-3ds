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
size_t NitroPort_Gx3dCountDroppedTriangles(const NitroGx3dScene *scene,
                                            size_t capacity);
size_t NitroPort_Gx3dBuildOpaqueVertices(const NitroGx3dScene *scene,
                                         NitroGx3dGpuVertex *out,
                                         size_t capacity);
#endif
