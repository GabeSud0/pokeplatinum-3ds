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
 * the map (NitroPort_LowerMenuSetPanel) */
#define NITRO_LOWERMENU_PANEL_PRESS NITRO_LOWERMENU_COUNT

/* Whether the platform can show it (the 3DS: the remaster's compositor,
 * with the memory for its picture; the PC runner: always); until it says
 * so, the menu is never on, and the game is the DS's (its start menu on the
 * upper screen). */
void NitroPort_LowerMenuSetAvailable(int available);
int NitroPort_LowerMenuAvailable(void);

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

/* An app shown in the panel (the game's thread: the bag, laid out tall for
 * it, the DS's main engine put on the lower screen).
 *   PanelTouch: whether a touch in the panel is the game's (1, as on the
 *     DS: the Pokétch) or nobody's (0: an app whose touch screen isn't
 *     shown, its main screen in the panel instead; its touch buttons would
 *     be pressed where they aren't shown). Back to 1 when the menu goes
 *     off.
 *   HoldUpper: the upper screen kept as it is (1: what it shows now stays
 *     there, the game's picture for it not shown: the field, while an app
 *     is in the panel); or let go (0): it shows the game's picture again
 *     once that's as bright as what was kept (kept at full brightness, the
 *     field: once it's back and faded in; kept black, after a fade: as soon
 *     as the game's picture is black too, so a fade in shows whole); or
 *     let go now (NITRO_LOWERMENU_LET_GO_NOW: the game's picture from the
 *     next frame, however bright: back without a fade, the field as it
 *     was). UpperHeld: the game's wish (> 0 kept, 0 let go, < 0 let go
 *     now; the platforms keep the picture). */
#define NITRO_LOWERMENU_LET_GO_NOW (-1)
/* (HoldUpper) Kept, showing the field's picture: the one the platform kept
 * last as the upper screen began to be kept at full brightness (the field,
 * a panel app opening from it), since the game last forgot it
 * (ForgetField: walking again). For an app in the panel after one that
 * took both screens (an evolution, the mail): the field above it, not the
 * black that one's fade out left. None kept: as 1. FieldSerial: the
 * forgetting's count (the platforms keep the picture with it). */
#define NITRO_LOWERMENU_HOLD_FIELD 2
void NitroPort_LowerMenuSetPanelTouch(int on);
void NitroPort_LowerMenuHoldUpper(int hold);
int NitroPort_LowerMenuUpperHeld(void);
void NitroPort_LowerMenuForgetField(void);
uint32_t NitroPort_LowerMenuFieldSerial(void);
/* (the platforms, the upper screen kept and let go now) Whether the game's
 * picture made at stamp (the clock, SetClock's, then) is one to show: made
 * since the game let go, or half a second on (the 3DS draws the game's
 * pictures a frame or more late: what's newest may be from before, the
 * app's hidden screen). No clock: yes. */
int NitroPort_LowerMenuUpperLetGoShows(uint32_t stamp);
/* (the platforms) A screen's master brightness (the MASTER_BRIGHT
 * register's value) as -16 (black) .. 16 (white), 0 as it is. */
int NitroPort_LowerMenuBrightness(unsigned masterBright);

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
/* (with the picture held; the display's) The rows changed since it last
 * asked: y0 to y1 - 1 (none: y0 >= y1). Taking a copy, it need only take
 * those again. The panel's left for the platforms to draw over: the DS's
 * lower screen, or the map's picture (below). */
void NitroPort_LowerMenuTakeDirtyRows(int *y0, int *y1);

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
/* Whether the press (pending, or taken last) was the second of two on the
 * same button within NITRO_LOWERMENU_DOUBLE_FRAMES of the menu's frames: a
 * double tap (MAP's opens the Town Map app). */
int NitroPort_LowerMenuPressDouble(void);
#define NITRO_LOWERMENU_DOUBLE_FRAMES 30

/* Which button (x, y) is on, or -1; and whether it's in the panel (with
 * the DS's lower screen's pixel there: tall, the canvas's; the game's touch
 * is the DS's, so a row past 191 is 191). */
