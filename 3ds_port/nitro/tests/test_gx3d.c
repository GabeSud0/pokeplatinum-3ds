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
int main(void)
{
    test_matrices();
    test_packed_fifo();
    test_triangles_and_depth();
    test_culling();
    test_textures();
    test_box_test();
    test_clear_and_status();
    printf("GX3D synthetic scene and raster: %d checks, %d failures\n",checks,failures);
    return failures?1:0;
}
