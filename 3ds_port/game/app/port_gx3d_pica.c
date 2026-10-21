/*
 * Stage 2: the PICA200 backend. Only the dedicated 3D thread calls
 * functions in this file: no GPU commands run in the game or 2D threads.
 *
 * Two ways of drawing a scene:
 *  - the classic one (NitroPort_PicaRender): a 256x192 off-screen RGBA8
 *    target with a 24-bit depth buffer. GX_DisplayTransfer untile-copies
 *    into one of three linear RGBA8 buffers; after completion it's handed
 *    over to the shared image API as it is, and the 3D screen's drawer
 *    converts it to RGB555 when it draws.
 *  - the compose one (NitroPort_PicaPrepareScene, NitroPort_PicaDrawScene;
 *    port_compose_pica.c): the polygons drawn straight into the screen's
 *    own render target, at the screen's resolution, between the 2D layers
 *    the compositor draws; with edge marking done by the GPU (below), and
 *    the stereoscopic shift and the view (where the DS's screen goes on
 *    the 3DS's) in the vertex program.
 *
 * PR2: textures. Each DS texture is decoded with the PC reference's own
 * decoder (port_gx3d_texture.c), tiled for the PICA and kept in a cache.
 * Triangles are drawn in batches that share a texture and polygon mode,
 * opaque ones first and translucent ones after, in the DS's order
 * (NitroPort_Gx3dDrawOrder), as the reference does.
 * Edge marking: the opaque pixels' polygon IDs go to the stencil buffer
 * in a second pass, and the depth/stencil buffer is read back with the
 * image; port_gx3d_edge.c outlines it as it's published.
 * Toon/highlight, shadow polygons and fog are later PRs.
 */
#ifdef __3DS__
#include "port_pica.h"
#include <nitro_port_gx3d_texture.h>
#include <nitro_port_xbrz.h>
#include <nitro_port_shadow.h>
#include <malloc.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

extern unsigned char NitroPort_IoReg[];
void NitroPort_Log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#define GX3D_W NITRO_GX3D_WIDTH
#define GX3D_H NITRO_GX3D_HEIGHT
#define IMAGE_BYTES ((size_t)GX3D_W*GX3D_H*4u)
/* Tiled PICA RGBA8 -> CPU-linear RGBA8, same orientation and dimensions.
 * Out-of-band readback uses no screen render target or framebuffers. */
#define GX3D_TRANSFER_FLAGS (GX_TRANSFER_FLIP_VERT(0) | \
     GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) | \
     GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | \
     GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGBA8) | \
     GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

#define CMDBUF_BYTES (1024u*1024u)
/* Words a batch's draw can take at most (texture, combiner, tests, fog,
 * the draw): drawing stops before the command list could run out. */
#define DRAW_WORDS 256u
extern u32 gpuCmdBufSize,gpuCmdBufOffset;
static unsigned sDrawsCut;   /* draws left out for want of room ([perf]) */
/* Where the drawing is, for the log when it hangs (NitroPort_PicaStageNow:
 * the stage, and the last draw): the GPU doesn't come back from a draw of
 * no vertices, and may not from other faults. */
static const char *sStage="idle";
static u32 sLastDrawFirst,sLastDrawCount,sDraws;
static void stage(const char *s){__atomic_store_n(&sStage,s,__ATOMIC_RELAXED);}
static void draw_arrays(int first,int count)
{
    static int told;
    if(count<=0 || count%3){
        if(!told){told=1;NitroPort_Log("[gx3d] a draw of %d vertices at %d in %s: left out",count,first,__atomic_load_n(&sStage,__ATOMIC_RELAXED));}
        return;
    }
    __atomic_store_n(&sLastDrawFirst,(u32)first,__ATOMIC_RELAXED);
    __atomic_store_n(&sLastDrawCount,(u32)count,__ATOMIC_RELAXED);
    __atomic_fetch_add(&sDraws,1,__ATOMIC_RELAXED);
    C3D_DrawArrays(GPU_TRIANGLES,first,count);
}
void NitroPort_PicaStage(const char *s){stage(s);}
const char *NitroPort_PicaStageNow(unsigned *first,unsigned *count,unsigned *draws)
{
    if(first)*first=__atomic_load_n(&sLastDrawFirst,__ATOMIC_RELAXED);
    if(count)*count=__atomic_load_n(&sLastDrawCount,__ATOMIC_RELAXED);
    if(draws)*draws=__atomic_load_n(&sDraws,__ATOMIC_RELAXED);
    return __atomic_load_n(&sStage,__ATOMIC_RELAXED);
}
static int room_for_draw(void)
{
    if(gpuCmdBufSize-gpuCmdBufOffset>DRAW_WORDS*4u)return 1;
    sDrawsCut++;
    return 0;
}
/* The command list's peak use and the draws left out since last asked. */
static float sCmdPeak;
static int sFramesOwn;   /* (the remaster's frames: NitroPort_PicaFrameOpen) */
void NitroPort_PicaCmdStats(float *peak,unsigned *cut)
{
    float now=sFramesOwn?0.0f:C3D_GetCmdBufUsage();
    if(now>sCmdPeak)sCmdPeak=now;
    if(peak)*peak=sCmdPeak;
    if(cut)*cut=sDrawsCut;
    sCmdPeak=0.0f;sDrawsCut=0;
}
void NitroPort_PicaNoteCmdUse(void)
{
    float now=C3D_GetCmdBufUsage();
    if(now>sCmdPeak)sCmdPeak=now;
}

/* ------------------------------------------------ the remaster's frames
 * The display (port_compose_pica.c) builds a frame's commands while the
 * GPU draws the frame before: two command lists, used in turn, and the
 * frame's GPU operations recorded in a queue of the display's own, run
 * once the frame before is done. citro3d's frames don't allow it
 * (C3D_FrameBegin waits for the GPU before a frame's commands may start,
 * into its one command list): they aren't used there. A list is cut by
 * citro3d's own C3Di_SplitFrame (with the framebuffer cache's flush, as
 * C3D_FrameSplit does). */
extern bool C3Di_SplitFrame(u32 **buf,u32 *size);
static uint32_t sFrame;   /* (DrawScene's count, below) */
static u32 *sFrameCmd[2];
static int sFrameCmdNow;
/* Textures and memory the GPU may still read: sFrame at the end of the
 * frame it was given last (sFlightFrame), at the end of the last it
 * finished (sSafeFrame); sGpuWait waits for it (NitroPort_PicaGpuWait). */
static u32 sFlightFrame,sSafeFrame;
static void (*sGpuWait)(void);

int NitroPort_PicaFramesInit(void)
{
    int i;
    for(i=0;i<2;i++){
        if(!sFrameCmd[i])sFrameCmd[i]=linearAlloc(CMDBUF_BYTES);
        if(!sFrameCmd[i])return 0;
    }
    sFramesOwn=1;
    return 1;
}

void NitroPort_PicaFrameOpen(void)
{
    sFrameCmdNow^=1;
    GPUCMD_SetBuffer(sFrameCmd[sFrameCmdNow],CMDBUF_BYTES/4u,0);
}

void NitroPort_PicaFrameSplit(void)
{
    u32 *buf,size;
    float use=1.0f-(float)(gpuCmdBufSize-gpuCmdBufOffset)/(float)(CMDBUF_BYTES/4u);   /* (of the whole list) */
    if(use>sCmdPeak)sCmdPeak=use;
    if(C3Di_SplitFrame(&buf,&size))GX_ProcessCommandList(buf,size*4u,GX_CMDLIST_FLUSH);
}

u32 NitroPort_PicaFrameClose(void)
{
    NitroPort_PicaFrameSplit();
    GPUCMD_SetBuffer(NULL,0,0);
    return sFrame;
}

void NitroPort_PicaFrameRunning(u32 mark)
{
    sFlightFrame=mark;
}

void NitroPort_PicaFrameDone(u32 mark)
{
    sSafeFrame=mark;
}

void NitroPort_PicaGpuWait(void (*wait)(void))
{
    sGpuWait=wait;
}

/* Before what the GPU may be reading is changed: a texture drawn in a
 * frame not known to be finished (but the one being built). */
static int in_flight(u32 frame)
{
    return (int32_t)(frame-sSafeFrame)>0 && (int32_t)(frame-sFlightFrame)<=0;
}
static void settle(void)
{
    if(sGpuWait && sFlightFrame!=sSafeFrame)sGpuWait();
}

void NitroPort_PicaFramesFree(void)
{
    int i;
    for(i=0;i<2;i++){
        if(sFrameCmd[i])linearFree(sFrameCmd[i]);
        sFrameCmd[i]=NULL;
    }
    sFramesOwn=0;
    sGpuWait=NULL;
}

/* A scene has at most 4096 triangles; a batch per triangle at worst. */
#define MAX_BATCHES NITRO_GX3D_MAX_TRIANGLES
/* After them, the sun shadows' own (NitroGx3dScene.shadow_triangles): their
 * vertices after the scene's, their batches after its. */
#define SHADOW_VERTICES (3u*NITRO_GX3D_MAX_SHADOW_TRIANGLES+3u*SHADOW_CARD_TRIANGLES)
#define SHADOW_BATCHES (2048u+SHADOW_CARD_BATCHES)
/* and the cards' copies turned to the sun (NitroPort_ShadowCards) */
#define SHADOW_CARD_TRIANGLES 512u
#define SHADOW_CARD_BATCHES 256u

/* The texture cache: converted textures stay while their source bytes hash
 * the same. Least recently used ones go when over budget. */
#define TEX_CACHE_ENTRIES 256
#define TEX_CACHE_BYTES   (8u*1024u*1024u)

typedef struct TexEntry {
    int used;
    uint32_t param, palette, hash;
    uint32_t checkedFrame, lastFrame;
    size_t bytes;
    unsigned scale;    /* texels a DS texel: 1, or 2 (sTexMode 2 and 3) */
    int filtered;      /* sampled with the bilinear filter */
    int holes;         /* some texels not fully opaque (its shape needs the alpha test) */
    int packed;        /* RGBA5551 (NitroPort_Gx3dTexIs5551, not filtered), else RGBA8 */
    /* sTexMode 3: XBRZ_WAITING its texels 2x2 for now, its xBRZ to come
     * (from xbrzAfter, a system tick: a texture that keeps changing waits
     * until it stops), XBRZ_DONE */
    int xbrz;
    u64 xbrzAfter,convertedAt;
    u32 conversions;   /* (each conversion's number: an xBRZ begun before is out of date) */
    C3D_Tex tex;
} TexEntry;
enum { XBRZ_NONE, XBRZ_WAITING, XBRZ_DONE };

/* How the compose path's textures are sampled (NitroPort_PicaTextureFilter,
 * after remaster.txt's filter): 0 as the DS, nearest; 1 smooth, bilinear;
 * 2 sharp: each texel 2x2 and bilinear, the DS's texels sharp with their
 * borders a texel soft, as the 2D is drawn. Drawn 1.25 times the DS's
 * size, a nearest texel is one pixel or two, and which ones changed as the
 * picture moved: the opening's flyover shimmered ("jittery") against its
 * fog, which is 2D (October 2026). 3 xbrz: 2x by xBRZ (edges and curves
 * smooth, port_xbrz.c), nearest, RGBA5551 where the DS's format allows:
 * half sharp's memory and what the GPU reads, and no neighbouring texels
 * filtered in at polygons' edges (sharp's lines). Worked out a little each
 * scene (xbrz_work), the texture's texels 2x2 meanwhile. 4 xbrz smooth: the
 * same, bilinear (no shimmer as the picture moves; sharp's lines may be
 * back). The classic path: nearest. */
static int sTexMode;
static uint32_t *sTexLinear, *sTexTiled;   /* (for converting: heap, grown as needed) */
static size_t sTexLinearWords, sTexTiledWords;
/* filter3d xbrz: the texture being worked on (xbrz_work), where it's got
 * to (pass 0 or 1, row), its texels, the corners' blends, the output. */
static struct {
    TexEntry *e;
    u32 conversions,param,palette;
    unsigned w,h,phase,row;
    int wrapS,wrapT;
    uint32_t *src,*out,*info;   /* (info: a byte a texel) */
    size_t srcWords,outWords,infoWords;
} sXbrz;
static int sXbrzWorked;   /* (once a scene prepared) */
/* The bound texture's cut for opaque pixels: a filtered texture's edges
 * are half see-through; the DS's texel boundary is at alpha one half. */
static int sBoundCut;
/* ... and whether it has see-through texels (TexEntry.holes) */
static int sBoundHoles;

/* The texture on unit 0 as bound here, so it's bound only when it
 * changes: citro3d clears the GPU's texture cache each time a texture is
 * bound, the same one or none (it was, for each batch of each of a
 * frame's nine draws of its polygons). Not known at the start of a
 * scene's draw (the compositor binds its own), nor after a texture is made
 * again (its C3D_Tex may describe other memory now). */
static C3D_Tex *sBoundTex;
static int sBoundTexKnown;

static void bind_tex(C3D_Tex *tex)
{
    if(sBoundTexKnown && tex==sBoundTex)return;
    C3D_TexBind(0,tex);
    sBoundTex=tex;
    sBoundTexKnown=1;
}


/* bin2s emits these from picasso's gx3d_passthrough.shbin. */
extern const u8 gx3d_passthrough_shbin[];
extern const u32 gx3d_passthrough_shbin_size;

/* All of these are worker-owned; neither game nor compositor touches them. */
static C3D_RenderTarget *sTarget;
static DVLB_s *sDvlb;
static shaderProgram_s sProgram;
static int sProgramReady;
static s8 sUnifMtx=-1,sUnifStereo=-1;   /* the vertex program's uniforms */
/* The view last set (NitroPort_PicaSetView): set again after another
 * program has been bound (the programs' uniforms share the GPU's
 * registers, each at its own place). */
static C3D_Mtx sViewMtx;
static float sViewStereo,sViewConvergence=1.0f;
/* Sun shadows (remaster.txt: shadows on; port_shadow.c). Each scene with a
 * ground: the opaque polygons drawn from the sun into a shadow map (the
 * PICA's shadow mode: each texel keeps the nearest distance), then the
 * scene's opaque polygons each pixel times the light there: 1 where the
 * sun reaches it, sShadowColor where something nearer the sun is in the
 * way (the map looked up by the PICA's shadow texture, 2x2 filtered: soft
 * edges a texel wide). The DS's texture goes on unit 1 meanwhile (only
 * unit 0 takes a shadow map). */
#define SHADOW_SIZE 512u
static shaderProgram_s sCastProgram,sRecvProgram;
static int sCastReady,sRecvReady;
static s8 sUnifCast=-1,sUnifRecvMtx=-1,sUnifRecvStereo=-1,sUnifRecvShadow=-1;
static C3D_Tex sShadowTex;
static C3D_RenderTarget *sShadowTarget;
static int sShadowTexReady;
static int sShadowMode;                      /* (NitroPort_PicaLook) */
static int sSunFixed;
static NitroShadowSun sSun={55.0f,32.0f},sFixedSun={55.0f,32.0f};
static u32 sShadowColor;                     /* (0xAABBGGRR, alpha 0: the shade, by the sun's strength) */
/* The colour grade (NitroPort_PicaLook): on, the time of day's period it's
 * made for, the one it should be (worked out as a scene is prepared; made
 * and the textures decoded again at the scene's first draw, the GPU done
 * with the textures then). */
static int sGradeMode,sGradePeriod=-1,sGradeWanted=-1;
static uint32_t *sGradeTable;
static int look_now(void);
static void free_pruning(void);
static void free_edge_copies(void);
static int make_shadow_spare(void);
static void free_shadow_spare(void);
static NitroShadowSetup sShadow;
static int sShadowReady,sShadowDrawn;        /* (the prepared scene has a ground; its map is drawn) */
static C3D_Tex *sRecvTex;                    /* (unit 1's texture, as bound) */
static int sRecvTexKnown;
/* The compose path: the scene prepared for drawing (its vertices in
 * sVertices[sPrepared], its batches), and its rear plane. */
