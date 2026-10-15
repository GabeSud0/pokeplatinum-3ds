/*
 * Wide art: hand-drawn pictures for the wider upper screen, and captures
 * for drawing them (include/nitro_port_wideart.h, docs/WIDE_ART.md).
 *
 * The folder's file names are read at load; a picture is read (PNG,
 * stb_image) the first time its layer shows, into one of a few cache
 * slots that are allocated once and never freed: a drawing thread may be
 * using a slot's pixels while another fills a different one.
 */
#include <nitro_port_wideart.h>
#include <nitro_port_render2d.h>
#include <nitro_port_wide.h>

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_NO_THREAD_LOCALS
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wtype-limits"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wcast-qual"
#endif
#include "../third_party/stb_image.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

void NitroPort_Log(const char *fmt, ...);
int NitroPort_EngineAOnTop(void);

#define MAX_PICTURES 512
/* Pictures kept read (120 KB each). The 3DS reads them ahead at the start
 * (NitroPort_WideArtPreload): one read as it first shows (a file from the
 * SD card, a PNG to decode) holds that screen's drawing up, and the
 * opening alone uses 17. */
#define CACHE_SLOTS 24
#define PIXELS (NITRO_WIDE_ART_W * NITRO_WIDE_ART_H)

typedef struct Picture {
    uint64_t key;
    int16_t hofs, vofs;
    uint8_t full, bad;
    /* (once read) its opaque pixels' columns: left .. right - 1 */
    uint8_t extentKnown;
    int16_t left, right;
    char name[48];
} Picture;

typedef struct Slot {
    uint64_t key;          /* 0: empty */
    int16_t hofs, vofs;    /* (with key: which of its pictures) */
    uint8_t full;
    int16_t left, right;   /* its opaque pixels' columns: left .. right - 1 */
    uint32_t lastUse;
    uint16_t *pixels;      /* allocated once */
} Slot;

static Picture sPictures[MAX_PICTURES];
static int sCount;
static char sDir[256];
static Slot sSlots[CACHE_SLOTS];
static uint32_t sUse;
static volatile uint32_t sSerial;
static volatile int sLock;

static void lock(void)
{
    while (__atomic_test_and_set(&sLock, __ATOMIC_ACQUIRE)) {
    }
}

static void unlock(void)
{
    __atomic_clear(&sLock, __ATOMIC_RELEASE);
}

/* ------------------------------------------------------------- the folder */

/* (not ctype's: those are a table in newlib the libraries' check doesn't
 * expect, tools/check_sdk_coverage.py) */
static int is_digit(int c)
{
    return c >= '0' && c <= '9';
}

int NitroPort_WideArtParseName(const char *name, uint64_t *key, int *hofs, int *vofs, int *full)
{
    const char *p = name;
    uint64_t k = 0;
    int i, h = 0, v = 0, f = 0;

    for (i = 0; i < 16; i++, p++) {
        int c = (*p >= 'A' && *p <= 'F') ? *p - 'A' + 'a' : *p, d;

        if (c >= '0' && c <= '9') {
            d = c - '0';
        } else if (c >= 'a' && c <= 'f') {
            d = c - 'a' + 10;
        } else {
            return 0;
        }
        k = (k << 4) | (uint64_t)d;
    }
    if (*p++ != '_' || !is_digit(*p)) {
        return 0;
    }
    while (is_digit(*p)) {
        h = h * 10 + (*p++ - '0');
    }
    if (*p++ != '_' || !is_digit(*p)) {
        return 0;
    }
    while (is_digit(*p)) {
        v = v * 10 + (*p++ - '0');
    }
    if (strncmp(p, "_full", 5) == 0) {
        f |= NITRO_WIDE_ART_FULL;
        p += 5;
    }
    if (strncmp(p, "_loop", 5) == 0) {
        f |= NITRO_WIDE_ART_LOOP;
        p += 5;
    }
    if (strcmp(p, ".png") != 0 && strcmp(p, ".PNG") != 0) {
        return 0;
    }
    if (k == 0 || h > 511 || v > 511) {
        return 0;
    }
    *key = k;
    *hofs = h;
    *vofs = v;
    *full = f;
    return 1;
}

