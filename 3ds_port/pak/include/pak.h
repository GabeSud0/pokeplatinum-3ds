#ifndef PLATINUM3DS_PAK_H
#define PLATINUM3DS_PAK_H

/* Platinum 3DS data pack v1. Portable C99; no Nintendo SDK dependency.
 * See docs/PAK_FORMAT.md for the on-disk format.
 */
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PAK_FORMAT_VERSION 1u
#define PAK_BUILD_ID_BYTES 32u

typedef struct Pak Pak;

typedef enum PakResult {
    PAK_OK = 0,
    PAK_ERR_ARGUMENT,  /* NULL pointer, invalid build ID or argument */
    PAK_ERR_IO,        /* Cannot open, seek or read the file */
    PAK_ERR_FORMAT,    /* Malformed, truncated or out-of-bounds pack */
    PAK_ERR_VERSION,   /* Unsupported on-disk format version */
    PAK_ERR_BUILD_ID,  /* Does not match the running game build */
    PAK_ERR_ROM_SHA1,  /* Unsupported source ROM revision */
    PAK_ERR_MEMORY,    /* Cannot allocate the index */
    PAK_ERR_NOT_FOUND, /* NitroFS file ID absent */
    PAK_ERR_RANGE      /* Requested file-relative bytes out of bounds */
} PakResult;

/* Validates the header and complete index without loading file contents.
 * expected_build_id must match the 32-byte, NUL-padded on-disk ID exactly.
 * On failure, *out_pak is NULL. The caller owns the returned handle.
 */
PakResult pak_open(const char *path, const char *expected_build_id, Pak **out_pak);

/* Binary search by original (possibly sparse) NitroFS file ID. */
PakResult pak_find(const Pak *pak, uint32_t file_id, uint32_t *out_size);

/* Read len bytes at a file-relative offset. len==0 is permitted, even if
 * buf==NULL; nonzero reads require a valid buffer. Never reads past a file.
 */
PakResult pak_read(Pak *pak, uint32_t file_id, uint32_t offset,
                   void *buf, size_t len);

/* Accepts NULL. */
void pak_close(Pak *pak);

#ifdef __cplusplus
}
#endif

#endif /* PLATINUM3DS_PAK_H */
