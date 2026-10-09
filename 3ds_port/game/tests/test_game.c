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
#include "int_distance.h"
#include "play_time.h"
#include "play_time_manager.h"
#include "poffin.h"
#include "game_options.h"
#include "berry_data.h"
#include "poffin_types.h"
#include "generated/abilities.h"
#include "generated/species.h"
#include "generated/move_flags.h"
#include "generated/ai_flags.h"
#include "timer.h"
#include "math_util.h"
#include "fx_util.h"
#include "field_script_context.h"
#include "map_tile_behavior.h"
#include "constants/field/map_tile_behaviors.h"
#include "generated/text_banks.h"
#include "res/pokemon/regional_pokedex_size.h"
#include "res/sound/pl_sound_data.naix"
#include "res/graphics/poketch/poketch.naix"
#include "res/graphics/frontier/backgrounds/frontier_backgrounds.naix"
#include "res/text/bank/box_messages.h"
#include "res/text/bank/ability_names_uppercase.h"
#include "res/text/bank/unk_0353.h"

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
    CHECK(name.name[4] == 0);
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

static void test_play_time(void)
{
    PlayTime time;

    PlayTime_Init(&time);
    CHECK(PlayTime_GetHours(&time) == 0);
    CHECK(PlayTime_GetMinutes(&time) == 0);
    CHECK(PlayTime_GetSeconds(&time) == 0);

    PlayTime_Increment(&time, 59);
    CHECK(PlayTime_GetSeconds(&time) == 59);
    PlayTime_Increment(&time, 1);
    CHECK(PlayTime_GetMinutes(&time) == 1);
    CHECK(PlayTime_GetSeconds(&time) == 0);

    PlayTime_Increment(&time, 59 * 60);
    CHECK(PlayTime_GetHours(&time) == 1);
    CHECK(PlayTime_GetMinutes(&time) == 0);

    /* Platinum caps save-file play time at 999:59:59. */
    PlayTime_Increment(&time, 1000 * 60 * 60);
    CHECK(PlayTime_GetHours(&time) == PLAYTIME_MAX_HOURS);
    CHECK(PlayTime_GetMinutes(&time) == PLAYTIME_MAX_MINUTES);
    CHECK(PlayTime_GetSeconds(&time) == PLAYTIME_MAX_SECONDS);
    PlayTime_Increment(&time, 60);
    CHECK(PlayTime_GetHours(&time) == PLAYTIME_MAX_HOURS);
}

static void test_distance(void)
{
    CHECK(CalcDistance2D(0, 0, 0, 0) == 0);
    CHECK(CalcDistance2D(0, 0, 3, 4) == 5);
    CHECK(CalcDistance2D(3, 4, 0, 0) == 5);
    CHECK(CalcDistance2D(1, 2, 1, 2) == 0);
}

static void test_random_and_math(void)
{
    const u8 bytes[] = {1, 3, 5, 7, 9};
    u16 encoded[] = {0x0123, 0x4567, 0x89ab, 0xcdef};
    const u16 original[] = {0x0123, 0x4567, 0x89ab, 0xcdef};
    u32 seed = 0x12345678U;

    CHECK(SumBytes(bytes, sizeof(bytes)) == 25);
    CHECK(FX_Min(3 * FX32_ONE, -2 * FX32_ONE) == -2 * FX32_ONE);
    CHECK(FX_Max(3 * FX32_ONE, -2 * FX32_ONE) == 3 * FX32_ONE);

    LCRNG_SetSeed(seed);
    seed = seed * (u32)LCRNG_MULTIPLIER + (u32)LCRNG_INCREMENT;
    CHECK(LCRNG_Next() == (u16)(seed >> 16));
    CHECK(LCRNG_GetSeed() == seed);
    seed = seed * (u32)LCRNG_MULTIPLIER + (u32)LCRNG_INCREMENT;
    CHECK(LCRNG_Next() == (u16)(seed >> 16));
    CHECK(LCRNG_GetSeed() == seed);

    /* Reference MT19937 outputs for the published seed 5489. */
    MTRNG_SetSeed(5489U);
    CHECK(MTRNG_Next() == 3499211612U);
    CHECK(MTRNG_Next() == 581869302U);
    CHECK(MTRNG_Next() == 3890346734U);

    EncodeData(encoded, sizeof(encoded), 12345U);
    CHECK(memcmp(encoded, original, sizeof(encoded)) != 0);
    DecodeData(encoded, sizeof(encoded), 12345U);
    CHECK(memcmp(encoded, original, sizeof(encoded)) == 0);
}

