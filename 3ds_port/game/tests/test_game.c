/*
 * Small host smoke tests of unmodified pokeplatinum C source code.
 * This uses neither ROM content nor generated game data. A larger game
 * compilation target will grow from this separately tested foundation.
 */
#include <nitro.h>
#include <nnsys.h>
#include <nitro_port.h>
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
#include "grid_menu_cursor_position.h"
#include "overworld_map_history.h"
#include "pokedex_language.h"
#include "persisted_map_features.h"
#include "berry_tag_data.h"
#include "pokedex_memory.h"
#include "pokedex_data_index.h"
#include "ribbon_save_data.h"
#include "battle_regulation.h"
#include "record_mixed_rng.h"
#include "move_reminder_data.h"
#include "pokedex_heightweight.h"
#include "daycare_save.h"
#include "easy_chat_sentence.h"
#include "easy_chat_words.h"
#include "string_list.h"
#include "battle_factory_save.h"
#include "battle_arcade_save.h"
#include "res/text/bank/union_room_sentences.h"
#include "res/text/bank/general_sentences.h"
#include "constants/items.h"
#include "constants/field/map.h"
#include "badges.h" /* Original badge IDs are generated/badges.h. */
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
#include "res/graphics/pokedex/zukan.naix"
#include "res/trainers/classes/field_encounteffect.naix"
#include "res/battle/scripts/sub_seq.naix"
#include "res/text/bank/box_messages.h"
#include "res/text/bank/ability_names_uppercase.h"
#include "res/text/bank/species_name.h"
#include "res/text/bank/species_name_with_articles.h"
#include "res/text/bank/move_names.h"
#include "res/text/bank/move_names_uppercase.h"
#include "res/text/bank/move_descriptions.h"
#include "res/text/bank/moves_used_in_battle.h"
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

/* The play-time manager reads the game's real timer (timer.c): DS timer 3,
 * counting at 1/64 of the system clock, with an overflow interrupt that
 * extends it to 64 bits. In the port the timer's count register is plain
 * memory, so the test sets the clock by writing the count and raising the
 * overflow interrupts that would have happened. */
static u64 sTimerTicks;

static void SetClock(u64 ticks)
{
    while ((sTimerTicks >> 16) < (ticks >> 16)) {
        sTimerTicks += 0x10000;
        NitroPort_RaiseIrq(OS_IE_TIMER3);   /* the count wrapped */
    }
    sTimerTicks = ticks;
    *(vu16 *)((u32)REG_TM0CNT_L_ADDR + OS_TIMER_3 * 4) = (u16)ticks;
}

/* A time `sec` seconds after the start, plus 0.1 s so whole seconds don't
 * round down when converted back. */
static u64 Seconds(u64 start, u32 sec)
{
    return start + OS_SecondsToTicks(sec) + OS_MilliSecondsToTicks(100);
}

