/*
 * DS geometry-engine command capture. Renderer independent: these clipped,
 * screen-space triangles will also be the input to the future 3DS GPU path.
 * No game assets, NitroSDK headers or libctru are needed to consume a scene.
 */
#ifndef NITRO_PORT_GX3D_H
#define NITRO_PORT_GX3D_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NITRO_GX3D_WIDTH 256
#define NITRO_GX3D_HEIGHT 192
#define NITRO_GX3D_MAX_TRIANGLES 4096u
#define NITRO_GX3D_MAX_POLYGONS 2048u
#define NITRO_GX3D_MAX_VERTICES 6144u
/* Triangles kept for the sun shadows only (NitroGx3dScene.shadow_triangles). */
#define NITRO_GX3D_MAX_SHADOW_TRIANGLES 8192u

typedef struct NitroGx3dVertex {
    /* Projection-space homogeneous coordinates, signed 20.12 fixed point. */
    int32_t clip[4];
    /* Screen coordinates (16.8), depth 0..0xFFFFFF, reciprocal W for
     * perspective-correct interpolation, and texture S/T in 1/16 texel. */
    int32_t x, y;
    uint32_t depth;
    float inv_w, s, t;
    uint16_t color;               /* DS RGB555 */
} NitroGx3dVertex;

typedef struct NitroGx3dTriangle {
    NitroGx3dVertex v[3];
    uint32_t polygon_attr;
    uint32_t texture_param;
    uint32_t palette_base;
    /* The DS's drawing order key of the polygon it belongs to: bit 16 set
     * for a translucent polygon, then its bottom and top screen lines
     * ((bottom << 8) | top). See NitroPort_Gx3dDrawOrder. */
    uint32_t sort_key;
} NitroGx3dTriangle;

#define NITRO_GX3D_REGS_AT 0x320u
#define NITRO_GX3D_REGS_SIZE 0xA0u
typedef struct NitroGx3dScene {
    uint32_t count, dropped, serial;
    /* W-buffer/Z-buffer mode belongs to the completed frame (SWAP_BUFFERS),
     * not to polygons submitted before the swap command. */
    uint32_t swap_param;
    /* The view's width this scene was made for (NitroPort_Gx3dSetWideView):
     * 256, or the screen's width when the view is wider. */
    uint32_t screen_width;
    /* The projection matrix at its SWAP_BUFFERS (20.12, rows as the DS's:
     * clip = position x projection): the camera's, in the field, which
     * every polygon of the scene went through (sun shadows, port_shadow.c,
     * take the vertices back to the camera's space with it). */
    int32_t projection[16];
    /* When the game ended it: NitroPort_Gx3dStamp() at its SWAP_BUFFERS
     * (the platform's clock; the 3DS's counts V-blanks). */
    uint32_t stamp;
    /* The 3D's registers as the game had them at its SWAP_BUFFERS (on the
     * game's thread): DISP3DCNT, and 0x4000320-0x40003BF (the edge
     * colours, the alpha test, the clear colour and depth, the fog, the
     * toon table) at regs3d[offset - NITRO_GX3D_REGS_AT]. What draws the
     * scene later reads them here, not as they are by then (the game may
     * have changed them for its next scene). */
    uint32_t disp3dcnt;
    uint8_t regs3d[NITRO_GX3D_REGS_SIZE];
    NitroGx3dTriangle triangles[NITRO_GX3D_MAX_TRIANGLES];
    /* With the sun shadows on (NitroPort_SunShadows), the opaque triangles
     * that aren't drawn but can still cast a shadow into the view: those
     * facing away from the camera (culled), and those outside the view's
     * sides or crossing them (whole, not clipped; a crossing one is also
     * among triangles, clipped), up to one and a half views beyond them,
     * in front of the camera. Their vertices' screen x and y are as
     * to_screen gives them (off the screen for most), the rest as in
     * triangles; sort_key 0. Kept apart (NITRO_GX3D_MAX_SHADOW_TRIANGLES
     * of them), made when the shadows are first turned on: NULL before,
     * with shadow_count 0 (most players never turn them on, and the 3DS's
     * memory is tight). */
    uint32_t shadow_count;
    NitroGx3dTriangle *shadow_triangles;
} NitroGx3dScene;