static void test_tile_behavior(void)
{
    CHECK(TileBehavior_IsTallGrass(TILE_BEHAVIOR_TALL_GRASS));
    CHECK(!TileBehavior_IsTallGrass(TILE_BEHAVIOR_NONE));
    CHECK(TileBehavior_HasEncounters(TILE_BEHAVIOR_TALL_GRASS));
    CHECK(!TileBehavior_HasEncounters(TILE_BEHAVIOR_NONE));
    CHECK(TileBehavior_IsDoor(TILE_BEHAVIOR_DOOR));
    CHECK(TileBehavior_IsSurfable(TILE_BEHAVIOR_WATER_RIVER));
    CHECK(!TileBehavior_IsSurfable(TILE_BEHAVIOR_SAND));
    CHECK(TileBehavior_IsJumpNorth(TILE_BEHAVIOR_JUMP_NORTH));
    CHECK(TileBehavior_BlocksMovementNorthward(TILE_BEHAVIOR_BLOCK_NORTH_AND_WEST));
    CHECK(!TileBehavior_BlocksMovementSouthward(TILE_BEHAVIOR_BLOCK_NORTH_AND_WEST));
    CHECK(TileBehavior_HasReflectiveSurface(TILE_BEHAVIOR_PUDDLE));
    CHECK(!TileBehavior_HasReflectiveSurface(TILE_BEHAVIOR_NONE));
    CHECK(TileBehavior_IsIce(TILE_BEHAVIOR_ICE));
}

static BOOL test_script_capture(ScriptContext *ctx)
{
    ctx->data[0] = ScriptContext_ReadWord(ctx);
    return TRUE;
}

static BOOL test_script_stop(ScriptContext *ctx)
{
    ScriptContext_Stop(ctx);
    return TRUE;
}

static BOOL test_script_resume(ScriptContext *ctx)
{
    (void)ctx;
    return TRUE;
}

static void test_field_script(void)
{
    const ScrCmdFunc commands[] = {test_script_capture, test_script_stop};
    const u8 bytecode[] = {0, 0, 0x78, 0x56, 0x34, 0x12, 1, 0};
    const u8 raw[] = {0x34, 0x12, 0xef, 0xcd, 0xab, 0x90};
    ScriptContext ctx;

    memset(&ctx, 0xcd, sizeof(ctx));
    ScriptContext_Init(&ctx, commands, 2);
    CHECK(TEXT_BANK_MOVES_USED_IN_BATTLE == 0);
    CHECK(TEXT_BANK_DIPLOMA == 1);
    CHECK(TEXT_BANK_SPECIES_CATEGORY_JP == 723);
    CHECK(ctx.scriptPtr == NULL);
    CHECK(ctx.stackPointer == 0);
    CHECK(ctx.data[0] == 0);
    CHECK(ScriptContext_Start(&ctx, bytecode));

    /* A waiting script resumes on one frame, executes on the next. */
    ScriptContext_Pause(&ctx, test_script_resume);
    CHECK(ScriptContext_Run(&ctx));
    CHECK(ctx.data[0] == 0);
    CHECK(ScriptContext_Run(&ctx));
    CHECK(ctx.data[0] == 0x12345678U);
    CHECK(ScriptContext_Run(&ctx));
    CHECK(!ScriptContext_Run(&ctx));

    ScriptContext_Start(&ctx, raw);
    CHECK(ScriptContext_ReadHalfWord(&ctx) == 0x1234);
    CHECK(ScriptContext_ReadWord(&ctx) == 0x90abcdefU);
    ScriptContext_Stop(&ctx);
    CHECK(ctx.scriptPtr == NULL);
}

/* Deterministic host clock for the original play-time manager. */
static u64 fakeTicks;

u64 Timer_GetCurrentTimestamp(void)
{
    return fakeTicks;
}

u64 Timer_TicksToSeconds(u64 ticks)
{
    return ticks;
}

static void test_play_time_manager(void)
{
    PlayTime playTime;
    PlayTime_Init(&playTime);
    fakeTicks = 100;
    PlayTime_Start(&playTime);
    PlayTime_IncrementTimer();
    CHECK(PlayTime_GetSeconds(&playTime) == 0);
    fakeTicks += 35;
    PlayTime_IncrementTimer();
    CHECK(PlayTime_GetSeconds(&playTime) == 35);
    PlayTime_IncrementTimer();
    CHECK(PlayTime_GetSeconds(&playTime) == 35);
    fakeTicks += 40;
    PlayTime_IncrementTimer();
    CHECK(PlayTime_GetMinutes(&playTime) == 1);
    CHECK(PlayTime_GetSeconds(&playTime) == 15);
    PlayTime_FlagNotStarted();
    fakeTicks += 10;
    PlayTime_IncrementTimer();
    CHECK(PlayTime_GetSeconds(&playTime) == 15);
}

