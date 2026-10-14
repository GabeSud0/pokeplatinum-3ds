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
 * made it (sLag, worked out from how long the scenes take): the 3D comes
 * at the game's own even pace (every second V-blank in the field), not
 * whenever a scene happens to be done, and the 2D in step with it.
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

/* RGBA8 render targets to RGB565 framebuffers. */
#define SCREEN_TRANSFER_FLAGS (GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | \
     GX_TRANSFER_RAW_COPY(0) | GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | \
     GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB565) | GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

/* A published picture: its textures and what it is. */
typedef struct LayerSet {
    void *under;          /* RGB565 texels, tiled */
    void *over;           /* RGBA8 texels, tiled (alpha multiplied in) */
    ComposeLayers info;   /* has3d etc., as published (no picture pointers) */
    uint32_t seq;         /* publish order (0: none, or being written) */
    int writing, held;
} LayerSet;

typedef struct Screen {
    C3D_RenderTarget *target[2];   /* left (and right) eye */
    C3D_FrameBuf edgeFb[2];        /* their colour with the edge marking depth/stencil */
    int width;                     /* 400 or 320 */
    LayerSet set[SETS];
    int texW, texH;                /* the layer textures (power-of-two) */
    int texScale;                  /* texels a DS pixel: 2 (sharp, scale2x) or 1 */
    /* What's shown: to see whether anything changed. */
    uint32_t shownSeq, shownScene;
    float shownSlider;
    int shownStereo;
    u32 swapVBlank;                /* the V-blank count when it was last swapped */
    int swapped;
} Screen;

static ComposeConfig sConfig;
static Screen sScreens[2];        /* 0: upper, 1: lower */
static void *sEdgeDepth;          /* 240 x 400 D24S8, shared by the screens */
static LightLock sSetLock;
static uint32_t sSeq;
static ComposeLayers sWork[2];    /* the drawers' pictures, by engine */
static uint16_t sWorkUnder[2][CANVAS_MAX * DS_H], sWorkOver[2][CANVAS_MAX * DS_H];
static uint8_t sWorkAlpha[2][CANVAS_MAX * DS_H];
static uint16_t sToRgb565[32768];
static uint32_t sConverted[2][CANVAS_MAX * DS_H];
static uint16_t sConverted16[2][CANVAS_MAX * DS_H];
static NitroGx3dGpuVertex *sQuads;   /* this frame's quads */
static int sQuadVertices;
#define MAX_QUAD_VERTICES (6 * 32)
static int sInFrame;
static int sReady;
static volatile u32 sVBlanks;
static u32 sFrames, sLate;
static u64 sGpuTicks;
static volatile u32 sPublished;
static volatile u64 sPublishTicks;
static uint32_t sPreparedScene;
static int sScenePrepared;
static float sSceneConvergence = 1.0f;
/* V-blanks from the game making a scene to its frame showing (adapts:
 * up when a frame is late, back down after a long while of slack). */
static int sLag = 3, sOnTime;

void NitroPort_ComposeVBlank(void)
{
    __atomic_fetch_add(&sVBlanks, 1u, __ATOMIC_RELAXED);
}

u32 NitroPort_ComposeVBlanks(void)
{
    return __atomic_load_n(&sVBlanks, __ATOMIC_RELAXED);
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
    int s, i;

    for (s = 0; s < 2; s++) {
        for (i = 0; i < 2; i++) {
            if (sScreens[s].target[i]) {
                C3D_RenderTargetDelete(sScreens[s].target[i]);
                sScreens[s].target[i] = NULL;
            }
        }
        free_sets(&sScreens[s]);
    }
    if (sEdgeDepth) {
        vramFree(sEdgeDepth);
        sEdgeDepth = NULL;
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
    }
    return 1;
}

