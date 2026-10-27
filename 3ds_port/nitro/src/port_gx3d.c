/*
 * Nintendo DS 3D geometry engine (ARM9), reference implementation.
 *
 * GXFIFO is drained at once as far as the game can tell: the game's
 * MI_SendGXCommandAsync expects a less-than-half-full FIFO interrupt, and
 * rendering is not paced to DS geometry-engine clock cycles. The work can
 * be on a thread of its own (NitroPort_Gx3dStartThread): the game's thread
 * then decodes the commands and keeps a shadow of the matrices, which is
 * all the game reads back, and hands the commands on through a ring.
 *
 * Matrices use row vectors and signed 20.12 fixed point. The command decoder
 * accepts packed command words with interleaved parameters and direct register
 * writes with the same parameter-count table. A SWAP publishes an immutable
 * triangle scene for the rasterizer (and, later, PICA200).
 */
#include <nitro.h>
#include <nitro_port.h>
#include <nitro_port_gx3d.h>
#include <nitro_port_gx3d_mailbox.h>
#include <nitro_port_gx3d_vertex.h>
#include <nitro_port_shadow.h>
#include "port_platform.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

extern unsigned char NitroPort_IoReg[];
typedef struct Matrix { int32_t a[16]; } Matrix;
typedef struct GeoVertex {
    int32_t clip[4];
    int32_t s,t;
    uint16_t color;
    uint8_t outside;   /* bit p: outside clip plane p (see plane_distance) */
    uint8_t hasScreen; /* screen is filled in (strips share vertices) */
    uint8_t lit;       /* color is worked out; else it's normal's (light_vertex) */
    uint32_t normal;   /* the NORMAL command's value, while not lit */
    NitroGx3dVertex screen;
} GeoVertex;
typedef struct GeoState {
    Matrix proj, pos, vec, tex;
    Matrix clip;        /* pos x proj, as the DS keeps it; see ensure_clip */
    /* What the vertices (and box tests) go through: the clip matrix, its
     * x column scaled for a wider view (NitroPort_Gx3dSetWideView), or
     * the clip matrix itself. */
    Matrix xf;
    int64_t xfTrans[4];   /* its row 3, << 12: what (0, 0, 0, 1.0) adds */
    int clipDirty;
    int32_t wideNum, wideDen;   /* the view's width scale (NitroPort_Gx3dSetWideView) */
    Matrix pjStack, texStack;
    Matrix posStack[31], vecStack[31];
    int mode, pvLevel, pjLevel, texLevel, stackError;
    int32_t coord[3];
    int32_t texcoord[2], normal[3];
    uint16_t rgb, diffuse, ambient, specular, emission;
    int32_t resolvedTexcoord[2];
    uint32_t attr, pendingAttr, texparam, plttbase, shininess[32];
    int useShininess;
    unsigned frameVertices, framePolygons;
    int32_t lightDir[4][3];
    int32_t lightHalf[4][3]; /* half vectors: (light + (0,0,-1)) / 2 */
    uint16_t lightColor[4];
    int viewport[4], primitive, nverts;
    GeoVertex vertices[4];
    int inBegin, boxVisible, stripStarted;
    /* The last NORMAL's colour isn't worked out yet (normalValue's): it is
     * when a polygon with it is drawn, or before the lighting changes. */
    int rgbPending;
    uint32_t normalValue;
    int stripOld, stripNew;   /* a triangle strip's last two (vertices[]) */
    int ready;
} GeoState;

/* The engine. With a geometry thread (NitroPort_Gx3dStartThread) that
 * thread owns it, and the game's thread keeps a shadow of its matrices
 * (sShadow): what the game reads back (matrix results, the stack level,
 * box/position/vector tests) depends on nothing else, so the game never
 * waits for the vertices. sResults is the one the registers come from. */
static GeoState s, sShadow;
static GeoState *sResults = &s;
static int sThreaded;

/* The command decoder (the game's thread): packed command words and their
 * parameters, or a direct register command's parameters. */
static struct {
    int packedIndex, packedCount, need, used, directOpcode, directNeed;
    uint8_t packed[4];
    uint32_t params[32];
} sDec;

/* A harmless host/library default. The 3DS application's main overrides
 * this weak function with LightEvent_Signal. noinline ensures a call
 * relocation remains so the linked strong override is actually invoked.
 * Unlike an undefined weak reference, a definition also satisfies the
 * port's strict SDK/host link coverage checker. */
__attribute__((weak,noinline)) void NitroPort_Gx3dScenePublished(void)
{
    __asm__ __volatile__("" ::: "memory");
}
/* Three ~0.6 MiB scenes: game builds one, GPU owns one, and a pending
 * completed scene occupies the third. Never copy or overwrite GPU-owned data.
 * A short atomic lock protects only the mailbox indices, never geometry. */
static NitroGx3dScene sScenes[3];
static NitroGx3dMailbox sMailbox;
static unsigned sBuilding, sDisplayed;
/* Where a finished polygon's triangles go: the scene being built, or
 * (making an in-between scene: replay_polygon) that scene. */
static NitroGx3dScene *sDest;
static inline NitroGx3dScene *dest_scene(void){return sDest?sDest:&sScenes[sBuilding];}
static uint32_t sSerial;
static uint32_t sSwapsDone;   /* (the geometry thread's; see below) */

static inline int32_t saturation(int64_t n)
{
    /* Nearly always in range: one comparison for that. */
    int32_t low=(int32_t)n;
    if(__builtin_expect((int64_t)low==n,1))return low;
    return n>0?INT32_MAX:INT32_MIN;
}
static int32_t signbits(uint32_t v, int bits)
{
    const uint32_t mask = (1u << bits) - 1u;
    v &= mask;
    return (int32_t)((v ^ (1u << (bits-1))) - (1u << (bits-1)));
}
static uint32_t io32(unsigned address)
{
    uint32_t n;
    memcpy(&n, NitroPort_IoReg + address, sizeof(n));
    return n;
}
static void put32(unsigned address, uint32_t value)
{
    memcpy(NitroPort_IoReg + address, &value, sizeof(value));
}
static void identity(Matrix *m)
{
    int i;
    memset(m, 0, sizeof(*m));
    for (i=0; i<4; i++) m->a[i*4+i] = 4096;
}
/* All 64 products: picking out the non-zero ones (most matrices are part
 * zeros) cost more in tests and branches than the products it saved. A
 * zero term doesn't change a sum, so it's the same either way. The rows
 * of the right-hand matrix are read once per row of the left. */
static void multiply(Matrix *out, const Matrix *lhs, const Matrix *rhs)
{
    Matrix result;
    int i,j;
    for(i=0;i<4;++i){
        int64_t l0=lhs->a[i*4],l1=lhs->a[i*4+1],l2=lhs->a[i*4+2],l3=lhs->a[i*4+3];
        for(j=0;j<4;++j){
            int64_t acc=l0*rhs->a[j]+l1*rhs->a[4+j]+l2*rhs->a[8+j]+l3*rhs->a[12+j];
            result.a[i*4+j]=saturation(acc >> 12);
        }
    }
    *out=result;
}
/* MTX_SCALE's product (diag(x, y, z, 1) x m): rows 0-2 scaled, row 3 as it
 * was (1.0 x v >> 12 is v). As multiply() would make it. */
static void scale_rows(Matrix *m,const uint32_t *p)
{
    int i,j;
    for(i=0;i<3;i++){
        int64_t f=(int32_t)p[i];
        for(j=0;j<4;j++)m->a[i*4+j]=saturation((f*m->a[i*4+j])>>12);
    }
}
/* MTX_TRANS's product (the identity with (x, y, z, 1) as row 3, x m):
 * rows 0-2 as they were, row 3 the translation through m. As multiply()
 * would make it. */
static void translate_rows(Matrix *m,const uint32_t *p)
{
    int64_t x=(int32_t)p[0],y=(int32_t)p[1],z=(int32_t)p[2];
    int j;
    for(j=0;j<4;j++)
        m->a[12+j]=saturation((x*m->a[j]+y*m->a[4+j]+z*m->a[8+j]+((int64_t)m->a[12+j]<<12))>>12);
}
static void transform(const int32_t in[4],const Matrix *m,int32_t out[4])
{
    int j,k;
    for(j=0;j<4;j++){
        int64_t v=0;
        for(k=0;k<4;k++) v+=(int64_t)in[k]*m->a[k*4+j];
        out[j]=saturation(v>>12);
    }
}
/* transform() for a direction (w = 0): normals and light vectors. */
static void transform_vector(const int32_t in[3],const Matrix *m,int32_t out[3])
{
    int64_t x=in[0],y=in[1],z=in[2];
    int j;
    for(j=0;j<3;j++)
        out[j]=saturation((x*m->a[j]+y*m->a[4+j]+z*m->a[8+j])>>12);
}
static Matrix *selected(GeoState *g)
{
    if (g->mode==0) return &g->proj;
    if (g->mode==3) return &g->tex;
    return &g->pos;
}
static void set_matrix(GeoState *g,const Matrix *m, int scale)
{
    *selected(g)=*m;
    if(g->mode==2 && !scale) g->vec=*m;
}
/* MTX_MULT/SCALE/TRANS: Current = Parameter x Current (GBATEK). With
 * row vectors (v' = v x M), the new matrix applies to a vertex first:
 * a model's own transform goes before the camera loaded earlier. */
static void mul_matrix(GeoState *g,const Matrix *m, int scale)
{
    Matrix newpos, newvec;
    multiply(&newpos,m,selected(g));
    if(g->mode==2 && !scale) multiply(&newvec,m,&g->vec);
    *selected(g)=newpos;
    if(g->mode==2 && !scale) g->vec=newvec;
}
static void matrix_stack(GeoState *g,unsigned opcode,int32_t param)
{
    int index;
    if(g->mode==0){
        if(opcode==0x11){if(g->pjLevel>=1) g->stackError=1;else {g->pjStack=g->proj;g->pjLevel++;}}
        else if(opcode==0x12){if(!g->pjLevel)g->stackError=1;else{g->proj=g->pjStack;g->pjLevel=0;}}
        else if(opcode==0x13){if(param!=0)g->stackError=1;else g->pjStack=g->proj;}
        else if(opcode==0x14){if(param!=0)g->stackError=1;else g->proj=g->pjStack;}
    }else if(g->mode==3){
        if(opcode==0x11){if(g->texLevel)g->stackError=1;else{g->texStack=g->tex;g->texLevel=1;}}
        else if(opcode==0x12){if(!g->texLevel)g->stackError=1;else{g->tex=g->texStack;g->texLevel=0;}}
        else if(opcode==0x13)g->texStack=g->tex;
        else if(opcode==0x14)g->tex=g->texStack;
    }else{
        if(opcode==0x11){
            if(g->pvLevel>=31) g->stackError=1;
            else{ g->posStack[g->pvLevel]=g->pos;g->vecStack[g->pvLevel]=g->vec; ++g->pvLevel; }
        }else if(opcode==0x12){
            index=g->pvLevel-signbits((uint32_t)param,6);
            /* Level 31 is the next free slot, not a valid saved entry. */
            if(index<0||index>30){g->stackError=1;return;}
            g->pvLevel=index;
            g->pos=g->posStack[index];g->vec=g->vecStack[index];
        }else if(opcode==0x13 || opcode==0x14){
            index=param & 31;
            if(index>30){g->stackError=1;return;}
            if(opcode==0x13){g->posStack[index]=g->pos;g->vecStack[index]=g->vec;}
            else{g->pos=g->posStack[index];g->vec=g->vecStack[index];}
        }
    }
}
static Matrix read_matrix(unsigned opcode,const uint32_t *p)
{
    Matrix m;
    int i,r,c;
    identity(&m);
    if(opcode==0x16||opcode==0x18){
        for(i=0;i<16;i++)m.a[i]=(int32_t)p[i];
    }else if(opcode==0x17||opcode==0x19){
        for(r=0;r<4;r++)for(c=0;c<3;c++)m.a[r*4+c]=(int32_t)p[r*3+c];
    }else if(opcode==0x1a){
        for(r=0;r<3;r++)for(c=0;c<3;c++)m.a[r*4+c]=(int32_t)p[r*3+c];
    }
    return m;
}
/* Multiply two RGB555 colours per component. Material reflectance and
 * the selected light colour are independent on the DS. */
static uint16_t rgb_product(uint16_t a,uint16_t b)
{
    unsigned r=((a&31)*(b&31)+15)/31;
    unsigned g=((((a>>5)&31)*((b>>5)&31))+15)/31;
    unsigned bl=((((a>>10)&31)*((b>>10)&31))+15)/31;
    return (uint16_t)(r|(g<<5)|(bl<<10));
}

/* Each light's colour times the material's ambient, diffuse and specular
 * colours (rgb_product), per component: they only change with LIGHT_COLOR,
 * DIF_AMB and SPE_EMI, not with every normal. */
static struct {
    int dirty;
    uint8_t ambient[4][3], diffuse[4][3], specular[4][3];
} sLightCache = { 1 };
/* rgb_product's per component, (a * b + 15) / 31: made once (the reset). */
static uint8_t sProduct[32][32];
static void make_products(void)
{
    unsigned a,b;
    for(a=0;a<32;a++)for(b=0;b<32;b++)sProduct[a][b]=(uint8_t)rgb_product((uint16_t)a,(uint16_t)b);
}
static void split_product(uint8_t out[3],uint16_t a,uint16_t b)
{
    out[0]=sProduct[a&31][b&31];
    out[1]=sProduct[(a>>5)&31][(b>>5)&31];
    out[2]=sProduct[(a>>10)&31][(b>>10)&31];
}
static void update_light_cache(void)
{
    int i;
    for(i=0;i<4;i++){
        split_product(sLightCache.ambient[i],s.ambient,s.lightColor[i]);
        split_product(sLightCache.diffuse[i],s.diffuse,s.lightColor[i]);
        split_product(sLightCache.specular[i],s.specular,s.lightColor[i]);
    }
    sLightCache.dirty=0;
}

/* GBATEK: NORMAL applies the material per enabled light, then emission
 * exactly once. The half vector uses a fixed (0,0,-1) view direction.
 * Shininess entries are 128 unsigned 0.8 values, not 32 32-bit values.
 * The sum is worked out per component and clamped once at the end: every
 * term is positive, so that's the same as clamping after each addition. */
/* The last normal's colour: flat surfaces repeat the same normal, vertex
 * after vertex. Forgotten when anything it depends on changes (matrices,
 * lights, materials, the polygon's lights at BEGIN_VTXS). */
