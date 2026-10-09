/*
 * Unit tests for libnitro (NitroSDK math and memory, built with GCC).
 *
 * Runs on the PC: `make` in 3ds_port/nitro. Each check compares against a
 * value worked out independently (by hand, from the DS hardware docs, or with
 * Python), or against another part of the SDK that is still Nintendo's
 * original C code, so a mistake in our C translations can't hide behind
 * itself.
 */

#include <nitro.h>
#include <stdio.h>
#include <string.h>

static int sChecks, sFailures;

#define CHECK(cond)                                                          \
    do {                                                                     \
        sChecks++;                                                           \
        if (!(cond)) {                                                       \
            sFailures++;                                                     \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);           \
        }                                                                    \
    } while (0)

#define CHECK_EQ(actual, expected)                                           \
    do {                                                                     \
        long long a_ = (long long)(actual), e_ = (long long)(expected);      \
        sChecks++;                                                           \
        if (a_ != e_) {                                                      \
            sFailures++;                                                     \
            printf("FAIL %s:%d: %s = %lld (0x%llx), expected %lld (0x%llx)\n", \
                   __FILE__, __LINE__, #actual, a_, (unsigned long long)a_,  \
                   e_, (unsigned long long)e_);                              \
        }                                                                    \
    } while (0)

#define ONE FX32_ONE

/* ------------------------------------------------------------------------ */

static void test_types(void)
{
    /* The whole port depends on these matching the DS. */
    CHECK_EQ(sizeof(u8), 1);
    CHECK_EQ(sizeof(u16), 2);
    CHECK_EQ(sizeof(u32), 4);
    CHECK_EQ(sizeof(u64), 8);
    CHECK_EQ(sizeof(fx32), 4);
    CHECK_EQ(sizeof(MtxFx43), 48);
}

static void test_divider_hardware(void)
{
    /* Plain 32/32 division, truncating toward zero. */
    CP_SetDiv32_32(100, 7);
    CHECK_EQ(CP_GetDivResult32(), 14);
    CHECK_EQ(CP_GetDivRemainder32(), 2);

    CP_SetDiv32_32((u32)-100, 7);
    CHECK_EQ(CP_GetDivResult32(), -14);
    CHECK_EQ(CP_GetDivRemainder32(), -2);

    /* 64/32 and 64/64. */
    CP_SetDiv64_32(10000000000ULL, 3);
    CHECK_EQ(CP_GetDivResult64(), 3333333333LL);
    CHECK_EQ(CP_GetDivRemainder64(), 1);

    CP_SetDiv64_64((u64)-10000000000LL, 3000000000ULL);
    CHECK_EQ(CP_GetDivResult64(), -3);
    CHECK_EQ(CP_GetDivRemainder64(), -1000000000LL);

    /* Division by zero: quotient -1 (+1 for negative numerators),
     * remainder = numerator, error flag set. */
    CP_SetDiv32_32(7, 0);
    CHECK_EQ(CP_GetDivResult32(), -1);
    CHECK_EQ(CP_GetDivRemainder32(), 7);
    CHECK(reg_CP_DIVCNT & REG_CP_DIVCNT_DIV0_MASK);

    CP_SetDiv32_32((u32)-7, 0);
    CHECK_EQ(CP_GetDivResult32(), 1);
    CHECK_EQ(CP_GetDivRemainder32(), -7);

    CP_SetDiv32_32(7, 1);
    CHECK(!(reg_CP_DIVCNT & REG_CP_DIVCNT_DIV0_MASK));
    CHECK(!CP_IsDivBusy());

    /* -2^31 / -1 in 32-bit mode doesn't overflow the 64-bit result. */
    CP_SetDiv32_32(0x80000000u, (u32)-1);
    CHECK_EQ(CP_GetDivResult64(), 0x80000000LL);
    CHECK_EQ(CP_GetDivResult32(), (s32)0x80000000);
    CHECK_EQ(CP_GetDivRemainder32(), 0);

    /* The fx wrappers the game calls. */
    CHECK_EQ(FX_DivS32(-7, 2), -3);
    CHECK_EQ(FX_ModS32(-7, 3), -1);
}

