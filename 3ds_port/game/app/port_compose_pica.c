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
#define CANVAS_MAX 400
#define CANVAS_MAX_H 240   /* (the field at the DS's pixel size: 400 x 240) */
/* (the drawers' pictures: the wider upper screen's, the field's at the DS's
 * pixel size, or the lower screen's as a tall panel) */
#define WORK_PIXELS (CANVAS_MAX * CANVAS_MAX_H > DS_W * NITRO_LOWERMENU_TALL_LINES ? CANVAS_MAX * CANVAS_MAX_H \
                                                                                   : DS_W * NITRO_LOWERMENU_TALL_LINES)
#define TOP_W 400
#define BOTTOM_W 320
#define SCREEN_H 240
/* The DS's picture on a 3DS screen: 1.25 times the size. */
#define SCALE 1.25f

/* (the field at the DS's pixel size: ComposeLayers.top) A picture's rows
 * with the layers in front of the 3D (the DS's 192, the 3D's; the field's
 * all of them), and its size on the screen: a pixel a pixel, or SCALE. */
#define OVER_ROWS(l) ((l)->top > 0 && (l)->height > DS_H ? (l)->height : DS_H)
#define PIC_SCALE(l) ((l) != NULL && (l)->top > 0 && (l)->height > DS_H ? (float)SCREEN_H / (float)(l)->height : SCALE)

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
    /* texels a DS pixel in its textures: the screen's texScale, or 1 for a
     * picture too wide for 2 (the Underground at the DS's pixel size on the
     * lower screen, 320 x 240, drawn a pixel a pixel) */
    int texScale;
    /* (the upper screen's) A BG the compositor draws itself (the field's
     * weather: ComposeLayers.gpuBg): its whole map, 2x2 texels a pixel
     * (512 x 512 RGBA5551, tiled), and its key (bgValid: written). */
    void *bgTex;
    uint64_t bgKey;
    int bgValid;
    /* (the upper screen's) A band of a BG drawn here as it scrolls (the
     * place name sign: ComposeLayers.gpuSign): its picture, a texel a pixel
     * (SIGN_TEX_W x SIGN_TEX_H RGBA5551, tiled), and its key (signValid:
     * written). */
    void *signTex;
    uint64_t signKey;
    int signValid;
    /* (the upper screen's) Engine A's sprites drawn here (ComposeLayers.
     * gpuObjCount; -1: in the picture): their pixels, 2x2 texels a pixel
     * (OBJ_TEX_W x OBJ_TEX_H RGBA5551, tiled), and what each sprite shows
     * and where in it (as the picture was drawn). */
    void *objTex;
    int objCount;
    NitroGpuObj objs[NITRO_GPU_OBJ_MAX];
    /* (what objTex holds: the atlas's first objRows rows, their key;
     * objValid: written) */
    uint64_t objKey;
    int objRows, objValid;
} LayerSet;

/* BG pictures: 2x2 texels a pixel of a 256 x 256 map (and a row of tiles
 * more, unused, that NitroPort_Double2xTiled16 writes the edge into). */
#define BG_TEX_W 512
#define BG_TEX_BYTES (BG_TEX_W * BG_TEX_W * 2)
#define BG_TEX_ALLOC (BG_TEX_BYTES + BG_TEX_W * 8 * 2)
static int sBgsAvailable;   /* (the upper screen's sets have their bgTex) */
static C3D_Tex sBgTex;
/* A band of a BG's pictures: a texel a pixel of the canvas's width (400 at
 * most) and NITRO_SIGN_MAX_ROWS lines: drawn on the field at the DS's pixel
 * size, a pixel a pixel (2x2 texels a pixel would take four times the
 * memory, 768 KB for the three sets). */
#define SIGN_TEX_W 512
#define SIGN_TEX_H 64
#define SIGN_TEX_BYTES (SIGN_TEX_W * SIGN_TEX_H * 2)
#define SIGN_TEX_ALLOC SIGN_TEX_BYTES
static int sSignsAvailable;  /* (the upper screen's sets have their signTex) */
static C3D_Tex sSignTex;
/* (the upper screen's picture drawn last: its sign's BG, or -1; whether
 * the sign moves is a reason for an in-between frame, fades_changed) */
static int sSignShown = -1;
/* Sprites' pictures: 2x2 texels a pixel of the atlas (and a row of tiles
 * more, as BG_TEX_ALLOC). */
#define OBJ_TEX_W (NITRO_GPU_OBJ_ATLAS_W * 2)
#define OBJ_TEX_H (NITRO_GPU_OBJ_ATLAS_H * 2)
#define OBJ_TEX_BYTES (OBJ_TEX_W * OBJ_TEX_H * 2)
#define OBJ_TEX_ALLOC (OBJ_TEX_BYTES + OBJ_TEX_W * 8 * 2)
static int sObjsAvailable;  /* (the upper screen's sets have their objTex) */
static C3D_Tex sObjTex;
/* Engine A's sprites by frame (NitroPort_ComposeNoteObjs): the last
 * OBJ_PLACES_KEPT frames the game noted. */
#define OBJ_PLACES_KEPT 8
static struct {
    uint32_t stamp, swaps;
    int count, valid;
    NitroGpuObj objs[NITRO_GPU_OBJ_MAX];
} sObjPlaces[OBJ_PLACES_KEPT];
static unsigned sObjPlacesNext;
static LightLock sObjPlacesLock;
/* Engine A's master brightness and circle fade by frame
 * (NitroPort_ComposeNoteFrame). */
static struct {
    uint32_t stamp, swaps;
    int valid, bright, on, x, y, r;
} sCircleNotes[OBJ_PLACES_KEPT];
static unsigned sCircleNotesNext;
/* Engine A's BG scrolls by frame (NitroPort_ComposeNoteBgOffsets): the
 * last BG_OFS_KEPT frames the game handed over. */
#define BG_OFS_KEPT 16
static struct {
    uint32_t stamp, swaps;
    int32_t ofs[8];   /* (fx32, finer than a pixel where the game has it: NitroPort_Render2DBgOffsetsFine) */
    int valid;
    /* (the place name sign in that frame, NitroPort_Render2DSignNow: its BG
     * or -1, and what its band shows, hashed) */
    int sign;
    uint32_t signContent;
} sBgOfs[BG_OFS_KEPT];
static unsigned sBgOfsNext;
static LightLock sBgOfsLock;   /* (LightLock_Init: the display's setting up) */
/* Engine A's lines by frame, while a battle's start plays in the field
 * (NitroPort_ComposeNoteLines): the last LINES_KEPT frames the game handed
 * over. */
#define LINES_KEPT 16
static struct {
    uint32_t stamp, swaps;
    int valid;
    NitroLineSpans lines;
} sLineNotes[LINES_KEPT];
static unsigned sLineNotesNext;
static LightLock sLineNotesLock;   /* (LightLock_Init: the display's setting up) */

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
    /* (and that scene's SWAP_BUFFERS count: a scene on the screen standing
     * for a later frame, NitroPort_Gx3dSameAs, is that frame's) */
    uint32_t shownSwap3d;
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
static uint16_t sWorkUnder[2][WORK_PIXELS], sWorkOver[2][CANVAS_MAX * CANVAS_MAX_H];
static uint8_t sWorkAlpha[2][CANVAS_MAX * CANVAS_MAX_H];
static uint16_t sToRgb565[32768];
static uint8_t sAlpha16[17];      /* the over layer's alpha, 0-16, as 0-255 */
static uint32_t sConverted[2][CANVAS_MAX * CANVAS_MAX_H];
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
 * brightness; engine A's sprites drawn here, each eye's: draw_gpu_objs) */
#define CIRCLE_SEGMENTS 96
#define MAX_QUAD_VERTICES (6 * (32 + 2 * 24 + 96 + 2 * 2 * NITRO_GPU_OBJ_MAX + 2 * CIRCLE_SEGMENTS))
/* An H-blank effect's 3D drawn once an eye into a texture (draw_slices):
 * the texture, a framebuffer too (the screen's 240 x 400 or 320 at its
 * corner: its sides powers of two, the framebuffer's width its stride),
 * and its runs' quads, a frame's (both eyes; each half of sSliceVertsAll
 * as sQuadsAll's). Made the first time it's needed, when there's VRAM for
 * it and SLICE_VRAM_SPARE more (the outlines' copies come later): RGBA8
 * (1 MB with its depth/stencil), or RGB565 (768 KB) without room for
 * that. */
#define SLICE_TEX_W 256
#define SLICE_TEX_H 512
#define SLICE_VERTICES (6 * DS_H * 2)
#define SLICE_VRAM_SPARE (512u * 1024u)
static C3D_Tex sSliceTex;
static uint8_t sSliceFb[PICA_FRAMEBUF_BYTES] __attribute__((aligned(8)));
static void *sSliceColor, *sSliceDepth;
static NitroGx3dGpuVertex *sSliceVertsAll, *sSliceVerts;
static int sSliceVertices;
static int sSliceState;   /* (0: not made yet; 1: made; -1: no room for it) */
static int sSliceRgba;    /* (its colour RGBA8: its alpha where the 3D drew, 0 elsewhere; else RGB565) */
static int sSliceWhy = -1, sSliceTold;   /* (the last decision logged: slices_why) */
static int sSliceLeft = -1;   /* (the frame's left eye: from the texture 1, a run at a time 0, not yet -1) */
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
        if (scr->set[i].bgTex) linearFree(scr->set[i].bgTex);
        if (scr->set[i].objTex) linearFree(scr->set[i].objTex);
        if (scr->set[i].signTex) linearFree(scr->set[i].signTex);
        scr->set[i].under = NULL;
        scr->set[i].over = NULL;
        scr->set[i].bgTex = NULL;
        scr->set[i].bgValid = 0;
        scr->set[i].objTex = NULL;
        scr->set[i].signTex = NULL;
        scr->set[i].signValid = 0;
        scr->set[i].objCount = -1;
        scr->set[i].objValid = 0;
    }
}

static void slices_free(void)
{
    if (sSliceColor) {
        vramFree(sSliceColor);
        sSliceColor = NULL;
    }
    if (sSliceDepth) {
        vramFree(sSliceDepth);
        sSliceDepth = NULL;
    }
    if (sSliceVertsAll) {
        linearFree(sSliceVertsAll);
        sSliceVertsAll = NULL;
    }
    sSliceVerts = NULL;
    sSliceVertices = 0;
    sSliceRgba = 0;
    sSliceState = 0;
}

/* Whether draw_slices can draw (its texture and quads made the first time
 * it's asked: VRAM for the texture and its depth/stencil, 1 MB or 768 KB,
 * with SLICE_VRAM_SPARE more; else never, the runs drawn as before). */
