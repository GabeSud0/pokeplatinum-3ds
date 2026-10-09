/*
 * MD5 ("Hash1") and SHA-1 ("Hash2") for NitroSDK's DGT interface.
 *
 * NitroSDK ships these only as a prebuilt Metrowerks library
 * (libraries/dgt/prebuilt/libdgt.a), which can't be used here, so this is a
 * plain C implementation of the two standard algorithms (RFC 1321 and
 * FIPS 180-1) using the SDK's context structures. The game reaches them
 * through MATH_CalcMD5 / MATH_CalcSHA1.
 *
 * HMAC and the download-play signature helpers are not implemented; the game
 * doesn't call them.
 */

#include <nitro/types.h>
#include <nitro/dgt/dgt.h>
#include <string.h>

/* ---------------------------------------------------------------- MD5 */

#define ROTL32(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

static const u32 md5K[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a,
    0xa8304613, 0xfd469501, 0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be,
    0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340,
    0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8,
    0x676f02d9, 0x8d2a4c8a, 0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c,
    0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70, 0x289b7ec6, 0xeaa127fa,
    0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92,
    0xffeff47d, 0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
    0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391,
};

static const u8 md5R[64] = {
    7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
    5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
    4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
    6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21,
};

static void md5Block(DGTHash1Context *ctx, const u8 *block)
{
    u32 w[16];
    u32 a = (u32)ctx->a, b = (u32)ctx->b, c = (u32)ctx->c, d = (u32)ctx->d;
    int i;

    for (i = 0; i < 16; i++) {
        w[i] = (u32)block[i * 4] | ((u32)block[i * 4 + 1] << 8) |
               ((u32)block[i * 4 + 2] << 16) | ((u32)block[i * 4 + 3] << 24);
    }

    for (i = 0; i < 64; i++) {
        u32 f, g, tmp;

        if (i < 16) {
            f = (b & c) | (~b & d);
            g = (u32)i;
        } else if (i < 32) {
            f = (d & b) | (~d & c);
            g = (u32)(5 * i + 1) & 15;
        } else if (i < 48) {
            f = b ^ c ^ d;
            g = (u32)(3 * i + 5) & 15;
        } else {
            f = c ^ (b | ~d);
            g = (u32)(7 * i) & 15;
        }
        tmp = d;
        d = c;
        c = b;
        b = b + ROTL32(a + f + md5K[i] + w[g], md5R[i]);
        a = tmp;
    }

    ctx->a = (u32)(ctx->a + a);
    ctx->b = (u32)(ctx->b + b);
    ctx->c = (u32)(ctx->c + c);
    ctx->d = (u32)(ctx->d + d);
}

void DGT_Hash1Reset(DGTHash1Context *ctx)
{
    ctx->a = 0x67452301;
    ctx->b = 0xefcdab89;
    ctx->c = 0x98badcfe;
    ctx->d = 0x10325476;
    ctx->length = 0;
}

void DGT_Hash1SetSource(DGTHash1Context *ctx, const void *src, unsigned long len)
{
    const u8 *p = src;
    u32 used = (u32)(ctx->length & 63);

    ctx->length += len;

    if (used != 0) {
        u32 take = 64 - used;
        if (take > len) {
            take = (u32)len;
        }
        memcpy(ctx->buffer8 + used, p, take);
        p += take;
        len -= take;
        if (used + take < 64) {
            return;
        }
        md5Block(ctx, ctx->buffer8);
    }
    while (len >= 64) {
        md5Block(ctx, p);
        p += 64;
        len -= 64;
    }
    memcpy(ctx->buffer8, p, len);
}

void DGT_Hash1GetDigest_R(void *digest, DGTHash1Context *ctx)
{
    static const u8 pad[64] = { 0x80 };
    u64 bits = (u64)ctx->length * 8;
    u32 used = (u32)(ctx->length & 63);
    u8 lenBytes[8];
    u8 *out = digest;
    int i;

    for (i = 0; i < 8; i++) {
        lenBytes[i] = (u8)(bits >> (8 * i));
    }
    DGT_Hash1SetSource(ctx, pad, (used < 56) ? (56 - used) : (120 - used));
    DGT_Hash1SetSource(ctx, lenBytes, 8);

    for (i = 0; i < 4; i++) {
        u32 v = (u32)ctx->state[i];
        out[i * 4 + 0] = (u8)v;
        out[i * 4 + 1] = (u8)(v >> 8);
        out[i * 4 + 2] = (u8)(v >> 16);
        out[i * 4 + 3] = (u8)(v >> 24);
    }
}

