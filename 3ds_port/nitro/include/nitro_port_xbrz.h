/*
 * xBRZ at 2x, for the 3D's textures (filter3d xbrz): a C version of the
 * 2x scaler in Zenju's xBRZ (the HqMAME project, GPL; the copy in PPSSPP's
 * ext/xbrz with its colour distance and rules), for pixels as this port
 * decodes them: RGBA8, red in the top byte, alpha in the bottom.
 *
 * xBRZ looks at each 2x2 of input pixels, decides from the colour
 * distances around it whether an edge runs through the corner between
 * them and which way, then blends the corner's output pixels along that
 * edge: slopes and curves come out smooth, small details (an eye, a lone
 * pixel) are kept.
 *
 * Two passes, so a texture can be done a few rows at a time over several
 * frames: first every corner's blend (into one byte a pixel, zeroed
 * before), then the output. Each pass can be given rows y0 <= y < y1; the
 * second needs all of the first. Wrapping per axis (as the texture is
 * drawn): CLAMP the edge pixel repeats (as the original xBRZ), REPEAT the
 * other side (a tile that repeats stays seamless), MIRROR reflected.
 *
 * keepSubtle: no blend at a corner whose four pixels' colours all count
 * as equal (xBRZ's own tolerance): the original smooths faint patterns
 * too (a grass texture's tufts, a shade lighter than the grass), into
 * strokes half a DS texel wide that drawn at the 3DS's size mostly fell
 * between pixels; the grass came out flat. 0: the original's output.
 */
#ifndef NITRO_PORT_XBRZ_H
#define NITRO_PORT_XBRZ_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { NITRO_XBRZ_CLAMP = 0, NITRO_XBRZ_REPEAT = 1, NITRO_XBRZ_MIRROR = 2 };

/* Pass 1, rows y0..y1-1 of the input (w x h): each corner's blend into
 * info (w * h bytes, zeroed before the first call). */
void NitroPort_Xbrz2xCorners(const uint32_t *src, unsigned w, unsigned h, int wrapS, int wrapT,
                             int keepSubtle, uint8_t *info, unsigned y0, unsigned y1);

/* Pass 2, rows y0..y1-1 of the input: their output rows (2 * y0 to
 * 2 * y1 - 1) of out (2w x 2h, row by row). */
void NitroPort_Xbrz2xBlend(const uint32_t *src, unsigned w, unsigned h, int wrapS, int wrapT,
                           const uint8_t *info, uint32_t *out, unsigned y0, unsigned y1);

/* Both passes over the whole picture (info: w * h bytes of scratch). */
void NitroPort_Xbrz2x(const uint32_t *src, unsigned w, unsigned h, int wrapS, int wrapT,
                      int keepSubtle, uint8_t *info, uint32_t *out);

#ifdef __cplusplus
}
#endif

#endif
