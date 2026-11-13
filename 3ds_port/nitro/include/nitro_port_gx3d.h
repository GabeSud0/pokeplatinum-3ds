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
    /* (PIXEL PERFECT's sprites, port_gx3d_vertex.c) Which way the polygon
     * moves on the screen by a fraction of a pixel a frame, as the
     * in-between scenes found it (NITRO_GX3D_SOFT_X across, _Y down; 0:
     * still, a whole number of pixels, or not known): a sprite is placed on
     * half pixels that way, moving with the ground under it, instead of on
     * whole ones (where it fell behind the ground and caught up a pixel at
     * a time, a jiggle). */
    uint32_t soft;
    /* The game's mark on it (NitroPort_Gx3dTag: 0 none): whose polygon it
     * is, for the sun shadows (a character's sprite, its round shadow). */
    uint32_t tag;
} NitroGx3dTriangle;
#define NITRO_GX3D_SOFT_X 1u
#define NITRO_GX3D_SOFT_Y 2u

#define NITRO_GX3D_REGS_AT 0x320u
#define NITRO_GX3D_REGS_SIZE 0xA0u
#define NITRO_GX3D_MAX_TAGGED_TRIANGLES 2048u
typedef struct NitroGx3dScene {
    uint32_t count, dropped, serial;
    /* W-buffer/Z-buffer mode belongs to the completed frame (SWAP_BUFFERS),
     * not to polygons submitted before the swap command. */
    uint32_t swap_param;
    /* The view's width this scene was made for (NitroPort_Gx3dSetWideView):
     * 256, or the screen's width when the view is wider. */
    uint32_t screen_width;
    /* And its height (NitroPort_Gx3dSetView): 192, or taller. */
    uint32_t screen_height;
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
    /* And engine A's BG scrolls then (BG0-3's HOFS and VOFS, 0x4000010-
     * 0x400001F, as words of 9 bits): what a BG the compositor draws itself
     * is scrolled by in the frame with this 3D (NitroRender2DSplit.gpuBgs:
     * the field's weather). An in-between scene's: halfway, the shorter way
     * round the map, from the scene before's. (The game sets its scrolls
     * in the V-blank after the SWAP: these are a frame behind the 2D the
     * DS shows the scene with; the compositor has those, by stamp, and
     * these only when it hasn't.) */
    uint16_t bg_ofs[8];
    /* The stamp of the scene before an in-between scene's (its own stamp
     * is the newer scene's); a scene's own: its stamp. */
    uint32_t stamp_before;
    /* Which of the game's SWAP_BUFFERS it is (NitroPort_Gx3dSwapsMade then,
     * counting it), and for an in-between scene the scene before's: what
     * the game set as it made this scene shows from the next V-blank's
     * work, when NitroPort_Gx3dSwapsMade is this; that, not the V-blank
     * clock (stamp), says which frame's scrolls go with it (the game's
     * frames don't keep step with the 3DS's V-blanks). */
    uint32_t swap_seq, swap_seq_before;
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
    /* The marked polygons' triangles (NitroPort_Gx3dTag), whole: as the
     * game sent them, not clipped at the view's sides, and kept when they
     * are beyond them or face away (up to one and a half views beyond, in
     * front of the camera, inside the near and far planes). Their
     * vertices' screen x and y are as to_screen gives them (off the screen
     * for some); sort_key 0. A visible one is among triangles too, clipped.
     * An in-between scene's: halfway between the two scenes', mark by mark.
     * For the sun shadows: a character at the screen's edge, or just past
     * it, casts its whole shadow. */
    uint32_t tagged_count;
    NitroGx3dTriangle tagged_triangles[NITRO_GX3D_MAX_TAGGED_TRIANGLES];
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
/* (the game's thread) How many SWAP_BUFFERS the game has made: a frame's
 * display state handed over with this many made goes with the scene whose
 * NitroGx3dScene.swap_seq it is. */
uint32_t NitroPort_Gx3dSwapsMade(void);

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
/* Scenes left out as the same as the one before, word for word, while a
 * battle's start plays in the field (NitroPort_FieldEffect): counted on the
 * game's thread. */
uint32_t NitroPort_Gx3dSameScenesLeftOut(void);
/* The scene of mailbox serial `serial` (on the screen) stands for a later
 * frame too: the newest scene left out as the same as it (with the
 * geometry thread). Then 1, that scene's time (NitroGx3dScene.stamp) and
 * SWAP_BUFFERS count (swap_seq): the 2D the DS shows with that frame goes
 * with it, and that frame's brightness and scrolls. 0: none. */
int NitroPort_Gx3dSameAs(uint32_t serial, uint32_t *stamp, uint32_t *swapSeq);
/* A model's shape's display list (src, bytes) with only the polygons that
 * reach into a slab of its coordinates, lo <= x (axis 0), y (1) or z (2)
 * <= hi, into dst (dstBytes of room); the polygons kept are drawn the same,
 * bit for bit (what those left out leave behind is sent in their place).
 * Made once, not every scene (a map drawn again mirrored past the
 * scenery: the strip of it that can be seen). Its size in bytes; 0 if the
 * list can't be cut (or doesn't fit): draw the whole of it. */
uint32_t NitroPort_Gx3dTrimList(const void *src, uint32_t bytes, void *dst, uint32_t dstBytes,
                                int axis, int32_t lo, int32_t hi);
/* The same, with only the polygons that reach into xlo <= x <= xhi and
 * start (their lowest z) in zlo <= z < zhi (inside), or anywhere but
 * there (!inside): the two lists share no polygon, and have them all
 * between them (rows of a map drawn again two ways). 0 if it can't be cut
 * (or doesn't fit). */
uint32_t NitroPort_Gx3dTrimListRows(const void *src, uint32_t bytes, void *dst, uint32_t dstBytes,
                                    int32_t xlo, int32_t xhi, int32_t zlo, int32_t zhi, int inside);
/* A shape's display list read once (NULL if it can't be), for: */
typedef struct NitroGx3dListSurvey NitroGx3dListSurvey;
NitroGx3dListSurvey *NitroPort_Gx3dSurveyList(const void *src, uint32_t bytes);
void NitroPort_Gx3dFreeSurvey(NitroGx3dListSurvey *survey);
/* its polygons (a strip of them whole: a group), in the list's order: each
 * one's lowest and highest coordinates (x, y, z, then x, y, z) into box, up
 * to max of them; the count. With slopes, each one's way up too: bit 0 if
 * it slopes along x (its vertices at its lowest x all at one height, those
 * at its highest x all at another), bit 1 along z. */
uint32_t NitroPort_Gx3dSurveyGroups(const NitroGx3dListSurvey *survey, int32_t (*box)[6], uint8_t *slopes, uint32_t max);
/* Whether every vertex in it is in one of those groups (a list that isn't
 * read as such, its first vertex relative to one before it, isn't). */
int NitroPort_Gx3dSurveyAllGrouped(const NitroGx3dListSurvey *survey);
/* The list with only the groups keep[] says, by that numbering (those past
 * nkeep left out), drawn the same: a BEGIN_VTXS whose groups are all left
 * out goes with them, and the coordinates left-out ones leave owed are
 * sent only for a vertex relative to them, as its own. 0 if it doesn't fit
 * (nothing kept: one word of NOPs). With repeatable, whether the cut list
 * sent twice in a row draws the same twice (TEXCOORD, NORMAL and COLOR
 * each either not in it or set before its first vertex). */
uint32_t NitroPort_Gx3dSurveyTrim(const NitroGx3dListSurvey *survey, void *dst, uint32_t dstBytes,
                                  const uint8_t *keep, uint32_t nkeep, int *repeatable);
/* The list cut into runs, all of them in one pass: a run each stretch of
 * its groups one after another whose mark[] is the same (with the commands
 * between them), its own list drawing those groups as the whole list does
 * whichever of the runs before it were sent before it: its BEGIN_VTXS
 * (again, if it starts inside one), the COLOR, NORMAL and TEXCOORD set
 * before it (in the order they were last set: after the BEGIN_VTXS, a
 * NORMAL lit with the polygon's own lights), its first vertex as its own
 * coordinates if it's relative to one before, an END_VTXS after it. Then,
 * if the list sets any, a last run marked 0xFF, to be sent after whichever
 * were: the COLOR, NORMAL and TEXCOORD the whole list leaves set, in a
 * BEGIN_VTXS and END_VTXS of their own without a vertex (what's
 * sent after it takes them: a shape's list without its own colour takes
 * the one before's last). The runs' lists one after another in dst, each
 * one's bytes in sizes[] and its mark in runMark[]; the number of runs, 0
 * if they don't fit (dst, or more than maxRuns) or the list has commands
 * besides its lists of vertices, COLOR, NORMAL and TEXCOORD, or a vertex
 * in no group. */
uint32_t NitroPort_Gx3dSurveyRuns(const NitroGx3dListSurvey *survey, const uint8_t *mark, uint32_t nmark,
                                  void *dst, uint32_t dstBytes, uint32_t *sizes, uint8_t *runMark, uint32_t maxRuns);
/* The list with only the groups keep[] says (as NitroPort_Gx3dSurveyTrim
 * has them), each vertex sent as its own coordinates (VTX_16) moved: x
 * scaled by num/den about originX, then all moved by dx, dy, dz (the
 * list's units); the COLOR, NORMAL and TEXCOORD a trim sends before a
 * BEGIN_VTXS sent after it instead (a NORMAL lit with the polygon's own
 * lights). Its bytes; 0 if it doesn't fit, nothing is kept, or a vertex
 * lands outside 16 bits. (A piece of a map drawn elsewhere, or squeezed.) */
uint32_t NitroPort_Gx3dSurveyMoved(const NitroGx3dListSurvey *survey, void *dst, uint32_t dstBytes, const uint8_t *keep, uint32_t nkeep,
                                   int32_t originX, int32_t num, int32_t den, int32_t dx, int32_t dy, int32_t dz);
/* As NitroPort_Gx3dSurveyMoved, the kept groups turned instead: x and z a
 * quarter clockwise (seen from above, north up: east to south) turn times
 * about pivotX, pivotZ (their NORMALs with them), then moved dx, dy, dz. A
 * piece of a map placed where its own kind faces another way (a bank's
 * corner turned to meet another bank; never mirrored). Its bytes; 0 as
 * there. */
uint32_t NitroPort_Gx3dSurveyTurned(const NitroGx3dListSurvey *survey, void *dst, uint32_t dstBytes, const uint8_t *keep, uint32_t nkeep,
                                    unsigned turn, int32_t pivotX, int32_t pivotZ, int32_t dx, int32_t dy, int32_t dz);
/* The land's trees, for the sun shadows: a land model's tree shapes'
 * display lists (lists, bytes, count of them: the materials with a tree's
 * texture; posScale: the model's, fx32: a world unit is 4096 * 4096 /
 * posScale of a list's) read once, when the model is first drawn.
 *
 * A tree: a list's polygons in order, a new tree where a polygon's middle
 * (across, x, and in depth, z) is more than 12 units from the tree's first
 * one's (a tree is three layers leaning back over a polygon flat on the
 * ground, the stump painted on it; the next tree stands two tiles on). It
 * stands on its flat polygons' middle; with none (a row's tops, their
 * stumps another material's), on the flat polygons of any of the lists its
 * middle is over, under it; else on its lowest vertices. A polygon over 48
 * units wide is a row of trees, 32 units each: cut into them.
 *
 * Each shape then has four lists (NULL, 0 bytes, for one that couldn't be
 * read that way: draw the shape as it is):
 * - its own without the flat polygons (Canopies) and the flat ones alone
 *   (Stumps), each drawing its polygons as the shape's list does, the
 *   canopies' sent first; and what the whole list leaves set after it
 *   (Tail: its last COLOR, NORMAL and TEXCOORD), to send after all;
 * - the trees' shadows for a sun (Shadows), made again when the sun given
 *   isn't the last one's: each layer's polygon laid on the ground from
 *   where its tree stands (a unit above it), as the tree seen from the sun
 *   would fall: across the tree goes across the light's way (the tree's
 *   right stays on the right), up it goes along the light's way, its
 *   height above the ground times stretch; the same texture coordinates.
 *   wayX, wayZ: the way the light goes along the ground (the model's x and
 *   z; fx32, a unit long), stretch fx32. To send between the Stumps and
 *   the Tail, marked NITRO_GX3D_TAG_SHADE, with the shape's material still
 *   set (its texture is the shadow's shape). */
typedef struct NitroGx3dTreeLists NitroGx3dTreeLists;
NitroGx3dTreeLists *NitroPort_Gx3dTreeLists(const void *const *lists, const uint32_t *bytes, uint32_t count,
                                            int32_t posScale);
void NitroPort_Gx3dFreeTreeLists(NitroGx3dTreeLists *trees);
const void *NitroPort_Gx3dTreeCanopies(const NitroGx3dTreeLists *trees, uint32_t shape, uint32_t *bytes);
/* The shape's layers again, each vertex with a COLOR before it for
 * NITRO_GX3D_TAG_CANOPY: darker towards the side the light (wayX to the
 * right, wayZ towards the camera) leaves, and towards a layer's foot. Made
 * again when the light's way changes. NULL for a shape whose list has
 * COLOR commands of its own, or a polygon that couldn't be read. */
const void *NitroPort_Gx3dTreeCanopiesLit(NitroGx3dTreeLists *trees, uint32_t shape, int32_t wayX, int32_t wayZ, uint32_t *bytes);
const void *NitroPort_Gx3dTreeStumps(const NitroGx3dTreeLists *trees, uint32_t shape, uint32_t *bytes);
const void *NitroPort_Gx3dTreeTail(const NitroGx3dTreeLists *trees, uint32_t shape, uint32_t *bytes);
/* The shape's trees' shadows for a light going wayX to the right and wayZ
 * towards the camera (4096ths), stretch (4096ths) along it for each unit
 * up: each tree's standing polygons laid on the ground with their texture
 * coordinates (NITRO_GX3D_TAG_SHADE: the renderer draws them with a
 * softened copy of the texture's outline). Made again when the light
 * changes. NULL with none. */
const void *NitroPort_Gx3dTreeShadows(NitroGx3dTreeLists *trees, uint32_t shape, int32_t wayX, int32_t wayZ,
                                      int32_t stretch, uint32_t *bytes);
/* Their trunks' shadows, made with them (call that first): a quad a tree
 * from its trunk to under its layers', for NITRO_GX3D_TAG_TRUNK. NULL with
 * none. */
const void *NitroPort_Gx3dTreeTrunks(const NitroGx3dTreeLists *trees, uint32_t shape, uint32_t *bytes);
/* (how many trees the shape's shadows are of: rows counted a tree each 32 units) */
/* The shape's trees' shadows where they stand (a pool, or a forest's
 * floor), for NITRO_GX3D_TAG_GROUND: made with the last
 * NitroPort_Gx3dTreeShadows of the shape, for its sun (call that first).
 * NULL with none. */
const void *NitroPort_Gx3dTreeGround(const NitroGx3dTreeLists *trees, uint32_t shape, uint32_t *bytes);
/* The shape's lit layers (shadows 0), its layers' shadows (1) or its
 * trunks' (2), as last made by NitroPort_Gx3dTreeCanopiesLit or
 * NitroPort_Gx3dTreeShadows (call that first), sent to the geometry engine
 * as NitroPort_GxFifoWords would send the list, but for the trees the view
 * can't see: the lists are made in parts, in the list's own order the
 * trees one after another in the same square of the map (8 tiles a side)
 * together, and a part whose box is all outside one side of the view, with the matrices
 * as the commands sent so far leave them, isn't sent. The scene is the
 * same as with the whole list (the engine passes over such trees itself,
 * one by one); the game's thread and the geometry thread have less to
 * read. The whole list while polygons beside the view are kept for
 * shadows. On the game's thread, between commands (after
 * NNS_G3dGeFlushBuffer); the words sent. */
uint32_t NitroPort_Gx3dTreeSend(const NitroGx3dTreeLists *trees, uint32_t shape, int shadows);
uint32_t NitroPort_Gx3dTreeCount(const NitroGx3dTreeLists *trees, uint32_t shape);
/* An MTX_TRANS (x, y, z: as its parameters, fx32) packed for a display
 * list, into dst: its bytes (16), 0 if it doesn't fit. */
uint32_t NitroPort_Gx3dTranslateCommand(void *dst, uint32_t dstBytes, int32_t x, int32_t y, int32_t z);
/* (for the [perf] geometry's work line) The time the thread has spent
 * making in-between scenes (NitroPort_Gx3dSetInterpolation), in the ticks
 * of NitroGx3dThreadOps.ticks, of it matching their polygons with the
 * scene before's, and how many it left out, behind (ending a scene a
 * V-blank or more after the game did: rather than a scene left out
 * later), so far. */
void NitroPort_Gx3dMidWork(uint64_t *ticks, uint64_t *matchTicks, uint32_t *leftOut);
/* (the [perf] in-between matching line) The matching's time so far, in
 * its four parts (the order's pairs, the scene's motion, each polygon
 * again by where it is, one each), in NitroGx3dThreadOps.ticks; the
 * polygons matched, those searched for in the window, and the window's
 * places looked at. */
void NitroPort_Gx3dMatchWork(uint64_t part[4], uint32_t *polygons, uint32_t *searched, uint32_t *scanned);
/* Where the thread's time goes (for the [perf] geometry line): the time
 * spent ending scenes (the clock of NitroGx3dThreadOps.ticks), the
 * vertices and polygons sent, and of the vertices those in lists wholly out
 * of the view (not worked out). Counts as they are: for a log. */
void NitroPort_Gx3dThreadWork(uint64_t *swapTicks, uint32_t *vertices, uint32_t *polygons, uint32_t *skipped);
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
/* The same, and taller: y scaled by 192 / screenHeight (with the DS's
 * full-size viewport), the picture showing more of the world above and
 * below (the field at the DS's pixel size on the 3DS's whole 400 x 240
 * screen). 192 (the DS's height) turns that off. */
void NitroPort_Gx3dSetView(int screenWidth, int screenHeight);

/* GXSTAT's bit 15 is write-one-to-clear on the DS, and resetting it
 * also clears the single-entry projection stack pointer. */
void NitroPort_Gx3dClearStackError(void);
/* A mark on the polygons the game sends from here on (0: none again), in
 * the command stream's order: each triangle of them carries it
 * (NitroGx3dTriangle.tag, the in-between scenes' too), and they're drawn
 * in batches of their own (NitroGx3dBatch.tag). Nothing else changes: the
 * same picture. Their triangles are also kept whole, on the screen or
 * beside it (NitroGx3dScene.tagged_triangles), and the game's box tests
 * made while a mark is set pass what's up to half a view beside the view
 * (what's drawn there is then kept whole for its shadow). Only with the
 * sun shadows on (NitroPort_GroundShadowsOn): off, a mark is no mark.
 * A map prop's mark is NITRO_GX3D_TAG_PROP | (its number << 1). The field's sprites are marked with their Billboard's
 * address, and a character's round shadow with its sprite's | 1 (the
 * game's patch; port_shadow.c pairs them). Not recorded
 * (NitroPort_Gx3dSetRecorder). */
void NitroPort_Gx3dTag(uint32_t tag);
/* The marks' kinds: a field sprite's is its Billboard's address (even,
 * below NITRO_GX3D_TAG_PROP on a 3DS); a character's round shadow's, its
 * sprite's | 1; a small standing map prop's (a tree, a sign, a lamp post),
 * NITRO_GX3D_TAG_PROP | (a number << 1).
 *
 * Two more only say how their polygons are drawn (NITRO_GX3D_TAG_IS_DRAW):
 * they aren't kept whole or reached for beside the view, the engine treats
 * them as any polygon. NITRO_GX3D_TAG_SHADE: not a thing but a shadow lying
 * on the ground (the land's trees': NitroPort_Gx3dTreeLists), drawn by the
 * renderer as the sun shadows are (the shade where a texel is opaque, a
 * pixel once), not at all by one that has no sun shadows.
 * NITRO_GX3D_TAG_STUMP: a tree's stump painted on the ground with a dark
 * patch round it: drawn without the patch (its green texels), which was
 * the game's own shadow under the tree. */
#define NITRO_GX3D_TAG_PROP 0xC0000000u
/* A small prop whose shadow the game sends as its outline on the ground (a
 * building's: NITRO_GX3D_TAG_GROUND and NITRO_GX3D_TAG_RIM batches): its
 * mark has this bit too, and its picture isn't laid (port_shadow.c). */
#define NITRO_GX3D_TAG_PROP_HULL 0x20000000u
/* (a small prop standing in a building's shadow, the game's to tell: this
 * bit in its mark; it's drawn in that shade, port_shadow.c) */
#define NITRO_GX3D_TAG_PROP_SHADED 0x10000000u
#define NITRO_GX3D_TAG_SHADE 0x40000000u
/* (the shadows on the ground where a tree stands, its pool or a forest's
 * floor: drawn before the layers' shadows, once; NitroPort_Gx3dTreeGround) */
#define NITRO_GX3D_TAG_GROUND 0x40000008u
/* (a building's shadow's soft edge: drawn with the trees' shadows, by its
 * grey, a pixel once) */
#define NITRO_GX3D_TAG_RIM 0x40000010u
/* (a tree's trunk's shadow: a strip drawn from its vertices' colours
 * alone, its texture not used; NitroPort_Gx3dTreeTrunks) */
#define NITRO_GX3D_TAG_TRUNK 0x40000018u
#define NITRO_GX3D_TAG_IS_SHADE(tag) (((tag)&~0x18u)==NITRO_GX3D_TAG_SHADE)
/* A model's shadow's outline on the ground it stands on (y 0), for a light:
 * the convex hull of every vertex of its shapes' display lists moved
 * shearX, shearZ (20.12) to the right and towards the camera for each unit
 * it's above the ground; at most `max` corners (the least telling ones
 * dropped), in order round it. out[i]: x, z and the height of the vertex
 * it came from, in the lists' units. A list with no vertex above flatY
 * (over 0) isn't looked at: a shape lying on the ground (the building's own
 * painted shadow). Returns the corners, 0 if a list couldn't be read or
 * there are under three. */
uint32_t NitroPort_Gx3dShadowHull(const void *const *lists, const uint32_t *bytes, uint32_t count, int32_t shearX,
                                  int32_t shearZ, int32_t flatY, int32_t (*out)[3], uint32_t max);
/* The same in parts (2026-11-11: one hull of a whole model, the Day
 * Care's house with its sheds round a yard, was one shadow over the yard):
 * the lists' polygons in groups, those with a vertex in the same square of
 * `join` units on the ground or the next, and a hull for each group, the
 * biggest `parts` of them; out[p * max ...] the corners of part p,
 * corners[p] how many. A group whose polygons cover under
 * NITRO_GX3D_HULL_COVER of its own outline on the ground (as it is, not
 * moved: a fence round a yard, posts and rails) casts none. Returns the
 * parts, 0 if none (or a list couldn't be read). */
/* A list's highest vertex (its y, in its own coordinates as sent);
 * INT32_MIN if it has none or can't be read. (A building's own painted
 * shadow is a see-through shape lying flat on the ground: left out with
 * the sun shadows on, a see-through shape standing up, a fountain's
 * water, drawn.) */
int32_t NitroPort_Gx3dListTop(const void *list, uint32_t bytes);
/* Whether a model is a picture standing up: every vertex of its shapes'
 * lists (not of those lying flat on the ground, their highest vertex no
 * higher than flatY: its painted shadow) within thick of one plane. Such
 * a prop (the Hallowed Tower and Eterna's bikes are one quad each) has no
 * shape for an outline's shadow: its outline is the quad's, and its
 * shadow was a square (on a 3DS, 2026-11-12, "the shadow from the tomb is
 * a square"). The game sends no outline for it. 0 too if a list can't be
 * read or there are under 3 vertices. */
int NitroPort_Gx3dListsFlat(const void *const *lists, const uint32_t *bytes, uint32_t count, int32_t flatY,
                            int32_t thick);
#define NITRO_GX3D_HULL_COVER 0.3
uint32_t NitroPort_Gx3dShadowHullParts(const void *const *lists, const uint32_t *bytes, uint32_t count, int32_t shearX,
                                       int32_t shearZ, int32_t flatY, int32_t join, int32_t (*out)[3], uint32_t max,
                                       uint32_t *corners, uint32_t parts);
/* The same for the shadow on ground fromY above the one the model stands
 * on (2026-11-12: the Hallowed Tower in its dip, its shadow on the higher
 * ground round it the one on its own ground moved up, as long and as far
 * out, and the two didn't meet: "2 shadows"): each vertex moved by its
 * height over fromY (none below it). */
uint32_t NitroPort_Gx3dShadowHullPartsFrom(const void *const *lists, const uint32_t *bytes, uint32_t count,
                                           int32_t shearX, int32_t shearZ, int32_t flatY, int32_t join, int32_t fromY,
                                           int32_t (*out)[3], uint32_t max, uint32_t *corners, uint32_t parts);
/* A building's shadow's outline of one height (corners, n: convex, in the
 * model's own coordinates) laid on the cells of the ground at that height
 * alone: cells is nz rows of nx numbers, a cell step a side from x0, z0;
 * those equal to level, in rectangles, the outline cut to each, and its
 * soft edge (rim wide outside it, in quarters) the same. Two display
 * lists, a command a word, added after *coreWords and *edgeWords (words):
 * core, one grey; edge, 26 of 31 at the outline to 0. Vertices a quarter
 * of the coordinates, at height y. The pieces laid; -1 if they don't fit. */
int32_t NitroPort_Gx3dShadowPieces(const int32_t (*corners)[3], uint32_t n, int32_t y, int32_t rim,
                                   const uint8_t *cells, uint32_t nx, uint32_t nz, uint8_t level,
                                   int32_t x0, int32_t z0, int32_t step,
                                   uint32_t *core, uint32_t coreRoom, uint32_t *coreWords,
                                   uint32_t *edge, uint32_t edgeRoom, uint32_t *edgeWords);
/* A building's walls lit by the sun (the game marks the big standing props
 * with it, 2026-11-10): draw-only too. The light's way on the ground in
 * the mark itself, as signed bytes of 1/127ths: to the right in bits 16
 * to 23, towards the camera in bits 8 to 15. Under it a vertex lit by a
 * NORMAL has its colour times a tint from the NORMAL and that way (a wall
 * facing the way the light comes from brighter, one facing where it goes
 * darker, a roof as it is), half of it as NITRO_GX3D_TAG_CANOPY's (the
 * renderer draws both at twice their colour); one with a COLOR, half its
 * colour. */
#define NITRO_GX3D_TAG_WALLS 0x60000000u
#define NITRO_GX3D_TAG_IS_WALLS(tag) (((tag)&0xF00000FFu)==NITRO_GX3D_TAG_WALLS)
/* (a building standing in another's shadow: its walls all in its shade,
 * whichever way they face) */
#define NITRO_GX3D_TAG_WALLS_SHADED 0x01000000u
#define NITRO_GX3D_TAG_STUMP 0x40000002u
/* (a tree's layers with their far side from the sun shaded: under this
 * mark a COLOR command isn't the vertices' colour but what their lit
 * colour, from the NORMAL, is times: NitroPort_Gx3dTreeCanopiesLit's
 * lists) */
#define NITRO_GX3D_TAG_CANOPY 0x40000004u
#define NITRO_GX3D_TAG_IS_PROP(tag) (((tag)&0xC0000001u)==NITRO_GX3D_TAG_PROP)
#define NITRO_GX3D_TAG_IS_DRAW(tag) (((tag)&0xC0000001u)==0x40000000u)
#define NITRO_GX3D_TAG_IS_ROUND(tag) (((tag)&1u) && !((tag)&0xC0000000u))

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