static int sPrepared=-1;
static unsigned sPreparedSerial;             /* (a new one each prepared scene) */
static size_t sPreparedCount,sPreparedBatches;
static size_t sShadowOnlyBatches;           /* (the shadows' own, at sBatches[MAX_BATCHES]) */
static size_t sShadowRoom;                   /* (vertices after the scene's for them: SHADOW_VERTICES, or 0) */
/* The prepared scene's batches that are the game's own painted shadows
 * (NitroPort_ShadowDecal): left out with the sun shadows on. */
static uint8_t sDecal[MAX_BATCHES];
static uint32_t sDecalTexels[4096];
static uint32_t sPreparedSwap;
static uint16_t sRearColor;
static unsigned sRearAlpha;
/* The prepared scene's depths, as the GPU gets them: d' = (d - sDepthA) *
 * sDepthK, d the DS's depth (0 near .. 1 far, depth / 0xFFFFFF). Linear,
 * so nothing in front of or behind anything else changes, but the scene's
 * own depths spread over the whole 0..1: the fog table (128 steps over
 * 0..1) has its steps where the scene is (normalize_depth). */
static float sDepthA=0.0f,sDepthK=1.0f;
/* The DS's fog (DISP3DCNT bit 7) for the prepared scene: the PICA's fog
 * table worked out from the DS's (sFogLut), its colour, and whether the
 * rear plane is fogged. */
static int sFogOn,sFogRear;
static u32 sFogColor;
static C3D_FogLut sFogLut;
static uint8_t sFogDensityRear;
static NitroGx3dGpuVertex *sVertices[2];
/* The finished images, read back: three, as one may be waiting to be
 * converted and another being converted (NitroPort_Gx3dSubmitRGBA8) while
 * the next is drawn into the third. */
#define IMAGE_SLOTS 3
static u32 *sImages[IMAGE_SLOTS];
/* The depth/stencil buffer, read back for edge marking: 24-bit depth and
 * the stencil (an opaque polygon ID, or 64-127/255 for none) << 24. */
static u32 *sDepthIds[IMAGE_SLOTS];
/* Edge marking works (off for good if the buffer read back ever isn't as
 * expected), how many scenes' buffers are still to be checked (scenes with
 * opaque polygons: an empty one can't show the IDs aren't written), and how
 * many of those checked had IDs in it. */
static int sEdgeMarking=1;
static unsigned sEdgeChecks=60,sEdgeScenesWithIds;
static int sBuffer;
static NitroGx3dBatch *sBatches;
static TexEntry sTextures[TEX_CACHE_ENTRIES];
static size_t sTextureBytes;
static uint32_t sFrame;
static NitroGx3dTexBanks sBanks;
/* Textures converted (new, or their bytes changed), their texels, and the
 * time it took, for the [perf] line. */
static volatile u32 sTexConverted;
static volatile u64 sTexTexels,sTexTicks;

static void free_texture(TexEntry *e)
{
    if(!e->used)return;
    C3D_TexDelete(&e->tex);
    sTextureBytes-=e->bytes;
    memset(e,0,sizeof(*e));
}

/* (the 3D thread, between frames) */
void NitroPort_PicaTextureFilter(int mode)
{
    int i;
    if(mode<0)mode=0;
    if(mode>4)mode=4;
    if(mode==sTexMode)return;
    sTexMode=mode;
    settle();
    for(i=0;i<TEX_CACHE_ENTRIES;i++)free_texture(&sTextures[i]);   /* made again the new way */
}

/* The colour grade the textures are decoded with now (sGradeWanted: a
 * period, or -1 for none), the cached ones made again with it. */
static void apply_grade(void)
{
    int i;
    if(sGradeWanted==sGradePeriod)return;
    if(sGradeWanted>=0){
        if(!sGradeTable)sGradeTable=malloc(32768u*sizeof(uint32_t));
        if(!sGradeTable){sGradeWanted=-1;if(sGradePeriod<0)return;}
        else NitroPort_LookGradeTable(sGradeWanted,sGradeTable);
    }
    NitroPort_Gx3dSetGrade(sGradeWanted>=0?sGradeTable:NULL);
    sGradePeriod=sGradeWanted;
    settle();
    for(i=0;i<TEX_CACHE_ENTRIES;i++)free_texture(&sTextures[i]);
}

static void clean_up(void)
{
    int i;
    for(i=0;i<TEX_CACHE_ENTRIES;i++)free_texture(&sTextures[i]);
    free(sTexLinear);sTexLinear=NULL;sTexLinearWords=0;
    free(sTexTiled);sTexTiled=NULL;sTexTiledWords=0;
    free(sXbrz.src);free(sXbrz.out);free(sXbrz.info);
    memset(&sXbrz,0,sizeof(sXbrz));
    for(i=0;i<2;i++){
        if(sVertices[i])linearFree(sVertices[i]);
        sVertices[i]=NULL;
    }
    for(i=0;i<IMAGE_SLOTS;i++){
        if(sImages[i])linearFree(sImages[i]);
        if(sDepthIds[i])linearFree(sDepthIds[i]);
        sImages[i]=NULL;sDepthIds[i]=NULL;
    }
    free(sBatches);sBatches=NULL;
    free_pruning();
    free_edge_copies();
    if(sProgramReady){
        shaderProgramFree(&sProgram);
        sProgramReady=0;
    }
    if(sCastReady){shaderProgramFree(&sCastProgram);sCastReady=0;}
    if(sRecvReady){shaderProgramFree(&sRecvProgram);sRecvReady=0;}
    if(sShadowTarget){C3D_RenderTargetDelete(sShadowTarget);sShadowTarget=NULL;}
    if(sShadowTexReady){C3D_TexDelete(&sShadowTex);sShadowTexReady=0;}
    free_shadow_spare();
    sShadowMode=0;
    NitroPort_SetSunShadows(0);
    NitroPort_Gx3dSetGrade(NULL);
    free(sGradeTable);sGradeTable=NULL;
    sGradeMode=0;sGradePeriod=sGradeWanted=-1;
    if(sDvlb){DVLB_Free(sDvlb);sDvlb=NULL;}
    if(sTarget){C3D_RenderTargetDelete(sTarget);sTarget=NULL;}
    C3D_Fini();
}
/* The memory left, for the log: the linear heap's free space, and the
 * program's heap (malloc's): what's in use of its size, and what malloc
 * has free inside what it took. */
extern u32 __ctru_heap_size,__ctru_linear_heap_size;
static void log_memory(const char *what)
{
    struct mallinfo m=mallinfo();
    NitroPort_Log("%s: linear heap %lu KB free of %lu KB; heap: %lu KB in use of %lu KB "
                  "(%lu KB taken, %lu KB of it free)",
                  what,(unsigned long)(linearSpaceFree()/1024u),(unsigned long)(__ctru_linear_heap_size/1024u),
                  (unsigned long)(m.uordblks/1024u),(unsigned long)(__ctru_heap_size/1024u),
                  (unsigned long)(m.arena/1024u),(unsigned long)(m.fordblks/1024u));
}
void NitroPort_PicaLogMemory(void)
{
    log_memory("[perf] memory");
}
/* Why NitroPort_PicaInit failed, in the log: the step, and the memory
 * left then. */
static int init_failed(const char *step)
{
    char what[96];
    snprintf(what,sizeof(what),"[gx3d] 3D setup: %s failed",step);
    log_memory(what);
    clean_up();
    return 0;
}
/* The sun shadows' two programs (DVLE 1 and 2 of the shaders), set up the
 * first time the shadows are turned on (NitroPort_PicaLook): most players
 * never turn them on. Tried again each time the shadows are turned on,
 * until they are. (Their DVLEs through NitroPort_PicaDvle: the port's own
 * DVLE_s isn't libctru's size; &sDvlb->DVLE[1] was wrong, and they never
 * set up on a 3DS, shaderProgramSetVsh failing with -2.) */
static int shadow_programs(void)
{
    Result rc;
    if(sCastReady && sRecvReady)return 1;
    if(!sDvlb || sDvlb->numDVLE<3){
        NitroPort_Log("[remaster] the sun shadows' shaders aren't in this build");
        return 0;
    }
    if(!sCastReady){
        shaderProgramInit(&sCastProgram);
        rc=shaderProgramSetVsh(&sCastProgram,(DVLE_s *)NitroPort_PicaDvle(sDvlb,1));
        if(R_FAILED(rc)){
            char what[96];
            shaderProgramFree(&sCastProgram);
            memset(&sCastProgram,0,sizeof(sCastProgram));
            snprintf(what,sizeof(what),"[remaster] the sun shadows' map shader didn't set up (%ld)",(long)rc);
            log_memory(what);
            return 0;
        }
        sCastReady=1;
        sUnifCast=shaderInstanceGetUniformLocation(sCastProgram.vertexShader,"smtx");
    }
    if(!sRecvReady){
        shaderProgramInit(&sRecvProgram);
        rc=shaderProgramSetVsh(&sRecvProgram,(DVLE_s *)NitroPort_PicaDvle(sDvlb,2));
        if(R_FAILED(rc)){
            char what[96];
            shaderProgramFree(&sRecvProgram);
            memset(&sRecvProgram,0,sizeof(sRecvProgram));
            snprintf(what,sizeof(what),"[remaster] the sun shadows' light shader didn't set up (%ld)",(long)rc);
            log_memory(what);
            return 0;
        }
        sRecvReady=1;
        sUnifRecvMtx=shaderInstanceGetUniformLocation(sRecvProgram.vertexShader,"mtx");
        sUnifRecvStereo=shaderInstanceGetUniformLocation(sRecvProgram.vertexShader,"stereo");
        sUnifRecvShadow=shaderInstanceGetUniformLocation(sRecvProgram.vertexShader,"smtx");
    }
    if(sUnifCast<0 || sUnifRecvMtx<0 || sUnifRecvStereo<0 || sUnifRecvShadow<0){
        NitroPort_Log("[remaster] the sun shadows' shaders lack their uniforms (%d %d %d %d)",
                      sUnifCast,sUnifRecvMtx,sUnifRecvStereo,sUnifRecvShadow);
        return 0;
    }
    return 1;
}
int NitroPort_PicaInit(void)
{
    int i;
    C3D_AttrInfo *attrs;
    log_memory("[gx3d] 3D setup");
    /* The command list: 1 MB (citro3d's default, 256 KB, ran out on the
     * 3DS with the remaster's passes, a heavy scene and stereo: citro3d
     * stops the program then, a "Break" on Luma's crash screen). Drawing
     * also stops short of the end (room_for_draw). */
    if(!C3D_Init(CMDBUF_BYTES))return init_failed("citro3d (C3D_Init)");
    /* (the classic path's render target: made by its first scene, VRAM
     * the remaster's screens use otherwise, NitroPort_PicaRender) */
    sDvlb=DVLB_ParseFile((u32*)gx3d_passthrough_shbin,
                         gx3d_passthrough_shbin_size);
    if(!sDvlb)return init_failed("the shaders (DVLB_ParseFile)");
    if(NitroPort_PicaDvleSize()!=sizeof(DVLE_s))
        NitroPort_Log("[gx3d] libctru's DVLE_s is %u bytes, the port's %u (enum sizes): through NitroPort_PicaDvle",
                      NitroPort_PicaDvleSize(),(unsigned)sizeof(DVLE_s));
    if(R_FAILED(shaderProgramInit(&sProgram)))return init_failed("the shader program");
    sProgramReady=1;
    if(R_FAILED(shaderProgramSetVsh(&sProgram,(DVLE_s *)NitroPort_PicaDvle(sDvlb,0))))
        return init_failed("the vertex shader");
    sUnifMtx=shaderInstanceGetUniformLocation(sProgram.vertexShader,"mtx");
    sUnifStereo=shaderInstanceGetUniformLocation(sProgram.vertexShader,"stereo");
    if(sUnifMtx<0||sUnifStereo<0)return init_failed("the vertex shader's uniforms");
    /* (the sun shadows' programs: shadow_programs, when they're first on) */
    C3D_BindProgram(&sProgram);

    attrs=C3D_GetAttrInfo();
    AttrInfo_Init(attrs);
    /* 4xfloat clip XYZ/W, 4xunsigned-byte RGB/A, 2xfloat UV. */
    AttrInfo_AddLoader(attrs,0,GPU_FLOAT,4);
    AttrInfo_AddLoader(attrs,1,GPU_UNSIGNED_BYTE,4);
    AttrInfo_AddLoader(attrs,2,GPU_FLOAT,2);
    C3D_CullFace(GPU_CULL_NONE); /* Stage 1 geometry already culled. */

    for(i=0;i<IMAGE_SLOTS;i++){
        sImages[i]=linearAlloc(IMAGE_BYTES);
        sDepthIds[i]=linearAlloc(IMAGE_BYTES);
        if(!sImages[i]||!sDepthIds[i])return init_failed("the images read back");
        memset(sImages[i],0,IMAGE_BYTES);
    }
    /* The vertices: room after the scene's for the sun shadows' own
     * (SHADOW_VERTICES) if there's memory for it; else none (no
     * shadow-only triangles: sShadowRoom 0). */
    sShadowRoom=SHADOW_VERTICES;
    for(i=0;i<2;i++){
        sVertices[i]=linearAlloc((NITRO_GX3D_PICA_MAX_VERTICES+sShadowRoom)*sizeof(NitroGx3dGpuVertex));
        if(!sVertices[i] && sShadowRoom){
            if(i)linearFree(sVertices[0]);
            sShadowRoom=0;
            i=-1;
            continue;
        }
        if(!sVertices[i])return init_failed("the vertex buffers");
    }
    sBatches=malloc((MAX_BATCHES+SHADOW_BATCHES)*sizeof(*sBatches));
    if(!sBatches)return init_failed("the batches");
    sBuffer=0;
    return 1;
}
void NitroPort_PicaExit(void)
{
    clean_up();
}

static GPU_TEXTURE_WRAP_PARAM wrap_mode(uint32_t param,int repeatBit,int flipBit)
{
    if(!(param&(1u<<repeatBit)))return GPU_CLAMP_TO_EDGE;
    return (param&(1u<<flipBit))?GPU_MIRRORED_REPEAT:GPU_REPEAT;
}

/* Frees the least recently used texture not drawn this frame, to make
 * room. Returns it, or NULL if every texture is in use this frame. One
 * the GPU may still be reading (the frame it's drawing, NitroPort_PicaFrameOpen)
 * only once it's done. */
static TexEntry *evict_one(void)
{
    TexEntry *oldest=NULL;
    int i;
    for(i=0;i<TEX_CACHE_ENTRIES;i++){
        TexEntry *e=&sTextures[i];
        if(!e->used || e->lastFrame==sFrame)continue;
        if(sFramesOwn && (int32_t)(e->lastFrame-sFlightFrame)>0)continue;   /* (this frame's) */
        if(!oldest || e->lastFrame<oldest->lastFrame)oldest=e;
    }
    if(oldest && in_flight(oldest->lastFrame))settle();
    if(oldest)free_texture(oldest);
    return oldest;
}

/* The converted texture for (param, palette), converting it if it's new or
 * its source bytes changed (hashed at most once a frame). Texture data is
 * read here, on the 3D thread, not at SWAP: the game updates textures in
 * V-blank, as on the DS, so this is close enough. NULL if there's no
 * memory for it. */
/* Room for n words in a conversion buffer. */
static uint32_t *scratch(uint32_t **buf,size_t *have,size_t n)
{
    if(*have<n){
        uint32_t *grown=realloc(*buf,n*sizeof(uint32_t));
        if(!grown)return NULL;
        *buf=grown;*have=n;
    }
    return *buf;
}

/* Whether any of n RGBA8 texels isn't fully opaque. */
static int any_holes(const uint32_t *t,size_t n)
{
    size_t i;
    for(i=0;i<n;i++)if((t[i]&0xFFu)!=0xFFu)return 1;
    return 0;
}

/* A texture's texels into its PICA texture: as they are (nearest),
 * through NitroPort_Gx3dFilterTexels for the bilinear filter, or 2x2 until
 * its xBRZ is done (xbrz_work). 0 if there's no memory for the conversion. */
