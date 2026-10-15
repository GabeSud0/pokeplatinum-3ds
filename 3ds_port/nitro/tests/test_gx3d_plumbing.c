/* Stage 2 Plumbing: invented triangles only, no ROM assets.
 * Concurrent producer / GPU-consumer handoff and clip-space vertex packing. */
#include <nitro_port_gx3d.h>
#include <nitro_port_gx3d_mailbox.h>
#include <nitro_port_gx3d_vertex.h>
#include <nitro_port_gx3d_texture.h>
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
/* The lowest number (tag) of a scene still to be read: one being read, or
 * the latest until a consumer takes it. */
static void test_mailbox_unread(void) {
    NitroGx3dMailbox b;
    int next,slot;
    uint32_t seen=0;
    NitroPort_MailboxInit(&b);
    CHECK(NitroPort_MailboxUnread(&b)==0);
    NitroPort_MailboxTag(&b,5);
    CHECK(NitroPort_MailboxPublish(&b,&next,NULL)==1);
    CHECK(NitroPort_MailboxUnread(&b)==5);  /* published, not taken */
    slot=NitroPort_MailboxAcquire(&b,&seen);
    CHECK(slot>=0);
    CHECK(NitroPort_MailboxUnread(&b)==5);  /* being read */
    NitroPort_MailboxTag(&b,6);
    CHECK(NitroPort_MailboxPublish(&b,&next,NULL)==1);
    CHECK(NitroPort_MailboxUnread(&b)==5);  /* 5 still being read */
    NitroPort_MailboxRelease(&b,slot);
    CHECK(NitroPort_MailboxUnread(&b)==6);
    NitroPort_MailboxTag(&b,7);
    CHECK(NitroPort_MailboxPublish(&b,&next,NULL)==1);
    CHECK(NitroPort_MailboxUnread(&b)==7);  /* 6 replaced, never to be read */
    NitroPort_MailboxMarkTaken(&b);         /* read without acquiring */
    CHECK(NitroPort_MailboxUnread(&b)==0);
    slot=NitroPort_MailboxAcquire(&b,&seen);
    CHECK(slot>=0);
    CHECK(NitroPort_MailboxUnread(&b)==7);
    NitroPort_MailboxRelease(&b,slot);
    CHECK(NitroPort_MailboxUnread(&b)==0);
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
    /* W-buffer depth is W in 20.12 over the 24-bit range (W 1.0 and 2.0
     * here). Was W / 16.0, which put everything beyond 16 units at the far
     * plane. W scenes draw with the PICA's W-buffer mode, whose stored
     * depth is z / W interpolated times the perspective-correct W, so z is
     * the depth itself. (This expected depth * W before: the Z-buffer
     * mode's z, which interpolates W linearly across the screen and broke
     * the intro's tunnel and Giratina on the 3DS.) */
    CHECK(verts[0].position[2]==-(4096.0f/16777215.0f));
    CHECK(verts[1].position[2]==-(8192.0f/16777215.0f));
    /* Z-buffer scenes keep depth * W: z / W is the DS depth. */
    scene.swap_param=0u;
    CHECK(NitroPort_Gx3dBuildOpaqueVertices(&scene,verts,12)==6);
    CHECK(verts[1].position[2]==-2.0f);
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

/* The conversion of the GPU's RGBA8 image, two pixels at a time, against
 * the rule it follows pixel by pixel: colour's top 5 bits; coverage bit
 * when alpha isn't 0; alpha's top 5 bits; partly transparent pixels
 * (alpha 1-254) un-premultiplied, min(255, v * 255 / a). Pairs of every
 * kind: nothing, opaque, mixed, partly transparent, alpha 0 with colour. */
static uint32_t next_random(void);
static void test_image_conversion(void)
{
    static uint32_t img[NITRO_GX3D_WIDTH*NITRO_GX3D_HEIGHT];
    const uint16_t *got;
    const uint8_t *gotAlpha;
    size_t i;
    unsigned bad=0,badAlpha=0,round;
    for(round=0;round<3;round++){
        int partly=0;
        for(i=0;i<NITRO_GX3D_WIDTH*NITRO_GX3D_HEIGHT;i++){
            uint32_t r=next_random(), kind=(r>>4)%8;
            uint32_t rgb=next_random()<<8;
            if(round==2 && kind>=5)kind=1;        /* a round with none partly transparent */
            img[i]=kind<=1?0:kind<=4?(rgb|255u):kind==5?rgb:(rgb|(1u+next_random()%254u));
        }
        CHECK(NitroPort_Gx3dPublishRGBA8(img,NITRO_GX3D_WIDTH*NITRO_GX3D_HEIGHT)==1);
        got=NitroPort_Gx3dAcquireImage();
        gotAlpha=got?NitroPort_Gx3dImageAlpha(got):NULL;
        CHECK(got!=NULL && gotAlpha!=NULL);
        if(!got || !gotAlpha)return;
        for(i=0;i<NITRO_GX3D_WIDTH*NITRO_GX3D_HEIGHT;i++){
            uint32_t c=img[i];
            unsigned a=c&255u,r=c>>24,g=(c>>16)&255u,b=(c>>8)&255u,want;
            if(a>0 && a<255){
                r=r*255u/a;g=g*255u/a;b=b*255u/a;
                r=r>255?255:r;g=g>255?255:g;b=b>255?255:b;
                partly|=(a>>3)<31u;
            }
            want=(r>>3)|((g>>3)<<5)|((b>>3)<<10)|(a?0x8000u:0u);
            if(got[i]!=want)bad++;
            if(gotAlpha[i]!=(a>>3))badAlpha++;
        }
        CHECK(NitroPort_Gx3dImageTranslucent(got)==(partly!=0));
        NitroPort_Gx3dReleaseImage(got);
    }
    CHECK(bad==0);
    CHECK(badAlpha==0);
}

/* Edge marking on a finished image: a 20x10 box of polygon ID 8, nearer,
 * on a ground of ID 56 that covers the screen; the rear plane is ID 63. */
static uint16_t edgeImage[NITRO_GX3D_WIDTH*NITRO_GX3D_HEIGHT];
static uint8_t edgeAlpha[NITRO_GX3D_WIDTH*NITRO_GX3D_HEIGHT];
static uint32_t edgeIds[NITRO_GX3D_WIDTH*NITRO_GX3D_HEIGHT];
static void edge_scene(void)
{
    int x,y;
    for(y=0;y<NITRO_GX3D_HEIGHT;y++)for(x=0;x<NITRO_GX3D_WIDTH;x++){
        int i=y*NITRO_GX3D_WIDTH+x, box=x>=100&&x<120&&y>=50&&y<60;
        edgeImage[i]=(uint16_t)(0x8000u|(box?0x001fu:0x03e0u));   /* red box, green ground */
        edgeAlpha[i]=31;
        edgeIds[i]=box?((8u<<24)|1000u):((56u<<24)|2000u);
    }
}
static void test_edge_marking(void)
{
    NitroGx3dEdgeSetup e;
    int x,y,wrong=0,translucent=0;
    unsigned k;
    memset(&e,0,sizeof(e));
    e.enabled=1;
    for(k=0;k<8;k++)e.colors[k]=(uint16_t)(k|(k<<5)|(k<<10));   /* grey k */
    e.clearId=63;e.clearColor=0x7c00u;e.clearAlpha=0;e.clearDepth=0xffffffu;
    /* Without anti-aliasing: the box's own outer ring in ID 8's colour;
     * the ground next to it not (it's further); the ground's pixels at the
     * screen's border in ID 56's colour (the rear plane is further and
     * another ID). Everything else as it was. */
    edge_scene();
    /* (It says how many: the box's ring, 2*20 + 2*8, and the border.) */
    CHECK(NitroPort_Gx3dMarkEdges(edgeImage,edgeAlpha,edgeIds,&e,&translucent)==
          56u+2u*NITRO_GX3D_WIDTH+2u*(NITRO_GX3D_HEIGHT-2));
    for(y=0;y<NITRO_GX3D_HEIGHT;y++)for(x=0;x<NITRO_GX3D_WIDTH;x++){
        int i=y*NITRO_GX3D_WIDTH+x, box=x>=100&&x<120&&y>=50&&y<60;
        int ring=box&&(x==100||x==119||y==50||y==59);
        int border=x==0||y==0||x==NITRO_GX3D_WIDTH-1||y==NITRO_GX3D_HEIGHT-1;
        uint16_t want=ring?(uint16_t)(0x8000u|0x0421u):border?(uint16_t)(0x8000u|(7u|(7u<<5)|(7u<<10))):
                      (uint16_t)(0x8000u|(box?0x001fu:0x03e0u));
        if(edgeImage[i]!=want||edgeAlpha[i]!=31)wrong++;
    }
    CHECK(wrong==0);
    CHECK(!translucent);
    /* The rear plane with the ground's ID: no outline at the border. */
    edge_scene();
    e.clearId=56;
    NitroPort_Gx3dMarkEdges(edgeImage,edgeAlpha,edgeIds,&e,NULL);
    CHECK(edgeImage[0]==(0x8000u|0x03e0u));
    CHECK(edgeImage[50*NITRO_GX3D_WIDTH+100]==(0x8000u|0x0421u));
    /* No opaque polygon where the ground was (the GPU path's 64-127 or
     * 255): it's the rear plane there, so the box is still outlined, the
     * border not (the box isn't on it). */
    edge_scene();
    for(k=0;k<NITRO_GX3D_WIDTH*NITRO_GX3D_HEIGHT;k++)
        if((edgeIds[k]>>24)==56u)edgeIds[k]=(edgeIds[k]&0xffffffu)|((k&1)?(70u<<24):(255u<<24));
    e.clearId=63;
    NitroPort_Gx3dMarkEdges(edgeImage,edgeAlpha,edgeIds,&e,NULL);
    CHECK(edgeImage[0]==(0x8000u|0x03e0u));
    CHECK(edgeImage[55*NITRO_GX3D_WIDTH+100]==(0x8000u|0x0421u));
    CHECK(edgeImage[55*NITRO_GX3D_WIDTH+99]==(0x8000u|0x03e0u));
    CHECK(edgeImage[55*NITRO_GX3D_WIDTH+101]==(0x8000u|0x001fu));
    /* Anti-aliasing: an outline is blended 17/32 with the neighbour behind
     * it; with the see-through rear plane, the colour stays but the pixel
     * is half transparent. */
    edge_scene();
    e.antialias=1;
    translucent=0;
    NitroPort_Gx3dMarkEdges(edgeImage,edgeAlpha,edgeIds,&e,&translucent);
    /* Box ring pixel next to green ground: (1*17 + 31*15) / 32 = 15 green,
     * 1*17/32 = 0 red and blue. */
    CHECK(edgeImage[50*NITRO_GX3D_WIDTH+110]==(0x8000u|(15u<<5)));
    CHECK(edgeAlpha[50*NITRO_GX3D_WIDTH+110]==31);
    CHECK(edgeImage[0]==(0x8000u|(7u|(7u<<5)|(7u<<10))));
    CHECK(edgeAlpha[0]==((31*17)>>5));
    CHECK(translucent);
    CHECK(NitroPort_Gx3dEdgeIdsUnexpected(edgeIds)==0);
    edgeIds[5]=(edgeIds[5]&0xffffffu)|(200u<<24);
    CHECK(NitroPort_Gx3dEdgeIdsUnexpected(edgeIds)==1);
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
    /* W-buffer depth is W in 20.12 (as the DS stores it), so it reaches
     * the far end at W = 4096.0, not at 16.0 as before: the title screen
     * draws Giratina about 300 units away. */
    farVertex.clip[3]=4096*4096;
    CHECK(NitroPort_Gx3dDepthValue(&farVertex,2)==1.0f);
    farVertex.clip[3]=4096*300;
    CHECK(NitroPort_Gx3dDepthValue(&farVertex,2)<1.0f);
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

/* The GPU thread hands images over unconverted; whoever acquires the
 * image converts it. */
#define HANDOFF_PIXELS (NITRO_GX3D_WIDTH*NITRO_GX3D_HEIGHT)
static uint32_t handoff[3][HANDOFF_PIXELS];
static uint32_t handoffIds[HANDOFF_PIXELS];
static void fill_handoff(uint32_t *image,uint32_t rgba)
{
    size_t i;
    for(i=0;i<HANDOFF_PIXELS;i++)image[i]=rgba;
}
static void test_image_submit(void)
{
    const uint16_t *got;
    uint32_t count;
    NitroGx3dEdgeSetup e;
    size_t i;
    /* One image: counted when handed over, converted when acquired, not
     * counted again; its buffer in use until then. */
    fill_handoff(handoff[0],0xff0000ffu);           /* red, opaque */
    count=NitroPort_Gx3dImageCount();
    CHECK(NitroPort_Gx3dSubmitRGBA8(handoff[0],NULL,NULL)==1);
    CHECK(NitroPort_Gx3dImageCount()==count+1);
    CHECK(NitroPort_Gx3dRawInUse(handoff[0]));
    CHECK(!NitroPort_Gx3dRawInUse(handoff[1]));
    got=NitroPort_Gx3dAcquireImage();
    CHECK(got!=NULL && got[0]==0x801fu && got[HANDOFF_PIXELS-1]==0x801fu);
    CHECK(!NitroPort_Gx3dRawInUse(handoff[0]));
    CHECK(NitroPort_Gx3dImageCount()==count+1);
    NitroPort_Gx3dReleaseImage(got);
    /* A newer one replaces one still waiting. */
    fill_handoff(handoff[1],0x00ff00ffu);           /* green */
    fill_handoff(handoff[2],0x0000ffffu);           /* blue */
    NitroPort_Gx3dSubmitRGBA8(handoff[1],NULL,NULL);
    NitroPort_Gx3dSubmitRGBA8(handoff[2],NULL,NULL);
    CHECK(!NitroPort_Gx3dRawInUse(handoff[1]));
    CHECK(NitroPort_Gx3dRawInUse(handoff[2]));
    got=NitroPort_Gx3dAcquireImage();
    CHECK(got!=NULL && got[0]==0xfc00u);
    NitroPort_Gx3dReleaseImage(got);
    /* Outlined as it's converted: an opaque image of polygon ID 8 on a
     * rear plane of ID 63, outlined round the screen's border. */
    memset(&e,0,sizeof(e));
    e.enabled=1;e.colors[1]=0x0421;e.clearId=63;e.clearDepth=0xffffffu;
    for(i=0;i<HANDOFF_PIXELS;i++)handoffIds[i]=(8u<<24)|100u;
    fill_handoff(handoff[0],0xff0000ffu);
    NitroPort_Gx3dSubmitRGBA8(handoff[0],handoffIds,&e);
    got=NitroPort_Gx3dAcquireImage();
    CHECK(got!=NULL && got[0]==0x8421u && got[NITRO_GX3D_WIDTH+1]==0x801fu);
    NitroPort_Gx3dReleaseImage(got);
    /* Converted before it's acquired (a drawer between frames): counted
     * once, its buffer free at once, and nothing left to convert. */
    CHECK(NitroPort_Gx3dConvertPending()==0);
    fill_handoff(handoff[1],0x00ff00ffu);           /* green */
    count=NitroPort_Gx3dImageCount();
    NitroPort_Gx3dSubmitRGBA8(handoff[1],NULL,NULL);
    CHECK(NitroPort_Gx3dConvertPending()==1);
    CHECK(!NitroPort_Gx3dRawInUse(handoff[1]));
    CHECK(NitroPort_Gx3dConvertPending()==0);
    CHECK(NitroPort_Gx3dImageCount()==count+1);
    got=NitroPort_Gx3dAcquireImage();
    CHECK(got!=NULL && got[0]==0x83e0u);
    NitroPort_Gx3dReleaseImage(got);
}
/* A GPU thread handing over images in three buffers, never drawing into
 * one in use; a drawer acquiring them meanwhile: every image it sees is
 * whole (one colour), and they come in order. */
static volatile int handoffDone;
static volatile unsigned handoffFailures;
static void *handoff_gpu(void *unused)
{
    unsigned frame,k;
    (void)unused;
    for(frame=1;frame<=400;frame++){
        for(k=0;k<2;k++)if(!NitroPort_Gx3dRawInUse(handoff[k]))break;
        if(NitroPort_Gx3dRawInUse(handoff[k])){handoffFailures++;continue;}
        /* The frame number in red and green, opaque. */
        fill_handoff(handoff[k],((frame&31u)<<27)|(((frame>>5)&31u)<<19)|255u);
        NitroPort_Gx3dSubmitRGBA8(handoff[k],NULL,NULL);
        if((frame&7)==0)sched_yield();
    }
    __atomic_store_n(&handoffDone,1,__ATOMIC_RELEASE);
    return NULL;
}
static void *handoff_drawer(void *unused)
{
    unsigned last=0,turn=0;
    (void)unused;
    while(!__atomic_load_n(&handoffDone,__ATOMIC_ACQUIRE)){
        const uint16_t *got;
        /* Every other time, converted between "frames" first (as the 3D
         * screen's drawer does when it's woken for a new image). */
        if(++turn&1){
            NitroPort_Gx3dConvertPending();
            sched_yield();
        }
        got=NitroPort_Gx3dAcquireImage();
        if(got){
            uint16_t first=got[0];
            unsigned frame=(first&31u)|(((first>>5)&31u)<<5);
            size_t i;
            for(i=0;i<HANDOFF_PIXELS;i+=97)if(got[i]!=first){handoffFailures++;break;}
            if(got[HANDOFF_PIXELS-1]!=first)handoffFailures++;
            if(frame<last)handoffFailures++;
            last=frame;
            NitroPort_Gx3dReleaseImage(got);
        }
        sched_yield();
    }
    return NULL;
}
static void test_concurrent_submit(void)
{
    pthread_t gpu,drawer;
    const uint16_t *got;
    handoffDone=0;handoffFailures=0;
    /* Frame 0 first, so the drawer never sees an earlier test's image. */
    fill_handoff(handoff[2],255u);
    NitroPort_Gx3dSubmitRGBA8(handoff[2],NULL,NULL);
    got=NitroPort_Gx3dAcquireImage();
    CHECK(got!=NULL && got[0]==0x8000u);
    NitroPort_Gx3dReleaseImage(got);
    CHECK(pthread_create(&gpu,NULL,handoff_gpu,NULL)==0);
    CHECK(pthread_create(&drawer,NULL,handoff_drawer,NULL)==0);
    pthread_join(gpu,NULL);
    pthread_join(drawer,NULL);
    CHECK(handoffFailures==0);
}

/* ---- Stage 2 PR2: textures and batches ---------------------------------- */

static unsigned char texSlots[4][0x20000];
static unsigned char palSlots[6][0x4000];
static uint32_t rng=12345u;
static uint32_t next_random(void){rng=rng*1103515245u+12345u;return rng>>8;}
static NitroGx3dTexBanks test_banks(void)
{
    NitroGx3dTexBanks b;
    int i;
    for(i=0;i<4;i++)b.texture[i]=texSlots[i];
    for(i=0;i<6;i++)b.palette[i]=palSlots[i];
    return b;
}
static uint32_t tex_param(unsigned fmt,unsigned w,unsigned h,uint32_t addr,unsigned flags)
{
    unsigned sw=0,sh=0;
    while((8u<<sw)<w)sw++;
    while((8u<<sh)<h)sh++;
    return (addr/8u)|(sw<<20)|(sh<<23)|(fmt<<26)|flags;
}
/* Where the PICA reads texel (u, v) of a w x h tiled texture: the address
 * order of an 8x8 tile is x and y bits interleaved (x lowest), tiles in
 * rows, and stored row (h - 1 - t) for coordinate t. Written from the
 * hardware description, independently of port_gx3d_texture.c. */
static unsigned pica_index(unsigned w,unsigned h,float u,float v)
{
    unsigned x=(unsigned)(u*(float)w), t=(unsigned)(v*(float)h);
    unsigned y=h-1-t, bit, morton=0;
    for(bit=0;bit<3;bit++){
        morton|=((x>>bit)&1u)<<(2*bit);
        morton|=((y>>bit)&1u)<<(2*bit+1);
    }
    return (y/8u)*(w/8u)*64u+(x/8u)*64u+morton;
}
static void test_texture_decode(void)
{
    static uint32_t linear[64*32],tiled[64*32],direct[64*32];
    NitroGx3dTexBanks banks=test_banks();
    unsigned fmt,i;
    for(i=0;i<sizeof(texSlots);i++)((unsigned char*)texSlots)[i]=(unsigned char)next_random();
    for(i=0;i<sizeof(palSlots);i++)((unsigned char*)palSlots)[i]=(unsigned char)next_random();
    for(fmt=1;fmt<=7;fmt++){
        uint32_t param=tex_param(fmt,64,32,0x400,(fmt>=2&&fmt<=4)?(1u<<29):0);
        uint32_t pal=3;
        unsigned s,t,mismatch=0,tileMismatch=0;
        NitroPort_Gx3dDecodeTexture(&banks,param,pal,linear);
        NitroPort_Gx3dTileRGBA8(linear,64,32,tiled);
        NitroPort_Gx3dDecodeTextureTiled(&banks,param,pal,direct);
        for(t=0;t<32;t++)for(s=0;s<64;s++){
            NitroGx3dTexel texel=NitroPort_Gx3dSampleTexel(&banks,param,pal,(int)s,(int)t);
            uint32_t word=linear[t*64+s];
            unsigned r=word>>24,a=word&255u;
            if(r!=(unsigned)(((texel.rgb&31)<<3)|((texel.rgb&31)>>2)) ||
               a!=(unsigned)((texel.alpha<<3)|(texel.alpha>>2)))mismatch++;
            /* The GPU samples the middle of texel (s, t) from here. */
            if(tiled[pica_index(64,32,(s+0.5f)/64.0f,(t+0.5f)/32.0f)]!=word)tileMismatch++;
        }
        CHECK(mismatch==0);
        CHECK(tileMismatch==0);
        CHECK(memcmp(tiled,direct,sizeof(tiled))==0);
    }
    /* Colour 0 is transparent only with bit 29 (paletted formats 2-4). */
    {
        uint32_t param=tex_param(4,8,8,0x800,0);
        texSlots[0][0x800]=0;
        CHECK(NitroPort_Gx3dSampleTexel(&banks,param,0,0,0).alpha==31);
        CHECK(NitroPort_Gx3dSampleTexel(&banks,param|(1u<<29),0,0,0).alpha==0);
    }
    /* Format 0: opaque white, as the reference draws untextured polygons. */
    CHECK(NitroPort_Gx3dSampleTexel(&banks,0,0,3,3).rgb==0x7fff);
}
/* The row-at-a-time decoder against the sampler, texel by texel: every
 * format and size, palettes anywhere, textures that run from one slot into
 * the next or into one without a bank, and parts of textures. */
static uint32_t decodeWant[1024*64],decodeGot[1024*64];
static void test_texture_decode_fast(void)
{
    NitroGx3dTexBanks banks=test_banks();
    unsigned n,i,bad=0,badRect=0,badKept=0;
    for(i=0;i<sizeof(texSlots);i++)((unsigned char*)texSlots)[i]=(unsigned char)next_random();
    for(i=0;i<sizeof(palSlots);i++)((unsigned char*)palSlots)[i]=(unsigned char)next_random();
    for(n=0;n<400;n++){
        unsigned fmt=n%8,w=8u<<(next_random()%8),h=8u<<(next_random()%8);
        uint32_t addr,pal=next_random()%0x600,param;
        unsigned s,t,s0,t0,s1,t1;
        while(w*h>1024*64)h/=2;
        /* Near the end of a slot every few textures, else anywhere. */
        addr=(n%5==0)?((next_random()%4)*0x20000u+0x20000u-8u*(1u+next_random()%64)):
                      (next_random()%0x80000u)&~7u;
        param=tex_param(fmt,w,h,addr,(next_random()&1)?(1u<<29):0);
        banks=test_banks();
        if(n%7==0)banks.texture[next_random()%4]=NULL;
        if(n%11==0)banks.palette[next_random()%6]=NULL;
        NitroPort_Gx3dDecodeTexture(&banks,param,pal,decodeGot);
        NitroPort_Gx3dTileRGBA8(decodeGot,w,h,decodeWant);
        memset(decodeGot,0x5a,w*h*4);
        NitroPort_Gx3dDecodeTextureTiled(&banks,param,pal,decodeGot);
        if(memcmp(decodeGot,decodeWant,w*h*4)!=0)bad++;
        /* A part: those texels the same, the rest left as they were. */
        s0=next_random()%w;s1=s0+1+next_random()%(w-s0);
        t0=next_random()%h;t1=t0+1+next_random()%(h-t0);
        memset(decodeGot,0x5a,w*h*4);
        NitroPort_Gx3dDecodeTextureTiledRect(&banks,param,pal,decodeGot,s0,t0,s1+(n&8),t1+(n&16));
        for(t=0;t<h;t++)for(s=0;s<w;s++){
            unsigned at=pica_index(w,h,(s+0.5f)/(float)w,(t+0.5f)/(float)h);
            if(s>=s0&&s<s1+(n&8)&&t>=t0&&t<t1+(n&16)){
                if(decodeGot[at]!=decodeWant[at])badRect++;
            }else if(decodeGot[at]!=0x5a5a5a5au)badKept++;
        }
    }
    CHECK(bad==0);
    CHECK(badRect==0);
    CHECK(badKept==0);
    /* An empty part changes nothing. */
    memset(decodeGot,0x5a,64*4);
    NitroPort_Gx3dDecodeTextureTiledRect(&banks,tex_param(3,8,8,0,0),0,decodeGot,4,0,4,8);
    CHECK(decodeGot[0]==0x5a5a5a5au && decodeGot[63]==0x5a5a5a5au);
}
/* Any one byte of the texel data changes the hash (it's read a word at a
 * time), at every place in a word. */
static void test_texture_hash_bytes(void)
{
    NitroGx3dTexBanks banks=test_banks();
    uint32_t param=tex_param(4,16,8,0x2000,0);    /* 128 bytes */
    uint32_t h0=NitroPort_Gx3dTextureHash(&banks,param,0);
    unsigned i,changed=0;
    for(i=0;i<128;i++){
        texSlots[0][0x2000+i]^=0x80;
        changed+=NitroPort_Gx3dTextureHash(&banks,param,0)!=h0;
        texSlots[0][0x2000+i]^=0x80;
    }
    CHECK(changed==128);
    CHECK(NitroPort_Gx3dTextureHash(&banks,param,0)==h0);
}
extern unsigned char NitroPort_IoReg[];
extern unsigned char NitroPort_LcdcVram[];
static void test_palette_banks(void)
{
    /* VRAMCNT F/G: the offset field picks texture palette slot 0, 1, 4 or 5. */
    NitroGx3dTexBanks b;
    unsigned char saved[7];
    memcpy(saved,NitroPort_IoReg+0x240,7);
    memset(NitroPort_IoReg+0x240,0,7);
    NitroPort_IoReg[0x246]=0x83;            /* G, offset 0 */
    NitroPort_Gx3dMapTextureBanks(&b);
    CHECK(b.palette[0]==NitroPort_LcdcVram+0x94000);
    NitroPort_IoReg[0x245]=0x83|(3u<<3);    /* F, offset 3: slot 5 */
    NitroPort_IoReg[0x246]=0x83|(1u<<3);    /* G, offset 1: slot 1 */
    NitroPort_Gx3dMapTextureBanks(&b);
    CHECK(b.palette[5]==NitroPort_LcdcVram+0x90000);
    CHECK(b.palette[1]==NitroPort_LcdcVram+0x94000);
    CHECK(b.palette[0]==NULL);
    memcpy(NitroPort_IoReg+0x240,saved,7);
}
static void test_texture_hash(void)
{
    NitroGx3dTexBanks banks=test_banks();
    uint32_t param=tex_param(3,16,16,0x1000,0); /* 16 colours: 128 bytes */
    uint32_t pal=2;                            /* palette at 0x20 */
    uint32_t h0=NitroPort_Gx3dTextureHash(&banks,param,pal);
    CHECK(NitroPort_Gx3dTextureHash(&banks,param,pal)==h0);
    texSlots[0][0x1000+127]^=0x10;            /* last texel byte */
    CHECK(NitroPort_Gx3dTextureHash(&banks,param,pal)!=h0);
    texSlots[0][0x1000+127]^=0x10;
    CHECK(NitroPort_Gx3dTextureHash(&banks,param,pal)==h0);
    texSlots[0][0x1000+128]^=0x10;            /* just past it: not used */
    CHECK(NitroPort_Gx3dTextureHash(&banks,param,pal)==h0);
    palSlots[0][0x20+31]^=0x01;               /* the 16th colour */
    CHECK(NitroPort_Gx3dTextureHash(&banks,param,pal)!=h0);
    palSlots[0][0x20+31]^=0x01;
    palSlots[0][0x20+32]^=0x01;               /* a 17th: not used */
    CHECK(NitroPort_Gx3dTextureHash(&banks,param,pal)==h0);
    CHECK(NitroPort_Gx3dTextureHash(&banks,param,pal+1)!=h0);
    banks.texture[0]=NULL;                    /* the bank is unmapped */
    CHECK(NitroPort_Gx3dTextureHash(&banks,param,pal)!=h0);
}
static void set_triangle(NitroGx3dTriangle *tri,unsigned alpha,uint32_t texparam,uint32_t pal,unsigned mode)
{
    int k;
    memset(tri,0,sizeof(*tri));
    for(k=0;k<3;k++){
        tri->v[k].clip[3]=4096;
        tri->v[k].x=(k*50)<<8;tri->v[k].y=(k*30)<<8;
        tri->v[k].s=(float)(k*4);tri->v[k].t=(float)(k*2);
        tri->v[k].color=0x7fff;
    }
    tri->polygon_attr=(alpha<<16)|(mode<<4)|(3u<<6);
    tri->texture_param=texparam;
    tri->palette_base=pal;
}
static void test_batches(void)
{
    static NitroGx3dScene scene;
    static NitroGx3dGpuVertex verts[NITRO_GX3D_PICA_MAX_VERTICES];
    static NitroGx3dBatch batches[16];
    uint32_t texA=tex_param(4,32,16,0x2000,0);
    uint32_t texT=tex_param(1,8,8,0x3000,0);   /* A3I5: translucent pass */
    size_t n,nb=0;
    unsigned dropped=9;
    memset(&scene,0,sizeof(scene));
    set_triangle(&scene.triangles[0],31,texA,1,0);
    set_triangle(&scene.triangles[1],16,0,0,0);   /* translucent polygon */
    set_triangle(&scene.triangles[2],31,texA,1,0); /* joins batch 0 */
    set_triangle(&scene.triangles[3],0,texA,1,0);  /* wireframe: skipped */
    set_triangle(&scene.triangles[4],31,texT,2,0);
    set_triangle(&scene.triangles[5],31,texA,1,1); /* decal: new batch */
    set_triangle(&scene.triangles[6],31,0,5,0);    /* untextured: no palette */
    scene.count=7;
    n=NitroPort_Gx3dBuildBatches(&scene,verts,NITRO_GX3D_PICA_MAX_VERTICES,batches,16,&nb,&dropped);
    CHECK(n==18);
    CHECK(dropped==0);
    CHECK(nb==5);
    /* Opaque pass: tri 0+2 (texture A), 5 (decal), 6 (untextured). */
    CHECK(batches[0].first==0 && batches[0].count==6 && batches[0].texture_param==texA &&
          batches[0].palette_base==1 && !batches[0].translucent);
    CHECK(batches[1].count==3 && ((batches[1].polygon_attr>>4)&3u)==1u);
    CHECK(batches[2].count==3 && batches[2].texture_param==0 && batches[2].palette_base==0);
    /* Translucent pass, in submission order: tri 1, then 4. */
    CHECK(batches[3].first==12 && batches[3].translucent && batches[3].texture_param==0);
    CHECK(batches[4].first==15 && batches[4].translucent && batches[4].texture_param==texT);
    /* Polygon alpha in vertex alpha; texture coordinates in [0, 1] per texel. */
    CHECK(verts[12].color[3]==(16u<<3|16u>>2));
    CHECK(verts[0].color[3]==255);
    CHECK(verts[2].uv[0]==8.0f/32.0f && verts[2].uv[1]==4.0f/16.0f);
    /* Not enough room: what doesn't fit is counted. */
    n=NitroPort_Gx3dBuildBatches(&scene,verts,7,batches,16,&nb,&dropped);
    CHECK(n==6 && dropped==4 && nb==1);
    n=NitroPort_Gx3dBuildBatches(&scene,verts,NITRO_GX3D_PICA_MAX_VERTICES,batches,2,&nb,&dropped);
    CHECK(nb==2 && dropped==3 && n==9);  /* tri 0, 2, 5 fit */
}

/* Textures for the GPU's bilinear filter: each texel 2x2, and a
 * see-through texel coloured as an opaque one beside it (alpha 0 kept). */
static void test_filter_texels(void)
{
    /* 2x2: red opaque, transparent black; transparent black, blue opaque */
    static const uint32_t in[4]={0xFF0000FFu,0x00000000u,0x00000000u,0x0000FFFFu};
    uint32_t out[16],one[4];
    int x,y;
    NitroPort_Gx3dFilterTexels(in,2,2,2,out);
    for(y=0;y<4;y++)for(x=0;x<4;x++){
        static const uint32_t want[4]={0xFF0000FFu,0xFF000000u,0x0000FF00u,0x0000FFFFu};
        CHECK(out[y*4+x]==want[(y/2)*2+(x/2)]);
    }
    {
        /* tiled in one pass: as filtering, then tiling; untiling undoes tiling */
        static uint32_t big[16*8],lin[32*16],tiled[32*16],tiled2[32*16],back[16*8];
        unsigned i;
        for(i=0;i<16*8;i++)big[i]=(i%5==0)?0u:(0x01020300u*i)|0xFFu;
        NitroPort_Gx3dFilterTexels(big,16,8,2,lin);
        NitroPort_Gx3dTileRGBA8(lin,32,16,tiled);
        NitroPort_Gx3dFilterTexelsTiled(big,16,8,2,tiled2);
        CHECK(memcmp(tiled,tiled2,sizeof(tiled))==0);
        NitroPort_Gx3dFilterTexels(big,16,8,1,lin);
        NitroPort_Gx3dTileRGBA8(lin,16,8,tiled);
        NitroPort_Gx3dFilterTexelsTiled(big,16,8,1,tiled2);
        CHECK(memcmp(tiled,tiled2,16*8*4)==0);
        NitroPort_Gx3dTileRGBA8(big,16,8,tiled);
        NitroPort_Gx3dUntileRGBA8(tiled,16,8,back);
        CHECK(memcmp(back,big,sizeof(big))==0);
    }
    NitroPort_Gx3dFilterTexels(in,2,2,1,one);
    CHECK(one[0]==in[0] && one[3]==in[3]);
    CHECK(one[1]==0xFF000000u);              /* (left: red) */
    CHECK(one[2]==0x0000FF00u);              /* (right: blue) */
    CHECK((one[1]&0xFFu)==0 && (one[2]&0xFFu)==0);
}

int main(void) {
    test_filter_texels();
    test_mailbox();
    test_mailbox_unread();
    test_lock_contention();
    test_vertices();
    test_depth_direction();
    test_full_vertex_capacity();
    test_image_handoff();
    test_image_conversion();
    test_edge_marking();
    test_png_dump();
    test_concurrent_image_handoff();
    test_image_submit();
    test_concurrent_submit();
    test_texture_decode();
    test_texture_decode_fast();
    test_texture_hash();
    test_texture_hash_bytes();
    test_palette_banks();
    test_batches();
    printf("GX3D plumbing: %d checks, %d failures\n",nchecks,nfail);
    return nfail?1:0;
}
