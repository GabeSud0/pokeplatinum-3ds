/*
 * No copyrighted game data. Geometry commands use the original ARM9 G3_*
 * API (or the exact packed FIFO bytes), then validate the exported scene and
 * the PC reference renderer independently of the 3DS GPU.
 */
#include <nitro.h>
#include <nitro_port.h>
#include <nitro_port_gx3d.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

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
static void test_matrices(void)
{
    reset_geometry();
    NitroPort_GxDirectWrite(0x10,2);
    G3_Translate(ONE,0,0);
    NitroPort_Gx3dRefreshStatus();
    CHECK(read32(0x640+12*4)==ONE); /* clip matrix's translation */
    G3_PushMtx();
    G3_Translate(2*ONE,0,0);
    NitroPort_Gx3dRefreshStatus();
    CHECK(read32(0x640+12*4)==3*ONE);
    CHECK(((uint32_t)read32(0x600)>>8&31)==1);
    G3_PopMtx(1);
    NitroPort_Gx3dRefreshStatus();
    CHECK(read32(0x640+12*4)==ONE);
    CHECK(((uint32_t)read32(0x600)>>8&31)==0);
    G3_StoreMtx(2);
    G3_Translate(2*ONE,0,0);
    G3_RestoreMtx(2);
    NitroPort_Gx3dRefreshStatus();
    CHECK(read32(0x640+12*4)==ONE);
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
    CHECK(read32(0x640)==2*ONE);
    CHECK(read32(0x640+5*4)==3*ONE);
    /* Matrix mode 2 excludes scale from the directional matrix. */
    CHECK(read32(0x680)==ONE);

    /* Multiplies go on the left: Current = Parameter x Current (GBATEK).
     * A model's scale after a camera that moves things by 5 doesn't scale
     * the camera's move (NNS draws camera, then each model's transform). */
    reset_geometry();
    NitroPort_GxDirectWrite(0x10,2);
    G3_Translate(5*ONE,0,0);
    G3_Scale(2*ONE,ONE,ONE);
    NitroPort_Gx3dRefreshStatus();
    CHECK(read32(0x640+12*4)==5*ONE);
    CHECK(read32(0x640)==2*ONE);
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
    CHECK(read32(0x640+12*4)==0);
    CHECK(read32(0x640+13*4)==3*ONE);
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
        CHECK(read32(0x640+12*4)==ONE);
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
    NitroPort_GxDirectWrite(0x15,0); /* matrix changes regenerate registers */
    CHECK(read32(0x640)==ONE);
}

static void test_translucency_and_scene_limits(void)
{
    const NitroGx3dScene *scene;
    unsigned i;
    reset_geometry();
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

int main(void)
{
    test_matrices();
    test_packed_fifo();
    test_vtx_diff();
    test_triangles_and_depth();
    test_culling();
    test_textures();
    test_box_test();
    test_clear_and_status();
    test_bg0_composition();
    test_cpu_fifo_writes();
    test_light_material_and_vectors();
    test_texture_matrix_modes();
    test_latched_polygon_attributes();
    test_viewport_and_immediate_texture_state();
    test_stack_vec_and_result_refresh();
    test_translucency_and_scene_limits();
    printf("GX3D synthetic scene and raster: %d checks, %d failures\n",checks,failures);
    return failures?1:0;
}
