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
 * the screen's own LCD framebuffers (LCD_BUFS; not libctru's two), and
 * queued with the V-blank it's for; the V-blank thread (main_3ds.c, which
 * calls NitroPort_ComposeVBlank) puts it on the LCD then. The display
 * thread never waits for a V-blank: it draws the next frame meanwhile
 * (the first version waited inside the frame, and the scenes made during
 * the wait were never drawn: 10 to 14 frames a second of a game making
 * 30).
 */
#ifdef __3DS__
#include "port_pica.h"
#include <nitro_port_lowermenu.h>
#include <nitro_port_upscale.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void NitroPort_Log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#define DS_W 256
#define DS_H 192
#define CANVAS_MAX 320
/* (the drawers' pictures: the wider upper screen's, or the lower screen's
 * as a tall panel) */
#define WORK_PIXELS (CANVAS_MAX * DS_H > DS_W * NITRO_LOWERMENU_TALL_LINES ? CANVAS_MAX * DS_H \
                                                                           : DS_W * NITRO_LOWERMENU_TALL_LINES)
#define TOP_W 400
#define BOTTOM_W 320
#define SCREEN_H 240
/* The DS's picture on a 3DS screen: 1.25 times the size. */
#define SCALE 1.25f

#define SETS 3
/* LCD framebuffers a screen: one on the LCD, the one before it (scanned
 * out until the next V-blank), and the frames waiting for their V-blanks.
 * The upper screen's: at 60 frames a second a frame is built 1-2 V-blanks
 * after the game made its scene and shown sLag + 1 after (the in-between
 * scene's a V-blank earlier), 3-4 V-blanks apart at the most lag, so 4-5
 * frames wait at once (with 3 buffers, one waiting frame: with lag 4 only
 * the in-between scenes got a buffer, the scenes were skipped, 2026-10-20).
 * The lower screen's frames show at the next V-blank, but for the 3D's
 * when it's there (the opening's flight): 5. Fewer if the linear heap
 * hasn't room (3 at least: 30 frames a second then). */
#define LCD_BUFS 7
#define LCD_BUFS_LOWER 5
#define LCD_BUFS_LEAST 3
#define PENDING_MAX LCD_BUFS
#define LCD_FORMAT GSP_RGB565_OES
#define LCD_STRIDE (SCREEN_H * 2)

/* The upper screen's framebuffers (its eyes' and the smooth 3D's) are
 * RGB565, as its LCD framebuffers are: half the bytes of RGBA8 to clear,
 * draw into and copy (nothing reads their alpha: every blend is by the
 * source's alpha or a constant), for the same picture on the LCD but the
 * blends' and the 2x2 averages' rounding, at 5-6 bits not 8. The lower
 * screen's stay RGBA8. */
#define TOP_FB_FORMAT GPU_RB_RGB565
#define TOP_TRANSFER_FMT GX_TRANSFER_FMT_RGB565
/* Render targets to RGB565 framebuffers (the lower screen's RGBA8, the
 * upper's RGB565). */
#define SCREEN_TRANSFER_FLAGS (GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | \
     GX_TRANSFER_RAW_COPY(0) | GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | \
     GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB565) | GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))
#define TOP_SCREEN_TRANSFER_FLAGS (GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | \
     GX_TRANSFER_RAW_COPY(0) | GX_TRANSFER_IN_FORMAT(TOP_TRANSFER_FMT) | \
     GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB565) | GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))
/* The same, each 2x2 pixels averaged into one (the smooth 3D; the output's
 * size given as the input's, as citro3d does: the engine halves it). */
#define SMOOTH_TRANSFER_FLAGS (GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | \
     GX_TRANSFER_RAW_COPY(0) | GX_TRANSFER_IN_FORMAT(TOP_TRANSFER_FMT) | \
     GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB565) | GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_XY))

/* A published picture: its textures and what it is. */
typedef struct LayerSet {
    void *under;          /* RGB565 texels, tiled */
    void *over;           /* RGBA8 texels, tiled (alpha multiplied in); RGBA5551 when pub.over16 */
    int overEmpty;        /* nothing in front of the 3D (the over texture not written) */
    int underColor;       /* the picture behind the 3D all this RGB565 colour, or -1 */
    int overTop, overBottom;   /* the DS's rows with anything in front of the 3D (a row more each side) */
    ComposeLayers info;   /* has3d etc., as published (no picture pointers) */
    uint32_t seq;         /* publish order (0: none, or being written) */
    int writing, held;    /* (held: by frames being built, a count) */
    u32 flightSerial;     /* the last frame drawn with it (busy until sGpuDone reaches it) */
    NitroPublishTarget pub;   /* what its textures hold (publishing only what changed) */
} LayerSet;

/* A drawn frame in an LCD framebuffer, waiting for its V-blank. */
typedef struct Pending {
    int buf;
    u32 target;
    int stereo;
    u32 serial;           /* its frame's GPU work (sGpuDone reaching it: drawn) */
} Pending;

typedef struct Screen {
    C3D_RenderTarget *target[2];   /* left (and right) eye */
    /* their colour with an edge marking depth/stencil a direction (a
     * C3D_FrameBuf each, in libctru's layout: NitroPort_PicaFrameBufWithDepth) */
    uint8_t edgeFb[2][4][PICA_FRAMEBUF_BYTES] __attribute__((aligned(8)));
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
    int nlcd;                      /* how many of them (LCD_BUFS or LCD_BUFS_LOWER) */
    int onLcd, lastLcd;            /* (lastLcd: scanned out until the swap takes: not free yet) */
    int onStereo;                  /* (what's on the LCD has a right eye of its own) */
    Pending pending[PENDING_MAX];
    int npending;
    unsigned presentSlot;
    /* The lower screen as the menu, as drawn last (and its picture's serial,
     * its map's, how dark its panel was). */
    int shownMenu;
    uint32_t shownMenuSerial, shownMapSerial;
    int shownFade;
    uint32_t shownRunSerial, shownUpSeq;   /* (its panel put together: the runs, the upper screen's picture) */
} Screen;

static ComposeConfig sConfig;
static int sLookRequest = -1;   /* (NitroPort_ComposeLook: 0 or 1, for the next frame) */

void NitroPort_ComposeLook(int on)
{
    __atomic_store_n(&sLookRequest, on ? 1 : 0, __ATOMIC_RELEASE);
}
static Screen sScreens[2];        /* 0: upper, 1: lower */

/* The lower screen as the menu (nitro_port_lowermenu.h): its picture in a
 * texture, a texel a pixel (RGB565: it's opaque), two of them taken by
 * turns so a frame in flight keeps the one it's drawn with; the DS's lower
 * screen in its panel at 15/16. */
#define MENU_TEX_W 512
#define MENU_TEX_H 256
static int sUpperKept, sUpperKeptBright;   /* (the upper screen kept: NitroPort_LowerMenuHoldUpper) */
/* The field's picture (NITRO_LOWERMENU_HOLD_FIELD): the upper LCD's, copied
 * as it began to be kept at full brightness (each eye, RGB565, as the LCD
 * takes it), and the game's forgetting's count then; whether the LCD shows
 * it now (or what it was copied from), and shows it again (back from the
 * HOME menu). */
static void *sFieldPic[2];
static int sFieldValid, sFieldStereo, sUpperShowsField, sFieldAgain;
static u32 sFieldSerial;
static int sUpperShownBright;             /* (its picture drawn last: how bright, -16..16) */
static void *sMenuTexData[2];
static C3D_Tex sMenuTex;
static int sMenuTexCur = -1;
static uint32_t sMenuTexSerial;
static uint32_t sMenuCopy[NITRO_LOWERMENU_W * NITRO_LOWERMENU_H];
static uint16_t sMenu565[NITRO_LOWERMENU_W * NITRO_LOWERMENU_H];
/* (each texture's rows behind the picture: to be made again when it's next
 * written; all of them to begin with) */
static int sMenuStaleY0[2], sMenuStaleY1[2] = { NITRO_LOWERMENU_H, NITRO_LOWERMENU_H };
/* The map's picture (NitroPort_LowerMenuLockMap), drawn over the panel when
 * it shows the map: the same way, in textures of its own. */
#define MAP_TEX_W 256
#define MAP_TEX_H 256
static void *sMapTexData[2];
static C3D_Tex sMapTex;
static int sMapTexCur = -1;
static uint32_t sMapTexSerial;
static uint32_t sMapCopy[NITRO_LOWERMENU_PANEL_W * NITRO_LOWERMENU_PANEL_H];
static uint16_t sMap565[NITRO_LOWERMENU_PANEL_W * NITRO_LOWERMENU_PANEL_H];
static int sMapStaleY0[2], sMapStaleY1[2] = { NITRO_LOWERMENU_PANEL_H, NITRO_LOWERMENU_PANEL_H };
static int sMenuPanel;                 /* (the menu's panel this frame: NitroPort_LowerMenuPanel, once) */
static int sMenuFade;                  /* (how dark its panel is this frame: NitroPort_LowerMenuPanelFade, once) */
static int sMenuNow;              /* (the screen being drawn is the menu) */
/* (and its panel put together: NitroPort_LowerMenuSetPanelRuns, as read
 * for the frame; the upper screen's picture its runs show, held for it) */
static NitroLowerPanelRun sPanelRuns[NITRO_LOWERMENU_RUNS_MAX];
static int sPanelRunCount;
static LayerSet *sPanelUp;
static int sMenu3dNow;            /* (and its 3D goes in its panel) */
static void *sEdgeDepth[4];       /* 240 x 400 D24S8, a direction each, shared by the screens */
static void *sEdgeBlock;          /* (the four, one after another: one VRAM block) */
/* The smooth 3D (smooth3d): the upper screen is drawn at twice the
 * resolution each way (480 x 800) and scaled down by the transfer engine:
 * every edge's pixels averaged, no jagged edges. Its depth/stencil is the
 * edge buffers' block (exactly the size), else a block of its own.
 *  - Without outlines, all of it (each eye in turn, with the slider up),
 *    scaled down into its LCD framebuffer as soon as it's drawn.
 *  - With outlines (sSmoothSplit; each eye in turn with the slider up):
 *    the picture behind and the opaque polygons, scaled down into the
 *    screen's own framebuffer (the eye's; through a linear copy in the
 *    smooth depth's memory, done with by then); there the opaque
 *    polygons' depth and IDs are drawn again (no colour), and the outlines
 *    (their buffers filled only now: the smooth depth was theirs), the
 *    translucent polygons and the 2D in front go on at the screen's
 *    resolution. The outlines stay one pixel wide, as the DS's are. */
#define SMOOTH_W (SCREEN_H * 2)
#define SMOOTH_H (TOP_W * 2)
/* (or 2x1, while 60 frames a second wouldn't fit 2x2: auto_fps60. Two
 * samples down the screen (the framebuffer's lines, which the transfer
 * engine can halve on their own), one across: TOP_W lines.) */
static void *sSmoothColor, *sSmoothOwnDepth, *sSmoothDepth;
static volatile int sSmooth3d;          /* (the setting now: sConfig.smooth3d's, then the options page's) */
static uint8_t sSmoothFb[PICA_FRAMEBUF_BYTES] __attribute__((aligned(8)));
static int sSmoothReady, sSmoothNow, sSmoothSplit, sSmoothHalf, sSmoothMadeHalf;
static u32 sEdgeClearNow;          /* (this frame: what the edge buffers are filled with) */
static void smooth_down(Screen *scr, int eye, int edges);
static unsigned sEdgeCopiesNow;    /* (and which are copied instead) */
static u32 sSmoothFrames, sSmoothHalfFrames;
static LightLock sSetLock, sPresentLock;
static uint32_t sSeq;
static ComposeLayers sWork[2];    /* the drawers' pictures, by engine */
static uint16_t sWorkUnder[2][WORK_PIXELS], sWorkOver[2][CANVAS_MAX * DS_H];
static uint8_t sWorkAlpha[2][CANVAS_MAX * DS_H];
static uint16_t sToRgb565[32768];
static uint8_t sAlpha16[17];      /* the over layer's alpha, 0-16, as 0-255 */
static uint32_t sConverted[2][CANVAS_MAX * DS_H];
static uint16_t sConverted16[2][WORK_PIXELS];

/* Publishing only what changed (the default, sharp 2D): each engine's
 * last published picture, and for each band of 4 rows the publish it last
 * changed in; a set's textures are brought up to date by doing again the
 * bands changed since it was last written (NitroPort_PublishDoubled). Most
 * of what changes on a screen is small (a location's name sliding in, a
 * text box, a cursor): doubled and tiled whole, engine A's picture cost
 * ~10 ms a frame on the 3D's core, and with the 3D's geometry there too,
 * frames came late while a name slid in (2026-10-15). */
static NitroPublishShadow sShadow[2];
static NitroGx3dGpuVertex *sQuads;   /* this frame's quads (half of sQuadsAll: the frame before's other half) */
static NitroGx3dGpuVertex *sQuadsAll;
static int sQuadHalf;
static int sQuadVertices;
/* (a frame's quads: 32, an H-blank effect's runs' rear planes and
 * tints, 2 x 24 at most: LINE_RUNS_MAX, LINE_RUNS_STEREO a side; the
 * menu's panel put together, 96 at most: PANEL_PIECES_MAX, their sides and
 * brightness) */
#define MAX_QUAD_VERTICES (6 * (32 + 2 * 24 + 96))
/* ------------------------------------------------- frames in flight
 * A frame's commands are built while the GPU draws the frame before
 * (NitroPort_PicaFrameOpen): its GPU operations (fills, command lists,
 * transfers) go into one of two queues of ours, bound while it's built
 * and run once the frame before is done (flight_finish). What that frame
 * reads stays as it is until then: its 2D (held), its quads (the other
 * half of sQuadsAll), its vertices (port_gx3d_pica.c keeps two), its
 * textures (NitroPort_PicaGpuWait). It shows at its V-blank once drawn
 * (Pending.serial). */
#define GX_QUEUE_ENTRIES 128
static gxCmdQueue_s sGxQueue[2];
static int sGxReady, sGxRecording;      /* (the queue the frame being built goes into) */
static int sFlight, sFlightQueue;       /* a frame the GPU has, and its queue */
static u32 sFlightMark;                 /* (NitroPort_PicaFrameClose's) */
static u32 sGpuSubmitted;
static volatile u32 sGpuDone;           /* (frames drawn: the queues' callback) */
static volatile u64 sGpuDoneTick;
static u64 sGpuRunTick, sWaitTicks, sLastGpuTicks;
static int sFlightStereo, sFlightEdgesWanted, sFlightEdges;   /* (the frame in flight: for auto_outlines) */
static int sFlight3D;                   /* (and whether it drew the 3D: for auto_fps60) */
static int sFlightRuns;                 /* (and its H-blank effect's runs: sFrameLineRuns) */
static int sFlightDrawn, sFlightSmoothable;   /* (and how: DRAWN_*, and whether it could have been 2x2) */
static int sFlightState;                /* (and its steps: fps60_state) */
static u32 sMidFrames;                  /* (in-between scenes drawn) */
static void gpu_finished(gxCmdQueue_s *queue);
static void flight_finish(int block);
static void flight_wait(void);
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
#define LAG_LOWER_MOST 16   /* (... at most this many times as long: 5 minutes) */
static int sLowerAfter = LAG_LOWER_AFTER, sLoweredRecently;
static u32 sLoweredAt;
static u32 sMidLate;        /* in-between scenes late (shown at their scene's V-blank, or left out) */
static int sMidBefore;      /* (the scene being queued had its in-between scene drawn before it) */

/* An in-between scene late for its V-blank (60 frames a second: it would
 * be skipped for its scene's frame, a step of 30 a second): more lag from
 * now on, as for a late scene. */
static void mid_late(void)
{
    sMidLate++;
    sLate++;
    if (sLag < LAG_MAX) {
        sLag++;
    }
    sOnTime = 0;
    if (sLoweredRecently && sTimed - sLoweredAt < 1800u && sLowerAfter < LAG_LOWER_AFTER * LAG_LOWER_MOST) {
        sLowerAfter *= 2;
    }
    sLoweredRecently = 0;
}

/* For the log, the hitches: the newest scene seen (and the V-blank it
 * came), the last one shown on the 3D's screen, whether that screen
 * showed the 3D the last time it was drawn, scenes never shown, frames
 * drawn in stereo, and the last hitch logged. */
static u32 sSeenSerial, sSeenAt, sShownSerial, sDropped, sStereoFrames;
static u32 sUnderFills;   /* (screens drawn with the picture behind as the clear) */
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
    scr->lastLcd = scr->onLcd;
    scr->onLcd = buf;
    scr->onStereo = s == 0 && stereo;
}

/* (the upper screen kept at full brightness, under sPresentLock) What's on
 * its LCD copied, as the field's picture. */
static void keep_field_picture(Screen *scr)
{
    size_t bytes = (size_t)scr->width * SCREEN_H * 2u;
    int eye;

    if (scr->onLcd < 0) {
        return;   /* (the field's picture itself on the LCD still: kept as it is) */
    }
    sFieldValid = 0;
    for (eye = 0; eye < (scr->onStereo ? 2 : 1); eye++) {
        if (sFieldPic[eye] == NULL) {
            sFieldPic[eye] = linearAlloc(bytes);
        }
        if (sFieldPic[eye] == NULL) {
            return;   /* (no memory: black, as before) */
        }
        GSPGPU_InvalidateDataCache(scr->lcd[scr->onLcd][eye], (u32)bytes);
        memcpy(sFieldPic[eye], scr->lcd[scr->onLcd][eye], bytes);
        GSPGPU_FlushDataCache(sFieldPic[eye], (u32)bytes);
    }
    sFieldStereo = scr->onStereo;
    sFieldSerial = NitroPort_LowerMenuFieldSerial();
    sFieldValid = 1;
}

