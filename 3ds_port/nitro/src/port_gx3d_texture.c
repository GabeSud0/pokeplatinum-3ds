/*
 * DS 3D texture decoding, shared by the PC reference rasterizer and the 3DS
 * GPU path (see nitro_port_gx3d_texture.h).
 *
 * Formats (TEXIMAGE_PARAM bits 26-28): 1 A3I5, 2 4-colour, 3 16-colour,
 * 4 256-colour, 5 4x4 compressed, 6 A5I3, 7 direct colour. Bit 29 makes
 * colour 0 transparent in the paletted formats 2-4.
 */
#include <nitro_port_gx3d_texture.h>
#include <string.h>

extern unsigned char NitroPort_IoReg[];
extern unsigned char NitroPort_LcdcVram[];

void NitroPort_Gx3dMapTextureBanks(NitroGx3dTexBanks *banks)
{
    unsigned i;
    memset(banks, 0, sizeof(*banks));
    /* A..D texture slots: 128 KiB each, controlled by VRAMCNT A-D. */
    for (i = 0; i < 4; i++) {
        unsigned v = NitroPort_IoReg[0x240 + i];
        if ((v & 0x87) == 0x83) banks->texture[(v >> 3) & 3] = NitroPort_LcdcVram + i * 0x20000;
    }
    /* E is 64 KiB; F/G are 16 KiB each. Standard texture palette banks. */
    if ((NitroPort_IoReg[0x244] & 0x87) == 0x83) {
        for (i = 0; i < 4; i++) banks->palette[i] = NitroPort_LcdcVram + 0x80000 + i * 0x4000;
    }
    /* F and G are 16 KiB: the offset field picks the palette slot, 0, 1,
     * 4 or 5 (GBATEK). The title screen maps G at slot 0. */
    for (i = 0; i < 2; i++) {
        unsigned v = NitroPort_IoReg[0x245 + i];
        if ((v & 0x87) == 0x83) {
            unsigned ofs = (v >> 3) & 3;
            banks->palette[(ofs & 1) + ((ofs >> 1) & 1) * 4] = NitroPort_LcdcVram + 0x90000 + i * 0x4000;
        }
    }
}

static int restrict_int(int x, int lo, int hi)
{
    return x < lo ? lo : x > hi ? hi : x;
}
static unsigned char texture_u8(const NitroGx3dTexBanks *banks, uint32_t offset)
{
    const unsigned char *bank = banks->texture[(offset >> 17) & 3];
    return bank ? bank[offset & 0x1ffff] : 0;
}
static uint16_t texture_u16(const NitroGx3dTexBanks *banks, uint32_t offset)
{
    return (uint16_t)(texture_u8(banks, offset) | ((uint16_t)texture_u8(banks, offset + 1) << 8));
}
static uint32_t texture_u32(const NitroGx3dTexBanks *banks, uint32_t offset)
{
    return (uint32_t)texture_u16(banks, offset) | ((uint32_t)texture_u16(banks, offset + 2) << 16);
}
static unsigned char palette_u8(const NitroGx3dTexBanks *banks, uint32_t byte_offset)
{
    unsigned slot = byte_offset >> 14;
    if (slot >= 6 || !banks->palette[slot]) return 0;
    return banks->palette[slot][byte_offset & 0x3fff];
}
static uint16_t palette_u16(const NitroGx3dTexBanks *banks, uint32_t byte_offset)
{
    unsigned slot = byte_offset >> 14;
    const unsigned char *p;
    if (slot >= 6 || !banks->palette[slot]) return 0;
    p = banks->palette[slot] + (byte_offset & 0x3ffe);
    return (uint16_t)((p[0] | ((unsigned)p[1] << 8)) & 0x7fff);
}
static int wrap_axis(int pos, int size, int repeat, int flip)
{
    if (pos >= 0 && pos < size) return pos;
    if (!repeat) return restrict_int(pos, 0, size - 1);
    if (flip) {
        int range = size * 2;
        int p = pos % range;
        if (p < 0) p += range;
        return p >= size ? range - 1 - p : p;
    }
    pos %= size;
    return pos < 0 ? pos + size : pos;
}
static uint16_t color_mix(uint16_t a, uint16_t b, int aw, int bw, int denom)
{
    int r = ((a & 31) * aw + (b & 31) * bw) / denom;
    int g = (((a >> 5) & 31) * aw + ((b >> 5) & 31) * bw) / denom;
    int bl = (((a >> 10) & 31) * aw + ((b >> 10) & 31) * bw) / denom;
    return (uint16_t)(r | (g << 5) | (bl << 10));
}
/* Format 5: where the block's descriptor is. Slot 1 holds them: its low
 * 64 KiB for blocks in slot 0, its upper 64 KiB for blocks in slot 2. */
