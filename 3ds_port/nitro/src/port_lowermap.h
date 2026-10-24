/*
 * (port_lowermenu.c's) The Town Map in the lower menu's panel: drawn by
 * port_lowermap.c (nitro_port_lowermenu.h: NitroPort_LowerMenuSetMap).
 * The game's thread.
 */
#ifndef PORT_LOWERMAP_H
#define PORT_LOWERMAP_H

#include <nitro_port_lowermenu.h>
#include <stdint.h>

/* The map's contents: the canvas made again, the panel's picture all drawn
 * again. 0 if the Town Map's pictures can't be read (no data pack). */
int NitroPort_LowerMapSet(const NitroLowerMap *map);
/* Whether it has a map to show. */
int NitroPort_LowerMapReady(void);
/* A frame gone by (its animations): 1 if the panel's picture changed. */
int NitroPort_LowerMapTick(void);
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