static u32 sConversionSeq;
static int convert_texture(TexEntry *e,unsigned width,unsigned height)
{
    uint32_t *lin,*out;
    if(e->xbrz!=XBRZ_NONE){
        /* (filter3d xbrz) each texel 2x2 for now; the xBRZ later
         * (xbrz_work), once the texture has kept still for a second if
         * it was changed just before (an animation, a fade) */
        u64 now=svcGetSystemTick();
        size_t n=(size_t)width*height;
        out=scratch(&sTexTiled,&sTexTiledWords,n*4u);
        lin=scratch(&sTexLinear,&sTexLinearWords,n);
        if(!lin||!out)return 0;
        NitroPort_Gx3dDecodeTextureTiled(&sBanks,e->param,e->palette,out);
        NitroPort_Gx3dUntileRGBA8(out,width,height,lin);
        e->holes=any_holes(lin,n);
        NitroPort_Gx3dFilterTexelsTiled(lin,width,height,2u,out);
        if(e->packed)NitroPort_Gx3dPackRGBA5551(out,(uint16_t*)e->tex.data,n*4u);
        else memcpy(e->tex.data,out,n*16u);
        e->xbrzAfter=e->conversions && now-e->convertedAt<SYSCLOCK_ARM11 ? now+SYSCLOCK_ARM11 : now;
        e->convertedAt=now;
        e->xbrz=XBRZ_WAITING;
        e->conversions=++sConversionSeq;
        return 1;
    }
    if(e->packed){
        /* (decoded as RGBA8, then each texel's 16 bits: exact for these
         * formats; half what the GPU reads in each of a frame's passes) */
        out=scratch(&sTexTiled,&sTexTiledWords,(size_t)width*height);
        if(!out)return 0;
        NitroPort_Gx3dDecodeTextureTiled(&sBanks,e->param,e->palette,out);
        e->holes=any_holes(out,(size_t)width*height);
        NitroPort_Gx3dPackRGBA5551(out,(uint16_t*)e->tex.data,(size_t)width*height);
        return 1;
    }
    if(!e->filtered){
        NitroPort_Gx3dDecodeTextureTiled(&sBanks,e->param,e->palette,(uint32_t*)e->tex.data);
        e->holes=any_holes((const uint32_t*)e->tex.data,(size_t)width*height);
        return 1;
    }
    /* (the fast decoder gives tiles: back to rows, then filtered and
     * tiled in one pass) */
    out=scratch(&sTexTiled,&sTexTiledWords,(size_t)width*height);
    lin=scratch(&sTexLinear,&sTexLinearWords,(size_t)width*height);
    if(!lin||!out)return 0;
    NitroPort_Gx3dDecodeTextureTiled(&sBanks,e->param,e->palette,out);
    NitroPort_Gx3dUntileRGBA8(out,width,height,lin);
    e->holes=any_holes(lin,(size_t)width*height);
    NitroPort_Gx3dFilterTexelsTiled(lin,width,height,e->scale,(uint32_t*)e->tex.data);
    return 1;
}

/* filter3d xbrz: one texture's xBRZ at a time, a few rows at a time, up
 * to XBRZ_BUDGET of the 3D thread's time a scene (NitroPort_PicaDrawScene).
 * A 256x256 texture takes xBRZ a millisecond or two on a PC, so some tens
 * on the 3DS: too long to wait for in one frame. A new map's textures come
 * out over a second or so instead, each one as it's finished, the most
 * recently drawn first. */
#define XBRZ_BUDGET (SYSCLOCK_ARM11/500u)   /* 2 ms */

static int xbrz_wrap(uint32_t param,int repeatBit,int flipBit)
{
    if(!(param&(1u<<repeatBit)))return NITRO_XBRZ_CLAMP;
    return (param&(1u<<flipBit))?NITRO_XBRZ_MIRROR:NITRO_XBRZ_REPEAT;
}

/* Whether the texture being worked on is still the one it was begun for
 * (not thrown out of the cache, converted again, or replaced). */
static int xbrz_current(void)
{
    TexEntry *e=sXbrz.e;
    return e && e->used && e->xbrz==XBRZ_WAITING && e->conversions==sXbrz.conversions &&
           e->param==sXbrz.param && e->palette==sXbrz.palette;
}

/* Begins the next waiting texture's xBRZ, its texels decoded again (the
 * same as when it was converted, by its hash). 0 if none is waiting. */
static int xbrz_begin(u64 now)
{
    TexEntry *e=NULL;
    unsigned w,h;
    size_t n;
    uint32_t *tiled;
    int i;
    sXbrz.e=NULL;
    for(i=0;i<TEX_CACHE_ENTRIES;i++){
        TexEntry *c=&sTextures[i];
        if(!c->used || c->xbrz!=XBRZ_WAITING || c->xbrzAfter>now)continue;
        if(!e || c->lastFrame>e->lastFrame)e=c;
    }
    if(!e)return 0;
    if(NitroPort_Gx3dTextureHash(&sBanks,e->param,e->palette)!=e->hash){
        /* (changed since: converted again when next drawn) */
        e->xbrzAfter=now+SYSCLOCK_ARM11;
        return 1;
    }
    w=NitroPort_Gx3dTexWidth(e->param);
    h=NitroPort_Gx3dTexHeight(e->param);
    n=(size_t)w*h;
    tiled=scratch(&sTexTiled,&sTexTiledWords,n);
    if(!tiled || !scratch(&sXbrz.src,&sXbrz.srcWords,n) || !scratch(&sXbrz.out,&sXbrz.outWords,n*4u) ||
       !scratch(&sXbrz.info,&sXbrz.infoWords,(n+3u)/4u)){
        e->xbrz=XBRZ_NONE;   /* (no memory for it: it stays 2x2) */
        return 1;
    }
    NitroPort_Gx3dDecodeTextureTiled(&sBanks,e->param,e->palette,tiled);
    NitroPort_Gx3dUntileRGBA8(tiled,w,h,sXbrz.src);
    memset(sXbrz.info,0,n);
    sXbrz.e=e;
    sXbrz.conversions=e->conversions;
    sXbrz.param=e->param;
    sXbrz.palette=e->palette;
    sXbrz.w=w;
    sXbrz.h=h;
    sXbrz.phase=0;
    sXbrz.row=0;
    sXbrz.wrapS=xbrz_wrap(e->param,16,18);
    sXbrz.wrapT=xbrz_wrap(e->param,17,19);
    return 1;
}

/* The finished xBRZ into the texture, in its format; see-through texels
 * coloured as an opaque one beside them (for xbrz smooth's filter: no dark
 * fringes). */
static void xbrz_finish(void)
{
    TexEntry *e=sXbrz.e;
    size_t n=(size_t)sXbrz.w*sXbrz.h*4u;
    uint32_t *tiled=scratch(&sTexTiled,&sTexTiledWords,n);
    sXbrz.e=NULL;
    if(!tiled)return;
    NitroPort_Gx3dFilterTexelsTiled(sXbrz.out,sXbrz.w*2u,sXbrz.h*2u,1u,tiled);
    if(e->packed)NitroPort_Gx3dPackRGBA5551(tiled,(uint16_t*)e->tex.data,n);
    else memcpy(e->tex.data,tiled,n*4u);
    C3D_TexFlush(&e->tex);
    e->xbrz=XBRZ_DONE;
}

/* Up to XBRZ_BUDGET of xBRZ, 4 rows at a time. Where textures are
 * converted (the GPU done with the last frame): their memory is free to
 * write. Its time goes in the [perf] line's texture time. */
static void xbrz_work(void)
{
    u64 start=svcGetSystemTick(),now=start;
    while(now-start<XBRZ_BUDGET){
        unsigned y1;
        if(!xbrz_current()){
            if(!xbrz_begin(now))break;
            if(!sXbrz.e){now=svcGetSystemTick();continue;}
        }
        y1=sXbrz.row+4u;
        if(sXbrz.phase==0){
            NitroPort_Xbrz2xCorners(sXbrz.src,sXbrz.w,sXbrz.h,sXbrz.wrapS,sXbrz.wrapT,1,
                                    (uint8_t*)sXbrz.info,sXbrz.row,y1);
        }else{
            NitroPort_Xbrz2xBlend(sXbrz.src,sXbrz.w,sXbrz.h,sXbrz.wrapS,sXbrz.wrapT,
                                  (const uint8_t*)sXbrz.info,sXbrz.out,sXbrz.row,y1);
        }
        sXbrz.row=y1;
        if(sXbrz.row>=sXbrz.h){
            sXbrz.row=0;
            if(sXbrz.phase++==1)xbrz_finish();
        }
        now=svcGetSystemTick();
    }
    sTexTicks+=now-start;
}

/* The converted texture for (param, palette), converting it if it's new or
 * its source bytes changed (hashed at most once a frame). Texture data is
 * read here, on the 3D thread, not at SWAP: the game updates textures in
 * V-blank, as on the DS, so this is close enough. NULL if there's no
 * memory for it. sBoundCut is set for it. */
static C3D_Tex *get_texture(uint32_t param,uint32_t palette)
{
    TexEntry *e=NULL,*free_slot=NULL;
    unsigned width=NitroPort_Gx3dTexWidth(param);
    unsigned height=NitroPort_Gx3dTexHeight(param);
    size_t bytes=(size_t)width*height*4u;
    uint32_t hash;
    int i;
    sBoundCut=0;
    sBoundHoles=1;
    for(i=0;i<TEX_CACHE_ENTRIES;i++){
        TexEntry *c=&sTextures[i];
        if(c->used && c->param==param && c->palette==palette){e=c;break;}
        if(!c->used && !free_slot)free_slot=c;
    }
    if(e && e->checkedFrame==sFrame){
        e->lastFrame=sFrame;
        sBoundCut=e->filtered?127:0;
        sBoundHoles=e->holes;
        return &e->tex;
    }
    hash=NitroPort_Gx3dTextureHash(&sBanks,param,palette);
    if(e && e->hash==hash){
        e->checkedFrame=e->lastFrame=sFrame;
        sBoundCut=e->filtered?127:0;
        sBoundHoles=e->holes;
        return &e->tex;
    }
    sBoundTexKnown=0;   /* (a texture made again, maybe the bound one's entry) */
    sRecvTexKnown=0;
    /* (changed: made again where it is; drawn since the GPU last caught
     * up, this frame's draws included, while it has a frame) */
    if(e && sFlightFrame!=sSafeFrame && (int32_t)(e->lastFrame-sSafeFrame)>0)settle();
    if(!e){
        /* 2x2 texels a texel when sharp, if it fits (the PICA's 1024 and
         * the cache), else as it is; 16 bits a texel where that's exact */
        unsigned scale=(sTexMode>=2 && width*2u<=1024u && height*2u<=1024u)?2u:1u;
        int packed=(sTexMode==0 || sTexMode>=3) && NitroPort_Gx3dTexIs5551(param);
        if(packed)bytes/=2u;
        if(scale==2u){
            while(sTextureBytes+bytes*4u>TEX_CACHE_BYTES && evict_one()){}
            if(sTextureBytes+bytes*4u>TEX_CACHE_BYTES)scale=1u;
        }
        while(sTextureBytes+bytes*scale*scale>TEX_CACHE_BYTES && evict_one()){}
        if(!free_slot || free_slot->used){
            free_slot=NULL;
            for(i=0;i<TEX_CACHE_ENTRIES;i++)if(!sTextures[i].used){free_slot=&sTextures[i];break;}
        }
        if(!free_slot){
            free_slot=evict_one();
            if(!free_slot)return NULL;
        }
        e=free_slot;
        memset(e,0,sizeof(*e));
        if(!C3D_TexInit(&e->tex,(u16)(width*scale),(u16)(height*scale),packed?GPU_RGBA5551:GPU_RGBA8)){
            if(scale==1u || !C3D_TexInit(&e->tex,(u16)width,(u16)height,GPU_RGBA8))return NULL;
            scale=1u;
        }
        e->scale=scale;
        e->packed=packed;
        e->filtered=sTexMode==1 || sTexMode==2 || sTexMode==4;
        e->xbrz=sTexMode>=3 && scale==2u ? XBRZ_WAITING : XBRZ_NONE;
        if(e->filtered)C3D_TexSetFilter(&e->tex,GPU_LINEAR,GPU_LINEAR);
        else C3D_TexSetFilter(&e->tex,GPU_NEAREST,GPU_NEAREST);
        C3D_TexSetWrap(&e->tex,wrap_mode(param,16,18),wrap_mode(param,17,19));
        e->used=1;
        e->param=param;
        e->palette=palette;
        e->bytes=bytes*scale*scale;
        sTextureBytes+=e->bytes;
    }
    {
        u64 start=svcGetSystemTick();
        if(!convert_texture(e,width,height)){
            free_texture(e);
            return NULL;
        }
        C3D_TexFlush(&e->tex);
        sTexTicks+=svcGetSystemTick()-start;
        sTexTexels+=(u64)width*height;
        sTexConverted++;
    }
    e->hash=hash;
    e->checkedFrame=e->lastFrame=sFrame;
    sBoundCut=e->filtered?127:0;
    sBoundHoles=e->holes;
    return &e->tex;
}

/* Polygon modes (attribute bits 4-5) on the colour combiner. */
static void polygon_combiner(C3D_TexEnv *env,uint32_t attr,int textured,GPU_TEVSRC tex)
{
    unsigned mode=(attr>>4)&3u;
    C3D_TexEnvInit(env);
    if(!textured){
        C3D_TexEnvSrc(env,C3D_Both,GPU_PRIMARY_COLOR,0,0);
        C3D_TexEnvFunc(env,C3D_Both,GPU_REPLACE);
    }else if(mode==1){
        /* Decal: RGB = mix(vertex, texture, texture alpha); alpha from
         * the vertex (the polygon). */
        C3D_TexEnvSrc(env,C3D_RGB,tex,GPU_PRIMARY_COLOR,tex);
        C3D_TexEnvOpRgb(env,GPU_TEVOP_RGB_SRC_COLOR,GPU_TEVOP_RGB_SRC_COLOR,
                        GPU_TEVOP_RGB_SRC_ALPHA);
        C3D_TexEnvFunc(env,C3D_RGB,GPU_INTERPOLATE);
        C3D_TexEnvSrc(env,C3D_Alpha,GPU_PRIMARY_COLOR,0,0);
        C3D_TexEnvFunc(env,C3D_Alpha,GPU_REPLACE);
    }else{
        /* Modulation; also toon/highlight and shadow polygons until their
         * PR, as in the PC reference. */
        C3D_TexEnvSrc(env,C3D_Both,tex,GPU_PRIMARY_COLOR,0);
        C3D_TexEnvFunc(env,C3D_Both,GPU_MODULATE);
    }
}
static void set_combiner(uint32_t attr,int textured)
{
    polygon_combiner(C3D_GetTexEnv(0),attr,textured,GPU_TEXTURE0);
}

/* DS 5-bit colour to 8 bits. */
static u32 widen5(u32 v)
{
    return (v<<3)|(v>>2);
}

/* The scene's depths spread over 0..1 (sDepthA, sDepthK). A vertex's d is
 * -z (W-buffer scenes) or -z / w (Z-buffer ones); the new z keeps that
 * form. W-buffer scenes keep d' <= w (the GPU clips z at -w): d is w /
 * 4096 there, so a scale up to 4096 keeps it. */
static void normalize_depth(NitroGx3dGpuVertex *v,size_t count,uint32_t swap)
{
    int wbuffer=(swap&2u)!=0;
    float lo=1.0f,hi=0.0f,k;
    size_t i;
    for(i=0;i<count;i++){
        float w=v[i].position[3];
        float d=wbuffer? -v[i].position[2] : (w>0.0f? -v[i].position[2]/w : 0.0f);
        if(d<lo)lo=d;
        if(d>hi)hi=d;
    }
    if(!count || hi<=lo){sDepthA=0.0f;sDepthK=1.0f;return;}
    k=1.0f/(hi-lo);
    if(wbuffer && k>4096.0f)k=4096.0f;
    /* Not past 1 (rounding): a hair's margin at the far end. */
    k*=0.999f;
    sDepthA=lo;sDepthK=k;
    for(i=0;i<count;i++){
        float w=v[i].position[3];
        if(wbuffer){
            float d=-v[i].position[2];
            v[i].position[2]=-(d-lo)*k;
        }else{
            /* z = -d w  ->  -(d - lo) k w = k z + lo k w */
            v[i].position[2]=k*v[i].position[2]+lo*k*w;
        }
    }
}

