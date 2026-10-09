#include "pak.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PAK_HEADER_BYTES 68u
#define PAK_ENTRY_BYTES 12u

struct PakEntry {
    uint32_t id;
    uint32_t offset;
    uint32_t size;
};

struct Pak {
    FILE *file;
    struct PakEntry *entries;
    uint32_t count;
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
    if (absolute > LONG_MAX || fseek(pak->file, (long)absolute, SEEK_SET) != 0)
        return PAK_ERR_IO;
    if (!read_exact(pak->file, buf, len)) return PAK_ERR_IO;
    return PAK_OK;
}

void pak_close(Pak *pak)
{
    if (pak == NULL) return;
    if (pak->file != NULL) fclose(pak->file);
    free(pak->entries);
    free(pak);
}