static uint32_t descriptor_address(uint32_t texelAddr)
{
    uint32_t slot1Addr = 0x20000u + (texelAddr & 0x1ffffu) / 2u;
    if (texelAddr >= 0x40000u) slot1Addr += 0x10000u;
    return slot1Addr;
}

NitroGx3dTexel NitroPort_Gx3dSampleTexel(const NitroGx3dTexBanks *banks, uint32_t param,
                                         uint32_t base, int s, int t)
{
    NitroGx3dTexel result = { 0x7fff, 31 };
    unsigned fmt = NitroPort_Gx3dTexFormat(param);
    unsigned width = NitroPort_Gx3dTexWidth(param);
    unsigned height = NitroPort_Gx3dTexHeight(param);
    uint32_t address = (param & 0xffffu) * 8u;
    uint32_t paletteAddr = base * (fmt == 2 ? 8u : 16u);
    unsigned i = 0;
    if (fmt == 0) return result;
    s = wrap_axis(s, (int)width, (param >> 16) & 1, (param >> 18) & 1);
    t = wrap_axis(t, (int)height, (param >> 17) & 1, (param >> 19) & 1);
    i = (unsigned)t * width + (unsigned)s;
    if (fmt == 1) {
        uint8_t value = texture_u8(banks, address + i);
        unsigned a = value >> 5;
        result.alpha = (uint8_t)(a * 4 + a / 2);
        result.rgb = palette_u16(banks, paletteAddr + (value & 31) * 2);
    } else if (fmt == 2) {
        i = (texture_u8(banks, address + i / 4) >> ((i % 4) * 2)) & 3;
        result.rgb = palette_u16(banks, paletteAddr + i * 2);
        if (i == 0 && (param & (1u << 29))) result.alpha = 0;
    } else if (fmt == 3) {
        i = (texture_u8(banks, address + i / 2) >> ((i % 2) * 4)) & 15;
        result.rgb = palette_u16(banks, paletteAddr + i * 2);
        if (i == 0 && (param & (1u << 29))) result.alpha = 0;
    } else if (fmt == 4) {
        i = texture_u8(banks, address + i);
        result.rgb = palette_u16(banks, paletteAddr + i * 2);
        if (i == 0 && (param & (1u << 29))) result.alpha = 0;
    } else if (fmt == 5) {
        unsigned blocksPerRow = width / 4;
        uint32_t block = (uint32_t)(t / 4) * blocksPerRow + (uint32_t)(s / 4);
        uint32_t texelAddr = address + block * 4;
        uint16_t descriptor;
        unsigned mode, selector;
        uint16_t cols[4];
        uint32_t bits = texture_u32(banks, texelAddr);
        selector = (bits >> (((t & 3) * 4 + (s & 3)) * 2)) & 3;
        descriptor = texture_u16(banks, descriptor_address(texelAddr));
        mode = descriptor >> 14;
        paletteAddr = base * 16u + (descriptor & 0x3fffu) * 4u;
        cols[0] = palette_u16(banks, paletteAddr);
        cols[1] = palette_u16(banks, paletteAddr + 2);
        cols[2] = palette_u16(banks, paletteAddr + 4);
        cols[3] = palette_u16(banks, paletteAddr + 6);
        if (mode == 1) cols[2] = color_mix(cols[0], cols[1], 1, 1, 2);
        if (mode == 3) {
            cols[2] = color_mix(cols[0], cols[1], 5, 3, 8);
            cols[3] = color_mix(cols[0], cols[1], 3, 5, 8);
        }
        if (selector == 3 && mode <= 1) result.alpha = 0;
        result.rgb = cols[selector];
    } else if (fmt == 6) {
        uint8_t value = texture_u8(banks, address + i);
        result.alpha = value >> 3;
        result.rgb = palette_u16(banks, paletteAddr + (value & 7) * 2);
    } else if (fmt == 7) {
        uint16_t value = texture_u16(banks, address + i * 2);
        result.rgb = value & 0x7fff;
        result.alpha = (value & 0x8000) ? 31 : 0;
    }
    return result;
}

