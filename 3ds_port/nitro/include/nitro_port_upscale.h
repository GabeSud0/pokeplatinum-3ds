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

/* Each pixel doubled (2x2 texels the same), into a tiled texture: the
 * pixels as they are, ready for the GPU to scale the rest of the way with
 * bilinear filtering (sharp: only the pixels' borders blend, a texel
 * wide). Then the column right of the picture and the row below it repeat
 * its last ones (so the filtering at the edges doesn't take in what's
 * beyond; a picture as wide as the texture has none: the GPU clamps there).
 * w and h multiples of 4; the texture at least 2 h + 8 rows. */
void NitroPort_Double2xTiled16(const uint16_t *src, int w, int h, uint16_t *dst, int dstWidth);
void NitroPort_Double2xTiled32(const uint32_t *src, int w, int h, uint32_t *dst, int dstWidth);
/* NitroPort_Double2xTiled16 of the DS's colours (bits 0-4 red, 5-9 green,
 * 10-14 blue; bit 15 ignored) turned into texels by table (32768 of them),
 * in one pass: no picture in between, the four texels of a pixel written
 * at once (dst 8-byte aligned). */
void NitroPort_Double2xTiled16Table(const uint16_t *src, int w, int h, uint16_t *dst, int dstWidth,
                                    const uint16_t *table);
/* The same for rows y0 .. y1 of the picture only (multiples of 4, y1 at
 * most h): their texels and the column right of them, and the row below
 * the picture when y1 is h. The rest of the texture is left as it is: a
 * picture whose other rows didn't change since the texture was made comes
 * out as the whole of it would. */
void NitroPort_Double2xTiled16Rows(const uint16_t *src, int w, int h, int y0, int y1, uint16_t *dst,
                                   int dstWidth);
void NitroPort_Double2xTiled32Rows(const uint32_t *src, int w, int h, int y0, int y1, uint32_t *dst,
                                   int dstWidth);
void NitroPort_Double2xTiled16TableRows(const uint16_t *src, int w, int h, int y0, int y1, uint16_t *dst,
                                        int dstWidth, const uint16_t *table);

/* ------------------------------------------------- publishing what changed
 *
 * A picture of up to NITRO_PUBLISH_MAX_W x NITRO_PUBLISH_H, or the DS's
 * width and up to NITRO_PUBLISH_MAX_H rows (the remaster's lower screen as
 * a tall panel) (the DS's 2D layers: an "under" picture in the DS's
 * colours, and an "over" one with its alpha 0-16) published again and
 * again into textures, each pixel
 * doubled (NitroPort_Double2xTiled*), into one of several sets of textures
 * in turn: only the bands of 4 rows changed since a set was last written
 * are done again. A shadow per source of pictures keeps its last picture
 * and, for each band, the publish it last changed in; a target per set of
 * textures, what they hold. */
#define NITRO_PUBLISH_MAX_W 320
#define NITRO_PUBLISH_H 192
#define NITRO_PUBLISH_MAX_H 248
/* (the larger of 320 x 192 and 256 x 248) */
#define NITRO_PUBLISH_MAX_PIXELS (256 * NITRO_PUBLISH_MAX_H)
#define NITRO_PUBLISH_BANDS (NITRO_PUBLISH_MAX_H / 4)

typedef struct NitroPublishShadow {
    uint16_t under[NITRO_PUBLISH_MAX_PIXELS];
    uint16_t over[NITRO_PUBLISH_MAX_PIXELS];
    uint8_t alpha[NITRO_PUBLISH_MAX_PIXELS];
    uint32_t premultiplied[NITRO_PUBLISH_MAX_PIXELS];   /* (working space) */
    uint16_t packed[NITRO_PUBLISH_MAX_PIXELS];          /* (and for RGBA5551) */
    int width, height, underValid, overValid;
    uint32_t gen;
    uint32_t underChanged[NITRO_PUBLISH_BANDS], overChanged[NITRO_PUBLISH_BANDS];
    uint8_t overAny[NITRO_PUBLISH_BANDS];
    uint8_t overPartial[NITRO_PUBLISH_BANDS];   /* (a band's over pixels: any of them partly see-through) */
} NitroPublishShadow;

typedef struct NitroPublishTarget {
    const NitroPublishShadow *heldBy;   /* whose pictures its textures hold (NULL: not known) */
    int heldWidth, heldHeight;
    uint32_t underGen, overGen;         /* up to date with that publish (0: not known) */
    int allow16;                        /* (set by the caller) its over texture may be RGBA5551 */
    int over16;                         /* its over texture is RGBA5551 now (else RGBA8) */
} NitroPublishTarget;

/* The over layer's pixels i .. j (colour bit 15: drawn; alpha 0-16) into
 * RGBA8 (red in the top byte) with the alpha multiplied in, alpha16 giving
 * 0-16 as 0-255; whether any isn't transparent. */
uint32_t NitroPort_PremultiplyOver(const uint16_t *over, const uint8_t *alpha, const uint8_t alpha16[17],
                                   uint32_t *out, int i, int j);
/* Whether any of the over layer's pixels i .. j is drawn and partly
 * see-through (its alpha, through alpha16, neither 0 nor 255). */
int NitroPort_OverPartial(const uint16_t *over, const uint8_t *alpha, const uint8_t alpha16[17], int i, int j);
/* Premultiplied RGBA8 texels i .. j, each alpha 0 or 255 (no pixel partly
 * see-through), into RGBA5551 (red in the top 5 bits, alpha bit 0): the
 * DS's colours exactly (5 bits each), and the same picture once the GPU
 * widens them back. Half the bytes for the GPU to read. */
void NitroPort_PackOver5551(const uint32_t *premultiplied, uint16_t *out, int i, int j);

/* A picture w wide (a multiple of 4, at most NITRO_PUBLISH_MAX_W) and h
 * tall (a multiple of 4, at most NITRO_PUBLISH_MAX_H; w x h at most
 * NITRO_PUBLISH_MAX_PIXELS) into a target's textures (texW texels wide, tiled; underTex RGB565 through
 * table as NitroPort_Double2xTiled16Table, 8-byte aligned; overTex RGBA8
 * premultiplied, written only if hasOver), the bands changed since it was
 * last written. With the target's allow16, the over texture is RGBA5551
 * (NitroPort_PackOver5551, the texels 16 bits each in the same memory)
 * while none of the over layer is partly see-through, and RGBA8 when some
 * is: target->over16 says which (all of it written again when that
 * changes). flush(data, bytes) for each part of a texture written.
 * Returns whether the over layer has anything in it. */
int NitroPort_PublishDoubled(NitroPublishShadow *shadow, NitroPublishTarget *target, const uint16_t *under,
                             const uint16_t *over, const uint8_t *overAlpha, int hasOver, int w, int h,
                             uint16_t *underTex, uint32_t *overTex, int texW, const uint16_t *table,
                             const uint8_t alpha16[17], void (*flush)(void *data, uint32_t bytes));

#ifdef __cplusplus
}
#endif
#endif
