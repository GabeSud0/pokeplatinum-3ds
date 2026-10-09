/*
 * C versions of NitroSDK's memory copy and fill routines
 * (libraries/mi/src/mi_memory.c, which is Metrowerks assembly only).
 *
 * `size` is always in bytes. Like the originals, copies run forward, so
 * overlapping source and destination behave the same way they did on the DS.
 * The 16- and 32-bit versions move whole halfwords and words; the "Fast"
 * versions move 32-byte blocks (read all, then write all) and then words.
 *
 * MI_CpuFill8/MI_CpuCopy8 used halfword read-modify-write on the DS because
 * VRAM ignores byte writes there. Our VRAM is ordinary memory, so plain byte
 * loops give the same result.
 */

#include <nitro/types.h>
#include <nitro/mi/memory.h>

void MIi_CpuClear16(u16 data, void *destp, u32 size)
{
    u16 *d = destp;
    u32 i;

    for (i = 0; i < size; i += 2) {
        *d++ = data;
    }
}

void MIi_CpuCopy16(const void *srcp, void *destp, u32 size)
{
    const u16 *s = srcp;
    u16 *d = destp;
    u32 i;

    for (i = 0; i < size; i += 2) {
        *d++ = *s++;
    }
}

void MIi_CpuSend16(const void *srcp, volatile void *destp, u32 size)
{
    const u16 *s = srcp;
    volatile u16 *d = destp; /* a FIFO port: every value goes to one address */
    u32 i;

    for (i = 0; i < size; i += 2) {
        *d = *s++;
    }
}

void MIi_CpuClear32(u32 data, void *destp, u32 size)
{
    u32 *d = destp;
    u32 i;

    for (i = 0; i < size; i += 4) {
        *d++ = data;
    }
}

void MIi_CpuCopy32(const void *srcp, void *destp, u32 size)
{
    const u32 *s = srcp;
    u32 *d = destp;
    u32 i;

    for (i = 0; i < size; i += 4) {
        *d++ = *s++;
    }
}

void MIi_CpuSend32(const void *srcp, volatile void *destp, u32 size)
{
    const u32 *s = srcp;
    volatile u32 *d = destp;
    u32 i;

    for (i = 0; i < size; i += 4) {
        *d = *s++;
    }
}

void MIi_CpuClearFast(u32 data, void *destp, u32 size)
{
    MIi_CpuClear32(data, destp, size);
}

void MIi_CpuCopyFast(const void *srcp, void *destp, u32 size)
{
    const u32 *s = srcp;
    u32 *d = destp;
    u32 blocks = size >> 5;
    u32 rest = size & 31;
    u32 tmp[8];
    u32 i;

    while (blocks--) {
        for (i = 0; i < 8; i++) {
            tmp[i] = s[i];
        }
        for (i = 0; i < 8; i++) {
            d[i] = tmp[i];
        }
        s += 8;
        d += 8;
    }
    for (i = 0; i < rest; i += 4) {
        *d++ = *s++;
    }
}

void MI_CpuFill8(void *dest, u8 data, u32 size)
{
    u8 *d = dest;

    while (size--) {
        *d++ = data;
    }
}

void MI_CpuCopy8(const void *src, void *dest, u32 size)
{
    const u8 *s = src;
    u8 *d = dest;

    while (size--) {
        *d++ = *s++;
    }
}

/* Fixed-size block copies and clears. All sizes are whole words. */

static void copyWords(const void *src, void *dest, u32 words)
{
    const u32 *s = src;
    u32 *d = dest;
    u32 tmp[32];
    u32 i;

    /* The originals load the whole block before storing it. */
    for (i = 0; i < words; i++) {
        tmp[i] = s[i];
    }
    for (i = 0; i < words; i++) {
        d[i] = tmp[i];
    }
}

void MI_Copy16B(const void *pSrc, void *pDest) { copyWords(pSrc, pDest, 4); }
void MI_Copy32B(const void *pSrc, void *pDest) { copyWords(pSrc, pDest, 8); }
void MI_Copy36B(const void *pSrc, void *pDest) { copyWords(pSrc, pDest, 9); }
void MI_Copy48B(const void *pSrc, void *pDest) { copyWords(pSrc, pDest, 12); }
void MI_Copy64B(const void *pSrc, void *pDest) { copyWords(pSrc, pDest, 16); }
void MI_Copy128B(const void *pSrc, void *pDest) { copyWords(pSrc, pDest, 32); }

void MI_Zero32B(void *pDest) { MIi_CpuClear32(0, pDest, 32); }
void MI_Zero36B(void *pDest) { MIi_CpuClear32(0, pDest, 36); }
void MI_Zero48B(void *pDest) { MIi_CpuClear32(0, pDest, 48); }
void MI_Zero64B(void *pDest) { MIi_CpuClear32(0, pDest, 64); }
