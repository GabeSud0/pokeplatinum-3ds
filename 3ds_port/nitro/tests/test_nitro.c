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
#include <nitro_port.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

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


/* ------------------------------------------------------------------------ */
/* OS: interrupts, the frame hook, lock IDs, arenas                         */

static int sVBlankCount, sTimerCount, sFrameCount;
static int sOrder[4], sOrderLen;

static void onVBlank(void)
{
    sVBlankCount++;
    sOrder[sOrderLen++ & 3] = 0;
    OS_SetIrqCheckFlag(OS_IE_V_BLANK);
}

static void onTimer0(void)
{
    sTimerCount++;
    sOrder[sOrderLen++ & 3] = 3;
    OS_SetIrqCheckFlag(OS_IE_TIMER0);
}

/* The tests own the frame hook (the port's default is empty and weak). */
void NitroPort_OnFrame(void)
{
    sFrameCount++;
}

static void test_interrupts(void)
{
    OSIntrMode old;

    /* The CPU starts in system mode with interrupts enabled. */
    CHECK_EQ(OS_GetProcMode(), HW_PSR_SYS_MODE);
    old = OS_DisableInterrupts();
    CHECK_EQ(old, 0);
    CHECK_EQ(OS_GetCpsrIrq(), HW_PSR_IRQ_DISABLE);
    CHECK_EQ(OS_DisableInterrupts(), HW_PSR_IRQ_DISABLE);
    (void)OS_RestoreInterrupts(old);
    CHECK_EQ(OS_GetCpsrIrq(), 0);

    OS_SetIrqFunction(OS_IE_V_BLANK, onVBlank);
    OS_SetIrqFunction(OS_IE_TIMER0, onTimer0);
    (void)OS_SetIrqMask(OS_IE_V_BLANK | OS_IE_TIMER0);
    (void)OS_EnableIrq();

    /* An enabled interrupt runs its handler and is acknowledged. */
    sVBlankCount = 0;
    NitroPort_RaiseIrq(OS_IE_V_BLANK);
    CHECK_EQ(sVBlankCount, 1);
    CHECK_EQ(reg_OS_IF & OS_IE_V_BLANK, 0);

    /* With the CPU's interrupts off it stays pending until they're back on. */
    old = OS_DisableInterrupts();
    NitroPort_RaiseIrq(OS_IE_V_BLANK);
    CHECK_EQ(sVBlankCount, 1);
    CHECK(reg_OS_IF & OS_IE_V_BLANK);
    (void)OS_RestoreInterrupts(old);
    CHECK_EQ(sVBlankCount, 2);   /* taken as soon as they're back on */
    NitroPort_RaiseIrq(0);
    CHECK_EQ(sVBlankCount, 2);

    /* IME off: nothing runs. */
    (void)OS_DisableIrq();
    NitroPort_RaiseIrq(OS_IE_V_BLANK);
    CHECK_EQ(sVBlankCount, 2);
    (void)OS_EnableIrq();
    NitroPort_RaiseIrq(0);
    CHECK_EQ(sVBlankCount, 3);

    /* A masked interrupt (not in IE) never runs. */
    (void)OS_SetIrqMask(OS_IE_V_BLANK);
    sTimerCount = 0;
    NitroPort_RaiseIrq(OS_IE_TIMER0);
    CHECK_EQ(sTimerCount, 0);
    reg_OS_IF = 0;

    /* Two at once: lowest bit (V-blank) first, like the DS. */
    (void)OS_SetIrqMask(OS_IE_V_BLANK | OS_IE_TIMER0);
    sOrderLen = 0;
    old = OS_DisableInterrupts();
    NitroPort_RaiseIrq(OS_IE_TIMER0 | OS_IE_V_BLANK);
    (void)OS_RestoreInterrupts(old);
    NitroPort_RaiseIrq(0);
    CHECK_EQ(sOrderLen, 2);
    CHECK(sOrder[0] == 0 && sOrder[1] == 3);

    /* Waiting for V-blank: OS_Halt advances one frame and raises it. */
    sFrameCount = 0;
    sVBlankCount = 0;
    OS_WaitInterrupt(TRUE, OS_IE_V_BLANK);
    CHECK_EQ(sFrameCount, 1);
    CHECK_EQ(sVBlankCount, 1);
    OS_WaitInterrupt(TRUE, OS_IE_V_BLANK);
    CHECK_EQ(sFrameCount, 2);

    /* Long work (card reads) within one frame: a V-blank for every frame's
     * worth of DS time, without a new frame; the count restarts each frame. */
    sFrameCount = 0;
    sVBlankCount = 0;
    NitroPort_SpendDsTime(NITRO_PORT_CYCLES_PER_FRAME - 1);
    CHECK_EQ(sVBlankCount, 0);
    NitroPort_SpendDsTime(1);
    CHECK_EQ(sVBlankCount, 1);
    NitroPort_SpendDsTime(2 * NITRO_PORT_CYCLES_PER_FRAME + 5);
    CHECK_EQ(sVBlankCount, 3);
    CHECK_EQ(sFrameCount, 0);
    OS_WaitInterrupt(TRUE, OS_IE_V_BLANK);   /* a new frame: count restarts */
    CHECK_EQ(sFrameCount, 1);
    CHECK_EQ(sVBlankCount, 4);
    NitroPort_SpendDsTime(NITRO_PORT_CYCLES_PER_FRAME - 1);
    CHECK_EQ(sVBlankCount, 4);
    /* With interrupts off it waits, like any interrupt. */
    old = OS_DisableInterrupts();
    NitroPort_SpendDsTime(NITRO_PORT_CYCLES_PER_FRAME);
    CHECK_EQ(sVBlankCount, 4);
    (void)OS_RestoreInterrupts(old);
    CHECK_EQ(sVBlankCount, 5);
}

