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
/* Edge marking: each outline pass draws only the triangles it needs (on:
 * `edges pruned`), or every one (off, the default: on a 3DS working them out
 * cost the CPU more than it saved the GPU). */
void NitroPort_PicaEdgePruning(int on);
/* Since last asked: the time working out those triangles (ms, each time),
 * how many times (and failed: every triangle drawn then), and the share of
 * the outline passes' triangles drawn in the moved passes (A) and the
 * outlines drawn (D), in percent. */
void NitroPort_PicaPruneStats(float *ms, unsigned *runs, unsigned *fails, float *a, float *d);
/* Edge marking: the left and right moved passes' buffers copied from the
 * screen's depth/stencil by the transfer engine (on, the default), or drawn
 * (off: `edges drawn`). */
void NitroPort_PicaEdgeCopy(int on);
/* For a frame, after its scene is prepared (edges: edge marking on this
 * frame): the directions (bits) whose edge buffers will be copied (the
 * caller doesn't fill those, and clears the screen's depth/stencil to the
 * edge buffers' value), or 0. */
unsigned NitroPort_PicaEdgeCopies(int edges);
/* The command list cut here, always (on a profiled frame: a part named
 * `part`, as NitroPort_PicaProfileMark). */
void NitroPort_PicaCut(const char *part);
/* The remaster's frames (port_compose_pica.c), built while the GPU draws
 * the one before: two command lists in turn (NitroPort_PicaFramesInit;
 * 0 if there's no memory for them). Open a frame's list; cut it (into a
 * GX command list, to the queue bound); close it (the mark: sFrame then).
 * Tell when a frame's queue runs, and when it's done (the textures it
 * drew may change then); wait: what waits for the frame running (a
 * texture it may read is about to change). */
int NitroPort_PicaFramesInit(void);
void NitroPort_PicaFramesFree(void);
void NitroPort_PicaFrameOpen(void);
void NitroPort_PicaFrameSplit(void);
u32 NitroPort_PicaFrameClose(void);
void NitroPort_PicaFrameRunning(u32 mark);
void NitroPort_PicaFrameDone(u32 mark);
void NitroPort_PicaGpuWait(void (*wait)(void));
/* A shader blob's DVLE (its shaders, in the order assembled), as libctru
 * lays them out (port_pica_abi.c: its enums are a different size from the
 * port's); NULL past the last. dvlb: a DVLB_s *. And libctru's DVLE_s's
 * size, for the log. */
void *NitroPort_PicaDvle(void *dvlb, unsigned index);
unsigned NitroPort_PicaDvleSize(void);
/* A C3D_FrameBuf in libctru's layout, likewise (16 bytes there, 24 in the
 * port: C3D_SetFrameBuf read the port's as garbage): a render target's
 * (a C3D_RenderTarget *) with another D24S8 depth/stencil buffer, into out
 * (outSize bytes: PICA_FRAMEBUF_BYTES is enough). Point at it as a
 * C3D_FrameBuf *; never read its fields past width and height. */
#define PICA_FRAMEBUF_BYTES 32
unsigned NitroPort_PicaFrameBufSize(void);
int NitroPort_PicaFrameBufWithDepth(void *out, unsigned outSize, const void *target, void *depth);
/* The same of buffers the caller has: RGBA8 (rgb565: RGB565) colour, D24S8
 * depth/stencil, width x height (the framebuffer's: 240 across a screen's
 * height). */
int NitroPort_PicaFrameBufMake(void *out, unsigned outSize, void *color, void *depth, unsigned width,
                               unsigned height, int rgb565);
/* Where the display thread's drawing is (for the log when it hangs): the
 * stage set last (any thread sets one), and the last draw's first vertex,
 * count and the draws so far. */
void NitroPort_PicaStage(const char *stage);
const char *NitroPort_PicaStageNow(unsigned *first, unsigned *count, unsigned *draws);
/* GPU timings (port_compose_pica.c): on a profiled frame (one every five
 * seconds or so with a 3D scene), the command list is cut at each mark
 * and the GPU's completion of each part timed, into [gpu] log lines. A
 * mark names the part just drawn; nothing happens on other frames. The
 * display thread's. */
