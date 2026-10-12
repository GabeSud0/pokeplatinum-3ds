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

/* Debug-only PNG output for the newest finished RGB555|coverage image.
 * Pixel order is always DS screen order (top-to-bottom, left-to-right).
 * DumpLatest acquires/releases the shared image, so it is concurrency-safe.
 * Returns 1 for success, 0 for missing image or an I/O failure. */
int NitroPort_Gx3dWritePng(const char *path, const uint16_t *image);
int NitroPort_Gx3dDumpLatest(const char *path);

/* Update read-only GXSTAT, matrix and test registers. The FIFO executes
 * synchronously for now, so the FIFO-empty/under-half IRQ stays level-based. */
void NitroPort_Gx3dRefreshStatus(void);

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

#ifdef __cplusplus
}
#endif
#endif