static int sDmaDone;

static void onDmaDone(void *arg)
{
    sDmaDone++;
    *(int *)arg = 1;
}

static void test_dma(void)
{
    /* DMA is done in software (src/port_dma.c). Expected values are the
     * plain C equivalent of each transfer. */
    static u32 src[64], dst[66];
    static u16 src16[8], dst16[10];
    static u8 pltt[32];
    static vu32 port;
    int i, flag;
    OSIntrMode old;

    for (i = 0; i < 64; i++) {
        src[i] = 0x01010101u * (u32)i ^ 0xA5000000u;
    }

    /* Immediate copy and fill, 32-bit: exactly the requested words. */
    memset(dst, 0, sizeof(dst));
    MI_DmaCopy32(3, src, dst, 64 * 4);
    CHECK(memcmp(dst, src, 64 * 4) == 0);
    CHECK_EQ(dst[64], 0);
    CHECK(!MI_IsDmaBusy(3));
    MI_DmaFill32(3, dst + 1, 0xDEADBEEF, 8 * 4);
    CHECK_EQ(dst[0], src[0]);
    CHECK_EQ(dst[1], 0xDEADBEEF);
    CHECK_EQ(dst[8], 0xDEADBEEF);
    CHECK_EQ(dst[9], src[9]);

    /* 16-bit, an odd number of halfwords. */
    for (i = 0; i < 8; i++) {
        src16[i] = (u16)(0x1111 * i + 1);
    }
    memset(dst16, 0, sizeof(dst16));
    MI_DmaCopy16(3, src16, dst16, 5 * 2);
    CHECK(memcmp(dst16, src16, 5 * 2) == 0);
    CHECK_EQ(dst16[5], 0);
    MI_DmaFill16(3, dst16 + 6, 0xBEEF, 3 * 2);
    CHECK_EQ(dst16[5], 0);
    CHECK(dst16[6] == 0xBEEF && dst16[7] == 0xBEEF && dst16[8] == 0xBEEF);
    CHECK_EQ(dst16[9], 0);

    /* Sending to a fixed address (an I/O port): every word goes to the
     * same place, so the last one remains. */
    port = 0;
    MI_DmaSend32(3, src, (volatile void *)&port, 16 * 4);
    CHECK_EQ(port, src[15]);

    /* Channel 0 after a wait and after a stop. On the DS the SDK leaves a
     * busy "dummy" transfer there; the port must not, or the next wait on
     * channel 0 never ends. This is the game's own sequence
     * (BufferManager_StopDMA). */
    MI_DmaCopy32(0, src, dst, 16);
    MI_WaitDma(0);
    CHECK(!MI_IsDmaBusy(0));
    MI_StopDma(0);
    MI_WaitDma(0);
    CHECK(!MI_IsDmaBusy(0));

    /* Channel 0 from 0x08000000 and up. On the DS that's the GBA slot and
     * the SDK refuses it; on the 3DS it's where the application heap
     * starts, so the port must allow it. (A page mapped there on the PC.) */
    {
#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif
        void *page = mmap((void *)0x08000000, 4096, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
        if (page == (void *)0x08000000) {
            memcpy(page, src, 64);
            memset(dst, 0, sizeof(dst));
            MI_DmaCopy32(0, page, dst, 64);
            CHECK(memcmp(dst, src, 64) == 0);
            munmap(page, 4096);
        } else {
            printf("  (skipped DMA from 0x08000000: address not free)\n");
        }
    }

    /* Asynchronous copy: the completion callback runs through the DMA
     * interrupt, exactly once. */
    (void)OS_EnableIrq();
    memset(dst, 0, sizeof(dst));
    flag = 0;
    sDmaDone = 0;
    MI_DmaCopy32Async(1, src, dst, 32 * 4, onDmaDone, &flag);
    CHECK_EQ(flag, 1);
    CHECK_EQ(sDmaDone, 1);
    CHECK(memcmp(dst, src, 32 * 4) == 0);
    CHECK_EQ(reg_OS_IF & OS_IE_DMA1, 0);

    /* Started with the CPU's interrupts off: the callback waits, and runs
     * the moment they're back on, as on the DS. */
    flag = 0;
    old = OS_DisableInterrupts();
    MI_DmaFill32Async(2, dst, 0, 4 * 4, onDmaDone, &flag);
    CHECK_EQ(flag, 0);
    (void)OS_RestoreInterrupts(old);
    CHECK_EQ(flag, 1);
    CHECK_EQ(sDmaDone, 2);

    /* V-blank timing: nothing moves until V-blank, then the copy is done
     * and the channel is free. */
    memset(dst, 0, sizeof(dst));
    flag = 0;
    MI_VBlankDmaCopy32Async(1, src, dst, 8 * 4, onDmaDone, &flag);
    CHECK_EQ(dst[0], 0);
    CHECK(MI_IsDmaBusy(1));
    OS_WaitInterrupt(TRUE, OS_IE_V_BLANK);
    CHECK(memcmp(dst, src, 8 * 4) == 0);
    CHECK_EQ(flag, 1);
    CHECK(!MI_IsDmaBusy(1));

    /* H-blank timing: armed until the port goes through a frame's lines
     * (NitroPort_HBlankLines), then a unit at each H-blank of the 192
     * lines drawn, into the same register (dest reload): each line has
     * what the H-blank before it wrote (a wavy screen); line 0 what the
     * register had. MI_StopDma releases the channel. */
    {
        static u32 lineSrc[192];
        int y, wrong = 0;

        for (y = 0; y < 192; y++) {
            lineSrc[y] = (u32)(y * 3 + 1);
        }
        NitroPort_LineRegs.active = 0;
        *(vu32 *)REG_BG0HOFS_ADDR = 7;
        MI_HBlankDmaCopy32(0, lineSrc, (void *)REG_BG0HOFS_ADDR, 4);
        CHECK(!NitroPort_WasReported("DMA timing: H-blank"));
        CHECK(MI_IsDmaBusy(0));
        CHECK_EQ(*(vu32 *)REG_BG0HOFS_ADDR, 7);   /* (nothing till the lines) */
        NitroPort_HBlankLines();
        CHECK(NitroPort_LineRegs.active);
        CHECK_EQ(NitroPort_LineRegs.regs[0][0][0x10], 7);
        for (y = 1; y < 192; y++) {
            u32 v = (u32)NitroPort_LineRegs.regs[0][y][0x10] | ((u32)NitroPort_LineRegs.regs[0][y][0x11] << 8);

            wrong += v != ((lineSrc[y - 1]) & 0xFFFFu);
        }
        CHECK_EQ(wrong, 0);
        CHECK_EQ(*(vu32 *)REG_BG0HOFS_ADDR, lineSrc[191]);   /* (the last line's H-blank: its unit too) */
        CHECK(MI_IsDmaBusy(0));   /* (it repeats) */
        MI_StopDma(0);
        MI_WaitDma(0);
        CHECK(!MI_IsDmaBusy(0));
        NitroPort_HBlankLines();   /* (nothing armed: no lines) */
        CHECK(!NitroPort_LineRegs.active);
        *(vu32 *)REG_BG0HOFS_ADDR = 0;
    }

    /* The game's graphics loads go through DMA channel 3 by default. */
    for (i = 0; i < 32; i++) {
        pltt[i] = (u8)(i * 7 + 3);
    }
    GX_LoadBGPltt(pltt, 32, sizeof(pltt));
    CHECK(memcmp((const void *)(HW_BG_PLTT + 32), pltt, sizeof(pltt)) == 0);

    /* G3X_Init clears the 3D engine's tables by DMA; it used to hang. */
    G3X_Init();
    CHECK_EQ(*(vu32 *)REG_FOG_TABLE_0_ADDR, 0);
    CHECK(!MI_IsDmaBusy(3));
}

/* The H-blank interrupt, line by line (NitroPort_HBlankLines): its
 * handler runs before each line drawn (the V-count the line before's; for
 * line 0 the frame's last, 262), what it writes is that line's; without
 * the interrupt asked for (DISPSTAT), nothing. */
static int sHBlankCalls, sHBlankFlagged;
static void onHBlank(void)
{
    sHBlankCalls++;
    sHBlankFlagged += GX_IsHBlank() != 0;   /* (some of the game's handlers do nothing without it) */
    *(vu16 *)REG_BG1HOFS_ADDR = (u16)(GX_GetVCount() & 0x1FF);
    *(vu16 *)(REG_BG1HOFS_ADDR + 0x1000) = (u16)((GX_GetVCount() + 1) & 0x1FF);   /* (engine B's) */
    OS_SetIrqCheckFlag(OS_IE_H_BLANK);
}

static void onHBlankQuiet(void)
{
    OS_SetIrqCheckFlag(OS_IE_H_BLANK);
}

static void test_hblank_lines(void)
{
    int y, wrong = 0;
    OSIrqMask saved;

    sHBlankCalls = 0;
    sHBlankFlagged = 0;
    OS_SetIrqFunction(OS_IE_H_BLANK, onHBlank);
    saved = OS_EnableIrqMask(OS_IE_H_BLANK);
    (void)OS_EnableIrq();
    NitroPort_HBlankLines();
    CHECK_EQ(sHBlankCalls, 0);   /* (DISPSTAT doesn't ask for it) */
    CHECK(!NitroPort_LineRegs.active);
    GX_HBlankIntr(TRUE);
    NitroPort_HBlankLines();
    CHECK_EQ(sHBlankCalls, 193);
    CHECK_EQ(sHBlankFlagged, 193);
    CHECK(!GX_IsHBlank());   /* (not after them) */
    CHECK(NitroPort_LineRegs.active);
    CHECK_EQ(NitroPort_LineRegs.regs[0][0][0x14] | (NitroPort_LineRegs.regs[0][0][0x15] << 8), 262);
    for (y = 1; y < 192; y++) {
        wrong += (NitroPort_LineRegs.regs[0][y][0x14] | (NitroPort_LineRegs.regs[0][y][0x15] << 8)) != y - 1;
        wrong += (NitroPort_LineRegs.regs[1][y][0x14] | (NitroPort_LineRegs.regs[1][y][0x15] << 8)) != y;
    }
    CHECK_EQ(wrong, 0);
    CHECK_EQ(reg_OS_IF & OS_IE_H_BLANK, 0);   /* (none left pending) */
    /* a handler that changes nothing drawn: no line differs (the V-count
     * and DISPSTAT aren't kept) */
    OS_SetIrqFunction(OS_IE_H_BLANK, onHBlankQuiet);
    NitroPort_HBlankLines();
    CHECK(!NitroPort_LineRegs.active);
    OS_SetIrqFunction(OS_IE_H_BLANK, onHBlank);
    /* once a V-blank: the first time, then not again before the next */
    sHBlankCalls = 0;
    CHECK(NitroPort_HBlankLinesOnce());
    CHECK_EQ(sHBlankCalls, 193);
    CHECK(!NitroPort_HBlankLinesDue());
    CHECK(!NitroPort_HBlankLinesOnce());
    CHECK_EQ(sHBlankCalls, 193);
    CHECK(NitroPort_LineRegs.active);   /* (the lines kept) */
    /* and again after the next V-blank */
    NitroPort_SpendDsTime(NITRO_PORT_CYCLES_PER_FRAME);
    CHECK(NitroPort_HBlankLinesDue());
    CHECK(NitroPort_HBlankLinesOnce());
    CHECK_EQ(sHBlankCalls, 2 * 193);
    GX_HBlankIntr(FALSE);
    (void)OS_SetIrqMask(saved);
    OS_SetIrqFunction(OS_IE_H_BLANK, NULL);
    *(vu16 *)REG_BG1HOFS_ADDR = 0;
    *(vu16 *)(REG_BG1HOFS_ADDR + 0x1000) = 0;
    NitroPort_SpendDsTime(NITRO_PORT_CYCLES_PER_FRAME);
    CHECK(!NitroPort_HBlankLinesDue());   /* (nothing armed) */
    NitroPort_HBlankLines();
    CHECK(!NitroPort_LineRegs.active);
}

static void test_lock_ids(void)
{
    vu32 *flags = (vu32 *)HW_LOCK_ID_FLAG_MAIN;
    s32 a, b, c;

    flags[0] = 0xFFFFFFFF; /* all IDs free (OS_InitLock does this on the DS) */
    flags[1] = 0xFFFFFFFF;

    a = OS_GetLockID();
    b = OS_GetLockID();
    CHECK_EQ(a, OS_MAINP_LOCK_ID_START);
    CHECK_EQ(b, OS_MAINP_LOCK_ID_START + 1);
    OS_ReleaseLockID((u16)a);
    c = OS_GetLockID();
    CHECK_EQ(c, a);

    /* Run out of the first word: IDs continue in the second. */
    flags[0] = 0;
    flags[1] = 0x80000000;
    CHECK_EQ(OS_GetLockID(), OS_MAINP_LOCK_ID_START + 32);
    CHECK_EQ(OS_GetLockID(), OS_LOCK_ID_ERROR);
    OS_ReleaseLockID(OS_MAINP_LOCK_ID_START + 33);
    CHECK_EQ(flags[1], 0x40000000);
}

static void test_arena(void)
{
    void *lo, *p;

    OS_InitArena();
    lo = OS_GetMainArenaLo();
    CHECK(lo == (void *)HW_MAIN_MEM);
    CHECK(OS_GetMainArenaHi() == (void *)HW_MAIN_MEM_MAIN_END);
    p = OS_AllocFromMainArenaLo(100, 32);
    CHECK(p == lo);
    CHECK((u8 *)OS_GetMainArenaLo() >= (u8 *)lo + 100);
}

static void test_memory_map(void)
{
    /* The texture loader stores LCDC VRAM addresses as 4 KB page numbers
     * in 16 bits, so LCDC VRAM must be page aligned and below 256 MB. */
    CHECK_EQ(HW_LCDC_VRAM & 0xFFF, 0);
    CHECK(HW_LCDC_VRAM < 0x10000000u);
    /* The shared area at the top of main memory is inside our block. */
    CHECK(HW_RESET_PARAMETER_BUF > HW_MAIN_MEM && HW_RESET_PARAMETER_BUF < HW_MAIN_MEM + 0x800000);
    CHECK_EQ(OS_GetResetParameter(), 0); /* a clean boot */
}

/* ------------------------------------------------------------------------ */
/* Decompression, checked against Nintendo's own compressors (mi_compress.c) */

static void test_decompression(void)
{
    static u8 src[3000], packed[8000], out[3100], work[MI_HUFFMAN_COMPRESS_WORK_SIZE];
    static const u8 rl[] = {
        0x30, 0x09, 0x00, 0x00,     /* RL, 9 bytes */
        0x82, 0xAA,                 /* run: 5 x 0xAA */
        0x03, 1, 2, 3, 4,           /* 4 literals */
    };
    static const u8 diff8[] = { 0x81, 4, 0, 0, 10, 1, 1, 0xFF };
    static const u8 bits[] = { 0xA5 }; /* 1010 0101 */
    u32 n, i;
    int ok;

    /* Text-like data with repeats, plus a long run (exercises long copies). */
    for (i = 0; i < sizeof(src); i++) {
        src[i] = (u8)("the quick brown fox jumps over the lazy dog. "[i % 45] + (i / 900));
    }
    memset(src + 1000, 'z', 600);

    n = MI_CompressLZ(src, sizeof(src), packed);
    CHECK(n > 0 && n < sizeof(src));
    memset(out, 0, sizeof(out));
    MI_UncompressLZ8(packed, out);
    CHECK(memcmp(out, src, sizeof(src)) == 0);
    CHECK_EQ(MI_GetUncompressedSize(packed), sizeof(src));

    n = MI_CompressLZEx(src, sizeof(src), packed); /* extended format: long copies */
    CHECK(n > 0);
    memset(out, 0, sizeof(out));
    MI_UncompressLZ16(packed, out);
    CHECK(memcmp(out, src, sizeof(src)) == 0);

    n = MI_CompressRL(src, sizeof(src), packed);
    CHECK(n > 0);
    memset(out, 0, sizeof(out));
    MI_UncompressRL8(packed, out);
    CHECK(memcmp(out, src, sizeof(src)) == 0);

    for (i = 4; i <= 8; i += 4) {
        n = MI_CompressHuffman(src, sizeof(src), packed, (u8)i, work);
        CHECK(n > 0);
        memset(out, 0, sizeof(out));
        MI_UncompressHuffman(packed, out);
        ok = memcmp(out, src, sizeof(src)) == 0;
        CHECK(ok);
        if (!ok) {
            printf("  (Huffman with %u-bit symbols)\n", (unsigned)i);
        }
    }

    memset(out, 0, sizeof(out));
    MI_UncompressRL8(rl, out);
    CHECK(out[0] == 0xAA && out[4] == 0xAA && out[5] == 1 && out[8] == 4 && out[9] == 0);

    MI_UnfilterDiff8(diff8, out);
    CHECK(out[0] == 10 && out[1] == 11 && out[2] == 12 && out[3] == 11);

    {
        /* 1 bit -> 4 bits, +1 on non-zero values: 1,0,1,0,0,1,0,1 (low bit first) */
        MIUnpackBitsParam param = { 1, 1, 4, 1, 0 };
        u32 word = 0;
        MI_UnpackBits(bits, &word, &param);
        CHECK_EQ(word, 0x20200202u);
    }
}

/* ------------------------------------------------------------------------ */
/* BIOS calls, coprocessor context, HMAC, the virtual ARM7, GBA slot       */

static void test_bios(void)
{
    static u16 h[8];
    static u32 w[16];
    static const u16 h5 = 0x1234;
    static const u32 w7 = 0xDEADBEEF;
    int i;

    CHECK_EQ(SVC_GetCRC16(0xFFFF, "123456789", 9), 0x4B37); /* CRC-16/MODBUS check value */
    CHECK_EQ(SVC_Sqrt(1000000), 1000);
    CHECK_EQ(SVC_Sqrt(0xFFFFFFFFu), 0xFFFF);
    CHECK_EQ(SVC_Div(-7, 2), -3);
    CHECK_EQ(SVC_DivRem(-7, 2), -1);

    SVC_CpuSet(&h5, h, (1u << 24) | 8);          /* fill 8 halfwords */
    for (i = 0; i < 8; i++) {
        CHECK_EQ(h[i], 0x1234);
    }
    SVC_CpuSetFast(&w7, w, (1u << 24) | 3);      /* rounds up to 8 words */
    CHECK(w[0] == w7 && w[7] == w7 && w[8] == 0);
    SVC_CpuSet(w, w + 8, (1u << 26) | 4);        /* copy 4 words */
    CHECK(w[8] == w7 && w[11] == w7 && w[12] == 0);
}

static void test_cp_context(void)
{
    CPContext saved;

    CP_SetDiv32_32(100, 7);
    CP_SetSqrt32(81);
    CP_SaveContext(&saved);

    CP_SetDiv64_64(1, 1);
    CP_SetSqrt32(4);
    CP_RestoreContext(&saved);

    CHECK_EQ(CP_GetDivResult32(), 14);
    CHECK_EQ(CP_GetDivRemainder32(), 2);
    CHECK_EQ(CP_GetSqrtResult32(), 9);
}

static void test_hmac(void)
{
    /* RFC 2202, test case 1 */
    static const u8 sha1[20] = {
        0xb6, 0x17, 0x31, 0x86, 0x55, 0x05, 0x72, 0x64, 0xe2, 0x8b,
        0xc0, 0xb6, 0xfb, 0x37, 0x8c, 0x8e, 0xf1, 0x46, 0xbe, 0x00,
    };
    static const u8 md5[16] = {
        0x92, 0x94, 0x72, 0x7a, 0x36, 0x38, 0xbb, 0x1c,
        0x13, 0xf4, 0x8e, 0xf8, 0x15, 0x8b, 0xfc, 0x9d,
    };
    static char data[] = "Hi There";
    u8 key[20], digest[20];

    memset(key, 0x0b, sizeof(key));
    DGT_Hash2CalcHmac(digest, data, 8, key, 20);
    CHECK(memcmp(digest, sha1, 20) == 0);
    DGT_Hash1CalcHmac(digest, data, 8, key, 16);
    CHECK(memcmp(digest, md5, 16) == 0);
}

static u32 sRtcReply;
static int sRtcReplies, sArm7Received;

static void rtcCallback(PXIFifoTag tag, u32 data, BOOL err)
{
    (void)tag;
    (void)err;
    sRtcReply = data;
    sRtcReplies++;
}

static void fakeArm7Rtc(int tag, u32 data, BOOL err)
{
    (void)err;
    sArm7Received++;
    NitroPort_Arm7Reply(tag, data + 1, FALSE); /* answer with data + 1 */
}

static void test_pxi(void)
{
    PXI_Init();

    /* The virtual ARM7 reports every service as ready. */
    CHECK(PXI_IsCallbackReady(PXI_FIFO_TAG_RTC, PXI_PROC_ARM7));

    /* No handler yet: the message is accepted and reported once. */
    CHECK_EQ(PXI_SendWordByFifo(PXI_FIFO_TAG_MIC, 5, FALSE), PXI_FIFO_SUCCESS);
    CHECK(NitroPort_WasReported("ARM7 service: microphone"));

    /* With a handler, the reply reaches the game-side callback. */
    PXI_SetFifoRecvCallback(PXI_FIFO_TAG_RTC, rtcCallback);
    CHECK(PXI_IsCallbackReady(PXI_FIFO_TAG_RTC, PXI_PROC_ARM9));
    NitroPort_SetArm7Handler(PXI_FIFO_TAG_RTC, fakeArm7Rtc);
    CHECK_EQ(PXI_SendWordByFifo(PXI_FIFO_TAG_RTC, 41, FALSE), PXI_FIFO_SUCCESS);
    CHECK_EQ(sArm7Received, 1);
    CHECK_EQ(sRtcReplies, 1);
    CHECK_EQ(sRtcReply, 42);
    NitroPort_SetArm7Handler(PXI_FIFO_TAG_RTC, NULL);
}

static void test_gba_slot(void)
{
    CTRDG_Init();
    CHECK(!CTRDG_IsExisting());
    CHECK(!CTRDG_IsAgbCartridge());
    CHECK(!CTRDG_IsPulledOut());
}

static void test_printf(void)
{
    /* The SDK's two printf implementations. %hh and %h arguments arrive as
     * int and are cut down to char/short, like C's own printf; reading them
     * with va_arg(ap, char) would crash with GCC. Expected strings are what
     * the C standard's printf produces for the same arguments. */
    typedef int (*SPrintf)(char *, size_t, const char *, ...);
    static const SPrintf impls[] = { STD_TSNPrintf, OS_SNPrintf };
    char buf[64];
    int i;

    for (i = 0; i < 2; i++) {
        SPrintf f = impls[i];
        f(buf, sizeof(buf), "%d %s %x", 42, "ab", 255);
        CHECK(strcmp(buf, "42 ab ff") == 0);
        f(buf, sizeof(buf), "%hhd %hhd", -5, 0x1FF);
        CHECK(strcmp(buf, "-5 -1") == 0);
        f(buf, sizeof(buf), "%hhu %hhx", 0x1FF, 0x1AB);
        CHECK(strcmp(buf, "255 ab") == 0);
        f(buf, sizeof(buf), "%hd %hu", -1234, 0x12345);
        CHECK(strcmp(buf, "-1234 9029") == 0);
    }

    /* char is signed, as with Nintendo's compiler (GCC on ARM defaults to
     * unsigned; common.mk passes -fsigned-char). */
    CHECK((char)0x80 < 0);
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
    test_interrupts();
    test_dma();
    test_hblank_lines();
    test_lock_ids();
    test_arena();
    test_memory_map();
    test_decompression();
    test_bios();
    test_cp_context();
    test_hmac();
    test_pxi();
    test_gba_slot();
    test_printf();

    printf("%d checks, %d failed\n", sChecks, sFailures);
    return sFailures == 0 ? 0 : 1;
}
