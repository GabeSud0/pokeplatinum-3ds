/*
 * DS geometry command capture. This interface is independent of Nintendo
 * headers and of the eventual PICA200 renderer. Units are DS 20.12 fixed
 * point for matrices/positions, and DS 15-bit RGB for colours.
 */
#ifndef NITRO_PORT_GX3D_H
#define NITRO_PORT_GX3D_H
#include <stddef.h>
#include <stdint.h>

#define GX3D_WIDTH 256
#define GX3D_HEIGHT 192
#define GX3D_POLYGON_LIMIT 2048
#define GX3D_VERT_LIMIT 6144

typedef struct {
    int32_t clip[4];            /* clip-space 20.12: x,y,z,w */
    int32_t x, y;               /* screen-space 16.16 */
    int32_t depth;              /* normalised depth 0..0x7fff */
    int16_t s, t;               /* texture 12.4 coordinates */
    uint16_t rgb;               /* DS RGB555 */
} NitroGX3DVertex;

typedef struct {
    NitroGX3DVertex v[3];
    uint32_t polygon_attr, texture_param, palette_base;
} NitroGX3DTriangle;

typedef struct {
    uint32_t count, dropped;
    NitroGX3DTriangle triangles[GX3D_POLYGON_LIMIT];
    uint32_t clear_color, clear_depth;
} NitroGX3DScene;

/* One word in the packed 0x04000400 FIFO (as sent by DMA/display lists). */
void NitroGX3D_Fifo(uint32_t word);
/* One word written to an immediate command register (0x440 + 4*(op-0x10)).
 * A command with multiple parameters is submitted once per parameter. */
void NitroGX3D_Direct(uint8_t op, uint32_t word);
void NitroGX3D_Reset(void);
const NitroGX3DScene *NitroGX3D_FrontScene(void);
uint32_t NitroGX3D_Status(void);   /* status bits, excluding FIFO IRQ mode */
uint32_t NitroGX3D_PosResult(unsigned index);
uint32_t NitroGX3D_VecResult(void);

/* PC reference renderer. Writes RGB555 pixels and depth values; transparent
 * pixels have bit15 clear, other colours have bit15 set (2D BG convention).
 * The optional texture/palette views are byte arrays, in DS VRAM layout. */
void NitroGX3D_Rasterize(const NitroGX3DScene *scene, uint16_t *pixels,
                        const uint8_t *texture, size_t texture_bytes,
                        const uint8_t *palette, size_t palette_bytes);
#endif