void NitroPort_PicaProfileMark(const char *part);
/* The memory left (both heaps), as a [perf] memory line. Any thread. */
void NitroPort_PicaLogMemory(void);
void NitroPort_PicaNoteCmdUse(void);
/* How 3D textures are sampled from now on: 0 nearest (as the DS), 1
 * bilinear, 2 sharp (each texel 2x2, then bilinear: as the remaster draws
 * the 2D). The compose path sets it from its filter. */
void NitroPort_PicaTextureFilter(int mode);
/* The remaster's look on the 3D (port_shadow.c): sun shadows on or off,
 * the colour grade on or off, and the sun: by the clock (the time of day,
 * as the game's), or fixed (degrees above the ground, and from the
 * screen's left, 0, towards its top, 90). */
void NitroPort_PicaLook(int shadows, int grade, int fixedSun, float elevation, float azimuth);
/* Debug only: submit and wait between shadow passes. Watchdog stages then
 * identify the outstanding GPU work, rather than the last CPU submission. */
void NitroPort_PicaShadowDebug(int enabled);

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
    int part;                /* PICA_DRAW_*: what of the scene to draw */
    /* the same frame's scene again (another part of the screen, its own
     * place: an H-blank effect's lines): its textures as they were checked
     * for the first */
    int again;
    /* only these of the framebuffer's pixels (C3D_SetScissor's left, top,
     * right, bottom: an H-blank effect's run of lines); 0: all of them */
    int scissor;
    u32 scissorRect[4];
    /* only the screen's rows bandTop .. bandBottom - 1 (band: an H-blank
     * effect's run of lines): the viewport just them, so the GPU clips the
     * polygons to them and draws nothing elsewhere (the scissor alone
     * still had each run's polygons gone through everywhere: a battle's
     * start took 62 ms of the GPU a frame, 2026-10-21) */
    int band, bandTop, bandBottom;
} PicaView;
/* What NitroPort_PicaDrawScene draws: all of it; or, for the smooth 3D
 * with the outlines (port_compose_pica.c), first the opaque polygons alone
 * into the 2x2 framebuffer, then (once that's scaled down into the
 * screen's) their depth and polygon IDs alone, their outlines and the
 * translucent polygons into the screen's own. */
enum {
    PICA_DRAW_ALL,
    PICA_DRAW_OPAQUE,
    PICA_DRAW_AFTER
};

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
    int edges;               /* edge marking (the game's outlines) on the GPU: 1, or 2 with each pass
                              * only the polygons it needs (NitroPort_PicaEdgePruning), or 3 with all
                              * their moved passes drawn (NitroPort_PicaEdgeCopy off) */
    int smooth3d;            /* the upper screen drawn at twice the resolution each way and scaled
                              * down (no jagged edges): 0 never, 1 with the 3D slider up, 2 always
                              * (with the slider down the outlines go on at its own resolution) */
    int edges3d;             /* the outlines with the 3D slider up too (off by default: each eye's
                              * outlines cost as much as the rest of its 3D, and the depth makes the
                              * shapes stand apart anyway) */
    int flipV;               /* (if the 2D comes out upside down) */
    int filter3d;            /* the 3D's textures: 0 nearest (the DS's), 1 bilinear, 2 sharp (2x2, bilinear),
                              * 3 xbrz (2x by xBRZ, nearest), 4 xbrz smooth (bilinear) */
    int shadows;             /* sun shadows on the 3D */
    int shadowDebug;         /* waits between passes to locate a hardware hang */
    int grade;               /* the colour grade on the 3D's textures */
    int sunFixed;            /* the sun where sunElevation, sunAzimuth say, not by the clock */
    float sunElevation, sunAzimuth;   /* (degrees; NitroPort_PicaLook) */
} ComposeConfig;

int NitroPort_ComposeInit(const ComposeConfig *config);
void NitroPort_ComposeExit(void);
void NitroPort_ComposeSetSmooth3d(int smooth3d);