static void test_play_time_manager(void)
{
    PlayTime playTime;
    const u64 start = 1000;

    (void)OS_EnableIrq();
    Timer_Start();
    sTimerTicks = 0;
    SetClock(start);
    CHECK(Timer_GetCurrentTimestamp() == start);

    PlayTime_Init(&playTime);
    PlayTime_Start(&playTime);
    PlayTime_IncrementTimer();
    CHECK(PlayTime_GetSeconds(&playTime) == 0);
    SetClock(Seconds(start, 35));
    PlayTime_IncrementTimer();
    CHECK(PlayTime_GetSeconds(&playTime) == 35);
    PlayTime_IncrementTimer();
    CHECK(PlayTime_GetSeconds(&playTime) == 35);
    SetClock(Seconds(start, 75));
    PlayTime_IncrementTimer();
    CHECK(PlayTime_GetMinutes(&playTime) == 1);
    CHECK(PlayTime_GetSeconds(&playTime) == 15);
    PlayTime_FlagNotStarted();
    SetClock(Seconds(start, 85));
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


/* Original menu navigation table: coordinates and direction-to-index map. */
static void test_grid_menu_cursor(void)
{
    const GridMenuCursorPosition positions[] = {
        { 10, 20, 11, 21, 1, 2, 3, 4 },
        { 12, 22, 13, 23, 1, 0, 3, 4 },
        { 14, 24, 15, 25, 0, 2, 3, 4 },
        { 16, 26, 17, 27, 1, 2, 3, 0 },
        { 18, 28, 19, 29, 1, 2, 0, 4 },
    };
    u8 x1 = 0, y1 = 0, x2 = 0, y2 = 0;
    u8 index;

    GridMenuCursor_GetFirstCoords(&positions[0], &x1, &y1);
    CHECK(x1 == 10 && y1 == 20);
    GridMenuCursor_GetSecondCoords(&positions[0], &x2, &y2);
    CHECK(x2 == 11 && y2 == 21);

    index = GridMenuCursor_CheckNavigation(positions, &x1, &y1, &x2, &y2,
                                           0, GRID_MENU_CURSOR_POSITION_DIRECTION_UP);
    CHECK(index == 1);
    CHECK(x1 == 12 && y1 == 22);
    CHECK(x2 == 13 && y2 == 23);
    index = GridMenuCursor_CheckNavigation(positions, &x1, &y1, &x2, &y2,
                                           0, GRID_MENU_CURSOR_POSITION_DIRECTION_DOWN);
    CHECK(index == 2);
    CHECK(x1 == 14 && y1 == 24);
    CHECK(x2 == 15 && y2 == 25);
    index = GridMenuCursor_CheckNavigation(positions, &x1, &y1, &x2, &y2,
                                           0, GRID_MENU_CURSOR_POSITION_DIRECTION_LEFT);
    CHECK(index == 3);
    CHECK(x1 == 16 && y1 == 26);
    index = GridMenuCursor_CheckNavigation(positions, &x1, &y1, &x2, &y2,
                                           0, GRID_MENU_CURSOR_POSITION_DIRECTION_RIGHT);
    CHECK(index == 4);
    CHECK(x2 == 19 && y2 == 29);

    /* NONE uses the current position; null outputs are permitted. */
    CHECK(GridMenuCursor_CheckNavigation(positions, NULL, NULL, NULL, NULL,
                                        0, GRID_MENU_CURSOR_POSITION_DIRECTION_NONE) == 0);
    CHECK(GridMenuCursor_CheckNavigation(positions, NULL, NULL, NULL, NULL,
                                        0, GRID_MENU_CURSOR_POSITION_DIRECTION_LEFT) == 3);
}

static void test_overworld_map_history(void)
{
    OverworldMapHistory history;
    int i;

    memset(&history, 0xAB, sizeof(history));
    OverworldMapHistory_Clear(&history);
    CHECK(history.historyPointer == 0);
    CHECK(history.currentMapX == 0 && history.currentMapZ == 0);
    for (i = 0; i < OVERWORLD_MAP_HISTORY_LENGTH; i++) {
        CHECK(history.items[i].mapX == 0);
        CHECK(history.items[i].mapZ == 0);
        CHECK(history.items[i].faceDirection == 0);
        CHECK(history.items[i].isSet == FALSE);
    }

    OverworldMapHistory_Push(&history, 1, 2, 3);
    CHECK(history.historyPointer == 1);
    CHECK(history.currentMapX == 1 && history.currentMapZ == 2);
    CHECK(history.items[0].mapX == 1 && history.items[0].mapZ == 2);
    CHECK(history.items[0].faceDirection == 3 && history.items[0].isSet == TRUE);

    /* Repeating the current map must not advance the ring buffer. */
    OverworldMapHistory_Push(&history, 1, 2, 4);
    CHECK(history.historyPointer == 1);
    CHECK(history.items[0].faceDirection == 3);

    OverworldMapHistory_PushViaWarp(&history, MAP_TILES_COUNT_X * 5 + 10,
                                   MAP_TILES_COUNT_Z * 6 + 1);
    CHECK(history.historyPointer == 2);
    CHECK(history.items[1].mapX == 5 && history.items[1].mapZ == 6);
    CHECK(history.items[1].faceDirection == (u8)-1);
    CHECK(history.items[1].isSet == TRUE);
    /* The original warp variant does not update currentMapX/currentMapZ. */
    CHECK(history.currentMapX == 1 && history.currentMapZ == 2);

    for (i = 0; i < OVERWORLD_MAP_HISTORY_LENGTH + 2; i++) {
        OverworldMapHistory_Push(&history, 20 + i, 40 + i, 2);
    }
    CHECK(history.historyPointer == 4);
    CHECK(history.currentMapX == 27 && history.currentMapZ == 47);
    CHECK(history.items[2].mapX == 26 && history.items[2].mapZ == 46);
    CHECK(history.items[3].mapX == 27 && history.items[3].mapZ == 47);
}

static void test_pokedex_language(void)
{
    const int order[NUM_LANGUAGES] = {
        LANGUAGE_JAPANESE, LANGUAGE_ENGLISH, LANGUAGE_FRENCH,
        LANGUAGE_GERMAN, LANGUAGE_ITALIAN, LANGUAGE_SPANISH,
    };
    int i;

    for (i = 0; i < NUM_LANGUAGES; i++) {
        CHECK(PokedexLanguage_LanguageToIndex(order[i]) == i);
        CHECK(PokedexLanguage_IndexToLanguage(i) == order[i]);
    }
    CHECK(PokedexLanguage_LanguageToIndex(LANGUAGE_NONE) == NUM_LANGUAGES);
    CHECK(PokedexLanguage_LanguageToIndex(LANGUAGE_KOREAN) == NUM_LANGUAGES);
    CHECK(PokedexLanguage_LanguageToIndex(-1) == NUM_LANGUAGES);
}

static void test_persisted_map_features(void)
{
    PersistedMapFeatures features;
    u8 *buffer;
    int i;

    memset(&features, 0xA5, sizeof(features));
    PersistedMapFeatures_Init(&features);
    CHECK(PersistedMapFeatures_GetID(&features) == 0);
    for (i = 0; i < PERSISTED_MAP_FEATURES_BUFFER_SIZE; i++) {
        CHECK(features.buffer[i] == 0);
    }

    buffer = PersistedMapFeatures_InitWithID(&features, 42);
    CHECK(buffer == features.buffer);
    CHECK(PersistedMapFeatures_GetID(&features) == 42);
    CHECK(PersistedMapFeatures_GetBuffer(&features, 42) == features.buffer);
    buffer[0] = 0x12;
    buffer[PERSISTED_MAP_FEATURES_BUFFER_SIZE - 1] = 0x34;
    CHECK(features.buffer[0] == 0x12);
    CHECK(features.buffer[PERSISTED_MAP_FEATURES_BUFFER_SIZE - 1] == 0x34);

    buffer = PersistedMapFeatures_InitWithID(&features, 7);
    CHECK(buffer == features.buffer);
    CHECK(PersistedMapFeatures_GetID(&features) == 7);
    CHECK(buffer[0] == 0 && buffer[PERSISTED_MAP_FEATURES_BUFFER_SIZE - 1] == 0);
}


/* Test the original 64-berry tag bitset without allocating game heap data. */
static void test_berry_tag_data(void)
{
    BerryTagData data;
    u8 scroll = 0, index = 0;

    memset(&data, 0, sizeof(data));
    CHECK(NUM_BERRIES == 64);
    CHECK(!BerryTagData_Contains(data.entries, 0));
    CHECK(!BerryTagData_Contains(data.entries, 32));

    BerryTagData_Add(&data, FIRST_BERRY_IDX, FALSE);
    CHECK(BerryTagData_Contains(data.entries, 0));
    CHECK(!BerryTagData_Contains(data.entries, 1));
    CHECK(data.entries[0] == 1u);
    CHECK(data.currentBerryID == 0);

    /* Index 32 lives in the second u32 and initial selects that berry. */
    BerryTagData_Add(&data, FIRST_BERRY_IDX + 32, TRUE);
    CHECK(BerryTagData_Contains(data.entries, 32));
    CHECK(!BerryTagData_Contains(data.entries, 33));
    CHECK(data.entries[1] == 1u);
    CHECK(data.currentBerryID == 32);

    /* Adding the same berry again does not clear or toggle its bit. */
    BerryTagData_Add(&data, FIRST_BERRY_IDX + 32, FALSE);
    CHECK(data.entries[1] == 1u);
    CHECK(data.currentBerryID == 32);

    BerryTagData_SetScroll(&data, 6, 3, 12);
    BerryTagData_GetScroll(&data, &scroll, &index);
    CHECK(scroll == 6 && index == 3);
    CHECK(data.count == 12);
}

static void test_pokedex_memory_state(void)
{
    PokedexMemory memory = { 0 };

    memory.bootMode = POKEDEX_BOOT_DEFAULT;
    CHECK(PokedexMemory_GetCurrentSpecies(&memory) == 0);
    CHECK(PokedexMemory_GetBootMode(&memory) == POKEDEX_BOOT_DEFAULT);
    PokedexMemory_SetCurrentSpecies(&memory, 493);
    CHECK(PokedexMemory_GetCurrentSpecies(&memory) == 493);
    PokedexMemory_SetBootMode(&memory, POKEDEX_BOOT_NATIONAL);
    CHECK(PokedexMemory_GetBootMode(&memory) == POKEDEX_BOOT_NATIONAL);
    PokedexMemory_SetCurrentSpecies(&memory, 1);
    PokedexMemory_SetBootMode(&memory, POKEDEX_BOOT_REGIONAL);
    CHECK(PokedexMemory_GetCurrentSpecies(&memory) == 1);
    CHECK(PokedexMemory_GetBootMode(&memory) == POKEDEX_BOOT_REGIONAL);
}

static void test_pokedex_data_bank_switch(void)
{
    /* Pin the original normal/Giratina NARC and message-bank selections. */
    Pokedex_SetupGiratina(1);
    CHECK(Pokedex_Data_NARC_Index() ==
          NARC_INDEX_APPLICATION__ZUKANLIST__ZKN_DATA__ZUKAN_DATA);
    CHECK(Weight_Message_Bank_Index() == TEXT_BANK_SPECIES_WEIGHT);
    CHECK(Height_Message_Bank_Index() == TEXT_BANK_SPECIES_HEIGHT);

    Pokedex_SetupGiratina(0);
    CHECK(Pokedex_Data_NARC_Index() ==
          NARC_INDEX_APPLICATION__ZUKANLIST__ZKN_DATA__ZUKAN_DATA_GIRA);
    CHECK(Weight_Message_Bank_Index() == TEXT_BANK_SPECIES_WEIGHT_GIRA);
    CHECK(Height_Message_Bank_Index() == TEXT_BANK_SPECIES_HEIGHT_GIRA);

    /* All non-zero arguments restore the original regular data banks. */
    Pokedex_SetupGiratina(2);
    CHECK(Pokedex_Data_NARC_Index() ==
          NARC_INDEX_APPLICATION__ZUKANLIST__ZKN_DATA__ZUKAN_DATA);
    CHECK(Weight_Message_Bank_Index() == TEXT_BANK_SPECIES_WEIGHT);
    CHECK(Height_Message_Bank_Index() == TEXT_BANK_SPECIES_HEIGHT);
}

static void test_ribbon_save_state(void)
{
    u8 guarded[RIBBONS_SAVE_SIZE + 2];
    int i;

    CHECK(Ribbons_SaveSize() == RIBBONS_SAVE_SIZE);
    CHECK(RIBBONS_SAVE_SIZE == 14);
    memset(guarded, 0xA5, sizeof(guarded));
    Ribbons_Init(&guarded[1]);
    CHECK(guarded[0] == 0xA5);
    CHECK(guarded[RIBBONS_SAVE_SIZE + 1] == 0xA5);
    for (i = 1; i <= RIBBONS_SAVE_SIZE; i++) {
        CHECK(guarded[i] == 0);
    }
}


/* This upstream symbol is defined in battle_regulation.c but not declared
 * in its public header. Referencing the real presets directly lets host
 * tests avoid BattleRegulation_GetByIndex's unported SaveData_SaveTable path. */
extern BattleRegulation BattleRegulation_PredefinedRules[];

/* Actual Platinum battle-rule tables and bitfield rule selection. */
static void test_battle_regulation_rules(void)
{
    BattleRegulationData saved;
    BattleRegulation custom = { 0 }, copy = { 0 };
    const BattleRegulation *defaults = BattleRegulation_GetDefault();
    const BattleRegulation *preset0 = &BattleRegulation_PredefinedRules[0];
    const BattleRegulation *preset1 = &BattleRegulation_PredefinedRules[1];
    const BattleRegulation *preset2 = &BattleRegulation_PredefinedRules[2];

    CHECK(BattleRegulation_Size() == sizeof(BattleRegulation));
    CHECK(BattleRegulation_SaveSize() == sizeof(BattleRegulationData));
    memset(&saved, 0xAA, sizeof(saved));
    RegulationBattles_Init(&saved);
    CHECK(saved.regulations[0].teamSize == 0);
    CHECK(saved.regulations[0].maxLevel == 0);

    CHECK(defaults != NULL);
    CHECK(BattleRegulation_GetRuleValue(defaults, BATTLE_REGULATION_RULE_TEAM_SIZE) == 6);
    CHECK(BattleRegulation_GetRuleValue(defaults, BATTLE_REGULATION_RULE_MAX_LEVEL) == 100);
    CHECK(BattleRegulation_GetRuleValue(defaults, BATTLE_REGULATION_RULE_EVOLUTION_RESTRICT) == 1);
    CHECK(BattleRegulation_GetRuleValue(defaults, BATTLE_REGULATION_RULE_ITEM_UNIQUE) == 1);
    CHECK(BattleRegulation_GetRuleValue(defaults, BATTLE_REGULATION_RULE_SPECIES_UNIQUE) == 1);
    CHECK(BattleRegulation_GetIndex(NULL) == 0xFF);

    /* Exercise original predefined data and its production GetIndex logic;
     * the GetByIndex API also references SaveData_SaveTable, not linked yet. */
    CHECK(preset0 != NULL && preset1 != NULL && preset2 != NULL);
    CHECK(BattleRegulation_GetIndex(preset0) == 0);
    CHECK(BattleRegulation_GetIndex(preset1) == 1);
    CHECK(BattleRegulation_GetIndex(preset2) == 2);
    CHECK(BattleRegulation_GetRuleValue(preset0, BATTLE_REGULATION_RULE_TEAM_SIZE) == 3);
    CHECK(BattleRegulation_GetRuleValue(preset1, BATTLE_REGULATION_RULE_HEIGHT_RESTRICT) == -20);
    CHECK(BattleRegulation_GetRuleValue(preset2, BATTLE_REGULATION_RULE_MAX_LEVEL) == 5);

    custom.teamSize = 4;
    custom.maxLevel = 50;
    custom.maxTotalLevel = 120;
    custom.heightRestriction = -15;
    custom.weightRestriction = 22;
    custom.evolutionRestriction = 1;
    custom.specialSpeciesRestriction = 1;
    custom.itemUniqueness = 1;
    custom.speciesUniqueness = 0;
    custom.moveRestrictions = 1;
    BattleRegulation_Copy(&custom, &copy);
    CHECK(memcmp(&custom, &copy, sizeof(custom)) == 0);
    CHECK(BattleRegulation_GetRuleValue(&copy, BATTLE_REGULATION_RULE_TEAM_SIZE) == 4);
    CHECK(BattleRegulation_GetRuleValue(&copy, BATTLE_REGULATION_RULE_MAX_LEVEL) == 50);
    CHECK(BattleRegulation_GetRuleValue(&copy, BATTLE_REGULATION_RULE_MAX_TOTAL_LEVEL) == 120);
    CHECK(BattleRegulation_GetRuleValue(&copy, BATTLE_REGULATION_RULE_HEIGHT_RESTRICT) == -15);
    CHECK(BattleRegulation_GetRuleValue(&copy, BATTLE_REGULATION_RULE_HEIGHT_TYPE) == -1);
    CHECK(BattleRegulation_GetRuleValue(&copy, BATTLE_REGULATION_RULE_WEIGHT_RESTRICT) == 22);
    CHECK(BattleRegulation_GetRuleValue(&copy, BATTLE_REGULATION_RULE_WEIGHT_TYPE) == 1);
    CHECK(BattleRegulation_GetRuleValue(&copy, BATTLE_REGULATION_RULE_SPECIAL_SPECIES_RESTRICT) == 1);
    CHECK(BattleRegulation_GetRuleValue(&copy, BATTLE_REGULATION_RULE_MOVE_RESTRICTIONS) == 1);
    CHECK(BattleRegulation_GetRuleValue(&copy, BATTLE_REGULATION_RULE_SPECIES_UNIQUE) == 0);
    CHECK(BattleRegulation_GetIndex(&copy) == 5);
    copy.heightRestriction = 0;
    copy.weightRestriction = 0;
    CHECK(BattleRegulation_GetRuleValue(&copy, BATTLE_REGULATION_RULE_HEIGHT_TYPE) == 0);
    CHECK(BattleRegulation_GetRuleValue(&copy, BATTLE_REGULATION_RULE_WEIGHT_TYPE) == 0);
}

static void test_record_mixed_rng(void)
{
    RecordMixedRNG entries[RECORD_MIXED_RNG_MAX];
    const charcode_t nameA[] = { CHAR_WIDE_A, CHAR_EOS };
    const charcode_t emptyName[] = { CHAR_EOS };
    u32 firstRand, secondRand;
    int i;

    memset(entries, 0xA5, sizeof(entries));
    RecordMixedRNG_Init(entries);
    CHECK(RECORD_MIXED_RNG_MAX == 6);
    CHECK(RecordMixedRNG_SaveSize() == sizeof(entries));
    for (i = 0; i < RECORD_MIXED_RNG_MAX; i++) {
        CHECK(entries[i].groupName[0] == CHAR_EOS);
        CHECK(entries[i].playerName[0] == CHAR_EOS);
        CHECK(entries[i].seed == 0);
        CHECK(entries[i].rand == 0);
        CHECK(!RecordMixedRNG_IsEntryValid(entries, i));
    }

    RecordMixedRNG_SetEntrySeed(entries, RECORD_MIXED_RNG_PLAYER_OVERRIDE, 1234u);
    firstRand = ARNG_Next(1234u);
    secondRand = ARNG_Next(firstRand);
    CHECK(RecordMixedRNG_GetEntrySeed(entries, RECORD_MIXED_RNG_PLAYER_OVERRIDE) == 1234u);
    CHECK(RecordMixedRNG_GetRand(entries) == firstRand);
    RecordMixedRNG_AdvanceEntries(entries, 0);
    CHECK(RecordMixedRNG_GetRand(entries) == firstRand);
    RecordMixedRNG_AdvanceEntries(entries, 1);
    CHECK(RecordMixedRNG_GetRand(entries) == secondRand);
    RecordMixedRNG_AdvanceEntries(entries, 2);
    CHECK(RecordMixedRNG_GetRand(entries) == ARNG_Next(ARNG_Next(secondRand)));

    entries[RECORD_MIXED_RNG_PLAYER_OVERRIDE].groupName[0] = CHAR_WIDE_A;
    entries[RECORD_MIXED_RNG_PLAYER_OVERRIDE].groupName[1] = CHAR_EOS;
    entries[RECORD_MIXED_RNG_PLAYER_OVERRIDE].playerName[0] = CHAR_WIDE_B;
    entries[RECORD_MIXED_RNG_PLAYER_OVERRIDE].playerName[1] = CHAR_EOS;
    RecordMixedRNG_SetEntryGender(entries, RECORD_MIXED_RNG_PLAYER_OVERRIDE, 1);
    RecordMixedRNG_SetEntryLanguage(entries, RECORD_MIXED_RNG_PLAYER_OVERRIDE, LANGUAGE_ENGLISH);
    CHECK(RecordMixedRNG_IsEntryValid(entries, RECORD_MIXED_RNG_PLAYER_OVERRIDE));
    CHECK(RecordMixedRNG_GetEntryGender(entries, RECORD_MIXED_RNG_PLAYER_OVERRIDE) == 1);
    CHECK(RecordMixedRNG_GetEntryLanguage(entries, RECORD_MIXED_RNG_PLAYER_OVERRIDE) == LANGUAGE_ENGLISH);
    CHECK(RecordMixedRNG_GetEntryName(entries, RECORD_MIXED_RNG_PLAYER_OVERRIDE,
                                     RECORD_MIXED_RNG_GROUP_NAME) ==
          entries[RECORD_MIXED_RNG_PLAYER_OVERRIDE].groupName);
    CHECK(RecordMixedRNG_GetEntryName(entries, RECORD_MIXED_RNG_PLAYER_OVERRIDE,
                                     RECORD_MIXED_RNG_PLAYER_NAME) ==
          entries[RECORD_MIXED_RNG_PLAYER_OVERRIDE].playerName);

    CHECK(!RecordMixedRNG_DoesCollectionContainGroup(entries, emptyName));
    CHECK(RecordMixedRNG_DoesCollectionContainGroup(entries, nameA));
    RecordMixedRNG_CopyEntry(entries, RECORD_MIXED_RNG_PLAYER_OVERRIDE, RECORD_MIXED_RNG_QUEUE_0);
    CHECK(RecordMixedRNG_IsEntryEqualToOverride(entries, RECORD_MIXED_RNG_QUEUE_0));
    entries[RECORD_MIXED_RNG_QUEUE_0].rand = 7;
    CHECK(RecordMixedRNG_AreEntriesEqual(
        &entries[RECORD_MIXED_RNG_PLAYER_OVERRIDE], &entries[RECORD_MIXED_RNG_QUEUE_0]));
    RecordMixedRNG_SetEntryLanguage(entries, RECORD_MIXED_RNG_QUEUE_0, LANGUAGE_SPANISH);
    CHECK(!RecordMixedRNG_IsEntryEqualToOverride(entries, RECORD_MIXED_RNG_QUEUE_0));
    CHECK(RecordMixedRNG_IsEntryEmpty(&entries[RECORD_MIXED_RNG_QUEUE_1]));
}


/* Test only original move-list sentinel detection. Generating moves from a
 * Pokémon still requires the save/game heap and full Pokémon data backends. */
static void test_move_reminder_data(void)
{
    u16 noMoves[] = { LEVEL_UP_MOVESET_TERMINATOR, 0 };
    u16 oneMove[] = { 1, LEVEL_UP_MOVESET_TERMINATOR };
    u16 zeroFirstMove[] = { 0, LEVEL_UP_MOVESET_TERMINATOR };

    CHECK(!MoveReminderData_HasMoves(noMoves));
    CHECK(MoveReminderData_HasMoves(oneMove));
    /* Original function checks the first value against the terminator only. */
    CHECK(MoveReminderData_HasMoves(zeroFirstMove));
}

/* Test genuine accessor functions with synthetic local measurements.
 * No NARC content or copyrighted Pokédex data is loaded. */
static void test_pokedex_heightweight_accessors(void)
{
    int heights[] = { 10, 45, 80 };
    int weights[] = { 100, 230, 900 };
    short trainerPositions[] = { -2, 3, 7 };
    short pokemonPositions[] = { 8, -5, 4 };
    short trainerScales[] = { 12, 16, 24 };
    short pokemonScales[] = { 27, 29, 33 };
    HeightWeightData data = {
        .height = heights,
        .weight = weights,
        .trainerPos = trainerPositions,
        .pokemonPos = pokemonPositions,
        .trainerScale = trainerScales,
        .pokemonScale = pokemonScales,
    };

    CHECK(Pokedex_HeightWeightData_Height(&data, 0) == 10);
    CHECK(Pokedex_HeightWeightData_Height(&data, 2) == 80);
    CHECK(Pokedex_HeightWeightData_Weight(&data, 1) == 230);
    CHECK(Pokedex_HeightWeightData_Weight(&data, 2) == 900);
    CHECK(Pokedex_HeightWeightData_TrainerPos(&data, 0) == -2);
    CHECK(Pokedex_HeightWeightData_TrainerPos(&data, 2) == 7);
    CHECK(Pokedex_HeightWeightData_PokemonPos(&data, 0) == 8);
    CHECK(Pokedex_HeightWeightData_PokemonPos(&data, 1) == -5);
    CHECK(Pokedex_HeightWeightData_TrainerScale(&data, 0) == 12);
    CHECK(Pokedex_HeightWeightData_TrainerScale(&data, 2) == 24);
    CHECK(Pokedex_HeightWeightData_PokemonScale(&data, 1) == 29);
    CHECK(Pokedex_HeightWeightData_PokemonScale(&data, 2) == 33);
}

/* Exercise original Day Care state helpers without BoxPokemon_Init(),
 * which will require the full Pokémon implementation later. */
static void test_daycare_state(void)
{
    Daycare state, copy;
    DaycareMon *first, *second;
    DaycareMail *mail;

    memset(&state, 0, sizeof(state));
    memset(&copy, 0, sizeof(copy));
    CHECK(Daycare_SaveSize() == sizeof(Daycare));
    CHECK(!Daycare_HasEgg(&state));
    CHECK(Daycare_GetOffspringPersonality(&state) == 0);
    CHECK(Daycare_GetStepCounter(&state) == 0);

    first = Daycare_GetDaycareMon(&state, 0);
    second = Daycare_GetDaycareMon(&state, 1);
    CHECK(first == &state.mons[0]);
    CHECK(second == &state.mons[1]);
    CHECK(DaycareMon_GetBoxMon(first) == &state.mons[0].boxMon);
    mail = DaycareMon_GetDaycareMail(first);
    CHECK(mail == &state.mons[0].daycareMail);
    CHECK(DaycareMail_GetMail(mail) == &mail->mail);

    DaycareMon_SetSteps(first, 100);
    DaycareMon_AddSteps(first, 25);
    CHECK(DaycareMon_GetSteps(first) == 125);
    CHECK(DaycareMon_GetSteps(second) == 0);
    DaycareMon_SetSteps(second, 7);
    CHECK(DaycareMon_GetSteps(second) == 7);
    DaycareMon_CopyToDaycareMon(&copy.mons[0], first);
    CHECK(DaycareMon_GetSteps(&copy.mons[0]) == 125);
    CHECK(memcmp(&copy.mons[0], first, sizeof(DaycareMon)) == 0);

    Daycare_SetOffspringPersonality(&state, 0x12345678);
    CHECK(Daycare_HasEgg(&state));
    CHECK(Daycare_GetOffspringPersonality(&state) == 0x12345678);
    Daycare_SetStepCounter(&state, 250);
    CHECK(Daycare_GetStepCounter(&state) == 250);
    Daycare_SetOffspringPersonality(&state, 0);
    CHECK(!Daycare_HasEgg(&state));
}



/* Real source-derived Easy Chat sentence state and comparison logic.
 * String expansion/slot count from the message NARC is not yet available. */
static void test_easy_chat_sentence(void)
{
    EasyChatSentence sentence, saved, other;
    u32 count;
    int type;

    memset(&sentence, 0xA5, sizeof(sentence));
    sentence.id = 77;
    EasyChatSentence_Init(&sentence);
    CHECK(!EasyChatSentence_IsValid(&sentence));
    CHECK(EasyChatSentence_GetType(&sentence) == EASY_CHAT_SENTENCE_TYPE_NONE);
    CHECK(EasyChatSentence_GetWord(&sentence, 0) == WORD_NONE);
    CHECK(EasyChatSentence_GetWord(&sentence, 1) == WORD_NONE);
    /* Original Init deliberately preserves the ID field. */
    CHECK(EasyChatSentence_GetID(&sentence) == 77);

    EasyChatSentence_InitWithType(&sentence, EASY_CHAT_SENTENCE_TYPE_PRE_BATTLE);
    CHECK(EasyChatSentence_IsValid(&sentence));
    CHECK(EasyChatSentence_GetType(&sentence) == EASY_CHAT_SENTENCE_TYPE_PRE_BATTLE);
    CHECK(EasyChatSentence_GetID(&sentence) == 0);
    CHECK(EasyChatSentence_GetWord(&sentence, 0) == WORD_NONE);
    CHECK(EasyChatSentence_GetWord(&sentence, 1) == WORD_NONE);

    EasyChatSentence_SetTypeAndID(&sentence, EASY_CHAT_SENTENCE_TYPE_WIN, 2);
    EasyChatSentence_SetWord(&sentence, 0, 123);
    EasyChatSentence_SetWord(&sentence, 1, 456);
    CHECK(EasyChatSentence_GetType(&sentence) == EASY_CHAT_SENTENCE_TYPE_WIN);
    CHECK(EasyChatSentence_GetID(&sentence) == 2);
    CHECK(EasyChatSentence_GetWord(&sentence, 0) == 123);
    CHECK(EasyChatSentence_GetWord(&sentence, 1) == 456);

    EasyChatSentence_Copy(&saved, &sentence);
    CHECK(EasyChatSentence_Compare(&sentence, &saved));
    CHECK(memcmp(&sentence, &saved, sizeof(sentence)) == 0);
    EasyChatSentence_SetWord(&saved, 1, 457);
    CHECK(!EasyChatSentence_Compare(&sentence, &saved));
    EasyChatSentence_Copy(&saved, &sentence);
    EasyChatSentence_SetTypeAndID(&saved, EASY_CHAT_SENTENCE_TYPE_LOSS, 2);
    CHECK(!EasyChatSentence_Compare(&sentence, &saved));
    EasyChatSentence_Copy(&saved, &sentence);
    EasyChatSentence_SetTypeAndID(&saved, EASY_CHAT_SENTENCE_TYPE_WIN, 3);
    CHECK(!EasyChatSentence_Compare(&sentence, &saved));

    EasyChatSentence_InitWithEnteredUnionRoom(&other);
    CHECK(EasyChatSentence_IsValid(&other));
    CHECK(EasyChatSentence_GetType(&other) == EASY_CHAT_SENTENCE_TYPE_UNION_ROOM);
    CHECK(EasyChatSentence_GetID(&other) == UnionRoomSentences_Text_IveEnteredUnionRoom);
    CHECK(EasyChatSentence_GetWord(&other, 0) == WORD_NONE);
    CHECK(EasyChatSentence_GetWord(&other, 1) == WORD_NONE);

    count = EasyChatSentence_GetBankEntryCountForType(EASY_CHAT_SENTENCE_TYPE_PRE_BATTLE);
    CHECK(count == TEXT_BANK_GENERAL_SENTENCES_ENTRY_COUNT);
    CHECK(count > 0);
    for (type = EASY_CHAT_SENTENCE_TYPE_WIN; type < EASY_CHAT_SENTENCE_TYPE_COUNT; type++) {
        CHECK(EasyChatSentence_GetBankEntryCountForType(type) == count);
    }
    CHECK(EasyChatSentence_GetBankEntryCountForType(EASY_CHAT_SENTENCE_TYPE_COUNT) == 0);
}

/* No heap or message archive needed: add pre-existing entries to a
 * caller-owned list, honoring its original terminal sentinel. */
static void test_string_list_copy_entry(void)
{
    StringList list[4];
    StringList source;
    int firstValue = 101, secondValue = 202, thirdValue = 303;

    memset(list, 0, sizeof(list));
    list[3].entry = STRING_LIST_TERMINATOR;
    list[3].index = 7;

    source.entry = &firstValue;
    source.index = 42;
    StringList_AddFromEntry(list, &source);
    CHECK(list[0].entry == &firstValue);
    CHECK(list[0].index == 42);
    CHECK(list[1].entry == NULL);
    CHECK(list[3].entry == STRING_LIST_TERMINATOR);
    CHECK(list[3].index == 7);

    source.entry = &secondValue;
    source.index = 0xABC;
    StringList_AddFromEntry(list, &source);
    CHECK(list[1].entry == &secondValue);
    CHECK(list[1].index == 0xABC);
    CHECK(list[0].entry == &firstValue);
    CHECK(list[0].index == 42);

    source.entry = &thirdValue;
    source.index = 0;
    StringList_AddFromEntry(list, &source);
    CHECK(list[2].entry == &thirdValue);
    CHECK(list[2].index == 0);
    CHECK(list[3].entry == STRING_LIST_TERMINATOR);
    CHECK(list[3].index == 7);
}


/* The original Battle Factory record's read/write functions operate on
 * caller-owned saved state. SaveData_SaveTable is not linked yet. */
static void test_battle_factory_saved_state(void)
{
    BattleFactorySave save;
    BattleFactoryStreakFlags flags;
    u8 challenge = 5, round = 6, level = 1, iv = 31;
    u8 off = 0, on = 1, positive = 2;
    u16 trainer = 438, rental = 2468, opponentRental = 1357;
    u32 personality = 0x1248ACE0u, opponentPersonality = 0xCAFEBABEu;

    memset(&save, 0x7B, sizeof(save));
    BattleFactorySave_Init(&save);
    CHECK(save.didSave == 0);
    CHECK(save.trainerIDs[13] == 0);
    CHECK(save.playerPersonality[3] == 0);
    CHECK(save.opponentPersonality[3] == 0);
    BattleFactorySave_RecordSave(&save, TRUE);
    CHECK(BattleFactorySave_HasSaved(&save));
    BattleFactorySave_RecordSave(&save, FALSE);
    CHECK(!BattleFactorySave_HasSaved(&save));

    BattleFactorySave_SetMember(&save, FACTORY_SAVE_OPEN_LEVEL, 0, &level);
    BattleFactorySave_SetMember(&save, FACTORY_SAVE_CHALLENGE_TYPE, 0, &challenge);
    BattleFactorySave_SetMember(&save, FACTORY_SAVE_CURRENT_BATTLE, 0, &round);
    BattleFactorySave_SetMember(&save, FACTORY_SAVE_TRAINER_IDS, 13, &trainer);
    BattleFactorySave_SetMember(&save, FACTORY_SAVE_PLAYER_SETS, 3, &rental);
    BattleFactorySave_SetMember(&save, FACTORY_SAVE_PLAYER_IVS, 3, &iv);
    BattleFactorySave_SetMember(&save, FACTORY_SAVE_PLAYER_PERSONALITY, 3, &personality);
    BattleFactorySave_SetMember(&save, FACTORY_SAVE_OPPONENT_SETS, 3, &opponentRental);
    BattleFactorySave_SetMember(&save, FACTORY_SAVE_OPPONENT_IVS, 3, &iv);
    BattleFactorySave_SetMember(&save, FACTORY_SAVE_OPPONENT_PERSONALITY, 3, &opponentPersonality);
    CHECK(BattleFactorySave_GetMember(&save, FACTORY_SAVE_OPEN_LEVEL, 0, NULL) == 1);
    CHECK(BattleFactorySave_GetMember(&save, FACTORY_SAVE_CHALLENGE_TYPE, 0, NULL) == 5);
    CHECK(BattleFactorySave_GetMember(&save, FACTORY_SAVE_CURRENT_BATTLE, 0, NULL) == 6);
    CHECK(BattleFactorySave_GetMember(&save, FACTORY_SAVE_TRAINER_IDS, 13, NULL) == trainer);
    CHECK(BattleFactorySave_GetMember(&save, FACTORY_SAVE_PLAYER_SETS, 3, NULL) == rental);
    CHECK(BattleFactorySave_GetMember(&save, FACTORY_SAVE_PLAYER_IVS, 3, NULL) == iv);
    CHECK(BattleFactorySave_GetMember(&save, FACTORY_SAVE_PLAYER_PERSONALITY, 3, NULL) == personality);
    CHECK(BattleFactorySave_GetMember(&save, FACTORY_SAVE_OPPONENT_SETS, 3, NULL) == opponentRental);
    CHECK(BattleFactorySave_GetMember(&save, FACTORY_SAVE_OPPONENT_IVS, 3, NULL) == iv);
    CHECK(BattleFactorySave_GetMember(&save, FACTORY_SAVE_OPPONENT_PERSONALITY, 3, NULL) == opponentPersonality);
    CHECK(BattleFactorySave_GetMember(&save, FACTORY_SAVE_TRAINER_IDS, 12, NULL) == 0);
    CHECK(BattleFactorySave_GetMember(&save, FACTORY_SAVE_PLAYER_PERSONALITY, 2, NULL) == 0);
    CHECK(BattleFactorySave_GetMember(&save, 255, 0, NULL) == 0);

    memset(&flags, 0x7B, sizeof(flags));
    BattleFactoryStreakFlags_Init(&flags);
    CHECK(flags.streakActiveFlags == 0);
    BattleFactoryStreakFlags_SetFlag(&flags, FACTORY_SAVE_STREAK_FLAGS, 0, &on);
    BattleFactoryStreakFlags_SetFlag(&flags, FACTORY_SAVE_STREAK_FLAGS, 3, &positive);
    BattleFactoryStreakFlags_SetFlag(&flags, FACTORY_SAVE_STREAK_FLAGS, 7, &on);
    CHECK(flags.streakActiveFlags == 0x89);
    CHECK(BattleFactoryStreakFlags_GetFlag(&flags, FACTORY_SAVE_STREAK_FLAGS, 3, NULL) == 1);
    BattleFactoryStreakFlags_SetFlag(&flags, FACTORY_SAVE_STREAK_FLAGS, 3, &off);
    CHECK(flags.streakActiveFlags == 0x81);
    CHECK(BattleFactoryStreakFlags_GetFlag(&flags, FACTORY_SAVE_STREAK_FLAGS, 3, NULL) == 0);
    CHECK(BattleFactoryStreakFlags_GetFlag(&flags, FACTORY_SAVE_STREAK_FLAGS, 7, NULL) == 1);
}

/* Original Arcade metadata and bit flags, without the online/save runtime. */
static void test_battle_arcade_saved_state(void)
{
    BattleArcadeSave save;
    BattleArcadeStreakFlags flags;
    u8 challenge = 4, roulette = 1, round = 7, speed = 3, partySlot = 2;
    u8 on = 1, off = 0, positive = 2;
    u16 heldItem = 450, trainer = 0xBEEFu, monSet = 1234;

    memset(&save, 0x7B, sizeof(save));
    BattleArcadeSave_Init(&save);
    CHECK(save.didSave == 0);
    CHECK(save.trainerIDs[13] == 0);
    CHECK(save.monSetIDs[13] == 0);
    BattleArcadeSave_RecordSave(&save, TRUE);
    CHECK(BattleArcadeSave_HasSaved(&save));
    BattleArcadeSave_RecordSave(&save, FALSE);
    CHECK(!BattleArcadeSave_HasSaved(&save));

    BattleArcadeSave_SetMember(&save, ARCADE_SAVE_CHALLENGE_TYPE, 0, 0, &challenge);
    BattleArcadeSave_SetMember(&save, ARCADE_SAVE_CURSOR_RANDOMIZED, 0, 0, &roulette);
    BattleArcadeSave_SetMember(&save, ARCADE_SAVE_CURRENT_BATTLE, 0, 0, &round);
    BattleArcadeSave_SetMember(&save, ARCADE_SAVE_ROULETTE_SPEED, 0, 0, &speed);
    BattleArcadeSave_SetMember(&save, ARCADE_SAVE_HELD_ITEMS, 3, 0, &heldItem);
    BattleArcadeSave_SetMember(&save, ARCADE_SAVE_TRAINER_IDS, 13, 0, &trainer);
    BattleArcadeSave_SetMember(&save, ARCADE_SAVE_PARTY_SLOTS, 2, 0, &partySlot);
    BattleArcadeSave_SetMember(&save, ARCADE_SAVE_MON_SET_IDS, 13, 0, &monSet);

    CHECK(BattleArcadeSave_GetMember(&save, ARCADE_SAVE_CHALLENGE_TYPE, 0, 0, NULL) == challenge);
    CHECK(BattleArcadeSave_GetMember(&save, ARCADE_SAVE_CURSOR_RANDOMIZED, 0, 0, NULL) == roulette);
    CHECK(BattleArcadeSave_GetMember(&save, ARCADE_SAVE_CURRENT_BATTLE, 0, 0, NULL) == round);
    CHECK(BattleArcadeSave_GetMember(&save, ARCADE_SAVE_ROULETTE_SPEED, 0, 0, NULL) == speed);
    CHECK(BattleArcadeSave_GetMember(&save, ARCADE_SAVE_HELD_ITEMS, 3, 0, NULL) == heldItem);
    CHECK(BattleArcadeSave_GetMember(&save, ARCADE_SAVE_TRAINER_IDS, 13, 0, NULL) == trainer);
    CHECK(BattleArcadeSave_GetMember(&save, ARCADE_SAVE_PARTY_SLOTS, 2, 0, NULL) == partySlot);
    CHECK(BattleArcadeSave_GetMember(&save, ARCADE_SAVE_MON_SET_IDS, 13, 0, NULL) == monSet);
    CHECK(BattleArcadeSave_GetMember(&save, ARCADE_SAVE_HELD_ITEMS, 2, 0, NULL) == 0);
    CHECK(BattleArcadeSave_GetMember(&save, ARCADE_SAVE_TRAINER_IDS, 12, 0, NULL) == 0);

    memset(&flags, 0x7B, sizeof(flags));
    BattleArcadeStreakFlags_Init(&flags);
    CHECK(flags.streakActiveFlags == 0);
    BattleArcadeStreakFlags_SetFlag(&flags, ARCADE_SAVE_STREAK_FLAGS, 2, 0, &on);
    BattleArcadeStreakFlags_SetFlag(&flags, ARCADE_SAVE_STREAK_FLAGS, 5, 0, &positive);
    BattleArcadeStreakFlags_SetFlag(&flags, ARCADE_SAVE_STREAK_FLAGS, 7, 0, &on);
    CHECK(flags.streakActiveFlags == 0xA4);
    CHECK(BattleArcadeStreakFlags_GetFlag(&flags, ARCADE_SAVE_STREAK_FLAGS, 5, 0, NULL) == 1);
    BattleArcadeStreakFlags_SetFlag(&flags, ARCADE_SAVE_STREAK_FLAGS, 5, 0, &off);
    CHECK(flags.streakActiveFlags == 0x84);
    CHECK(BattleArcadeStreakFlags_GetFlag(&flags, ARCADE_SAVE_STREAK_FLAGS, 5, 0, NULL) == 0);
    CHECK(BattleArcadeStreakFlags_GetFlag(&flags, ARCADE_SAVE_STREAK_FLAGS, 7, 0, NULL) == 1);
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
    /* Original moveproc visits MOVE_NONE..MOVE_SHADOW_FORCE in enum order.
     * We validate IDs, not game text or the compiled move-data NARC. */
    CHECK(MoveNames_Text_none == 0);
    CHECK(MoveNamesUppercase_Text_pound == 1);
    CHECK(MoveDescriptions_Text_shadow_force == 467);
    CHECK(MoveNamesUppercase_Text_u_turn == 369);
    CHECK(TEXT_BANK_MOVE_NAMES_ENTRY_COUNT == 468);
    CHECK(TEXT_BANK_MOVE_NAMES_UPPERCASE_ENTRY_COUNT == 468);
    CHECK(TEXT_BANK_MOVE_DESCRIPTIONS_ENTRY_COUNT == 468);
    CHECK(MovesUsedInBattle_Text_none_Ally == 0);
    CHECK(MovesUsedInBattle_Text_pound_Ally == 3);
    CHECK(MovesUsedInBattle_Text_pound_Foe == 5);
    CHECK(MovesUsedInBattle_Text_shadow_force_Foe == 1403);
    CHECK(TEXT_BANK_MOVES_USED_IN_BATTLE_ENTRY_COUNT == 1404);
    /* The legacy badges.h include must expose Metang's original enum IDs. */
    CHECK(BADGE_ID_COAL == 0);
    CHECK(BADGE_ID_BEACON == 7);
    CHECK(MAX_BADGES == 8);
    /* Original speciesproc follows SPECIES_NONE..SPECIES_BAD_EGG order.
     * Names and descriptions are not emitted into our generated headers. */
    CHECK(species_name_none == 0);
    CHECK(species_name_bulbasaur == 1);
    CHECK(species_name_arceus == 493);
    CHECK(species_name_egg == 494);
    CHECK(species_name_bad_egg == 495);
    CHECK(TEXT_BANK_SPECIES_NAME_ENTRY_COUNT == 496);
    CHECK(species_name_with_articles_none == 0);
    CHECK(species_name_with_articles_bulbasaur == 1);
    CHECK(species_name_with_articles_arceus == 493);
    CHECK(species_name_with_articles_bad_egg == 495);
    CHECK(TEXT_BANK_SPECIES_NAME_WITH_ARTICLES_ENTRY_COUNT == 496);
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
    /* Exact original Meson --files-from mappings; indices, not NARCs. */
    CHECK(background_scroll_default_NCLR == 0);
    CHECK(search_button_forms_cell_NCER_lz == 129);
    CHECK(_shared_enc_trainer_NCLR == 0);
    CHECK(player_female_mugshot_anim_NANR == 154);
    CHECK(subscript_start_encounter == 0);
    CHECK(subscript_giratina_form_change == 296);
    test_battle_factory_saved_state();
    test_battle_arcade_saved_state();
    test_easy_chat_sentence();
    test_string_list_copy_entry();
    test_move_reminder_data();
    test_pokedex_heightweight_accessors();
    test_daycare_state();
    test_battle_regulation_rules();
    test_record_mixed_rng();
    test_berry_tag_data();
    test_pokedex_memory_state();
    test_pokedex_data_bank_switch();
    test_ribbon_save_state();
    test_grid_menu_cursor();
    test_overworld_map_history();
    test_pokedex_language();
    test_persisted_map_features();
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
