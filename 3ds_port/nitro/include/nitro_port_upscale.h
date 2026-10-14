/*
 * Pixel-art upscaling and PICA texture tiling, for the 3DS's compositor
 * (the game's 2D pictures are drawn by the GPU at the screen's size). Plain
 * C on arrays: shared by the 3DS program and the PC tests.
 *
 * The PICA200 keeps a texture in 8x8 tiles, row by row, each tile's 64
 * texels in Morton (Z) order: texel (x, y) of a tile is at
 *   (x & 1) | (y & 1) << 1 | (x & 2) << 1 | (y & 2) << 2 | (x & 4) << 2 | (y & 4) << 3.
 * Row 0 of the picture goes into the first row of tiles (texture coordinate
 * v = 0 at the picture's top, when sampled). Textures are powers of two
 * wide (dstWidth): a picture narrower than that fills each tile row's
 * first tiles; shorter: the first tile rows.
 */
#ifndef NITRO_PORT_UPSCALE_H
#define NITRO_PORT_UPSCALE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Where texel (x, y) of a tiled texture dstWidth texels wide is. */
static inline unsigned NitroPort_TiledIndex(unsigned x, unsigned y, unsigned dstWidth)
{
    return ((y >> 3) * (dstWidth >> 3) + (x >> 3)) * 64u +
           ((x & 1u) | ((y & 1u) << 1) | ((x & 2u) << 1) | ((y & 2u) << 2) | ((x & 4u) << 2) |
            ((y & 4u) << 3));
}

/* A w x h picture (w, h multiples of 8) into a tiled texture, as it is. */
void NitroPort_Tile16(const uint16_t *src, int w, int h, uint16_t *dst, int dstWidth);
void NitroPort_Tile32(const uint32_t *src, int w, int h, uint32_t *dst, int dstWidth);

/* Scale2x (EPX): each pixel becomes 2x2, and where a pixel sits on a
 * diagonal edge between two colours the corners take the neighbours'
 * colour, so slopes come out as slopes rather than steps:
 *   top left     = up == left  && up != right && left != down  ? up    : it
 *   top right    = up == right && up != left  && right != down ? right : it
 *   bottom left  = down == left  && down != right && left != up  ? left  : it
 *   bottom right = down == right && down != left  && right != up ? right : it
 * (texels compared whole, colour and alpha together; beyond the picture's
 * edge, the edge pixel). The 2w x 2h result goes into a tiled texture
 * (w, h multiples of 4). */
void NitroPort_Scale2xTiled16(const uint16_t *src, int w, int h, uint16_t *dst, int dstWidth);
void NitroPort_Scale2xTiled32(const uint32_t *src, int w, int h, uint32_t *dst, int dstWidth);

#ifdef __cplusplus
}
#endif
#endif