/* The 2D layers of an engine's picture: the drawer's working buffers
 * (owned by the compositor; one set per engine), filled by the 2D renderer
 * (NitroPort_Render2DSplit), then published: upscaled into a texture the
 * GPU draws from. Publishing is the drawer's work (on its own core). */
typedef struct ComposeLayers {
    uint16_t *under;         /* width x height */
    uint16_t *over;          /* width x 192 (when split.has3d) */
    uint8_t *overAlpha;      /* width x 192 */
    int width;               /* the picture's width: 256, or up to 320 (the wider upper screen) */
    /* its rows: 192, or NITRO_LOWERMENU_TALL_LINES (the lower screen as a
     * tall panel: nitro_port_lowermenu.h; the DS's width then) */
    int height;
    /* NitroRender2DSplit, as the 2D renderer filled it (declared in
     * nitro_port.h, which can't be included next to 3ds.h): the fields the
     * compositor needs, copied by the caller. */
    int has3d, scroll3d, tint3d, eva, evb, evy, masterBright, backdrop;
    /* (an H-blank effect) scroll3d on each of the 192 lines, when it
     * differs from line to line (the battles' start slides the 3D's lines
     * apart; a wavy screen) */
    int scroll3dLines;
    short scroll3dLine[192];
    int displayOff;          /* nothing drawn (the engine is off) */
    uint32_t stamp;          /* the V-blank count when the game's display state was read */
    /* A BG left out of the picture for the compositor to draw itself
     * (NitroRender2DSplit.gpuBgLeftOut: the field's weather), or -1: its
     * blending onto the 3D (eva, evb: 0-16), its scroll when the picture
     * was drawn (the 3D scene's are newer: NitroGx3dScene.bg_ofs), and its
     * picture's key (NitroPort_Render2DBgPicture made again when it
     * changes). */
    int gpuBg, gpuBgEva, gpuBgEvb, gpuBgHofs, gpuBgVofs;
    uint64_t gpuBgKey;
    /* (the drawer's) its picture (NitroPort_Render2DBgPicture: 256 x 256
     * RGBA5551), made as the game's state was read, when the key changed */
    const uint16_t *gpuBgPicture;
} ComposeLayers;
/* Whether the compositor can draw a BG itself (its picture's textures
 * made: NitroRender2DSplit.gpuBgs). Any thread. */
int NitroPort_ComposeDrawsBgs(void);
/* (the game's thread, each frame it hands over: after its V-blank work)
 * Engine A's BG scrolls in the frame of this V-blank count: what a BG the
 * compositor draws itself is scrolled by with the 3D shown in it (a scene
 * of stamp S: the frame S + 1's, as the DS has it). */
void NitroPort_ComposeNoteBgOffsets(uint32_t stamp, const uint16_t ofs[8]);
ComposeLayers *NitroPort_ComposeLayersOf(int engine);
/* Into the textures of the screen (0 upper, 1 lower) the engine is on. */
/* 0: no set free to publish into (one the GPU's drawing, one the frame
 * being built, the newest): draw it all again next time. */
int NitroPort_ComposePublish(int engine, int screen);
/* How many times each engine's layers have been published. */
uint32_t NitroPort_ComposePublished(int engine);

/* Draw and show whatever changed (new layers, a new scene, the slider,
 * the engines swapping screens): returns the screens drawn (bit 0 upper,
 * bit 1 lower), 0 if nothing was. scene: the newest 3D scene (prepared
 * here), or NULL. A new scene waits for the 2D it's shown with; flush:
 * not any longer (the next scene has come). Waits for the GPU. */
int NitroPort_ComposeFrame(const NitroGx3dScene *scene, uint32_t sceneSerial, int aOnTop,
                           float slider, int forced, int flush, int mid);
