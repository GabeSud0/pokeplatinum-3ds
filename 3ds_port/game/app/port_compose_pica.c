/*
 * The screens put together on the GPU (the "remaster" display path; see
 * docs/REMASTER.md). On the GPU thread, except where said.
 *
 * The DS's 256x192 screens are drawn 1.25 times bigger on the 3DS's: the
 * lower screen's 320x240 exactly; the upper one's 400x240 from a picture
 * 320 DS pixels wide (port_render2d.c's wider canvas, laid out by
 * port_wide.c), the 3D drawn straight into the screen at its resolution,
 * over its whole width (the geometry engine's wider view), with depth
 * from the 3D slider.
 *
 * Each screen is a render target (240 x 400 or 240 x 320: the 3DS keeps
 * its framebuffers turned a quarter turn), drawn in this order:
 *   1. the 2D layers behind the 3D (the whole picture, without 3D);
 *   2. the 3D's rear plane: the clear colour, with its alpha;
 *   3. the 3D's polygons (port_gx3d_pica.c), with edge marking and fog;
 *   4. a brightness effect on the 3D, when the game has one;
 *   5. the 2D layers in front of the 3D, with per-pixel alpha;
 *   6. the master brightness (a white or black quad).
 * For stereoscopic 3D the upper screen is drawn twice, the 3D shifted the
 * opposite way for each eye; the 2D is at the screen's depth.
 *
 * The 2D: the drawing threads draw an engine's picture in two layers
 * (behind and in front of the 3D) and publish them into textures of the
 * screen they're on (NitroPort_ComposePublish: three sets a screen, so one
 * can be drawn from, one be the newest and one be written). By default
 * each pixel becomes 2x2 texels and the GPU's bilinear filter does the
 * rest: the DS's pixels as they are, evenly sized, their borders a texel
 * soft (remaster.txt's filter setting: sharp, nearest, smooth, scale2x).
 *
 * Timing. Each 3D scene and each published 2D picture carries the game's
 * time (in V-blanks) of the display state it shows. A frame pairs the 3D
 * with the 2D the DS would have shown it with (the DS's 3D is a frame
 * behind its 2D), and is shown a fixed number of V-blanks after the game
 * made it (sLag, worked out from how long the frames take): the 3D comes
 * at the game's own even pace (every second V-blank in the field), not
 * whenever a scene happens to be done, and the 2D in step with it.
 *
 * Showing is apart from drawing. A drawn frame is transferred into one of
 * three LCD framebuffers of the screen's own (not libctru's two), and
 * queued with the V-blank it's for; the V-blank thread (main_3ds.c, which
 * calls NitroPort_ComposeVBlank) puts it on the LCD then. The display
 * thread never waits for a V-blank: it draws the next frame meanwhile
 * (the first version waited inside the frame, and the scenes made during
 * the wait were never drawn: 10 to 14 frames a second of a game making
 * 30).
 */
#ifdef __3DS__
#include "port_pica.h"
#include <nitro_port_upscale.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

void NitroPort_Log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#define DS_W 256
#define DS_H 192
#define CANVAS_MAX 320
#define TOP_W 400
#define BOTTOM_W 320
#define SCREEN_H 240
/* The DS's picture on a 3DS screen: 1.25 times the size. */
#define SCALE 1.25f

#define SETS 3
/* LCD framebuffers a screen: the one on the LCD, and up to two drawn
 * frames waiting for their V-blank. */
#define LCD_BUFS 3
#define LCD_FORMAT GSP_RGB565_OES
#define LCD_STRIDE (SCREEN_H * 2)

/* RGBA8 render targets to RGB565 framebuffers. */
#define SCREEN_TRANSFER_FLAGS (GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | \
     GX_TRANSFER_RAW_COPY(0) | GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | \
     GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB565) | GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

/* A published picture: its textures and what it is. */
typedef struct LayerSet {
    void *under;          /* RGB565 texels, tiled */
    void *over;           /* RGBA8 texels, tiled (alpha multiplied in) */
    int overEmpty;        /* nothing in front of the 3D (the over texture not written) */
    ComposeLayers info;   /* has3d etc., as published (no picture pointers) */
    uint32_t seq;         /* publish order (0: none, or being written) */
    int writing, held;
    NitroPublishTarget pub;   /* what its textures hold (publishing only what changed) */
} LayerSet;

/* A drawn frame in an LCD framebuffer, waiting for its V-blank. */
typedef struct Pending {
    int buf;
    u32 target;
    int stereo;
} Pending;

typedef struct Screen {
    C3D_RenderTarget *target[2];   /* left (and right) eye */
    C3D_FrameBuf edgeFb[2][4];     /* their colour with an edge marking depth/stencil a direction */
    int width;                     /* 400 or 320 */
    LayerSet set[SETS];
    int texW, texH;                /* the layer textures (power-of-two) */
    int texScale;                  /* texels a DS pixel: 2 (sharp, scale2x) or 1 */
    /* What's drawn last: to see whether anything changed. */
    uint32_t shownSeq, shownScene;
    float shownSlider;
    int shownStereo;
    /* The LCD framebuffers (left and, for the upper screen, right eye),
     * which is on the LCD (-1: none of ours yet), and the frames waiting
     * (under sPresentLock). */
    void *lcd[LCD_BUFS][2];
    int onLcd;
    Pending pending[2];
    int npending;
    unsigned presentSlot;
} Screen;

static ComposeConfig sConfig;
static int sLookRequest = -1;   /* (NitroPort_ComposeLook: 0 or 1, for the next frame) */

void NitroPort_ComposeLook(int on)
{
    __atomic_store_n(&sLookRequest, on ? 1 : 0, __ATOMIC_RELEASE);
}
static Screen sScreens[2];        /* 0: upper, 1: lower */
static void *sEdgeDepth[4];       /* 240 x 400 D24S8, a direction each, shared by the screens */
static LightLock sSetLock, sPresentLock;
static uint32_t sSeq;
static ComposeLayers sWork[2];    /* the drawers' pictures, by engine */
static uint16_t sWorkUnder[2][CANVAS_MAX * DS_H], sWorkOver[2][CANVAS_MAX * DS_H];
static uint8_t sWorkAlpha[2][CANVAS_MAX * DS_H];
static uint16_t sToRgb565[32768];
static uint8_t sAlpha16[17];      /* the over layer's alpha, 0-16, as 0-255 */
static uint32_t sConverted[2][CANVAS_MAX * DS_H];
static uint16_t sConverted16[2][CANVAS_MAX * DS_H];

/* Publishing only what changed (the default, sharp 2D): each engine's
 * last published picture, and for each band of 4 rows the publish it last
 * changed in; a set's textures are brought up to date by doing again the
 * bands changed since it was last written (NitroPort_PublishDoubled). Most
 * of what changes on a screen is small (a location's name sliding in, a
 * text box, a cursor): doubled and tiled whole, engine A's picture cost
 * ~10 ms a frame on the 3D's core, and with the 3D's geometry there too,
 * frames came late while a name slid in (2026-10-15). */
