/*
 * Wide art: hand-drawn pictures for the wider upper screen, and captures
 * for drawing them (include/nitro_port_wideart.h, docs/WIDE_ART.md).
 *
 * The built-in pictures' and the folder's file names are read at load; a
 * picture is read (PNG, stb_image) the first time its layer shows, into
 * one of a few cache slots that are allocated once and never freed: a
 * drawing thread may be using a slot's pixels while another fills a
 * different one.
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
    int16_t builtIn;       /* its NitroPort_WideArtBuiltIn file, or -1: the folder's */
    uint8_t full, bad;
    uint8_t shared;        /* (the folder's) in its share/, as it is now */
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
    uint8_t rowUsed[NITRO_WIDE_ART_H];   /* (each row: anything in it, a pixel of its own or the game's) */
} Slot;

static Picture sPictures[MAX_PICTURES];
static int sCount, sUnshared;
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

/* (the program's: the build makes one from wideart/, tools/embed_wideart.py) */
__attribute__((weak)) const NitroWideArtFile *NitroPort_WideArtBuiltIn(int *count)
{
    *count = 0;
    return NULL;
}

/* Whether the folder's file name is in its share/ as it is now (written
 * after the file last changed: a second after, the SD card's times being
 * that coarse; else it's shared again, which is no harm). */
