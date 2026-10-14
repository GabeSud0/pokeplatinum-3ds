/*
 * Stage 2: the PICA200 backend. Only the dedicated 3D thread calls
 * functions in this file: no GPU commands run in the game or 2D threads.
 * Renders a 256x192 off-screen RGBA8 target with a 24-bit depth buffer.
 * GX_DisplayTransfer untile-copies into two linear RGBA8 buffers; after
 * completion, the shared image API publishes a CPU-readable RGB555 image.
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
#include <3ds.h>
#include <citro3d.h>
#include <nitro_port_gx3d.h>
#include <nitro_port_gx3d_texture.h>
#include <nitro_port_gx3d_vertex.h>
#include <stdlib.h>
#include <string.h>

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

/* A scene has at most 4096 triangles; a batch per triangle at worst. */
#define MAX_BATCHES NITRO_GX3D_MAX_TRIANGLES

/* The texture cache: converted textures stay while their source bytes hash
 * the same. Least recently used ones go when over budget. */
#define TEX_CACHE_ENTRIES 256
#define TEX_CACHE_BYTES   (8u*1024u*1024u)

typedef struct TexEntry {
    int used;
    uint32_t param, palette, hash;
    uint32_t checkedFrame, lastFrame;
    size_t bytes;
    C3D_Tex tex;
} TexEntry;

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
/* The depth/stencil buffer, read back for edge marking: 24-bit depth and
 * the stencil (an opaque polygon ID, or 64-127/255 for none) << 24. */
static u32 *sDepthIds[2];
/* Edge marking works (off for good if the buffer read back ever isn't as
 * expected), and how many scenes' buffers are still to be checked. */
static int sEdgeMarking=1;
static unsigned sEdgeChecks=60;
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