static struct { int valid; uint32_t value; uint16_t rgb; } sNormalCache;
static uint16_t shade(uint32_t value)
{
    int32_t normal[3], transformed[3];
    unsigned r=s.emission&31u,g=(s.emission>>5)&31u,b=(s.emission>>10)&31u;
    int i,j;
    uint16_t rgb;
    if(sNormalCache.valid && sNormalCache.value==value)return sNormalCache.rgb;
    for(j=0;j<3;j++)normal[j]=signbits(value>>(j*10),10)*8;
    transform_vector(normal,&s.vec,transformed);
    if(sLightCache.dirty)update_light_cache();
    for(i=0;i<4;i++)if(s.attr&(1u<<i)){
        int64_t dot=0,halfDot=0;
        unsigned diffuse,specIntensity;
        int d,h;
        for(j=0;j<3;j++){
            dot+=(int64_t)transformed[j]*s.lightDir[i][j];
            halfDot+=(int64_t)transformed[j]*s.lightHalf[i][j];
        }
        d=(int)(-dot>>12);
        diffuse=d<0?0u:d>4096?4096u:(unsigned)d;
        h=(int)(-halfDot>>12);
        specIntensity=h<0?0u:h>4096?4096u:(unsigned)h;
        specIntensity=(specIntensity*specIntensity)>>12;
        if(s.useShininess){
            unsigned index=(specIntensity*128u)>>12;
            unsigned entry;
            if(index>127u)index=127u;
            entry=(s.shininess[index>>2]>>((index&3u)*8u))&255u;
            specIntensity=entry*4096u/255u;
        }
        r+=sLightCache.ambient[i][0]+((sLightCache.diffuse[i][0]*diffuse+2048u)>>12)+
           ((sLightCache.specular[i][0]*specIntensity+2048u)>>12);
        g+=sLightCache.ambient[i][1]+((sLightCache.diffuse[i][1]*diffuse+2048u)>>12)+
           ((sLightCache.specular[i][1]*specIntensity+2048u)>>12);
        b+=sLightCache.ambient[i][2]+((sLightCache.diffuse[i][2]*diffuse+2048u)>>12)+
           ((sLightCache.specular[i][2]*specIntensity+2048u)>>12);
    }
    rgb=(uint16_t)((r>31u?31u:r)|((g>31u?31u:g)<<5)|((b>31u?31u:b)<<10));
    sNormalCache.valid=1;
    sNormalCache.value=value;
    sNormalCache.rgb=rgb;
    return rgb;
}
/* A vertex's colour from its normal, worked out when its polygon is drawn:
 * most of a big map's polygons are off the screen, and their vertices
 * never need one. The lighting is as it was at the vertex: anything that
 * changes it first works out the colours still owed (settle_lighting). */
static void light_vertex(GeoVertex *v)
{
    if(!v->lit){v->color=shade(v->normal);v->lit=1;}
}
static void settle_lighting(void)
{
    int i,n=s.nverts;
    if(s.primitive==2 && n>=2){
        light_vertex(&s.vertices[s.stripOld]);
        light_vertex(&s.vertices[s.stripNew]);
    }else{
        for(i=0;i<n && i<4;i++)light_vertex(&s.vertices[i]);
    }
    if(s.rgbPending){s.rgb=shade(s.normalValue);s.rgbPending=0;}
}

/* Texture coordinates are stored in signed 1/16-texel units. All texture
 * matrix elements and NORMAL/VTX inputs use signed 20.12 fixed point.
 * Source mode 1 includes two constant 1/16 inputs; modes 2 and 3 replace
 * the bottom matrix row with the most recent raw TEXCOORD (GBATEK). */
static void transform_texcoords(const int32_t *source,int mode,int32_t out[2])
{
    int j;
    for(j=0;j<2;j++){
        int64_t value;
        if(mode==1){
            value=(int64_t)source[0]*s.tex.a[j]+
                  (int64_t)source[1]*s.tex.a[4+j]+
                  (int64_t)s.tex.a[8+j]+s.tex.a[12+j];
            out[j]=saturation(value>>12);
        }else{
            value=(int64_t)source[0]*s.tex.a[j]+
                  (int64_t)source[1]*s.tex.a[4+j]+
                  (int64_t)source[2]*s.tex.a[8+j];
            out[j]=saturation((value>>20)+s.texcoord[j]);
        }
    }
}
static void set_geo_position(uint32_t cmd,const uint32_t *p)
{
    if(cmd==0x23){s.coord[0]=signbits(p[0],16);s.coord[1]=signbits(p[0]>>16,16);s.coord[2]=signbits(p[1],16);}
    else if(cmd==0x24){int i;for(i=0;i<3;i++)s.coord[i]=signbits(p[0]>>(i*10),10)*64;}
    else if(cmd==0x25){s.coord[0]=signbits(p[0],16);s.coord[1]=signbits(p[0]>>16,16);}
    else if(cmd==0x26){s.coord[0]=signbits(p[0],16);s.coord[2]=signbits(p[0]>>16,16);}
    else if(cmd==0x27){s.coord[1]=signbits(p[0],16);s.coord[2]=signbits(p[0]>>16,16);}
    /* VTX_DIFF: 10-bit signed differences in the coordinates' own units
     * (1/4096; GX_VTXDIFF_MAX is 0x1ff as an fx32), not scaled. */
    else if(cmd==0x28){int i;for(i=0;i<3;i++)s.coord[i]+=signbits(p[0]>>(i*10),10);}
}

/* Sutherland-Hodgman clipping in clip space, preserving interpolation
 * attributes. The real GX clips to six homogeneous planes ±x/y/z <= w. */
static int64_t plane_distance(const GeoVertex *v,int plane)
{
    return (int64_t)v->clip[3]+(plane&1?-(int64_t)v->clip[plane>>1]:v->clip[plane>>1]);
}
/* Which clip planes a vertex is outside of, one bit per plane. */
static inline unsigned outcode(const int32_t clip[4])
{
    unsigned bits=0;
    int64_t w=clip[3];
    if(__builtin_expect(clip[3]>0,1)){
        /* (Nearly every vertex: in front of the camera.) w + v < 0 is
         * v < -w, and w - v < 0 is v > w; with 0 < w, -w can't overflow. */
        int32_t w32=clip[3];
        bits=(unsigned)(clip[0]<-w32)|((unsigned)(clip[0]>w32)<<1)|
             ((unsigned)(clip[1]<-w32)<<2)|((unsigned)(clip[1]>w32)<<3)|
             ((unsigned)(clip[2]<-w32)<<4)|((unsigned)(clip[2]>w32)<<5);
        return bits;
    }
    if(w+clip[0]<0)bits|=1u;
    if(w-clip[0]<0)bits|=2u;
    if(w+clip[1]<0)bits|=4u;
    if(w-clip[1]<0)bits|=8u;
    if(w+clip[2]<0)bits|=16u;
    if(w-clip[2]<0)bits|=32u;
    return bits;
}
/* The clip matrix (position x projection), recomputed only when a matrix
 * changed since: geometry lists hold thousands of vertices per matrix
 * change. Vertices go through it in one step, as on the DS. Also writes
 * the clip/vector matrix result registers the SDK reads back (through
 * G3X_GetClipMtx/G3X_GetVectorMtx, which call NitroPort_Gx3dSyncResults
 * first; patches/NitroSDK/0026). */
/* A wider view (the 3DS's wider screen): clip x scaled by sWideNum /
 * sWideDen before clipping, so the screen's width shows more of the
 * world, as a wider camera would; what was on screen stays where it was
 * (the display stretches the picture back by the same amount). The
 * game's own view of its matrices (the result registers, POS_TEST) isn't
 * changed; its box tests are (they then pass what the wider view shows).
 * Only with the DS's full-width viewport: in a narrower one, the view
 * is the game's. */
static void set_viewport(GeoState *g,uint32_t p0)
{
    g->viewport[0]=(int)(p0&255);
    g->viewport[1]=(int)((p0>>8)&255);
    g->viewport[2]=(int)((p0>>16)&255);
    g->viewport[3]=(int)((p0>>24)&255);
    if(g->wideNum!=g->wideDen)g->clipDirty=1;   /* (ensure_clip: the view's width) */
}
static int sWideWidth=256;
/* With the geometry thread: the next SWAP_BUFFERS's time and 3D registers
 * (NitroGx3dScene.stamp, disp3dcnt, regs3d), as the game had them when it
 * sent it: handed over in the ring just before it (OP_SCENE_REGS, from the
 * game's thread: swap_noted), so the thread can't get to the SWAP first. */
static uint32_t sPendingStamp, sPendingDisp3d;
static uint8_t sPendingRegs3d[NITRO_GX3D_REGS_SIZE];
static uint16_t sPendingBgOfs[8];
/* (the scene before's BG scrolls: an in-between scene's are halfway) */
static uint16_t sLastBgOfs[8];
static int sLastBgOfsValid;
static void io_bg_ofs(uint16_t out[8])
{
    int i;
    for(i=0;i<8;i++)out[i]=(uint16_t)((NitroPort_IoReg[0x10+i*2]|(NitroPort_IoReg[0x11+i*2]<<8))&0x1FF);
}
static uint32_t io_disp3dcnt(void)
{
    return (uint32_t)NitroPort_IoReg[0x60]|((uint32_t)NitroPort_IoReg[0x61]<<8)|
           ((uint32_t)NitroPort_IoReg[0x62]<<16)|((uint32_t)NitroPort_IoReg[0x63]<<24);
}
/* The platform's clock for scenes (the 3DS: V-blanks); none by default. */
__attribute__((weak,noinline)) uint32_t NitroPort_Gx3dStamp(void)
{
    return 0;
}
static void set_wide_view(GeoState *g,int screenWidth)
{
    g->wideNum=256;g->wideDen=screenWidth;
    if(screenWidth==256){g->wideNum=1;g->wideDen=1;}
    g->clipDirty=1;
}
static void ensure_clip(GeoState *g)
{
    unsigned j;
    if(!g->clipDirty)return;
    g->clipDirty=0;
    multiply(&g->clip,&g->pos,&g->proj);
    g->xf=g->clip;
    if(g->wideNum!=g->wideDen && ((g->viewport[2]-g->viewport[0]+1)&0x1FF)==256){
        for(j=0;j<4;j++){
            int64_t v=(int64_t)g->clip.a[j*4]*g->wideNum;
            /* rounded to nearest, towards zero at a half */
            v=(v>=0?v+g->wideDen/2:v-g->wideDen/2)/g->wideDen;
            g->xf.a[j*4]=saturation(v);
        }
    }
    for(j=0;j<4;j++)g->xfTrans[j]=(int64_t)g->xf.a[12+j]<<12;
    if(g!=sResults)return;
    for(j=0;j<16;j++)put32(0x640+j*4,(uint32_t)g->clip.a[j]);
    for(j=0;j<9;j++)put32(0x680+j*4,(uint32_t)g->vec.a[(j/3)*4+(j%3)]);
}
static GeoVertex interpolate(const GeoVertex *a,const GeoVertex *b,double t)
{
    GeoVertex v;
    int i;
    memset(&v,0,sizeof(v));   /* no outcode or screen position yet */
    for(i=0;i<4;i++)v.clip[i]=(int32_t)((double)a->clip[i]+
                                              ((double)b->clip[i]-a->clip[i])*t);
    v.s=(int32_t)((double)a->s+((double)b->s-a->s)*t);
    v.t=(int32_t)((double)a->t+((double)b->t-a->t)*t);
    for(i=0;i<3;i++){
        int shift=i*5, av=(a->color>>shift)&31,bv=(b->color>>shift)&31;
        ((void)shift);
        /* Initialize below with whole RGB rather than bitfield writes. */
        if(i==0)v.color=0;
        v.color |= (uint16_t)((int)(av+(bv-av)*t+0.5)<<shift);
    }
    return v;
}
/* floor(), to an int32 (saturated), without a library call. */
static inline int32_t floor_int(double v)
{
    int32_t n;
    if(v>=2147483647.0)return INT32_MAX;
    if(v<=-2147483648.0)return INT32_MIN;
    n=(int32_t)v;
    return n-(v<(double)n);
}
static NitroGx3dVertex to_screen(const GeoVertex *v)
{
    NitroGx3dVertex out;
    double inv=(v->clip[3]>0)?1.0/v->clip[3]:0.0;
    double nz=(double)v->clip[2]*inv;
    int i;
    memset(&out,0,sizeof(out));
    for(i=0;i<4;i++)out.clip[i]=v->clip[i];
    /* The DS's viewport transform: x = (X + W) * width / 2W + x0, and
     * y = (W - Y) * height / 2W + top, top = 191 - the viewport's top line
     * (y counts down; viewport edges are inclusive). Kept to 1/256 pixel
     * rather than cut to whole pixels as the DS does: the 3DS's GPU draws
     * smoother edges from it, and neighbouring models still meet. */
    {
        double x0=s.viewport[0]&255,top=(191-(s.viewport[3]&255))&255;
        double width=(s.viewport[2]-s.viewport[0]+1)&0x1FF;
        double height=(s.viewport[3]-s.viewport[1]+1)&0xFF;
        double w=(double)v->clip[3];
        if(w>0){
            out.x=floor_int(((double)v->clip[0]+w)*width/(2*w)*256+x0*256+0.5);
            out.y=floor_int((w-(double)v->clip[1])*height/(2*w)*256+top*256+0.5);
        }
    }
    if(nz<-1.0)nz=-1.0;
    if(nz>1.0)nz=1.0;
    out.depth=(uint32_t)((nz+1)*8388607.5);
    out.inv_w=(float)inv;
    out.s=v->s/16.0f;out.t=v->t/16.0f;
    out.color=v->color;
    return out;
}
/* Where the geometry went, logged every 120 scenes as a [gx3d] line: why a
 * scene can end up without triangles (all clipped, all culled, ...). */
static struct {
    unsigned scenes, vertices, polygons, triangles;
    unsigned clipped, cullBoth, cullFacing, degenerate, overLimit, behind;
    unsigned projLoads, projMults, projStackOps, stackErrors, swaps;
    unsigned alphaOpaque, alphaTranslucent, alphaWire;
    uint32_t lastAttr;
} sDiag;

static void diag_log(void)
{
    NitroPort_Log("[gx3d] %u scenes: %u vtx, %u polys -> %u tris (%u opaque, %u translucent, %u alpha0); "
                  "lost: %u clipped, %u cull-both, %u culled, %u degenerate, %u over-limit, %u w<=0",
                  sDiag.scenes, sDiag.vertices, sDiag.polygons, sDiag.triangles,
                  sDiag.alphaOpaque, sDiag.alphaTranslucent, sDiag.alphaWire,
                  sDiag.clipped, sDiag.cullBoth, sDiag.cullFacing, sDiag.degenerate,
                  sDiag.overLimit, sDiag.behind);
    NitroPort_Log("[gx3d] projection: %u loads, %u mults, %u push/pop/store/restore, %u stack errors; "
                  "now %ld %ld %ld %ld / %ld %ld %ld %ld / %ld %ld %ld %ld / %ld %ld %ld %ld; "
                  "last attr %08lx, a recent vertex (%ld,%ld,%ld,%ld)",
                  sDiag.projLoads, sDiag.projMults, sDiag.projStackOps, sDiag.stackErrors,
                  (long)s.proj.a[0], (long)s.proj.a[1], (long)s.proj.a[2], (long)s.proj.a[3],
                  (long)s.proj.a[4], (long)s.proj.a[5], (long)s.proj.a[6], (long)s.proj.a[7],
                  (long)s.proj.a[8], (long)s.proj.a[9], (long)s.proj.a[10], (long)s.proj.a[11],
                  (long)s.proj.a[12], (long)s.proj.a[13], (long)s.proj.a[14], (long)s.proj.a[15],
                  (unsigned long)sDiag.lastAttr,
                  (long)s.vertices[0].clip[0], (long)s.vertices[0].clip[1],
                  (long)s.vertices[0].clip[2], (long)s.vertices[0].clip[3]);
    memset(&sDiag, 0, sizeof(sDiag));
}

