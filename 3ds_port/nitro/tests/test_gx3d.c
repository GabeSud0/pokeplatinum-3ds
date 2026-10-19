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
int main(void)
{
    static const NitroGx3dThreadOps ops = {
        NULL, wait_worker, wake_worker, wait_producer, wake_producer, NULL
    };
    pthread_t thread;
    int synchronous;

    run_all();
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
    test_interpolation();
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
