/*
 * Tests for the lower screen as a menu (src/port_lowermenu.c): where a
 * touch goes (the panel's DS pixel, or a button), a press only when let go
 * on the button it began on and only on one that can be used now, a press
 * not taken soon dropped, the picture redrawn when the cursor moves, and
 * nothing while it's off; double taps, the panel's fade, the menu kept
 * shown while the game swaps what's under it, the map handed over. (No data
 * pack: the start menu's icons and the Town Map are left out, the rest
 * drawn.)
 */
#include <nitro_port_lowermenu.h>
#include <stdio.h>

static int sChecks, sFailures;
#define CHECK(cond) do { sChecks++; if (!(cond)) { sFailures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static uint32_t pixel(int x, int y)
{
    uint32_t c;
    const uint32_t *p = NitroPort_LowerMenuLockFrame(NULL);

    c = p[y * NITRO_LOWERMENU_W + x];
    NitroPort_LowerMenuUnlockFrame();
    return c;
}

static void test_panel_mapping(void)
{
    int x = -1, y = -1;

    /* The panel's top left: the canvas's (0, 0), the DS's top row (the
     * canvas's first 27 rows are its top row carried on). */
    CHECK(NitroPort_LowerMenuPanelAt(3, 4, &x, &y) == 1);
    CHECK(x == 0 && y == 0);
    /* Each screen pixel's middle: the DS's row 0 starts at canvas row 27,
     * at y 4 + 27 * 232 / 247 = 29.4, so pixel row 29's middle (29.5) is in
     * it and row 28's isn't. */
    CHECK(NitroPort_LowerMenuPanelAt(3, 28, &x, &y) == 1 && y == 0);   /* (the strap: the top row) */
    CHECK(NitroPort_LowerMenuPanelAt(3, 29, &x, &y) == 1 && y == 0);
    CHECK(NitroPort_LowerMenuPanelAt(3, 30, &x, &y) == 1 && y == 1);
    /* The middle: (123, 120)'s middle is canvas (128.5, 124.0), the DS's (128, 97). */
    CHECK(NitroPort_LowerMenuPanelAt(123, 120, &x, &y) == 1);
    CHECK(x == 128 && y == 97);
    /* The bottom right: the DS's last column and bottom row. */
    CHECK(NitroPort_LowerMenuPanelAt(242, 235, &x, &y) == 1);
    CHECK(x == 255 && y == 191);
    /* Outside it: the frame, the column. */
    CHECK(NitroPort_LowerMenuPanelAt(2, 100, &x, &y) == 0);
    CHECK(NitroPort_LowerMenuPanelAt(243, 100, &x, &y) == 0);
    CHECK(NitroPort_LowerMenuPanelAt(100, 236, &x, &y) == 0);
    CHECK(NitroPort_LowerMenuPanelAt(280, 10, &x, &y) == 0);
}

static void test_tall_mapping(void)
{
    int x = -1, y = -1;
    uint32_t before = NitroPort_LowerMenuSerial();

    /* Tall: the canvas's row is the DS's (no strap above); a row past 191
     * is 191 for the game's touch. */
    NitroPort_LowerMenuSetTall(1);
    CHECK(NitroPort_LowerMenuTall() == 1);
    CHECK(NitroPort_LowerMenuSerial() != before);
    CHECK(NitroPort_LowerMenuPanelAt(3, 4, &x, &y) == 1 && x == 0 && y == 0);
    CHECK(NitroPort_LowerMenuPanelAt(3, 28, &x, &y) == 1 && y == 26);   /* (pixel row 28's middle: 24.5 * 247 / 232 = 26.1) */
    CHECK(NitroPort_LowerMenuPanelAt(123, 120, &x, &y) == 1 && x == 128 && y == 124);
    CHECK(NitroPort_LowerMenuPanelAt(242, 235, &x, &y) == 1 && y == 191);
    /* The same again: no change. */
    before = NitroPort_LowerMenuSerial();
    NitroPort_LowerMenuSetTall(1);
    CHECK(NitroPort_LowerMenuSerial() == before);
    /* Not tall: the strap again. */
    NitroPort_LowerMenuSetTall(0);
    CHECK(NitroPort_LowerMenuTall() == 0);
    CHECK(NitroPort_LowerMenuPanelAt(123, 120, &x, &y) == 1 && y == 97);
    /* The menu going off: not tall any more. */
    NitroPort_LowerMenuShow(1);
    NitroPort_LowerMenuSetTall(1);
    NitroPort_LowerMenuShow(0);
    CHECK(NitroPort_LowerMenuTall() == 0);
}