static uint32_t widen(unsigned v)
{
    return (uint32_t)((v << 3) | (v >> 2));
}

void NitroPort_Gx3dDecodeTexture(const NitroGx3dTexBanks *banks, uint32_t param,
                                 uint32_t palette_base, uint32_t *rgba8)
{
    unsigned width = NitroPort_Gx3dTexWidth(param);
    unsigned height = NitroPort_Gx3dTexHeight(param);
    unsigned s, t;
    for (t = 0; t < height; t++) {
        for (s = 0; s < width; s++) {
            NitroGx3dTexel texel = NitroPort_Gx3dSampleTexel(banks, param, palette_base, (int)s, (int)t);
            rgba8[t * width + s] = (widen(texel.rgb & 31) << 24) | (widen((texel.rgb >> 5) & 31) << 16) |
                                   (widen((texel.rgb >> 10) & 31) << 8) | widen(texel.alpha);
        }
    }
}

/* Position of (x, y) inside an 8x8 tile: x and y bits interleaved, x first. */
static unsigned morton8(unsigned x, unsigned y)
{
    return (x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3);
}

void NitroPort_Gx3dTileRGBA8(const uint32_t *linear, unsigned width, unsigned height, uint32_t *tiled)
{
    unsigned x, y;
    for (y = 0; y < height; y++) {
        /* The PICA reads row (height - 1 - t) for coordinate t: DS row y
         * goes in stored row height - 1 - y. */
        unsigned row = height - 1 - y;
        for (x = 0; x < width; x++) {
            unsigned index = (row & ~7u) * width + (x & ~7u) * 8u + morton8(x & 7, row & 7);
            tiled[index] = linear[y * width + x];
        }
    }
}

/* ------------------------------------------------- decoding for the PICA */
/* NitroPort_Gx3dSampleTexel gives every texel, but working out each one on
 * its own (wrapping, bank, palette entry) took the 3DS tens of milliseconds
 * for a 256x256 texture: a battle's Pokemon sprites are one, decoded again
 * for every frame of a palette fade. This does the same a row at a time,
 * the palette looked up once. The tests check it against the sampler. */

/* Where texel (s, t) goes in the PICA's tiled layout: rows bottom-up, 8x8
 * tiles, inside a tile x and y bits interleaved (morton8 above). */
static const uint8_t sMortonX[8] = { 0, 1, 4, 5, 16, 17, 20, 21 };
static const uint8_t sMortonY[8] = { 0, 2, 8, 10, 32, 34, 40, 42 };

static uint32_t rgba8(uint16_t rgb, unsigned alpha)
{
    return (widen(rgb & 31) << 24) | (widen((rgb >> 5) & 31) << 16) |
           (widen((rgb >> 10) & 31) << 8) | widen(alpha);
}

/* The bytes of one texture row: in the bank, or copied byte by byte (as
 * texture_u8 reads them) when the row crosses into another slot or its
 * slot has no bank. */
static const unsigned char *row_bytes(const NitroGx3dTexBanks *banks, uint32_t offset,
                                      uint32_t length, unsigned char *copy)
{
    const unsigned char *bank = banks->texture[(offset >> 17) & 3];
    uint32_t i;
    if (bank && (offset & 0x1ffffu) + length <= 0x20000u) return bank + (offset & 0x1ffffu);
    for (i = 0; i < length; i++) copy[i] = texture_u8(banks, offset + i);
    return copy;
}

