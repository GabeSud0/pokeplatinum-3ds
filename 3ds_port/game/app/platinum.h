/*
 * The game's start, shared by the 3DS program (main_3ds.c) and the PC
 * runner (main_pc.c): what the DS firmware and the cartridge loader did
 * before Pokémon Platinum's NitroMain, then NitroMain itself.
 *
 * Plain C types only: main_3ds.c includes libctru's headers, which can't be
 * mixed with NitroSDK's. platinum.c is the side that talks to the SDK.
 *
 * A program also provides the port's hooks (see nitro_port.h), with these
 * plain-C equivalents of their SDK types:
 *   void NitroPort_Log(const char *fmt, ...);        every log line
 *   void NitroPort_OnFrame(void);                   once a frame: show it,
 *                                                   read the buttons
 *   void NitroPort_Terminate(void);                 the game stopped itself
 *   void NitroPort_ResetSystem(uint32_t parameter); the game asked to restart
 */

#ifndef PLATINUM_APP_H
#define PLATINUM_APP_H

#include <stdint.h>

/* The data pack's build ID: the builder must be given the same one
 * (python builder/pack.py data_pack/ -o platinum3ds.pak --build-id ...).
 * It names how the pack's contents are laid out, so a program only opens
 * packs it can read. These packs hold the ROM's NitroFS files unchanged. */
#define PLATINUM_PAK_BUILD_ID "platinum-nitrofs-1"

/* The user's settings, as a DS's firmware would hand them to the game.
 * Platinum_DefaultSettings fills in neutral values; a program may replace
 * them (the 3DS one uses the console's profile). */
typedef struct PlatinumSettings {
    uint16_t nickname[11];   /* UTF-16, 0-terminated */
    uint8_t language;        /* 0 Japanese, 1 English, 2 French, 3 German,
                                4 Italian, 5 Spanish */
    uint8_t birthMonth;
    uint8_t birthDay;
    uint8_t favoriteColor;   /* 0-15 */
} PlatinumSettings;

void Platinum_DefaultSettings(PlatinumSettings *settings);

/* Open the data pack and its manifest. Returns NULL, or what's wrong in
 * words. */
const char *Platinum_OpenPak(const char *pakPath, const char *manifestPath);

/* Set up the DS's state as the firmware leaves it, register the overlays,
 * start the ARM7 services, and run the game (NitroMain, which never
 * returns). The data pack must be open. */
void Platinum_Run(const PlatinumSettings *settings) __attribute__((noreturn));

/* A self-check of the overlays in the program (no data pack needed): load
 * every overlay with the game's own overlay manager, as the game does, and
 * check that loading resets its data. Returns the number of failures. */
int Platinum_CheckOverlays(void);

/* The DS's buttons, as bits for Platinum_SetButtons (NitroSDK's PAD_*). */
#define PLATINUM_BUTTON_A       0x0001
#define PLATINUM_BUTTON_B       0x0002
#define PLATINUM_BUTTON_SELECT  0x0004
#define PLATINUM_BUTTON_START   0x0008
#define PLATINUM_KEY_RIGHT      0x0010
#define PLATINUM_KEY_LEFT       0x0020
#define PLATINUM_KEY_UP         0x0040
#define PLATINUM_KEY_DOWN       0x0080
#define PLATINUM_BUTTON_R       0x0100
#define PLATINUM_BUTTON_L       0x0200
#define PLATINUM_BUTTON_X       0x0400
#define PLATINUM_BUTTON_Y       0x0800

/* The buttons held now; call once a frame (from NitroPort_OnFrame). */
void Platinum_SetButtons(uint16_t held);

/* The stylus on the DS's touch screen (0-255, 0-191), if this build has the
 * touch-screen ARM7 service; otherwise ignored. */
void Platinum_SetTouch(int down, int x, int y);

#endif
