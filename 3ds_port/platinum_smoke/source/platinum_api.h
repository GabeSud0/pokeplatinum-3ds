#ifndef PLATINUM_SMOKE_ORIGINAL_GAME_API_H
#define PLATINUM_SMOKE_ORIGINAL_GAME_API_H

#include <stdint.h>

/*
 * ABI-identical declarations of functions in the pinned pret/pokeplatinum
 * headers: coins.h, math_util.h and pokedex_language.h.
 *
 * The original game headers are compiled with the Nintendo SDK and a global
 * PCH; they must not be included in a libctru translation unit. Keep this
 * narrow native-to-game boundary in sync with those upstream declarations.
 * These are ORIGINAL game symbols, not simulated gameplay implementations.
 *
 * NitroSDK u16 = uint16_t; u32 = uint32_t; BOOL = int.
 */
extern void Coins_Init(uint16_t *coins);
extern uint16_t Coins_GetValue(uint16_t *coins);
extern int Coins_Add(uint16_t *coins, uint16_t amount);
extern int Coins_Subtract(uint16_t *coins, uint16_t amount);
extern int Coins_CanAdd(uint16_t *coins, uint16_t amount);

extern void LCRNG_SetSeed(uint32_t seed);
extern uint32_t LCRNG_GetSeed(void);
extern uint16_t LCRNG_Next(void);

extern int PokedexLanguage_LanguageToIndex(int language);

#endif