static NitroPublishShadow sShadow[2];
static NitroGx3dGpuVertex *sQuads;   /* this frame's quads */
static int sQuadVertices;
#define MAX_QUAD_VERTICES (6 * 32)
static int sInFrame;
static int sReady;
static volatile u32 sVBlanks;
static u32 sFrames, sLate, sTimed, sSkipped;
static u64 sGpuTicks, sCpuTicks;
static volatile u32 sPublished;
static volatile u64 sPublishTicks;
static uint32_t sPreparedScene;
static int sScenePrepared;
static float sSceneConvergence = 1.0f;
/* V-blanks from the game making a scene to its frame showing (adapts:
 * up when a frame is late, back down after a long while of slack). */
#define LAG_MIN 1
#define LAG_MAX 4
static int sLag = 2, sOnTime;
/* Lowered only after this many 3D frames in a row with a V-blank to
 * spare; doubled each time a lowered lag doesn't hold (a frame late soon
 * after). Counting frames with room since the last late one, it was
 * lowered every 20-26 seconds in the field, a frame came late, and it went
 * back up: a hitch every 26 seconds (2026-10-15). */
#define LAG_LOWER_AFTER 600
static int sLowerAfter = LAG_LOWER_AFTER, sLoweredRecently;
static u32 sLoweredAt;

/* For the log, the hitches: the newest scene seen (and the V-blank it
 * came), the last one shown on the 3D's screen, whether that screen
 * showed the 3D the last time it was drawn, scenes never shown, frames
 * drawn in stereo, and the last hitch logged. */
static u32 sSeenSerial, sSeenAt, sShownSerial, sDropped, sStereoFrames;
static int sSeenValid, sLast3D;
static u32 sHitchFrame, sHitchQuiet;

/* A drawn frame onto the LCD (the V-blank thread; under sPresentLock). */
static void present(int s, int buf, int stereo)
{
    Screen *scr = &sScreens[s];
    u32 mode = LCD_FORMAT | (1u << 8);   /* (linear memory) */
    const void *left = scr->lcd[buf][0], *right = left;

    if (s == 0) {
        if (stereo) {
            mode |= 1u << 5;
            right = scr->lcd[buf][1];
        } else {
            mode |= 1u << 6;
        }
    }
    scr->presentSlot ^= 1u;
    {
        /* (GSPGPU_SetBufferSwap rather than gspPresentBuffer: every libctru
         * has it) */
        GSPGPU_FramebufferInfo info;

        info.active_framebuf = scr->presentSlot;
        info.framebuf0_vaddr = (u32 *)left;
        info.framebuf1_vaddr = (u32 *)right;
        info.framebuf_widthbytesize = LCD_STRIDE;
        info.format = mode;
        info.framebuf_dispselect = scr->presentSlot;
        info.unk = 0;
        GSPGPU_SetBufferSwap(s == 0 ? GSP_SCREEN_TOP : GSP_SCREEN_BOTTOM, &info);
    }
    scr->onLcd = buf;
}

/* While the HOME menu (or another applet) has the screens, or the program
 * is closing: nothing put on the LCD, and what was waiting dropped. */
static volatile int sHeld;

void NitroPort_ComposeHold(int on)
{
    int s;

    if (!sReady) {
        sHeld = on;   /* (not set up, or gone: nothing waiting) */
        return;
    }
    LightLock_Lock(&sPresentLock);
    sHeld = on;
    if (on) {
        for (s = 0; s < 2; s++) {
            sScreens[s].npending = 0;
        }
    }
    LightLock_Unlock(&sPresentLock);
}

void NitroPort_ComposeVBlank(void)
{
    u32 now = __atomic_add_fetch(&sVBlanks, 1u, __ATOMIC_RELAXED);
    int s;

    if (!sReady || sHeld) {
        return;
    }
    LightLock_Lock(&sPresentLock);
    if (!sReady || sHeld) {
        LightLock_Unlock(&sPresentLock);
        return;
    }
    for (s = 0; s < 2; s++) {
        Screen *scr = &sScreens[s];

        /* The frames whose V-blank has come: the newest of them shows. */
        while (scr->npending > 0 && (int32_t)(scr->pending[0].target - now) <= 0) {
            if (scr->npending > 1 && (int32_t)(scr->pending[1].target - now) <= 0) {
                scr->pending[0] = scr->pending[1];   /* (the older one skipped) */
                scr->npending = 1;
                continue;
            }
            present(s, scr->pending[0].buf, scr->pending[0].stereo);
            scr->pending[0] = scr->pending[1];
            scr->npending--;
        }
    }
    LightLock_Unlock(&sPresentLock);
}

u32 NitroPort_ComposeVBlanks(void)
{
    return __atomic_load_n(&sVBlanks, __ATOMIC_RELAXED);
}

/* Engine A's 2D frames (V-blank counts): the last two the game offered
 * its drawer, the last one it was handed (it may have been busy), and the
 * last one it was done with. */
static volatile u32 s2DOffered, s2DOfferedBefore, s2DHanded, s2DDone;
static volatile int s2DKnown;

void NitroPort_Compose2DOffered(u32 stamp)
{
    if (__atomic_load_n(&s2DOffered, __ATOMIC_RELAXED) != stamp) {
        __atomic_store_n(&s2DOfferedBefore, __atomic_load_n(&s2DOffered, __ATOMIC_RELAXED), __ATOMIC_RELAXED);
    }
    __atomic_store_n(&s2DOffered, stamp, __ATOMIC_RELEASE);
}

void NitroPort_Compose2DHanded(u32 stamp)
{
    __atomic_store_n(&s2DHanded, stamp, __ATOMIC_RELEASE);
    __atomic_store_n(&s2DKnown, 1, __ATOMIC_RELEASE);
}

void NitroPort_Compose2DDone(u32 stamp)
{
    __atomic_store_n(&s2DDone, stamp, __ATOMIC_RELEASE);
}

/* Whether a new scene can be drawn yet: the DS shows it with the 2D of the
 * frame after it (stamp + 1). Drawn before engine A's drawer has that
 * frame, it went out with the 2D before and was drawn again when that came:
 * the 3D drawn twice, and if the 2D was late, a frame with the wrong
 * picture or brightness on the screen (2026-10-15: the title's Giratina
 * showed, bright, before its fade in from black). So it waits for that
 * frame of 2D: for the drawer to be done with it, or to know there's none
 * (the game offers a frame every other V-blank, or the drawer was busy and
 * left it out). A scene still waiting when the next one comes is drawn
 * then, with the 2D there is (NitroPort_ComposeFrame's flush; else each
 * scene was overtaken by the next where engine A's drawer takes more than
 * a frame, the opening's Pokemon on green and the battles: 2 to 9 frames a
 * second of 30, the 3D's screen standing still). No time limit short of
 * that: cut at the V-blank after the 2D's frame, a scene went out with
 * that 2D or with the one before by turns where the drawer takes about a
 * frame, and the opening forest's clouds (2D) jumped back and forth over
 * the trees (3D). (A few V-blanks at most, should no next scene come.) */