static void test_buttons(void)
{
    /* The column's eight buttons: rows 3 + 23 i to + 21, x 250-316. */
    CHECK(NitroPort_LowerMenuButtonAt(280, 3) == NITRO_LOWERMENU_MAP);
    CHECK(NitroPort_LowerMenuButtonAt(250, 24) == NITRO_LOWERMENU_MAP);
    CHECK(NitroPort_LowerMenuButtonAt(280, 25) == -1);   /* (between two) */
    CHECK(NitroPort_LowerMenuButtonAt(316, 26) == NITRO_LOWERMENU_POKEMON);
    CHECK(NitroPort_LowerMenuButtonAt(280, 3 + 23 * 7 + 10) == NITRO_LOWERMENU_OPTIONS);
    CHECK(NitroPort_LowerMenuButtonAt(249, 10) == -1);
    CHECK(NitroPort_LowerMenuButtonAt(317, 10) == -1);
    /* The two squares under them. */
    CHECK(NitroPort_LowerMenuButtonAt(260, 210) == NITRO_LOWERMENU_ITEM1);
    CHECK(NitroPort_LowerMenuButtonAt(300, 210) == NITRO_LOWERMENU_ITEM2);
    CHECK(NitroPort_LowerMenuButtonAt(283, 210) == -1);
    CHECK(NitroPort_LowerMenuButtonAt(100, 100) == -1);
}

static void test_touches(void)
{
    int x = -1, y = -1;
    const int bagY = 3 + 23 * NITRO_LOWERMENU_BAG + 10;

    /* The platform can't show it: never on. */
    NitroPort_LowerMenuSetAvailable(0);
    NitroPort_LowerMenuShow(1);
    CHECK(NitroPort_LowerMenuOn() == 0);
    NitroPort_LowerMenuShow(0);
    NitroPort_LowerMenuSetAvailable(1);

    /* Off: nothing is the menu's. */
    NitroPort_LowerMenuShow(0);
    CHECK(NitroPort_LowerMenuOn() == 0);
    CHECK(NitroPort_LowerMenuTouch(1, 280, bagY, &x, &y) == 0);
    NitroPort_LowerMenuTouch(0, 0, 0, NULL, NULL);
    CHECK(NitroPort_LowerMenuTakePress() == -1);

    /* On with a finger already down (from the screen before): nobody's
     * touch until it's let go, not a press. */
    NitroPort_LowerMenuTouch(1, 280, bagY, &x, &y);
    NitroPort_LowerMenuShow(1);
    CHECK(NitroPort_LowerMenuOn() == 1);
    CHECK(NitroPort_LowerMenuTouch(1, 280, bagY, &x, &y) == 0);
    CHECK(NitroPort_LowerMenuTouch(1, 123, 120, &x, &y) == 0);
    NitroPort_LowerMenuTouch(0, 0, 0, NULL, NULL);
    CHECK(NitroPort_LowerMenuTakePress() == -1);
    /* A button: pressed while held, a press when let go on it. */
    CHECK(NitroPort_LowerMenuTouch(1, 280, bagY, &x, &y) == 0);
    CHECK(NitroPort_LowerMenuTakePress() == -1);
    CHECK(NitroPort_LowerMenuTouch(1, 285, bagY + 2, &x, &y) == 0);
    CHECK(NitroPort_LowerMenuTouch(0, 0, 0, NULL, NULL) == 0);
    CHECK(NitroPort_LowerMenuTakePress() == NITRO_LOWERMENU_BAG);
    CHECK(NitroPort_LowerMenuTakePress() == -1);
    /* Slid off it before letting go: no press. */
    NitroPort_LowerMenuTouch(1, 280, bagY, &x, &y);
    NitroPort_LowerMenuTouch(1, 280, bagY + 23, &x, &y);
    NitroPort_LowerMenuTouch(0, 0, 0, NULL, NULL);
    CHECK(NitroPort_LowerMenuTakePress() == -1);
    /* Begun in the panel: the game's all the way, even dragged onto the
     * buttons (the panel's nearest pixel); no press. */
    CHECK(NitroPort_LowerMenuTouch(1, 123, 120, &x, &y) == 1);
    CHECK(x == 128 && y == 97);
    CHECK(NitroPort_LowerMenuTouch(1, 280, bagY, &x, &y) == 1);
    CHECK(x == 255);
    NitroPort_LowerMenuTouch(0, 0, 0, NULL, NULL);
    CHECK(NitroPort_LowerMenuTakePress() == -1);
    /* Begun on the frame off the buttons: nothing. */
    CHECK(NitroPort_LowerMenuTouch(1, 245, 100, &x, &y) == 0);
    NitroPort_LowerMenuTouch(0, 0, 0, NULL, NULL);
    CHECK(NitroPort_LowerMenuTakePress() == -1);
}

/* An app shown in the panel (the bag): the panel's touches nobody's, the
 * upper screen held. */