static void test_sqrt_hardware(void)
{
    CP_SetSqrt32(16);
    CHECK_EQ(CP_GetSqrtResult32(), 4);
    CP_SetSqrt32(17);
    CHECK_EQ(CP_GetSqrtResult32(), 4);
    CP_SetSqrt64(1ULL << 62);
    CHECK_EQ(CP_GetSqrtResult32(), 1u << 31);
    CP_SetSqrt64(0xFFFFFFFFFFFFFFFFULL);
    CHECK_EQ(CP_GetSqrtResult32(), 0xFFFFFFFFu);
    CP_SetSqrt64(0);
    CHECK_EQ(CP_GetSqrtResult32(), 0);
    CHECK(!CP_IsSqrtBusy());
}

static void test_fixed_point(void)
{
    CHECK_EQ(FX_Mul(2 * ONE, 3 * ONE), 6 * ONE);
    CHECK_EQ(FX_Mul(-ONE / 2, ONE / 2), -ONE / 4);

    /* (4096 << 32) / 12288 = 1431655765, rounded >> 20 = 1365 */
    CHECK_EQ(FX_Div(ONE, 3 * ONE), 1365);
    CHECK_EQ(FX_Div(-ONE * 15 / 2, ONE * 5 / 2), -3 * ONE);
    CHECK_EQ(FX_Inv(2 * ONE), ONE / 2);

    /* sqrt(2) * 4096 = 5792.6, and the SDK rounds to 5793 */
    CHECK_EQ(FX_Sqrt(4 * ONE), 2 * ONE);
    CHECK_EQ(FX_Sqrt(2 * ONE), 5793);
    CHECK_EQ(FX_Sqrt(0), 0);

    /* Trig tables: 0x10000 is a full circle. */
    CHECK_EQ(FX_SinIdx(0), 0);
    CHECK_EQ(FX_SinIdx(0x4000), ONE);
    CHECK_EQ(FX_CosIdx(0), ONE);
    CHECK_EQ(FX_CosIdx(0x8000), -ONE);
    CHECK_EQ(FX_SinIdx(0xC000), -ONE);

    CHECK_EQ(FX_Atan2Idx(ONE, ONE), 0x2000);   /* 45 degrees */
    CHECK_EQ(FX_Atan2Idx(ONE, 0), 0x4000);     /* 90 degrees */
}

static void test_vectors(void)
{
    VecFx32 a = { 3 * ONE, 4 * ONE, 0 };
    VecFx32 n;

    CHECK_EQ(VEC_Mag(&a), 5 * ONE);
    VEC_Normalize(&a, &n);
    CHECK(n.x >= 2457 - 1 && n.x <= 2457 + 1);  /* 0.6 */
    CHECK(n.y >= 3277 - 1 && n.y <= 3277 + 1);  /* 0.8 */
    CHECK_EQ(n.z, 0);
}

/* ------------------------------------------------------------------------ */
/* Matrices: our C versions replace Metrowerks assembly. Check them against
 * explicit expected matrices, against each other, and against the SDK's
 * original C functions (MTX_RotAxis33, MTX_Concat33, MTX_Inverse33). */

static int mtx33Equal(const MtxFx33 *a, const MtxFx33 *b, int tolerance)
{
    int i;

    for (i = 0; i < 9; i++) {
        int d = a->a[i] - b->a[i];
        if (d < -tolerance || d > tolerance) {
            return 0;
        }
    }
    return 1;
}

