/*
 * The 3DS remaster's lower screen as a menu (port_lowermenu.c; the plan:
 * the project's lower-screen menu doc). In the field the lower screen is
 * the port's own picture at the 3DS's resolution, 320 x 240: a panel on the
 * left with the DS's lower screen in it at 15/16 of its size, and on the
 * right a column of eight buttons (MAP, POKéMON, BAG, TRAINER, POKéDEX,
 * POKéTCH, SAVE, OPTIONS) over two squares (the registered items).
 *
 * The panel is a canvas 256 x 247 DS pixels (the DS's 192 rows, at canvas
 * rows NITRO_LOWERMENU_DS_ROW0 to + 191, and above and below them the
 * screen's top and bottom rows carried on: the Pokétch's strap), drawn
 * 240 x 232 at (NITRO_LOWERMENU_PANEL_X0, NITRO_LOWERMENU_PANEL_Y0).
 *
 * The decompilation turns it on and off with the field's lower screen
 * (patches/pokeplatinum: the Pokétch's handlers); the screens are drawn by
 * the 3DS compositor (app/port_compose_pica.c) or the PC runner's mshot.
 * Plain C, no SDK headers: the 3DS program includes it next to libctru.
 */
#ifndef NITRO_PORT_LOWERMENU_H
#define NITRO_PORT_LOWERMENU_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NITRO_LOWERMENU_W 320
#define NITRO_LOWERMENU_H 240

/* The panel: where the DS's lower screen is drawn. */
#define NITRO_LOWERMENU_PANEL_X0 3
#define NITRO_LOWERMENU_PANEL_Y0 4
#define NITRO_LOWERMENU_PANEL_W 240
#define NITRO_LOWERMENU_PANEL_H 232
#define NITRO_LOWERMENU_CANVAS_W 256
#define NITRO_LOWERMENU_CANVAS_H 247
/* The canvas row the DS's row 0 is at (27 rows of its top row above it,
 * 28 of its bottom row below). */
#define NITRO_LOWERMENU_DS_ROW0 27

/* The buttons, top to bottom, then the two squares. */
enum NitroLowerMenuButton {
    NITRO_LOWERMENU_MAP,
    NITRO_LOWERMENU_POKEMON,
    NITRO_LOWERMENU_BAG,
    NITRO_LOWERMENU_TRAINER,
    NITRO_LOWERMENU_POKEDEX,
    NITRO_LOWERMENU_POKETCH,
    NITRO_LOWERMENU_SAVE,
    NITRO_LOWERMENU_OPTIONS,
    NITRO_LOWERMENU_ITEM1,
    NITRO_LOWERMENU_ITEM2,
    NITRO_LOWERMENU_COUNT
};

/* On or off (the game's thread: the field's lower screen). Turning it on
 * the first time reads the start menu's icons from the ROM. */
void NitroPort_LowerMenuShow(int on);
int NitroPort_LowerMenuOn(void);

/* The button the game's cursor is on (-1: none), and whether the player's
 * a girl (her bag's icon). The game's thread. */
void NitroPort_LowerMenuSelect(int button);
void NitroPort_LowerMenuSetFemale(int female);

/* The picture: NITRO_LOWERMENU_W x NITRO_LOWERMENU_H texels, each
 * (r << 24) | (g << 16) | (b << 8) | a, rows from the top; held until
 * NitroPort_LowerMenuUnlockFrame. *serial changes whenever it does. */
const uint32_t *NitroPort_LowerMenuLockFrame(uint32_t *serial);
void NitroPort_LowerMenuUnlockFrame(void);
uint32_t NitroPort_LowerMenuSerial(void);

/* The screen touched at (x, y) of the 320 x 240 (down: 1 while touched, 0
 * when let go). Returns 1 with the DS's lower screen's (*dsX, *dsY) when
 * the game is to have the touch (it began in the panel), else 0 (on a
 * button: it shows pressed, and letting go on it is a press for
 * NitroPort_LowerMenuTakePress). */
int NitroPort_LowerMenuTouch(int down, int x, int y, int *dsX, int *dsY);

/* The button pressed since the last call (released on it), or -1. */
int NitroPort_LowerMenuTakePress(void);

/* Which button (x, y) is on, or -1; and whether it's in the panel (with
 * the DS's lower screen's pixel there, when it's one of its 192 rows). */
int NitroPort_LowerMenuButtonAt(int x, int y);
int NitroPort_LowerMenuPanelAt(int x, int y, int *dsX, int *dsY);

#ifdef __cplusplus
}
#endif

#endif