static void test_app_in_panel(void)
{
    int x = -1, y = -1;
    const int bagY = 3 + 23 * NITRO_LOWERMENU_BAG + 10;

    CHECK(NitroPort_LowerMenuAvailable() == 1);
    NitroPort_LowerMenuShow(1);
    NitroPort_LowerMenuTouch(0, 0, 0, NULL, NULL);   /* (no finger down) */
    NitroPort_LowerMenuSetTall(1);
    NitroPort_LowerMenuSetPanelTouch(0);
    /* A touch in the panel: nobody's, all the way (dragged onto a button
     * too); no press. */
    CHECK(NitroPort_LowerMenuTouch(1, 123, 120, &x, &y) == 0);
    CHECK(NitroPort_LowerMenuTouch(1, 280, bagY, &x, &y) == 0);
    NitroPort_LowerMenuTouch(0, 0, 0, NULL, NULL);
    CHECK(NitroPort_LowerMenuTakePress() == -1);
    /* The buttons still the menu's. */
    NitroPort_LowerMenuTouch(1, 280, bagY, &x, &y);
    NitroPort_LowerMenuTouch(0, 0, 0, NULL, NULL);
    CHECK(NitroPort_LowerMenuTakePress() == NITRO_LOWERMENU_BAG);
    /* The menu off: the panel's touches the game's again. */
    NitroPort_LowerMenuShow(0);
    NitroPort_LowerMenuShow(1);
    NitroPort_LowerMenuTouch(0, 0, 0, NULL, NULL);
    CHECK(NitroPort_LowerMenuTouch(1, 123, 120, &x, &y) == 1);
    NitroPort_LowerMenuTouch(0, 0, 0, NULL, NULL);
    NitroPort_LowerMenuShow(0);

    /* The upper screen held: the game's wish, until let go. */
    CHECK(NitroPort_LowerMenuUpperHeld() == 0);
    NitroPort_LowerMenuHoldUpper(1);
    CHECK(NitroPort_LowerMenuUpperHeld() == 1);
    NitroPort_LowerMenuHoldUpper(1);
    CHECK(NitroPort_LowerMenuUpperHeld() == 1);
    NitroPort_LowerMenuHoldUpper(0);
    CHECK(NitroPort_LowerMenuUpperHeld() == 0);
    NitroPort_LowerMenuHoldUpper(1);
    NitroPort_LowerMenuHoldUpper(NITRO_LOWERMENU_LET_GO_NOW);
    CHECK(NitroPort_LowerMenuUpperHeld() == NITRO_LOWERMENU_LET_GO_NOW);
    NitroPort_LowerMenuHoldUpper(0);
    CHECK(NitroPort_LowerMenuUpperHeld() == 0);
    /* How bright a screen is, from its MASTER_BRIGHT. */
    CHECK(NitroPort_LowerMenuBrightness(0) == 0);
    CHECK(NitroPort_LowerMenuBrightness(0x8010) == -16);   /* black */
    CHECK(NitroPort_LowerMenuBrightness(0x8008) == -8);
    CHECK(NitroPort_LowerMenuBrightness(0x4010) == 16);    /* white */
    CHECK(NitroPort_LowerMenuBrightness(0x801F) == -16);   /* (16 at most) */
    CHECK(NitroPort_LowerMenuBrightness(0x0010) == 0);     /* (no mode: as it is) */
    CHECK(NitroPort_LowerMenuBrightness(0xC010) == 0);
}

static void test_enabled_and_expiry(void)
{
    int i;
    const int bagY = 3 + 23 * NITRO_LOWERMENU_BAG + 10, mapY = 3 + 10;
    uint32_t before;

    NitroPort_LowerMenuShow(1);
    NitroPort_LowerMenuTouch(0, 0, 0, NULL, NULL);
    NitroPort_LowerMenuTakePress();
    /* A button that can't be used now: drawn dimmed (its name grey), and a
     * touch on it isn't a press; the others still are. */
    before = NitroPort_LowerMenuSerial();
    NitroPort_LowerMenuSetEnabled(~(1u << NITRO_LOWERMENU_MAP));
    CHECK(NitroPort_LowerMenuSerial() != before);
    NitroPort_LowerMenuTouch(1, 280, mapY, NULL, NULL);
    NitroPort_LowerMenuTouch(0, 0, 0, NULL, NULL);
    CHECK(NitroPort_LowerMenuTakePress() == -1);
    NitroPort_LowerMenuTouch(1, 280, bagY, NULL, NULL);
    NitroPort_LowerMenuTouch(0, 0, 0, NULL, NULL);
    CHECK(NitroPort_LowerMenuPressPending() == NITRO_LOWERMENU_BAG);
    CHECK(NitroPort_LowerMenuPressPending() == NITRO_LOWERMENU_BAG);   /* (still there) */
    CHECK(NitroPort_LowerMenuTakePress() == NITRO_LOWERMENU_BAG);
    CHECK(NitroPort_LowerMenuPressPending() == -1);
    /* The same mask again: not drawn again. */
    before = NitroPort_LowerMenuSerial();
    NitroPort_LowerMenuSetEnabled(~(1u << NITRO_LOWERMENU_MAP));
    CHECK(NitroPort_LowerMenuSerial() == before);
    NitroPort_LowerMenuSetEnabled(0xFFFFFFFFu);

    /* A press not taken: still there for NITRO_LOWERMENU_PRESS_FRAMES frames,
     * then dropped. */
    NitroPort_LowerMenuTouch(1, 280, bagY, NULL, NULL);
    NitroPort_LowerMenuTouch(0, 0, 0, NULL, NULL);
    for (i = 0; i < NITRO_LOWERMENU_PRESS_FRAMES; i++) {
        NitroPort_LowerMenuTouch(0, 0, 0, NULL, NULL);
    }
    CHECK(NitroPort_LowerMenuPressPending() == NITRO_LOWERMENU_BAG);
    NitroPort_LowerMenuTouch(0, 0, 0, NULL, NULL);
    CHECK(NitroPort_LowerMenuPressPending() == -1);
    CHECK(NitroPort_LowerMenuTakePress() == -1);
}