/* In-between scenes (60 frames a second in the field; port_gx3d.c): with
 * them on, each scene's polygons are kept as the game drew them (before
 * clipping), and at each SWAP_BUFFERS a scene halfway between the one
 * before and the new one is made, each polygon matched with the same one
 * of the scene before and its vertices halfway. On from the next
 * SWAP_BUFFERS (any thread); off by default, at no cost. */
void NitroPort_Gx3dSetInterpolation(int on);
int NitroPort_Gx3dInterpolation(void);
/* The newest in-between scene (between the newest scene and the one
 * before it), or NULL (off, the first scene, or too little in common: a
 * cut). matched/polygons: the newest scene's polygons placed halfway, of
 * all it has. The geometry thread's (single-threaded inspection: the PC
 * runner). */
const NitroGx3dScene *NitroPort_Gx3dMidScene(unsigned *matched, unsigned *polygons);
/* An acquired scene's in-between scene (between it and the scene before
 * it; made before it was published, with it), or NULL: as stable as the
 * scene, till it's released. */
const NitroGx3dScene *NitroPort_Gx3dMidOf(const NitroGx3dScene *scene);

/* Reset engine and scene state, without clearing the 2D registers. */
void NitroPort_Gx3dReset(void);

/* Direct opcode/register writes, or a 32-bit word of the packed GXFIFO.
 * The SDK direct-call interception and the port's DMA driver use these. */
void NitroPort_GxDirectWrite(int opcode, uint32_t value);
void NitroPort_GxFifoWord(uint32_t value);
/* Several GXFIFO words in a row (a DMA or CPU send to the FIFO). */
void NitroPort_GxFifoWords(const uint32_t *words, size_t count);

/* The completed display scene (published on SWAP_BUFFERS).
 * Legacy single-threaded inspection, for synthetic tests ONLY: never retain
 * this pointer across SWAP. The 3DS worker must acquire/release instead. */
const NitroGx3dScene *NitroPort_Gx3dScene(void);

/* `*serial` is the last scene consumed. Acquire returns NULL if no newer
 * scene is available. Only one scene consumer at a time (GPU on 3DS or PC
 * reference renderer); the pointer remains stable until Release. SWAP
 * never overwrites the acquired scene. No whole-scene copies are made. */
const NitroGx3dScene *NitroPort_Gx3dAcquireScene(uint32_t *serial);
void NitroPort_Gx3dReleaseScene(const NitroGx3dScene *scene);
/* Whether a scene newer than the one last acquired (its serial) is out: a
 * consumer that keeps the acquired scene (to draw it again) can tell when
 * to release it and acquire the next. */
int NitroPort_Gx3dNewerScene(uint32_t serial);
/* How many newer than that have been made (the newest takes the place of
 * any not yet taken). */
unsigned NitroPort_Gx3dNewerScenes(uint32_t serial);
/* The clock scenes are stamped with (NitroGx3dScene.stamp): weak, 0 unless
 * the platform has one. Called on the game's thread. */
uint32_t NitroPort_Gx3dStamp(void);

/* Every 2D BG0 drawer uses this RGB555|coverage interface. The PC fills it
 * via the software rasterizer; the 3DS GPU worker publishes its latest
 * completed RGBA8 transfer, converted to this 16-bit representation.
 * Acquire must be paired with Release after drawing the full frame. */
const uint16_t *NitroPort_Gx3dAcquireImage(void);
void NitroPort_Gx3dReleaseImage(const uint16_t *image);
int NitroPort_Gx3dPublishRGBA8(const uint32_t *image, size_t pixels);

/* Edge marking (DISP3DCNT bit 5; see port_gx3d_edge.c): its registers. */
typedef struct NitroGx3dEdgeSetup {
    int enabled;             /* DISP3DCNT bit 5 */
    int antialias;           /* DISP3DCNT bit 4: outlines half blended */
    uint16_t colors[8];      /* EDGE_COLOR, by polygon ID / 8 (RGB555) */
    uint8_t clearId;         /* the rear plane's polygon ID (CLEAR_COLOR) */
    uint16_t clearColor;     /* its colour (RGB555) */
    uint8_t clearAlpha;      /* and alpha, 0-31 */
    uint32_t clearDepth;     /* its depth, 24 bits (CLEAR_DEPTH) */
} NitroGx3dEdgeSetup;
void NitroPort_Gx3dEdgeSetup(NitroGx3dEdgeSetup *setup);
/* The same from a scene's registers (NitroGx3dScene.disp3dcnt, regs3d). */
void NitroPort_Gx3dEdgeSetupFrom(NitroGx3dEdgeSetup *setup, uint32_t disp3dcnt, const uint8_t *regs3d);
/* Outline a finished image: image is RGB555 | 0x8000 (drawn), alpha 0-31
 * per pixel; depthId per pixel: depth (24 bits, smaller is nearer) | the
 * opaque polygon ID there << 24, or 64-127 or 255 for none (64-127: the
 * GPU path's last translucent ID). *translucent (if not NULL) is set if
 * an outline made a pixel partly transparent. Returns how many pixels it
 * outlined. */