/* One polygon's triangles, held until the polygon is known to be stored. */
static NitroGx3dTriangle sStaged[16];
static unsigned sStagedCount, sStagedVertices, sStagedClipped;

/* Clip a triangle of the polygon being built to the view volume, cull it,
 * and stage what's left. */
/* Which side of the polygon faces the camera, decided once per polygon
 * from its first three vertices, as the DS does: a quad split into two
 * triangles (or a clipped polygon) is culled as a whole, not per triangle.
 * In clip space (x, y, w), front means det > 0 (the screen's y points
 * down); this works for vertices behind the camera too. */
static int sPolygonFront;
static int polygon_front(const GeoVertex *a,const GeoVertex *b,const GeoVertex *c)
{
    double x0=a->clip[0],y0=a->clip[1],w0=a->clip[3];
    double x1=b->clip[0],y1=b->clip[1],w1=b->clip[3];
    double x2=c->clip[0],y2=c->clip[1],w2=c->clip[3];
    double det=x0*(y1*w2-w1*y2)-y0*(x1*w2-w1*x2)+w0*(x1*y2-y1*x2);
    return det>0;
}
/* The vertex's screen position, worked out once: strips and quads share
 * vertices between their triangles. */
static const NitroGx3dVertex *screen_of(GeoVertex *v)
{
    if(!v->hasScreen){v->screen=to_screen(v);v->hasScreen=1;}
    return &v->screen;
}
/* The sun shadows' own triangles (NitroGx3dScene.shadow_triangles), kept
 * with the shadows on (read at each SWAP_BUFFERS, for the next scene). */
static int sShadowCapture;
/* Their room in the three scenes, made the first time the shadows are on
 * (never freed: kept for the next time). 0 if there's no memory for it:
 * no shadow triangles then. The scene being built has none yet (its
 * SWAP_BUFFERS turned them on), and the others' shadow_count is 0 until
 * they're built again, so the consumer never reads one half set. */
static NitroGx3dTriangle *sShadowStore;
static void shadow_store_attach(void)
{
    unsigned i;
    for(i=0;i<3;i++)
        sScenes[i].shadow_triangles=sShadowStore?sShadowStore+i*NITRO_GX3D_MAX_SHADOW_TRIANGLES:NULL;
}
static int shadow_store(void)
{
    if(!sShadowStore){
        sShadowStore=malloc(3u*NITRO_GX3D_MAX_SHADOW_TRIANGLES*sizeof(*sShadowStore));
        if(!sShadowStore){
            static int told;
            if(!told){told=1;NitroPort_Log("[gx3d] no memory for the sun shadows' triangles");}
            return 0;
        }
        shadow_store_attach();
    }
    return 1;
}
/* How far beyond the view's sides they're kept: x and y up to 4 w (one and
 * a half views beyond each side), in front of the camera and inside the
 * near and far planes. The game's box tests pass what's up to 2 w (half a
 * view beyond each side; test_command): what it draws there is kept
 * whole, the 4 w only to keep a big building's triangles that reach
 * further (a building cut off there cast a shadow cut off along a line,
 * which came and went as the camera moved). */
#define SHADOW_GUARD 4
#define BOX_GUARD 2
static int shadow_reach(const GeoVertex *v)
{
    int64_t w=v->clip[3];
    if(w<=0 || (v->outside & 0x30u))return 0;
    return llabs((long long)v->clip[0])<=SHADOW_GUARD*w && llabs((long long)v->clip[1])<=SHADOW_GUARD*w;
}
/* Keep a whole triangle of the polygon being built for the shadows only:
 * opaque, drawn on some side (not a shadow volume's, mode 3). Its screen
 * positions worked out apart from screen_of's (a strip's vertex may yet
 * be drawn, lit, by a later triangle). */
static void keep_for_shadow(const GeoVertex *a,const GeoVertex *b,const GeoVertex *c)
{
    NitroGx3dScene *scene=dest_scene();
    NitroGx3dTriangle *tri;
    if(!sShadowCapture || !((s.attr>>6)&3u) || ((s.attr>>4)&3u)==3u)return;
    if(((s.attr>>16)&31u)!=31u)return;
    if(!scene->shadow_triangles || scene->shadow_count>=NITRO_GX3D_MAX_SHADOW_TRIANGLES)return;
    if(!shadow_reach(a)||!shadow_reach(b)||!shadow_reach(c))return;
    tri=&scene->shadow_triangles[scene->shadow_count];
    tri->polygon_attr=s.attr;
    tri->texture_param=s.texparam;
    tri->palette_base=s.plttbase;
    tri->sort_key=0;
    if(NitroPort_Gx3dTriangleTranslucent(tri))return;
    tri->v[0]=to_screen(a);tri->v[1]=to_screen(b);tri->v[2]=to_screen(c);
    scene->shadow_count++;
}
/* Whether the polygon being built is culled: facing the side not drawn. */
static int polygon_culled(void)
{
    unsigned mask=(s.attr>>6)&3u;
    return mask && !(mask & (sPolygonFront?2u:1u));
}
/* Cull a screen-space triangle and stage what's left. 1 if it had no area
 * on the screen (edge on). */
static int add_screen_triangle(const NitroGx3dVertex *aa,const NitroGx3dVertex *bb,
                               const NitroGx3dVertex *cc)
{
    NitroGx3dTriangle *tri;
    int64_t cross=(int64_t)(bb->x-aa->x)*(cc->y-aa->y)-
                  (int64_t)(bb->y-aa->y)*(cc->x-aa->x);
    int front=sPolygonFront, mask=(s.attr>>6)&3;
    sDiag.lastAttr=s.attr;
    if(!cross){sDiag.degenerate++;return 1;}
    if(!mask){sDiag.cullBoth++;return 0;}
    if(!(mask & (front?2:1))){sDiag.cullFacing++;return 0;}
    if(sStagedCount>=sizeof(sStaged)/sizeof(sStaged[0]))return 0;
    tri=&sStaged[sStagedCount++];
    tri->v[0]=*aa;tri->v[1]=*bb;tri->v[2]=*cc;
    tri->polygon_attr=s.attr;
    tri->texture_param=s.texparam;
    tri->palette_base=s.plttbase;
    return 0;
}
static void stage_triangle(GeoVertex *a,GeoVertex *b,GeoVertex *c)
{
    GeoVertex v[12],tmp[12];
    unsigned i,p,n=3,clipped=0;
    /* All three outside one plane: nothing is left (but for the shadows,
     * if it's only the view's sides). None outside any: no clipping
     * needed. Most of a map is one or the other. */
    if(a->outside & b->outside & c->outside){
        if(sShadowCapture && !((a->outside & b->outside & c->outside)&0x30u))keep_for_shadow(a,b,c);
        sDiag.clipped++;
        return;
    }
    light_vertex(a);light_vertex(b);light_vertex(c);
    if(!(a->outside | b->outside | c->outside)){
        if(add_screen_triangle(screen_of(a),screen_of(b),screen_of(c)) || polygon_culled())
            keep_for_shadow(a,b,c);
        return;
    }
    /* Crossing the view's sides, or culled: the whole of it for the shadows. */
    if(sShadowCapture && (((a->outside|b->outside|c->outside)&0x0Fu) || polygon_culled()))
        keep_for_shadow(a,b,c);
    v[0]=*a;v[1]=*b;v[2]=*c;
    for(p=0;p<6;p++){
        unsigned m=0;
        /* A plane none of the three is outside of doesn't cut the triangle
         * (nor what's left of it). */
        if(!((a->outside|b->outside|c->outside)&(1u<<p)))continue;
        for(i=0;i<n;i++){
            GeoVertex *left=&v[i],*right=&v[(i+1)%n];
            int64_t da=plane_distance(left,(int)p),db=plane_distance(right,(int)p);
            if(da>=0 && m<12)tmp[m++]=*left;
            if((da<0)!=(db<0)&&m<12){
                double t=(double)da/(double)(da-db);
                tmp[m++]=interpolate(left,right,t);
                clipped=1;
            }
            if(da<0)clipped=1;
        }
        if(m<3){sDiag.clipped++;return;}
        memcpy(v,tmp,m*sizeof(GeoVertex));n=m;
    }
    for(i=1;i+1<n;i++)
        add_screen_triangle(screen_of(&v[0]),screen_of(&v[i]),screen_of(&v[i+1]));
    if(clipped){sStagedClipped=1;sStagedVertices+=n;}
}

static void begin_polygon(void)
{
    sStagedCount=0;sStagedVertices=0;sStagedClipped=0;
    sDiag.polygons++;
}
/* POLYGON_ATTR bit 12 clear: a polygon that crosses the far plane isn't
 * drawn at all (rather than clipped there). Outcode bit 5 is the far
 * plane (w - z < 0). */
static int far_hidden(unsigned outside)
{
    if((outside & 0x20u) && !(s.attr & (1u<<12))){sDiag.clipped++;return 1;}
    return 0;
}
/* Nothing of the polygon is drawn: all its vertices outside the same clip
 * plane (most of a big map), or crossing the far plane (far_hidden).
 * any/all: its vertices' outcodes OR'd/AND'ed. */
static int polygon_hidden(unsigned any,unsigned all)
{
    /* (With the sun shadows on, one outside only the view's sides goes on
     * to stage_triangle, which keeps it for them.) */
    if(all && !(sShadowCapture && !(all&0x30u))){sDiag.clipped++;return 1;}
    return far_hidden(any);
}

/* The DS stores each polygon that survives culling and clipping in its
 * polygon RAM (2048 polygons) and its vertices in vertex RAM (6144); what
 * doesn't fit is dropped. Polygons that are culled or entirely outside the
 * view take no room, which matters for big maps that are mostly off-screen.
 * newVertices: the vertices this polygon adds when it isn't clipped (strips
 * share the previous polygon's). A clipped polygon stores its clipped
 * outline instead. */
static void end_polygon(unsigned newVertices)
{
    NitroGx3dScene *scene=dest_scene();
    unsigned i, vertices;
    if(!sStagedCount)return;
    vertices=sStagedClipped?sStagedVertices:newVertices;
    if(s.framePolygons>=2048u || s.frameVertices+vertices>6144u){
        scene->dropped++;
        sDiag.overLimit++;
        return;
    }
    s.framePolygons++;
    s.frameVertices+=vertices;
    {
        /* The polygon's top and bottom screen lines, for the DS's drawing
         * order (NitroPort_Gx3dDrawOrder). */
        int top=192,bottom=0,k;
        uint32_t key;
        for(i=0;i<sStagedCount;i++)for(k=0;k<3;k++){
            int y=sStaged[i].v[k].y>>8;
            if(y<top)top=y;
            if(y>bottom)bottom=y;
        }
        if(top<0)top=0;
        if(bottom>255)bottom=255;
        key=((uint32_t)bottom<<8)|(uint32_t)top;
        if(NitroPort_Gx3dTriangleTranslucent(&sStaged[0]))key|=1u<<16;
        for(i=0;i<sStagedCount;i++)sStaged[i].sort_key=key;
    }
    for(i=0;i<sStagedCount;i++){
        unsigned a=(sStaged[i].polygon_attr>>16)&31u;
        sDiag.triangles++;
        if(a==31u)sDiag.alphaOpaque++;
        else if(a==0u)sDiag.alphaWire++;
        else sDiag.alphaTranslucent++;
        if(scene->count>=NITRO_GX3D_MAX_TRIANGLES){scene->dropped++;continue;}
        scene->triangles[scene->count++]=sStaged[i];
    }
}

/* ------------------------------------------------ in-between scenes
 * 60 frames a second in the field: the game draws a scene every other
 * V-blank. With the in-between scenes on (NitroPort_Gx3dSetInterpolation),
 * each polygon that isn't wholly outside the view is kept as the game
 * drew it (its vertices in clip space, before clipping and culling: a
 * PolyRecord), and at each SWAP_BUFFERS the new scene's polygons are
 * matched with the scene before's, in order (the game draws the same
 * things in the same order from one scene to the next; a polygon's
 * signature is its kind, not its texture's address or its colour, which
 * animate), and a scene is made of each polygon halfway between its two
 * places, clipped and culled as any other (replay_polygon). A polygon
 * with no match, or one that moved more than a quarter of the screen,
 * is where the new scene has it. Too few matches (a cut, a new map): no
 * in-between scene. */
typedef struct PolyRecord {
    int32_t clip[4][4];      /* the vertices, in the order stage_triangle takes them */
    int32_t st[4][2];
    float px[4], py[4];      /* where they are on the screen, in DS pixels (onScreen) */
    uint16_t color[4];       /* lit */
    uint32_t attr, texparam, plttbase, sig;
    uint8_t count;           /* 3, or 4: triangles (0, 1, 2) and (0, 2, 3) */
    uint8_t onScreen;        /* all its vertices in front of the camera (w > 0) */
} PolyRecord;
#define MAX_RECORDS 4096u
#define MATCH_LOOKAHEAD 48u
/* Pairing a polygon (match_records): among the scene before's polygons of
 * its kind, MATCH_WINDOW either side of where the order says, the one
 * nearest to where it is now less the scene's motion, if nearer than
 * MATCH_NEAR DS pixels (a vertex, on average). */
#define MATCH_WINDOW 64
#define MATCH_NEAR 16.0f
#define MATCH_SURE 1.0f
static int sInterpWanted, sInterp;
static PolyRecord *sRec[2];              /* [sRecBuild]: this scene's; the other: the scene before's */
static unsigned sRecCount[2], sRecBuild;
static int sRecBeforeValid;
static int sReplaying;
/* Each mailbox slot's in-between scene (the one between its scene and the
 * scene before), made before the slot is published: with the scene, the
 * display has its in-between scene too (NitroPort_Gx3dMidOf). */
static NitroGx3dScene *sMids[3];
static int sMidOk[3];
static int32_t *sMatch;                  /* for each of this scene's polygons, the one before's, or -1 */
static unsigned sMidMatched, sMidPolygons;

static uint32_t record_signature(unsigned n)
{
    /* Its kind: the attributes but its alpha (fades), the texture's
     * format, size and repeat/flip but not its address (an animation's
     * frames), and its vertex count. */
    uint32_t h=(s.attr&~(31u<<16))*2654435761u;
    h^=(s.texparam&0xFFFF0000u)+0x9E3779B9u+(h<<6)+(h>>2);
    h^=n*0x85EBCA6Bu;
    return h;
}
static void record_polygon(GeoVertex *a,GeoVertex *b,GeoVertex *c,GeoVertex *d)
{
    GeoVertex *v[4];
    PolyRecord *r;
    unsigned k,n=d?4u:3u;
    if(!sInterp || sReplaying || !sRec[sRecBuild] || sRecCount[sRecBuild]>=MAX_RECORDS)return;
    v[0]=a;v[1]=b;v[2]=c;v[3]=d;
    r=&sRec[sRecBuild][sRecCount[sRecBuild]++];
    for(k=0;k<n;k++){
        memcpy(r->clip[k],v[k]->clip,sizeof(r->clip[k]));
        r->st[k][0]=v[k]->s;r->st[k][1]=v[k]->t;
        r->color[k]=v[k]->lit?v[k]->color:shade(v[k]->normal);
    }
    r->count=(uint8_t)n;
    r->attr=s.attr;r->texparam=s.texparam;r->plttbase=s.plttbase;
    r->sig=record_signature(n);
    r->onScreen=1;
    for(k=0;k<n;k++){
        if(r->clip[k][3]<=0){r->onScreen=0;break;}
        r->px[k]=(float)r->clip[k][0]/(float)r->clip[k][3]*128.0f;
        r->py[k]=(float)r->clip[k][1]/(float)r->clip[k][3]*96.0f;
    }
}
/* A polygon as the game would have drawn it with these vertices (clip),
 * into dest_scene(): clipped, culled, its triangles staged and stored. */
