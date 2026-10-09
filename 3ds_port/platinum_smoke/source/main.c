/*
 * Native Nintendo 3DS executable that CALLS original Pokémon Platinum C.
 * This is a diagnostics milestone, not the original NitroMain or a game boot.
 *
 * START exits. A/B change real Coin Case state; X/Y exercise Platinum's
 * original LCRNG; L/R navigate its original Pokédex-language table.
 *
 * No ROM assets, graphics, Nintendo DS register access or network calls.
 */
#include <3ds.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "platinum_api.h"

#define INITIAL_SEED 0x12345678u
#define LANGUAGE_COUNT 6

/* Values from pinned constants/versions.h, ordered exactly as the original
 * pokedex_language.c table: Japanese, English, French, German, Italian,
 * Spanish. The actual ID-to-index lookup remains ORIGINAL game code. */
static const int originalLanguageIDs[LANGUAGE_COUNT] = { 1, 2, 3, 5, 4, 7 };

static void print_top(PrintConsole *screen, uint16_t *coins,
                      uint16_t lastRandom, int languageIndex,
                      unsigned actions)
{
    const int originalLanguageID = originalLanguageIDs[languageIndex];
    const int gameIndex = PokedexLanguage_LanguageToIndex(originalLanguageID);

    consoleSelect(screen);
    printf("\x1b[2J\x1b[H");
    printf("PLATINUM / native 3DS smoke test\n");
    printf("================================\n\n");
    printf("This app calls ORIGINAL Platinum\n");
    printf("functions linked from libplatinum_game.a.\n\n");
    printf("COIN CASE\n");
    printf("  Current : %5u / 50000\n", (unsigned)Coins_GetValue(coins));
    printf("  Can +100: %s\n\n", Coins_CanAdd(coins, 100) ? "yes" : "no");
    printf("PLATINUM RANDOM NUMBER GENERATOR\n");
    printf("  Seed    : %08lX\n", (unsigned long)LCRNG_GetSeed());
    printf("  Last RNG: %5u\n\n", (unsigned)lastRandom);
    printf("POKEDEX LANGUAGE LOOKUP\n");
    printf("  Index   : %d / %d\n", languageIndex, LANGUAGE_COUNT - 1);
    printf("  Game ID : %d\n", originalLanguageID);
    printf("  Lookup  : %d\n\n", gameIndex);
    printf("Button actions: %u\n\n", actions);
    printf("Original game loop: NOT YET\n");
    printf("Virtual DS hardware: NOT YET\n");
    printf("ROM resources: NOT LOADED\n");
}

static void print_bottom(PrintConsole *screen)
{
    consoleSelect(screen);
    printf("\x1b[2J\x1b[H");
    printf("   pokeplatinum-3ds / smoke\n\n");
    printf("A       Add 100 coins\n");
    printf("B       Spend 25 coins\n");
    printf("SELECT  Reset Coin Case\n\n");
    printf("X       Advance original RNG\n");
    printf("Y       Reset RNG seed\n\n");
    printf("L / R   Change Pokedex language\n\n");
    printf("START   Exit\n\n");
    printf("No assets or ROM required.\n");
    printf("Not a playable Pokemon game.\n");
}

int main(void)
{
    PrintConsole top;
    PrintConsole bottom;
    uint16_t coins;
    uint16_t lastRandom = 0;
    int languageIndex = 1; /* English is position 1 in Platinum's table. */
    unsigned actions = 0;
    bool dirty = true;

    gfxInitDefault();
    gfxSet3D(false);
    consoleInit(GFX_TOP, &top);
    consoleInit(GFX_BOTTOM, &bottom);

    Coins_Init(&coins);
    LCRNG_SetSeed(INITIAL_SEED);
    print_bottom(&bottom);

    while (aptMainLoop()) {
        hidScanInput();
        const uint32_t pressed = hidKeysDown();

        if (pressed & KEY_START)
            break;
        if (pressed & KEY_A) {
            Coins_Add(&coins, 100);
            dirty = true;
            actions++;
        }
        if (pressed & KEY_B) {
            Coins_Subtract(&coins, 25);
            dirty = true;
            actions++;
        }
        if (pressed & KEY_SELECT) {
            Coins_Init(&coins);
            dirty = true;
            actions++;
        }
        if (pressed & KEY_X) {
            lastRandom = LCRNG_Next();
            dirty = true;
            actions++;
        }
        if (pressed & KEY_Y) {
            LCRNG_SetSeed(INITIAL_SEED);
            lastRandom = 0;
            dirty = true;
            actions++;
        }
        if (pressed & KEY_L) {
            languageIndex = (languageIndex + LANGUAGE_COUNT - 1) % LANGUAGE_COUNT;
            dirty = true;
            actions++;
        }
        if (pressed & KEY_R) {
            languageIndex = (languageIndex + 1) % LANGUAGE_COUNT;
            dirty = true;
            actions++;
        }

        if (dirty) {
            print_top(&top, &coins, lastRandom, languageIndex, actions);
            dirty = false;
        }

        gfxFlushBuffers();
        gfxSwapBuffers();
        gspWaitForVBlank();
    }

    gfxExit();
    return 0;
}
