/* Only synthetic data: C fixtures plus a pack from builder/pack.py. */
#include "pak.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); return 1; \
} } while (0)

static char path[512];
static const char *rev1 = "0862ec35b24de5c7e2dcb88c9eea0873110d755c";
static const char *rev0 = "ce81046eda7d232513069519cb2085349896dec7";
static const char *build = "test-build";

static void u32(unsigned char *p, uint32_t n)
{
    p[0] = (unsigned char)n; p[1] = (unsigned char)(n >> 8);
    p[2] = (unsigned char)(n >> 16); p[3] = (unsigned char)(n >> 24);
}

static int nibble(char c) { return c <= '9' ? c - '0' : c - 'a' + 10; }

static void sha(unsigned char *p, const char *hex)
{
    size_t i;
    for (i = 0; i < 20; ++i)
        p[i] = (unsigned char)((nibble(hex[2*i]) << 4) | nibble(hex[2*i+1]));
}

static int write_file(const unsigned char *data, size_t n)
{
    FILE *f = fopen(path, "wb");
    int ok;
    if (!f) return 0;
    ok = fwrite(data, 1, n, f) == n;
    return fclose(f) == 0 && ok;
}

static int fixture(void)
{
    unsigned char data[104] = {0};
    const unsigned char second[5] = {0, 1, 255, 2, 3};
    memcpy(data, "PLATPAK\0", 8);
    u32(data + 8, 1);
    memcpy(data + 12, build, strlen(build));
    sha(data + 44, rev1);
    u32(data + 64, 2);
    u32(data + 68, 2); u32(data + 72, 92); u32(data + 76, 3);
    u32(data + 80, 99); u32(data + 84, 96); u32(data + 88, 5);
    memcpy(data + 92, "abc", 3);
    memcpy(data + 96, second, 5);
    return write_file(data, sizeof(data));
}

static int patch(size_t at, const unsigned char *bytes, size_t n)
{
    FILE *f = fopen(path, "r+b");
    int ok;
    if (!f) return 0;
    ok = fseek(f, (long)at, SEEK_SET) == 0 && fwrite(bytes, 1, n, f) == n;
    return fclose(f) == 0 && ok;
}

static int patch32(size_t at, uint32_t n)
{
    unsigned char b[4];
    u32(b, n);
    return patch(at, b, 4);
}

static int open_is(PakResult expected)
{
    Pak *pak = (Pak *)1;
    PakResult actual = pak_open(path, build, &pak);
    if (actual != expected || (actual == PAK_OK) != (pak != NULL)) {
        fprintf(stderr, "open result %d, expected %d\n", (int)actual, (int)expected);
        if (actual == PAK_OK) pak_close(pak);
        return 0;
    }
    pak_close(pak);
    return 1;
}

