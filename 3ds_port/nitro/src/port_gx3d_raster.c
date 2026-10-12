/*
 * PC reference rasterizer for DS scene triangles (not a 3DS GPU backend).
 * Perspective-correct texture coordinates, z-buffer, the DS texture formats,
 * and palettes from VRAM bank slots, including 4x4 compressed textures.
 *
 * Unimplemented DS-specific postprocessing: toon/highlight, fog, edge
 * marking, antialiasing, polygon IDs and shadow stencil. Translucent faces
 * are drawn after opaque faces, but full DS alpha-sort modes, per-pixel
 * translucent polygon IDs, and translucency sorting by depth remain TODO.
 * Geometry capture
 * and the scene data format are independent of all rasterizer choices.
 */
#include <nitro_port_gx3d.h>
#include <nitro.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

extern unsigned char NitroPort_IoReg[];
extern unsigned char NitroPort_LcdcVram[];
#define W NITRO_GX3D_WIDTH
#define H NITRO_GX3D_HEIGHT

typedef struct Texel { uint16_t rgb; uint8_t alpha; } Texel;
static unsigned char *sTexture[4];
static unsigned char *sPalette[6];
static uint32_t sDepth[W*H];

static uint16_t rd16(const unsigned char *p)
{
    return (uint16_t)(p[0]|((unsigned)p[1]<<8));
}
static uint32_t rd32(const unsigned char *p)
{
    return (uint32_t)rd16(p)|((uint32_t)rd16(p+2)<<16);
}
static int restrict_int(int x,int lo,int hi)
{
    return x<lo?lo:x>hi?hi:x;
}
static void map_banks(void)
{
    unsigned i;
    memset(sTexture,0,sizeof(sTexture));
    memset(sPalette,0,sizeof(sPalette));
    /* A..D texture slots: 128 KiB each, controlled by VRAMCNT A-D. */
    for(i=0;i<4;i++){
        unsigned v=NitroPort_IoReg[0x240+i];
        if((v&0x87)==0x83)sTexture[(v>>3)&3]=NitroPort_LcdcVram+i*0x20000;
    }
    /* E is 64 KiB; F/G are 16 KiB each. Standard texture palette banks. */
    if((NitroPort_IoReg[0x244]&0x87)==0x83){
        for(i=0;i<4;i++)sPalette[i]=NitroPort_LcdcVram+0x80000+i*0x4000;
    }
    if((NitroPort_IoReg[0x245]&0x87)==0x83)
        sPalette[0]=NitroPort_LcdcVram+0x90000;
    if((NitroPort_IoReg[0x246]&0x87)==0x83)
        sPalette[1]=NitroPort_LcdcVram+0x94000;
}
static unsigned char texture_u8(uint32_t offset)
{
    unsigned char *bank=sTexture[(offset>>17)&3];
    return bank ? bank[offset & 0x1ffff] : 0;
}
static uint16_t texture_u16(uint32_t offset)
{
    return (uint16_t)(texture_u8(offset)|((uint16_t)texture_u8(offset+1)<<8));
}
static uint32_t texture_u32(uint32_t offset)
{
    return (uint32_t)texture_u16(offset)|((uint32_t)texture_u16(offset+2)<<16);
}
static uint16_t palette_u16(uint32_t byte_offset)
{
    unsigned slot=byte_offset>>14;
    if(slot>=6||!sPalette[slot])return 0;
    return rd16(sPalette[slot]+(byte_offset & 0x3ffe))&0x7fff;
}
static int wrap_axis(int pos,int size,int repeat,int flip)
{
    if(pos>=0 && pos<size)return pos;
    if(!repeat)return restrict_int(pos,0,size-1);
    if(flip){
        int range=size*2;
        int p=pos%range;if(p<0)p+=range;
        return p>=size?range-1-p:p;
    }
    pos%=size;
    return pos<0?pos+size:pos;
}
static uint16_t color_mix(uint16_t a,uint16_t b,int aw,int bw,int denom)
{
    int r=((a&31)*aw+(b&31)*bw)/denom;
    int g=(((a>>5)&31)*aw+((b>>5)&31)*bw)/denom;
    int bl=(((a>>10)&31)*aw+((b>>10)&31)*bw)/denom;
    return (uint16_t)(r|(g<<5)|(bl<<10));
}
static Texel sample_texture(uint32_t param,uint32_t base,int s,int t)
{
    Texel result={0x7fff,31};
    unsigned fmt=(param>>26)&7;
    unsigned width=8u<<((param>>20)&7);
    unsigned height=8u<<((param>>23)&7);
    uint32_t address=(param&0xffffu)*8u;
    uint32_t paletteAddr=base*(fmt==2?8u:16u);
    unsigned i=0;
    if(fmt==0)return result;
    s=wrap_axis(s,(int)width,(param>>16)&1,(param>>18)&1);
    t=wrap_axis(t,(int)height,(param>>17)&1,(param>>19)&1);
    i=(unsigned)t*width+(unsigned)s;
    if(fmt==1){
        uint8_t value=texture_u8(address+i);
        unsigned a=value>>5;
        result.alpha=(uint8_t)(a*4+a/2);
        result.rgb=palette_u16(paletteAddr+(value&31)*2);
    }else if(fmt==2){
        i=(texture_u8(address+i/4)>>((i%4)*2))&3;
        result.rgb=palette_u16(paletteAddr+i*2);
        if(i==0 && (param&(1u<<29)))result.alpha=0;
    }else if(fmt==3){
        i=(texture_u8(address+i/2)>>((i%2)*4))&15;
        result.rgb=palette_u16(paletteAddr+i*2);
        if(i==0 && (param&(1u<<29)))result.alpha=0;
    }else if(fmt==4){
        i=texture_u8(address+i);
        result.rgb=palette_u16(paletteAddr+i*2);
        if(i==0 && (param&(1u<<29)))result.alpha=0;
    }else if(fmt==5){
        unsigned blocksPerRow=width/4;
        uint32_t block=(uint32_t)(t/4)*blocksPerRow+(uint32_t)(s/4);
        uint32_t texelAddr=address+block*4;
        uint32_t slot1Addr;
        uint16_t descriptor;
        unsigned mode,selector;
        uint16_t cols[4];
        uint32_t bits=texture_u32(texelAddr);
        selector=(bits>>(((t&3)*4+(s&3))*2))&3;
        /* Slot 1: low 64 KiB for compressed tiles in slot 0;
         * upper 64 KiB for slot 2. */
        slot1Addr=0x20000u+(texelAddr&0x1ffffu)/2u;
        if(texelAddr>=0x40000u)slot1Addr+=0x10000u;
        descriptor=texture_u16(slot1Addr);
        mode=descriptor>>14;
        paletteAddr=base*16u+(descriptor&0x3fffu)*4u;
        cols[0]=palette_u16(paletteAddr);
        cols[1]=palette_u16(paletteAddr+2);
        cols[2]=palette_u16(paletteAddr+4);
        cols[3]=palette_u16(paletteAddr+6);
        if(mode==1)cols[2]=color_mix(cols[0],cols[1],1,1,2);
        if(mode==3){
            cols[2]=color_mix(cols[0],cols[1],5,3,8);
            cols[3]=color_mix(cols[0],cols[1],3,5,8);
        }
        if(selector==3 && mode<=1)result.alpha=0;
        result.rgb=cols[selector];
    }else if(fmt==6){
        uint8_t value=texture_u8(address+i);
        result.alpha=value>>3;
        result.rgb=palette_u16(paletteAddr+(value&7)*2);
    }else if(fmt==7){
        uint16_t value=texture_u16(address+i*2);
        result.rgb=value&0x7fff;
        result.alpha=(value&0x8000)?31:0;
    }
    return result;
}
static double edge(double ax,double ay,double bx,double by,double px,double py)
{
    return (px-ax)*(by-ay)-(py-ay)*(bx-ax);
}
static uint16_t interp_rgb(const NitroGx3dTriangle *tri,double u,double v,double w)
{
    const uint16_t a=tri->v[0].color,b=tri->v[1].color,c=tri->v[2].color;
    int r=(int)((a&31)*u+(b&31)*v+(c&31)*w+0.5);
    int g=(int)(((a>>5)&31)*u+((b>>5)&31)*v+((c>>5)&31)*w+0.5);
    int bl=(int)(((a>>10)&31)*u+((b>>10)&31)*v+((c>>10)&31)*w+0.5);
    return (uint16_t)(restrict_int(r,0,31)|(restrict_int(g,0,31)<<5)|
                      (restrict_int(bl,0,31)<<10));
}
static uint16_t modulate(uint16_t tex,uint16_t vertex)
{
    int r=((tex&31)*(vertex&31)+15)/31;
    int g=(((tex>>5)&31)*((vertex>>5)&31)+15)/31;
    int b=(((tex>>10)&31)*((vertex>>10)&31)+15)/31;
    return (uint16_t)(r|(g<<5)|(b<<10));
}
static void raster_triangle(const NitroGx3dTriangle *tri,uint16_t *out)
{
    double x[3],y[3],invarea;
    double area;
    int x0,y0,x1,y1,xp,yp,i,alpha;
    unsigned fmt=(tri->texture_param>>26)&7;
    for(i=0;i<3;i++){x[i]=tri->v[i].x/256.0;y[i]=tri->v[i].y/256.0;}
    area=edge(x[0],y[0],x[1],y[1],x[2],y[2]);
    if(fabs(area)<1e-8)return;
    invarea=1.0/area;
    x0=restrict_int((int)floor(fmin(x[0],fmin(x[1],x[2]))),0,W-1);
    x1=restrict_int((int)ceil(fmax(x[0],fmax(x[1],x[2]))),0,W-1);
    y0=restrict_int((int)floor(fmin(y[0],fmin(y[1],y[2]))),0,H-1);
    y1=restrict_int((int)ceil(fmax(y[0],fmax(y[1],y[2]))),0,H-1);
    alpha=(tri->polygon_attr>>16)&31;
    if(!alpha)return;
    for(yp=y0;yp<=y1;yp++)for(xp=x0;xp<=x1;xp++){
        double u=edge(x[1],y[1],x[2],y[2],xp+0.5,yp+0.5)*invarea;
        double v=edge(x[2],y[2],x[0],y[0],xp+0.5,yp+0.5)*invarea;
        double w=1.0-u-v;
        double iz,uu,vv;
        uint32_t depth;
        unsigned index=(unsigned)yp*W+(unsigned)xp;
        uint16_t rgb;
        int pixelAlpha = alpha;
        if(u<-1e-6||v<-1e-6||w<-1e-6)continue;
        depth=(uint32_t)(tri->v[0].depth*u+tri->v[1].depth*v+tri->v[2].depth*w);
        if(depth>=sDepth[index] && !(tri->polygon_attr&(1u<<14)))continue;
        if((tri->polygon_attr&(1u<<14)) &&
           (depth>sDepth[index]+0x200 || depth+0x200<sDepth[index]))continue;
        rgb=interp_rgb(tri,u,v,w);
        if(fmt){
            Texel tex;
            iz=u*tri->v[0].inv_w+v*tri->v[1].inv_w+w*tri->v[2].inv_w;
            if(fabs(iz)<1e-15)continue;
            uu=(u*tri->v[0].s*tri->v[0].inv_w+
                v*tri->v[1].s*tri->v[1].inv_w+
                w*tri->v[2].s*tri->v[2].inv_w)/iz;
            vv=(u*tri->v[0].t*tri->v[0].inv_w+
                v*tri->v[1].t*tri->v[1].inv_w+
                w*tri->v[2].t*tri->v[2].inv_w)/iz;
            tex=sample_texture(tri->texture_param,tri->palette_base,(int)floor(uu),(int)floor(vv));
            pixelAlpha=(alpha*tex.alpha+15)/31;
            if(pixelAlpha==0)continue;
            rgb=modulate(tex.rgb,rgb);
        }
        /* DISP3DCNT bit3 enables alpha blending with the 3D colour buffer.
         * When disabled, fractional alpha still controls coverage but the
         * source colour replaces the destination on a visible fragment. */
        if(pixelAlpha<31 && (NitroPort_IoReg[0x60]&8u) &&
           (out[index]&0x8000)){
            uint16_t previous=out[index]&0x7fff;
            rgb=color_mix(rgb,previous,pixelAlpha,31-pixelAlpha,31);
        }
        out[index]=(uint16_t)(rgb|0x8000);
        if(pixelAlpha==31 || (tri->polygon_attr&(1u<<11)))sDepth[index]=depth;
    }
}
void NitroPort_Gx3dRasterize(uint16_t *rgba555,uint32_t *depth_out)
{
    const NitroGx3dScene *scene;
    uint32_t clear,depth;
    size_t i;
    if(!rgba555)return;
    scene=NitroPort_Gx3dScene();
    clear=rd32(NitroPort_IoReg+0x350);
    depth=rd16(NitroPort_IoReg+0x354)&0x7fff;
    /* CLEAR_DEPTH 15-bit maps to 24-bit Z; clear alpha 0 keeps BG0 transparent. */
    for(i=0;i<W*H;i++){
        sDepth[i]=depth*0x200u;
        rgba555[i]=(uint16_t)((clear&0x7fff)|(((clear>>16)&31)?0x8000:0));
    }
    if(scene->count){
        unsigned pass;
        map_banks();
        /* DS polygon RAM draws the opaque pass before translucent faces
         * regardless of submission order. Alpha-bearing texel formats may
         * contain fractional alpha even with POLYGON_ATTR alpha=31. */
        for(pass=0;pass<2;pass++)for(i=0;i<scene->count;i++){
            const NitroGx3dTriangle *tri=&scene->triangles[i];
            unsigned fmt=(tri->texture_param>>26)&7u;
            unsigned alpha=(tri->polygon_attr>>16)&31u;
            int translucent=alpha<31u || fmt==1u || fmt==6u ||
                             (fmt>=2u && fmt<=4u &&
                              (tri->texture_param&(1u<<29))) ||
                             fmt==5u || fmt==7u;
            if((int)pass==translucent)raster_triangle(tri,rgba555);
        }
    }
    if(depth_out)memcpy(depth_out,sDepth,sizeof(sDepth));
}