static int by_key(const void *a, const void *b)
{
    const Picture *pa = a, *pb = b;

    if (pa->key != pb->key) {
        return pa->key < pb->key ? -1 : 1;
    }
    if (pa->vofs != pb->vofs) {
        return pa->vofs - pb->vofs;
    }
    if (pa->hofs != pb->hofs) {
        return pa->hofs - pb->hofs;
    }
    return (int)pa->full - (int)pb->full;
}

int NitroPort_WideArtLoad(const char *dir)
{
    DIR *d;
    struct dirent *ent;
    int n = 0, i;

    lock();
    snprintf(sDir, sizeof(sDir), "%s", dir);
    d = opendir(dir);
    if (d != NULL) {
        while ((ent = readdir(d)) != NULL && n < MAX_PICTURES) {
            Picture *p = &sPictures[n];
            int h, v, f;

            if (strlen(ent->d_name) >= sizeof(p->name) ||
                !NitroPort_WideArtParseName(ent->d_name, &p->key, &h, &v, &f)) {
                continue;
            }
            p->hofs = (int16_t)h;
            p->vofs = (int16_t)v;
            p->full = (uint8_t)f;
            p->bad = 0;
            p->extentKnown = 0;
            snprintf(p->name, sizeof(p->name), "%s", ent->d_name);
            n++;
        }
        closedir(d);
    }
    /* sorted; of two files for one key, the whole picture last (found) */
    qsort(sPictures, (size_t)n, sizeof(sPictures[0]), by_key);
    sCount = n;
    for (i = 0; i < CACHE_SLOTS; i++) {
        sSlots[i].key = 0;
    }
    sSerial++;
    unlock();
    if (n > 0) {
        NitroPort_Log("[wide art] %d hand-drawn pictures in %s", n, dir);
    }
    return n;
}

static const uint16_t *picture_pixels(int index);

int NitroPort_WideArtPreload(int max)
{
    int i, n, read = 0;

    lock();
    n = sCount;
    unlock();
    if (max > CACHE_SLOTS) {
        max = CACHE_SLOTS;
    }
    for (i = 0; i < n && i < max; i++) {
        lock();
        if (i >= sCount || sPictures[i].bad) {
            unlock();
            continue;
        }
        if (picture_pixels(i) != NULL) {   /* (unlocks) */
            read++;
        }
    }
    return read;
}

const char *NitroPort_WideArtDir(void)
{
    return sDir;
}

int NitroPort_WideArtCount(void)
{
    return __atomic_load_n(&sCount, __ATOMIC_RELAXED);
}

uint32_t NitroPort_WideArtSerial(void)
{
    return sSerial;
}

/* The first picture of a key: -1 if none. */
static int first_picture(uint64_t key)
{
    int lo = 0, hi = sCount - 1, found = -1;

    while (lo <= hi) {
        int mid = (lo + hi) / 2;

        if (sPictures[mid].key < key) {
            lo = mid + 1;
        } else {
            if (sPictures[mid].key == key) {
                found = mid;
            }
            hi = mid - 1;
        }
    }
    return found;
}

/* The last picture of a key (sorted: the whole one if there's both). */
static int find_picture(uint64_t key)
{
    int lo = 0, hi = sCount - 1, found = -1;

    while (lo <= hi) {
        int mid = (lo + hi) / 2;

        if (sPictures[mid].key < key) {
            lo = mid + 1;
        } else if (sPictures[mid].key > key) {
            hi = mid - 1;
        } else {
            found = mid;
            lo = mid + 1;
        }
    }
    return found;
}