int NitroPort_LowerMenuButtonAt(int x, int y);
int NitroPort_LowerMenuPanelAt(int x, int y, int *dsX, int *dsY);

/* ------------------------------------------------------------- the map
 *
 * The panel can show the Town Map instead of the DS's lower screen (which
 * goes on underneath, the Pokétch running; its touches the menu's then; the
 * platforms needn't draw it meanwhile): the
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
/* What the panel shows (the game's thread; the map only once the Town Map's
 * pictures could be read: NitroPort_LowerMenuMapAvailable). The map: the
 * panel's touches are the menu's (a tap a press,
 * NITRO_LOWERMENU_PANEL_PRESS). */
void NitroPort_LowerMenuSetPanel(int shows);
int NitroPort_LowerMenuPanel(void);

/* ------------------------------------------------------ the panel's fade
 *
 * The panel faded to black and back as the game changes what it shows (the
 * map, the Pokétch, the bag: the loading in between hidden), the menu's
 * frame and buttons as they are. FadePanel (the game's thread): towards
 * black (1) or clear (0) from how dark it is now, at a whole fade's speed
 * of NITRO_LOWERMENU_FADE_FRAMES of the platform's frames; or clear at once
 * (NITRO_LOWERMENU_CLEAR_NOW: an app popping in). Fading in to the map, the
 * fade waits (black) till the map's drawn as the game last gave it, half a
 * second at most; with no map drawn yet, the map's panel is black.
 * PanelFade (any thread): how dark it is now, 0 (clear) to 16 (black): the
 * platforms draw black over the panel by as much. SetClock: the platform's
 * frame count (its V-blanks: the 3DS's compositor's, the PC runner's frames)
 * for the fades; without one, a fade's done at once. */
#define NITRO_LOWERMENU_FADE_FRAMES 8
#define NITRO_LOWERMENU_CLEAR_NOW (-1)
void NitroPort_LowerMenuFadePanel(int black);
int NitroPort_LowerMenuPanelFade(void);
void NitroPort_LowerMenuSetClock(uint32_t (*now)(void));
/* (the platform, as it draws the panel showing the DS's lower screen) The
 * clock (SetClock's) when the picture it draws there was made. Fading in to
 * the DS's lower screen, or clearing at once (an app popping in), the panel
 * stays black till there's one made since (half a second at most): no frame
 * of what was there before the app was whole (the 3DS draws the game's
 * pictures a frame or more late). A platform that never says: no wait. */
void NitroPort_LowerMenuPanelShown(uint32_t stamp);
/* What the panel shows, changed with the fade (the game's thread: a tab
 * chosen): it fades to black, the panel then shows it and fades back in (the
 * DS's lower screen a few frames on: its picture drawn first), on the
 * platform's frames whatever the game does meanwhile. What it shows (or will)
 * already: nothing. Meanwhile SetPanel changes what it will show, fading in
 * waits for it, fading to black (or clear at once) calls it off. */
void NitroPort_LowerMenuFadeToPanel(int shows);

/* The menu kept on the lower screen while the game swaps what's under it
 * (the field going, an app coming, and back) and turns the menu off and on
 * meanwhile, its panel black (faded: what the DS's lower screen shows then
 * isn't anything) or the map; its buttons drawn as the game says, their
 * touches nobody's while the menu's off. KeptShown: whether the platforms
 * draw it so now. */
void NitroPort_LowerMenuKeepShown(int on);
int NitroPort_LowerMenuKeptShown(void);

/* ------------------------------------------------- the panel put together
 *
 * An app whose two DS screens both go in the panel (the trainer case: its
 * card on the upper screen, its case on the lower) has the canvas put
 * together from runs of rows: each the DS's lower screen's rows (as tall as
 * the game draws it), the upper screen's (its DS 256 columns), or the
 * port's tabs; a row after the other (step 1), upside down (-1) or the same
 * row again (0); the canvas's columns 0 and 255 painted one colour (sides),
 * 1 and 254 another (sidesIn), where the layout wants an edge the DS's rows
 * haven't.
 * Rows no run covers: as the DS's lower screen is usually shown. Touches:
 * on the lower screen's rows the game's (as PanelTouch says), at the row
 * they show; on the upper screen's nobody's; on the tabs a press,
 * NITRO_LOWERMENU_TAB_PRESS(tab), when let go. None (count 0, and when the
 * menu goes off): the panel as usual. Runs: the platforms' copy and its
 * serial. */