static int shared_now(const char *dir, const char *name)
{
    char path[400];
    struct stat file, copy;

    snprintf(path, sizeof(path), "%s/%s", dir, name);
    if (stat(path, &file) != 0) {
        return 0;
    }
    snprintf(path, sizeof(path), "%s/share/%s", dir, name);
    return stat(path, &copy) == 0 && copy.st_mtime > file.st_mtime;
}

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
    const NitroWideArtFile *files;
    int n = 0, i, k, files_n = 0, built = 0, local = 0, unshared = 0, used = 0, listed = 0;
    char line[256];

    lock();
    snprintf(sDir, sizeof(sDir), "%s", dir);
    files = NitroPort_WideArtBuiltIn(&files_n);
    for (k = 0; k < files_n && n < MAX_PICTURES; k++) {
        Picture *p = &sPictures[n];
        int h, v, f;

        if (strlen(files[k].name) >= sizeof(p->name) ||
            !NitroPort_WideArtParseName(files[k].name, &p->key, &h, &v, &f)) {
            continue;
        }
        p->hofs = (int16_t)h;
        p->vofs = (int16_t)v;
        p->full = (uint8_t)f;
        p->bad = 0;
        p->extentKnown = 0;
        p->builtIn = (int16_t)k;
        p->shared = 1;
        snprintf(p->name, sizeof(p->name), "%s", files[k].name);
        n++;
        built++;
    }
    d = opendir(dir);
    if (d != NULL) {
        while ((ent = readdir(d)) != NULL && n < MAX_PICTURES) {
            Picture *p;
            uint64_t key;
            int h, v, f;

            if (strlen(ent->d_name) >= sizeof(p->name) ||
                !NitroPort_WideArtParseName(ent->d_name, &key, &h, &v, &f)) {
                continue;
            }
            /* (the folder's in the place of a built-in one of its name) */
            for (i = 0; i < n && !(sPictures[i].builtIn >= 0 && strcmp(sPictures[i].name, ent->d_name) == 0); i++) {
            }
            p = &sPictures[i];
            if (i == n) {
                n++;
            } else {
                built--;
            }
            p->key = key;
            p->hofs = (int16_t)h;
            p->vofs = (int16_t)v;
            p->full = (uint8_t)f;
            p->bad = 0;
            p->extentKnown = 0;
            p->builtIn = -1;
            snprintf(p->name, sizeof(p->name), "%s", ent->d_name);
            p->shared = (uint8_t)shared_now(dir, ent->d_name);
            unshared += !p->shared;
            local++;
        }
        closedir(d);
    }
    /* sorted; of two files for one key, the whole picture last (found) */
    qsort(sPictures, (size_t)n, sizeof(sPictures[0]), by_key);
    sCount = n;
    sUnshared = unshared;
    for (i = 0; i < CACHE_SLOTS; i++) {
        sSlots[i].key = 0;
    }
    sSerial++;
    /* (which aren't shared: to go to their screens; the share folder has
     * the rest) */
    line[0] = 0;
    for (i = 0; i < n && used < (int)sizeof(line) - 1; i++) {
        if (sPictures[i].builtIn < 0 && !sPictures[i].shared) {
            int wrote = snprintf(line + used, sizeof(line) - (size_t)used, "%s%s", listed ? ", " : "",
                                 sPictures[i].name);

            if (wrote < 0 || used + wrote >= (int)sizeof(line)) {
                break;
            }
            used += wrote;
            listed++;
        }
    }
    unlock();
    if (n > 0) {
        NitroPort_Log("[wide art] %d hand-drawn pictures: %d built in, %d in %s (%d of those not shared yet)", n,
                      built, local, dir, unshared);
    }
    if (unshared > 0) {
        NitroPort_Log("[wide art] not shared yet (the PC runner writes them into %s/share as their screens show, "
                      "docs/WIDE_ART.md): %s%s", dir, line, listed < unshared ? ", ..." : "");
    }
    /* the layouts' pictures: read now, so none holds a screen up later */
    for (i = 1; i < NITRO_WIDE_PICTURES; i++) {
        int w, h, dx, dy;

        (void)NitroPort_WideArtPicture(i, &w, &h, &dx, &dy);
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

/* A file's bytes (malloc'd; NULL if it can't be read), and how many. */
static unsigned char *read_file(const char *path, long max, long *size)
{
    FILE *f = fopen(path, "rb");
    unsigned char *bytes;

    if (f == NULL) {
        return NULL;
    }
    (void)setvbuf(f, NULL, _IONBF, 0);   /* (read whole: one request, not one a KB) */
    fseek(f, 0, SEEK_END);
    *size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (*size <= 0 || *size > max) {
        fclose(f);
        return NULL;
    }
    bytes = malloc((size_t)*size);
    if (bytes == NULL || fread(bytes, 1, (size_t)*size, f) != (size_t)*size) {
        free(bytes);
        fclose(f);
        return NULL;
    }
    fclose(f);
    return bytes;
}

/* A picture's PNG into RGB555 | 0x8000 pixels (0: transparent; alpha 64:
 * NITRO_WIDE_ART_GAME, the layer's own, red and green how far it was
 * moved: nitro_port_wideart.h). */
static uint16_t game_pixel(const unsigned char *p)
{
    int dx = (int)p[0] + ((int)p[2] << 8) - 256, dy = (int)p[1] - 128;

    if (dx < -NITRO_WIDE_ART_MOVE_MAX || dx > NITRO_WIDE_ART_MOVE_MAX) {
        dx = 0;
    }
    if (dy < -NITRO_WIDE_ART_MOVE_UP_MAX || dy >= NITRO_WIDE_ART_MOVE_UP_MAX) {
        dy = 0;
    }
    return NitroPort_WideArtGameAt(dx, dy);
}

static int decode_picture(const unsigned char *bytes, long size, uint16_t *out)
{
    unsigned char *rgba;
    int w, h, n, i;

    rgba = stbi_load_from_memory(bytes, (int)size, &w, &h, &n, 4);
    if (rgba == NULL) {
        return 0;
    }
    if (w != NITRO_WIDE_ART_W || h != NITRO_WIDE_ART_H) {
        stbi_image_free(rgba);
        return 0;
    }
    for (i = 0; i < PIXELS; i++) {
        const unsigned char *p = rgba + i * 4;

        out[i] = p[3] == 64  ? game_pixel(p)
                 : p[3] < 128 ? 0
                              : (uint16_t)(0x8000 | (p[0] >> 3) | ((p[1] >> 3) << 5) | ((p[2] >> 3) << 10));
    }
    stbi_image_free(rgba);
    return 1;
}

/* Picture pic's pixels: its built-in file's, or its file's in the folder. */
static int read_picture(const Picture *pic, const char *dir, uint16_t *out)
{
    char path[400];
    unsigned char *bytes;
    long size = 0;
    int ok;

    if (pic->builtIn >= 0) {
        int count;
        const NitroWideArtFile *files = NitroPort_WideArtBuiltIn(&count);

        return pic->builtIn < count && decode_picture(files[pic->builtIn].data, (long)files[pic->builtIn].size, out);
    }
    snprintf(path, sizeof(path), "%s/%s", dir, pic->name);
    bytes = read_file(path, 8 * 1024 * 1024, &size);
    if (bytes == NULL) {
        return 0;
    }
    ok = decode_picture(bytes, size, out);
    free(bytes);
    return ok;
}

/* ---------------------------------------------- the layouts' pictures */

/* (NitroPort_WideArtPicture) Each picture as read for the folder loaded
 * (serial): its pixels, kept (a drawing thread may be using them: a new
 * read goes into them if it fits, else into new ones, the old ones left). */
static struct {
    uint16_t *pixels;
    int capacity, w, h, dx, dy;
    uint32_t serial;
    uint8_t state;   /* for serial: 0 not read yet, 1 read, 2 none */
} sNamed[NITRO_WIDE_PICTURES];

/* A PNG's oFFs chunk (where its top-left goes, in pixels; the editor
 * writes it for a picture moved from its layout's place): 1 if it has
 * one. */
static int png_offset(const unsigned char *bytes, long size, int *x, int *y)
{
    long at = 8;

    while (at + 12 <= size) {
        uint32_t len = ((uint32_t)bytes[at] << 24) | ((uint32_t)bytes[at + 1] << 16) |
                       ((uint32_t)bytes[at + 2] << 8) | bytes[at + 3];
        const unsigned char *type = bytes + at + 4, *d = bytes + at + 8;

        if (len > (uint32_t)(size - at - 12)) {
            return 0;
        }
        if (memcmp(type, "IDAT", 4) == 0 || memcmp(type, "IEND", 4) == 0) {
            return 0;   /* (it comes before the pixels) */
        }
        if (memcmp(type, "oFFs", 4) == 0 && len == 9 && d[8] == 0) {
            *x = (int32_t)(((uint32_t)d[0] << 24) | ((uint32_t)d[1] << 16) | ((uint32_t)d[2] << 8) | d[3]);
            *y = (int32_t)(((uint32_t)d[4] << 24) | ((uint32_t)d[5] << 16) | ((uint32_t)d[6] << 8) | d[7]);
            return *x > -NITRO_WIDE_ART_W && *x < NITRO_WIDE_ART_W && *y > -NITRO_WIDE_ART_H &&
                   *y < NITRO_WIDE_ART_H;
        }
        at += 12 + (long)len;
    }
    return 0;
}

/* A PNG of any size up to the canvas, into RGB555 | 0x8000 pixels (0:
 * transparent; malloc'd), only its drawn part (the box round what isn't
 * transparent: a picture turns about the middle of what's drawn in it,
 * 2026-10-16), and where that goes from the layout's place (*dx, *dy: the
 * file's offset, if it has one, and the margin left out); NULL if there's
 * none, or nothing in it. */
static uint16_t *decode_named(const unsigned char *bytes, long size, int *w, int *h, int *dx, int *dy)
{
    unsigned char *rgba;
    uint16_t *out;
    int n, i, x, y, x0, y0, x1, y1, ox = 0, oy = 0;

    if (!png_offset(bytes, size, &ox, &oy)) {
        ox = oy = 0;
    }
    rgba = stbi_load_from_memory(bytes, (int)size, w, h, &n, 4);
    if (rgba == NULL) {
        return NULL;
    }
    if (*w <= 0 || *h <= 0 || *w > NITRO_WIDE_ART_W || *h > NITRO_WIDE_ART_H) {
        stbi_image_free(rgba);
        return NULL;
    }
    /* the drawn part */
    x0 = *w;
    y0 = *h;
    x1 = y1 = 0;
    for (y = 0; y < *h; y++) {
        for (x = 0; x < *w; x++) {
            if (rgba[(y * *w + x) * 4 + 3] >= 128) {
                x0 = x < x0 ? x : x0;
                x1 = x + 1 > x1 ? x + 1 : x1;
                y0 = y < y0 ? y : y0;
                y1 = y + 1 > y1 ? y + 1 : y1;
            }
        }
    }
    if (x0 >= x1 || (out = malloc((size_t)(x1 - x0) * (size_t)(y1 - y0) * sizeof(uint16_t))) == NULL) {
        stbi_image_free(rgba);
        return NULL;
    }
    i = 0;
    for (y = y0; y < y1; y++) {
        for (x = x0; x < x1; x++) {
            const unsigned char *p = rgba + (y * *w + x) * 4;

            out[i++] = p[3] < 128 ? 0
                                  : (uint16_t)(0x8000 | (p[0] >> 3) | ((p[1] >> 3) << 5) | ((p[2] >> 3) << 10));
        }
    }
    stbi_image_free(rgba);
    *w = x1 - x0;
    *h = y1 - y0;
    *dx = ox + x0;
    *dy = oy + y0;
    return out;
}

/* The picture named name: the folder's file (dir/name.png), else the
 * built-in one of that name. */
static uint16_t *read_named(const char *dir, const char *name, int *w, int *h, int *dx, int *dy)
{
    char path[400];
    unsigned char *bytes = NULL;
    long size = 0;
    uint16_t *out;
    int count, k;
    const NitroWideArtFile *files;

    if (dir[0] != 0) {
        snprintf(path, sizeof(path), "%s/%s.png", dir, name);
        bytes = read_file(path, 1024 * 1024, &size);
    }
    if (bytes != NULL) {
        out = decode_named(bytes, size, w, h, dx, dy);
        free(bytes);
        return out;
    }
    files = NitroPort_WideArtBuiltIn(&count);
    snprintf(path, sizeof(path), "%s.png", name);
    for (k = 0; k < count; k++) {
        if (strcmp(files[k].name, path) == 0) {
            return decode_named(files[k].data, (long)files[k].size, w, h, dx, dy);
        }
    }
    return NULL;
}

const uint16_t *NitroPort_WideArtPicture(int picture, int *w, int *h, int *dx, int *dy)
{
    const char *name = NitroPort_WidePictureName(picture);
    char dir[256];
    uint16_t *read;
    uint32_t serial;
    int rw = 0, rh = 0, rdx = 0, rdy = 0;

    if (name == NULL) {
        return NULL;
    }
    lock();
    serial = sSerial;
    if (sNamed[picture].serial != serial) {
        sNamed[picture].serial = serial;
        sNamed[picture].state = 0;
    }
    if (sNamed[picture].state != 0) {
        const uint16_t *pixels = sNamed[picture].state == 1 ? sNamed[picture].pixels : NULL;

        *w = sNamed[picture].w;
        *h = sNamed[picture].h;
        *dx = sNamed[picture].dx;
        *dy = sNamed[picture].dy;
        unlock();
        return pixels;
    }
    snprintf(dir, sizeof(dir), "%s", sDir);
    unlock();

    /* (read outside the lock: it takes a while) */
    read = read_named(dir, name, &rw, &rh, &rdx, &rdy);
    lock();
    if (sNamed[picture].serial == serial && sNamed[picture].state == 0) {
        if (read == NULL) {
            sNamed[picture].state = 2;
        } else {
            if (sNamed[picture].pixels != NULL && sNamed[picture].capacity >= rw * rh) {
                memcpy(sNamed[picture].pixels, read, (size_t)rw * (size_t)rh * sizeof(uint16_t));
                free(read);
            } else {
                sNamed[picture].pixels = read;
                sNamed[picture].capacity = rw * rh;
            }
            read = NULL;
            sNamed[picture].w = rw;
            sNamed[picture].h = rh;
            sNamed[picture].dx = rdx;
            sNamed[picture].dy = rdy;
            sNamed[picture].state = 1;
            NitroPort_Log("[wide art] picture %s: %dx%d, %+d %+d from its place", name, rw, rh, rdx, rdy);
        }
    }
    free(read);
    {
        const uint16_t *pixels = sNamed[picture].state == 1 ? sNamed[picture].pixels : NULL;

        *w = sNamed[picture].w;
        *h = sNamed[picture].h;
        *dx = sNamed[picture].dx;
        *dy = sNamed[picture].dy;
        unlock();
        return pixels;
    }
}

/* Picture index's pixels (read the first time, kept in a slot); under the
 * lock on entry, not on return. NULL if it can't be read. */
static const uint16_t *picture_pixels(int index)
{
    int i, oldest = 0;
    Picture pic = sPictures[index];
    char dir[256];
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
    snprintf(dir, sizeof(dir), "%s", sDir);
    unlock();

    /* Not read yet: read it (outside the lock: it takes a while), then
     * into the slot used longest ago. */
    pixels = malloc(PIXELS * sizeof(uint16_t));
    if (pixels == NULL) {
        return NULL;
    }
    if (!read_picture(&pic, dir, pixels)) {
        free(pixels);
        lock();
        if (index < sCount && strcmp(sPictures[index].name, pic.name) == 0) {
            sPictures[index].bad = 1;
        }
        unlock();
        NitroPort_Log("[wide art] can't read %s%s (a PNG of %dx%d?)", pic.name, pic.builtIn >= 0 ? " (built in)" : "",
                      NITRO_WIDE_ART_W, NITRO_WIDE_ART_H);
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
            sSlots[oldest].rowUsed[y] = 0;
            for (x = 0; x < NITRO_WIDE_ART_W; x++) {
                if (px[y * NITRO_WIDE_ART_W + x]) {
                    if (x < left) left = x;
                    if (x >= right) right = x + 1;
                    sSlots[oldest].rowUsed[y] = 1;
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

const uint8_t *NitroPort_WideArtRows(const uint16_t *pixels)
{
    const uint8_t *rows = NULL;
    int i;

    if (pixels == NULL) {
        return NULL;
    }
    lock();
    for (i = 0; i < CACHE_SLOTS; i++) {
        if (sSlots[i].key != 0 && sSlots[i].pixels == pixels) {
            rows = sSlots[i].rowUsed;
            break;
        }
    }
    unlock();
    return rows;
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

/* ------------------------------------------------------------- sharing */

int NitroPort_WideArtUnshared(void)
{
    return __atomic_load_n(&sUnshared, __ATOMIC_RELAXED);
}

int NitroPort_WideArtToShare(uint64_t key, int i, const uint16_t **pixels, int *hofs, int *vofs, int *full,
                             char *name, size_t nameSize)
{
    int index, seen = 0;

    if (key == 0 || NitroPort_WideArtUnshared() == 0) {
        return 0;
    }
    lock();
    for (index = first_picture(key); index >= 0 && index < sCount && sPictures[index].key == key; index++) {
        const Picture *p = &sPictures[index];

        if (p->builtIn >= 0 || p->shared || p->bad || seen++ < i) {
            continue;
        }
        *hofs = p->hofs;
        *vofs = p->vofs;
        *full = p->full;
        snprintf(name, nameSize, "%s", p->name);
        *pixels = picture_pixels(index);   /* (unlocks) */
        return *pixels != NULL;
    }
    unlock();
    return 0;
}

static void to_rgba(const uint16_t *src, uint8_t *dst, int n);

int NitroPort_WideArtShareWrite(const char *name, const uint16_t *pixels)
{
    static uint8_t rgba[PIXELS * 4];
    char path[400];
    int i, game = 0, moved = 0, yours = 0, left;

    lock();
    snprintf(path, sizeof(path), "%s/share", sDir);
    unlock();
    mkdir(path, 0777);
    snprintf(path + strlen(path), sizeof(path) - strlen(path), "/%s", name);
    to_rgba(pixels, rgba, PIXELS);
    for (i = 0; i < PIXELS; i++) {
        if (NitroPort_WideArtIsGame(pixels[i])) {
            rgba[i * 4 + 0] = (uint8_t)(256 + NitroPort_WideArtGameDx(pixels[i]));
            rgba[i * 4 + 1] = (uint8_t)(128 + NitroPort_WideArtGameDy(pixels[i]));
            rgba[i * 4 + 2] = (uint8_t)((256 + NitroPort_WideArtGameDx(pixels[i])) >> 8);
            rgba[i * 4 + 3] = 64;
            game++;
            if (pixels[i] != NITRO_WIDE_ART_GAME) {
                moved++;
            }
        } else if (pixels[i] & 0x8000) {
            yours++;
        }
    }
    if (NitroPort_WideArtWritePng(path, rgba, NITRO_WIDE_ART_W, NITRO_WIDE_ART_H) != 0) {
        NitroPort_Log("[wide art] can't write %s", path);
        return -1;
    }
    lock();
    for (i = 0; i < sCount; i++) {
        if (sPictures[i].builtIn < 0 && !sPictures[i].shared && strcmp(sPictures[i].name, name) == 0) {
            sPictures[i].shared = 1;
            sUnshared--;
        }
    }
    left = sUnshared;
    unlock();
    NitroPort_Log("[wide art] shared %s: %d of its pixels the game's own (left out; %d of them moved), %d yours; %d still to share",
                  name, game, moved, yours, left);
    return 0;
}

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
    {
        /* The pictures the screen's layout draws (its own: the party's
         * panel, the boxes' divider), each once (where it's first drawn),
         * unturned, on a canvas of its own: the editor saves the box it's
         * in back to its file. */
        NitroWidePictureShown pics[32];
        int count, k, done[NITRO_WIDE_PICTURES] = { 0 };

        memset(&split, 0, sizeof(split));
        NitroPort_WideLayout(engine, &split);
        count = NitroPort_Render2DPictures(engine, &split, pics, 32);
        for (i = 0; i < count; i++) {
            const char *name = NitroPort_WidePictureName(pics[i].picture);
            const uint16_t *px;
            int w, h, dx, dy, x, y;

            if (name == NULL || done[pics[i].picture] ||
                (px = NitroPort_WideArtPicture(pics[i].picture, &w, &h, &dx, &dy)) == NULL) {
                continue;
            }
            done[pics[i].picture] = 1;
            for (k = 0; k < PIXELS; k++) {
                layer[k] = 0;
            }
            for (y = 0; y < h; y++) {
                for (x = 0; x < w; x++) {
                    int cx = pics[i].x + x, cy = pics[i].y + y;

                    if (cx >= 0 && cx < NITRO_WIDE_ART_W && cy >= 0 && cy < NITRO_WIDE_ART_H) {
                        layer[cy * NITRO_WIDE_ART_W + cx] = px[y * w + x];
                    }
                }
            }
            to_rgba(layer, rgba, PIXELS);
            snprintf(path, sizeof(path), "%s/picture_%s.png", folder, name);
            if (NitroPort_WideArtWritePng(path, rgba, NITRO_WIDE_ART_W, NITRO_WIDE_ART_H) != 0) {
                fclose(info);
                return -1;
            }
            /* (dx, dy: how far it is from the layout's place, its file's
             * offset and margin: the editor keeps it at its place) */
            fprintf(info, "picture %s bg %d prio %d x %d y %d w %d h %d angle %d dx %d dy %d file picture_%s.png\n",
                    name, pics[i].bg, pics[i].prio, pics[i].x, pics[i].y, w, h, pics[i].angle, dx, dy, name);
        }
    }
    fclose(info);
    if (made != NULL && madeSize > 0) {
        snprintf(made, madeSize, "%s", folder);
    }
    NitroPort_Log("[wide art] captured the upper screen into %s", folder);
    return 0;
}
