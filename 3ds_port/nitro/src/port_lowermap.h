/*
 * (port_lowermenu.c's) The Town Map in the lower menu's panel: drawn by
 * port_lowermap.c (nitro_port_lowermenu.h: NitroPort_LowerMenuMapBegin..End).
 * Everything here but Load and Loaded on the map's drawer only (one thread
 * at a time: port_lowermenu.c's worker, or the game's thread without one).
 */
#ifndef PORT_LOWERMAP_H
#define PORT_LOWERMAP_H

#include <nitro_port_lowermenu.h>
#include <stdint.h>

/* (the game's thread) The Town Map's pictures read from the data pack, the
 * first time: whether they could be (Load), have been (Loaded). */
int NitroPort_LowerMapLoad(void);
int NitroPort_LowerMapLoaded(void);
/* The map's contents: what changed of the canvas made again, and of the
 * panel's picture (the first map, or other hidden places shown: all of it).
 * 0 if the Town Map's pictures haven't been read. */
int NitroPort_LowerMapSet(const NitroLowerMap *map);
/* The panel's rows drawn again since last asked: y0 to y1 - 1 (none: y0 >=
 * y1). */
void NitroPort_LowerMapTakeDirty(int *y0, int *y1);
/* The menu's frame count now (its animations: the cursor's frames, the dot
 * shown): 1 if the panel's picture changed. FrameChanges: whether the
 * animations change at that frame (pure: any thread). */
int NitroPort_LowerMapSetFrame(uint32_t frame);
int NitroPort_LowerMapFrameChanges(uint32_t frame);
/* The panel's picture: NITRO_LOWERMENU_PANEL_W x NITRO_LOWERMENU_PANEL_H
 * texels, each (r << 24) | (g << 16) | (b << 8) | 255, rows from the top:
 * the canvas (256 x 247) at 15/16, as the screens draw the DS's (each pixel
 * 2 x 2 texels, then bilinear). */
const uint32_t *NitroPort_LowerMapPanel(void);
/* (tests) The canvas: 256 x 247 RGB555 pixels. */
const uint16_t *NitroPort_LowerMapCanvas(void);

/* (both files') A member of a NARC (the whole file in memory): its bytes
 * and size, or NULL; a block of a Nitro file (RGCN, RLCN, RCSN): its
 * contents (after its 8-byte header) and their size, or NULL. Every size
 * the file's, checked. */
const uint8_t *NitroPort_NarcMember(const uint8_t *narc, uint32_t size, int index, uint32_t *memberSize);
const uint8_t *NitroPort_NitroBlock(const uint8_t *file, uint32_t size, const char *magic, uint32_t *blockSize);

#endif