void NitroPort_Gx3dDecodeTextureTiledRect(const NitroGx3dTexBanks *banks, uint32_t param,
                                          uint32_t palette_base, uint32_t *tiled,
                                          unsigned s0, unsigned t0, unsigned s1, unsigned t1)
{
    static const unsigned bitsPerTexel[8] = { 0, 8, 2, 4, 8, 2, 8, 16 };
    unsigned fmt = NitroPort_Gx3dTexFormat(param);
    unsigned width = NitroPort_Gx3dTexWidth(param);
    unsigned height = NitroPort_Gx3dTexHeight(param);
    uint32_t address = (param & 0xffffu) * 8u;
    uint32_t paletteAddr = palette_base * (fmt == 2 ? 8u : 16u);
    uint32_t rowLength = width * bitsPerTexel[fmt] / 8u;
    uint32_t table[256];
    unsigned char copy[2048];
    unsigned s, t, i;

    if (s1 > width) s1 = width;
    if (t1 > height) t1 = height;
    if (s0 >= s1 || t0 >= t1) return;

    /* Each texel value's RGBA8 word, from the palette. */
    switch (fmt) {
    case 1:   /* A3I5: 32 colours, 3-bit alpha */
        for (i = 0; i < 256; i++) {
            unsigned a = i >> 5;
            table[i] = rgba8(palette_u16(banks, paletteAddr + (i & 31) * 2), a * 4 + a / 2);
        }
        break;
    case 2: case 3: case 4: {   /* 4, 16 or 256 colours */
        unsigned colors = fmt == 2 ? 4 : fmt == 3 ? 16 : 256;
        for (i = 0; i < colors; i++) table[i] = rgba8(palette_u16(banks, paletteAddr + i * 2), 31);
        if (param & (1u << 29)) table[0] &= ~0xffu;   /* colour 0 transparent */
        break;
    }
    case 6:   /* A5I3: 8 colours, 5-bit alpha */
        for (i = 0; i < 256; i++) table[i] = rgba8(palette_u16(banks, paletteAddr + (i & 7) * 2), i >> 3);
        break;
    default:
        break;
    }

    if (fmt == 5) {
        /* 4x4 blocks: a 32-bit word of 2-bit selectors each, and a 16-bit
         * descriptor (palette entry and mode) in slot 1. */
        unsigned blocksPerRow = width / 4;
        unsigned bx, by;
        for (by = t0 / 4; by <= (t1 - 1) / 4; by++) {
            for (bx = s0 / 4; bx <= (s1 - 1) / 4; bx++) {
                uint32_t texelAddr = address + (by * blocksPerRow + bx) * 4;
                uint32_t bits = texture_u32(banks, texelAddr);
                uint16_t descriptor = texture_u16(banks, descriptor_address(texelAddr));
                unsigned mode = descriptor >> 14;
                uint32_t colorAddr = palette_base * 16u + (descriptor & 0x3fffu) * 4u;
                uint16_t cols[4];
                uint32_t words[4];
                unsigned y;
                for (i = 0; i < 4; i++) cols[i] = palette_u16(banks, colorAddr + i * 2);
                if (mode == 1) cols[2] = color_mix(cols[0], cols[1], 1, 1, 2);
                if (mode == 3) {
                    cols[2] = color_mix(cols[0], cols[1], 5, 3, 8);
                    cols[3] = color_mix(cols[0], cols[1], 3, 5, 8);
                }
                for (i = 0; i < 4; i++) words[i] = rgba8(cols[i], (i == 3 && mode <= 1) ? 0 : 31);
                for (y = 0; y < 4; y++) {
                    unsigned tt = by * 4 + y, row = height - 1 - tt, x;
                    uint32_t *out = tiled + (row & ~7u) * width + sMortonY[row & 7];
                    if (tt < t0 || tt >= t1) continue;
                    for (x = 0; x < 4; x++) {
                        unsigned ss = bx * 4 + x;
                        if (ss < s0 || ss >= s1) continue;
                        out[(ss & ~7u) * 8u + sMortonX[ss & 7]] = words[(bits >> ((y * 4 + x) * 2)) & 3];
                    }
                }
            }
        }
        return;
    }

    for (t = t0; t < t1; t++) {
        unsigned row = height - 1 - t;
        uint32_t *out = tiled + (row & ~7u) * width + sMortonY[row & 7];
        const unsigned char *src = fmt ? row_bytes(banks, address + t * rowLength, rowLength, copy) : NULL;
        /* Eight texels at a time (a tile's row: these places in it), any
         * texels before or after one at a time. */
        s = s0;
        for (;;) {
            if ((s & 7) == 0 && s + 8 <= s1) {
                uint32_t *o = out + s * 8u;
                const unsigned char *b = src ? src + s * bitsPerTexel[fmt] / 8u : NULL;
                switch (fmt) {
                case 0:
                    o[0] = o[1] = o[4] = o[5] = o[16] = o[17] = o[20] = o[21] = 0xffffffffu;
                    break;
                case 1: case 4: case 6:
                    o[0] = table[b[0]]; o[1] = table[b[1]]; o[4] = table[b[2]]; o[5] = table[b[3]];
                    o[16] = table[b[4]]; o[17] = table[b[5]]; o[20] = table[b[6]]; o[21] = table[b[7]];
                    break;
                case 2:
                    o[0] = table[b[0] & 3]; o[1] = table[(b[0] >> 2) & 3];
                    o[4] = table[(b[0] >> 4) & 3]; o[5] = table[b[0] >> 6];
                    o[16] = table[b[1] & 3]; o[17] = table[(b[1] >> 2) & 3];
                    o[20] = table[(b[1] >> 4) & 3]; o[21] = table[b[1] >> 6];
                    break;
                case 3:
                    o[0] = table[b[0] & 15]; o[1] = table[b[0] >> 4];
                    o[4] = table[b[1] & 15]; o[5] = table[b[1] >> 4];
                    o[16] = table[b[2] & 15]; o[17] = table[b[2] >> 4];
                    o[20] = table[b[3] & 15]; o[21] = table[b[3] >> 4];
                    break;
                case 7: {
                    static const uint8_t place[8] = { 0, 1, 4, 5, 16, 17, 20, 21 };
                    for (i = 0; i < 8; i++) {
                        unsigned value = b[i * 2] | ((unsigned)b[i * 2 + 1] << 8);
                        o[place[i]] = rgba8((uint16_t)(value & 0x7fff), (value & 0x8000) ? 31 : 0);
                    }
                    break;
                }
                }
                s += 8;
            } else if (s < s1) {
                uint32_t word;
                switch (fmt) {
                case 0: word = 0xffffffffu; break;
                case 2: word = table[(src[s >> 2] >> ((s & 3) * 2)) & 3]; break;
                case 3: word = table[(src[s >> 1] >> ((s & 1) * 4)) & 15]; break;
                case 7: {
                    unsigned value = src[s * 2] | ((unsigned)src[s * 2 + 1] << 8);
                    word = rgba8((uint16_t)(value & 0x7fff), (value & 0x8000) ? 31 : 0);
                    break;
                }
                default: word = table[src[s]]; break;
                }
                out[(s & ~7u) * 8u + sMortonX[s & 7]] = word;
                s++;
            } else {
                break;
            }
        }
    }
}