static void replay_polygon(const PolyRecord *r,int32_t clip[4][4])
{
    GeoVertex v[4];
    unsigned k,any=0,all=0x3Fu;
    uint32_t attr=s.attr,texparam=s.texparam,plttbase=s.plttbase;
    s.attr=r->attr;s.texparam=r->texparam;s.plttbase=r->plttbase;
    for(k=0;k<r->count;k++){
        memset(&v[k],0,sizeof(v[k]));
        memcpy(v[k].clip,clip[k],sizeof(v[k].clip));
        v[k].s=r->st[k][0];v[k].t=r->st[k][1];
        v[k].color=r->color[k];v[k].lit=1;
        v[k].outside=(uint8_t)outcode(v[k].clip);
        any|=v[k].outside;all&=v[k].outside;
    }
    begin_polygon();
    if(!polygon_hidden(any,all)){
        sPolygonFront=polygon_front(&v[0],&v[1],&v[2]);
        stage_triangle(&v[0],&v[1],&v[2]);
        if(r->count==4)stage_triangle(&v[0],&v[2],&v[3]);
        end_polygon(1);   /* (the DS's vertex RAM: as the scene's own, roughly) */
    }
    s.attr=attr;s.texparam=texparam;s.plttbase=plttbase;
}
/* Where a vertex is on the screen, -1..1 (w > 0). */
static int ndc_of(const int32_t clip[4],double *x,double *y)
{
    if(clip[3]<=0)return 0;
    *x=(double)clip[0]/clip[3];*y=(double)clip[1]/clip[3];
    return 1;
}
/* Pair this scene's polygons (C, n) with the scene before's (P, m).
 * First in order (the game draws the same things in the same order),
 * skipping what's in one but not the other (the nearest place the two
 * agree again, MATCH_LOOKAHEAD ahead at most): the scene's motion is the
 * middle of those pairs' moves (most of a scene moves with the camera).
 * Then each polygon again, with the polygon of its kind nearest to where
 * it is less that motion, MATCH_WINDOW either side of where the order
 * says: a map's many tiles of one kind are told apart by where they are,
 * not by their order (which shifts as tiles come into the view and leave
 * it). out[j]: P's index, or -1. The number paired. */
static float median_of(float *v,unsigned n)
{
    /* (selection: Hoare's, to the middle; v is reordered) */
    unsigned lo=0,hi=n-1u,mid=n/2u;
    while(lo<hi){
        float pivot=v[(lo+hi)/2u];
        unsigned i=lo,j=hi;
        while(i<=j){
            while(v[i]<pivot)i++;
            while(v[j]>pivot)j--;
            if(i<=j){
                float t=v[i];v[i]=v[j];v[j]=t;
                i++;
                if(j==0)break;
                j--;
            }
        }
        if(mid<=j)hi=j;
        else if(mid>=i)lo=i;
        else break;
    }
    return v[mid];
}
/* (limit: the average past which the caller doesn't care; it stops
 * there, returning at least the limit) */
static float poly_distance(const PolyRecord *p,const PolyRecord *c,float dx,float dy,float limit)
{
    float d=0.0f,stop=limit*(float)c->count;
    unsigned k;
    for(k=0;k<c->count;k++){
        d+=fabsf(c->px[k]-(p->px[k]+dx))+fabsf(c->py[k]-(p->py[k]+dy));
        if(d>=stop)return limit;
    }
    return d/(float)c->count;
}
/* The scene's motion as match_records last worked it out (DS pixels, as
 * PolyRecord.px/py: x / w * 128, y / w * 96); valid with 8 pairs or more. */
static int sMotionValid;
static uint8_t *sPUsed;   /* (the scene before's records paired with one of this one's) */
static float sMotionX, sMotionY;
static unsigned match_records(const PolyRecord *P,unsigned m,const PolyRecord *C,unsigned n,int32_t *out)
{
    unsigned i=0,j=0,matched=0,moves=0;
    float dx=0.0f,dy=0.0f,near;
    static float mx[256],my[256];
    int offset=0;
    /* in order */
    while(j<n){
        unsigned k,skipP=0,skipC=0;
        if(i<m && P[i].sig==C[j].sig){out[j++]=(int32_t)i++;continue;}
        for(k=1;k<=MATCH_LOOKAHEAD;k++){
            if(i+k<m && P[i+k].sig==C[j].sig){skipP=k;break;}
            if(j+k<n && i<m && C[j+k].sig==P[i].sig){skipC=k;break;}
        }
        if(skipP){i+=skipP;continue;}
        if(skipC){
            while(skipC--)out[j++]=-1;
            continue;
        }
        out[j++]=-1;
        if(i<m)i++;
    }
    /* the scene's motion: the middle of the pairs' moves (some of them) */
    for(j=0;j<n && moves<256u;j+=1u+n/256u){
        if(out[j]<0 || !P[out[j]].onScreen || !C[j].onScreen)continue;
        mx[moves]=C[j].px[0]-P[out[j]].px[0];
        my[moves]=C[j].py[0]-P[out[j]].py[0];
        moves++;
    }
    if(moves>=8u){dx=median_of(mx,moves);dy=median_of(my,moves);}
    sMotionValid=moves>=8u;
    sMotionX=dx;sMotionY=dy;
    /* (a figure that moves on its own, the player against the camera's
     * motion: as far again as the scene moves) */
    near=MATCH_NEAR+1.5f*(fabsf(dx)+fabsf(dy));
    /* each again, by where it is */
    for(j=0;j<n;j++){
        int center=out[j]>=0?out[j]:(int)j+offset,order=out[j],lo,hi,q,best=-1;
        float bestD=near;
        if(!C[j].onScreen){
            /* (behind the camera in part: in order, or not at all) */
            if(out[j]>=0 && P[out[j]].onScreen)out[j]=-1;
            if(out[j]>=0)matched++;
            continue;
        }
        /* The pair the order gave, where the scene's motion puts it (a
         * pixel at most): that one, without looking further. (Most are:
         * the window's search, every polygon of a kind against each
         * other, took the in-between scene longer than the scene itself
         * where a model is many polygons of one kind, the starters'
         * briefcase: 2026-10-26.) */
        if(out[j]>=0 && P[out[j]].onScreen && P[out[j]].count==C[j].count &&
           poly_distance(&P[out[j]],&C[j],dx,dy,MATCH_SURE)<MATCH_SURE){
            offset=out[j]-(int)j;matched++;
            continue;
        }
        lo=center-MATCH_WINDOW;hi=center+MATCH_WINDOW;
        if(lo<0)lo=0;
        if(hi>(int)m-1)hi=(int)m-1;
        for(q=lo;q<=hi;q++){
            float d;
            if(P[q].sig!=C[j].sig || !P[q].onScreen || P[q].count!=C[j].count)continue;
            d=poly_distance(&P[q],&C[j],dx,dy,bestD);
            if(d<bestD){bestD=d;best=q;}
        }
        /* What the camera follows stays where it was on the screen as the
         * scene moves: the player, and one who follows him. The order's
         * pair where it was (a pixel at most), when that fits better than
         * anything the scene's motion explains: the scene moving further
         * than half the gap between the two (at a run, with a scene left
         * out), Cheryl behind the player in Eterna Forest took his place
         * from the scene before, and jumped about (2026-10-27). */
        if(order>=0 && order!=best && P[order].onScreen && P[order].count==C[j].count){
            float still=poly_distance(&P[order],&C[j],0.0f,0.0f,MATCH_SURE);
            if(still<MATCH_SURE && (best<0 || still+MATCH_SURE<bestD))best=order;
        }
        out[j]=best;
        if(best>=0){offset=best-(int)j;matched++;}
    }
    return matched;
}
/* The in-between scene of the scene just finished (`now', its records
 * sRec[sRecBuild]) and the one before (sRec[sRecBuild ^ 1]), into sMid.
 * Whether there is one. */
static int make_mid_scene(const NitroGx3dScene *now,NitroGx3dScene *sMid)
{
    const PolyRecord *P=sRec[sRecBuild^1u],*C=sRec[sRecBuild];
    unsigned m=sRecCount[sRecBuild^1u],n=sRecCount[sRecBuild],j,k,used=0;
    unsigned framePolygons=s.framePolygons,frameVertices=s.frameVertices;
    int capture=sShadowCapture;
    if(!sMid || !sMatch || !sRecBeforeValid || !n){
        sMidMatched=0;sMidPolygons=n;
        return 0;
    }
    sMidMatched=match_records(P,m,C,n,sMatch);
    sMidPolygons=n;
    if(n>=16u && sMidMatched*2u<n)return 0;   /* (a cut: too little in common) */
    sMid->count=0;sMid->dropped=0;sMid->shadow_count=0;sMid->shadow_triangles=NULL;
    sMid->serial=now->serial;sMid->swap_param=now->swap_param;
    sMid->screen_width=now->screen_width;
    memcpy(sMid->projection,now->projection,sizeof(sMid->projection));
    sMid->stamp=now->stamp;
    sMid->disp3dcnt=now->disp3dcnt;
    memcpy(sMid->regs3d,now->regs3d,sizeof(sMid->regs3d));
    {
        /* (the BGs' scrolls halfway, the shorter way round a 512 map;
         * a jump of more than a quarter of it: the new one) */
        int i;
        for(i=0;i<8;i++){
            int a=sLastBgOfsValid?sLastBgOfs[i]:now->bg_ofs[i],d=((int)now->bg_ofs[i]-a)&0x1FF;
            if(d>=256)d-=512;
            sMid->bg_ofs[i]=(uint16_t)((d>128||d<-128)?now->bg_ofs[i]:((a+d/2)&0x1FF));
        }
    }
    {
        /* (replaying: no shadow triangles, no counts in the [gx3d] lines,
         * the DS's limits counted afresh) */
        __typeof__(sDiag) diag=sDiag;
        sDest=sMid;sReplaying=1;sShadowCapture=0;
        s.framePolygons=0;s.frameVertices=0;
        for(j=0;j<n;j++){
            int32_t clip[4][4];
            const PolyRecord *c=&C[j];
            int mid=0;
            if(sMatch[j]>=0){
                const PolyRecord *p=&P[sMatch[j]];
                double x0,y0,x1,y1;
                mid=p->count==c->count;
                /* (moved more than a quarter of the screen: not the same
                 * thing, or a jump; where it is now) */
                if(mid && ndc_of(p->clip[0],&x0,&y0) && ndc_of(c->clip[0],&x1,&y1) &&
                   (fabs(x1-x0)>0.5 || fabs(y1-y0)>0.5))mid=0;
                if(mid){
                    for(k=0;k<c->count;k++){
                        int q;
                        for(q=0;q<4;q++)clip[k][q]=(int32_t)(((int64_t)p->clip[k][q]+c->clip[k][q])/2);
                    }
                    used++;
                }
            }
            if(!mid)memcpy(clip,c->clip,sizeof(clip));
            if(sMatch[j]<0 && c->onScreen && sMotionValid && (sMotionX!=0.0f || sMotionY!=0.0f)){
                /* (one with no match: the scene's new ones, mostly, as a
                 * model comes into the view whole: where the scene's
                 * motion puts it halfway, not where it is now, half a
                 * step ahead of the rest: a building's roof coming in at
                 * the top jumped for a frame, 2026-10-21) */
                double hx=-0.5*(double)sMotionX/128.0,hy=-0.5*(double)sMotionY/96.0;
                for(k=0;k<c->count;k++){
                    clip[k][0]+=(int32_t)floor(hx*(double)clip[k][3]+0.5);
                    clip[k][1]+=(int32_t)floor(hy*(double)clip[k][3]+0.5);
                }
            }
            replay_polygon(c,clip);
        }
        if(sMotionValid && (sMotionX!=0.0f || sMotionY!=0.0f) && sPUsed){
            /* The scene before's that the scene's motion takes out of the
             * view (none of this one's: they've gone): halfway out, not
             * gone half a step early (a strip at the screen's edge showed
             * what's behind for a frame). */
            double fx=(double)sMotionX/128.0,fy=(double)sMotionY/96.0;
            unsigned i;
            memset(sPUsed,0,m);
            for(j=0;j<n;j++)if(sMatch[j]>=0)sPUsed[sMatch[j]]=1;
            for(i=0;i<m;i++){
                const PolyRecord *p=&P[i];
                int32_t clip[4][4],full[4];
                unsigned all=0x3Fu;
                if(sPUsed[i] || !p->onScreen)continue;
                for(k=0;k<p->count;k++){
                    memcpy(full,p->clip[k],sizeof(full));
                    full[0]+=(int32_t)floor(fx*(double)full[3]+0.5);
                    full[1]+=(int32_t)floor(fy*(double)full[3]+0.5);
                    all&=outcode(full);
                }
                if(!all)continue;   /* (not leaving the view: gone, as this scene has it) */
                for(k=0;k<p->count;k++){
                    memcpy(clip[k],p->clip[k],sizeof(clip[k]));
                    clip[k][0]+=(int32_t)floor(0.5*fx*(double)clip[k][3]+0.5);
                    clip[k][1]+=(int32_t)floor(0.5*fy*(double)clip[k][3]+0.5);
                }
                replay_polygon(p,clip);
            }
        }
        sDest=NULL;sReplaying=0;sShadowCapture=capture;
        s.framePolygons=framePolygons;s.frameVertices=frameVertices;
        sDiag=diag;
    }
    sMidMatched=used;
    return 1;
}
/* At a SWAP_BUFFERS, before its scene (the building slot) is published:
 * its in-between scene. */
static void interp_mid(const NitroGx3dScene *now)
{
    sMidOk[sBuilding]=sInterp && make_mid_scene(now,sMids[sBuilding]);
}
/* At a SWAP_BUFFERS, after its scene is published: this scene's records
 * become the scene before's. */
static void interp_swap(void)
{
    int wanted=__atomic_load_n(&sInterpWanted,__ATOMIC_ACQUIRE),i;
    if(wanted && (!sRec[0] || !sMids[2])){
        if(!sRec[0])sRec[0]=malloc(MAX_RECORDS*sizeof(PolyRecord));
        if(!sRec[1])sRec[1]=malloc(MAX_RECORDS*sizeof(PolyRecord));
        if(!sMatch)sMatch=malloc(MAX_RECORDS*sizeof(int32_t));
        if(!sPUsed)sPUsed=malloc(MAX_RECORDS);
        for(i=0;i<3;i++)if(!sMids[i])sMids[i]=malloc(sizeof(NitroGx3dScene));
        if(!sRec[0]||!sRec[1]||!sMatch||!sMids[0]||!sMids[1]||!sMids[2]){
            static int told;
            if(!told){told=1;NitroPort_Log("[gx3d] no memory for the in-between scenes");}
            __atomic_store_n(&sInterpWanted,0,__ATOMIC_RELEASE);
            wanted=0;
        }
    }
    if(sInterp && wanted)sRecBeforeValid=1;
    else sRecBeforeValid=0;
    __atomic_store_n(&sInterp,wanted,__ATOMIC_RELEASE);
    sRecBuild^=1u;
    sRecCount[sRecBuild]=0;
}