static int field_picture_kept(void)
{
    return sFieldValid && sFieldSerial == NitroPort_LowerMenuFieldSerial();
}

/* (under sPresentLock, nothing of the upper screen's waiting) The field's
 * picture onto the upper LCD from the next V-blank. */
static void present_field_picture(Screen *scr)
{
    u32 mode = LCD_FORMAT | (1u << 8) | (sFieldStereo ? 1u << 5 : 1u << 6);
    GSPGPU_FramebufferInfo info;

    scr->presentSlot ^= 1u;
    info.active_framebuf = scr->presentSlot;
    info.framebuf0_vaddr = (u32 *)sFieldPic[0];
    info.framebuf1_vaddr = (u32 *)(sFieldStereo ? sFieldPic[1] : sFieldPic[0]);
    info.framebuf_widthbytesize = LCD_STRIDE;
    info.format = mode;
    info.framebuf_dispselect = scr->presentSlot;
    info.unk = 0;
    GSPGPU_SetBufferSwap(GSP_SCREEN_TOP, &info);
    scr->lastLcd = scr->onLcd;
    scr->onLcd = -1;   /* (none of the LCD framebuffers: all free once that's off) */
    scr->onStereo = sFieldStereo;
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

        /* The frames whose V-blank has come (and the GPU has drawn): the
         * newest of them shows. */
        u32 done = __atomic_load_n(&sGpuDone, __ATOMIC_ACQUIRE);
        int i, newest = -1;

        /* (of those whose V-blank has come, the newest the GPU has drawn;
         * the older ones are skipped; none drawn yet: a V-blank later.
         * All of them looked at: they needn't be in order of V-blank.) */
        for (i = 0; i < scr->npending; i++) {
            if ((int32_t)(scr->pending[i].target - now) <= 0 && (int32_t)(done - scr->pending[i].serial) >= 0) {
                newest = i;
            }
        }
        if (newest >= 0) {
            present(s, scr->pending[newest].buf, scr->pending[newest].stereo);
            scr->npending -= newest + 1;
            memmove(scr->pending, scr->pending + newest + 1, (size_t)scr->npending * sizeof(scr->pending[0]));
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

static volatile u32 s2DCurrent[2];   /* (NitroPort_Compose2DCurrent) */

void NitroPort_Compose2DCurrent(int screen, u32 stamp)
{
    __atomic_store_n(&s2DCurrent[screen ? 1 : 0], stamp, __ATOMIC_RELEASE);
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
    NitroPort_LowerMenuSetAvailable(0);
    for (i = 0; i < 2; i++) {
        if (sMenuTexData[i]) {
            linearFree(sMenuTexData[i]);
            sMenuTexData[i] = NULL;
        }
        sMenuStaleY0[i] = 0;   /* (made whole again when there's one) */
        sMenuStaleY1[i] = NITRO_LOWERMENU_H;
    }
    sMenuTexCur = -1;
    for (i = 0; i < 2; i++) {
        if (sMapTexData[i]) {
            linearFree(sMapTexData[i]);
            sMapTexData[i] = NULL;
        }
        sMapStaleY0[i] = 0;
        sMapStaleY1[i] = NITRO_LOWERMENU_PANEL_H;
    }
    sMapTexCur = -1;
    for (i = 0; i < 4; i++) {
        sEdgeDepth[i] = NULL;
    }
    if (sEdgeBlock) {
        vramFree(sEdgeBlock);
        sEdgeBlock = NULL;
    }
    if (sSmoothColor) {
        vramFree(sSmoothColor);
        sSmoothColor = NULL;
    }
    if (sSmoothOwnDepth) {
        vramFree(sSmoothOwnDepth);
        sSmoothOwnDepth = NULL;
    }
    sSmoothReady = 0;
    sSmoothDepth = NULL;
    if (sQuadsAll) {
        linearFree(sQuadsAll);
        sQuadsAll = NULL;
        sQuads = NULL;
    }
    if (sGxReady) {
        /* (the GPU done with both: flight_finish) */
        gxCmdQueueStop(&sGxQueue[0]);
        GX_BindQueue(NULL);
        NitroPort_PicaGpuWait(NULL);
        sGxReady = 0;
    }
    for (i = 0; i < 2; i++) {
        free(sGxQueue[i].entries);
        memset(&sGxQueue[i], 0, sizeof(sGxQueue[i]));
    }
    NitroPort_PicaFramesFree();
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
    /* (room for the lower screen as a tall panel: its rows past the DS's) */
    while (scr->texH < NITRO_LOWERMENU_TALL_LINES * scale + 8) {
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
        /* (the layers in front 16 bits a texel while none of them is
         * partly see-through: in the field a text box's quad took the GPU
         * 2-3.7 ms with RGBA8 texels, 2026-10-20) */
        scr->set[i].pub.allow16 = 1;
    }
    return 1;
}

/* (GPU timings, below: the GSP's events recorded) */
enum { PROF_LIST, PROF_FILL, PROF_FILL1, PROF_TRANSFER };
static void prof_event(void *data);

/* The smooth 3D as the options page sets it (0 off, 1 with the slider up,
 * 2 always): from the next frame. */
void NitroPort_ComposeSetSmooth3d(int smooth3d)
{
    __atomic_store_n(&sSmooth3d, smooth3d, __ATOMIC_RELEASE);
}

/* Linear memory the 3D's textures want left once the display is set up
 * (port_gx3d_pica.c's cache: up to 8 MB; the field's in a few MB). */
#define COMPOSE_TEXTURE_ROOM (4u * 1024u * 1024u)

static int compose_init(const ComposeConfig *config, int small);

/* The display on the GPU; with too little linear memory for it as it is
 * (a .3dsx started from the Homebrew Launcher in its small memory, about
 * 18 MB of it: issue #188), smaller: the fewest LCD framebuffers and the
 * 2D's textures a texel a pixel, rather than the classic display. */
int NitroPort_ComposeInit(const ComposeConfig *config)
{
    NitroPort_Log("[compose] linear memory: %lu KB free", (unsigned long)(linearSpaceFree() / 1024u));
    if (compose_init(config, 0)) {
        if (linearSpaceFree() >= COMPOSE_TEXTURE_ROOM) {
            return 1;
        }
        NitroPort_Log("[compose] %lu KB of linear memory left for the 3D's textures: the display made smaller",
                      (unsigned long)(linearSpaceFree() / 1024u));
        free_all();
    }
    if (compose_init(config, 1)) {
        NitroPort_Log("[compose] the display made smaller for the memory (%lu KB left): 3 frames on the way at "
                      "most, the 2D a texel a pixel",
                      (unsigned long)(linearSpaceFree() / 1024u));
        return 1;
    }
    return 0;
}

static int compose_init(const ComposeConfig *config, int small)
{
    static const char *const filters[] = { "sharp (the pixels as they are, 2x then bilinear)", "nearest",
                                           "smooth (bilinear)", "Scale2x" };
    int s, i, c, e;

    sConfig = *config;
    /* (GPU timings: the GSP's interrupts, recorded on profiled frames only) */
    gspSetEventCallback(GSPGPU_EVENT_P3D, prof_event, (void *)(uintptr_t)PROF_LIST, false);
    gspSetEventCallback(GSPGPU_EVENT_PSC0, prof_event, (void *)(uintptr_t)PROF_FILL, false);
    gspSetEventCallback(GSPGPU_EVENT_PSC1, prof_event, (void *)(uintptr_t)PROF_FILL1, false);
    gspSetEventCallback(GSPGPU_EVENT_PPF, prof_event, (void *)(uintptr_t)PROF_TRANSFER, false);
    /* The 3D's textures: nearest, as the DS's, unless remaster.txt says
     * otherwise (filter3d: see main_3ds.c). */
    NitroPort_PicaTextureFilter(config->filter3d);
    NitroPort_PicaShadowDebug(config->shadowDebug);
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
        int scale = !small && (config->filter == COMPOSE_FILTER_SHARP || config->filter == COMPOSE_FILTER_SCALE2X) ? 2 : 1;

        scr->width = s == 0 ? TOP_W : BOTTOM_W;
        scr->onLcd = -1;
        scr->lastLcd = -1;
        scr->nlcd = small ? LCD_BUFS_LEAST : s == 0 ? LCD_BUFS : LCD_BUFS_LOWER;
        for (i = 0; i < (s == 0 && config->stereo ? 2 : 1); i++) {
            int k;

            scr->target[i] = C3D_RenderTargetCreate(SCREEN_H, scr->width, s == 0 ? TOP_FB_FORMAT : GPU_RB_RGBA8,
                                                    GPU_RB_DEPTH24_STENCIL8);
            if (!scr->target[i]) {
                NitroPort_Log("[compose] no VRAM for the %s screen's render target", s ? "lower" : "upper");
                goto fail;
            }
            for (k = 0; k < scr->nlcd; k++) {
                scr->lcd[k][i] = linearAlloc((size_t)scr->width * SCREEN_H * 2u);
                if (!scr->lcd[k][i]) {
                    if (k >= LCD_BUFS_LEAST) {
                        /* (fewer, then: frames wait in fewer at once) */
                        NitroPort_Log("[compose] linear memory for %d LCD framebuffers of the %s screen's, not %d",
                                      k, s ? "lower" : "upper", scr->nlcd);
                        scr->nlcd = k;
                        break;
                    }
                    NitroPort_Log("[compose] not enough memory for the LCD framebuffers (%lu KB free)",
                                  (unsigned long)(linearSpaceFree() / 1024u));
                    goto fail;
                }
                memset(scr->lcd[k][i], 0, (size_t)scr->width * SCREEN_H * 2u);
            }
        }
        if (!alloc_sets(scr, canvasW, scale)) {
            /* Not enough memory at 2 texels a pixel: 1 (bilinear). */
            free_sets(scr);
            if (scale == 1 || !alloc_sets(scr, canvasW, 1)) {
                NitroPort_Log("[compose] not enough memory for the 2D layer textures (%lu KB free)",
                              (unsigned long)(linearSpaceFree() / 1024u));
                goto fail;
            }
            NitroPort_Log("[compose] not enough memory for the sharp 2D on the %s screen: smooth instead",
                          s ? "lower" : "upper");
        }
    }
    NitroPort_PicaEdgePruning(config->edges == 2);
    NitroPort_PicaEdgeCopy(config->edges != 3);
    if (config->edges) {
        u32 size = C3D_CalcDepthBufSize(SCREEN_H, TOP_W, GPU_RB_DEPTH24_STENCIL8);
        int d;

        sEdgeBlock = vramAlloc((size_t)size * 4u);
        if (!sEdgeBlock) {
            NitroPort_Log("[compose] no VRAM for the edge marking buffers: outlines off");
            sConfig.edges = 0;
        } else {
            for (d = 0; d < 4; d++) {
                sEdgeDepth[d] = (u8 *)sEdgeBlock + (size_t)size * (size_t)d;
            }
        }
    }
    /* (made whatever the setting: the options page can turn it on while
     * the game runs, NitroPort_ComposeSetSmooth3d) */
    __atomic_store_n(&sSmooth3d, config->smooth3d, __ATOMIC_RELEASE);
    {
        u32 colorSize = C3D_CalcColorBufSize(SMOOTH_W, SMOOTH_H, TOP_FB_FORMAT);
        u32 depthSize = C3D_CalcDepthBufSize(SMOOTH_W, SMOOTH_H, GPU_RB_DEPTH24_STENCIL8);
        void *depth = NULL;

        if (sEdgeBlock && C3D_CalcDepthBufSize(SCREEN_H, TOP_W, GPU_RB_DEPTH24_STENCIL8) * 4u >= depthSize) {
            depth = sEdgeBlock;
        } else {
            depth = sSmoothOwnDepth = vramAlloc(depthSize);
        }
        if (depth) {
            /* (the colour in the other VRAM bank from the depth, as
             * citro3d puts them: 0x1F000000-0x1F2FFFFF is bank A) */
            sSmoothColor = vramAllocAt(colorSize, (u32)depth < 0x1F300000u ? VRAM_ALLOC_B : VRAM_ALLOC_A);
            if (!sSmoothColor) {
                sSmoothColor = vramAlloc(colorSize);
            }
        }
        if (sSmoothColor &&
            NitroPort_PicaFrameBufMake(sSmoothFb, sizeof(sSmoothFb), sSmoothColor, depth, SMOOTH_W, SMOOTH_H,
                                       TOP_FB_FORMAT == GPU_RB_RGB565)) {
            sSmoothReady = 1;
            sSmoothDepth = depth;
            sSmoothMadeHalf = 0;
        } else {
            NitroPort_Log("[compose] no VRAM for the smooth 3D (drawn 2x2): the slider's 3D drawn as it is");
            if (sSmoothColor) {
                vramFree(sSmoothColor);
                sSmoothColor = NULL;
            }
            if (sSmoothOwnDepth) {
                vramFree(sSmoothOwnDepth);
                sSmoothOwnDepth = NULL;
            }
        }
    }
    for (s = 0; s < 2 && sEdgeDepth[0]; s++) {
        for (i = 0; i < 2 && sScreens[s].target[i]; i++) {
            int d;

            for (d = 0; d < 4; d++) {
                NitroPort_PicaFrameBufWithDepth(sScreens[s].edgeFb[i][d], PICA_FRAMEBUF_BYTES,
                                                sScreens[s].target[i], sEdgeDepth[d]);
            }
        }
    }
    sQuadsAll = linearAlloc(2u * MAX_QUAD_VERTICES * sizeof(NitroGx3dGpuVertex));
    if (!sQuadsAll) {
        NitroPort_Log("[compose] not enough memory for the quads");
        goto fail;
    }
    sQuads = sQuadsAll;
    /* The frames: two command lists and two queues of ours (frames in
     * flight); bound from now on, citro3d's own frames unused. */
    for (i = 0; i < 2; i++) {
        sGxQueue[i].entries = malloc(GX_QUEUE_ENTRIES * sizeof(gxCmdEntry_s));
        sGxQueue[i].maxEntries = GX_QUEUE_ENTRIES;
        if (!sGxQueue[i].entries) {
            goto fail;
        }
        gxCmdQueueSetCallback(&sGxQueue[i], gpu_finished, NULL);
    }
    if (!NitroPort_PicaFramesInit()) {
        NitroPort_Log("[compose] not enough memory for a second command list");
        goto fail;
    }
    /* The lower screen as the menu, after everything the display needs:
     * without the memory for its picture, the lower screen stays the DS's,
     * filling the screen. */
    for (i = 0; i < 2; i++) {
        sMenuTexData[i] = linearAlloc((size_t)MENU_TEX_W * MENU_TEX_H * 2u);
        sMapTexData[i] = linearAlloc((size_t)MAP_TEX_W * MAP_TEX_H * 2u);
    }
    if (!sMenuTexData[0] || !sMenuTexData[1] || !sMapTexData[0] || !sMapTexData[1]) {
        NitroPort_Log("[compose] no linear memory for the lower screen's menu: the DS's lower screen instead");
        for (i = 0; i < 2; i++) {
            if (sMenuTexData[i]) {
                linearFree(sMenuTexData[i]);
                sMenuTexData[i] = NULL;
            }
            if (sMapTexData[i]) {
                linearFree(sMapTexData[i]);
                sMapTexData[i] = NULL;
            }
        }
    }
    NitroPort_LowerMenuSetClock(NitroPort_ComposeVBlanks);   /* (the panel's fades) */
    NitroPort_LowerMenuSetAvailable(sMenuTexData[0] != NULL);
    sGxRecording = 0;
    sFlight = 0;
    GX_BindQueue(&sGxQueue[sGxRecording]);
    NitroPort_PicaGpuWait(flight_wait);
    sGxReady = 1;
    sReady = 1;
    NitroPort_Log("[compose] screens drawn by the GPU: %s, 2D %s, 3D %s%s%s%s%s",
                  config->wide ? "wide upper screen" : "the DS's width",
                  filters[config->filter & 3],
                  config->stereo ? "with depth from the slider" : "flat",
                  !sConfig.edges        ? ", no outlines"
                  : sConfig.edges == 2  ? ", outlines on the GPU (each pass only the polygons it needs)"
                  : sConfig.edges == 3  ? ", outlines on the GPU (all their moved passes drawn)"
                                        : ", outlines on the GPU",
                  !config->stereo ? ""
                  : sConfig.edges && !config->edges3d ? " (none with the slider up)" : "",
                  !sSmoothReady ? ""
                  : config->smooth3d == 1 ? ", with the slider up the upper screen drawn 2x2 and scaled down"
                                          : ", the upper screen drawn 2x2 and scaled down",
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
    /* The frame the GPU has, done first (its buffers are freed next). */
    flight_finish(1);
    gspSetEventCallback(GSPGPU_EVENT_P3D, NULL, NULL, false);
    gspSetEventCallback(GSPGPU_EVENT_PSC0, NULL, NULL, false);
    gspSetEventCallback(GSPGPU_EVENT_PSC1, NULL, NULL, false);
    gspSetEventCallback(GSPGPU_EVENT_PPF, NULL, NULL, false);
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
static void publish_whole(int e, const ComposeLayers *l, Screen *scr, LayerSet *set, int w, int h)
{
    uint16_t *c16 = sConverted16[e];
    int i, n = w * h;
    /* (the texture's rows the picture uses: tile rows, whole) */
    u32 rowsUsed = ((u32)h * scr->texScale + 8u + 7u) & ~7u;

    set->pub.heldBy = NULL;   /* (not tracked this way) */
    set->pub.over16 = 0;      /* (RGBA8 here) */
    for (i = 0; i < n; i++) {
        c16[i] = sToRgb565[l->under[i] & 0x7FFF];
    }
    if (sConfig.filter == COMPOSE_FILTER_SCALE2X && scr->texScale == 2) {
        NitroPort_Scale2xTiled16(c16, w, h, set->under, scr->texW);
    } else {
        NitroPort_Tile16(c16, w, h, set->under, scr->texW);
    }
    GSPGPU_FlushDataCache(set->under, (u32)scr->texW * rowsUsed * 2u);
    if (l->has3d) {
        /* Often there's nothing in front: then the texture isn't written.
         * (A tall picture's: the DS's lines, the 3D's.) */
        uint32_t *c32 = sConverted[e];

        if (NitroPort_PremultiplyOver(l->over, l->overAlpha, sAlpha16, c32, 0, w * DS_H)) {
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

/* A picture all one colour: that colour as RGB565, else -1. (In the field
 * the picture behind the 3D is often only the backdrop: then the frame's
 * clear paints it, rather than a quad the size of the screen: 1.3-2 ms of
 * the GPU's an eye, 2026-10-19.) */
static int uniform_color(const uint16_t *px, int n)
{
    const uint32_t *w = (const uint32_t *)px;
    uint32_t first;
    int i;

    if (n <= 0 || ((uintptr_t)px & 3u) || (n & 1)) {
        return -1;
    }
    first = (uint32_t)(px[0] & 0x7FFF) * 0x00010001u;
    for (i = 0; i < n / 2; i++) {
        if ((w[i] & 0x7FFF7FFFu) != first) {
            return -1;
        }
    }
    return sToRgb565[px[0] & 0x7FFF];
}

/* That colour as the GPU samples it from the RGB565 texture (and writes it
 * to an RGBA8 framebuffer): 0xRRGGBBAA. */
static u32 rgba_of_565(int c)
{
    u32 r = ((u32)c >> 11) & 31u, g = ((u32)c >> 5) & 63u, b = (u32)c & 31u;

    return (((r << 3) | (r >> 2)) << 24) | (((g << 2) | (g >> 4)) << 16) | (((b << 3) | (b >> 2)) << 8) | 0xFFu;
}

/* That colour as a fill of the upper screen's framebuffers (in their
 * format: a 16-bit fill takes the value's low half as it is). */
static u32 top_clear_color(int c)
{
    return TOP_FB_FORMAT == GPU_RB_RGB565 ? (u32)c & 0xFFFFu : rgba_of_565(c);
}

/* The rows of the layer in front of the 3D with anything drawn in them
 * (colour bit 15), and one more each side (the filter reads a texel past
 * them): the quad drawing it covers those (a text box: a quarter of the
 * screen, the GPU's time for it with it). Empty: *top >= *bottom. */
static void over_rows(const uint16_t *over, int w, int *top, int *bottom)
{
    const uint32_t *words = (const uint32_t *)over;
    int y, i, first = -1, last = -1;

    *top = 0;
    *bottom = DS_H;
    if (((uintptr_t)over & 3u) || (w & 1)) {
        return;   /* (all of it) */
    }
    for (y = 0; y < DS_H; y++) {
        const uint32_t *row = words + (size_t)y * (size_t)(w / 2);
        uint32_t any = 0;

        for (i = 0; i < w / 2; i++) {
            any |= row[i];
        }
        if (any & 0x80008000u) {
            if (first < 0) {
                first = y;
            }
            last = y;
        }
    }
    if (first < 0) {
        *bottom = 0;
        return;
    }
    *top = first > 0 ? first - 1 : 0;
    *bottom = last + 2 < DS_H ? last + 2 : DS_H;
}

/* The picture behind fills the screen (its colour can be the clear). */
static int under_fills_screen(const Screen *scr, const LayerSet *set)
{
    int canvasW = set->info.width > 0 ? set->info.width : DS_W;

    return set->underColor >= 0 && !set->info.displayOff && (float)canvasW * SCALE >= (float)scr->width - 0.5f;
}

static void flush_texture(void *data, uint32_t bytes)
{
    GSPGPU_FlushDataCache(data, bytes);
}

int NitroPort_ComposePublish(int engine, int screen)
{
    int e = engine ? 1 : 0, i, buf = -1;
    Screen *scr = &sScreens[screen ? 1 : 0];
    ComposeLayers *l = &sWork[e];
    LayerSet *set;
    u64 start = svcGetSystemTick();
    int w = l->width > 0 ? l->width : DS_W;
    int h = l->height > DS_H ? l->height : DS_H;

    if (!sReady || w > CANVAS_MAX || w * scr->texScale > scr->texW || w * h > WORK_PIXELS ||
        h * scr->texScale + 8 > scr->texH) {
        return 1;
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

            if (c->writing || c->held || (c->seq == newest && newest != 0) ||
                (int32_t)(__atomic_load_n(&sGpuDone, __ATOMIC_ACQUIRE) - c->flightSerial) < 0) {
                continue;   /* (in use, or the GPU still drawing a frame with it) */
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
        /* (both engines publishing to one screen as they swap; or one set
         * drawn by the GPU, one by the frame being built, the third the
         * newest: the caller draws it all again next time) */
        return 0;
    }
    set = &scr->set[buf];
    set->info = *l;
    set->info.under = NULL;
    set->info.over = NULL;
    set->info.overAlpha = NULL;
    set->overEmpty = 1;
    set->info.height = h;
    set->underColor = l->displayOff ? -1 : uniform_color(l->under, w * h);
    set->overTop = 0;
    set->overBottom = DS_H;
    if (!l->displayOff && l->has3d && l->over) {
        over_rows(l->over, w, &set->overTop, &set->overBottom);
    }
    if (!l->displayOff && sConfig.filter != COMPOSE_FILTER_SCALE2X && scr->texScale == 2) {
        set->overEmpty = !NitroPort_PublishDoubled(&sShadow[e], &set->pub, l->under, l->over, l->overAlpha,
                                                   l->has3d, w, h, set->under, set->over, scr->texW,
                                                   sToRgb565, sAlpha16, flush_texture);
    } else if (!l->displayOff) {
        publish_whole(e, l, scr, set, w, h);
    }
    LightLock_Lock(&sSetLock);
    set->writing = 0;
    set->seq = ++sSeq;
    LightLock_Unlock(&sSetLock);
    __atomic_fetch_add(&sPublished, 1u, __ATOMIC_RELAXED);
    __atomic_fetch_add(&sPublishTicks, svcGetSystemTick() - start, __ATOMIC_RELAXED);
    return 1;
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

    extern u32 gpuCmdBufSize, gpuCmdBufOffset;

    if (sQuadVertices + 6 > MAX_QUAD_VERTICES || gpuCmdBufSize - gpuCmdBufOffset < 1024u) {
        return;   /* (no room: in the quads, or in the command list) */
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

/* ---------------------------------------------------------- GPU timings
 * One frame every PROF_EVERY frames with a 3D scene: its command list cut
 * at each NitroPort_PicaProfileMark (NitroPort_PicaFrameSplit, as the
 * second eye's outline buffers already have it), and each part's
 * completion (the GPU's P3D interrupt; the fills' PSC0 and the transfers'
 * PPF too) timed on the GSP event thread. GSP runs the frame's queue in
 * order: a part's GPU time is from the event before it to its own. The
 * report once the frame's drawn (flight_finish). A frame's queue holds
 * GX_QUEUE_ENTRIES: PROF_MARKS cuts and the rest. */
#define PROF_EVERY 150u
#define PROF_MARKS 30
#define PROF_EVENTS 96
typedef struct ProfInfo {
    int stereo, edges, smooth, split, half;
    u64 t0;                                /* (its queue's start) */
} ProfInfo;
static ProfInfo sProfFlight;               /* (the frame in flight, when profiled) */
static int sProfFlightOn;
static int sProfOn;                        /* (this frame is profiled) */
static unsigned sProfFrames;
static int sProfMarks, sProfGroup;
static const char *sProfPart[PROF_MARKS + 1];
static int sProfPartGroup[PROF_MARKS + 1];
static int sProfLost;
static int sProfArmed;
static u32 sProfEvents;
static u64 sProfTick[PROF_EVENTS];
static u8 sProfType[PROF_EVENTS];
extern u32 gpuCmdBufOffset;

static void prof_event(void *data)
{
    u32 i;

    if (!__atomic_load_n(&sProfArmed, __ATOMIC_ACQUIRE)) {
        return;
    }
    i = __atomic_fetch_add(&sProfEvents, 1u, __ATOMIC_ACQ_REL);
    if (i < PROF_EVENTS) {
        sProfTick[i] = svcGetSystemTick();
        sProfType[i] = (u8)(uintptr_t)data;
    }
}

/* The command list cut here, if anything was drawn since the last cut:
 * forced, always (the frame needs it); else only on a profiled frame,
 * and up to PROF_MARKS cuts. A cut on a profiled frame is a part named
 * `part'. */
static void prof_split(const char *part, int forced)
{
    if (gpuCmdBufOffset == 0) {
        return;   /* (nothing drawn since the last cut: no part) */
    }
    if (sProfOn && sProfMarks < PROF_MARKS + 1 && (forced || sProfMarks < PROF_MARKS)) {
        sProfPart[sProfMarks] = part;
        sProfPartGroup[sProfMarks] = sProfGroup;
        sProfMarks++;
    } else if (!forced) {
        return;
    } else if (sProfOn) {
        sProfLost = 1;   /* (a part not named: the report would be off by one) */
    }
    NitroPort_PicaFrameSplit();
}

void NitroPort_PicaProfileMark(const char *part)
{
    if (sProfOn) {
        prof_split(part, 0);
    }
}

void NitroPort_PicaCut(const char *part)
{
    prof_split(part, 1);
}

/* The profiled frame's [gpu] lines: a line a screen (an eye), then the
 * clears, the transfers and the whole. */
static void prof_report(const ProfInfo *info)
{
    int stereo = info->stereo, edges = info->edges;
    u64 t0 = info->t0;
    static const char *const groups[3] = { "upper screen, left eye", "upper screen, right eye",
                                           "lower screen" };
    const double ms = 1000.0 / SYSCLOCK_ARM11;
    char line[400];
    size_t len = 0;
    u32 n = __atomic_load_n(&sProfEvents, __ATOMIC_ACQUIRE), i;
    u64 prev = t0;
    double fills = 0.0, transfers = 0.0, rest = 0.0;
    int mark = 0, group = -1, lists = 0;

    if (n > PROF_EVENTS) {
        n = PROF_EVENTS;
    }
    if (sProfLost) {
        NitroPort_Log("[gpu] a frame's parts not timed: more cuts than names");
        return;
    }
    line[0] = 0;
    for (i = 0; i < n; i++) {
        double d;

        if (sProfType[i] == PROF_FILL1) {
            continue;   /* (the second buffer of a fill: the queue ignores it too) */
        }
        d = (double)(sProfTick[i] - prev) * ms;
        prev = sProfTick[i];
        if (sProfType[i] == PROF_FILL) {
            fills += d;
        } else if (sProfType[i] == PROF_TRANSFER) {
            transfers += d;
        } else if (mark < sProfMarks) {
            if (sProfPartGroup[mark] != group) {
                if (len) {
                    NitroPort_Log("%s", line);
                }
                group = sProfPartGroup[mark];
                len = (size_t)snprintf(line, sizeof(line), "[gpu] %s (%s, outlines %s):",
                                       groups[group < 0 || group > 2 ? 2 : group],
                                       info->smooth && group != 2
                                           ? (info->split ? (stereo ? (info->half ? "stereo, drawn 2x1 then 1x1" : "stereo, drawn 2x2 then 1x1")
                                                                    : (info->half ? "mono, drawn 2x1 then 1x1" : "mono, drawn 2x2 then 1x1"))
                                                          : stereo ? (info->half ? "stereo, drawn 2x1" : "stereo, drawn 2x2")
                                                                   : (info->half ? "mono, drawn 2x1" : "mono, drawn 2x2"))
                                           : stereo ? "stereo" : "mono",
                                       !edges ? "off" : sConfig.edges == 2 ? "on, pruned"
                                       : sConfig.edges == 3 ? "on, all drawn" : "on");
            }
            if (len < sizeof(line)) {
                len += (size_t)snprintf(line + len, sizeof(line) - len, " %s %.1f ms;", sProfPart[mark], d);
            }
            mark++;
            lists++;
        } else {
            rest += d;
            lists++;
        }
    }
    if (len) {
        NitroPort_Log("%s", line);
    }
    NitroPort_Log("[gpu] the frame: the clears %.1f ms, the rest %.1f ms, the transfers (the outlines' copies, "
                  "to the screens) %.1f ms; "
                  "%.1f ms in all (%d parts of %d marked, %lu events)",
                  fills, rest, transfers, n ? (double)(sProfTick[n - 1] - t0) * ms : 0.0, lists, sProfMarks,
                  (unsigned long)n);
}

/* ------------------------------------------- the outlines with the slider up
 * With the slider up the outlines are drawn for each eye: in the field a
 * stereo frame took 28-35 ms of the GPU's in towns and 40-51 on busier
 * routes with them, 9-17 without (2026-10-20), against 33 ms a frame at 30
 * frames a second. So they're drawn while they fit: off after a few
 * stereo frames in a row over AUTO_OVER_MS, on again after a while of
 * frames that would fit with them (AUTO_UNDER_MS, the frame without them
 * times sAutoRatio: what they cost there, measured each time they go off
 * or on, the frames either side of it being much the same scene). */
#define AUTO_OVER_MS 33.0     /* (a frame at 30 a second: the CPU's part is done meanwhile) */
#define AUTO_UNDER_MS 29.0
#define AUTO_OVER_FRAMES 3
#define AUTO_UNDER_FRAMES 45
static int sAutoEdges = 1;               /* drawn now with the slider up */
static int sAutoOver, sAutoUnder, sAutoSwitched;   /* (1 just on, -1 just off) */
static float sAutoRatio = 3.0f;
static u64 sAutoOn, sAutoOff;            /* (the last stereo frames' GPU time with them, without) */
static u32 sStereoEdgeFrames;

static void auto_outlines(u64 ticks, int drawn)
{
    const double ms = (double)ticks * 1000.0 / SYSCLOCK_ARM11;

    if (drawn) {
        if (sAutoSwitched == 1 && sAutoOff > 0) {
            sAutoRatio = (float)ticks / (float)sAutoOff;
        }
        sAutoSwitched = 0;
        sAutoOn = ticks;
        sAutoOver = ms > AUTO_OVER_MS ? sAutoOver + 1 : 0;
        if (sAutoOver >= AUTO_OVER_FRAMES) {
            sAutoEdges = 0;
            sAutoSwitched = -1;
            sAutoOver = 0;
            sAutoUnder = 0;
            NitroPort_Log("[compose] the outlines with the slider up: off (a stereo frame took the GPU %.1f ms)", ms);
        }
    } else {
        if (sAutoSwitched == -1 && ticks > 0) {
            sAutoRatio = (float)sAutoOn / (float)ticks;
        }
        sAutoSwitched = 0;
        sAutoOff = ticks;
        sAutoUnder = ms * sAutoRatio < AUTO_UNDER_MS ? sAutoUnder + 1 : 0;
        if (sAutoUnder >= AUTO_UNDER_FRAMES) {
            sAutoEdges = 1;
            sAutoSwitched = 1;
            sAutoUnder = 0;
            sAutoOver = 0;
            NitroPort_Log("[compose] the outlines with the slider up: on (%.1f ms without them, about %.1f with)", ms,
                          ms * sAutoRatio);
        }
    }
    if (sAutoRatio < 1.5f) {
        sAutoRatio = 1.5f;
    }
    if (sAutoRatio > 5.0f) {
        sAutoRatio = 5.0f;
    }
}

/* ------------------------------------------------- 60 frames a second
 * With the in-between scenes (fps60) a frame has 16.7 ms of the GPU, not
 * 33. 60 frames a second come first: while frames don't fit, the upper
 * screen's 3D is drawn more cheaply, step by step (2x2, then 2x1, then
 * as it is), and only if they still don't, the scenes alone (30 a second,
 * 2x2 again: it fits in 33 ms). A step down after FPS60_OVER_FRAMES frames
 * in a row over FPS60_OVER_MS; a step up after FPS60_UNDER_FRAMES frames
 * in a row that would be under FPS60_UNDER_MS the next way up (the frame
 * as drawn times what that step costs: ratio, measured each time a step
 * is taken, the frames either side of it being much the same scene), more
 * of them (twice, four, eight times) after each step down, till the
 * steps have stayed put for a while (FPS60_SETTLED_FRAMES: 10 s). With the
 * slider down and up the GPU's work differs (outlines; each eye): each
 * has its own steps, and so do frames with the 3D on the lower screen
 * (never drawn 2x2: their only step is to 30). In the field (2026-10-20, a 3DS): slider up, 9-11
 * ms 2x2, 14-16.5 in the busiest places; slider down (with the outlines),
 * 14-15.5 2x2, 18-20 with a text box up. */
#define FPS60_OVER_MS 16.0
#define FPS60_UNDER_MS 14.5
#define FPS60_OVER_FRAMES 4
#define FPS60_UNDER_FRAMES 60
#define FPS60_SETTLED_FRAMES 600
#define FPS60_MAX_BACKOFF 3
enum { DRAWN_PLAIN = 1, DRAWN_HALF = 2, DRAWN_FULL = 3 };   /* (as it is, 2x1, 2x2) */
typedef struct Fps60 {
    int level;            /* 0: 30 a second (drawn the best way); 1-3: 60, drawn as DRAWN_* */
    int over, under, best, backoff;
    u32 sinceDown;
    float ratio[3];       /* [d]: a frame's GPU time drawn d + 1 / drawn d (d: DRAWN_PLAIN, DRAWN_HALF) */
    u64 lastTicks;
    u32 lastFrame;        /* (the last frame of this slider position's: which, how drawn) */
    int lastDrawn, lastSmoothable;
} Fps60;
static Fps60 sFps60[3] = {
    { DRAWN_FULL, 0, 0, 0, 0, 0, { 1.0f, 1.15f, 1.2f }, 0, 0, 0, 0 },   /* slider down */
    { DRAWN_FULL, 0, 0, 0, 0, 0, { 1.0f, 1.4f, 1.4f }, 0, 0, 0, 0 },    /* slider up */
    { DRAWN_FULL, 0, 0, 0, 0, 0, { 1.0f, 1.15f, 1.2f }, 0, 0, 0, 0 },   /* the 3D on the lower screen */
};
static u32 sFps60Frames;
static const char *const sDrawnName[4] = { "", "as it is", "2x1", "2x2" };
static const char *const sSliderName[3] = { "slider down", "slider up", "the 3D on the lower screen" };

/* Which steps a frame's: the slider's position's, or (the 3D on the lower
 * screen) their own. */
static int fps60_state(int stereo, int aOnTop)
{
    return !aOnTop ? 2 : stereo ? 1 : 0;
}

/* A finished frame with 3D: its GPU time, its steps (fps60_state), how
 * its upper screen's 3D was drawn and whether it could have been drawn
 * 2x2. */
static void auto_fps60(u64 ticks, int state, int drawn, int smoothable)
{
    Fps60 *f = &sFps60[state];
    const int stereo = state;   /* (for the log lines' names) */
    const double ms = (double)ticks * 1000.0 / SYSCLOCK_ARM11;
    int top = smoothable ? DRAWN_FULL : DRAWN_PLAIN, now;
    u32 frame = ++sFps60Frames;

    /* What a step costs: this frame against the one just before, drawn
     * the next way up or down. */
    if (f->lastDrawn && f->lastFrame + 1u == frame && f->lastSmoothable && smoothable && f->lastTicks && ticks &&
        (drawn == f->lastDrawn + 1 || drawn + 1 == f->lastDrawn)) {
        int d = drawn < f->lastDrawn ? drawn : f->lastDrawn;
        float r = drawn > f->lastDrawn ? (float)ticks / (float)f->lastTicks : (float)f->lastTicks / (float)ticks;

        f->ratio[d] = r < 1.02f ? 1.02f : r > 3.0f ? 3.0f : r;
    }
    f->lastDrawn = drawn;
    f->lastTicks = ticks;
    f->lastFrame = frame;
    f->lastSmoothable = smoothable;
    if (f->level >= 1) {
        now = f->level < top ? f->level : top;
        if (drawn > now) {
            return;   /* (built before the step down: the GPU had it already) */
        }
        f->over = ms > FPS60_OVER_MS ? f->over + 1 : 0;
        if (f->over >= FPS60_OVER_FRAMES) {
            f->level = now > DRAWN_PLAIN ? now - 1 : 0;
            f->over = 0;
            f->under = 0;
            f->sinceDown = 0;
            if (f->backoff < FPS60_MAX_BACKOFF) {
                f->backoff++;
            }
            if (f->level) {
                NitroPort_Log("[compose] 60 frames a second, %s: the 3D drawn %s for now (a frame took the GPU "
                              "%.1f ms drawn %s)", sSliderName[stereo], sDrawnName[f->level], ms,
                              sDrawnName[drawn]);
            } else {
                NitroPort_Log("[compose] 60 frames a second, %s: off for now (a frame took the GPU %.1f ms drawn %s)",
                              sSliderName[stereo], ms, sDrawnName[drawn]);
            }
            return;
        }
        if (f->sinceDown < FPS60_SETTLED_FRAMES && ++f->sinceDown == FPS60_SETTLED_FRAMES) {
            f->backoff = 0;
        }
        if (now < top && drawn == now) {
            double est = ms * f->ratio[now];

            f->under = est < FPS60_UNDER_MS ? f->under + 1 : 0;
            if (f->under >= (FPS60_UNDER_FRAMES << f->backoff)) {
                f->level = now + 1;
                f->under = 0;
                NitroPort_Log("[compose] 60 frames a second, %s: the 3D drawn %s again (%.1f ms a frame drawn %s, "
                              "about %.1f %s)", sSliderName[stereo], sDrawnName[f->level], ms,
                              sDrawnName[drawn], est, sDrawnName[f->level]);
            }
        } else {
            f->under = 0;
        }
    } else {
        /* 30 a second: back to 60 the best way that fits. */
        int d = drawn;
        double est = ms;

        while (d > DRAWN_PLAIN && est >= FPS60_UNDER_MS) {
            d--;
            est /= f->ratio[d];
        }
        if (est < FPS60_UNDER_MS) {
            f->best = f->under == 0 || d < f->best ? d : f->best;
            f->under++;
        } else {
            f->under = 0;
        }
        if (f->under >= (FPS60_UNDER_FRAMES << f->backoff)) {
            f->level = f->best;
            f->under = 0;
            f->over = 0;
            f->sinceDown = 0;
            NitroPort_Log("[compose] 60 frames a second, %s: on again, the 3D drawn %s (frames take the GPU %.1f ms "
                          "drawn %s, about %.1f %s)", sSliderName[stereo], sDrawnName[f->level], ms,
                          sDrawnName[drawn], est, sDrawnName[d]);
        }
    }
}

/* The slider's position as ComposeFrame takes it (stereo or not). */
static int stereo_for(float slider, int aOnTop)
{
    return sConfig.stereo && slider > 0.0f && sScreens[0].target[1] != NULL && aOnTop;
}

int NitroPort_ComposeWantsMid(float slider, int aOnTop)
{
    return sFps60[fps60_state(stereo_for(slider, aOnTop), aOnTop)].level >= 1;
}

/* How the upper screen's 3D is drawn now, the slider as it is: 60 frames
 * a second's step, or 2x2 at 30. */
static int fps60_drawn(int stereo)
{
    int level = sFps60[stereo ? 1 : 0].level;

    return level >= 1 ? level : DRAWN_FULL;
}

/* A queue of ours run dry: its frame drawn (the GSP event thread). */
static void gpu_finished(gxCmdQueue_s *queue)
{
    (void)queue;
    sGpuDoneTick = svcGetSystemTick();
    __atomic_add_fetch(&sGpuDone, 1u, __ATOMIC_ACQ_REL);
}

/* The frame the GPU has, once drawn (block: waiting for it): its queue
 * stopped and emptied (the next frame's, after this one's turn), its 2D
 * let go, its textures free to change, its GPU time and [gpu] report. */
static void flight_finish(int block)
{
    gxCmdQueue_s *q;

    if (!sFlight) {
        return;
    }
    if ((int32_t)(__atomic_load_n(&sGpuDone, __ATOMIC_ACQUIRE) - sGpuSubmitted) < 0) {
        u64 t;

        if (!block) {
            return;
        }
        t = svcGetSystemTick();
        NitroPort_PicaStage("waiting for the GPU (the frame before)");
        for (;;) {
            gxCmdQueue_s *fq = &sGxQueue[sFlightQueue];
            u64 since = svcGetSystemTick();

            gxCmdQueueWait(fq, -1);
            /* (the callback comes a moment after the queue's end) */
            while ((int32_t)(__atomic_load_n(&sGpuDone, __ATOMIC_ACQUIRE) - sGpuSubmitted) < 0 &&
                   svcGetSystemTick() - since < SYSCLOCK_ARM11 / 5) {
                svcSleepThread(50000LL);
            }
            if ((int32_t)(__atomic_load_n(&sGpuDone, __ATOMIC_ACQUIRE) - sGpuSubmitted) >= 0) {
                break;
            }
            /* Not running, not finished (stopped from outside: the HOME
             * menu's coming): run on from where it is. */
            NitroPort_Log("[compose] a frame's GPU queue stopped at %u of %u: run again", (unsigned)fq->curEntry,
                          (unsigned)fq->numEntries);
            if (fq->lastEntry >= fq->numEntries) {
                __atomic_add_fetch(&sGpuDone, 1u, __ATOMIC_ACQ_REL);   /* (all of it was done) */
                sGpuDoneTick = svcGetSystemTick();
                break;
            }
            fq->curEntry = fq->lastEntry;   /* (what was sent but not done, sent again) */
            gxCmdQueueRun(fq);
        }
        sWaitTicks += svcGetSystemTick() - t;
    }
    q = &sGxQueue[sFlightQueue];
    gxCmdQueueStop(q);
    gxCmdQueueClear(q);
    sFlight = 0;
    sLastGpuTicks = sGpuDoneTick - sGpuRunTick;
    /* (not a frame with an H-blank effect's runs: see LINE_RUNS_MAX) */
    if (sFlightStereo && sFlightEdgesWanted && sFlightRuns <= 1) {
        auto_outlines(sLastGpuTicks, sFlightEdges);
    }
    if (sFlight3D && sFlightRuns <= 1) {
        auto_fps60(sLastGpuTicks, sFlightState, sFlightDrawn, sFlightSmoothable);
    }
    sGpuTicks += sLastGpuTicks;
    NitroPort_PicaFrameDone(sFlightMark);
    if (sProfFlightOn) {
        /* (the GSP event thread runs the queue's end before its own
         * callbacks: a moment for the last event's time) */
        svcSleepThread(500000);
        __atomic_store_n(&sProfArmed, 0, __ATOMIC_RELEASE);
        prof_report(&sProfFlight);
        sProfFlightOn = 0;
    }
}

/* (port_gx3d_pica.c: a texture the GPU may be reading is to change) */
static void flight_wait(void)
{
    flight_finish(1);
}

int NitroPort_ComposeGpuBusy(void)
{
    return sReady && (int32_t)(__atomic_load_n(&sGpuDone, __ATOMIC_ACQUIRE) - sGpuSubmitted) < 0;
}

/* The layer textures, described for the GPU. */
static C3D_Tex sUnderTex, sOverTex, sPanelUpTex;

/* (an H-blank effect: ComposeLayers.scroll3dLines) The 3D's lines in runs
 * of the same scroll, each drawn by itself, moved by it, inside its rows
 * (the scissor): at most LINE_RUNS_MAX (LINE_RUNS_STEREO an eye with the
 * slider up), each a scene drawn again (its commands and vertices; the
 * pixels only inside the run), and no more than the command list has room
 * for (all of the frame's together a third of the room at its start, each
 * run's commands as many as a scene's last time, sSceneWords; both eyes
 * the same number: runs_most).
 * With more, k runs in a row (k odd: lines sliding apart by turns stay
 * apart by turns) are one, its scroll the middle line's: the grass's
 * slices 2 lines high, 96 of them, drawn 10 or 18 lines high. Frames with
 * runs aren't counted by the 60 frames a second steps (a moment's effect
 * shouldn't step the 3D down for the next minute). */
#define LINE_RUNS_MAX 24
#define LINE_RUNS_STEREO 12
static u32 sSceneWords;      /* (a scene's commands, in words, the last time measured) */
static int sFrameLineRuns;   /* (the most runs of a screen in the frame being built) */
static int sFrameRunsMost;   /* (the most a screen may have, the frame being built: both eyes the same) */

/* The most runs this frame's screens may have: LINE_RUNS_MAX (or
 * LINE_RUNS_STEREO), no more than a third of the command list's room
 * for them all. */
static void runs_most(int stereo)
{
    extern u32 gpuCmdBufSize, gpuCmdBufOffset;
    int most = stereo ? LINE_RUNS_STEREO : LINE_RUNS_MAX;

    if (sSceneWords > 0) {
        u32 room = (gpuCmdBufSize - gpuCmdBufOffset) / 3u / (stereo ? 2u : 1u) / sSceneWords;

        most = room < 1u ? 1 : room < (u32)most ? (int)room : most;
    }
    sFrameRunsMost = most;
    sFrameLineRuns = 0;
}
typedef struct LineRun {
    short y0, y1, scroll;
} LineRun;

static int scroll_signed(int scroll)
{
    return scroll >= 256 ? scroll - 512 : scroll;
}

static int line_runs(const ComposeLayers *info, LineRun *runs, int most)
{
    LineRun all[DS_H];
    int n = 0, y, k, i, out = 0;

    for (y = 0; y < DS_H; y++) {
        int scroll = scroll_signed(info->scroll3dLine[y] & 0x1FF);

        if (n > 0 && all[n - 1].scroll == scroll) {
            all[n - 1].y1 = (short)(y + 1);
        } else {
            all[n].y0 = (short)y;
            all[n].y1 = (short)(y + 1);
            all[n].scroll = (short)scroll;
            n++;
        }
    }
    k = (n + most - 1) / most;
    if (k <= 1) {
        memcpy(runs, all, (size_t)n * sizeof(LineRun));
        return n;
    }
    if ((k & 1) == 0) {
        k++;
    }
    for (i = 0; i < n; i += k) {
        int last = i + k < n ? i + k - 1 : n - 1, mid = all[(i + last) / 2].y0 + (all[(i + last) / 2].y1 - all[(i + last) / 2].y0) / 2;

        runs[out].y0 = all[i].y0;
        runs[out].y1 = all[last].y1;
        runs[out].scroll = (short)scroll_signed(info->scroll3dLine[mid] & 0x1FF);
        out++;
    }
    return out;
}

/* A brightness effect on the 3D (BLDCNT with BG0 as a first target),
 * over its place: alpha blending with the layer behind can't be done
 * after the fact (it's left as drawn). The compositor's own view. */
static void tint_3d(const ComposeLayers *info, const PicaRect *rect)
{
    if (info->tint3d == 2 || info->tint3d == 3) {
        u32 a = (u32)(info->evy > 16 ? 16 : info->evy) * 255u / 16u;

        quad_combiner(0);
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA, GPU_ONE, GPU_ZERO);
        quad(rect->x0, rect->y0, rect->x1, rect->y1, 0.0f, 0, 0, 0, 0, (info->tint3d == 2 ? 0xFFFFFF00u : 0x00000000u) | a);
    }
}

/* The master brightness: the whole screen towards white or black by
 * factor / 16 (the menu's frame too, on the lower screen: it fades with
 * the game's). */
static void master_brightness(const Screen *scr, const ComposeLayers *info)
{
    int mode = (info->masterBright >> 14) & 3, factor = info->masterBright & 31;

    if (factor > 16) {
        factor = 16;
    }
    if (factor && (mode == 1 || mode == 2)) {
        quad_combiner(0);
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA, GPU_ONE, GPU_ZERO);
        quad(0, 0, (float)scr->width, SCREEN_H, 0.0f, 0, 0, 0, 0,
             (mode == 1 ? 0xFFFFFF00u : 0x00000000u) | ((u32)factor * 255u / 16u));
    }
}

int NitroPort_ComposeLowerMenuShown(void)
{
    return __atomic_load_n(&sScreens[1].shownMenu, __ATOMIC_ACQUIRE);
}

/* The menu's picture into the texture not used last, when it's changed:
 * only the rows it's behind on (the map's few, as the player walks; the
 * other texture keeps them to catch up on next time). */
static void menu_texture(void)
{
    const uint32_t *pic;
    uint32_t serial;
    int i, b, y0, y1, k;

    if (sMenuTexCur >= 0 && NitroPort_LowerMenuSerial() == sMenuTexSerial) {
        return;
    }
    b = sMenuTexCur == 0 ? 1 : 0;
    /* (copied under the lock, converted after it: the game's thread waits
     * on it for as little as can be) */
    pic = NitroPort_LowerMenuLockFrame(&serial);
    NitroPort_LowerMenuTakeDirtyRows(&y0, &y1);
    for (k = 0; k < 2 && y0 < y1; k++) {
        if (sMenuStaleY0[k] >= sMenuStaleY1[k]) {
            sMenuStaleY0[k] = y0;
            sMenuStaleY1[k] = y1;
        } else {
            sMenuStaleY0[k] = y0 < sMenuStaleY0[k] ? y0 : sMenuStaleY0[k];
            sMenuStaleY1[k] = y1 > sMenuStaleY1[k] ? y1 : sMenuStaleY1[k];
        }
    }
    /* (whole tiles: 8 rows) */
    y0 = sMenuStaleY0[b] & ~7;
    y1 = (sMenuStaleY1[b] + 7) & ~7;
    y1 = y1 > NITRO_LOWERMENU_H ? NITRO_LOWERMENU_H : y1;
    if (y0 < y1) {
        memcpy(sMenuCopy + y0 * NITRO_LOWERMENU_W, pic + y0 * NITRO_LOWERMENU_W,
               (size_t)(y1 - y0) * NITRO_LOWERMENU_W * sizeof(uint32_t));
    }
    NitroPort_LowerMenuUnlockFrame();
    if (y0 < y1) {
        for (i = y0 * NITRO_LOWERMENU_W; i < y1 * NITRO_LOWERMENU_W; i++) {
            uint32_t c = sMenuCopy[i];

            sMenu565[i] = (uint16_t)(((c >> 27) << 11) | (((c >> 18) & 63u) << 5) | ((c >> 11) & 31u));
        }
        NitroPort_Tile16(sMenu565 + y0 * NITRO_LOWERMENU_W, NITRO_LOWERMENU_W, y1 - y0,
                         (uint16_t *)sMenuTexData[b] + (y0 / 8) * (MENU_TEX_W / 8) * 64, MENU_TEX_W);
        GSPGPU_FlushDataCache((uint16_t *)sMenuTexData[b] + (y0 / 8) * (MENU_TEX_W / 8) * 64,
                              (u32)MENU_TEX_W * (u32)(y1 - y0) * 2u);
    }
    sMenuStaleY0[b] = NITRO_LOWERMENU_H;
    sMenuStaleY1[b] = 0;
    sMenuTexCur = b;
    sMenuTexSerial = serial;
}

/* The map's picture into the texture not used last, when it's changed: as
 * the menu's (the rows it's behind on). Not while the map's drawer has its
 * lock (the display doesn't wait on it: the next frame; sMapTexSerial says
 * what's in the texture). 0 if there's no picture in a texture yet. */
static int map_texture(void)
{
    const uint32_t *pic;
    uint32_t serial;
    int i, b, y0, y1, k;

    if (sMapTexCur >= 0 && NitroPort_LowerMenuMapSerial() == sMapTexSerial) {
        return 1;
    }
    b = sMapTexCur == 0 ? 1 : 0;
    if (!NitroPort_LowerMenuTryLockMap(&pic, &serial)) {
        return sMapTexCur >= 0;
    }
    if (pic == NULL) {
        NitroPort_LowerMenuUnlockMap();
        return 0;
    }
    NitroPort_LowerMenuTakeMapDirtyRows(&y0, &y1);
    for (k = 0; k < 2 && y0 < y1; k++) {
        if (sMapStaleY0[k] >= sMapStaleY1[k]) {
            sMapStaleY0[k] = y0;
            sMapStaleY1[k] = y1;
        } else {
            sMapStaleY0[k] = y0 < sMapStaleY0[k] ? y0 : sMapStaleY0[k];
            sMapStaleY1[k] = y1 > sMapStaleY1[k] ? y1 : sMapStaleY1[k];
        }
    }
    /* (whole tiles: 8 rows) */
    y0 = sMapStaleY0[b] & ~7;
    y1 = (sMapStaleY1[b] + 7) & ~7;
    y1 = y1 > NITRO_LOWERMENU_PANEL_H ? NITRO_LOWERMENU_PANEL_H : y1;
    if (y0 < y1) {
        memcpy(sMapCopy + y0 * NITRO_LOWERMENU_PANEL_W, pic + y0 * NITRO_LOWERMENU_PANEL_W,
               (size_t)(y1 - y0) * NITRO_LOWERMENU_PANEL_W * sizeof(uint32_t));
    }
    NitroPort_LowerMenuUnlockMap();
    if (y0 < y1) {
        for (i = y0 * NITRO_LOWERMENU_PANEL_W; i < y1 * NITRO_LOWERMENU_PANEL_W; i++) {
            uint32_t c = sMapCopy[i];

            sMap565[i] = (uint16_t)(((c >> 27) << 11) | (((c >> 18) & 63u) << 5) | ((c >> 11) & 31u));
        }
        NitroPort_Tile16(sMap565 + y0 * NITRO_LOWERMENU_PANEL_W, NITRO_LOWERMENU_PANEL_W, y1 - y0,
                         (uint16_t *)sMapTexData[b] + (y0 / 8) * (MAP_TEX_W / 8) * 64, MAP_TEX_W);
        GSPGPU_FlushDataCache((uint16_t *)sMapTexData[b] + (y0 / 8) * (MAP_TEX_W / 8) * 64,
                              (u32)MAP_TEX_W * (u32)(y1 - y0) * 2u);
    }
    sMapStaleY0[b] = NITRO_LOWERMENU_PANEL_H;
    sMapStaleY1[b] = 0;
    sMapTexCur = b;
    sMapTexSerial = serial;
    return 1;
}

/* The lower screen as the menu: its picture (a texel a pixel), and in its
 * panel the map's picture (a texel a pixel too), or the DS's lower screen
 * at 15/16, a canvas 247 rows tall: the DS's 192 in the middle, its top row
 * carried on above them and its bottom row below (the Pokétch's strap); or,
 * the picture drawn tall (the game's NitroPort_LowerMenuSetTall, as the
 * drawer had it), its first 247 rows. Over the panel, black as dark as its
 * fade (NitroPort_LowerMenuPanelFade). (u1, vTop, vBottom: the DS's 192
 * rows in their texture.) */
/* The 3D in the menu's panel (an app shown there with its 3D: the
 * summary's Pokemon, the party's sparkles): the DS's 256 x 192 at rect,
 * clipped to it; then the layers in front of it, over the same rows. */
static void draw_menu_3d(const Screen *scr, const LayerSet *set, const NitroGx3dScene *scene, C3D_FrameBuf *fb,
                         const PicaRect *rect, float u1, float vTop, float vBottom, int linear)
{
    const ComposeLayers *info = &set->info;
    C3D_Mtx pixels;
    PicaView view;
    uint16_t rearColor;
    unsigned rearAlpha;
    u32 sy = (u32)fb->width / SCREEN_H, sx = (u32)fb->height / (u32)scr->width;
    int dir;

    memset(&view, 0, sizeof(view));
    view.fb = fb;
    for (dir = 0; dir < 4; dir++) {
        view.edgeFb[dir] = NULL;
    }
    view.screenWidth = scr->width;
    view.screenHeight = SCREEN_H;
    view.rect = *rect;
    view.rect.x0 -= (float)scroll_signed(info->scroll3d & 0x1FF) * (rect->x1 - rect->x0) / DS_W;
    view.rect.x1 -= (float)scroll_signed(info->scroll3d & 0x1FF) * (rect->x1 - rect->x0) / DS_W;
    view.stereo = 0.0f;
    view.convergence = sConfig.convergence > 0.0f ? sConfig.convergence : sSceneConvergence;
    view.part = PICA_DRAW_ALL;
    /* (only the panel's pixels: screen pixel (x, y) is the framebuffer's
     * (height - 1 - y, width - 1 - x)) */
    view.scissor = 1;
    view.scissorRect[0] = (u32)((float)SCREEN_H - rect->y1 + 0.5f) * sy;
    view.scissorRect[1] = (u32)((float)scr->width - rect->x1 + 0.5f) * sx;
    view.scissorRect[2] = (u32)((float)SCREEN_H - rect->y0 + 0.5f) * sy;
    view.scissorRect[3] = (u32)((float)scr->width - rect->x0 + 0.5f) * sx;
    C3D_SetScissor(GPU_SCISSOR_NORMAL, view.scissorRect[0], view.scissorRect[1], view.scissorRect[2], view.scissorRect[3]);
    NitroPort_PicaRearPlane(&rearColor, &rearAlpha);
    if (rearAlpha) {
        quad_combiner(0);
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA, GPU_ONE, GPU_ZERO);
        quad(rect->x0, rect->y0, rect->x1, rect->y1, -1.0f, 0, 0, 0, 0, rgba_of(rearColor, rearAlpha * 255u / 31u));
    }
    NitroPort_PicaDrawScene(&view);
    /* (the compositor's own view again: the scene took it off) */
    C3D_SetFrameBuf(fb);
    C3D_SetViewport(0, 0, fb->width, fb->height);
    NitroPort_PicaPixelMatrix(&pixels, scr->width, SCREEN_H);
    NitroPort_PicaSetView(&pixels, 0.0f, 1.0f);
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    C3D_StencilTest(false, GPU_ALWAYS, 0, 0xFF, 0xFF);
    C3D_AlphaTest(false, GPU_ALWAYS, 0);
    C3D_SetScissor(GPU_SCISSOR_NORMAL, view.scissorRect[0], view.scissorRect[1], view.scissorRect[2], view.scissorRect[3]);
    tint_3d(info, rect);
    C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
    if (!set->overEmpty && set->overTop < set->overBottom) {
        make_texture(&sOverTex, set->over, set->pub.over16 ? GPU_RGBA5551 : GPU_RGBA8, scr->texW, scr->texH, linear,
                     GPU_CLAMP_TO_EDGE);
        C3D_TexBind(0, &sOverTex);
        quad_combiner(1);
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ONE_MINUS_SRC_ALPHA, GPU_ONE, GPU_ONE_MINUS_SRC_ALPHA);
        quad(rect->x0, rect->y0, rect->x1, rect->y1, 0.0f, 0, vTop, u1, vBottom, 0xFFFFFFFFu);
    }
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
}

/* The panel put together (nitro_port_lowermenu.h): each run's rows from
 * the lower screen's picture (bound) or the upper screen's (sPanelUp: its
 * DS's 256 columns), a row after another, upside down or one row again;
 * the rows no run covers as usual; the tabs' rows left as the menu's
 * picture has them (the tabs). Then the sides' colours, at the panel's
 * outer two pixel columns each side, and the upper screen's own master
 * brightness over its rows. */
#define PANEL_PIECES_MAX 32
typedef struct PanelPiece {
    int y, rows, from, srcY, step;
} PanelPiece;

static int panel_pieces(int tall, PanelPiece *out)
{
    int n = 0, y = 0;

    while (y < NITRO_LOWERMENU_CANVAS_H && n < PANEL_PIECES_MAX) {
        int i, next = NITRO_LOWERMENU_CANVAS_H, run = -1;

        for (i = 0; i < sPanelRunCount; i++) {
            const NitroLowerPanelRun *r = &sPanelRuns[i];

            if (y >= r->y && y < r->y + r->rows) {
                run = i;
                break;
            }
            if (r->y > y && r->y < next) {
                next = r->y;
            }
        }
        if (run >= 0) {
            const NitroLowerPanelRun *r = &sPanelRuns[run];
            int end = r->y + r->rows;

            /* (overlapping runs: the first one has a row, as for touches) */
            for (i = 0; i < run; i++) {
                if (sPanelRuns[i].y > y && sPanelRuns[i].y < end) {
                    end = sPanelRuns[i].y;
                }
            }
            out[n].y = y;
            out[n].rows = end - y;
            out[n].from = r->from;
            out[n].srcY = r->srcY + (y - r->y) * r->step;
            out[n].step = r->step;
        } else {
            /* (as usual: tall, the canvas's row the picture's; else the
             * DS's 192 from row 27, its top row above them, its bottom
             * row below) */
            out[n].y = y;
            out[n].from = NITRO_LOWERMENU_FROM_LOWER;
            if (tall) {
                out[n].rows = next - y;
                out[n].srcY = y;
                out[n].step = 1;
            } else if (y < NITRO_LOWERMENU_DS_ROW0) {
                out[n].rows = (next < NITRO_LOWERMENU_DS_ROW0 ? next : NITRO_LOWERMENU_DS_ROW0) - y;
                out[n].srcY = 0;
                out[n].step = 0;
            } else if (y < NITRO_LOWERMENU_DS_ROW0 + DS_H) {
                out[n].rows = (next < NITRO_LOWERMENU_DS_ROW0 + DS_H ? next : NITRO_LOWERMENU_DS_ROW0 + DS_H) - y;
                out[n].srcY = y - NITRO_LOWERMENU_DS_ROW0;
                out[n].step = 1;
            } else {
                out[n].rows = next - y;
                out[n].srcY = DS_H - 1;
                out[n].step = 0;
            }
        }
        y += out[n].rows;
        n++;
    }
    return n;
}

/* A piece's quad: its rows' texture rows, from vTop a row (row, in v) each. */
static void panel_piece_quad(const PanelPiece *p, float x0, float x1, float u0, float u1, float vTop, float row)
{
    const float k = (float)NITRO_LOWERMENU_PANEL_H / (float)NITRO_LOWERMENU_CANVAS_H;
    float ya = NITRO_LOWERMENU_PANEL_Y0 + (float)p->y * k, yb = NITRO_LOWERMENU_PANEL_Y0 + (float)(p->y + p->rows) * k;
    float va, vb;

    if (p->step > 0) {
        va = vTop - (float)p->srcY * row;
        vb = vTop - (float)(p->srcY + p->rows) * row;
    } else if (p->step < 0) {
        va = vTop - (float)(p->srcY + 1) * row;
        vb = vTop - (float)(p->srcY + 1 - p->rows) * row;
    } else {
        va = vb = vTop - ((float)p->srcY + 0.5f) * row;
    }
    quad(x0, ya, x1, yb, 0.0f, u0, va, u1, vb, 0xFFFFFFFFu);
}

static u32 rgba_of_side(unsigned short side)
{
    return ((u32)widen5(side & 31u) << 24) | ((u32)widen5((side >> 5) & 31u) << 16) |
           ((u32)widen5((side >> 10) & 31u) << 8) | 0xFFu;
}

static void draw_panel_runs(const LayerSet *set, float u1, float vTop, float vBottom, int linear)
{
    const float k = (float)NITRO_LOWERMENU_PANEL_H / (float)NITRO_LOWERMENU_CANVAS_H;
    float x0 = NITRO_LOWERMENU_PANEL_X0, x1 = x0 + NITRO_LOWERMENU_PANEL_W;
    float row = (vTop - vBottom) / (float)DS_H;
    PanelPiece pieces[PANEL_PIECES_MAX];
    int n = panel_pieces(set->info.height >= NITRO_LOWERMENU_CANVAS_H, pieces), i, side;

    /* The lower screen's (its texture bound). */
    for (i = 0; i < n; i++) {
        if (pieces[i].from == NITRO_LOWERMENU_FROM_LOWER) {
            panel_piece_quad(&pieces[i], x0, x1, 0.0f, u1, vTop, row);
        }
    }
    /* The upper screen's: its DS's 256 columns (in the middle of a wider
     * picture). */
    if (sPanelUp != NULL && !sPanelUp->info.displayOff) {
        const Screen *up = &sScreens[0];
        int w = sPanelUp->info.width > 0 ? sPanelUp->info.width : DS_W;
        float uA = (float)((w - DS_W) / 2 * up->texScale) / (float)up->texW;
        float uB = uA + (float)(DS_W * up->texScale) / (float)up->texW;
        float vT = 1.0f, vB = 1.0f - (float)(DS_H * up->texScale) / (float)up->texH;
        int mode = (sPanelUp->info.masterBright >> 14) & 3, factor = sPanelUp->info.masterBright & 31;

        if (sConfig.flipV) {
            float t = vT;

            vT = vB;
            vB = t;
        }
        make_texture(&sPanelUpTex, sPanelUp->under, GPU_RGB565, up->texW, up->texH, linear, GPU_CLAMP_TO_EDGE);
        C3D_TexBind(0, &sPanelUpTex);
        for (i = 0; i < n; i++) {
            if (pieces[i].from == NITRO_LOWERMENU_FROM_UPPER) {
                panel_piece_quad(&pieces[i], x0, x1, uA, uB, vT, (vT - vB) / (float)DS_H);
            }
        }
        /* (its master brightness, over its rows) */
        factor = factor > 16 ? 16 : factor;
        if (factor && (mode == 1 || mode == 2)) {
            quad_combiner(0);
            C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA, GPU_ONE, GPU_ZERO);
            for (i = 0; i < n; i++) {
                if (pieces[i].from == NITRO_LOWERMENU_FROM_UPPER) {
                    quad(x0, NITRO_LOWERMENU_PANEL_Y0 + (float)pieces[i].y * k, x1,
                         NITRO_LOWERMENU_PANEL_Y0 + (float)(pieces[i].y + pieces[i].rows) * k, 0.0f, 0, 0, 0, 0,
                         (mode == 1 ? 0xFFFFFF00u : 0x00000000u) | ((u32)factor * 255u / 16u));
                }
            }
            C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
            quad_combiner(1);
        }
    } else {
        /* (no picture of it yet: black) */
        quad_combiner(0);
        for (i = 0; i < n; i++) {
            if (pieces[i].from == NITRO_LOWERMENU_FROM_UPPER) {
                quad(x0, NITRO_LOWERMENU_PANEL_Y0 + (float)pieces[i].y * k, x1,
                     NITRO_LOWERMENU_PANEL_Y0 + (float)(pieces[i].y + pieces[i].rows) * k, 0.0f, 0, 0, 0, 0,
                     0x000000FFu);
            }
        }
        quad_combiner(1);
    }
    /* The sides: the outer pixel column each side, then the one inside it. */
    quad_combiner(0);
    for (i = 0; i < sPanelRunCount; i++) {
        const NitroLowerPanelRun *r = &sPanelRuns[i];
        float ya = NITRO_LOWERMENU_PANEL_Y0 + (float)r->y * k, yb = NITRO_LOWERMENU_PANEL_Y0 + (float)(r->y + r->rows) * k;

        for (side = 0; side < 2; side++) {
            unsigned short c = side == 0 ? r->sides : r->sidesIn;

            if (c & 0x8000u) {
                quad(x0 + (float)side, ya, x0 + (float)side + 1.0f, yb, 0.0f, 0, 0, 0, 0, rgba_of_side(c));
                quad(x1 - (float)side - 1.0f, ya, x1 - (float)side, yb, 0.0f, 0, 0, 0, 0, rgba_of_side(c));
            }
        }
    }
    quad_combiner(1);
}

static void draw_menu(const Screen *scr, const LayerSet *set, float u1, float vTop, float vBottom, int linear,
                      const NitroGx3dScene *scene3d, C3D_FrameBuf *fb)
{
    const float k = (float)NITRO_LOWERMENU_PANEL_H / (float)NITRO_LOWERMENU_CANVAS_H;
    float x0 = NITRO_LOWERMENU_PANEL_X0, x1 = x0 + NITRO_LOWERMENU_PANEL_W, y0 = NITRO_LOWERMENU_PANEL_Y0;
    float yA = y0 + NITRO_LOWERMENU_DS_ROW0 * k, yB = y0 + (NITRO_LOWERMENU_DS_ROW0 + DS_H) * k;
    float yEnd = y0 + NITRO_LOWERMENU_PANEL_H;
    float row = (vTop - vBottom) / (float)DS_H;   /* a DS row, in v */
    float vRow0 = vTop - 0.5f * row, vRowLast = vBottom + 0.5f * row;

    float mTop = 1.0f, mBottom = 1.0f - (float)NITRO_LOWERMENU_H / MENU_TEX_H;
    float pTop = 1.0f, pBottom = 1.0f - (float)NITRO_LOWERMENU_PANEL_H / MAP_TEX_H;

    if (sConfig.flipV) {   /* (as the DS's pictures: see the flip setting) */
        float t = mTop;

        mTop = mBottom;
        mBottom = t;
        t = pTop;
        pTop = pBottom;
        pBottom = t;
    }
    menu_texture();
    make_texture(&sMenuTex, sMenuTexData[sMenuTexCur], GPU_RGB565, MENU_TEX_W, MENU_TEX_H, 0, GPU_CLAMP_TO_EDGE);
    C3D_TexBind(0, &sMenuTex);
    quad_combiner(1);
    quad(0, 0, NITRO_LOWERMENU_W, NITRO_LOWERMENU_H, 0.0f, 0, mTop, (float)NITRO_LOWERMENU_W / MENU_TEX_W, mBottom,
         0xFFFFFFFFu);
    if (sMenuPanel == NITRO_LOWERMENU_PANEL_MAP) {
        /* (the map: none drawn yet, the panel black, PanelFade) */
        if (map_texture()) {
            make_texture(&sMapTex, sMapTexData[sMapTexCur], GPU_RGB565, MAP_TEX_W, MAP_TEX_H, 0, GPU_CLAMP_TO_EDGE);
            C3D_TexBind(0, &sMapTex);
            quad(x0, y0, x1, yEnd, 0.0f, 0, pTop, (float)NITRO_LOWERMENU_PANEL_W / MAP_TEX_W, pBottom, 0xFFFFFFFFu);
        }
    } else {
        PicaRect rect3d;

        make_texture(&sUnderTex, set->under, GPU_RGB565, scr->texW, scr->texH, linear, GPU_CLAMP_TO_EDGE);
        C3D_TexBind(0, &sUnderTex);
        rect3d.x0 = x0;
        rect3d.x1 = x1;
        if (sPanelRunCount > 0) {
            draw_panel_runs(set, u1, vTop, vBottom, linear);
            rect3d.y0 = set->info.height >= NITRO_LOWERMENU_CANVAS_H ? y0 : yA;
            rect3d.y1 = set->info.height >= NITRO_LOWERMENU_CANVAS_H ? y0 + DS_H * k : yB;
        } else if (set->info.height >= NITRO_LOWERMENU_CANVAS_H) {
            /* (tall: canvas row y is the picture's row y) */
            quad(x0, y0, x1, yEnd, 0.0f, 0, vTop, u1, vTop - row * (float)NITRO_LOWERMENU_CANVAS_H, 0xFFFFFFFFu);
            rect3d.y0 = y0;
            rect3d.y1 = y0 + DS_H * k;
        } else {
            quad(x0, y0, x1, yA, 0.0f, 0, vRow0, u1, vRow0, 0xFFFFFFFFu);
            quad(x0, yA, x1, yB, 0.0f, 0, vTop, u1, vBottom, 0xFFFFFFFFu);
            quad(x0, yB, x1, yEnd, 0.0f, 0, vRowLast, u1, vRowLast, 0xFFFFFFFFu);
            rect3d.y0 = yA;
            rect3d.y1 = yB;
        }
        /* (put together: no 3D, its rows aren't where the runs put the
         * lower screen's) */
        if (scene3d != NULL && sPanelRunCount == 0) {
            draw_menu_3d(scr, set, scene3d, fb, &rect3d, u1, vTop, vBottom, linear);
        }
    }
    if (sMenuFade > 0) {
        quad_combiner(0);
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA, GPU_ONE, GPU_ZERO);
        quad(x0, y0, x1, yEnd, 0.0f, 0, 0, 0, 0, (u32)(sMenuFade > 16 ? 16 : sMenuFade) * 255u / 16u);
    }
}

/* One screen (one eye of it), into its render target. */
static void draw_screen(Screen *scr, int eye, const LayerSet *set, const NitroGx3dScene *scene, float stereo,
                        int edges, int underCleared)
{
    C3D_RenderTarget *target = scr->target[eye];
    int eyeDir;
    /* (the smooth 3D: the 2x2 framebuffer; the same picture, the viewport
     * twice the size) */
    int smooth = scr == &sScreens[0] && sSmoothNow;
    C3D_FrameBuf *fb = smooth ? (C3D_FrameBuf *)sSmoothFb : &target->frameBuf;
    C3D_Mtx pixels;
    const ComposeLayers *info = set ? &set->info : NULL;
    int canvasW = info && info->width > 0 ? info->width : DS_W;
    PicaRect pic, rect3d;
    float u1, vTop, vBottom;
    int linear = sConfig.filter != COMPOSE_FILTER_NEAREST;
    int has3d = info != NULL && !info->displayOff && info->has3d && scene != NULL;

    sProfGroup = scr == &sScreens[0] ? eye : 2;

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
    /* The lower screen as the menu (its 3D in the panel: see the frame's
     * start). */
    if (sMenuNow && scr == &sScreens[1]) {
        draw_menu(scr, set, u1, vTop, vBottom, linear, sMenu3dNow && has3d ? scene : NULL, fb);
        master_brightness(scr, info);
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
        NitroPort_PicaProfileMark("the 2D");
        return;
    }
    /* Beside a picture narrower than the screen (the DS's width on the
     * upper screen without the wide setting): its backdrop colour. */
    if (pic.x0 > 0.5f) {
        quad_combiner(0);
        quad(0, 0, (float)scr->width, SCREEN_H, 0.0f, 0, 0, 0, 0, rgba_of((uint16_t)info->backdrop, 255));
    }
    /* 1. The picture behind the 3D (all one colour: the clear did it). */
    if (!underCleared) {
        make_texture(&sUnderTex, set->under, GPU_RGB565, scr->texW, scr->texH, linear, GPU_CLAMP_TO_EDGE);
        C3D_TexBind(0, &sUnderTex);
        quad_combiner(1);
        quad(pic.x0, pic.y0, pic.x1, pic.y1, 0.0f, 0, vTop, u1, vBottom, 0xFFFFFFFFu);
    }
    if (has3d) {
        uint16_t rearColor;
        unsigned rearAlpha;
        PicaView view;
        PicaRect base;
        LineRun runs[LINE_RUNS_MAX];
        int nruns = 1, r;

        /* Where the 3D goes: the DS's screen in the picture, or (a scene
         * made for the wider view) the whole width; moved left by BG0HOFS
         * (the game's screen shakes). */
        if (scene->screen_width > DS_W) {
            base.x0 = ((float)scr->width - (float)scene->screen_width * SCALE) * 0.5f;
            base.x1 = base.x0 + (float)scene->screen_width * SCALE;
        } else {
            base.x0 = pic.x0 + (canvasW - DS_W) * 0.5f * SCALE;
            base.x1 = base.x0 + DS_W * SCALE;
        }
        base.y0 = 0.0f;
        base.y1 = SCREEN_H;
        /* (an H-blank effect: its lines scrolled apart, a run at a time;
         * not with the outlines, whose passes have scissors of their own) */
        runs[0].y0 = 0;
        runs[0].y1 = DS_H;
        runs[0].scroll = (short)scroll_signed(info->scroll3d & 0x1FF);
        if (info->scroll3dLines && !edges) {
            nruns = line_runs(info, runs, sFrameRunsMost > 0 ? sFrameRunsMost : 1);
        }
        if (nruns > sFrameLineRuns) {
            sFrameLineRuns = nruns;
        }
        rect3d = base;
        NitroPort_PicaRearPlane(&rearColor, &rearAlpha);
        view.fb = fb;
        for (eyeDir = 0; eyeDir < 4; eyeDir++) {
            view.edgeFb[eyeDir] = edges ? (C3D_FrameBuf *)scr->edgeFb[eye][eyeDir] : NULL;
        }
        view.screenWidth = scr->width;
        view.screenHeight = SCREEN_H;
        view.stereo = stereo;
        view.convergence = sConfig.convergence > 0.0f ? sConfig.convergence : sSceneConvergence;
        view.part = smooth && sSmoothSplit ? PICA_DRAW_OPAQUE : PICA_DRAW_ALL;
        view.again = 0;
        view.scissor = 0;
        view.band = 0;
        NitroPort_PicaProfileMark("the 2D behind");
        for (r = 0; r < nruns; r++) {
            PicaRect rows;   /* (the run's place and rows: its rear plane's, its tint's) */

            rect3d.x0 = base.x0 - (float)runs[r].scroll * SCALE;
            rect3d.x1 = base.x1 - (float)runs[r].scroll * SCALE;
            rows = rect3d;
            if (nruns > 1) {
                /* the run's rows (screen pixel (x, y) is the framebuffer's
                 * (height - 1 - y, width - 1 - x), in its own pixels: the
                 * smooth 3D's twice the screen's), for everything of the
                 * run: its rear plane, its polygons (each pass sets it
                 * again: see begin_pass), its tint */
                u32 sy = (u32)fb->width / SCREEN_H, sx = (u32)fb->height / (u32)scr->width;
                u32 y0 = (u32)((float)runs[r].y0 * SCALE + 0.5f), y1 = (u32)((float)runs[r].y1 * SCALE + 0.5f);

                if (y1 <= y0) {
                    continue;
                }
                view.scissor = 1;
                view.band = 1;
                view.bandTop = (int)y0;
                view.bandBottom = (int)y1;
                rows.y0 = (float)y0;
                rows.y1 = (float)y1;
                view.scissorRect[0] = (SCREEN_H - y1) * sy;
                view.scissorRect[1] = 0;
                view.scissorRect[2] = (SCREEN_H - y0) * sy;
                view.scissorRect[3] = (u32)scr->width * sx;
                C3D_SetScissor(GPU_SCISSOR_NORMAL, view.scissorRect[0], view.scissorRect[1], view.scissorRect[2],
                               view.scissorRect[3]);
            }
            /* 2. The rear plane: the clear colour, blended by its alpha. */
            if (rearAlpha) {
                quad_combiner(0);
                C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA,
                               GPU_ONE, GPU_ZERO);
                quad(rows.x0, rows.y0, rows.x1, rows.y1, -1.0f, 0, 0, 0, 0, rgba_of(rearColor, rearAlpha * 255u / 31u));
            }
            /* 3. The polygons (their commands counted, the first run's). */
            view.rect = rect3d;
            {
                /* (by the room left: a cut in the list, GPUCMD_Split, keeps it) */
                extern u32 gpuCmdBufSize, gpuCmdBufOffset;
                u32 before = gpuCmdBufSize - gpuCmdBufOffset;

                NitroPort_PicaDrawScene(&view);
                if (!view.again && view.part == PICA_DRAW_ALL && before > gpuCmdBufSize - gpuCmdBufOffset) {
                    sSceneWords = before - (gpuCmdBufSize - gpuCmdBufOffset);
                }
            }
            view.again = 1;
            if (nruns > 1) {
                /* (the run's tint and the next run's quad: the compositor's
                 * own view again, the run's scissor too: the viewport and
                 * the scene took it off) */
                C3D_SetFrameBuf(fb);
                C3D_SetViewport(0, 0, fb->width, fb->height);
                NitroPort_PicaSetView(&pixels, 0.0f, 1.0f);
                C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
                C3D_StencilTest(false, GPU_ALWAYS, 0, 0xFF, 0xFF);
                C3D_AlphaTest(false, GPU_ALWAYS, 0);
                C3D_SetScissor(GPU_SCISSOR_NORMAL, view.scissorRect[0], view.scissorRect[1], view.scissorRect[2],
                               view.scissorRect[3]);
                tint_3d(info, &rows);
            }
        }
        if (nruns > 1) {
            C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
            view.scissor = 0;
            view.band = 0;
        }
        if (smooth && sSmoothSplit) {
            /* The rest at the screen's resolution (see sSmoothFb). */
            smooth_down(scr, eye, edges);
            fb = &target->frameBuf;
            view.fb = fb;
            view.part = PICA_DRAW_AFTER;
            NitroPort_PicaDrawScene(&view);
        }
        C3D_SetFrameBuf(fb);
        C3D_SetViewport(0, 0, fb->width, fb->height);
        NitroPort_PicaSetView(&pixels, 0.0f, 1.0f);
        C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
        /* 4. A brightness effect on the 3D (with an H-blank effect's runs:
         * each run's, done above). */
        if (nruns == 1) {
            tint_3d(info, &rect3d);
        }
        /* 5. The layers in front, with their alpha (multiplied in). */
        if (!set->overEmpty && set->overTop < set->overBottom) {
            /* (only the rows with anything in them; all of them flipped) */
            int top = sConfig.flipV ? 0 : set->overTop, bottom = sConfig.flipV ? DS_H : set->overBottom;
            float y0 = pic.y0 + (float)top * SCALE, y1 = pic.y0 + (float)bottom * SCALE;
            float v0 = vTop + (vBottom - vTop) * (float)top / (float)DS_H;
            float v1 = vTop + (vBottom - vTop) * (float)bottom / (float)DS_H;

            make_texture(&sOverTex, set->over, set->pub.over16 ? GPU_RGBA5551 : GPU_RGBA8, scr->texW, scr->texH, linear,
                         GPU_CLAMP_TO_EDGE);
            C3D_TexBind(0, &sOverTex);
            quad_combiner(1);
            C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ONE_MINUS_SRC_ALPHA,
                           GPU_ONE, GPU_ONE_MINUS_SRC_ALPHA);
            quad(pic.x0, y0, pic.x1, y1, 0.0f, 0, v0, u1, v1, 0xFFFFFFFFu);
        }
    }
    /* 6. The master brightness. */
    master_brightness(scr, info);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
    NitroPort_PicaProfileMark(has3d ? "the 2D in front" : "the 2D");
}

/* The convergence distance a scene asks for: the geometric middle of the
 * depths most of its perspective vertices (w > 1) are at, the 5th and the
 * 95th percentile of their w (a histogram of 1/16 octaves, from the float's
 * bits), so the scene's middle is at the screen; 0 for a scene with none.
 * It was the nearest and the farthest vertex's: a few polygons near the
 * camera or far away coming into view at an edge moved every polygon's
 * stereo shift (each eye's picture jumped sideways: the geometry flickered
 * with the slider up as things came on screen, 2026-10-21). */
#define CONV_BINS 256          /* 16 octaves of w from 1, 16 bins each */
static float scene_convergence(const NitroGx3dScene *scene)
{
    static uint32_t hist[CONV_BINS];
    uint32_t i, total = 0, seen, lo = 0, hi = 0;
    int b;

    memset(hist, 0, sizeof(hist));
    for (i = 0; i < scene->count; i++) {
        int k;

        for (k = 0; k < 3; k++) {
            union {
                float f;
                uint32_t u;
            } w;
            int32_t bin;

            w.f = (float)scene->triangles[i].v[k].clip[3] / 4096.0f;
            if (!(w.f > 1.0001f)) {
                continue;
            }
            bin = (int32_t)(w.u >> 19) - (127 << 4);   /* (exponent, then the mantissa's top 4 bits) */
            hist[bin < 0 ? 0 : bin >= CONV_BINS ? CONV_BINS - 1 : bin]++;
            total++;
        }
    }
    if (total == 0) {
        return 0.0f;
    }
    for (b = 0, seen = 0; b < CONV_BINS; b++) {
        if (seen <= total / 20u && seen + hist[b] > total / 20u) {
            lo = (uint32_t)b;
        }
        if (seen <= total - total / 20u - 1u && seen + hist[b] > total - total / 20u - 1u) {
            hi = (uint32_t)b;
        }
        seen += hist[b];
    }
    /* (each bin's middle: 2 to the (bin + 0.5) / 16) */
    return exp2f(((float)(lo + hi) * 0.5f + 0.5f) / 16.0f);
}

/* The convergence distance the eyes are drawn with: the scenes', followed
 * a tenth of the way (in octaves) each new scene (not the in-between
 * scenes: theirs is their scene's), so it moves smoothly with the camera
 * and the scene; 1 (no shift) for a scene without perspective vertices,
 * the followed distance kept through a few of them; taken at once the
 * first time and after half a second (about) of them. */
#define CONV_FOLLOW 0.1f
#define CONV_FORGET 15u
static float sConvFollowed;          /* (0: none yet) */
static uint32_t sConvFlatScenes;
static void follow_convergence(const NitroGx3dScene *scene)
{
    float target = scene_convergence(scene);

    if (target <= 0.0f) {
        if (++sConvFlatScenes >= CONV_FORGET) {
            sConvFollowed = 0.0f;
        }
        sSceneConvergence = 1.0f;
        return;
    }
    sConvFlatScenes = 0;
    if (sConvFollowed <= 0.0f) {
        sConvFollowed = target;
    } else {
        sConvFollowed *= powf(target / sConvFollowed, CONV_FOLLOW);
    }
    sSceneConvergence = sConvFollowed;
}

/* The newest set's publish order (0: none). */
static uint32_t newest_seq(const Screen *scr)
{
    uint32_t newest = 0;
    int i;

    for (i = 0; i < SETS; i++) {
        if (!scr->set[i].writing && scr->set[i].seq > newest) {
            newest = scr->set[i].seq;
        }
    }
    return newest;
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
        best->held++;   /* (till the frame drawn with it is: flight_finish) */
    }
    return best;
}