void NitroPort_Gx3dDecodeTextureTiled(const NitroGx3dTexBanks *banks, uint32_t param,
                                      uint32_t palette_base, uint32_t *tiled)
{
    NitroPort_Gx3dDecodeTextureTiledRect(banks, param, palette_base, tiled, 0, 0,
                                         NitroPort_Gx3dTexWidth(param),
                                         NitroPort_Gx3dTexHeight(param));
}

/* FNV-1a. */
static uint32_t mix_byte(uint32_t hash, unsigned char byte)
{
    return (hash ^ byte) * 16777619u;
}
static uint32_t mix_word(uint32_t hash, uint32_t word)
{
    hash = mix_byte(hash, (unsigned char)word);
    hash = mix_byte(hash, (unsigned char)(word >> 8));
    hash = mix_byte(hash, (unsigned char)(word >> 16));
    return mix_byte(hash, (unsigned char)(word >> 24));
}
static uint32_t mix_texture(uint32_t hash, const NitroGx3dTexBanks *banks, uint32_t offset, uint32_t length)
{
    while (length > 0) {
        const unsigned char *bank = banks->texture[(offset >> 17) & 3];
        uint32_t inBank = 0x20000u - (offset & 0x1ffffu);
        uint32_t n = length < inBank ? length : inBank;
        uint32_t i = 0;
        hash = mix_word(hash, bank ? 1u : 0u);
        if (bank) {
            const unsigned char *p = bank + (offset & 0x1ffff);
            /* A word at a time (each step still changes the hash for any
             * change in the word); texel data is 8-byte aligned. */
            if (((uintptr_t)p & 3u) == 0) {
                for (; i + 4 <= n; i += 4) {
                    uint32_t word;
                    memcpy(&word, p + i, 4);
                    hash = (hash ^ word) * 16777619u;
                }
            }
            for (; i < n; i++) hash = mix_byte(hash, p[i]);
        }
        offset += n;
        length -= n;
    }
    return hash;
}
static uint32_t mix_palette(uint32_t hash, const NitroGx3dTexBanks *banks, uint32_t offset, uint32_t length)
{
    uint32_t i;
    for (i = 0; i < length; i++) hash = mix_byte(hash, palette_u8(banks, offset + i));
    return hash;
}

