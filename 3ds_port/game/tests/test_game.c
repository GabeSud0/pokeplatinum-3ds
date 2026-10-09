/*
 * Small host smoke tests of unmodified pokeplatinum C source code.
 * This uses neither ROM content nor generated game data. A larger game
 * compilation target will grow from this separately tested foundation.
 */
#include <nitro.h>
#include <nnsys.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ascii_util.h"
#include "charcode.h"
#include "charcode_util.h"
#include "coins.h"
#include "constants/charcode.h"
#include "constants/versions.h"
#include "global/pm_version.h"

static int checks;
static int fails;

#define CHECK(expr) do { \
    checks++; \
    if (!(expr)) { \
        fails++; \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
    } \
} while (0)

/* Keep the original game's GF_ASSERT enabled, but fail loudly in tests
 * instead of calling the game's not-yet-compiled error handling subsystem. */
void ErrorHandling_AssertFail(void)
{
    fputs("Game GF_ASSERT failed in host test\n", stderr);
    abort();
}

static void test_coins(void)
{
    u16 coins = 123;

    Coins_Init(&coins);
    CHECK(Coins_GetValue(&coins) == 0);
    CHECK(Coins_Add(&coins, 170));
    CHECK(Coins_GetValue(&coins) == 170);
    CHECK(Coins_CanAdd(&coins, 1000));
    CHECK(Coins_Subtract(&coins, 20));
    CHECK(Coins_GetValue(&coins) == 150);
    CHECK(!Coins_Subtract(&coins, 151));
    CHECK(Coins_GetValue(&coins) == 150);
    Coins_SetValue(&coins, MAX_COINS);
    CHECK(Coins_GetValue(&coins) == MAX_COINS);
    CHECK(!Coins_Add(&coins, 1));
    CHECK(!Coins_CanAdd(&coins, 1));
    CHECK(Coins_Subtract(&coins, MAX_COINS));
    CHECK(Coins_GetValue(&coins) == 0);
    CHECK(Coins_Add(&coins, MAX_COINS + 1));
    CHECK(Coins_GetValue(&coins) == MAX_COINS);
}

static void test_ascii(void)
{
    char src[] = "alpha\r\nbeta";
    char dst[32] = {0};
    char *next = Ascii_CopyToTerminator(src, dst, '\r');
    NNSG3dResName name;

    CHECK(Ascii_Length("Hello") == 5);
    CHECK(Ascii_Length("") == 0);
    CHECK(Ascii_ConvertToInt("1234") == 1234);
    CHECK(Ascii_ConvertToInt("-17") == -17);
    CHECK(Ascii_ConvertToInt("12x") == -1);
    CHECK(strcmp(dst, "alpha") == 0);
    CHECK(next != NULL && strcmp(next, "beta") == 0);

    memset(&name, 0xFF, sizeof(name));
    Ascii_SetResourceName(&name, "Hero");
    CHECK(memcmp(name.name, "Hero", 4) == 0);
    CHECK(name.val[4] == 0);
}

static void test_charcode(void)
{
    const charcode_t format[] = {CHAR_FORMAT_ARG, 0x0100, 2, 40, 50, CHAR_EOS};
    charcode_t buf[] = {CHAR_WIDE_A, CHAR_WIDE_B, CHAR_EOS};
    charcode_t out[16] = {0};
    charcode_t encoded[16] = {0};

    CHECK(CharCode_FormatArgType(format) == 0x100);
    CHECK(CharCode_FormatArgParam(format, 0) == 40);
    CHECK(CharCode_FormatArgParam(format, 1) == 50);
    CHECK(CharCode_IsFormatArg(format));
    CHECK(CharCode_SkipFormatArg(format) == format + 5);
    CHECK(CharCode_Length(buf) == 2);
    CHECK(CharCode_Compare(buf, buf) == 0);
    CHECK(CharCode_Copy(out, buf) == out + 2);
    CHECK(CharCode_Compare(out, buf) == 0);
    CHECK(CharCode_CompareNumChars(out, buf, 2) == 0);
    CharCode_FromInt(encoded, 123, PADDING_MODE_ZEROES, 3);
    CHECK(encoded[0] == CHAR_WIDE_1);
    CHECK(encoded[1] == CHAR_WIDE_2);
    CHECK(encoded[2] == CHAR_WIDE_3);
    CHECK(encoded[3] == CHAR_EOS);
}

static void test_sdk_link(void)
{
    /* Require a real external NitroSystem function at link time. */
    NNSGfdTexKey texture;
    NNS_GfdInitFrmTexVramManager(1, TRUE);
    texture = NNS_GfdAllocTexVram(0x1000, FALSE, 0);
    CHECK(texture != NNS_GFD_ALLOC_ERROR_TEXKEY);
    CHECK(NNS_GfdGetTexKeySize(texture) == 0x1000);
}

int main(void)
{
    printf("Original Platinum game-code smoke tests (%d-bit)\n",
           (int)(sizeof(void *) * 8));
    CHECK(gGameVersion == VERSION_PLATINUM);
    CHECK(gGameLanguage == LANGUAGE_ENGLISH);
    test_coins();
    test_ascii();
    test_charcode();
    test_sdk_link();
    printf("%d checks, %d failed\n", checks, fails);
    return fails == 0 ? 0 : 1;
}
