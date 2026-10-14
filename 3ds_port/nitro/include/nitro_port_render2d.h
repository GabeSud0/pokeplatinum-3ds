/*
 * The 2D renderer's two-layer output (port_render2d.c), for a compositor
 * that draws the 3D between the layers itself (the 3DS's GPU, at the
 * screen's size). Plain C types only: the 3DS program includes this next
 * to libctru, where NitroSDK's headers can't go.
 */
#ifndef NITRO_PORT_RENDER2D_H
#define NITRO_PORT_RENDER2D_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* NitroPort_Render2DSplit draws an engine's picture in two layers: under
 * gets what's behind the 3D (the picture without BG0), over the layers in
 * front of it: colour | 0x8000 where they cover the 3D, 0 elsewhere, and
 * overAlpha each covering pixel's alpha over the 3D (16: opaque; less: it
 * blends with the 3D by alpha / 16, as BLDCNT says). Where a window hides
 * the 3D, over shows what's behind it. The master brightness isn't
 * applied: it's for the compositor (masterBright), as is BLDCNT's effect
 * on the 3D itself (tint3d). Without a 3D layer (has3d 0: BG0 isn't the
 * 3D, or the display mode isn't the normal one), under is the whole
 * picture and over isn't touched. The caller provides over and overAlpha
 * and the canvas (below); the rest is filled in. */
typedef struct NitroRender2DSplit {
    int has3d;         /* BG0 is the 3D, drawn between under and over */
    int prio3d;        /* its priority, 0-3 */
    int scroll3d;      /* BG0HOFS, 0-511: the 3D moved left by this many pixels */
    /* The 3D as a first target of BLDCNT's effect: 0 none; 1 its pixels
     * blended with what's behind them by eva / 16 and evb / 16; 2
     * brightened by evy / 16; 3 darkened. */
    int tint3d;
    int eva, evb, evy;
    int blend3d;       /* BLDCNT's second targets (bits 8-13): what 3D pixels of alpha 1-30 blend with */
    int masterBright;  /* the MASTER_BRIGHT register: mode in bits 14-15, factor 0-16 */
    int backdrop;      /* the backdrop colour (RGB555): what shows where no layer does */
    uint16_t *over;    /* width x 192 */
    uint8_t *overAlpha;/* width x 192 */
    /* The picture's canvas (the caller's choice, kept by the call): 0 or
     * 256 for the DS's screen; up to 320 for the remaster's wider upper
     * screen, the DS's x = 0 at origin. under, over and overAlpha are
     * width pixels a line then. Beyond the DS's 256 pixels: a BG with
     * clip set shows nothing there (the backdrop does), without it its map
     * goes on; OBJs placed past the DS's edges show; with anchorWindows,
     * the game's windows on a BG (text boxes, menus, the place name sign)
     * move to the nearer edge of the screen unless they're across its
     * middle (NitroPort_WideWindows). */
    int width, origin;
    uint8_t clip[4];
    int anchorWindows;
} NitroRender2DSplit;
void NitroPort_Render2DSplit(int engine, uint16_t *under, NitroRender2DSplit *split);
/* The same, unless nothing it draws from changed since the last call for
 * that engine (returns 0 then, 1 when drawn). */
int NitroPort_Render2DSplitIfChanged(int engine, uint16_t *under, NitroRender2DSplit *split);

/* One BG of an engine alone (for capturing it: nitro_port_wideart.h), on
 * the canvas layout gives (width, origin, clip, anchorWindows; NULL: the
 * DS's), its bands drawn as the picture's are: out is width x 192 pixels
 * (colour | 0x8000, or 0). Its key (NitroPort_WideArtFind), scroll and
 * priority go to the others. 0 if the BG isn't shown or is the 3D (nothing
 * drawn then). */
int NitroPort_Render2DLayer(int engine, int bg, const NitroRender2DSplit *layout, uint16_t *out,
                            uint64_t *key, int *hofs, int *vofs, int *prio);

#ifdef __cplusplus
}
#endif
#endif