static int slices_ready(void)
{
    if (sSliceState == 0) {
        u32 depthSize = C3D_CalcDepthBufSize(SLICE_TEX_W, SLICE_TEX_H, GPU_RB_DEPTH24_STENCIL8);
        u32 room = vramSpaceFree();
        /* (RGBA8 where there's room: the 3D's empty pixels known, what's
         * behind it on the screen showing there whatever it is) */
        int rgba = room >= C3D_CalcColorBufSize(SLICE_TEX_W, SLICE_TEX_H, GPU_RB_RGBA8) + depthSize + SLICE_VRAM_SPARE;
        u32 colorSize = C3D_CalcColorBufSize(SLICE_TEX_W, SLICE_TEX_H, rgba ? GPU_RB_RGBA8 : GPU_RB_RGB565);

        sSliceState = -1;
        if (room >= colorSize + depthSize + SLICE_VRAM_SPARE) {
            sSliceDepth = vramAlloc(depthSize);
            if (sSliceDepth) {
                /* (the colour in the other VRAM bank from the depth, as
                 * citro3d puts them: 0x1F000000-0x1F2FFFFF is bank A) */
                sSliceColor = vramAllocAt(colorSize, (u32)sSliceDepth < 0x1F300000u ? VRAM_ALLOC_B : VRAM_ALLOC_A);
                if (!sSliceColor) {
                    sSliceColor = vramAlloc(colorSize);
                }
                if (!sSliceColor && rgba) {
                    /* (the free VRAM not in one piece that big: RGB565) */
                    rgba = 0;
                    colorSize = C3D_CalcColorBufSize(SLICE_TEX_W, SLICE_TEX_H, GPU_RB_RGB565);
                    sSliceColor = vramAlloc(colorSize);
                }
            }
            if (sSliceColor) {
                sSliceVertsAll = linearAlloc(2u * SLICE_VERTICES * sizeof(NitroGx3dGpuVertex));
            }
        }
        if (sSliceColor && sSliceDepth && sSliceVertsAll &&
            NitroPort_PicaFrameBufMake(sSliceFb, sizeof(sSliceFb), sSliceColor, sSliceDepth, SLICE_TEX_W, SLICE_TEX_H,
                                       !rgba)) {
            make_texture(&sSliceTex, sSliceColor, rgba ? GPU_RGBA8 : GPU_RGB565, SLICE_TEX_W, SLICE_TEX_H, 0,
                         GPU_CLAMP_TO_EDGE);
            sSliceVerts = sSliceVertsAll + (sQuadHalf ? SLICE_VERTICES : 0);
            sSliceVertices = 0;
            sSliceRgba = rgba;
            sSliceState = 1;
            NitroPort_Log("[compose] an H-blank effect's 3D drawn once into a texture (%s), its lines from it (%lu KB "
                          "of VRAM left)", rgba ? "RGBA8" : "RGB565", (unsigned long)(vramSpaceFree() / 1024u));
        } else {
            NitroPort_Log("[compose] no room for the H-blank effects' texture (%lu KB of VRAM free): their 3D "
                          "drawn a run of lines at a time", (unsigned long)(room / 1024u));
            slices_free();
            sSliceState = -1;
        }
    }
    return sSliceState > 0;
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
    sBgsAvailable = 0;
    sObjsAvailable = 0;
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
    slices_free();
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
        scr->set[i].texScale = scale;
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
 * (the 64 MB memory mode: about 20 MB of it, issue #188), smaller rather
 * than the classic display: first the 2D's textures a texel a pixel (most
 * of the memory: about 10 MB less), then also the fewest LCD framebuffers
 * (frames waiting for their V-blank: fewer, and 60 frames a second holds
 * less well). Each kept only if it leaves room for the 3D's textures,
 * the last whatever it leaves. */
int NitroPort_ComposeInit(const ComposeConfig *config)
{
    static const char *const how[] = { "", "the 2D a texel a pixel", "the 2D a texel a pixel, 3 frames on the way at most" };
    int small;

    NitroPort_Log("[compose] linear memory: %lu KB free", (unsigned long)(linearSpaceFree() / 1024u));
    for (small = 0; small <= 2; small++) {
        if (!compose_init(config, small)) {
            continue;
        }
        if (small < 2 && linearSpaceFree() < COMPOSE_TEXTURE_ROOM) {
            NitroPort_Log("[compose] %lu KB of linear memory left for the 3D's textures: the display made smaller",
                          (unsigned long)(linearSpaceFree() / 1024u));
            free_all();
            continue;
        }
        if (small) {
            NitroPort_Log("[compose] the display made smaller for the memory (%lu KB left): %s",
                          (unsigned long)(linearSpaceFree() / 1024u), how[small]);
        }
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
    LightLock_Init(&sBgOfsLock);
    LightLock_Init(&sLineNotesLock);
    memset(sLineNotes, 0, sizeof(sLineNotes));
    sLineNotesNext = 0;
    memset(sBgOfs, 0, sizeof(sBgOfs));
    LightLock_Init(&sObjPlacesLock);
    memset(sObjPlaces, 0, sizeof(sObjPlaces));
    memset(sCircleNotes, 0, sizeof(sCircleNotes));
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
        scr->nlcd = small >= 2 ? LCD_BUFS_LEAST : s == 0 ? LCD_BUFS : LCD_BUFS_LOWER;
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
    /* (last, as it can do without: the field's weather's BG drawn by the
     * GPU, NitroPort_ComposeDrawsBgs) */
    sBgsAvailable = 1;
    for (i = 0; i < SETS && sBgsAvailable; i++) {
        sScreens[0].set[i].bgTex = linearAlloc(BG_TEX_ALLOC);
        sScreens[0].set[i].bgValid = 0;
        if (!sScreens[0].set[i].bgTex) {
            sBgsAvailable = 0;
        }
    }
    if (!sBgsAvailable) {
        for (i = 0; i < SETS; i++) {
            if (sScreens[0].set[i].bgTex) {
                linearFree(sScreens[0].set[i].bgTex);
                sScreens[0].set[i].bgTex = NULL;
            }
        }
        NitroPort_Log("[compose] not enough memory for the weather's BG on the GPU: drawn with the 2D");
    }
    /* (as well: the sprites drawn by the GPU, NitroPort_ComposeDrawsObjs) */
    sObjsAvailable = 1;
    for (i = 0; i < SETS && sObjsAvailable; i++) {
        sScreens[0].set[i].objTex = linearAlloc(OBJ_TEX_ALLOC);
        sScreens[0].set[i].objCount = -1;
        sScreens[0].set[i].objValid = 0;
        if (!sScreens[0].set[i].objTex) {
            sObjsAvailable = 0;
        }
    }
    if (!sObjsAvailable) {
        for (i = 0; i < SETS; i++) {
            if (sScreens[0].set[i].objTex) {
                linearFree(sScreens[0].set[i].objTex);
                sScreens[0].set[i].objTex = NULL;
            }
        }
        NitroPort_Log("[compose] not enough memory for sprites on the GPU: drawn with the 2D");
    }
    /* (and the place name sign's band, NitroPort_ComposeDrawsSigns) */
    sSignsAvailable = 1;
    for (i = 0; i < SETS && sSignsAvailable; i++) {
        sScreens[0].set[i].signTex = linearAlloc(SIGN_TEX_ALLOC);
        sScreens[0].set[i].signValid = 0;
        if (!sScreens[0].set[i].signTex) {
            sSignsAvailable = 0;
        }
    }
    if (!sSignsAvailable) {
        for (i = 0; i < SETS; i++) {
            if (sScreens[0].set[i].signTex) {
                linearFree(sScreens[0].set[i].signTex);
                sScreens[0].set[i].signTex = NULL;
            }
        }
        NitroPort_Log("[compose] not enough memory for the place name sign on the GPU: drawn with the 2D");
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

int NitroPort_ComposeDrawsBgs(void)
{
    return sReady && sBgsAvailable;
}

int NitroPort_ComposeDrawsSigns(void)
{
    return sReady && sSignsAvailable;
}

int NitroPort_ComposeDrawsObjs(void)
{
    return sReady && sObjsAvailable;
}

void NitroPort_ComposeNoteObjs(uint32_t stamp, uint32_t swaps, const NitroGpuObj *objs, int count)
{
    if (count < 0 || count > NITRO_GPU_OBJ_MAX) {
        return;
    }
    LightLock_Lock(&sObjPlacesLock);
    sObjPlaces[sObjPlacesNext].stamp = stamp;
    sObjPlaces[sObjPlacesNext].swaps = swaps;
    sObjPlaces[sObjPlacesNext].count = count;
    memcpy(sObjPlaces[sObjPlacesNext].objs, objs, (size_t)count * sizeof(objs[0]));
    sObjPlaces[sObjPlacesNext].valid = 1;
    sObjPlacesNext = (sObjPlacesNext + 1u) % OBJ_PLACES_KEPT;
    LightLock_Unlock(&sObjPlacesLock);
}

void NitroPort_ComposeNoteFrame(uint32_t stamp, uint32_t swaps, int bright, int on, int x, int y, int r)
{
    LightLock_Lock(&sObjPlacesLock);
    sCircleNotes[sCircleNotesNext].stamp = stamp;
    sCircleNotes[sCircleNotesNext].swaps = swaps;
    sCircleNotes[sCircleNotesNext].bright = bright;
    sCircleNotes[sCircleNotesNext].on = on;
    sCircleNotes[sCircleNotesNext].x = x;
    sCircleNotes[sCircleNotesNext].y = y;
    sCircleNotes[sCircleNotesNext].r = r;
    sCircleNotes[sCircleNotesNext].valid = 1;
    sCircleNotesNext = (sCircleNotesNext + 1u) % OBJ_PLACES_KEPT;
    LightLock_Unlock(&sObjPlacesLock);
}

/* The frame the DS shows a scene in, as noted (NitroPort_ComposeNoteFrame;
 * found as objs_of): its index in sCircleNotes, or -1. Under
 * sObjPlacesLock. */
static int frame_note_of(uint32_t seq, uint32_t stamp)
{
    int i, found = -1;
    uint32_t best = 0;

    for (i = 0; i < OBJ_PLACES_KEPT; i++) {
        if (sCircleNotes[i].valid && sCircleNotes[i].swaps == seq &&
            (found < 0 || (int32_t)(sCircleNotes[i].stamp - best) > 0)) {
            found = i;
            best = sCircleNotes[i].stamp;
        }
    }
    if (found < 0) {
        for (i = 0; i < OBJ_PLACES_KEPT; i++) {
            uint32_t d = sCircleNotes[i].stamp - stamp;

            if (sCircleNotes[i].valid && d >= 1u && d <= 4u && (found < 0 || d < best)) {
                found = i;
                best = d;
            }
        }
    }
    return found;
}

/* Whether the frame the DS shows a scene in (its SWAP_BUFFERS count) is
 * still to be noted: the game notes a frame in its V-blank work, a V-blank
 * after the scene's SWAP_BUFFERS, and the display thread has the scene as
 * soon as the 3D's made it. An in-between scene drawn before then had no
 * scrolls, sprites, circle or brightness of its own frame and took those
 * of the frame before (or the picture's, older still): Eterna Forest's
 * light shaking as the player walked, the HM cut-in's streaks jumping
 * back a step and the circle fade's in-between frames standing still
 * (videos at 120 frames a second, 2026-10-31). Not when a newer frame's
 * been noted already (that scene's frame left out: nothing to wait for). */
static int frame_note_due(uint32_t seq)
{
    int i, any = 0, due = 1;

    LightLock_Lock(&sObjPlacesLock);
    for (i = 0; i < OBJ_PLACES_KEPT; i++) {
        if (sCircleNotes[i].valid) {
            any = 1;
            if ((int32_t)(sCircleNotes[i].swaps - seq) >= 0) {
                due = 0;
            }
        }
    }
    LightLock_Unlock(&sObjPlacesLock);
    return any && due;
}

/* Engine A's circle fade as the DS shows it with a scene: whether it's
 * known and on (1, or 2 for a band of lines: NitroPort_Render2DCircleNow),
 * its middle and radius (a band's first line and the line after its last);
 * the V-blank it was noted in (noted). */
static int circle_of(uint32_t seq, uint32_t stamp, int *x, int *y, int *r, uint32_t *noted)
{
    int found, on = 0;

    LightLock_Lock(&sObjPlacesLock);
    found = frame_note_of(seq, stamp);
    if (found >= 0 && sCircleNotes[found].on > 0) {
        on = sCircleNotes[found].on;   /* (1 a circle, 2 a band of lines) */
        *x = sCircleNotes[found].x;
        *y = sCircleNotes[found].y;
        *r = sCircleNotes[found].r;
        *noted = sCircleNotes[found].stamp;
    }
    LightLock_Unlock(&sObjPlacesLock);
    return on;
}

/* Engine A's master brightness as the DS shows it with a scene, as a
 * signed factor (+ brighter, - darker, 0-16): whether it's known; the
 * V-blank it was noted in (noted), and its circle or band (kind: 0 none,
 * 1 a circle, 2 a band of lines). */
static int bright_of(uint32_t seq, uint32_t stamp, int *v, uint32_t *noted, int *kind)
{
    int found;

    LightLock_Lock(&sObjPlacesLock);
    found = frame_note_of(seq, stamp);
    if (found >= 0) {
        int b = sCircleNotes[found].bright, mode = (b >> 14) & 3, f = b & 31;

        *v = mode == 1 ? (f > 16 ? 16 : f) : mode == 2 ? -(f > 16 ? 16 : f) : 0;
        *noted = sCircleNotes[found].stamp;
        *kind = sCircleNotes[found].on > 0 ? sCircleNotes[found].on : 0;
    }
    LightLock_Unlock(&sObjPlacesLock);
    return found >= 0;
}

/* Whether a picture (its 2D's V-blank, set) is of the frame noted in a
 * V-blank or newer: the frame's brightness or circle goes with it. A
 * picture older than that (the 2D's drawer behind, a map's first frames)
 * keeps its own: walking out of a building, the outside showed whole at
 * full brightness before the opening band came (the picture from before
 * the band, with the band's frames' brightness), 2026-10-31. (The note is
 * made before the 2D's stamp is read, in the same V-blank work: a picture
 * of the frame has its stamp or a later one. A V-blank's slack let the
 * picture from just before the band through, the band's halfway brightness
 * on the whole outside: a dim flash of it, a video of 2026-10-31.) */
static int picture_of_frame(uint32_t set, uint32_t noted)
{
    return (int32_t)(set - noted) >= 0;
}

/* (an in-between frame drawn for the 2D's fades alone, the 3D the scene's:
 * no in-between scene, NitroPort_ComposeFrame's mid 2) This frame's, and
 * the frame of the last scene shown (its SWAP_BUFFERS count and stamp),
 * which the fades are halfway from. */
static int sEffectsMid;
static int sLastMainValid;
static uint32_t sLastMainSeq, sLastMainStamp;

static int objs_of(uint32_t seq, uint32_t stamp, NitroGpuObj *out);

/* Whether the compositor's sprites (the HM cut-in's) are other in a scene's
 * frame than in the last scene shown's: how many, which, or where. */
static int objs_moved(const NitroGx3dScene *scene)
{
    static NitroGpuObj now[NITRO_GPU_OBJ_MAX], before[NITRO_GPU_OBJ_MAX];
    int n = objs_of(scene->swap_seq, scene->stamp, now), nb, k;

    if (n < 0 || (nb = objs_of(sLastMainSeq, sLastMainStamp, before)) < 0) {
        return 0;   /* (not noted: not drawn by the compositor) */
    }
    if (n != nb) {
        return 1;
    }
    for (k = 0; k < n; k++) {
        if (now[k].key != before[k].key || now[k].x != before[k].x || now[k].y != before[k].y) {
            return 1;
        }
    }
    return 0;
}

static int bg_offsets_of(uint32_t seq, uint32_t stamp, float out[8], int *sign, uint32_t *signContent);

/* Whether the band of a BG the compositor draws as it scrolls (the place
 * name sign, as the upper screen's last picture has it) is scrolled other
 * in a scene's frame than in the last scene shown's: it slides on in an
 * in-between frame drawn for it, as the sprites do. */
static int sign_moved(const NitroGx3dScene *scene)
{
    float now[8], before[8];
    int bg = sSignShown;

    if (bg <= 0 || bg > 3 || !bg_offsets_of(scene->swap_seq, scene->stamp, now, NULL, NULL) ||
        !bg_offsets_of(sLastMainSeq, sLastMainStamp, before, NULL, NULL)) {
        return 0;
    }
    return now[bg * 2 + 1] != before[bg * 2 + 1];
}

static int lines_note_of(uint32_t seq, uint32_t stamp, NitroLineSpans *out);
/* (the upper screen's newest picture published: its windows left out for
 * the compositor, ComposeLayers.gpuSpansOn) */
static int sSpansPublished;

/* Whether a battle's start's slices (their windows the compositor's,
 * NitroPort_ComposeNoteLines: slices in both frames, the upper screen's
 * newest picture leaving them out) are other in a scene's frame than in the
 * last scene shown's: they move on in an in-between frame drawn for them, as
 * the sprites do. */
static int lines_moved(const NitroGx3dScene *scene)
{
    static NitroLineSpans now, before;

    return __atomic_load_n(&sSpansPublished, __ATOMIC_ACQUIRE) &&
           lines_note_of(scene->swap_seq, scene->stamp, &now) && now.slices &&
           lines_note_of(sLastMainSeq, sLastMainStamp, &before) && before.slices &&
           (memcmp(now.scroll, before.scroll, sizeof(now.scroll)) != 0 ||
            memcmp(now.lo, before.lo, sizeof(now.lo)) != 0 || memcmp(now.hi, before.hi, sizeof(now.hi)) != 0);
}

/* Whether the 2D's fades (the master brightness, a circle or band) are
 * other in a scene's frame than in the last scene shown's; or the
 * compositor's sprites, or its sign. (The HM cut-in's sprites move every game step:
 * halfway in the in-between frames. With an in-between scene left out
 * (the 3D behind, its core shared with the 2D's drawing: 4-11 a second
 * on Route 206, log 8532faaf) there was no in-between frame for them, the
 * frame before shown twice: the cut-in not smooth, the 3D still behind
 * it. Now that in-between frame is drawn with the scene's 3D, the sprites
 * halfway.) */
static int fades_changed(const NitroGx3dScene *scene)
{
    int a = 0, b = 0, ka = 0, kb = 0, ax = 0, ay = 0, ar = 0, bx = 0, by = 0, br = 0, ca, cb;
    uint32_t noted;

    if (!sLastMainValid) {
        return 0;
    }
    if (!bright_of(scene->swap_seq, scene->stamp, &a, &noted, &ka) ||
        !bright_of(sLastMainSeq, sLastMainStamp, &b, &noted, &kb)) {
        return 0;
    }
    ca = circle_of(scene->swap_seq, scene->stamp, &ax, &ay, &ar, &noted);
    cb = circle_of(sLastMainSeq, sLastMainStamp, &bx, &by, &br, &noted);
    return a != b || ca != cb || (ca && (ax != bx || ay != by || ar != br)) || objs_moved(scene) || sign_moved(scene) ||
           lines_moved(scene);
}

/* A scene's frame before (as an in-between scene has it: the frame the
 * scene before it is shown in) and whether it has one: an in-between
 * scene's own, or, in an in-between frame for the fades alone, the last
 * scene shown's. */
static int frame_before(const NitroGx3dScene *scene, uint32_t *seq, uint32_t *stamp)
{
    if (sEffectsMid) {
        *seq = sLastMainSeq;
        *stamp = sLastMainStamp;
        return sLastMainValid && (sLastMainSeq != scene->swap_seq || sLastMainStamp != scene->stamp);
    }
    *seq = scene->swap_seq_before;
    *stamp = scene->stamp_before;
    return scene->stamp_before != scene->stamp || scene->swap_seq_before != scene->swap_seq;
}

/* Engine A's sprites as the DS shows them with a scene (its swap_seq and
 * stamp): the last frame noted with its SWAP_BUFFERS made (as bg_offsets_of).
 * How many, or -1. */
static int objs_of(uint32_t seq, uint32_t stamp, NitroGpuObj *out)
{
    int i, found = -1, n = -1;
    uint32_t best = 0;

    LightLock_Lock(&sObjPlacesLock);
    for (i = 0; i < OBJ_PLACES_KEPT; i++) {
        if (sObjPlaces[i].valid && sObjPlaces[i].swaps == seq &&
            (found < 0 || (int32_t)(sObjPlaces[i].stamp - best) > 0)) {
            found = i;
            best = sObjPlaces[i].stamp;
        }
    }
    if (found < 0) {
        for (i = 0; i < OBJ_PLACES_KEPT; i++) {
            uint32_t d = sObjPlaces[i].stamp - stamp;

            if (sObjPlaces[i].valid && d >= 1u && d <= 4u && (found < 0 || d < best)) {
                found = i;
                best = d;
            }
        }
    }
    if (found >= 0) {
        n = sObjPlaces[found].count;
        memcpy(out, sObjPlaces[found].objs, (size_t)n * sizeof(out[0]));
    }
    LightLock_Unlock(&sObjPlacesLock);
    return n;
}


void NitroPort_ComposeNoteBgOffsets(uint32_t stamp, uint32_t swaps, const int32_t fine[8], int sign,
                                    uint32_t signContent)
{
    LightLock_Lock(&sBgOfsLock);
    sBgOfs[sBgOfsNext].stamp = stamp;
    sBgOfs[sBgOfsNext].swaps = swaps;
    memcpy(sBgOfs[sBgOfsNext].ofs, fine, sizeof(sBgOfs[0].ofs));
    sBgOfs[sBgOfsNext].sign = sign;
    sBgOfs[sBgOfsNext].signContent = signContent;
    sBgOfs[sBgOfsNext].valid = 1;
    sBgOfsNext = (sBgOfsNext + 1u) % BG_OFS_KEPT;
    LightLock_Unlock(&sBgOfsLock);
}

/* The scrolls the DS shows a scene with: a frame handed over after its
 * SWAP_BUFFERS (its V-blank set them), the game's count of them then the
 * scene's (swap_seq); the last such (the game making no new scene for a
 * while, a map's change: the frames since, as they go on). Not by the V-blank clock alone: the game's
 * frames don't keep step with the 3DS's V-blanks, and a frame handed over
 * in the same V-blank as the scene was made was taken for the one before
 * it, or the one after it for it, now and then: the field's weather (Eterna
 * Forest's light) went back and forth as the player walked, 2026-10-30.
 * That only when none was noted so (the nearest within 4 V-blanks after
 * its stamp). Whether they're known; and (sign, signContent: NULL for none)
 * the place name sign then. */
static int bg_offsets_of(uint32_t seq, uint32_t stamp, float out[8], int *sign, uint32_t *signContent)
{
    int i, found = -1;
    uint32_t best = 0;

    LightLock_Lock(&sBgOfsLock);
    for (i = 0; i < BG_OFS_KEPT; i++) {
        if (sBgOfs[i].valid && sBgOfs[i].swaps == seq && (found < 0 || (int32_t)(sBgOfs[i].stamp - best) > 0)) {
            found = i;
            best = sBgOfs[i].stamp;
        }
    }
    if (found < 0) {
        for (i = 0; i < BG_OFS_KEPT; i++) {
            uint32_t d = sBgOfs[i].stamp - stamp;

            if (sBgOfs[i].valid && d >= 1u && d <= 4u && (found < 0 || d < best)) {
                found = i;
                best = d;
            }
        }
    }
    if (found >= 0) {
        for (i = 0; i < 8; i++) {
            out[i] = (float)sBgOfs[found].ofs[i] / 4096.0f;
        }
        if (sign != NULL) {
            *sign = sBgOfs[found].sign;
        }
        if (signContent != NULL) {
            *signContent = sBgOfs[found].signContent;
        }
    }
    LightLock_Unlock(&sBgOfsLock);
    return found >= 0;
}

void NitroPort_ComposeNoteLines(uint32_t stamp, uint32_t swaps, const NitroLineSpans *lines)
{
    LightLock_Lock(&sLineNotesLock);
    sLineNotes[sLineNotesNext].stamp = stamp;
    sLineNotes[sLineNotesNext].swaps = swaps;
    sLineNotes[sLineNotesNext].lines = *lines;
    sLineNotes[sLineNotesNext].valid = 1;
    sLineNotesNext = (sLineNotesNext + 1u) % LINES_KEPT;
    LightLock_Unlock(&sLineNotesLock);
}

/* Engine A's lines as the DS shows them with a scene (its swap_seq and
 * stamp: the frame bg_offsets_of finds), into out; whether they're known. */
static int lines_note_of(uint32_t seq, uint32_t stamp, NitroLineSpans *out)
{
    int i, found = -1;
    uint32_t best = 0;

    LightLock_Lock(&sLineNotesLock);
    for (i = 0; i < LINES_KEPT; i++) {
        if (sLineNotes[i].valid && sLineNotes[i].swaps == seq &&
            (found < 0 || (int32_t)(sLineNotes[i].stamp - best) > 0)) {
            found = i;
            best = sLineNotes[i].stamp;
        }
    }
    if (found < 0) {
        for (i = 0; i < LINES_KEPT; i++) {
            uint32_t d = sLineNotes[i].stamp - stamp;

            if (sLineNotes[i].valid && d >= 1u && d <= 4u && (found < 0 || d < best)) {
                found = i;
                best = d;
            }
        }
    }
    if (found >= 0) {
        *out = sLineNotes[found].lines;
    }
    LightLock_Unlock(&sLineNotesLock);
    return found >= 0;
}

/* Halfway from a to b (9-bit scrolls, finer than a pixel where the game
 * has them so), the shorter way round the BG's picture, which repeats every
 * 256 pixels (the compositor draws only a 256 x 256 one: Eterna Forest's
 * light scrolled from 202 to 459 is a pixel on, not half the map back); a
 * jump of more than a quarter of it: b. */
static float bg_offset_halfway(float a, float b)
{
    float d = fmodf(b - a, 256.0f);

    if (d < 0.0f) {
        d += 256.0f;
    }
    if (d >= 128.0f) {
        d -= 256.0f;
    }
    return d > 64.0f || d < -64.0f ? b : b - d * 0.5f;
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
static void publish_whole(int e, const ComposeLayers *l, Screen *scr, LayerSet *set, int w, int h, int texScale)
{
    uint16_t *c16 = sConverted16[e];
    int i, n = w * h;
    /* (the texture's rows the picture uses: tile rows, whole) */
    u32 rowsUsed = ((u32)h * texScale + 8u + 7u) & ~7u;

    set->pub.heldBy = NULL;   /* (not tracked this way) */
    set->pub.over16 = 0;      /* (RGBA8 here) */
    for (i = 0; i < n; i++) {
        c16[i] = sToRgb565[l->under[i] & 0x7FFF];
    }
    if (sConfig.filter == COMPOSE_FILTER_SCALE2X && texScale == 2) {
        NitroPort_Scale2xTiled16(c16, w, h, set->under, scr->texW);
    } else {
        NitroPort_Tile16(c16, w, h, set->under, scr->texW);
    }
    GSPGPU_FlushDataCache(set->under, (u32)scr->texW * rowsUsed * 2u);
    if (l->has3d) {
        /* Often there's nothing in front: then the texture isn't written.
         * (A tall picture's: the DS's lines, the 3D's.) */
        uint32_t *c32 = sConverted[e];

        if (NitroPort_PremultiplyOver(l->over, l->overAlpha, sAlpha16, c32, 0, w * OVER_ROWS(l))) {
            if (sConfig.filter == COMPOSE_FILTER_SCALE2X && texScale == 2) {
                NitroPort_Scale2xTiled32(c32, w, OVER_ROWS(l), set->over, scr->texW);
            } else {
                NitroPort_Tile32(c32, w, OVER_ROWS(l), set->over, scr->texW);
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
static void over_rows(const uint16_t *over, int w, int rows, int *top, int *bottom)
{
    const uint32_t *words = (const uint32_t *)over;
    int y, i, first = -1, last = -1;

    *top = 0;
    *bottom = rows;
    if (((uintptr_t)over & 3u) || (w & 1)) {
        return;   /* (all of it) */
    }
    for (y = 0; y < rows; y++) {
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
    *bottom = last + 2 < rows ? last + 2 : rows;
}

/* The picture behind fills the screen (its colour can be the clear). */
static int under_fills_screen(const Screen *scr, const LayerSet *set)
{
    int canvasW = set->info.width > 0 ? set->info.width : DS_W;

    return set->underColor >= 0 && !set->info.displayOff &&
           (float)canvasW * PIC_SCALE(&set->info) >= (float)scr->width - 0.5f;
}

static void flush_texture(void *data, uint32_t bytes)
{
    GSPGPU_FlushDataCache(data, bytes);
}

/* (NitroPort_ComposePublish) The sprites' atlas rows in use: down to the
 * row below the lowest of their pictures, 4 at a time (a tile's 8 texels
 * doubled). */
static int atlas_rows(const NitroGpuObj *objs, int n)
{
    int k, rows = 0;

    for (k = 0; k < n; k++) {
        int bottom = (int)objs[k].ay + (int)objs[k].h + 1;

        if (bottom > rows) {
            rows = bottom;
        }
    }
    rows = (rows + 3) & ~3;
    return rows > NITRO_GPU_OBJ_ATLAS_H ? NITRO_GPU_OBJ_ATLAS_H : rows;
}

/* (and) A key of what those rows hold: two hashes of their words. */
static uint64_t atlas_key(const uint16_t *atlas, int rows)
{
    const uint32_t *w = (const uint32_t *)(const void *)atlas;
    uint32_t a = 2166136261u, b = 0x9E3779B9u;
    int i, n = rows * NITRO_GPU_OBJ_ATLAS_W / 2;

    for (i = 0; i < n; i++) {
        a = (a ^ w[i]) * 16777619u;
        b = (b + w[i]) * 2654435761u;
        b ^= b >> 15;
    }
    return (((uint64_t)a << 32) | b) ^ (uint64_t)rows;
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
    /* (a picture too wide for the screen's textures at 2 texels a pixel: 1,
     * drawn a pixel a pixel) */
    int texScale = w * scr->texScale > scr->texW || h * scr->texScale + 8 > scr->texH ? 1 : scr->texScale;

    if (!sReady || w > CANVAS_MAX || w * texScale > scr->texW || w * h > WORK_PIXELS || h * texScale + 8 > scr->texH) {
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
    if (!screen) {
        __atomic_store_n(&sSpansPublished, l->gpuSpansOn && l->stretch, __ATOMIC_RELEASE);
    }
    set->info.under = NULL;
    set->info.over = NULL;
    set->info.overAlpha = NULL;
    set->info.gpuBgPicture = NULL;   /* (the drawer's: the set has its own, bgTex) */
    set->info.gpuSignPicture = NULL; /* (the same: signTex) */
    set->info.gpuObj = NULL;         /* (the same: objs, objTex) */
    set->info.gpuObjAtlas = NULL;
    set->overEmpty = 1;
    set->info.height = h;
    set->underColor = l->displayOff ? -1 : uniform_color(l->under, w * h);
    if (l->stretch && l->has3d && !l->displayOff && set->underColor < 0) {
        /* (a battle's start: where its picture behind the 3D isn't one
         * colour, once in the log) */
        static int told;
        int k;

        for (k = 1; !told && k < w * h; k++) {
            if ((l->under[k] & 0x7FFF) != (l->under[0] & 0x7FFF)) {
                NitroPort_Log("[compose] a battle's start: behind the 3D %04x at the top left, %04x at %d,%d (%dx%d)",
                              l->under[0] & 0x7FFF, l->under[k] & 0x7FFF, k % w, k / w, w, h);
                break;
            }
        }
        told = 1;
    }
    set->overTop = 0;
    set->overBottom = OVER_ROWS(l);
    if (!l->displayOff && l->has3d && l->over) {
        over_rows(l->over, w, OVER_ROWS(l), &set->overTop, &set->overBottom);
    }
    set->texScale = texScale;
    if (!l->displayOff && sConfig.filter != COMPOSE_FILTER_SCALE2X && texScale == 2) {
        set->overEmpty = !NitroPort_PublishDoubledOver(&sShadow[e], &set->pub, l->under, l->over, l->overAlpha,
                                                       l->has3d, w, h, OVER_ROWS(l), set->under, set->over,
                                                       scr->texW, sToRgb565, sAlpha16, flush_texture);
    } else if (!l->displayOff) {
        publish_whole(e, l, scr, set, w, h, texScale);
    }
    /* A BG drawn here (the field's weather): its picture, when the set
     * doesn't hold it yet (made by the drawer as it read the game's state:
     * l->gpuBgPicture). */
    if (!l->displayOff && l->gpuBg >= 0 && set->bgTex && l->gpuBgPicture &&
        (!set->bgValid || set->bgKey != l->gpuBgKey)) {
        NitroPort_Double2xTiled16(l->gpuBgPicture, 256, 256, set->bgTex, BG_TEX_W);
        GSPGPU_FlushDataCache(set->bgTex, BG_TEX_BYTES);
        set->bgKey = l->gpuBgKey;
        set->bgValid = 1;
    }
    /* A band of a BG drawn here as it scrolls (the place name sign): its
     * picture, when the set doesn't hold it yet. */
    if (!l->displayOff && l->gpuSign > 0 && set->signTex && l->gpuSignPicture && l->gpuSignRows > 0 &&
        l->gpuSignRows <= NITRO_SIGN_MAX_ROWS && (l->gpuSignRows & 7) == 0 && w <= SIGN_TEX_W && (w & 7) == 0 &&
        (!set->signValid || set->signKey != l->gpuSignKey)) {
        NitroPort_Tile16(l->gpuSignPicture, w, l->gpuSignRows, set->signTex, SIGN_TEX_W);
        GSPGPU_FlushDataCache(set->signTex, SIGN_TEX_BYTES);
        set->signKey = l->gpuSignKey;
        set->signValid = 1;
    }
    /* Engine A's sprites drawn here: their pixels, and what each shows. */
    set->objCount = -1;
    if (!l->displayOff && l->gpuObjCount >= 0 && set->objTex && l->gpuObj && l->gpuObjAtlas) {
        /* (their pixels: the rows in use, and only if the set doesn't hold
         * them already. Through the HM cut-in its sprites mostly move, their
         * pictures the same: the atlas, 512 x 256 texels made and flushed
         * for each of its pictures, was much of the hand-over's ~7 ms on
         * the 3D's core, 2026-11-03) */
        int rows = atlas_rows(l->gpuObj, l->gpuObjCount);
        uint64_t key = rows > 0 ? atlas_key(l->gpuObjAtlas, rows) : 0;

        if (rows > 0 && (!set->objValid || set->objKey != key)) {
            NitroPort_Double2xTiled16Rows(l->gpuObjAtlas, NITRO_GPU_OBJ_ATLAS_W, NITRO_GPU_OBJ_ATLAS_H, 0, rows,
                                          set->objTex, OBJ_TEX_W);
            GSPGPU_FlushDataCache(set->objTex, (u32)rows * OBJ_TEX_W * 4u);
            set->objKey = key;
            set->objRows = rows;
            set->objValid = 1;
        }
        memcpy(set->objs, l->gpuObj, (size_t)l->gpuObjCount * sizeof(set->objs[0]));
        set->objCount = l->gpuObjCount;
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

/* The combiner for a quad: the texture times the vertex colour. */
static void quad_combiner_modulate(void)
{
    C3D_TexEnv *env = C3D_GetTexEnv(0);

    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_PRIMARY_COLOR, 0);
    C3D_TexEnvFunc(env, C3D_Both, GPU_MODULATE);
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
/* The master brightness over a screen. scene: (engine A's, with its 3D)
 * as the game had it in the frame the DS shows the scene in, halfway to
 * that from the frame before in an in-between scene (the field fading to
 * white or black, walking out of Eterna Forest, at 60 frames a second where
 * the game changes it 30 times: in 32nds, the DS's 16ths each halved); as
 * the picture had it where that isn't known, or without one. */
static void master_brightness_of(const Screen *scr, const ComposeLayers *info, const NitroGx3dScene *scene)
{
    int mode = (info->masterBright >> 14) & 3, factor = info->masterBright & 31, a, b;
    int v32;   /* (32nds: + brighter, - darker) */
    uint32_t at = 0, bt = 0;
    int ak = 0, bk = 0;

    if (factor > 16) {
        factor = 16;
    }
    v32 = mode == 1 ? 2 * factor : mode == 2 ? -2 * factor : 0;
    if (scene != NULL && bright_of(scene->swap_seq, scene->stamp, &a, &at, &ak)) {
        uint32_t from = at, seqB, stampB;
        int both = frame_before(scene, &seqB, &stampB) && bright_of(seqB, stampB, &b, &bt, &bk);

        if (both) {
            from = bt;   /* (an in-between scene: its picture is the frame before's) */
        }
        /* (and the picture as that frame has its window: a circle's or a
         * band's picture is drawn whole, another frame's not) */
        if (picture_of_frame(info->stamp, from) && ak == info->gpuCircle && (!both || bk == info->gpuCircle)) {
            v32 = both ? a + b : 2 * a;
        }
    }
    if (v32 != 0) {
        quad_combiner(0);
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA, GPU_ONE, GPU_ZERO);
        quad(0, 0, (float)scr->width, SCREEN_H, 0.0f, 0, 0, 0, 0,
             (v32 > 0 ? 0xFFFFFF00u : 0x00000000u) | ((u32)(v32 > 0 ? v32 : -v32) * 255u / 32u));
    }
}

static void master_brightness(const Screen *scr, const ComposeLayers *info)
{
    master_brightness_of(scr, info, NULL);
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
/* A circle fade the compositor draws (ComposeLayers.gpuCircle): outside the
 * circle its colour, over everything (pic: the picture's rectangle on the
 * screen, sx and sy its pixels' size). The circle where the game had it in
 * the frame the DS shows the scene in, halfway to that from the frame
 * before in an in-between scene (walking into Eterna Forest: the screen
 * closing round the player at 60 frames a second, as the field does, where
 * the game changes it 30 times); as the picture had it where that isn't
 * known. A ring of CIRCLE_SEGMENTS pieces, from the circle to past the
 * screen's corners. */
/* (draw_circle_mask) A band of lines as drawn: its first line and the line
 * after its last on the canvas (in fractions of a line), each edge's speed
 * (lines a V-blank), the canvas's lines, and the V-blank the frame it was
 * last drawn for shows at (once a frame: both eyes have it as it is). */
static struct {
    int on, rows;
    float top, bottom, vTop, vBottom;
    int wantTop, wantBottom;   /* (the game's lines, as last drawn) */
    uint32_t vblank;
    u32 rgba;   /* (its colour) */
} sBand;
static uint32_t sComposedFrames;   /* (NitroPort_ComposeFrame's calls) */
static uint32_t sDrawTarget;       /* (the V-blank the frame being drawn shows at, as far as known) */

#define BAND_TRAIL 20.0f   /* (lines: how far behind the game's edge an edge goes) */
#define BAND_SPEED 6.0f    /* (lines a V-blank: the game's own, about, as the map's first steps come) */

/* One edge, a V-blank on: towards the game's (want, always further out:
 * the band opening) at about the game's speed, a little faster when
 * further behind than BAND_TRAIL, slower when nearer; never past it, never
 * back. (out: -1 the top edge, going up; 1 the bottom one.) */
static void band_edge_step(float *at, float *v, float want, int out)
{
    float behind = (want - *at) * (float)out, step;

    *v = *v * 0.75f + (BAND_SPEED + (behind - BAND_TRAIL) / 8.0f) * 0.25f;
    step = *v < 1.0f ? 1.0f : *v > 16.0f ? 16.0f : *v;
    if (step > behind) {
        step = behind > 0.0f ? behind : 0.0f;
    }
    *at += step * (float)out;
}

/* A band's lines as drawn in a frame (*top, *bottom: where the game has
 * them in; drawn: eased). Opening, driven by the V-blanks rather than the
 * game's steps: right after the map changes the game's steps come
 * unevenly (19 to 26 a second, 2 or 3 V-blanks each, a log of 2026-10-31),
 * and a band eased once a step, each step 16 lines on, jumped. So each
 * edge goes on every V-blank at about the game's speed, trailing its edge
 * by about BAND_TRAIL lines (a step and a bit): a step that comes late
 * doesn't stop it. (The upper screen is drawn again each V-blank for it
 * while it moves: band_moving.) New, or closing: as the game has it. */
static void band_ease(int *top, int *bottom, int rows)
{
    float t = (float)*top, b = (float)*bottom;
    int32_t frames = (int32_t)(sDrawTarget - sBand.vblank);
    int fresh = !sBand.on || frames > 30 || frames < -30;

    if (!fresh && frames <= 0) {
        *top = (int)(sBand.top + 0.5f);   /* (the same V-blank's, or one before: as it is) */
        *bottom = (int)(sBand.bottom + 0.5f);
        return;
    }
    if (fresh || t > sBand.top + 0.5f || b < sBand.bottom - 0.5f) {
        float mid = (t + b) * 0.5f;

        /* (new: opening from a narrow band (less than half the picture),
         * from a little more closed than the game's, its speed the game's;
         * else, or closing, as the game has it: a band standing still (the
         * picture framed) isn't eased) */
        int trail = fresh && b - t < (float)rows * 0.5f;

        sBand.top = trail ? (t + BAND_TRAIL < mid ? t + BAND_TRAIL : mid) : t;
        sBand.bottom = trail ? (b - BAND_TRAIL > mid ? b - BAND_TRAIL : mid) : b;
        sBand.vTop = sBand.vBottom = BAND_SPEED;
    } else {
        for (; frames > 0; frames--) {
            band_edge_step(&sBand.top, &sBand.vTop, t, -1);
            band_edge_step(&sBand.bottom, &sBand.vBottom, b, 1);
        }
    }
    sBand.on = 1;
    sBand.rows = rows;
    sBand.wantTop = *top;
    sBand.wantBottom = *bottom;
    sBand.vblank = sDrawTarget;
    *top = (int)(sBand.top + 0.5f);
    *bottom = (int)(sBand.bottom + 0.5f);
}

/* Whether a band was drawn in the last half second, short of the whole
 * picture. */
static int band_open(void)
{
    u32 now = __atomic_load_n(&sVBlanks, __ATOMIC_RELAXED);

    return sBand.on && (int32_t)(now - sBand.vblank) <= 30 &&
           (sBand.top > 0.5f || sBand.bottom < (float)sBand.rows - 0.5f);
}

/* Whether a band is opening still (band_open, and behind the game's: as
 * the game had it when last drawn, or the whole picture where the game's
 * band is over): the upper screen's drawn again each V-blank. (A band
 * standing still, caught up: not.) */
static int band_moving(void)
{
    return band_open() &&
           (sBand.top > (float)sBand.wantTop + 0.5f || sBand.bottom < (float)sBand.wantBottom - 0.5f);
}

/* Whether the frame the DS shows a scene in is noted at all. */
static int frame_noted(uint32_t seq, uint32_t stamp)
{
    int found;

    LightLock_Lock(&sObjPlacesLock);
    found = frame_note_of(seq, stamp);
    LightLock_Unlock(&sObjPlacesLock);
    return found >= 0;
}

/* (a band of lines, ComposeLayers.gpuCircle 2) Its columns, as noted
 * (gpuCircleX: left | right << 16, 0 all of them), on a canvas columns
 * wide. */
static void band_columns(int packed, int columns, int *left, int *right)
{
    *left = packed == 0 ? 0 : packed & 0xFFFF;
    *right = packed == 0 ? columns : (packed >> 16) & 0xFFFF;
}

/* (a band of lines) Halfway between two bands' columns (packed as noted).
 * One of them empty (aEmpty, bEmpty: its lines none, the screen shown
 * nowhere, as the gate's wipe starts, black): as none of the other's
 * columns, from the edge the other's are from (else the first step went
 * from all the columns, the screen shown halfway across at once). */
static int band_columns_halfway(int a, int aEmpty, int b, int bEmpty, int columns)
{
    int la, ra, lb, rb, l, r;

    band_columns(a, columns, &la, &ra);
    band_columns(b, columns, &lb, &rb);
    if (aEmpty && !bEmpty) {
        la = ra = lb == 0 ? 0 : columns;
    } else if (bEmpty && !aEmpty) {
        lb = rb = la == 0 ? 0 : columns;
    }
    l = (la + lb) / 2;
    r = (ra + rb + 1) / 2;
    return l <= 0 && r >= columns ? 0 : l | (r << 16);
}

/* A band drawn: the backdrop above its first line (y) and from the line
 * after its last (r) on, over the picture (pic, sx its columns' width, sy
 * its lines' height); and beside its columns (columns: gpuCircleX's, packed
 * as noted; 0 all of them: the gate's wipe from one side). */
static void draw_band(const PicaRect *pic, float sx, float sy, int y, int r, int columns, u32 rgba)
{
    float top = pic->y0 + (float)y * sy, bottom = pic->y0 + (float)r * sy, t;
    int cols = (int)((pic->x1 - pic->x0) / sx + 0.5f), left, right;

    if (sConfig.flipV) {
        t = top;
        top = pic->y0 + pic->y1 - bottom;
        bottom = pic->y0 + pic->y1 - t;
    }
    if (sQuadVertices + 24 > MAX_QUAD_VERTICES) {
        return;
    }
    quad_combiner(0);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
    if (top > pic->y0) {
        quad(pic->x0, pic->y0, pic->x1, top, 0.0f, 0, 0, 0, 0, rgba);
    }
    if (bottom < pic->y1) {
        quad(pic->x0, bottom > pic->y0 ? bottom : pic->y0, pic->x1, pic->y1, 0.0f, 0, 0, 0, 0, rgba);
    }
    band_columns(columns, cols, &left, &right);
    if (bottom > top && (left > 0 || right < cols)) {
        float x0 = pic->x0 + (float)left * sx, x1 = pic->x0 + (float)right * sx;
        float t0 = top > pic->y0 ? top : pic->y0, t1 = bottom < pic->y1 ? bottom : pic->y1;

        if (x1 < x0) {
            x1 = x0;
        }
        if (x0 > pic->x0) {
            quad(pic->x0, t0, x0 < pic->x1 ? x0 : pic->x1, t1, 0.0f, 0, 0, 0, 0, rgba);
        }
        if (x1 < pic->x1) {
            quad(x1 > pic->x0 ? x1 : pic->x0, t0, pic->x1, t1, 0.0f, 0, 0, 0, 0, rgba);
        }
    }
    NitroPort_PicaProfileMark("the band of lines");
}

/* A band still opening (band_open) where the picture has none: the
 * game's band done and its windows off, the 2D's drawer with a picture
 * without them already, the band drawn trailing behind the game's still
 * goes on to the whole picture (else the last lines of it went at once). */
static void draw_band_tail(const PicaRect *pic, float sy)
{
    int rows = (int)((pic->y1 - pic->y0) / sy + 0.5f), y = 0, r = rows;

    if (!band_open() || rows != sBand.rows) {
        sBand.on = 0;
        return;
    }
    band_ease(&y, &r, rows);
    draw_band(pic, 1.0f, sy, y, r, 0, sBand.rgba);
}

/* Whether a frame (its SWAP_BUFFERS count and stamp) is noted with engine
 * A's master brightness all black. */
static int frame_black(uint32_t seq, uint32_t stamp)
{
    uint32_t noted;
    int v = 0, kind = 0;

    return bright_of(seq, stamp, &v, &noted, &kind) && v <= -16;
}

/* A band of columns where the picture has none (the gate's wipe: its
 * window off, the picture already without it): in the in-between frame
 * after its last step, its columns halfway to all of them, as its other
 * steps are halfway (else its last 42 of the DS's columns showed at once).
 * And a frame noted with one, the picture ahead of it (its window off
 * already): the frame's columns, halfway from the frame before's as
 * draw_circle_mask has them (else the whole screen for a frame, then the
 * band again). Only from the frames' notes, so both eyes alike. */
static void draw_columns_tail(const NitroGx3dScene *scene, const PicaRect *pic, float sx, float sy)
{
    uint32_t seqB = 0, stampB = 0, noted;
    int ax = 0, ay = 0, ar = 0, bx = 0, by = 0, br = 0, ka, kb, before;
    int cols = (int)((pic->x1 - pic->x0) / sx + 0.5f);

    if (scene == NULL) {
        return;
    }
    ka = circle_of(scene->swap_seq, scene->stamp, &ax, &ay, &ar, &noted);
    before = frame_before(scene, &seqB, &stampB);
    kb = before ? circle_of(seqB, stampB, &bx, &by, &br, &noted) : 0;
    if (ka == 2 && ax != 0 && ay < ar) {
        int x = kb == 2                                          ? band_columns_halfway(ax, 0, bx, by >= br, cols)
                : kb == 0 && before && frame_black(seqB, stampB) ? band_columns_halfway(ax, 0, 0, 1, cols)
                                                                 : ax;

        draw_band(pic, sx, sy, ay, ar, x, sBand.rgba);
    } else if (ka == 0 && kb == 2 && bx != 0 && by < br && frame_noted(scene->swap_seq, scene->stamp)) {
        draw_band(pic, sx, sy, by, br, band_columns_halfway(0, 0, bx, 0, cols), sBand.rgba);
    }
}

/* (draw_circle_mask) The last circle or band noted for a frame drawn: its
 * kind (1 a circle, 2 a band of lines), middle and radius. */
static int sLastCircle, sLastCircleX, sLastCircleY, sLastCircleR;
/* (draw_circle_mask) The circle last drawn: when (a V-blank), where. */
static int sCircleDrawnValid, sCircleDrawnX, sCircleDrawnY, sCircleDrawnR;
static u32 sCircleDrawnAt;
/* (draw_circle_mask) A band with columns (the gate's wipe) as drawn in a
 * frame: the compose it was drawn in (NitroPort_ComposeFrame's call: both
 * eyes, and only them; a frame composed again, a picture published, works
 * it out anew) and the scene (its frame and the frame before), its lines,
 * columns and colour. The other eye draws it as it is: drawing it changes
 * the band's state (sBand), which would take the other eye another way. */
static struct {
    int valid, y, r, x;
    uint32_t compose, seq, stamp, seqBefore, stampBefore;
    u32 rgba;
} sColsDrawn;

static int cols_drawn_key(const NitroGx3dScene *scene)
{
    return sColsDrawn.valid && sColsDrawn.compose == sComposedFrames &&
           sColsDrawn.seq == (scene != NULL ? scene->swap_seq : 0u) &&
           sColsDrawn.stamp == (scene != NULL ? scene->stamp : 0u) &&
           sColsDrawn.seqBefore == (scene != NULL ? scene->swap_seq_before : 0u) &&
           sColsDrawn.stampBefore == (scene != NULL ? scene->stamp_before : 0u);
}

static void draw_circle_mask(const ComposeLayers *info, const NitroGx3dScene *scene, const PicaRect *pic, float sx,
                             float sy)
{
    int x = info->gpuCircleX, y = info->gpuCircleY, r = info->gpuCircleR, ax, ay, ar, bx, by, br, i, k;
    int kind = info->gpuCircle;   /* (1 a circle, 2 a band of lines) */
    uint32_t noted;
    float cx, cy, inner, outer;
    u32 rgba = rgba_of((uint16_t)info->gpuCircleColor, 255);
    extern u32 gpuCmdBufSize, gpuCmdBufOffset;

    if (kind == 2 && cols_drawn_key(scene)) {
        draw_band(pic, sx, sy, sColsDrawn.y, sColsDrawn.r, sColsDrawn.x, sColsDrawn.rgba);   /* (the other eye) */
        return;
    }

    /* (any picture of the circle fade goes with any circle: drawn as inside
     * the circle all over, it isn't drawn again as the circle changes; so
     * the picture's own circle is the one it had when it was last drawn,
     * steps behind. Where the frame's isn't noted as one (the game's last
     * step of a circle fade turns it off, its window still on: the
     * in-between frame after it went back to the picture's, twice the
     * size for a frame, a simulation of 2026-10-31), the frame before's,
     * else the last one noted.) */
    /* (a band of lines within a second of a circle drawn is the circle
     * fade's end (going into Eterna Forest: the circle closed, an empty
     * band, 120-120): as the picture has it, all black, not eased as an
     * opening band. Eased, a strip of the forest showed across the screen
     * for the fade's last frame; drawn as the circle last drawn instead,
     * the circle stood still at its end rather than closing, 2026-11-01.) */
    if (kind == 2 && sCircleDrawnValid &&
        (int32_t)(__atomic_load_n(&sVBlanks, __ATOMIC_RELAXED) - sCircleDrawnAt) <= 60) {
        draw_band(pic, sx, sy, info->gpuCircleY, info->gpuCircleR, info->gpuCircleX, rgba);
        return;
    } else {
        uint32_t seqB, stampB;
        int ka = scene != NULL ? circle_of(scene->swap_seq, scene->stamp, &ax, &ay, &ar, &noted) : 0;
        int before = scene != NULL && frame_before(scene, &seqB, &stampB);
        int kb = before ? circle_of(seqB, stampB, &bx, &by, &br, &noted) : 0;

        /* (a band's picture is drawn whole: the frame noted with no band,
         * the window off as the band ends, it's the picture as it is. Else,
         * the 2D's drawer still on the last band's picture, the band stood
         * at its last noted lines till the next picture: the outside framed
         * in black for a sixth of a second after the door's band opened,
         * then the frame gone at once, a video of 2026-10-31) */
        if (info->gpuCircle == 2 && ka != 2 && scene != NULL && frame_noted(scene->swap_seq, scene->stamp)) {
            /* (a band just drawn going on to the whole picture, its last
             * steps eased as the rest: the game's own went 40 lines at
             * once each side) */
            int rowsAll = (int)((pic->y1 - pic->y0) / sy + 0.5f);
            u32 now = __atomic_load_n(&sVBlanks, __ATOMIC_RELAXED);
            int recent = sBand.on && (int32_t)(now - sBand.vblank) <= 30;

            if (recent && !band_open()) {
                /* (over, and opened all the way; but for a wipe from one
                 * side, whose last step is halfway in an in-between frame
                 * as the others are) */
                if (kb == 2 && bx != 0 && by < br) {
                    draw_band(pic, sx, sy, by, br,
                              band_columns_halfway(0, 0, bx, 0, (int)((pic->x1 - pic->x0) / sx + 0.5f)), rgba);
                }
                return;
            }
            if (!recent) {
                /* (no band drawn lately, none in the frame before: a band
                 * starting, its picture here before a note of it (the
                 * lines noted late): as the picture has it, rather than
                 * the whole picture at once; its columns too, the gate's
                 * wipe's first step) */
                if (info->gpuCircleX != 0 && frame_black(scene->swap_seq, scene->stamp)) {
                    /* (the frame black, the picture ahead of it with the
                     * wipe's first step: none of it shown yet, black; not
                     * its columns at the picture's brightness, then
                     * halfway back in the in-between frame after. As it
                     * is, the band's state left alone: not a band of lines
                     * opening from the middle) */
                    int mid = (info->gpuCircleY + info->gpuCircleR) / 2;

                    draw_band(pic, sx, sy, mid, mid, 0, rgba_of(0, 255));
                    return;
                }
                ka = 2;
                kb = 0;
                ax = info->gpuCircleX;
                ay = info->gpuCircleY;
                ar = info->gpuCircleR;
            } else {
                /* (its columns too: from the frame before's, where that had
                 * the band, to all of them; the gate's wipe's last step) */
                bx = kb == 2 ? bx : 0;
                ka = kb = 2;
                ax = 0;
                ay = by = 0;
                ar = br = rowsAll;
            }
        }
        if (ka == info->gpuCircle) {
            x = ax, y = ay, r = ar;
            if (kb == info->gpuCircle && kind == 2 && ((ax != 0 && ay < ar) || (bx != 0 && by < br))) {
                /* (a band of columns, the gate's wipe: on every line, so
                 * its lines the one with columns'; an empty band before
                 * it (the screen black as the wipe starts) as none of its
                 * columns, not the lines in the middle opening) */
                x = band_columns_halfway(ax, ay >= ar, bx, by >= br, (int)((pic->x1 - pic->x0) / sx + 0.5f));
                y = ax != 0 && ay < ar ? ay : by;
                r = ax != 0 && ay < ar ? ar : br;
            } else if (kb == info->gpuCircle) {
                x = (bx + ax) / 2, y = (by + ay) / 2, r = (br + ar) / 2;
            } else if (kind == 2 && kb == 0 && before && ax != 0 && ay < ar && frame_black(seqB, stampB)) {
                /* (the frame before no band but black, its brightness:
                 * the gate's wipe starting, its first step's window on
                 * with the screen still black. As an empty band before it:
                 * its first step halfway too, not its columns at once) */
                x = band_columns_halfway(ax, 0, 0, 1, (int)((pic->x1 - pic->x0) / sx + 0.5f));
            }
            sLastCircle = ka, sLastCircleX = ax, sLastCircleY = ay, sLastCircleR = ar;
        } else if (kb == info->gpuCircle) {
            x = bx, y = by, r = br;
        } else if (sLastCircle == info->gpuCircle && (kind != 2 || (sLastCircleX == 0 && info->gpuCircleX == 0))) {
            /* (a band's: of lines only; one of columns, the gate's wipe,
             * as the picture has it: not one kept from before, a wipe the
             * other way, nor for a building's door) */
            x = sLastCircleX, y = sLastCircleY, r = sLastCircleR;
        }
    }
    if (kind == 2) {
        /* A band of lines (walking out of a building): the backdrop above
         * and below it, its edges halfway in the in-between frames (the
         * game's band 32 lines taller every other frame: 16 each frame),
         * and opening, eased by the V-blank (band_ease). A band of columns
         * (the gate's wipe) isn't: its lines, every line, as they are (and
         * the band after it, the window off, not a band opening still). */
        if (x != 0) {
            sBand.on = 0;
        }
        band_ease(&y, &r, (int)((pic->y1 - pic->y0) / sy + 0.5f));
        sBand.rgba = rgba;
        draw_band(pic, sx, sy, y, r, x, rgba);
        if (x != 0 || info->gpuCircleX != 0) {
            sColsDrawn.valid = 1;
            sColsDrawn.compose = sComposedFrames;
            sColsDrawn.seq = scene != NULL ? scene->swap_seq : 0u;
            sColsDrawn.stamp = scene != NULL ? scene->stamp : 0u;
            sColsDrawn.seqBefore = scene != NULL ? scene->swap_seq_before : 0u;
            sColsDrawn.stampBefore = scene != NULL ? scene->stamp_before : 0u;
            sColsDrawn.y = y, sColsDrawn.r = r, sColsDrawn.x = x, sColsDrawn.rgba = rgba;
        }
        return;
    }
    if (sQuadVertices + 6 * CIRCLE_SEGMENTS > MAX_QUAD_VERTICES || gpuCmdBufSize - gpuCmdBufOffset < 1024u) {
        return;
    }
    cx = pic->x0 + (float)x * sx;
    cy = pic->y0 + (float)y * sy;
    if (sConfig.flipV) {
        cy = pic->y0 + pic->y1 - cy;
    }
    inner = (float)(r < 0 ? 0 : r) * sx;
    outer = inner + 2000.0f;
    for (i = 0; i < CIRCLE_SEGMENTS; i++) {
        static const int corner[6] = { 0, 1, 2, 2, 1, 3 };   /* (inner a, outer a, inner b; inner b, outer a, outer b) */
        float a0 = (float)i * (2.0f * 3.14159265f / CIRCLE_SEGMENTS), a1 = (float)(i + 1) * (2.0f * 3.14159265f / CIRCLE_SEGMENTS);
        float px[4], py[4];

        px[0] = cx + inner * cosf(a0), py[0] = cy + inner * sinf(a0) * (sy / sx);
        px[1] = cx + outer * cosf(a0), py[1] = cy + outer * sinf(a0);
        px[2] = cx + inner * cosf(a1), py[2] = cy + inner * sinf(a1) * (sy / sx);
        px[3] = cx + outer * cosf(a1), py[3] = cy + outer * sinf(a1);
        for (k = 0; k < 6; k++) {
            NitroGx3dGpuVertex *v = &sQuads[sQuadVertices + i * 6 + k];

            v->position[0] = px[corner[k]];
            v->position[1] = py[corner[k]];
            v->position[2] = 0.0f;
            v->position[3] = 1.0f;
            v->color[0] = (uint8_t)(rgba >> 24);
            v->color[1] = (uint8_t)(rgba >> 16);
            v->color[2] = (uint8_t)(rgba >> 8);
            v->color[3] = (uint8_t)rgba;
            v->uv[0] = 0.0f;
            v->uv[1] = 0.0f;
        }
    }
    quad_combiner(0);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
    NitroPort_PicaBindVertices(sQuads);
    C3D_DrawArrays(GPU_TRIANGLES, sQuadVertices, 6 * CIRCLE_SEGMENTS);
    sQuadVertices += 6 * CIRCLE_SEGMENTS;
    {
        sCircleDrawnValid = 1;
        sCircleDrawnAt = __atomic_load_n(&sVBlanks, __ATOMIC_RELAXED);
        sCircleDrawnX = x, sCircleDrawnY = y, sCircleDrawnR = r;
    }
    NitroPort_PicaProfileMark("the circle fade");
}

/* (draw_screen, 5) One BG in a band of lines (ComposeLayers.gpuCircle 3:
 * the HM cut-in's band, BG3, opening and closing 5 to 13 lines each side a
 * step, every other frame; drawn in the picture, it opened and closed 30
 * times a second): the picture has it all over, alone in front of the 3D,
 * and it's shown in the band's lines only, as the game had them in the
 * frame the DS shows the scene in, halfway to that from the frame before
 * in an in-between frame (as the picture has them where they aren't
 * noted): its first line and the line after its last (*y, *r). */
/* (bg_band_rows) The band as drawn: its lines (in fractions of a line),
 * each edge's speed closing (lines a V-blank), the game's lines it was last
 * drawn for, and the V-blank that frame shows at. */
static struct {
    int on, wantY, wantR;
    float y, r, vTop, vBottom;
    uint32_t vblank;
} sBgBand;

static void bg_band_rows(const ComposeLayers *info, const NitroGx3dScene *scene, int *y, int *r, int *left,
                         int *right)
{
    int ax, ay, ar, bx, by, br, ka, kb, ty, tr;
    uint32_t noted, seqB, stampB;
    int32_t frames;

    ty = info->gpuCircleY;
    tr = info->gpuCircleR;
    /* (its columns: the noted x's two halves, NitroPort_Render2DCircleNow;
     * halfway too. The cut-in's stripe slides on from the right, 64 of the
     * DS's columns a step, before it opens: in the picture, it went whole
     * every step, the forest's fog the 2D's then, 2026-11-03) */
    *left = info->gpuCircleX;
    *right = info->gpuCircleRight;
    if (scene != NULL) {
        ka = circle_of(scene->swap_seq, scene->stamp, &ax, &ay, &ar, &noted);
        kb = frame_before(scene, &seqB, &stampB) ? circle_of(seqB, stampB, &bx, &by, &br, &noted) : 0;
        if (ka == 3) {
            ty = ay, tr = ar;
            *left = ax & 0xFFFF, *right = (int)((unsigned)ax >> 16);
            if (kb == 3) {
                ty = (by + ay) / 2, tr = (br + ar) / 2;
                *left = ((bx & 0xFFFF) + *left) / 2, *right = ((int)((unsigned)bx >> 16) + *right) / 2;
            }
        } else if (kb == 3) {
            ty = by, tr = br;
            *left = bx & 0xFFFF, *right = (int)((unsigned)bx >> 16);
        }
    }
    /* (closing, then held: the cut-in's band stops 16 lines tall and the
     * game keeps it so 4 steps, its rings then the backdrop alone, before
     * it goes; after the steady close it looked like a stop, a video of
     * 2026-11-01. Held after closing, it goes on closing at its last
     * speed, never opening again while the game holds it; moved, it's as
     * the game has it) */
    frames = (int32_t)(sDrawTarget - sBgBand.vblank);
    if (!sBgBand.on || frames > 30 || frames < -30) {
        sBgBand.on = 1;
        sBgBand.y = (float)ty, sBgBand.r = (float)tr;
        sBgBand.vTop = sBgBand.vBottom = 0.0f;
        frames = 1;   /* (noted below) */
    } else if (frames > 0) {
        if (ty != sBgBand.wantY || tr != sBgBand.wantR) {
            sBgBand.vTop = (float)(ty - sBgBand.wantY) / (float)frames;
            sBgBand.vBottom = (float)(sBgBand.wantR - tr) / (float)frames;
            sBgBand.y = (float)ty, sBgBand.r = (float)tr;
        } else if (sBgBand.vTop > 0.0f || sBgBand.vBottom > 0.0f) {
            float mid = (sBgBand.y + sBgBand.r) * 0.5f;

            sBgBand.y += (sBgBand.vTop > 0.5f ? sBgBand.vTop : 0.5f) * (float)frames;
            sBgBand.r -= (sBgBand.vBottom > 0.5f ? sBgBand.vBottom : 0.5f) * (float)frames;
            if (sBgBand.y > mid) {
                sBgBand.y = mid;
            }
            if (sBgBand.r < mid) {
                sBgBand.r = mid;
            }
        }
    }
    if (frames > 0) {
        sBgBand.wantY = ty, sBgBand.wantR = tr;
        sBgBand.vblank = sDrawTarget;
    }
    *y = (int)(sBgBand.y + 0.5f);
    *r = (int)(sBgBand.r + 0.5f);
}

/* (draw_gpu_objs) The screen's lines the sprites are cut to, when on. */
static struct {
    int on;
    float y0, y1;
} sObjClip;

/* Engine A's sprites the compositor draws (LayerSet.objs, objTex), in front
 * of the rest of the picture (pic: its rectangle on the screen, sx and sy
 * its pixels' size): each where the game had it in the frame the DS shows
 * the scene in (objs_of), halfway to that from the frame before in an
 * in-between scene (NitroPort_GpuObjsBetween: the cut-in's wind blowing on
 * at 60 frames a second, as the field does, where the game moves it 30
 * times); as the set has them where that isn't known. Each with the set's
 * picture of what it shows (one the set has no picture of isn't drawn: the
 * next set has it). Back to front, cut at the picture's edges. */
static void draw_gpu_objs(const LayerSet *set, const NitroGx3dScene *scene, const PicaRect *pic, float sx, float sy,
                          int linear)
{
    /* (cut to sObjClip's lines, the screen's: a BG's band closed further
     * than the game's, its sprites only in it) */
    float clipY0 = sObjClip.on && sObjClip.y0 > pic->y0 ? sObjClip.y0 : pic->y0;
    float clipY1 = sObjClip.on && sObjClip.y1 < pic->y1 ? sObjClip.y1 : pic->y1;

    static NitroGpuObj before[NITRO_GPU_OBJ_MAX], after[NITRO_GPU_OBJ_MAX], shown[2 * NITRO_GPU_OBJ_MAX];
    const NitroGpuObj *list = set->objs;
    int n = set->objCount, nb = -1, na = -1, k;

    if (scene != NULL) {
        uint32_t seqB, stampB;

        /* (halfway from the frame before: an in-between scene's, or in an
         * in-between frame without one the last scene shown's) */
        na = objs_of(scene->swap_seq, scene->stamp, after);
        if (na >= 0 && frame_before(scene, &seqB, &stampB)) {
            nb = objs_of(seqB, stampB, before);
        }
    }
    if (na >= 0) {
        n = NitroPort_GpuObjsBetween(before, nb, after, na, shown, 2 * NITRO_GPU_OBJ_MAX);
        list = shown;
    }
    make_texture(&sObjTex, set->objTex, GPU_RGBA5551, OBJ_TEX_W, OBJ_TEX_H, linear, GPU_CLAMP_TO_EDGE);
    C3D_TexBind(0, &sObjTex);
    quad_combiner(1);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ONE_MINUS_SRC_ALPHA, GPU_ONE, GPU_ONE_MINUS_SRC_ALPHA);
    for (k = 0; k < n; k++) {
        const NitroGpuObj *o = &list[k], *g = NULL;
        float x0, y0, x1, y1, u0, u1, vT, vB, t;
        int i;

        for (i = 0; i < set->objCount && g == NULL; i++) {
            if (set->objs[i].key == o->key) {
                g = &set->objs[i];
            }
        }
        if (g == NULL) {
            /* (its picture not in the picture's atlas: a frame of its
             * animation newer than the 2D drawn yet; the nearest of its size
             * there instead, its picture before: the HM cut-in's trainer went
             * out for a frame as it swung, 2026-10-31) */
            int bestD = 0;

            for (i = 0; i < set->objCount; i++) {
                const NitroGpuObj *c = &set->objs[i];
                int dx = c->x - o->x, dy = c->y - o->y, d;

                if (c->w != o->w || c->h != o->h || dx < -32 || dx > 32 || dy < -16 || dy > 16) {
                    continue;
                }
                d = dx * dx + dy * dy;
                if (g == NULL || d < bestD) {
                    g = c;
                    bestD = d;
                }
            }
        }
        if (g == NULL) {
            continue;
        }
        x0 = pic->x0 + (float)o->x * sx;
        y0 = pic->y0 + (float)o->y * sy;
        x1 = x0 + (float)o->w * sx;
        y1 = y0 + (float)o->h * sy;
        u0 = (float)(g->ax * 2) / (float)OBJ_TEX_W;
        u1 = (float)((g->ax + o->w) * 2) / (float)OBJ_TEX_W;
        vT = 1.0f - (float)(g->ay * 2) / (float)OBJ_TEX_H;
        vB = 1.0f - (float)((g->ay + o->h) * 2) / (float)OBJ_TEX_H;
        if (o->hflip) {
            t = u0, u0 = u1, u1 = t;
        }
        if (o->vflip != (sConfig.flipV != 0)) {
            t = vT, vT = vB, vB = t;
        }
        if (sConfig.flipV) {
            /* (the picture flipped as a whole: the sprite's place too) */
            t = y0;
            y0 = pic->y0 + pic->y1 - y1;
            y1 = pic->y0 + pic->y1 - t;
        }
        /* (cut at the picture's edges, its texture with it) */
        if (x0 < pic->x0) {
            u0 += (u1 - u0) * (pic->x0 - x0) / (x1 - x0);
            x0 = pic->x0;
        }
        if (x1 > pic->x1) {
            u1 -= (u1 - u0) * (x1 - pic->x1) / (x1 - x0);
            x1 = pic->x1;
        }
        if (y0 < clipY0) {
            vT += (vB - vT) * (clipY0 - y0) / (y1 - y0);
            y0 = clipY0;
        }
        if (y1 > clipY1) {
            vB -= (vB - vT) * (y1 - clipY1) / (y1 - y0);
            y1 = clipY1;
        }
        if (x0 < x1 && y0 < y1) {
            quad(x0, y0, x1, y1, 0.0f, u0, vT, u1, vB, 0xFFFFFFFFu);
        }
    }
    NitroPort_PicaProfileMark("the sprites");
}

/* An H-blank effect's 3D drawn from the texture (why 0) or a run at a
 * time, and why: in the log when it changes, SLICE_TOLD_MAX times at most
 * (a battle's start comes and goes: a line or two each). */
#define SLICE_TOLD_MAX 24
static void slices_why(int why, int runs)
{
    static const char *const text[] = {
        "from its texture",
        "a run of lines at a time: not the upper screen",
        "a run of lines at a time: what's behind the 3D isn't one colour (and the texture is RGB565, or a rear plane half shows it)",
        "a run of lines at a time: the 3D goes past the screen's edges",
        "a run of lines at a time: the 3D drawn in parts (the smooth 3D with outlines)",
        "a run of lines at a time: no texture for it (memory)",
        "a run of lines at a time: no room in the frame's quads or commands",
    };

    if (why != sSliceWhy && sSliceTold < SLICE_TOLD_MAX) {
        sSliceTold++;
        NitroPort_Log("[compose] an H-blank effect's 3D (%d runs of lines) drawn %s", runs, text[why]);
    }
    sSliceWhy = why;
}

/* An H-blank effect's lines as they're drawn: each line's 3D scroll (DS
 * pixels, the 3D moved left by it; finer than a pixel halfway between two
 * frames) and (ComposeLayers.gpuSpansOn) the part its windows show (lo to
 * hi, the DS's x), the backdrop on the rest. */
typedef struct SliceLines {
    float scroll[DS_H], lo[DS_H], hi[DS_H];
} SliceLines;
static SliceLines sLinesPicture, sLinesShown;   /* (draw_screen's: the picture's own, and as shown) */

/* The picture's own lines (its scrolls, its windows: all of each line
 * without them left out). */
static void lines_of_picture(const ComposeLayers *info, SliceLines *out)
{
    int y;

    for (y = 0; y < DS_H; y++) {
        out->scroll[y] = (float)scroll_signed(info->scroll3dLine[y] & 0x1FF);
        out->lo[y] = info->gpuSpansOn ? (float)info->spanLo[y] : 0.0f;
        out->hi[y] = info->gpuSpansOn ? (float)info->spanHi[y] : (float)DS_W;
    }
}

/* The lines an H-blank effect whose windows the compositor draws
 * (ComposeLayers.gpuSpansOn: a battle's start's slices) is drawn with:
 * those of the frame the DS shows the scene in, halfway from the frame
 * before's in an in-between frame (the game moves them 30 times a second:
 * at 60 the slices and their edges move each frame, as the 3D does);
 * the picture's own when they aren't known. Whether from the frames'. */
static int lines_shown(const ComposeLayers *info, const NitroGx3dScene *scene, SliceLines *out)
{
    static NitroLineSpans before, after;
    uint32_t seqB, stampB;

    if (!info->gpuSpansOn || !info->scroll3dLines || scene == NULL ||
        !lines_note_of(scene->swap_seq, scene->stamp, &after) || !after.on) {
        lines_of_picture(info, out);
        return 0;
    }
    NitroPort_LineSpansHalfway(frame_before(scene, &seqB, &stampB) && lines_note_of(seqB, stampB, &before) ? &before
                                                                                                        : NULL,
                               &after, out->scroll, out->lo, out->hi);
    return 1;
}

/* (draw_screen, 5d) An H-blank effect's windows left out of the picture
 * (ComposeLayers.gpuSpansOn): the backdrop over each line where they show
 * nothing, in front of the layers, on the lines its 3D is drawn with (the
 * slices': their edges move with them), lines of the same edge together.
 * The compositor's own view. */
static void draw_window_edges(const ComposeLayers *info, const SliceLines *sl, const PicaRect *pic, int top,
                              float scale, int canvasW)
{
    extern u32 gpuCmdBufSize, gpuCmdBufOffset;
    static const int corner[6][2] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 0 }, { 1, 1 }, { 0, 1 } };
    u32 rgba = rgba_of((uint16_t)info->backdrop, 255);
    float sx = (pic->x1 - pic->x0) / (float)canvasW;
    int side, y, end, k, vertices = 0;

    if (gpuCmdBufSize - gpuCmdBufOffset < 1024u) {
        return;
    }
    for (side = 0; side < 2; side++) {
        for (y = 0; y < DS_H; y = end) {
            /* (the edge's screen pixels: whole ones, as the slices move) */
            float e = side == 0 ? floorf(sl->lo[y] * sx + 0.5f) : floorf(sl->hi[y] * sx + 0.5f);
            float x0 = side == 0 ? pic->x0 : pic->x0 + e, x1 = side == 0 ? pic->x0 + e : pic->x1, y0, y1;

            if (sl->hi[y] <= sl->lo[y]) {
                x0 = pic->x0;   /* (nothing shown: the whole line, once) */
                x1 = side == 0 ? pic->x1 : pic->x0;
            }
            for (end = y + 1; end < DS_H; end++) {
                int emptyY = sl->hi[y] <= sl->lo[y], emptyE = sl->hi[end] <= sl->lo[end];

                if (emptyY != emptyE ||
                    (!emptyY && (side == 0 ? sl->lo[end] != sl->lo[y] : sl->hi[end] != sl->hi[y]))) {
                    break;
                }
            }
            if (x1 <= x0) {
                continue;
            }
            if (sQuadVertices + vertices + 6 > MAX_QUAD_VERTICES) {
                break;   /* (no room left in the frame's quads) */
            }
            /* (the DS's lines on the screen, as the slices have them) */
            y0 = y == 0 ? 0.0f : (float)(u32)((float)(y + top) * scale + 0.5f);
            y1 = end >= DS_H ? (float)SCREEN_H : (float)(u32)((float)(end + top) * scale + 0.5f);
            if (y1 > (float)SCREEN_H) {
                y1 = (float)SCREEN_H;
            }
            if (sConfig.flipV) {
                float t = y0;

                y0 = pic->y0 + pic->y1 - y1;
                y1 = pic->y0 + pic->y1 - t;
            }
            if (y1 <= y0) {
                continue;
            }
            for (k = 0; k < 6; k++) {
                NitroGx3dGpuVertex *v = &sQuads[sQuadVertices + vertices + k];

                v->position[0] = corner[k][0] ? x1 : x0;
                v->position[1] = corner[k][1] ? y1 : y0;
                v->position[2] = 0.0f;
                v->position[3] = 1.0f;
                v->color[0] = (uint8_t)(rgba >> 24);
                v->color[1] = (uint8_t)(rgba >> 16);
                v->color[2] = (uint8_t)(rgba >> 8);
                v->color[3] = (uint8_t)rgba;
                v->uv[0] = 0.0f;
                v->uv[1] = 0.0f;
            }
            vertices += 6;
        }
    }
    if (vertices > 0) {
        quad_combiner(0);
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
        NitroPort_PicaBindVertices(sQuads);
        C3D_DrawArrays(GPU_TRIANGLES, sQuadVertices, vertices);
        sQuadVertices += vertices;
    }
    NitroPort_PicaProfileMark("the H-blank effect's windows");
}

/* An H-blank effect's 3D (its lines scrolled apart: the grass's 96 slices
 * of 2 lines going apart at a battle's start, the water's waves), drawn
 * once into sSliceTex as the screen would have it unscrolled (on the
 * colour behind it, or alpha 0: its rear plane, its polygons), then each of
 * the DS's runs of lines from that at its own scroll: a textured quad a
 * run, all in one draw, then the tint over them. The scene drawn again for each run (the way below,
 * still for what this can't do) cost the GPU about 0.3 ms a run on top of
 * its pixels, so the runs were taken together, 24 at most (12 an eye): a
 * battle's start at 15 to 30 frames a second on a 3DS, with the slider up
 * fewer, its slices 10 or 18 lines high instead of 2 (2026-11-04). Only
 * where the texture can stand for the screen: what's behind the 3D all
 * one colour, or a rear plane that hides it, or (an RGBA8 texture, no
 * rear plane: the field's) the screen's own showing where the 3D drew
 * nothing (its translucent pixels over nothing then drawn as they are,
 * over the screen's black backdrop the same; over anything else darker
 * than the runs: rare); the 3D inside the screen. Drawn at the screen's own pixels (the smooth
 * 3D's 2x2 too: its texels doubled). Whether it was drawn so (else
 * nothing was). */
static int draw_slices_eye(const Screen *scr, const LayerSet *set, const SliceLines *sl, const PicaView *screenView,
                           const PicaRect *pic, const PicaRect *base, float sx3, int top, float scale,
                           C3D_FrameBuf *fb, const C3D_Mtx *pixels, uint16_t rearColor, unsigned rearAlpha)
{
    /* (the DS's runs of lines of the same scroll: sl's) */
    static struct {
        short y0, y1;
        float scroll;
    } lines[DS_H];
    extern u32 gpuCmdBufSize, gpuCmdBufOffset;
    const ComposeLayers *info = &set->info;
    C3D_FrameBuf *tfb = (C3D_FrameBuf *)sSliceFb;
    PicaView view = *screenView;
    float W = (float)scr->width;
    int n = 0, r, k, y, vertices = 0, dir, see;
    u32 fill;

    /* (behind the 3D, the picture all one colour where the 3D goes) */
    int plain = set->underColor >= 0 && base->x0 >= pic->x0 - 0.5f && base->x1 <= pic->x1 + 0.5f &&
                ((pic->x0 <= 0.5f && pic->x1 >= W - 0.5f) ||
                 sToRgb565[info->backdrop & 0x7FFF] == (uint16_t)set->underColor);

    for (y = 0; y < DS_H; y++) {
        if (n > 0 && lines[n - 1].scroll == sl->scroll[y]) {
            lines[n - 1].y1 = (short)(y + 1);
        } else {
            lines[n].y0 = (short)y;
            lines[n].y1 = (short)(y + 1);
            lines[n].scroll = sl->scroll[y];
            n++;
        }
    }
    if (n < 2) {
        return 0;   /* (no runs: not an effect) */
    }
    if (scr != &sScreens[0] || scr->width > SLICE_TEX_H) {
        slices_why(1, n);
        return 0;
    }
    if (base->x0 < -0.5f || base->x1 > W + 0.5f) {
        slices_why(3, n);
        return 0;
    }
    if (view.part != PICA_DRAW_ALL) {
        slices_why(4, n);
        return 0;
    }
    if (!slices_ready()) {
        slices_why(5, n);
        return 0;
    }
    /* (what's behind the 3D: hidden by its rear plane, or all one colour,
     * the texture filled with it; or, the texture RGBA8 and no rear plane,
     * the screen's own where the 3D drew nothing: the texture's alpha 0
     * there, its pixels left out) */
    see = rearAlpha < 31 && !plain;
    if (see && !(sSliceRgba && rearAlpha == 0)) {
        slices_why(2, n);
        return 0;
    }
    if (sSliceVertices + 6 * n > SLICE_VERTICES || sQuadVertices + 6 * 2 > MAX_QUAD_VERTICES ||
        gpuCmdBufSize - gpuCmdBufOffset < 4096u) {
        slices_why(6, n);
        return 0;
    }
    slices_why(0, n);
    /* The runs' quads: run r's rows (the DS's lines on the screen, as the
     * runs below have them), its place moved by its scroll; each pixel
     * from the texture where it was unscrolled. Screen pixel (x, y) is the
     * framebuffer's (240 - 1 - y, width - 1 - x), the texture's texel too
     * (its row t at v = (t + 0.5) / 512: see draw_screen's picture). */
    for (r = 0; r < n; r++) {
        static const int corner[6][2] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 0 }, { 1, 1 }, { 0, 1 } };
        /* (moved by whole pixels: each pixel's middle a texel's, never
         * between two, where the GPU's nearest could take either) */
        float dx = floorf(-lines[r].scroll * sx3 + 0.5f);
        float x0 = base->x0 + dx, x1 = base->x1 + dx, y0, y1;

        y0 = lines[r].y0 == 0 ? 0.0f : (float)(u32)((float)(lines[r].y0 + top) * scale + 0.5f);
        y1 = lines[r].y1 >= DS_H ? (float)SCREEN_H : (float)(u32)((float)(lines[r].y1 + top) * scale + 0.5f);
        if (x0 < 0.0f) {
            x0 = 0.0f;
        }
        if (x1 > W) {
            x1 = W;
        }
        if (y1 > (float)SCREEN_H) {
            y1 = (float)SCREEN_H;
        }
        if (x1 <= x0 || y1 <= y0) {
            continue;
        }
        for (k = 0; k < 6; k++) {
            NitroGx3dGpuVertex *v = &sSliceVerts[sSliceVertices + vertices + k];
            float x = corner[k][0] ? x1 : x0, y = corner[k][1] ? y1 : y0;

            v->position[0] = x;
            v->position[1] = y;
            v->position[2] = 0.0f;
            v->position[3] = 1.0f;
            v->color[0] = v->color[1] = v->color[2] = v->color[3] = 255;
            v->uv[0] = ((float)SCREEN_H - y) / (float)SLICE_TEX_W;
            v->uv[1] = (W - (x - dx)) / (float)SLICE_TEX_H;
        }
        vertices += 6;
    }
    /* 1. The 3D into the texture, unscrolled: the texture's depth/stencil
     * and colour filled first (the colour behind the 3D, or alpha 0), a
     * fill queued after what's drawn so far (the cut). */
    fill = see ? 0u : !plain ? (sSliceRgba ? 0x000000FFu : 0u)
                            : sSliceRgba ? rgba_of_565(set->underColor) : (u32)set->underColor & 0xFFFFu;
    NitroPort_PicaCut("the 2D behind");
    C3D_FrameBufClear(tfb, C3D_CLEAR_ALL, fill, 0xFFFFFFFFu);
    C3D_SetFrameBuf(tfb);
    C3D_SetViewport(0, 0, SCREEN_H, (u32)scr->width);
    NitroPort_PicaSetView(pixels, 0.0f, 1.0f);
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    C3D_StencilTest(false, GPU_ALWAYS, 0, 0xFF, 0xFF);
    C3D_AlphaTest(false, GPU_ALWAYS, 0);
    if (rearAlpha) {
        /* (2. as the runs below have it: the clear colour, blended by its
         * alpha) */
        quad_combiner(0);
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA, GPU_ONE, GPU_ZERO);
        quad(base->x0, base->y0, base->x1, base->y1, -1.0f, 0, 0, 0, 0, rgba_of(rearColor, rearAlpha * 255u / 31u));
    }
    view.fb = tfb;
    for (dir = 0; dir < 4; dir++) {
        view.edgeFb[dir] = NULL;
    }
    view.viewWidth = SCREEN_H;
    view.viewHeight = scr->width;
    view.rect = *base;
    view.again = 0;
    view.scissor = 0;
    view.band = 0;
    {
        u32 before = gpuCmdBufSize - gpuCmdBufOffset;

        NitroPort_PicaDrawScene(&view);
        /* (the whole scene's, as the first run's below) */
        if (before > gpuCmdBufSize - gpuCmdBufOffset) {
            sSceneWords = before - (gpuCmdBufSize - gpuCmdBufOffset);
        }
    }
    NitroPort_PicaProfileMark("the 3D into the slices' texture");
    /* 2. The runs from it onto the screen (switching back writes the
     * texture's pixels out; binding it clears the GPU's texture cache):
     * where the 3D drew nothing, with the screen's own behind it, those
     * pixels left out (alpha 0). Then the 3D's tint over each run's place,
     * as the runs below have it (the same quads, one colour). */
    C3D_SetFrameBuf(fb);
    C3D_SetViewport(0, 0, fb->width, fb->height);
    NitroPort_PicaSetView(pixels, 0.0f, 1.0f);
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    C3D_StencilTest(false, GPU_ALWAYS, 0, 0xFF, 0xFF);
    if (vertices > 0) {
        C3D_TexBind(0, &sSliceTex);
        quad_combiner(1);
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
        C3D_AlphaTest(see != 0, GPU_GREATER, 0);
        NitroPort_PicaBindVertices(sSliceVerts);
        C3D_DrawArrays(GPU_TRIANGLES, sSliceVertices, vertices);
        C3D_AlphaTest(false, GPU_ALWAYS, 0);
        C3D_TexBind(0, NULL);   /* (not left on while the next eye draws into it) */
        if (info->tint3d == 2 || info->tint3d == 3) {
            u32 a = (u32)(info->evy > 16 ? 16 : info->evy) * 255u / 16u;
            C3D_TexEnv *env = C3D_GetTexEnv(0);

            /* (tint_3d's colour; the constant's 0xAABBGGRR) */
            C3D_TexEnvInit(env);
            C3D_TexEnvColor(env, (a << 24) | (info->tint3d == 2 ? 0x00FFFFFFu : 0u));
            C3D_TexEnvSrc(env, C3D_Both, GPU_CONSTANT, 0, 0);
            C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
            C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA, GPU_ONE, GPU_ZERO);
            C3D_DrawArrays(GPU_TRIANGLES, sSliceVertices, vertices);
            quad_combiner(0);
        }
        sSliceVertices += vertices;
    }
    if (n > sFrameLineRuns) {
        sFrameLineRuns = n;
    }
    NitroPort_PicaProfileMark("the 3D's slices");
    return 1;
}

/* (draw_slices_eye, the right eye the left's way: both from the texture,
 * or both a run at a time) */
static int draw_slices(const Screen *scr, int eye, const LayerSet *set, const SliceLines *sl, const PicaView *screenView,
                       const PicaRect *pic, const PicaRect *base, float sx3, int top, float scale, C3D_FrameBuf *fb,
                       const C3D_Mtx *pixels, uint16_t rearColor, unsigned rearAlpha)
{
    int drawn;

    if (eye == 1 && sSliceLeft == 0) {
        return 0;
    }
    drawn = draw_slices_eye(scr, set, sl, screenView, pic, base, sx3, top, scale, fb, pixels, rearColor, rearAlpha);
    if (eye == 0) {
        sSliceLeft = drawn;
    }
    return drawn;
}

/* (draw_screen's step 5b) Engine A's sprites the compositor draws
 * (ComposeLayers.gpuObjCount), when there are: cut to a BG's band where
 * one's closing (its lines bandY to bandR - 1, of rows). */
static void draw_gpu_objs_step(const ComposeLayers *info, const LayerSet *set, const NitroGx3dScene *scene,
                               const PicaRect *pic, float scale, int rows, int bandY, int bandR, int linear)
{
    if (info->gpuObjCount >= 0 && set->objTex && set->objCount > 0) {
        /* (a BG's band closing, or closed further than the game has it
         * (bg_band_rows: going on closing as the game holds it): the
         * sprites in it only, the cut-in's wind and Pokemon closed away
         * with it; over the field the dots and the Pokemon's parts
         * showed for a few frames after it closed, 2026-11-01) */
        sObjClip.on = info->gpuCircle == 3 &&
                      (sBgBand.vTop > 0.0f || sBgBand.vBottom > 0.0f || sBgBand.y > (float)sBgBand.wantY + 0.5f ||
                       sBgBand.r < (float)sBgBand.wantR - 0.5f);
        if (sObjClip.on) {
            sObjClip.y0 = pic->y0 + (float)(sConfig.flipV ? rows - bandR : bandY) * scale;
            sObjClip.y1 = pic->y0 + (float)(sConfig.flipV ? rows - bandY : bandR) * scale;
        }
        if (!sObjClip.on || sObjClip.y1 > sObjClip.y0) {
            draw_gpu_objs(set, scene, pic, scale, (pic->y1 - pic->y0) / (float)rows, linear);
        }
        sObjClip.on = 0;
    }
}

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
    /* (the field at the DS's pixel size: a pixel a pixel, its rows all
     * drawn from, the DS's line 0 at row top) */
    float scale = PIC_SCALE(info);
    int rows = info != NULL ? OVER_ROWS(info) : DS_H, top = info != NULL && scale == 1.0f ? info->top : 0;

    sProfGroup = scr == &sScreens[0] ? eye : 2;

    /* The picture, 1.25 times the DS's size, in the middle (it fills the
     * screen: 320 DS pixels on the upper one, 256 on the lower); the field
     * at the DS's pixel size, a pixel a pixel (its 400 x 240 fill it). */
    pic.x0 = ((float)scr->width - canvasW * scale) * 0.5f;
    pic.x1 = pic.x0 + canvasW * scale;
    pic.y0 = 0.0f;
    pic.y1 = SCREEN_H;
    if (info != NULL && info->stretch) {
        /* (a battle's start on the wider field: the DS's picture over the
         * whole screen) */
        pic.x0 = 0.0f;
        pic.x1 = (float)scr->width;
    }
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
    u1 = (float)(canvasW * set->texScale) / (float)scr->texW;
    vTop = 1.0f;
    vBottom = 1.0f - (float)(rows * set->texScale) / (float)scr->texH;
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
    if (scr == &sScreens[0]) {
        sSignShown = -1;   /* (the sign's BG, drawn below: step 5a) */
    }
    if (has3d) {
        uint16_t rearColor;
        unsigned rearAlpha;
        PicaView view;
        PicaRect base;
        LineRun runs[LINE_RUNS_MAX];
        int nruns = 1, r, sliced = 0;

        /* Where the 3D goes: the DS's screen in the picture, or (a scene
         * made for the wider view) the whole width; moved left by BG0HOFS
         * (the game's screen shakes). */
        /* (a scene's own size: its view's, a pixel a DS pixel at the
         * screen's height: 1.25 for the DS's 192 lines, 1 for the field's
         * 240 at the DS's pixel size) */
        float s3 = (float)SCREEN_H / (float)(scene->screen_height > DS_H ? scene->screen_height : DS_H);

        if (info->stretch) {
            /* (a battle's start on the wider field: the whole screen, as
             * the stretched picture over it) */
            base.x0 = 0.0f;
            base.x1 = (float)scr->width;
        } else if (scene->screen_width > DS_W) {
            base.x0 = ((float)scr->width - (float)scene->screen_width * s3) * 0.5f;
            base.x1 = base.x0 + (float)scene->screen_width * s3;
        } else {
            base.x0 = pic.x0 + (canvasW - DS_W) * 0.5f * s3;
            base.x1 = base.x0 + DS_W * s3;
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
        view.viewWidth = 0;
        view.viewHeight = 0;
        /* (the runs from the 3D drawn once, when they can be: draw_slices;
         * else each run's own, below. With the windows left out of the
         * picture, the lines of the frame shown, halfway in an in-between
         * frame: lines_shown; the right eye the left's) */
        if (info->scroll3dLines && eye == 0) {
            lines_of_picture(info, &sLinesPicture);
            (void)lines_shown(info, scene, &sLinesShown);
        }
        sliced = info->scroll3dLines && !edges &&
                 draw_slices(scr, eye, set, &sLinesShown, &view, &pic, &base,
                             info->stretch ? (float)scr->width / (float)DS_W : s3, top, scale, fb, &pixels, rearColor,
                             rearAlpha);
        if (!sliced) {
            NitroPort_PicaProfileMark("the 2D behind");
        }
        for (r = 0; !sliced && r < nruns; r++) {
            PicaRect rows;   /* (the run's place and rows: its rear plane's, its tint's) */

            /* (a stretched picture's scrolls stretched with it) */
            float sx3 = info->stretch ? (float)scr->width / (float)DS_W : s3;

            rect3d.x0 = base.x0 - (float)runs[r].scroll * sx3;
            rect3d.x1 = base.x1 - (float)runs[r].scroll * sx3;
            rows = rect3d;
            if (nruns > 1) {
                /* the run's rows (screen pixel (x, y) is the framebuffer's
                 * (height - 1 - y, width - 1 - x), in its own pixels: the
                 * smooth 3D's twice the screen's), for everything of the
                 * run: its rear plane, its polygons (each pass sets it
                 * again: see begin_pass), its tint */
                u32 sy = (u32)fb->width / SCREEN_H, sx = (u32)fb->height / (u32)scr->width;
                /* (the DS's lines on the screen: a tall field's from its
                 * row top, the first run from the screen's top, the last
                 * to its bottom) */
                u32 y0 = runs[r].y0 == 0 ? 0u : (u32)((float)(runs[r].y0 + top) * scale + 0.5f);
                u32 y1 = runs[r].y1 >= DS_H ? (u32)SCREEN_H : (u32)((float)(runs[r].y1 + top) * scale + 0.5f);

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
                /* (the whole scene's: a run draws only its rows' triangles) */
                if (!view.again && !view.band && view.part == PICA_DRAW_ALL &&
                    before > gpuCmdBufSize - gpuCmdBufOffset) {
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
        if (nruns == 1 && !sliced) {
            tint_3d(info, &rect3d);   /* (draw_slices': over its quads) */
        }
        /* 4b. A BG drawn here, in front of the 3D and behind the rest (the
         * field's weather: ComposeLayers.gpuBg), from its picture, repeated
         * and scrolled as the 3D scene had it, blended onto the 3D as the
         * DS does: its colour x eva / 16 + the 3D's x evb / 16 where it isn't
         * clear (the texture's colours are 0 there: what's under stays). */
        if (info->gpuBg >= 0 && set->bgTex && set->bgValid && set->bgKey == info->gpuBgKey) {
            float hofs = scene->bg_ofs[info->gpuBg * 2], vofs = scene->bg_ofs[info->gpuBg * 2 + 1];
            float now[8], before[8];

            /* (the scrolls of the frame the DS shows the scene in; an
             * in-between scene's halfway from the scene before's; the
             * scene's own, a frame behind, when they aren't known) */
            if (bg_offsets_of(scene->swap_seq, scene->stamp, now, NULL, NULL)) {
                hofs = now[info->gpuBg * 2];
                vofs = now[info->gpuBg * 2 + 1];
                if ((scene->stamp_before != scene->stamp || scene->swap_seq_before != scene->swap_seq) &&
                    bg_offsets_of(scene->swap_seq_before, scene->stamp_before, before, NULL, NULL)) {
                    hofs = bg_offset_halfway(before[info->gpuBg * 2], hofs);
                    vofs = bg_offset_halfway(before[info->gpuBg * 2 + 1], vofs);
                }
            }
            int origin = (canvasW - DS_W) / 2;
            float bu0 = (hofs - (float)origin) / 256.0f, bu1 = bu0 + (float)canvasW / 256.0f;
            float bvTop = 1.0f - (vofs - (float)top) / 256.0f, bvBottom = bvTop - (float)rows / 256.0f;
            u32 a = (u32)(info->gpuBgEva * 255 / 16), b = (u32)((16 - info->gpuBgEvb) * 255 / 16);

            if (sConfig.flipV) {
                float t = bvTop;

                bvTop = bvBottom;
                bvBottom = t;
            }
            make_texture(&sBgTex, set->bgTex, GPU_RGBA5551, BG_TEX_W, BG_TEX_W, linear, GPU_REPEAT);
            C3D_TexBind(0, &sBgTex);
            quad_combiner_modulate();
            C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ONE_MINUS_SRC_ALPHA,
                           GPU_ONE, GPU_ONE_MINUS_SRC_ALPHA);
            quad(pic.x0, pic.y0, pic.x1, pic.y1, 0.0f, bu0, bvTop, bu1, bvBottom, (a << 24) | (a << 16) | (a << 8) | b);
            NitroPort_PicaProfileMark("the weather's BG");
        }
        /* 5. The layers in front, with their alpha (multiplied in). One BG
         * in a band of lines (ComposeLayers.gpuCircle 3): those layers are
         * that BG alone, shown in the band's lines over the backdrop. */
        int bandY = 0, bandR = rows, bandL = 0, bandRt = canvasW;
        float bx0 = pic.x0, bx1 = pic.x1, bu0 = 0.0f, bu1 = u1;

        if (info->gpuCircle == 3) {
            bg_band_rows(info, scene, &bandY, &bandR, &bandL, &bandRt);
            bandY = bandY < 0 ? 0 : bandY > rows ? rows : bandY;
            bandR = bandR < bandY ? bandY : bandR > rows ? rows : bandR;
            bandL = bandL < 0 ? 0 : bandL > canvasW ? canvasW : bandL;
            bandRt = bandRt < bandL ? bandL : bandRt > canvasW ? canvasW : bandRt;
            /* (its columns on the screen, and in the picture's texture) */
            bx0 = pic.x0 + (pic.x1 - pic.x0) * (float)bandL / (float)canvasW;
            bx1 = pic.x0 + (pic.x1 - pic.x0) * (float)bandRt / (float)canvasW;
            bu0 = u1 * (float)bandL / (float)canvasW;
            bu1 = u1 * (float)bandRt / (float)canvasW;
            if (bandR > bandY && bandRt > bandL && sQuadVertices + 6 <= MAX_QUAD_VERTICES) {
                float by0 = pic.y0 + (float)(sConfig.flipV ? rows - bandR : bandY) * scale;
                float by1 = pic.y0 + (float)(sConfig.flipV ? rows - bandY : bandR) * scale;

                quad_combiner(0);
                C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
                quad(bx0, by0, bx1, by1, 0.0f, 0, 0, 0, 0, rgba_of((uint16_t)info->gpuCircleColor, 255));
            }
        }
        if (!set->overEmpty && set->overTop < set->overBottom && bandR > bandY && bandRt > bandL) {
            /* (only the rows with anything in them; all of them flipped) */
            int first = sConfig.flipV ? 0 : set->overTop, bottom = sConfig.flipV ? rows : set->overBottom;

            if (info->gpuCircle == 3) {
                first = sConfig.flipV ? rows - bandR : (first > bandY ? first : bandY);
                bottom = sConfig.flipV ? rows - bandY : (bottom < bandR ? bottom : bandR);
            }
            float y0 = pic.y0 + (float)first * scale, y1 = pic.y0 + (float)bottom * scale;
            float v0 = vTop + (vBottom - vTop) * (float)first / (float)rows;
            float v1 = vTop + (vBottom - vTop) * (float)bottom / (float)rows;

            make_texture(&sOverTex, set->over, set->pub.over16 ? GPU_RGBA5551 : GPU_RGBA8, scr->texW, scr->texH, linear,
                         GPU_CLAMP_TO_EDGE);
            C3D_TexBind(0, &sOverTex);
            quad_combiner(1);
            C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ONE_MINUS_SRC_ALPHA,
                           GPU_ONE, GPU_ONE_MINUS_SRC_ALPHA);
            if (bottom > first) {
                quad(bx0, y0, bx1, y1, 0.0f, bu0, v0, bu1, v1, 0xFFFFFFFFu);
            }
        }
        /* 5d. An H-blank effect's windows (ComposeLayers.gpuSpansOn: a
         * battle's start's slices): the backdrop where they show nothing,
         * on the lines the 3D was drawn with (from its texture: the frame
         * shown's, halfway in an in-between frame; a run at a time: the
         * picture's). */
        if (info->gpuSpansOn && info->scroll3dLines) {
            draw_window_edges(info, sliced ? &sLinesShown : &sLinesPicture, &pic, top, scale, canvasW);
        }
        /* (a weather's sprites behind the place name sign: drawn under it,
         * as the DS has them; 5b) */
        if (info->gpuObjsUnderSign) {
            draw_gpu_objs_step(info, set, scene, &pic, scale, rows, bandY, bandR, linear);
        }
        /* 5a. A band of a BG drawn here, in front of the rest (the place
         * name sign: ComposeLayers.gpuSign), moved up by the BG's vertical
         * scroll in the frame the DS shows the scene in, halfway from the
         * frame before's in an in-between frame (the sign slides 4 lines a
         * game step: 2 a frame at 60 frames a second, as the picture can't
         * be drawn again at that rate: drawn whole for every step, it was
         * 30 a second at most and the in-between scenes gave way). */
        if (scr == &sScreens[0]) {
            sSignShown = info->gpuSign;
        }
        /* (with a band of columns too, the gate's wipe, drawn over it below:
         * 5c; not a band of lines, a building's door) */
        if (info->gpuSign > 0 && (info->gpuCircle == 0 || (info->gpuCircle == 2 && info->gpuCircleX != 0)) &&
            !info->stretch && set->signTex &&
            set->signValid &&
            set->signKey == info->gpuSignKey && sQuadVertices + 6 <= MAX_QUAD_VERTICES) {
            float v = (float)info->gpuSignVofs, now[8], before[8];
            uint32_t seqB, stampB, content = 0;
            int sign = -1, up = 1;

            /* (only while the frame shown has this sign up, the same: the
             * game puts it up scrolled 38 (the frame before, none) and takes
             * it away cleared, scrolled 0; the picture isn't drawn again as
             * it slides, so it may be of a frame before or after the one
             * shown) */
            if (bg_offsets_of(scene->swap_seq, scene->stamp, now, &sign, &content)) {
                up = sign == info->gpuSign && content == info->gpuSignContent;
                v = now[info->gpuSign * 2 + 1];
                if (frame_before(scene, &seqB, &stampB) && bg_offsets_of(seqB, stampB, before, &sign, &content) &&
                    sign == info->gpuSign && content == info->gpuSignContent) {
                    v = bg_offset_halfway(before[info->gpuSign * 2 + 1], v);
                }
            }
            /* (the scroll as the map's 256 lines go round: -128 to 127) */
            v = fmodf(v, 256.0f);
            if (v < 0.0f) {
                v += 256.0f;
            }
            if (v >= 128.0f) {
                v -= 256.0f;
            }
            if (up) {
                float sy0 = (float)info->gpuSignY - v, sy1 = sy0 + (float)info->gpuSignRows;
                float c0 = sy0 < 0.0f ? 0.0f : sy0, c1 = sy1 > (float)rows ? (float)rows : sy1;

                if (c1 > c0) {
                    /* (its texture's rows: a texel a line, row 0 at v = 1) */
                    float t0 = 1.0f - (c0 - sy0) / (float)SIGN_TEX_H;
                    float t1 = 1.0f - (c1 - sy0) / (float)SIGN_TEX_H;
                    float y0 = pic.y0 + (sConfig.flipV ? (float)rows - c1 : c0) * scale;
                    float y1 = pic.y0 + (sConfig.flipV ? (float)rows - c0 : c1) * scale;

                    if (sConfig.flipV) {
                        float t = t0;

                        t0 = t1;
                        t1 = t;
                    }
                    /* (a texel a pixel: as it is, unless the picture's scaled) */
                    make_texture(&sSignTex, set->signTex, GPU_RGBA5551, SIGN_TEX_W, SIGN_TEX_H,
                                 scale != 1.0f && linear, GPU_CLAMP_TO_EDGE);
                    C3D_TexBind(0, &sSignTex);
                    quad_combiner(1);
                    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ONE_MINUS_SRC_ALPHA, GPU_ONE,
                                   GPU_ONE_MINUS_SRC_ALPHA);
                    quad(pic.x0, y0, pic.x1, y1, 0.0f, 0.0f, t0, (float)canvasW / (float)SIGN_TEX_W, t1, 0xFFFFFFFFu);
                    NitroPort_PicaProfileMark("the place name sign");
                }
            }
        }
        /* 5b. Engine A's sprites drawn here (ComposeLayers.gpuObjCount: the
         * HM cut-in's), in front of it all; a weather's behind the sign
         * (gpuObjsUnderSign) before it, above. */
        if (!info->gpuObjsUnderSign) {
            draw_gpu_objs_step(info, set, scene, &pic, scale, rows, bandY, bandR, linear);
        }
    }
    /* 5c. A circle fade drawn here (ComposeLayers.gpuCircle). */
    if (info->gpuCircle == 1 || info->gpuCircle == 2) {
        draw_circle_mask(info, has3d ? scene : NULL, &pic, scale, (pic.y1 - pic.y0) / (float)rows);
    } else if (has3d && sBand.on) {
        draw_band_tail(&pic, (pic.y1 - pic.y0) / (float)rows);
    }
    if (info->gpuCircle == 0 && has3d) {
        draw_columns_tail(scene, &pic, scale, (pic.y1 - pic.y0) / (float)rows);
    }
    /* 6. The master brightness (with the 3D: engine A's, by frame). */
    master_brightness_of(scr, info, has3d && scr == &sScreens[0] ? scene : NULL);
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
 * is a frame behind its 2D): stamp <= the scene's + 1. With an in-between
 * scene (mid), halfway between the scene before and its own: no newer
 * than its own (stamp <= the scene's), the 2D the DS showed then, there
 * already (the scene before's). Held for the frame (the drawers won't
 * write it). Under sSetLock. */
static LayerSet *pick_set(Screen *scr, const NitroGx3dScene *scene, int syncTo3d, int mid)
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
            (int32_t)(c->info.stamp - (scene->stamp + (mid ? 0u : 1u))) > 0) {
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

    sEffectsMid = 0;
    sComposedFrames++;
    if (!sReady) {
        return 0;
    }
    /* An in-between frame for the 2D's fades alone (mid 2: no in-between
     * scene, the scene's own 3D): once the scene's frame is noted, and
     * only if a fade, circle or band changed since the last scene shown;
     * else nothing (as late: its scene next). */
    if (mid == 2) {
        if (scene == NULL) {
            return COMPOSE_MID_LATE;
        }
        if (frame_note_due(scene->swap_seq)) {
            sSceneWaiting = 1;
            sSceneWaitingSerial = sceneSerial;
            return 0;
        }
        if (!fades_changed(scene)) {
            return COMPOSE_MID_LATE;
        }
        sEffectsMid = 1;
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
        LayerSet *set = pick_set(scr, scene, screen3d, mid);
        int has3d, newFrame3d;

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
            panelUp = pick_set(&sScreens[0], NULL, 0, 0);
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
        /* (a new 3D frame: a new scene, or the scene on the screen standing
         * for a later frame, a battle's start's scenes left out as the same:
         * main_3ds.c) */
        newFrame3d = sceneSerial != scr->shownScene || (scene != NULL && scene->swap_seq != scr->shownSwap3d);
        need[s] = forced || set->seq != scr->shownSeq || (has3d && newFrame3d) || (mid && has3d) ||
                  menu[s] != scr->shownMenu || (menu[s] && menuSerial != scr->shownMenuSerial) ||
                  (menu[s] && sMenuFade != scr->shownFade) ||
                  (menu[s] && sMenuPanel == NITRO_LOWERMENU_PANEL_MAP && mapSerial != scr->shownMapSerial) ||
                  (menu[s] && sMenuPanel != NITRO_LOWERMENU_PANEL_MAP &&
                   (runSerial != scr->shownRunSerial || (panelUp != NULL ? panelUp->seq : 0u) != scr->shownUpSeq)) ||
                  (s == 0 && has3d && (stereoNow != scr->shownStereo ||
                                        (stereoNow && slider != scr->shownSlider))) ||
                  (has3d && !mid && !newFrame3d && scr->npending == 0 && band_moving());
        /* (the last: a band opening, the scene shown already and nothing
         * of the screen's waiting for its V-blank, drawn again for the next
         * V-blank, its band on; queued after a frame still to show, it
         * would have shown before it, the scene early) */
        /* A new scene: once the 2D it's shown with is here (or the next
         * scene has come: flush). Not its in-between scene, which has its
         * 2D (pick_set): waiting for the scene's, the in-between scene
         * went out with it or not at all, and while the 2D changed (a
         * sign sliding in, a window) the 3D stepped at 30 frames a second
         * by turns, an in-between frame left out for each new picture, 5
         * to 15 a second (a log of 2026-10-30: the people in Eterna
         * seemed to shake as the player walked). */
        if (need[s] && has3d && !forced && !flush && !mid && newFrame3d && !paired_2d_ready(scene->stamp)) {
            need[s] = 0;
            sSceneWaiting = 1;
            sSceneWaitingSerial = sceneSerial;
        }
        /* A scene or its in-between scene: once the game has noted the
         * frame the DS shows the scene in (frame_note_due), its scrolls,
         * sprites, circle and brightness. */
        if (need[s] && has3d && !forced && !flush && scene != NULL && newFrame3d && frame_note_due(scene->swap_seq)) {
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
        if (has3d && newFrame3d) {
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
    /* (when it will show, as far as known now: band_ease's time) */
    {
        u32 soonest = startVBlank + 1u, at = soonest;

        if (timed && scene != NULL) {
            at = scene->stamp + (u32)sLag + (mid ? 0u : 1u);
        }
        sDrawTarget = (int32_t)(at - soonest) < 0 ? soonest : at;
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
    if (sSliceVertsAll) {
        sSliceVerts = sSliceVertsAll + (sQuadHalf ? SLICE_VERTICES : 0);
    }
    sSliceVertices = 0;
    sSliceLeft = -1;
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
            scr->shownSwap3d = scene != NULL ? scene->swap_seq : 0u;
            sSceneWaiting = 0;
            if (scene != NULL) {
                sLastMainSeq = scene->swap_seq;
                sLastMainStamp = scene->stamp;
                sLastMainValid = 1;
            }
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
    if (sSliceVertices > 0) {
        GSPGPU_FlushDataCache(sSliceVerts, (u32)(sSliceVertices * sizeof(NitroGx3dGpuVertex)));
    }
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
                                  (unsigned long)((int32_t)(sSeenAt - scene->stamp) > 0 ? sSeenAt - scene->stamp : 0u),
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
