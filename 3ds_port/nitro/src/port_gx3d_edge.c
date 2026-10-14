/*
 * DS edge marking (DISP3DCNT bit 5), done on a finished 3D image, shared
 * by the PC reference rasterizer and the 3DS GPU path.
 *
 * The DS outlines opaque polygons: a pixel of one is given the edge colour
 * of its polygon ID (EDGE_COLOR, one colour for each eight IDs) when a
 * pixel next to it (left, right, above or below) belongs to a different
 * opaque polygon ID and is further away. Next to the screen's border, or
 * to a pixel without an opaque polygon, it's the rear plane that counts:
 * the clear polygon ID and depth (CLEAR_COLOR, CLEAR_DEPTH); so polygons
 * cut by the screen's edges get outlined there too, as on the DS.
 *
 * The DS does this only for pixels on a polygon's edge. A pixel next to a
 * further one of another ID is always on its polygon's edge (its polygon
 * would otherwise cover that pixel too, and be nearer there), so the
 * neighbours alone decide here.
 *
 * With anti-aliasing on (DISP3DCNT bit 4) the DS blends an outlined pixel
 * with what is behind it, at about half strength: 17/32 the edge colour.
 * What's behind it is taken to be the further neighbour that caused the
 * outline (or the rear plane): the DS keeps the pixel underneath, the GPU
 * path doesn't. Its colour is only blended in if it isn't transparent; the
 * alphas always are, so an outline against a see-through rear plane is
 * half transparent, as on the DS.
 */
#include <nitro_port_gx3d.h>
#include <string.h>

extern unsigned char NitroPort_IoReg[];

#define W NITRO_GX3D_WIDTH
#define H NITRO_GX3D_HEIGHT

static unsigned io16(unsigned offset)
{
    return NitroPort_IoReg[offset] | ((unsigned)NitroPort_IoReg[offset + 1] << 8);
}

void NitroPort_Gx3dEdgeSetup(NitroGx3dEdgeSetup *setup)
{
    unsigned disp3dcnt = io16(0x60), i;
    uint32_t clear = io16(0x350) | ((uint32_t)io16(0x352) << 16);

    setup->enabled = (disp3dcnt >> 5) & 1;
    setup->antialias = (disp3dcnt >> 4) & 1;
    for (i = 0; i < 8; i++) setup->colors[i] = (uint16_t)(io16(0x330 + i * 2) & 0x7fff);
    setup->clearId = (uint8_t)((clear >> 24) & 63);
    setup->clearColor = (uint16_t)(clear & 0x7fff);
    setup->clearAlpha = (uint8_t)((clear >> 16) & 31);
    setup->clearDepth = (io16(0x354) & 0x7fffu) * 0x200u + 0x1ffu;
}

/* (a * 17 + b * 15) / 32 for each colour. */
static uint16_t blend_edge(uint16_t edge, uint16_t behind)
{
    unsigned r = ((edge & 31) * 17 + (behind & 31) * 15) >> 5;
    unsigned g = (((edge >> 5) & 31) * 17 + ((behind >> 5) & 31) * 15) >> 5;
    unsigned b = (((edge >> 10) & 31) * 17 + ((behind >> 10) & 31) * 15) >> 5;
    return (uint16_t)(r | (g << 5) | (b << 10));
}

/* What the rows above and this one were before any outline went in:
 * an outline's "behind" is a neighbour as it was drawn. */
typedef struct EdgeRows {
    uint16_t color[2][W];
    uint8_t alpha[2][W];
    unsigned odd;
} EdgeRows;

/* Pixel (x, y): outlined if it has an opaque polygon and a neighbour of
 * another ID further away. */