/* (the map's worker's wake, counted) */
static int sWakes;
static void wake(void)
{
    sWakes++;
}

/* The map asked for without the Town Map's pictures (no data pack here):
 * the panel stays the DS's lower screen, its touches the game's; nothing
 * handed to the map's drawer, no map picture. */
static void test_map_without_pictures(void)
{
    int x = -1, y = -1, y0, y1, i;
    static uint8_t tiles[32 * 24 * 32];
    static const uint16_t palette[16] = { 0, 0x7FFF, 0x001F };
    uint32_t serial = 99;

    CHECK(NitroPort_LowerMenuMapAvailable() == 0);
    NitroPort_LowerMenuSetMapWorker(wake);
    NitroPort_LowerMenuShow(1);
    NitroPort_LowerMenuTouch(0, 0, 0, NULL, NULL);
    NitroPort_LowerMenuMapBegin();
    NitroPort_LowerMenuMapSprite(NITRO_LOWERMAP_CURSOR, 60, 134, 0, 0);
    NitroPort_LowerMenuMapDot(60, 127);
    NitroPort_LowerMenuMapLayout(0, 8, 288);
    NitroPort_LowerMenuMapText(0, 24, 168, 29, 3, tiles, palette, 7);
    NitroPort_LowerMenuMapText(0, 0, 0, 33, 3, tiles, palette, 8);   /* (too wide: none, not kept) */
    NitroPort_LowerMenuMapText(1, 8, 64, 28, 14, tiles, palette, 7);
    /* (the texts kept by their keys, each kind its own) */
    CHECK(NitroPort_LowerMenuMapTextKept(0, 7) == 1);
    CHECK(NitroPort_LowerMenuMapTextKept(0, 8) == 0);
    CHECK(NitroPort_LowerMenuMapTextKept(1, 7) == 1);
    CHECK(NitroPort_LowerMenuMapTextKept(1, 9) == 0);
    for (i = 0; i < NITRO_LOWERMENU_TEXTS_KEPT; i++) {
        NitroPort_LowerMenuMapText(1, 8, 64, 28, 14, tiles, palette, 100u + (uint32_t)i);
    }
    CHECK(NitroPort_LowerMenuMapTextKept(1, 7) == 0);   /* (the one used longest ago gone) */
    CHECK(NitroPort_LowerMenuMapTextKept(1, 100) == 1);
    CHECK(NitroPort_LowerMenuMapTextKept(0, 7) == 1);
    NitroPort_LowerMenuMapEnd();
    CHECK(sWakes == 0);   /* (nothing to draw) */
    CHECK(NitroPort_LowerMenuMapWork() == 0);
    CHECK(NitroPort_LowerMenuLockMap(&serial) == NULL);
    NitroPort_LowerMenuTakeMapDirtyRows(&y0, &y1);
    CHECK(y0 >= y1);
    NitroPort_LowerMenuUnlockMap();
    CHECK(serial == NitroPort_LowerMenuMapSerial());
    NitroPort_LowerMenuSetPanel(NITRO_LOWERMENU_PANEL_MAP);
    CHECK(NitroPort_LowerMenuPanel() == NITRO_LOWERMENU_PANEL_DS);
    /* (no map: no black for it either) */
    CHECK(NitroPort_LowerMenuPanelFade() == 0);
    NitroPort_LowerMenuSetMapWorker(NULL);
    /* (the panel's touches still the game's: no map shown) */
    CHECK(NitroPort_LowerMenuTouch(1, 123, 120, &x, &y) == 1);
    NitroPort_LowerMenuTouch(0, 0, 0, NULL, NULL);
    CHECK(NitroPort_LowerMenuTakePress() == -1);
    NitroPort_LowerMenuSetPanel(NITRO_LOWERMENU_PANEL_DS);
}

