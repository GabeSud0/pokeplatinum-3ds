/*
 * The 3DS remaster's wider upper screen: where the game's windows are
 * (port_wide.c), and what the picture is laid out like (the rules).
 *
 * The game's windows (bg_window.c: text boxes, menus, the place name sign)
 * are rectangles of a BG's tilemap. The decompilation tells the port when
 * one is added and removed (patches/pokeplatinum/0012), which ones go to
 * an edge of the wider screen (NitroPort_WindowAnchor: the place name
 * sign, patch 0014), and which have a frame drawn round them
 * (NitroPort_WindowFramed, patch 0015: the menus by the DS's edges go to
 * the screen's, the text boxes widen); the 2D renderer moves those
 * (port_render2d.c, TextBgSegments; the sprites over a moved window go
 * with it) and leaves the rest where the DS has them. A screen redrawn
 * for the wider screen (a reference made for it: the trainer card, the
 * summary, the party, the bag, the boxes, the Pokédex's list) is laid out
 * by the game in parts that move apart (NitroPort_WideSplitScreen, patch
 * 0016; NitroPort_WideScreenParts, patches 0017-0022). Plain C, no SDK
 * headers: the 3DS program includes it next to libctru.
 */
#ifndef NITRO_PORT_WIDE_H
#define NITRO_PORT_WIDE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NITRO_WIDE_MAX_WINDOWS 24

/* A window: its rectangle of the BG's tilemap, in tiles (with its frame,
 * when the game drew one round it), and the edge it goes to (-1 left, 1
 * right); or, a text box across the DS's screen (anchor 0, widen 1), its
 * frame across the wider screen: the frame's left frameLeft tiles at the
 * screen's left edge, its right frameRight tiles at the right edge, the
 * window inside them (its text) where the DS has it, and between them each
 * line of the frame's inner columns drawn on. */
typedef struct NitroWideWindow {
    uint8_t left, top, width, height;
    int8_t anchor;
    uint8_t widen, frameLeft, frameRight;
} NitroWideWindow;

/* From the game (bgLayer: 0-3 engine A's BGs, 4-7 engine B's). */
void NitroPort_WindowAdded(const void *window, int bgLayer, int left, int top, int width, int height);
void NitroPort_WindowRemoved(const void *window);
/* From the game: this window goes to the wider screen's left (side < 0)
 * or right (side > 0) edge; 0 puts it back where the DS has it. widthTiles:
 * only the window's first (left) tiles move, when what it shows is
 * narrower than the window (0: all of it). */
void NitroPort_WindowAnchor(const void *window, int side, int widthTiles);
/* From the game (patches/pokeplatinum/0015): a frame was drawn round this
 * window (1: a menu's, a tile all round; 2: a text box's, 2 tiles left and
 * 3 right) or taken off (0). On the wider screen, a framed window by the
 * DS's left or right edge (within 2 tiles) goes to the wider screen's edge,
 * keeping its distance from it (the menus, YES/NO, the save's summary); a
 * text box across the DS's screen has its frame widened to the screen's
 * edges. */
void NitroPort_WindowFramed(const void *window, int frame);
/* The windows on an engine's BG that move or widen, as they are (up to
 * max): how many. A window whose struct the game reused or cleared without
 * removing it is left out. Any thread. */
int NitroPort_WideWindows(int engine, int bg, NitroWideWindow *out, int max);
/* Whether the screens are pictures (the opening movie, the title, the
 * cutscenes) rather than the game's UI: pictures are mirrored at the DS's
 * edges, the UI isn't. */
int NitroPort_WidePictures(void);
/* (tests, the PC runner) 1: pictures, 0: the UI, -1: as the overlays say. */
void NitroPort_WideForcePictures(int mode);
/* Changes whenever a window is added or removed. */
uint32_t NitroPort_WideWindowsSerial(void);

/* A screen laid out anew for the wider screen, from a reference drawn for
 * it (the trainer card, 2026-10-15): on lines top..bottom-1 of the BGs in
 * bgs (bit n: BG n), the pixels left of column x move by left pixels and
 * those from x on by right pixels (negative: to the left); between them,
 * when they move apart, column x is drawn on (fill 1: a card's body,
 * widened) or nothing is (fill 0: the text and pictures over it). Lines
 * and columns of the BG's own picture: a text BG's map, an affine BG's
 * picture before its matrix (a card turning over stays laid out as it
 * turns); only the DS's 256 columns of it move, nothing from beyond them.
 * A BG with splits shows nothing beside the DS's screen but what they
 * move there (on lines without one, nothing at all), its hand-drawn
 * picture (if there's one) where the BG's picture would be. */
typedef struct NitroWideSplit {
    uint8_t bgs, fill;
    uint16_t top, bottom;
    int16_t x, left, right;
} NitroWideSplit;
#define NITRO_WIDE_MAX_SPLITS 16
/* From the game: an engine's screen laid out with these splits, count rows
 * of 7 numbers each: bgs, top, bottom, x, left, right, fill (copied; up to
 * NITRO_WIDE_MAX_SPLITS); count 0: as the DS has it. Only the wider screen
 * is laid out with them. */