static int test_c_fixtures(void)
{
    Pak *pak = NULL;
    uint32_t size = 123;
    unsigned char bytes[8] = {0};
    const unsigned char expected[5] = {0, 1, 255, 2, 3};
    unsigned char hash[20] = {0};
    unsigned char truncated[102] = {0};
    FILE *f;
    const unsigned char bad = 1;
    const struct { size_t at; uint32_t value; PakResult error; } corruptions[] = {
        {8, 2, PAK_ERR_VERSION}, {64, 4, PAK_ERR_FORMAT},
        {72, 0xfffffff0u, PAK_ERR_FORMAT}, {76, 0xffffffffu, PAK_ERR_FORMAT},
        {80, 1, PAK_ERR_FORMAT}, {80, 2, PAK_ERR_FORMAT},
        {84, 92, PAK_ERR_FORMAT}, {72, 93, PAK_ERR_FORMAT}
    };
    size_t i;

    CHECK(fixture());
    CHECK(pak_open(path, build, &pak) == PAK_OK);
    CHECK(pak_find(pak, 2, &size) == PAK_OK && size == 3);
    CHECK(pak_read(pak, 2, 0, bytes, 3) == PAK_OK && memcmp(bytes, "abc", 3) == 0);
    CHECK(pak_read(pak, 2, 1, bytes, 2) == PAK_OK && memcmp(bytes, "bc", 2) == 0);
    CHECK(pak_find(pak, 99, &size) == PAK_OK && size == 5);
    CHECK(pak_read(pak, 99, 0, bytes, 5) == PAK_OK && memcmp(bytes, expected, 5) == 0);
    CHECK(pak_read(pak, 99, 2, bytes, 3) == PAK_OK && memcmp(bytes, expected + 2, 3) == 0);
    CHECK(pak_read(pak, 99, 5, NULL, 0) == PAK_OK);
    CHECK(pak_read(pak, 99, 5, bytes, 1) == PAK_ERR_RANGE);
    CHECK(pak_read(pak, 99, 6, bytes, 0) == PAK_ERR_RANGE);
    CHECK(pak_read(pak, 99, UINT32_MAX, bytes, 1) == PAK_ERR_RANGE);
    CHECK(pak_read(pak, 99, 0, NULL, 1) == PAK_ERR_ARGUMENT);
    CHECK(pak_find(pak, 3, &size) == PAK_ERR_NOT_FOUND && size == 5);
    CHECK(pak_read(pak, 3, 0, bytes, 1) == PAK_ERR_NOT_FOUND);
    CHECK(pak_find(pak, 2, NULL) == PAK_ERR_ARGUMENT);
    {
        /* the file read once (the small pack in a block), then kept */
        uint32_t reads, again;
        uint64_t read_bytes;
        pak_set_cache_blocks(pak, 4096);   /* (capped at the most) */
        pak_stats(pak, &reads, &read_bytes);
        CHECK(reads >= 1 && read_bytes >= 3);
        CHECK(pak_read(pak, 2, 0, bytes, 3) == PAK_OK && memcmp(bytes, "abc", 3) == 0);
        pak_stats(pak, &again, NULL);
        CHECK(again == reads);
    }
    pak_close(pak);

    CHECK(fixture());
    CHECK(pak_open(path, "wrong-build", &pak) == PAK_ERR_BUILD_ID && pak == NULL);
    CHECK(pak_open(path, "", &pak) == PAK_ERR_ARGUMENT);
    CHECK(pak_open(path, "012345678901234567890123456789012", &pak) == PAK_ERR_ARGUMENT);
    CHECK(patch(44, hash, 20) && open_is(PAK_ERR_ROM_SHA1));
    sha(hash, rev0);
    CHECK(patch(44, hash, 20) && open_is(PAK_OK));

    CHECK(fixture());
    f = fopen(path, "rb");
    CHECK(f != NULL && fread(truncated, 1, sizeof(truncated), f) == sizeof(truncated));
    CHECK(fclose(f) == 0);
    CHECK(write_file(truncated, 16) && open_is(PAK_ERR_FORMAT));
    CHECK(write_file(truncated, sizeof(truncated)) && open_is(PAK_ERR_FORMAT));

    for (i = 0; i < sizeof(corruptions) / sizeof(corruptions[0]); ++i) {
        CHECK(fixture());
        CHECK(patch32(corruptions[i].at, corruptions[i].value));
        CHECK(open_is(corruptions[i].error));
    }
    CHECK(fixture() && patch(0, &bad, 1) && open_is(PAK_ERR_FORMAT));
    CHECK(fixture() && patch(95, &bad, 1) && open_is(PAK_ERR_FORMAT));

    {
        unsigned char header[68] = {0};
        memcpy(header, "PLATPAK\0", 8);
        u32(header + 8, 1);
        memcpy(header + 12, build, strlen(build));
        sha(header + 44, rev1);
        CHECK(write_file(header, sizeof(header)) && open_is(PAK_OK));
    }
    return 0;
}

static int test_python_writer(const char *python_pak)
{
    Pak *pak = NULL;
    uint32_t size;
    unsigned char bytes[32];
    unsigned char expected[17];
    size_t i;
    CHECK(pak_open(python_pak, "c-python-v1", &pak) == PAK_OK);
    CHECK(pak_find(pak, 3, &size) == PAK_OK && size == 15);
    CHECK(pak_read(pak, 3, 0, bytes, 15) == PAK_OK);
    CHECK(memcmp(bytes, "python-writer\0\xff", 15) == 0);
    CHECK(pak_find(pak, 4096, &size) == PAK_OK && size == 0);
    CHECK(pak_read(pak, 4096, 0, NULL, 0) == PAK_OK);
    CHECK(pak_find(pak, 70000, &size) == PAK_OK && size == 17);
    for (i = 0; i < sizeof(expected); ++i) expected[i] = (unsigned char)i;
    CHECK(pak_read(pak, 70000, 0, bytes, 17) == PAK_OK);
    CHECK(memcmp(bytes, expected, 17) == 0);
    CHECK(pak_read(pak, 70000, 8, bytes, 4) == PAK_OK);
    CHECK(memcmp(bytes, expected + 8, 4) == 0);
    CHECK(pak_find(pak, 4, &size) == PAK_ERR_NOT_FOUND);
    pak_close(pak);
    return 0;
}

/* A pack bigger than the read cache (40 blocks of 16 KB in one file):
 * pieces of every size and place read back the same, again and again,
 * small ones through the cache, big ones straight from the file. */
