/* Which triangles the GPU's edge marking passes need (see
 * nitro_port_gx3d_vertex.h, NitroPort_Gx3dPruneEdges, and
 * port_gx3d_pica.c's mark_edges for the passes themselves).
 *
 * In the screen's pixels, as the eyes see them (the 3D slider moves
 * vertices by their depth, as the vertex program does):
 *  - a triangle with another of another polygon ID within two pixels of
 *    its bounding box (over both eyes' boxes) may get an outline from it:
 *    D;
 *  - of the rest, those whose polygon ID isn't the rear plane's may get an
 *    outline from it where no solid outline-pass triangle covers a pixel
 *    within two pixels of their box. Surely covers: the pixel's centre
 *    inside it by a sixteenth of a pixel (the GPU's own rounding can't leave
 *    it out). NitroPort_Gx3dExpandTriangle moved every edge out by a tenth
 *    of a DS pixel (an eighth of a 3DS one), so on an edge two triangles
 *    share, every pixel is surely covered by one of them. Such pixels in a
 *    zone along the border (past the DS's screen's edges, where the scene
 *    stops; see zones()) give the triangle that zone's lists; anywhere else,
 *    D;
 *  - A: D, and the triangles within two pixels of one in D (what its
 *    pixels' neighbours show, in pass A); a zone's pass A: what touches it.
 * Two pixels: a neighbour is one away, and the GPU's coverage can be a
 * fraction of a pixel off a triangle's true edges.
 *
 * It runs on the 3DS's CPU every scene (twice the work with the slider
 * up), so the coverage is a bit a pixel, filled a row's span at a time. */
#include <nitro_port_gx3d_vertex.h>

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define NEAR_PX 2.0f            /* two triangles are near: their boxes this close */
#define INSET_PX (1.0f / 16.0f) /* a pixel surely covered: its centre this far inside */
#define CELL 32                 /* the grid for finding near triangles: pixels a cell */
#define GRID_MARGIN 64          /* the grid reaches this far past the screen */

enum { T_D = 1, T_A = 2, T_WILD = 4, T_ZONE = 16 /* << zone */ };

/* (not the library's fminf and fmaxf: calls, on the 3DS) */
#define MINF(a, b) ((a) < (b) ? (a) : (b))
#define MAXF(a, b) ((a) > (b) ? (a) : (b))

typedef struct Box {
    float x0, y0, x1, y1;
} Box;

/* A triangle's corners as an eye sees them (screen pixels). */
typedef struct Corners {
    float x[3], y[3];
} Corners;

/* Map columns and rows, inclusive (empty when c0 > c1 or r0 > r1). */
typedef struct Region {
    long c0, r0, c1, r1;
} Region;

/* A grid's lists: each cell's triangles, from start[cell] to
 * start[cell + 1]; those of the commonest polygon ID first, then from
 * others[cell] the rest. */
typedef struct GridLists {
    size_t cells, entries;      /* room for */
    uint32_t *start, *others;
    uint16_t *entry;
} GridLists;

/* Kept between scenes, grown as needed. */
static struct {
    size_t triangles;
    uint32_t *first;            /* a triangle's first vertex */
    uint16_t *batch;
    uint8_t *id, *role, *flags;
    uint16_t *lists;            /* the lists it's in (bits) */
    Box *box;                   /* over both eyes */
    int16_t (*cellsOf)[4];      /* the grid cells its box touches: x0, y0, x1, y1 */
    Corners *corners;           /* two a triangle: the eyes' */
    size_t words;
    uint32_t *covered;          /* the map: a bit a pixel, set: surely covered */
    size_t rows;
    uint16_t *rowOpen;          /* a row's words with an uncovered pixel (map_summarize) */
    size_t rowsBeforeRoom;
    uint16_t *rowsBefore;       /* the rows with one before each row (map_summarize) */
    size_t cells;
    uint64_t *cellIds;          /* the polygon IDs in a cell */
    uint8_t *cellNeed;          /* a cell some triangle's map query reaches */
    GridLists all, ofD;         /* every triangle; those in D */
} sWork;

void NitroPort_Gx3dPruneFree(void)
{
    free(sWork.first);
    free(sWork.batch);
    free(sWork.id);
    free(sWork.role);
    free(sWork.flags);
    free(sWork.lists);
    free(sWork.box);
    free(sWork.cellsOf);
    free(sWork.corners);
    free(sWork.covered);
    free(sWork.rowOpen);
    free(sWork.rowsBefore);
    free(sWork.cellIds);
    free(sWork.cellNeed);
    free(sWork.all.start);
    free(sWork.all.others);
    free(sWork.all.entry);
    free(sWork.ofD.start);
    free(sWork.ofD.others);
    free(sWork.ofD.entry);
    memset(&sWork, 0, sizeof(sWork));
}

static int grow(void **p, size_t *have, size_t want, size_t size)
{
    void *n;

    if (*have >= want) {
        return 1;
    }
    n = realloc(*p, want * size);
    if (n == NULL) {
        return 0;
    }
    *p = n;
    *have = want;
    return 1;
}