/* The prepared scene's 3D registers (NitroGx3dScene.disp3dcnt, regs3d: as
 * the game had them at its SWAP_BUFFERS). Read live, a scene drawn a few
 * V-blanks after the game made it had the clear colour, the fog and the
 * outlines' colours of the game's scene since. */
static u32 sDisp3d;
static uint8_t sRegs3d[NITRO_GX3D_REGS_SIZE];
static void scene_registers(const NitroGx3dScene *scene)
{
    sDisp3d=scene->disp3dcnt;
    memcpy(sRegs3d,scene->regs3d,sizeof(sRegs3d));
}
/* A register of the prepared scene's: DISP3DCNT (0x60), or 0x320-0x3BF. */
static u32 io32(unsigned offset)
{
    const uint8_t *r;
    if(offset==0x60)return sDisp3d;
    r=&sRegs3d[offset-NITRO_GX3D_REGS_AT];
    return (u32)r[0]|((u32)r[1]<<8)|((u32)r[2]<<16)|((u32)r[3]<<24);
}
static void scene_edge_setup(NitroGx3dEdgeSetup *edges)
{
    NitroPort_Gx3dEdgeSetupFrom(edges,sDisp3d,sRegs3d);
}

/* The DS's fog density (0-128) at a 24-bit depth: FOG_OFFSET, the shift
 * (DISP3DCNT bits 8-11) and the 32 entries of FOG_TABLE, interpolated
 * (as melonDS works it out). */
static unsigned fog_density(u32 depth,u32 offset,unsigned shift,const uint8_t table[34])
{
    u32 id,frac,z;
    unsigned density;
    if(depth<offset){id=0;frac=0;}
    else{
        z=((depth-offset)>>2)<<shift;
        id=z>>17;
        if(id>=32){id=32;frac=0;}
        else frac=z&0x1FFFFu;
    }
    density=(unsigned)(((u32)table[id]*(0x20000u-frac)+(u32)table[id+1]*frac)>>17);
    return density>=127u?128u:density;
}

/* The fog for the prepared scene, from the registers as they are now. */
static void setup_fog(void)
{
    u32 disp=io32(0x60),color=io32(0x358),clear=io32(0x350);
    u32 offset=(io32(0x35C)&0x7FFFu)*0x200u;
    unsigned shift=(disp>>8)&15u,i;
    uint8_t table[34];
    float data[256],f[129];
    /* DISP3DCNT bit 7: fog. Bit 6: alpha only, which the PICA's fog can't
     * do (it fogs the colour): left out. */
    sFogOn=(disp&0x80u) && !(disp&0x40u);
    if(!sFogOn)return;
    table[0]=sRegs3d[0x360-NITRO_GX3D_REGS_AT]&127u;
    for(i=0;i<32;i++)table[i+1]=sRegs3d[0x360-NITRO_GX3D_REGS_AT+i]&127u;
    table[33]=table[32];
    for(i=0;i<=128;i++){
        float d=sDepthA+((float)i/128.0f)/sDepthK;
        u32 depth=d<=0.0f?0u:d>=1.0f?0xFFFFFFu:(u32)(d*16777215.0f);
        /* The PICA's table: 1 keeps the colour, 0 is all fog. */
        f[i]=1.0f-(float)fog_density(depth,offset,shift,table)/128.0f;
    }
    for(i=0;i<128;i++){data[i]=f[i];data[128+i]=f[i+1]-f[i];}
    FogLut_FromArray(&sFogLut,data);
    sFogColor=widen5(color&31u)|(widen5((color>>5)&31u)<<8)|(widen5((color>>10)&31u)<<16);
    /* CLEAR_COLOR bit 15: the rear plane is fogged too, at CLEAR_DEPTH. */
    sFogRear=(clear>>15)&1u;
    sFogDensityRear=(uint8_t)fog_density((io32(0x354)&0x7FFFu)*0x200u+0x1FFu,offset,shift,table);
}

/* The fog mode as last set here; -1 when not known (something else may
 * have set it since). Each change costs a few words of command list, and
 * binding the table costs 130 more: citro3d sends the whole table again
 * on the next fogged draw. Per batch, a fogged scene sent the table for
 * every batch, in each of the five draws of its polygons with the fog
 * (2026-10-15, the opening's forest). Now the table and colour go in once
 * per scene's draw and the mode only when it changes. */
static int sFogMode=-1;

static void fog_mode(int on)
{
    if(on==sFogMode)return;
    C3D_FogGasMode(on?GPU_FOG:GPU_NO_FOG,GPU_PLAIN_DENSITY,false);
    sFogMode=on;
}

/* At the start of a scene's draw: its fog table and colour, fog off. */
static void fog_begin(void)
{
    sFogMode=-1;
    if(sFogOn){
        C3D_FogColor(sFogColor);
        C3D_FogLutBind(&sFogLut);
    }
    fog_mode(0);
}

/* Fog on for a batch's polygons (attribute bit 15) or off. */
static void batch_fog(uint32_t attr)
{
    fog_mode(sFogOn && (attr&(1u<<15)));
}

/* The rear plane's colour with the fog on it, if it's fogged. */
static uint16_t fog_rear(uint16_t color)
{
    unsigned d=sFogDensityRear,k;
    uint16_t out=0;
    if(!sFogOn||!sFogRear||!d)return color;
    for(k=0;k<3;k++){
        unsigned c=(color>>(k*5))&31u;
        unsigned fc=((sFogColor>>(k*8))&255u)>>3;
        out|=(uint16_t)(((fc*d+c*(128u-d))>>7)<<(k*5));
    }
    return out;
}

/* Returns number of GPU vertices on success, -1 if frame submission or
 * transfer failed. 3DS image publication is after GPU/PPF completion only. */
/* Where the 3D thread's time goes, for the [perf] line: building the
 * draws (CPU), waiting for the GPU and the copy, converting the image. */
static volatile u64 sBuildTicks,sWaitTicks,sPublishTicks;
void NitroPort_PicaTimes(u64 *build,u64 *wait,u64 *publish)
{
    *build=sBuildTicks;*wait=sWaitTicks;*publish=sPublishTicks;
}
void NitroPort_PicaTextureStats(u32 *converted,u64 *texels,u64 *ticks)
{
    *converted=sTexConverted;*texels=sTexTexels;*ticks=sTexTicks;
}
int NitroPort_PicaRender(const NitroGx3dScene *scene, unsigned *dropped)
{
    u64 t0=svcGetSystemTick(),t1,t2;
    size_t count,nbatches=0,b;
    unsigned lost=0;
    C3D_BufInfo *buffers;
    int slot,image,blend,edging;
    unsigned idBatches;   /* opaque batches drawn again for their IDs */
    u32 clear,clearColor;
    NitroGx3dEdgeSetup edges;
    if(dropped)*dropped=scene ? scene->dropped : 0;
    if(!scene)return -1;
    scene_registers(scene);
    if(!sTarget){
        static int failed;
        if(failed)return -1;
        sTarget=C3D_RenderTargetCreate(GX3D_W,GX3D_H,GPU_RB_RGBA8,GPU_RB_DEPTH24_STENCIL8);
        if(!sTarget){
            failed=1;
            NitroPort_Log("[gx3d] no VRAM for the classic display's render target: no 3D");
            return -1;
        }
    }
    slot=sBuffer;
    sBuffer^=1;
    sFrame++;
    count=NitroPort_Gx3dBuildBatches(scene,sVertices[slot],
                                     NITRO_GX3D_PICA_MAX_VERTICES,
                                     sBatches,MAX_BATCHES,&nbatches,&lost);
    if(dropped)*dropped+=lost;
    normalize_depth(sVertices[slot],count,scene->swap_param);
    setup_fog();
    NitroPort_Gx3dMapTextureBanks(&sBanks);
    /* CLEAR_COLOR: RGB555, alpha in bits 16-20 (alpha 0 leaves BG0
     * see-through where nothing is drawn). CLEAR_DEPTH is nearly always
     * the far plane; the target clears to the maximum depth. */
    clear=io32(0x350);
    clearColor=(widen5(clear&31u)<<24)|(widen5((clear>>5)&31u)<<16)|
               (widen5((clear>>10)&31u)<<8)|widen5((clear>>16)&31u);
    /* A see-through background clears to black: translucent polygons
     * blended onto it come out premultiplied, which publishing undoes
     * (the DS writes their colour unblended where alpha is 0). */
    if(!((clear>>16)&31u))clearColor=0;
    else{
        uint16_t c=fog_rear((uint16_t)(clear&0x7FFFu));
        clearColor=(widen5(c&31u)<<24)|(widen5((c>>5)&31u)<<16)|
                   (widen5((c>>10)&31u)<<8)|widen5((clear>>16)&31u);
    }
    /* DISP3DCNT bit 3: alpha blending. */
    blend=(sDisp3d&8u)!=0;
    /* Bit 5: edge marking. The rear plane is at the depth this target is
     * cleared to, the farthest. */
    scene_edge_setup(&edges);
    edges.clearDepth=0xFFFFFFu;
    edging=edges.enabled && sEdgeMarking;
    /* Waits for the GPU to finish the previous frame (it has: this thread
     * waited for its transfer). No SYNCDRAW: that waits for a V-blank of
     * both screens, and this image isn't shown by itself; the 2D drawer
     * shows it at its own pace. */
    if(!C3D_FrameBegin(0))return -1;
    /* As in hello3d, schedule framebuffer clear BEFORE drawing on it. */
    /* Depth24/stencil8 memory layout varies by byte packing; fill all
     * bits so the 24-bit depth is unambiguously the maximum (1.0). */
    C3D_RenderTargetClear(sTarget,C3D_CLEAR_ALL,clearColor,0xFFFFFFFFu);
    if(!C3D_FrameDrawOn(sTarget)){
        C3D_FrameEnd(0);
        return -1;
    }
    C3D_BindProgram(&sProgram);
    fog_begin();
    sBoundTexKnown=0;
    {
        /* The DS's clip space is the whole target. */
        C3D_Mtx identity;
        Mtx_Identity(&identity);
        NitroPort_PicaSetView(&identity,0.0f,1.0f);
    }
    NitroPort_PicaBindVertices(sVertices[slot]);
    buffers=C3D_GetBufInfo();
    (void)buffers;
    C3D_CullFace(GPU_CULL_NONE);
    /* W-buffered scenes use the PICA's W-buffer, Z-buffered ones its
     * Z-buffer; NitroPort_Gx3dClipZ gives z to match. */
    C3D_DepthMap(!(scene->swap_param&2u),-1.0f,0.0f);
    /* Texels and polygons with alpha 0 are never drawn on the DS. */
    C3D_AlphaTest(true,GPU_GREATER,0);
    if(count){
        GSPGPU_FlushDataCache(sVertices[slot],
                        (u32)(count*sizeof(NitroGx3dGpuVertex)));
    }
    for(b=0;b<nbatches;b++){
        const NitroGx3dBatch *batch=&sBatches[b];
        int textured=batch->texture_param!=0;
        uint32_t attr=batch->polygon_attr;
        /* Attribute bit 14: draw where the depth is equal (decals on a
         * surface). The DS allows a small tolerance; LEQUAL is closest. */
        GPU_TESTFUNC depthFunc=(attr&(1u<<14))?GPU_LEQUAL:GPU_LESS;
        if(textured){
            C3D_Tex *tex=get_texture(batch->texture_param,batch->palette_base);
            if(!tex){
                /* No memory for it: draw in vertex colour rather than
                 * with whatever texture was bound last. */
                textured=0;
            }else{
                bind_tex(tex);
            }
        }
        set_combiner(attr,textured);
        batch_fog(attr);
        if(!room_for_draw())break;
        /* The stencil buffer keeps the polygon ID of each pixel's last
         * translucent draw, plus 64 (edge marking puts opaque IDs, 0-63,
         * there afterwards), 0xFF after an opaque one (and cleared to
         * it): the DS doesn't draw a translucent pixel over a translucent
         * one of the same polygon ID. Opaque pixels set it to 0xFF. */
        C3D_StencilTest(true,GPU_ALWAYS,0xFF,0xFF,0xFF);
        C3D_StencilOp(GPU_STENCIL_KEEP,GPU_STENCIL_KEEP,GPU_STENCIL_REPLACE);
        if(!batch->translucent){
            C3D_AlphaTest(true,GPU_GREATER,0);
            C3D_AlphaBlend(GPU_BLEND_ADD,GPU_BLEND_ADD,GPU_ONE,GPU_ZERO,
                           GPU_ONE,GPU_ZERO);
            C3D_DepthTest(true,depthFunc,GPU_WRITE_ALL);
            draw_arrays((int)batch->first,(int)batch->count);
            continue;
        }
        /* Translucent pass. A pixel that ends up with alpha 31 counts as
         * opaque on the DS: drawn as is, and it writes depth. A polygon
         * of alpha 31 here is only translucent through its texture, so
         * draw its fully opaque texels first, like opaque polygons ... */
        if(((attr>>16)&31u)==31u){
            C3D_AlphaTest(true,GPU_GREATER,254);
            C3D_AlphaBlend(GPU_BLEND_ADD,GPU_BLEND_ADD,GPU_ONE,GPU_ZERO,
                           GPU_ONE,GPU_ZERO);
            C3D_DepthTest(true,depthFunc,GPU_WRITE_ALL);
            draw_arrays((int)batch->first,(int)batch->count);
        }
        /* Alpha-0 texels aren't drawn at all (nor change the stencil). The
         * opaque texels drawn above fail the depth test this time. */
        C3D_AlphaTest(true,GPU_GREATER,0);
        C3D_StencilTest(true,GPU_NOTEQUAL,(int)(64u|((attr>>24)&63u)),0xFF,0xFF);
        /* ... then the partly transparent ones. They blend when DISP3DCNT
         * says so, otherwise replace what's there, and write depth only
         * with polygon attribute bit 11. (Alpha-0 texels also reach this
         * second draw; blending leaves the pixel as it was.) */
        if(blend){
            C3D_AlphaBlend(GPU_BLEND_ADD,GPU_BLEND_ADD,
                           GPU_SRC_ALPHA,GPU_ONE_MINUS_SRC_ALPHA,
                           GPU_ONE,GPU_ONE_MINUS_SRC_ALPHA);
        }else{
            C3D_AlphaBlend(GPU_BLEND_ADD,GPU_BLEND_ADD,GPU_ONE,GPU_ZERO,
                           GPU_ONE,GPU_ZERO);
        }
        C3D_DepthTest(true,depthFunc,
                      (attr&(1u<<11))?GPU_WRITE_ALL:GPU_WRITE_COLOR);
        draw_arrays((int)batch->first,(int)batch->count);
    }
    /* Edge marking needs each pixel's opaque polygon ID: the opaque
     * pixels drawn again (the same alpha tests), only where they're what
     * is in front (depth equal), writing their polygon ID to the stencil
     * buffer and nothing else. Under a translucent pixel the opaque one's
     * ID goes in too, as on the DS. */
    idBatches=0;
    fog_mode(0);
    if(edging){
        for(b=0;b<nbatches;b++){
            const NitroGx3dBatch *batch=&sBatches[b];
            int textured=batch->texture_param!=0;
            uint32_t attr=batch->polygon_attr;
            /* Translucent batches have opaque pixels only at alpha 31:
             * their fully opaque texels. */
            if(batch->translucent && ((attr>>16)&31u)!=31u)continue;
            if(!room_for_draw())break;
            idBatches++;
            if(textured){
                C3D_Tex *tex=get_texture(batch->texture_param,batch->palette_base);
                if(!tex)textured=0;
                else bind_tex(tex);
            }
            set_combiner(attr,textured);
            C3D_AlphaTest(true,GPU_GREATER,batch->translucent?254:0);
            C3D_DepthTest(true,GPU_EQUAL,0);
            C3D_StencilTest(true,GPU_ALWAYS,(int)((attr>>24)&63u),0xFF,0xFF);
            C3D_StencilOp(GPU_STENCIL_KEEP,GPU_STENCIL_KEEP,GPU_STENCIL_REPLACE);
            draw_arrays((int)batch->first,(int)batch->count);
        }
    }
    /* GX_CMDLIST_FLUSH: flush only the command list. Without it citro3d
     * flushes the CPU cache over all of linear memory every frame; what
     * the GPU reads here is flushed already (vertices above, textures in
     * get_texture). No C3D_FrameSync either: it waits for a V-blank. */
    C3D_FrameEnd(GX_CMDLIST_FLUSH);
    NitroPort_PicaNoteCmdUse();
    t1=svcGetSystemTick();
    /* An image buffer not waiting to be converted nor being converted:
     * with three, there always is one. */
    for(image=0;image<IMAGE_SLOTS-1;image++)
        if(!NitroPort_Gx3dRawInUse(sImages[image]))break;
    /* Off-screen target isn't bound to a 3DS LCD. Copy its tiles to
     * ordinary linear memory: this waits for the drawing to finish, then
     * for the copy (PPF), then the CPU cache is invalidated. */
    GSPGPU_FlushDataCache(sImages[image],(u32)IMAGE_BYTES);
    C3D_SyncDisplayTransfer((u32*)sTarget->frameBuf.colorBuf,
                 GX_BUFFER_DIM(GX3D_W,GX3D_H),sImages[image],
                 GX_BUFFER_DIM(GX3D_W,GX3D_H),GX3D_TRANSFER_FLAGS);
    GSPGPU_InvalidateDataCache(sImages[image],(u32)IMAGE_BYTES);
    if(edging){
        /* The depth/stencil buffer the same way: D24S8, a 32-bit word a
         * pixel, the depth in its low 24 bits and the stencil above. */
        GSPGPU_FlushDataCache(sDepthIds[image],(u32)IMAGE_BYTES);
        C3D_SyncDisplayTransfer((u32*)sTarget->frameBuf.depthBuf,
                     GX_BUFFER_DIM(GX3D_W,GX3D_H),sDepthIds[image],
                     GX_BUFFER_DIM(GX3D_W,GX3D_H),GX3D_TRANSFER_FLAGS);
        GSPGPU_InvalidateDataCache(sDepthIds[image],(u32)IMAGE_BYTES);
        /* Every stencil value must be an ID, an ID + 64 or 255; anything
         * else means the buffer isn't laid out as above. And where opaque
         * polygons were drawn, their IDs must be there: otherwise the ID
         * pass doesn't reach the stencil buffer. Checked on the first
         * scenes with opaque polygons: if either fails, edge marking stays
         * off rather than draw outlines in the wrong places (or spend the
         * time drawing none). */
        if(sEdgeChecks>0 && idBatches>0){
            unsigned odd=NitroPort_Gx3dEdgeIdsUnexpected(sDepthIds[image]),i,ids=0;
            for(i=0;i<GX3D_W*GX3D_H;i++)ids+=(sDepthIds[image][i]>>24)<=63u;
            sEdgeChecks--;
            if(ids)sEdgeScenesWithIds++;
            if(odd){
                sEdgeMarking=0;
                edging=0;
                NitroPort_Log("[gx3d] edge marking off: %u pixels of the depth/stencil buffer read back unexpected (first word %08lx)",
                              odd,(unsigned long)sDepthIds[image][0]);
            }else if(sEdgeChecks==0 && !sEdgeScenesWithIds){
                sEdgeMarking=0;
                edging=0;
                NitroPort_Log("[gx3d] edge marking off: no opaque polygon's ID in the depth/stencil buffer read back, in 60 scenes with opaque polygons (first word %08lx)",
                              (unsigned long)sDepthIds[image][0]);
            }else if(sEdgeChecks==0){
                NitroPort_Log("[gx3d] edge marking on: depth/stencil buffer as expected (%u of 60 scenes with opaque polygons had their IDs in it; the last, %u of %u pixels)",
                              sEdgeScenesWithIds,ids,(unsigned)(GX3D_W*GX3D_H));
            }
        }
    }
    t2=svcGetSystemTick();
    /* Handed over as it is: the 3D screen's drawer converts it (and
     * outlines it) when it draws, on its own core, not this thread on the
     * game's (port_gx3d_image.c). */
    NitroPort_Gx3dSubmitRGBA8(sImages[image],edging?sDepthIds[image]:NULL,edging?&edges:NULL);
    sBuildTicks+=t1-t0;
    sWaitTicks+=t2-t1;
    sPublishTicks+=svcGetSystemTick()-t2;
    return (int)(count/3);
}

