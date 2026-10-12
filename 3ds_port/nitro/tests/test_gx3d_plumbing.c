/* Stage 2 Plumbing: invented triangles only, no ROM assets.
 * Concurrent producer / GPU-consumer handoff and clip-space vertex packing. */
#include <nitro_port_gx3d.h>
#include <nitro_port_gx3d_mailbox.h>
#include <nitro_port_gx3d_vertex.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
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
    CHECK(NitroPort_Gx3dBuildOpaqueVertices(&scene,verts,12)==3);
}
int main(void) {
    test_mailbox();
    test_vertices();
    printf("GX3D plumbing: %d checks, %d failures\n",nchecks,nfail);
    return nfail?1:0;
}