/* A free LCD framebuffer of a screen: not on the LCD, not waiting. Under
 * sPresentLock. -1: none (on the LCD, scanned out, or waiting). */
static int free_lcd_buffer(const Screen *scr)
{
    int b, i;

    for (b = 0; b < scr->nlcd; b++) {
        int taken = b == scr->onLcd || b == scr->lastLcd;

        for (i = 0; i < scr->npending; i++) {
            taken |= scr->pending[i].buf == b;
        }
        if (!taken) {
            return b;
        }
    }
    return -1;
}

/* The smooth 3D's framebuffer cleared (a memory fill, queued): the depth,
 * and the colour when the picture behind the 3D is all one colour. */
static void smooth_clear(const LayerSet *set, int underCleared)
{
    C3D_FrameBufClear((C3D_FrameBuf *)sSmoothFb, underCleared ? C3D_CLEAR_ALL : C3D_CLEAR_DEPTH,
                      underCleared ? top_clear_color(set->underColor) : 0, 0xFFFFFFFFu);
}

/* The smooth framebuffer 2x2 or 2x1 for this frame (its size: what the
 * clears, the drawing's viewport and the transfers take from it). */
static void smooth_size(int half)
{
    if (half != sSmoothMadeHalf &&
        NitroPort_PicaFrameBufMake(sSmoothFb, sizeof(sSmoothFb), sSmoothColor, sSmoothDepth, SMOOTH_W,
                                   half ? TOP_W : SMOOTH_H, TOP_FB_FORMAT == GPU_RB_RGB565)) {
        sSmoothMadeHalf = half;
    }
}