static void test_picture(void)
{
    uint32_t serial, before;
    const int x = 311, y = 3 + 23 * NITRO_LOWERMENU_SAVE + 8;   /* the SAVE button, right of its name */

    NitroPort_LowerMenuShow(1);
    NitroPort_LowerMenuTouch(0, 0, 0, NULL, NULL);
    NitroPort_LowerMenuSelect(-1);
    /* The frame's light top-left edge, the panel's navy outline. */
    CHECK(pixel(0, 0) == 0xFCFDFDFFu);
    CHECK(pixel(100, 2) == 0x072136FFu);
    /* A button not selected is near white; selected, green: the picture
     * drawn again, its serial changed. */
    CHECK(pixel(x, y) == 0xF4F7FEFFu);
    before = NitroPort_LowerMenuSerial();
    NitroPort_LowerMenuSelect(NITRO_LOWERMENU_SAVE);
    serial = NitroPort_LowerMenuSerial();
    CHECK(serial != before);
    CHECK(pixel(x, y) == 0x2ED09BFFu);
    /* The same again: not drawn again. */
    NitroPort_LowerMenuSelect(NITRO_LOWERMENU_SAVE);
    CHECK(NitroPort_LowerMenuSerial() == serial);
    NitroPort_LowerMenuSelect(-1);
    CHECK(pixel(x, y) == 0xF4F7FEFFu);
    /* A finger on a button shows it pressed. */
    NitroPort_LowerMenuTouch(1, x, y, NULL, NULL);
    CHECK(pixel(x, y) == 0x2ED09BFFu);
    NitroPort_LowerMenuTouch(0, 0, 0, NULL, NULL);
    CHECK(pixel(x, y) == 0xF4F7FEFFu);
    CHECK(NitroPort_LowerMenuTakePress() == NITRO_LOWERMENU_SAVE);
    /* Off: the serial changes too (the compositor draws the DS's screen again). */
    before = NitroPort_LowerMenuSerial();
    NitroPort_LowerMenuShow(0);
    CHECK(NitroPort_LowerMenuSerial() != before);
}

/* A tap on (x, y): down, then let go. */
static void tap(int x, int y)
{
    NitroPort_LowerMenuTouch(1, x, y, NULL, NULL);
    NitroPort_LowerMenuTouch(0, 0, 0, NULL, NULL);
}

/* A double tap: the second press on a button within
 * NITRO_LOWERMENU_DOUBLE_FRAMES of the first; a third starts again; one on
 * another button in between, or too late, isn't. */
static void test_double_tap(void)
{
    const int mapY = 3 + 23 * NITRO_LOWERMENU_MAP + 10, bagY = 3 + 23 * NITRO_LOWERMENU_BAG + 10;
    int i;

    NitroPort_LowerMenuShow(1);
    NitroPort_LowerMenuSetEnabled(0xFFFFFFFFu);
    NitroPort_LowerMenuTouch(0, 0, 0, NULL, NULL);
    for (i = 0; i < NITRO_LOWERMENU_DOUBLE_FRAMES + 2; i++) {
        NitroPort_LowerMenuTouch(0, 0, 0, NULL, NULL);
    }
    tap(280, mapY);
    CHECK(NitroPort_LowerMenuPressPending() == NITRO_LOWERMENU_MAP);
    CHECK(NitroPort_LowerMenuPressDouble() == 0);
    CHECK(NitroPort_LowerMenuTakePress() == NITRO_LOWERMENU_MAP);
    NitroPort_LowerMenuTouch(0, 0, 0, NULL, NULL);
    tap(280, mapY);
    CHECK(NitroPort_LowerMenuPressDouble() == 1);
    CHECK(NitroPort_LowerMenuTakePress() == NITRO_LOWERMENU_MAP);
    CHECK(NitroPort_LowerMenuPressDouble() == 1);   /* (the press taken last) */
    tap(280, mapY);
    CHECK(NitroPort_LowerMenuPressDouble() == 0);
    NitroPort_LowerMenuTakePress();
    /* (another button between) */
    tap(280, bagY);
    NitroPort_LowerMenuTakePress();
    tap(280, mapY);
    CHECK(NitroPort_LowerMenuPressDouble() == 0);
    NitroPort_LowerMenuTakePress();
    /* (too late) */
    for (i = 0; i < NITRO_LOWERMENU_DOUBLE_FRAMES + 1; i++) {
        NitroPort_LowerMenuTouch(0, 0, 0, NULL, NULL);
    }
    tap(280, mapY);
    CHECK(NitroPort_LowerMenuPressDouble() == 0);
    NitroPort_LowerMenuTakePress();
    /* (the menu off between: not counted, however few of its frames) */
    NitroPort_LowerMenuShow(0);
    NitroPort_LowerMenuShow(1);
    NitroPort_LowerMenuTouch(0, 0, 0, NULL, NULL);
    tap(280, mapY);
    CHECK(NitroPort_LowerMenuPressDouble() == 0);
    NitroPort_LowerMenuTakePress();
    NitroPort_LowerMenuShow(0);
}

/* The panel's fade: towards black and back at a whole fade's speed of
 * NITRO_LOWERMENU_FADE_FRAMES of the platform's frames, from how dark it is;
 * clear at once; done at once without a clock. While it's dark, the panel's
 * touches are nobody's. */
static uint32_t sNow;
static uint32_t now(void)
{
    return sNow;
}