/* (its bits: the screens drawn, and when a wanted frame wasn't, why) */
#define COMPOSE_DRAWN_UPPER 1
#define COMPOSE_DRAWN_LOWER 2
#define COMPOSE_NO_BUFFER_UPPER 4   /* no LCD framebuffer of the screen's free: the next V-blank frees one, draw it again then */
#define COMPOSE_NO_BUFFER_LOWER 8
#define COMPOSE_MID_LATE 16         /* the in-between scene's V-blank has come: its scene's turn */
/* (mid: scene is the in-between scene of the scene of serial sceneSerial
 * (NitroPort_Gx3dMidOf), shown a V-blank before it; draw it first.) Whether
 * in-between scenes fit now, the slider and the screens as they are (60
 * frames a second: the GPU has 16.7 ms a frame; the 3D drawn more cheaply,
 * then off, while frames take longer: port_compose_pica.c auto_fps60). */
int NitroPort_ComposeWantsMid(float slider, int aOnTop);
/* The look (sun shadows and the colour grade, NitroPort_PicaLook) on or
 * off from the next frame drawn. Any thread (L + R + B). */
void NitroPort_ComposeLook(int on);

/* Whether the lower screen was last drawn as the port's menu
 * (nitro_port_lowermenu.h): its touches go through the menu then, else to
 * the DS's screen filling the 3DS's. Any thread. */
int NitroPort_ComposeLowerMenuShown(void);
/* Whether that scene is waiting for its 2D (not drawn yet). */
int NitroPort_ComposeSceneWaiting(uint32_t sceneSerial);
/* For the [perf] line: frames drawn, the time building them (CPU) and the
 * GPU time waited for, publishing (upscaling) by the drawers, the V-blanks
 * from a scene's making to its showing (lag), of the 3D frames (timed)
 * how many were late, the scenes never shown (overtaken), and the frames
 * drawn in stereo. Counts since the start. */
typedef struct ComposeStats {
    u32 frames, published, timed, late, dropped, stereo;
    u32 underFills;          /* screens drawn whose picture behind the 3D was all one colour (the clear) */
    u32 smooth;              /* frames whose upper screen was drawn 2x2 (smooth3d) */
    u32 smoothHalf;          /* and 2x1 (60 frames a second while 2x2 doesn't fit) */
    u64 waitTicks;           /* the display thread waiting for the GPU (to finish the frame before) */
    u32 stereoEdges;         /* frames drawn in stereo with outlines (they're left out while they don't fit) */
    u32 midFrames;           /* frames of in-between scenes (60 frames a second) */
    u32 midLate;             /* of them, late for their V-blank (shown at their scene's, or left out) */
    u64 gpuTicks, cpuTicks, publishTicks;
    int lag;
} ComposeStats;
void NitroPort_ComposeStats(ComposeStats *stats);
/* The GPU still drawing a frame (it's given one and goes on with it while
 * the next is built): what draws on the screens itself waits for it. */
int NitroPort_ComposeGpuBusy(void);
/* A V-blank happened: the frames due go onto the LCD (the V-blank thread
 * calls this, each V-blank); and how many V-blanks there have been (any
 * thread). */
void NitroPort_ComposeVBlank(void);
/* Nothing put on the LCD while on (the HOME menu has the screens, or the
 * program is closing); the frames waiting are dropped. */
void NitroPort_ComposeHold(int on);
u32 NitroPort_ComposeVBlanks(void);
/* Engine A's 2D (the 3D's screen), for pairing a new scene with it: the
 * game offered a frame of it (stamp: the V-blank count), the drawer was
 * handed one, the drawer is done with one (published, or unchanged). */
void NitroPort_Compose2DOffered(u32 stamp);
void NitroPort_Compose2DHanded(u32 stamp);
void NitroPort_Compose2DDone(u32 stamp);
/* A screen's (0 upper, 1 lower) newest published picture is the game's as
 * of the frame at stamp: published then, or unchanged since (not when its
 * publish found no set free). For the lower menu's waits for a picture
 * made since (NitroPort_LowerMenuPanelShown, UpperLetGoShows). */
void NitroPort_Compose2DCurrent(int screen, u32 stamp);
#endif
#endif
