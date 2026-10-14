/*
 * The screens put together on the GPU (the "remaster" display path; see
 * docs/REMASTER.md). On the GPU thread, except where said.
 *
 * The DS's 256x192 screens are drawn bigger on the 3DS's (320x240 on both,
 * the upper screen's 400 pixels for the 3D in the wide view), the 3D at the
 * screen's own resolution rather than the DS's, with depth from the 3D
 * slider, and the 2D layers upscaled with a pixel-art filter (Scale2x)
 * before the GPU scales them the rest of the way.
 *
 * Each screen is a render target (240 x 400 or 240 x 320: the 3DS keeps
 * its framebuffers turned a quarter turn), drawn in this order:
 *   1. the 2D layers behind the 3D, a textured quad (the texture spans the
 *      whole screen: beyond the DS's picture, its edge colour continues);
 *   2. the 3D's rear plane: the clear colour, with its alpha;
 *   3. the 3D's polygons (port_gx3d_pica.c), with edge marking;
 *   4. a brightness effect on the 3D, when the game has one;
 *   5. the 2D layers in front of the 3D, a quad with per-pixel alpha;
 *   6. the master brightness (a white or black quad).
 * A screen without a 3D layer is just 1 (the whole picture) and 6. For
 * stereoscopic 3D the upper screen is drawn twice, the 3D shifted the
 * opposite way for each eye; the 2D is at the screen's depth.
 *
 * The 2D layers come from the drawing threads (NitroPort_Render2DSplit on
 * the game's display state), who also upscale them into textures here
 * (NitroPort_ComposePublish; three texture buffers per engine, so one can
 * be drawn from, one be the newest, and one be written). A frame is
 * composed when something changed: new layers, a new 3D scene, the slider
 * moved, or the engines swapped screens. The frame's targets are transferred
 * to the LCD framebuffers and shown at the next V-blank; a screen isn't
 * drawn again until that V-blank has passed (the framebuffer it would draw
 * into is still on the LCD until then).
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
#define TOP_W 400
#define BOTTOM_W 320
#define SCREEN_H 240
/* The DS's picture on a 3DS screen: 1.25 times the size (320 x 240). */
#define SCALE 1.25f

#define LAYER_BUFS 3

/* RGBA8 render targets to RGB565 framebuffers. */
#define SCREEN_TRANSFER_FLAGS (GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | \
     GX_TRANSFER_RAW_COPY(0) | GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | \
     GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB565) | GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

typedef struct Screen {
    C3D_RenderTarget *target[2];   /* left (and right) eye */
    C3D_FrameBuf edgeFb[2];        /* their colour with the edge marking depth/stencil */
    int width;                     /* 400 or 320 */
    /* What's shown: to see whether anything changed. */
    uint32_t shownPublished, shownScene;
    float shownSlider;
    int shownEngine, shownStereo;
    u32 swapVBlank;                /* the V-blank count when it was last swapped */
    int swapped;                   /* swapped at least once */
} Screen;

/* An engine's 2D layers: the pictures the drawer fills, and the textures
 * they're published into. */
typedef struct LayerSet {
    void *under;          /* RGBA5551 texels, tiled (or NULL: display off) */
    void *over;           /* RGBA8 texels, tiled (engine A only) */
    ComposeLayers info;   /* has3d etc., as published */
} LayerSet;

typedef struct Layers {
    ComposeLayers cpu;    /* the drawer's working pictures */
    LayerSet set[LAYER_BUFS];
    volatile int newest;  /* -1: nothing published yet */
    volatile int inUse;   /* being drawn from by the GPU, or -1 */
    volatile uint32_t published;
    LightLock lock;
} Layers;