/* ---------------------------------------------------------------- SHA-1 */
/* Uses the context's h0..h4, Nl/Nh (message length in bits), num (bytes
 * waiting in the buffer) and data[] (the buffer, as bytes). */

static void sha1Block(DGTHash2Context *ctx, const u8 *block)
{
    u32 w[80];
    u32 a = (u32)ctx->h0, b = (u32)ctx->h1, c = (u32)ctx->h2, d = (u32)ctx->h3, e = (u32)ctx->h4;
    int i;

    for (i = 0; i < 16; i++) {
        w[i] = ((u32)block[i * 4] << 24) | ((u32)block[i * 4 + 1] << 16) |
               ((u32)block[i * 4 + 2] << 8) | (u32)block[i * 4 + 3];
    }
    for (i = 16; i < 80; i++) {
        w[i] = ROTL32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }

    for (i = 0; i < 80; i++) {
        u32 f, k, tmp;

        if (i < 20) {
            f = (b & c) | (~b & d);
            k = 0x5a827999;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ed9eba1;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8f1bbcdc;
        } else {
            f = b ^ c ^ d;
            k = 0xca62c1d6;
        }
        tmp = ROTL32(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = ROTL32(b, 30);
        b = a;
        a = tmp;
    }

    ctx->h0 = (u32)(ctx->h0 + a);
    ctx->h1 = (u32)(ctx->h1 + b);
    ctx->h2 = (u32)(ctx->h2 + c);
    ctx->h3 = (u32)(ctx->h3 + d);
    ctx->h4 = (u32)(ctx->h4 + e);
}

void DGT_Hash2Reset(DGTHash2Context *ctx)
{
    ctx->h0 = 0x67452301;
    ctx->h1 = 0xefcdab89;
    ctx->h2 = 0x98badcfe;
    ctx->h3 = 0x10325476;
    ctx->h4 = 0xc3d2e1f0;
    ctx->Nl = 0;
    ctx->Nh = 0;
    ctx->num = 0;
}

void DGT_Hash2SetSource(DGTHash2Context *ctx, const unsigned char *src, unsigned long len)
{
    u8 *buf = (u8 *)ctx->data;
    u64 bits = ((u64)(u32)ctx->Nh << 32) | (u32)ctx->Nl;

    bits += (u64)len * 8;
    ctx->Nl = (u32)bits;
    ctx->Nh = (u32)(bits >> 32);

    while (len > 0) {
        u32 take = 64 - (u32)ctx->num;
        if (take > len) {
            take = (u32)len;
        }
        memcpy(buf + ctx->num, src, take);
        ctx->num += (int)take;
        src += take;
        len -= take;
        if (ctx->num == 64) {
            sha1Block(ctx, buf);
            ctx->num = 0;
        }
    }
}

void DGT_Hash2GetDigest(DGTHash2Context *ctx, unsigned char *digest)
{
    static const u8 pad[64] = { 0x80 };
    u64 bits = ((u64)(u32)ctx->Nh << 32) | (u32)ctx->Nl;
    u32 used = (u32)ctx->num;
    u8 lenBytes[8];
    u32 h[5];
    int i;

    for (i = 0; i < 8; i++) {
        lenBytes[i] = (u8)(bits >> (56 - 8 * i));
    }
    DGT_Hash2SetSource(ctx, pad, (used < 56) ? (56 - used) : (120 - used));
    DGT_Hash2SetSource(ctx, lenBytes, 8);

    h[0] = (u32)ctx->h0;
    h[1] = (u32)ctx->h1;
    h[2] = (u32)ctx->h2;
    h[3] = (u32)ctx->h3;
    h[4] = (u32)ctx->h4;
    for (i = 0; i < 5; i++) {
        digest[i * 4 + 0] = (u8)(h[i] >> 24);
        digest[i * 4 + 1] = (u8)(h[i] >> 16);
        digest[i * 4 + 2] = (u8)(h[i] >> 8);
        digest[i * 4 + 3] = (u8)h[i];
    }
}
