/*
 * The 3DS remaster's wider upper screen: where the game's windows are
 * (port_wide.c), and what the picture is laid out like (the rules).
 *
 * The game's windows (bg_window.c: text boxes, menus, the place name sign)
 * are rectangles of a BG's tilemap. The decompilation tells the port when
 * one is added and removed (patches/pokeplatinum/0012); the 2D renderer
 * moves each to the nearer edge of the wider screen, unless it's across
 * the middle (port_render2d.c, TextBgSegments). Plain C, no SDK headers:
 * the 3DS program includes it next to libctru.
 */
#ifndef NITRO_PORT_WIDE_H
#define NITRO_PORT_WIDE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NITRO_WIDE_MAX_WINDOWS 24

/* A window: its rectangle of the BG's tilemap, in tiles. */
typedef struct NitroWideWindow {
    uint8_t left, top, width, height;
} NitroWideWindow;

/* From the game (bgLayer: 0-3 engine A's BGs, 4-7 engine B's). */
void NitroPort_WindowAdded(const void *window, int bgLayer, int left, int top, int width, int height);
void NitroPort_WindowRemoved(const void *window);
/* The windows on an engine's BG, as they are (up to max): how many. A
 * window whose struct the game reused or cleared without removing it is
 * left out. Any thread. */
int NitroPort_WideWindows(int engine, int bg, NitroWideWindow *out, int max);
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