enum NitroLowerPanelFrom {
    NITRO_LOWERMENU_FROM_LOWER,
    NITRO_LOWERMENU_FROM_UPPER,
    NITRO_LOWERMENU_FROM_TABS,
};
typedef struct NitroLowerPanelRun {
    short y, rows;           /* the canvas's rows */
    short from;              /* NITRO_LOWERMENU_FROM_* */
    short srcY;              /* the source's row for the first */
    short step;              /* 1, -1, 0 */
    unsigned short sides;    /* columns 0 and 255: 0x8000 | RGB555, or 0 */
    unsigned short sidesIn;  /* columns 1 and 254: the same */
} NitroLowerPanelRun;
#define NITRO_LOWERMENU_SIDE(r, g, b) ((unsigned short)(0x8000u | ((b) >> 3) << 10 | ((g) >> 3) << 5 | ((r) >> 3)))
#define NITRO_LOWERMENU_RUNS_MAX 12
void NitroPort_LowerMenuSetPanelRuns(const NitroLowerPanelRun *runs, int count);
int NitroPort_LowerMenuPanelRuns(NitroLowerPanelRun *out, uint32_t *serial);
/* Some of the runs are the upper screen's (its drawer draws it at the DS's
 * width then: no wider layout). */
int NitroPort_LowerMenuPanelShowsUpper(void);
/* Where a canvas row comes from (the runs; or as usual: from the lower
 * screen's row, which may be past its 192 when tall, or the map's): from
 * (NITRO_LOWERMENU_FROM_*) and the source's row; -1, no run there. */
int NitroPort_LowerMenuRunRow(const NitroLowerPanelRun *runs, int count, int canvasY, int *srcY);

/* The tabs (the runs' NITRO_LOWERMENU_FROM_TABS rows): drawn by the port in
 * its picture, at the 3DS's pixels, in its own font, as folder tabs on the
 * top of what's under them: count of them side by side (4 pixels apart),
 * active the one open, each a small ball and its label (capitals); below
 * the tabs, in the run's last rows, the top of what they're the tabs of
 * (fill, its edge at the sides, its top line under the ones not open).
 * Colours (RGB888): what's round them, the open one's fill, the others',
 * the edge, the lettering and its shadow (the others': the shadow's colour
 * and inkOff's). Labels: up to 15 characters. */
#define NITRO_LOWERMENU_TABS_MAX 4
typedef struct NitroLowerTabStyle {
    uint32_t back, fill, fillOff, edge, ink, inkShadow, inkOff;
} NitroLowerTabStyle;
void NitroPort_LowerMenuSetTabs(int count, const char *const *labels, int active, const NitroLowerTabStyle *style);
#define NITRO_LOWERMENU_TAB_PRESS(tab) (NITRO_LOWERMENU_PANEL_PRESS + 1 + (tab))

/* Buttons in the panel, the game's (the save's YES and NO: its picture
 * tall, past the DS's 192 rows the game's touch can't reach): rectangles
 * in the canvas's pixels (x0, y0, x1, y1, inclusive), a press each,
 * NITRO_LOWERMENU_BUTTON_PRESS(button), when a touch that began on it is
 * let go on it (also while the panel's other touches are nobody's); held
 * on it: NitroPort_LowerMenuPanelButtonHeld. None (count 0, and when the
 * menu goes off): no buttons. */