unsigned NitroPort_Gx3dMarkEdges(uint16_t *image, uint8_t *alpha, const uint32_t *depthId,
                                 const NitroGx3dEdgeSetup *setup, int *translucent);
/* Images outlined (converted with edge marking on) and the pixels outlined
 * in them, so far (the [perf] line; both wrap round). */
void NitroPort_Gx3dEdgeStats(uint32_t *images, uint32_t *pixels);
/* How many pixels of depthId have an ID byte 128-254: none should (the
 * GPU path's depth/stencil buffer isn't read back as expected otherwise). */
unsigned NitroPort_Gx3dEdgeIdsUnexpected(const uint32_t *depthId);
/* Publish with edge marking: depthId as for NitroPort_Gx3dMarkEdges, in
 * the image's pixel order. */
int NitroPort_Gx3dPublishRGBA8Edges(const uint32_t *image, size_t pixels,
                                    const uint32_t *depthId, const NitroGx3dEdgeSetup *setup);
/* The 3D alpha (0-31) of each pixel of an acquired image, or NULL. On the
 * DS a 3D pixel with alpha 1-30 blends with the 2D layer under BG0. */
const uint8_t *NitroPort_Gx3dImageAlpha(const uint16_t *image);
/* Whether an acquired image has pixels of alpha 1-30 (worked out once,
 * when it's published). */
int NitroPort_Gx3dImageTranslucent(const uint16_t *image);
/* How many 3D images have been published or handed over: a new one when
 * it changes. (On the PC, images are drawn when acquired, so it changes
 * with the scene.) */
uint32_t NitroPort_Gx3dImageCount(void);
/* Hand a finished RGBA8 image (and its depth/IDs for edge marking, or
 * NULL) over without converting it: the next NitroPort_Gx3dAcquireImage
 * does (on the caller's core), as NitroPort_Gx3dPublishRGBA8Edges would.
 * The buffers are the caller's: not to be drawn into again while
 * NitroPort_Gx3dRawInUse says so (waiting, or being converted). A newer
 * image replaces one still waiting. */
int NitroPort_Gx3dSubmitRGBA8(const uint32_t *image, const uint32_t *depthId,
                              const NitroGx3dEdgeSetup *edges);
int NitroPort_Gx3dRawInUse(const uint32_t *image);
/* Convert the image handed over, if there's one, now rather than when the
 * 3D is next drawn: for a drawer with a core of its own, between frames.
 * Returns 1 if it converted one. (By one thread at a time, as
 * NitroPort_Gx3dAcquireImage: the 3D screen's drawer.) */
int NitroPort_Gx3dConvertPending(void);

/* Debug-only PNG output for the newest finished RGB555|coverage image.
 * Pixel order is always DS screen order (top-to-bottom, left-to-right).
 * DumpLatest acquires/releases the shared image, so it is concurrency-safe.
 * Returns 1 for success, 0 for missing image or an I/O failure. */
int NitroPort_Gx3dWritePng(const char *path, const uint16_t *image);
int NitroPort_Gx3dDumpLatest(const char *path);

/* Update read-only GXSTAT, matrix and test registers. The FIFO executes
 * synchronously for now, so the FIFO-empty/under-half IRQ stays level-based. */
/* Brings the clip and vector matrix result registers (0x640, 0x680) up to
 * date; the SDK's G3X_GetClipMtx/G3X_GetVectorMtx call it first. */
void NitroPort_Gx3dSyncResults(void);
void NitroPort_Gx3dRefreshStatus(void);

/* A thread of its own for the geometry engine. The game's thread then
 * only decodes the commands and keeps a copy of the matrices (all the
 * game reads back); the thread does the vertices, lighting, clipping and
 * scenes. The platform starts the thread, which runs
 * NitroPort_Gx3dThreadMain until *running is 0 (then wake it), after
 * NitroPort_Gx3dStartThread and before any geometry command. */