void NitroPort_WideSplitScreen(int engine, const short *splits, int count);
/* (the 2D renderer) An engine's splits now (up to NITRO_WIDE_MAX_SPLITS
 * into out): how many. Any thread. */
int NitroPort_WideSplits(int engine, NitroWideSplit *out);

/* A screen laid out anew in parts (the party, the summary, the bag...,
 * 2026-10-16): what the splits above are made of, and more.
 *   MOVE: on lines top..bottom-1 of the BGs in bgs, the BG's columns
 *     x0..x1-1 drawn dx pixels to the right (negative: to the left);
 *   FILL: on those lines, where the BG's columns x0..x1-1 would be (as far
 *     beyond the DS's edges as the canvas goes), its column col drawn on;
 *   OBJS: the sprites whose middle is in x0..x1-1, top..bottom-1 of the
 *     DS's screen moved dx pixels to the right (the first part that has
 *     them); col 1: stretched, each one's edge column drawn on back to
 *     where it was (the right half of a cursor moved with its row's right
 *     end: the cursor as wide as the row);
 *   SHIFT3D: the 3D picture moved dx pixels to the right;
 *   PICTURE: the picture col (NITRO_WIDE_PICTURE_*: a file drawn for the
 *     screen, NitroPort_WideArtPicture) drawn with the BG in bgs, in front
 *     of that BG's own pixels (behind what's in front of the BG), its
 *     file's top-left at the DS's x0, top (anywhere on the canvas; moved
 *     by the file's offset, if the editor gave it one), turned dx degrees
 *     clockwise about the middle of what's drawn in it (x1, bottom: not
 *     used).
 * Lines and columns of the BG's own picture, as with splits (a text BG's
 * map, an affine BG's picture before its matrix); only the DS's 256
 * columns move. On a line of a BG with parts, the DS's picture shows where
 * it is but for the columns MOVEs take from it, and each part is drawn
 * over it in turn (a column shows the last part over it: what's clear in
 * that part is clear, not the part before); nothing is drawn beside the
 * DS's screen but what the parts put there. A BG with parts on other lines
 * shows those where the DS has them, nothing beside. A BG with MOVE parts
 * shows its own pixels, not a hand-drawn picture of it, but for one made
 * right where it is (a text BG: a capture of the screen laid out, drawn
 * on); sprites moved show themselves, not their picture. */
enum {
    NITRO_WIDE_MOVE = 1,
    NITRO_WIDE_FILL = 2,
    NITRO_WIDE_OBJS = 3,
    NITRO_WIDE_SHIFT3D = 4,
    NITRO_WIDE_PICTURE = 5,
};
/* The pictures a screen's layout can draw (NITRO_WIDE_PICTURE's col): each
 * a file <name>.png in the wide art folder (NitroPort_WidePictureName). */
enum {
    NITRO_WIDE_PICTURE_PC_BOXES_DIVIDER = 1,   /* the boxes: between the panel and the box */
    NITRO_WIDE_PICTURE_PC_BOXES_BALL,          /* its Poké Ball (it spins as the boxes change) */
    NITRO_WIDE_PICTURE_PARTY_PANEL,            /* the party: the panel at the left edge */
    NITRO_WIDE_PICTURE_PARTY_BALL,             /* its Poké Ball (it turns as the cursor moves) */
    NITRO_WIDE_PICTURE_PARTY_LIGHT_OFF,        /* its lights, each colour a picture */
    NITRO_WIDE_PICTURE_PARTY_LIGHT_RED,
    NITRO_WIDE_PICTURE_PARTY_LIGHT_BLUE,
    NITRO_WIDE_PICTURE_PARTY_LIGHT_GREEN,
    NITRO_WIDE_PICTURE_PARTY_LIGHT_AMBER,
    NITRO_WIDE_PICTURE_PARTY_LIGHT_WHITE,
    NITRO_WIDE_PICTURES
};
const char *NitroPort_WidePictureName(int picture);
typedef struct NitroWidePart {
    uint8_t kind, bgs;
    int16_t top, bottom, x0, x1, dx, col;
} NitroWidePart;
/* (in a part's bgs, with the BGs) Its columns x0..x1-1 are the DS screen's
 * rather than the BG's picture's: they stay where they are as the BG
 * scrolls (the PC's boxes sliding past behind a frame). A FILL's col is
 * still the picture's. */
#define NITRO_WIDE_SCREEN_COLUMNS 0x10
#define NITRO_WIDE_MAX_PARTS 128
/* From the game: an engine's screen laid out in these parts, count rows of
 * 8 numbers each: kind, bgs, top, bottom, x0, x1, dx, col (copied; up to
 * NITRO_WIDE_MAX_PARTS); count 0: as the DS has it. Apart from the splits
 * (both apply). Only the wider screen is laid out with them. */
void NitroPort_WideScreenParts(int engine, const short *rows, int count);
/* (the 2D renderer) An engine's parts now (up to NITRO_WIDE_MAX_PARTS into
 * out): how many. Any thread. */
int NitroPort_WideParts(int engine, NitroWidePart *out);

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