#define NITRO_LOWERMENU_BUTTONS_MAX 4
void NitroPort_LowerMenuSetPanelButtons(const short (*rects)[4], int count);
int NitroPort_LowerMenuPanelButtonHeld(void);
#define NITRO_LOWERMENU_BUTTON_PRESS(button) (NITRO_LOWERMENU_TAB_PRESS(NITRO_LOWERMENU_TABS_MAX) + (button))

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
/* The map's contents, drawn at once on the calling thread (the tests;
 * not with a map worker: NitroPort_LowerMenuSetMapWorker). Copied. */
void NitroPort_LowerMenuSetMap(const NitroLowerMap *map);
/* (the game's thread) Whether the port can show the map: the Town Map's
 * pictures read from the data pack (the first time it's asked). */
int NitroPort_LowerMenuMapAvailable(void);
/* The map in plain calls (the game's code: no port headers there), the
 * game's thread: begun, its parts in turn (the sprites front first), then
 * handed over (End) to the map's drawer. Text: 0 the name, 1 the
 * description, its window at (x, y) as the game's window holds it: wTiles x
 * hTiles tiles of 8 x 8, 4 bits a pixel, one after another (rows of tiles),
 * and its 16 colours (RGB555; colour 0: nothing there); copied (the name's
 * up to 32 x 8 tiles, the description's 32 x 24; bigger: none). Each text
 * stays as last given from one map to the next (the game prints it again
 * only when it changes) until new text, or none (MapNoText). */
void NitroPort_LowerMenuMapBegin(void);
void NitroPort_LowerMenuMapSprite(int kind, int x, int y, int frame, int palette);
void NitroPort_LowerMenuMapDot(int x, int y);
void NitroPort_LowerMenuMapLayout(int hidden, int zoomX, int zoomY);
void NitroPort_LowerMenuMapText(int which, int x, int y, int wTiles, int hTiles, const void *tiles,
                                const uint16_t *palette, uint32_t key);
void NitroPort_LowerMenuMapNoText(int which);
/* The texts given are kept (the last NITRO_LOWERMENU_TEXTS_KEPT of each,
 * under the game's key for what was printed: the place, the description
 * and its checks): TextKept makes the one kept under key this map's text, as
 * MapText would, and says whether there was one (the game prints it only
 * if not: walking back and forth between two places prints nothing). */
#define NITRO_LOWERMENU_TEXTS_KEPT 8
int NitroPort_LowerMenuMapTextKept(int which, uint32_t key);
void NitroPort_LowerMenuMapEnd(void);

/* The map's drawer: what the game hands over (the player on a new square, a
 * new place) and the sprites' animations are drawn on a thread of the
 * platform's, off the game's (the 3DS: a low priority, on a core with time
 * to spare), so walking costs the game nothing (it never waits for the
 * drawer: a map given while the drawer's taking the last one is handed over
 * the next frame). The platform starts it and
 * gives SetMapWorker the function that wakes it; woken, it calls MapWork
 * (which draws what's new since it last did: the newest the game gave, the
 * animations' frame now). Without one, the game's thread draws as it hands
 * over. MapWork: 1 if it drew something. */
void NitroPort_LowerMenuSetMapWorker(void (*wake)(void));
int NitroPort_LowerMenuMapWork(void);

/* (the platforms) The map's picture, drawn over the panel when it shows the
 * map: NITRO_LOWERMENU_PANEL_W x NITRO_LOWERMENU_PANEL_H texels as the
 * menu's picture's, held till UnlockMap (always called); NULL while
 * there's none yet. *serial changes whenever it does. TakeMapDirtyRows (held):
 * as TakeDirtyRows, the map's. */
const uint32_t *NitroPort_LowerMenuLockMap(uint32_t *serial);
/* The same if the map's drawer hasn't the lock (0 if it has: nothing held;
 * the display, which mustn't wait on it, takes it next time). */
int NitroPort_LowerMenuTryLockMap(const uint32_t **map, uint32_t *serial);
void NitroPort_LowerMenuTakeMapDirtyRows(int *y0, int *y1);
void NitroPort_LowerMenuUnlockMap(void);
uint32_t NitroPort_LowerMenuMapSerial(void);

#ifdef __cplusplus
}
#endif

#endif