static int paired_2d_ready(u32 sceneStamp)
{
    u32 want = sceneStamp + 1u;
    u32 now = __atomic_load_n(&sVBlanks, __ATOMIC_RELAXED);
    u32 done, handed, offered, before, every;

    if (!__atomic_load_n(&s2DKnown, __ATOMIC_ACQUIRE)) {
        return 1;
    }
    done = __atomic_load_n(&s2DDone, __ATOMIC_ACQUIRE);
    handed = __atomic_load_n(&s2DHanded, __ATOMIC_ACQUIRE);
    offered = __atomic_load_n(&s2DOffered, __ATOMIC_ACQUIRE);
    before = __atomic_load_n(&s2DOfferedBefore, __ATOMIC_ACQUIRE);
    if ((int32_t)(done - want) >= 0) {
        return 1;   /* done with that frame (or a later one) */
    }
    if ((int32_t)(now - (want + (u32)LAG_MAX + 1u)) >= 0) {
        return 1;   /* (no longer than this) */
    }
    if (handed != done && (int32_t)(handed - want) <= 0) {
        return 0;   /* drawing a frame up to that one: its 2D is on the way */
    }
    if ((int32_t)(offered - want) >= 0) {
        return 1;   /* that frame was offered, and left out (the drawer was busy) */
    }
    every = offered - before;
    if (every >= 2u && every <= 4u && (int32_t)(offered + every - want) > 0) {
        return 1;   /* the game offers a frame every other V-blank: none then */
    }
    return 0;       /* none offered for it yet */
}

/* The scene the 3D's screen is waiting to draw (paired_2d_ready), if any
 * (the display thread's own). */
static int sSceneWaiting;
static uint32_t sSceneWaitingSerial;

int NitroPort_ComposeSceneWaiting(uint32_t sceneSerial)
{
    return sSceneWaiting && sSceneWaitingSerial == sceneSerial;
}

static void make_texture(C3D_Tex *tex, void *data, GPU_TEXCOLOR fmt, int w, int h, int linear,
                         GPU_TEXTURE_WRAP_PARAM wrap)
{
    memset(tex, 0, sizeof(*tex));
    tex->data = data;
    tex->fmt = fmt;
    tex->size = (u32)w * h * (fmt == GPU_RGBA8 ? 4u : 2u);
    tex->width = (u16)w;
    tex->height = (u16)h;
    tex->param = GPU_TEXTURE_MODE(GPU_TEX_2D) |
                 GPU_TEXTURE_MAG_FILTER(linear ? GPU_LINEAR : GPU_NEAREST) |
                 GPU_TEXTURE_MIN_FILTER(linear ? GPU_LINEAR : GPU_NEAREST) |
                 GPU_TEXTURE_WRAP_S(wrap) | GPU_TEXTURE_WRAP_T(wrap);
}

static void free_sets(Screen *scr)
{
    int i;

    for (i = 0; i < SETS; i++) {
        if (scr->set[i].under) linearFree(scr->set[i].under);
        if (scr->set[i].over) linearFree(scr->set[i].over);
        scr->set[i].under = NULL;
        scr->set[i].over = NULL;
    }
}

static void free_all(void)
{
    int s, i, k;

    for (s = 0; s < 2; s++) {
        for (i = 0; i < 2; i++) {
            if (sScreens[s].target[i]) {
                C3D_RenderTargetDelete(sScreens[s].target[i]);
                sScreens[s].target[i] = NULL;
            }
        }
        for (i = 0; i < LCD_BUFS; i++) {
            for (k = 0; k < 2; k++) {
                if (sScreens[s].lcd[i][k]) {
                    linearFree(sScreens[s].lcd[i][k]);
                    sScreens[s].lcd[i][k] = NULL;
                }
            }
        }
        free_sets(&sScreens[s]);
    }
    for (i = 0; i < 4; i++) {
        if (sEdgeDepth[i]) {
            vramFree(sEdgeDepth[i]);
            sEdgeDepth[i] = NULL;
        }
    }
    if (sQuads) {
        linearFree(sQuads);
        sQuads = NULL;
    }
    sReady = 0;
}

/* The layer textures of a screen: big enough for its widest picture at
 * scale texels a pixel. */
static int alloc_sets(Screen *scr, int canvasW, int scale)
{
    int i;

    scr->texScale = scale;
    scr->texW = 256;
    while (scr->texW < canvasW * scale) {
        scr->texW *= 2;
    }
    scr->texH = 256;
    while (scr->texH < DS_H * scale + 8) {
        scr->texH *= 2;
    }
    for (i = 0; i < SETS; i++) {
        scr->set[i].under = linearAlloc((size_t)scr->texW * scr->texH * 2u);
        scr->set[i].over = linearAlloc((size_t)scr->texW * scr->texH * 4u);
        if (!scr->set[i].under || !scr->set[i].over) {
            return 0;
        }
        memset(scr->set[i].under, 0, (size_t)scr->texW * scr->texH * 2u);
        memset(scr->set[i].over, 0, (size_t)scr->texW * scr->texH * 4u);
        memset(&scr->set[i].pub, 0, sizeof(scr->set[i].pub));   /* (holding nothing known) */
    }
    return 1;
}