typedef struct NitroGx3dThreadOps {
    void *context;
    void (*wait_worker)(void *context);   /* the thread sleeps until woken */
    void (*wake_worker)(void *context);
    void (*wait_producer)(void *context); /* the game's thread, the same */
    void (*wake_producer)(void *context);
    uint64_t (*ticks)(void *context);     /* a clock (optional): busy time */
    /* Never leave out a scene the thread is behind on: the game waits
     * instead (the same scenes as without the thread, for comparing). */
    int keep_every_scene;
} NitroGx3dThreadOps;
void NitroPort_Gx3dStartThread(const NitroGx3dThreadOps *ops);
void NitroPort_Gx3dThreadMain(const volatile int *running);
/* Back to the caller's thread, once the thread has done what it was given
 * (then stop the thread). For a thread that couldn't be started, too. */
void NitroPort_Gx3dStopThread(void);
/* Hand the thread what has been written so far (once a frame). */
void NitroPort_Gx3dFlush(void);
/* The thread's busy time (in ticks), scenes finished, and scenes left out
 * because it was behind (the game never waits for it). */
void NitroPort_Gx3dThreadStats(uint64_t *busy, uint32_t *scenes, uint32_t *skipped);
/* The game's thread's time waiting for the geometry thread (its ring full,
 * or results it needs), in microseconds since the start. */
uint64_t NitroPort_Gx3dProducerWaitMicros(void);
/* How many times the thread has woken up to work (it sleeps whenever it has
 * done all it was handed), and its longest stretch of work, in ticks (both
 * for the [perf] line; the longest is reset when read). */
void NitroPort_Gx3dThreadWakeups(uint32_t *wakeups, uint64_t *longest);
/* Whether a scene the game ended before its newest SWAP_BUFFERS may still
 * be read (the 3DS's GPU thread reads the texture VRAM as it draws one):
 * the geometry thread hasn't finished it, or the consumer is reading it or
 * hasn't taken it yet. Texture VRAM changed now would show in it. Never
 * without the geometry thread. */
int NitroPort_Gx3dEarlierSceneUnread(void);

/* Debugging (the PC runner's --gx-record): every call into the engine,
 * to replay it (tests/gx3d_replay.c). Types: 1 GXFIFO words, 3 a direct
 * command (opcode, value), 4 ClearStackError,
 * 5 SyncResults, 6 RefreshStatus, 7 Reset. */
typedef void (*NitroGx3dRecorder)(int type, const uint32_t *words, size_t count);
void NitroPort_Gx3dSetRecorder(NitroGx3dRecorder recorder);

/* A wider view, for a screen screenWidth DS pixels wide (the 3DS's upper
 * screen: 320, its 400 pixels at the size the 256 take up on it): the clip
 * x of every vertex is scaled by 256 / screenWidth before clipping, so the
 * picture shows more of the world at the sides, and the game's own box
 * tests pass what that shows. What was on screen stays where it was: the
 * display stretches the picture back by the same amount. 256 (the DS's
 * width) turns it off. Not in a viewport narrower than the screen. Takes
 * effect from the next command (with the geometry thread, at that point of
 * its stream); each scene says what it was made for (screen_width). */
void NitroPort_Gx3dSetWideView(int screenWidth);

/* GXSTAT's bit 15 is write-one-to-clear on the DS, and resetting it
 * also clears the single-entry projection stack pointer. */
void NitroPort_Gx3dClearStackError(void);

/* PC reference output. Samples from virtual DS texture/palette VRAM banks.
 * Each pixel is RGB555|0x8000 when a 3D polygon covers it, otherwise zero.
 * The 2D BG0 compositor decides whether/how to display it. */
void NitroPort_Gx3dRasterize(uint16_t *rgba555, uint32_t *depth_out);
/* Internal PC handoff: rasterize the scene while the consumer owns its slot. */
void NitroPort_Gx3dRasterizeScene(const NitroGx3dScene *scene,
                                  uint16_t *rgba555, uint32_t *depth_out);
/* The same, also giving each pixel's 3D alpha (0-31) when alpha_out isn't NULL. */
void NitroPort_Gx3dRasterizeSceneAlpha(const NitroGx3dScene *scene, uint16_t *rgba555,
                                       uint8_t *alpha_out, uint32_t *depth_out);

#ifdef __cplusplus
}
#endif
#endif