uint32_t NitroPort_Gx3dTextureHash(const NitroGx3dTexBanks *banks, uint32_t param, uint32_t palette_base)
{
    static const unsigned bitsPerTexel[8] = { 0, 8, 2, 4, 8, 2, 8, 16 };
    static const unsigned paletteColors[8] = { 0, 32, 4, 16, 256, 0, 8, 0 };
    unsigned fmt = NitroPort_Gx3dTexFormat(param);
    unsigned width = NitroPort_Gx3dTexWidth(param);
    unsigned height = NitroPort_Gx3dTexHeight(param);
    uint32_t address = (param & 0xffffu) * 8u;
    uint32_t paletteAddr = palette_base * (fmt == 2 ? 8u : 16u);
    uint32_t hash = 2166136261u;
    unsigned i;

    hash = mix_word(hash, param);
    hash = mix_word(hash, palette_base);
    for (i = 0; i < 4; i++) hash = mix_word(hash, (uint32_t)(uintptr_t)banks->texture[i]);
    for (i = 0; i < 6; i++) hash = mix_word(hash, (uint32_t)(uintptr_t)banks->palette[i]);
    if (fmt == 0) return hash;

    hash = mix_texture(hash, banks, address, width * height * bitsPerTexel[fmt] / 8u);
    if (fmt == 5) {
        /* Each 4x4 block has a 16-bit descriptor in slot 1; it picks the
         * block's palette entries (up to 4 colours from palette_base). */
        uint32_t blocks = (width / 4u) * (height / 4u);
        uint32_t descriptors = descriptor_address(address);
        uint32_t lastColor = 0;
        uint32_t b;
        hash = mix_texture(hash, banks, descriptors, blocks * 2u);
        for (b = 0; b < blocks; b++) {
            uint32_t entry = (texture_u16(banks, descriptors + b * 2u) & 0x3fffu) * 2u + 4u;
            if (entry > lastColor) lastColor = entry;
        }
        hash = mix_palette(hash, banks, palette_base * 16u, lastColor * 2u);
    } else {
        hash = mix_palette(hash, banks, paletteAddr, paletteColors[fmt] * 2u);
    }
    return hash;
}