int NitroPort_ComposeInit(const ComposeConfig *config)
{
    static const char *const filters[] = { "sharp (the pixels as they are, 2x then bilinear)", "nearest",
                                           "smooth (bilinear)", "Scale2x" };
    int s, i, c, e;

    sConfig = *config;
    /* The 3D's textures: nearest, as the DS's, unless remaster.txt says
     * otherwise (filter3d: see main_3ds.c). */
    NitroPort_PicaTextureFilter(config->filter3d);
    NitroPort_PicaLook(config->shadows, config->grade, config->sunFixed, config->sunElevation,
                       config->sunAzimuth);
    memset(sScreens, 0, sizeof(sScreens));
    LightLock_Init(&sSetLock);
    LightLock_Init(&sPresentLock);
    sHeld = 0;
    for (c = 0; c < 32768; c++) {
        int r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;

        sToRgb565[c] = (u16)((r << 11) | (g << 6) | (g >> 4) | b);
    }
    for (c = 0; c <= 16; c++) {
        sAlpha16[c] = (uint8_t)(c >= 16 ? 255 : c * 255 / 16);
    }
    for (e = 0; e < 2; e++) {
        memset(&sWork[e], 0, sizeof(sWork[e]));
        sWork[e].under = sWorkUnder[e];
        sWork[e].over = sWorkOver[e];
        sWork[e].overAlpha = sWorkAlpha[e];
    }
    for (s = 0; s < 2; s++) {
        Screen *scr = &sScreens[s];
        int canvasW = s == 0 && config->wide ? CANVAS_MAX : DS_W;
        int scale = config->filter == COMPOSE_FILTER_SHARP || config->filter == COMPOSE_FILTER_SCALE2X ? 2 : 1;

        scr->width = s == 0 ? TOP_W : BOTTOM_W;
        scr->onLcd = -1;
        for (i = 0; i < (s == 0 && config->stereo ? 2 : 1); i++) {
            int k;

            scr->target[i] = C3D_RenderTargetCreate(SCREEN_H, scr->width, GPU_RB_RGBA8,
                                                    GPU_RB_DEPTH24_STENCIL8);
            if (!scr->target[i]) {
                NitroPort_Log("[compose] no VRAM for the %s screen's render target", s ? "lower" : "upper");
                goto fail;
            }
            for (k = 0; k < LCD_BUFS; k++) {
                scr->lcd[k][i] = linearAlloc((size_t)scr->width * SCREEN_H * 2u);
                if (!scr->lcd[k][i]) {
                    NitroPort_Log("[compose] not enough memory for the LCD framebuffers");
                    goto fail;
                }
                memset(scr->lcd[k][i], 0, (size_t)scr->width * SCREEN_H * 2u);
            }
        }
        if (!alloc_sets(scr, canvasW, scale)) {
            /* Not enough memory at 2 texels a pixel: 1 (bilinear). */
            free_sets(scr);
            if (scale == 1 || !alloc_sets(scr, canvasW, 1)) {
                NitroPort_Log("[compose] not enough memory for the 2D layer textures");
                goto fail;
            }
            NitroPort_Log("[compose] not enough memory for the sharp 2D on the %s screen: smooth instead",
                          s ? "lower" : "upper");
        }
    }
    if (config->edges) {
        int d;

        for (d = 0; d < 4; d++) {
            sEdgeDepth[d] = vramAlloc(C3D_CalcDepthBufSize(SCREEN_H, TOP_W, GPU_RB_DEPTH24_STENCIL8));
        }
        if (!sEdgeDepth[3]) {
            NitroPort_Log("[compose] no VRAM for the edge marking buffers: outlines off");
            for (d = 0; d < 4; d++) {
                if (sEdgeDepth[d]) {
                    vramFree(sEdgeDepth[d]);
                    sEdgeDepth[d] = NULL;
                }
            }
            sConfig.edges = 0;
        }
    }
    for (s = 0; s < 2 && sEdgeDepth[0]; s++) {
        for (i = 0; i < 2 && sScreens[s].target[i]; i++) {
            int d;

            for (d = 0; d < 4; d++) {
                C3D_FrameBuf *fb = &sScreens[s].edgeFb[i][d];

                *fb = sScreens[s].target[i]->frameBuf;
                C3D_FrameBufDepth(fb, sEdgeDepth[d], GPU_RB_DEPTH24_STENCIL8);
            }
        }
    }
    sQuads = linearAlloc(MAX_QUAD_VERTICES * sizeof(NitroGx3dGpuVertex));
    if (!sQuads) {
        goto fail;
    }
    sInFrame = 0;
    sReady = 1;
    NitroPort_Log("[compose] screens drawn by the GPU: %s, 2D %s, 3D %s%s%s",
                  config->wide ? "wide upper screen" : "the DS's width",
                  filters[config->filter & 3],
                  config->stereo ? "with depth from the slider" : "flat",
                  sConfig.edges ? ", outlines on the GPU" : ", no outlines",
                  config->flipV ? ", 2D flipped" : "");
    return 1;
fail:
    free_all();
    return 0;
}

void NitroPort_ComposeExit(void)
{
    /* (nothing put on the LCD from the buffers being freed) */
    LightLock_Lock(&sPresentLock);
    sReady = 0;
    sHeld = 1;
    LightLock_Unlock(&sPresentLock);
    /* The frame begun after the last one drawn (to wait for the GPU) ends
     * first: citro3d stops the program (svcBreak) if a render target is
     * deleted inside a frame. Closing from HOME crashed the console there
     * (2026-10-15: a prefetch abort in svcBreak, from
     * C3D_RenderTargetDelete). */
    if (sInFrame) {
        C3D_FrameEnd(0);
        sInFrame = 0;
    }
    free_all();
}

/* ---------------------------------------------------------- the layers */
/* (the drawing threads) */

ComposeLayers *NitroPort_ComposeLayersOf(int engine)
{
    return &sWork[engine ? 1 : 0];
}

uint32_t NitroPort_ComposePublished(int engine)
{
    (void)engine;
    return __atomic_load_n(&sSeq, __ATOMIC_ACQUIRE);
}

static u32 widen5(u32 v)
{
    return (v << 3) | (v >> 2);
}

/* The picture into a set's textures whole: the filters other than sharp
 * (or a texel a pixel). */
static void publish_whole(int e, const ComposeLayers *l, Screen *scr, LayerSet *set, int w)
{
    uint16_t *c16 = sConverted16[e];
    int i, n = w * DS_H;
    /* (the texture's rows the picture uses: tile rows, whole) */
    u32 rowsUsed = ((u32)DS_H * scr->texScale + 8u + 7u) & ~7u;

    set->pub.heldBy = NULL;   /* (not tracked this way) */
    for (i = 0; i < n; i++) {
        c16[i] = sToRgb565[l->under[i] & 0x7FFF];
    }
    if (sConfig.filter == COMPOSE_FILTER_SCALE2X && scr->texScale == 2) {
        NitroPort_Scale2xTiled16(c16, w, DS_H, set->under, scr->texW);
    } else {
        NitroPort_Tile16(c16, w, DS_H, set->under, scr->texW);
    }
    GSPGPU_FlushDataCache(set->under, (u32)scr->texW * rowsUsed * 2u);
    if (l->has3d) {
        /* Often there's nothing in front: then the texture isn't written. */
        uint32_t *c32 = sConverted[e];

        if (NitroPort_PremultiplyOver(l->over, l->overAlpha, sAlpha16, c32, 0, n)) {
            if (sConfig.filter == COMPOSE_FILTER_SCALE2X && scr->texScale == 2) {
                NitroPort_Scale2xTiled32(c32, w, DS_H, set->over, scr->texW);
            } else {
                NitroPort_Tile32(c32, w, DS_H, set->over, scr->texW);
            }
            GSPGPU_FlushDataCache(set->over, (u32)scr->texW * rowsUsed * 4u);
            set->overEmpty = 0;
        }
    }
}

static void flush_texture(void *data, uint32_t bytes)
{
    GSPGPU_FlushDataCache(data, bytes);
}

