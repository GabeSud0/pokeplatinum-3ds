#include "pak.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The port builds with -fno-short-enums; fail the build if libpak doesn't. */
typedef char pak_result_is_int_sized[sizeof(PakResult) == sizeof(int) ? 1 : -1];

#define PAK_HEADER_BYTES 68u
#define PAK_ENTRY_BYTES 12u

struct PakEntry {
    uint32_t id;
    uint32_t offset;
    uint32_t size;
};

/* Recently read parts of the pack, in blocks: the game reads many small
 * pieces (a field's load is ~1900 reads, nine in ten under 256 bytes:
 * archive headers, file tables, text), each a seek and a read of the SD
 * card through stdio, ~1 ms apiece on the 3DS: going between a route and
 * a town, the game lost about a second (2026-10-15: 32 frames a second
 * over two). Reads from a block already read cost a copy; reads of two
 * blocks or more go straight to the file. 8 KB: on a recorded run (3220
 * reads, 2 MB), 242 reads of the card for 3.1 MB; 16 KB blocks made 194
 * for 4 MB, 4 KB ones 315 for 2.6 MB. */
#define PAK_BLOCK_BYTES 8192u
#define PAK_BLOCKS 64u
struct PakBlock {
    uint64_t start;        /* (its offset in the file; a multiple of the block size) */
    uint32_t length;       /* 0: empty */
    uint32_t used;
    unsigned char *data;
};

struct Pak {
    FILE *file;
    struct PakEntry *entries;
    uint32_t count;
    uint64_t size;
    struct PakBlock blocks[PAK_BLOCKS];
    uint32_t clock;
};

