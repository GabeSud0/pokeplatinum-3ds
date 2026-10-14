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
static unsigned mark_pixel(uint16_t *image, uint8_t *alpha, const uint32_t *depthId,
                           const NitroGx3dEdgeSetup *setup, const EdgeRows *rows,
                           int x, int y, int *translucent)
{
    const uint16_t *above = rows->color[rows->odd], *here = rows->color[rows->odd ^ 1];
    const uint8_t *aboveAlpha = rows->alpha[rows->odd], *hereAlpha = rows->alpha[rows->odd ^ 1];
    const uint32_t *p = depthId + y * W + x;
    uint32_t word = *p;
    unsigned id = word >> 24, k;
    uint32_t z = word & 0xffffffu;
    uint16_t behind = 0;
    unsigned behindAlpha = 0;
    /* Left, right, above, below: the first of another ID further away
     * decides. Off the screen it's the rear plane; in it, a pixel without
     * an opaque polygon has the rear plane's ID but its own depth. */
    int inside[4];
    uint32_t n[4];

    if (id > 63) return 0;   /* no opaque polygon here */
    inside[0] = x > 0;
    inside[1] = x < W - 1;
    inside[2] = y > 0;
    inside[3] = y < H - 1;
    n[0] = inside[0] ? p[-1] : 0;
    n[1] = inside[1] ? p[1] : 0;
    n[2] = inside[2] ? p[-W] : 0;
    n[3] = inside[3] ? p[W] : 0;
    for (k = 0; k < 4; k++) {
        unsigned nid = setup->clearId;
        uint32_t nz = setup->clearDepth;

        if (inside[k]) {
            if ((n[k] >> 24) <= 63) nid = n[k] >> 24;
            nz = n[k] & 0xffffffu;
        }
        if (nid == id || z >= nz) continue;
        if (!inside[k]) {
            behind = (uint16_t)(setup->clearColor | (setup->clearAlpha ? 0x8000u : 0u));
            behindAlpha = setup->clearAlpha;
        } else if (k == 0) {
            behind = here[x - 1];
            behindAlpha = hereAlpha[x - 1];
        } else if (k == 2) {
            behind = above[x];
            behindAlpha = aboveAlpha[x];
        } else {
            /* Right and below aren't changed yet. */
            unsigned j = (unsigned)(y * W + x) + (k == 1 ? 1u : (unsigned)W);
            behind = image[j];
            behindAlpha = alpha[j];
        }
        /* Nothing drawn there: the rear plane, see-through. */
        if (!(behind & 0x8000)) behindAlpha = 0;
        break;
    }
    if (k == 4) return 0;
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
    return 1;
}

/* Each pixel's ID byte, four to a word: most pixels have the same ID as
 * all four neighbours (inside a polygon, or nothing at all), and four at a
 * time can be passed over with three comparisons. */
typedef uint32_t IdWord __attribute__((may_alias, aligned(4)));
static uint8_t sIds[H][W] __attribute__((aligned(4)));

unsigned NitroPort_Gx3dMarkEdges(uint16_t *image, uint8_t *alpha, const uint32_t *depthId,
                                 const NitroGx3dEdgeSetup *setup, int *translucent)
{
    static EdgeRows rows;
    unsigned outlined = 0;
    int x, y, i;

    if (translucent) *translucent = 0;
    {
        IdWord *ids = (IdWord *)sIds;
        const uint32_t *d = depthId;

        for (i = 0; i < W * H / 4; i++, d += 4) {
            ids[i] = (d[0] >> 24) | ((d[1] >> 16) & 0xFF00u) | ((d[2] >> 8) & 0xFF0000u) | (d[3] & 0xFF000000u);
        }
    }
    rows.odd = 0;
    for (y = 0; y < H; y++) {
        memcpy(rows.color[rows.odd ^ 1], image + y * W, sizeof(rows.color[0]));
        memcpy(rows.alpha[rows.odd ^ 1], alpha + y * W, sizeof(rows.alpha[0]));
        if (y == 0 || y == H - 1) {
            for (x = 0; x < W; x++) outlined += mark_pixel(image, alpha, depthId, setup, &rows, x, y, translucent);
        } else {
            const IdWord *up = (const IdWord *)sIds[y - 1], *here = (const IdWord *)sIds[y];
            const IdWord *down = (const IdWord *)sIds[y + 1];
            const uint8_t *ids = sIds[y];

            for (i = 0; i < W / 4; i++) {
                uint32_t word = here[i];
                int x0 = i * 4, x1 = x0 + 4;

                /* All four, the ones above and below, and those either
                 * side of the four, the same ID: none is on an edge (the
                 * screen's left and right columns always are looked at). */
                if (word == up[i] && word == down[i] && word == (word & 0xFFu) * 0x01010101u &&
                    (i == 0 || ids[x0 - 1] == (uint8_t)word) &&
                    (i == W / 4 - 1 || ids[x1] == (uint8_t)word)) {
                    if (i == 0) outlined += mark_pixel(image, alpha, depthId, setup, &rows, 0, y, translucent);
                    if (i == W / 4 - 1) outlined += mark_pixel(image, alpha, depthId, setup, &rows, W - 1, y, translucent);
                    continue;
                }
                for (x = x0; x < x1; x++) {
                    unsigned id = ids[x];

                    if (x > 0 && x < W - 1 && ids[x - 1] == id && ids[x + 1] == id &&
                        sIds[y - 1][x] == id && sIds[y + 1][x] == id) {
                        continue;
                    }
                    outlined += mark_pixel(image, alpha, depthId, setup, &rows, x, y, translucent);
                }
            }
        }
        rows.odd ^= 1;
    }
    return outlined;
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
