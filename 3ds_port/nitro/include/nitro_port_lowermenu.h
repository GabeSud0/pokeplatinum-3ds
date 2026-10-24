/*
 * The 3DS remaster's lower screen as a menu (port_lowermenu.c; the plan:
 * the project's lower-screen menu doc). In the field the lower screen is
 * the port's own picture at the 3DS's resolution, 320 x 240: a panel on the
 * left with the DS's lower screen in it at 15/16 of its size, and on the
 * right a column of eight buttons (MAP, POKéMON, BAG, TRAINER, POKéDEX,
 * POKéTCH, SAVE, OPTIONS) over two squares (the registered items).
 *
 * The panel is a canvas 256 x 247 DS pixels, drawn 240 x 232 at
 * (NITRO_LOWERMENU_PANEL_X0, NITRO_LOWERMENU_PANEL_Y0): the DS's 192 rows
 * at canvas rows NITRO_LOWERMENU_DS_ROW0 to + 191, and above and below them
 * the screen's top and bottom rows carried on (the Pokétch's strap); or,
 * tall (NitroPort_LowerMenuSetTall), the lower screen's engine drawn 247
 * rows tall (the 192 and 55 more, its BGs and OBJs going on below the DS's
 * screen), canvas row y its row y.
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
/* Tall: the lines the lower screen's engine is drawn (the canvas's 247, and
 * one more: the pictures go in bands of 4 rows). */
#define NITRO_LOWERMENU_TALL_LINES 248

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
/* (a press, NitroPort_LowerMenuTakePress) the panel tapped while it shows
 * the map */
#define NITRO_LOWERMENU_PANEL_PRESS NITRO_LOWERMENU_COUNT

/* Whether the platform can show it (the 3DS: the remaster's compositor,
 * with the memory for its picture; the PC runner: always); until it says
 * so, the menu is never on, and the game is the DS's (its start menu on the
 * upper screen). */
void NitroPort_LowerMenuSetAvailable(int available);

/* On or off (the game's thread: the field's lower screen). Turning it on
 * the first time reads the start menu's icons from the ROM. On: shown and
 * the start menu (it can be shown, and the game has it on). */
void NitroPort_LowerMenuShow(int on);
int NitroPort_LowerMenuOn(void);

/* The panel tall or not (the game's thread: a screen made for it, with
 * what goes below the DS's 192 rows in its BGs' maps and OBJs; off when the
 * menu goes off). The picture's serial changes with it. Tall: whether the
 * game has it so (the screens draw the panel from what they were given:
 * the lower screen's picture tall or not). */
void NitroPort_LowerMenuSetTall(int tall);
int NitroPort_LowerMenuTall(void);

/* The button the game's cursor is on (-1: none), the buttons that can be
 * used now (a bit each, 1 << NITRO_LOWERMENU_...; the others drawn dimmed
 * and not pressed by a touch; all of them until it's said), and whether
 * the player's a girl (her bag's icon). The game's thread. */
void NitroPort_LowerMenuSelect(int button);
void NitroPort_LowerMenuSetEnabled(uint32_t mask);
void NitroPort_LowerMenuSetFemale(int female);

/* The picture: NITRO_LOWERMENU_W x NITRO_LOWERMENU_H texels, each
 * (r << 24) | (g << 16) | (b << 8) | a, rows from the top; held until
 * NitroPort_LowerMenuUnlockFrame. *serial changes whenever it does. */
const uint32_t *NitroPort_LowerMenuLockFrame(uint32_t *serial);
void NitroPort_LowerMenuUnlockFrame(void);
uint32_t NitroPort_LowerMenuSerial(void);
/* (with the picture held) Whether its panel is drawn in it (the map), not
 * the DS's lower screen for the screens to draw there. */
int NitroPort_LowerMenuFrameHasPanel(void);

/* The screen touched at (x, y) of the 320 x 240 (down: 1 while touched, 0
 * when let go). Returns 1 with the DS's lower screen's (*dsX, *dsY) when
 * the game is to have the touch (it began in the panel), else 0 (on a
 * button: it shows pressed, and letting go on it is a press for
 * NitroPort_LowerMenuTakePress). */
int NitroPort_LowerMenuTouch(int down, int x, int y, int *dsX, int *dsY);

/* The button pressed (released on it) and not taken yet, or -1; a press
 * not taken within NITRO_LOWERMENU_PRESS_FRAMES of the menu's frames
 * (NitroPort_LowerMenuTouch is told every frame) is dropped: one the game
 * couldn't use then (a script running) isn't used much later. Half a
 * second: the field takes it when the player's step ends. Take: the press
 * is the caller's; Pending: it stays. */
#define NITRO_LOWERMENU_PRESS_FRAMES 30
int NitroPort_LowerMenuTakePress(void);
int NitroPort_LowerMenuPressPending(void);