static void test_matrix_basic(void)
{
    static const fx32 identity33[9] = { ONE, 0, 0, 0, ONE, 0, 0, 0, ONE };
    MtxFx22 m22;
    MtxFx33 m33, t33;
    MtxFx43 m43;
    MtxFx44 m44;
    int i;

    memset(&m33, 0x55, sizeof(m33));
    MTX_Identity33(&m33);
    CHECK(memcmp(m33.a, identity33, sizeof(identity33)) == 0);

    memset(&m22, 0x55, sizeof(m22));
    MTX_Identity22(&m22);
    CHECK(m22._00 == ONE && m22._01 == 0 && m22._10 == 0 && m22._11 == ONE);

    memset(&m43, 0x55, sizeof(m43));
    MTX_Identity43(&m43);
    for (i = 0; i < 12; i++) {
        CHECK_EQ(m43.a[i], (i == 0 || i == 4 || i == 8) ? ONE : 0);
    }

    memset(&m44, 0x55, sizeof(m44));
    MTX_Identity44(&m44);
    for (i = 0; i < 16; i++) {
        CHECK_EQ(m44.a[i], (i % 5 == 0) ? ONE : 0);
    }

    /* Scale */
    MTX_Scale33(&m33, 2, 3, 4);
    CHECK(m33._00 == 2 && m33._11 == 3 && m33._22 == 4 && m33._01 == 0 && m33._21 == 0);
    MTX_Scale43(&m43, 2, 3, 4);
    CHECK(m43._00 == 2 && m43._11 == 3 && m43._22 == 4 && m43._30 == 0 && m43._32 == 0);
    MTX_Scale44(&m44, 2, 3, 4);
    CHECK(m44._00 == 2 && m44._11 == 3 && m44._22 == 4 && m44._33 == ONE && m44._23 == 0);
    MTX_Scale22(&m22, 5, 6);
    CHECK(m22._00 == 5 && m22._11 == 6 && m22._01 == 0 && m22._10 == 0);

    /* Transpose, including in place */
    for (i = 0; i < 9; i++) {
        m33.a[i] = i + 1;
    }
    MTX_Transpose33(&m33, &t33);
    CHECK(t33._01 == m33._10 && t33._02 == m33._20 && t33._12 == m33._21 && t33._11 == m33._11);
    MTX_Transpose33(&m33, &m33);
    CHECK(mtx33Equal(&m33, &t33, 0));

    for (i = 0; i < 16; i++) {
        m44.a[i] = i + 1;
    }
    MTX_Transpose44(&m44, &m44);
    CHECK(m44._01 == 5 && m44._10 == 2 && m44._03 == 13 && m44._30 == 4 && m44._33 == 16);

    for (i = 0; i < 12; i++) {
        m43.a[i] = i + 1;
    }
    MTX_Transpose43(&m43, &m43);
    CHECK(m43._01 == 4 && m43._10 == 2 && m43._22 == 9 && m43._30 == 0 && m43._32 == 0);

    m22._00 = 1; m22._01 = 2; m22._10 = 3; m22._11 = 4;
    MTX_Transpose22(&m22, &m22);
    CHECK(m22._00 == 1 && m22._01 == 3 && m22._10 == 2 && m22._11 == 4);

    /* Copies between sizes */
    for (i = 0; i < 9; i++) {
        m33.a[i] = i + 1;
    }
    MTX_Copy33To44(&m33, &m44);
    CHECK(m44._00 == 1 && m44._12 == 6 && m44._22 == 9 && m44._03 == 0 && m44._30 == 0 && m44._33 == ONE);
    MTX_Copy33To43(&m33, &m43);
    CHECK(m43._22 == 9 && m43._30 == 0 && m43._31 == 0 && m43._32 == 0);
    for (i = 0; i < 12; i++) {
        m43.a[i] = i + 1;
    }
    MTX_Copy43To44(&m43, &m44);
    CHECK(m44._30 == 10 && m44._32 == 12 && m44._13 == 0 && m44._33 == ONE);
    MTX_Copy44To43(&m44, &m43);
    CHECK(m43._30 == 10 && m43._32 == 12 && m43._00 == 1);
    MTX_Copy44To33(&m44, &m33);
    CHECK(m33._00 == 1 && m33._22 == 9 && m33._12 == 6);
}