/* ------------------------------------------------------- the compose path */

void NitroPort_PicaViewMatrix(C3D_Mtx *m,int screenWidth,int screenHeight,const PicaRect *rect)
{
    /* A 3DS framebuffer is stored in columns (the screen turned a quarter
     * turn): a render target of 240 x screenWidth, where target x runs
     * down the screen and target y along it (citro3d's Mtx_OrthoTilt).
     * Screen pixel (px, py) is at target NDC (1 - 2 py / H, 1 - 2 px / W).
     * A DS clip-space vertex (x, y, z, w), x and y in -w..w over the DS's
     * screen with y up, goes to px * w = x (x1 - x0) / 2 + w (x0 + x1) / 2
     * and py * w = -y (y1 - y0) / 2 + w (y0 + y1) / 2; z is kept (the
     * depth mapping is C3D_DepthMap's). */
    float W=(float)screenWidth,H=(float)screenHeight;
    Mtx_Zeros(m);
    m->r[0].y=(rect->y1-rect->y0)/H;
    m->r[0].w=1.0f-(rect->y0+rect->y1)/H;
    m->r[1].x=-(rect->x1-rect->x0)/W;
    m->r[1].w=1.0f-(rect->x0+rect->x1)/W;
    m->r[2].z=1.0f;
    m->r[3].w=1.0f;
}

void NitroPort_PicaPixelMatrix(C3D_Mtx *m,int screenWidth,int screenHeight)
{
    Mtx_Zeros(m);
    m->r[0].y=-2.0f/(float)screenHeight;
    m->r[0].w=1.0f;
    m->r[1].x=-2.0f/(float)screenWidth;
    m->r[1].w=1.0f;
    m->r[2].z=1.0f;
    m->r[3].w=1.0f;
}

void NitroPort_PicaSetView(const C3D_Mtx *m,float strength,float convergence)
{
    sViewMtx=*m;
    sViewStereo=strength;
    sViewConvergence=convergence;
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER,sUnifMtx,m);
    C3D_FVUnifSet(GPU_VERTEX_SHADER,sUnifStereo,strength,convergence,0.0f,0.0f);
}

void NitroPort_PicaBindVertices(const void *vertices)
{
    C3D_BufInfo *buffers=C3D_GetBufInfo();
    BufInfo_Init(buffers);
    BufInfo_Add(buffers,vertices,sizeof(NitroGx3dGpuVertex),3,0x210u);
}

/* The shadow matrices and bias, all finite (NitroPort_ShadowSetup and
 * NitroPort_ShadowReach divide by extents and ranges). */
static int shadow_finite(void)
{
    int i,j;
    for(i=0;i<4;i++){
        for(j=0;j<4;j++){
            if(!isfinite(sShadow.receiver[i][j]) || !isfinite(sShadow.caster[i][j]))return 0;
        }
    }
    return isfinite(sShadow.bias)!=0;
}
int NitroPort_PicaPrepareScene(const NitroGx3dScene *scene,unsigned *dropped)
{
    u64 t0=svcGetSystemTick();
    unsigned lost=0;
    u32 clear;
    int slot;
    if(dropped)*dropped=scene ? scene->dropped : 0;
    if(!sProgramReady||!scene)return -1;
    scene_registers(scene);
    slot=sBuffer;
    sBuffer^=1;
    sPreparedCount=NitroPort_Gx3dBuildBatches(scene,sVertices[slot],
                                              NITRO_GX3D_PICA_MAX_VERTICES,
                                              sBatches,MAX_BATCHES,&sPreparedBatches,&lost);
    if(dropped)*dropped+=lost;
    normalize_depth(sVertices[slot],sPreparedCount,scene->swap_param);
    if(sPreparedCount){
        GSPGPU_FlushDataCache(sVertices[slot],
                        (u32)(sPreparedCount*sizeof(NitroGx3dGpuVertex)));
    }
    sPrepared=slot;
    sPreparedSerial++;   /* (the outline passes' lists are for another) */
    sPreparedSwap=scene->swap_param;
    sXbrzWorked=0;
    /* sun shadows: if the scene has a ground (a camera's perspective) */
    sShadowDrawn=0;
    sShadowReady=look_now() && sShadowMode && sPreparedCount &&
                 NitroPort_ShadowSetup(sVertices[slot][0].position,sizeof(NitroGx3dGpuVertex),sPreparedCount,
                                       scene->projection,scene->screen_width,&sSun,SHADOW_SIZE,&sShadow);
    sShadowOnlyBatches=0;
    if(sShadowReady){
        /* what only casts (outside the view, or facing away), after the
         * scene's vertices */
        NitroGx3dGpuVertex *extra=sVertices[slot]+sPreparedCount;
        size_t n=NitroPort_Gx3dBuildShadowBatches(scene,sPreparedCount,extra,sShadowRoom,
                                                  sBatches+MAX_BATCHES,SHADOW_BATCHES,&sShadowOnlyBatches);
        size_t cards=0,b;
        /* what casts and what receives (the vertex programs read it) */
        NitroPort_ShadowRoles(sVertices[slot][0].position,&sVertices[slot][0].color[3],
                              sizeof(NitroGx3dGpuVertex),sPreparedCount+n,&sShadow);
        /* characters' and trees' cards: their copies turned to the sun */
        if(sShadowRoom>n){
            n+=NitroPort_ShadowCards(sVertices[slot],sBatches,sPreparedBatches,sPreparedCount+n,extra+n,
                                     sShadowRoom-n,sBatches+MAX_BATCHES+sShadowOnlyBatches,
                                     SHADOW_BATCHES-sShadowOnlyBatches,&cards,&sShadow);
            sShadowOnlyBatches+=cards;
        }
        NitroPort_ShadowReach(extra[0].position,sizeof(NitroGx3dGpuVertex),n,&sShadow);
        GSPGPU_FlushDataCache(sVertices[slot],(u32)((sPreparedCount+n)*sizeof(NitroGx3dGpuVertex)));
        if(!shadow_finite()){
            /* (a NaN or an infinity in a uniform can hang the GPU: 3dbrew) */
            static int told;
            if(!told){told=1;NitroPort_Log("[gx3d] a sun shadow matrix isn't finite: no shadows on this scene");}
            sShadowReady=0;
            sShadowOnlyBatches=0;
        }
        /* the game's own painted shadows (left out in the draw) */
        NitroPort_Gx3dMapTextureBanks(&sBanks);
        for(b=0;b<sPreparedBatches;b++){
            const NitroGx3dBatch *batch=&sBatches[b];
            uint32_t param=batch->texture_param;
            unsigned fmt=NitroPort_Gx3dTexFormat(param);
            size_t texels=fmt?(size_t)NitroPort_Gx3dTexWidth(param)*NitroPort_Gx3dTexHeight(param):0;
            sDecal[b]=0;
            if(!batch->translucent||!batch->count||texels>4096u)continue;
            if(fmt)NitroPort_Gx3dDecodeTexture(&sBanks,param,batch->palette_base,sDecalTexels);
            sDecal[b]=(uint8_t)NitroPort_ShadowDecal(sVertices[slot][batch->first].position,
                                                     sizeof(NitroGx3dGpuVertex),(batch->polygon_attr>>16)&31u,
                                                     fmt==1u||fmt==6u,fmt?sDecalTexels:NULL,texels,
                                                     0x7FFF,&sShadow);
        }
    }
    setup_fog();
    /* CLEAR_COLOR: the rear plane's colour and alpha, as the game has them
     * at this scene (fogged, if it is). */
    clear=io32(0x350)&0xFFFFFFu;
    sRearColor=fog_rear((uint16_t)(clear&0x7FFFu));
    sRearAlpha=(clear>>16)&31u;
    sBuildTicks+=svcGetSystemTick()-t0;
    return (int)(sPreparedCount/3);
}

int NitroPort_PicaEdgeClearValue(u32 *value)
{
    NitroGx3dEdgeSetup edges;
    scene_edge_setup(&edges);
    if(value)*value=((u32)edges.clearId<<24)|0xFFFFFFu;
    return edges.enabled;
}

void NitroPort_PicaRearPlane(uint16_t *color,unsigned *alpha)
{
    if(color)*color=sRearColor;
    if(alpha)*alpha=sRearAlpha;
}

/* The view's scissor (PicaView.scissor: an H-blank effect's run of
 * lines), set again by each pass (C3D_SetViewport turns the scissor off)
 * and after the outline passes' own; off once the scene is drawn. */
static int sScissorOn;
static u32 sScissor[4];
static void view_scissor(void)
{
    if(sScissorOn)C3D_SetScissor(GPU_SCISSOR_NORMAL,sScissor[0],sScissor[1],sScissor[2],sScissor[3]);
    else C3D_SetScissor(GPU_SCISSOR_DISABLE,0,0,0,0);
}
static void scissor_off(void)
{
    if(sScissorOn){
        sScissorOn=0;
        C3D_SetScissor(GPU_SCISSOR_DISABLE,0,0,0,0);
    }
}

/* The state every pass needs: the shader, its view, the vertices; the
 * view's scissor. */
static void begin_pass(const PicaView *view,const PicaRect *rect)
{
    C3D_Mtx m;
    if(view->band && view->bandBottom>view->bandTop){
        /* (PicaView.band) the viewport the run's rows alone, the view
         * moved to match: the screen's row y is the band's y - bandTop */
        PicaRect r=*rect;
        u32 sy=(u32)view->fb->width/(u32)view->screenHeight;
        r.y0-=(float)view->bandTop;
        r.y1-=(float)view->bandTop;
        C3D_SetViewport((u32)(view->screenHeight-view->bandBottom)*sy,0,
                        (u32)(view->bandBottom-view->bandTop)*sy,view->fb->height);
        NitroPort_PicaViewMatrix(&m,view->screenWidth,view->bandBottom-view->bandTop,&r);
    }else{
        NitroPort_PicaViewMatrix(&m,view->screenWidth,view->screenHeight,rect);
    }
    NitroPort_PicaSetView(&m,view->stereo,view->convergence);
    sScissorOn=view->scissor;
    if(sScissorOn)memcpy(sScissor,view->scissorRect,sizeof(sScissor));
    view_scissor();
}

/* The texture, combiner and alpha test of a batch, as the colour pass
 * has them. */
static int bind_batch(const NitroGx3dBatch *batch)
{
    int textured=batch->texture_param!=0;
    sBoundCut=0;
    if(textured){
        C3D_Tex *tex=get_texture(batch->texture_param,batch->palette_base);
        if(!tex){textured=0;sBoundCut=0;}
        else bind_tex(tex);
    }
    set_combiner(batch->polygon_attr,textured);
    return textured;
}

/* For the outline passes, where only a polygon's shape counts: its
 * texture only if it has see-through texels (a tree's cut-out), else none
 * (the texture unit off: most of the scene, the GPU's texture reads saved
 * in 8 of the 9 times a frame's polygons are drawn). Whether textured. */
static int bind_batch_shape(const NitroGx3dBatch *batch)
{
    int textured=batch->texture_param!=0;
    C3D_Tex *tex=NULL;
    sBoundCut=0;
    if(textured){
        tex=get_texture(batch->texture_param,batch->palette_base);
        if(!tex || (!sBoundHoles && !batch->translucent))textured=0;
    }
    bind_tex(textured?tex:NULL);
    if(!textured)sBoundCut=0;
    set_combiner(batch->polygon_attr,textured);
    return textured;
}

/* The combiner giving the edge colour, with the batch's alpha (the
 * texels' or the polygon's) for the alpha test. */
static void set_edge_combiner(int textured,uint16_t color555)
{
    C3D_TexEnv *env=C3D_GetTexEnv(0);
    u32 rgba=widen5(color555&31u)|(widen5((color555>>5)&31u)<<8)|
             (widen5((color555>>10)&31u)<<16)|0xFF000000u;
    C3D_TexEnvInit(env);
    C3D_TexEnvColor(env,rgba);
    C3D_TexEnvSrc(env,C3D_RGB,GPU_CONSTANT,0,0);
    C3D_TexEnvFunc(env,C3D_RGB,GPU_REPLACE);
    C3D_TexEnvSrc(env,C3D_Alpha,textured?GPU_TEXTURE0:GPU_PRIMARY_COLOR,0,0);
    C3D_TexEnvFunc(env,C3D_Alpha,GPU_REPLACE);
}