/* Which button (x, y) is on, or -1; and whether it's in the panel (with
 * the DS's lower screen's pixel there: tall, the canvas's; the game's touch
 * is the DS's, so a row past 191 is 191). */
int NitroPort_LowerMenuButtonAt(int x, int y);
int NitroPort_LowerMenuPanelAt(int x, int y, int *dsX, int *dsY);

/* ------------------------------------------------------------- the map
 *
 * The panel can show the Town Map instead of the DS's lower screen (which
 * goes on underneath, the Pokétch running; its touches the menu's then): the
 * Town Map app's upper screen (its map, the towns' blocks, the cursor with
 * the player's head on it, the places been through before blinking in
 * turn, the place's name in its bar) and under it, the canvas's last 55
 * rows, the place's description as its guide page shows it (the guide
 * page's rows 61-115: the description over the zoomed-in map). Drawn by the
 * port (port_lowermap.c) from the ROM's pictures (graphic/tmap_gra.narc),
 * laid out as the game tells it: where its sprites are (the Town Map's own
 * positions), which hidden places show, the zoomed-in map's scroll, and the
 * name and the description as the game printed them. Animated as the Town
 * Map's: the cursor's two frames, the dots one at a time. */
enum NitroLowerMenuPanelShows {
    NITRO_LOWERMENU_PANEL_DS,
    NITRO_LOWERMENU_PANEL_MAP,
};
/* What the panel shows (the game's thread; the map only once it has been
 * given: NitroPort_LowerMenuSetMap). */
void NitroPort_LowerMenuSetPanel(int shows);
int NitroPort_LowerMenuPanel(void);

enum NitroLowerMapSpriteKind {
    NITRO_LOWERMAP_CURSOR,   /* 16 x 16, its two frames by turns */
    NITRO_LOWERMAP_PLAYER,   /* 16 x 16; frame 0 the boy, 1 the girl */
    NITRO_LOWERMAP_BLOCK,    /* a town's block, 16 x 16; frame: its shape (0-6) */
};
typedef struct NitroLowerMapSprite {
    short x, y;              /* its middle, on the upper screen (the Town Map's sprite position) */
    uint8_t kind, frame;
    uint8_t palette;         /* the OBJ palette (top_screen_sprites_NCLR's) */
    uint8_t pad;
} NitroLowerMapSprite;
#define NITRO_LOWERMAP_MAX_SPRITES 24
#define NITRO_LOWERMAP_MAX_DOTS 8
typedef struct NitroLowerMap {
    /* the sprites, the front one first (the Town Map's OAM order) */
    int sprites;
    NitroLowerMapSprite sprite[NITRO_LOWERMAP_MAX_SPRITES];
    /* the places been through before (a dot each, 8 x 8, palette 2, drawn
     * behind the cursor and the head, in front of the blocks), shown one
     * at a time from the last, every 26 frames */
    int dots;
    struct { short x, y; } dot[NITRO_LOWERMAP_MAX_DOTS];
    uint16_t hidden;         /* the hidden places shown (a bit each, the game's HIDDEN_LOCATION_*) */
    short zoomX, zoomY;      /* the guide page's zoomed-in map's scroll */
    /* The place's name (its window on the upper screen) and description
     * (its window on the guide page), as the game printed them: each pixel
     * its colour (0x8000 | RGB555) or 0 (nothing there), width x height,
     * the window's top-left at (x, y) on its screen. NULL: none. */
    const uint16_t *name;
    short nameX, nameY, nameW, nameH;
    const uint16_t *desc;
    short descX, descY, descW, descH;
} NitroLowerMap;
/* The map's contents (the game's thread; copied). */
void NitroPort_LowerMenuSetMap(const NitroLowerMap *map);
/* The same in plain calls (the game's code: no port headers there): begun,
 * its parts in turn (the sprites front first), then handed over. Text: 0 the
 * name, 1 the description, its window at (x, y), w x h pixels: the buffer
 * for the game to fill (w x h, each pixel as NitroLowerMap's text), the
 * port's (no room on the game's heaps), or NULL if too big (the name's up
 * to 256 x 64, the description's 256 x 192). Each text stays as last given
 * from one map to the next (the game prints it again only when it
 * changes) until new text, or none (MapNoText). */
void NitroPort_LowerMenuMapBegin(void);
void NitroPort_LowerMenuMapSprite(int kind, int x, int y, int frame, int palette);
void NitroPort_LowerMenuMapDot(int x, int y);
void NitroPort_LowerMenuMapLayout(int hidden, int zoomX, int zoomY);
uint16_t *NitroPort_LowerMenuMapText(int which, int x, int y, int w, int h);
void NitroPort_LowerMenuMapNoText(int which);
void NitroPort_LowerMenuMapEnd(void);

#ifdef __cplusplus
}
#endif

#endif
