/*
 * Wide art: pictures drawn by hand for the remaster's wider upper screen
 * (docs/WIDE_ART.md), replacing what the port works out by itself beside
 * the DS's picture (port_wide.c's rules: an edge colour going on, a
 * picture mirrored, nothing).
 *
 * A picture is for one layer of one screen of the game: a BG or a group of
 * OBJs (those of one priority and palette: NitroPort_Render2DObjGroups).
 * It's found by that layer's key, a hash of the layer's own art (a BG's
 * map, the tiles the map uses, their palettes; a group's OBJs' pixels and
 * colours; not where it's scrolled or moved to). So it shows whenever that
 * layer does, whatever the other layers do (text printing, a cursor
 * moving), and moves with the layer. A group's picture is drawn into the
 * OBJ layer where no OBJ is, at the group's priority.
 *
 * Files: in the art folder (sdmc:/3ds/pokeplatinum/wide on the 3DS),
 *   <key>_<hofs>_<vofs>.png        the side bands only
 *   <key>_<hofs>_<vofs>_full.png   the whole picture, middle as well
 *   ..._loop.png                   (either) its bands go round with the
 *                                  layer as it scrolls (a strip)
 * <key> 16 hex digits; hofs, vofs: the layer's scroll when the picture was
 * captured, or the group's position (it's drawn shifted by the distance
 * since). 320 x 192 pixels, the
 * DS's picture at x 32-287; transparent pixels (alpha < 128) show what's
 * behind the layer. Made with the WideArt editor (tools/wideart) from
 * captures: NitroPort_WideArtCapture.
 *
 * Plain C (stdio, dirent): the 3DS program and the PC runner.
 */
#ifndef NITRO_PORT_WIDEART_H
#define NITRO_PORT_WIDEART_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NITRO_WIDE_ART_W 320
#define NITRO_WIDE_ART_H 192
/* A picture's kind (the "full" the functions below give): the whole layer
 * (<name>_full.png; else the bands only), and whether its bands go round
 * with the layer as it scrolls (<name>_loop.png: a strip that scrolls all
 * the time). Without that, its bands show only while the layer is across
 * where the picture was made (give or take a few pixels; up and down
 * doesn't matter): a picture sliding in, or parked beside the DS's
 * screen, shows nothing beyond the DS's edges, as on the DS. */
#define NITRO_WIDE_ART_FULL 1
#define NITRO_WIDE_ART_LOOP 2

/* What's in the art folder (its file names; the pictures are read when
 * first needed): how many pictures. Again to take in new files. */
int NitroPort_WideArtLoad(const char *dir);
/* Read pictures ahead (the first max of them, as many as are kept: see
 * port_wideart.c), so a picture's first showing doesn't wait for its file:
 * how many were read. */
int NitroPort_WideArtPreload(int max);
const char *NitroPort_WideArtDir(void);
int NitroPort_WideArtCount(void);
/* A picture a screen's layout draws (NITRO_WIDE_PICTURE: the folder's
 * <NitroPort_WidePictureName>.png, any size up to 320 x 192, transparent
 * where alpha < 128): its pixels (RGB555 | 0x8000, 0 transparent), only
 * the box round what's drawn in it, that box's size and where it goes from
 * the layout's place (the file's oFFs chunk, if it has one: the editor's,
 * for a picture moved there; and the margin left out); NULL if there's
 * none, or nothing's drawn in it. Read the first time it's asked for (the
 * folder's load reads them all ahead), kept from then on. Any thread. */
const uint16_t *NitroPort_WideArtPicture(int picture, int *w, int *h, int *dx, int *dy);
/* Changes whenever the pictures may have (loaded again). */
uint32_t NitroPort_WideArtSerial(void);
/* The picture for a layer's key (NITRO_WIDE_ART_W x H pixels: RGB555 |
 * 0x8000, or 0 for transparent), the scroll it was made at, and whether
 * it's the whole picture; NULL if there's none (or it can't be read). Any
 * thread. */
const uint16_t *NitroPort_WideArtFind(uint64_t key, int *hofs, int *vofs, int *full);
/* A layer may have several pictures, made at different scrolls (one BG
 * map can hold two scenes: the opening's sky above, its strip below).
 * Of a key's pictures: the one made nearest the layer's scroll now (curH,
 * curV; the map going round every periodH, periodV), if it's less than
 * maxH and maxV away (0: any distance), so that its picture is of what's
 * on screen. Its scroll then, and whether it's the whole picture. */
const uint16_t *NitroPort_WideArtFindNear(uint64_t key, int curH, int curV, int periodH, int periodV, int maxH,
                                          int maxV, int *hofs, int *vofs, int *full);

/* A picture's opaque columns (left .. right - 1), by its pixels as the
 * functions above gave them: 1 if known. Any thread. */
int NitroPort_WideArtExtent(const uint16_t *pixels, int *left, int *right);
/* Where a whole picture goes when its layer has moved `moved` pixels
 * sideways from where the picture was made (> 0: to the right), the
 * picture's opaque columns left .. right - 1: as far, while what it shows
 * is well inside the DS's screen; further as it nears the DS's edge (the
 * last 32 pixels of the DS's way taking 64 of the wide screen's), so that
 * what the DS has beyond its edge, coming in or gone out, is beyond the
 * wide screen's too, and comes in from there; past that, 32 more. */
int NitroPort_WideArtMovedOffset(int moved, int left, int right);

/* Capture the upper screen for the editor: a new folder in dir
 * (captures/0001, ...) with screen.png (the whole picture, without the
 * 3D), bg<N>.png (each BG alone, as the port draws it now, its bands
 * included), obj<G>.png (each OBJ group shown, G its number) and info.txt
 * (each layer's key, scroll or position, and priority). The path
 * made goes to made. 0 if done. On the game's thread, between frames. */
int NitroPort_WideArtCapture(const char *dir, char *made, size_t madeSize);

/* (tests) a picture's file name read: 1 if it is one. */
int NitroPort_WideArtParseName(const char *name, uint64_t *key, int *hofs, int *vofs, int *full);
/* (tests) a PNG of RGBA pixels written: 0 if done. */
int NitroPort_WideArtWritePng(const char *path, const uint8_t *rgba, int w, int h);

#ifdef __cplusplus
}
#endif
#endif