static int room_for_triangles(size_t n)
{
    void *p;

    if (sWork.triangles >= n) {
        return 1;
    }
#define GROW(field, each)                              \
    do {                                               \
        p = realloc(sWork.field, n * (each));          \
        if (p == NULL) {                               \
            return 0;                                  \
        }                                              \
        sWork.field = p;                               \
    } while (0)
    GROW(first, sizeof(*sWork.first));
    GROW(batch, sizeof(*sWork.batch));
    GROW(id, 1);
    GROW(role, 1);
    GROW(flags, 1);
    GROW(lists, sizeof(*sWork.lists));
    GROW(box, sizeof(*sWork.box));
    GROW(cellsOf, sizeof(*sWork.cellsOf));
    GROW(corners, 2 * sizeof(*sWork.corners));
#undef GROW
    sWork.triangles = n;
    return 1;
}

static int room_for_cells(GridLists *l, size_t cells)
{
    void *p;

    if (l->cells >= cells) {
        return 1;
    }
    p = realloc(l->start, (cells + 1u) * sizeof(*l->start));
    if (p == NULL) {
        return 0;
    }
    l->start = p;
    p = realloc(l->others, cells * sizeof(*l->others));
    if (p == NULL) {
        return 0;
    }
    l->others = p;
    l->cells = cells;
    return 1;
}

/* A vertex where an eye sees it, in the screen's pixels (as the vertex
 * program and NitroPort_PicaViewMatrix put it), and how far the eye's
 * shift moved it. 0 if it can't be placed. */
static inline int place(const NitroGx3dGpuVertex *v, const NitroGx3dPruneView *view, float stereo, float *px, float *py,
                 float *moved)
{
    float x = v->position[0], y = v->position[1], w = v->position[3], inv, shift = 0.0f;

    if (!(w > 0.0f) || !isfinite(x) || !isfinite(y) || !isfinite(w)) {
        return 0;
    }
    if (w > 1.0001f) {
        float s = w - view->convergence;

        if (s < -w) {
            s = -w;
        }
        shift = stereo * s;
        x += shift;
    }
    inv = 1.0f / w;
    *px = (view->x0 + view->x1) * 0.5f + x * inv * (view->x1 - view->x0) * 0.5f;
    *py = (view->y0 + view->y1) * 0.5f - y * inv * (view->y1 - view->y0) * 0.5f;
    *moved = fabsf(shift * inv) * (view->x1 - view->x0) * 0.5f;
    /* (far off: as good as unplaceable) */
    return fabsf(*px) < 1.0e5f && fabsf(*py) < 1.0e5f;
}

static inline Box box_of(const Corners *c)
{
    Box b;

    b.x0 = MINF(c->x[0], MINF(c->x[1], c->x[2]));
    b.x1 = MAXF(c->x[0], MAXF(c->x[1], c->x[2]));
    b.y0 = MINF(c->y[0], MINF(c->y[1], c->y[2]));
    b.y1 = MAXF(c->y[0], MAXF(c->y[1], c->y[2]));
    return b;
}

static inline int near_boxes(const Box *a, const Box *b)
{
    return !(a->x1 + NEAR_PX < b->x0 || b->x1 + NEAR_PX < a->x0 || a->y1 + NEAR_PX < b->y0 ||
             b->y1 + NEAR_PX < a->y0);
}

/* (for values in -1e5..1e5, without the library's floorf and ceilf) */
static inline long floor_px(float v)
{
    long i = (long)v;

    return (float)i > v ? i - 1 : i;
}

static inline long ceil_px(float v)
{
    long i = (long)v;

    return (float)i < v ? i + 1 : i;
}

/* ------------------------------------------------------------------ the map */
/* Pixels -1..width by -1..height (a pixel round the screen: the DS's
 * screen can reach past it, and pass A draws what's there into the
 * screen's edge pixels); map column = pixel + 1, a bit each, rows of
 * MAP_WORDS words. */
#define MAP_W(view) ((long)(view)->width + 2)
#define MAP_H(view) ((long)(view)->height + 2)
#define MAP_WORDS(view) ((MAP_W(view) + 31) / 32)

/* The pixels a box can test, in map columns and rows: two pixels round
 * those it can cover. */
static Region query_of(const NitroGx3dPruneView *view, const Box *b)
{
    Region q;

    q.c0 = floor_px(b->x0) - 2 + 1;
    q.c1 = floor_px(b->x1) + 2 + 1;
    q.r0 = floor_px(b->y0) - 2 + 1;
    q.r1 = floor_px(b->y1) + 2 + 1;
    if (q.c0 < 0) {
        q.c0 = 0;
    }
    if (q.r0 < 0) {
        q.r0 = 0;
    }
    if (q.c1 > MAP_W(view) - 1) {
        q.c1 = MAP_W(view) - 1;
    }
    if (q.r1 > MAP_H(view) - 1) {
        q.r1 = MAP_H(view) - 1;
    }
    return q;
}

/* Rows r0..r1 of the map: nothing covered yet in the needed cells
 * (sWork.cellNeed), the rest as if covered (never looked at). */
struct Grid;
static void map_clear(const NitroGx3dPruneView *view, const struct Grid *g, long r0, long r1);

/* A word's bits for columns c0..c1. */
static inline uint32_t span_bits(long c0, long c1, long word)
{
    long lo = word * 32, hi = lo + 31;

    if (c0 > lo) {
        lo = c0;
    }
    if (c1 < hi) {
        hi = c1;
    }
    if (lo > hi) {
        return 0;
    }
    return (hi - lo == 31 ? 0xFFFFFFFFu : ((1u << (hi - lo + 1)) - 1u)) << (lo - word * 32);
}