static void test_poffin(void)
{
    Poffin poffin;
    Poffin copy;
    PoffinCase c;
    u8 attrs[8];

    Poffin_Clear(&poffin);
    CHECK(Poffin_SizeOf() == 8);
    CHECK(!Poffin_HasValidFlavor(&poffin));
    CHECK(poffin.spiciness == 0);
    CHECK(poffin.smoothness == 0);
    poffin.type = 1;
    poffin.spiciness = 15;
    poffin.sweetness = 25;
    poffin.smoothness = 30;
    CHECK(Poffin_HasValidFlavor(&poffin));
    CHECK(Poffin_GetAttribute(&poffin, POFFIN_ATTRIBUTEID_SPICINESS) == 15);
    Poffin_Copy(&poffin, &copy);
    CHECK(copy.type == poffin.type);
    CHECK(copy.spiciness == 15);
    CHECK(copy.sweetness == 25);
    CHECK(copy.smoothness == 30);

    PoffinCase_Init(&c);
    CHECK(PoffinCase_GetEmptySlot(&c) == 0);
    CHECK(PoffinCase_CountFilledSlots(&c) == 0);
    CHECK(PoffinCase_CountEmptySlots(&c) == MAX_POFFINS);
    CHECK(PoffinCase_AddPoffin(&c, &poffin) == 0);
    CHECK(PoffinCase_CountFilledSlots(&c) == 1);
    CHECK(PoffinCase_CountEmptySlots(&c) == MAX_POFFINS - 1);
    CHECK(PoffinCase_ClearSlot(&c, 0));
    CHECK(PoffinCase_CountFilledSlots(&c) == 0);
    (void)attrs;
}

static void test_game_options(void)
{
    Options options;
    Options copy;
    CHECK(sizeof(Options) == sizeof(u16));

    memset(&options, 0xFF, sizeof(options));
    Options_Init(&options);
    CHECK(Options_TextSpeed(&options) == OPTIONS_TEXT_SPEED_NORMAL);
    CHECK(Options_SoundMode(&options) == OPTIONS_SOUND_MODE_STEREO);
    CHECK(Options_BattleStyle(&options) == OPTIONS_BATTLE_STYLE_SHIFT);
    CHECK(Options_BattleScene(&options) == OPTIONS_BATTLE_SCENE_ON);
    CHECK(Options_ButtonMode(&options) == OPTIONS_BUTTON_MODE_NORMAL);
    CHECK(Options_Frame(&options) == OPTIONS_FRAME_1);
    CHECK(Options_TextFrameDelay(&options) == 4);

    Options_SetTextSpeed(&options, OPTIONS_TEXT_SPEED_FAST);
    Options_SetSoundMode(&options, OPTIONS_SOUND_MODE_MONO);
    Options_SetBattleStyle(&options, OPTIONS_BATTLE_STYLE_SET);
    Options_SetBattleScene(&options, OPTIONS_BATTLE_SCENE_OFF);
    Options_SetButtonMode(&options, OPTIONS_BUTTON_MODE_L_IS_A);
    Options_SetFrame(&options, OPTIONS_FRAME_12);

    CHECK(Options_TextFrameDelay(&options) == 1);
    CHECK(Options_SoundMode(&options) == OPTIONS_SOUND_MODE_MONO);
    CHECK(Options_BattleStyle(&options) == OPTIONS_BATTLE_STYLE_SET);
    CHECK(Options_BattleScene(&options) == OPTIONS_BATTLE_SCENE_OFF);
    CHECK(Options_ButtonMode(&options) == OPTIONS_BUTTON_MODE_L_IS_A);
    CHECK(Options_Frame(&options) == OPTIONS_FRAME_12);

    memset(&copy, 0, sizeof(copy));
    Options_Copy(&options, &copy);
    CHECK(memcmp(&options, &copy, sizeof(copy)) == 0);
}

/* Verify the official Metang generated C enums are visible to the real
 * game source build. The expected values come from upstream manifests. */
static void test_generated_constants(void)
{
    CHECK(ABILITY_NONE == 0);
    CHECK(ABILITY_STENCH == 1);
    CHECK(SPECIES_BULBASAUR == 1);
    CHECK(TEXT_BANK_MOVES_USED_IN_BATTLE == 0);
    CHECK(TEXT_BANK_DIPLOMA == 1);
    CHECK(TEXT_BANK_SPECIES_CATEGORY_JP == 723);
    CHECK(MOVE_FLAG_MAKES_CONTACT == 1);
    CHECK(AI_FLAG_BASIC == 1);
    CHECK((u64)AI_FLAG_ALL == 0xFFFFFFFFULL);
}

