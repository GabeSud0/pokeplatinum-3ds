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
#include <nitro_port_gx3d_texture.h>
#include <nitro.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

extern unsigned char NitroPort_IoReg[];
#define W NITRO_GX3D_WIDTH
#define H NITRO_GX3D_HEIGHT

typedef NitroGx3dTexel Texel;
static NitroGx3dTexBanks sBanks;
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
static uint16_t color_mix(uint16_t a,uint16_t b,int aw,int bw,int denom)
{
    int r=((a&31)*aw+(b&31)*bw)/denom;
    int g=(((a>>5)&31)*aw+((b>>5)&31)*bw)/denom;
    int bl=(((a>>10)&31)*aw+((b>>10)&31)*bw)/denom;
    return (uint16_t)(r|(g<<5)|(bl<<10));
}
/* The texel decoder is shared with the 3DS GPU path (port_gx3d_texture.c). */
static Texel sample_texture(uint32_t param,uint32_t base,int s,int t)
{
    return NitroPort_Gx3dSampleTexel(&sBanks,param,base,s,t);
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
void NitroPort_Gx3dRasterizeScene(const NitroGx3dScene *scene,
                                  uint16_t *rgba555,uint32_t *depth_out)
{
    uint32_t clear,depth;
    size_t i;
    if(!rgba555 || !scene)return;
    clear=rd32(NitroPort_IoReg+0x350);
    depth=rd16(NitroPort_IoReg+0x354)&0x7fff;
    /* CLEAR_DEPTH 15-bit maps to 24-bit Z; clear alpha 0 keeps BG0 transparent. */
    for(i=0;i<W*H;i++){
        sDepth[i]=depth*0x200u;
        rgba555[i]=(uint16_t)((clear&0x7fff)|(((clear>>16)&31)?0x8000:0));
    }
    if(scene->count){
        unsigned pass;
        NitroPort_Gx3dMapTextureBanks(&sBanks);
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

/* Compatibility wrapper for original Stage 1 callers; GPU worker should
 * always use AcquireScene/ReleaseScene directly, never this shortcut. */
void NitroPort_Gx3dRasterize(uint16_t *rgba555,uint32_t *depth_out)
{
    uint32_t serial=0;
    const NitroGx3dScene *scene=NitroPort_Gx3dAcquireScene(&serial);
    if(scene){
        NitroPort_Gx3dRasterizeScene(scene,rgba555,depth_out);
        NitroPort_Gx3dReleaseScene(scene);
    }else{
        /* No SWAP yet; preserve the original clear-only result. */
        NitroPort_Gx3dRasterizeScene(NitroPort_Gx3dScene(),rgba555,depth_out);
    }
}