static ComposeConfig sConfig;
static Screen sScreens[2];        /* 0: upper, 1: lower */
static void *sEdgeDepth;          /* 240 x 400 D24S8, shared by the screens */
static Layers sLayers[2];         /* by engine */
static int sTexW, sTexH, sPicW, sPicH;   /* the layer textures and the picture in them */
static u16 sToRgba5551[32768];
static NitroGx3dGpuVertex *sQuads;   /* this frame's quads */
static int sQuadVertices;
#define MAX_QUAD_VERTICES (6 * 32)
static int sInFrame;
static int sReady;
static volatile u32 sVBlanks;
static u32 sFrames;
static u64 sGpuTicks;
static volatile u32 sPublished;
static volatile u64 sPublishTicks;
static uint32_t sPreparedScene;
static int sScenePrepared;
static float sSceneConvergence = 1.0f;

/* Scratch for the drawers: the picture in texels before tiling. */
static uint32_t sConverted[2][DS_W * DS_H];
static uint16_t sConverted16[2][DS_W * DS_H];
static uint16_t sUnder[2][DS_W * DS_H], sOver[DS_W * DS_H];
static uint8_t sOverAlpha[DS_W * DS_H];

void NitroPort_ComposeVBlank(void)
{
    __atomic_fetch_add(&sVBlanks, 1u, __ATOMIC_RELAXED);
}

static void make_layer_texture(C3D_Tex *tex, void *data, GPU_TEXCOLOR fmt, GPU_TEXTURE_WRAP_PARAM wrap)
{
    memset(tex, 0, sizeof(*tex));
    tex->data = data;
    tex->fmt = fmt;
    tex->size = (u32)sTexW * sTexH * (fmt == GPU_RGBA8 ? 4u : 2u);
    tex->width = (u16)sTexW;
    tex->height = (u16)sTexH;
    tex->param = GPU_TEXTURE_MODE(GPU_TEX_2D) | GPU_TEXTURE_MAG_FILTER(GPU_LINEAR) |
                 GPU_TEXTURE_MIN_FILTER(GPU_LINEAR) | GPU_TEXTURE_WRAP_S(wrap) |
                 GPU_TEXTURE_WRAP_T(wrap);
    tex->border = 0;
    tex->lodParam = 0;
}

static void free_all(void)
{
    int s, e, i;

    for (s = 0; s < 2; s++) {
        for (i = 0; i < 2; i++) {
            if (sScreens[s].target[i]) {
                C3D_RenderTargetDelete(sScreens[s].target[i]);
                sScreens[s].target[i] = NULL;
            }
        }
    }
    if (sEdgeDepth) {
        vramFree(sEdgeDepth);
        sEdgeDepth = NULL;
    }
    for (e = 0; e < 2; e++) {
        for (i = 0; i < LAYER_BUFS; i++) {
            if (sLayers[e].set[i].under) linearFree(sLayers[e].set[i].under);
            if (sLayers[e].set[i].over) linearFree(sLayers[e].set[i].over);
            sLayers[e].set[i].under = NULL;
            sLayers[e].set[i].over = NULL;
        }
    }
    if (sQuads) {
        linearFree(sQuads);
        sQuads = NULL;
    }
    sReady = 0;
}