int NitroPort_ComposeInit(const ComposeConfig *config)
{
    static const char *const filters[] = { "sharp (the pixels as they are, 2x then bilinear)", "nearest",
                                           "smooth (bilinear)", "Scale2x" };
    int s, i, c, e;

    sConfig = *config;
    memset(sScreens, 0, sizeof(sScreens));
    LightLock_Init(&sSetLock);
    for (c = 0; c < 32768; c++) {
        int r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;

        sToRgb565[c] = (u16)((r << 11) | (g << 6) | (g >> 4) | b);
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
        for (i = 0; i < (s == 0 && config->stereo ? 2 : 1); i++) {
            scr->target[i] = C3D_RenderTargetCreate(SCREEN_H, scr->width, GPU_RB_RGBA8,
                                                    GPU_RB_DEPTH24_STENCIL8);
            if (!scr->target[i]) {
                NitroPort_Log("[compose] no VRAM for the %s screen's render target", s ? "lower" : "upper");
                goto fail;
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
        sEdgeDepth = vramAlloc(C3D_CalcDepthBufSize(SCREEN_H, TOP_W, GPU_RB_DEPTH24_STENCIL8));
        if (!sEdgeDepth) {
            NitroPort_Log("[compose] no VRAM for the edge marking buffer: outlines off");
            sConfig.edges = 0;
        }
    }
    for (s = 0; s < 2 && sEdgeDepth; s++) {
        for (i = 0; i < 2 && sScreens[s].target[i]; i++) {
            C3D_FrameBuf *fb = &sScreens[s].edgeFb[i];

            *fb = sScreens[s].target[i]->frameBuf;
            C3D_FrameBufDepth(fb, sEdgeDepth, GPU_RB_DEPTH24_STENCIL8);
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

void NitroPort_ComposePublish(int engine, int screen)
{
    int e = engine ? 1 : 0, i, buf = -1;
    Screen *scr = &sScreens[screen ? 1 : 0];
    ComposeLayers *l = &sWork[e];
    LayerSet *set;
    u64 start = svcGetSystemTick();
    int w = l->width > 0 ? l->width : DS_W, n = w * DS_H;

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
    if (!l->displayOff) {
        uint16_t *c16 = sConverted16[e];

        for (i = 0; i < n; i++) {
            c16[i] = sToRgb565[l->under[i] & 0x7FFF];
        }
        if (sConfig.filter == COMPOSE_FILTER_SCALE2X && scr->texScale == 2) {
            NitroPort_Scale2xTiled16(c16, w, DS_H, set->under, scr->texW);
        } else if (scr->texScale == 2) {
            NitroPort_Double2xTiled16(c16, w, DS_H, set->under, scr->texW);
        } else {
            NitroPort_Tile16(c16, w, DS_H, set->under, scr->texW);
        }
        GSPGPU_FlushDataCache(set->under, (u32)scr->texW * scr->texH * 2u);
        if (l->has3d) {
            /* The layers in front: RGBA8 with the alpha multiplied in (so
             * the GPU's filtering and blending come out right). */
            uint32_t *c32 = sConverted[e];

            for (i = 0; i < n; i++) {
                uint16_t c = l->over[i];
                u32 a, r, g, b;

                if (!(c & 0x8000)) {
                    c32[i] = 0;
                    continue;
                }
                a = l->overAlpha[i];
                a = a >= 16 ? 255u : a * 255u / 16u;
                r = widen5(c & 31) * a / 255u;
                g = widen5((c >> 5) & 31) * a / 255u;
                b = widen5((c >> 10) & 31) * a / 255u;
                c32[i] = (r << 24) | (g << 16) | (b << 8) | a;
            }
            if (sConfig.filter == COMPOSE_FILTER_SCALE2X && scr->texScale == 2) {
                NitroPort_Scale2xTiled32(c32, w, DS_H, set->over, scr->texW);
            } else if (scr->texScale == 2) {
                NitroPort_Double2xTiled32(c32, w, DS_H, set->over, scr->texW);
            } else {
                NitroPort_Tile32(c32, w, DS_H, set->over, scr->texW);
            }
            GSPGPU_FlushDataCache(set->over, (u32)scr->texW * scr->texH * 4u);
        }
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
static void draw_screen(Screen *scr, int eye, const LayerSet *set, const NitroGx3dScene *scene, float stereo)
{
    C3D_RenderTarget *target = scr->target[eye];
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
        view.edgeFb = sEdgeDepth ? &scr->edgeFb[eye] : NULL;
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
        make_texture(&sOverTex, set->over, GPU_RGBA8, scr->texW, scr->texH, linear, GPU_CLAMP_TO_EDGE);
        C3D_TexBind(0, &sOverTex);
        quad_combiner(1);
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ONE_MINUS_SRC_ALPHA,
                       GPU_ONE, GPU_ONE_MINUS_SRC_ALPHA);
        quad(pic.x0, pic.y0, pic.x1, pic.y1, 0.0f, 0, vTop, u1, vBottom, 0xFFFFFFFFu);
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

int NitroPort_ComposeFrame(const NitroGx3dScene *scene, uint32_t sceneSerial, int aOnTop, float slider,
                           int forced)
{
    int s, drawn = 0, stereoNow, need[2] = { 0, 0 }, timed = 0;
    u32 vblanks = __atomic_load_n(&sVBlanks, __ATOMIC_RELAXED);
    LayerSet *sets[2] = { NULL, NULL };
    u64 start;

    if (!sReady) {
        return 0;
    }
    stereoNow = sConfig.stereo && slider > 0.0f && sScreens[0].target[1] != NULL && aOnTop;
    LightLock_Lock(&sSetLock);
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
        /* Not until the framebuffer it would draw into is off the LCD. */
        if (need[s] && scr->swapped && scr->swapVBlank == vblanks) {
            need[s] = 0;
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
    LightLock_Unlock(&sSetLock);
    if (!need[0] && !need[1]) {
        return 0;
    }
    if (scene != NULL && (!sScenePrepared || sceneSerial != sPreparedScene)) {
        NitroPort_PicaPrepareScene(scene, NULL);
        sPreparedScene = sceneSerial;
        sScenePrepared = 1;
        if (sConfig.stereo && sConfig.convergence <= 0.0f) {
            sSceneConvergence = scene_convergence(scene);
        }
    }
    gfxSet3D(stereoNow);
    if (!sInFrame) {
        C3D_FrameBegin(0);
        sInFrame = 1;
    }
    sQuadVertices = 0;
    /* The depth/stencil buffers cleared (before any drawing: the fills go
     * ahead of the frame's commands). */
    for (s = 0; s < 2; s++) {
        int eye;

        if (!need[s]) {
            continue;
        }
        for (eye = 0; eye < (s == 0 && stereoNow ? 2 : 1); eye++) {
            C3D_RenderTargetClear(sScreens[s].target[eye], C3D_CLEAR_DEPTH, 0, 0xFFFFFFFFu);
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

            draw_screen(scr, 0, sets[s], sc, -shift);
            draw_screen(scr, 1, sets[s], sc, shift);
        } else {
            draw_screen(scr, 0, sets[s], sc, 0.0f);
        }
        scr->shownSeq = sets[s]->seq;
        if (screen3d) {
            scr->shownScene = sceneSerial;
        }
        scr->shownSlider = slider;
        scr->shownStereo = stereoNow;
        drawn |= 1 << s;
    }
    GSPGPU_FlushDataCache(sQuads, (u32)(sQuadVertices * sizeof(NitroGx3dGpuVertex)));
    C3D_FrameEnd(GX_CMDLIST_FLUSH);
    NitroPort_PicaNoteCmdUse();
    sInFrame = 0;
    /* The targets to the LCD framebuffers, after the drawing (queued
     * behind it), then wait for all of it. */
    for (s = 0; s < 2; s++) {
        if (!(drawn & (1 << s))) {
            continue;
        }
        C3D_FrameBufTransfer(&sScreens[s].target[0]->frameBuf, s == 0 ? GFX_TOP : GFX_BOTTOM, GFX_LEFT,
                             SCREEN_TRANSFER_FLAGS);
        if (s == 0 && stereoNow) {
            C3D_FrameBufTransfer(&sScreens[s].target[1]->frameBuf, GFX_TOP, GFX_RIGHT,
                                 SCREEN_TRANSFER_FLAGS);
        }
    }
    start = svcGetSystemTick();
    C3D_FrameBegin(0);   /* (waits for the GPU and the transfers) */
    sInFrame = 1;
    sGpuTicks += svcGetSystemTick() - start;
    sFrames++;
    /* A new 3D frame shows sLag V-blanks after the game made its scene
     * (+1: the DS shows a scene the frame after): wait for that. */
    vblanks = __atomic_load_n(&sVBlanks, __ATOMIC_RELAXED);
    if (timed && scene != NULL) {
        u32 target = scene->stamp + 1u + (u32)sLag;   /* the V-blank it should show from */
        int32_t slack = (int32_t)(target - (vblanks + 1u));

        if (slack < 0) {
            /* late: more lag from now on */
            sLate++;
            sLag += -slack > 2 ? 2 : -slack;
            if (sLag > 6) {
                sLag = 6;
            }
            sOnTime = 0;
        } else {
            if (slack >= 1 && ++sOnTime >= 600 && sLag > 1) {
                sLag--;          /* ten seconds with room to spare */
                sOnTime = 0;
            }
            if (slack > 6) {
                target = vblanks + 1u + 6u;   /* (a stamp from before a pause) */
            }
            while ((int32_t)(target - (__atomic_load_n(&sVBlanks, __ATOMIC_RELAXED) + 1u)) > 0) {
                gspWaitForVBlank();
            }
        }
    }
    vblanks = __atomic_load_n(&sVBlanks, __ATOMIC_RELAXED);
    LightLock_Lock(&sSetLock);
    for (s = 0; s < 2; s++) {
        Screen *scr = &sScreens[s];

        if (sets[s] != NULL) {
            sets[s]->held = 0;
        }
        if (!(drawn & (1 << s))) {
            continue;
        }
        gfxScreenSwapBuffers(s == 0 ? GFX_TOP : GFX_BOTTOM, s == 0 && stereoNow);
        scr->swapVBlank = vblanks;
        scr->swapped = 1;
    }
    LightLock_Unlock(&sSetLock);
    return drawn;
}

void NitroPort_ComposeStats(u32 *frames, u64 *gpuTicks, u32 *published, u64 *publishTicks, int *lag,
                            u32 *late)
{
    *frames = sFrames;
    *gpuTicks = sGpuTicks;
    *published = __atomic_load_n(&sPublished, __ATOMIC_RELAXED);
    *publishTicks = __atomic_load_n(&sPublishTicks, __ATOMIC_RELAXED);
    *lag = sLag;
    *late = sLate;
}
#endif