/* (each 2x1 averaged: the lines halved, as many of them) */
#define SMOOTH_HALF_FLAGS (GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | \
     GX_TRANSFER_RAW_COPY(0) | GX_TRANSFER_IN_FORMAT(TOP_TRANSFER_FMT) | \
     GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB565) | GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_X))

/* An eye drawn by the smooth 3D into its LCD framebuffer, scaled down
 * (queued after the drawing). */
static void smooth_out(Screen *scr, int buf, int eye)
{
    u32 dim = GX_BUFFER_DIM(SMOOTH_W, sSmoothMadeHalf ? TOP_W : SMOOTH_H);

    GX_DisplayTransfer((u32 *)sSmoothColor, dim, (u32 *)scr->lcd[buf][eye], dim,
                       sSmoothMadeHalf ? SMOOTH_HALF_FLAGS : SMOOTH_TRANSFER_FLAGS);
}

/* The upper screen's format to itself: tiled, each 2x2 (2x1) averaged,
 * into linear; linear into tiled. */
#define SMOOTH_DOWN_FLAGS (GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) | \
     GX_TRANSFER_IN_FORMAT(TOP_TRANSFER_FMT) | GX_TRANSFER_OUT_FORMAT(TOP_TRANSFER_FMT) | \
     GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_XY))