void NitroPort_ComposePublish(int engine, int screen)
{
    int e = engine ? 1 : 0, i, buf = -1;
    Screen *scr = &sScreens[screen ? 1 : 0];
    ComposeLayers *l = &sWork[e];
    LayerSet *set;
    u64 start = svcGetSystemTick();
    int w = l->width > 0 ? l->width : DS_W;

    if (!sReady || w > CANVAS_MAX || w * scr->texScale > scr->texW) {
        return;
    }
    /* The oldest set not held by the compositor, not the newest, and not
     * being written. */
    LightLock_Lock(&sSetLock);
    {
        uint32_t newest = 0;

        for (i = 0; i < SETS; i++) {
            if (scr->set[i].seq > newest) {
                newest = scr->set[i].seq;
            }
        }
        for (i = 0; i < SETS; i++) {
            LayerSet *c = &scr->set[i];

            if (c->writing || c->held || (c->seq == newest && newest != 0)) {
                continue;
            }
            if (buf < 0 || c->seq < scr->set[buf].seq) {
                buf = i;
            }
        }
        if (buf >= 0) {
            scr->set[buf].writing = 1;
            scr->set[buf].seq = 0;   /* (not a candidate while written) */
        }
    }
    LightLock_Unlock(&sSetLock);
    if (buf < 0) {
        return;   /* (both engines publishing to one screen as they swap) */
    }
    set = &scr->set[buf];
    set->info = *l;
    set->info.under = NULL;
    set->info.over = NULL;
    set->info.overAlpha = NULL;
    set->overEmpty = 1;
    if (!l->displayOff && sConfig.filter != COMPOSE_FILTER_SCALE2X && scr->texScale == 2) {
        set->overEmpty = !NitroPort_PublishDoubled(&sShadow[e], &set->pub, l->under, l->over, l->overAlpha,
                                                   l->has3d, w, set->under, set->over, scr->texW, sToRgb565,
                                                   sAlpha16, flush_texture);
    } else if (!l->displayOff) {
        publish_whole(e, l, scr, set, w);
    }
    LightLock_Lock(&sSetLock);
    set->writing = 0;
    set->seq = ++sSeq;
    LightLock_Unlock(&sSetLock);
    __atomic_fetch_add(&sPublished, 1u, __ATOMIC_RELAXED);
    __atomic_fetch_add(&sPublishTicks, svcGetSystemTick() - start, __ATOMIC_RELAXED);
}

/* -------------------------------------------------------------- the quads */

/* A quad in screen pixels (x, y from the top left), at depth z (clip
 * space: -1 the farthest), textured over (u0, v0)-(u1, v1) of the bound
 * texture, in colour rgba (the combiner says which counts). */
static void quad(float x0, float y0, float x1, float y1, float z, float u0, float v0, float u1, float v1,
                 u32 rgba)
{
    static const int corner[6][2] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 0 }, { 1, 1 }, { 0, 1 } };
    int i;

    if (sQuadVertices + 6 > MAX_QUAD_VERTICES) {
        return;
    }
    for (i = 0; i < 6; i++) {
        NitroGx3dGpuVertex *v = &sQuads[sQuadVertices + i];

        v->position[0] = corner[i][0] ? x1 : x0;
        v->position[1] = corner[i][1] ? y1 : y0;
        v->position[2] = z;
        v->position[3] = 1.0f;
        v->color[0] = (uint8_t)(rgba >> 24);
        v->color[1] = (uint8_t)(rgba >> 16);
        v->color[2] = (uint8_t)(rgba >> 8);
        v->color[3] = (uint8_t)rgba;
        v->uv[0] = corner[i][0] ? u1 : u0;
        v->uv[1] = corner[i][1] ? v1 : v0;
    }
    NitroPort_PicaBindVertices(sQuads);
    C3D_DrawArrays(GPU_TRIANGLES, sQuadVertices, 6);
    sQuadVertices += 6;
}

/* The combiner for a quad: the texture, or the vertex colour. */
static void quad_combiner(int textured)
{
    C3D_TexEnv *env = C3D_GetTexEnv(0);

    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_Both, textured ? GPU_TEXTURE0 : GPU_PRIMARY_COLOR, 0, 0);
    C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
}

static u32 rgba_of(uint16_t c, u32 alpha)
{
    return (widen5(c & 31) << 24) | (widen5((c >> 5) & 31) << 16) | (widen5((c >> 10) & 31) << 8) | alpha;
}

/* The layer textures, described for the GPU. */
static C3D_Tex sUnderTex, sOverTex;