static uint32_t read_u32(const unsigned char *bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static uint64_t align4(uint64_t value)
{
    return (value + 3u) & ~(uint64_t)3u;
}

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static int matches_sha1(const unsigned char *digest, const char *hex)
{
    size_t i;
    for (i = 0; i < 20; ++i) {
        unsigned int value = (unsigned int)((hex_nibble(hex[2 * i]) << 4) |
                                            hex_nibble(hex[2 * i + 1]));
        if (digest[i] != value) return 0;
    }
    return 1;
}

static int supported_sha1(const unsigned char *digest)
{
    /* Keep these in sync with builder/verify_rom.py (USA Rev 1 and Rev 0). */
    return matches_sha1(digest, "0862ec35b24de5c7e2dcb88c9eea0873110d755c") ||
           matches_sha1(digest, "ce81046eda7d232513069519cb2085349896dec7");
}

static int read_exact(FILE *file, void *data, size_t length)
{
    return fread(data, 1, length, file) == length;
}

static const struct PakEntry *lookup(const Pak *pak, uint32_t id)
{
    size_t low = 0, high = pak->count;
    while (low < high) {
        size_t mid = low + (high - low) / 2;
        if (pak->entries[mid].id < id) low = mid + 1;
        else high = mid;
    }
    if (low < pak->count && pak->entries[low].id == id)
        return &pak->entries[low];
    return NULL;
}

PakResult pak_open(const char *path, const char *expected_build_id, Pak **out_pak)
{
    FILE *file = NULL;
    Pak *pak = NULL;
    unsigned char header[PAK_HEADER_BYTES];
    unsigned char expected_id[PAK_BUILD_ID_BYTES] = {0};
    uint64_t file_size, cursor, index_end;
    uint32_t count, i;
    size_t id_len = 0;
    long length;
    PakResult result = PAK_ERR_FORMAT;

    if (out_pak == NULL) return PAK_ERR_ARGUMENT;
    *out_pak = NULL;
    if (path == NULL || expected_build_id == NULL) return PAK_ERR_ARGUMENT;
    while (id_len <= PAK_BUILD_ID_BYTES && expected_build_id[id_len] != '\0')
        ++id_len;
    if (id_len == 0 || id_len > PAK_BUILD_ID_BYTES) return PAK_ERR_ARGUMENT;
    memcpy(expected_id, expected_build_id, id_len);

    file = fopen(path, "rb");
    if (file == NULL) return PAK_ERR_IO;
    /* Unbuffered: each read straight from the file, in one request. The
     * reader keeps its own blocks (read_cached); through stdio's buffer
     * (newlib's: 1 KB) a block, or a big read, was a request to the SD card
     * per kilobyte. */
    (void)setvbuf(file, NULL, _IONBF, 0);
    if (fseek(file, 0, SEEK_END) != 0 || (length = ftell(file)) < 0 ||
        fseek(file, 0, SEEK_SET) != 0) {
        result = PAK_ERR_IO;
        goto fail;
    }
    file_size = (uint64_t)(unsigned long)length;
    if (file_size < PAK_HEADER_BYTES || file_size > UINT32_MAX) goto fail;
    if (!read_exact(file, header, sizeof(header))) {
        result = ferror(file) ? PAK_ERR_IO : PAK_ERR_FORMAT;
        goto fail;
    }
    if (memcmp(header, "PLATPAK\0", 8) != 0) goto fail;
    if (read_u32(header + 8) != PAK_FORMAT_VERSION) {
        result = PAK_ERR_VERSION;
        goto fail;
    }
    if (memcmp(header + 12, expected_id, sizeof(expected_id)) != 0) {
        result = PAK_ERR_BUILD_ID;
        goto fail;
    }
    if (!supported_sha1(header + 44)) {
        result = PAK_ERR_ROM_SHA1;
        goto fail;
    }

    count = read_u32(header + 64);
    if ((uint64_t)count > (file_size - PAK_HEADER_BYTES) / PAK_ENTRY_BYTES ||
        (uint64_t)count * sizeof(struct PakEntry) > (uint64_t)SIZE_MAX) goto fail;
    index_end = PAK_HEADER_BYTES + (uint64_t)count * PAK_ENTRY_BYTES;
    cursor = align4(index_end);
    if (cursor > file_size) goto fail;

    pak = (Pak *)calloc(1, sizeof(*pak));
    if (pak == NULL) {
        result = PAK_ERR_MEMORY;
        goto fail;
    }
    if (count != 0) {
        pak->entries = (struct PakEntry *)malloc((size_t)count * sizeof(*pak->entries));
        if (pak->entries == NULL) {
            result = PAK_ERR_MEMORY;
            goto fail;
        }
    }
    pak->count = count;
    for (i = 0; i < count; ++i) {
        unsigned char raw[PAK_ENTRY_BYTES];
        struct PakEntry *entry = &pak->entries[i];
        uint64_t end;
        if (!read_exact(file, raw, sizeof(raw))) {
            result = ferror(file) ? PAK_ERR_IO : PAK_ERR_FORMAT;
            goto fail;
        }
        entry->id = read_u32(raw);
        entry->offset = read_u32(raw + 4);
        entry->size = read_u32(raw + 8);
        if ((i != 0 && entry->id <= pak->entries[i - 1].id) ||
            (entry->offset & 3u) != 0 || entry->offset != cursor ||
            (uint64_t)entry->size > file_size - cursor) goto fail;
        end = cursor + entry->size;
        cursor = align4(end);
        if (cursor > file_size) goto fail;
    }
    /* The Python writer emits canonical, contiguous data with zero padding.
     * Reject extra bytes or hidden gaps rather than trusting the offsets.
     */
    if (cursor != file_size) goto fail;
    for (i = 0; i < count; ++i) {
        uint64_t end = (uint64_t)pak->entries[i].offset + pak->entries[i].size;
        size_t padding = (size_t)(align4(end) - end);
        unsigned char zeros[3];
        size_t j;
        if (padding == 0) continue;
        if (fseek(file, (long)end, SEEK_SET) != 0 ||
            !read_exact(file, zeros, padding)) {
            result = PAK_ERR_IO;
            goto fail;
        }
        for (j = 0; j < padding; ++j) if (zeros[j] != 0) goto fail;
    }
    pak->file = file;
    pak->size = file_size;
    *out_pak = pak;
    return PAK_OK;

fail:
    if (file != NULL) fclose(file);
    if (pak != NULL) {
        free(pak->entries);
        free(pak);
    }
    return result;
}

PakResult pak_find(const Pak *pak, uint32_t file_id, uint32_t *out_size)
{
    const struct PakEntry *entry;
    if (pak == NULL || out_size == NULL) return PAK_ERR_ARGUMENT;
    entry = lookup(pak, file_id);
    if (entry == NULL) return PAK_ERR_NOT_FOUND;
    *out_size = entry->size;
    return PAK_OK;
}

/* The block of the pack starting at start (a multiple of the block size),
 * read if it isn't here; NULL if it can't be read. */
static const struct PakBlock *block_at(Pak *pak, uint64_t start)
{
    struct PakBlock *slot, *empty = NULL, *oldest = NULL;
    uint32_t i, want;
    for (i = 0; i < PAK_BLOCKS; ++i) {
        struct PakBlock *b = &pak->blocks[i];
        if (b->length != 0 && b->start == start) {
            b->used = ++pak->clock;
            return b;
        }
        if (b->length == 0) {
            if (empty == NULL) empty = b;
        } else if (oldest == NULL || b->used < oldest->used) {
            oldest = b;
        }
    }
    /* an empty one, else the one used longest ago */
    slot = empty != NULL ? empty : oldest;
    if (slot->data == NULL) {
        slot->data = (unsigned char *)malloc(PAK_BLOCK_BYTES);
        if (slot->data == NULL) return NULL;
    }
    slot->length = 0;
    want = start + PAK_BLOCK_BYTES <= pak->size ? PAK_BLOCK_BYTES : (uint32_t)(pak->size - start);
    if (start > LONG_MAX || fseek(pak->file, (long)start, SEEK_SET) != 0 ||
        !read_exact(pak->file, slot->data, want))
        return NULL;
    slot->start = start;
    slot->length = want;
    slot->used = ++pak->clock;
    return slot;
}

static PakResult read_cached(Pak *pak, uint64_t absolute, unsigned char *out, size_t len)
{
    PakResult result = PAK_OK;
    /* (one thread reads the pack: the game's, as with the FILE before) */
    if (len >= 2u * PAK_BLOCK_BYTES) {
        /* (a big read: straight from the file) */
        if (absolute > LONG_MAX || fseek(pak->file, (long)absolute, SEEK_SET) != 0 ||
            !read_exact(pak->file, out, len))
            result = PAK_ERR_IO;
    } else {
        while (len > 0) {
            uint64_t start = absolute - absolute % PAK_BLOCK_BYTES;
            const struct PakBlock *b = block_at(pak, start);
            size_t at = (size_t)(absolute - start), n;
            if (b == NULL || at >= b->length) {
                result = PAK_ERR_IO;
                break;
            }
            n = b->length - at < len ? b->length - at : len;
            memcpy(out, b->data + at, n);
            out += n;
            absolute += n;
            len -= n;
        }
    }
    return result;
}

PakResult pak_read(Pak *pak, uint32_t file_id, uint32_t offset,
                   void *buf, size_t len)
{
    const struct PakEntry *entry;
    uint64_t absolute;
    if (pak == NULL || (len != 0 && buf == NULL)) return PAK_ERR_ARGUMENT;
    entry = lookup(pak, file_id);
    if (entry == NULL) return PAK_ERR_NOT_FOUND;
    if (offset > entry->size || len > (size_t)(entry->size - offset))
        return PAK_ERR_RANGE;
    if (len == 0) return PAK_OK;
    absolute = (uint64_t)entry->offset + offset;
    return read_cached(pak, absolute, (unsigned char *)buf, len);
}

void pak_close(Pak *pak)
{
    uint32_t i;
    if (pak == NULL) return;
    for (i = 0; i < PAK_BLOCKS; ++i) free(pak->blocks[i].data);
    if (pak->file != NULL) fclose(pak->file);
    free(pak->entries);
    free(pak);
}