#define SMOOTH_DOWN_HALF_FLAGS (GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) | \
     GX_TRANSFER_IN_FORMAT(TOP_TRANSFER_FMT) | GX_TRANSFER_OUT_FORMAT(TOP_TRANSFER_FMT) | \
     GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_X))
#define SMOOTH_TILE_FLAGS (GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(1) | GX_TRANSFER_RAW_COPY(0) | \
     GX_TRANSFER_IN_FORMAT(TOP_TRANSFER_FMT) | GX_TRANSFER_OUT_FORMAT(TOP_TRANSFER_FMT) | \
     GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

static void fill_edge_buffers(Screen *scr, u32 value, unsigned copied);

/* The smooth 3D with outlines (sSmoothSplit): what's drawn 2x2 so far
 * (queued after it: the command list cut) scaled down into the upper
 * screen's own framebuffer, through the smooth depth's memory (done with:
 * a linear 240 x 400 copy fits in it); then the edge buffers filled (that
 * memory is theirs). */
static void smooth_down(Screen *scr, int eye, int edges)
{
    C3D_FrameBuf *fb = (C3D_FrameBuf *)sSmoothFb;
    u32 *temp = (u32 *)fb->depthBuf;
    u32 dim = GX_BUFFER_DIM(SCREEN_H, TOP_W);
    u32 in = GX_BUFFER_DIM(SMOOTH_W, sSmoothMadeHalf ? TOP_W : SMOOTH_H);

    prof_split("the opaque polygons", 1);
    GX_DisplayTransfer((u32 *)sSmoothColor, in, temp, in, sSmoothMadeHalf ? SMOOTH_DOWN_HALF_FLAGS : SMOOTH_DOWN_FLAGS);
    GX_DisplayTransfer(temp, dim, (u32 *)scr->target[eye]->frameBuf.colorBuf, dim, SMOOTH_TILE_FLAGS);
    if (edges) {
        fill_edge_buffers(scr, sEdgeClearNow, sEdgeCopiesNow);
    }
}