static void test_fade(void)
{
    int x, y;

    NitroPort_LowerMenuShow(1);
    NitroPort_LowerMenuTouch(0, 0, 0, NULL, NULL);
    NitroPort_LowerMenuSetPanel(NITRO_LOWERMENU_PANEL_DS);
    /* Without a clock: at once. */
    NitroPort_LowerMenuFadePanel(1);
    CHECK(NitroPort_LowerMenuPanelFade() == 16);
    NitroPort_LowerMenuFadePanel(0);
    CHECK(NitroPort_LowerMenuPanelFade() == 0);
    /* With one: 16 levels over NITRO_LOWERMENU_FADE_FRAMES frames. */
    NitroPort_LowerMenuSetClock(now);
    sNow = 1000;
    NitroPort_LowerMenuFadePanel(1);
    CHECK(NitroPort_LowerMenuPanelFade() == 0);
    sNow += NITRO_LOWERMENU_FADE_FRAMES / 2;
    CHECK(NitroPort_LowerMenuPanelFade() == 8);
    /* (asked again: no change) */
    NitroPort_LowerMenuFadePanel(1);
    CHECK(NitroPort_LowerMenuPanelFade() == 8);
    /* Dark: the panel's touches nobody's (the buttons still the menu's). */
    CHECK(NitroPort_LowerMenuTouch(1, 123, 120, &x, &y) == 0);
    NitroPort_LowerMenuTouch(0, 0, 0, NULL, NULL);
    CHECK(NitroPort_LowerMenuTakePress() == -1);
    /* Back from half way: half a fade's time. */
    NitroPort_LowerMenuFadePanel(0);
    CHECK(NitroPort_LowerMenuPanelFade() == 8);
    sNow += NITRO_LOWERMENU_FADE_FRAMES / 4;
    CHECK(NitroPort_LowerMenuPanelFade() == 4);
    sNow += NITRO_LOWERMENU_FADE_FRAMES / 4;
    CHECK(NitroPort_LowerMenuPanelFade() == 0);
    sNow += 100;
    CHECK(NitroPort_LowerMenuPanelFade() == 0);
    /* Black, then clear at once. */
    NitroPort_LowerMenuFadePanel(1);
    sNow += NITRO_LOWERMENU_FADE_FRAMES;
    CHECK(NitroPort_LowerMenuPanelFade() == 16);
    sNow += 1000;
    CHECK(NitroPort_LowerMenuPanelFade() == 16);
    NitroPort_LowerMenuFadePanel(NITRO_LOWERMENU_CLEAR_NOW);
    CHECK(NitroPort_LowerMenuPanelFade() == 0);
    /* Clear: the panel's touches the game's again. */
    CHECK(NitroPort_LowerMenuTouch(1, 123, 120, &x, &y) == 1);
    NitroPort_LowerMenuTouch(0, 0, 0, NULL, NULL);

    /* A tab changed with the fade: what it shows already, nothing. */
    NitroPort_LowerMenuFadeToPanel(NITRO_LOWERMENU_PANEL_DS);
    CHECK(NitroPort_LowerMenuPanelFade() == 0);
    /* (the map: no Town Map's pictures here, the DS's shown for it; the
     * change made all the same) To black, then back, the change made once
     * it's black; asked for meanwhile, the same. */
    NitroPort_LowerMenuFadeToPanel(NITRO_LOWERMENU_PANEL_MAP);
    sNow += NITRO_LOWERMENU_FADE_FRAMES / 2;
    CHECK(NitroPort_LowerMenuPanelFade() == 8);
    NitroPort_LowerMenuSetPanel(NITRO_LOWERMENU_PANEL_MAP);   /* (the game's every frame: still on its way) */
    NitroPort_LowerMenuFadePanel(0);                          /* (fading in waits for the change) */
    sNow += NITRO_LOWERMENU_FADE_FRAMES / 2;
    CHECK(NitroPort_LowerMenuPanelFade() == 16);
    sNow += NITRO_LOWERMENU_FADE_FRAMES / 2;
    CHECK(NitroPort_LowerMenuPanelFade() == 8);
    sNow += NITRO_LOWERMENU_FADE_FRAMES / 2;
    CHECK(NitroPort_LowerMenuPanelFade() == 0);
    /* Back to the DS's lower screen, asked only well after: as it would be
     * then (black at 8 frames, 4 more for its picture, 8 to fade in). */
    NitroPort_LowerMenuFadeToPanel(NITRO_LOWERMENU_PANEL_DS);
    sNow += NITRO_LOWERMENU_FADE_FRAMES + 4 + NITRO_LOWERMENU_FADE_FRAMES / 2;
    CHECK(NitroPort_LowerMenuPanelFade() == 8);
    sNow += NITRO_LOWERMENU_FADE_FRAMES / 2;
    CHECK(NitroPort_LowerMenuPanelFade() == 0);
    /* Called off by a fade to black (it stays black, nothing changed) or a
     * clear at once. */
    NitroPort_LowerMenuFadeToPanel(NITRO_LOWERMENU_PANEL_MAP);
    NitroPort_LowerMenuFadePanel(1);
    sNow += 100;
    CHECK(NitroPort_LowerMenuPanelFade() == 16);
    NitroPort_LowerMenuFadePanel(NITRO_LOWERMENU_CLEAR_NOW);
    NitroPort_LowerMenuFadeToPanel(NITRO_LOWERMENU_PANEL_MAP);
    sNow += 2;
    NitroPort_LowerMenuFadePanel(NITRO_LOWERMENU_CLEAR_NOW);
    sNow += 100;
    CHECK(NitroPort_LowerMenuPanelFade() == 0);
    NitroPort_LowerMenuSetPanel(NITRO_LOWERMENU_PANEL_DS);
    NitroPort_LowerMenuSetClock(NULL);
    NitroPort_LowerMenuShow(0);
}