static void mark_pixel(uint16_t *image, uint8_t *alpha, const uint32_t *depthId,
                       const NitroGx3dEdgeSetup *setup, const EdgeRows *rows,
                       int x, int y, int *translucent)
{
    const uint16_t *above = rows->color[rows->odd], *here = rows->color[rows->odd ^ 1];
    const uint8_t *aboveAlpha = rows->alpha[rows->odd], *hereAlpha = rows->alpha[rows->odd ^ 1];
    uint32_t word = depthId[y * W + x];
    unsigned id = word >> 24, k;
    uint32_t z = word & 0xffffffu;
    uint16_t behind = 0;
    unsigned behindAlpha = 0;

    if (id > 63) return;   /* no opaque polygon here */
    for (k = 0; k < 4; k++) {
        int nx = x + (k == 0 ? -1 : k == 1 ? 1 : 0);
        int ny = y + (k == 2 ? -1 : k == 3 ? 1 : 0);
        int inside = nx >= 0 && nx < W && ny >= 0 && ny < H;
        unsigned nid = setup->clearId;
        uint32_t nz = setup->clearDepth;

        if (inside) {
            uint32_t n = depthId[ny * W + nx];
            if ((n >> 24) <= 63) nid = n >> 24;
            nz = n & 0xffffffu;
        }
        if (nid == id || z >= nz) continue;
        if (!inside) {
            behind = (uint16_t)(setup->clearColor | (setup->clearAlpha ? 0x8000u : 0u));
            behindAlpha = setup->clearAlpha;
        } else if (k == 0) {
            behind = here[nx];
            behindAlpha = hereAlpha[nx];
        } else if (k == 2) {
            behind = above[nx];
            behindAlpha = aboveAlpha[nx];
        } else {
            /* Right and below aren't changed yet. */
            behind = image[ny * W + nx];
            behindAlpha = alpha[ny * W + nx];
        }
        /* Nothing drawn there: the rear plane, see-through. */
        if (!(behind & 0x8000)) behindAlpha = 0;
        break;
    }
    if (k == 4) return;
    {
        uint16_t edge = setup->colors[id >> 3];
        unsigned i = (unsigned)(y * W + x), a = alpha[i];

        if (setup->antialias) {
            if (behindAlpha > 0) edge = blend_edge(edge, behind & 0x7fff);
            a = (a * 17 + behindAlpha * 15) >> 5;
            if (translucent && a < 31) *translucent = 1;
        }
        image[i] = (uint16_t)(edge | (a ? 0x8000u : 0u));
        alpha[i] = (uint8_t)a;
    }
}

void NitroPort_Gx3dMarkEdges(uint16_t *image, uint8_t *alpha, const uint32_t *depthId,
                             const NitroGx3dEdgeSetup *setup, int *translucent)
{
    static EdgeRows rows;
    int x, y;

    if (translucent) *translucent = 0;
    rows.odd = 0;
    for (y = 0; y < H; y++) {
        const uint32_t *row = depthId + y * W;

        memcpy(rows.color[rows.odd ^ 1], image + y * W, sizeof(rows.color[0]));
        memcpy(rows.alpha[rows.odd ^ 1], alpha + y * W, sizeof(rows.alpha[0]));
        if (y == 0 || y == H - 1) {
            for (x = 0; x < W; x++) mark_pixel(image, alpha, depthId, setup, &rows, x, y, translucent);
        } else {
            mark_pixel(image, alpha, depthId, setup, &rows, 0, y, translucent);
            for (x = 1; x < W - 1; x++) {
                /* Most pixels: the same ID byte all around. */
                uint32_t word = row[x];
                if (((row[x - 1] ^ word) | (row[x + 1] ^ word) |
                     (row[x - W] ^ word) | (row[x + W] ^ word)) < 0x1000000u) {
                    continue;
                }
                mark_pixel(image, alpha, depthId, setup, &rows, x, y, translucent);
            }
            mark_pixel(image, alpha, depthId, setup, &rows, W - 1, y, translucent);
        }
        rows.odd ^= 1;
    }
}

unsigned NitroPort_Gx3dEdgeIdsUnexpected(const uint32_t *depthId)
{
    unsigned i, odd = 0;

    for (i = 0; i < W * H; i++) {
        unsigned id = depthId[i] >> 24;
        odd += id >= 128 && id != 255;
    }
    return odd;
}