void NitroPort_Gx3dSetInterpolation(int on)
{
    __atomic_store_n(&sInterpWanted,on!=0,__ATOMIC_RELEASE);
}
int NitroPort_Gx3dInterpolation(void)
{
    return __atomic_load_n(&sInterp,__ATOMIC_ACQUIRE);
}
const NitroGx3dScene *NitroPort_Gx3dMidScene(unsigned *matched,unsigned *polygons)
{
    if(matched)*matched=sMidMatched;
    if(polygons)*polygons=sMidPolygons;
    return sMidOk[sDisplayed]?sMids[sDisplayed]:NULL;
}
const NitroGx3dScene *NitroPort_Gx3dMidOf(const NitroGx3dScene *scene)
{
    int i;
    for(i=0;i<3;i++)if(scene==&sScenes[i])return sMidOk[i]?sMids[i]:NULL;
    return NULL;
}

static void submit_vertex(void)
{
    GeoVertex *vp;
    int32_t src[4];
    int n=s.nverts;
    if(!s.inBegin)return;
    /* Built where it's kept (no copying): a triangle strip's third and
     * later vertices go where the one before the last two was. */
    vp=(s.primitive==2 && n>=2)?&s.vertices[3-s.stripOld-s.stripNew]:&s.vertices[n];
    src[0]=s.coord[0];src[1]=s.coord[1];src[2]=s.coord[2];src[3]=4096;
    ensure_clip(&s);
    {
        /* transform() of the point (w = 1.0) through the clip matrix,
         * its row 3 shifted already (ensure_clip). */
        int64_t x=src[0],y=src[1],z=src[2];
        int j;
        for(j=0;j<4;j++)
            vp->clip[j]=saturation((x*s.xf.a[j]+y*s.xf.a[4+j]+z*s.xf.a[8+j]+s.xfTrans[j])>>12);
    }
    vp->outside=(uint8_t)outcode(vp->clip);
    vp->hasScreen=0;
    sDiag.vertices++;
    if(vp->clip[3]<=0)sDiag.behind++;
    if(((s.texparam>>30)&3u)==3u)transform_texcoords(src,3,s.resolvedTexcoord);
    vp->s=s.resolvedTexcoord[0];vp->t=s.resolvedTexcoord[1];vp->color=s.rgb;
    vp->lit=(uint8_t)!s.rgbPending;vp->normal=s.normalValue;
    if(s.primitive==0){
        n++;
        if(n==3){
            begin_polygon();
            if(!polygon_hidden(s.vertices[0].outside|s.vertices[1].outside|s.vertices[2].outside,
                               s.vertices[0].outside&s.vertices[1].outside&s.vertices[2].outside)){
                sPolygonFront=polygon_front(&s.vertices[0],&s.vertices[1],&s.vertices[2]);
                stage_triangle(&s.vertices[0],&s.vertices[1],&s.vertices[2]);
                end_polygon(3);
                record_polygon(&s.vertices[0],&s.vertices[1],&s.vertices[2],NULL);
            }
            n=0;
        }
    }else if(s.primitive==1){
        n++;
        if(n==4){
            begin_polygon();
            if(!polygon_hidden(s.vertices[0].outside|s.vertices[1].outside|
                               s.vertices[2].outside|s.vertices[3].outside,
                               s.vertices[0].outside&s.vertices[1].outside&
                               s.vertices[2].outside&s.vertices[3].outside)){
                sPolygonFront=polygon_front(&s.vertices[0],&s.vertices[1],&s.vertices[2]);
                stage_triangle(&s.vertices[0],&s.vertices[1],&s.vertices[2]);
                stage_triangle(&s.vertices[0],&s.vertices[2],&s.vertices[3]);
                end_polygon(4);
                record_polygon(&s.vertices[0],&s.vertices[1],&s.vertices[2],&s.vertices[3]);
            }
            n=0;
        }
    }else if(s.primitive==2){
        /* Triangle strip: vertices[stripOld] the older of the last two,
         * [stripNew] the newer. GBATEK: v0 v1 v2, v2 v1 v3, v2 v3 v4,
         * v4 v3 v5, ...: every other triangle has its first two vertices
         * the other way round. */
        if(n<2){
            n++;
            s.stripOld=0;s.stripNew=1;
        }else{
            /* The two before stay where they are; the new one goes in the
             * third place (no copying), and becomes the newer. */
            GeoVertex *older=&s.vertices[s.stripOld],*newer=&s.vertices[s.stripNew];
            GeoVertex *first=(n&1)?newer:older;
            GeoVertex *second=(n&1)?older:newer;
            begin_polygon();
            if(!polygon_hidden(first->outside|second->outside|vp->outside,
                               first->outside&second->outside&vp->outside)){
                sPolygonFront=polygon_front(first,second,vp);
                stage_triangle(first,second,vp);
                end_polygon(n==2?3:1);
                record_polygon(first,second,vp,NULL);
            }
            s.stripOld=s.stripNew;s.stripNew=(int)(vp-s.vertices);n++;
        }
    }else{
        n++;
        if(n==4){
            begin_polygon();
            if(!polygon_hidden(s.vertices[0].outside|s.vertices[1].outside|
                               s.vertices[2].outside|s.vertices[3].outside,
                               s.vertices[0].outside&s.vertices[1].outside&
                               s.vertices[2].outside&s.vertices[3].outside)){
                sPolygonFront=polygon_front(&s.vertices[0],&s.vertices[1],&s.vertices[3]);
                stage_triangle(&s.vertices[0],&s.vertices[1],&s.vertices[3]);
                stage_triangle(&s.vertices[0],&s.vertices[3],&s.vertices[2]);
                /* The first quad of a strip brings 4 vertices, later ones 2. */
                end_polygon(s.stripStarted?2:4);
                record_polygon(&s.vertices[0],&s.vertices[1],&s.vertices[3],&s.vertices[2]);
            }
            s.stripStarted=1;
            s.vertices[0]=s.vertices[2];s.vertices[1]=s.vertices[3];n=2;
        }
    }
    s.nverts=n;
}
/* How many parameter words each command takes. */
static const uint8_t sParamCount[256]={
    [0x10]=1,[0x12]=1,[0x13]=1,[0x14]=1,
    [0x16]=16,[0x17]=12,[0x18]=16,[0x19]=12,[0x1a]=9,[0x1b]=3,[0x1c]=3,
    [0x20]=1,[0x21]=1,[0x22]=1,[0x23]=2,[0x24]=1,[0x25]=1,[0x26]=1,
    [0x27]=1,[0x28]=1,[0x29]=1,[0x2a]=1,[0x2b]=1,
    [0x30]=1,[0x31]=1,[0x32]=1,[0x33]=1,[0x34]=32,
    [0x40]=1,[0x50]=1,[0x60]=1,[0x70]=3,[0x71]=2,[0x72]=1,
};
static int param_count(unsigned op)
{
    return sParamCount[op&255u];
}
/* Matrix commands (0x10-0x1c) on an engine's matrices: the engine's own
 * (s) or the game thread's shadow of them (sShadow). */
static void matrix_command(GeoState *g,unsigned op,const uint32_t *p)
{
    Matrix m;
    switch(op){
    case 0x10:g->mode=p[0]&3;return;
    case 0x11:case 0x12:case 0x13:case 0x14:
        matrix_stack(g,op,op==0x11?0:(int32_t)p[0]);break;
    case 0x15:identity(&m);set_matrix(g,&m,0);break;
    case 0x16:case 0x17:m=read_matrix(op,p);set_matrix(g,&m,0);break;
    case 0x18:case 0x19:case 0x1a:m=read_matrix(op,p);mul_matrix(g,&m,0);break;
    /* Scale (never the vector matrix) and translation: only the rows they
     * change worked out. */
    case 0x1b:scale_rows(selected(g),p);break;
    case 0x1c:translate_rows(selected(g),p);
        if(g->mode==2)translate_rows(&g->vec,p);
        break;
    default:return;
    }
    /* The clip matrix is worked out again when a vertex, a test or the SDK
     * (NitroPort_Gx3dSyncResults) next needs it: models change matrices
     * several times between vertices. */
    g->clipDirty=1;
}
/* GXSTAT from an engine's state: the box test result, the stack levels
 * and overflow. */
static void refresh_status(const GeoState *g)
{
    uint32_t status=io32(0x600);
    status &= ~((3u<<0)|(31u<<8)|(1u<<13)|(1u<<14)|(1u<<15)|
                (511u<<16)|(1u<<25)|(1u<<26)|(1u<<27));
    status |= ((uint32_t)g->boxVisible<<1) |
              ((uint32_t)(g->pvLevel&31)<<8) |
              ((uint32_t)(g->pjLevel&1)<<13)|
              ((uint32_t)(g->stackError!=0)<<15)|
              (1u<<25)|(1u<<26);
    put32(0x600,status);
}
/* BOX_TEST, POS_TEST and VEC_TEST (0x70-0x72): results in the registers. */
static void test_command(GeoState *g,unsigned op,const uint32_t *p)
{
    int i;
    switch(op){
    case 0x70: {
        /* 6 packed 4.12 coordinates: XYZ and size XYZ. A box is invisible
         * only when all eight corners are outside the same clip plane.
         * This is conservative for frustum edges, as the DS box test is. */
        int32_t origin[3], size[3], clip[4];
        unsigned mask, all=0x3fu;
        origin[0]=signbits(p[0],16);
        origin[1]=signbits(p[0]>>16,16);
        origin[2]=signbits(p[1],16);
        size[0]=signbits(p[1]>>16,16);
        size[1]=signbits(p[2],16);
        size[2]=signbits(p[2]>>16,16);
        ensure_clip(g);
        {
            /* A corner's sum before the shift is the origin's plus the
             * edges' it adds (exact, in 64 bits): 8 corners for the cost
             * of 4 transforms. */
            int64_t base[4],edge[3][4];
            int j;
            for(j=0;j<4;j++){
                base[j]=(int64_t)origin[0]*g->xf.a[j]+(int64_t)origin[1]*g->xf.a[4+j]+
                        (int64_t)origin[2]*g->xf.a[8+j]+g->xfTrans[j];
                for(i=0;i<3;i++)edge[i][j]=(int64_t)size[i]*g->xf.a[i*4+j];
            }
            for(mask=0;mask<8;mask++){
                for(j=0;j<4;j++){
                    int64_t v=base[j];
                    for(i=0;i<3;i++)if((mask>>i)&1)v+=edge[i][j];
                    clip[j]=saturation(v>>12);
                }
                all&=outcode(clip);
            }
        }
        if(all && !(all&0x30u) && NitroPort_SunShadows()){
            /* Outside only the view's sides: in reach of the shadows? The
             * box's corners against x and y of BOX_GUARD w instead. */
            int64_t base[4],edge[3][4];
            int j;
            unsigned wide=0x0Fu;
            for(j=0;j<4;j++){
                base[j]=(int64_t)origin[0]*g->xf.a[j]+(int64_t)origin[1]*g->xf.a[4+j]+
                        (int64_t)origin[2]*g->xf.a[8+j]+g->xfTrans[j];
                for(i=0;i<3;i++)edge[i][j]=(int64_t)size[i]*g->xf.a[i*4+j];
            }
            for(mask=0;mask<8;mask++){
                int64_t c[4];
                for(j=0;j<4;j++){
                    int64_t v=base[j];
                    for(i=0;i<3;i++)if((mask>>i)&1)v+=edge[i][j];
                    c[j]=v>>12;
                }
                wide&=(unsigned)(c[0]<-BOX_GUARD*c[3])|((unsigned)(c[0]>BOX_GUARD*c[3])<<1)|
                      ((unsigned)(c[1]<-BOX_GUARD*c[3])<<2)|((unsigned)(c[1]>BOX_GUARD*c[3])<<3);
            }
            if(!wide)all=0;
        }
        g->boxVisible=all?0:1;
        refresh_status(g);
        break;
    }
    case 0x71:{
        int32_t src[4],result[4];
        src[0]=signbits(p[0],16);src[1]=signbits(p[0]>>16,16);
        src[2]=signbits(p[1],16);src[3]=4096;
        ensure_clip(g);
        transform(src,&g->clip,result);
        for(i=0;i<4;i++)put32(0x620+i*4,(uint32_t)result[i]);
        break;
    }
    case 0x72:{
        int32_t src[4],res[4];
        for(i=0;i<3;i++)src[i]=signbits(p[0]>>(i*10),10)*8;
        src[3]=0;transform(src,&g->vec,res);
        for(i=0;i<3;i++){
            /* Both the vector input/output are 4.12; no extra >>3. */
            uint16_t val=(uint16_t)res[i];
            memcpy(NitroPort_IoReg+0x630+i*2,&val,sizeof(val));
        }
        break;
    }
    default:break;
    }
}
/* Not a DS command: GXSTAT's stack overflow cleared
 * (NitroPort_Gx3dClearStackError), in the command stream's order. */
#define OP_CLEAR_STACK_ERROR 0xF0u
#define OP_WIDE_VIEW 0xF2u   /* one parameter: the screen width (NitroPort_Gx3dSetWideView) */
/* The next SWAP_BUFFERS's time, DISP3DCNT and 3D registers (swap_noted) */
#define OP_SCENE_REGS 0xF3u
#define SCENE_REGS_WORDS (2u+NITRO_GX3D_REGS_SIZE/4u+4u)