/* A picture's file into RGB555 | 0x8000 pixels (0: transparent). */
static int read_picture(const char *path, uint16_t *out)
{
    FILE *f = fopen(path, "rb");
    unsigned char *bytes, *rgba;
    long size;
    int w, h, n, i;

    if (f == NULL) {
        return 0;
    }
    (void)setvbuf(f, NULL, _IONBF, 0);   /* (read whole: one request, not one a KB) */
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0 || size > 8 * 1024 * 1024) {
        fclose(f);
        return 0;
    }
    bytes = malloc((size_t)size);
    if (bytes == NULL || fread(bytes, 1, (size_t)size, f) != (size_t)size) {
        free(bytes);
        fclose(f);
        return 0;
    }
    fclose(f);
    rgba = stbi_load_from_memory(bytes, (int)size, &w, &h, &n, 4);
    free(bytes);
    if (rgba == NULL) {
        return 0;
    }
    if (w != NITRO_WIDE_ART_W || h != NITRO_WIDE_ART_H) {
        stbi_image_free(rgba);
        return 0;
    }
    for (i = 0; i < PIXELS; i++) {
        const unsigned char *p = rgba + i * 4;

        out[i] = p[3] < 128 ? 0
                            : (uint16_t)(0x8000 | (p[0] >> 3) | ((p[1] >> 3) << 5) | ((p[2] >> 3) << 10));
    }
    stbi_image_free(rgba);
    return 1;
}

/* Picture index's pixels (read the first time, kept in a slot); under the
 * lock on entry, not on return. NULL if it can't be read. */
static const uint16_t *picture_pixels(int index)
{
    int i, oldest = 0;
    Picture pic = sPictures[index];
    char path[320];
    uint16_t *pixels;

    for (i = 0; i < CACHE_SLOTS; i++) {
        if (sSlots[i].key == pic.key && sSlots[i].hofs == pic.hofs && sSlots[i].vofs == pic.vofs &&
            sSlots[i].full == pic.full && sSlots[i].pixels != NULL) {
            sSlots[i].lastUse = ++sUse;
            pixels = sSlots[i].pixels;
            sPictures[index].left = sSlots[i].left;
            sPictures[index].right = sSlots[i].right;
            sPictures[index].extentKnown = 1;
            unlock();
            return pixels;
        }
    }
    snprintf(path, sizeof(path), "%s/%s", sDir, pic.name);
    unlock();

    /* Not read yet: read it (outside the lock: it takes a while), then
     * into the slot used longest ago. */
    pixels = malloc(PIXELS * sizeof(uint16_t));
    if (pixels == NULL) {
        return NULL;
    }
    if (!read_picture(path, pixels)) {
        free(pixels);
        lock();
        if (index < sCount && strcmp(sPictures[index].name, pic.name) == 0) {
            sPictures[index].bad = 1;
        }
        unlock();
        NitroPort_Log("[wide art] can't read %s (a PNG of %dx%d?)", path, NITRO_WIDE_ART_W, NITRO_WIDE_ART_H);
        return NULL;
    }
    lock();
    for (i = 0; i < CACHE_SLOTS; i++) {
        if (sSlots[i].key == 0) {
            oldest = i;
            break;
        }
        if (sSlots[i].lastUse < sSlots[oldest].lastUse) {
            oldest = i;
        }
    }
    if (sSlots[oldest].pixels == NULL) {
        sSlots[oldest].pixels = pixels;
        pixels = NULL;
    } else {
        memcpy(sSlots[oldest].pixels, pixels, PIXELS * sizeof(uint16_t));
    }
    sSlots[oldest].key = pic.key;
    sSlots[oldest].hofs = pic.hofs;
    sSlots[oldest].vofs = pic.vofs;
    sSlots[oldest].full = pic.full;
    sSlots[oldest].lastUse = ++sUse;
    {
        const uint16_t *px = sSlots[oldest].pixels;
        int x, y, left = NITRO_WIDE_ART_W, right = 0;

        for (y = 0; y < NITRO_WIDE_ART_H; y++) {
            for (x = 0; x < NITRO_WIDE_ART_W; x++) {
                if (px[y * NITRO_WIDE_ART_W + x]) {
                    if (x < left) left = x;
                    if (x >= right) right = x + 1;
                }
            }
        }
        sSlots[oldest].left = (int16_t)left;
        sSlots[oldest].right = (int16_t)right;
        if (index < sCount && strcmp(sPictures[index].name, pic.name) == 0) {
            sPictures[index].left = (int16_t)left;
            sPictures[index].right = (int16_t)right;
            sPictures[index].extentKnown = 1;
        }
    }
    {
        const uint16_t *result = sSlots[oldest].pixels;

        unlock();
        free(pixels);
        return result;
    }
}

