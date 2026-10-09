/*
 * C versions of NitroSDK's decompressors (libraries/mi/src/mi_uncompress.c,
 * which is Metrowerks assembly only). The game stores most of its graphics
 * compressed, so these matter.
 *
 * Formats are the DS BIOS ones (GBATEK, "BIOS Decompression Functions"),
 * as decoded from the SDK's assembly. Every stream starts with a 32-bit
 * header: bits 4-7 the type, bits 0-3 a parameter, bits 8-31 the output size.
 *
 * The "8" and "16" variants differ only in how the DS writes the output
 * (bytes for main memory, halfwords for VRAM, which ignores byte writes).
 * Our VRAM is ordinary memory, so both produce the same bytes here.
 */

#include <nitro/types.h>
#include <nitro/mi/uncompress.h>
#include <stdint.h>

static u32 readHeader(const u8 **src)
{
    const u8 *p = *src;
    u32 header = (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);

    *src = p + 4;
    return header;
}

/* ------------------------------------------------------------ LZ77 */
/* Flag byte, then 8 blocks (MSB first): 0 = one literal byte, 1 = a copy of
 * earlier output. Normal copies: 3-18 bytes from up to 4096 bytes back.
 * Header parameter != 0 selects the extended format with longer copies. */

void MI_UncompressLZ8(const void *srcp, void *destp)
{
    const u8 *src = srcp;
    u8 *dest = destp;
    u32 header = readHeader(&src);
    s32 remaining = (s32)(header >> 8);
    BOOL extended = (header & 0x0F) != 0;

    while (remaining > 0) {
        u32 flags = *src++;
        int block;

        for (block = 0; block < 8 && remaining > 0; block++, flags <<= 1) {
            if (!(flags & 0x80)) {
                *dest++ = *src++;
                remaining--;
            } else {
                u32 b0 = src[0];
                u32 length;
                u32 distance;

                if (!extended) {
                    length = 3;
                } else if (b0 & 0xE0) {
                    length = 1;
                } else {
                    src++;
                    length = (b0 & 0x0F) << 4;
                    if (b0 & 0x10) {
                        length <<= 8;
                        length += (u32)*src++ << 4;
                        length += 0x100;
                    }
                    length += 0x11;
                    b0 = src[0];
                }
                length += b0 >> 4;
                distance = (((b0 & 0x0F) << 8) | src[1]) + 1;
                src += 2;
                remaining -= (s32)length;
                while (length--) {
                    *dest = *(dest - distance);
                    dest++;
                }
            }
        }
    }
}

void MI_UncompressLZ16(const void *srcp, void *destp)
{
    MI_UncompressLZ8(srcp, destp);
}

/* ------------------------------------------------------- run length */
/* Flag byte: bit 7 set = the next byte repeated (flag & 0x7F) + 3 times;
 * clear = (flag & 0x7F) + 1 literal bytes follow. */

void MI_UncompressRL8(const void *srcp, void *destp)
{
    const u8 *src = srcp;
    u8 *dest = destp;
    s32 remaining = (s32)(readHeader(&src) >> 8);

    while (remaining > 0) {
        u32 flag = *src++;
        u32 length;

        if (flag & 0x80) {
            u8 value = *src++;
            length = (flag & 0x7F) + 3;
            remaining -= (s32)length;
            while (length--) {
                *dest++ = value;
            }
        } else {
            length = (flag & 0x7F) + 1;
            remaining -= (s32)length;
            while (length--) {
                *dest++ = *src++;
            }
        }
    }
}

void MI_UncompressRL16(const void *srcp, void *destp)
{
    MI_UncompressRL8(srcp, destp);
}

/* ---------------------------------------------------------- Huffman */
/* After the header: a tree (its first byte is the tree size in halfwords
 * minus one, the root node follows), then the bit stream as 32-bit
 * little-endian words read from the top bit down. Each node byte: bits 0-5
 * offset to its children, bit 7 = left child is a leaf, bit 6 = right child
 * is a leaf. Leaves hold 4- or 8-bit values (header parameter), packed into
 * the output words from the low bits up. */

