/*
 * The 3DS program's GPU side, shared by port_gx3d_pica.c (the DS's
 * polygons) and port_compose_pica.c (the screens put together on the GPU).
 * Everything here runs on the GPU thread only.
 */
#ifndef PORT_PICA_H
#define PORT_PICA_H
#ifdef __3DS__
#include <3ds.h>
#include <citro3d.h>
#include <nitro_port_gx3d.h>
#include <nitro_port_gx3d_vertex.h>

/* ---------------------------------------------------- the polygon drawer */

int NitroPort_PicaInit(void);
void NitroPort_PicaExit(void);
/* The classic path: a 256x192 image of the scene, read back and handed to
 * the 2D renderer (NitroPort_Gx3dSubmitRGBA8). */
int NitroPort_PicaRender(const NitroGx3dScene *scene, unsigned *dropped);
void NitroPort_PicaTimes(u64 *build, u64 *wait, u64 *publish);
void NitroPort_PicaTextureStats(u32 *converted, u64 *texels, u64 *ticks);
/* The GPU command list's peak use (0-1) and the draws left out for want of
 * room in it, since last asked; NoteCmdUse after each C3D_FrameEnd. */
void NitroPort_PicaCmdStats(float *peak, unsigned *cut);
void NitroPort_PicaNoteCmdUse(void);
/* How 3D textures are sampled from now on: 0 nearest (as the DS), 1
 * bilinear, 2 sharp (each texel 2x2, then bilinear: as the remaster draws
 * the 2D). The compose path sets it from its filter. */
void NitroPort_PicaTextureFilter(int mode);

/* The compose path: the scene's polygons drawn straight into a screen's
 * render target, between the 2D layers. */

/* Where the DS's screen goes on a 3DS screen: a rectangle in that screen's
 * pixels (x from the left, y from the top), on a screen width x height. */
typedef struct PicaRect {
    float x0, y0, x1, y1;
} PicaRect;

/* The vertex program's view matrix for polygons in the DS's clip space
 * (x and y in -w..w over the DS's screen), putting that screen at rect on a
 * rotated 240 x screenWidth render target. */
void NitroPort_PicaViewMatrix(C3D_Mtx *m, int screenWidth, int screenHeight, const PicaRect *rect);
/* The same for vertices given in the screen's pixels (x, y, z, 1). */
void NitroPort_PicaPixelMatrix(C3D_Mtx *m, int screenWidth, int screenHeight);
/* Set the vertex program's uniforms: the view matrix, and the stereoscopic
 * shift (strength in screen widths, signed for the eye; the convergence
 * distance in the DS's units of w). */
void NitroPort_PicaSetView(const C3D_Mtx *m, float strength, float convergence);
/* The vertex attribute layout (position, colour, texture coordinates) of
 * NitroGx3dGpuVertex, as the program takes it: a buffer of them bound. */
void NitroPort_PicaBindVertices(const void *vertices);

/* Prepare a scene's polygons for drawing (once per scene; a scene stays
 * prepared until the next): returns the triangle count, or -1. */
int NitroPort_PicaPrepareScene(const NitroGx3dScene *scene, unsigned *dropped);

typedef struct PicaView {
    C3D_FrameBuf *fb;        /* the screen's render target (colour, depth/stencil) */
    /* The same colour buffer with an edge marking depth/stencil buffer of
     * its own for each direction (left, right, up, down), each filled by
     * the caller with NitroPort_PicaEdgeClearValue before the scene is
     * drawn; NULL: no edge marking. */
    C3D_FrameBuf *edgeFb[4];
    int screenWidth, screenHeight;
    PicaRect rect;           /* where the DS's screen goes */
    float stereo;            /* the eye's shift, in screen widths (0: none) */
    float convergence;       /* the distance (w) at which there's no shift */
} PicaView;

/* Draw the prepared scene into view->fb (depth/stencil cleared by the
 * caller): the opaque polygons, their outlines (edge marking, when the
 * game has it on and view->edgeFb is given), the translucent ones. The
 * rear plane (the clear colour) is the caller's. */
void NitroPort_PicaDrawScene(const PicaView *view);
/* Whether the game has edge marking on, and what the edge marking
 * depth/stencil buffers are to be filled with first (D24S8: the farthest
 * depth, the rear plane's polygon ID). */