/* Fading in to the DS's lower screen, or clearing at once (an app popping
 * in), once the platform says what it draws there: black till it's drawn a
 * picture made since (half a second at most). Last: the port then waits
 * for the rest of the run. */
static void test_fade_waits_for_picture(void)
{
    NitroPort_LowerMenuShow(1);
    NitroPort_LowerMenuSetPanel(NITRO_LOWERMENU_PANEL_DS);
    NitroPort_LowerMenuSetClock(now);
    sNow = 5000;
    NitroPort_LowerMenuPanelShown(4990);
    NitroPort_LowerMenuFadePanel(1);
    sNow += NITRO_LOWERMENU_FADE_FRAMES;
    CHECK(NitroPort_LowerMenuPanelFade() == 16);
    /* Cleared at once: black, its picture from before. */
    NitroPort_LowerMenuFadePanel(NITRO_LOWERMENU_CLEAR_NOW);
    CHECK(NitroPort_LowerMenuPanelFade() == 16);
    sNow += 2;
    NitroPort_LowerMenuPanelShown(sNow - 3);
    CHECK(NitroPort_LowerMenuPanelFade() == 16);
    /* A picture made since: clear. */
    NitroPort_LowerMenuPanelShown(sNow);
    CHECK(NitroPort_LowerMenuPanelFade() == 0);
    /* Faded in: the fade begins once there's one. */
    NitroPort_LowerMenuFadePanel(1);
    sNow += NITRO_LOWERMENU_FADE_FRAMES;
    CHECK(NitroPort_LowerMenuPanelFade() == 16);
    NitroPort_LowerMenuFadePanel(0);
    sNow += 3;
    CHECK(NitroPort_LowerMenuPanelFade() == 16);
    NitroPort_LowerMenuPanelShown(sNow);
    CHECK(NitroPort_LowerMenuPanelFade() == 16);
    sNow += NITRO_LOWERMENU_FADE_FRAMES / 2;
    CHECK(NitroPort_LowerMenuPanelFade() == 8);
    sNow += NITRO_LOWERMENU_FADE_FRAMES / 2;
    CHECK(NitroPort_LowerMenuPanelFade() == 0);
    /* None coming: clear after half a second all the same. */
    NitroPort_LowerMenuFadePanel(1);
    sNow += NITRO_LOWERMENU_FADE_FRAMES;
    NitroPort_LowerMenuFadePanel(NITRO_LOWERMENU_CLEAR_NOW);
    sNow += 29;
    CHECK(NitroPort_LowerMenuPanelFade() == 16);
    sNow += 1;
    CHECK(NitroPort_LowerMenuPanelFade() == 0);
    /* Left again before it popped in: black all along, never the picture
     * from before. */
    NitroPort_LowerMenuFadePanel(1);
    sNow += NITRO_LOWERMENU_FADE_FRAMES;
    NitroPort_LowerMenuFadePanel(NITRO_LOWERMENU_CLEAR_NOW);
    NitroPort_LowerMenuPanelShown(sNow - 1);
    sNow += 1;
    CHECK(NitroPort_LowerMenuPanelFade() == 16);
    NitroPort_LowerMenuFadePanel(1);
    CHECK(NitroPort_LowerMenuPanelFade() == 16);
    sNow += NITRO_LOWERMENU_FADE_FRAMES / 2;
    CHECK(NitroPort_LowerMenuPanelFade() == 16);
    NitroPort_LowerMenuSetClock(NULL);
    NitroPort_LowerMenuShow(0);
}

/* The upper screen let go now: the game's picture shown once it's one made
 * since (or half a second on); without a clock, at once. */
