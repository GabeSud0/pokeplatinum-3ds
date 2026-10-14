/*
 * One image interface for DS BG0 on both machines. Two RGB555|coverage
 * images are published by the GPU worker (3DS) or the reference renderer
 * (PC). A short atomic lock manages readers and index publication; drawing
 * never waits for the GPU, and no published image is overwritten in use.
 *
 * On the 3DS the GPU thread hands its finished RGBA8 image over as it is
 * (NitroPort_Gx3dSubmitRGBA8), and whoever draws the 3D next converts it,
 * when it acquires the image: the 3D's screen drawer, on a core of its
 * own, rather than the GPU thread on the game's busy core. That drawer is
 * woken to convert it as soon as it's handed over, if it's free
 * (NitroPort_Gx3dConvertPending): not while the game waits for it to read
 * the display.
 */
#include <nitro_port_gx3d.h>
#include "port_platform.h"
#include <stdint.h>
#include <stddef.h>
#define PIXELS (NITRO_GX3D_WIDTH*NITRO_GX3D_HEIGHT)
/* Two pixels of an image, or of its alphas, written at once. */
typedef uint32_t PairWord __attribute__((may_alias,aligned(4)));
typedef uint16_t PairHalf __attribute__((may_alias,aligned(2)));
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "the two-pixel stores below assume a little-endian machine"
#endif
static uint16_t sImages[2][PIXELS] __attribute__((aligned(4)));
static uint8_t sAlphas[2][PIXELS] __attribute__((aligned(4)));   /* 3D alpha 0-31 per pixel */
static uint8_t sTranslucent[2];      /* the image has pixels of alpha 1-30 */
static unsigned char sGate;
static int sPublished=-1,sReserved=-1,sReaders[2];
static uint32_t sPublishCount;   /* images published or handed over so far */
/* A GPU image handed over, still to be converted, and the one being
 * converted (its buffers mustn't be drawn into meanwhile). */
static const uint32_t *sPending,*sPendingDepth,*sConverting;
static NitroGx3dEdgeSetup sPendingEdges;
static int sPendingHasEdges;
/* A lower-priority GPU or same-priority drawer can own this gate.
 * Never busy-loop indefinitely on core 0: yield after a short spin. */
static void lock(void) {
    unsigned spins = 0;
    while (__atomic_test_and_set(&sGate, __ATOMIC_ACQUIRE)) {
        while (__atomic_load_n(&sGate, __ATOMIC_RELAXED)) {
            if (++spins >= 64u) {
                NitroPortPlatform_Sleep(50);
                spins = 0;
            }
        }
    }
}
static void unlock(void){__atomic_clear(&sGate,__ATOMIC_RELEASE);}
static int reserve(void) {
    int i,result=-1;
    lock();
    for(i=0;i<2;i++)if(i!=sPublished && i!=sReserved && !sReaders[i]){
        sReserved=result=i;break;
    }
    unlock();
    return result;
}
/* count: a new image (not one handed over before and counted then). */
static void finish(int index,int count) {
    lock();
    if(sReserved==index){
        sPublished=index;
        sReserved=-1;
        if(count)sPublishCount++;
    }
    unlock();
}
/* min(255, v * 255 / a) for every alpha a and colour value v: the 3DS's
 * CPU has no divide instruction, and a screen of translucent sparkles
 * took tens of milliseconds to convert with three divisions a pixel. */
static uint8_t sUnpremultiply[256][256];
static int sUnpremultiplyReady;
static void make_unpremultiply(void)
{
    unsigned a,v;
    for(a=1;a<256;a++)for(v=0;v<256;v++){
        unsigned x=v*255u/a;
        sUnpremultiply[a][v]=(uint8_t)(x>255u?255u:x);
    }
    sUnpremultiplyReady=1;
}
/* PICA RGBA8 is a 32-bit 0xRRGGBBAA pixel (little-endian memory holds
 * bytes A,B,G,R): each colour's top 5 bits, straight into DS RGB555. */
#define RGB555(c) (((c)>>27)|(((c)>>14)&0x3E0u)|(((c)>>1)&0x7C00u))
static int publish(const uint32_t *src,size_t pixels,const uint32_t *depthId,
                   const NitroGx3dEdgeSetup *edges,int count);