static void test_matrix_rotation(void)
{
    static const VecFx32 axisX = { ONE, 0, 0 };
    static const VecFx32 axisY = { 0, ONE, 0 };
    static const VecFx32 axisZ = { 0, 0, ONE };
    int angle;

    for (angle = 0; angle < 0x10000; angle += 0x0B6D) {  /* ~16 degrees apart */
        fx32 s = FX_SinIdx(angle), c = FX_CosIdx(angle);
        MtxFx33 rot, ref, back, product, inv;
        MtxFx43 r43;
        MtxFx44 r44;
        MtxFx22 r22;
        int i;

        /* Each rotation must equal the SDK's own C axis-angle rotation. */
        MTX_RotX33(&rot, s, c);
        MTX_RotAxis33(&ref, &axisX, s, c);
        CHECK(mtx33Equal(&rot, &ref, 1));

        MTX_RotY33(&rot, s, c);
        MTX_RotAxis33(&ref, &axisY, s, c);
        CHECK(mtx33Equal(&rot, &ref, 1));

        MTX_RotZ33(&rot, s, c);
        MTX_RotAxis33(&ref, &axisZ, s, c);
        CHECK(mtx33Equal(&rot, &ref, 1));

        /* R * R^T = I and inverse(R) = R^T, using Nintendo's C code. */
        MTX_Transpose33(&rot, &back);
        MTX_Concat33(&rot, &back, &product);
        MTX_Identity33(&ref);
        CHECK(mtx33Equal(&product, &ref, 4));
        CHECK(MTX_Inverse33(&rot, &inv) == 0);
        CHECK(mtx33Equal(&inv, &back, 4));

        /* 4x3 and 4x4 versions have the same 3x3 part. */
        MTX_RotX43(&r43, s, c);
        MTX_RotX33(&rot, s, c);
        MTX_Copy43To33(&r43, &ref);
        CHECK(mtx33Equal(&ref, &rot, 0));
        CHECK(r43._30 == 0 && r43._31 == 0 && r43._32 == 0);

        MTX_RotY43(&r43, s, c);
        MTX_RotY33(&rot, s, c);
        MTX_Copy43To33(&r43, &ref);
        CHECK(mtx33Equal(&ref, &rot, 0));

        MTX_RotZ43(&r43, s, c);
        MTX_RotZ33(&rot, s, c);
        MTX_Copy43To33(&r43, &ref);
        CHECK(mtx33Equal(&ref, &rot, 0));

        MTX_RotX44(&r44, s, c);
        MTX_RotX33(&rot, s, c);
        MTX_Copy44To33(&r44, &ref);
        CHECK(mtx33Equal(&ref, &rot, 0));
        for (i = 0; i < 3; i++) {
            CHECK(r44.m[i][3] == 0 && r44.m[3][i] == 0);
        }
        CHECK(r44._33 == ONE);

        MTX_RotY44(&r44, s, c);
        MTX_RotY33(&rot, s, c);
        MTX_Copy44To33(&r44, &ref);
        CHECK(mtx33Equal(&ref, &rot, 0));

        MTX_RotZ44(&r44, s, c);
        MTX_RotZ33(&rot, s, c);
        MTX_Copy44To33(&r44, &ref);
        CHECK(mtx33Equal(&ref, &rot, 0));

        /* 2x2 matches the top-left of the Z rotation. */
        MTX_Rot22(&r22, s, c);
        CHECK(r22._00 == rot._00 && r22._01 == rot._01 && r22._10 == rot._10 && r22._11 == rot._11);
    }
}

/* ------------------------------------------------------------------------ */

