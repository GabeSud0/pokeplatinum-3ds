/*
 * Stage 2 PR1: thin PICA200 backend. Only the dedicated 3D thread calls
 * functions in this file: no GPU commands run in the game or 2D threads.
 * Renders a 256x192 off-screen RGBA8 target with a 24-bit depth buffer.
 * GX_DisplayTransfer untile-copies into two linear RGBA8 buffers; after
 * completion, the shared image API publishes a CPU-readable RGB555 image.
 * Textures, transparency and polygon modes are subsequent PRs.
 */
#ifdef __3DS__
#include <3ds.h>
#include <citro3d.h>
#include <nitro_port_gx3d.h>
#include <nitro_port_gx3d_vertex.h>
#include <stdlib.h>
#include <string.h>

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

/* bin2s emits these from picasso's gx3d_passthrough.shbin. */
extern const u8 gx3d_passthrough_shbin[];
extern const u32 gx3d_passthrough_shbin_size;

/* All of these are worker-owned; neither game nor compositor touches them. */
static C3D_RenderTarget *sTarget;
static DVLB_s *sDvlb;
static shaderProgram_s sProgram;
static int sProgramReady;
static NitroGx3dGpuVertex *sVertices[2];
static u32 *sImages[2];
static int sBuffer;

static void clean_up(void)
{
    int i;
    for(i=0;i<2;i++){
        if(sVertices[i])linearFree(sVertices[i]);
        if(sImages[i])linearFree(sImages[i]);
        sVertices[i]=NULL;sImages[i]=NULL;
    }
    if(sProgramReady){
        shaderProgramFree(&sProgram);
        sProgramReady=0;
    }
    if(sDvlb){DVLB_Free(sDvlb);sDvlb=NULL;}
    if(sTarget){C3D_RenderTargetDelete(sTarget);sTarget=NULL;}
    C3D_Fini();
}
int NitroPort_PicaInit(void)
{
    int i;
    C3D_AttrInfo *attrs;
    C3D_TexEnv *env;
    if(!C3D_Init(C3D_DEFAULT_CMDBUF_SIZE))return 0;
    sTarget=C3D_RenderTargetCreate(GX3D_W,GX3D_H,
                     GPU_RB_RGBA8,GPU_RB_DEPTH24_STENCIL8);
    if(!sTarget)goto fail;
    sDvlb=DVLB_ParseFile((u32*)gx3d_passthrough_shbin,
                         gx3d_passthrough_shbin_size);
    if(!sDvlb)goto fail;
    if(R_FAILED(shaderProgramInit(&sProgram)))goto fail;
    sProgramReady=1;
    if(R_FAILED(shaderProgramSetVsh(&sProgram,&sDvlb->DVLE[0])))
        goto fail;
    C3D_BindProgram(&sProgram);

    attrs=C3D_GetAttrInfo();
    AttrInfo_Init(attrs);
    /* 4xfloat clip XYZ/W, 4xunsigned-byte RGB/A, 2xfloat UV. */
    AttrInfo_AddLoader(attrs,0,GPU_FLOAT,4);
    AttrInfo_AddLoader(attrs,1,GPU_UNSIGNED_BYTE,4);
    AttrInfo_AddLoader(attrs,2,GPU_FLOAT,2);
    env=C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env,C3D_Both,GPU_PRIMARY_COLOR,0,0);
    C3D_TexEnvFunc(env,C3D_Both,GPU_REPLACE);
    C3D_CullFace(GPU_CULL_NONE); /* Stage 1 geometry already culled. */
    /* The vertex builder outputs z=-d*w (d: near 0 ... far 1).
     * citro3d maps this to stored depth d, so use LESS + max clear. */
    C3D_DepthTest(true,GPU_LESS,GPU_WRITE_ALL);

    for(i=0;i<2;i++){
        sVertices[i]=linearAlloc(NITRO_GX3D_PICA_MAX_VERTICES*
                                 sizeof(NitroGx3dGpuVertex));
        sImages[i]=linearAlloc(IMAGE_BYTES);
        if(!sVertices[i]||!sImages[i])goto fail;
        memset(sImages[i],0,IMAGE_BYTES);
    }
    sBuffer=0;
    return 1;
fail:
    clean_up();
    return 0;
}
void NitroPort_PicaExit(void)
{
    clean_up();
}
/* Returns number of GPU vertices on success, -1 if frame submission or
 * transfer failed. 3DS image publication is after GPU/PPF completion only. */
int NitroPort_PicaRender(const NitroGx3dScene *scene, unsigned *dropped)
{
    size_t count;
    C3D_BufInfo *buffers;
    int slot;
    Result result;
    if(dropped)*dropped=scene ? scene->dropped : 0;
    if(!sTarget||!scene)return -1;
    slot=sBuffer;
    sBuffer^=1;
    count=NitroPort_Gx3dBuildOpaqueVertices(scene,sVertices[slot],
                                             NITRO_GX3D_PICA_MAX_VERTICES);
    if(dropped)*dropped+=(unsigned)NitroPort_Gx3dCountDroppedTriangles(
        scene,NITRO_GX3D_PICA_MAX_VERTICES);
    /* Previous GPU frame must have finished using this buffer. This worker
     * may wait; neither the game nor the 2D drawer ever waits on it. */
    if(!C3D_FrameBegin(C3D_FRAME_SYNCDRAW))return -1;
    /* As in hello3d, schedule framebuffer clear BEFORE drawing on it. */
    C3D_RenderTargetClear(sTarget,C3D_CLEAR_ALL,0,0x00FFFFFFu);
    if(!C3D_FrameDrawOn(sTarget)){
        C3D_FrameEnd(0);
        return -1;
    }
    C3D_BindProgram(&sProgram);
    buffers=C3D_GetBufInfo();
    BufInfo_Init(buffers);
    BufInfo_Add(buffers,sVertices[slot],sizeof(NitroGx3dGpuVertex),
                3,0x210u);
    C3D_CullFace(GPU_CULL_NONE);
    C3D_DepthTest(true,GPU_LESS,GPU_WRITE_ALL);
    /* The first plumbing PR clears transparent black. Mapping DS
     * CLEAR_COLOR/DEPTH and alpha are PR3, not part of this pipeline. */
    if(count){
        GSPGPU_FlushDataCache(sVertices[slot],
                        (u32)(count*sizeof(NitroGx3dGpuVertex)));
        C3D_DrawArrays(GPU_TRIANGLES,0,(int)count);
    }
    C3D_FrameEnd(0);
    C3D_FrameSync();
    /* Off-screen target isn't bound to a 3DS LCD. Copy tiles to ordinary
     * linear memory, wait for the PPF event, then invalidate the CPU cache. */
    GSPGPU_FlushDataCache(sImages[slot],(u32)IMAGE_BYTES);
    result=GX_DisplayTransfer((u32*)sTarget->frameBuf.colorBuf,
                 GX_BUFFER_DIM(GX3D_W,GX3D_H),sImages[slot],
                 GX_BUFFER_DIM(GX3D_W,GX3D_H),GX3D_TRANSFER_FLAGS);
    if(R_FAILED(result))return -1;
    gspWaitForPPF();
    GSPGPU_InvalidateDataCache(sImages[slot],(u32)IMAGE_BYTES);
    NitroPort_Gx3dPublishRGBA8(sImages[slot],GX3D_W*GX3D_H);
    return (int)(count/3);
}
#endif