/* The whole engine (s): one command and its parameters. */
static inline __attribute__((always_inline)) void execute(unsigned op,const uint32_t *p)
{
    int i;
    if(op>=0x10u && op<=0x1cu){
        int before=s.stackError;
        if(s.mode==0){
            if(op>=0x11u && op<=0x14u)sDiag.projStackOps++;
            else if(op==0x15u||op==0x16u||op==0x17u)sDiag.projLoads++;
            else if(op>=0x18u && op<=0x1au)sDiag.projMults++;
        }
        /* The vector matrix (lighting's) changes: colours owed first. */
        if((s.mode==1||s.mode==2) && op!=0x10u && op!=0x11u && op!=0x13u)settle_lighting();
        matrix_command(&s,op,p);
        sNormalCache.valid=0;
        if(s.stackError&&!before)sDiag.stackErrors++;
        /* Stack commands change GXSTAT (the shadow's, with a thread). */
        if(op>=0x11u && op<=0x14u && sResults==&s)refresh_status(&s);
        return;
    }
    switch(op){
    case 0x20:s.rgb=p[0]&0x7FFF;s.rgbPending=0;break;
    case 0x21:
        for(i=0;i<3;i++)s.normal[i]=signbits(p[0]>>(i*10),10)*8;
        s.normalValue=p[0];s.rgbPending=1;
        if(((s.texparam>>30)&3u)==2u)transform_texcoords(s.normal,2,s.resolvedTexcoord);
        break;
    case 0x22:{
        int mode=(int)((s.texparam>>30)&3u);
        s.texcoord[0]=signbits(p[0],16);
        s.texcoord[1]=signbits(p[0]>>16,16);
        if(mode==1)transform_texcoords(s.texcoord,1,s.resolvedTexcoord);
        else if(mode==0){
            s.resolvedTexcoord[0]=s.texcoord[0];
            s.resolvedTexcoord[1]=s.texcoord[1];
        }
        break;
    }
    case 0x23:case 0x24:case 0x25:case 0x26:case 0x27:case 0x28:
        set_geo_position(op,p);submit_vertex();break;
    case 0x29:s.pendingAttr=p[0];break;
    case 0x2a:s.texparam=p[0];break;
    case 0x2b:s.plttbase=p[0];break;
    case 0x30:settle_lighting();
        s.diffuse=p[0]&0x7FFF;s.ambient=(p[0]>>16)&0x7FFF;sLightCache.dirty=1;sNormalCache.valid=0;
        if(p[0]&(1u<<15))s.rgb=s.diffuse;
        break;
    case 0x31:settle_lighting();
        s.specular=p[0]&0x7FFF;s.emission=(p[0]>>16)&0x7FFF;sLightCache.dirty=1;sNormalCache.valid=0;
        s.useShininess=(p[0]&(1u<<15))!=0;break;
    case 0x32:{
        int id=(p[0]>>30)&3;
        int32_t incoming[4],transformed[4];
        settle_lighting();
        for(i=0;i<3;i++)incoming[i]=signbits(p[0]>>(i*10),10)*8;
        incoming[3]=0;
        /* LIGHT_VECTOR samples the directional matrix at command time. */
        transform(incoming,&s.vec,transformed);
        for(i=0;i<3;i++){
            s.lightDir[id][i]=transformed[i];
            s.lightHalf[id][i]=(transformed[i]+(i==2?-4096:0))/2;
        }
        sNormalCache.valid=0;
        break;
    }
    case 0x33:settle_lighting();s.lightColor[(p[0]>>30)&3]=p[0]&0x7FFF;sLightCache.dirty=1;sNormalCache.valid=0;break;
    case 0x34:settle_lighting();memcpy(s.shininess,p,sizeof(s.shininess));sNormalCache.valid=0;break;
    case 0x40:settle_lighting();s.primitive=p[0]&3;s.nverts=0;s.inBegin=1;s.stripStarted=0;s.attr=s.pendingAttr;
        sNormalCache.valid=0;break;
    case 0x41:s.inBegin=0;s.nverts=0;break;
    case 0x50:{
        int next=-1;
        uint32_t serial=0;
        NitroGx3dScene *scene=&sScenes[sBuilding];
        /* All vertex and attribute writes precede the release publication. */
        scene->swap_param=p[0];
        scene->serial=sSerial+1;
        scene->screen_width=(uint32_t)(s.wideNum==s.wideDen?256:s.wideDen);
        memcpy(scene->projection,s.proj.a,sizeof(scene->projection));
        /* When the game ended it (with the thread: noted on the game's
         * thread as the SWAP went in; swap_noted). */
        /* (and its registers: as the game had them then) */
        if(sThreaded){
            scene->stamp=sPendingStamp;
            scene->disp3dcnt=sPendingDisp3d;
            memcpy(scene->regs3d,sPendingRegs3d,NITRO_GX3D_REGS_SIZE);
            memcpy(scene->bg_ofs,sPendingBgOfs,sizeof(scene->bg_ofs));
        }else{
            scene->stamp=NitroPort_Gx3dStamp();
            scene->disp3dcnt=io_disp3dcnt();
            memcpy(scene->regs3d,NitroPort_IoReg+NITRO_GX3D_REGS_AT,NITRO_GX3D_REGS_SIZE);
            io_bg_ofs(scene->bg_ofs);
        }
        /* Which SWAP_BUFFERS it is (with the geometry thread, counting
         * them as the game's thread does: NitroPort_Gx3dEarlierSceneUnread). */
        NitroPort_MailboxTag(&sMailbox,sSwapsDone+1u);
        interp_mid(scene);   /* (published with it) */
        memcpy(sLastBgOfs,scene->bg_ofs,sizeof(sLastBgOfs));
        sLastBgOfsValid=1;
        if(NitroPort_MailboxPublish(&sMailbox,&next,&serial)){
            sDisplayed=sBuilding;
            sBuilding=(unsigned)next;
            sSerial=serial;
            sScenes[sBuilding].count=0;
            sScenes[sBuilding].shadow_count=0;
            sScenes[sBuilding].dropped=0;
            NitroPort_Gx3dScenePublished();
        }else{
            /* Keep the newest image and reuse this building slot on the
             * exceptionally slow consumer path; never stall the game. */
            scene->count=0;scene->shadow_count=0;scene->dropped++;
        }
        sShadowCapture=NitroPort_SunShadows() && shadow_store();
        interp_swap();
        s.frameVertices=0;s.framePolygons=0;
        if(++sDiag.scenes>=120)diag_log();
        break;
    }
    case 0x60:
        set_viewport(&s,p[0]);
        break;
    case 0x70:case 0x71:case 0x72:test_command(&s,op,p);break;
    case OP_CLEAR_STACK_ERROR:s.stackError=0;s.pjLevel=0;break;
    case OP_WIDE_VIEW:set_wide_view(&s,(int)p[0]);break;
    case OP_SCENE_REGS:
        sPendingStamp=p[0];
        sPendingDisp3d=p[1];
        memcpy(sPendingRegs3d,&p[2],NITRO_GX3D_REGS_SIZE);
        memcpy(sPendingBgOfs,&p[2+NITRO_GX3D_REGS_SIZE/4],sizeof(sPendingBgOfs));
        break;
    default:break;
    }
    (void)i;
}

/* ---------------------------------------------------- geometry thread */
/* With a geometry thread, the game's thread decodes the commands, keeps
 * the shadow matrices, and hands each command on through this ring of
 * words: a header (command, parameter count, flags) and the parameters,
 * never split by the ring's end (a PAD header skips to the start). The
 * game waits only when the ring is full, or for NitroPort_Gx3dReset. */
#define RING_WORDS (1u<<18)
#define RING_MASK  (RING_WORDS-1u)
#define RING_PAD   0xFFFFFFFFu
/* Flag: the command belongs to a scene left out (see produce): only the
 * state it leaves behind is kept, no vertices are made. */
#define RING_SKIP  (1u<<16)
/* Words written before the thread is told: fewer, larger hand-overs. */
#define RING_PUBLISH_WORDS 256u
static uint32_t sRing[RING_WORDS];
static uint32_t sRingWrite, sRingRead;     /* shared: words written / done */
static uint32_t sWriteLocal, sWritePublished; /* the game's thread's */
static uint32_t sReadSeen;                    /* sRingRead, as last looked at */
static int sWorkerSleeping, sProducerWaiting;
static NitroGx3dThreadOps sOps;
/* Scenes (SWAP_BUFFERS) handed on / finished, and left out. */
static uint32_t sSwapsQueued, sSwapsDone, sSwapsSkipped;
static int sSkipScene;
static uint64_t sWorkerBusy;
static uint32_t sWorkerWakeups;      /* stretches of work (each after a sleep) */
static uint64_t sWorkerLongest;      /* the longest of them since last read */

static void publish(void)
{
    sWritePublished=sWriteLocal;
    __atomic_store_n(&sRingWrite,sWriteLocal,__ATOMIC_SEQ_CST);
    if(__atomic_load_n(&sWorkerSleeping,__ATOMIC_SEQ_CST) &&
       __atomic_exchange_n(&sWorkerSleeping,0,__ATOMIC_SEQ_CST))
        sOps.wake_worker(sOps.context);
}
/* Wait until the ring has room for `words` more (or, with RING_WORDS, is
 * empty: everything handed on has been done). */
/* (the game's thread's time waiting for the geometry thread, for the log) */
static uint64_t sProducerWaitNanos;
uint64_t NitroPort_Gx3dProducerWaitMicros(void)
{
    return sProducerWaitNanos/1000u;
}
static void wait_for_room(uint32_t words)
{
    uint64_t start;
    /* Usually there's room as far as the game's thread already knows. */
    if(RING_WORDS-(sWriteLocal-sReadSeen)>=words)return;
    start=NitroPortPlatform_Nanoseconds();
    for(;;){
        uint32_t used=sWriteLocal-(sReadSeen=__atomic_load_n(&sRingRead,__ATOMIC_SEQ_CST));
        if(RING_WORDS-used>=words)break;
        publish();
        __atomic_store_n(&sProducerWaiting,1,__ATOMIC_SEQ_CST);
        used=sWriteLocal-(sReadSeen=__atomic_load_n(&sRingRead,__ATOMIC_SEQ_CST));
        if(RING_WORDS-used>=words){
            __atomic_store_n(&sProducerWaiting,0,__ATOMIC_SEQ_CST);
            break;
        }
        sOps.wait_producer(sOps.context);
    }
    sProducerWaitNanos+=NitroPortPlatform_Nanoseconds()-start;
}
static void drain(void)
{
    if(sThreaded)wait_for_room(RING_WORDS);
}
static void enqueue(unsigned op,const uint32_t *p,unsigned n,uint32_t flags)
{
    uint32_t need=1u+n, pos=sWriteLocal&RING_MASK;
    if(pos+need>RING_WORDS){
        wait_for_room(RING_WORDS-pos);
        sRing[pos]=RING_PAD;
        sWriteLocal+=RING_WORDS-pos;
        pos=0;
    }
    wait_for_room(need);
    sRing[pos]=op|(n<<8)|flags;
    {
        uint32_t *out=&sRing[pos+1];
        unsigned i;
        for(i=0;i<n;i++)out[i]=p[i];
    }
    sWriteLocal+=need;
    if(sWriteLocal-sWritePublished>=RING_PUBLISH_WORDS)publish();
}
/* Not a DS command: a block of whole GXFIFO packets (command words and
 * their parameters) as the game sent them; the next word is its length. */
#define OP_PACKETS 0xF1u
#define RING_PACKETS_MAX 8192u
static void enqueue_packets(const uint32_t *words,uint32_t count,uint32_t flags)
{
    uint32_t need=2u+count, pos=sWriteLocal&RING_MASK;
    if(pos+need>RING_WORDS){
        wait_for_room(RING_WORDS-pos);
        sRing[pos]=RING_PAD;
        sWriteLocal+=RING_WORDS-pos;
        pos=0;
    }
    wait_for_room(need);
    sRing[pos]=OP_PACKETS|flags;
    sRing[pos+1]=count;
    memcpy(&sRing[pos+2],words,count*sizeof(*words));
    sWriteLocal+=need;
    if(sWriteLocal-sWritePublished>=RING_PUBLISH_WORDS)publish();
}
/* Before a SWAP_BUFFERS is handed on: its time and the 3D registers, in
 * the ring ahead of it (a word written in the ring may be the thread's at
 * once: whatever the SWAP takes must be there before it). */
static void swap_noted(void)
{
    uint32_t w[SCENE_REGS_WORDS];
    w[0]=NitroPort_Gx3dStamp();
    w[1]=io_disp3dcnt();
    memcpy(&w[2],NitroPort_IoReg+NITRO_GX3D_REGS_AT,NITRO_GX3D_REGS_SIZE);
    io_bg_ofs((uint16_t *)&w[2+NITRO_GX3D_REGS_SIZE/4]);
    enqueue(OP_SCENE_REGS,w,SCENE_REGS_WORDS,0);
}
/* After a SWAP_BUFFERS is handed on: hand it over now, and whether the
 * next scene is to be left out. */
static void swap_queued(void)
{
    uint32_t behind;
    sSwapsQueued++;
    publish();
    behind=sSwapsQueued-__atomic_load_n(&sSwapsDone,__ATOMIC_ACQUIRE);
    /* Still working on the scene before this one: leave out the next. */
    sSkipScene=behind>=2u && !sOps.keep_every_scene;
}
/* What the game's thread does with a command (the rest is the thread's):
 * bit 0 matrices, 1 tests, 2 SWAP_BUFFERS. */
static uint8_t sShadowOps[256];
/* Whole packets at the start of `words` (up to a SWAP_BUFFERS, which ends
 * a block): the shadow's commands done, the length returned. */
static size_t scan_packets(const uint32_t *words,size_t count,int *swapped)
{
    size_t i=0;
    if(count>RING_PACKETS_MAX)count=RING_PACKETS_MAX;
    *swapped=0;
    while(i<count){
        uint32_t word=words[i];
        unsigned o0=word&255u,o1=(word>>8)&255u,o2=(word>>16)&255u,o3=word>>24;
        size_t total=1u+sParamCount[o0]+sParamCount[o1]+sParamCount[o2]+sParamCount[o3];
        if(i+total>count)break;
        if(sShadowOps[o0]|sShadowOps[o1]|sShadowOps[o2]|sShadowOps[o3]){
            const uint32_t *p=&words[i+1];
            int k;
            for(k=0;k<4;k++){
                unsigned op=(word>>(k*8))&255u, what=sShadowOps[op];
                if(what&1u){
                    matrix_command(&sShadow,op,p);
                    if(op>=0x11u && op<=0x14u)refresh_status(&sShadow);
                }else if(what&2u){
                    test_command(&sShadow,op,p);
                }else if(what&8u){
                    set_viewport(&sShadow,p[0]);
                }else if(what&4u){
                    *swapped=1;
                }
                p+=sParamCount[op];
            }
        }
        i+=total;
        if(*swapped)break;
    }
    return i;
}
/* A decoded command, on the game's thread with a geometry thread. */
static void produce(unsigned op,const uint32_t *p)
{
    unsigned n=(unsigned)param_count(op);
    uint32_t flags=0;
    if(op>=0x70u && op<=0x72u){
        /* Tests only give results to the game: the shadow's matrices. */
        test_command(&sShadow,op,p);
        return;
    }
    if(op>=0x10u && op<=0x1cu){
        matrix_command(&sShadow,op,p);
        if(op>=0x11u && op<=0x14u)refresh_status(&sShadow);
    }else if(!n && op!=0x41u){
        return; /* NOP, or not a command */
    }
    /* A scene the thread can't keep up with is left out (its vertices and
     * normals aren't worked out, and it isn't shown): the game keeps its
     * speed, and the sound its time, whatever the 3D costs. */
    if(sSkipScene && ((op>=0x21u && op<=0x28u && op!=0x22u) || op==0x50u))
        flags=RING_SKIP;
    if(op==0x50u)swap_noted();
    enqueue(op,p,n,flags);
    if(op==0x50u)swap_queued();
}
/* A command on the thread; skip: its scene is left out. */
static inline __attribute__((always_inline)) void work_op(unsigned op,const uint32_t *p,int skip)
{
    if(skip){
        if(op==0x21u){
            int j;
            for(j=0;j<3;j++)s.normal[j]=signbits(p[0]>>(j*10),10)*8;
            return;
        }
        if(op>=0x23u && op<=0x28u){
            set_geo_position(op,p);
            return;
        }
        if(op==0x50u){
            /* Nothing published: the scene being built starts again. */
            sScenes[sBuilding].count=0;
            sScenes[sBuilding].shadow_count=0;
            sScenes[sBuilding].dropped=0;
            s.frameVertices=0;s.framePolygons=0;
            __atomic_store_n(&sSwapsSkipped,sSwapsSkipped+1u,__ATOMIC_RELAXED);
            __atomic_store_n(&sSwapsDone,sSwapsDone+1u,__ATOMIC_RELEASE);
            return;
        }
    }
    execute(op,p);
    if(op==0x50u)__atomic_store_n(&sSwapsDone,sSwapsDone+1u,__ATOMIC_RELEASE);
}
/* ------------------------------------------- polygons out of the view
 *
 * Most of a big map is off the screen: in the field about three in four
 * vertices belong to a BEGIN_VTXS whose polygons are all outside the view
 * volume, worked out only to be thrown away. Such a list of vertices
 * (from BEGIN_VTXS to the next END_VTXS or BEGIN_VTXS, nothing among them
 * but vertices and what goes with them: TEXCOORD, NORMAL, COLOR) shows
 * itself by its coordinates' bounding box: if all eight corners are
 * outside one clip plane, so is every vertex in it (clip coordinates are
 * linear in a vertex's), and every polygon of them is one the engine
 * throws away whole (polygon_hidden). Then only what the list leaves
 * behind is worked out: the last coordinates, texture coordinates, normal
 * and colour, and the [gx3d] counts. The same scene as without.
 *
 * The game sends its display lists in pieces (NitroSystem's geometry
 * buffer, ~118 words; commands written one at a time), so a list goes on
 * over several ring entries: it's followed through them, as far as the
 * game's thread has written. */