/* One screen (one eye of it), into its render target. */
static void draw_screen(Screen *scr, int eye, const LayerSet *set, const NitroGx3dScene *scene, float stereo,
                        int edges)
{
    C3D_RenderTarget *target = scr->target[eye];
    int eyeDir;
    C3D_FrameBuf *fb = &target->frameBuf;
    C3D_Mtx pixels;
    const ComposeLayers *info = set ? &set->info : NULL;
    int canvasW = info && info->width > 0 ? info->width : DS_W;
    PicaRect pic, rect3d;
    float u1, vTop, vBottom;
    int linear = sConfig.filter != COMPOSE_FILTER_NEAREST;
    int has3d = info != NULL && !info->displayOff && info->has3d && scene != NULL;

    /* The picture, 1.25 times the DS's size, in the middle (it fills the
     * screen: 320 DS pixels on the upper one, 256 on the lower). */
    pic.x0 = ((float)scr->width - canvasW * SCALE) * 0.5f;
    pic.x1 = pic.x0 + canvasW * SCALE;
    pic.y0 = 0.0f;
    pic.y1 = SCREEN_H;
    C3D_SetFrameBuf(fb);
    C3D_SetViewport(0, 0, fb->width, fb->height);
    NitroPort_PicaPixelMatrix(&pixels, scr->width, SCREEN_H);
    NitroPort_PicaSetView(&pixels, 0.0f, 1.0f);
    C3D_CullFace(GPU_CULL_NONE);
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    C3D_StencilTest(false, GPU_ALWAYS, 0, 0xFF, 0xFF);
    C3D_AlphaTest(false, GPU_ALWAYS, 0);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
    C3D_FogGasMode(GPU_NO_FOG, GPU_PLAIN_DENSITY, false);
    if (info == NULL || info->displayOff) {
        /* Nothing published yet, or the engine is off: black. */
        quad_combiner(0);
        quad(0, 0, (float)scr->width, SCREEN_H, 0.0f, 0, 0, 0, 0, 0x000000FFu);
        return;
    }
    /* The texture coordinates of the picture's corners: its top row is
     * texture row 0, at v = 1 (the PICA reads row height - 1 - t). */
    u1 = (float)(canvasW * scr->texScale) / (float)scr->texW;
    vTop = 1.0f;
    vBottom = 1.0f - (float)(DS_H * scr->texScale) / (float)scr->texH;
    if (sConfig.flipV) {
        float t = vTop;

        vTop = vBottom;
        vBottom = t;
    }
    /* Beside a picture narrower than the screen (the DS's width on the
     * upper screen without the wide setting): its backdrop colour. */
    if (pic.x0 > 0.5f) {
        quad_combiner(0);
        quad(0, 0, (float)scr->width, SCREEN_H, 0.0f, 0, 0, 0, 0, rgba_of((uint16_t)info->backdrop, 255));
    }
    /* 1. The picture behind the 3D. */
    make_texture(&sUnderTex, set->under, GPU_RGB565, scr->texW, scr->texH, linear, GPU_CLAMP_TO_EDGE);
    C3D_TexBind(0, &sUnderTex);
    quad_combiner(1);
    quad(pic.x0, pic.y0, pic.x1, pic.y1, 0.0f, 0, vTop, u1, vBottom, 0xFFFFFFFFu);
    if (has3d) {
        uint16_t rearColor;
        unsigned rearAlpha;
        PicaView view;

        /* Where the 3D goes: the DS's screen in the picture, or (a scene
         * made for the wider view) the whole width; moved left by BG0HOFS
         * (the game's screen shakes). */
        if (scene->screen_width > DS_W) {
            rect3d.x0 = ((float)scr->width - (float)scene->screen_width * SCALE) * 0.5f;
            rect3d.x1 = rect3d.x0 + (float)scene->screen_width * SCALE;
        } else {
            rect3d.x0 = pic.x0 + (canvasW - DS_W) * 0.5f * SCALE;
            rect3d.x1 = rect3d.x0 + DS_W * SCALE;
        }
        rect3d.y0 = 0.0f;
        rect3d.y1 = SCREEN_H;
        if (info->scroll3d) {
            int scroll = info->scroll3d >= 256 ? info->scroll3d - 512 : info->scroll3d;

            rect3d.x0 -= (float)scroll * SCALE;
            rect3d.x1 -= (float)scroll * SCALE;
        }
        /* 2. The rear plane: the clear colour, blended by its alpha. */
        NitroPort_PicaRearPlane(&rearColor, &rearAlpha);
        if (rearAlpha) {
            quad_combiner(0);
            C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA,
                           GPU_ONE, GPU_ZERO);
            quad(rect3d.x0, rect3d.y0, rect3d.x1, rect3d.y1, -1.0f, 0, 0, 0, 0,
                 rgba_of(rearColor, rearAlpha * 255u / 31u));
        }
        /* 3. The polygons. */
        view.fb = fb;
        for (eyeDir = 0; eyeDir < 4; eyeDir++) {
            view.edgeFb[eyeDir] = edges ? &scr->edgeFb[eye][eyeDir] : NULL;
        }
        view.screenWidth = scr->width;
        view.screenHeight = SCREEN_H;
        view.rect = rect3d;
        view.stereo = stereo;
        view.convergence = sConfig.convergence > 0.0f ? sConfig.convergence : sSceneConvergence;
        NitroPort_PicaDrawScene(&view);
        C3D_SetFrameBuf(fb);
        C3D_SetViewport(0, 0, fb->width, fb->height);
        NitroPort_PicaSetView(&pixels, 0.0f, 1.0f);
        C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
        /* 4. A brightness effect on the 3D (BLDCNT with BG0 as a first
         * target; alpha blending with the layer behind can't be done after
         * the fact: it's left as drawn). */
        if (info->tint3d == 2 || info->tint3d == 3) {
            u32 a = (u32)(info->evy > 16 ? 16 : info->evy) * 255u / 16u;

            quad_combiner(0);
            C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA,
                           GPU_ONE, GPU_ZERO);
            quad(rect3d.x0, rect3d.y0, rect3d.x1, rect3d.y1, 0.0f, 0, 0, 0, 0,
                 (info->tint3d == 2 ? 0xFFFFFF00u : 0x00000000u) | a);
        }
        /* 5. The layers in front, with their alpha (multiplied in). */
        if (!set->overEmpty) {
            make_texture(&sOverTex, set->over, GPU_RGBA8, scr->texW, scr->texH, linear, GPU_CLAMP_TO_EDGE);
            C3D_TexBind(0, &sOverTex);
            quad_combiner(1);
            C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ONE_MINUS_SRC_ALPHA,
                           GPU_ONE, GPU_ONE_MINUS_SRC_ALPHA);
            quad(pic.x0, pic.y0, pic.x1, pic.y1, 0.0f, 0, vTop, u1, vBottom, 0xFFFFFFFFu);
        }
    }
    /* 6. The master brightness: towards white or black by factor / 16. */
    {
        int mode = (info->masterBright >> 14) & 3, factor = info->masterBright & 31;

        if (factor > 16) {
            factor = 16;
        }
        if (factor && (mode == 1 || mode == 2)) {
            quad_combiner(0);
            C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA,
                           GPU_ONE, GPU_ZERO);
            quad(0, 0, (float)scr->width, SCREEN_H, 0.0f, 0, 0, 0, 0,
                 (mode == 1 ? 0xFFFFFF00u : 0x00000000u) | ((u32)factor * 255u / 16u));
        }
    }
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
}

/* The convergence distance for a scene: between the nearest and the
 * farthest of its perspective vertices (w > 1), so the scene's middle is
 * at the screen; 1 (no shift) for a scene with none. */
static float scene_convergence(const NitroGx3dScene *scene)
{
    float lo = 0.0f, hi = 0.0f;
    uint32_t i;

    for (i = 0; i < scene->count; i++) {
        int k;

        for (k = 0; k < 3; k++) {
            float w = (float)scene->triangles[i].v[k].clip[3] / 4096.0f;

            if (w <= 1.0001f) {
                continue;
            }
            if (lo == 0.0f || w < lo) lo = w;
            if (w > hi) hi = w;
        }
    }
    if (lo == 0.0f) {
        return 1.0f;
    }
    return sqrtf(lo * hi);
}

/* Which of a screen's sets to show: the newest, or with a 3D scene the
 * newest no newer than the 2D the DS showed that scene with (the DS's 3D
 * is a frame behind its 2D): stamp <= the scene's + 1. Held for the frame
 * (the drawers won't write it). Under sSetLock. */
static LayerSet *pick_set(Screen *scr, const NitroGx3dScene *scene, int syncTo3d)
{
    LayerSet *best = NULL, *oldest = NULL;
    int i;

    for (i = 0; i < SETS; i++) {
        LayerSet *c = &scr->set[i];

        if (c->writing || c->seq == 0) {
            continue;
        }
        if (oldest == NULL || c->seq < oldest->seq) {
            oldest = c;
        }
        if (syncTo3d && scene != NULL && c->info.has3d &&
            (int32_t)(c->info.stamp - (scene->stamp + 1u)) > 0) {
            continue;   /* newer 2D than this scene goes with */
        }
        if (best == NULL || c->seq > best->seq) {
            best = c;
        }
    }
    if (best == NULL) {
        best = oldest;
    }
    if (best != NULL) {
        best->held = 1;
    }
    return best;
}

/* A free LCD framebuffer of a screen: not on the LCD, not waiting. Under
 * sPresentLock. -1: none (two frames waiting already). */
