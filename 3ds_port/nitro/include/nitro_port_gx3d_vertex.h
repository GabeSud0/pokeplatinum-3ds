/* Host-testable Stage 2 vertex construction, no libctru dependencies. */
#ifndef NITRO_PORT_GX3D_VERTEX_H
#define NITRO_PORT_GX3D_VERTEX_H
#include <nitro_port_gx3d.h>
#include <stddef.h>
#include <stdint.h>
#define NITRO_GX3D_PICA_MAX_VERTICES 6144u
typedef struct NitroGx3dGpuVertex {
    float position[4];          /* clip-space x,y,z,w; perspective preserved */
    uint8_t color[4];           /* RGBA8 from RGB555, alpha is opaque for PR1 */
    float uv[2];                /* normalized by original DS texture size */
} NitroGx3dGpuVertex;
/* Only Stage 2 PR1's untextured, fully-opaque triangles. Polygon textures,
 * transparency, sorting and combiner modes belong to subsequent PRs.
 * Returns number of GPU vertices (always divisible by 3), capped by capacity.
 * The 6144-vertex buffer can hold fewer vertices than 4096 clipped triangles.
 * Excess triangles are deliberately skipped rather than overflowing. */
size_t NitroPort_Gx3dBuildOpaqueVertices(const NitroGx3dScene *scene,
                                         NitroGx3dGpuVertex *out,
                                         size_t capacity);
#endif