/* ------------------------------------------- the outline passes, made cheaper */
/* Most of the scene can't get an outline anywhere; NitroPort_Gx3dPruneEdges
 * works out, once a scene (and 3D slider setting), which triangles each
 * outline pass needs, as index lists the GPU reads (linear memory: one
 * buffer a prepared scene's vertex buffer, as they alternate). The same
 * pixels come out as with every triangle in every pass (tests/test_gx3d.c
 * checks that). Off by default (`edges pruned` in remaster.txt): on a New
 * 3DS it took the CPU 5-10 ms a scene (0.1-0.2 ms on a PC) and the moved
 * passes still drew 85-100% of the polygons, so it saved the GPU only 1-2
 * ms (log of 2026-10-19). */
#define PRUNE_INDICES (4u*NITRO_GX3D_PICA_MAX_VERTICES)
static int sPruning=0;   /* (`edges pruned`: see NitroPort_PicaEdgePruning) */
static uint16_t *sPruneIdx[2];
static int sPruneNoMemory;
static NitroGx3dPruneList *sPruneLists;
static uint8_t *sPruneRoles;
static NitroGx3dPruneResult sPrune;
static struct {
    int valid;
    unsigned serial,clearId;
    float stereo,convergence;
    PicaRect rect;
    int width,height;
} sPruneKey;
/* For the [perf] line: the time it took, how often, and the triangles of
 * the outline passes against those drawn in the moved passes (A) and the
 * outlines drawn (D). */
static u64 sPruneTicks;
static unsigned sPruneRuns,sPruneFails,sPruneTriangles,sPruneA,sPruneD;

void NitroPort_PicaEdgePruning(int on)
{
    sPruning=on;
    sPruneKey.valid=0;
}

void NitroPort_PicaPruneStats(float *ms,unsigned *runs,unsigned *fails,float *a,float *d)
{
    if(ms)*ms=sPruneRuns?(float)((double)sPruneTicks*1000.0/SYSCLOCK_ARM11/sPruneRuns):0.0f;
    if(runs)*runs=sPruneRuns;
    if(fails)*fails=sPruneFails;
    if(a)*a=sPruneTriangles?100.0f*(float)sPruneA/(float)sPruneTriangles:0.0f;
    if(d)*d=sPruneTriangles?100.0f*(float)sPruneD/(float)sPruneTriangles:0.0f;
    sPruneTicks=0;
    sPruneRuns=sPruneFails=sPruneTriangles=sPruneA=sPruneD=0;
}

static void free_pruning(void)
{
    int i;
    for(i=0;i<2;i++){
        if(sPruneIdx[i])linearFree(sPruneIdx[i]);
        sPruneIdx[i]=NULL;
    }
    free(sPruneLists);sPruneLists=NULL;
    free(sPruneRoles);sPruneRoles=NULL;
    NitroPort_Gx3dPruneFree();
    sPruneKey.valid=0;
}

/* The prepared scene's lists for this view: NULL if they can't be had
 * (every triangle drawn then). After the opaque polygons: their textures
 * are known (whether they have see-through texels). */
static const NitroGx3dPruneResult *prune_for(const PicaView *view,const NitroGx3dEdgeSetup *edges)
{
    NitroGx3dPruneView pv;
    float stereo=view->stereo<0.0f?-view->stereo:view->stereo;
    u64 t0;
    size_t b;
    int ok;
    if(!sPruning||sPrepared<0||sPruneNoMemory)return NULL;
    if(sPruneKey.valid && sPruneKey.serial==sPreparedSerial && sPruneKey.clearId==edges->clearId &&
       sPruneKey.stereo==stereo && sPruneKey.convergence==view->convergence &&
       sPruneKey.width==view->screenWidth && sPruneKey.height==view->screenHeight &&
       !memcmp(&sPruneKey.rect,&view->rect,sizeof(view->rect)))
        return sPruneKey.valid>0?&sPrune:NULL;
    if(!sPruneIdx[0]){
        sPruneIdx[0]=linearAlloc(PRUNE_INDICES*sizeof(uint16_t));
        sPruneIdx[1]=linearAlloc(PRUNE_INDICES*sizeof(uint16_t));
        sPruneLists=malloc(MAX_BATCHES*sizeof(*sPruneLists));
        sPruneRoles=malloc(MAX_BATCHES);
        if(!sPruneIdx[0]||!sPruneIdx[1]||!sPruneLists||!sPruneRoles){
            NitroPort_Log("[gx3d] no memory for the outline passes' lists: every polygon drawn in them");
            free_pruning();
            sPruneNoMemory=1;
            return NULL;
        }
    }
    t0=svcGetSystemTick();
    /* each batch's part, as bind_batch_shape draws it */
    for(b=0;b<sPreparedBatches;b++){
        const NitroGx3dBatch *batch=&sBatches[b];
        uint32_t attr=batch->polygon_attr;
        if(batch->translucent){
            sPruneRoles[b]=((attr>>16)&31u)==31u?NITRO_GX3D_PRUNE_SHAPE:NITRO_GX3D_PRUNE_OUT;
        }else if(batch->texture_param){
            C3D_Tex *tex=get_texture(batch->texture_param,batch->palette_base);
            sPruneRoles[b]=(tex&&sBoundHoles)?NITRO_GX3D_PRUNE_SHAPE:NITRO_GX3D_PRUNE_SOLID;
        }else{
            sPruneRoles[b]=NITRO_GX3D_PRUNE_SOLID;
        }
    }
    sBoundTexKnown=0;   /* (get_texture may have made one again) */
    pv.x0=view->rect.x0;pv.y0=view->rect.y0;pv.x1=view->rect.x1;pv.y1=view->rect.y1;
    pv.width=view->screenWidth;
    pv.height=view->screenHeight;
    pv.eyes=stereo>0.0f?2:1;
    pv.stereo[0]=-stereo;
    pv.stereo[1]=stereo;
    pv.convergence=view->convergence;
    pv.clear_id=edges->clearId;
    settle();   /* (the frame the GPU has may be reading these lists) */
    ok=NitroPort_Gx3dPruneEdges(sVertices[sPrepared],sBatches,sPreparedBatches,sPruneRoles,&pv,
                                sPruneIdx[sPrepared],PRUNE_INDICES,sPruneLists,&sPrune);
    if(ok && sPrune.indices)GSPGPU_FlushDataCache(sPruneIdx[sPrepared],(u32)(sPrune.indices*sizeof(uint16_t)));
    sPruneTicks+=svcGetSystemTick()-t0;
    sPruneRuns++;
    if(ok){
        sPruneTriangles+=sPrune.triangles;
        sPruneA+=sPrune.a;
        sPruneD+=sPrune.d;
    }else{
        sPruneFails++;
    }
    sPruneKey.valid=ok?1:-1;
    sPruneKey.serial=sPreparedSerial;
    sPruneKey.clearId=edges->clearId;
    sPruneKey.stereo=stereo;
    sPruneKey.convergence=view->convergence;
    sPruneKey.width=view->screenWidth;
    sPruneKey.height=view->screenHeight;
    sPruneKey.rect=view->rect;
    return ok?&sPrune:NULL;
}

static void draw_elements(const uint16_t *indices,unsigned count)
{
    static int told;
    if(!count || count%3u){
        if(!told){told=1;NitroPort_Log("[gx3d] an indexed draw of %u vertices in %s: left out",count,__atomic_load_n(&sStage,__ATOMIC_RELAXED));}
        return;
    }
    __atomic_store_n(&sLastDrawFirst,(u32)indices[0],__ATOMIC_RELAXED);
    __atomic_store_n(&sLastDrawCount,(u32)count,__ATOMIC_RELAXED);
    __atomic_fetch_add(&sDraws,1,__ATOMIC_RELAXED);
    C3D_DrawElements(GPU_TRIANGLES,(int)count,C3D_UNSIGNED_SHORT,indices);
}

/* A batch's part of an outline pass: its whole list (A or D), then each
 * zone's (ZONE_A or ZONE_B) with the scissor test on that zone. */
static void draw_pruned(size_t b,int whole,int zones,const unsigned scissor[4][4])
{
    const NitroGx3dPruneList *l=&sPruneLists[b];
    const uint16_t *idx=sPruneIdx[sPrepared];
    int z,scissored=0;
    if(l->count[whole])draw_elements(idx+l->offset[whole],l->count[whole]);
    for(z=0;z<4;z++){
        unsigned n=l->count[zones+z];
        if(!n || scissor[z][0]>=scissor[z][2] || scissor[z][1]>=scissor[z][3])continue;
        if(!room_for_draw())break;
        C3D_SetScissor(GPU_SCISSOR_NORMAL,scissor[z][0],scissor[z][1],scissor[z][2],scissor[z][3]);
        scissored=1;
        draw_elements(idx+l->offset[zones+z],n);
    }
    if(scissored)view_scissor();
}

/* Whether a batch has anything in an outline pass's lists. */
static int pruned_any(size_t b,int whole,int zones,const unsigned scissor[4][4])
{
    const NitroGx3dPruneList *l=&sPruneLists[b];
    int z;
    if(l->count[whole])return 1;
    for(z=0;z<4;z++)
        if(l->count[zones+z] && scissor[z][0]<scissor[z][2] && scissor[z][1]<scissor[z][3])return 1;
    return 0;
}

/* -------------------------- the left and right moved passes, copied */
/* What a moved pass leaves in its buffer is, at each pixel, the depth and
 * polygon ID of the opaque polygons at its neighbour: the screen's own
 * depth/stencil buffer moved a pixel, once the opaque polygons write their
 * polygon ID as the stencil (which they do while this is on). Turned a
 * quarter turn, the framebuffer's lines run down the screen, 240 pixels
 * each: the neighbours to the left and right are a line away. So the GPU's
 * transfer engine (as fast as the copies to the screens: a fraction of a
 * millisecond) makes those two buffers instead of the scene drawn twice:
 * the screen's buffer untiled into a linear one with a spare line each
 * side (the rear plane, as the edge buffers are cleared to: past the
 * screen's edges, as on the DS), then tiled into each buffer from a line on
 * and a line back. (Which one is left and which right doesn't matter: the
 * outlines drawn test both.) Up and down are 4 bytes away and the engine
 * takes 8-byte addresses: the up one is drawn, and the down one is the
 * up one two pixels on, 8 bytes (derive_down_buffer). Not with the sun
 * shadows on (they use the stencil); `edges drawn` draws all four. */
#define EDGE_LINE 240u                 /* (pixels a framebuffer line: the screen's height) */
#define EDGE_LINES_MAX 400u            /* (lines: the upper screen's width) */
#define EDGE_UNTILE GX3D_TRANSFER_FLAGS
#define EDGE_TILE (GX3D_TRANSFER_FLAGS|GX_TRANSFER_OUT_TILED(1))
static int sEdgeCopy=1;
static int sEdgeCopying;               /* (this frame: NitroPort_PicaEdgeCopies) */
static int sEdgeDeriving;              /* (and the down one from the up one: derive_down_buffer) */
static u32 *sEdgeLinear;               /* (EDGE_LINE x (EDGE_LINES_MAX + 2): VRAM, else the linear heap) */
static int sEdgeLinearInVram;
static int sEdgeLinearFailed;
static u32 sEdgePadValue,sEdgePadLines; /* (what its spare lines hold now: 0, nothing yet) */

void NitroPort_PicaEdgeCopy(int on)
{
    sEdgeCopy=on;
}

unsigned NitroPort_PicaEdgeCopies(int edges)
{
    sEdgeCopying=0;
    if(!edges||!sEdgeCopy||sEdgeLinearFailed||sShadowReady)return 0;
    if(!sEdgeLinear){
        size_t bytes=(size_t)EDGE_LINE*(EDGE_LINES_MAX+2u)*sizeof(u32);
        /* In VRAM, the copies stay in it (in the linear heap they took 1.4
         * ms an eye, 2026-10-19). */
        sEdgeLinear=vramAlloc(bytes);
        sEdgeLinearInVram=sEdgeLinear!=NULL;
        if(!sEdgeLinear)sEdgeLinear=linearAlloc(bytes);
        if(!sEdgeLinear){
            sEdgeLinearFailed=1;
            NitroPort_Log("[gx3d] no memory for the outlines' copies: all their passes drawn");
            return 0;
        }
        NitroPort_Log("[gx3d] the outlines' copies go through %s",sEdgeLinearInVram?"VRAM":"the linear heap");
        if(!sEdgeLinearInVram){
            /* (nothing of it left in the CPU's cache to be written back
             * over what the transfer engine writes) */
            memset(sEdgeLinear,0,bytes);
            GSPGPU_FlushDataCache(sEdgeLinear,(u32)bytes);
        }
        sEdgePadValue=sEdgePadLines=0;
    }
    sEdgeCopying=1;
    /* (the down one from the up one too, but with `edges pruned`: its
     * zones' triangles are drawn only inside their scissors, and two
     * pixels on, the bottom zone's two inner rows would read where they
     * weren't drawn) */
    sEdgeDeriving=!sPruning;
    return sEdgeDeriving?11u:3u;   /* (the directions: 0, 1, and 3) */
}

static void free_edge_copies(void)
{
    if(sEdgeLinear){
        if(sEdgeLinearInVram)vramFree(sEdgeLinear);
        else linearFree(sEdgeLinear);
    }
    sEdgeLinear=NULL;
    sEdgeCopying=0;
}

/* Queued after the opaque polygons (their command list cut off first: the
 * GPU's framebuffer cache is written back at the cut). 0 if they can't be. */
static int copy_edge_buffers(const PicaView *view,u32 clear)
{
    C3D_FrameBuf *fb=view->fb;
    u32 lines=fb->height,dim,i;
    u32 *mid=sEdgeLinear+EDGE_LINE;
    if(!sEdgeLinear||fb->width!=EDGE_LINE||lines>EDGE_LINES_MAX||!view->edgeFb[0]||!view->edgeFb[1])return 0;
    if(sEdgePadValue!=clear||sEdgePadLines!=lines){
        /* the spare lines: in VRAM by a fill (queued ahead of the copies;
         * a frame's GPU queue has room for it), else by the CPU */
        if(sEdgeLinearInVram){
            u32 *after=mid+lines*EDGE_LINE;
            GX_MemoryFill(sEdgeLinear,clear,sEdgeLinear+EDGE_LINE,BIT(0)|(2u<<8),
                          after,clear,after+EDGE_LINE,BIT(0)|(2u<<8));
        }else{
            settle();   /* (the frame the GPU has may be copying through them) */
            for(i=0;i<EDGE_LINE;i++){
                sEdgeLinear[i]=clear;
                mid[lines*EDGE_LINE+i]=clear;
            }
            GSPGPU_FlushDataCache(sEdgeLinear,EDGE_LINE*sizeof(u32));
            GSPGPU_FlushDataCache(mid+lines*EDGE_LINE,EDGE_LINE*sizeof(u32));
        }
        sEdgePadValue=clear;
        sEdgePadLines=lines;
    }
    dim=GX_BUFFER_DIM(EDGE_LINE,lines);
    GX_DisplayTransfer((u32*)fb->depthBuf,dim,mid,dim,EDGE_UNTILE);
    GX_DisplayTransfer(mid+EDGE_LINE,dim,(u32*)view->edgeFb[0]->depthBuf,dim,EDGE_TILE);
    GX_DisplayTransfer(mid-EDGE_LINE,dim,(u32*)view->edgeFb[1]->depthBuf,dim,EDGE_TILE);
    return 1;
}