static int test_cache(void)
{
    enum { SIZE = 40 * 16384 + 123 };
    static unsigned char data[92 + SIZE + 3], got[3 * 16384 + 7];
    Pak *pak = NULL;
    uint32_t size = 0, seed = 12345;
    size_t i, k;
    memcpy(data, "PLATPAK\0", 8);
    u32(data + 8, 1);
    memcpy(data + 12, build, strlen(build));
    sha(data + 44, rev1);
    u32(data + 64, 2);
    u32(data + 68, 2); u32(data + 72, 92); u32(data + 76, SIZE);
    for (i = 0; i < SIZE; ++i) data[92 + i] = (unsigned char)(i * 7 + (i >> 9));
    /* (the second file, empty, right after the first's padding) */
    u32(data + 80, 5); u32(data + 84, (uint32_t)((92 + SIZE + 3) & ~3u)); u32(data + 88, 0);
    CHECK(write_file(data, (92 + SIZE + 3) & ~3u));
    CHECK(pak_open(path, build, &pak) == PAK_OK);
    CHECK(pak_find(pak, 2, &size) == PAK_OK && size == SIZE);
    for (k = 0; k < 3000; ++k) {
        size_t len, at;
        seed = seed * 1103515245u + 12345u;
        len = (k % 50 == 0) ? (seed >> 8) % sizeof(got) : (seed >> 8) % 600 + 1;
        seed = seed * 1103515245u + 12345u;
        at = (seed >> 4) % (SIZE - len + 1);
        if (k % 7 == 0) at = (at / 16384) * 16384 + 16384 - len / 2;   /* across a block's end */
        if (at + len > SIZE) at = SIZE - len;
        memset(got, 0xAA, sizeof(got));
        CHECK(pak_read(pak, 2, (uint32_t)at, got, len) == PAK_OK);
        CHECK(memcmp(got, data + 92 + at, len) == 0);
    }
    CHECK(pak_read(pak, 2, SIZE - 1, got, 1) == PAK_OK && got[0] == data[92 + SIZE - 1]);
    CHECK(pak_read(pak, 2, SIZE - 1, got, 2) == PAK_ERR_RANGE);
    pak_close(pak);
    return 0;
}

/* A pack bigger than the cache (4 MB): blocks pushed out and read again,
 * big reads kept and small ones served from them, in every order; and
 * reads bigger than the reading buffer (256 KB). */
static int test_cache_eviction(void)
{
    enum { SIZE = 600 * 8192 + 123, BIG = 80 * 1024 + 5, HUGE = 300 * 1024 + 77 };
    const size_t total = (92 + SIZE + 3) & ~(size_t)3;
    unsigned char *data = (unsigned char *)calloc(1, total);
    unsigned char *got = (unsigned char *)malloc(HUGE);
    Pak *pak = NULL;
    uint32_t size = 0, seed = 777;
    size_t i, k, pass;
    CHECK(data != NULL && got != NULL);
    memcpy(data, "PLATPAK\0", 8);
    u32(data + 8, 1);
    memcpy(data + 12, build, strlen(build));
    sha(data + 44, rev1);
    u32(data + 64, 1);
    u32(data + 68, 9); u32(data + 72, 80); u32(data + 76, SIZE);
    for (i = 0; i < SIZE; ++i) data[80 + i] = (unsigned char)(i * 13 + (i >> 11) + (i >> 19));
    CHECK(write_file(data, (80 + SIZE + 3) & ~(size_t)3));
    CHECK(pak_open(path, build, &pak) == PAK_OK);
    CHECK(pak_find(pak, 9, &size) == PAK_OK && size == SIZE);
    /* Twice through the whole pack in small pieces (each pass pushing the
     * first one's blocks out), then backwards in big ones. */
    for (pass = 0; pass < 2; ++pass)
        for (i = 0; i < SIZE; i += 3000) {
            size_t len = SIZE - i < 3000 ? SIZE - i : 3000;
            CHECK(pak_read(pak, 9, (uint32_t)i, got, len) == PAK_OK);
            CHECK(memcmp(got, data + 80 + i, len) == 0);
        }
    for (i = SIZE; i > 0;) {
        size_t len = i < BIG ? i : BIG;
        i -= len;
        CHECK(pak_read(pak, 9, (uint32_t)i, got, len) == PAK_OK);
        CHECK(memcmp(got, data + 80 + i, len) == 0);
    }
    for (k = 0; k < 6000; ++k) {
        size_t len, at;
        seed = seed * 1103515245u + 12345u;
        len = (k % 9 == 0) ? (seed >> 8) % BIG + 1 : (seed >> 8) % 900 + 1;
        if (k % 97 == 0) len = HUGE - (seed >> 8) % 4096;
        seed = seed * 1103515245u + 12345u;
        at = (seed >> 3) % (SIZE - len + 1);
        if (k % 5 == 0) at = (at / 8192) * 8192 + 8192 - len / 3;   /* across a block's end */
        if (k % 11 == 0) at = SIZE - len;                            /* the last, short block */
        if (at + len > SIZE) at = SIZE - len;
        memset(got, 0x5A, len);
        CHECK(pak_read(pak, 9, (uint32_t)at, got, len) == PAK_OK);
        CHECK(memcmp(got, data + 80 + at, len) == 0);
    }
    pak_close(pak);
    free(got);
    free(data);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 3 || snprintf(path, sizeof(path), "%s/test-c.pak", argv[1]) >=
        (int)sizeof(path)) {
        fprintf(stderr, "Usage: test_pak <work-dir> <python-generated-pak>\n");
        return 2;
    }
    CHECK(test_c_fixtures() == 0);
    CHECK(test_python_writer(argv[2]) == 0);
    CHECK(test_cache() == 0);
    CHECK(test_cache_eviction() == 0);
    remove(path);
    pak_close(NULL);
    puts("pak: synthetic C cases and Python-writer cross-check passed");
    return 0;
}
