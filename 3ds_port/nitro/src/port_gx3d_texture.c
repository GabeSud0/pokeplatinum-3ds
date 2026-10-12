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

void NitroPort_Gx3dDecodeTextureTiled(const NitroGx3dTexBanks *banks, uint32_t param,
                                      uint32_t palette_base, uint32_t *tiled)
{
    unsigned width = NitroPort_Gx3dTexWidth(param);
    unsigned height = NitroPort_Gx3dTexHeight(param);
    unsigned s, t;
    for (t = 0; t < height; t++) {
        unsigned row = height - 1 - t;
        for (s = 0; s < width; s++) {
            NitroGx3dTexel texel = NitroPort_Gx3dSampleTexel(banks, param, palette_base, (int)s, (int)t);
            tiled[(row & ~7u) * width + (s & ~7u) * 8u + morton8(s & 7, row & 7)] =
                (widen(texel.rgb & 31) << 24) | (widen((texel.rgb >> 5) & 31) << 16) |
                (widen((texel.rgb >> 10) & 31) << 8) | widen(texel.alpha);
        }
    }
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
        uint32_t i;
        hash = mix_word(hash, bank ? 1u : 0u);
        if (bank) {
            const unsigned char *p = bank + (offset & 0x1ffff);
            for (i = 0; i < n; i++) hash = mix_byte(hash, p[i]);
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