/* The rows r0..r1's pixels a triangle surely covers, marked. */
static void map_cover(const NitroGx3dPruneView *view, long r0, long r1, const Corners *c)
{
    long words = MAP_WORDS(view), r;
    float area = (c->x[1] - c->x[0]) * (c->y[2] - c->y[0]) - (c->x[2] - c->x[0]) * (c->y[1] - c->y[0]);
    /* The span's ends on a row: the greater of the (at most two) edges
     * bounding it from the left, the lesser of those from the right, each
     * x = x_k + (INSET_PX - eb (y - y_k)) / ea: precise where it's on the
     * screen, even for an edge nearly along a row (unused ones: far off).
     * An edge along a row bounds the rows instead. */
    float lx[2] = { -8.0f, -8.0f }, ly[2] = { 0.0f, 0.0f }, lb[2] = { 0.0f, 0.0f }, li[2] = { 1.0f, 1.0f };
    float ux[2] = { 1.0e6f, 1.0e6f }, uy[2] = { 0.0f, 0.0f }, ub[2] = { 0.0f, 0.0f }, ui[2] = { 1.0f, 1.0f };
    float ylo = -1.0e30f, yhi = 1.0e30f, miny, maxy, right = (float)view->width + 1.25f;
    float wide = MAXF(c->x[0], MAXF(c->x[1], c->x[2])) - MINF(c->x[0], MINF(c->x[1], c->x[2]));
    int nl = 0, nu = 0, k;

    if (!(area > 1.0e-6f || area < -1.0e-6f)) {
        return;
    }
    for (k = 0; k < 3; k++) {
        int n = k == 2 ? 0 : k + 1;
        float dx = c->x[n] - c->x[k], dy = c->y[n] - c->y[k], len = sqrtf(dx * dx + dy * dy), ea, eb;

        if (!(len > 1.0e-6f)) {
            return;
        }
        if (area < 0.0f) {
            len = -len;
        }
        /* the inward normal: ea (x - x_k) + eb (y - y_k) >= INSET_PX */
        ea = -dy / len;
        eb = dx / len;
        if (ea > 1.0e-6f && nl < 2) {
            lx[nl] = c->x[k];
            ly[nl] = c->y[k];
            lb[nl] = eb;
            li[nl++] = 1.0f / ea;
        } else if (ea < -1.0e-6f && nu < 2) {
            ux[nu] = c->x[k];
            uy[nu] = c->y[k];
            ub[nu] = eb;
            ui[nu++] = 1.0f / ea;
        } else {
            /* along a row: ea (x - x_k) is at least -|ea| wide over the
             * triangle, so the rows where eb (y - y_k) is INSET_PX more */
            float need = INSET_PX + (ea < 0.0f ? -ea : ea) * wide;

            if (eb > 0.0f) {
                ylo = MAXF(ylo, c->y[k] + need / eb);
            } else {
                yhi = MINF(yhi, c->y[k] + need / eb);
            }
        }
    }
    miny = MAXF(MINF(c->y[0], MINF(c->y[1], c->y[2])), ylo);
    maxy = MINF(MAXF(c->y[0], MAXF(c->y[1], c->y[2])), yhi);
    miny = MAXF(miny, -2.0f);
    maxy = MINF(maxy, (float)view->height + 2.0f);
    if (!(miny <= maxy)) {
        return;
    }
    /* rows whose centres (row - 0.5) are within miny..maxy */
    if (ceil_px(miny + 0.5f) > r0) {
        r0 = ceil_px(miny + 0.5f);
    }
    if (floor_px(maxy + 0.5f) < r1) {
        r1 = floor_px(maxy + 0.5f);
    }
    {
        uint32_t *row = sWork.covered + r0 * words;

        for (r = r0; r <= r1; r++, row += words) {
            float yc = (float)r - 0.5f;
            float lo = lx[0] + (INSET_PX - lb[0] * (yc - ly[0])) * li[0];
            float hi = ux[0] + (INSET_PX - ub[0] * (yc - uy[0])) * ui[0];
            long c0, c1, w0, w1;
            uint32_t first, end;

            if (nl == 2) {
                float l1 = lx[1] + (INSET_PX - lb[1] * (yc - ly[1])) * li[1];

                lo = MAXF(lo, l1);
            }
            if (nu == 2) {
                float u1 = ux[1] + (INSET_PX - ub[1] * (yc - uy[1])) * ui[1];

                hi = MINF(hi, u1);
            }
            /* columns whose centres (column - 0.5) are within lo..hi: from
             * pixel -1's (map column 0) to pixel width's (MAP_W - 1) */
            lo = MAXF(lo, -0.5f);
            hi = MINF(hi, right);
            if (!(lo <= hi)) {
                continue;
            }
            /* (ceil and floor by truncating a positive number: a pixel whose
             * centre is within 1/30000 of a pixel of the inset line may
             * count as covered, still well inside the triangle) */
            c0 = 512 - (long)(512.0f - (lo + 0.5f));
            c1 = (long)(hi + 0.5f + 8.0f) - 8;
            if (c0 > c1) {
                continue;
            }
            w0 = c0 >> 5;
            w1 = c1 >> 5;
            first = 0xFFFFFFFFu << (c0 & 31);
            end = 0xFFFFFFFFu >> (31 - (c1 & 31));
            if (w0 == w1) {
                row[w0] |= first & end;
            } else {
                long w;

                row[w0] |= first;
                for (w = w0 + 1; w < w1; w++) {
                    row[w] = 0xFFFFFFFFu;
                }
                row[w1] |= end;
            }
        }
    }
}

