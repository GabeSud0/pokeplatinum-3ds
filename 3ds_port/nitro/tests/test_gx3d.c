/*
 * No copyrighted game data. Geometry commands use the original ARM9 G3_*
 * API (or the exact packed FIFO bytes), then validate the exported scene and
 * the PC reference renderer independently of the 3DS GPU.
 */
#include <nitro.h>
#include <nitro_port.h>
#include <nitro_port_gx3d.h>
#include <nitro_port_gx3d_vertex.h>
#include <nitro_port_shadow.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <semaphore.h>

extern unsigned char NitroPort_IoReg[];
extern unsigned char NitroPort_LcdcVram[];
static int checks, failures;
static uint16_t pixels[256*192];
static uint32_t depths[256*192];
#define CHECK(x) do { checks++; if(!(x)){ failures++; \
    printf("FAIL %s:%d: %s\n",__FILE__,__LINE__,#x); } } while(0)
#define RGB(r,g,b) ((uint16_t)((r)|((g)<<5)|((b)<<10)))
#define ONE 4096
static uint32_t word2(int16_t x,int16_t y)
{
    return (uint16_t)x|((uint32_t)(uint16_t)y<<16);
}
static int32_t read32(unsigned offset)
{
    int32_t n;
    memcpy(&n,NitroPort_IoReg+offset,sizeof(n));
    return n;
}
static void write32(unsigned offset,uint32_t value)
{
    memcpy(NitroPort_IoReg+offset,&value,sizeof(value));
}
static void reset_geometry(void)
{
    NitroPort_Gx3dReset();
    /* G3X_Init configures the real DS rear-plane depth as 0x7FFF.
     * Do the same here; reset_geometry tests geometry, not G3X_Init. */
    NitroPort_IoReg[0x354]=0xff;
    NitroPort_IoReg[0x355]=0x7f;
    NitroPort_GxDirectWrite(0x10,0); /* projection */
    NitroPort_GxDirectWrite(0x15,0);
    NitroPort_GxDirectWrite(0x10,2); /* position + vector */
    NitroPort_GxDirectWrite(0x15,0);
    NitroPort_GxDirectWrite(0x60,0xbfFF0000u); /* viewport 0,0 to 255,191 */
    NitroPort_GxDirectWrite(0x29,(3u<<6)|(31u<<16));
    NitroPort_GxDirectWrite(0x20,0x7fff);
    NitroPort_GxDirectWrite(0x2a,0); /* no texture */
    NitroPort_GxDirectWrite(0x2b,0);
}
static void vertex(int x,int y,int z)
{
    NitroPort_GxDirectWrite(0x23,word2((int16_t)x,(int16_t)y));
    NitroPort_GxDirectWrite(0x23,(uint16_t)z);
}
static void triangle(int z,uint16_t color)
{
    NitroPort_GxDirectWrite(0x20,color);
    NitroPort_GxDirectWrite(0x40,0);
    vertex(-3000,-3000,z);
    vertex(3000,-3000,z);
    vertex(0,3000,z);
    NitroPort_GxDirectWrite(0x41,0);
}
static void quad(int z,uint16_t color)
{
    NitroPort_GxDirectWrite(0x20,color);
    NitroPort_GxDirectWrite(0x40,1); /* quads */
    vertex(-3000,-3000,z);
    vertex(3000,-3000,z);
    vertex(3000,3000,z);
    vertex(-3000,3000,z);
    NitroPort_GxDirectWrite(0x41,0);
}
static void swap(void){NitroPort_GxDirectWrite(0x50,0);}
/* The clip / vector matrix results, read as the SDK reads them (the port
 * brings the registers up to date then, not after every matrix command). */
static int32_t clip_result(int i)
{
    MtxFx44 m;
    CHECK(G3X_GetClipMtx(&m)==0);
    return ((const fx32 *)&m)[i];
}
static int32_t vec_result(int i)
{
    MtxFx33 m;
    CHECK(G3X_GetVectorMtx(&m)==0);
    return ((const fx32 *)&m)[i];
}
static void test_matrices(void)
{
    reset_geometry();
    NitroPort_GxDirectWrite(0x10,2);
    G3_Translate(ONE,0,0);
    NitroPort_Gx3dRefreshStatus();
    CHECK(clip_result(12)==ONE); /* clip matrix's translation */
    G3_PushMtx();
    G3_Translate(2*ONE,0,0);
    NitroPort_Gx3dRefreshStatus();
    CHECK(clip_result(12)==3*ONE);
    CHECK(((uint32_t)read32(0x600)>>8&31)==1);
    G3_PopMtx(1);
    NitroPort_Gx3dRefreshStatus();
    CHECK(clip_result(12)==ONE);
    CHECK(((uint32_t)read32(0x600)>>8&31)==0);
    G3_StoreMtx(2);
    G3_Translate(2*ONE,0,0);
    G3_RestoreMtx(2);
    NitroPort_Gx3dRefreshStatus();
    CHECK(clip_result(12)==ONE);
    /* Projection stack has only one entry; overflowing sets GXSTAT bit15. */
    G3_MtxMode(GX_MTXMODE_PROJECTION);
    G3_PushMtx();G3_PushMtx();
    NitroPort_Gx3dRefreshStatus();
    CHECK((read32(0x600)&0x8000)!=0);
    G3X_ResetMtxStackOverflow();
    NitroPort_Gx3dRefreshStatus();
    CHECK((read32(0x600)&0x8000)==0);

    reset_geometry();
    G3_Scale(2*ONE,3*ONE,ONE);
    NitroPort_Gx3dRefreshStatus();
    CHECK(clip_result(0)==2*ONE);
    CHECK(clip_result(5)==3*ONE);
    /* Matrix mode 2 excludes scale from the directional matrix. */
    CHECK(vec_result(0)==ONE);

    /* Multiplies go on the left: Current = Parameter x Current (GBATEK).
     * A model's scale after a camera that moves things by 5 doesn't scale
     * the camera's move (NNS draws camera, then each model's transform). */
    reset_geometry();
    NitroPort_GxDirectWrite(0x10,2);
    G3_Translate(5*ONE,0,0);
    G3_Scale(2*ONE,ONE,ONE);
    NitroPort_Gx3dRefreshStatus();
    CHECK(clip_result(12)==5*ONE);
    CHECK(clip_result(0)==2*ONE);
    /* ... and a model's move after a camera rotation is rotated: a 90
     * degree turn about Z (x -> y) then a move of 3 along x moves along x
     * first, then turns: the result moves along y. */
    reset_geometry();
    NitroPort_GxDirectWrite(0x10,2);
    {
        MtxFx43 turn;
        MTX_Identity43(&turn);
        turn._00=0; turn._01=ONE; turn._10=-ONE; turn._11=0;
        G3_MultMtx43(&turn);
    }
    G3_Translate(3*ONE,0,0);
    NitroPort_Gx3dRefreshStatus();
    CHECK(clip_result(12)==0);
    CHECK(clip_result(13)==3*ONE);
}
static void test_vtx_diff(void)
{
    /* VTX_DIFF adds 10-bit signed differences in 1/4096 units (as the SDK's
     * GX_VTXDIFF_MAX = 0x1ff fx32 says): after VTX_16 (100, 200, 300) a
     * diff of (+511, -512, -1) gives (611, -312, 299). */
    const NitroGx3dScene *scene;
    reset_geometry();
    NitroPort_GxDirectWrite(0x40,0);
    vertex(100,200,300);
    NitroPort_GxDirectWrite(0x28,(511u&0x3ffu)|((uint32_t)(-512&0x3ff)<<10)|((uint32_t)(-1&0x3ff)<<20));
    vertex(0,3000,0);
    NitroPort_GxDirectWrite(0x41,0);
    swap();
    scene=NitroPort_Gx3dScene();
    CHECK(scene->count==1);
    if(scene->count){
        const NitroGx3dTriangle *t=&scene->triangles[0];
        int k,found=0;
        for(k=0;k<3;k++)
            if(t->v[k].clip[0]==611 && t->v[k].clip[1]==-312 && t->v[k].clip[2]==299)found=1;
        CHECK(found);
    }
}
static void test_packed_fifo(void)
{
    const NitroGx3dScene *scene;
    reset_geometry();
    /* Packed command IDs: mode, identity, begin, NOP. */
    NitroPort_GxFifoWord(0x00401510u);
    NitroPort_GxFifoWord(1); /* position matrix */
    NitroPort_GxFifoWord(0); /* triangles */
    NitroPort_GxFifoWord(0x00232323u); /* three VTX_16 */
    NitroPort_GxFifoWord(word2(-3000,-3000));
    NitroPort_GxFifoWord(0);
    NitroPort_GxFifoWord(word2(3000,-3000));
    NitroPort_GxFifoWord(0);
    NitroPort_GxFifoWord(word2(0,3000));
    NitroPort_GxFifoWord(0);
    NitroPort_GxFifoWord(0x00000050u);
    NitroPort_GxFifoWord(0);
    scene=NitroPort_Gx3dScene();
    CHECK(scene->count==1);
    CHECK(scene->triangles[0].v[0].clip[0]==-3000);
    CHECK((read32(0x600)&(3u<<25))==(3u<<25));
}
static void test_triangles_and_depth(void)
{
    const NitroGx3dScene *scene;
    reset_geometry();
    triangle(1200,RGB(31,0,0));
    triangle(-1200,RGB(0,31,0));
    swap();
    scene=NitroPort_Gx3dScene();
    CHECK(scene->count==2);
    NitroPort_Gx3dRasterize(pixels,depths);
    CHECK((pixels[96*256+128]&0x7fff)==RGB(0,31,0));
    CHECK((pixels[0]&0x8000)==0); /* transparent CLEAR_COLOR by default */
    CHECK(depths[96*256+128]!=depths[0]);
}
static void test_culling(void)
{
    reset_geometry();
    NitroPort_GxDirectWrite(0x29,(0u<<6)|(31u<<16));
    triangle(0,RGB(31,0,0));
    swap();
    CHECK(NitroPort_Gx3dScene()->count==0);
    reset_geometry();
    NitroPort_GxDirectWrite(0x29,(3u<<6)|(31u<<16));
    triangle(0,RGB(31,0,0));
    swap();
    CHECK(NitroPort_Gx3dScene()->count==1);
}
static void test_textures(void)
{
    uint32_t fmt4=(4u<<26); /* indexed 8x8, 256-colour palette */
    unsigned i;
    reset_geometry();
    /* VRAM bank A as texture slot 0, E as palette bank 0. */
    NitroPort_IoReg[0x240]=0x83;
    NitroPort_IoReg[0x244]=0x83;
    memset(NitroPort_LcdcVram,0,0x200);
    for(i=0;i<64;i++)NitroPort_LcdcVram[i]=1;
    NitroPort_LcdcVram[0x80000+2]=31;
    NitroPort_LcdcVram[0x80000+3]=0;
    NitroPort_GxDirectWrite(0x2a,fmt4);
    quad(0,RGB(31,31,31));
    swap();
    CHECK(NitroPort_Gx3dScene()->count==2);
    NitroPort_Gx3dRasterize(pixels,NULL);
    CHECK((pixels[96*256+128]&0x7fff)==RGB(31,0,0));
    /* Direct-colour texture, nonzero texel with bit15 opaque. */
    reset_geometry();
    NitroPort_LcdcVram[0]=0xe0;NitroPort_LcdcVram[1]=0x83; /* blue + visible */
    NitroPort_GxDirectWrite(0x2a,(7u<<26));
    triangle(0,RGB(31,31,31));swap();
    NitroPort_Gx3dRasterize(pixels,NULL);
    CHECK((pixels[96*256+128]&0x7fff)==(0x83e0u&0x7fff));

    /* A 4x4-compressed 8x8 texture: all four-texel rows use colour #1.
     * Attribute descriptor in slot 1 picks direct palette mode 2. */
    reset_geometry();
    NitroPort_IoReg[0x241]=0x8b; /* bank B -> texture slot 1 */
    NitroPort_LcdcVram[0]=0x55;
    NitroPort_LcdcVram[1]=0x55;
    NitroPort_LcdcVram[2]=0x55;
    NitroPort_LcdcVram[3]=0x55;
    NitroPort_LcdcVram[0x20000]=0;
    NitroPort_LcdcVram[0x20001]=0x80; /* mode 2, base palette 0 */
    NitroPort_GxDirectWrite(0x2a,5u<<26);
    quad(0,RGB(31,31,31));
    swap();
    NitroPort_Gx3dRasterize(pixels,NULL);
    CHECK((pixels[96*256+128]&0x7fff)==RGB(31,0,0));
}
/* Vertices all out of the view, in display lists sent in pieces (as
 * NitroSystem's geometry buffer does) and a word at a time: no polygon of
 * theirs is drawn, and what they leave behind (the last coordinates, the
 * colour) holds for the vertices after them. (With the geometry thread,
 * such lists are passed over without working their vertices out.) */
static void test_hidden_vertices(void)
{
    const uint32_t part1[]={
        0x23232040u, 1, RGB(0,0,31),                 /* BEGIN quads, COLOR, VTX_16 x2 */
        word2(-9000,-3000), 777, word2(-6000,-3000), 777,
    };
    const uint32_t part2[]={
        0x25402323u,                                 /* VTX_16 x2, BEGIN triangles, VTX_XY */
        word2(-6000,3000), 777, word2(-9000,3000), 1234,
        0, word2(-3000,-3000),
    };
    const uint32_t part3[]={
        0x00412525u, word2(3000,-3000), word2(0,3000),   /* VTX_XY x2, END */
    };
    const NitroGx3dScene *scene;
    reset_geometry();
    NitroPort_GxDirectWrite(0x29,(3u<<6)|(31u<<16));     /* both sides */
    NitroPort_GxFifoWords(part1,sizeof(part1)/4);
    NitroPort_GxFifoWords(part2,sizeof(part2)/4);
    NitroPort_GxFifoWords(part3,sizeof(part3)/4);
    swap();
    scene=NitroPort_Gx3dScene();
    /* Only the triangle: blue (the hidden quad's colour), at the hidden
     * quad's last Z (its X/Y-only vertices keep it). */
    CHECK(scene->count==1);
    if(scene->count==1){
        CHECK(scene->triangles[0].v[0].color==RGB(0,0,31));
        CHECK(scene->triangles[0].v[0].clip[2]==1234);
        CHECK(scene->triangles[0].v[0].clip[0]==-3000);
    }
    /* A strip out to the right, its last vertex and END written a word at
     * a time; then one partly in view, which is drawn. */
    reset_geometry();
    NitroPort_GxDirectWrite(0x29,(3u<<6)|(31u<<16));
    {
        const uint32_t strip[]={
            0x23232040u, 2, RGB(31,0,0), word2(6000,-3000), 5, word2(6000,3000), 6,
            0x00000023u, word2(9000,0), 7,
        };
        const uint32_t partly[]={
            0x23232340u, 0, word2(2000,-3000), 8, word2(6000,-3000), 8, word2(6000,3000), 8,
        };
        NitroPort_GxFifoWords(strip,7);
        NitroPort_GxFifoWord(strip[7]);
        NitroPort_GxFifoWord(strip[8]);
        NitroPort_GxFifoWord(strip[9]);
        NitroPort_GxFifoWord(0x41u);                     /* END */
        NitroPort_GxFifoWord(0x20u);                     /* COLOR */
        NitroPort_GxFifoWord(RGB(0,31,0));
        NitroPort_GxFifoWords(partly,sizeof(partly)/4);
        NitroPort_GxFifoWord(0x41u);
    }
    swap();
    scene=NitroPort_Gx3dScene();
    CHECK(scene->count>=1);
    if(scene->count>=1){
        CHECK(scene->triangles[0].v[0].color==RGB(0,31,0));
        CHECK(scene->triangles[0].v[0].clip[2]==8);
    }
}
/* Triangle strips: v0 v1 v2, then v2 v1 v3, v2 v3 v4 (GBATEK): each
 * triangle after the first uses the strip's last two vertices. */
static int strip_vertex_index(const NitroGx3dVertex *v, const int32_t *xs)
{
    int i;
    for(i=0;i<5;i++)if(v->x==xs[i])return i;
    return -1;
}
static void test_triangle_strip(void)
{
    static const int px[5]={-3000,-1500,0,1500,3000};
    static const int py[5]={-2000,2000,-2000,2000,-2000};
    int32_t xs[5];
    const NitroGx3dScene *scene;
    unsigned i;
    reset_geometry();
    NitroPort_GxDirectWrite(0x29,(3u<<6)|(31u<<16));    /* both sides */
    NitroPort_GxDirectWrite(0x40,2);                     /* triangle strip */
    for(i=0;i<5;i++)vertex(px[i],py[i],0);
    NitroPort_GxDirectWrite(0x41,0);
    swap();
    scene=NitroPort_Gx3dScene();
    CHECK(scene->count==3);
    /* Screen x of each strip vertex (1/256 pixels, as the scene has them). */
    for(i=0;i<5;i++)xs[i]=(int32_t)((((double)px[i]+4096)*256/(2*4096))*256+0.5);
    if(scene->count==3){
        static const int want[3][3]={{0,1,2},{2,1,3},{2,3,4}};
        for(i=0;i<3;i++){
            int k;
            for(k=0;k<3;k++)CHECK(strip_vertex_index(&scene->triangles[i].v[k],xs)==want[i][k]);
        }
    }
}

static void test_box_test(void)
{
    reset_geometry();
    /* Origin far outside the right clip plane, tiny 4.12 box. */
    NitroPort_GxDirectWrite(0x70,word2(5*ONE,0));
    NitroPort_GxDirectWrite(0x70,word2(0,ONE));
    NitroPort_GxDirectWrite(0x70,word2(ONE,ONE));
    NitroPort_Gx3dRefreshStatus();
    CHECK((read32(0x600)&2)==0);
    NitroPort_GxDirectWrite(0x70,word2(0,0));
    NitroPort_GxDirectWrite(0x70,word2(0,ONE));
    NitroPort_GxDirectWrite(0x70,word2(ONE,ONE));
    NitroPort_Gx3dRefreshStatus();
    CHECK((read32(0x600)&2)!=0);
}
/* With the sun shadows on, what isn't drawn but may cast a shadow into the
 * view is kept for them (shadow_triangles): a triangle facing away, one
 * outside the view's sides (within 4 w), one crossing them (whole); not a
 * translucent one, nor one too far out; box tests pass what's within 2 w. The scene after a SWAP_BUFFERS with them on. */
static void flat_triangle(int x0,int x1,int y0,int y1)
{
    NitroPort_GxDirectWrite(0x40,0);
    vertex(x0,y0,0);
    vertex(x1,y0,0);
    vertex((x0+x1)/2,y1,0);
    NitroPort_GxDirectWrite(0x41,0);
}
/* A quad 2000 wide, its left side at x (attr: POLYGON_ATTR). */
static void quad_at(int x,uint32_t attr)
{
    NitroPort_GxDirectWrite(0x29,attr);
    NitroPort_GxDirectWrite(0x20,0x7fff);
    NitroPort_GxDirectWrite(0x40,1);
    vertex(x,-1000,0);
    vertex(x+2000,-1000,0);
    vertex(x+2000,1000,0);
    vertex(x,1000,0);
    NitroPort_GxDirectWrite(0x41,0);
}
/* A scene's end, and (with the geometry thread) the thread caught up: it
 * leaves out scenes it's two behind on, and their polygons with them. */
static void swap_done(void){swap();NitroPort_Gx3dScene();}
/* The in-between scenes (NitroPort_Gx3dSetInterpolation): a polygon
 * halfway between its places in the scenes either side; one with no
 * match, or one that jumped, where the newer scene has it; none after a
 * cut, nor with them off. */
static void test_interpolation(void)
{
    const NitroGx3dScene *mid;
    unsigned matched=99,polygons=99;
    int i;
    const uint32_t opaque=(3u<<6)|(31u<<16);
    reset_geometry();
    NitroPort_Gx3dSetInterpolation(1);
    swap_done();                             /* (on from here) */
    quad_at(-3000,opaque);
    swap_done();
    CHECK(NitroPort_Gx3dInterpolation());
    CHECK(NitroPort_Gx3dMidScene(NULL,NULL)==NULL);   /* (nothing before it) */
    quad_at(-2800,opaque);
    swap_done();
    mid=NitroPort_Gx3dMidScene(&matched,&polygons);
    CHECK(mid!=NULL);
    CHECK(matched==1 && polygons==1);
    CHECK(mid && mid->count==2);
    if(mid && mid->count==2){
        CHECK(mid->triangles[0].v[0].clip[0]==-2900);   /* halfway: (-3000 + -2800) / 2 */
        CHECK(mid->triangles[0].v[1].clip[0]==-900);
        CHECK(mid->triangles[0].v[0].clip[3]==ONE);
    }
    /* a new one (no match: where it is), and the quad on */
    quad_at(-2600,opaque);
    triangle(0,0x001f);
    swap_done();
    mid=NitroPort_Gx3dMidScene(&matched,&polygons);
    CHECK(mid && matched==1 && polygons==2);
    if(mid && mid->count==3){
        CHECK(mid->triangles[0].v[0].clip[0]==-2700);
        CHECK(mid->triangles[2].v[0].clip[0]==-3000);  /* the triangle as drawn */
    }else CHECK(0);
    {
        /* the display's way: the acquired scene's own in-between scene,
         * kept with it while it's held (scenes made meanwhile have theirs) */
        uint32_t serial=0;
        const NitroGx3dScene *scene=NitroPort_Gx3dAcquireScene(&serial);
        const NitroGx3dScene *held=scene?NitroPort_Gx3dMidOf(scene):NULL;
        CHECK(scene!=NULL && held==mid);
        CHECK(NitroPort_Gx3dMidOf(NULL)==NULL);
        quad_at(-2400,opaque);
        swap_done();
        CHECK(held && held->count==3 && held->triangles[0].v[0].clip[0]==-2700);
        CHECK(NitroPort_Gx3dMidScene(NULL,NULL)!=held);
        if(scene)NitroPort_Gx3dReleaseScene(scene);
        quad_at(-2600,opaque);
        triangle(0,0x001f);
        swap_done();
    }
    /* a new one while the scene moves (8 quads 200 to the right: 6.25 DS
     * pixels): where the scene's motion puts it halfway, 100 back (a
     * model coming into the view, not half a step ahead of the rest) */
    for(i=0;i<8;i++)quad_at(-4000+500*i,opaque);
    swap_done();
    for(i=0;i<8;i++)quad_at(-3800+500*i,opaque);
    triangle(0,0x001f);
    swap_done();
    mid=NitroPort_Gx3dMidScene(&matched,&polygons);
    CHECK(mid && matched==8 && polygons==9);
    if(mid && mid->count==17){
        CHECK(mid->triangles[0].v[0].clip[0]==-3900);   /* (a quad: halfway) */
        CHECK(mid->triangles[16].v[0].clip[0]==-3100);  /* the triangle */
        CHECK(mid->triangles[16].v[0].clip[1]==-3000);
    }else CHECK(0);
    /* and one the scene's motion takes out of the view: halfway out (on
     * the right edge, 100 further right, cut at x = w), not gone early */
    for(i=0;i<8;i++)quad_at(-4000+500*i,opaque);
    NitroPort_GxDirectWrite(0x20,0x03e0);
    NitroPort_GxDirectWrite(0x40,0);
    vertex(3900,-100,0);vertex(4090,-100,0);vertex(4000,100,0);
    NitroPort_GxDirectWrite(0x41,0);
    swap_done();
    for(i=0;i<8;i++)quad_at(-3800+500*i,opaque);
    swap_done();
    mid=NitroPort_Gx3dMidScene(&matched,&polygons);
    CHECK(mid && matched==8 && polygons==8);
    if(mid && mid->count>16){
        const NitroGx3dTriangle *t=&mid->triangles[16];
        int k,lo=99999,hi=-99999;
        for(k=0;k<3;k++){
            if(t->v[k].clip[0]<lo)lo=t->v[k].clip[0];
            if(t->v[k].clip[0]>hi)hi=t->v[k].clip[0];
        }
        CHECK(t->v[0].color==0x03e0);
        CHECK(lo==4000 && hi==4096);
    }else CHECK(0);
    quad_at(-2600,opaque);
    triangle(0,0x001f);
    swap_done();
    /* a jump (112 DS pixels in a scene): where it is now */
    quad_at(1000,opaque);
    swap_done();
    mid=NitroPort_Gx3dMidScene(&matched,&polygons);
    CHECK(mid && matched==0 && polygons==1);
    CHECK(mid && mid->count==2 && mid->triangles[0].v[0].clip[0]==1000);
    /* a cut: 20 polygons, none of the kind before */
    for(i=0;i<20;i++)triangle(0,0x7fff);
    swap_done();
    for(i=0;i<20;i++)quad_at(-3000,opaque|(1u<<24));   /* (polygon ID 1: another kind) */
    swap_done();
    CHECK(NitroPort_Gx3dMidScene(&matched,&polygons)==NULL);
    CHECK(matched==0 && polygons==20);
    /* off */
    NitroPort_Gx3dSetInterpolation(0);
    quad_at(-3000,opaque);
    swap_done();
    quad_at(-2800,opaque);
    swap_done();
    CHECK(!NitroPort_Gx3dInterpolation());
    CHECK(NitroPort_Gx3dMidScene(NULL,NULL)==NULL);
}
/* (a small quad, its corner at x, y) */
static void small_quad_at(int x,int y,uint32_t attr)
{
    NitroPort_GxDirectWrite(0x29,attr);
    NitroPort_GxDirectWrite(0x20,0x7fff);
    NitroPort_GxDirectWrite(0x40,1);
    vertex(x,y,0);
    vertex(x+400,y,0);
    vertex(x+400,y+400,0);
    vertex(x,y+400,0);
    NitroPort_GxDirectWrite(0x41,0);
}
/* What the camera follows (the player, and one who follows him: Cheryl in
 * Eterna Forest) stays where it was on the screen as the scene moves. The
 * scene moving further than half the gap between the two (at a run, a
 * scene left out): the follower in its own place halfway, not halfway
 * from where the player was (she jumped about). */
static void test_interpolation_followed(void)
{
    const NitroGx3dScene *mid;
    unsigned matched=0,polygons=0;
    int i,scene;
    const uint32_t opaque=(3u<<6)|(31u<<16);
    reset_geometry();
    NitroPort_Gx3dSetInterpolation(1);
    swap_done();
    /* the map: 8 of them, 256 (8 DS pixels) to the left each scene; the
     * follower 448 (14 DS pixels) left of the player, both still */
    for(scene=0;scene<2;scene++){
        for(i=0;i<8;i++)small_quad_at(-3600+900*i-256*scene,-3000,opaque);
        small_quad_at(-448,0,opaque);
        small_quad_at(0,0,opaque);
        swap_done();
    }
    mid=NitroPort_Gx3dMidScene(&matched,&polygons);
    CHECK(mid && matched==10 && polygons==10);
    if(mid && mid->count==20){
        CHECK(mid->triangles[0].v[0].clip[0]==-3600-128);   /* the map: halfway */
        CHECK(mid->triangles[16].v[0].clip[0]==-448);       /* the follower: where she is */
        CHECK(mid->triangles[18].v[0].clip[0]==0);          /* the player */
    }else CHECK(0);
    /* and one who walks on her own, past the player: still halfway */
    for(scene=0;scene<2;scene++){
        for(i=0;i<8;i++)small_quad_at(-3600+900*i,-3000,opaque);
        small_quad_at(-448+64*scene,0,opaque);
        small_quad_at(0,0,opaque);
        swap_done();
    }
    mid=NitroPort_Gx3dMidScene(&matched,&polygons);
    CHECK(mid && mid->count==20 && mid->triangles[16].v[0].clip[0]==-448+32);
    NitroPort_Gx3dSetInterpolation(0);
}
/* A figure new in the scene beside another of its kind already there,
 * both still (the follower's Poké Ball made again beside the player,
 * 2026-10-29): the player is paired with himself; the new one with
 * nothing, where it is, not halfway from the player. */
static void test_interpolation_one_each(void)
{
    const NitroGx3dScene *mid;
    unsigned matched=0,polygons=0;
    int i,scene;
    const uint32_t opaque=(3u<<6)|(31u<<16);
    reset_geometry();
    NitroPort_Gx3dSetInterpolation(1);
    swap_done();
    for(scene=0;scene<2;scene++){
        for(i=0;i<8;i++)small_quad_at(-3600+900*i,-3000,opaque);
        if(scene==1)small_quad_at(-448,0,opaque);   /* (the new one, before the player) */
        small_quad_at(0,0,opaque);
        swap_done();
    }
    mid=NitroPort_Gx3dMidScene(&matched,&polygons);
    CHECK(mid && polygons==10 && matched==9);
    if(mid && mid->count==20){
        CHECK(mid->triangles[16].v[0].clip[0]==-448);   /* the new one: where it is */
        CHECK(mid->triangles[18].v[0].clip[0]==0);      /* the player */
    }else CHECK(0);
    NitroPort_Gx3dSetInterpolation(0);
}
/* A map's block coming into the view: a new tile at the list's start,
 * all the rest one place on in the order, so the order pairs each tile
 * with its neighbour (Route 205's lawn at a run: stretched stairs and
 * holes in the in-between scene). The scene's motion the most of them fit
 * (the scene before's here): each tile halfway along it. */
static void test_interpolation_order_shift(void)
{
    const NitroGx3dScene *mid;
    unsigned matched=0,polygons=0;
    int i,scene;
    const uint32_t opaque=(3u<<6)|(31u<<16);
    reset_geometry();
    NitroPort_Gx3dSetInterpolation(1);
    swap_done();
    /* 16 tiles, 256 (8 DS pixels) to the left each scene */
    for(scene=0;scene<3;scene++){
        if(scene==2)small_quad_at(3200-256*scene,1000,opaque);   /* (the new one, first) */
        for(i=0;i<16;i++)small_quad_at(-3600+450*i+70*(i%3)-256*scene,-3000,opaque);
        swap_done();
    }
    mid=NitroPort_Gx3dMidScene(&matched,&polygons);
    CHECK(mid && polygons==17 && matched>=16);
    if(mid && mid->count>=34){
        int ok=1;
        for(i=0;i<16;i++){
            /* (after the new one's two triangles) */
            if(mid->triangles[2+2*i].v[0].clip[0]!=-3600+450*i+70*(i%3)-256-128)ok=0;
        }
        CHECK(ok);
    }else CHECK(0);
    NitroPort_Gx3dSetInterpolation(0);
}
/* A floor moving a tile a scene (the bike's top gear): each tile where
 * its neighbour was, rows of them further apart nearer the camera (the
 * perspective). Still a camera moving: each tile halfway along its own
 * row's motion, not where it is now. */
static void test_interpolation_tile_a_scene(void)
{
    const NitroGx3dScene *mid;
    unsigned matched=0,polygons=0;
    int i,r,scene,bad=0,t=0;
    const uint32_t opaque=(3u<<6)|(31u<<16);
    reset_geometry();
    NitroPort_Gx3dSetInterpolation(1);
    swap_done();
    for(scene=0;scene<3;scene++){
        for(r=0;r<4;r++){
            int sp=256+64*r;
            for(i=0;i<16;i++)small_quad_at(-3000+sp*(i-scene),-3000+1500*r,opaque);
        }
        swap_done();
    }
    mid=NitroPort_Gx3dMidScene(&matched,&polygons);
    CHECK(mid && polygons==64);
    if(mid && mid->count==128){
        for(r=0;r<4;r++){
            int sp=256+64*r;
            for(i=0;i<16;i++,t++){
                int want=-3000+sp*(i-2)+sp/2,got=mid->triangles[2*t].v[0].clip[0];
                if(got<want-8 || got>want+8)bad++;
            }
        }
        CHECK(bad==0);
    }else CHECK(0);
    NitroPort_Gx3dSetInterpolation(0);
}
/* A model that sways where it is (the starters' Poké Balls in their
 * briefcase): its parts a few pixels from where they were, some nearer a
 * neighbour of their kind than their own place. The scene still: each
 * part with the one the order gives (bits of the wrong ones made grey
 * blocks). */
static void test_interpolation_swaying(void)
{
    const NitroGx3dScene *mid;
    unsigned matched=0,polygons=0;
    int i,scene;
    const uint32_t opaque=(3u<<6)|(31u<<16);
    reset_geometry();
    NitroPort_Gx3dSetInterpolation(1);
    swap_done();
    for(scene=0;scene<2;scene++){
        /* the still room around it: another kind */
        for(i=0;i<12;i++)small_quad_at(-3600+600*i,-3000,opaque|(1u<<24));
        /* the model: 6 parts 120 apart, every other one 100 right, the
         * rest 100 left */
        for(i=0;i<6;i++)small_quad_at(-600+120*i+(scene?(i&1?-100:100):0),0,opaque);
        swap_done();
    }
    mid=NitroPort_Gx3dMidScene(&matched,&polygons);
    CHECK(mid && polygons==18 && matched==18);
    if(mid && mid->count==36){
        int ok=1;
        for(i=0;i<6;i++){
            if(mid->triangles[24+2*i].v[0].clip[0]!=-600+120*i+(i&1?-50:50))ok=0;
        }
        CHECK(ok);
    }else CHECK(0);
    NitroPort_Gx3dSetInterpolation(0);
}
/* PIXEL PERFECT's sprites (the 400 x 240 view): a textured quad, every
 * corner at one depth, moving by a fraction of a pixel a frame across is
 * soft that way (NitroGx3dTriangle.soft), in its scene and its in-between
 * scene; by a whole number of pixels a frame, or untextured, it isn't. */
static void textured_quad_at(int x)
{
    NitroPort_GxDirectWrite(0x2a,(3u<<26)|(2u<<20)|(2u<<23));   /* 16 colours, 32 x 32 */
    quad_at(x,(3u<<6)|(31u<<16));
    NitroPort_GxDirectWrite(0x2a,0);
}
static void test_soft_sprites(void)
{
    const NitroGx3dScene *scene,*mid;
    int i;
    reset_geometry();
    NitroPort_Gx3dSetView(400,240);
    NitroPort_Gx3dSetInterpolation(1);
    swap_done();
    textured_quad_at(-3000);
    swap_done();
    /* 89 units a scene: about 2.2 pixels a frame across */
    textured_quad_at(-3000+89);
    quad_at(-3000+89,(3u<<6)|(31u<<16));                        /* (untextured) */
    swap_done();
    scene=NitroPort_Gx3dScene();
    mid=NitroPort_Gx3dMidScene(NULL,NULL);
    CHECK(scene && scene->count==4 && mid && mid->count==4);
    if(scene && scene->count==4 && mid && mid->count==4){
        for(i=0;i<2;i++)CHECK(scene->triangles[i].soft==NITRO_GX3D_SOFT_X && mid->triangles[i].soft==NITRO_GX3D_SOFT_X);
        for(i=2;i<4;i++)CHECK(scene->triangles[i].soft==0 && mid->triangles[i].soft==0);
    }
    /* still: not */
    textured_quad_at(-3000+89);
    quad_at(-3000+89,(3u<<6)|(31u<<16));
    swap_done();
    scene=NitroPort_Gx3dScene();
    CHECK(scene && scene->count==4 && scene->triangles[0].soft==0 && scene->triangles[1].soft==0);
    NitroPort_Gx3dSetInterpolation(0);
    NitroPort_Gx3dSetView(256,192);
    swap_done();
}
/* PIXEL PERFECT's sprites a texel a pixel (port_gx3d_vertex.c,
 * sprite_texel_a_pixel): a person's quad (32 x 32 texels) drawn by the
 * camera at 35.7 pixels and a fraction of a pixel off comes out 32 pixels
 * on pixel corners, its feet where they were; one scaled on purpose, one
 * not facing the camera, one of a room's own in an orthographic camera's
 * scene, or the 1.25x field, as it was. */
static NitroGx3dScene sSpriteScene;
static void sprite_quad(NitroGx3dScene *sc,float px0,float py0,float size,int32_t w2)
{
    /* (corners in screen pixels of a 400 x 240 view; s, t in texels) */
    static const float cs[4]={0,32,32,0}, ct[4]={32,32,0,0};
    static const int tri[2][3]={{0,1,2},{0,2,3}};
    int i,k;
    sc->count=2;
    for(i=0;i<2;i++){
        NitroGx3dTriangle *t=&sc->triangles[i];
        memset(t,0,sizeof(*t));
        t->polygon_attr=(31u<<16);
        t->texture_param=(3u<<26)|(2u<<20)|(2u<<23);   /* 16 colours, 32 x 32 */
        for(k=0;k<3;k++){
            int c=tri[i][k];
            NitroGx3dVertex *v=&t->v[k];
            float x=px0+cs[c]/32.0f*size, y=py0+ct[c]/32.0f*size-size;
            v->x=(int32_t)(x/(400.0f/256.0f)*256.0f);
            v->y=(int32_t)(y/1.25f*256.0f);
            v->clip[3]=w2;
            v->s=cs[c]; v->t=ct[c];
        }
    }
}
static void sprite_corners(NitroGx3dScene *sc,float *minx,float *maxx,float *miny,float *maxy)
{
    static NitroGx3dGpuVertex verts[64];
    static NitroGx3dBatch batches[8];
    size_t nb,n,i;
    unsigned lost;
    n=NitroPort_Gx3dBuildBatches(sc,verts,64,batches,8,&nb,&lost);
    *minx=*miny=1e9f;*maxx=*maxy=-1e9f;
    for(i=0;i<n;i++){
        float x=(verts[i].position[0]/verts[i].position[3]+1.0f)*200.0f;   /* (400 pixels across) */
        float y=(1.0f-verts[i].position[1]/verts[i].position[3])*0.5f*240.0f;
        if(x<*minx)*minx=x;
        if(x>*maxx)*maxx=x;
        if(y<*miny)*miny=y;
        if(y>*maxy)*maxy=y;
    }
}
static void test_sprite_texel_a_pixel(void)
{
    NitroGx3dScene *sc=&sSpriteScene;
    float x0,x1,y0,y1;
    memset(sc,0,sizeof(*sc));
    sc->screen_width=400;sc->screen_height=240;
    /* feet at (180.3 + 35.7 / 2, 150.4) */
    sprite_quad(sc,180.3f,150.4f,35.7f,2481011);
    sprite_corners(sc,&x0,&x1,&y0,&y1);
    CHECK(fabsf((x1-x0)-32.0f)<0.01f && fabsf((y1-y0)-32.0f)<0.01f);
    CHECK(fabsf(x0-floorf(x0+0.5f))<0.01f && fabsf(y0-floorf(y0+0.5f))<0.01f);
    CHECK(fabsf((x0+x1)*0.5f-(180.3f+35.7f*0.5f))<=0.5f);
    CHECK(fabsf(y1-150.4f)<=0.5f);
    /* growing out of its ball: half its size, as it was (its edges moved
     * out a little, as every polygon's) */
    sprite_quad(sc,180.3f,150.4f,16.0f,2481011);
    sprite_corners(sc,&x0,&x1,&y0,&y1);
    CHECK(fabsf((x1-x0)-16.0f)<1.0f);
    /* not facing the camera (its corners' depths differ): as it was */
    sprite_quad(sc,180.3f,150.4f,35.7f,2481011);
    sc->triangles[0].v[2].clip[3]=2481111;
    sprite_corners(sc,&x0,&x1,&y0,&y1);
    CHECK(fabsf((x1-x0)-35.7f)<0.5f);
    /* an orthographic camera's scene (the interiors': the room's own
     * polygons all at one depth too): an unlit one (the room's) as it
     * was; a lit one, its whole texture (a person), a texel a pixel */
    sprite_quad(sc,180.3f,150.4f,35.7f,4096);
    sc->projection[15]=4096;
    sprite_corners(sc,&x0,&x1,&y0,&y1);
    CHECK(fabsf((x1-x0)-35.7f)<1.0f && fabsf(x0-180.3f)<0.5f);   /* (its edges moved out a little) */
    sprite_quad(sc,180.3f,150.4f,35.7f,4096);
    sc->triangles[0].polygon_attr|=1u;sc->triangles[1].polygon_attr|=1u;
    sprite_corners(sc,&x0,&x1,&y0,&y1);
    CHECK(fabsf((x1-x0)-32.0f)<0.01f && fabsf(x0-floorf(x0+0.5f))<0.01f);
    sprite_quad(sc,180.3f,150.4f,35.7f,4096);   /* (lit, but a part of its texture) */
    sc->triangles[0].polygon_attr|=1u;sc->triangles[1].polygon_attr|=1u;
    sc->triangles[0].v[0].s=sc->triangles[1].v[0].s=4.0f;
    sprite_corners(sc,&x0,&x1,&y0,&y1);
    CHECK(fabsf(x0-floorf(x0+0.5f))>0.05f || fabsf((x1-x0)-32.0f)>0.5f);
    sc->projection[15]=0;
    /* moving by a fraction of a pixel a frame across (soft, the in-between
     * scenes' finding): on half a pixel across (a 64th on), still whole
     * down; and the other way round */
    sprite_quad(sc,180.55f,150.4f,35.7f,2481011);   /* (feet at 198.4: on 198.5) */
    sc->triangles[0].soft=sc->triangles[1].soft=NITRO_GX3D_SOFT_X;
    sprite_corners(sc,&x0,&x1,&y0,&y1);
    CHECK(fabsf((x1-x0)-32.0f)<0.01f && fabsf((y1-y0)-32.0f)<0.01f);
    CHECK(fabsf(x0-1.0f/64.0f-floorf(x0*2.0f+0.5f)*0.5f)<0.01f && fabsf(x0-floorf(x0+0.5f))>0.3f);
    CHECK(fabsf(y0-floorf(y0+0.5f))<0.01f);
    sprite_quad(sc,180.3f,150.4f,35.7f,2481011);
    sc->triangles[0].soft=sc->triangles[1].soft=NITRO_GX3D_SOFT_Y;
    sprite_corners(sc,&x0,&x1,&y0,&y1);
    CHECK(fabsf(x0-floorf(x0+0.5f))<0.01f);
    CHECK(fabsf(y0-1.0f/64.0f-floorf(y0*2.0f+0.5f)*0.5f)<0.01f);
    /* the 1.25x field (192 lines): as it was */
    sprite_quad(sc,180.3f,150.4f,35.7f,2481011);
    sc->screen_height=192;
    sprite_corners(sc,&x0,&x1,&y0,&y1);
    CHECK(fabsf((x1-x0)-32.0f)>1.0f);
}

static void test_shadow_triangles(void)
{
    const NitroGx3dScene *scene;
    int i,whole;
    NitroPort_SetSunShadows(1);
    reset_geometry();
    swap();
    NitroPort_Gx3dScene();                             /* (done, and read) */
    NitroPort_GxDirectWrite(0x29,(1u<<6)|(31u<<16));   /* (only the back drawn) */
    flat_triangle(-3000,3000,-3000,3000);
    NitroPort_GxDirectWrite(0x29,(3u<<6)|(31u<<16));
    flat_triangle(5000,7000,-1000,1000);               /* x 1.2-1.7: off the right */
    flat_triangle(18500,20500,-1000,1000);             /* x 4.5-5.0: beyond reach */
    flat_triangle(2000,6000,-1000,1000);               /* crossing the right side */
    NitroPort_GxDirectWrite(0x29,(1u<<6)|(15u<<16));   /* translucent, facing away */
    flat_triangle(-3000,3000,-3000,3000);
    NitroPort_GxDirectWrite(0x29,(0u<<6)|(31u<<16));   /* drawn on neither side */
    flat_triangle(-3000,3000,-3000,3000);
    swap();
    scene=NitroPort_Gx3dScene();
    CHECK(scene->count>=1);                            /* the crossing one's piece in view */
    for(i=0;i<(int)scene->count;i++)CHECK(scene->triangles[i].v[0].clip[0]<=4096 ||
                                           scene->triangles[i].v[1].clip[0]<=4096);
    CHECK(scene->shadow_count==3);
    whole=0;
    for(i=0;i<(int)scene->shadow_count;i++){
        const NitroGx3dTriangle *tri=&scene->shadow_triangles[i];
        if(tri->v[0].clip[0]==2000 && tri->v[1].clip[0]==6000)whole=1;
        CHECK(((tri->polygon_attr>>16)&31u)==31u);
        CHECK(tri->v[0].clip[3]==4096);
    }
    CHECK(whole);
    CHECK(scene->shadow_triangles[1].v[0].x==(int)(((5000+4096)*256.0/(2*4096))*256+0.5));
    /* box tests: just off the right side passes, far off doesn't */
    NitroPort_GxDirectWrite(0x70,word2(6000,0));
    NitroPort_GxDirectWrite(0x70,word2(0,ONE/8));
    NitroPort_GxDirectWrite(0x70,word2(ONE/8,ONE/8));
    NitroPort_Gx3dRefreshStatus();
    CHECK((read32(0x600)&2)!=0);
    NitroPort_GxDirectWrite(0x70,word2(5*ONE,0));
    NitroPort_GxDirectWrite(0x70,word2(0,ONE/8));
    NitroPort_GxDirectWrite(0x70,word2(ONE/8,ONE/8));
    NitroPort_Gx3dRefreshStatus();
    CHECK((read32(0x600)&2)==0);
    /* off: none kept, and a box just off the side doesn't pass */
    NitroPort_SetSunShadows(0);
    swap();
    NitroPort_Gx3dScene();                             /* (done, and read) */
    NitroPort_GxDirectWrite(0x29,(1u<<6)|(31u<<16));
    flat_triangle(-3000,3000,-3000,3000);
    swap();
    scene=NitroPort_Gx3dScene();
    CHECK(scene->count==0);
    CHECK(scene->shadow_count==0);
    NitroPort_GxDirectWrite(0x70,word2(6000,0));
    NitroPort_GxDirectWrite(0x70,word2(0,ONE/8));
    NitroPort_GxDirectWrite(0x70,word2(ONE/8,ONE/8));
    NitroPort_Gx3dRefreshStatus();
    CHECK((read32(0x600)&2)==0);
}
/* A wider view (NitroPort_Gx3dSetWideView): x scaled by 256/320, so a
 * vertex at x = 1.2 (out of view) is in view at 0.96, and one at 0.5 is
 * drawn at 0.4 (the display stretches it back to where it was); the clip
 * matrix the game reads back isn't scaled; box tests pass what the wider
 * view shows; a narrower viewport keeps the game's view. */
static void test_wide_view(void)
{
    const NitroGx3dScene *scene;
    int32_t xs[3];
    unsigned i,k;
    reset_geometry();
    NitroPort_Gx3dSetWideView(320);
    NitroPort_GxDirectWrite(0x40,0);
    vertex(2048,-3000,0);vertex(4915,-3000,0);vertex(0,3000,0);  /* x 0.5, 1.2, 0 */
    NitroPort_GxDirectWrite(0x41,0);
    swap();
    scene=NitroPort_Gx3dScene();
    CHECK(scene->count==1);   /* nothing clipped away */
    for(i=0;i<3;i++)xs[i]=-1;
    if(scene->count==1)for(k=0;k<3;k++)xs[k]=scene->triangles[0].v[k].x;
    /* screen x (1/256 pixel) of x = 0.4, 0.96, 0 */
    /* (within 1/64 pixel: the scale is 4.12 fixed point) */
    CHECK(abs(xs[0]-(int32_t)((0.4+1)/2*256*256+0.5))<=4);
    CHECK(abs(xs[1]-(int32_t)((0.96+1)/2*256*256+0.5))<=4);
    CHECK(xs[2]==128*256);
    CHECK(clip_result(0)==ONE);   /* the game's clip matrix: still identity */
    /* A box just right of the DS's view, within the wider one. */
    NitroPort_GxDirectWrite(0x70,word2(4300,0));
    NitroPort_GxDirectWrite(0x70,word2(0,ONE/8));
    NitroPort_GxDirectWrite(0x70,word2(ONE/8,ONE/8));
    NitroPort_Gx3dRefreshStatus();
    CHECK((read32(0x600)&2)!=0);
    NitroPort_GxDirectWrite(0x70,word2(5200,0));   /* beyond the wider view too */
    NitroPort_GxDirectWrite(0x70,word2(0,ONE/8));
    NitroPort_GxDirectWrite(0x70,word2(ONE/8,ONE/8));
    NitroPort_Gx3dRefreshStatus();
    CHECK((read32(0x600)&2)==0);
    /* A viewport of half the width: the game's own view. */
    NitroPort_GxDirectWrite(0x60,0xbf7f0000u);   /* 0,0 to 127,191 */
    NitroPort_GxDirectWrite(0x40,0);
    vertex(2048,-3000,0);vertex(4915,-3000,0);vertex(0,3000,0);
    NitroPort_GxDirectWrite(0x41,0);
    swap();
    scene=NitroPort_Gx3dScene();
    CHECK(scene->count>=1);
    if(scene->count>=1){
        int found=0;
        for(k=0;k<3;k++)found|=scene->triangles[0].v[k].x==(int32_t)((0.5+1)/2*128*256+0.5);
        CHECK(found);   /* x = 0.5 drawn at 0.5 of the viewport */
    }
    NitroPort_Gx3dSetWideView(256);
    reset_geometry();
}
static void test_bg0_composition(void)
{
    /* The original GX display setup chooses the 3D engine for BG0.
     * The existing 2D compositor, not a bespoke 3D overlay, must show it. */
    /* The compositor operates on virtual DS registers. Set a minimal
     * known-good synthetic display state (engine A on, graphics display,
     * BG0 visible, and its source set to 3D). Dedicated 2D tests exercise
     * GX_Init / GX_SetGraphicsMode separately. */
    {
        const uint32_t disp = (1u << 16) | (1u << 8) | (1u << 3);
        memset(NitroPort_IoReg, 0, 0x80);
        memcpy(NitroPort_IoReg, &disp, sizeof(disp));
        NitroPort_IoReg[0x304] |= 2u;
    }
    reset_geometry();
    triangle(0,RGB(0,0,31));
    swap();
    NitroPort_Render2D(0,pixels);
    CHECK((pixels[96*256+128]&0x7fff)==RGB(0,0,31));
    CHECK((pixels[96*256+128]&0x8000)==0); /* final 2D RGB555 */
}
static void test_clear_and_status(void)
{
    reset_geometry();
    NitroPort_IoReg[0x350]=31;
    NitroPort_IoReg[0x352]=31; /* clear alpha 31 */
    NitroPort_IoReg[0x354]=0xff;
    NitroPort_IoReg[0x355]=0x7f;
    swap();
    NitroPort_Gx3dRasterize(pixels,depths);
    CHECK(pixels[5*256+5]==(0x8000|31));
    CHECK(depths[0]==0x7fff*0x200u);
    memset(NitroPort_IoReg+0x350,0,6);
}

/* This is the ORIGINAL NitroSystem gecom.c function, compiled into this test
 * from the freshly bootstrapped (and port-patched) upstream source. Its
 * implementation does a raw reg_G3X_GXFIFO = op before MI_CpuSend32. */
extern void NNS_G3dGeBufferOP_N(u32 op, const u32 *args, u32 count);
extern void NNS_G3dGeSendDL(const void *data, u32 bytes);

static void test_cpu_fifo_writes(void)
{
    /* The two packed words form COLOR, BEGIN, three VTX_16s, END, SWAP.
     * No decoder entrypoint is called by this test. */
    static const u32 words[] = {
        0x23234020u, RGB(31,0,0), 0,
        0, 0, 0, 0, /* filled with vertices immediately below */
        0x00504123u, 0, 0, 0
    };
    u32 stream[11];
    const NitroGx3dScene *scene;
    memcpy(stream,words,sizeof(stream));
    stream[3]=word2(-3000,-3000);stream[4]=0;
    stream[5]=word2(3000,-3000);stream[6]=0;
    stream[8]=word2(0,3000);stream[9]=0;stream[10]=0;

    reset_geometry();
    MI_CpuSend32(stream,(volatile void *)&reg_G3X_GXFIFO,sizeof(stream));
    scene=NitroPort_Gx3dScene();
    CHECK(scene->count==1);
    if(scene->count)CHECK(scene->triangles[0].v[0].clip[0]==-3000);

    reset_geometry();
    /* Covers the direct store in NitroSystem's NNS_G3dGeBufferOP_N. */
    NNS_G3dGeBufferOP_N(stream[0],stream+1,6);
    NNS_G3dGeBufferOP_N(stream[7],stream+8,3);
    scene=NitroPort_Gx3dScene();
    CHECK(scene->count==1);
    if(scene->count)CHECK(scene->triangles[0].v[0].clip[0]==-3000);

    reset_geometry();
    /* NNS_G3dGeSendDL chooses CPU-send for short display lists. */
    NNS_G3dGeSendDL(stream,sizeof(stream));
    CHECK(NitroPort_Gx3dScene()->count==1);

    reset_geometry();
    {
        u32 delta[3]={ONE,0,0};
        MI_CpuSend32(delta,(volatile void *)&reg_G3_MTX_TRANS,sizeof(delta));
        CHECK(clip_result(12)==ONE);
    }
}

static void emit_normal_triangle(uint32_t normal)
{
    NitroPort_GxDirectWrite(0x40,0);
    NitroPort_GxDirectWrite(0x21,normal);
    vertex(-3000,-3000,0);
    vertex(3000,-3000,0);
    vertex(0,3000,0);
    NitroPort_GxDirectWrite(0x41,0);
    swap();
}

static void test_light_material_and_vectors(void)
{
    const NitroGx3dScene *scene;
    const u32 forwardNormal=(511u<<20);
    const u32 rearLight=(512u<<20); /* signed 10-bit -512 */
    const u32 enabled=(1u)|(3u<<6)|(31u<<16);
    unsigned i;
    reset_geometry();
    /* Ambient is only added for ENABLED lights, and is modulated by their
     * light colour. No enabled lights means emission alone. */
    NitroPort_GxDirectWrite(0x30,(u32)RGB(31,0,0)<<16);
    NitroPort_GxDirectWrite(0x31,(u32)RGB(0,0,5)<<16);
    emit_normal_triangle(forwardNormal);
    scene=NitroPort_Gx3dScene();
    CHECK(scene->count==1);
    if(scene->count)CHECK(scene->triangles[0].v[0].color==RGB(0,0,5));

    reset_geometry();
    NitroPort_GxDirectWrite(0x29,enabled);
    NitroPort_GxDirectWrite(0x30,(u32)RGB(31,0,0)<<16);
    NitroPort_GxDirectWrite(0x31,(u32)RGB(0,0,5)<<16);
    NitroPort_GxDirectWrite(0x33,RGB(0,0,31)); /* blue light, red ambient */
    emit_normal_triangle(forwardNormal);
    scene=NitroPort_Gx3dScene();
    CHECK(scene->count==1);
    if(scene->count)CHECK(scene->triangles[0].v[0].color==RGB(0,0,5));

    /* Transform a +Z light through the *current* directional matrix,
     * then reset it before transforming the +Z normal. The result is
     * illuminated ONLY if LIGHT_VECTOR stored the rotated -Z direction. */
    reset_geometry();
    NitroPort_GxDirectWrite(0x29,enabled);
    NitroPort_GxDirectWrite(0x30,RGB(31,0,0));
    NitroPort_GxDirectWrite(0x33,RGB(31,31,31));
    NitroPort_GxDirectWrite(0x10,2);
    {
        static const int32_t flip[9]={
            ONE,0,0, 0,ONE,0, 0,0,-ONE
        };
        for(i=0;i<9;i++)NitroPort_GxDirectWrite(0x1a,(u32)flip[i]);
    }
    NitroPort_GxDirectWrite(0x32,511u<<20); /* input +Z, stored -Z */
    NitroPort_GxDirectWrite(0x15,0); /* back to identity for normal */
    emit_normal_triangle(forwardNormal);
    scene=NitroPort_Gx3dScene();
    CHECK(scene->count==1);
    if(scene->count)CHECK((scene->triangles[0].v[0].color&31u)>20u);

    /* A zero shininess table blocks specular, a full table restores it.
     * Diffuse and ambient are zero, so only the specular path may be red. */
    for(i=0;i<2;i++){
        unsigned j;
        reset_geometry();
        NitroPort_GxDirectWrite(0x29,enabled);
        NitroPort_GxDirectWrite(0x30,0);
        NitroPort_GxDirectWrite(0x31,0x8000u|RGB(31,0,0));
        NitroPort_GxDirectWrite(0x32,rearLight);
        NitroPort_GxDirectWrite(0x33,RGB(31,31,31));
        for(j=0;j<32;j++)NitroPort_GxDirectWrite(0x34,i?0xffffffffu:0u);
        emit_normal_triangle(forwardNormal);
        scene=NitroPort_Gx3dScene();
        CHECK(scene->count==1);
        if(scene->count){
            uint16_t red=scene->triangles[0].v[0].color&31u;
            CHECK(i?red>20u:red==0u);
        }
    }
}

static void test_texture_matrix_modes(void)
{
    const NitroGx3dScene *scene;
    reset_geometry();
    NitroPort_GxDirectWrite(0x10,3); /* texture matrix */
    NitroPort_GxDirectWrite(0x15,0);
    NitroPort_GxDirectWrite(0x1b,2*ONE);
    NitroPort_GxDirectWrite(0x1b,3*ONE);
    NitroPort_GxDirectWrite(0x1b,ONE);
    NitroPort_GxDirectWrite(0x2a,1u<<30); /* TEXCOORD source */
    G3_TexCoord(ONE,2*ONE);
    triangle(0,RGB(31,31,31));swap();
    scene=NitroPort_Gx3dScene();
    CHECK(scene->count==1);
    if(scene->count){
        CHECK(scene->triangles[0].v[0].s==2.0f);
        CHECK(scene->triangles[0].v[0].t==6.0f);
    }

    reset_geometry();
    NitroPort_GxDirectWrite(0x2a,2u<<30); /* NORMAL source */
    G3_TexCoord(0,0);
    emit_normal_triangle(511u);
    scene=NitroPort_Gx3dScene();
    CHECK(scene->count==1);
    if(scene->count)CHECK(scene->triangles[0].v[0].s>=0.9f &&
                          scene->triangles[0].v[0].s<=1.0f);

    reset_geometry();
    NitroPort_GxDirectWrite(0x2a,3u<<30); /* VERTEX source */
    G3_TexCoord(0,0);
    NitroPort_GxDirectWrite(0x40,0);
    vertex(ONE,0,0);
    vertex(0,ONE,0);
    vertex(-ONE,0,0);
    NitroPort_GxDirectWrite(0x41,0);
    swap();
    scene=NitroPort_Gx3dScene();
    CHECK(scene->count==1);
    if(scene->count)CHECK(scene->triangles[0].v[0].s==1.0f);
}

static void test_latched_polygon_attributes(void)
{
    const NitroGx3dScene *scene;
    const u32 opaque=(3u<<6)|(31u<<16), translucent=(3u<<6)|(16u<<16);
    reset_geometry();
    NitroPort_GxDirectWrite(0x29,opaque);
    NitroPort_GxDirectWrite(0x40,0);
    NitroPort_GxDirectWrite(0x29,translucent); /* applies at NEXT begin */
    vertex(-3000,-3000,0);vertex(3000,-3000,0);vertex(0,3000,0);
    NitroPort_GxDirectWrite(0x41,0);
    NitroPort_GxDirectWrite(0x40,0);
    vertex(-3000,-3000,500);vertex(3000,-3000,500);vertex(0,3000,500);
    NitroPort_GxDirectWrite(0x41,0);
    swap();
    scene=NitroPort_Gx3dScene();
    CHECK(scene->count==2);
    if(scene->count==2){
        CHECK(scene->triangles[0].polygon_attr==opaque);
        CHECK(scene->triangles[1].polygon_attr==translucent);
    }
}

static void test_viewport_and_immediate_texture_state(void)
{
    const NitroGx3dScene *scene;
    reset_geometry();
    triangle(0,RGB(31,31,31));
    swap();
    scene=NitroPort_Gx3dScene();
    CHECK(scene->count==1);
    /* Viewport X=0..255 has width 256 (inclusive endpoint). */
    if(scene->count)CHECK(scene->triangles[0].v[2].x==128*256);

    reset_geometry();
    NitroPort_GxDirectWrite(0x40,0);
    /* Unlike POLYGON_ATTR, texture parameters apply during BEGIN. */
    NitroPort_GxDirectWrite(0x2a,(4u<<26));
    NitroPort_GxDirectWrite(0x2b,42u);
    vertex(-3000,-3000,0);vertex(3000,-3000,0);vertex(0,3000,0);
    NitroPort_GxDirectWrite(0x41,0);
    swap();
    scene=NitroPort_Gx3dScene();
    CHECK(scene->count==1);
    if(scene->count){
        CHECK(scene->triangles[0].texture_param==(4u<<26));
        CHECK(scene->triangles[0].palette_base==42u);
    }
}

static void test_stack_vec_and_result_refresh(void)
{
    unsigned i;
    uint32_t marker=0x12345678u;
    reset_geometry();
    for(i=0;i<31;i++)NitroPort_GxDirectWrite(0x11,0);
    CHECK(((uint32_t)read32(0x600)&0x8000u)==0);
    CHECK((((uint32_t)read32(0x600)>>8)&31u)==31u);
    NitroPort_GxDirectWrite(0x12,1u); /* pop 31 -> 30, stored slot 30 */
    CHECK((((uint32_t)read32(0x600)>>8)&31u)==30u);
    CHECK(((uint32_t)read32(0x600)&0x8000u)==0u);
    NitroPort_GxDirectWrite(0x11,0); /* allowed push to 31 again */
    NitroPort_GxDirectWrite(0x11,0); /* overflow at 31 */
    CHECK(((uint32_t)read32(0x600)&0x8000u)!=0);

    reset_geometry();
    NitroPort_GxDirectWrite(0x72,511u); /* X: 511/512 in 4.12 */
    CHECK((uint16_t)read32(0x630)==4088u);
    memcpy(NitroPort_IoReg+0x640,&marker,sizeof(marker));
    for(i=0;i<100;i++)NitroPort_GxFifoWord(0); /* NOP, no matrix changes */
    CHECK((uint32_t)read32(0x640)==marker);
    NitroPort_GxDirectWrite(0x15,0); /* a matrix change: the SDK reads the new one */
    CHECK(clip_result(0)==ONE);
}

static void test_translucency_and_scene_limits(void)
{
    const NitroGx3dScene *scene;
    unsigned i;
    reset_geometry();
    /* The scene below is W-buffered: depth is W, so give W the distance
     * (W = z + 2.0) for "near" and "far" to mean something. */
    {
        static const uint32_t proj[16]={ONE,0,0,0, 0,ONE,0,0, 0,0,ONE,ONE, 0,0,0,2*ONE};
        unsigned k;
        NitroPort_GxDirectWrite(0x10,0);
        for(k=0;k<16;k++)NitroPort_GxDirectWrite(0x16,proj[k]);
        NitroPort_GxDirectWrite(0x10,2);
    }
    NitroPort_IoReg[0x60]|=8u; /* DISP3DCNT alpha blend */
    NitroPort_GxDirectWrite(0x29,(3u<<6)|(16u<<16));
    triangle(-1200,RGB(0,0,31)); /* translucent near object, submitted first */
    NitroPort_GxDirectWrite(0x29,(3u<<6)|(31u<<16));
    triangle(1200,RGB(0,31,0)); /* opaque far object, submitted second */
    NitroPort_GxDirectWrite(0x50,2); /* W-buffer flag lives on scene */
    scene=NitroPort_Gx3dScene();
    CHECK(scene->count==2);
    CHECK(scene->swap_param==2u);
    NitroPort_Gx3dRasterize(pixels,NULL);
    {
        uint16_t col=pixels[96*256+128];
        CHECK(((col>>10)&31u)>0u); /* blended blue source */
        CHECK(((col>>5)&31u)>0u);  /* opaque green behind it */
    }
    NitroPort_IoReg[0x60]&=(uint8_t)~8u;
    NitroPort_Gx3dRasterize(pixels,NULL);
    CHECK(((pixels[96*256+128]>>10)&31u)==31u);
    CHECK(((pixels[96*256+128]>>5)&31u)==0u);

    /* A translucent pixel doesn't write depth (attribute bit 11 clear),
     * even over an opaque one: the DS keeps the larger alpha for the
     * pixel, but that doesn't make the translucent pixel opaque. A second
     * translucent polygon behind the first but in front of the opaque one
     * still shows. (The intro's tunnel walls depend on this.) */
    reset_geometry();
    {
        static const uint32_t proj[16]={ONE,0,0,0, 0,ONE,0,0, 0,0,ONE,ONE, 0,0,0,2*ONE};
        unsigned k;
        NitroPort_GxDirectWrite(0x10,0);
        for(k=0;k<16;k++)NitroPort_GxDirectWrite(0x16,proj[k]);
        NitroPort_GxDirectWrite(0x10,2);
    }
    NitroPort_IoReg[0x60]|=8u;
    NitroPort_GxDirectWrite(0x29,(3u<<6)|(31u<<16));
    triangle(1200,RGB(0,31,0));                      /* opaque, far */
    NitroPort_GxDirectWrite(0x29,(3u<<6)|(16u<<16)|(1u<<24));
    triangle(-1200,RGB(0,0,31));                     /* translucent, near */
    NitroPort_GxDirectWrite(0x29,(3u<<6)|(16u<<16)|(2u<<24));
    triangle(0,RGB(31,0,0));                         /* translucent, between */
    NitroPort_GxDirectWrite(0x50,2);
    NitroPort_Gx3dRasterize(pixels,NULL);
    CHECK((pixels[96*256+128]&31u)>0u);              /* the red one shows */
    NitroPort_IoReg[0x60]&=(uint8_t)~8u;

    /* A translucent pixel isn't drawn over a translucent pixel of the same
     * polygon ID: overlapping parts of one model don't add up. */
    reset_geometry();
    NitroPort_IoReg[0x60]|=8u;
    NitroPort_GxDirectWrite(0x29,(3u<<6)|(16u<<16)|(5u<<24));
    triangle(0,RGB(31,0,0));                         /* translucent red, ID 5 */
    NitroPort_GxDirectWrite(0x29,(3u<<6)|(16u<<16)|(5u<<24));
    triangle(0,RGB(0,0,31));                         /* blue, same ID: not drawn */
    swap();
    NitroPort_Gx3dRasterize(pixels,NULL);
    CHECK((pixels[96*256+128]&31u)>0u);
    CHECK(((pixels[96*256+128]>>10)&31u)==0u);
    reset_geometry();
    NitroPort_GxDirectWrite(0x29,(3u<<6)|(16u<<16)|(5u<<24));
    triangle(0,RGB(31,0,0));
    NitroPort_GxDirectWrite(0x29,(3u<<6)|(16u<<16)|(6u<<24));
    triangle(0,RGB(0,0,31));                         /* another ID: blends */
    swap();
    NitroPort_Gx3dRasterize(pixels,NULL);
    CHECK(((pixels[96*256+128]>>10)&31u)>0u);
    NitroPort_IoReg[0x60]&=(uint8_t)~8u;

    reset_geometry();
    for(i=0;i<2049u;i++)triangle(0,RGB(31,0,0));
    swap();
    scene=NitroPort_Gx3dScene();
    CHECK(scene->count==NITRO_GX3D_MAX_POLYGONS);
    CHECK(scene->dropped>0);

    /* Polygons outside the view take no polygon RAM on the DS: a big map
     * that's mostly off-screen doesn't push out what's drawn after it. */
    reset_geometry();
    NitroPort_GxDirectWrite(0x10,2);
    NitroPort_GxDirectWrite(0x1c,20*ONE); /* far off to the right */
    NitroPort_GxDirectWrite(0x1c,0);
    NitroPort_GxDirectWrite(0x1c,0);
    for(i=0;i<2100u;i++)triangle(0,RGB(31,0,0));
    NitroPort_GxDirectWrite(0x15,0);
    triangle(0,RGB(0,31,0));
    swap();
    scene=NitroPort_Gx3dScene();
    CHECK(scene->count==1);
    CHECK(scene->dropped==0);
}

/* The same tests with the geometry engine on a thread of its own. */
static sem_t sWorkerSem, sProducerSem, sGate;
static volatile int sRunning = 1, sHold;
/* With sHold set, the thread stops when next woken (until sGate). */
static void wait_worker(void *c){(void)c;sem_wait(&sWorkerSem);while(sHold)sem_wait(&sGate);}
static void wake_worker(void *c){(void)c;sem_post(&sWorkerSem);}
static void wait_producer(void *c){(void)c;sem_wait(&sProducerSem);}
static void wake_producer(void *c){(void)c;sem_post(&sProducerSem);}
static void *geometry_thread(void *unused)
{
    (void)unused;
    NitroPort_Gx3dThreadMain(&sRunning);
    return NULL;
}
/* Edge marking (DISP3DCNT bit 5) in the reference rasterizer and in the
 * PC's stand-in for the GPU path: an opaque triangle of polygon ID 8 over
 * an opaque quad of ID 0 that it doesn't cover all of, the rear plane ID
 * 63 (see-through). The triangle's outline in ID 8's colour where the quad
 * is behind it; the quad's where the rear plane is (round its outside). */
static uint16_t gpuPixels[256*192];
static void test_edge_marking(void)
{
    static const uint16_t colors[8]={0x0421,0x0842,0x0c63,0x1084,0x14a5,0x18c6,0x1ce7,0x2108};
    uint16_t *images[2]={pixels,gpuPixels};
    int k,which;
    reset_geometry();
    for(k=0;k<8;k++){NitroPort_IoReg[0x330+k*2]=(uint8_t)colors[k];NitroPort_IoReg[0x331+k*2]=(uint8_t)(colors[k]>>8);}
    NitroPort_IoReg[0x350]=0;NitroPort_IoReg[0x351]=0;NitroPort_IoReg[0x352]=0;
    NitroPort_IoReg[0x353]=63;                                   /* rear plane ID */
    NitroPort_IoReg[0x60]|=0x20u;                                /* edge marking */
    NitroPort_GxDirectWrite(0x29,(3u<<6)|(31u<<16)|(0u<<24));
    quad(1200,RGB(0,31,0));
    NitroPort_GxDirectWrite(0x29,(3u<<6)|(31u<<16)|(8u<<24));
    NitroPort_GxDirectWrite(0x20,RGB(31,0,0));
    NitroPort_GxDirectWrite(0x40,0);
    vertex(-1500,-1500,-1200);vertex(1500,-1500,-1200);vertex(0,1500,-1200);
    NitroPort_GxDirectWrite(0x41,0);
    swap();
    NitroPort_Gx3dRasterize(pixels,NULL);
    NitroPort_Gx3dSimulateGpu(NitroPort_Gx3dScene(),gpuPixels);
    for(which=0;which<2;which++){
        const uint16_t *img=images[which];
        int x,first=-1,quadFirst=-1;
        /* Row 120, crossing both. */
        for(x=0;x<256;x++)if((img[120*256+x]&0x8000)&&quadFirst<0)quadFirst=x;
        for(x=0;x<256;x++)if((img[120*256+x]&0x7fff)==RGB(31,0,0)){first=x;break;}
        CHECK(quadFirst>0 && first>quadFirst);
        if(first>quadFirst && quadFirst>0){
            CHECK((img[120*256+quadFirst]&0x7fff)==colors[0]);   /* quad's outline */
            CHECK((img[120*256+quadFirst+1]&0x7fff)==RGB(0,31,0));
            CHECK((img[120*256+first-1]&0x7fff)==colors[1]);     /* triangle's */
            CHECK((img[120*256+first-2]&0x7fff)==RGB(0,31,0));   /* quad behind: not */
        }
        CHECK((img[96*256+128]&0x7fff)==RGB(31,0,0));            /* inside */
        CHECK((img[0]&0x8000)==0);                               /* rear plane */
    }
    NitroPort_IoReg[0x60]&=(uint8_t)~0x20u;
    NitroPort_IoReg[0x353]=0;
}

/* --- Edge marking with fewer polygons (NitroPort_Gx3dPruneEdges) ---
 * A model of the GPU's outline passes (port_gx3d_pica.c, mark_edges): the
 * vertex program's 3D shift, NitroPort_PicaViewMatrix's matrix, the
 * framebuffer turned a quarter turn, pixel centres, depth and stencil, the
 * scissor test. Each pixel's outlines (how many draws marked it, and which
 * triangles) must come out the same with every triangle drawn in every
 * pass as with the pruned lists. */
#define PM_MAXV 9000
#define PM_MAXB 64
#define PM_W 400
#define PM_H 240
static struct {
    NitroGx3dGpuVertex v[PM_MAXV];
    NitroGx3dBatch b[PM_MAXB];
    uint8_t role[PM_MAXB];
    size_t nv,nb;
} sPm;
static uint16_t sPmIdx[3*PM_MAXV];
static NitroGx3dPruneList sPmLists[PM_MAXB];
static float sPmDepth[PM_W*PM_H];
static uint8_t sPmId[PM_W*PM_H];
static uint16_t sPmCount[2][PM_W*PM_H];
static uint32_t sPmSum[2][PM_W*PM_H];
static void pm_batch(unsigned id,int role)
{
    NitroGx3dBatch *b=&sPm.b[sPm.nb];
    memset(b,0,sizeof(*b));
    b->first=(uint32_t)sPm.nv;
    b->polygon_attr=(id<<24)|(31u<<16);
    sPm.role[sPm.nb++]=(uint8_t)role;
}
/* A triangle at DS screen pixels (x, y), each corner's w, at depth z
 * (0..1), its edges moved out as NitroPort_Gx3dBuildBatches does. */
static void pm_tri(float x0,float y0,float x1,float y1,float x2,float y2,const float w[3],float z)
{
    float x[3]={x0,x1,x2},y[3]={y0,y1,y2},dx[3],dy[3];
    int k;
    NitroPort_Gx3dExpandTriangle(x,y,dx,dy);
    for(k=0;k<3;k++){
        NitroGx3dGpuVertex *v=&sPm.v[sPm.nv++];
        memset(v,0,sizeof(*v));
        v->position[0]=((x[k]+dx[k])/128.0f-1.0f)*w[k];
        v->position[1]=(1.0f-(y[k]+dy[k])/96.0f)*w[k];
        v->position[2]=z*w[k];
        v->position[3]=w[k];
    }
    sPm.b[sPm.nb-1].count+=3;
}
/* The ground's w at a DS row: far at the top. */
static float pm_w(float y){return 4.0f-2.6f*y/192.0f;}
static void pm_quad(float x0,float y0,float x1,float y1,float z,float wq)
{
    float w[3];
    w[0]=w[1]=w[2]=wq>0?wq:0;
    if(wq>0){
        pm_tri(x0,y0,x1,y0,x1,y1,w,z);
        pm_tri(x0,y0,x1,y1,x0,y1,w,z);
    }else{
        float wa[3]={pm_w(y0),pm_w(y0),pm_w(y1)},wb[3]={pm_w(y0),pm_w(y1),pm_w(y1)};
        pm_tri(x0,y0,x1,y0,x1,y1,wa,z);
        pm_tri(x0,y0,x1,y1,x0,y1,wb,z);
    }
}
static uint32_t pm_rand(uint32_t *s){*s=*s*1664525u+1013904223u;return *s>>8;}
static float pm_frand(uint32_t *s,float lo,float hi){return lo+(hi-lo)*(float)(pm_rand(s)&0xffff)/65535.0f;}
/* A town (kind 0): the ground (ID 0) in tiles over the whole DS screen, a
 * tile left out (the rear plane through it), houses (ID 8), a tree with
 * see-through texels (ID 16), people (ID 2), a sign of opaque texels on a
 * translucent polygon (ID 32), smoke (translucent: no outlines). A route
 * (1): the ground, a few trees and people. 2 and 3: the town, and random
 * triangles of IDs 0 and 8 all over it. */
static void pm_scene(int kind,uint32_t seed)
{
    int tx,ty,i,extra=kind>=2?150:0;
    sPm.nv=sPm.nb=0;
    pm_batch(0,NITRO_GX3D_PRUNE_SOLID);
    for(ty=0;ty<12;ty++)for(tx=0;tx<16;tx++){
        if(kind!=1&&tx==11&&ty==7)continue;
        pm_quad(tx*16.0f,ty*16.0f,tx*16.0f+16,ty*16.0f+16,0.8f-ty*0.02f,0);
    }
    if(kind==1){
        pm_batch(16,NITRO_GX3D_PRUNE_SHAPE);
        pm_quad(40,30,60,60,0.5f,3.2f);
        pm_quad(180,90,200,120,0.4f,2.2f);
        pm_batch(2,NITRO_GX3D_PRUNE_SOLID);
        pm_quad(120,100,130,118,0.3f,2.0f);
        pm_quad(124,150,134,168,0.3f,1.4f);
        return;
    }
    pm_batch(8,NITRO_GX3D_PRUNE_SOLID);
    pm_quad(30,40,90,100,0.5f,2.5f);
    pm_quad(150,20,230,70,0.55f,3.0f);
    pm_quad(-0.0f,120,40,180,0.45f,1.6f);   /* at the left edge */
    pm_batch(16,NITRO_GX3D_PRUNE_SHAPE);
    pm_quad(100,110,130,150,0.4f,1.8f);
    pm_batch(2,NITRO_GX3D_PRUNE_SOLID);
    pm_quad(60,130,70,150,0.3f,1.5f);
    pm_quad(250,60,256,75,0.3f,2.2f);       /* at the right edge */
    pm_batch(32,NITRO_GX3D_PRUNE_SHAPE);
    pm_quad(200,150,220,165,0.35f,1.4f);
    pm_batch(40,NITRO_GX3D_PRUNE_OUT);
    pm_quad(180,100,200,120,0.2f,1.5f);
    if(extra){
        pm_batch(0,NITRO_GX3D_PRUNE_SOLID);
        for(i=0;i<extra;i++){
            float x=pm_frand(&seed,0,240),y=pm_frand(&seed,0,176),s=pm_frand(&seed,2,24);
            float w[3]={pm_frand(&seed,1.2f,4),pm_frand(&seed,1.2f,4),pm_frand(&seed,1.2f,4)};
            if(i==extra/2)pm_batch(8,NITRO_GX3D_PRUNE_SOLID);
            pm_tri(x,y,x+s,y+pm_frand(&seed,0,s),x+pm_frand(&seed,0,s),y+s,w,pm_frand(&seed,0.1f,0.9f));
        }
    }
}
/* Where a vertex lands on the render target (pixels), as the GPU puts it:
 * the vertex program's shift, then NitroPort_PicaViewMatrix's rows. */
static int sPmScreenW;
static unsigned sPmMarked;
static void pm_target(const NitroGx3dGpuVertex *v,const float rect[4],float stereo,float conv,
                      float *tx,float *ty,float *depth)
{
    float x=v->position[0],y=v->position[1],w=v->position[3],W=(float)sPmScreenW,H=(float)PM_H,ox,oy;
    if(1.0001f<w){float s=w-conv;if(s<-w)s=-w;x+=stereo*s;}
    ox=((rect[3]-rect[1])/H)*y+(1.0f-(rect[1]+rect[3])/H)*w;
    oy=(-(rect[2]-rect[0])/W)*x+(1.0f-(rect[0]+rect[2])/W)*w;
    *tx=(ox/w+1.0f)*0.5f*H;
    *ty=(oy/w+1.0f)*0.5f*W;
    *depth=v->position[2]/w;
}
static uint32_t pm_mix(uint32_t t){t=(t+1u)*2654435761u;return t^(t>>15);}
/* A triangle drawn in a pass: A (into the depth/ID buffer) or B (marking
 * outlines), its pixels within the scissor (target pixels; NULL: off). */
static void pm_draw(int pass,size_t first,const float rect[4],int dx,int dy,float stereo,float conv,
                    const unsigned *scissor,int slot)
{
    const NitroGx3dGpuVertex *v=&sPm.v[first];
    int H=PM_H,W=sPmScreenW,k,i,j,i0,i1,j0,j1;
    float tx[3],ty[3],dz[3],area;
    size_t b;
    unsigned id=0;
    int role=0;
    for(b=0;b<sPm.nb;b++)if(first>=sPm.b[b].first&&first<sPm.b[b].first+sPm.b[b].count){
        id=(sPm.b[b].polygon_attr>>24)&63u;role=sPm.role[b];
    }
    for(k=0;k<3;k++){
        float r[4]={rect[0]+dx,rect[1]+dy,rect[2]+dx,rect[3]+dy};
        pm_target(&v[k],r,stereo,conv,&tx[k],&ty[k],&dz[k]);
    }
    area=(tx[1]-tx[0])*(ty[2]-ty[0])-(tx[2]-tx[0])*(ty[1]-ty[0]);
    if(area==0)return;
    i0=(int)floorf(fminf(tx[0],fminf(tx[1],tx[2])));i1=(int)ceilf(fmaxf(tx[0],fmaxf(tx[1],tx[2])));
    j0=(int)floorf(fminf(ty[0],fminf(ty[1],ty[2])));j1=(int)ceilf(fmaxf(ty[0],fmaxf(ty[1],ty[2])));
    if(i0<0)i0=0;
    if(j0<0)j0=0;
    if(i1>H-1)i1=H-1;
    if(j1>W-1)j1=W-1;
    for(j=j0;j<=j1;j++)for(i=i0;i<=i1;i++){
        float cx=i+0.5f,cy=j+0.5f,l0,l1,l2,z;
        int px,py,p;
        if(scissor&&(i<(int)scissor[0]||i>=(int)scissor[2]||j<(int)scissor[1]||j>=(int)scissor[3]))continue;
        l0=((tx[1]-cx)*(ty[2]-cy)-(tx[2]-cx)*(ty[1]-cy))/area;
        l1=((tx[2]-cx)*(ty[0]-cy)-(tx[0]-cx)*(ty[2]-cy))/area;
        l2=1.0f-l0-l1;
        if(l0<0||l1<0||l2<0)continue;
        px=W-1-j;py=H-1-i;p=py*W+px;
        if(role==NITRO_GX3D_PRUNE_SHAPE){
            /* see-through texels, moving with the polygon */
            uint32_t h=pm_mix((uint32_t)first*7919u^(uint32_t)(px-dx)*104729u^(uint32_t)(py-dy)*1299709u);
            if((h&3u)==0)continue;
        }
        z=l0*dz[0]+l1*dz[1]+l2*dz[2];
        if(pass==0){
            if(z<sPmDepth[p]){sPmDepth[p]=z;sPmId[p]=(uint8_t)id;}
        }else if(z<sPmDepth[p]&&id!=sPmId[p]){
            sPmCount[slot][p]++;
            sPmSum[slot][p]+=pm_mix((uint32_t)first);
        }
    }
}
/* The four directions' passes for one eye, everything or pruned. */
static void pm_passes(const float rect[4],float stereo,float conv,unsigned clearId,const NitroGx3dPruneResult *res,
                      int slot)
{
    static const int dx[4]={-1,1,0,0},dy[4]={0,0,-1,1};
    int dir,z,pass;
    size_t b,t;
    for(dir=0;dir<4;dir++){
        for(t=0;t<(size_t)PM_W*PM_H;t++){sPmDepth[t]=1e30f;sPmId[t]=(uint8_t)clearId;}
        for(pass=0;pass<2;pass++){
            int sx=pass?0:dx[dir],sy=pass?0:dy[dir];
            for(b=0;b<sPm.nb;b++){
                if(sPm.role[b]==NITRO_GX3D_PRUNE_OUT)continue;
                if(!res){
                    for(t=0;t<sPm.b[b].count;t+=3)pm_draw(pass,sPm.b[b].first+t,rect,sx,sy,stereo,conv,NULL,slot);
                    continue;
                }
                {
                    const NitroGx3dPruneList *l=&sPmLists[b];
                    int whole=pass?NITRO_GX3D_PRUNE_D:NITRO_GX3D_PRUNE_A;
                    for(t=0;t<l->count[whole];t+=3)pm_draw(pass,sPmIdx[l->offset[whole]+t],rect,sx,sy,stereo,conv,NULL,slot);
                    for(z=0;z<4;z++){
                        int list=(pass?NITRO_GX3D_PRUNE_ZONE_B:NITRO_GX3D_PRUNE_ZONE_A)+z;
                        unsigned sc[4];
                        NitroPort_Gx3dPruneScissor(res->zone[z],sPmScreenW,PM_H,sc);
                        for(t=0;t<l->count[list];t+=3)pm_draw(pass,sPmIdx[l->offset[list]+t],rect,sx,sy,stereo,conv,sc,slot);
                    }
                }
            }
        }
    }
}
static int pm_compare(const float rect[4],int eyes,float stereo,float conv,unsigned clearId,int screenW,
                      NitroGx3dPruneResult *out)
{
    NitroGx3dPruneView view;
    NitroGx3dPruneResult res;
    size_t p,b;
    int eye,diff=0,ok,indices;
    float st[2]={eyes==2?-stereo:0.0f,stereo};
    sPmScreenW=screenW;
    view.x0=rect[0];view.y0=rect[1];view.x1=rect[2];view.y1=rect[3];
    view.width=screenW;view.height=PM_H;view.eyes=eyes;
    view.stereo[0]=st[0];view.stereo[1]=st[1];view.convergence=conv;view.clear_id=clearId;
    ok=NitroPort_Gx3dPruneEdges(sPm.v,sPm.b,sPm.nb,sPm.role,&view,sPmIdx,sizeof(sPmIdx)/sizeof(sPmIdx[0]),
                                sPmLists,&res);
    CHECK(ok);
    if(!ok)return -1;
    /* (the lists: whole triangles, in their batch, 8-aligned) */
    indices=1;
    for(b=0;b<sPm.nb;b++){
        int l;
        for(l=0;l<NITRO_GX3D_PRUNE_LISTS;l++){
            unsigned t;
            if(sPmLists[b].offset[l]%8u)indices=0;
            for(t=0;t<sPmLists[b].count[l];t++){
                uint16_t ix=sPmIdx[sPmLists[b].offset[l]+t];
                if(ix<sPm.b[b].first||ix>=sPm.b[b].first+sPm.b[b].count||(t%3u)!=(unsigned)((ix-sPm.b[b].first)%3u))indices=0;
            }
        }
    }
    CHECK(indices);
    for(eye=0;eye<eyes;eye++){
        memset(sPmCount,0,sizeof(sPmCount));
        memset(sPmSum,0,sizeof(sPmSum));
        pm_passes(rect,eyes==2?st[eye]:0.0f,conv,clearId,NULL,0);
        pm_passes(rect,eyes==2?st[eye]:0.0f,conv,clearId,&res,1);
        for(p=0;p<(size_t)screenW*PM_H;p++){
            if(sPmCount[0][p]!=sPmCount[1][p]||sPmSum[0][p]!=sPmSum[1][p])diff++;
            if(sPmCount[0][p])sPmMarked++;
        }
    }
    if(out)*out=res;
    return diff;
}
static void test_edge_pruning(void)
{
    static const struct { float rect[4]; int screenW,eyes; float stereo,conv; unsigned clearId; } views[]={
        {{40,0,360,240},400,1,0.0f,1.0f,63},         /* the upper screen, the slider down */
        {{40,0,360,240},400,2,0.01f,2.5f,63},        /* the slider up */
        {{40,0,360,240},400,2,0.04f,2.0f,63},        /* (more than it goes) */
        {{41.25f,0,361.25f,240},400,2,0.01f,2.5f,63},/* the screen shaking */
        {{-12.5f,0,307.5f,240},400,2,0.01f,2.5f,63}, /* past the screen's edges */
        {{92.5f,0,412.5f,240},400,1,0.0f,1.0f,0},
        {{0,0,400,240},400,2,0.01f,2.5f,63},         /* a wide view */
        {{0,0,320,240},320,1,0.0f,1.0f,63},          /* the lower screen */
        {{40,0,360,240},400,1,0.0f,1.0f,0},          /* the rear plane's ID the ground's */
    };
    size_t k;
    unsigned scene;
    unsigned pruned=0,all=0;
    for(scene=0;scene<4;scene++){
        pm_scene((int)scene,0x1234u+scene*977u);
        for(k=0;k<sizeof(views)/sizeof(views[0]);k++){
            NitroGx3dPruneResult res;
            int diff;
            memset(&res,0,sizeof(res));
            diff=pm_compare(views[k].rect,views[k].eyes,views[k].stereo,views[k].conv,views[k].clearId,
                                views[k].screenW,&res);
            if(diff){printf("edge pruning: scene %u view %u: %d pixels differ\n",scene,(unsigned)k,diff);}
            if(getenv("PM_DEBUG"))printf("scene %u view %u: %u triangles, A %u D %u zones A %u B %u, %u marked\n",scene,(unsigned)k,res.triangles,res.a,res.d,res.zone_a,res.zone_b,sPmMarked);
            sPmMarked=0;
            CHECK(diff==0);
            pruned+=res.d;all+=res.triangles;
            if(scene==1&&k<2){
                /* most of a route's ground can't get an outline */
                CHECK(res.d*5u<res.triangles);
                CHECK(res.a*2u<res.triangles);
            }
        }
    }
    printf("edge pruning: %u of %u triangles drawn in the outlines' passes\n",pruned,all);
    {
        /* no room for the lists: 0 (the passes draw every triangle then) */
        NitroGx3dPruneView view;
        memset(&view,0,sizeof(view));
        view.x0=40;view.x1=360;view.y1=240;view.width=400;view.height=240;view.eyes=1;view.convergence=1;view.clear_id=63;
        CHECK(!NitroPort_Gx3dPruneEdges(sPm.v,sPm.b,sPm.nb,sPm.role,&view,sPmIdx,6,sPmLists,NULL));
    }
    NitroPort_Gx3dPruneFree();
}
static void run_all(void);
/* A thread that falls behind: the game doesn't wait, the scene after the
 * one it's on is left out (nothing drawn, the last scene stays), and what
 * a left-out scene sets (here the vertex Z) still holds after it. */
static void test_thread_behind(void)
{
    const NitroGx3dScene *scene;
    uint32_t skippedBefore, skipped;
    reset_geometry();
    NitroPort_Gx3dScene();                 /* the thread has caught up */
    NitroPort_Gx3dThreadStats(NULL,NULL,&skippedBefore);
    sHold=1;                               /* and now stops */
    triangle(0,RGB(31,0,0));swap();        /* A */
    triangle(0,RGB(0,31,0));swap();        /* B: two behind, so... */
    triangle(0,RGB(0,0,31));swap();        /* C: left out */
    swap();                                /* D: left out */
    sHold=0;sem_post(&sGate);
    scene=NitroPort_Gx3dScene();
    CHECK(scene->count==1);
    if(scene->count==1)CHECK(scene->triangles[0].v[0].color==RGB(0,31,0));
    /* E, decided at D's SWAP: left out too. Its vertices' Z stays. */
    NitroPort_GxDirectWrite(0x40,0);
    vertex(-3000,-3000,2000);vertex(3000,-3000,2000);vertex(0,3000,2000);
    NitroPort_GxDirectWrite(0x41,0);
    swap();
    /* F: caught up, drawn, with E's Z under its X/Y-only vertices. */
    NitroPort_GxDirectWrite(0x20,RGB(31,31,0));
    NitroPort_GxDirectWrite(0x40,0);
    NitroPort_GxDirectWrite(0x25,word2(-3000,-3000));
    NitroPort_GxDirectWrite(0x25,word2(3000,-3000));
    NitroPort_GxDirectWrite(0x25,word2(0,3000));
    NitroPort_GxDirectWrite(0x41,0);
    swap();
    scene=NitroPort_Gx3dScene();
    CHECK(scene->count==1);
    if(scene->count==1){
        CHECK(scene->triangles[0].v[0].color==RGB(31,31,0));
        CHECK(scene->triangles[0].v[0].clip[2]==2000);
    }
    NitroPort_Gx3dThreadStats(NULL,NULL,&skipped);
    CHECK(skipped-skippedBefore==3);
}
/* Whether a scene from before the game's newest SWAP_BUFFERS may still be
 * read (OS_Halt does the texture transfers queued for the V-blank early
 * only when none may): the thread hasn't got to it, or a consumer is
 * reading it or hasn't taken it. */
static void test_earlier_scene_unread(void)
{
    const NitroGx3dScene *scene;
    uint32_t seen=0;
    reset_geometry();
    NitroPort_Gx3dScene();                 /* done, and read */
    CHECK(!NitroPort_Gx3dEarlierSceneUnread());
    sHold=1;                               /* the thread stops */
    triangle(0,RGB(31,0,0));swap();        /* A: nothing before it */
    CHECK(!NitroPort_Gx3dEarlierSceneUnread());
    triangle(0,RGB(0,31,0));swap();        /* B: A not done yet */
    CHECK(NitroPort_Gx3dEarlierSceneUnread());
    sHold=0;sem_post(&sGate);
    NitroPort_Gx3dScene();
    CHECK(!NitroPort_Gx3dEarlierSceneUnread());
    swap();                                /* C (left out: B was behind) */
    triangle(0,RGB(0,0,31));swap();        /* D */
    NitroPort_Gx3dScene();
    scene=NitroPort_Gx3dAcquireScene(&seen); /* a consumer reading D... */
    CHECK(scene!=NULL);
    triangle(0,RGB(31,31,0));swap();       /* ...when E is ended */
    CHECK(NitroPort_Gx3dEarlierSceneUnread());
    NitroPort_Gx3dReleaseScene(scene);
    CHECK(!NitroPort_Gx3dEarlierSceneUnread());
    NitroPort_Gx3dScene();
}
/* A scene's 3D registers are the game's at its SWAP_BUFFERS: what the
 * game sets for its next scene (here before the thread has even got to
 * this one) isn't the scene's. */
static void test_scene_registers(void)
{
    const NitroGx3dScene *scene;
    uint32_t clear, fog;
    reset_geometry();
    NitroPort_Gx3dScene();                 /* (caught up) */
    sHold=1;
    write32(0x350,0x001F7C1Fu);            /* CLEAR_COLOR: alpha 31, magenta */
    write32(0x358,0x000003E0u);            /* FOG_COLOR: green */
    write32(0x60,0x0089u);                 /* DISP3DCNT: textures, blending, fog */
    triangle(0,RGB(31,0,0));swap();
    write32(0x350,0x00000000u);            /* (the next scene's) */
    write32(0x358,0x0000001Fu);
    write32(0x60,0x0001u);
    sHold=0;sem_post(&sGate);
    scene=NitroPort_Gx3dScene();
    memcpy(&clear,&scene->regs3d[0x350-NITRO_GX3D_REGS_AT],4);
    memcpy(&fog,&scene->regs3d[0x358-NITRO_GX3D_REGS_AT],4);
    CHECK(clear==0x001F7C1Fu);
    CHECK(fog==0x000003E0u);
    CHECK((scene->disp3dcnt&0xFFFFu)==0x0089u);
    write32(0x60,0);
    write32(0x358,0);
}
int main(void)
{
    static const NitroGx3dThreadOps ops = {
        NULL, wait_worker, wake_worker, wait_producer, wake_producer, NULL
    };
    pthread_t thread;
    int synchronous;

    run_all();
    test_edge_pruning();
    swap();
    CHECK(!NitroPort_Gx3dEarlierSceneUnread()); /* without the thread: never */
    synchronous = checks;
    printf("GX3D synthetic scene and raster: %d checks, %d failures\n",checks,failures);
    sem_init(&sWorkerSem, 0, 0);
    sem_init(&sProducerSem, 0, 0);
    sem_init(&sGate, 0, 0);
    NitroPort_Gx3dStartThread(&ops);
    pthread_create(&thread, NULL, geometry_thread, NULL);
    run_all();
    test_thread_behind();
    test_earlier_scene_unread();
    test_scene_registers();
    sRunning = 0;
    wake_worker(NULL);
    pthread_join(thread, NULL);
    printf("GX3D on its own thread: %d checks, %d failures\n",checks-synchronous,failures);
    return failures?1:0;
}
static void run_all(void)
{
    test_matrices();
    test_packed_fifo();
    test_vtx_diff();
    test_triangles_and_depth();
    test_culling();
    test_shadow_triangles();
    test_sprite_texel_a_pixel();
    test_soft_sprites();
    test_interpolation();
    test_interpolation_followed();
    test_interpolation_one_each();
    test_interpolation_order_shift();
    test_interpolation_tile_a_scene();
    test_interpolation_swaying();
    test_triangle_strip();
    test_hidden_vertices();
    test_textures();
    test_box_test();
    test_wide_view();
    test_clear_and_status();
    test_bg0_composition();
    test_cpu_fifo_writes();
    test_light_material_and_vectors();
    test_texture_matrix_modes();
    test_latched_polygon_attributes();
    test_viewport_and_immediate_texture_state();
    test_stack_vec_and_result_refresh();
    test_translucency_and_scene_limits();
    test_edge_marking();
}