/* The down buffer (3) from the up one (2), drawn: the same, two pixels
 * along its lines (8 bytes: an address the transfer engine takes),
 * untiled into the linear buffer and tiled back from two pixels on. A
 * line runs up the screen: each line's last two pixels (the screen's two
 * top rows) come from the next line's first two, so the second row gets
 * the rear plane (the up buffer's pixel past the screen's bottom) where
 * it would get the top row, and the top row gets the next column's
 * bottom pixel where it would get the rear plane: the outline the DS
 * draws along the 3D's top edge (against the rear plane past it) comes a
 * row lower. Nothing else changes. (The last line's come from the spare
 * line after it: the rear plane.) Queued after the up buffer's pass (the
 * command list cut: the framebuffer cache written back). It saves a
 * pass: in the field 1.2-1.3 ms, against two transfers. */
static void derive_down_buffer(const PicaView *view)
{
    u32 lines=view->fb->height,dim=GX_BUFFER_DIM(EDGE_LINE,lines);
    u32 *mid=sEdgeLinear+EDGE_LINE;
    GX_DisplayTransfer((u32*)view->edgeFb[2]->depthBuf,dim,mid,dim,EDGE_UNTILE);
    GX_DisplayTransfer(mid+2,dim,(u32*)view->edgeFb[3]->depthBuf,dim,EDGE_TILE);
}

/* Edge marking on the GPU. The DS outlines a pixel of an opaque polygon
 * when a pixel next to it (left, right, above, below) belongs to another
 * polygon ID and is further away (the rear plane, with CLEAR_COLOR's ID
 * and the farthest depth, counts as one; so does the screen's border).
 * For each of the four directions:
 *  - pass A: the opaque polygons drawn again, moved one pixel the other
 *    way, into a depth/stencil buffer of their own (cleared by the caller
 *    to the rear plane, NitroPort_PicaEdgeClearValue: farthest, the clear
 *    ID), depth-tested, writing their IDs to the stencil and nothing to
 *    the colour. Afterwards that buffer holds, at each pixel, the depth
 *    and ID of what's visible at its neighbour. A textured polygon keeps
 *    its texture in both passes: its see-through texels (a tree's
 *    cut-out) are no part of its shape.
 *  - pass B: the opaque polygons drawn in place with that buffer as the
 *    depth/stencil: only where they're nearer than it (the neighbour is
 *    further) and the stencil isn't their ID (another polygon), writing
 *    their edge colour to the screen. (A polygon hidden behind another
 *    one at that pixel could pass too, if it were nearer than the
 *    neighbour: it would then have to lie between the visible surface
 *    and its neighbour's, a hair's breadth; it doesn't happen in practice.)
 * With anti-aliasing (DISP3DCNT bit 4) the outline is blended in at about
 * half strength, as the DS does. Each pass draws only the triangles it
 * needs (prune_for), when they're known. */
static void mark_edges(const PicaView *view,const NitroGx3dEdgeSetup *edges)
{
    static const float dx[4]={-1.0f,1.0f,0.0f,0.0f},dy[4]={0.0f,0.0f,-1.0f,1.0f};
    const NitroGx3dPruneResult *pruned=prune_for(view,edges);
    unsigned scissor[4][4];
    size_t b;
    int dir,z,copied=0;
    u32 clear;
    /* the left and right ones copied (the caller cut the command list) */
    NitroPort_PicaEdgeClearValue(&clear);
    if(sEdgeCopying){
        copied=sEdgeDeriving?11:3;
        if((sEdgeDeriving&&(!view->edgeFb[2]||!view->edgeFb[3]))||!copy_edge_buffers(view,clear)){
            /* (can't be: the caller left them unfilled, so filled here) */
            u32 bytes=(u32)view->fb->width*view->fb->height*4u;
            u8 *a=(u8*)view->edgeFb[0]->depthBuf,*b1=(u8*)view->edgeFb[1]->depthBuf;
            GX_MemoryFill((u32*)a,clear,(u32*)(a+bytes),BIT(0)|(2u<<8),(u32*)b1,clear,(u32*)(b1+bytes),BIT(0)|(2u<<8));
            if(sEdgeDeriving&&view->edgeFb[3]){
                u8 *d=(u8*)view->edgeFb[3]->depthBuf;
                GX_MemoryFill((u32*)d,clear,(u32*)(d+bytes),BIT(0)|(2u<<8),NULL,0,NULL,0);
            }
            copied=0;
        }
    }
    if(pruned)
        for(z=0;z<4;z++)NitroPort_Gx3dPruneScissor(pruned->zone[z],view->screenWidth,view->screenHeight,scissor[z]);
    stage("the outlines");
    fog_mode(0);
    /* (the four A passes first, then the four B passes: each direction has
     * a buffer of its own, and A writes no colour; the same result, and
     * each half of the work timed on its own: NitroPort_PicaProfileMark) */
    for(dir=0;dir<4;dir++){
        PicaRect shifted=view->rect;
        C3D_FrameBuf *fb=view->edgeFb[dir];
        if(fb==NULL || (copied&(1<<dir)))continue;
        /* pass A */
        shifted.x0+=dx[dir];shifted.x1+=dx[dir];
        shifted.y0+=dy[dir];shifted.y1+=dy[dir];
        C3D_SetFrameBuf(fb);
        C3D_SetViewport(0,0,fb->width,fb->height);
        begin_pass(view,&shifted);
        C3D_StencilOp(GPU_STENCIL_KEEP,GPU_STENCIL_KEEP,GPU_STENCIL_REPLACE);
        C3D_AlphaBlend(GPU_BLEND_ADD,GPU_BLEND_ADD,GPU_ONE,GPU_ZERO,GPU_ONE,GPU_ZERO);
        for(b=0;b<sPreparedBatches;b++){
            const NitroGx3dBatch *batch=&sBatches[b];
            uint32_t attr=batch->polygon_attr;
            int textured;
            if(batch->translucent && ((attr>>16)&31u)!=31u)continue;
            if(pruned && !pruned_any(b,NITRO_GX3D_PRUNE_A,NITRO_GX3D_PRUNE_ZONE_A,scissor))continue;
            if(!room_for_draw())break;
            textured=bind_batch_shape(batch);
            C3D_AlphaTest(textured,GPU_GREATER,batch->translucent?254:sBoundCut);
            C3D_DepthTest(true,GPU_LESS,GPU_WRITE_DEPTH);
            C3D_StencilTest(true,GPU_ALWAYS,(int)((attr>>24)&63u),0xFF,0xFF);
            if(pruned)draw_pruned(b,NITRO_GX3D_PRUNE_A,NITRO_GX3D_PRUNE_ZONE_A,scissor);
            else draw_arrays((int)batch->first,(int)batch->count);
        }
    }
    if(copied&8){
        NitroPort_PicaCut("outlines, the shifted passes");
        derive_down_buffer(view);
    }else{
        NitroPort_PicaProfileMark("outlines, the shifted passes");
    }
    for(dir=0;dir<4;dir++){
        C3D_FrameBuf *fb=view->edgeFb[dir];
        if(fb==NULL)continue;
        /* pass B */
        C3D_SetFrameBuf(fb);
        C3D_SetViewport(0,0,fb->width,fb->height);
        begin_pass(view,&view->rect);
        C3D_StencilOp(GPU_STENCIL_KEEP,GPU_STENCIL_KEEP,GPU_STENCIL_KEEP);
        if(edges->antialias){
            C3D_BlendingColor(0x88000000u);   /* 17/32 */
            C3D_AlphaBlend(GPU_BLEND_ADD,GPU_BLEND_ADD,GPU_CONSTANT_ALPHA,GPU_ONE_MINUS_CONSTANT_ALPHA,
                           GPU_ZERO,GPU_ONE);
        }else{
            C3D_AlphaBlend(GPU_BLEND_ADD,GPU_BLEND_ADD,GPU_ONE,GPU_ZERO,GPU_ZERO,GPU_ONE);
        }
        for(b=0;b<sPreparedBatches;b++){
            const NitroGx3dBatch *batch=&sBatches[b];
            uint32_t attr=batch->polygon_attr;
            int textured;
            unsigned id;
            if(batch->translucent && ((attr>>16)&31u)!=31u)continue;
            if(pruned && !pruned_any(b,NITRO_GX3D_PRUNE_D,NITRO_GX3D_PRUNE_ZONE_B,scissor))continue;
            if(!room_for_draw())break;
            id=(attr>>24)&63u;
            textured=bind_batch_shape(batch);
            set_edge_combiner(textured,edges->colors[id>>3]);
            batch_fog(attr);
            C3D_AlphaTest(textured,GPU_GREATER,batch->translucent?254:sBoundCut);
            C3D_DepthTest(true,GPU_LESS,GPU_WRITE_COLOR);
            C3D_StencilTest(true,GPU_NOTEQUAL,(int)id,0xFF,0xFF);
            if(pruned)draw_pruned(b,NITRO_GX3D_PRUNE_D,NITRO_GX3D_PRUNE_ZONE_B,scissor);
            else draw_arrays((int)batch->first,(int)batch->count);
        }
    }
    NitroPort_PicaProfileMark("outlines, the outlines drawn");
    /* back to the screen's own depth/stencil */
    fog_mode(0);
    C3D_SetFrameBuf(view->fb);
    C3D_SetViewport(0,0,view->fb->width,view->fb->height);
    begin_pass(view,&view->rect);
}

/* ------------------------------------------------------------ sun shadows */
/* Parked: since PR #114 the sun shadows hang a 3DS's GPU (the whole
 * console: the HOME menu can't draw either) on their first frame, in the
 * receiving pass. PRs #116 to #119 narrowed it down: even the receiving
 * pass exactly as the #113 build drew it (which worked on a 3DS then)
 * hangs in today's build, so the cause is something PR #114 or #115
 * changed elsewhere, not the pass itself. Until it's found, `shadows on`
 * leaves them off (the colour grade still works); the code stays, and
 * SUN_SHADOWS_PARKED 0 turns them back on for that hunt. The PC runner's
 * simulation (port_gx3d_gpusim.c) still draws them. */
#ifndef SUN_SHADOWS_PARKED
#define SUN_SHADOWS_PARKED 1
#endif

/* (the 3D thread, between frames) The look: sun shadows, the colour
 * grade, the sun by the clock or fixed. The shadow map (512x512, in VRAM)
 * is made the first time the shadows are on. */
void NitroPort_PicaLook(int shadows,int grade,int fixedSun,float elevation,float azimuth)
{
    sSunFixed=fixedSun;
    sFixedSun.elevation=elevation;
    sFixedSun.azimuth=azimuth;
    sGradeMode=grade;
    if(!grade)sGradeWanted=-1;   /* (the textures decoded again at the next scene's first draw) */
    sShadowMode=0;
    sShadowReady=0;
    NitroPort_SetSunShadows(0);
    if(SUN_SHADOWS_PARKED && shadows){
        static int told;
        if(!told){
            told=1;
            NitroPort_Log("[remaster] sun shadows are parked in this build (they hang the 3DS's GPU): off; the colour grade as set");
        }
        return;
    }
    if(!shadows || !shadow_programs())return;
    if(!sShadowTexReady){
        if(!C3D_TexInitShadow(&sShadowTex,SHADOW_SIZE,SHADOW_SIZE)){
            NitroPort_Log("[remaster] no VRAM for the sun shadows' map");
            return;
        }
        sShadowTexReady=1;
        C3D_TexSetFilter(&sShadowTex,GPU_NEAREST,GPU_NEAREST);
        /* past its edges: lit (the farthest distance) */
        C3D_TexSetWrap(&sShadowTex,GPU_CLAMP_TO_BORDER,GPU_CLAMP_TO_BORDER);
        sShadowTex.border=0xFFFFFFFFu;
    }
    if(!sShadowTarget){
        sShadowTarget=C3D_RenderTargetCreateFromTex(&sShadowTex,GPU_TEXFACE_2D,0,-1);
        if(!sShadowTarget){
            NitroPort_Log("[remaster] no render target for the sun shadows' map");
            return;
        }
    }
    if(!make_shadow_spare()){
        free_shadow_spare();
        NitroPort_Log("[remaster] no linear memory for the sun shadows' spare texture");
        return;
    }
    sShadowMode=1;
    NitroPort_SetSunShadows(1);
}

/* For a scene: the time of day's sun (or the fixed one) and grade. 0 if
 * there are no shadows now (the sun's strength nothing: at dusk). */
static int look_now(void)
{
    time_t now=time(NULL);
    struct tm local;
    float strength=1.0f;
    int hour=12,minute=0;
    if(now!=(time_t)-1 && localtime_r(&now,&local)){hour=local.tm_hour;minute=local.tm_min;}
    if(sGradeMode)sGradeWanted=NitroPort_LookPeriod(hour);
    if(sSunFixed)sSun=sFixedSun;
    else NitroPort_ShadowSunAt(hour,minute,&sSun,&strength);
    if(strength<0.02f)return 0;
    sShadowColor=((u32)(255.0f-(255.0f-NITRO_SHADOW_SHADE_B)*strength+0.5f)<<16)|
                 ((u32)(255.0f-(255.0f-NITRO_SHADOW_SHADE_G)*strength+0.5f)<<8)|
                 (u32)(255.0f-(255.0f-NITRO_SHADOW_SHADE_R)*strength+0.5f);
    return 1;
}

/* The shadow map pass's colour and alpha: 0 for the shadow mode's
 * "intensity" (green: 0 keeps the distance, a hard shadow), the texture's
 * alpha for the alpha test (a tree's cut-out casts its shape). */
static void set_cast_combiner(int textured)
{
    C3D_TexEnv *env=C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    C3D_TexEnvColor(env,0xFF000000u);
    C3D_TexEnvSrc(env,C3D_RGB,GPU_CONSTANT,0,0);
    C3D_TexEnvFunc(env,C3D_RGB,GPU_REPLACE);
    C3D_TexEnvSrc(env,C3D_Alpha,textured?GPU_TEXTURE0:GPU_CONSTANT,0,0);
    C3D_TexEnvFunc(env,C3D_Alpha,GPU_REPLACE);
}

/* The prepared scene's opaque polygons from the sun, into the shadow map.
 * Once a scene, before its first draw on a screen. */
static void draw_shadow_map(void)
{
    size_t b;
    int i;
    stage("the shadow map");
    C3D_FrameBufClear(&sShadowTarget->frameBuf,C3D_CLEAR_COLOR,0xFFFFFFFFu,0);
    C3D_SetFrameBuf(&sShadowTarget->frameBuf);
    C3D_SetViewport(0,0,SHADOW_SIZE,SHADOW_SIZE);
    C3D_BindProgram(&sCastProgram);
    for(i=0;i<4;i++){
        C3D_FVUnifSet(GPU_VERTEX_SHADER,sUnifCast+i,sShadow.caster[i][0],sShadow.caster[i][1],
                      sShadow.caster[i][2],sShadow.caster[i][3]);
    }
    NitroPort_PicaBindVertices(sVertices[sPrepared]);
    fog_begin();
    C3D_CullFace(GPU_CULL_NONE);
    C3D_DepthMap(true,-1.0f,0.0f);   /* (distance 0..1 is -z) */
    C3D_DepthTest(false,GPU_ALWAYS,GPU_WRITE_COLOR);
    C3D_StencilTest(false,GPU_ALWAYS,0,0xFF,0xFF);
    C3D_AlphaBlend(GPU_BLEND_ADD,GPU_BLEND_ADD,GPU_ONE,GPU_ZERO,GPU_ONE,GPU_ZERO);
    C3D_FragOpMode(GPU_FRAGOPMODE_SHADOW);
    sBoundTexKnown=0;
    for(b=0;b<sPreparedBatches+sShadowOnlyBatches;b++){
        const NitroGx3dBatch *batch=&sBatches[b<sPreparedBatches?b:MAX_BATCHES+(b-sPreparedBatches)];
        int textured;
        if(batch->translucent)continue;
        if(!room_for_draw())break;
        textured=bind_batch_shape(batch);
        set_cast_combiner(textured);
        C3D_AlphaTest(true,GPU_GREATER,textured?sBoundCut:0);
        draw_arrays((int)batch->first,(int)batch->count);
    }
    C3D_FragOpMode(GPU_FRAGOPMODE_GL);
    sBoundTexKnown=0;
}

/* Keep unit 1 valid after the receiver pass. C3D_TexBind(1, NULL)
 * dereferences NULL in citro3d. All three programs also output texcoord1
 * while this unit remains enabled, including the compositor's program. */