/* Walking a block of packets command by command: the next command word at
 * w[i], what's left of the current one in `word`. */
typedef struct OpCursor {
    const uint32_t *w;
    uint32_t count, i, word;
} OpCursor;
static inline int next_op(OpCursor *c,unsigned *op,const uint32_t **p)
{
    for(;;){
        while(!c->word){
            if(c->i>=c->count)return 0;
            c->word=c->w[c->i++];
        }
        *op=c->word&255u;
        c->word>>=8;
        if(*op)break;
    }
    *p=&c->w[c->i];
    c->i+=sParamCount[*op];
    return 1;
}
/* A ring entry's length in words. */
static inline uint32_t entry_length(const uint32_t *entry)
{
    return (entry[0]&255u)==OP_PACKETS?2u+entry[1]:1u+((entry[0]>>8)&255u);
}
/* Walking the ring's commands across entries, up to `end` (what the game's
 * thread has written): the entry at r, and in it c (a block of packets),
 * or `single` (a command of its own, not taken yet). */
typedef struct StreamCursor {
    uint32_t r, end;
    OpCursor c;
    int single;
} StreamCursor;
/* The next command; 0 at the end of what's written, or at a left-out
 * scene's entry (RING_SKIP): nothing is known beyond. */
static int stream_next(StreamCursor *sc,unsigned *op,const uint32_t **p)
{
    for(;;){
        const uint32_t *entry;
        uint32_t next,header;
        if(sc->single){
            entry=&sRing[sc->r&RING_MASK];
            sc->single=0;
            sc->c.count=sc->c.i=sc->c.word=0;   /* taken: the entry is done */
            *op=entry[0]&255u;
            *p=entry+1;
            return 1;
        }
        if(next_op(&sc->c,op,p))return 1;
        next=sc->r+entry_length(&sRing[sc->r&RING_MASK]);
        if(next==sc->end)return 0;
        header=sRing[next&RING_MASK];
        if(header==RING_PAD){
            next=(next|RING_MASK)+1u;
            if(next==sc->end)return 0;
            header=sRing[next&RING_MASK];
        }
        if(header&RING_SKIP)return 0;
        sc->r=next;
        if((header&255u)==OP_PACKETS){
            sc->c.w=&sRing[(next+2u)&RING_MASK];
            sc->c.count=sRing[(next+1u)&RING_MASK];
            sc->c.i=sc->c.word=0;
        }else{
            sc->single=1;
        }
    }
}
/* A vertex command's coordinates into xyz (as set_geo_position). */
static inline void vertex_coords(unsigned op,const uint32_t *p,int32_t xyz[3])
{
    switch(op){
    case 0x23:xyz[0]=signbits(p[0],16);xyz[1]=signbits(p[0]>>16,16);xyz[2]=signbits(p[1],16);break;
    case 0x24:xyz[0]=signbits(p[0],10)*64;xyz[1]=signbits(p[0]>>10,10)*64;xyz[2]=signbits(p[0]>>20,10)*64;break;
    case 0x25:xyz[0]=signbits(p[0],16);xyz[1]=signbits(p[0]>>16,16);break;
    case 0x26:xyz[0]=signbits(p[0],16);xyz[2]=signbits(p[0]>>16,16);break;
    case 0x27:xyz[1]=signbits(p[0],16);xyz[2]=signbits(p[0]>>16,16);break;
    default:xyz[0]+=signbits(p[0],10);xyz[1]+=signbits(p[0]>>10,10);xyz[2]+=signbits(p[0]>>20,10);break;
    }
}
/* The polygons n vertices of a primitive make (as submit_vertex does). */
static unsigned polygons_of(int primitive,unsigned n)
{
    switch(primitive){
    case 0:return n/3u;
    case 1:return n/4u;
    case 2:return n>=3u?n-2u:0u;
    default:return n>=4u?1u+(n-4u)/2u:0u;
    }
}
/* Right after a BEGIN_VTXS (done), with *sc where it was: if the vertices
 * that follow are all outside one clip plane, what they'd leave behind is
 * done instead of them, *sc is moved to the command that ends them (not
 * taken), and 1 returned. */
/* What a list of vertices leaves behind, and their bounding box, as
 * they're looked through (skip_hidden_vertices). */
typedef struct VertexScan {
    int32_t xyz[3],lo[3],hi[3];
    unsigned n;
    uint32_t texcoord,normal,color;
    int haveTexcoord,haveNormal,haveColor,normalLast;
} VertexScan;
/* One command of the list: 0 to go on, 1 if it ends the list, -1 if it's
 * something else (then the list is done as usual). */
static inline int scan_op(VertexScan *v,unsigned op,const uint32_t *p)
{
    if(op>=0x23u && op<=0x28u){
        vertex_coords(op,p,v->xyz);
        if(!v->n){
            v->lo[0]=v->hi[0]=v->xyz[0];v->lo[1]=v->hi[1]=v->xyz[1];v->lo[2]=v->hi[2]=v->xyz[2];
        }else{
            if(v->xyz[0]<v->lo[0])v->lo[0]=v->xyz[0];else if(v->xyz[0]>v->hi[0])v->hi[0]=v->xyz[0];
            if(v->xyz[1]<v->lo[1])v->lo[1]=v->xyz[1];else if(v->xyz[1]>v->hi[1])v->hi[1]=v->xyz[1];
            if(v->xyz[2]<v->lo[2])v->lo[2]=v->xyz[2];else if(v->xyz[2]>v->hi[2])v->hi[2]=v->xyz[2];
        }
        v->n++;
        return 0;
    }
    switch(op){
    case 0x22u:v->texcoord=p[0];v->haveTexcoord=1;return 0;
    case 0x21u:v->normal=p[0];v->haveNormal=1;v->normalLast=1;return 0;
    case 0x20u:v->color=p[0];v->haveColor=1;v->normalLast=0;return 0;
    case 0x40u:case 0x41u:return 1;
    default:return -1;
    }
}
static int skip_hidden_vertices(StreamCursor *sc)
{
    StreamCursor scan=*sc,end;
    VertexScan v;
    unsigned op,n,polys,k,j,outside=0x3fu,behind=0,front=0;
    unsigned mode=(s.texparam>>30)&3u;
    const uint32_t *p;
    int32_t *xyz=v.xyz,*lo=v.lo,*hi=v.hi;

    /* Texture coordinates from normals or vertices: done as usual. */
    if(mode>=2u)return 0;
    /* The vertices' bounding box, where they end, and what they leave. */
    memset(&v,0,sizeof(v));
    v.xyz[0]=s.coord[0];v.xyz[1]=s.coord[1];v.xyz[2]=s.coord[2];
    for(;;){
        /* Through the block of packets it's in (most of the way). */
        const uint32_t *w=scan.c.w;
        uint32_t count=scan.c.count,i=scan.c.i,word=scan.c.word;
        int r;
        for(;;){
            if(!word){
                if(i>=count)break;
                word=w[i++];
                continue;
            }
            op=word&255u;
            if(!op){word>>=8;continue;}
            r=scan_op(&v,op,&w[i]);
            if(r>0){
                end=scan;
                end.c.i=i;end.c.word=word;   /* (that command not taken) */
                goto found;
            }
            if(r<0)return 0;
            word>>=8;
            i+=sParamCount[op];
        }
        scan.c.i=i;scan.c.word=word;
        /* On into the next entry (or a command of its own). */
        end=scan;
        if(!stream_next(&scan,&op,&p))return 0;   /* the rest isn't known yet */
        r=scan_op(&v,op,p);
        if(r>0)goto found;
        if(r<0)return 0;
    }
found:
    n=v.n;
    if(!n)return 0;
    /* Each corner through the clip matrix, before the >> 12 (S): with
     * a = S >> 12, S_v + S_w < 0 makes v + w < 0, and S_v - S_w >= 4096
     * makes v > w: outside that plane (outcode). */
    ensure_clip(&s);
    {
        /* The corner at lo, and what each edge of the box adds (exact,
         * in 64 bits): eight corners for the cost of four transforms. */
        int64_t base[4],edge[3][4];
        for(j=0;j<4;j++){
            base[j]=(int64_t)lo[0]*s.xf.a[j]+(int64_t)lo[1]*s.xf.a[4+j]+
                    (int64_t)lo[2]*s.xf.a[8+j]+s.xfTrans[j];
            for(k=0;k<3;k++)edge[k][j]=((int64_t)hi[k]-lo[k])*s.xf.a[k*4+j];
        }
        for(k=0;k<8;k++){
            int64_t c[4];
            for(j=0;j<4;j++){
                c[j]=base[j]+((k&1)?edge[0][j]:0)+((k&2)?edge[1][j]:0)+((k&4)?edge[2][j]:0);
                /* Far from saturation() for every point of the box. */
                if(c[j]>=((int64_t)1<<42)||c[j]<=-((int64_t)1<<42))return 0;
            }
            if(!(c[0]+c[3]<0))outside&=~1u;
            if(!(c[0]-c[3]>=4096))outside&=~2u;
            if(!(c[1]+c[3]<0))outside&=~4u;
            if(!(c[1]-c[3]>=4096))outside&=~8u;
            if(!(c[2]+c[3]<0))outside&=~16u;
            if(!(c[2]-c[3]>=4096))outside&=~32u;
            /* w <= 0 (behind the camera; a [gx3d] count): all or none. */
            if(c[3]<4096)behind++;else front++;
            if(!outside)return 0;
        }
    }
    if(behind&&front)return 0;
    /* Hidden: what the commands leave behind, without the vertices (each
     * command only sets state; the last one of each kind is what stays). */
    s.coord[0]=xyz[0];s.coord[1]=xyz[1];s.coord[2]=xyz[2];
    if(v.haveTexcoord){
        s.texcoord[0]=signbits(v.texcoord,16);
        s.texcoord[1]=signbits(v.texcoord>>16,16);
        if(mode==0){
            s.resolvedTexcoord[0]=s.texcoord[0];
            s.resolvedTexcoord[1]=s.texcoord[1];
        }else{
            transform_texcoords(s.texcoord,1,s.resolvedTexcoord);
        }
    }
    if(v.haveNormal){
        for(j=0;j<3;j++)s.normal[j]=signbits(v.normal>>(j*10),10)*8;
        s.normalValue=v.normal;
    }
    if(v.haveColor)s.rgb=v.color&0x7FFF;
    if(v.haveNormal||v.haveColor)s.rgbPending=v.normalLast;
    polys=polygons_of(s.primitive,n);
    sDiag.vertices+=n;
    if(behind)sDiag.behind+=n;
    sDiag.polygons+=polys;
    sDiag.clipped+=polys;
    /* Its END_VTXS or BEGIN_VTXS is next; no vertex of it is kept. */
    s.nverts=0;
    *sc=end;
    return 1;
}

/* Where the worker goes on after a skip that ended in a later entry: in
 * the block of packets there, from (sResumeI, sResumeWord). */
static int sResume;
static uint32_t sResumeI,sResumeWord;

/* A block of whole packets of a scene that's kept (the ring entry at r,
 * up to `end` written), from command word i / the rest of the current one
 * `word`: as work_op would do each command, with what's in nearly every
 * vertex (TEXCOORD, NORMAL, COLOR, the VTX commands) done here, one branch
 * away; a command word's commands after its last non-NOP are NOPs, and
 * aren't looked at. After a BEGIN_VTXS, vertices all out of the view are
 * passed over (skip_hidden_vertices), maybe into later entries: returns
 * the entry to go on with. */
static uint32_t work_packets(uint32_t r,uint32_t end,uint32_t i,uint32_t word)
{
    const uint32_t *w=&sRing[(r+2u)&RING_MASK];
    uint32_t count=sRing[(r+1u)&RING_MASK];
    for(;;){
        if(!word){
            if(i>=count)break;
            word=w[i++];
            continue;
        }
        {
            unsigned o=word&255u;
            const uint32_t *p=&w[i];
            word>>=8;
            switch(o){
            case 0x00:break;
            case 0x20:s.rgb=p[0]&0x7FFF;s.rgbPending=0;i++;break;
            case 0x21:{
                int j;
                for(j=0;j<3;j++)s.normal[j]=signbits(p[0]>>(j*10),10)*8;
                s.normalValue=p[0];s.rgbPending=1;
                if(((s.texparam>>30)&3u)==2u)transform_texcoords(s.normal,2,s.resolvedTexcoord);
                i++;
                break;
            }
            case 0x22:{
                unsigned mode=(s.texparam>>30)&3u;
                s.texcoord[0]=signbits(p[0],16);
                s.texcoord[1]=signbits(p[0]>>16,16);
                if(mode==0){
                    s.resolvedTexcoord[0]=s.texcoord[0];
                    s.resolvedTexcoord[1]=s.texcoord[1];
                }else if(mode==1){
                    transform_texcoords(s.texcoord,1,s.resolvedTexcoord);
                }
                i++;
                break;
            }
            case 0x23:
                s.coord[0]=signbits(p[0],16);s.coord[1]=signbits(p[0]>>16,16);s.coord[2]=signbits(p[1],16);
                submit_vertex();i+=2;break;
            case 0x24:
                s.coord[0]=signbits(p[0],10)*64;s.coord[1]=signbits(p[0]>>10,10)*64;
                s.coord[2]=signbits(p[0]>>20,10)*64;
                submit_vertex();i++;break;
            case 0x25:s.coord[0]=signbits(p[0],16);s.coord[1]=signbits(p[0]>>16,16);submit_vertex();i++;break;
            case 0x26:s.coord[0]=signbits(p[0],16);s.coord[2]=signbits(p[0]>>16,16);submit_vertex();i++;break;
            case 0x27:s.coord[1]=signbits(p[0],16);s.coord[2]=signbits(p[0]>>16,16);submit_vertex();i++;break;
            case 0x28:
                s.coord[0]+=signbits(p[0],10);s.coord[1]+=signbits(p[0]>>10,10);s.coord[2]+=signbits(p[0]>>20,10);
                submit_vertex();i++;break;
            case 0x40:{
                StreamCursor sc;
                work_op(o,p,0);
                i++;
                sc.r=r;sc.end=end;sc.single=0;
                sc.c.w=w;sc.c.count=count;sc.c.i=i;sc.c.word=word;
                if(skip_hidden_vertices(&sc)){
                    if(sc.r!=r){
                        /* Ended in a later entry: go on there; in a block
                         * of packets where it stopped, a command of its own
                         * from the start unless it was taken already. */
                        if(sc.single)return sc.r;
                        if((sRing[sc.r&RING_MASK]&255u)!=OP_PACKETS)
                            return sc.r+entry_length(&sRing[sc.r&RING_MASK]);
                        sResume=1;
                        sResumeI=sc.c.i;
                        sResumeWord=sc.c.word;
                        return sc.r;
                    }
                    i=sc.c.i;word=sc.c.word;
                }
                break;
            }
            case 0x70:case 0x71:case 0x72:
                /* Tests are the game's (the shadow's) only. */
                i+=sParamCount[o];break;
            default:
                work_op(o,p,0);
                i+=sParamCount[o];
                break;
            }
        }
    }
    return r+2u+count;
}
/* The ring entry at r (up to `end` written): returns the entry to go on
 * with (usually the next). */
