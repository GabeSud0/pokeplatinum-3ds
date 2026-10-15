/*
 * DS 3D textures, decoded the same way for both renderers: the PC reference
 * rasterizer samples texels with NitroPort_Gx3dSampleTexel, and the 3DS GPU
 * path converts whole textures with NitroPort_Gx3dDecodeTextureTiled, which
 * gives the same texels a row at a time (much faster); the tests check the
 * two against each other, so they can't drift apart.
 *
 * Textures and palettes are read from the DS VRAM banks the game has mapped
 * as texture image / texture palette slots (VRAMCNT A-G). No libctru here:
 * everything in this file is host-testable.
 */
#ifndef NITRO_PORT_GX3D_TEXTURE_H
#define NITRO_PORT_GX3D_TEXTURE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Where each texture slot (4 x 128 KiB) and palette slot (6 x 16 KiB) is,
 * or NULL when no bank is mapped there. */
typedef struct NitroGx3dTexBanks {
    const unsigned char *texture[4];
    const unsigned char *palette[6];
} NitroGx3dTexBanks;

/* One texel: RGB555 colour and alpha 0 (transparent) ... 31 (opaque). */
typedef struct NitroGx3dTexel {
    uint16_t rgb;
    uint8_t alpha;
} NitroGx3dTexel;

/* Read VRAMCNT A-G and point the slots at the port's DS VRAM. */
void NitroPort_Gx3dMapTextureBanks(NitroGx3dTexBanks *banks);

/* TEXIMAGE_PARAM fields. */
static inline unsigned NitroPort_Gx3dTexFormat(uint32_t param) { return (param >> 26) & 7u; }
static inline unsigned NitroPort_Gx3dTexWidth(uint32_t param) { return 8u << ((param >> 20) & 7u); }
static inline unsigned NitroPort_Gx3dTexHeight(uint32_t param) { return 8u << ((param >> 23) & 7u); }

/* The texel at (s, t), in texels, after the DS's repeat/flip/clamp rules.
 * Format 0 (no texture) gives opaque white. */
NitroGx3dTexel NitroPort_Gx3dSampleTexel(const NitroGx3dTexBanks *banks, uint32_t param,
                                         uint32_t palette_base, int s, int t);

/* The whole texture, row by row from the top (width x height words), as
 * PICA RGBA8 words: 0xRRGGBBAA. 5-bit values widen as (v << 3) | (v >> 2). */
void NitroPort_Gx3dDecodeTexture(const NitroGx3dTexBanks *banks, uint32_t param,
                                 uint32_t palette_base, uint32_t *rgba8);

/* Re-order a decoded texture for the PICA: 8x8 tiles in Morton order, rows
 * stored bottom-up (the PICA's t = 0 is the last row), so that texture
 * coordinate (s / width, t / height) samples DS texel (s, t). */
void NitroPort_Gx3dTileRGBA8(const uint32_t *linear, unsigned width, unsigned height,
                             uint32_t *tiled);

/* A decoded texture (width x height, as NitroPort_Gx3dDecodeTexture gives
 * it) for the GPU's bilinear filter, into width * scale x height * scale
 * words: each texel scale x scale texels (scale 2: the DS's texels sharp,
 * their borders a texel soft, as the remaster's 2D is drawn), and each
 * transparent texel coloured as an opaque one beside it (alpha still 0),
 * so the filter doesn't darken a cut-out's edge. */
void NitroPort_Gx3dFilterTexels(const uint32_t *rgba8, unsigned width, unsigned height, unsigned scale,
                                uint32_t *out);

/* The same, straight into the PICA's tiled order (NitroPort_Gx3dTileRGBA8
 * of the above, in one pass). */
void NitroPort_Gx3dFilterTexelsTiled(const uint32_t *rgba8, unsigned width, unsigned height, unsigned scale,
                                     uint32_t *tiled);
/* NitroPort_Gx3dTileRGBA8 undone. */
void NitroPort_Gx3dUntileRGBA8(const uint32_t *tiled, unsigned width, unsigned height, uint32_t *linear);

/* Both at once, without the linear copy: what the 3DS GPU path uses. The
 * same texels as the sampler gives, worked out a row at a time. */
void NitroPort_Gx3dDecodeTextureTiled(const NitroGx3dTexBanks *banks, uint32_t param,
                                      uint32_t palette_base, uint32_t *tiled);
/* Only texels s0 <= s < s1, t0 <= t < t1 (clamped to the texture), into
 * their places in the whole tiled texture; the rest is left as it is. */
void NitroPort_Gx3dDecodeTextureTiledRect(const NitroGx3dTexBanks *banks, uint32_t param,
                                          uint32_t palette_base, uint32_t *tiled,
                                          unsigned s0, unsigned t0, unsigned s1, unsigned t1);

/* Whether a format's texels are exactly RGBA5551: the DS's colours are 5
 * bits a channel, and every format but A3I5 (1) and A5I3 (6) is opaque or
 * fully see-through, texel by texel. Half the memory of RGBA8, and half
 * what the GPU reads for each texel it draws. */
static inline int NitroPort_Gx3dTexIs5551(uint32_t param)
{
    unsigned fmt = NitroPort_Gx3dTexFormat(param);
    return fmt != 0 && fmt != 1 && fmt != 6;
}
/* RGBA8 texels (red in the top byte, as decoded) into the PICA's RGBA5551
 * (red in bits 15-11, alpha bit 0), in the same order: each channel's top
 * 5 bits, alpha set from 128 up. */
void NitroPort_Gx3dPackRGBA5551(const uint32_t *rgba8, uint16_t *out, size_t count);

/* A hash of everything the texture's texels come from: the texel data, the
 * 4x4 block descriptors (format 5), the palette entries it can use, and the
 * bank mapping. A converted texture is reused while this stays the same. */
uint32_t NitroPort_Gx3dTextureHash(const NitroGx3dTexBanks *banks, uint32_t param,
                                   uint32_t palette_base);

#ifdef __cplusplus
}
#endif
#endif