static void test_memory(void)
{
    static u8 src[256], dst[256];
    static vu32 port32;
    static vu16 port16;
    static vu32 lock;
    static vu8 lock8;
    int i;

    for (i = 0; i < 256; i++) {
        src[i] = (u8)(i * 7 + 1);
    }

    memset(dst, 0, sizeof(dst));
    MI_CpuCopy8(src + 1, dst + 3, 13);
    CHECK(memcmp(dst + 3, src + 1, 13) == 0 && dst[2] == 0 && dst[16] == 0);

    MI_CpuFill8(dst, 0xAB, 5);
    CHECK(dst[0] == 0xAB && dst[4] == 0xAB && dst[5] != 0xAB);

    memset(dst, 0, sizeof(dst));
    MI_CpuCopy16(src, dst, 34);
    CHECK(memcmp(dst, src, 34) == 0 && dst[34] == 0);

    memset(dst, 0, sizeof(dst));
    MI_CpuCopy32(src, dst, 64);
    CHECK(memcmp(dst, src, 64) == 0 && dst[64] == 0);

    /* Fast copy: 32-byte blocks plus a word tail */
    memset(dst, 0, sizeof(dst));
    MI_CpuCopyFast(src, dst, 100);
    CHECK(memcmp(dst, src, 100) == 0 && dst[100] == 0);

    MI_CpuFill32(dst, 0x11223344, 12);
    CHECK(((u32 *)dst)[0] == 0x11223344 && ((u32 *)dst)[2] == 0x11223344 && ((u32 *)dst)[3] != 0x11223344);
    MI_CpuFill16(dst, 0xBEEF, 6);
    CHECK(((u16 *)dst)[0] == 0xBEEF && ((u16 *)dst)[2] == 0xBEEF);
    MI_CpuFillFast(dst, 0xCAFEF00D, 96);
    CHECK(((u32 *)dst)[0] == 0xCAFEF00D && ((u32 *)dst)[23] == 0xCAFEF00D && ((u32 *)dst)[24] != 0xCAFEF00D);
    MI_CpuClearFast(dst, 64);
    CHECK(((u32 *)dst)[0] == 0 && ((u32 *)dst)[15] == 0);

    /* Forward copy into an overlapping region repeats the first word,
     * exactly like the original word-by-word assembly. */
    ((u32 *)dst)[0] = 0xAAAAAAAA;
    ((u32 *)dst)[1] = 0xBBBBBBBB;
    ((u32 *)dst)[2] = 0xCCCCCCCC;
    MI_CpuCopy32(dst, dst + 4, 8);
    CHECK(((u32 *)dst)[1] == 0xAAAAAAAA && ((u32 *)dst)[2] == 0xAAAAAAAA);

    /* Send: every value goes to the same address (a hardware FIFO). */
    MI_CpuSend32(src, &port32, 16);
    CHECK_EQ(port32, ((u32 *)src)[3]);
    MI_CpuSend16(src, &port16, 8);
    CHECK_EQ(port16, ((u16 *)src)[3]);

    /* Fixed-size blocks */
    memset(dst, 0, sizeof(dst));
    MI_Copy36B(src, dst);
    CHECK(memcmp(dst, src, 36) == 0 && dst[36] == 0);
    MI_Copy128B(src, dst);
    CHECK(memcmp(dst, src, 128) == 0);
    MI_Zero48B(dst);
    CHECK(dst[0] == 0 && dst[47] == 0 && dst[48] == src[48]);

    /* Atomic swap */
    lock = 5;
    CHECK_EQ(MI_SwapWord(9, &lock), 5);
    CHECK_EQ(lock, 9);
    lock8 = 1;
    CHECK_EQ(MI_SwapByte(0, &lock8), 1);
    CHECK_EQ(lock8, 0);
}