int NitroPort_Gx3dPublishRGBA8(const uint32_t *src,size_t pixels)
{
    return publish(src,pixels,NULL,NULL,1);
}
int NitroPort_Gx3dPublishRGBA8Edges(const uint32_t *src,size_t pixels,
                                    const uint32_t *depthId,const NitroGx3dEdgeSetup *edges)
{
    return publish(src,pixels,depthId,edges,1);
}
int NitroPort_Gx3dSubmitRGBA8(const uint32_t *image,const uint32_t *depthId,
                              const NitroGx3dEdgeSetup *edges)
{
    if(!image)return 0;
    lock();
    /* A newer one replaces one still waiting: only the newest is shown. */
    sPending=image;
    sPendingDepth=depthId;
    sPendingHasEdges=depthId && edges && edges->enabled;
    if(sPendingHasEdges)sPendingEdges=*edges;
    sPublishCount++;
    unlock();
    return 1;
}
int NitroPort_Gx3dRawInUse(const uint32_t *image)
{
    int used;
    lock();
    used=image==sPending || image==sConverting;
    unlock();
    return used;
}
/* The image handed over, converted (by one caller at a time). */
static int convert_pending(void)
{
    const uint32_t *image,*depthId;
    NitroGx3dEdgeSetup edges;
    int hasEdges;
    lock();
    if(!sPending || sConverting){
        unlock();
        return 0;
    }
    image=sConverting=sPending;
    depthId=sPendingDepth;
    hasEdges=sPendingHasEdges;
    if(hasEdges)edges=sPendingEdges;
    sPending=NULL;
    unlock();
    (void)publish(image,PIXELS,hasEdges?depthId:NULL,hasEdges?&edges:NULL,0);
    lock();
    sConverting=NULL;
    unlock();
    return 1;
}
int NitroPort_Gx3dConvertPending(void)
{
    return convert_pending();
}
static int publish(const uint32_t *src,size_t pixels,const uint32_t *depthId,
                   const NitroGx3dEdgeSetup *edges,int count)
{
    int index;
    size_t i;
    unsigned partly=0;
    if(!src || pixels<PIXELS)return 0;
    if(!sUnpremultiplyReady)make_unpremultiply();
    index=reserve();
    if(index<0)return 0;
    /* Two pixels at a time, stored as one word (and the alphas as one
     * halfword; both machines are little-endian): most pairs are nothing
     * at all (around a battle's Pokemon) or opaque (the field). */
    {
        PairWord *out=(PairWord*)sImages[index];
        PairHalf *alphas=(PairHalf*)sAlphas[index];
        for(i=0;i<PIXELS/2;i++){
            uint32_t c0=src[2*i],c1=src[2*i+1];
            uint16_t pair[2];
            uint8_t pairAlpha[2];
            int k;
            if((c0|c1)==0){
                out[i]=0;
                alphas[i]=0;
                continue;
            }
            if((c0&c1&255u)==255u){
                out[i]=0x80008000u|RGB555(c0)|(RGB555(c1)<<16);
                alphas[i]=31u|(31u<<8);
                continue;
            }
            for(k=0;k<2;k++){
                uint32_t c=k?c1:c0;
                unsigned a=c&255u;
                uint32_t rgb=RGB555(c);
                if(a-1u<254u){
                    /* Partly transparent (rare): the GPU path blends
                     * translucent polygons into a cleared alpha-0
                     * background as premultiplied colour
                     * (port_gx3d_pica.c); undo it. On the DS such a pixel
                     * just takes the polygon's colour. */
                    const uint8_t *undo=sUnpremultiply[a];
                    unsigned r=undo[c>>24],g=undo[(c>>16)&255u],b=undo[(c>>8)&255u];
                    rgb=(r>>3)|((g>>3)<<5)|((b>>3)<<10);
                    partly|=(a>>3)<31u;
                }
                pair[k]=(uint16_t)(rgb|(a?0x8000u:0u));
                pairAlpha[k]=(uint8_t)(a>>3);
            }
            out[i]=pair[0]|((uint32_t)pair[1]<<16);
            alphas[i]=(uint16_t)(pairAlpha[0]|(pairAlpha[1]<<8));
        }
    }
    if(depthId && edges && edges->enabled){
        int outlinesPartly=0;
        NitroPort_Gx3dMarkEdges(sImages[index],sAlphas[index],depthId,edges,&outlinesPartly);
        partly|=(unsigned)outlinesPartly;
    }
    sTranslucent[index]=(uint8_t)(partly!=0);
    finish(index,count);
    return 1;
}
#ifndef __3DS__
/* The PC uses the exact Stage 1 software renderer behind the same image
 * API. Draw only on demand, after SWAP, and hold the immutable scene while
 * the software rasterizer reads its textures and triangles. */
static uint16_t sScratch[PIXELS];
static void render_host(void)
{
    /* Render on every BG0 draw as Stage 1 did. The game's VRAM texture
     * banks can change without SWAP, and tests reset scene serials. */
    uint32_t lastSerial=0;
    const NitroGx3dScene *scene=NitroPort_Gx3dAcquireScene(&lastSerial);
    int index;
    if(!scene)return;
    index=reserve();
    if(index>=0){
        unsigned partly=0;
        NitroPort_Gx3dRasterizeSceneAlpha(scene,sScratch,sAlphas[index],NULL);
        for(size_t i=0;i<PIXELS;i++){
            sImages[index][i]=sScratch[i];
            partly|=(sScratch[i]&0x8000u) && sAlphas[index][i]<31u;
        }
        sTranslucent[index]=(uint8_t)(partly!=0);
        finish(index,1);
    }
    NitroPort_Gx3dReleaseScene(scene);
}
#endif
const uint16_t *NitroPort_Gx3dAcquireImage(void)
{
    int index;
    convert_pending();
#ifndef __3DS__
    render_host();
#endif
    lock();
    index=sPublished;
    if(index>=0)sReaders[index]++;
    unlock();
    return index>=0?sImages[index]:NULL;
}
void NitroPort_Gx3dReleaseImage(const uint16_t *ptr)
{
    int i;
    if(!ptr)return;
    lock();
    for(i=0;i<2;i++)if(ptr==sImages[i]){
        if(sReaders[i]>0)sReaders[i]--;
        break;
    }
    unlock();
}

uint32_t NitroPort_Gx3dImageCount(void)
{
    return __atomic_load_n(&sPublishCount,__ATOMIC_ACQUIRE);
}

int NitroPort_Gx3dImageTranslucent(const uint16_t *image)
{
    int i;
    for(i=0;i<2;i++)if(image==sImages[i])return sTranslucent[i];
    return 0;
}

const uint8_t *NitroPort_Gx3dImageAlpha(const uint16_t *image)
{
    int i;
    for(i=0;i<2;i++)if(image==sImages[i])return sAlphas[i];
    return NULL;
}