static void test_upper_let_go_waits_for_picture(void)
{
    NitroPort_LowerMenuHoldUpper(1);
    CHECK(NitroPort_LowerMenuUpperLetGoShows(0) == 1);   /* (no clock) */
    NitroPort_LowerMenuSetClock(now);
    sNow = 7000;
    NitroPort_LowerMenuHoldUpper(NITRO_LOWERMENU_LET_GO_NOW);
    CHECK(NitroPort_LowerMenuUpperHeld() < 0);
    CHECK(NitroPort_LowerMenuUpperLetGoShows(6998) == 0);
    sNow += 2;
    CHECK(NitroPort_LowerMenuUpperLetGoShows(6999) == 0);
    CHECK(NitroPort_LowerMenuUpperLetGoShows(7000) == 1);
    CHECK(NitroPort_LowerMenuUpperLetGoShows(7001) == 1);
    /* (none coming: shown after half a second all the same) */
    sNow = 7000 + 29;
    CHECK(NitroPort_LowerMenuUpperLetGoShows(6990) == 0);
    sNow += 1;
    CHECK(NitroPort_LowerMenuUpperLetGoShows(6990) == 1);
    /* (let go again later: its own time) */
    NitroPort_LowerMenuHoldUpper(1);
    sNow = 8000;
    NitroPort_LowerMenuHoldUpper(NITRO_LOWERMENU_LET_GO_NOW);
    CHECK(NitroPort_LowerMenuUpperLetGoShows(7500) == 0);
    CHECK(NitroPort_LowerMenuUpperLetGoShows(8000) == 1);
    NitroPort_LowerMenuHoldUpper(0);
    NitroPort_LowerMenuSetClock(NULL);
    /* Kept showing the field's picture: the platforms' wish 2; forgotten
     * (walking again): its count goes on. */
    {
        uint32_t serial = NitroPort_LowerMenuFieldSerial();

        NitroPort_LowerMenuHoldUpper(NITRO_LOWERMENU_HOLD_FIELD);
        CHECK(NitroPort_LowerMenuUpperHeld() == NITRO_LOWERMENU_HOLD_FIELD);
        NitroPort_LowerMenuHoldUpper(1);
        CHECK(NitroPort_LowerMenuUpperHeld() == 1);
        NitroPort_LowerMenuHoldUpper(5);
        CHECK(NitroPort_LowerMenuUpperHeld() == 1);
        NitroPort_LowerMenuHoldUpper(0);
        CHECK(NitroPort_LowerMenuFieldSerial() == serial);
        NitroPort_LowerMenuForgetField();
        CHECK(NitroPort_LowerMenuFieldSerial() != serial);
    }
}

/* The menu kept shown while off: its buttons still pressed then, the panel
 * nobody's; the press not dropped when it goes off and on again meanwhile. */
static void test_kept_shown(void)
{
    const int bagY = 3 + 23 * NITRO_LOWERMENU_BAG + 10;
    uint32_t before;
    int x, y;

    NitroPort_LowerMenuShow(1);
    NitroPort_LowerMenuTouch(0, 0, 0, NULL, NULL);
    CHECK(NitroPort_LowerMenuKeptShown() == 0);
    before = NitroPort_LowerMenuSerial();
    NitroPort_LowerMenuKeepShown(1);
    CHECK(NitroPort_LowerMenuKeptShown() == 1);
    CHECK(NitroPort_LowerMenuSerial() != before);
    /* Off meanwhile: the buttons still pressed, the panel nobody's; a
     * press made then still there when it's on again. */
    NitroPort_LowerMenuShow(0);
    CHECK(NitroPort_LowerMenuOn() == 0);
    CHECK(NitroPort_LowerMenuTouch(1, 123, 120, &x, &y) == 0);
    NitroPort_LowerMenuTouch(0, 0, 0, NULL, NULL);
    CHECK(NitroPort_LowerMenuTakePress() == -1);
    tap(280, bagY);
    NitroPort_LowerMenuShow(1);
    CHECK(NitroPort_LowerMenuTakePress() == NITRO_LOWERMENU_BAG);
    NitroPort_LowerMenuKeepShown(0);
    CHECK(NitroPort_LowerMenuKeptShown() == 0);
    /* Off, not kept shown: nothing. */
    NitroPort_LowerMenuShow(0);
    tap(280, bagY);
    CHECK(NitroPort_LowerMenuTakePress() == -1);
}

/* The picture's rows changed, for the display to copy only them: all of
 * it when it's drawn again; none once taken. */
static void test_dirty_rows(void)
{
    int y0, y1;

    NitroPort_LowerMenuShow(1);
    NitroPort_LowerMenuSelect(-1);
    NitroPort_LowerMenuSelect(NITRO_LOWERMENU_SAVE);
    NitroPort_LowerMenuLockFrame(NULL);
    NitroPort_LowerMenuTakeDirtyRows(&y0, &y1);
    CHECK(y0 == 0 && y1 == NITRO_LOWERMENU_H);
    NitroPort_LowerMenuTakeDirtyRows(&y0, &y1);
    CHECK(y0 >= y1);
    NitroPort_LowerMenuUnlockFrame();
    NitroPort_LowerMenuSelect(-1);
    NitroPort_LowerMenuShow(0);
}

int main(void)
{
    NitroPort_LowerMenuSetAvailable(1);
    test_panel_mapping();
    test_tall_mapping();
    test_buttons();
    test_touches();
    test_app_in_panel();
    test_enabled_and_expiry();
    test_map_without_pictures();
    test_picture();
    test_double_tap();
    test_fade();
    test_kept_shown();
    test_dirty_rows();
    test_fade_waits_for_picture();
    test_upper_let_go_waits_for_picture();
    printf("lowermenu: %d checks, %d failures\n", sChecks, sFailures);
    return sFailures ? 1 : 0;
}