static void test_math(void)
{
    static const char msg[] = "123456789";
    static const char abc[] = "abc";
    static const u8 md5abc[16] = {
        0x90, 0x01, 0x50, 0x98, 0x3c, 0xd2, 0x4f, 0xb0,
        0xd6, 0x96, 0x3f, 0x7d, 0x28, 0xe1, 0x7f, 0x72,
    };
    static const u8 sha1abc[20] = {
        0xa9, 0x99, 0x3e, 0x36, 0x47, 0x06, 0x81, 0x6a, 0xba, 0x3e,
        0x25, 0x71, 0x78, 0x50, 0xc2, 0x6c, 0x9c, 0xd0, 0xd8, 0x9d,
    };
    MATHCRC16Table t16;
    MATHCRC32Table t32;
    MATHRandContext32 rand;
    u8 digest[20];

    /* Standard check values for "123456789" */
    MATH_CRC32InitTable(&t32);
    CHECK_EQ(MATH_CalcCRC32(&t32, msg, 9), 0xCBF43926u);
    MATH_CRC16InitTable(&t16);
    CHECK_EQ(MATH_CalcCRC16(&t16, msg, 9), 0xBB3D);
    MATH_CRC16CCITTInitTable(&t16);
    CHECK_EQ(MATH_CalcCRC16CCITT(&t16, msg, 9), 0x29B1);

    MATH_CalcMD5(digest, abc, 3);
    CHECK(memcmp(digest, md5abc, 16) == 0);
    MATH_CalcSHA1(digest, abc, 3);
    CHECK(memcmp(digest, sha1abc, 20) == 0);

    /* One million 'a's fed in uneven chunks: exercises block boundaries. */
    {
        static const u8 md5Million[16] = {
            0x77, 0x07, 0xd6, 0xae, 0x4e, 0x02, 0x7c, 0x70,
            0xee, 0xa2, 0xa9, 0x35, 0xc2, 0x29, 0x6f, 0x21,
        };
        static const u8 sha1Million[20] = {
            0x34, 0xaa, 0x97, 0x3c, 0xd4, 0xc4, 0xda, 0xa4, 0xf6, 0x1e,
            0xeb, 0x2b, 0xdb, 0xad, 0x27, 0x31, 0x65, 0x34, 0x01, 0x6f,
        };
        static u8 chunk[100];
        MATHMD5Context md5;
        MATHSHA1Context sha1;
        u32 done = 0, step = 1;

        memset(chunk, 'a', sizeof(chunk));
        MATH_MD5Init(&md5);
        MATH_SHA1Init(&sha1);
        while (done < 1000000) {
            u32 n = step;
            if (n > 1000000 - done) {
                n = 1000000 - done;
            }
            MATH_MD5Update(&md5, chunk, n);
            MATH_SHA1Update(&sha1, chunk, n);
            done += n;
            step = step % 97 + 1;
        }
        MATH_MD5GetHash(&md5, digest);
        CHECK(memcmp(digest, md5Million, 16) == 0);
        MATH_SHA1GetHash(&sha1, digest);
        CHECK(memcmp(digest, sha1Million, 20) == 0);
    }

    /* Count leading zeros: our GCC version must give 32 for zero, like ARM. */
    CHECK_EQ(MATH_CountLeadingZeros(0), 32);
    CHECK_EQ(MATH_CountLeadingZeros(1), 31);
    CHECK_EQ(MATH_CountLeadingZeros(0x80000000u), 0);
    CHECK_EQ(MATH_CountLeadingZerosFunc(0x00010000u), 15);
    CHECK_EQ(MATH_CountPopulation(0xF0F0F0F1u), 17);
    CHECK_EQ(MATH_ILog2(1024), 10);

    /* The game's random numbers must be identical to the DS. */
    MATH_InitRand32(&rand, 12345);
    CHECK_EQ(MATH_Rand32(&rand, 0), 0x62da0dd6u);
    CHECK_EQ(MATH_Rand32(&rand, 0), 0x33271c22u);
    CHECK_EQ(MATH_Rand32(&rand, 100), 28);
}

int main(void)
{
    test_types();
    test_divider_hardware();
    test_sqrt_hardware();
    test_fixed_point();
    test_vectors();
    test_matrix_basic();
    test_matrix_rotation();
    test_memory();
    test_math();

    printf("%d checks, %d failed\n", sChecks, sFailures);
    return sFailures == 0 ? 0 : 1;
}