int NitroPort_PicaEdgeClearValue(u32 *value);
/* The prepared scene's rear plane: colour (RGB555), alpha 0-31, and
 * whether its pixels of alpha 1-30 blend (DISP3DCNT). */
void NitroPort_PicaRearPlane(uint16_t *color, unsigned *alpha);

/* ------------------------------------------------------- the compositor */

/* How the 2D pictures are scaled up (remaster.txt's filter setting). */
enum {
    COMPOSE_FILTER_SHARP,    /* each pixel 2x2 texels, then bilinear: the pixels as they are, edges a texel soft */
    COMPOSE_FILTER_NEAREST,  /* nearest: the pixels as they are, unevenly sized (1.25 times) */
    COMPOSE_FILTER_SMOOTH,   /* bilinear from the DS's pixels: blurry */
    COMPOSE_FILTER_SCALE2X   /* Scale2x (EPX), then bilinear: rounded diagonals */
};

/* What the screens are made of and how: see port_compose_pica.c. */
typedef struct ComposeConfig {
    int wide;                /* the upper screen's full width: the 2D's wider canvas and the 3D's wider view */
    int filter;              /* COMPOSE_FILTER_* */
    int stereo;              /* the 3D slider gives depth */
    float stereoStrength;    /* the shift at full slider, in screen widths */
    float convergence;       /* 0: worked out from each scene */
    int edges;               /* edge marking (the game's outlines) on the GPU */
    int flipV;               /* (if the 2D comes out upside down) */
} ComposeConfig;

int NitroPort_ComposeInit(const ComposeConfig *config);
void NitroPort_ComposeExit(void);

/* The 2D layers of an engine's picture: the drawer's working buffers
 * (owned by the compositor; one set per engine), filled by the 2D renderer
 * (NitroPort_Render2DSplit), then published: upscaled into a texture the
 * GPU draws from. Publishing is the drawer's work (on its own core). */
typedef struct ComposeLayers {
    uint16_t *under;         /* width x 192 */
    uint16_t *over;          /* width x 192 (when split.has3d) */
    uint8_t *overAlpha;      /* width x 192 */
    int width;               /* the picture's width: 256, or up to 320 (the wider upper screen) */
    /* NitroRender2DSplit, as the 2D renderer filled it (declared in
     * nitro_port.h, which can't be included next to 3ds.h): the fields the
     * compositor needs, copied by the caller. */
    int has3d, scroll3d, tint3d, eva, evb, evy, masterBright, backdrop;
    int displayOff;          /* nothing drawn (the engine is off) */
    uint32_t stamp;          /* the V-blank count when the game's display state was read */
} ComposeLayers;
ComposeLayers *NitroPort_ComposeLayersOf(int engine);
/* Into the textures of the screen (0 upper, 1 lower) the engine is on. */
void NitroPort_ComposePublish(int engine, int screen);
/* How many times each engine's layers have been published. */
uint32_t NitroPort_ComposePublished(int engine);

/* Draw and show whatever changed (new layers, a new scene, the slider,
 * the engines swapping screens): returns the screens drawn (bit 0 upper,
 * bit 1 lower), 0 if nothing was. scene: the newest 3D scene (prepared
 * here), or NULL. Waits for the GPU. */
int NitroPort_ComposeFrame(const NitroGx3dScene *scene, uint32_t sceneSerial, int aOnTop,
                           float slider, int forced);
/* For the [perf] line: frames drawn, the time building them (CPU) and the
 * GPU time waited for, publishing (upscaling) by the drawers, the V-blanks
 * from a scene's making to its showing (lag), and of the 3D frames (timed)
 * how many were late. Counts since the start. */
typedef struct ComposeStats {
    u32 frames, published, timed, late;
    u64 gpuTicks, cpuTicks, publishTicks;
    int lag;
} ComposeStats;
void NitroPort_ComposeStats(ComposeStats *stats);
/* A V-blank happened: the frames due go onto the LCD (the V-blank thread
 * calls this, each V-blank); and how many V-blanks there have been (any
 * thread). */
void NitroPort_ComposeVBlank(void);
/* Nothing put on the LCD while on (the HOME menu has the screens, or the
 * program is closing); the frames waiting are dropped. */
void NitroPort_ComposeHold(int on);
u32 NitroPort_ComposeVBlanks(void);
#endif
#endif