/* --------------------------------------------------------------- the zones */
/* Along each border of the DS's screen: the scene stops there (the CPU
 * clipped it to the DS's view), the slider moving it up to `spill` pixels
 * either way, and the rear plane shows past it (on the 3DS, the pictures
 * beside it or past the screen's own edges). Each zone (in the screen's
 * pixels, left, right, top, bottom; not overlapping) reaches three pixels
 * past where the scene can stop. Its inner part (map columns and rows):
 * the pixels whose neighbours on the screen are all in it. A triangle that
 * may get an outline from the rear plane only there (no uncovered pixel
 * near it elsewhere) can get it only in the zone: it's drawn there (its
 * lists) whenever it's near the inner part, uncovered or not (a few pixels
 * drawn for nothing, rather than the map worked out there too). */
typedef struct Zones {
    int out[4][4];
    Region inner[4];
    /* the inner parts' columns on a row, as map words: beside the top
     * zone, the bottom one, or neither (the side zones only) */
    uint32_t colsTop[16], colsBottom[16], colsSide[16];
    uint32_t valid[16];         /* the map's columns (not the last word's spare bits) */
} Zones;

static void zones(const NitroGx3dPruneView *view, float spill, Zones *zs)
{
    long w = view->width, h = view->height, words = MAP_WORDS(view), k;
    long a = ceil_px(view->x0 + spill) + 3, b = floor_px(view->x1 - spill) - 3;
    long c = ceil_px(view->y0) + 3, d = floor_px(view->y1) - 3;
    int z;

    a = a < 0 ? 0 : a > w ? w : a;
    b = b < 0 ? 0 : b > w ? w : b;
    c = c < 0 ? 0 : c > h ? h : c;
    d = d < 0 ? 0 : d > h ? h : d;
    memset(zs, 0, sizeof(*zs));
    for (z = 0; z < 4; z++) {
        zs->inner[z].c0 = zs->inner[z].r0 = 1;
        zs->inner[z].c1 = zs->inner[z].r1 = 0;
    }
    if (a <= b && c <= d) {
        if (a > 0) {
            zs->out[0][2] = (int)a;
            zs->out[0][3] = (int)h;
            zs->inner[0].c0 = 0;            /* pixels -1 .. a - 2 */
            zs->inner[0].c1 = a - 1;
            zs->inner[0].r0 = 0;
            zs->inner[0].r1 = h + 1;
        }
        if (b < w) {
            zs->out[1][0] = (int)b;
            zs->out[1][2] = (int)w;
            zs->out[1][3] = (int)h;
            zs->inner[1].c0 = b + 2;        /* b + 1 .. w */
            zs->inner[1].c1 = w + 1;
            zs->inner[1].r0 = 0;
            zs->inner[1].r1 = h + 1;
        }
        if (a < b && c > 0) {
            zs->out[2][0] = (int)a;
            zs->out[2][2] = (int)b;
            zs->out[2][3] = (int)c;
            zs->inner[2].c0 = a + 2;        /* a + 1 .. b - 2 */
            zs->inner[2].c1 = b - 1;
            zs->inner[2].r0 = 0;            /* -1 .. c - 2 */
            zs->inner[2].r1 = c - 1;
        }
        if (a < b && d < h) {
            zs->out[3][0] = (int)a;
            zs->out[3][1] = (int)d;
            zs->out[3][2] = (int)b;
            zs->out[3][3] = (int)h;
            zs->inner[3].c0 = a + 2;
            zs->inner[3].c1 = b - 1;
            zs->inner[3].r0 = d + 2;        /* d + 1 .. h */
            zs->inner[3].r1 = h + 1;
        }
    }
    for (k = 0; k < words && k < 16; k++) {
        uint32_t cols[4];

        for (z = 0; z < 4; z++) {
            cols[z] = zs->inner[z].c0 <= zs->inner[z].c1 ? span_bits(zs->inner[z].c0, zs->inner[z].c1, k) : 0;
        }
        zs->colsSide[k] = cols[0] | cols[1];
        zs->colsTop[k] = cols[0] | cols[1] | cols[2];
        zs->colsBottom[k] = cols[0] | cols[1] | cols[3];
        zs->valid[k] = span_bits(0, MAP_W(view) - 1, k);
    }
}

/* The zones' inner columns on a row (map words). */
static const uint32_t *zone_cols(const Zones *zs, long r)
{
    return r <= zs->inner[2].r1 && zs->inner[2].c0 <= zs->inner[2].c1 ? zs->colsTop
         : r >= zs->inner[3].r0 && zs->inner[3].c0 <= zs->inner[3].c1 ? zs->colsBottom
         : zs->colsSide;
}

/* Each row's words with an uncovered pixel outside the zones (bits),
 * after the covering. */
