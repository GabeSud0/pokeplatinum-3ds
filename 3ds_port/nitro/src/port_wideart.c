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
#define CACHE_SLOTS 12
#define PIXELS (NITRO_WIDE_ART_W * NITRO_WIDE_ART_H)

typedef struct Picture {
    uint64_t key;
    int16_t hofs, vofs;
    uint8_t full, bad;
    char name[48];
} Picture;

typedef struct Slot {
    uint64_t key;          /* 0: empty */
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
        f = 1;
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

    return pa->key < pb->key ? -1 : pa->key > pb->key ? 1 : (int)pa->full - (int)pb->full;
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

const uint16_t *NitroPort_WideArtFind(uint64_t key, int *hofs, int *vofs, int *full)
{
    int i, index, oldest = 0;
    Picture pic;
    char path[320];
    uint16_t *pixels;

    if (key == 0 || NitroPort_WideArtCount() == 0) {
        return NULL;
    }
    lock();
    index = find_picture(key);
    if (index < 0 || sPictures[index].bad) {
        unlock();
        return NULL;
    }
    pic = sPictures[index];
    *hofs = pic.hofs;
    *vofs = pic.vofs;
    *full = pic.full;
    for (i = 0; i < CACHE_SLOTS; i++) {
        if (sSlots[i].key == key && sSlots[i].pixels != NULL) {
            sSlots[i].lastUse = ++sUse;
            pixels = sSlots[i].pixels;
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
        index = find_picture(key);
        if (index >= 0) {
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
    sSlots[oldest].key = key;
    sSlots[oldest].lastUse = ++sUse;
    {
        const uint16_t *result = sSlots[oldest].pixels;

        unlock();
        free(pixels);
        return result;
    }
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
        fprintf(info, "bg %d prio %d key %016llx hofs %d vofs %d file bg%d.png\n", bg, prio,
                (unsigned long long)key, hofs, vofs, bg);
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
