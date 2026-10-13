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

typedef struct NitroGx3dScene {
    uint32_t count, dropped, serial;
    /* W-buffer/Z-buffer mode belongs to the completed frame (SWAP_BUFFERS),
     * not to polygons submitted before the swap command. */
    uint32_t swap_param;
    NitroGx3dTriangle triangles[NITRO_GX3D_MAX_TRIANGLES];
} NitroGx3dScene;

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

/* Every 2D BG0 drawer uses this RGB555|coverage interface. The PC fills it
 * via the software rasterizer; the 3DS GPU worker publishes its latest
 * completed RGBA8 transfer, converted to this 16-bit representation.
 * Acquire must be paired with Release after drawing the full frame. */
const uint16_t *NitroPort_Gx3dAcquireImage(void);
void NitroPort_Gx3dReleaseImage(const uint16_t *image);
int NitroPort_Gx3dPublishRGBA8(const uint32_t *image, size_t pixels);
/* The 3D alpha (0-31) of each pixel of an acquired image, or NULL. On the
 * DS a 3D pixel with alpha 1-30 blends with the 2D layer under BG0. */
const uint8_t *NitroPort_Gx3dImageAlpha(const uint16_t *image);
/* Whether an acquired image has pixels of alpha 1-30 (worked out once,
 * when it's published). */
int NitroPort_Gx3dImageTranslucent(const uint16_t *image);
/* How many 3D images have been published: a new one when it changes. (On
 * the PC, images are drawn when acquired, so it changes with the scene.) */
uint32_t NitroPort_Gx3dImageCount(void);

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

/* Debugging (the PC runner's --gx-record): every call into the engine,
 * to replay it (tests/gx3d_replay.c). Types: 1 GXFIFO words, 3 a direct
 * command (opcode, value), 4 ClearStackError,
 * 5 SyncResults, 6 RefreshStatus, 7 Reset. */
typedef void (*NitroGx3dRecorder)(int type, const uint32_t *words, size_t count);
void NitroPort_Gx3dSetRecorder(NitroGx3dRecorder recorder);

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
