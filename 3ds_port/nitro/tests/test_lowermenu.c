/*
 * Tests for the lower screen as a menu (src/port_lowermenu.c): where a
 * touch goes (the panel's DS pixel, or a button), a press only when let go
 * on the button it began on and only on one that can be used now, a press
 * not taken soon dropped, the picture redrawn when the cursor moves, and
 * nothing while it's off. (No data pack: the start menu's icons are
 * left out, the rest drawn.)
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

int main(void)
{
    NitroPort_LowerMenuSetAvailable(1);
    test_panel_mapping();
    test_buttons();
    test_touches();
    test_enabled_and_expiry();
    test_picture();
    printf("lowermenu: %d checks, %d failures\n", sChecks, sFailures);
    return sFailures ? 1 : 0;
}