const uint16_t *NitroPort_WideArtFind(uint64_t key, int *hofs, int *vofs, int *full)
{
    int index;

    if (key == 0 || NitroPort_WideArtCount() == 0) {
        return NULL;
    }
    lock();
    index = find_picture(key);
    if (index < 0 || sPictures[index].bad) {
        unlock();
        return NULL;
    }
    *hofs = sPictures[index].hofs;
    *vofs = sPictures[index].vofs;
    *full = sPictures[index].full;
    return picture_pixels(index);
}

int NitroPort_WideArtExtent(const uint16_t *pixels, int *left, int *right)
{
    int i, found = 0;

    if (pixels == NULL) {
        return 0;
    }
    lock();
    for (i = 0; i < CACHE_SLOTS; i++) {
        if (sSlots[i].key != 0 && sSlots[i].pixels == pixels) {
            *left = sSlots[i].left;
            *right = sSlots[i].right;
            found = 1;
            break;
        }
    }
    unlock();
    return found;
}

/* The last ART_ENTRY pixels of the DS's slide (before what's coming in is
 * whole on the DS's screen) cover twice that on the wide screen. */
#define ART_DS0 ((NITRO_WIDE_ART_W - 256) / 2)
#define ART_DS1 (ART_DS0 + 256)
#define ART_ENTRY 32

/* d (>= 0: how far the picture's left edge is from where it comes in, the
 * DS's edge at ds, the wide screen's at wide: ds < wide) widened. */
static int widen(int d, int ds, int wide)
{
    int d0 = ds - ART_ENTRY;

    if (d0 < 0) {
        d0 = 0;
    }
    if (d <= d0) {
        return d;
    }
    if (d >= ds) {
        return d + (wide - ds);
    }
    return d0 + ((d - d0) * (wide - d0) + (ds - d0) / 2) / (ds - d0);
}

int NitroPort_WideArtMovedOffset(int moved, int left, int right)
{
    if (moved > 0 && left < ART_DS1 && left < right) {
        /* coming in from (or going out to) the right */
        return widen(moved, ART_DS1 - left, NITRO_WIDE_ART_W - left);
    }
    if (moved < 0 && right > ART_DS0 && left < right) {
        return -widen(-moved, right - ART_DS0, right);
    }
    return moved;
}

/* a - b round a circle of period, -period/2 .. period/2 - 1 */
static int wrapped(int a, int b, int period)
{
    int d = ((a - b) % period + period) % period;

    return d >= period / 2 ? d - period : d;
}

/* (a whole picture away from its place, moved with its layer) Whether the
 * picture's own edge would show: moved left (dx > 0, the layer scrolled
 * right) its right edge comes in, moved right its left edge; it shows if
 * the picture has something there (what it shows went on past it: the
 * opening's Torterra, cut at the wide screen's edge). Another of its
 * pictures, made further along, has that part. */
#define REST 8
static int edge_shows(const Picture *p, int dx)
{
    if (!(p->full & NITRO_WIDE_ART_FULL) || (p->full & NITRO_WIDE_ART_LOOP) || !p->extentKnown ||
        (dx >= -REST && dx <= REST)) {
        return 0;
    }
    return dx > 0 ? p->right >= NITRO_WIDE_ART_W : p->left <= 0;
}