static void map_summarize(const NitroGx3dPruneView *view, long r0, long r1, const Zones *zs)
{
    long words = MAP_WORDS(view), r, w;

    for (r = r0; r <= r1; r++) {
        const uint32_t *row = sWork.covered + r * words, *zone = zone_cols(zs, r);
        uint32_t open = 0;

        for (w = 0; w < words; w++) {
            open |= ((~row[w] & ~zone[w] & zs->valid[w]) != 0u) << w;
        }
        sWork.rowOpen[r] = (uint16_t)open;
    }
    sWork.rowsBefore[0] = 0;
    for (r = 0; r < MAP_H(view); r++) {
        sWork.rowsBefore[r + 1] = (uint16_t)(sWork.rowsBefore[r] + (r >= r0 && r <= r1 && sWork.rowOpen[r] != 0));
    }
}

/* Whether a region has an uncovered pixel outside the zones. */
static int map_uncovered(const NitroGx3dPruneView *view, const Region *q, const Zones *zs)
{
    long words = MAP_WORDS(view), r, w, w0 = q->c0 >> 5, w1 = q->c1 >> 5;
    uint32_t cols[16], wanted = 0;

    if (q->c0 > q->c1 || q->r0 > q->r1 || sWork.rowsBefore[q->r1 + 1] == sWork.rowsBefore[q->r0]) {
        return 0;   /* (no row there with one: the usual) */
    }
    for (w = w0; w <= w1; w++) {
        cols[w] = span_bits(q->c0, q->c1, w);
        wanted |= 1u << w;
    }
    for (r = q->r0; r <= q->r1; r++) {
        const uint32_t *row, *zone;

        if (!(sWork.rowOpen[r] & wanted)) {
            continue;   /* (all covered there: most rows) */
        }
        row = sWork.covered + r * words;
        zone = zone_cols(zs, r);
        for (w = w0; w <= w1; w++) {
            if (~row[w] & ~zone[w] & cols[w]) {
                return 1;
            }
        }
    }
    return 0;
}

static int overlaps(const Region *a, const Region *b)
{
    return a->c0 <= b->c1 && b->c0 <= a->c1 && a->r0 <= b->r1 && b->r0 <= a->r1 && a->c0 <= a->c1 &&
           a->r0 <= a->r1 && b->c0 <= b->c1 && b->r0 <= b->r1;
}

static int touches_zone(const Box *b, const int *s)
{
    if (s[0] >= s[2] || s[1] >= s[3]) {
        return 0;
    }
    return b->x1 + NEAR_PX >= (float)s[0] && b->x0 - NEAR_PX <= (float)s[2] && b->y1 + NEAR_PX >= (float)s[1] &&
           b->y0 - NEAR_PX <= (float)s[3];
}

void NitroPort_Gx3dPruneScissor(const int zone[4], int width, int height, unsigned out[4])
{
    /* screen pixel (x, y) is target pixel (height - 1 - y, width - 1 - x) */
    out[0] = (unsigned)(height - zone[3]);
    out[1] = (unsigned)(width - zone[2]);
    out[2] = (unsigned)(height - zone[1]);
    out[3] = (unsigned)(width - zone[0]);
}

/* ---------------------------------------------------------------- the grid */
typedef struct Grid {
    long w, h;
} Grid;

static inline long cell_of(float v, long cells)
{
    long c;

    if (!(v > -(float)GRID_MARGIN)) {
        return 0;
    }
    c = (long)((v + (float)GRID_MARGIN) * (1.0f / (float)CELL));
    return c >= cells ? cells - 1 : c;
}

/* The cells a box touches, grown by `by`. */
static inline void cells_of(const Grid *g, const Box *b, float by, long *cx0, long *cy0, long *cx1, long *cy1)
{
    *cx0 = cell_of(b->x0 - by, g->w);
    *cx1 = cell_of(b->x1 + by, g->w);
    *cy0 = cell_of(b->y0 - by, g->h);
    *cy1 = cell_of(b->y1 + by, g->h);
}

/* The triangles (all, or those in D) in the cells their boxes touch,
 * the commonest polygon ID's first in each cell. */