static uint32_t work(uint32_t r,uint32_t end)
{
    const uint32_t *entry=&sRing[r&RING_MASK];
    uint32_t header=entry[0];
    unsigned op=header&255u;
    int skip=(header&RING_SKIP)!=0;
    if(op==OP_PACKETS && !skip){
        uint32_t i=0,word=0;
        if(sResume){i=sResumeI;word=sResumeWord;sResume=0;}
        return work_packets(r,end,i,word);
    }
    sResume=0;
    if(op==OP_PACKETS){
        uint32_t count=entry[1], i=0;
        const uint32_t *w=entry+2;
        while(i<count){
            uint32_t word=w[i++];
            int k;
            for(k=0;k<4;k++){
                unsigned o=(word>>(k*8))&255u;
                /* Tests are the game's (the shadow's) only. */
                if(o && (o<0x70u || o>0x72u))work_op(o,&w[i],skip);
                i+=sParamCount[o];
            }
        }
        return r+2u+count;
    }
    work_op(op,entry+1,skip);
    return r+1u+((header>>8)&255u);
}
void NitroPort_Gx3dThreadMain(const volatile int *running)
{
    uint32_t r=__atomic_load_n(&sRingRead,__ATOMIC_SEQ_CST);
    for(;;){
        uint32_t w=__atomic_load_n(&sRingWrite,__ATOMIC_SEQ_CST), stored=r;
        uint64_t start;
        if(r==w){
            if(!*running)break;
            __atomic_store_n(&sWorkerSleeping,1,__ATOMIC_SEQ_CST);
            if(__atomic_load_n(&sRingWrite,__ATOMIC_SEQ_CST)==r && *running)
                sOps.wait_worker(sOps.context);
            __atomic_store_n(&sWorkerSleeping,0,__ATOMIC_SEQ_CST);
            continue;
        }
        start=sOps.ticks?sOps.ticks(sOps.context):0;
        while(r!=w){
            uint32_t header=sRing[r&RING_MASK];
            if(header==RING_PAD){r=(r|RING_MASK)+1u;continue;}
            r=work(r,w);
            if(r-stored>=4096u){
                /* Room for the game as soon as there is some. */
                stored=r;
                __atomic_store_n(&sRingRead,r,__ATOMIC_SEQ_CST);
                if(__atomic_load_n(&sProducerWaiting,__ATOMIC_SEQ_CST) &&
                   __atomic_exchange_n(&sProducerWaiting,0,__ATOMIC_SEQ_CST))
                    sOps.wake_producer(sOps.context);
            }
        }
        __atomic_store_n(&sRingRead,r,__ATOMIC_SEQ_CST);
        if(sOps.ticks){
            uint64_t took=sOps.ticks(sOps.context)-start;
            __atomic_store_n(&sWorkerBusy,sWorkerBusy+took,__ATOMIC_RELAXED);
            if(took>__atomic_load_n(&sWorkerLongest,__ATOMIC_RELAXED))
                __atomic_store_n(&sWorkerLongest,took,__ATOMIC_RELAXED);
        }
        __atomic_store_n(&sWorkerWakeups,sWorkerWakeups+1u,__ATOMIC_RELAXED);
        if(__atomic_load_n(&sProducerWaiting,__ATOMIC_SEQ_CST) &&
           __atomic_exchange_n(&sProducerWaiting,0,__ATOMIC_SEQ_CST))
            sOps.wake_producer(sOps.context);
    }
}
static void initialize(void);
void NitroPort_Gx3dStartThread(const NitroGx3dThreadOps *ops)
{
    initialize();
    if(sThreaded)return;
    sOps=*ops;
    {
        unsigned op;
        for(op=0x10u;op<=0x1cu;op++)sShadowOps[op]=1u;
        for(op=0x70u;op<=0x72u;op++)sShadowOps[op]=2u;
        sShadowOps[0x60]=8u;   /* the viewport: its width (a wide view) */
        sShadowOps[0x50]=4u;
    }
    sShadow=s;
    sResults=&sShadow;
    sThreaded=1;
}
void NitroPort_Gx3dStopThread(void)
{
    if(!sThreaded)return;
    drain();
    sThreaded=0;
    sResults=&s;    /* the same matrices as the shadow's */
    sSkipScene=0;
    refresh_status(&s);
}
void NitroPort_Gx3dFlush(void)
{
    if(sThreaded && sWriteLocal!=sWritePublished)publish();
}
int NitroPort_Gx3dEarlierSceneUnread(void)
{
    uint32_t last,unread;
    if(!sThreaded)return 0;      /* scenes are read before the game goes on */
    last=sSwapsQueued;           /* the game's newest SWAP_BUFFERS */
    if(last==0)return 0;
    /* The thread hasn't got to the one before it yet. */
    if(__atomic_load_n(&sSwapsDone,__ATOMIC_ACQUIRE)+1u<last)return 1;
    unread=NitroPort_MailboxUnread(&sMailbox);
    return unread!=0 && unread<last;
}
void NitroPort_Gx3dThreadWakeups(uint32_t *wakeups,uint64_t *longest)
{
    if(wakeups)*wakeups=__atomic_load_n(&sWorkerWakeups,__ATOMIC_RELAXED);
    if(longest)*longest=__atomic_exchange_n(&sWorkerLongest,0,__ATOMIC_RELAXED);
}
void NitroPort_Gx3dThreadStats(uint64_t *busy,uint32_t *scenes,uint32_t *skipped)
{
    if(busy)*busy=__atomic_load_n(&sWorkerBusy,__ATOMIC_RELAXED);
    if(scenes)*scenes=__atomic_load_n(&sSwapsDone,__ATOMIC_ACQUIRE);
    if(skipped)*skipped=__atomic_load_n(&sSwapsSkipped,__ATOMIC_RELAXED);
}

/* ------------------------------------------------- the game's side */
static NitroGx3dRecorder sRecorder;
void NitroPort_Gx3dSetRecorder(NitroGx3dRecorder recorder){sRecorder=recorder;}
void NitroPort_Gx3dSetWideView(int screenWidth)
{
    uint32_t p=(uint32_t)screenWidth;
    initialize();
    if(screenWidth<=0||screenWidth>1024)screenWidth=256;
    p=(uint32_t)screenWidth;
    sWideWidth=screenWidth;
    if(sResults->wideDen==(screenWidth==256?1:screenWidth) && sResults->wideNum==(screenWidth==256?1:256))
        return;   /* (as it is) */
    /* The shadow now (the game's box tests); the thread at this point of
     * its stream. */
    set_wide_view(sResults,screenWidth);
    if(sThreaded)enqueue(OP_WIDE_VIEW,&p,1,0);
    else if(sResults!=&s)set_wide_view(&s,screenWidth);
}
#define RECORD(type,words,count) do{if(sRecorder)sRecorder((type),(words),(count));}while(0)
/* A decoded command: done here, or handed to the geometry thread. */
static void run(unsigned op,const uint32_t *p)
{
    if(sThreaded)produce(op,p);
    else execute(op,p);
}
void NitroPort_Gx3dClearStackError(void)
{
    RECORD(4,NULL,0);
    initialize();
    sResults->stackError=0;
    sResults->pjLevel=0;
    if(sThreaded)enqueue(OP_CLEAR_STACK_ERROR,NULL,0,0);
    refresh_status(sResults);
}
void NitroPort_Gx3dReset(void)
{
    RECORD(7,NULL,0);
    drain();
    memset(&s,0,sizeof(s));memset(sScenes,0,sizeof(sScenes));
    shadow_store_attach();
    memset(&sDec,0,sizeof(sDec));
    sLightCache.dirty=1;
    sNormalCache.valid=0;
    make_products();
    NitroPort_MailboxInit(&sMailbox);
    sBuilding=0;sDisplayed=1;sSerial=0;
    sRecCount[0]=sRecCount[1]=0;sRecBeforeValid=0;sMidOk[0]=sMidOk[1]=sMidOk[2]=0;sDest=NULL;sReplaying=0;
    identity(&s.proj);identity(&s.pos);identity(&s.vec);identity(&s.tex);
    set_wide_view(&s,sWideWidth);   /* (kept across resets) */
    s.viewport[0]=0;s.viewport[1]=0;s.viewport[2]=255;s.viewport[3]=191;
    s.rgb=0x7fff;s.diffuse=0x7fff;s.attr=(3u<<6)|(31u<<16);
    s.pendingAttr=s.attr;
    s.boxVisible=1;s.ready=1;
    s.clipDirty=1;
    {
        int i;
        for(i=0;i<4;i++)s.lightHalf[i][2]=-4096/2; /* light vectors (0,0,0) */
    }
    sShadow=s;
    sSkipScene=0;
    refresh_status(sResults);
}
static void initialize(void){if(!s.ready)NitroPort_Gx3dReset();}
void NitroPort_GxDirectWrite(int opcode,uint32_t value)
{
    int needed;
    if(sRecorder){uint32_t rec[2]={(uint32_t)opcode,value};sRecorder(3,rec,2);}
    initialize();
    if(sDec.directNeed && sDec.directOpcode==opcode){
        sDec.params[sDec.used++]=value;
        if(sDec.used>=sDec.directNeed){run((unsigned)opcode,sDec.params);sDec.directNeed=0;sDec.used=0;}
        return;
    }
    if(sDec.directNeed){sDec.directNeed=0;sDec.used=0;} /* incorrect partial stream */
    needed=param_count((unsigned)opcode);
    if(!needed){run((unsigned)opcode,sDec.params);return;}
    sDec.directOpcode=opcode;sDec.directNeed=needed;sDec.used=0;
    sDec.params[sDec.used++]=value;
    if(sDec.used>=needed){run((unsigned)opcode,sDec.params);sDec.directNeed=0;sDec.used=0;}
}
/* After a command: the next command of the packed word that needs
 * parameters (running those that don't), or none left. */
static void next_packed(void)
{
    while(sDec.packedIndex<4){
        unsigned op=sDec.packed[sDec.packedIndex];
        int need=param_count(op);
        if(need){sDec.need=need;sDec.used=0;return;}
        run(op,sDec.params);
        sDec.packedIndex++;
    }
    sDec.packedIndex=0;sDec.packedCount=0;sDec.need=0;sDec.used=0;
}
/* Words written to GXFIFO: packed command words (up to four commands),
 * each followed by its commands' parameters. A command's parameters may
 * arrive in several calls (DMA bursts). */
void NitroPort_GxFifoWords(const uint32_t *words,size_t count)
{
    RECORD(1,words,count);
    initialize();
    while(count){
        if(sDec.need>0){
            unsigned op=sDec.packed[sDec.packedIndex];
            size_t take=(size_t)(sDec.need-sDec.used);
            if(sDec.used==0 && count>=take){
                /* All of them here: straight from the stream. */
                run(op,words);
            }else{
                if(take>count)take=count;
                memcpy(&sDec.params[sDec.used],words,take*sizeof(*words));
                sDec.used+=(int)take;
                words+=take;count-=take;
                if(sDec.used<sDec.need)return;
                run(op,sDec.params);
                take=0;
            }
            words+=take;count-=take;
            sDec.packedIndex++;sDec.need=0;sDec.used=0;
            next_packed();
        }else{
            uint32_t word;
            if(sThreaded){
                /* Whole packets go to the thread as they are. */
                int swapped;
                size_t n=scan_packets(words,count,&swapped);
                if(n){
                    if(swapped)swap_noted();
                    enqueue_packets(words,(uint32_t)n,sSkipScene?RING_SKIP:0u);
                    words+=n;count-=n;
                    if(swapped)swap_queued();
                    continue;
                }
            }
            word=*words++;
            count--;
            sDec.packed[0]=(uint8_t)word;sDec.packed[1]=(uint8_t)(word>>8);
            sDec.packed[2]=(uint8_t)(word>>16);sDec.packed[3]=(uint8_t)(word>>24);
            sDec.packedCount=4;sDec.packedIndex=0;
            next_packed();
        }
    }
}
void NitroPort_GxFifoWord(uint32_t word)
{
    /* Most words are a command's parameters: just keep them. */
    if(sDec.need>0 && sDec.used+1<sDec.need && !sRecorder){
        sDec.params[sDec.used++]=word;
        return;
    }
    NitroPort_GxFifoWords(&word,1);
}
const NitroGx3dScene *NitroPort_Gx3dScene(void)
{
    initialize();
    drain();
    NitroPort_MailboxMarkTaken(&sMailbox);
    return &sScenes[sDisplayed];
}
const NitroGx3dScene *NitroPort_Gx3dAcquireScene(uint32_t *serial)
{
    int slot;
    initialize();
    slot=NitroPort_MailboxAcquire(&sMailbox,serial);
    return slot<0?NULL:&sScenes[slot];
}
int NitroPort_Gx3dNewerScene(uint32_t serial)
{
    initialize();
    return NitroPort_MailboxNewer(&sMailbox,serial);
}
unsigned NitroPort_Gx3dNewerScenes(uint32_t serial)
{
    initialize();
    return NitroPort_MailboxNewerCount(&sMailbox,serial);
}
void NitroPort_Gx3dReleaseScene(const NitroGx3dScene *scene)
{
    int i;
    if(!scene)return;
    for(i=0;i<3;i++)if(scene==&sScenes[i]){
        NitroPort_MailboxRelease(&sMailbox,i);
        break;
    }
}
/* GXSTAT and the matrix result registers, for the SDK to read. The port
 * can't see register reads, so GXSTAT is brought up to date after stack
 * commands and box tests and at every interrupt check, and the matrix
 * results when the SDK reads them (NitroPort_Gx3dSyncResults). With a
 * geometry thread they come from the shadow matrices, so they're up to
 * date without waiting for the thread. */
void NitroPort_Gx3dSyncResults(void)
{
    RECORD(5,NULL,0);
    initialize();
    ensure_clip(sResults);
}
void NitroPort_Gx3dRefreshStatus(void)
{
    RECORD(6,NULL,0);
    initialize();
    refresh_status(sResults);
}