/* The edge marking buffers filled for a scene (memory fills, queued
 * ahead of the drawing), but those the transfer engine makes (copied: a
 * direction a bit): two a fill (a frame's GPU queue holds GX_QUEUE_ENTRIES). */
static void fill_edge_buffers(Screen *scr, u32 value, unsigned copied)
{
    u32 bytes = (u32)SCREEN_H * (u32)scr->width * 4u;
    u8 *buf[4];
    int n = 0, d;

    for (d = 0; d < 4; d++) {
        if (!(copied & (1u << d))) {
            buf[n++] = sEdgeDepth[d];
        }
    }
    for (d = 0; d < n; d += 2) {
        if (d + 1 < n) {
            GX_MemoryFill((u32 *)buf[d], value, (u32 *)(buf[d] + bytes), BIT(0) | (2u << 8), (u32 *)buf[d + 1], value,
                          (u32 *)(buf[d + 1] + bytes), BIT(0) | (2u << 8));
        } else {
            GX_MemoryFill((u32 *)buf[d], value, (u32 *)(buf[d] + bytes), BIT(0) | (2u << 8), NULL, 0, NULL, 0);
        }
    }
}

int NitroPort_ComposeFrame(const NitroGx3dScene *scene, uint32_t sceneSerial, int aOnTop, float slider,
                           int forced, int flush, int mid)
{
    int s, drawn = 0, stereoNow, need[2] = { 0, 0 }, timed = 0, edges = 0, lcdBuf[2] = { -1, -1 }, top3d = 0;
    int edgesWanted = 0, smoothable = 0, drawnAs = DRAWN_PLAIN, smooth3d;
    int underCleared[2] = { 0, 0 };
    int menu[2] = { 0, 0 };            /* (the lower screen as the menu) */
    int has3dOf[2] = { 0, 0 };         /* (each screen's 3D, as decided: in the menu's panel too) */
    int keptBlack = 0;                 /* (the upper screen kept, but drawn again: black) */
    uint32_t menuSerial = NitroPort_LowerMenuSerial(), mapSerial = NitroPort_LowerMenuMapSerial();
    unsigned copies;
    u32 vblanks, edgeClear = 0, startVBlank, texBefore, texAfter;
    LayerSet *sets[2] = { NULL, NULL };
    LayerSet *panelUp = NULL;          /* (the upper screen's picture, in the menu's panel put together) */
    uint32_t runSerial = 0;
    int panelUpper = 0;
    int keptShown;
    u32 current2d[2];
    u64 start = svcGetSystemTick(), gpuStart, prepareTicks = 0, texTicksBefore, texTicksAfter, texels;

    if (!sReady) {
        return 0;
    }
    flight_finish(0);   /* (the frame before, if it's drawn: its 2D let go) */
    {
        int look = __atomic_exchange_n(&sLookRequest, -1, __ATOMIC_ACQ_REL);

        if (look >= 0) {
            sConfig.shadows = look;
            sConfig.grade = look;
            NitroPort_PicaLook(sConfig.shadows, sConfig.grade, sConfig.sunFixed, sConfig.sunElevation,
                               sConfig.sunAzimuth);
        }
    }
    keptShown = NitroPort_LowerMenuKeptShown();
    sMenuPanel = NitroPort_LowerMenuPanel();   /* (as decided: the game may change it while this frame's built) */
    sPanelRunCount = NitroPort_LowerMenuPanelRuns(sPanelRuns, &runSerial);
    for (s = 0; s < sPanelRunCount; s++) {
        panelUpper |= sPanelRuns[s].from == NITRO_LOWERMENU_FROM_UPPER;
    }
    sMenuFade = NitroPort_LowerMenuPanelFade();
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
    stereoNow = stereo_for(slider, aOnTop);
    /* (before the sets are picked: the newest then is at least as new) */
    current2d[0] = __atomic_load_n(&s2DCurrent[0], __ATOMIC_ACQUIRE);
    current2d[1] = __atomic_load_n(&s2DCurrent[1], __ATOMIC_ACQUIRE);
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
        /* The upper screen kept as it is (an app in the lower menu's
         * panel: the field stays above it), till the game's picture is as
         * bright as what's kept (nitro_port_lowermenu.h; kept mid-fade: as
         * the end of a fade nearest it, which every fade reaches). Kept:
         * not drawn, what's on its LCD stays. Drawn again regardless (back
         * from the HOME menu, sleep or the log): what was kept may be
         * gone, and the game's picture there is the app's hidden screen
         * (the bag's touch screen): black instead. */
        if (s == 0) {
            int wish = NitroPort_LowerMenuUpperHeld();

            if (!sUpperKept && wish > 0) {
                sUpperKept = 1;
                sUpperKeptBright = sUpperShownBright <= -8 ? -16 : sUpperShownBright >= 8 ? 16 : 0;
                /* (at full brightness: the field, a panel app opening from
                 * it; its picture kept for NITRO_LOWERMENU_HOLD_FIELD) */
                sUpperShowsField = 0;
                if (sUpperKeptBright == 0 && wish == NITRO_LOWERMENU_HOLD_FIELD && field_picture_kept()) {
                    /* (the field's picture kept already, asked for again:
                     * what's on the LCD now isn't the field. The bag after
                     * an evolution, which ends in a palette fade: the
                     * master brightness full, the LCD the evolution's
                     * screen gone dark, and that was kept as the field,
                     * 2026-10-26.) It's put back below. */
                } else if (sUpperKeptBright == 0) {
                    keep_field_picture(scr);
                    sUpperShowsField = sFieldValid;
                }
            } else if (sUpperKept && wish < 0) {
                /* (let go now: once the game's picture there is one made
                 * since (the newest set: the game's as of its drawer's last
                 * frame, changed or not), not the app's hidden screen from
                 * before) */
                u32 shownStamp = set->info.stamp;

                if (set->seq == newest_seq(scr) && (int32_t)(current2d[s] - shownStamp) > 0) {
                    shownStamp = current2d[s];
                }
                if (NitroPort_LowerMenuUpperLetGoShows(shownStamp)) {
                    sUpperKept = 0;
                }
            } else if (sUpperKept && wish == 0 && !set->info.displayOff &&
                       NitroPort_LowerMenuBrightness(set->info.masterBright) == sUpperKeptBright) {
                sUpperKept = 0;
            }
            if (!sUpperKept) {
                sUpperShowsField = 0;
                sFieldAgain = 0;
            }
            /* Kept showing the field's picture (an app in the panel after
             * one that took both screens: what's kept is that one's black):
             * the field's put back on the LCD, once nothing of the upper
             * screen's is waiting for its V-blank; again after the HOME
             * menu. As bright as the field from then on. */
            if (sUpperKept && forced && sUpperShowsField) {
                sFieldAgain = 1;
            }
            if (sUpperKept && wish == NITRO_LOWERMENU_HOLD_FIELD && field_picture_kept() &&
                (!sUpperShowsField || sFieldAgain) && scr->npending == 0) {
                present_field_picture(scr);
                sUpperShowsField = 1;
                sFieldAgain = 0;
                sUpperKeptBright = 0;
            }
            if (sUpperKept && (!forced || (sUpperShowsField && field_picture_kept()))) {
                stereoNow = 0;   /* (nothing drawn there: no 3D, no stereo) */
                set->held--;
                continue;
            }
            keptBlack = sUpperKept;
        }
        has3d = !(s == 0 && keptBlack) && !(s == 1 && keptShown && sMenuFade >= 16) && screen3d && set->info.has3d &&
                !set->info.displayOff && scene != NULL;
        /* The lower screen as the menu: in the field (the game says), with
         * its textures; or kept shown while the game swaps what's under it
         * (its panel black or the map: whatever the DS's lower screen is
         * then). The 3D on that screen goes in the panel (an app shown
         * there: the summary's Pokemon), not with the map. */
        menu[s] = s == 1 && sMenuTexData[0] != NULL &&
                  ((!set->info.displayOff && NitroPort_LowerMenuOn()) || keptShown);
        if (menu[s] && (sMenuPanel == NITRO_LOWERMENU_PANEL_MAP || sMenuFade >= 16)) {
            has3d = 0;
        }
        has3dOf[s] = has3d;
        /* (the panel showing the DS's lower screen: when its picture was
         * made, for the port's fade in (NitroPort_LowerMenuPanelShown); the
         * newest set is the game's as of its drawer's last frame, changed
         * or not: NitroPort_Compose2DCurrent) */
        if (menu[s] && sMenuPanel != NITRO_LOWERMENU_PANEL_MAP && panelUpper) {
            /* (put together with the upper screen's rows: its picture too,
             * held for the frame) */
            panelUp = pick_set(&sScreens[0], NULL, 0);
        }
        if (menu[s] && sMenuPanel != NITRO_LOWERMENU_PANEL_MAP) {
            u32 shownStamp = set->info.stamp;

            if (set->seq == newest_seq(scr) && (int32_t)(current2d[s] - shownStamp) > 0) {
                shownStamp = current2d[s];
            }
            if (panelUp != NULL) {
                /* (the panel's as fresh as the older of its two pictures) */
                u32 upStamp = panelUp->info.stamp;

                if (panelUp->seq == newest_seq(&sScreens[0]) && (int32_t)(current2d[0] - upStamp) > 0) {
                    upStamp = current2d[0];
                }
                shownStamp = (int32_t)(upStamp - shownStamp) < 0 ? upStamp : shownStamp;
            }
            NitroPort_LowerMenuPanelShown(shownStamp);
        }
        need[s] = forced || set->seq != scr->shownSeq || (has3d && sceneSerial != scr->shownScene) || (mid && has3d) ||
                  menu[s] != scr->shownMenu || (menu[s] && menuSerial != scr->shownMenuSerial) ||
                  (menu[s] && sMenuFade != scr->shownFade) ||
                  (menu[s] && sMenuPanel == NITRO_LOWERMENU_PANEL_MAP && mapSerial != scr->shownMapSerial) ||
                  (menu[s] && sMenuPanel != NITRO_LOWERMENU_PANEL_MAP &&
                   (runSerial != scr->shownRunSerial || (panelUp != NULL ? panelUp->seq : 0u) != scr->shownUpSeq)) ||
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
        /* An in-between scene whose V-blank has come (its scene's turn
         * next): not drawn, and the lag is short (mid_late). */
        if (need[s] && has3d && mid && (int32_t)(scene->stamp + (u32)sLag - (startVBlank + 1u)) < 0) {
            need[s] = 0;
            drawn |= COMPOSE_MID_LATE;
            mid_late();
        }
        /* Not without a framebuffer to draw into (the frames waiting for
         * their V-blanks have them all): the next V-blank frees one. */
        if (need[s]) {
            lcdBuf[s] = free_lcd_buffer(scr);
            if (lcdBuf[s] < 0) {
                need[s] = 0;
                drawn |= s == 0 ? COMPOSE_NO_BUFFER_UPPER : COMPOSE_NO_BUFFER_LOWER;
            }
        }
        if (!need[s]) {
            set->held--;
            if (s == 1 && panelUp != NULL) {
                panelUp->held--;
                panelUp = NULL;
            }
            continue;
        }
        sets[s] = set;
        if (s == 0 && !keptBlack) {
            sUpperShownBright = set->info.displayOff ? -16 : NitroPort_LowerMenuBrightness(set->info.masterBright);
        }
        if (has3d && sceneSerial != scr->shownScene) {
            timed = 1;   /* a new 3D frame: shown at its time */
        }
        if (s == 0 && !has3d) {
            stereoNow = 0;
        }
        if (s == 0) {
            top3d = has3d;
        }
    }
    LightLock_Unlock(&sPresentLock);
    LightLock_Unlock(&sSetLock);
    if (!need[0] && !need[1]) {
        return drawn;   /* (nothing drawn: why, if a frame was wanted) */
    }
    NitroPort_PicaTextureStats(&texBefore, &texels, &texTicksBefore);
    /* (an in-between scene has its scene's serial: told apart) */
    if (scene != NULL && (!sScenePrepared || sceneSerial * 2u + (mid ? 1u : 0u) != sPreparedScene)) {
        prepareTicks = svcGetSystemTick();
        NitroPort_PicaPrepareScene(scene, NULL);
        prepareTicks = svcGetSystemTick() - prepareTicks;
        sPreparedScene = sceneSerial * 2u + (mid ? 1u : 0u);
        sScenePrepared = 1;
        if (sConfig.stereo && sConfig.convergence <= 0.0f && !mid) {
            follow_convergence(scene);
        }
    }
    /* (built while the GPU draws the frame before: frames in flight) */
    NitroPort_PicaFrameOpen();
    sQuadHalf ^= 1;
    sQuads = sQuadsAll + (sQuadHalf ? MAX_QUAD_VERTICES : 0);
    NitroPort_PicaStage("the frame's clears");
    sProfOn = 0;
    if (scene != NULL && ++sProfFrames >= PROF_EVERY) {
        sProfFrames = 0;
        sProfOn = 1;
        sProfMarks = 0;
        sProfLost = 0;
    }
    sQuadVertices = 0;
    edges = sEdgeDepth[0] != NULL && scene != NULL && NitroPort_PicaEdgeClearValue(&edgeClear);
    /* With the 3D slider up (each eye drawn: the outlines twice), only
     * with `edges3d on`. */
    /* (wanted with the slider up: for auto_outlines) */
    edgesWanted = edges && stereoNow && sConfig.edges3d;
    if (stereoNow && (!sConfig.edges3d || !sAutoEdges)) {
        edges = 0;
    }
    /* The smooth 3D (sSmoothFb): on the upper screen when the 3D is
     * there; with outlines, split (sSmoothSplit), each eye in turn. */
    runs_most(stereoNow);
    smooth3d = __atomic_load_n(&sSmooth3d, __ATOMIC_ACQUIRE);
    smoothable = sSmoothReady && need[0] && top3d && smooth3d != 0 && (stereoNow || smooth3d == 2);
    /* (2x2, or as 60 frames a second has it now: auto_fps60) */
    drawnAs = smoothable ? fps60_drawn(stereoNow) : DRAWN_PLAIN;
    sSmoothNow = smoothable && drawnAs >= DRAWN_HALF;
    sSmoothHalf = sSmoothNow && drawnAs == DRAWN_HALF;
    if (sSmoothNow) {
        smooth_size(sSmoothHalf);
        if (sSmoothMadeHalf != sSmoothHalf) {
            sSmoothHalf = sSmoothMadeHalf;   /* (couldn't be changed: as it was) */
            drawnAs = sSmoothHalf ? DRAWN_HALF : DRAWN_FULL;
        }
    } else {
        drawnAs = DRAWN_PLAIN;
    }
    sSmoothSplit = sSmoothNow && edges;
    /* The left and right edge buffers copied from the screen's own
     * (NitroPort_PicaEdgeCopies): its stencil then the rear plane's ID where
     * nothing is drawn, as theirs. */
    copies = NitroPort_PicaEdgeCopies(edges);
    sEdgeClearNow = edgeClear;
    sEdgeCopiesNow = copies;
    /* The depth/stencil buffers cleared (before any drawing: the fills go
     * ahead of the frame's commands), the edge marking buffers with them. */
    for (s = 0; s < 2; s++) {
        int eye, screen3d = (s == 0) == (aOnTop != 0);

        if (!need[s]) {
            continue;
        }
        underCleared[s] = !menu[s] && under_fills_screen(&sScreens[s], sets[s]);
        if (underCleared[s]) {
            sUnderFills++;
        }
        if (s == 0 && sSmoothNow) {
            /* (the left eye's; the right eye's after it's drawn) */
            smooth_clear(sets[s], underCleared[s]);
            if (sSmoothSplit) {
                /* (the screen's own framebuffers (an eye each): their
                 * depth; their colour comes scaled down, the edge buffers
                 * are filled after that) */
                u32 bytes = (u32)SCREEN_H * (u32)TOP_W * 4u, value = copies ? edgeClear : 0xFFFFFFFFu;
                u8 *d0 = sScreens[0].target[0]->frameBuf.depthBuf;
                u8 *d1 = stereoNow ? (u8 *)sScreens[0].target[1]->frameBuf.depthBuf : NULL;

                GX_MemoryFill((u32 *)d0, value, (u32 *)(d0 + bytes), BIT(0) | (2u << 8), (u32 *)d1, d1 ? value : 0,
                              d1 ? (u32 *)(d1 + bytes) : NULL, d1 ? BIT(0) | (2u << 8) : 0);
            }
            continue;
        }
        for (eye = 0; eye < (s == 0 && stereoNow ? 2 : 1); eye++) {
            /* (one fill: the colour buffer too when the picture behind is
             * all one colour) */
            C3D_RenderTargetClear(sScreens[s].target[eye], underCleared[s] ? C3D_CLEAR_ALL : C3D_CLEAR_DEPTH,
                                  underCleared[s] ? (s == 0 ? top_clear_color(sets[s]->underColor)
                                                            : rgba_of_565(sets[s]->underColor))
                                                  : 0,
                                  screen3d && copies ? edgeClear : 0xFFFFFFFFu);
        }
        if (screen3d && edges) {
            fill_edge_buffers(&sScreens[s], edgeClear, copies);
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

            draw_screen(scr, 0, sets[s], sc, -shift, edges, underCleared[s]);
            if (sSmoothNow) {
                /* The left eye scaled down into its LCD framebuffer (with
                 * outlines it's in its own framebuffer already), then the
                 * 2x2 framebuffer cleared for the right eye: the commands
                 * so far go ahead. */
                prof_split("the left eye's end", 1);
                if (!sSmoothSplit) {
                    smooth_out(scr, lcdBuf[s], 0);
                }
                smooth_clear(sets[s], underCleared[s]);
            } else if (sc != NULL && edges) {
                /* The second eye needs the edge buffers filled again: the
                 * commands so far go ahead, the fills after them. */
                prof_split("the left eye's end", 1);
                fill_edge_buffers(scr, edgeClear, copies);
            }
            draw_screen(scr, 1, sets[s], sc, shift, edges, underCleared[s]);
        } else if (s == 0 && keptBlack) {
            draw_screen(scr, 0, NULL, NULL, 0.0f, edges, underCleared[s]);   /* (black) */
        } else {
            sMenuNow = menu[s];
            sMenu3dNow = has3dOf[s];
            sPanelUp = s == 1 ? panelUp : NULL;
            if (!(s == 1 && menu[s] && sMenuPanel != NITRO_LOWERMENU_PANEL_MAP)) {
                sPanelRunCount = s == 1 ? 0 : sPanelRunCount;   /* (as usual) */
            }
            draw_screen(scr, 0, sets[s], sc, 0.0f, edges, underCleared[s]);
            sMenuNow = 0;
            sMenu3dNow = 0;
            sPanelUp = NULL;
        }
        /* (drawn black: the game's picture there still to be drawn) */
        scr->shownSeq = s == 0 && keptBlack ? 0u : sets[s]->seq;
        __atomic_store_n(&scr->shownMenu, menu[s], __ATOMIC_RELEASE);
        scr->shownMenuSerial = menuSerial;
        scr->shownMapSerial = s == 1 && menu[s] && sMenuPanel == NITRO_LOWERMENU_PANEL_MAP ? sMapTexSerial : mapSerial;
        scr->shownFade = sMenuFade;
        scr->shownRunSerial = runSerial;
        scr->shownUpSeq = s == 1 && panelUp != NULL ? panelUp->seq : 0u;
        if (screen3d && !mid) {   /* (an in-between scene: its scene's still to come) */
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
    NitroPort_PicaStage("the frame's end");
    {
        u32 mark = NitroPort_PicaFrameClose();

        /* The targets into the screens' LCD framebuffers, after the
         * drawing (queued behind it). */
        for (s = 0; s < 2; s++) {
            Screen *scr = &sScreens[s];
            int eye;

            if (!(drawn & (1 << s))) {
                continue;
            }
            for (eye = 0; eye < (s == 0 && stereoNow ? 2 : 1); eye++) {
                if (s == 0 && sSmoothNow && !sSmoothSplit) {
                    if (!stereoNow || eye == 1) {
                        smooth_out(scr, lcdBuf[s], eye);   /* (the left eye's went after it was drawn) */
                    }
                    continue;
                }
                GX_DisplayTransfer((u32 *)scr->target[eye]->frameBuf.colorBuf, GX_BUFFER_DIM(SCREEN_H, scr->width),
                                   (u32 *)scr->lcd[lcdBuf[s]][eye], GX_BUFFER_DIM(SCREEN_H, scr->width),
                                   s == 0 ? TOP_SCREEN_TRANSFER_FLAGS : SCREEN_TRANSFER_FLAGS);
            }
        }
        gpuStart = svcGetSystemTick();
        sCpuTicks += gpuStart - start;
        /* The frame before done (the GPU's had it while this one was
         * built), then this one to the GPU. */
        flight_finish(1);
        sGpuSubmitted++;
        sFlightQueue = sGxRecording;
        sFlightMark = mark;
        sFlightStereo = stereoNow;
        sFlight3D = top3d || (scene != NULL && !aOnTop);
        sFlightRuns = sFrameLineRuns;
        sFlightDrawn = drawnAs;
        sFlightSmoothable = smoothable;
        sFlightState = fps60_state(stereoNow, aOnTop);
        sFlightEdgesWanted = edgesWanted;
        sFlightEdges = edges;
        if (stereoNow && edges) {
            sStereoEdgeFrames++;
        }
        /* (its 2D: no longer held by it as a frame being built, busy till
         * the GPU's drawn it) */
        LightLock_Lock(&sSetLock);
        for (s = 0; s < 2; s++) {
            if (sets[s] != NULL) {
                sets[s]->held--;
                sets[s]->flightSerial = sGpuSubmitted;
            }
        }
        if (panelUp != NULL) {
            panelUp->held--;
            panelUp->flightSerial = sGpuSubmitted;
        }
        LightLock_Unlock(&sSetLock);
        if (sProfOn) {
            sProfFlight.stereo = stereoNow;
            sProfFlight.edges = edges;
            sProfFlight.smooth = sSmoothNow;
            sProfFlight.half = sSmoothHalf;
            sProfFlight.split = sSmoothSplit;
            sProfFlight.t0 = svcGetSystemTick();
            __atomic_store_n(&sProfEvents, 0u, __ATOMIC_RELEASE);
            __atomic_store_n(&sProfArmed, 1, __ATOMIC_RELEASE);
            sProfFlightOn = 1;
            sProfOn = 0;
        }
        sGpuRunTick = svcGetSystemTick();
        NitroPort_PicaFrameRunning(mark);
        if (sGxQueue[sFlightQueue].numEntries > 0) {
            sFlight = 1;
            gxCmdQueueRun(&sGxQueue[sFlightQueue]);
        } else {
            /* (nothing for the GPU: drawn as it is) */
            __atomic_add_fetch(&sGpuDone, 1u, __ATOMIC_ACQ_REL);
            sFlight = 1;
            sGpuDoneTick = sGpuRunTick;
        }
        sGxRecording ^= 1;
        GX_BindQueue(&sGxQueue[sGxRecording]);
    }
    NitroPort_PicaStage("between frames");
    sFrames++;
    if (stereoNow) {
        sStereoFrames++;
    }
    if (sSmoothNow) {
        if (sSmoothHalf) {
            sSmoothHalfFrames++;
        } else {
            sSmoothFrames++;
        }
    }
    NitroPort_PicaTextureStats(&texAfter, &texels, &texTicksAfter);
    {
        /* A frame that took the display thread more than two V-blanks:
         * the scenes made meanwhile may never show (a jump). */
        const double ms = 1000.0 / SYSCLOCK_ARM11;
        double total = (double)(svcGetSystemTick() - start) * ms;

        if (total > 40.0 && (sFrames - sHitchFrame >= 15u || sHitchFrame == 0u)) {
            NitroPort_Log("[hitch] a frame took the display thread %.1f ms: %.1f ms preparing the scene, %.1f ms "
                          "building (%lu textures converted: %.1f ms), %.1f ms GPU (the frame before)%s",
                          total, (double)prepareTicks * ms, (double)(gpuStart - start) * ms,
                          (unsigned long)(texAfter - texBefore),
                          (double)(texTicksAfter - texTicksBefore) * ms,
                          (double)sLastGpuTicks * ms, stereoNow ? "; stereo" : "");
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

        if (!(drawn & (1 << s))) {
            continue;
        }
        if (timed && screen3d && scene != NULL && mid) {
            /* An in-between scene: a V-blank before its scene's (not
             * before the GPU can have drawn it), the lag left to its
             * scene. */
            u32 gpuVBlanks = (u32)(sLastGpuTicks / (u64)(SYSCLOCK_ARM11 / 60));
            u32 wanted = scene->stamp + (u32)sLag;

            target = (int32_t)(wanted - (vblanks + 1u + gpuVBlanks)) >= 0 ? wanted : vblanks + 1u + gpuVBlanks;
            if ((int32_t)(target - (vblanks + 1u)) > 6) {
                target = vblanks + 1u + 6u;   /* (a stamp from before a pause) */
            }
            if (target != wanted) {
                /* Late: it would show at its scene's V-blank (or after),
                 * and be skipped for it: a step of 30 a second. More lag
                 * from now on, as for a late scene (the scene itself, a
                 * V-blank later, still makes it: its lateness wouldn't
                 * show the lag is short). */
                mid_late();
            }
            sMidFrames++;
            sMidBefore = 1;
        } else if (timed && screen3d && scene != NULL) {
            u32 wanted = scene->stamp + 1u + (u32)sLag;
            /* (it's drawn from now on: the V-blanks the GPU's last frame
             * took count) */
            u32 gpuVBlanks = (u32)(sLastGpuTicks / (u64)(SYSCLOCK_ARM11 / 60));
            int32_t slack = (int32_t)(wanted - (vblanks + 1u + gpuVBlanks));
            int midBefore = sMidBefore;

            sTimed++;
            sMidBefore = 0;
            if (slack < 0) {
                /* late: more lag from now on */
                sLate++;
                if (sLag < LAG_MAX) {
                    sLag++;
                }
                sOnTime = 0;
                if (sLoweredRecently && sTimed - sLoweredAt < 1800u && sLowerAfter < LAG_LOWER_AFTER * LAG_LOWER_MOST) {
                    sLowerAfter *= 2;   /* (it didn't hold: twice as long before the next try) */
                }
                sLoweredRecently = 0;
                /* Where the time went, for the log (one line every half
                 * second at most; the others counted in the next). */
                if (sFrames - sHitchFrame >= 15u || sHitchFrame == 0u) {
                    const double ms = 1000.0 / SYSCLOCK_ARM11;

                    NitroPort_Log("[hitch] a 3D frame %d V-blank%s late (lag now %d): made at V-blank %lu, "
                                  "reached the display thread %lu V-blanks later, drawn from %lu after; %.1f ms "
                                  "preparing, %.1f ms building (%lu textures converted: %.1f ms), %.1f ms GPU (the frame "
                                  "before)%s%s",
                                  (int)-slack, slack == -1 ? "" : "s", sLag, (unsigned long)scene->stamp,
                                  (unsigned long)(sSeenAt - scene->stamp),
                                  (unsigned long)(startVBlank - scene->stamp),
                                  (double)prepareTicks * ms, (double)(gpuStart - start) * ms,
                                  (unsigned long)(texAfter - texBefore),
                                  (double)(texTicksAfter - texTicksBefore) * ms,
                                  (double)sLastGpuTicks * ms,
                                  stereoNow ? "; stereo" : "",
                                  sHitchQuiet ? "; more since the last line" : "");
                    sHitchFrame = sFrames;
                    sHitchQuiet = 0;
                } else {
                    sHitchQuiet++;
                }
            } else {
                /* Lowered after a long while of frames with a V-blank to
                 * spare, in a row (one without resets the count); with an
                 * in-between scene before each (a V-blank earlier), two
                 * to spare, so that they're still on time with it lowered. */
                if (slack >= 1 + midBefore) {
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
        if (scr->npending >= PENDING_MAX) {
            /* (can't happen: a free buffer was found above) */
            memmove(scr->pending, scr->pending + 1, (size_t)(PENDING_MAX - 1) * sizeof(scr->pending[0]));
            scr->npending = PENDING_MAX - 1;
            sSkipped++;
        }
        p = &scr->pending[scr->npending++];
        p->buf = lcdBuf[s];
        p->target = target;
        p->stereo = s == 0 && stereoNow;
        p->serial = sGpuSubmitted;
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
    stats->underFills = sUnderFills;
    stats->smooth = sSmoothFrames;
    stats->smoothHalf = sSmoothHalfFrames;
    stats->waitTicks = sWaitTicks;
    stats->stereoEdges = sStereoEdgeFrames;
    stats->midFrames = sMidFrames;
    stats->midLate = sMidLate;
}
#endif