static int grid_fill(const Grid *g, GridLists *l, size_t n, int onlyD, unsigned common)
{
    size_t cells = (size_t)(g->w * g->h), i, entries = 0;
    long c;
    int pass;

    if (!room_for_cells(l, cells)) {
        return 0;
    }
    memset(l->start, 0, (cells + 1u) * sizeof(*l->start));
    if (!onlyD) {
        if (sWork.cells < cells) {
            void *p = realloc(sWork.cellIds, cells * sizeof(*sWork.cellIds));

            if (p == NULL) {
                return 0;
            }
            sWork.cellIds = p;
            p = realloc(sWork.cellNeed, cells);
            if (p == NULL) {
                return 0;
            }
            sWork.cellNeed = p;
            sWork.cells = cells;
        }
        memset(sWork.cellIds, 0, cells * sizeof(*sWork.cellIds));
    }
    for (i = 0; i < n; i++) {
        long cx0, cy0, cx1, cy1, cx, cy;

        if (onlyD && !(sWork.flags[i] & T_D)) {
            continue;
        }
        if (!onlyD) {
            cells_of(g, &sWork.box[i], 0.0f, &cx0, &cy0, &cx1, &cy1);
            sWork.cellsOf[i][0] = (int16_t)cx0;
            sWork.cellsOf[i][1] = (int16_t)cy0;
            sWork.cellsOf[i][2] = (int16_t)cx1;
            sWork.cellsOf[i][3] = (int16_t)cy1;
        }
        cx0 = sWork.cellsOf[i][0];
        cy0 = sWork.cellsOf[i][1];
        cx1 = sWork.cellsOf[i][2];
        cy1 = sWork.cellsOf[i][3];
        for (cy = cy0; cy <= cy1; cy++) {
            for (cx = cx0; cx <= cx1; cx++) {
                l->start[cy * g->w + cx + 1]++;
                if (!onlyD) {
                    sWork.cellIds[cy * g->w + cx] |= 1ull << sWork.id[i];
                }
            }
        }
        entries += (size_t)((cx1 - cx0 + 1) * (cy1 - cy0 + 1));
    }
    if (!grow((void **)&l->entry, &l->entries, entries ? entries : 1u, sizeof(*l->entry))) {
        return 0;
    }
    for (c = 0; c < (long)cells; c++) {
        l->start[c + 1] += l->start[c];
    }
    /* (each cell's start moves up as it fills: then back) */
    for (pass = 0; pass < 2; pass++) {
        for (i = 0; i < n; i++) {
            long cx0, cy0, cx1, cy1, cx, cy;

            if ((onlyD && !(sWork.flags[i] & T_D)) || (sWork.id[i] == common) != (pass == 0)) {
                continue;
            }
            cx0 = sWork.cellsOf[i][0];
            cy0 = sWork.cellsOf[i][1];
            cx1 = sWork.cellsOf[i][2];
            cy1 = sWork.cellsOf[i][3];
            for (cy = cy0; cy <= cy1; cy++) {
                for (cx = cx0; cx <= cx1; cx++) {
                    l->entry[l->start[cy * g->w + cx]++] = (uint16_t)i;
                }
            }
        }
        if (pass == 0) {
            memcpy(l->others, l->start, cells * sizeof(*l->others));
        }
    }
    for (c = (long)cells; c > 0; c--) {
        l->start[c] = l->start[c - 1];
    }
    l->start[0] = 0;
    return 1;
}

/* A triangle near another of another polygon ID (onlyD 0), or near one in
 * D (onlyD 1, in the grid of those). */
static int grid_near(const Grid *g, const GridLists *l, size_t i, int onlyD, unsigned common)
{
    const Box *bx = &sWork.box[i];
    uint64_t others = ~(1ull << sWork.id[i]);
    int isCommon = sWork.id[i] == common;
    long cx0, cy0, cx1, cy1, cx, cy, ix0, iy0, ix1, iy1;

    cells_of(g, bx, NEAR_PX, &cx0, &cy0, &cx1, &cy1);
    /* the cells wholly inside its box grown by NEAR_PX (not the edge
     * cells: they reach on past the grid): any triangle in one is near */
    ix0 = cx0 + 1;
    iy0 = cy0 + 1;
    ix1 = cx1 - 1;
    iy1 = cy1 - 1;
    ix0 = ix0 < 1 ? 1 : ix0;
    iy0 = iy0 < 1 ? 1 : iy0;
    ix1 = ix1 > g->w - 2 ? g->w - 2 : ix1;
    iy1 = iy1 > g->h - 2 ? g->h - 2 : iy1;
    for (cy = cy0; cy <= cy1; cy++) {
        int rowInside = cy >= iy0 && cy <= iy1;

        for (cx = cx0; cx <= cx1; cx++) {
            long cell = cy * g->w + cx;
            uint32_t e, end = l->start[cell + 1];

            if (onlyD ? end == l->start[cell] : !(sWork.cellIds[cell] & others)) {
                continue;
            }
            if (rowInside && cx >= ix0 && cx <= ix1) {
                return 1;
            }
            /* (the commonest ID's first: one of it needn't look at them) */
            for (e = !onlyD && isCommon ? l->others[cell] : l->start[cell]; e < end; e++) {
                uint16_t j = l->entry[e];

                if ((onlyD || sWork.id[j] != sWork.id[i]) && near_boxes(bx, &sWork.box[j])) {
                    return 1;
                }
            }
        }
    }
    return 0;
}

static void map_clear(const NitroGx3dPruneView *view, const Grid *g, long r0, long r1)
{
    long words = MAP_WORDS(view), mw = MAP_W(view), r, c, cyPrev = -1;
    uint32_t open[16];

    for (r = r0; r <= r1; r++) {
        long cy = cell_of((float)(r - 1), g->h), w;
        uint32_t *row = sWork.covered + r * words;

        if (cy != cyPrev) {
            /* (this row of cells' needed columns, as map words) */
            cyPrev = cy;
            memset(open, 0, sizeof(open));
            for (c = 0; c < mw;) {
                long cx = cell_of((float)(c - 1), g->w), end = c;

                while (end + 1 < mw && cell_of((float)end, g->w) == cx) {
                    end++;
                }
                if (sWork.cellNeed[cy * g->w + cx]) {
                    for (w = c >> 5; w <= end >> 5; w++) {
                        open[w] |= span_bits(c, end, w);
                    }
                }
                c = end + 1;
            }
        }
        for (w = 0; w < words; w++) {
            row[w] = ~open[w];
        }
    }
}