static int free_lcd_buffer(const Screen *scr)
{
    int b, i;

    for (b = 0; b < LCD_BUFS; b++) {
        int taken = b == scr->onLcd;

        for (i = 0; i < scr->npending; i++) {
            taken |= scr->pending[i].buf == b;
        }
        if (!taken) {
            return b;
        }
    }
    return -1;
}

/* The edge marking buffers filled for a scene (memory fills, queued
 * ahead of the drawing). */
static void fill_edge_buffers(Screen *scr, int eye, u32 value)
{
    int d;

    for (d = 0; d < 4; d++) {
        C3D_FrameBufClear(&scr->edgeFb[eye][d], C3D_CLEAR_DEPTH, 0, value);
    }
}

int NitroPort_ComposeFrame(const NitroGx3dScene *scene, uint32_t sceneSerial, int aOnTop, float slider,
                           int forced, int flush)
{
    int s, drawn = 0, stereoNow, need[2] = { 0, 0 }, timed = 0, edges = 0, lcdBuf[2] = { -1, -1 };
    u32 vblanks, edgeClear = 0, startVBlank, texBefore, texAfter;
    LayerSet *sets[2] = { NULL, NULL };
    u64 start = svcGetSystemTick(), gpuStart, prepareTicks = 0, texTicksBefore, texTicksAfter, texels;

    if (!sReady) {
        return 0;
    }
    {
        int look = __atomic_exchange_n(&sLookRequest, -1, __ATOMIC_ACQ_REL);

        if (look >= 0) {
            sConfig.shadows = look;
            sConfig.grade = look;
            NitroPort_PicaLook(sConfig.shadows, sConfig.grade, sConfig.sunFixed, sConfig.sunElevation,
                               sConfig.sunAzimuth);
        }
    }
    startVBlank = __atomic_load_n(&sVBlanks, __ATOMIC_RELAXED);
    if (scene != NULL && (!sSeenValid || sceneSerial != sSeenSerial)) {
        /* A scene not seen before: the one before it, if the 3D was on
         * its screen and it never showed, and any made in between (the
         * newest is taken), went unseen. */
        if (sSeenValid && sLast3D) {
            u32 gap = sceneSerial - sSeenSerial;

            if (sShownSerial != sSeenSerial) {
                sDropped++;
            }
            if (gap > 1u && gap < 1000u) {
                sDropped += gap - 1u;
            }
        }
        sSeenSerial = sceneSerial;
        sSeenAt = startVBlank;
        sSeenValid = 1;
    }
    stereoNow = sConfig.stereo && slider > 0.0f && sScreens[0].target[1] != NULL && aOnTop;
    LightLock_Lock(&sSetLock);
    LightLock_Lock(&sPresentLock);
    for (s = 0; s < 2; s++) {
        Screen *scr = &sScreens[s];
        int screen3d = (s == 0) == (aOnTop != 0);   /* engine A's screen: the 3D's */
        LayerSet *set = pick_set(scr, scene, screen3d);
        int has3d;

        if (set == NULL) {
            continue;   /* nothing to show yet */
        }
        has3d = screen3d && set->info.has3d && !set->info.displayOff && scene != NULL;
        need[s] = forced || set->seq != scr->shownSeq || (has3d && sceneSerial != scr->shownScene) ||
                  (s == 0 && has3d && (stereoNow != scr->shownStereo ||
                                        (stereoNow && slider != scr->shownSlider)));
        /* A new scene: once the 2D it's shown with is here (or the next
         * scene has come: flush). */
        if (need[s] && has3d && !forced && !flush && sceneSerial != scr->shownScene &&
            !paired_2d_ready(scene->stamp)) {
            need[s] = 0;
            sSceneWaiting = 1;
            sSceneWaitingSerial = sceneSerial;
        }
        /* Not without a framebuffer to draw into (two frames waiting for
         * their V-blanks already): the next V-blank frees one. */
        if (need[s]) {
            lcdBuf[s] = free_lcd_buffer(scr);
            if (lcdBuf[s] < 0) {
                need[s] = 0;
            }
        }
        if (!need[s]) {
            set->held = 0;
            continue;
        }
        sets[s] = set;
        if (has3d && sceneSerial != scr->shownScene) {
            timed = 1;   /* a new 3D frame: shown at its time */
        }
        if (s == 0 && !has3d) {
            stereoNow = 0;
        }
    }
    LightLock_Unlock(&sPresentLock);
    LightLock_Unlock(&sSetLock);
    if (!need[0] && !need[1]) {
        return 0;
    }
    NitroPort_PicaTextureStats(&texBefore, &texels, &texTicksBefore);
    if (scene != NULL && (!sScenePrepared || sceneSerial != sPreparedScene)) {
        prepareTicks = svcGetSystemTick();
        NitroPort_PicaPrepareScene(scene, NULL);
        prepareTicks = svcGetSystemTick() - prepareTicks;
        sPreparedScene = sceneSerial;
        sScenePrepared = 1;
        if (sConfig.stereo && sConfig.convergence <= 0.0f) {
            sSceneConvergence = scene_convergence(scene);
        }
    }
    if (!sInFrame) {
        C3D_FrameBegin(0);
        sInFrame = 1;
    }
    sQuadVertices = 0;
    edges = sEdgeDepth[0] != NULL && scene != NULL && NitroPort_PicaEdgeClearValue(&edgeClear);
    /* The depth/stencil buffers cleared (before any drawing: the fills go
     * ahead of the frame's commands), the edge marking buffers with them. */
    for (s = 0; s < 2; s++) {
        int eye, screen3d = (s == 0) == (aOnTop != 0);

        if (!need[s]) {
            continue;
        }
        for (eye = 0; eye < (s == 0 && stereoNow ? 2 : 1); eye++) {
            C3D_RenderTargetClear(sScreens[s].target[eye], C3D_CLEAR_DEPTH, 0, 0xFFFFFFFFu);
        }
        if (screen3d && edges) {
            fill_edge_buffers(&sScreens[s], 0, edgeClear);
        }
    }
    for (s = 0; s < 2; s++) {
        Screen *scr = &sScreens[s];
        int screen3d = (s == 0) == (aOnTop != 0);
        const NitroGx3dScene *sc = screen3d ? scene : NULL;

        if (!need[s]) {
            continue;
        }
        if (s == 0 && stereoNow) {
            float shift = sConfig.stereoStrength * slider * 0.5f;

            draw_screen(scr, 0, sets[s], sc, -shift, edges);
            if (sc != NULL && edges) {
                /* The second eye needs the edge buffers filled again: the
                 * commands so far go ahead, the fills after them. */
                C3D_FrameSplit(GX_CMDLIST_FLUSH);
                fill_edge_buffers(scr, 1, edgeClear);
            }
            draw_screen(scr, 1, sets[s], sc, shift, edges);
        } else {
            draw_screen(scr, 0, sets[s], sc, 0.0f, edges);
        }
        scr->shownSeq = sets[s]->seq;
        if (screen3d) {
            scr->shownScene = sceneSerial;
            sSceneWaiting = 0;
            sLast3D = sc != NULL && sets[s]->info.has3d && !sets[s]->info.displayOff;
            if (sc != NULL) {
                sShownSerial = sceneSerial;
            }
        }
        scr->shownSlider = slider;
        scr->shownStereo = stereoNow;
        drawn |= 1 << s;
    }
    GSPGPU_FlushDataCache(sQuads, (u32)(sQuadVertices * sizeof(NitroGx3dGpuVertex)));
    C3D_FrameEnd(GX_CMDLIST_FLUSH);
    NitroPort_PicaNoteCmdUse();
    sInFrame = 0;
    /* The targets into the screens' LCD framebuffers, after the drawing
     * (queued behind it), then wait for all of it. */
    for (s = 0; s < 2; s++) {
        Screen *scr = &sScreens[s];
        int eye;

        if (!(drawn & (1 << s))) {
            continue;
        }
        for (eye = 0; eye < (s == 0 && stereoNow ? 2 : 1); eye++) {
            GX_DisplayTransfer((u32 *)scr->target[eye]->frameBuf.colorBuf, GX_BUFFER_DIM(SCREEN_H, scr->width),
                               (u32 *)scr->lcd[lcdBuf[s]][eye], GX_BUFFER_DIM(SCREEN_H, scr->width),
                               SCREEN_TRANSFER_FLAGS);
        }
    }
    gpuStart = svcGetSystemTick();
    sCpuTicks += gpuStart - start;
    C3D_FrameBegin(0);   /* (waits for the GPU and the transfers) */
    sInFrame = 1;
    sGpuTicks += svcGetSystemTick() - gpuStart;
    sFrames++;
    if (stereoNow) {
        sStereoFrames++;
    }
    NitroPort_PicaTextureStats(&texAfter, &texels, &texTicksAfter);
    {
        /* A frame that took the display thread more than two V-blanks:
         * the scenes made meanwhile may never show (a jump). */
        const double ms = 1000.0 / SYSCLOCK_ARM11;
        double total = (double)(svcGetSystemTick() - start) * ms;

        if (total > 40.0 && (sFrames - sHitchFrame >= 15u || sHitchFrame == 0u)) {
            NitroPort_Log("[hitch] a frame took the display thread %.1f ms: %.1f ms preparing the scene, %.1f ms "
                          "building (%lu textures converted: %.1f ms), %.1f ms GPU%s",
                          total, (double)prepareTicks * ms, (double)(gpuStart - start) * ms,
                          (unsigned long)(texAfter - texBefore),
                          (double)(texTicksAfter - texTicksBefore) * ms,
                          (double)(svcGetSystemTick() - gpuStart) * ms, stereoNow ? "; stereo" : "");
            sHitchFrame = sFrames;
        }
    }
    /* The frames queued for their V-blanks: a new 3D frame for sLag
     * V-blanks after the game made its scene (+1: the DS shows a scene the
     * frame after); anything else for the next one. */
    vblanks = __atomic_load_n(&sVBlanks, __ATOMIC_RELAXED);
    LightLock_Lock(&sPresentLock);
    for (s = 0; s < 2; s++) {
        Screen *scr = &sScreens[s];
        int screen3d = (s == 0) == (aOnTop != 0);
        u32 target = vblanks + 1u;
        Pending *p;

        if (sets[s] != NULL) {
            sets[s]->held = 0;
        }
        if (!(drawn & (1 << s))) {
            continue;
        }
        if (timed && screen3d && scene != NULL) {
            u32 wanted = scene->stamp + 1u + (u32)sLag;
            int32_t slack = (int32_t)(wanted - (vblanks + 1u));

            sTimed++;
            if (slack < 0) {
                /* late: more lag from now on */
                sLate++;
                if (sLag < LAG_MAX) {
                    sLag++;
                }
                sOnTime = 0;
                if (sLoweredRecently && sTimed - sLoweredAt < 1800u && sLowerAfter < LAG_LOWER_AFTER * 64) {
                    sLowerAfter *= 2;   /* (it didn't hold: twice as long before the next try) */
                }
                sLoweredRecently = 0;
                /* Where the time went, for the log (one line every half
                 * second at most; the others counted in the next). */
                if (sFrames - sHitchFrame >= 15u || sHitchFrame == 0u) {
                    const double ms = 1000.0 / SYSCLOCK_ARM11;

                    NitroPort_Log("[hitch] a 3D frame %d V-blank%s late (lag now %d): made at V-blank %lu, "
                                  "reached the display thread %lu V-blanks later, drawn from %lu after; %.1f ms "
                                  "preparing, %.1f ms building (%lu textures converted: %.1f ms), %.1f ms GPU%s%s",
                                  (int)-slack, slack == -1 ? "" : "s", sLag, (unsigned long)scene->stamp,
                                  (unsigned long)(sSeenAt - scene->stamp),
                                  (unsigned long)(startVBlank - scene->stamp),
                                  (double)prepareTicks * ms, (double)(gpuStart - start) * ms,
                                  (unsigned long)(texAfter - texBefore),
                                  (double)(texTicksAfter - texTicksBefore) * ms,
                                  (double)(svcGetSystemTick() - gpuStart) * ms,
                                  stereoNow ? "; stereo" : "",
                                  sHitchQuiet ? "; more since the last line" : "");
                    sHitchFrame = sFrames;
                    sHitchQuiet = 0;
                } else {
                    sHitchQuiet++;
                }
            } else {
                /* Lowered after a long while of frames with a V-blank to
                 * spare, in a row (one without resets the count). */
                if (slack >= 1) {
                    sOnTime++;
                } else {
                    sOnTime = 0;
                }
                if (sOnTime >= sLowerAfter && sLag > LAG_MIN) {
                    sLag--;
                    sOnTime = 0;
                    sLoweredRecently = 1;
                    sLoweredAt = sTimed;
                }
                target = slack > 6 ? vblanks + 1u + 6u : wanted;   /* (a stamp from before a pause) */
            }
        }
        if (scr->npending >= 2) {
            /* (can't happen: a free buffer was found above) */
            scr->npending = 1;
            sSkipped++;
        }
        p = &scr->pending[scr->npending++];
        p->buf = lcdBuf[s];
        p->target = target;
        p->stereo = s == 0 && stereoNow;
    }
    LightLock_Unlock(&sPresentLock);
    return drawn;
}

void NitroPort_ComposeStats(ComposeStats *stats)
{
    stats->frames = sFrames;
    stats->gpuTicks = sGpuTicks;
    stats->cpuTicks = sCpuTicks;
    stats->published = __atomic_load_n(&sPublished, __ATOMIC_RELAXED);
    stats->publishTicks = __atomic_load_n(&sPublishTicks, __ATOMIC_RELAXED);
    stats->lag = sLag;
    stats->late = sLate;
    stats->timed = sTimed;
    stats->dropped = sDropped;
    stats->stereo = sStereoFrames;
}
#endif