static C3D_Tex sSpareTex;
static int sSpareReady;
static int sShadowDebug;

void NitroPort_PicaShadowDebug(int enabled)
{
    sShadowDebug=enabled!=0;
    if(sShadowDebug)NitroPort_Log("[gx3d] shadowdebug on: GPU completion waits between shadow passes");
}

/* An actual completion boundary, unlike a CPU-side stage marker.
 * FrameEnd(0) flushes ALL linear data: the compositor may already have
 * queued underlay vertices which it normally flushes at the frame's end.
 * FrameBegin waits for this submission before resetting the command
 * buffer. The compositor still owns an open frame on return. Debug only:
 * these waits deliberately sacrifice overlap and are not a perf mode. */
static void shadow_checkpoint(const char *name)
{
    /* (not with the remaster's own frames, NitroPort_PicaFrameOpen: there
     * are no citro3d frames to end; the sun shadows are parked anyway) */
    if(!sShadowDebug || sFramesOwn)return;
    stage(name);
    C3D_FrameEnd(0);
    NitroPort_PicaNoteCmdUse();
    C3D_FrameBegin(0);
}

static int make_shadow_spare(void)
{
    if(sSpareReady)return 1;
    if(!C3D_TexInit(&sSpareTex,8,8,GPU_RGBA8))return 0;
    memset(sSpareTex.data,0xFF,8*8*4);
    GSPGPU_FlushDataCache(sSpareTex.data,8*8*4);
    sSpareReady=1;
    return 1;
}

static void free_shadow_spare(void)
{
    if(sSpareReady){
        C3D_TexDelete(&sSpareTex);
        memset(&sSpareTex,0,sizeof(sSpareTex));
    }
    sSpareReady=0;
}

/* Unit 0 is the sun's depth lookup, unit 1 the polygon's cut-out.
 * RGB = white * light + shade * (1 - light). Constant alpha is zero,
 * so ONE_MINUS_SRC_ALPHA supplies white without another TEV stage.
 * Alpha retains the cut-out test; destination alpha is never changed. */
static void bind_batch_shadow(const NitroGx3dBatch *batch)
{
    int textured=batch->texture_param!=0;
    unsigned mode=(batch->polygon_attr>>4)&3u;
    C3D_Tex *tex=NULL;
    C3D_TexEnv *env;
    sBoundCut=0;
    if(textured){
        tex=get_texture(batch->texture_param,batch->palette_base);
        if(!tex){textured=0;sBoundCut=0;}
    }
    if(textured && (!sRecvTexKnown || tex!=sRecvTex)){
        C3D_TexBind(1,tex);
        sRecvTex=tex;
        sRecvTexKnown=1;
    }
    env=C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    C3D_TexEnvColor(env,sShadowColor);
    C3D_TexEnvSrc(env,C3D_RGB,GPU_CONSTANT,GPU_CONSTANT,GPU_TEXTURE0);
    C3D_TexEnvOpRgb(env,GPU_TEVOP_RGB_ONE_MINUS_SRC_ALPHA,
                    GPU_TEVOP_RGB_SRC_COLOR,GPU_TEVOP_RGB_SRC_COLOR);
    C3D_TexEnvFunc(env,C3D_RGB,GPU_INTERPOLATE);
    if(textured && mode!=1){
        C3D_TexEnvSrc(env,C3D_Alpha,GPU_TEXTURE1,GPU_PRIMARY_COLOR,0);
        C3D_TexEnvFunc(env,C3D_Alpha,GPU_MODULATE);
    }else{
        C3D_TexEnvSrc(env,C3D_Alpha,GPU_PRIMARY_COLOR,0,0);
        C3D_TexEnvFunc(env,C3D_Alpha,GPU_REPLACE);
    }
}

/* Hardware baseline: draw the receivers directly over the opaque scene,
 * at the SAME resolution and with the SAME position/depth calculation.
 * Depth EQUAL shades only the visible surface. No depth/stencil writes,
 * no half-size mask targets, blur quads or mask depth buffer.
 *
 * One receiver per visible pixel: the opaque pass left stencil 0xFF.
 * Mark shaded pixels 0xFE, so coplanar overlapping batches cannot multiply
 * the shade twice. The later translucent pass still has its own IDs. */
static void draw_shadow_receivers(const PicaView *view)
{
    size_t b;
    int q;
    stage("the direct sun shadow receivers");
    C3D_SetFrameBuf(view->fb);
    C3D_SetViewport(0,0,view->fb->width,view->fb->height);
    C3D_BindProgram(&sProgram);
    begin_pass(view,&view->rect);
    C3D_BindProgram(&sRecvProgram);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER,sUnifRecvMtx,&sViewMtx);
    C3D_FVUnifSet(GPU_VERTEX_SHADER,sUnifRecvStereo,sViewStereo,sViewConvergence,0.0f,0.0f);
    for(q=0;q<4;q++){
        C3D_FVUnifSet(GPU_VERTEX_SHADER,sUnifRecvShadow+q,sShadow.receiver[q][0],sShadow.receiver[q][1],
                      sShadow.receiver[q][2],sShadow.receiver[q][3]);
    }
    NitroPort_PicaBindVertices(sVertices[sPrepared]);
    /* This is a different target from the map; binding flushes citro3d's
     * texture cache after the map's framebuffer writes. */
    C3D_TexBind(0,&sShadowTex);
    C3D_TexBind(1,&sSpareTex);
    C3D_TexShadowParams(false,sShadow.bias*NITRO_SHADOW_BIAS_SCALE);
    sBoundTexKnown=0;
    sRecvTexKnown=0;
    fog_begin();
    C3D_CullFace(GPU_CULL_NONE);
    C3D_DepthMap(!(sPreparedSwap&2u),-1.0f,0.0f);
    C3D_DepthTest(true,GPU_EQUAL,GPU_WRITE_COLOR);
    /* Compare only bit 0: reference 0xFE compares as 0, whereas the
     * opaque pass's 0xFF compares as 1. REPLACE writes 0xFE after shading. */
    C3D_StencilTest(true,GPU_NOTEQUAL,0xFE,0x01,0xFF);
    C3D_StencilOp(GPU_STENCIL_KEEP,GPU_STENCIL_KEEP,GPU_STENCIL_REPLACE);
    C3D_AlphaBlend(GPU_BLEND_ADD,GPU_BLEND_ADD,GPU_DST_COLOR,GPU_ZERO,GPU_ZERO,GPU_ONE);
    for(b=0;b<sPreparedBatches;b++){
        const NitroGx3dBatch *batch=&sBatches[b];
        if(batch->translucent)continue;
        if(!room_for_draw())break;
        bind_batch_shadow(batch);
        C3D_AlphaTest(true,GPU_GREATER,sBoundCut);
        draw_arrays((int)batch->first,(int)batch->count);
    }
    C3D_TexBind(1,&sSpareTex);
    sRecvTexKnown=0;
    C3D_TexBind(0,NULL);
    sBoundTexKnown=0;
    shadow_checkpoint("waiting for the direct sun shadow receivers");
    /* The opaque texels, edge marking and translucent passes use the
     * scene's own program and may write stencil again. */
    C3D_BindProgram(&sProgram);
    begin_pass(view,&view->rect);
    C3D_StencilOp(GPU_STENCIL_KEEP,GPU_STENCIL_KEEP,GPU_STENCIL_REPLACE);
}

void NitroPort_PicaDrawScene(const PicaView *view)
{
    size_t b;
    int blend,edging,copying,part,depthOnly;
    NitroGx3dEdgeSetup edges;
    if(sPrepared<0||!view||!view->fb)return;
    part=view->part;
    /* (PICA_DRAW_AFTER: the opaque polygons' depth and IDs alone, the
     * colour already there, scaled down from the 2x2 framebuffer) */
    depthOnly=part==PICA_DRAW_AFTER;
    if(!depthOnly && !view->again)sFrame++;   /* (once a frame) */
    NitroPort_Gx3dMapTextureBanks(&sBanks);
    if(!sXbrzWorked){
        sXbrzWorked=1;   /* (once a scene: its first draw) */
        apply_grade();
        if(sTexMode>=3){
            settle();   /* (xBRZ redoes textures where they are) */
            xbrz_work();
        }
    }
    blend=(sDisp3d&8u)!=0;
    scene_edge_setup(&edges);
    edging=edges.enabled && view->edgeFb[0]!=NULL && part!=PICA_DRAW_OPAQUE;
    /* (the opaque polygons' stencil is their polygon ID, for the copies) */
    copying=edging && sEdgeCopying && !sShadowReady;
    if(sShadowReady && !sShadowDrawn){
        sShadowDrawn=1;
        shadow_checkpoint("waiting before the sun shadow map");
        draw_shadow_map();
        shadow_checkpoint("waiting for the sun shadow map");
    }
    C3D_BindProgram(&sProgram);
    stage("the opaque polygons");
    fog_begin();
    sBoundTexKnown=0;
    NitroPort_PicaBindVertices(sVertices[sPrepared]);
    C3D_SetFrameBuf(view->fb);
    C3D_SetViewport(0,0,view->fb->width,view->fb->height);
    begin_pass(view,&view->rect);
    C3D_CullFace(GPU_CULL_NONE);
    C3D_DepthMap(!(sPreparedSwap&2u),-1.0f,0.0f);
    C3D_AlphaTest(true,GPU_GREATER,0);
    /* The opaque polygons, as the classic path draws them (see there);
     * then the sun shadows' light over them, if there are. */
    C3D_StencilOp(GPU_STENCIL_KEEP,GPU_STENCIL_KEEP,GPU_STENCIL_REPLACE);
    for(b=0;b<sPreparedBatches;b++){
        const NitroGx3dBatch *batch=&sBatches[b];
        uint32_t attr=batch->polygon_attr;
        GPU_TESTFUNC depthFunc=(attr&(1u<<14))?GPU_LEQUAL:GPU_LESS;
        if(batch->translucent)continue;
        if(!room_for_draw())break;
        if(depthOnly){
            /* (as the outlines' moved passes draw them: the shape alone) */
            int textured=bind_batch_shape(batch);
            C3D_StencilTest(true,GPU_ALWAYS,copying?(int)((attr>>24)&63u):0xFF,0xFF,0xFF);
            C3D_AlphaTest(true,GPU_GREATER,textured?sBoundCut:0);
            C3D_AlphaBlend(GPU_BLEND_ADD,GPU_BLEND_ADD,GPU_ONE,GPU_ZERO,GPU_ONE,GPU_ZERO);
            C3D_DepthTest(true,depthFunc,GPU_WRITE_DEPTH);
            draw_arrays((int)batch->first,(int)batch->count);
            continue;
        }
        bind_batch(batch);
        batch_fog(attr);
        C3D_StencilTest(true,GPU_ALWAYS,copying?(int)((attr>>24)&63u):0xFF,0xFF,0xFF);
        /* (a filtered texture: its texels' edges at half alpha) */
        C3D_AlphaTest(true,GPU_GREATER,sBoundCut);
        C3D_AlphaBlend(GPU_BLEND_ADD,GPU_BLEND_ADD,GPU_ONE,GPU_ZERO,GPU_ONE,GPU_ZERO);
        C3D_DepthTest(true,depthFunc,GPU_WRITE_ALL);
        draw_arrays((int)batch->first,(int)batch->count);
    }
    if(sShadowReady && !depthOnly){
        shadow_checkpoint("waiting for the opaque scene before sun shadows");
        draw_shadow_receivers(view);
    }
    /* Their fully opaque texels of translucent polygons of alpha 31 are
     * opaque too (drawn here so the outlines go over them as well). */
    stage("the opaque texels of translucent polygons");
    for(b=0;b<sPreparedBatches;b++){
        const NitroGx3dBatch *batch=&sBatches[b];
        uint32_t attr=batch->polygon_attr;
        GPU_TESTFUNC depthFunc=(attr&(1u<<14))?GPU_LEQUAL:GPU_LESS;
        if(!batch->translucent || ((attr>>16)&31u)!=31u)continue;
        if(sShadowReady && sDecal[b])continue;
        if(!room_for_draw())break;
        if(depthOnly){
            int textured=bind_batch_shape(batch);
            C3D_StencilTest(true,GPU_ALWAYS,copying?(int)((attr>>24)&63u):0xFF,0xFF,0xFF);
            C3D_AlphaTest(textured,GPU_GREATER,254);
            C3D_AlphaBlend(GPU_BLEND_ADD,GPU_BLEND_ADD,GPU_ONE,GPU_ZERO,GPU_ONE,GPU_ZERO);
            C3D_DepthTest(true,depthFunc,GPU_WRITE_DEPTH);
            draw_arrays((int)batch->first,(int)batch->count);
            continue;
        }
        bind_batch(batch);
        batch_fog(attr);
        C3D_StencilTest(true,GPU_ALWAYS,copying?(int)((attr>>24)&63u):0xFF,0xFF,0xFF);
        C3D_AlphaTest(true,GPU_GREATER,254);
        C3D_AlphaBlend(GPU_BLEND_ADD,GPU_BLEND_ADD,GPU_ONE,GPU_ZERO,GPU_ONE,GPU_ZERO);
        C3D_DepthTest(true,depthFunc,GPU_WRITE_ALL);
        draw_arrays((int)batch->first,(int)batch->count);
    }
    if(part==PICA_DRAW_OPAQUE){
        /* (the rest after the caller has scaled it down) */
        fog_mode(0);
        C3D_StencilTest(false,GPU_ALWAYS,0,0xFF,0xFF);
        C3D_DepthTest(false,GPU_ALWAYS,GPU_WRITE_COLOR);
        C3D_AlphaTest(false,GPU_ALWAYS,0);
        NitroPort_PicaProfileMark("the opaque polygons");
        stage("the scene's opaque polygons drawn");
        scissor_off();
        return;
    }
    /* (with the copies: cut here, always, for them to come after) */
    if(copying)NitroPort_PicaCut(depthOnly?"the opaque polygons' depth":"the opaque polygons");
    else NitroPort_PicaProfileMark(depthOnly?"the opaque polygons' depth":"the opaque polygons");
    if(edging)mark_edges(view,&edges);
    stage("the translucent polygons");
    /* The translucent polygons, over the outlines (but the game's own
     * painted shadows, with the sun shadows on). */
    for(b=0;b<sPreparedBatches;b++){
        const NitroGx3dBatch *batch=&sBatches[b];
        uint32_t attr=batch->polygon_attr;
        GPU_TESTFUNC depthFunc=(attr&(1u<<14))?GPU_LEQUAL:GPU_LESS;
        if(!batch->translucent)continue;
        if(sShadowReady && sDecal[b])continue;
        if(!room_for_draw())break;
        bind_batch(batch);
        batch_fog(attr);
        C3D_StencilOp(GPU_STENCIL_KEEP,GPU_STENCIL_KEEP,GPU_STENCIL_REPLACE);
        C3D_AlphaTest(true,GPU_GREATER,0);
        C3D_StencilTest(true,GPU_NOTEQUAL,(int)(64u|((attr>>24)&63u)),0xFF,0xFF);
        if(blend){
            C3D_AlphaBlend(GPU_BLEND_ADD,GPU_BLEND_ADD,
                           GPU_SRC_ALPHA,GPU_ONE_MINUS_SRC_ALPHA,
                           GPU_ONE,GPU_ONE_MINUS_SRC_ALPHA);
        }else{
            C3D_AlphaBlend(GPU_BLEND_ADD,GPU_BLEND_ADD,GPU_ONE,GPU_ZERO,
                           GPU_ONE,GPU_ZERO);
        }
        C3D_DepthTest(true,depthFunc,
                      (attr&(1u<<11))?GPU_WRITE_ALL:GPU_WRITE_COLOR);
        draw_arrays((int)batch->first,(int)batch->count);
    }
    /* Leave the GPU state plain for the compositor's quads. */
    fog_mode(0);
    C3D_StencilTest(false,GPU_ALWAYS,0,0xFF,0xFF);
    C3D_DepthTest(false,GPU_ALWAYS,GPU_WRITE_COLOR);
    C3D_AlphaTest(false,GPU_ALWAYS,0);
    scissor_off();
    NitroPort_PicaProfileMark("the translucent polygons");
    stage("the scene drawn");
}
#endif
