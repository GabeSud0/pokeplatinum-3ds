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
    /* (an H-blank effect, NitroPort_LineRegs) scroll3d on each line, when
     * it differs from line to line (scroll3dLines; else 0 and scroll3d is
     * every line's) */
    int scroll3dLines;
    short scroll3dLine[192];
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
    /* Draw from the display state taken by NitroPort_Render2DTakeState
     * (registers, palettes, OAM as they were then), not the live one. */
    int snapshot;
    /* (the remaster's screens) A window from the DS's x 1 or to its x 255
     * (games' 0-255: the right end is exclusive) taken to the screen's edge:
     * no column of the DS's picture left out (the wider canvas does so
     * anyway). */
    int windowsToEdge;
} NitroRender2DSplit;
/* (the game's thread, when it hands a frame to a drawing thread) Take the
 * engine's registers, palettes and OAM as they are now, for the next draw
 * with split->snapshot set: what the game changes while it's being drawn
 * doesn't get mixed in. */
void NitroPort_Render2DTakeState(int engine);
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

/* The OBJ groups of an engine's picture (for wide art: the OBJs of one
 * priority and one palette, the 8-bit ones of a priority together; not
 * the window or bitmap OBJs): each group's number (priority << 5 |
 * palette, or 16 for 8-bit), its key (a fingerprint of its OBJs' pixels and
 * colours, not where they are: NitroPort_WideArtFind), its priority, where
 * it is (OAM's x 0-511 and y 0-255 of one of its OBJs: its art moves with
 * it) and how many OBJs. How many groups went to groups (at most max). */
typedef struct NitroObjGroup {
    uint64_t key;
    int group, prio, x, y, count;
} NitroObjGroup;
int NitroPort_Render2DObjGroups(int engine, const NitroRender2DSplit *layout, NitroObjGroup *groups, int max);
/* One OBJ group alone, as NitroPort_Render2DLayer draws a BG (its art in
 * the bands where it has one). 0 if none of it shows. Not on the drawing
 * threads (one at a time). */
int NitroPort_Render2DObjLayer(int engine, int group, const NitroRender2DSplit *layout, uint16_t *out);
/* The pictures a screen's layout draws (NITRO_WIDE_PICTURE parts whose
 * files are there): which (NitroPort_WidePictureName), with which BG and
 * that BG's priority, the picture's top-left on the canvas and size, how
 * far it's turned. How many went to out (at most max). */
typedef struct NitroWidePictureShown {
    int picture, bg, prio, x, y, w, h, angle;
} NitroWidePictureShown;
int NitroPort_Render2DPictures(int engine, const NitroRender2DSplit *layout, NitroWidePictureShown *out, int max);
/* The art folder's pictures of the layers an engine shows now (its BGs',
 * its OBJ groups'), not shared yet, written into the folder's share/
 * without the game's art (nitro_port_wideart.h): each pixel the same as
 * the layer's own, where the picture was made, left out (NITRO_WIDE_ART_GAME
 * for a BG's: the renderer puts the player's own game's back; nothing for
 * an OBJ group's: its sprites show there anyway), and a picture of the
 * bands only, nothing in the DS's part. A BG laid out in parts only where
 * its picture was made. How many were written. The game's thread, between
 * frames (as NitroPort_WideArtCapture). */
int NitroPort_Render2DShareArt(int engine);

#ifdef __cplusplus
}
#endif
#endif
