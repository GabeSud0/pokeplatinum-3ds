/*
 * One image interface for DS BG0 on both machines. Two RGB555|coverage
 * images are published by the GPU worker (3DS) or the reference renderer
 * (PC). A short atomic lock manages readers and index publication; drawing
 * never waits for the GPU, and no published image is overwritten in use.
 * Stage 2 PR3 will extend this path to preserve 5-bit pixel alpha.
 */
#include <nitro_port_gx3d.h>
#include "port_platform.h"
#include <stdint.h>
#include <stddef.h>
#define PIXELS (NITRO_GX3D_WIDTH*NITRO_GX3D_HEIGHT)
static uint16_t sImages[2][PIXELS];
static uint8_t sAlphas[2][PIXELS];   /* 3D alpha 0-31 per pixel */
static uint8_t sTranslucent[2];      /* the image has pixels of alpha 1-30 */
static unsigned char sGate;
static int sPublished=-1,sReserved=-1,sReaders[2];
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
static void finish(int index) {
    lock();
    if(sReserved==index){
        sPublished=index;
        sReserved=-1;
    }
    unlock();
}
int NitroPort_Gx3dPublishRGBA8(const uint32_t *src,size_t pixels)
{
    int index;
    size_t i;
    unsigned partly=0;
    if(!src || pixels<PIXELS)return 0;
    index=reserve();
    if(index<0)return 0;
    for(i=0;i<PIXELS;i++){
        /* PICA RGBA8 is a 32-bit 0xRRGGBBAA pixel. Little-endian
         * linear memory therefore holds bytes A,B,G,R. */
        uint32_t c=src[i];
        unsigned r=(c>>24)&255u,g=(c>>16)&255u,b=(c>>8)&255u,a=c&255u;
        /* The GPU path blends translucent polygons into a cleared alpha-0
         * background as premultiplied colour (port_gx3d_pica.c); where
         * the result is partly transparent, undo it. On the DS such a
         * pixel just takes the polygon's colour. */
        if(a && a<255u){
            r=r*255u/a;g=g*255u/a;b=b*255u/a;
            if(r>255u)r=255u;
            if(g>255u)g=255u;
            if(b>255u)b=255u;
        }
        sImages[index][i]=(uint16_t)((r>>3)|((g>>3)<<5)|((b>>3)<<10)|
                                        (a?0x8000u:0u));
        sAlphas[index][i]=(uint8_t)(a>>3);
        partly|=(a!=0u)&((a>>3)<31u);   /* drawn, not opaque */
    }
    sTranslucent[index]=(uint8_t)(partly!=0);
    finish(index);
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
        finish(index);
    }
    NitroPort_Gx3dReleaseScene(scene);
}
#endif
const uint16_t *NitroPort_Gx3dAcquireImage(void)
{
    int index;
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
