/* Stage 2 Plumbing: invented triangles only, no ROM assets.
 * Concurrent producer / GPU-consumer handoff and clip-space vertex packing. */
#include <nitro_port_gx3d.h>
#include <nitro_port_gx3d_mailbox.h>
#include <nitro_port_gx3d_vertex.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#define CHECK(x) do { nchecks++; if(!(x)){ nfail++; \
  printf("FAIL %s:%d: %s\n",__FILE__,__LINE__,#x); }} while(0)
#define LOOPS 12000
static int nchecks,nfail;
static NitroGx3dMailbox mailbox;
static volatile int produced,finished;
static volatile unsigned int badReads;
static uint32_t payload[3];
static void *producer(void *unused) {
    int i;
    (void)unused;
    for(i=1;i<=LOOPS;i++){
        int next=-1;
        uint32_t serial=0;
        payload[mailbox.building]=(uint32_t)i;
        if(!NitroPort_MailboxPublish(&mailbox,&next,&serial)){badReads++;break;}
        if(serial!=(uint32_t)i || next<0 || next>2)badReads++;
        __atomic_store_n(&produced,i,__ATOMIC_RELEASE);
        if((i&31)==0)sched_yield();
    }
    __atomic_store_n(&finished,1,__ATOMIC_RELEASE);
    return NULL;
}
static void *consumer(void *unused) {
    uint32_t serial=0;
    (void)unused;
    while(!__atomic_load_n(&finished,__ATOMIC_ACQUIRE)||
          serial<(uint32_t)__atomic_load_n(&produced,__ATOMIC_ACQUIRE)){
        int slot=NitroPort_MailboxAcquire(&mailbox,&serial);
        if(slot<0){sched_yield();continue;}
        /* Read once before producer advances, then again after it gets
         * plenty of opportunities to publish/reuse other scene slots. */
        uint32_t first=payload[slot];
        int k;
        for(k=0;k<16;k++)sched_yield();
        if(payload[slot]!=first || first!=serial)badReads++;
        NitroPort_MailboxRelease(&mailbox,slot);
    }
    return NULL;
}
static void test_mailbox(void) {
    pthread_t p,c;
    int next;
    uint32_t serial=0;
    NitroPort_MailboxInit(&mailbox);
    CHECK(NitroPort_MailboxAcquire(&mailbox,&serial)==-1);
    CHECK(mailbox.building==0);
    CHECK(pthread_create(&p,NULL,producer,NULL)==0);
    CHECK(pthread_create(&c,NULL,consumer,NULL)==0);
    pthread_join(p,NULL);
    pthread_join(c,NULL);
    CHECK(badReads==0);
    CHECK(mailbox.serial==LOOPS);
    CHECK(mailbox.skipped==0);
    CHECK(mailbox.building!=mailbox.latest);
    /* Last published slot remains owned by producer only until next swap. */
    serial=0;
    int inUse=NitroPort_MailboxAcquire(&mailbox,&serial);
    CHECK(inUse>=0);
    CHECK(NitroPort_MailboxAcquire(&mailbox,&serial)==-1);
    payload[mailbox.building]=LOOPS+1;
    CHECK(NitroPort_MailboxPublish(&mailbox,&next,NULL)==1);
    CHECK(next!=inUse);
    CHECK(payload[inUse]==LOOPS);
    NitroPort_MailboxRelease(&mailbox,inUse);
    CHECK(mailbox.reading==-1);
}
static NitroGx3dScene scene;
static NitroGx3dGpuVertex verts[12];
static void test_vertices(void) {
    size_t n;
    memset(&scene,0,sizeof(scene));
    scene.count=2;
    scene.triangles[0].polygon_attr=(3u<<6)|(31u<<16);
    scene.triangles[0].texture_param=0;
    scene.triangles[0].v[0].x=128*256;
    scene.triangles[0].v[0].y=96*256;
    scene.triangles[0].v[0].clip[3]=4096;
    scene.triangles[0].v[0].color=0x001f;
    scene.triangles[0].v[0].depth=0;
    scene.triangles[0].v[1]=scene.triangles[0].v[0];
    scene.triangles[0].v[1].x=256*256;
    scene.triangles[0].v[1].y=0;
    scene.triangles[0].v[1].clip[3]=8192;
    scene.triangles[0].v[1].depth=0xffffffu;
    scene.triangles[0].v[1].color=0x03e0;
    scene.triangles[0].v[2]=scene.triangles[0].v[0];
    scene.triangles[0].v[2].color=0x7c00;
    scene.triangles[0].v[2].s=4.0f;
    scene.triangles[0].v[2].t=2.0f;
    scene.triangles[1]=scene.triangles[0];
    scene.triangles[1].polygon_attr=(3u<<6)|(16u<<16);
    n=NitroPort_Gx3dBuildOpaqueVertices(&scene,verts,12);
    CHECK(n==3);
    CHECK(verts[0].position[0]==0.0f);
    CHECK(verts[0].position[1]==0.0f);
    CHECK(verts[0].position[3]==1.0f);
    CHECK(verts[0].position[2]==0.0f);
    CHECK(verts[1].position[0]==2.0f);
    CHECK(verts[1].position[1]==2.0f);
    CHECK(verts[1].position[2]==-2.0f);
    CHECK(verts[1].position[3]==2.0f);
    CHECK(verts[0].color[0]==255 && verts[0].color[1]==0);
    CHECK(verts[1].color[1]==255 && verts[1].color[0]==0);
    CHECK(verts[2].color[2]==255 && verts[2].color[3]==255);
    CHECK(verts[2].uv[0]==0.5f && verts[2].uv[1]==0.25f);
    CHECK(NitroPort_Gx3dBuildOpaqueVertices(&scene,verts,2)==0);
    scene.triangles[1].polygon_attr=(3u<<6)|(31u<<16);
    scene.triangles[1].texture_param=(4u<<26);
    /* Until PR2 adds textures, textured geometry MUST remain visible in
     * vertex colour. Only translucency excludes a polygon in PR1. */
    CHECK(NitroPort_Gx3dBuildOpaqueVertices(&scene,verts,12)==6);
    scene.swap_param=2u; /* DS W-buffer flag */
    CHECK(NitroPort_Gx3dBuildOpaqueVertices(&scene,verts,12)==6);
    CHECK(verts[0].position[2]==-0.0625f);
    CHECK(verts[1].position[2]==-0.25f);
}
static uint32_t imagePixels[NITRO_GX3D_WIDTH*NITRO_GX3D_HEIGHT];
static void test_image_handoff(void)
{
    const uint16_t *red,*green,*blue;
    size_t i;
    for(i=0;i<sizeof(imagePixels)/sizeof(imagePixels[0]);i++)
        imagePixels[i]=0xff0000ffu; /* RGBA red with full alpha */
    CHECK(NitroPort_Gx3dPublishRGBA8(imagePixels,
           NITRO_GX3D_WIDTH*NITRO_GX3D_HEIGHT)==1);
    red=NitroPort_Gx3dAcquireImage();
    CHECK(red!=NULL);
    if(red)CHECK(red[0]==0x801fu);
    for(i=0;i<sizeof(imagePixels)/sizeof(imagePixels[0]);i++)
        imagePixels[i]=0x00ff00ffu; /* green */
    CHECK(NitroPort_Gx3dPublishRGBA8(imagePixels,
           NITRO_GX3D_WIDTH*NITRO_GX3D_HEIGHT)==1);
    green=NitroPort_Gx3dAcquireImage();
    CHECK(green!=NULL && green!=red);
    if(green)CHECK(green[0]==0x83e0u);
    if(red)CHECK(red[0]==0x801fu); /* old acquired image is still stable */
    for(i=0;i<sizeof(imagePixels)/sizeof(imagePixels[0]);i++)
        imagePixels[i]=0x0000ffffu; /* blue */
    /* Both outputs are being read: the GPU must drop rather than overwrite. */
    CHECK(NitroPort_Gx3dPublishRGBA8(imagePixels,
           NITRO_GX3D_WIDTH*NITRO_GX3D_HEIGHT)==0);
    NitroPort_Gx3dReleaseImage(red);
    NitroPort_Gx3dReleaseImage(green);
    CHECK(NitroPort_Gx3dPublishRGBA8(imagePixels,
           NITRO_GX3D_WIDTH*NITRO_GX3D_HEIGHT)==1);
    blue=NitroPort_Gx3dAcquireImage();
    CHECK(blue!=NULL);
    if(blue)CHECK(blue[0]==0xfc00u);
    NitroPort_Gx3dReleaseImage(blue);
}


/* Emulate a lower-priority gate owner that cannot release the gate until
 * another runnable thread gets CPU time. On 3DS the same-core sleep in
 * the lock is essential; this test deterministically takes that path. */
static void *unlock_mailbox_later(void *unused)
{
    struct timespec interval = {0, 20000000L};
    (void)unused;
    nanosleep(&interval, NULL);
    __atomic_clear(&mailbox.gate,__ATOMIC_RELEASE);
    return NULL;
}
static void test_lock_contention(void)
{
    pthread_t owner;
    NitroPort_MailboxInit(&mailbox);
    __atomic_test_and_set(&mailbox.gate,__ATOMIC_ACQUIRE);
    CHECK(pthread_create(&owner,NULL,unlock_mailbox_later,NULL)==0);
    CHECK(NitroPort_MailboxLatest(&mailbox)==-1);
    pthread_join(owner,NULL);
    CHECK(__atomic_load_n(&mailbox.gate,__ATOMIC_ACQUIRE)==0);
}

/* The backend's depth state is GPU_LESS with a depth clear of 1.0.
 * A smaller (nearer) mapped depth must win regardless of triangle order. */
static void test_depth_direction(void)
{
    NitroGx3dVertex nearVertex={0},farVertex={0};
    float nearDepth,farDepth;
    nearVertex.depth=0x100000u;
    farVertex.depth=0xf00000u;
    nearVertex.clip[3]=4096;
    farVertex.clip[3]=4096*12;
    nearDepth=NitroPort_Gx3dDepthValue(&nearVertex,0);
    farDepth=NitroPort_Gx3dDepthValue(&farVertex,0);
    CHECK(nearDepth<farDepth);
    CHECK(nearDepth<1.0f); /* passes clear */
    CHECK(!(farDepth<nearDepth)); /* farther fragment fails */
    CHECK(NitroPort_Gx3dDepthValue(&nearVertex,2)<
          NitroPort_Gx3dDepthValue(&farVertex,2));
    nearVertex.clip[3]=-4096;
    CHECK(NitroPort_Gx3dDepthValue(&nearVertex,2)==0.0f);
    farVertex.clip[3]=4096*128;
    CHECK(NitroPort_Gx3dDepthValue(&farVertex,2)==1.0f);
}
static NitroGx3dScene maxScene;
static NitroGx3dGpuVertex maxVertices[NITRO_GX3D_PICA_MAX_VERTICES];
static void test_full_vertex_capacity(void)
{
    size_t i;
    CHECK(NITRO_GX3D_PICA_MAX_VERTICES==12288u);
    memset(&maxScene,0,sizeof(maxScene));
    maxScene.count=NITRO_GX3D_MAX_TRIANGLES;
    for(i=0;i<maxScene.count;i++){
        maxScene.triangles[i].polygon_attr=(3u<<6)|(31u<<16);
        maxScene.triangles[i].v[0].clip[3]=4096;
        maxScene.triangles[i].v[1].clip[3]=4096;
        maxScene.triangles[i].v[2].clip[3]=4096;
    }
    CHECK(NitroPort_Gx3dBuildOpaqueVertices(
          &maxScene,maxVertices,NITRO_GX3D_PICA_MAX_VERTICES)==12288u);
    CHECK(NitroPort_Gx3dCountDroppedTriangles(
          &maxScene,NITRO_GX3D_PICA_MAX_VERTICES)==0u);
    CHECK(NitroPort_Gx3dBuildOpaqueVertices(
          &maxScene,maxVertices,12285u)==12285u);
    CHECK(NitroPort_Gx3dCountDroppedTriangles(&maxScene,12285u)==1u);
    CHECK(NitroPort_Gx3dCountDroppedTriangles(&maxScene,6144u)==2048u);
    maxScene.triangles[1].polygon_attr=(3u<<6)|(16u<<16);
    CHECK(NitroPort_Gx3dCountDroppedTriangles(&maxScene,6144u)==2047u);
}
static uint16_t testPixels[NITRO_GX3D_WIDTH*NITRO_GX3D_HEIGHT];
static void test_png_dump(void)
{
    char name[128];
    unsigned char png[64];
    FILE *f;
    snprintf(name,sizeof(name),"/tmp/gx3d-plumbing-%ld.png",(long)getpid());
    memset(testPixels,0,sizeof(testPixels));
    testPixels[0]=0x801fu; /* red opaque */
    testPixels[1]=0x03e0u; /* green transparent */
    CHECK(NitroPort_Gx3dWritePng(NULL,testPixels)==0);
    CHECK(NitroPort_Gx3dWritePng(name,testPixels)==1);
    f=fopen(name,"rb");
    CHECK(f!=NULL);
    if(f){
        CHECK(fread(png,1,sizeof(png),f)==sizeof(png));
        fclose(f);
        CHECK(memcmp(png,"\x89PNG\r\n\x1a\n",8)==0);
        CHECK(png[16]==0 && png[17]==0 && png[18]==1 && png[19]==0);
        CHECK(png[20]==0 && png[21]==0 && png[22]==0 && png[23]==192);
        CHECK(png[25]==6); /* RGBA, not RGB; coverage is preserved */
        CHECK(png[48]==0); /* top row filter */
        CHECK(png[49]==255 && png[50]==0 && png[51]==0 && png[52]==255);
        CHECK(png[53]==0 && png[54]==255 && png[55]==0 && png[56]==0);
    }
    remove(name);
}

/* Concurrent image publish/acquire must leave acquired pixels immutable. */
static volatile int imageWriterDone;
static volatile unsigned imageRaceFailures;
static void *image_writer(void *ignored)
{
    int i;
    (void)ignored;
    for(i=0;i<600;i++){
        int j;
        uint32_t value=(i&1)?0x00ff00ffu:0xff0000ffu;
        for(j=0;j<NITRO_GX3D_WIDTH*NITRO_GX3D_HEIGHT;j++)
            imagePixels[j]=value;
        (void)NitroPort_Gx3dPublishRGBA8(
             imagePixels,NITRO_GX3D_WIDTH*NITRO_GX3D_HEIGHT);
        if((i&15)==0)sched_yield();
    }
    __atomic_store_n(&imageWriterDone,1,__ATOMIC_RELEASE);
    return NULL;
}
static void *image_reader(void *ignored)
{
    int i;
    (void)ignored;
    for(i=0;i<1000;i++){
        const uint16_t *img=NitroPort_Gx3dAcquireImage();
        if(img){
            uint16_t value=img[0];
            int k;
            for(k=0;k<4;k++)sched_yield();
            if(img[0]!=value)imageRaceFailures++;
            NitroPort_Gx3dReleaseImage(img);
        }
        if(__atomic_load_n(&imageWriterDone,__ATOMIC_ACQUIRE))break;
    }
    return NULL;
}
static void test_concurrent_image_handoff(void)
{
    pthread_t writer,reader;
    imageWriterDone=0;imageRaceFailures=0;
    CHECK(pthread_create(&writer,NULL,image_writer,NULL)==0);
    CHECK(pthread_create(&reader,NULL,image_reader,NULL)==0);
    pthread_join(writer,NULL);
    pthread_join(reader,NULL);
    CHECK(imageRaceFailures==0);
}

int main(void) {
    test_mailbox();
    test_lock_contention();
    test_vertices();
    test_depth_direction();
    test_full_vertex_capacity();
    test_image_handoff();
    test_png_dump();
    test_concurrent_image_handoff();
    printf("GX3D plumbing: %d checks, %d failures\n",nchecks,nfail);
    return nfail?1:0;
}
