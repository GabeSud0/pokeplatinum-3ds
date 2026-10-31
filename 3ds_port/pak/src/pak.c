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
 * card, ~1 ms apiece on the 3DS: going between a route and a town, the
 * game lost about a second (2026-10-15: 32 frames a second over two).
 * Reads from blocks already here cost a copy. The blocks a read needs that
 * aren't here are read together, one request to the file for each run of
 * them, and kept whole (edges included: the same read again finds them).
 * 8 KB blocks: on a recorded run (3220 reads, 2 MB), 242 reads of the card
 * for 3.1 MB; 16 KB blocks made 194 for 4 MB, 4 KB ones 315 for 2.6 MB.
 * 4 MB of them: back in the field after a battle, the field's data is
 * still here (on a recorded battle: nothing read again; with 512 KB, 750
 * KB was; 2 MB kept too little, the battle's own data pushing it out). */
#define PAK_BLOCK_BYTES 8192u
#define PAK_BLOCKS 512u
/* At most (pak_set_cache_blocks: a New 3DS's 16 MB, 2026-10-30: crossing
 * between a route and a town, 120-240 small reads in a frame, the place
 * name's font, the zone's events, encounters, scripts and people; the
 * field's streaming reads, its map blocks, buildings and music, pushed
 * them out of 4 MB). */
#define PAK_BLOCKS_MAX 2048u
/* The most read at once through a buffer of its own (a bigger read goes
 * straight to the caller's memory, its edge blocks not kept). */
#define PAK_SCRATCH_MAX (256u * 1024u)
struct PakBlock {
    uint32_t number;       /* (which block of the file: its offset / PAK_BLOCK_BYTES) */
    uint32_t length;       /* 0: empty */
    uint32_t used;
    unsigned char *data;
};

struct Pak {
    FILE *file;
    struct PakEntry *entries;
    uint32_t count;
    uint64_t size;
    struct PakBlock blocks[PAK_BLOCKS_MAX];
    uint32_t clock;
    uint32_t disk_reads;   /* (pak_stats) */
    uint64_t disk_bytes;
    uint16_t *slot_of;     /* each block of the file: its slot + 1, or 0 (not here) */
    uint32_t nblocks;
    uint32_t slots;        /* the slots in use: PAK_BLOCKS, fewer if memory ran out */
    unsigned char *scratch; /* (for reading several blocks at once) */
    size_t scratch_size;
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
    pak->nblocks = (uint32_t)((file_size + PAK_BLOCK_BYTES - 1u) / PAK_BLOCK_BYTES);
    pak->slot_of = (uint16_t *)calloc(pak->nblocks, sizeof(*pak->slot_of));
    if (pak->slot_of == NULL) {
        result = PAK_ERR_MEMORY;
        goto fail;
    }
    pak->slots = PAK_BLOCKS;
    pak->file = file;
    pak->size = file_size;
    *out_pak = pak;
    return PAK_OK;

fail:
    if (file != NULL) fclose(file);
    if (pak != NULL) {
        free(pak->slot_of);
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

/* Block number's slot, or NULL if it isn't here. */
static struct PakBlock *cached_block(Pak *pak, uint32_t number)
{
    uint16_t slot = pak->slot_of[number];
    struct PakBlock *b;
    if (slot == 0) return NULL;
    b = &pak->blocks[slot - 1u];
    b->used = ++pak->clock;
    return b;
}

/* A slot for a block (an empty one, else the one used longest ago), its
 * data allocated; NULL if there's no memory for any. Not yet filled: length
 * 0, and not in slot_of until it is (keep_block). Slots get their memory
 * as they're first used, in order; if there's none for one, the cache
 * stays at the slots it has (the rest of the program needing it more). */
static struct PakBlock *free_slot(Pak *pak)
{
    for (;;) {
        struct PakBlock *slot = NULL;
        uint32_t i;
        for (i = 0; i < pak->slots; ++i) {
            struct PakBlock *b = &pak->blocks[i];
            if (b->length == 0) {
                slot = b;
                break;
            }
            if (slot == NULL || b->used < slot->used) slot = b;
        }
        if (slot == NULL) return NULL;
        if (slot->length != 0) {
            pak->slot_of[slot->number] = 0;
            slot->length = 0;
        }
        if (slot->data == NULL) {
            slot->data = (unsigned char *)malloc(PAK_BLOCK_BYTES);
            if (slot->data == NULL) {
                pak->slots = (uint32_t)(slot - pak->blocks);
                continue;
            }
        }
        return slot;
    }
}

static uint32_t block_length(const Pak *pak, uint32_t number)
{
    uint64_t start = (uint64_t)number * PAK_BLOCK_BYTES;
    return start + PAK_BLOCK_BYTES <= pak->size ? PAK_BLOCK_BYTES : (uint32_t)(pak->size - start);
}

/* Block number's bytes (data) kept in a slot; NULL if there's no memory
 * for one. */
static struct PakBlock *keep_block(Pak *pak, uint32_t number, const unsigned char *data)
{
    struct PakBlock *slot = free_slot(pak);
    if (slot == NULL) return NULL;
    slot->length = block_length(pak, number);
    memcpy(slot->data, data, slot->length);
    slot->number = number;
    slot->used = ++pak->clock;
    pak->slot_of[number] = (uint16_t)(slot - pak->blocks + 1);
    return slot;
}

/* A buffer of at least size bytes for reading several blocks at once
 * (kept for the next time); NULL if there's no memory for one. */
static unsigned char *scratch(Pak *pak, size_t size)
{
    if (size > PAK_SCRATCH_MAX) return NULL;
    if (pak->scratch_size < size) {
        free(pak->scratch);
        pak->scratch = (unsigned char *)malloc(size);
        pak->scratch_size = pak->scratch != NULL ? size : 0;
    }
    return pak->scratch;
}

static PakResult read_file(Pak *pak, uint64_t at, unsigned char *out, size_t len)
{
    pak->disk_reads++;
    pak->disk_bytes += len;
    if (at > LONG_MAX || fseek(pak->file, (long)at, SEEK_SET) != 0 ||
        !read_exact(pak->file, out, len))
        return PAK_ERR_IO;
    return PAK_OK;
}

static PakResult read_cached(Pak *pak, uint64_t absolute, unsigned char *out, size_t len)
{
    /* (one thread reads the pack: the game's, as with the FILE before) */
    const uint64_t end = absolute + len;
    uint32_t first = (uint32_t)(absolute / PAK_BLOCK_BYTES);
    uint32_t last = (uint32_t)((end - 1u) / PAK_BLOCK_BYTES);
    uint32_t n;

    /* The blocks here marked used first, so that keeping the others
     * doesn't push them out before they're copied. */
    for (n = first; n <= last; ++n)
        if (pak->slot_of[n] != 0) (void)cached_block(pak, n);
    n = first;
    while (n <= last) {
        const struct PakBlock *b = cached_block(pak, n);
        uint64_t start = (uint64_t)n * PAK_BLOCK_BYTES, from, to, run_end;
        uint32_t m;
        unsigned char *buf;

        if (b != NULL) {
            from = absolute > start ? absolute : start;
            to = end < start + b->length ? end : start + b->length;
            if (to - start > b->length) return PAK_ERR_IO;
            memcpy(out + (from - absolute), b->data + (from - start), (size_t)(to - from));
            ++n;
            continue;
        }
        /* Blocks n .. m aren't here: read together, in one request, and
         * kept whole (the first and last too, though the read wants only
         * part of them: the next read of the same piece finds them all). */
        for (m = n; m < last && pak->slot_of[m + 1u] == 0; ++m) {}
        run_end = (uint64_t)m * PAK_BLOCK_BYTES + block_length(pak, m);
        from = absolute > start ? absolute : start;
        to = end < run_end ? end : run_end;
        buf = scratch(pak, (size_t)(run_end - start));
        if (buf == NULL) {
            /* (too big for the buffer: what's wanted straight into out,
             * the blocks it covers whole kept) */
            uint32_t k;
            if (read_file(pak, from, out + (from - absolute), (size_t)(to - from)) != PAK_OK)
                return PAK_ERR_IO;
            for (k = n; k <= m; ++k) {
                uint64_t s = (uint64_t)k * PAK_BLOCK_BYTES;
                if (s >= from && s + block_length(pak, k) <= to)
                    (void)keep_block(pak, k, out + (s - absolute));
            }
        } else {
            uint32_t k;
            if (read_file(pak, start, buf, (size_t)(run_end - start)) != PAK_OK)
                return PAK_ERR_IO;
            for (k = n; k <= m; ++k)
                (void)keep_block(pak, k, buf + (size_t)(k - n) * PAK_BLOCK_BYTES);
            memcpy(out + (from - absolute), buf + (from - start), (size_t)(to - from));
        }
        n = m + 1u;
    }
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
    return read_cached(pak, absolute, (unsigned char *)buf, len);
}

void pak_set_cache_blocks(Pak *pak, uint32_t blocks)
{
    if (pak == NULL) return;
    if (blocks > PAK_BLOCKS_MAX) blocks = PAK_BLOCKS_MAX;
    /* (only more: the slots in use stay; memory comes as they're used) */
    if (blocks > pak->slots) pak->slots = blocks;
}

void pak_stats(const Pak *pak, uint32_t *disk_reads, uint64_t *disk_bytes)
{
    if (disk_reads) *disk_reads = pak != NULL ? pak->disk_reads : 0;
    if (disk_bytes) *disk_bytes = pak != NULL ? pak->disk_bytes : 0;
}

void pak_close(Pak *pak)
{
    uint32_t i;
    if (pak == NULL) return;
    for (i = 0; i < PAK_BLOCKS_MAX; ++i) free(pak->blocks[i].data);
    if (pak->file != NULL) fclose(pak->file);
    free(pak->scratch);
    free(pak->slot_of);
    free(pak->entries);
    free(pak);
}
