/*
 * The 3DS remaster's wider upper screen: where the game's windows are
 * (port_wide.c), and what the picture is laid out like (the rules).
 *
 * The game's windows (bg_window.c: text boxes, menus, the place name sign)
 * are rectangles of a BG's tilemap. The decompilation tells the port when
 * one is added and removed (patches/pokeplatinum/0012), and which ones go
 * to an edge of the wider screen (NitroPort_WindowAnchor: the place name
 * sign, patch 0014); the 2D renderer moves those (port_render2d.c,
 * TextBgSegments) and leaves the rest where the DS has them (a menu's
 * icons, the game's sprites, are placed to match). Plain C, no SDK
 * headers: the 3DS program includes it next to libctru.
 */
#ifndef NITRO_PORT_WIDE_H
#define NITRO_PORT_WIDE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NITRO_WIDE_MAX_WINDOWS 24

/* A window: its rectangle of the BG's tilemap, in tiles, and the edge it
 * goes to (-1 left, 1 right). */
typedef struct NitroWideWindow {
    uint8_t left, top, width, height;
    int8_t anchor;
} NitroWideWindow;

/* From the game (bgLayer: 0-3 engine A's BGs, 4-7 engine B's). */
void NitroPort_WindowAdded(const void *window, int bgLayer, int left, int top, int width, int height);
void NitroPort_WindowRemoved(const void *window);
/* From the game: this window goes to the wider screen's left (side < 0)
 * or right (side > 0) edge; 0 puts it back where the DS has it. widthTiles:
 * only the window's first (left) tiles move, when what it shows is
 * narrower than the window (0: all of it). */
void NitroPort_WindowAnchor(const void *window, int side, int widthTiles);
/* The anchored windows on an engine's BG, as they are (up to max): how
 * many. A window whose struct the game reused or cleared without removing
 * it is left out. Any thread. */
int NitroPort_WideWindows(int engine, int bg, NitroWideWindow *out, int max);
/* Whether the screens are pictures (the opening movie, the title, the
 * cutscenes) rather than the game's UI: pictures are mirrored at the DS's
 * edges, the UI isn't. */
int NitroPort_WidePictures(void);
/* (tests, the PC runner) 1: pictures, 0: the UI, -1: as the overlays say. */
void NitroPort_WideForcePictures(int mode);
/* Changes whenever a window is added or removed. */
uint32_t NitroPort_WideWindowsSerial(void);

/* The wider screen: how many DS pixels it adds each side (32 for the
 * remaster's upper screen: 400 pixels at 1.25 times the DS's size; 0 for
 * the DS's screen). The game reads it (patches/pokeplatinum/0013: the
 * battle's health boxes go to the screen's edges) and the layout rules. */
void NitroPort_SetWideScreen(int extra);
int NitroPort_WideScreenExtra(void);

/* The layout of an engine's picture on the wider upper screen, for this
 * moment of the game (NitroRender2DSplit's canvas: width, origin, clip,
 * anchorWindows; declared in nitro_port_render2d.h). */
struct NitroRender2DSplit;
void NitroPort_WideLayout(int engine, struct NitroRender2DSplit *split);

/* (port_overlay.c) Whether the overlay of that name is loaded. */
int NitroPort_OverlayLoadedNamed(const char *name);

#ifdef __cplusplus
}
#endif
#endif
