/*
 * One image interface for DS BG0 on both machines. Two RGB555|coverage
 * images are published by the GPU worker (3DS) or the reference renderer
 * (PC). A short atomic lock manages readers and index publication; drawing
 * never waits for the GPU, and no published image is overwritten in use.
 * Stage 2 PR3 will extend this path to preserve 5-bit pixel alpha.
 */
#include <nitro_port_gx3d.h>
#include <stdint.h>
#include <stddef.h>
#define PIXELS (NITRO_GX3D_WIDTH*NITRO_GX3D_HEIGHT)
static uint16_t sImages[2][PIXELS];
static unsigned char sGate;
static int sPublished=-1,sReserved=-1,sReaders[2];
static void lock(void) {
    while(__atomic_test_and_set(&sGate,__ATOMIC_ACQUIRE)){}
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
    if(!src || pixels<PIXELS)return 0;
    index=reserve();
    if(index<0)return 0;
    for(i=0;i<PIXELS;i++){
        /* PICA RGBA8 is a 32-bit 0xRRGGBBAA pixel. Little-endian
         * linear memory therefore holds bytes A,B,G,R. */
        uint32_t c=src[i];
        unsigned r=(c>>24)&255u,g=(c>>16)&255u,b=(c>>8)&255u,a=c&255u;
        sImages[index][i]=(uint16_t)((r>>3)|((g>>3)<<5)|((b>>3)<<10)|
                                        (a?0x8000u:0u));
    }
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
    static uint32_t lastSerial;
    const NitroGx3dScene *scene=NitroPort_Gx3dAcquireScene(&lastSerial);
    int index;
    if(!scene)return;
    index=reserve();
    if(index>=0){
        NitroPort_Gx3dRasterizeScene(scene,sScratch,NULL);
        for(size_t i=0;i<PIXELS;i++)sImages[index][i]=sScratch[i];
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