#define MAX_NEAR 16
const uint16_t *NitroPort_WideArtFindNear(uint64_t key, int curH, int curV, int periodH, int periodV, int maxH,
                                          int maxV, int *hofs, int *vofs, int *full)
{
    int i, n = 0, k, best = -1, bestDist = 0, bestShows = 0;
    int cand[MAX_NEAR], candDx[MAX_NEAR], candDist[MAX_NEAR];

    if (key == 0 || NitroPort_WideArtCount() == 0) {
        return NULL;
    }
    lock();
    i = first_picture(key);
    for (; i >= 0 && i < sCount && sPictures[i].key == key && n < MAX_NEAR; i++) {
        int dx = wrapped(curH, sPictures[i].hofs, periodH), dy = wrapped(curV, sPictures[i].vofs, periodV);

        if (sPictures[i].bad || (maxH > 0 && (dx >= maxH || dx <= -maxH)) ||
            (maxV > 0 && (dy >= maxV || dy <= -maxV))) {
            continue;
        }
        cand[n] = i;
        candDx[n] = dx;
        candDist[n] = (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
        n++;
    }
    /* (a BG's, with more than one to choose from: their edges known, each
     * read once) */
    if (maxH > 0 && n > 1) {
        for (k = 0; k < n; k++) {
            if (!sPictures[cand[k]].extentKnown && (sPictures[cand[k]].full & NITRO_WIDE_ART_FULL)) {
                picture_pixels(cand[k]);   /* (unlocks) */
                lock();
                if (cand[k] >= sCount || sPictures[cand[k]].key != key) {
                    unlock();
                    return NULL;           /* (loaded again meanwhile) */
                }
            }
        }
    }
    for (k = 0; k < n; k++) {
        const Picture *p = &sPictures[cand[k]];
        int shows = maxH > 0 && n > 1 && edge_shows(p, candDx[k]), dist = candDist[k];

        /* the nearest whose own edge doesn't show; of two at the same
         * place, the whole picture (sorted after) */
        if (best >= 0) {
            if (shows != bestShows) {
                if (shows) {
                    continue;
                }
            } else if (dist > bestDist) {
                continue;
            } else if (dist == bestDist && (p->hofs != sPictures[best].hofs || p->vofs != sPictures[best].vofs)) {
                continue;
            }
        }
        best = cand[k];
        bestDist = dist;
        bestShows = shows;
    }
    if (best < 0) {
        unlock();
        return NULL;
    }
    *hofs = sPictures[best].hofs;
    *vofs = sPictures[best].vofs;
    *full = sPictures[best].full;
    return picture_pixels(best);
}

/* ------------------------------------------------------------------- PNG */

static uint32_t sCrcTable[256];

static uint32_t crc(uint32_t c, const uint8_t *p, size_t n)
{
    size_t i;

    if (sCrcTable[1] == 0) {
        uint32_t k;

        for (k = 0; k < 256; k++) {
            uint32_t v = k;
            int b;

            for (b = 0; b < 8; b++) {
                v = (v & 1) ? 0xEDB88320u ^ (v >> 1) : v >> 1;
            }
            sCrcTable[k] = v;
        }
    }
    c ^= 0xFFFFFFFFu;
    for (i = 0; i < n; i++) {
        c = sCrcTable[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFu;
}

static void be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static int chunk(FILE *f, const char *type, const uint8_t *data, uint32_t n)
{
    uint8_t head[8], tail[4];
    uint32_t c;

    be32(head, n);
    memcpy(head + 4, type, 4);
    c = crc(0, head + 4, 4);
    c = crc(c, data, n);
    be32(tail, c);
    return fwrite(head, 1, 8, f) == 8 && (n == 0 || fwrite(data, 1, n, f) == n) && fwrite(tail, 1, 4, f) == 4;
}

/* RGBA as a PNG, its pixels stored (deflate without compression: no
 * zlib needed, and these are small). */
int NitroPort_WideArtWritePng(const char *path, const uint8_t *rgba, int w, int h)
{
    static const uint8_t magic[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
    size_t raw = (size_t)h * (size_t)(1 + w * 4), blocks = (raw + 65534) / 65535, pos = 0, out = 0;
    uint8_t *idat = malloc(2 + raw + blocks * 5 + 4), ihdr[13];
    uint32_t a = 1, b = 0;
    FILE *f;
    int ok;

    if (idat == NULL) {
        return -1;
    }
    idat[out++] = 0x78;
    idat[out++] = 0x01;
    while (pos < raw) {
        size_t n = raw - pos > 65535 ? 65535 : raw - pos, i;

        idat[out++] = (uint8_t)(pos + n == raw);
        idat[out++] = (uint8_t)n;
        idat[out++] = (uint8_t)(n >> 8);
        idat[out++] = (uint8_t)~n;
        idat[out++] = (uint8_t)(~n >> 8);
        for (i = 0; i < n; i++, pos++) {
            /* each row: filter 0, then its pixels */
            size_t row = pos / (size_t)(1 + w * 4), col = pos % (size_t)(1 + w * 4);
            uint8_t v = col == 0 ? 0 : rgba[row * (size_t)w * 4 + col - 1];

            idat[out++] = v;
            a = (a + v) % 65521;
            b = (b + a) % 65521;
        }
    }
    be32(idat + out, (b << 16) | a);
    out += 4;
    be32(ihdr, (uint32_t)w);
    be32(ihdr + 4, (uint32_t)h);
    ihdr[8] = 8;    /* 8 bits */
    ihdr[9] = 6;    /* RGBA */
    ihdr[10] = 0;
    ihdr[11] = 0;
    ihdr[12] = 0;
    f = fopen(path, "wb");
    if (f == NULL) {
        free(idat);
        return -1;
    }
    ok = fwrite(magic, 1, 8, f) == 8 && chunk(f, "IHDR", ihdr, 13) && chunk(f, "IDAT", idat, (uint32_t)out) &&
         chunk(f, "IEND", NULL, 0);
    free(idat);
    return fclose(f) == 0 && ok ? 0 : -1;
}

/* --------------------------------------------------------------- capture */

static void to_rgba(const uint16_t *src, uint8_t *dst, int n)
{
    int i;

    for (i = 0; i < n; i++) {
        uint16_t c = src[i];
        int r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;

        dst[i * 4 + 0] = (uint8_t)((r << 3) | (r >> 2));
        dst[i * 4 + 1] = (uint8_t)((g << 3) | (g >> 2));
        dst[i * 4 + 2] = (uint8_t)((b << 3) | (b >> 2));
        dst[i * 4 + 3] = (c & 0x8000) ? 255 : 0;
    }
}

int NitroPort_WideArtCapture(const char *dir, char *made, size_t madeSize)
{
    static uint16_t under[PIXELS], over[PIXELS], layer[PIXELS];
    static uint8_t overAlpha[PIXELS], rgba[PIXELS * 4];
    char folder[300], path[340];
    NitroRender2DSplit split;
    FILE *info;
    int engine = NitroPort_EngineAOnTop() ? 0 : 1, n, i, bg;
    struct stat st;

    if (NitroPort_WideScreenExtra() * 2 + 256 != NITRO_WIDE_ART_W) {
        NitroPort_Log("[wide art] capturing needs the remaster's wide upper screen");
        return -1;
    }
    mkdir(dir, 0777);
    snprintf(folder, sizeof(folder), "%s/captures", dir);
    mkdir(folder, 0777);
    for (n = 1; n < 10000; n++) {
        snprintf(folder, sizeof(folder), "%s/captures/%04d", dir, n);
        if (stat(folder, &st) != 0) {
            break;
        }
    }
    if (n >= 10000 || mkdir(folder, 0777) != 0) {
        NitroPort_Log("[wide art] can't make a capture folder in %s", dir);
        return -1;
    }
    /* the whole picture (the layers behind and in front of the 3D; no 3D) */
    memset(&split, 0, sizeof(split));
    split.over = over;
    split.overAlpha = overAlpha;
    NitroPort_WideLayout(engine, &split);
    NitroPort_Render2DSplit(engine, under, &split);
    for (i = 0; i < PIXELS; i++) {
        uint16_t c = (uint16_t)(under[i] | 0x8000);

        if (split.has3d && (over[i] & 0x8000)) {
            int a = overAlpha[i] > 16 ? 16 : overAlpha[i], k;
            uint16_t m = 0x8000;

            for (k = 0; k < 15; k += 5) {
                m |= (uint16_t)(((((over[i] >> k) & 31) * a + ((c >> k) & 31) * (16 - a)) / 16) << k);
            }
            c = m;
        }
        layer[i] = c;
    }
    to_rgba(layer, rgba, PIXELS);
    snprintf(path, sizeof(path), "%s/screen.png", folder);
    if (NitroPort_WideArtWritePng(path, rgba, NITRO_WIDE_ART_W, NITRO_WIDE_ART_H) != 0) {
        NitroPort_Log("[wide art] can't write %s", path);
        return -1;
    }
    snprintf(path, sizeof(path), "%s/info.txt", folder);
    info = fopen(path, "w");
    if (info == NULL) {
        return -1;
    }
    fprintf(info, "# pokeplatinum-3ds wide art capture (docs/WIDE_ART.md)\n");
    fprintf(info, "size %d %d\n", NITRO_WIDE_ART_W, NITRO_WIDE_ART_H);
    fprintf(info, "ds %d %d\n", NitroPort_WideScreenExtra(), 256);
    fprintf(info, "engine %c\n", engine ? 'B' : 'A');
    fprintf(info, "has3d %d\n", split.has3d ? 1 : 0);
    fprintf(info, "backdrop %04x\n", split.backdrop & 0x7FFF);
    for (bg = 0; bg < 4; bg++) {
        uint64_t key;
        int hofs, vofs, prio;

        memset(&split, 0, sizeof(split));
        NitroPort_WideLayout(engine, &split);
        if (!NitroPort_Render2DLayer(engine, bg, &split, layer, &key, &hofs, &vofs, &prio)) {
            continue;
        }
        to_rgba(layer, rgba, PIXELS);
        snprintf(path, sizeof(path), "%s/bg%d.png", folder, bg);
        if (NitroPort_WideArtWritePng(path, rgba, NITRO_WIDE_ART_W, NITRO_WIDE_ART_H) != 0) {
            fclose(info);
            return -1;
        }
        /* and the BG's map going on beyond the DS's edges (the game's own
         * art there, when it has any: what filling the bands can start from) */
        memset(&split, 0, sizeof(split));
        NitroPort_WideLayout(engine, &split);
        split.clip[bg] = 0;
        if (NitroPort_Render2DLayer(engine, bg, &split, layer, &key, &hofs, &vofs, &prio)) {
            to_rgba(layer, rgba, PIXELS);
            snprintf(path, sizeof(path), "%s/bg%d_map.png", folder, bg);
            if (NitroPort_WideArtWritePng(path, rgba, NITRO_WIDE_ART_W, NITRO_WIDE_ART_H) != 0) {
                fclose(info);
                return -1;
            }
        }
        fprintf(info, "bg %d prio %d key %016llx hofs %d vofs %d file bg%d.png map bg%d_map.png\n", bg, prio,
                (unsigned long long)key, hofs, vofs, bg, bg);
    }
    {
        /* the OBJs, group by group (a scene's sprite background is one) */
        NitroObjGroup groups[32];
        int count;

        memset(&split, 0, sizeof(split));
        NitroPort_WideLayout(engine, &split);
        count = NitroPort_Render2DObjGroups(engine, &split, groups, 32);
        for (i = 0; i < count; i++) {
            if (!NitroPort_Render2DObjLayer(engine, groups[i].group, &split, layer)) {
                continue;
            }
            to_rgba(layer, rgba, PIXELS);
            snprintf(path, sizeof(path), "%s/obj%d.png", folder, groups[i].group);
            if (NitroPort_WideArtWritePng(path, rgba, NITRO_WIDE_ART_W, NITRO_WIDE_ART_H) != 0) {
                fclose(info);
                return -1;
            }
            fprintf(info, "obj %d prio %d key %016llx x %d y %d count %d file obj%d.png\n", groups[i].group,
                    groups[i].prio, (unsigned long long)groups[i].key, groups[i].x, groups[i].y, groups[i].count,
                    groups[i].group);
        }
    }
    fclose(info);
    if (made != NULL && madeSize > 0) {
        snprintf(made, madeSize, "%s", folder);
    }
    NitroPort_Log("[wide art] captured the upper screen into %s", folder);
    return 0;
}