void MI_UncompressHuffman(const void *srcp, void *destp)
{
    const u8 *src = srcp;
    u32 *dest = destp;
    u32 header = readHeader(&src);
    u32 dataBits = header & 0x0F;
    s32 remaining = (s32)(header >> 8);
    const u8 *tree = src;
    const u8 *root = tree + 1;
    const u8 *node = root;
    const u8 *bits = tree + ((u32)tree[0] + 1) * 2;
    u32 unitsPerWord = (dataBits & 7) + 4; /* 8 for 4-bit data, 4 for 8-bit */
    u32 out = 0;
    u32 units = 0;

    while (remaining > 0) {
        u32 word = (u32)bits[0] | ((u32)bits[1] << 8) | ((u32)bits[2] << 16) | ((u32)bits[3] << 24);
        int i;

        bits += 4;
        for (i = 0; i < 32 && remaining > 0; i++, word <<= 1) {
            u32 bit = word >> 31;
            u32 nodeByte = *node;
            u32 leafMask = bit ? 0x40 : 0x80;
            /* Children pair: next even address after this node, plus offset. */
            const u8 *child = (const u8 *)((uintptr_t)node & ~(uintptr_t)1) + ((nodeByte & 0x3F) + 1) * 2 + bit;

            if (nodeByte & leafMask) {
                out = (out >> dataBits) | ((u32)*child << (32 - dataBits));
                node = root;
                if (++units == unitsPerWord) {
                    *dest++ = out;
                    remaining -= 4;
                    units = 0;
                }
            } else {
                node = child;
            }
        }
    }
}

/* ------------------------------------------------------- filters */

void MI_UnfilterDiff8(const void *srcp, void *destp)
{
    const u8 *src = srcp;
    u8 *dest = destp;
    u32 size = readHeader(&src) >> 8;
    u8 sum = 0;
    u32 i;

    for (i = 0; i < size; i++) {
        sum = (u8)(sum + src[i]);
        dest[i] = sum;
    }
}

void MI_UnfilterDiff16(const void *srcp, void *destp)
{
    const u8 *src = srcp;
    u16 *dest = destp;
    u32 size = readHeader(&src) >> 8;
    u16 sum = 0;
    u32 i;

    for (i = 0; i < size; i += 2) {
        sum = (u16)(sum + (src[i] | (src[i + 1] << 8)));
        *dest++ = sum;
    }
}

/* ------------------------------------------------------- bit unpacking */
/* Expands each srcBitNum-bit value (taken from the low bits of each source
 * byte upward) to destBitNum bits, adding destOffset to non-zero values
 * (or to all of them if destOffset0_on), packing results into 32-bit words. */

void MI_UnpackBits(const void *srcp, void *destp, MIUnpackBitsParam *paramp)
{
    const u8 *src = srcp;
    u32 *dest = destp;
    u32 srcBits = paramp->srcBitNum;
    u32 destBits = paramp->destBitNum;
    u32 offset = paramp->destOffset;
    BOOL offsetZero = paramp->destOffset0_on;
    u32 srcMask = (srcBits >= 32) ? 0xFFFFFFFFu : ((1u << srcBits) - 1);
    u32 out = 0;
    u32 outBits = 0;
    u32 n;

    for (n = 0; n < paramp->srcNum; n++) {
        u32 byte = src[n];
        u32 shift;

        for (shift = 0; shift < 8; shift += srcBits) {
            u32 value = (byte >> shift) & srcMask;

            if (value != 0 || offsetZero) {
                value += offset;
            }
            out |= value << outBits;
            outBits += destBits;
            if (outBits >= 32) {
                *dest++ = out;
                out = 0;
                outBits = 0;
            }
        }
    }
}