static void clean_up(void)
{
    int i;
    for(i=0;i<TEX_CACHE_ENTRIES;i++)free_texture(&sTextures[i]);
    for(i=0;i<2;i++){
        if(sVertices[i])linearFree(sVertices[i]);
        if(sImages[i])linearFree(sImages[i]);
        if(sDepthIds[i])linearFree(sDepthIds[i]);
        sVertices[i]=NULL;sImages[i]=NULL;sDepthIds[i]=NULL;
    }
    free(sBatches);sBatches=NULL;
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
    C3D_CullFace(GPU_CULL_NONE); /* Stage 1 geometry already culled. */

    for(i=0;i<2;i++){
        sVertices[i]=linearAlloc(NITRO_GX3D_PICA_MAX_VERTICES*
                                 sizeof(NitroGx3dGpuVertex));
        sImages[i]=linearAlloc(IMAGE_BYTES);
        sDepthIds[i]=linearAlloc(IMAGE_BYTES);
        if(!sVertices[i]||!sImages[i]||!sDepthIds[i])goto fail;
        memset(sImages[i],0,IMAGE_BYTES);
    }
    sBatches=malloc(MAX_BATCHES*sizeof(*sBatches));
    if(!sBatches)goto fail;
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

static GPU_TEXTURE_WRAP_PARAM wrap_mode(uint32_t param,int repeatBit,int flipBit)
{
    if(!(param&(1u<<repeatBit)))return GPU_CLAMP_TO_EDGE;
    return (param&(1u<<flipBit))?GPU_MIRRORED_REPEAT:GPU_REPEAT;
}

/* Frees the least recently used texture not drawn this frame, to make
 * room. Returns it, or NULL if every texture is in use this frame. */
static TexEntry *evict_one(void)
{
    TexEntry *oldest=NULL;
    int i;
    for(i=0;i<TEX_CACHE_ENTRIES;i++){
        TexEntry *e=&sTextures[i];
        if(!e->used || e->lastFrame==sFrame)continue;
        if(!oldest || e->lastFrame<oldest->lastFrame)oldest=e;
    }
    if(oldest)free_texture(oldest);
    return oldest;
}

/* The converted texture for (param, palette), converting it if it's new or
 * its source bytes changed (hashed at most once a frame). Texture data is
 * read here, on the 3D thread, not at SWAP: the game updates textures in
 * V-blank, as on the DS, so this is close enough. NULL if there's no
 * memory for it. */
static C3D_Tex *get_texture(uint32_t param,uint32_t palette)
{
    TexEntry *e=NULL,*free_slot=NULL;
    unsigned width=NitroPort_Gx3dTexWidth(param);
    unsigned height=NitroPort_Gx3dTexHeight(param);
    size_t bytes=(size_t)width*height*4u;
    uint32_t hash;
    int i;
    for(i=0;i<TEX_CACHE_ENTRIES;i++){
        TexEntry *c=&sTextures[i];
        if(c->used && c->param==param && c->palette==palette){e=c;break;}
        if(!c->used && !free_slot)free_slot=c;
    }
    if(e && e->checkedFrame==sFrame){
        e->lastFrame=sFrame;
        return &e->tex;
    }
    hash=NitroPort_Gx3dTextureHash(&sBanks,param,palette);
    if(e && e->hash==hash){
        e->checkedFrame=e->lastFrame=sFrame;
        return &e->tex;
    }
    if(!e){
        while(sTextureBytes+bytes>TEX_CACHE_BYTES && evict_one()){}
        if(!free_slot){
            free_slot=evict_one();
            if(!free_slot)return NULL;
        }
        e=free_slot;
        memset(e,0,sizeof(*e));
        if(!C3D_TexInit(&e->tex,(u16)width,(u16)height,GPU_RGBA8))return NULL;
        C3D_TexSetFilter(&e->tex,GPU_NEAREST,GPU_NEAREST);
        C3D_TexSetWrap(&e->tex,wrap_mode(param,16,18),wrap_mode(param,17,19));
        e->used=1;
        e->param=param;
        e->palette=palette;
        e->bytes=bytes;
        sTextureBytes+=bytes;
    }
    {
        u64 start=svcGetSystemTick();
        NitroPort_Gx3dDecodeTextureTiled(&sBanks,param,palette,(uint32_t*)e->tex.data);
        C3D_TexFlush(&e->tex);
        sTexTicks+=svcGetSystemTick()-start;
        sTexTexels+=(u64)width*height;
        sTexConverted++;
    }
    e->hash=hash;
    e->checkedFrame=e->lastFrame=sFrame;
    return &e->tex;
}

/* Polygon modes (attribute bits 4-5) on the colour combiner. */
static void set_combiner(uint32_t attr,int textured)
{
    C3D_TexEnv *env=C3D_GetTexEnv(0);
    unsigned mode=(attr>>4)&3u;
    C3D_TexEnvInit(env);
    if(!textured){
        C3D_TexEnvSrc(env,C3D_Both,GPU_PRIMARY_COLOR,0,0);
        C3D_TexEnvFunc(env,C3D_Both,GPU_REPLACE);
    }else if(mode==1){
        /* Decal: RGB = mix(vertex, texture, texture alpha); alpha from
         * the vertex (the polygon). */
        C3D_TexEnvSrc(env,C3D_RGB,GPU_TEXTURE0,GPU_PRIMARY_COLOR,GPU_TEXTURE0);
        C3D_TexEnvOpRgb(env,GPU_TEVOP_RGB_SRC_COLOR,GPU_TEVOP_RGB_SRC_COLOR,
                        GPU_TEVOP_RGB_SRC_ALPHA);
        C3D_TexEnvFunc(env,C3D_RGB,GPU_INTERPOLATE);
        C3D_TexEnvSrc(env,C3D_Alpha,GPU_PRIMARY_COLOR,0,0);
        C3D_TexEnvFunc(env,C3D_Alpha,GPU_REPLACE);
    }else{
        /* Modulation; also toon/highlight and shadow polygons until their
         * PR, as in the PC reference. */
        C3D_TexEnvSrc(env,C3D_Both,GPU_TEXTURE0,GPU_PRIMARY_COLOR,0);
        C3D_TexEnvFunc(env,C3D_Both,GPU_MODULATE);
    }
}

/* DS 5-bit colour to 8 bits. */
static u32 widen5(u32 v)
{
    return (v<<3)|(v>>2);
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
    int slot,blend,edging;
    u32 clear,clearColor;
    NitroGx3dEdgeSetup edges;
    if(dropped)*dropped=scene ? scene->dropped : 0;
    if(!sTarget||!scene)return -1;
    slot=sBuffer;
    sBuffer^=1;
    sFrame++;
    count=NitroPort_Gx3dBuildBatches(scene,sVertices[slot],
                                     NITRO_GX3D_PICA_MAX_VERTICES,
                                     sBatches,MAX_BATCHES,&nbatches,&lost);
    if(dropped)*dropped+=lost;
    NitroPort_Gx3dMapTextureBanks(&sBanks);
    /* CLEAR_COLOR: RGB555, alpha in bits 16-20 (alpha 0 leaves BG0
     * see-through where nothing is drawn). CLEAR_DEPTH is nearly always
     * the far plane; the target clears to the maximum depth. */
    clear=(u32)NitroPort_IoReg[0x350]|((u32)NitroPort_IoReg[0x351]<<8)|
          ((u32)NitroPort_IoReg[0x352]<<16)|((u32)NitroPort_IoReg[0x353]<<24);
    clearColor=(widen5(clear&31u)<<24)|(widen5((clear>>5)&31u)<<16)|
               (widen5((clear>>10)&31u)<<8)|widen5((clear>>16)&31u);
    /* A see-through background clears to black: translucent polygons
     * blended onto it come out premultiplied, which publishing undoes
     * (the DS writes their colour unblended where alpha is 0). */
    if(!((clear>>16)&31u))clearColor=0;
    /* DISP3DCNT bit 3: alpha blending. */
    blend=(NitroPort_IoReg[0x60]&8u)!=0;
    /* Bit 5: edge marking. The rear plane is at the depth this target is
     * cleared to, the farthest. */
    NitroPort_Gx3dEdgeSetup(&edges);
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
    buffers=C3D_GetBufInfo();
    BufInfo_Init(buffers);
    BufInfo_Add(buffers,sVertices[slot],sizeof(NitroGx3dGpuVertex),
                3,0x210u);
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
                C3D_TexBind(0,tex);
            }
        }
        set_combiner(attr,textured);
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
            C3D_DrawArrays(GPU_TRIANGLES,(int)batch->first,(int)batch->count);
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
            C3D_DrawArrays(GPU_TRIANGLES,(int)batch->first,(int)batch->count);
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
        C3D_DrawArrays(GPU_TRIANGLES,(int)batch->first,(int)batch->count);
    }
    /* Edge marking needs each pixel's opaque polygon ID: the opaque
     * pixels drawn again (the same alpha tests), only where they're what
     * is in front (depth equal), writing their polygon ID to the stencil
     * buffer and nothing else. Under a translucent pixel the opaque one's
     * ID goes in too, as on the DS. */
    if(edging){
        for(b=0;b<nbatches;b++){
            const NitroGx3dBatch *batch=&sBatches[b];
            int textured=batch->texture_param!=0;
            uint32_t attr=batch->polygon_attr;
            /* Translucent batches have opaque pixels only at alpha 31:
             * their fully opaque texels. */
            if(batch->translucent && ((attr>>16)&31u)!=31u)continue;
            if(textured){
                C3D_Tex *tex=get_texture(batch->texture_param,batch->palette_base);
                if(!tex)textured=0;
                else C3D_TexBind(0,tex);
            }
            set_combiner(attr,textured);
            C3D_AlphaTest(true,GPU_GREATER,batch->translucent?254:0);
            C3D_DepthTest(true,GPU_EQUAL,0);
            C3D_StencilTest(true,GPU_ALWAYS,(int)((attr>>24)&63u),0xFF,0xFF);
            C3D_StencilOp(GPU_STENCIL_KEEP,GPU_STENCIL_KEEP,GPU_STENCIL_REPLACE);
            C3D_DrawArrays(GPU_TRIANGLES,(int)batch->first,(int)batch->count);
        }
    }
    /* GX_CMDLIST_FLUSH: flush only the command list. Without it citro3d
     * flushes the CPU cache over all of linear memory every frame; what
     * the GPU reads here is flushed already (vertices above, textures in
     * get_texture). No C3D_FrameSync either: it waits for a V-blank. */
    C3D_FrameEnd(GX_CMDLIST_FLUSH);
    t1=svcGetSystemTick();
    /* Off-screen target isn't bound to a 3DS LCD. Copy its tiles to
     * ordinary linear memory: this waits for the drawing to finish, then
     * for the copy (PPF), then the CPU cache is invalidated. */
    GSPGPU_FlushDataCache(sImages[slot],(u32)IMAGE_BYTES);
    C3D_SyncDisplayTransfer((u32*)sTarget->frameBuf.colorBuf,
                 GX_BUFFER_DIM(GX3D_W,GX3D_H),sImages[slot],
                 GX_BUFFER_DIM(GX3D_W,GX3D_H),GX3D_TRANSFER_FLAGS);
    GSPGPU_InvalidateDataCache(sImages[slot],(u32)IMAGE_BYTES);
    if(edging){
        /* The depth/stencil buffer the same way: D24S8, a 32-bit word a
         * pixel, the depth in its low 24 bits and the stencil above. */
        GSPGPU_FlushDataCache(sDepthIds[slot],(u32)IMAGE_BYTES);
        C3D_SyncDisplayTransfer((u32*)sTarget->frameBuf.depthBuf,
                     GX_BUFFER_DIM(GX3D_W,GX3D_H),sDepthIds[slot],
                     GX_BUFFER_DIM(GX3D_W,GX3D_H),GX3D_TRANSFER_FLAGS);
        GSPGPU_InvalidateDataCache(sDepthIds[slot],(u32)IMAGE_BYTES);
        /* Every stencil value must be an ID, an ID + 64 or 255; anything
         * else means the buffer isn't laid out as above. Checked on the
         * first scenes: then edge marking stays off rather than draw
         * outlines in the wrong places. */
        if(sEdgeChecks>0){
            unsigned odd=NitroPort_Gx3dEdgeIdsUnexpected(sDepthIds[slot]);
            sEdgeChecks--;
            if(odd){
                sEdgeMarking=0;
                edging=0;
                NitroPort_Log("[gx3d] edge marking off: %u pixels of the depth/stencil buffer read back unexpected (first word %08lx)",
                              odd,(unsigned long)sDepthIds[slot][0]);
            }else if(sEdgeChecks==0){
                unsigned i,ids=0;
                for(i=0;i<GX3D_W*GX3D_H;i++)ids+=(sDepthIds[slot][i]>>24)<=63u;
                NitroPort_Log("[gx3d] edge marking on: depth/stencil buffer as expected (%u of %u pixels with an opaque polygon ID)",
                              ids,(unsigned)(GX3D_W*GX3D_H));
            }
        }
    }
    t2=svcGetSystemTick();
    if(edging)NitroPort_Gx3dPublishRGBA8Edges(sImages[slot],GX3D_W*GX3D_H,sDepthIds[slot],&edges);
    else NitroPort_Gx3dPublishRGBA8(sImages[slot],GX3D_W*GX3D_H);
    sBuildTicks+=t1-t0;
    sWaitTicks+=t2-t1;
    sPublishTicks+=svcGetSystemTick()-t2;
    return (int)(count/3);
}
#endif