int NitroPort_ComposeInit(const ComposeConfig *config)
{
    int s, e, i, c;

    sConfig = *config;
    memset(sScreens, 0, sizeof(sScreens));
    memset(sLayers, 0, sizeof(sLayers));
    sPicW = config->filter ? 2 * DS_W : DS_W;
    sPicH = config->filter ? 2 * DS_H : DS_H;
    sTexW = sPicW;                                   /* 512 or 256 */
    sTexH = config->filter ? 512 : 256;
    for (c = 0; c < 32768; c++) {
        sToRgba5551[c] = (u16)(((c & 31) << 11) | (((c >> 5) & 31) << 6) | (((c >> 10) & 31) << 1) | 1);
    }
    for (s = 0; s < 2; s++) {
        Screen *scr = &sScreens[s];

        scr->width = s == 0 ? TOP_W : BOTTOM_W;
        for (i = 0; i < (s == 0 && config->stereo ? 2 : 1); i++) {
            scr->target[i] = C3D_RenderTargetCreate(SCREEN_H, scr->width, GPU_RB_RGBA8,
                                                    GPU_RB_DEPTH24_STENCIL8);
            if (!scr->target[i]) {
                NitroPort_Log("[compose] no VRAM for the %s screen's render target", s ? "lower" : "upper");
                goto fail;
            }
        }
        scr->shownEngine = -1;
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
    for (e = 0; e < 2; e++) {
        Layers *l = &sLayers[e];

        LightLock_Init(&l->lock);
        l->newest = -1;
        l->inUse = -1;
        l->cpu.under = sUnder[e];
        l->cpu.over = e == 0 ? sOver : NULL;
        l->cpu.overAlpha = e == 0 ? sOverAlpha : NULL;
        for (i = 0; i < LAYER_BUFS; i++) {
            l->set[i].under = linearAlloc((size_t)sTexW * sTexH * 2u);
            if (!l->set[i].under) goto nomem;
            memset(l->set[i].under, 0, (size_t)sTexW * sTexH * 2u);
            if (e == 0) {
                l->set[i].over = linearAlloc((size_t)sTexW * sTexH * 4u);
                if (!l->set[i].over) goto nomem;
                memset(l->set[i].over, 0, (size_t)sTexW * sTexH * 4u);
            }
        }
    }
    sQuads = linearAlloc(MAX_QUAD_VERTICES * sizeof(NitroGx3dGpuVertex));
    if (!sQuads) goto nomem;
    sInFrame = 0;
    sReady = 1;
    NitroPort_Log("[compose] screens drawn by the GPU: %s, 2D %s, 3D %s%s%s",
                  config->wide ? "wide view" : "the DS's view",
                  config->filter ? "upscaled with Scale2x" : "as it is",
                  config->stereo ? "with depth from the slider" : "flat",
                  sConfig.edges ? ", outlines on the GPU" : ", no outlines",
                  config->flipV ? ", 2D flipped" : "");
    return 1;
nomem:
    NitroPort_Log("[compose] not enough memory for the 2D layer textures");
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
    return &sLayers[engine ? 1 : 0].cpu;
}

uint32_t NitroPort_ComposePublished(int engine)
{
    return __atomic_load_n(&sLayers[engine ? 1 : 0].published, __ATOMIC_ACQUIRE);
}

/* Copy picture row `from` of a tiled texture to row `to` (so sampling at
 * the picture's bottom edge, half a texel into the row below, finds the
 * same colour). */
static void copy_tiled_row16(uint16_t *tiled, int from, int to)
{
    int x;

    for (x = 0; x < sPicW; x++) {
        tiled[NitroPort_TiledIndex((unsigned)x, (unsigned)to, (unsigned)sTexW)] =
            tiled[NitroPort_TiledIndex((unsigned)x, (unsigned)from, (unsigned)sTexW)];
    }
}

static void copy_tiled_row32(uint32_t *tiled, int from, int to)
{
    int x;

    for (x = 0; x < sPicW; x++) {
        tiled[NitroPort_TiledIndex((unsigned)x, (unsigned)to, (unsigned)sTexW)] =
            tiled[NitroPort_TiledIndex((unsigned)x, (unsigned)from, (unsigned)sTexW)];
    }
}

static u32 widen5(u32 v)
{
    return (v << 3) | (v >> 2);
}

void NitroPort_ComposePublish(int engine)
{
    int e = engine ? 1 : 0;
    Layers *l = &sLayers[e];
    LayerSet *set;
    int i, buf = -1;
    u64 start = svcGetSystemTick();

    if (!sReady) {
        return;
    }
    /* A buffer that isn't the newest nor being drawn from. */
    LightLock_Lock(&l->lock);
    for (i = 0; i < LAYER_BUFS; i++) {
        if (i != l->newest && i != l->inUse) {
            buf = i;
            break;
        }
    }
    LightLock_Unlock(&l->lock);
    if (buf < 0) {
        return;   /* (can't happen with three) */
    }
    set = &l->set[buf];
    set->info = l->cpu;
    if (!l->cpu.displayOff) {
        /* The picture behind the 3D (or the whole picture): RGB555 to the
         * PICA's RGBA5551, opaque. */
        uint16_t *c16 = sConverted16[e];

        for (i = 0; i < DS_W * DS_H; i++) {
            c16[i] = sToRgba5551[l->cpu.under[i] & 0x7FFF];
        }
        if (sConfig.filter) {
            NitroPort_Scale2xTiled16(c16, DS_W, DS_H, set->under, sTexW);
        } else {
            NitroPort_Tile16(c16, DS_W, DS_H, set->under, sTexW);
        }
        copy_tiled_row16(set->under, sPicH - 1, sPicH);
        GSPGPU_FlushDataCache(set->under, (u32)sTexW * sTexH * 2u);
        if (l->cpu.has3d && set->over) {
            /* The layers in front: RGBA8 with the alpha multiplied in (so
             * the GPU's bilinear filtering and blending come out right). */
            uint32_t *c32 = sConverted[e];

            for (i = 0; i < DS_W * DS_H; i++) {
                uint16_t c = l->cpu.over[i];
                u32 a, r, g, b;

                if (!(c & 0x8000)) {
                    c32[i] = 0;
                    continue;
                }
                a = l->cpu.overAlpha[i];
                a = a >= 16 ? 255u : a * 255u / 16u;
                r = widen5(c & 31) * a / 255u;
                g = widen5((c >> 5) & 31) * a / 255u;
                b = widen5((c >> 10) & 31) * a / 255u;
                c32[i] = (r << 24) | (g << 16) | (b << 8) | a;
            }
            if (sConfig.filter) {
                NitroPort_Scale2xTiled32(c32, DS_W, DS_H, set->over, sTexW);
            } else {
                NitroPort_Tile32(c32, DS_W, DS_H, set->over, sTexW);
            }
            copy_tiled_row32(set->over, sPicH - 1, sPicH);
            GSPGPU_FlushDataCache(set->over, (u32)sTexW * sTexH * 4u);
        }
    }
    LightLock_Lock(&l->lock);
    l->newest = buf;
    l->published++;
    LightLock_Unlock(&l->lock);
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

/* The combiner for a quad: the texture (modulated by nothing), or the
 * vertex colour. */
static void quad_combiner(int textured)
{
    C3D_TexEnv *env = C3D_GetTexEnv(0);

    C3D_TexEnvInit(env);
    if (textured) {
        C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, 0, 0);
    } else {
        C3D_TexEnvSrc(env, C3D_Both, GPU_PRIMARY_COLOR, 0, 0);
    }
    C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
}

/* Where the DS's picture goes on a screen: 320 x 240, in the middle. */
static PicaRect picture_rect(const Screen *scr)
{
    PicaRect r;

    r.x0 = (float)(scr->width - DS_W * SCALE) * 0.5f;
    r.x1 = r.x0 + DS_W * SCALE;
    r.y0 = 0.0f;
    r.y1 = SCREEN_H;
    return r;
}

/* The texture coordinates of the picture's corners in a layer texture
 * (its top row is at v = 1: the PICA reads row height - 1 - t for t). */
static void picture_uv(float *u1, float *vTop, float *vBottom)
{
    *u1 = (float)sPicW / (float)sTexW;
    *vTop = 1.0f;
    *vBottom = 1.0f - (float)sPicH / (float)sTexH;
    if (sConfig.flipV) {
        float t = *vTop;

        *vTop = *vBottom;
        *vBottom = t;
    }
}

/* The 2D layer textures, described for the GPU. */
static C3D_Tex sUnderTex, sOverTex;

/* One screen (one eye of it), into its render target. */
static void draw_screen(Screen *scr, int eye, int engine, const LayerSet *set, const NitroGx3dScene *scene,
                        float stereo)
{
    C3D_RenderTarget *target = scr->target[eye];
    C3D_FrameBuf *fb = &target->frameBuf;
    C3D_Mtx pixels;
    PicaRect pic = picture_rect(scr), rect3d = pic;
    float u1, vTop, vBottom;
    int has3d = set != NULL && !set->info.displayOff && set->info.has3d && scene != NULL;
    const ComposeLayers *info = set ? &set->info : NULL;

    (void)engine;
    C3D_SetFrameBuf(fb);
    C3D_SetViewport(0, 0, fb->width, fb->height);
    NitroPort_PicaPixelMatrix(&pixels, scr->width, SCREEN_H);
    NitroPort_PicaSetView(&pixels, 0.0f, 1.0f);
    C3D_CullFace(GPU_CULL_NONE);
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    C3D_StencilTest(false, GPU_ALWAYS, 0, 0xFF, 0xFF);
    C3D_AlphaTest(false, GPU_ALWAYS, 0);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
    picture_uv(&u1, &vTop, &vBottom);
    if (set == NULL || set->info.displayOff) {
        /* Nothing published yet, or the engine is off: black. */
        quad_combiner(0);
        quad(0, 0, (float)scr->width, SCREEN_H, 0.0f, 0, 0, 0, 0, 0x000000FFu);
        return;
    }
    /* 1. The picture behind the 3D, over the whole screen: beyond the
     * picture, its edge columns (the texture clamps). */
    make_layer_texture(&sUnderTex, set->under, GPU_RGBA5551, GPU_CLAMP_TO_EDGE);
    C3D_TexBind(0, &sUnderTex);
    quad_combiner(1);
    {
        float side = pic.x0 / (pic.x1 - pic.x0);   /* the band each side, in picture widths */

        quad(0, 0, (float)scr->width, SCREEN_H, 0.0f, -side * u1, vTop, (1.0f + side) * u1, vBottom,
             0xFFFFFFFFu);
    }
    if (has3d) {
        uint16_t rearColor;
        unsigned rearAlpha;
        PicaView view;

        if (sConfig.wide && scr->width == TOP_W) {
            rect3d.x0 = 0.0f;
            rect3d.x1 = (float)scr->width;
        }
        /* BG0HOFS: the 3D moved left by that many DS pixels (the game's
         * screen shakes). */
        if (info->scroll3d) {
            int scroll = info->scroll3d >= 256 ? info->scroll3d - 512 : info->scroll3d;
            float shift = (float)scroll * (rect3d.x1 - rect3d.x0) / DS_W;

            rect3d.x0 -= shift;
            rect3d.x1 -= shift;
        }
        /* 2. The rear plane: the clear colour, blended by its alpha. */
        NitroPort_PicaRearPlane(&rearColor, &rearAlpha);
        if (rearAlpha) {
            u32 a = rearAlpha * 255u / 31u;

            quad_combiner(0);
            C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA,
                           GPU_ONE, GPU_ZERO);
            quad(rect3d.x0, rect3d.y0, rect3d.x1, rect3d.y1, -1.0f, 0, 0, 0, 0,
                 (widen5(rearColor & 31) << 24) | (widen5((rearColor >> 5) & 31) << 16) |
                 (widen5((rearColor >> 10) & 31) << 8) | a);
        }
        /* 3. The polygons. Where the picture doesn't reach (the wide view's
         * bands, a scrolled 3D) the scissor keeps them in the screen. */
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
        make_layer_texture(&sOverTex, set->over, GPU_RGBA8, GPU_CLAMP_TO_BORDER);
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

int NitroPort_ComposeFrame(const NitroGx3dScene *scene, uint32_t sceneSerial, int aOnTop, float slider,
                           int forced)
{
    int s, drawn = 0, stereoNow;
    u32 vblanks = __atomic_load_n(&sVBlanks, __ATOMIC_RELAXED);
    LayerSet *sets[2] = { NULL, NULL };
    uint32_t heldPublished[2] = { 0, 0 };
    int engineOf[2], need[2] = { 0, 0 };
    u64 start;

    if (!sReady) {
        return 0;
    }
    engineOf[0] = aOnTop ? 0 : 1;
    engineOf[1] = aOnTop ? 1 : 0;
    stereoNow = sConfig.stereo && slider > 0.0f && sScreens[0].target[1] != NULL;
    for (s = 0; s < 2; s++) {
        Screen *scr = &sScreens[s];
        Layers *l = &sLayers[engineOf[s]];
        uint32_t published;
        int has3d;

        /* The newest layers, held (the drawer won't write them) while this
         * frame may draw from them. */
        LightLock_Lock(&l->lock);
        l->inUse = l->newest;
        published = l->published;
        LightLock_Unlock(&l->lock);
        if (l->inUse < 0) {
            continue;   /* nothing to show yet */
        }
        sets[s] = &l->set[l->inUse];
        heldPublished[s] = published;
        has3d = sets[s]->info.has3d && !sets[s]->info.displayOff && scene != NULL;
        if (s == 0 && !has3d) {
            stereoNow = 0;   /* (nothing on the upper screen has depth) */
        }
        need[s] = forced || scr->shownEngine != engineOf[s] || published != scr->shownPublished ||
                  (has3d && sceneSerial != scr->shownScene) ||
                  (s == 0 && has3d && (stereoNow != scr->shownStereo ||
                                        (stereoNow && slider != scr->shownSlider)));
        /* Not until the framebuffer it would draw into is off the LCD. */
        if (need[s] && scr->swapped && scr->swapVBlank == vblanks) {
            need[s] = 0;
        }
        if (!need[s]) {
            LightLock_Lock(&l->lock);
            l->inUse = -1;
            LightLock_Unlock(&l->lock);
            sets[s] = NULL;
        }
    }
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

        if (!need[s]) {
            continue;
        }
        if (s == 0 && stereoNow) {
            float shift = sConfig.stereoStrength * slider * 0.5f;

            draw_screen(scr, 0, engineOf[s], sets[s], scene, -shift);
            draw_screen(scr, 1, engineOf[s], sets[s], scene, shift);
        } else {
            draw_screen(scr, 0, engineOf[s], sets[s], scene, 0.0f);
        }
        scr->shownEngine = engineOf[s];
        scr->shownPublished = heldPublished[s];
        scr->shownScene = sceneSerial;
        scr->shownSlider = slider;
        scr->shownStereo = stereoNow;
        drawn |= 1 << s;
    }
    GSPGPU_FlushDataCache(sQuads, (u32)(sQuadVertices * sizeof(NitroGx3dGpuVertex)));
    C3D_FrameEnd(GX_CMDLIST_FLUSH);
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
    /* Shown from the next V-blank. */
    vblanks = __atomic_load_n(&sVBlanks, __ATOMIC_RELAXED);
    for (s = 0; s < 2; s++) {
        Screen *scr = &sScreens[s];
        Layers *l = &sLayers[engineOf[s]];

        if (!(drawn & (1 << s))) {
            continue;
        }
        gfxScreenSwapBuffers(s == 0 ? GFX_TOP : GFX_BOTTOM, s == 0 && stereoNow);
        scr->swapVBlank = vblanks;
        scr->swapped = 1;
        LightLock_Lock(&l->lock);
        l->inUse = -1;
        LightLock_Unlock(&l->lock);
    }
    return drawn;
}

void NitroPort_ComposeStats(u32 *frames, u64 *gpuTicks, u32 *published, u64 *publishTicks)
{
    *frames = sFrames;
    *gpuTicks = sGpuTicks;
    *published = __atomic_load_n(&sPublished, __ATOMIC_RELAXED);
    *publishTicks = __atomic_load_n(&sPublishTicks, __ATOMIC_RELAXED);
}
#endif