/* ------------------------------------------------------------------- it all */
int NitroPort_Gx3dPruneEdges(const NitroGx3dGpuVertex *vertices, const NitroGx3dBatch *batches,
                             size_t batch_count, const uint8_t *roles, const NitroGx3dPruneView *view,
                             uint16_t *indices, size_t capacity, NitroGx3dPruneList *lists,
                             NitroGx3dPruneResult *result)
{
    size_t n = 0, b, t, i, used = 0;
    int eye, eyes = view->eyes == 2 ? 2 : 1, z;
    unsigned common;
    float spill = 0.0f;
    long need0 = 1L << 30, need1 = -(1L << 30), words = MAP_WORDS(view);
    NitroGx3dPruneResult res;
    Grid grid;
    Zones zs;

    memset(&res, 0, sizeof(res));
    if (result) {
        *result = res;
    }
    if (view->width <= 0 || view->height <= 0 || words > 16 || !(view->x1 > view->x0) ||
        !(view->y1 > view->y0)) {
        return 0;
    }
    /* the outline passes' triangles, and where they are */
    for (b = 0; b < batch_count; b++) {
        if (roles[b] != NITRO_GX3D_PRUNE_OUT) {
            n += batches[b].count / 3u;
        }
    }
    if (n > 65535u || !room_for_triangles(n ? n : 1u)) {
        return 0;
    }
    n = 0;
    for (b = 0; b < batch_count; b++) {
        if (roles[b] == NITRO_GX3D_PRUNE_OUT) {
            continue;
        }
        if (batches[b].first + batches[b].count > 65536u) {
            return 0;   /* (beyond 16-bit indices) */
        }
        for (t = 0; t + 3u <= batches[b].count; t += 3u) {
            const NitroGx3dGpuVertex *v = vertices + batches[b].first + t;
            Box box = { 1.0e30f, 1.0e30f, -1.0e30f, -1.0e30f };
            int wild = 0;

            for (eye = 0; eye < eyes && !wild; eye++) {
                Corners *c = &sWork.corners[n * 2 + (size_t)eye];
                Box e;
                int k;

                for (k = 0; k < 3; k++) {
                    float moved;

                    if (!place(&v[k], view, view->stereo[eye], &c->x[k], &c->y[k], &moved)) {
                        wild = 1;
                        break;
                    }
                    spill = MAXF(spill, moved);
                }
                if (wild) {
                    break;
                }
                e = box_of(c);
                box.x0 = MINF(box.x0, e.x0);
                box.y0 = MINF(box.y0, e.y0);
                box.x1 = MAXF(box.x1, e.x1);
                box.y1 = MAXF(box.y1, e.y1);
            }
            if (wild) {
                /* (anywhere, for all that's known) */
                box.x0 = box.y0 = -(float)GRID_MARGIN;
                box.x1 = (float)(view->width + GRID_MARGIN);
                box.y1 = (float)(view->height + GRID_MARGIN);
            }
            sWork.first[n] = batches[b].first + (uint32_t)t;
            sWork.batch[n] = (uint16_t)b;
            sWork.id[n] = (uint8_t)((batches[b].polygon_attr >> 24) & 63u);
            sWork.role[n] = roles[b];
            sWork.flags[n] = wild ? (T_WILD | T_D | T_A) : 0;
            sWork.box[n] = box;
            n++;
        }
    }
    res.triangles = (unsigned)n;
    zones(view, spill, &zs);
    memcpy(res.zone, zs.out, sizeof(zs.out));
    /* D: another polygon ID near */
    grid.w = (long)((view->width + 2 * GRID_MARGIN) / CELL) + 1;
    grid.h = (long)((view->height + 2 * GRID_MARGIN) / CELL) + 1;
    {
        unsigned count[64], best = 0;

        memset(count, 0, sizeof(count));
        for (i = 0; i < n; i++) {
            count[sWork.id[i]]++;
        }
        for (i = 1; i < 64; i++) {
            if (count[i] > count[best]) {
                best = (unsigned)i;
            }
        }
        common = best;
    }
    if (!grid_fill(&grid, &sWork.all, n, 0, common)) {
        return 0;
    }
    for (i = 0; i < n; i++) {
        if (!(sWork.flags[i] & T_D) && grid_near(&grid, &sWork.all, i, 0, common)) {
            sWork.flags[i] |= T_D;
        }
    }
    /* D, or a zone's: the rear plane near, for the rest of another ID than
     * its. The map only where they can look (the grid's cells). */
    memset(sWork.cellNeed, 0, (size_t)(grid.w * grid.h));
    for (i = 0; i < n; i++) {
        Region q;
        long cx0, cy0, cx1, cy1, cx, cy;

        if ((sWork.flags[i] & T_D) || sWork.id[i] == view->clear_id) {
            continue;
        }
        q = query_of(view, &sWork.box[i]);
        if (q.c0 > q.c1 || q.r0 > q.r1) {
            continue;
        }
        need0 = q.r0 < need0 ? q.r0 : need0;
        need1 = q.r1 > need1 ? q.r1 : need1;
        /* (map column c is pixel c - 1) */
        cx0 = cell_of((float)(q.c0 - 1), grid.w);
        cx1 = cell_of((float)(q.c1 - 1), grid.w);
        cy0 = cell_of((float)(q.r0 - 1), grid.h);
        cy1 = cell_of((float)(q.r1 - 1), grid.h);
        for (cy = cy0; cy <= cy1; cy++) {
            for (cx = cx0; cx <= cx1; cx++) {
                sWork.cellNeed[cy * grid.w + cx] = 1;
            }
        }
    }
    if (need0 <= need1) {
        if (!grow((void **)&sWork.covered, &sWork.words, (size_t)(words * MAP_H(view)), sizeof(uint32_t)) ||
            !grow((void **)&sWork.rowOpen, &sWork.rows, (size_t)MAP_H(view) + 1u, sizeof(uint16_t)) ||
            !grow((void **)&sWork.rowsBefore, &sWork.rowsBeforeRoom, (size_t)MAP_H(view) + 1u, sizeof(uint16_t))) {
            return 0;
        }
        /* the solid triangles in a needed cell */
        for (i = 0; i < n; i++) {
            long cx, cy, needed = 0;

            sWork.lists[i] = 0;
            if (sWork.role[i] != NITRO_GX3D_PRUNE_SOLID || (sWork.flags[i] & T_WILD)) {
                continue;
            }
            for (cy = sWork.cellsOf[i][1]; cy <= sWork.cellsOf[i][3] && !needed; cy++) {
                for (cx = sWork.cellsOf[i][0]; cx <= sWork.cellsOf[i][2]; cx++) {
                    needed |= sWork.cellNeed[cy * grid.w + cx];
                }
            }
            sWork.lists[i] = (uint16_t)needed;   /* (for now: the lists come after) */
        }
        for (eye = 0; eye < eyes; eye++) {
            map_clear(view, &grid, need0, need1);
            for (i = 0; i < n; i++) {
                if (sWork.lists[i]) {
                    map_cover(view, need0, need1, &sWork.corners[i * 2 + (size_t)eye]);
                }
            }
            map_summarize(view, need0, need1, &zs);
            for (i = 0; i < n; i++) {
                Box e;
                Region q;

                if ((sWork.flags[i] & T_D) || sWork.id[i] == view->clear_id) {
                    continue;
                }
                e = box_of(&sWork.corners[i * 2 + (size_t)eye]);
                q = query_of(view, &e);
                if (map_uncovered(view, &q, &zs)) {
                    sWork.flags[i] |= T_D;   /* (not in a zone) */
                    continue;
                }
                for (z = 0; z < 4; z++) {
                    if (overlaps(&q, &zs.inner[z])) {
                        sWork.flags[i] |= (uint8_t)(T_ZONE << z);
                    }
                }
            }
        }
    }
    /* A: D, and near one in D */
    for (i = 0; i < n; i++) {
        if (sWork.flags[i] & T_D) {
            sWork.flags[i] |= T_A;
        }
    }
    if (!grid_fill(&grid, &sWork.ofD, n, 1, 64u)) {
        return 0;
    }
    for (i = 0; i < n; i++) {
        if (!(sWork.flags[i] & T_A) && grid_near(&grid, &sWork.ofD, i, 1, 64u)) {
            sWork.flags[i] |= T_A;
        }
    }
    /* each triangle's lists (bits), then the lists batch by batch */
    for (i = 0; i < n; i++) {
        uint8_t f = sWork.flags[i];
        unsigned in = 0;

        if (f & T_A) {
            in |= 1u << NITRO_GX3D_PRUNE_A;
        }
        if (f & T_D) {
            in |= 1u << NITRO_GX3D_PRUNE_D;
        } else {
            in |= (unsigned)(f >> 4) << NITRO_GX3D_PRUNE_ZONE_B;
        }
        if (!(f & T_A)) {
            for (z = 0; z < 4; z++) {
                if (touches_zone(&sWork.box[i], zs.out[z])) {
                    in |= 1u << (NITRO_GX3D_PRUNE_ZONE_A + z);
                }
            }
        }
        sWork.lists[i] = (uint16_t)in;
    }
    for (b = 0, t = 0; b < batch_count; b++) {
        size_t first = t, k;
        int l;

        memset(&lists[b], 0, sizeof(lists[b]));
        if (roles[b] == NITRO_GX3D_PRUNE_OUT) {
            continue;
        }
        while (t < n && sWork.batch[t] == b) {
            t++;
        }
        for (l = 0; l < NITRO_GX3D_PRUNE_LISTS; l++) {
            size_t start = (used + 7u) & ~(size_t)7u, count = 0;
            uint16_t bit = (uint16_t)(1u << l);

            for (k = first; k < t; k++) {
                if (!(sWork.lists[k] & bit)) {
                    continue;
                }
                if (start + count + 3u > capacity || count + 3u > 65535u) {
                    return 0;
                }
                indices[start + count] = (uint16_t)sWork.first[k];
                indices[start + count + 1] = (uint16_t)(sWork.first[k] + 1u);
                indices[start + count + 2] = (uint16_t)(sWork.first[k] + 2u);
                count += 3u;
            }
            lists[b].offset[l] = (uint32_t)start;
            lists[b].count[l] = (uint16_t)count;
            if (count) {
                used = start + count;
            }
            if (l == NITRO_GX3D_PRUNE_A) {
                res.a += (unsigned)(count / 3u);
            } else if (l == NITRO_GX3D_PRUNE_D) {
                res.d += (unsigned)(count / 3u);
            } else if (l < NITRO_GX3D_PRUNE_ZONE_B) {
                res.zone_a += (unsigned)(count / 3u);
            } else {
                res.zone_b += (unsigned)(count / 3u);
            }
        }
    }
    res.indices = used;
    if (result) {
        *result = res;
    }
    return 1;
}