static void test_berry_data(void)
{
    /* This is a synthetic record, not an extracted ROM or NARC asset.
     * The real original BerryData_GetAttribute() accesses its fields. */
    BerryData data = {
        .size = 90,
        .firmness = 1,
        .baseYield = 2,
        .stageDuration = 3,
        .moistureDrainRate = 4,
        .spiciness = 5,
        .dryness = 6,
        .sweetness = 7,
        .bitterness = 8,
        .sourness = 9,
        .smoothness = 10,
    };
    CHECK(BerryData_GetAttribute(&data, BERRYATTR_SIZE) == 90);
    CHECK(BerryData_GetAttribute(&data, BERRYATTR_FIRMNESS) == 1);
    CHECK(BerryData_GetAttribute(&data, BERRYATTR_BASE_YIELD) == 2);
    CHECK(BerryData_GetAttribute(&data, BERRYATTR_STAGE_DURATION) == 3);
    CHECK(BerryData_GetAttribute(&data, BERRYATTR_MOISTURE_DRAIN_RATE) == 4);
    CHECK(BerryData_GetAttribute(&data, BERRYATTR_SPICINESS) == 5);
    CHECK(BerryData_GetAttribute(&data, BERRYATTR_DRYNESS) == 6);
    CHECK(BerryData_GetAttribute(&data, BERRYATTR_SWEETNESS) == 7);
    CHECK(BerryData_GetAttribute(&data, BERRYATTR_BITTERNESS) == 8);
    CHECK(BerryData_GetAttribute(&data, BERRYATTR_SOURNESS) == 9);
    CHECK(BerryData_GetAttribute(&data, BERRYATTR_SMOOTHNESS) == 10);
    CHECK(BerryData_GetAttribute(&data, (u32)BERRYATTR_SMOOTHNESS + 1) == 0);
}

static void test_poffin_type_lookup(void)
{
    PoffinTypeList types;
    memset(&types, 0, sizeof(types));
    /* The getter must read from the requested index without attempting to
     * access message files, allocators or game hardware. */
    CHECK(PoffinTypeList_GetString(&types, POFFIN_TYPE_SPICY) == NULL);
    CHECK(PoffinTypeList_GetString(&types, POFFIN_TYPE_DRY) == NULL);
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
    CHECK(REGIONAL_DEX_COUNT == 210); /* 211 upstream entries minus reserved slot */
    /* Original msgenc assigns message IDs sequentially from their JSON
     * arrays; these headers contain identifiers only, no message text. */
    CHECK(BoxText_MonSelected == 0);
    CHECK(BoxText_MarkMon == 1);
    CHECK(TEXT_BANK_BOX_MESSAGES_ENTRY_COUNT == 46);
    CHECK(pl_msg_00000611_00000 == 0);
    CHECK(pl_msg_00000611_00001 == 1);
    CHECK(TEXT_BANK_ABILITY_NAMES_UPPERCASE_ENTRY_COUNT == 124);
    CHECK(TEXT_BANK_UNK_0353_ENTRY_COUNT == 123);
    /* NitroSFX -naix uses filenames, not SDAT symbol names, and retains
     * zero-sized/reserved array positions as indices. */
    CHECK(SEQ_PV001_sseq == 1);
    CHECK(SEQ_PV001_sseq_1 == 2);
    CHECK(bulbasaur_cry_sbnk == 1);
    CHECK(PLAYER_PV == 0);
    /* NitroArc's --files-from order defines stable explicit archive indices.
     * Binary NARCs and graphics assets are deliberately not part of tests. */
    CHECK(generic_bg_tiles_NCLR == 0);
    CHECK(generic_bg_tiles_dupe_NCLR == 1);
    CHECK(digits_NCGR_lz == 2);
    CHECK(battle_factory_NCGR_lz == 0);
    CHECK(battle_factory_battle_room_NSCR_lz == 1);
    test_coins();
    test_ascii();
    test_charcode();
    test_play_time();
    test_distance();
    test_random_and_math();
    test_tile_behavior();
    test_field_script();
    test_play_time_manager();
    test_poffin();
    test_game_options();
    test_generated_constants();
    test_berry_data();
    test_poffin_type_lookup();
    test_sdk_link();
    printf("%d checks, %d failed\n", checks, fails);
    return fails == 0 ? 0 : 1;
}
