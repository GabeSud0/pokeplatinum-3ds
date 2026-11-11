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
#include <nitro_port_wide.h>
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
    int32_t tallNum, tallDen;   /* and its height's (NitroPort_Gx3dSetView) */
    Matrix pjStack, texStack;
    Matrix posStack[31], vecStack[31];
    int mode, pvLevel, pjLevel, texLevel, stackError;
    int32_t coord[3];
    int32_t texcoord[2], normal[3];
    uint16_t rgb, diffuse, ambient, specular, emission;
    int32_t resolvedTexcoord[2];
    uint32_t attr, pendingAttr, texparam, plttbase, shininess[32];
    uint32_t tag;            /* (the game's mark on the polygons now: NitroPort_Gx3dTag) */
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
/* The mark the game has set now (NitroPort_Gx3dTag), on its own thread:
 * what it's about to draw is for the sun shadows too, so its box tests
 * pass a little beside the view (test_command). */
static uint32_t sTagGame;
/* (a mark whose polygons are kept whole and reached for beside the view:
 * any but those that only say how they're drawn, NITRO_GX3D_TAG_IS_DRAW) */
#define TAG_KEPT(tag) ((tag) && !NITRO_GX3D_TAG_IS_DRAW(tag))
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
static uint32_t sSwapsQueued; /* (the game's thread's; see below) */

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
/* Normals' colours: a map's ground repeats its normals, and the game sets
 * its matrices for every shape (a map's materials, the props), most not
 * the vector matrix. 64 of them (by the normal's bits), forgotten together
 * when anything a colour depends on changes: the vector matrix, the
 * lights, the material, the polygon's lights at BEGIN_VTXS. (Before, the
 * last normal's alone, forgotten at every matrix command: on the Cycling
 * Road 12% less lighting work, 2026-11-01.) */
#define NORMAL_CACHE 64u
static struct { uint32_t value, age; uint16_t rgb; } sNormalCache[NORMAL_CACHE];
static uint32_t sNormalAge = 1;   /* (an entry of another age is forgotten; never 0) */
static void forget_normals(void)
{
    if(++sNormalAge==0){
        memset(sNormalCache,0,sizeof(sNormalCache));
        sNormalAge=1;
    }
}
static unsigned normal_slot(uint32_t value)
{
    return (value^(value>>10)^(value>>20))&(NORMAL_CACHE-1u);
}
static uint16_t shade(uint32_t value)
{
    int32_t normal[3], transformed[3];
    unsigned r=s.emission&31u,g=(s.emission>>5)&31u,b=(s.emission>>10)&31u;
    int i,j;
    uint16_t rgb;
    unsigned slot=normal_slot(value);
    if(sNormalCache[slot].age==sNormalAge && sNormalCache[slot].value==value)return sNormalCache[slot].rgb;
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
    sNormalCache[slot].age=sNormalAge;
    sNormalCache[slot].value=value;
    sNormalCache[slot].rgb=rgb;
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
    if(g->wideNum!=g->wideDen || g->tallNum!=g->tallDen)g->clipDirty=1;   /* (ensure_clip: the view's size) */
}
static int sWideWidth=256,sWideHeight=192;
/* With the geometry thread: the next SWAP_BUFFERS's time and 3D registers
 * (NitroGx3dScene.stamp, disp3dcnt, regs3d), as the game had them when it
 * sent it: handed over in the ring just before it (OP_SCENE_REGS, from the
 * game's thread: swap_noted), so the thread can't get to the SWAP first. */
static uint32_t sPendingStamp, sPendingDisp3d, sPendingSeq;
/* SWAP_BUFFERS the game has made, counted on its thread
 * (NitroPort_Gx3dSwapsMade) */
static uint32_t sSwapsMade;
static uint8_t sPendingRegs3d[NITRO_GX3D_REGS_SIZE];
static uint16_t sPendingBgOfs[8];
/* (the scene before's BG scrolls: an in-between scene's are halfway) */
static uint16_t sLastBgOfs[8];
static int sLastBgOfsValid;
/* (on the thread: the SWAP_BUFFERS before was published, or left out as
 * the same as one that was: the scene on the screen is what the next one
 * left out as the same stands for) */
static int sSameChain;
static uint32_t sLastStamp, sLastSeq, sLastWidth, sLastHeight;   /* (the scene before's) */
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
uint32_t NitroPort_Gx3dSwapsMade(void)
{
    return __atomic_load_n(&sSwapsMade,__ATOMIC_ACQUIRE);
}
/* The platform's clock for scenes (the 3DS: V-blanks); none by default. */
__attribute__((weak,noinline)) uint32_t NitroPort_Gx3dStamp(void)
{
    return 0;
}
static void set_wide_view(GeoState *g,int screenWidth,int screenHeight)
{
    g->wideNum=256;g->wideDen=screenWidth;
    if(screenWidth==256){g->wideNum=1;g->wideDen=1;}
    g->tallNum=192;g->tallDen=screenHeight;
    if(screenHeight==192){g->tallNum=1;g->tallDen=1;}
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
    /* (and taller: y, with the DS's full-height viewport) */
    if(g->tallNum!=g->tallDen && ((g->viewport[3]-g->viewport[1]+1)&0xFF)==192 &&
       ((g->viewport[2]-g->viewport[0]+1)&0x1FF)==256){
        for(j=0;j<4;j++){
            int64_t v=(int64_t)g->clip.a[j*4+1]*g->tallNum;
            v=(v>=0?v+g->tallDen/2:v-g->tallDen/2)/g->tallDen;
            g->xf.a[j*4+1]=saturation(v);
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
/* (floor, of a float well inside an int32: a place on the screen) */
static inline int32_t floor_small(float v)
{
    int32_t n=(int32_t)v;
    return n-(v<(float)n);
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
        if(v->clip[3]>0){
            /* (by 1 / W, worked out once above: (X + W) / 2W is X / W / 2
             * + 1 / 2. Two divisions fewer for every vertex drawn, in
             * doubles on a CPU that takes 30 cycles and more for one; the
             * same to the 1/256 pixel, but for one vertex in tens of
             * millions that lands a 256th to the side.) */
            out.x=floor_int(((double)v->clip[0]*inv+1.0)*(width*128.0)+x0*256+0.5);
            out.y=floor_int((1.0-(double)v->clip[1]*inv)*(height*128.0)+top*256+0.5);
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
/* (for the [perf] geometry line, on the thread's side) vertices and
 * polygons worked out, added up as each [gx3d] line's are cleared; the
 * time spent ending scenes (SWAP_BUFFERS: the scene's triangles sorted,
 * matched with the one before, published). */
static uint32_t sTotalVertices, sTotalPolygons, sSkippedVertices; /* (those in lists wholly out of the view: not worked out) */
static uint64_t sSwapTicks;

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
    __atomic_store_n(&sTotalVertices,sTotalVertices+sDiag.vertices,__ATOMIC_RELAXED);
    __atomic_store_n(&sTotalPolygons,sTotalPolygons+sDiag.polygons,__ATOMIC_RELAXED);
    memset(&sDiag, 0, sizeof(sDiag));
}

/* One polygon's triangles, held until the polygon is known to be stored. */
static NitroGx3dTriangle sStaged[16];
static unsigned sStagedCount, sStagedVertices, sStagedClipped;
/* (the in-between scenes, below) The scene's triangles end_polygon added
 * for the polygon just made (record_polygon keeps them with its record),
 * and, while a polygon is replayed into an in-between scene, its soft
 * flags (NitroGx3dTriangle.soft). */
static unsigned sLastTriFirst, sLastTriAdded;
static uint32_t sReplaySoft;
static int sReplaying;

/* Clip a triangle of the polygon being built to the view volume, cull it,
 * and stage what's left. */
/* Which side of the polygon faces the camera, decided once per polygon
 * from its first three vertices, as the DS does: a quad split into two
 * triangles (or a clipped polygon) is culled as a whole, not per triangle.
 * In clip space (x, y, w), front means det > 0 (the screen's y points
 * down); this works for vertices behind the camera too. */
static int sPolygonFront;
static int clip_front(const int32_t *a,const int32_t *b,const int32_t *c)
{
    double x0=a[0],y0=a[1],w0=a[3];
    double x1=b[0],y1=b[1],w1=b[3];
    double x2=c[0],y2=c[1],w2=c[3];
    double det=x0*(y1*w2-w1*y2)-y0*(x1*w2-w1*x2)+w0*(x1*y2-y1*x2);
    return det>0;
}
static int polygon_front(const GeoVertex *a,const GeoVertex *b,const GeoVertex *c)
{
    return clip_front(a->clip,b->clip,c->clip);
}
/* The vertex's screen position, worked out once: strips and quads share
 * vertices between their triangles. */
static const NitroGx3dVertex *screen_of(GeoVertex *v)
{
    if(!v->hasScreen){v->screen=to_screen(v);v->hasScreen=1;}
    v->screen.color=v->color;   /* (lit since, maybe: the sun shadows' took it first) */
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
 * positions are screen_of's, kept with the vertex (a strip's vertex may yet
 * be drawn, lit, by a later triangle: screen_of gives the colour as it is
 * then). */
static void keep_for_shadow(GeoVertex *a,GeoVertex *b,GeoVertex *c)
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
    tri->soft=0;
    tri->tag=0;
    if(NitroPort_Gx3dTriangleTranslucent(tri))return;
    tri->v[0]=*screen_of(a);tri->v[1]=*screen_of(b);tri->v[2]=*screen_of(c);
    scene->shadow_count++;
}
/* Keep a whole triangle of a marked polygon (NitroPort_Gx3dTag) as the
 * game sent it, wherever it is within the shadows' reach of the view
 * (NitroGx3dScene.tagged_triangles): not the in-between scenes' own
 * (theirs are made of the two scenes', tagged_halfway). */
static void keep_tagged(GeoVertex *a,GeoVertex *b,GeoVertex *c)
{
    NitroGx3dScene *scene=dest_scene();
    NitroGx3dTriangle *tri;
    if(sReplaying || scene->tagged_count>=NITRO_GX3D_MAX_TAGGED_TRIANGLES)return;
    if(!shadow_reach(a)||!shadow_reach(b)||!shadow_reach(c))return;
    tri=&scene->tagged_triangles[scene->tagged_count++];
    tri->v[0]=*screen_of(a);tri->v[1]=*screen_of(b);tri->v[2]=*screen_of(c);
    tri->polygon_attr=s.attr;
    tri->texture_param=s.texparam;
    tri->palette_base=s.plttbase;
    tri->sort_key=0;
    tri->soft=0;
    tri->tag=s.tag;
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
    tri->soft=sReplaying?sReplaySoft:0u;
    tri->tag=s.tag;
    return 0;
}
static void stage_triangle(GeoVertex *a,GeoVertex *b,GeoVertex *c)
{
    GeoVertex v[12],tmp[12];
    unsigned i,p,n=3,clipped=0;
    if(TAG_KEPT(s.tag))keep_tagged(a,b,c);
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
 * plane (w - z < 0). In the wider or taller view it's clipped there
 * instead: that view sees past the DS's edges, where a big ground polygon
 * reaching past the far plane would leave a black hole (Route 211 West's
 * river, from 378,532). */
static int far_hidden(unsigned outside)
{
    if((outside & 0x20u) && !(s.attr & (1u<<12)) && s.wideNum==s.wideDen && s.tallNum==s.tallDen){sDiag.clipped++;return 1;}
    return 0;
}
/* Nothing of the polygon is drawn: all its vertices outside the same clip
 * plane (most of a big map), or crossing the far plane (far_hidden).
 * any/all: its vertices' outcodes OR'd/AND'ed. */
static int polygon_hidden(unsigned any,unsigned all)
{
    /* (With the sun shadows on, one outside only the view's sides goes on
     * to stage_triangle, which keeps it for them; a marked one too.) */
    if(all && !((sShadowCapture || TAG_KEPT(s.tag)) && !(all&0x30u))){sDiag.clipped++;return 1;}
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
    sLastTriFirst=scene->count;sLastTriAdded=0;
    if(!sStagedCount)return;
    vertices=sStagedClipped?sStagedVertices:newVertices;
    /* (the trees' sun shadows, NITRO_GX3D_TAG_SHADE, are the port's own
     * polygons: they take none of the DS's room from the game's) */
    if(s.tag!=NITRO_GX3D_TAG_SHADE){
        if(s.framePolygons>=2048u || s.frameVertices+vertices>6144u){
            scene->dropped++;
            sDiag.overLimit++;
            return;
        }
        s.framePolygons++;
        s.frameVertices+=vertices;
    }
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
        sLastTriAdded++;
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
    uint32_t tag;            /* (NitroPort_Gx3dTag's, as its triangles have it) */
    uint8_t count;           /* 3, or 4: triangles (0, 1, 2) and (0, 2, 3) */
    uint8_t onScreen;        /* all its vertices in front of the camera (w > 0) */
    uint16_t ntri;           /* its triangles in its scene: ntri from tri0 */
    uint32_t tri0;
    /* (screenOk: all its vertices inside the view, with the viewport
     * then; their depths as to_screen had them; replay_blended) */
    uint32_t sdepth[4];
    uint32_t viewport;
    uint8_t screenOk, pad[3];
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
#define ORDER_SLACK 3.0f   /* (match_records: the order's pair, if no further than this from the nearest's) */
static int sInterpWanted, sInterp;
static PolyRecord *sRec[2];              /* [sRecBuild]: this scene's; the other: the scene before's */
/* What matching looks at of each record (match_records), apart: the
 * window's search (up to MATCH_WINDOW either side, for every polygon the
 * order doesn't pair where the scene's motion puts it: on the Cycling Road
 * at a ride, the ground's rows each moving their own way, many) read a
 * whole record for each, 160 bytes, a cache line or two apiece on the 3DS:
 * half the in-between scene's time, 3-9 ms a scene (2026-11-02). Here 8
 * and 32 bytes, filled as the records are (the same bytes: the same
 * pairs, bit for bit). */
typedef struct RecTag {
    uint32_t sig;
    uint8_t count, onScreen;
    uint16_t unused;
    float px0, py0;           /* (its first corner: most of the window's are far, told by it alone) */
    float w0;                 /* (and its w: static_rematch's first look, by the camera's move) */
    float px2, py2, w2;       /* (and its third: a polygon's own, not a neighbour's sharing a corner) */
} RecTag;
typedef struct RecPos {
    float px[4], py[4];
} RecPos;
static RecTag *sTag[2];
static RecPos *sPos[2];
static unsigned sRecCount[2], sRecBuild;
static int sRecBeforeValid;
/* Each mailbox slot's in-between scene (the one between its scene and the
 * scene before), made before the slot is published: with the scene, the
 * display has its in-between scene too (NitroPort_Gx3dMidOf). */
static NitroGx3dScene *sMids[3];
static int sMidOk[3];
static int32_t *sMatch;                  /* for each of this scene's polygons, the one before's, or -1 */
static unsigned sMidMatched, sMidPolygons;

static uint32_t viewport_key(void)
{
    return (uint32_t)(s.viewport[0]&255)|((uint32_t)(s.viewport[1]&255)<<8)|
           ((uint32_t)(s.viewport[2]&255)<<16)|((uint32_t)(s.viewport[3]&255)<<24);
}
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
    /* All outside one plane: not drawn, as without the sun shadows (they
     * keep it, outside only the view's sides; the geometry thread passes
     * over lists of them): the in-between scenes are the same either way. */
    if(a->outside & b->outside & c->outside & (d?d->outside:0x3Fu))return;
    v[0]=a;v[1]=b;v[2]=c;v[3]=d;
    r=&sRec[sRecBuild][sRecCount[sRecBuild]++];
    for(k=0;k<n;k++){
        memcpy(r->clip[k],v[k]->clip,sizeof(r->clip[k]));
        r->st[k][0]=v[k]->s;r->st[k][1]=v[k]->t;
        r->color[k]=v[k]->lit?v[k]->color:shade(v[k]->normal);
    }
    r->count=(uint8_t)n;
    r->attr=s.attr;r->texparam=s.texparam;r->plttbase=s.plttbase;
    r->tag=s.tag;
    r->sig=record_signature(n);
    r->tri0=sLastTriFirst;r->ntri=(uint16_t)sLastTriAdded;
    r->viewport=viewport_key();
    r->screenOk=!(a->outside|b->outside|c->outside|(d?d->outside:0u));
    for(k=0;k<n && r->screenOk;k++){
        if(!v[k]->hasScreen){r->screenOk=0;break;}
        r->sdepth[k]=v[k]->screen.depth;
    }
    r->onScreen=1;
    for(k=0;k<n;k++){
        if(r->clip[k][3]<=0){r->onScreen=0;break;}
        r->px[k]=(float)r->clip[k][0]/(float)r->clip[k][3]*128.0f;
        r->py[k]=(float)r->clip[k][1]/(float)r->clip[k][3]*96.0f;
    }
    if(!r->onScreen)r->screenOk=0;   /* (w = 0, all of it at 0: inside, but no place on the screen) */
    {
        size_t at=(size_t)(r-sRec[sRecBuild]);
        RecTag *tag=&sTag[sRecBuild][at];
        tag->sig=r->sig;tag->count=r->count;tag->onScreen=r->onScreen;tag->unused=0;
        memcpy(&tag->px0,&r->px[0],sizeof(tag->px0));
        memcpy(&tag->py0,&r->py[0],sizeof(tag->py0));
        tag->w0=(float)r->clip[0][3];
        memcpy(&tag->px2,&r->px[2],sizeof(tag->px2));
        memcpy(&tag->py2,&r->py[2],sizeof(tag->py2));
        tag->w2=(float)r->clip[2][3];
        memcpy(sPos[sRecBuild][at].px,r->px,sizeof(r->px));
        memcpy(sPos[sRecBuild][at].py,r->py,sizeof(r->py));
    }
}
/* A polygon as the game would have drawn it with these vertices (clip),
 * into dest_scene(): clipped, culled, its triangles staged and stored. */
static void replay_polygon(const PolyRecord *r,int32_t clip[4][4])
{
    GeoVertex v[4];
    unsigned k,any=0,all=0x3Fu;
    uint32_t attr=s.attr,texparam=s.texparam,plttbase=s.plttbase,tag=s.tag;
    s.attr=r->attr;s.texparam=r->texparam;s.plttbase=r->plttbase;s.tag=r->tag;
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
    s.attr=attr;s.texparam=texparam;s.plttbase=plttbase;s.tag=tag;
}
/* to_screen's viewport transform, from px and py (DS pixels from the
 * middle): x = (X / W + 1) * width / 2 + x0, y = (1 - Y / W) * height / 2
 * + top, in 1/256 pixel; worked out once a scene (blend_viewport). */
typedef struct BlendView { float ax, bx, ay, by; } BlendView;
static BlendView blend_viewport(void)
{
    BlendView b;
    float width=(float)((s.viewport[2]-s.viewport[0]+1)&0x1FF);
    float height=(float)((s.viewport[3]-s.viewport[1]+1)&0xFF);
    b.ax=width;
    b.bx=width*128.0f+(float)(s.viewport[0]&255)*256.0f+0.5f;
    b.ay=-height*(4.0f/3.0f);
    b.by=height*128.0f+(float)((191-(s.viewport[3]&255))&255)*256.0f+0.5f;
    return b;
}
/* The same, for a polygon wholly inside the view in both scenes (p's and
 * c's screenOk, the same viewport), without clipping it or projecting it
 * again: halfway between in clip space is inside the view too (it's
 * convex), and on the screen it's between where the two scenes have it
 * (px, py, as matching has them; the depths to_screen gave), weighed by
 * their w: the projection of the halfway point, x = (X0 + X1) / (W0 +
 * W1), the same for y and depth, to the rounding. Most of a scene's
 * polygons, and most of the in-between scene's time (two thirds on the
 * Cycling Road at a ride, 5.5-7.5 ms a scene, 2026-11-02): one division a
 * vertex here rather than three, and none of the clipping's copies.
 * clip: halfway, as replay_polygon has it. */
static void replay_blended(const PolyRecord *p,const PolyRecord *c,int32_t clip[4][4],const BlendView *bv)
{
    NitroGx3dVertex v[4];
    unsigned k;
    uint32_t attr=s.attr,texparam=s.texparam,plttbase=s.plttbase,tag=s.tag;
    s.attr=c->attr;s.texparam=c->texparam;s.plttbase=c->plttbase;s.tag=c->tag;
    for(k=0;k<c->count;k++){
        float w0=(float)p->clip[k][3],w1=(float)c->clip[k][3];
        float r=1.0f/(w0+w1),t=w1*r,dz;
        float px=p->px[k]+(c->px[k]-p->px[k])*t,py=p->py[k]+(c->py[k]-p->py[k])*t;
        memset(&v[k],0,sizeof(v[k]));
        memcpy(v[k].clip,clip[k],sizeof(v[k].clip));
        dz=(float)((int32_t)c->sdepth[k]-(int32_t)p->sdepth[k])*t;
        v[k].x=floor_small(px*bv->ax+bv->bx);
        v[k].y=floor_small(py*bv->ay+bv->by);
        v[k].depth=(uint32_t)((int32_t)p->sdepth[k]+(int32_t)(dz+(dz>=0?0.5f:-0.5f)));
        v[k].inv_w=2.0f*r;
        v[k].s=c->st[k][0]/16.0f;v[k].t=c->st[k][1]/16.0f;
        v[k].color=c->color[k];
    }
    begin_polygon();
    sPolygonFront=clip_front(clip[0],clip[1],clip[2]);
    add_screen_triangle(&v[0],&v[1],&v[2]);
    if(c->count==4)add_screen_triangle(&v[0],&v[2],&v[3]);
    end_polygon(1);
    s.attr=attr;s.texparam=texparam;s.plttbase=plttbase;s.tag=tag;
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
static float poly_distance(const RecPos *p,const RecPos *c,unsigned count,float dx,float dy,float limit)
{
    float d=0.0f,stop=limit*(float)count;
    unsigned k;
    for(k=0;k<count;k++){
        d+=fabsf(c->px[k]-(p->px[k]+dx))+fabsf(c->py[k]-(p->py[k]+dy));
        if(d>=stop)return limit;
    }
    return d/(float)count;
}
/* Whether poly_distance would stop at the first corner (its first step
 * alone at the limit or past it: the limit), from the tags alone: the
 * window's search, most of whose polygons of a kind are far, without
 * their corners' table. The same sums, the same answer. */
static int first_far(const RecTag *p,const RecTag *c,float dx,float dy,float limit)
{
    float d=0.0f,stop=limit*(float)c->count;
    d+=fabsf(c->px0-(p->px0+dx))+fabsf(c->py0-(p->py0+dy));
    return d>=stop;
}
/* The scene's motion as match_records last worked it out (DS pixels, as
 * PolyRecord.px/py: x / w * 128, y / w * 96); valid with 8 pairs or more. */
static int sMotionValid;
static uint8_t *sPUsed;   /* (the scene before's records paired with one of this one's) */
static int32_t sOrderOut[MAX_RECORDS], sOwner[MAX_RECORDS];   /* (match_records: the order's pairs; each before's pair) */
static float sMotionX, sMotionY;
/* The motion the scene before had (sMotionValid then): the camera's pace
 * at a walk or a run is steady from one scene to the next. */
static int sPrevMotionValid;
static float sPrevMotionX, sPrevMotionY;
/* How many of a sample of this scene's polygons (up to 64, spread through
 * it) have one of their kind in the scene before, near where the order
 * pairs them, where the motion (dx, dy) puts it (2 DS pixels, a vertex on
 * average). */
static unsigned motion_support(const RecTag *P,const RecPos *PP,unsigned m,const RecTag *C,const RecPos *CP,unsigned n,
                               const int32_t *out,float dx,float dy)
{
    unsigned j,found=0,step=n/64u+1u;
    for(j=0;j<n;j+=step){
        int center=out[j]>=0?out[j]:(int)j,lo=center-MATCH_WINDOW,hi=center+MATCH_WINDOW,q;
        if(!C[j].onScreen)continue;
        if(lo<0)lo=0;
        if(hi>(int)m-1)hi=(int)m-1;
        for(q=lo;q<=hi;q++){
            if(P[q].sig!=C[j].sig || !P[q].onScreen || P[q].count!=C[j].count)continue;
            if(first_far(&P[q],&C[j],dx,dy,2.0f*MATCH_SURE))continue;
            if(poly_distance(&PP[q],&CP[j],C[j].count,dx,dy,2.0f*MATCH_SURE)<2.0f*MATCH_SURE){found++;break;}
        }
    }
    return found;
}
static NitroGx3dThreadOps sOps;   /* (NitroPort_Gx3dStartThread) */
/* (the [perf] in-between matching line, NitroPort_Gx3dMatchWork) Its
 * time in its four parts: the order's pairs, the scene's motion, each
 * again by where it is (the window's search among them), one each; the
 * polygons, those searched for, the window's places looked at. Only the
 * geometry thread writes them. */
static uint64_t sMatchPart[4];
static uint32_t sMatchPolygons, sMatchSearched, sMatchScanned, sMatchQuick;
static uint64_t match_ticks(void)
{
    return sOps.ticks?sOps.ticks(sOps.context):0;
}
void NitroPort_Gx3dMatchWork(uint64_t part[4],uint32_t *polygons,uint32_t *searched,uint32_t *scanned)
{
    unsigned k;
    for(k=0;k<4;k++)part[k]=*(volatile uint64_t *)&sMatchPart[k];
    if(polygons)*polygons=*(volatile uint32_t *)&sMatchPolygons;
    if(searched)*searched=*(volatile uint32_t *)&sMatchSearched;
    if(scanned)*scanned=*(volatile uint32_t *)&sMatchScanned;
}
/* (match_records) The scene before's polygon q for this one's j, one of
 * its kind, in front of the camera, a pixel at most from where the motion
 * (dx, dy) puts it; not `not' (one already looked at). */
#define MATCH_LOCAL 2.0f
static int match_fits(const RecTag *P,const RecPos *PP,unsigned m,const RecTag *C,const RecPos *CP,
                      unsigned j,int q,int not,float dx,float dy)
{
    if(q<0 || q>=(int)m || q==not)return 0;
    if(P[q].sig!=C[j].sig || !P[q].onScreen || P[q].count!=C[j].count)return 0;
    return poly_distance(&PP[q],&CP[j],C[j].count,dx,dy,MATCH_SURE)<MATCH_SURE;
}
/* Whether two records are the same polygon of a model, by its texture's
 * place on it: every corner's texture coordinates the same (a model drawn
 * again, its parts in its order: the same; another part of its kind,
 * another face of a ball, mostly not; a map's tiles of one kind often all
 * the same, telling nothing). */
static int same_st(const PolyRecord *a,const PolyRecord *b)
{
    unsigned k;
    if(a->count!=b->count)return 0;
    for(k=0;k<a->count;k++)if(a->st[k][0]!=b->st[k][0] || a->st[k][1]!=b->st[k][1])return 0;
    return 1;
}
#define ORDER_SAME_FAR 2.0f   /* (match_records: the order's pair, the same polygon, if no further than this many times near) */
static unsigned match_records(const RecTag *P,const RecPos *PP,const PolyRecord *PR,unsigned m,
                              const RecTag *C,const RecPos *CP,const PolyRecord *CR,unsigned n,int32_t *out)
{
    unsigned i=0,j=0,matched=0,moves=0;
    float dx=0.0f,dy=0.0f,near;
    uint64_t t0=match_ticks(),t1,t2,t3;
    float ldx=0.0f,ldy=0.0f;   /* (where the last pair moved) */
    int haveLocal=0;
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
    t1=match_ticks();
    /* the scene's motion: the middle of the pairs' moves (some of them) */
    for(j=0;j<n && moves<256u;j+=1u+n/256u){
        if(out[j]<0 || !P[out[j]].onScreen || !C[j].onScreen)continue;
        mx[moves]=CP[j].px[0]-PP[out[j]].px[0];
        my[moves]=CP[j].py[0]-PP[out[j]].py[0];
        moves++;
    }
    if(moves>=8u){dx=median_of(mx,moves);dy=median_of(my,moves);}
    /* The order's pairs give the motion only while the order holds. A
     * map's block coming into the view puts its tiles among the rest,
     * all of one kind, and every pair after it is a tile with another
     * (Route 205's lawn at a run, 2026-10-27: its tiles halfway to the
     * wrong ones, holes and stretched stairs in the in-between scene).
     * So the motion that most of a sample of polygons fits: the order's
     * or the scene before's (the camera's pace is steady); none only
     * without either. (Not none against a moving camera's: a floor moving
     * a tile a scene, the bike's top gear, has every tile where its
     * neighbour was, and none fitted them all: the floor at 30.) The
     * order's wins a tie. */
    if(n>=16u){
        float cx[3],cy[3];
        unsigned c=0,k,best=0,bestSupport=0;
        if(moves>=8u){cx[c]=dx;cy[c]=dy;c++;}
        if(sPrevMotionValid && (sPrevMotionX!=0.0f || sPrevMotionY!=0.0f) &&
           (!c || sPrevMotionX!=dx || sPrevMotionY!=dy)){cx[c]=sPrevMotionX;cy[c]=sPrevMotionY;c++;}
        if(!c){cx[c]=0.0f;cy[c]=0.0f;c++;}
        for(k=0;k<c;k++){
            unsigned support=motion_support(P,PP,m,C,CP,n,out,cx[k],cy[k]);
            if(support>bestSupport){bestSupport=support;best=k;}
        }
        if(bestSupport>0){
            if(moves<8u || cx[best]!=dx || cy[best]!=dy)moves=8u;   /* (valid: it fits) */
            dx=cx[best];dy=cy[best];
        }
    }
    sMotionValid=moves>=8u;
    sPrevMotionValid=sMotionValid;
    sPrevMotionX=dx;sPrevMotionY=dy;
    sMotionX=dx;sMotionY=dy;
    /* (a figure that moves on its own, the player against the camera's
     * motion: as far again as the scene moves) */
    near=MATCH_NEAR+1.5f*(fabsf(dx)+fabsf(dy));
    memcpy(sOrderOut,out,n*sizeof(*out));
    t2=match_ticks();
    /* each again, by where it is */
    for(j=0;j<n;j++){
        int center=out[j]>=0?out[j]:(int)j+offset,order=out[j],lo,hi,q,best=-1;
        float bestD=near;
        int shifted=(int)j+offset;
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
           poly_distance(&PP[out[j]],&CP[j],C[j].count,dx,dy,MATCH_SURE)<MATCH_SURE){
            offset=out[j]-(int)j;matched++;
            ldx=C[j].px0-P[out[j]].px0;ldy=C[j].py0-P[out[j]].py0;haveLocal=1;
            continue;
        }
        /* The same, one off the order as the last pair was: polygons going
         * out of the view at one side and coming in at the other shift the
         * order by a few, and every pair after it is the order's
         * neighbour of its kind (on the Cycling Road at a ride, two in five
         * of the order's pairs, each searched for in the window: 60,000
         * places a scene, 3-7 ms on the 3DS, 2026-11-02). Then where the
         * pair before moved (the ground's rows each move their own way in
         * the perspective: a pixel or two off the scene's motion, near the
         * screen's bottom or top), if it's no more than MATCH_LOCAL from the
         * scene's. Either a pixel at most from where it puts it. */
        if(match_fits(P,PP,m,C,CP,j,shifted,out[j],dx,dy)){
            out[j]=shifted;offset=shifted-(int)j;matched++;sMatchQuick++;
            ldx=C[j].px0-P[shifted].px0;ldy=C[j].py0-P[shifted].py0;haveLocal=1;
            continue;
        }
        if(haveLocal && fabsf(ldx-dx)+fabsf(ldy-dy)<=MATCH_LOCAL){
            int pick=-1;
            if(match_fits(P,PP,m,C,CP,j,out[j],-1,ldx,ldy))pick=out[j];
            else if(match_fits(P,PP,m,C,CP,j,shifted,out[j],ldx,ldy))pick=shifted;
            if(pick>=0){
                out[j]=pick;offset=pick-(int)j;matched++;sMatchQuick++;
                ldx=C[j].px0-P[pick].px0;ldy=C[j].py0-P[pick].py0;
                continue;
            }
        }
        lo=center-MATCH_WINDOW;hi=center+MATCH_WINDOW;
        if(lo<0)lo=0;
        if(hi>(int)m-1)hi=(int)m-1;
        sMatchSearched++;
        sMatchScanned+=(uint32_t)(hi-lo+1);
        for(q=lo;q<=hi;q++){
            float d;
            if(P[q].sig!=C[j].sig || !P[q].onScreen || P[q].count!=C[j].count)continue;
            if(first_far(&P[q],&C[j],dx,dy,bestD))continue;
            d=poly_distance(&PP[q],&CP[j],C[j].count,dx,dy,bestD);
            if(d<bestD){bestD=d;best=q;}
        }
        /* The order's pair when it's nearly as near as the nearest: a model
         * that turns or sways (the starters' Poké Balls in the briefcase,
         * wobbling) has its parts a pixel or two from where they were, and
         * as near a part of its kind (the ball's other faces, the other
         * balls): taken, the in-between scene showed bits of the wrong
         * ones, grey blocks (2026-10-27). Only while the scene itself
         * stays (a map scrolling, the order shifts as its tiles come and go:
         * a cliff's tile paired a little off left a crack). */
        if(order>=0 && order!=best && fabsf(dx)+fabsf(dy)<MATCH_SURE &&
           P[order].onScreen && P[order].count==C[j].count){
            float d=poly_distance(&PP[order],&CP[j],C[j].count,dx,dy,near);
            if(d<near && (best<0 || d<=bestD+ORDER_SLACK)){best=order;bestD=d;}
        }
        /* The order's pair when it's the same polygon (same_st) and the
         * nearest of its kind isn't: a model moving or turning fast, its
         * parts of one kind each nearer another's place in the scene
         * before than its own (the starters' Poké Balls jumping out of
         * the briefcase, spinning, as the camera closed in: their faces
         * paired with each other's, and with the case's, and the
         * in-between scenes showed grey specks on the balls and the case's
         * handle torn, a video of 2026-11-06). As far as twice near. */
        if(order>=0 && order!=best && P[order].onScreen && P[order].count==C[j].count &&
           same_st(&PR[order],&CR[j]) && (best<0 || !same_st(&PR[best],&CR[j]))){
            float d=poly_distance(&PP[order],&CP[j],C[j].count,dx,dy,ORDER_SAME_FAR*near);
            if(d<ORDER_SAME_FAR*near){best=order;bestD=d;}
        }
        /* What the camera follows stays where it was on the screen as the
         * scene moves: the player, and one who follows him. The order's
         * pair where it was (a pixel at most), when that fits better than
         * anything the scene's motion explains: the scene moving further
         * than half the gap between the two (at a run, with a scene left
         * out), Cheryl behind the player in Eterna Forest took his place
         * from the scene before, and jumped about (2026-10-27). */
        if(order>=0 && order!=best && P[order].onScreen && P[order].count==C[j].count){
            /* (only when the motion's fit is clearly off: a map's floor
             * moves unevenly in the perspective, and a tile's neighbour
             * of its kind, paired by an order that shifted, can be where
             * it is now when the scene moved by a tile) */
            float clearly=fmaxf(4.0f*MATCH_SURE,0.5f*(fabsf(dx)+fabsf(dy)));
            float still=poly_distance(&PP[order],&CP[j],C[j].count,0.0f,0.0f,MATCH_SURE);
            if(still<MATCH_SURE && (best<0 || bestD>clearly))best=order;
        }
        out[j]=best;
        if(best>=0){
            offset=best-(int)j;matched++;
            ldx=C[j].px0-P[best].px0;ldy=C[j].py0-P[best].py0;haveLocal=1;
        }
    }
    /* One each: a polygon of the scene before paired with two of this
     * one's goes to the nearer (where the scene's motion puts it, or where
     * it was: what the camera follows; a tie, the order's pair, else the
     * first); the other has none, and is where it is now. A figure new in
     * the scene beside one of its kind (the follower Pokémon's Poké Ball
     * made again beside the player as it closed, 2026-10-29) took the
     * player's pair, by the order and by nearness: the in-between scene
     * drew it halfway to the player, a flicker to the side. */
    t3=match_ticks();
    for(j=0;j<m;j++)sOwner[j]=-1;
    for(j=0;j<n;j++){
        int q=out[j],o;
        float dj,doo;
        if(q<0)continue;
        o=sOwner[q];
        if(o<0){sOwner[q]=(int32_t)j;continue;}
        dj=fminf(poly_distance(&PP[q],&CP[j],C[j].count,dx,dy,1e9f),poly_distance(&PP[q],&CP[j],C[j].count,0.0f,0.0f,1e9f));
        doo=fminf(poly_distance(&PP[q],&CP[o],C[o].count,dx,dy,1e9f),poly_distance(&PP[q],&CP[o],C[o].count,0.0f,0.0f,1e9f));
        {
            /* (the one that's the same polygon as it, same_st, if only one
             * is: as above; two the same, two copies of a model, by where
             * they are) */
            int sj=same_st(&PR[q],&CR[j]),so=same_st(&PR[q],&CR[o]);
            if(sj!=so){dj=sj?0.0f:1.0f;doo=so?0.0f:1.0f;}
        }
        if(dj<doo || (dj==doo && sOrderOut[j]==q && sOrderOut[o]!=q)){
            out[o]=-1;sOwner[q]=(int32_t)j;
        }else{
            out[j]=-1;
        }
        matched--;
    }
    if(sOps.ticks){
        uint64_t t4=match_ticks();
        sMatchPart[0]+=t1-t0;sMatchPart[1]+=t2-t1;sMatchPart[2]+=t3-t2;sMatchPart[3]+=t4-t3;
    }
    sMatchPolygons+=n;
    return matched;
}
/* The in-between scene of the scene just finished (`now', its records
 * sRec[sRecBuild]) and the one before (sRec[sRecBuild ^ 1]), into sMid.
 * Whether there is one. */
/* (PIXEL PERFECT's sprites) A polygon's soft flags (NitroGx3dTriangle.
 * soft): the ways it moved on the screen by other than a whole number of
 * pixels a frame since the scene before (half the scene's move: the
 * in-between scene shows it halfway). Only what can be a sprite there
 * (sprite_texel_a_pixel): textured, every vertex at one depth, in the
 * 240 lines' view. The camera's perspective moves each row of the ground
 * by its own amount (running: 1.94 pixels a frame at the top of Eterna's
 * screen, 2.2 at the bottom, 2 at the player's row): a sprite snapped to
 * whole pixels there fell behind its ground and caught up a pixel at a
 * time, about 10 times a second near the bottom (a video at 120 frames a
 * second, 2026-10-30), the people's jiggle; on half pixels it moves with
 * its ground, a little soft that way while it moves. Still (the player,
 * whom the camera follows) or a whole number of pixels a frame (the
 * player's row), snapped as before. */
#define SOFT_NEAR 0.02f   /* (pixels a frame off a whole number: under it, a pixel's catching up every 50 frames or rarer) */
static uint32_t soft_of(const PolyRecord *p,const PolyRecord *c,const NitroGx3dScene *now)
{
    float dx,dy;
    unsigned k;
    uint32_t soft=0;
    if(now->screen_height!=240u || !((c->texparam>>26)&7u) || !p->onScreen || !c->onScreen || p->count!=c->count)
        return 0;
    for(k=1;k<c->count;k++)if(c->clip[k][3]!=c->clip[0][3])return 0;
    dx=0.5f*(c->px[0]-p->px[0])*((float)now->screen_width/256.0f);
    dy=0.5f*(c->py[0]-p->py[0])*(240.0f/192.0f);
    if(fabsf(dx-floorf(dx+0.5f))>SOFT_NEAR)soft|=NITRO_GX3D_SOFT_X;
    if(fabsf(dy-floorf(dy+0.5f))>SOFT_NEAR)soft|=NITRO_GX3D_SOFT_Y;
    return soft;
}
/* The camera's move in clip space. The field's camera keeps its angle and
 * only moves: every vertex of what stays where it is in the world (the
 * map, its props, the people standing) moves by the same 4 numbers in
 * clip space between two scenes, the far and the near alike, while on the
 * screen the near ground moves further (the perspective). Matching by
 * the screen's motion alone paired a ledge's steps (a row of one kind
 * every two DS pixels near the screen's bottom, moving 6 pixels a scene
 * against the scene's 4) one step off, and the in-between frames had
 * white lines along the rocks riding the Cycling Road (videos of
 * 2026-11-02, IMG_2955). sClipMotion: the middle of the pairs' moves
 * (their first corners), valid when at least half of them fit it on every
 * corner. On the Cycling Road they fit it exactly, to a clip unit or two
 * (rec_river: 99.6% of the corners); STATIC_FIT allows w / 16384 (1/128
 * of a DS pixel) and STATIC_SLACK clip units. Anything looser let things
 * that only happen to be where the move would put them in: the player's
 * shadow, 4 quads each as tall as the bike's move in a scene, paired a
 * quad off. Not valid (the camera turning or zooming: a battle's start, a
 * cut-scene): matching as it was. */
#define STATIC_FIT 14
#define STATIC_SLACK 8
#define STATIC_NEAR 0.0625f   /* (DS pixels: static_rematch's first look in the window, a corner's, well over STATIC_FIT's) */
#define STATIC_SAMPLES 256u
static int sClipMotionValid;
static int32_t sClipMotion[4];
static int32_t sStaticPair[MAX_RECORDS], sStaticOwner[MAX_RECORDS];
/* How far (clip units, summed) the scene before's p is from where the
 * camera's move mo puts c then, on every corner; -1 not within STATIC_FIT
 * on every one. */
static float static_misfit(const PolyRecord *p,const PolyRecord *c,const int32_t mo[4])
{
    unsigned k,i;
    int64_t e=0;
    if(p->count!=c->count || !p->onScreen || !c->onScreen)return -1.0f;
    for(k=0;k<c->count;k++){
        int64_t tol=((int64_t)c->clip[k][3]>>STATIC_FIT)+STATIC_SLACK;
        for(i=0;i<4;i++){
            int64_t d=(int64_t)c->clip[k][i]-mo[i]-p->clip[k][i];
            if(d<0)d=-d;
            if(d>tol)return -1.0f;
            e+=d;
        }
    }
    return (float)e;
}
/* The same from the tags alone (the first and third corners, their x, y
 * and w, as floats: a clip unit or two off): every pair's look, without
 * reading the records (a cache line or three each on the 3DS, after the
 * matching read the tags). Two corners: a neighbour of its kind shares
 * one with it (on the PC's rides, 2 of 5 in-between frames with holes left
 * with the first alone). */
static float static_corner_misfit(float pw,float px,float py,float cw,float cx,float cy,const int32_t mo[4])
{
    float tol=cw*(1.0f/(float)(1<<STATIC_FIT))+(float)STATIC_SLACK+2.0f,d,e;
    d=fabsf(cw-(float)mo[3]-pw);
    if(d>tol)return -1.0f;
    e=d;
    d=fabsf((cx*cw-px*pw)*(1.0f/128.0f)-(float)mo[0]);
    if(d>tol)return -1.0f;
    e+=d;
    d=fabsf((cy*cw-py*pw)*(1.0f/96.0f)-(float)mo[1]);
    if(d>tol)return -1.0f;
    return e+d;
}
static float static_tag_misfit(const RecTag *p,const RecTag *c,const int32_t mo[4])
{
    float e0,e2;
    if(p->count!=c->count || !p->onScreen || !c->onScreen)return -1.0f;
    e0=static_corner_misfit(p->w0,p->px0,p->py0,c->w0,c->px0,c->py0,mo);
    if(e0<0.0f)return -1.0f;
    e2=static_corner_misfit(p->w2,p->px2,p->py2,c->w2,c->px2,c->py2,mo);
    if(e2<0.0f)return -1.0f;
    return e0+e2;
}
static int clip_motion(const RecTag *PT,const RecTag *CT,unsigned n,const int32_t *out,int32_t mo[4])
{
    static float sample[3][STATIC_SAMPLES];
    static uint16_t at[STATIC_SAMPLES];
    unsigned j,i,count=0,fit=0,step=n/STATIC_SAMPLES+1u;
    for(j=0;j<n && count<STATIC_SAMPLES;j+=step){
        int q=out[j];
        if(q<0 || !CT[j].onScreen || !PT[q].onScreen || PT[q].count!=CT[j].count)continue;
        sample[0][count]=(CT[j].px0*CT[j].w0-PT[q].px0*PT[q].w0)*(1.0f/128.0f);
        sample[1][count]=(CT[j].py0*CT[j].w0-PT[q].py0*PT[q].w0)*(1.0f/96.0f);
        sample[2][count]=CT[j].w0-PT[q].w0;
        at[count++]=(uint16_t)j;
    }
    if(count<16u)return 0;
    for(i=0;i<3;i++){
        float v=median_of(sample[i],count);
        if(v>2.0e9f || v<-2.0e9f)return 0;
        mo[i==2?3:i]=(int32_t)floorf(v+0.5f);
    }
    /* (z, not in the tags: as w for now, clip_motion_z finds it) */
    mo[2]=mo[3];
    /* (the camera still: matching as it was, the scene's own motion none) */
    if(!mo[0] && !mo[1] && !mo[3])return 0;
    for(i=0;i<count;i++){
        j=at[i];
        if(static_tag_misfit(&PT[out[j]],&CT[j],mo)>=0.0f)fit++;
    }
    return fit*2u>=count;
}
/* (clip_motion's z: the middle of the fitting pairs' z moves, from 16 of
 * their records at most) */
static void clip_motion_z(const PolyRecord *P,const PolyRecord *C,const RecTag *PT,const RecTag *CT,
                          unsigned n,const int32_t *out,int32_t mo[4])
{
    static float zs[16];
    unsigned j,count=0,step=n/16u+1u;
    for(j=0;j<n && count<16u;j+=step){
        int q=out[j];
        if(q<0 || static_tag_misfit(&PT[q],&CT[j],mo)<0.0f)continue;
        zs[count++]=(float)((int64_t)C[j].clip[0][2]-P[q].clip[0][2]);
    }
    if(count)mo[2]=(int32_t)floorf(median_of(zs,count)+0.5f);
}
/* (static_rematch) What the camera follows: where it was on the screen, a
 * pixel at most (on average, a corner, as match_records has it), and as
 * far from the camera (w) to a half of the camera's own move that way
 * (the move mo); a row of one kind in the perspective, a row where its
 * neighbour was, is nearer or further. */
static int near_still(const PolyRecord *p,const PolyRecord *c,const int32_t mo[4])
{
    unsigned k;
    float d=0.0f;
    int64_t mw=mo[3]<0?-(int64_t)mo[3]:(int64_t)mo[3];
    if(p->count!=c->count || !p->onScreen || !c->onScreen)return 0;
    for(k=0;k<c->count;k++){
        int64_t dw=(int64_t)c->clip[k][3]-p->clip[k][3];
        if(dw<0)dw=-dw;
        if(dw>mw/2+((int64_t)c->clip[k][3]>>STATIC_FIT)+STATIC_SLACK)return 0;
        d+=fabsf(c->px[k]-p->px[k])+fabsf(c->py[k]-p->py[k]);
    }
    return d<MATCH_SURE*(float)c->count;
}
/* With the camera's move (mo): each polygon whose pair isn't where the
 * move puts it, paired with the one of its kind that is (MATCH_WINDOW
 * either side of its pair, or of where the pairs before it were), the
 * nearest; one taken that way is no other's. What the camera follows (the
 * player: where its pair was on the screen) keeps its pair, and what the
 * move doesn't fit (the people moving) the pair it had, unless that one
 * is another's exactly. Each pair's misfit (sStaticErr) decides who has a
 * polygon wanted twice (the same polygon drawn twice over: the one after
 * takes the other). Its own pair's fit, from the tags (two corners);
 * another's, on every corner. */
static float sStaticErr[MAX_RECORDS];
static void static_rematch(const PolyRecord *P,unsigned m,const PolyRecord *C,unsigned n,int32_t *out,const int32_t mo[4])
{
    const RecTag *PT=sTag[sRecBuild^1u],*CT=sTag[sRecBuild];
    unsigned j;
    int offset=0;
    for(j=0;j<m;j++)sStaticOwner[j]=-1;
    for(j=0;j<n;j++){
        int q=out[j],center,lo,hi,k,best=-1,o;
        float bestE=0.0f,e;
        sStaticPair[j]=-1;
        if(!CT[j].onScreen)continue;
        /* (taken: by one before it that fits it no worse) */
#define STATIC_TAKEN(k,e) (sStaticOwner[k]>=0 && sStaticErr[sStaticOwner[k]]<=(e))
        if(q>=0 && (e=static_tag_misfit(&PT[q],&CT[j],mo))>=0.0f && !STATIC_TAKEN(q,e)){best=q;bestE=e;}
        else if(q>=0 && near_still(&P[q],&C[j],mo)){
            /* (what the camera follows: its own pair, no other, and none
             * takes it from it; as it was paired. The player's shadow is
             * 4 quads, and at a ride its quad below fitted the move to the
             * one above it the scene before exactly, the bike's move a
             * scene being the quad's size: it was torn in the in-between
             * frames. The camera lags the player a little: a pixel.) */
            if(!STATIC_TAKEN(q,-1.0f)){best=q;bestE=-1.0f;}
        }else{
            /* (where the move puts its first corner on the screen then: the
             * window's polygons told by their tags alone, most of them) */
            float wq=CT[j].w0-(float)mo[3],fx,fy;
            if(wq<=0.0f)continue;
            fx=(CT[j].px0*CT[j].w0*(1.0f/128.0f)-(float)mo[0])/wq*128.0f;
            fy=(CT[j].py0*CT[j].w0*(1.0f/96.0f)-(float)mo[1])/wq*96.0f;
            center=q>=0?q:(int)j+offset;
            lo=center-MATCH_WINDOW;hi=center+MATCH_WINDOW;
            if(lo<0)lo=0;
            if(hi>(int)m-1)hi=(int)m-1;
            /* (its pair the order's and the same polygon, same_st: only
             * another as much the same takes its place; a map's repeated
             * rows, the same as each other, as before) */
            int same=q>=0 && q==sOrderOut[j] && same_st(&P[q],&C[j]);
            for(k=lo;k<=hi;k++){
                if(k==q || PT[k].sig!=CT[j].sig || !PT[k].onScreen || PT[k].count!=CT[j].count)continue;
                if(fabsf(PT[k].px0-fx)+fabsf(PT[k].py0-fy)>STATIC_NEAR)continue;
                if(same && !same_st(&P[k],&C[j]))continue;
                e=static_misfit(&P[k],&C[j],mo);
                if(e>=0.0f && (best<0 || e<bestE) && !STATIC_TAKEN(k,e)){best=k;bestE=e;}
            }
        }
#undef STATIC_TAKEN
        if(best<0)continue;
        o=sStaticOwner[best];
        if(o>=0)sStaticPair[o]=-1;   /* (it fitted worse: what it had, as below) */
        sStaticOwner[best]=(int32_t)j;
        sStaticPair[j]=best;sStaticErr[j]=bestE;offset=best-(int)j;
    }
    for(j=0;j<n;j++){
        int k=sStaticPair[j],was=out[j];
        if(k>=0)out[j]=k;
        else if(was>=0 && sStaticOwner[was]>=0)out[j]=-1;   /* (another's, exactly) */
    }
}
/* Where the camera's move puts a clip-space vertex halfway: as a pair's
 * halfway is made (make_mid_scene), the other of the pair where the move
 * puts it (sign -1: this scene's, the scene before's less the move; 1: the
 * scene before's, the next one's plus it); the same to the unit as its
 * neighbours paired. */
static int32_t static_halfway(int32_t v,int32_t mo,int sign)
{
    int64_t other=sign>0?(int64_t)v+mo:(int64_t)v-mo;
    return (int32_t)(((int64_t)v+other)/2);
}
/* (the [perf] geometry's work line: the in-between scenes' matching of
 * polygons with the scene before's, NitroPort_Gx3dMidWork) */
static uint64_t sMidMatchTicks;
/* The marked polygons' whole triangles (NitroGx3dScene.tagged_triangles)
 * of the scene before, and an in-between scene's: each of the new scene's
 * halfway from the one of the same mark's run and place in it in the
 * scene before (as replay_blended: on the screen where the halfway point
 * of clip space is, weighed by w); one with none before, or that moved
 * more than a quarter of the screen, where the new scene has it. */
static NitroGx3dTriangle sTaggedBefore[NITRO_GX3D_MAX_TAGGED_TRIANGLES];
static uint32_t sTaggedBeforeCount;
static void tagged_keep(const NitroGx3dScene *now)
{
    sTaggedBeforeCount=now->tagged_count;
    memcpy(sTaggedBefore,now->tagged_triangles,now->tagged_count*sizeof(sTaggedBefore[0]));
}
static void tagged_halfway(const NitroGx3dScene *now,NitroGx3dScene *mid)
{
    /* A mark's triangles come in a run, the runs in the same order from
     * one scene to the next (the game draws the same things in the same
     * order): each run's pair is looked for from where the last one's
     * ended, round once. */
    uint32_t i,j,hint=0,run=0,runBefore=0,lenBefore=0,tag=0;
    int have=0;
    mid->tagged_count=now->tagged_count;
    for(i=0;i<now->tagged_count;i++){
        const NitroGx3dTriangle *c=&now->tagged_triangles[i],*p=NULL;
        NitroGx3dTriangle *out=&mid->tagged_triangles[i];
        unsigned k;
        int jumped=0;
        *out=*c;
        if(!i || c->tag!=tag){
            uint32_t n;
            tag=c->tag;run=i;have=0;lenBefore=0;
            for(n=0;n<sTaggedBeforeCount;n++){
                j=hint+n;
                if(j>=sTaggedBeforeCount)j-=sTaggedBeforeCount;
                if(sTaggedBefore[j].tag==tag && (!j || sTaggedBefore[j-1].tag!=tag)){have=1;runBefore=j;break;}
            }
            if(have){
                for(j=runBefore;j<sTaggedBeforeCount && sTaggedBefore[j].tag==tag;j++)lenBefore++;
                hint=runBefore+lenBefore;
                if(hint>=sTaggedBeforeCount)hint=0;
            }
        }
        if(have && i-run<lenBefore)p=&sTaggedBefore[runBefore+(i-run)];
        if(!p)continue;
        for(k=0;k<3;k++){
            int64_t dx=(int64_t)c->v[k].x-p->v[k].x,dy=(int64_t)c->v[k].y-p->v[k].y;
            if(p->v[k].clip[3]<=0 || c->v[k].clip[3]<=0 || llabs((long long)dx)>64*256 || llabs((long long)dy)>48*256)jumped=1;
        }
        if(jumped)continue;
        for(k=0;k<3;k++){
            float w0=(float)p->v[k].clip[3],w1=(float)c->v[k].clip[3];
            float t=w1/(w0+w1);
            float dx=(float)(c->v[k].x-p->v[k].x)*t,dy=(float)(c->v[k].y-p->v[k].y)*t;
            out->v[k].x=p->v[k].x+(int32_t)(dx+(dx>=0?0.5f:-0.5f));
            out->v[k].y=p->v[k].y+(int32_t)(dy+(dy>=0?0.5f:-0.5f));
            out->v[k].clip[3]=(int32_t)(((int64_t)p->v[k].clip[3]+c->v[k].clip[3])/2);
        }
    }
}
static int make_mid_scene(NitroGx3dScene *now,NitroGx3dScene *sMid)
{
    const PolyRecord *P=sRec[sRecBuild^1u],*C=sRec[sRecBuild];
    unsigned m=sRecCount[sRecBuild^1u],n=sRecCount[sRecBuild],j,k,used=0;
    unsigned framePolygons=s.framePolygons,frameVertices=s.frameVertices;
    int capture=sShadowCapture;
    if(!sMid || !sMatch || !sRecBeforeValid || !n){
        sMidMatched=0;sMidPolygons=n;sPrevMotionValid=0;
        return 0;
    }
    {
        uint64_t t0=sOps.ticks?sOps.ticks(sOps.context):0;
        sMidMatched=match_records(sTag[sRecBuild^1u],sPos[sRecBuild^1u],P,m,sTag[sRecBuild],sPos[sRecBuild],C,n,sMatch);
        sClipMotionValid=sMidMatched*2u>=n && clip_motion(sTag[sRecBuild^1u],sTag[sRecBuild],n,sMatch,sClipMotion);
        if(sClipMotionValid){
            clip_motion_z(P,C,sTag[sRecBuild^1u],sTag[sRecBuild],n,sMatch,sClipMotion);
            static_rematch(P,m,C,n,sMatch,sClipMotion);
        }
        if(sOps.ticks)__atomic_store_n(&sMidMatchTicks,sMidMatchTicks+(sOps.ticks(sOps.context)-t0),__ATOMIC_RELAXED);
    }
    sMidPolygons=n;
    if(n>=16u && sMidMatched*2u<n){sPrevMotionValid=0;return 0;}   /* (a cut: too little in common) */
    /* (the view's size changed between them: no in-between, its clip
     * spaces aren't the same) */
    if(sLastBgOfsValid && (now->screen_width!=sLastWidth || now->screen_height!=sLastHeight)){sPrevMotionValid=0;return 0;}
    sMid->count=0;sMid->dropped=0;sMid->shadow_count=0;sMid->shadow_triangles=NULL;
    tagged_halfway(now,sMid);
    sMid->serial=now->serial;sMid->swap_param=now->swap_param;
    sMid->screen_width=now->screen_width;
    sMid->screen_height=now->screen_height;
    memcpy(sMid->projection,now->projection,sizeof(sMid->projection));
    sMid->stamp=now->stamp;
    sMid->stamp_before=sLastBgOfsValid?sLastStamp:now->stamp;
    sMid->swap_seq=now->swap_seq;
    sMid->swap_seq_before=sLastBgOfsValid?sLastSeq:now->swap_seq;
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
        const BlendView view=blend_viewport();
        const uint32_t viewport=viewport_key();
        sDest=sMid;sReplaying=1;sShadowCapture=0;
        s.framePolygons=0;s.frameVertices=0;
        for(j=0;j<n;j++){
            int32_t clip[4][4];
            const PolyRecord *c=&C[j];
            int mid=0;
            uint32_t soft=0;
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
                    soft=soft_of(p,c,now);
                }
            }
            /* (the scene's own triangles of it, and the in-between one's) */
            for(k=0;k<c->ntri && c->tri0+k<now->count;k++)now->triangles[c->tri0+k].soft=soft;
            sReplaySoft=soft;
            if(!mid)memcpy(clip,c->clip,sizeof(clip));
            if(sMatch[j]<0 && c->onScreen && sClipMotionValid){
                /* (with the camera's move: halfway as it puts it, the near
                 * ground further than the far, as the rest around it) */
                for(k=0;k<c->count;k++){
                    unsigned q;
                    for(q=0;q<4;q++)clip[k][q]=static_halfway(c->clip[k][q],sClipMotion[q],-1);
                }
            }else if(sMatch[j]<0 && c->onScreen && sMotionValid && (sMotionX!=0.0f || sMotionY!=0.0f)){
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
            if(mid && c->screenOk && P[sMatch[j]].screenOk &&
               c->viewport==viewport && P[sMatch[j]].viewport==viewport)
                replay_blended(&P[sMatch[j]],c,clip,&view);
            else replay_polygon(c,clip);
            sReplaySoft=0;
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
                    unsigned q;
                    memcpy(full,p->clip[k],sizeof(full));
                    if(sClipMotionValid)for(q=0;q<4;q++)full[q]=(int32_t)((int64_t)full[q]+sClipMotion[q]);
                    else{
                        full[0]+=(int32_t)floor(fx*(double)full[3]+0.5);
                        full[1]+=(int32_t)floor(fy*(double)full[3]+0.5);
                    }
                    all&=outcode(full);
                }
                if(!all)continue;   /* (not leaving the view: gone, as this scene has it) */
                for(k=0;k<p->count;k++){
                    unsigned q;
                    memcpy(clip[k],p->clip[k],sizeof(clip[k]));
                    if(sClipMotionValid)for(q=0;q<4;q++)clip[k][q]=static_halfway(p->clip[k][q],sClipMotion[q],1);
                    else{
                        clip[k][0]+=(int32_t)floor(0.5*fx*(double)clip[k][3]+0.5);
                        clip[k][1]+=(int32_t)floor(0.5*fy*(double)clip[k][3]+0.5);
                    }
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
 * its in-between scene. With the thread a scene behind (the game has
 * ended the next one already), none: the game leaves out the one after
 * that when the thread is still on this one then (a jump); without the
 * in-between scene (a third of the time on the Cycling Road) it catches
 * up, and the frame between shows the scene twice. Not when every scene
 * is kept (comparing). */
static uint32_t sMidsLeftOut;
static uint64_t sMidTicks;
static void interp_mid(NitroGx3dScene *now)
{
    uint64_t t0;
    /* (a scene behind: the game has ended the next one already. Not just
     * ended late: the thread shares its core with the 2D's drawing, which
     * holds it up a V-blank or two while the HM cut-in or a battle's start
     * plays, and it caught up again; leaving the in-between scenes out
     * then put the field at 30 frames a second, 10-13 a second left out,
     * log 24186035. Nor a V-blank late just after a 2D picture drawn on
     * its core: so left out, 6-11 a second through the HM cut-in's text
     * and start and the place names' signs, the field stuttered, log
     * 6fb7c4a4; before that rule 0-0.5, log 31a6ea6d) */
    if(sInterp && sThreaded && !sOps.keep_every_scene &&
       (int32_t)(__atomic_load_n(&sSwapsQueued,__ATOMIC_RELAXED)-sSwapsDone)>=2){
        sMidOk[sBuilding]=0;
        __atomic_store_n(&sMidsLeftOut,sMidsLeftOut+1u,__ATOMIC_RELAXED);
        tagged_keep(now);
        return;
    }
    t0=sOps.ticks?sOps.ticks(sOps.context):0;
    sMidOk[sBuilding]=sInterp && make_mid_scene(now,sMids[sBuilding]);
    tagged_keep(now);
    if(sOps.ticks && sInterp)__atomic_store_n(&sMidTicks,sMidTicks+(sOps.ticks(sOps.context)-t0),__ATOMIC_RELAXED);
}
void NitroPort_Gx3dMidWork(uint64_t *ticks,uint64_t *matchTicks,uint32_t *leftOut)
{
    if(ticks)*ticks=__atomic_load_n(&sMidTicks,__ATOMIC_RELAXED);
    if(matchTicks)*matchTicks=__atomic_load_n(&sMidMatchTicks,__ATOMIC_RELAXED);
    if(leftOut)*leftOut=__atomic_load_n(&sMidsLeftOut,__ATOMIC_RELAXED);
}
/* At a SWAP_BUFFERS, after its scene is published: this scene's records
 * become the scene before's. */
static void interp_swap(void)
{
    int wanted=__atomic_load_n(&sInterpWanted,__ATOMIC_ACQUIRE),i;
    if(wanted && (!sRec[0] || !sPos[1] || !sMids[2])){
        if(!sRec[0])sRec[0]=malloc(MAX_RECORDS*sizeof(PolyRecord));
        if(!sRec[1])sRec[1]=malloc(MAX_RECORDS*sizeof(PolyRecord));
        for(i=0;i<2;i++){
            if(!sTag[i])sTag[i]=malloc(MAX_RECORDS*sizeof(RecTag));
            if(!sPos[i])sPos[i]=malloc(MAX_RECORDS*sizeof(RecPos));
        }
        if(!sMatch)sMatch=malloc(MAX_RECORDS*sizeof(int32_t));
        if(!sPUsed)sPUsed=malloc(MAX_RECORDS);
        for(i=0;i<3;i++)if(!sMids[i])sMids[i]=malloc(sizeof(NitroGx3dScene));
        if(!sRec[0]||!sRec[1]||!sTag[0]||!sTag[1]||!sPos[0]||!sPos[1]||!sMatch||!sMids[0]||!sMids[1]||!sMids[2]){
            static int told;
            if(!told){told=1;NitroPort_Log("[gx3d] no memory for the in-between scenes");}
            __atomic_store_n(&sInterpWanted,0,__ATOMIC_RELEASE);
            wanted=0;
        }
    }
    if(sInterp && wanted)sRecBeforeValid=1;
    else sRecBeforeValid=0;
    if(!sRecBeforeValid)sPrevMotionValid=0;   /* (the motion before: from scenes in a row only) */
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
        if(all && !(all&0x30u) && (NitroPort_ShadowReachOn() || TAG_KEPT(sTagGame))){
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
#define OP_TAG 0xF4u         /* one parameter: the mark on the polygons from here on (NitroPort_Gx3dTag) */
#define SCENE_REGS_WORDS (2u+NITRO_GX3D_REGS_SIZE/4u+4u+1u)

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
        if((s.mode==1||s.mode==2) && op!=0x10u && op!=0x11u && op!=0x13u)forget_normals();   /* (the vector matrix) */
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
        s.diffuse=p[0]&0x7FFF;s.ambient=(p[0]>>16)&0x7FFF;sLightCache.dirty=1;forget_normals();
        if(p[0]&(1u<<15))s.rgb=s.diffuse;
        break;
    case 0x31:settle_lighting();
        s.specular=p[0]&0x7FFF;s.emission=(p[0]>>16)&0x7FFF;sLightCache.dirty=1;forget_normals();
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
        forget_normals();
        break;
    }
    case 0x33:settle_lighting();s.lightColor[(p[0]>>30)&3]=p[0]&0x7FFF;sLightCache.dirty=1;forget_normals();break;
    case 0x34:settle_lighting();memcpy(s.shininess,p,sizeof(s.shininess));forget_normals();break;
    case 0x40:settle_lighting();s.primitive=p[0]&3;s.nverts=0;s.inBegin=1;s.stripStarted=0;
        if((s.attr^s.pendingAttr)&15u)forget_normals();   /* (its lights) */
        s.attr=s.pendingAttr;
        break;
    case 0x41:s.inBegin=0;s.nverts=0;break;
    case 0x50:{
        int next=-1;
        uint32_t serial=0;
        NitroGx3dScene *scene=&sScenes[sBuilding];
        /* All vertex and attribute writes precede the release publication. */
        scene->swap_param=p[0];
        scene->serial=sSerial+1;
        scene->screen_width=(uint32_t)(s.wideNum==s.wideDen?256:s.wideDen);
        scene->screen_height=(uint32_t)(s.tallNum==s.tallDen?192:s.tallDen);
        memcpy(scene->projection,s.proj.a,sizeof(scene->projection));
        /* When the game ended it (with the thread: noted on the game's
         * thread as the SWAP went in; swap_noted). */
        /* (and its registers: as the game had them then) */
        if(sThreaded){
            scene->stamp=sPendingStamp;
            scene->swap_seq=sPendingSeq;
            scene->disp3dcnt=sPendingDisp3d;
            memcpy(scene->regs3d,sPendingRegs3d,NITRO_GX3D_REGS_SIZE);
            memcpy(scene->bg_ofs,sPendingBgOfs,sizeof(scene->bg_ofs));
        }else{
            scene->stamp=NitroPort_Gx3dStamp();
            scene->swap_seq=__atomic_add_fetch(&sSwapsMade,1u,__ATOMIC_RELEASE);
            scene->disp3dcnt=io_disp3dcnt();
            memcpy(scene->regs3d,NitroPort_IoReg+NITRO_GX3D_REGS_AT,NITRO_GX3D_REGS_SIZE);
            io_bg_ofs(scene->bg_ofs);
        }
        scene->stamp_before=scene->stamp;
        scene->swap_seq_before=scene->swap_seq;
        /* Which SWAP_BUFFERS it is (with the geometry thread, counting
         * them as the game's thread does: NitroPort_Gx3dEarlierSceneUnread). */
        NitroPort_MailboxTag(&sMailbox,sSwapsDone+1u);
        interp_mid(scene);   /* (published with it) */
        memcpy(sLastBgOfs,scene->bg_ofs,sizeof(sLastBgOfs));
        sLastStamp=scene->stamp;
        sLastSeq=scene->swap_seq;
        sLastWidth=scene->screen_width;sLastHeight=scene->screen_height;
        sLastBgOfsValid=1;
        sSameChain=0;
        if(NitroPort_MailboxPublish(&sMailbox,&next,&serial)){
            sSameChain=1;
            sDisplayed=sBuilding;
            sBuilding=(unsigned)next;
            sSerial=serial;
            sScenes[sBuilding].count=0;
            sScenes[sBuilding].shadow_count=0;
            sScenes[sBuilding].tagged_count=0;
            sScenes[sBuilding].dropped=0;
            NitroPort_Gx3dScenePublished();
        }else{
            /* Keep the newest image and reuse this building slot on the
             * exceptionally slow consumer path; never stall the game. */
            scene->count=0;scene->shadow_count=0;scene->tagged_count=0;scene->dropped++;
        }
        /* (what only casts: the shadow map's alone, FULL; not the sprites') */
        sShadowCapture=NitroPort_ShadowReachOn() && shadow_store();
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
    case OP_WIDE_VIEW:set_wide_view(&s,(int)(p[0]&0xFFFFu),(int)(p[0]>>16));break;
    case OP_TAG:s.tag=p[0];break;
    case OP_SCENE_REGS:
        sPendingStamp=p[0];
        sPendingDisp3d=p[1];
        memcpy(sPendingRegs3d,&p[2],NITRO_GX3D_REGS_SIZE);
        memcpy(sPendingBgOfs,&p[2+NITRO_GX3D_REGS_SIZE/4],sizeof(sPendingBgOfs));
        sPendingSeq=p[2+NITRO_GX3D_REGS_SIZE/4+4];
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
/* And (with RING_SKIP) left out as the same as the scene before: the one
 * on the screen stands for it (NitroPort_Gx3dSameAs). */
#define RING_SAME  (1u<<17)
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
/* A scene the same as the one before, word for word (the 3D registers
 * too, not its time), while a battle's start plays in the field
 * (NitroPort_FieldEffect): the field stands still, its 3D made again and
 * again on the core that draws the effect's 2D. The next scene is left out
 * then, as one the thread can't keep up with (the one before stays on the
 * screen): if it moved after all, it's on the screen a scene later. Not
 * otherwise: the first step after standing still would be a scene late.
 * sSceneHash: FNV-1a of the scene's words so far (on the game's thread,
 * while sHashScenes). */
static uint32_t sSceneHash=2166136261u, sLastSceneHash;
static int sHashScenes, sSameScene;
static uint32_t sSameLeftOut;   /* scenes left out as the same (for the [gx3d] line) */
static int sSkipSame;           /* (the next scene left out as the same: RING_SAME) */
/* The newest scene left out as the same, on the thread: the serial of the
 * scene on the screen it's the same as, its own time and SWAP_BUFFERS
 * count (NitroPort_Gx3dSameAs; sSameGen odd while written). */
static uint32_t sSameGen, sSameSerial, sSameStamp, sSameSeq;
static inline void hash_words(const uint32_t *p,uint32_t n)
{
    uint32_t h=sSceneHash,i;
    for(i=0;i<n;i++)h=(h^p[i])*16777619u;
    sSceneHash=h;
}
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
    if(sHashScenes && op!=OP_SCENE_REGS){uint32_t h=op|(n<<8);hash_words(&h,1);hash_words(p,n);}
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
    if(sHashScenes)hash_words(words,count);
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
    w[2+NITRO_GX3D_REGS_SIZE/4+4]=__atomic_add_fetch(&sSwapsMade,1u,__ATOMIC_RELEASE);
    if(sHashScenes)hash_words(&w[1],1u+NITRO_GX3D_REGS_SIZE/4u);   /* (not its time, nor the BGs' scroll) */
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
    /* The same as the one before, in a battle's start: the next too. */
    sSameScene=sHashScenes && sSceneHash==sLastSceneHash;
    if(sSameScene && !sSkipScene && !sOps.keep_every_scene){
        sSkipScene=1;
        sSameLeftOut++;
    }
    /* (left out, and the same as the one before: whatever the reason) */
    sSkipSame=sSkipScene && sSameScene;
    sLastSceneHash=sSceneHash;
    sSceneHash=2166136261u;
    sHashScenes=NitroPort_FieldEffect();
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
        flags=RING_SKIP|(sSkipSame?RING_SAME:0u);
    if(op==0x50u)swap_noted();
    enqueue(op,p,n,flags);
    if(op==0x50u)swap_queued();
}
/* A command on the thread; skip: its scene is left out (2: as the same
 * as the one before). */
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
            if(skip==2 && sThreaded && sSameChain){
                /* The same as the scene on the screen: that one stands for
                 * this frame too (its time and SWAP_BUFFERS count, for what
                 * the 2D shows with it: NitroPort_Gx3dSameAs), and the next
                 * in-between scene starts from this frame. */
                uint32_t g=sSameGen;
                __atomic_store_n(&sSameGen,g+1u,__ATOMIC_RELEASE);
                __atomic_thread_fence(__ATOMIC_SEQ_CST);
                __atomic_store_n(&sSameSerial,sSerial,__ATOMIC_RELAXED);
                __atomic_store_n(&sSameStamp,sPendingStamp,__ATOMIC_RELAXED);
                __atomic_store_n(&sSameSeq,sPendingSeq,__ATOMIC_RELAXED);
                __atomic_store_n(&sSameGen,g+2u,__ATOMIC_RELEASE);
                if(sLastBgOfsValid){
                    memcpy(sLastBgOfs,sPendingBgOfs,sizeof(sLastBgOfs));
                    sLastStamp=sPendingStamp;
                    sLastSeq=sPendingSeq;
                }
            }
            /* (left out for the thread's time: the scene on the screen is
             * older than the one before this) */
            if(skip!=2)sSameChain=0;
            /* Nothing published: the scene being built starts again. */
            sScenes[sBuilding].count=0;
            sScenes[sBuilding].shadow_count=0;
            sScenes[sBuilding].tagged_count=0;
            sScenes[sBuilding].dropped=0;
            s.frameVertices=0;s.framePolygons=0;
            __atomic_store_n(&sSwapsSkipped,sSwapsSkipped+1u,__ATOMIC_RELAXED);
            __atomic_store_n(&sSwapsDone,sSwapsDone+1u,__ATOMIC_RELEASE);
            return;
        }
    }
    if(op==0x50u && sOps.ticks){
        uint64_t t=sOps.ticks(sOps.context);
        execute(op,p);
        __atomic_store_n(&sSwapTicks,sSwapTicks+(sOps.ticks(sOps.context)-t),__ATOMIC_RELAXED);
    }else{
        execute(op,p);
    }
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
            if(sShadowCapture || TAG_KEPT(s.tag)){
                /* (the sun shadows keep what's beside the view up to their
                 * reach, keep_for_shadow, and the marked polygons,
                 * keep_tagged: past it, with a margin for the rounding) */
                const int64_t margin=(int64_t)4096*8;
                if(!(c[0]+SHADOW_GUARD*c[3]<-margin))outside&=~1u;
                if(!(c[0]-SHADOW_GUARD*c[3]>=margin))outside&=~2u;
                if(!(c[1]+SHADOW_GUARD*c[3]<-margin))outside&=~4u;
                if(!(c[1]-SHADOW_GUARD*c[3]>=margin))outside&=~8u;
            }else{
                if(!(c[0]+c[3]<0))outside&=~1u;
                if(!(c[0]-c[3]>=4096))outside&=~2u;
                if(!(c[1]+c[3]<0))outside&=~4u;
                if(!(c[1]-c[3]>=4096))outside&=~8u;
            }
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
    __atomic_add_fetch(&sSkippedVertices,n,__ATOMIC_RELAXED);
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
    int skip=(header&RING_SKIP)?((header&RING_SAME)?2:1):0;
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
void NitroPort_Gx3dThreadWork(uint64_t *swapTicks,uint32_t *vertices,uint32_t *polygons,uint32_t *skipped)
{
    if(skipped)*skipped=__atomic_load_n(&sSkippedVertices,__ATOMIC_RELAXED);
    if(swapTicks)*swapTicks=__atomic_load_n(&sSwapTicks,__ATOMIC_RELAXED);
    /* (the [gx3d] line's counts so far too: read as they are, for a log) */
    if(vertices)*vertices=__atomic_load_n(&sTotalVertices,__ATOMIC_RELAXED)+*(volatile unsigned *)&sDiag.vertices;
    if(polygons)*polygons=__atomic_load_n(&sTotalPolygons,__ATOMIC_RELAXED)+*(volatile unsigned *)&sDiag.polygons;
}
uint32_t NitroPort_Gx3dSameScenesLeftOut(void)
{
    return sSameLeftOut;   /* (the game's thread's own) */
}
int NitroPort_Gx3dSameAs(uint32_t serial,uint32_t *stamp,uint32_t *swapSeq)
{
    int tries;
    for(tries=0;tries<8;tries++){
        uint32_t g=__atomic_load_n(&sSameGen,__ATOMIC_ACQUIRE),sr,st,sq;
        if(g==0u)return 0;   /* (none yet) */
        if(g&1u)continue;
        sr=__atomic_load_n(&sSameSerial,__ATOMIC_RELAXED);
        st=__atomic_load_n(&sSameStamp,__ATOMIC_RELAXED);
        sq=__atomic_load_n(&sSameSeq,__ATOMIC_RELAXED);
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        if(__atomic_load_n(&sSameGen,__ATOMIC_ACQUIRE)!=g)continue;
        if(sr!=serial)return 0;
        if(stamp)*stamp=st;
        if(swapSeq)*swapSeq=sq;
        return 1;
    }
    return 0;
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
void NitroPort_Gx3dSetView(int screenWidth,int screenHeight)
{
    uint32_t p;
    initialize();
    if(screenWidth<=0||screenWidth>1024)screenWidth=256;
    if(screenHeight<=0||screenHeight>1024)screenHeight=192;
    p=(uint32_t)screenWidth|((uint32_t)screenHeight<<16);
    sWideWidth=screenWidth;sWideHeight=screenHeight;
    if(sResults->wideDen==(screenWidth==256?1:screenWidth) && sResults->wideNum==(screenWidth==256?1:256) &&
       sResults->tallDen==(screenHeight==192?1:screenHeight) && sResults->tallNum==(screenHeight==192?1:192))
        return;   /* (as it is) */
    /* The shadow now (the game's box tests); the thread at this point of
     * its stream. */
    set_wide_view(sResults,screenWidth,screenHeight);
    if(sThreaded)enqueue(OP_WIDE_VIEW,&p,1,0);
    else if(sResults!=&s)set_wide_view(&s,screenWidth,screenHeight);
}
void NitroPort_Gx3dSetWideView(int screenWidth)
{
    NitroPort_Gx3dSetView(screenWidth,192);
}
/* (the thread's, at this point of its stream; without one, now) */
void NitroPort_Gx3dTag(uint32_t tag)
{
    initialize();
    /* (only with the sun shadows on: off, nothing is ever marked) */
    if(!NitroPort_GroundShadowsOn())tag=0;
    if(tag==sTagGame)return;
    sTagGame=tag;
    if(sThreaded)enqueue(OP_TAG,&tag,1,0);
    else s.tag=tag;
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
    sTagGame=0;
    memset(&s,0,sizeof(s));memset(sScenes,0,sizeof(sScenes));
    shadow_store_attach();
    memset(&sDec,0,sizeof(sDec));
    sLightCache.dirty=1;
    forget_normals();
    make_products();
    NitroPort_MailboxInit(&sMailbox);
    sBuilding=0;sDisplayed=1;sSerial=0;
    sRecCount[0]=sRecCount[1]=0;sRecBeforeValid=0;sMidOk[0]=sMidOk[1]=sMidOk[2]=0;sDest=NULL;sReplaying=0;
    sPrevMotionValid=0;
    identity(&s.proj);identity(&s.pos);identity(&s.vec);identity(&s.tex);
    set_wide_view(&s,sWideWidth,sWideHeight);   /* (kept across resets) */
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
                    enqueue_packets(words,(uint32_t)n,sSkipScene?RING_SKIP|(sSkipSame?RING_SAME:0u):0u);
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
/* ------------------------------------------- a display list cut to a slab
 *
 * NitroPort_Gx3dTrimList: a model's shape's display list with only the
 * polygons that reach into a slab of its coordinates (lo <= x, y or z <=
 * hi), made once (Route 206's river, mirrored past the scenery: a strip of
 * the map by the border, not the whole map, each scene). Its lists of
 * triangles and quads are looked through polygon by polygon (a strip
 * whole): one with no vertex in the slab is left out, and what it leaves
 * behind goes instead: its last TEXCOORD, NORMAL and COLOR, in their order,
 * before the next command kept; the next vertex kept, if it's one relative
 * to the one before, as its own coordinates (VTX_16); and before the
 * list's BEGIN_VTXS or END_VTXS, its last coordinates as a vertex alone
 * (no polygon: the list's polygons are whole). The polygons kept are drawn
 * the same, bit for bit. */
typedef struct DlGroup {
    uint32_t begin, end;          /* its commands: ops[begin..end) */
    int32_t lo[3], hi[3];
    uint32_t texcoord, normal, color, vertices;
    uint8_t haveTexcoord, haveNormal, haveColor, normalLast;
} DlGroup;
typedef struct DlInfo {
    uint32_t nops, ngroups;
    uint32_t *ops;                /* op | (its parameters' word index << 8) */
    int32_t (*xyz)[3];            /* the coordinates after each command */
    DlGroup *groups;
} DlInfo;

static void dl_free(DlInfo *info)
{
    if(info){free(info->ops);free(info->xyz);free(info->groups);free(info);}
}
static int dl_s16(const int32_t xyz[3])
{
    return xyz[0]>=-32768 && xyz[0]<=32767 && xyz[1]>=-32768 && xyz[1]<=32767 &&
           xyz[2]>=-32768 && xyz[2]<=32767;
}
static int dl_add_group(DlInfo *info,uint32_t *cap,const DlGroup *g)
{
    if(info->ngroups>=*cap){
        DlGroup *more=realloc(info->groups,(*cap*=2)*sizeof(*more));
        if(!more)return 0;
        info->groups=more;
    }
    info->groups[info->ngroups++]=*g;
    return 1;
}
/* A list's commands and its groups of polygons (perGroup polygons each);
 * NULL if it isn't whole packets, or there's no room. */
static DlInfo *dl_survey(const uint32_t *w,uint32_t n,uint32_t perGroup)
{
    DlInfo *info=calloc(1,sizeof(*info));
    uint32_t i=0,cap=n+4u,k,o,groupCap=16;
    int32_t xyz[3]={0,0,0};
    int known=0;
    if(!info)return NULL;
    info->ops=malloc(cap*sizeof(*info->ops));
    info->groups=malloc(groupCap*sizeof(*info->groups));
    if(!info->ops || !info->groups){dl_free(info);return NULL;}
    while(i<n){
        uint32_t word=w[i++],params=i;
        for(k=0;k<4;k++){
            unsigned op=(word>>(k*8))&255u;
            if(!op)continue;
            if(params+sParamCount[op]>n || info->nops>=cap){dl_free(info);return NULL;}
            info->ops[info->nops++]=op|(params<<8);
            params+=sParamCount[op];
        }
        i=params;
    }
    info->xyz=malloc((info->nops?info->nops:1u)*sizeof(*info->xyz));
    if(!info->xyz){dl_free(info);return NULL;}
    /* The coordinates after each command. */
    for(o=0;o<info->nops;o++){
        unsigned op=info->ops[o]&255u;
        if(op>=0x23u && op<=0x28u)vertex_coords(op,&w[info->ops[o]>>8],xyz);
        info->xyz[o][0]=xyz[0];info->xyz[o][1]=xyz[1];info->xyz[o][2]=xyz[2];
    }
    /* Its lists of vertices (after a BEGIN_VTXS: nothing but vertices,
     * TEXCOORD, NORMAL, COLOR, then BEGIN_VTXS or END_VTXS in this list),
     * in groups. */
    for(o=0;o<info->nops;o++){
        unsigned op=info->ops[o]&255u;
        uint32_t e,per,count=0,ok=1;
        if(op>=0x23u && op<=0x24u)known=1;
        if(op!=0x40u)continue;
        per=(w[info->ops[o]>>8]&3u)==0?3u:(w[info->ops[o]>>8]&3u)==1?4u:0u;
        for(e=o+1;e<info->nops;e++){
            unsigned q=info->ops[e]&255u;
            if(q>=0x23u && q<=0x28u){
                if(q<=0x24u)known=1;
                else if(!known)ok=0;
                if(!dl_s16(info->xyz[e]))ok=0;
                count++;
            }else if(q<0x20u || q>0x22u){
                break;
            }
        }
        if(ok && count && e<info->nops && ((info->ops[e]&255u)==0x40u || (info->ops[e]&255u)==0x41u)){
            DlGroup g;
            uint32_t q,inGroup=0,limit=per?per*perGroup:0xFFFFFFFFu;
            memset(&g,0,sizeof(g));
            g.begin=o+1;
            for(q=o+1;q<e;q++){
                unsigned c=info->ops[q]&255u;
                const uint32_t *pp=&w[info->ops[q]>>8];
                if(c>=0x23u && c<=0x28u){
                    const int32_t *v=info->xyz[q];
                    int j;
                    if(!g.vertices){
                        for(j=0;j<3;j++)g.lo[j]=g.hi[j]=v[j];
                    }else{
                        for(j=0;j<3;j++){if(v[j]<g.lo[j])g.lo[j]=v[j];if(v[j]>g.hi[j])g.hi[j]=v[j];}
                    }
                    g.vertices++;
                    if(++inGroup==limit){
                        g.end=q+1;
                        if(!dl_add_group(info,&groupCap,&g)){dl_free(info);return NULL;}
                        memset(&g,0,sizeof(g));
                        g.begin=q+1;
                        inGroup=0;
                    }
                }else if(c==0x22u){g.texcoord=pp[0];g.haveTexcoord=1;}
                else if(c==0x21u){g.normal=pp[0];g.haveNormal=1;g.normalLast=1;}
                else{g.color=pp[0];g.haveColor=1;g.normalLast=0;}
            }
            if(g.vertices){
                g.end=e;
                if(!dl_add_group(info,&groupCap,&g)){dl_free(info);return NULL;}
            }
        }
        o=e-1;
    }
    return info;
}
/* Commands into a buffer, packed as the DS has them (up to four to a word). */
typedef struct DlWriter {
    uint32_t *out;
    uint32_t cap, n, header;   /* room, words written, the packed word being filled (index) */
    unsigned slots;            /* commands in it */
    int full;
} DlWriter;
static void dl_put(DlWriter *wr,unsigned op,const uint32_t *p)
{
    unsigned k,np=sParamCount[op];
    if(wr->slots==0 || wr->slots==4){
        if(wr->n+1u+np>wr->cap){wr->full=1;return;}
        wr->header=wr->n++;
        wr->out[wr->header]=0;
        wr->slots=0;
    }else if(wr->n+np>wr->cap){
        wr->full=1;
        return;
    }
    wr->out[wr->header]|=(uint32_t)op<<(wr->slots*8);
    wr->slots++;
    for(k=0;k<np;k++)wr->out[wr->n++]=p[k];
}
/* What left-out groups leave behind, still to send. */
typedef struct DlOwed {
    uint32_t texcoord, normal, color;
    uint8_t texcoordOwed, normalOwed, colorOwed, normalLast, coordOwed;
    int32_t xyz[3];
} DlOwed;
static void dl_owe(DlOwed *ow,const DlGroup *g,const int32_t last[3])
{
    if(g->haveTexcoord){ow->texcoord=g->texcoord;ow->texcoordOwed=1;}
    if(g->haveNormal){ow->normal=g->normal;ow->normalOwed=1;}
    if(g->haveColor){ow->color=g->color;ow->colorOwed=1;}
    if(g->haveNormal && g->haveColor)ow->normalLast=g->normalLast;
    else if(g->haveNormal)ow->normalLast=1;
    else if(g->haveColor)ow->normalLast=0;
    ow->coordOwed=1;
    ow->xyz[0]=last[0];ow->xyz[1]=last[1];ow->xyz[2]=last[2];
}
static void dl_pay_state(DlWriter *wr,DlOwed *ow)
{
    if(ow->texcoordOwed)dl_put(wr,0x22u,&ow->texcoord);
    if(ow->normalOwed && ow->colorOwed){
        if(ow->normalLast){dl_put(wr,0x20u,&ow->color);dl_put(wr,0x21u,&ow->normal);}
        else{dl_put(wr,0x21u,&ow->normal);dl_put(wr,0x20u,&ow->color);}
    }else if(ow->normalOwed){
        dl_put(wr,0x21u,&ow->normal);
    }else if(ow->colorOwed){
        dl_put(wr,0x20u,&ow->color);
    }
    ow->texcoordOwed=ow->normalOwed=ow->colorOwed=0;
}
static void dl_vtx16(DlWriter *wr,const int32_t xyz[3])
{
    uint32_t v[2];
    v[0]=((uint32_t)xyz[0]&0xFFFFu)|((uint32_t)xyz[1]<<16);
    v[1]=(uint32_t)xyz[2]&0xFFFFu;
    dl_put(wr,0x23u,v);
}
/* (a group kept: reaching into lo..hi on each axis, or for those in
 * `starts`, starting there, lo <= its lowest < hi (each polygon in one of
 * two lists cut side by side); for those in `outside` too, starting
 * anywhere but there) */
static int dl_group_kept(const DlGroup *grp,const int32_t lo[3],const int32_t hi[3],unsigned starts,unsigned outside)
{
    int a;
    for(a=0;a<3;a++){
        if(starts&(1u<<a)){
            int in=grp->lo[a]>=lo[a] && grp->lo[a]<hi[a];
            if(in==!!(outside&(1u<<a)))return 0;
        }else if(grp->hi[a]<lo[a] || grp->lo[a]>hi[a])return 0;
    }
    return 1;
}
static uint32_t dl_trim_surveyed(const DlInfo *info,const uint32_t *w,void *dst,uint32_t dstBytes,
                                 const int32_t lo[3],const int32_t hi[3],unsigned starts,unsigned outside,const uint8_t *keep,uint32_t nkeep)
{
    uint32_t o,g=0;
    DlWriter wr;
    DlOwed ow;
    int open=0;                    /* (with keep: a BEGIN_VTXS sent, its END not yet) */
    if(!dst)return 0;
    memset(&wr,0,sizeof(wr));
    wr.out=dst;wr.cap=dstBytes/4u;
    memset(&ow,0,sizeof(ow));
    for(o=0;o<info->nops && !wr.full;){
        unsigned op=info->ops[o]&255u;
        const uint32_t *p=&w[info->ops[o]>>8];
        while(g<info->ngroups && info->groups[g].begin<o)g++;
        if(g<info->ngroups && info->groups[g].begin==o){
            const DlGroup *grp=&info->groups[g];
            if(keep ? (g>=nkeep || !keep[g]) : !dl_group_kept(grp,lo,hi,starts,outside)){
                dl_owe(&ow,grp,info->xyz[grp->end-1u]);
                o=grp->end;
                g++;
                continue;
            }
        }
        if(keep && op==0x40u){
            /* (groups chosen one by one: a BEGIN_VTXS whose commands are
             * all in groups left out, left out with them, and an END_VTXS
             * after nothing sent; the coordinates they leave owed sent only
             * for a vertex relative to them, as its own) */
            uint32_t e,h=g,at=o+1u;
            int any=0;
            for(e=o+1u;e<info->nops;e++){
                unsigned q=info->ops[e]&255u;
                if(q==0x40u || q==0x41u)break;
            }
            while(h<info->ngroups && info->groups[h].begin==at && info->groups[h].end<=e){
                if(h<nkeep && keep[h])any=1;
                at=info->groups[h].end;
                h++;
            }
            if(at==e && !any){o++;continue;}
        }
        if(keep && op==0x41u && !open){o++;continue;}
        dl_pay_state(&wr,&ow);
        if(ow.coordOwed){
            if(op==0x40u || op==0x41u){
                if(!keep){
                    dl_vtx16(&wr,ow.xyz);          /* (a vertex alone: no polygon) */
                    ow.coordOwed=0;
                }
            }else if(op>=0x23u && op<=0x28u){
                ow.coordOwed=0;
                if(op>=0x25u){
                    dl_vtx16(&wr,info->xyz[o]);    /* (relative to one left out: its own coordinates) */
                    o++;
                    continue;
                }
            }
        }
        if(op==0x40u)open=1;
        else if(op==0x41u)open=0;
        dl_put(&wr,op,p);
        o++;
    }
    if(!keep){
        dl_pay_state(&wr,&ow);
        if(ow.coordOwed)dl_vtx16(&wr,ow.xyz);      /* (not reached: groups end before a BEGIN or END) */
    }else if(wr.n==0u && wr.cap>0u){
        wr.out[wr.n++]=0u;                         /* (nothing kept: a word of NOPs) */
    }
    return wr.full?0:wr.n*4u;
}
static uint32_t dl_trim(const void *src,uint32_t bytes,void *dst,uint32_t dstBytes,
                        const int32_t lo[3],const int32_t hi[3],unsigned starts,unsigned outside,const uint8_t *keep,uint32_t nkeep)
{
    DlInfo *info;
    uint32_t r;
    if((bytes&3u) || !dst)return 0;
    info=dl_survey(src,bytes/4u,1u);
    if(!info)return 0;
    r=dl_trim_surveyed(info,src,dst,dstBytes,lo,hi,starts,outside,keep,nkeep);
    dl_free(info);
    return r;
}
uint32_t NitroPort_Gx3dTrimList(const void *src,uint32_t bytes,void *dst,uint32_t dstBytes,
                                int axis,int32_t lo,int32_t hi)
{
    int32_t l[3]={INT32_MIN,INT32_MIN,INT32_MIN},h[3]={INT32_MAX,INT32_MAX,INT32_MAX};
    if(axis<0 || axis>2)return 0;
    l[axis]=lo;h[axis]=hi;
    return dl_trim(src,bytes,dst,dstBytes,l,h,0u,0u,NULL,0u);
}
struct NitroGx3dListSurvey {
    const uint32_t *w;
    DlInfo *info;
};
NitroGx3dListSurvey *NitroPort_Gx3dSurveyList(const void *src,uint32_t bytes)
{
    NitroGx3dListSurvey *sv;
    if((bytes&3u) || !src)return NULL;
    sv=malloc(sizeof(*sv));
    if(!sv)return NULL;
    sv->w=src;
    sv->info=dl_survey(src,bytes/4u,1u);
    if(!sv->info){free(sv);return NULL;}
    return sv;
}
void NitroPort_Gx3dFreeSurvey(NitroGx3dListSurvey *sv)
{
    if(!sv)return;
    dl_free(sv->info);
    free(sv);
}
uint32_t NitroPort_Gx3dSurveyGroups(const NitroGx3dListSurvey *sv,int32_t (*box)[6],uint8_t *slopes,uint32_t max)
{
    const DlInfo *info;
    uint32_t g,n,q;
    int a;
    if(!sv)return 0;
    info=sv->info;
    n=info->ngroups;
    for(g=0;g<n && g<max;g++){
        const DlGroup *grp=&info->groups[g];
        for(a=0;a<3;a++){box[g][a]=grp->lo[a];box[g][3+a]=grp->hi[a];}
        if(slopes){
            /* (along x, then z: its vertices at the lowest and at the
             * highest each at one height, the two different) */
            uint8_t sl=0;
            for(a=0;a<3;a+=2){
                int32_t y0=0,y1=0;
                int have0=0,have1=0,one=1;
                if(grp->lo[a]==grp->hi[a])continue;
                for(q=grp->begin;q<grp->end && one;q++){
                    unsigned c=info->ops[q]&255u;
                    const int32_t *v=info->xyz[q];
                    if(c<0x23u || c>0x28u)continue;
                    if(v[a]==grp->lo[a]){if(!have0){y0=v[1];have0=1;}else if(v[1]!=y0)one=0;}
                    else if(v[a]==grp->hi[a]){if(!have1){y1=v[1];have1=1;}else if(v[1]!=y1)one=0;}
                }
                if(one && have0 && have1 && y0!=y1)sl|=(uint8_t)(a==0?1u:2u);
            }
            slopes[g]=sl;
        }
    }
    return n;
}
int NitroPort_Gx3dSurveyAllGrouped(const NitroGx3dListSurvey *sv)
{
    uint32_t o,g,vertices=0,grouped=0;
    if(!sv)return 0;
    for(o=0;o<sv->info->nops;o++){
        unsigned op=sv->info->ops[o]&255u;
        if(op>=0x23u && op<=0x28u)vertices++;
    }
    for(g=0;g<sv->info->ngroups;g++)grouped+=sv->info->groups[g].vertices;
    return vertices==grouped;
}
/* (a list sent twice in a row drawing the same twice: TEXCOORD, NORMAL and
 * COLOR each either not in it or set before its first vertex) */
static int dl_repeatable(const uint32_t *w,uint32_t n)
{
    uint32_t i=0,k;
    unsigned set=0,seen=0;
    int vertex=0;
    while(i<n){
        uint32_t word=w[i++],params=i;
        for(k=0;k<4;k++){
            unsigned op=(word>>(k*8))&255u,bit;
            if(!op)continue;
            params+=sParamCount[op];
            if(op>=0x23u && op<=0x28u){vertex=1;continue;}
            if(op<0x20u || op>0x22u)continue;
            bit=1u<<(op-0x20u);
            if(!vertex)set|=bit;
            else if(!(set&bit))return 0;
            seen|=bit;
        }
        i=params;
    }
    (void)seen;
    return 1;
}
uint32_t NitroPort_Gx3dSurveyTrim(const NitroGx3dListSurvey *sv,void *dst,uint32_t dstBytes,const uint8_t *keep,uint32_t nkeep,int *repeatable)
{
    int32_t l[3]={INT32_MIN,INT32_MIN,INT32_MIN},h[3]={INT32_MAX,INT32_MAX,INT32_MAX};
    uint32_t r;
    if(!sv || !keep)return 0;
    r=dl_trim_surveyed(sv->info,sv->w,dst,dstBytes,l,h,0u,0u,keep,nkeep);
    if(repeatable)*repeatable=r?dl_repeatable(dst,r/4u):0;
    return r;
}
uint32_t NitroPort_Gx3dSurveyRuns(const NitroGx3dListSurvey *sv,const uint8_t *mark,uint32_t nmark,
                                  void *dst,uint32_t dstBytes,uint32_t *sizes,uint8_t *runMark,uint32_t maxRuns)
{
    const DlInfo *info;
    uint32_t o,g,runs=0,at=0,begin=0;
    uint32_t value[3]={0,0,0},when[3]={0,0,0};  /* (COLOR, NORMAL, TEXCOORD: last set, and at which command, +1) */
    if(!sv || !mark || !dst || !sizes || !runMark)return 0;
    info=sv->info;
    if(!info->ngroups || nmark<info->ngroups || !NitroPort_Gx3dSurveyAllGrouped(sv))return 0;
    for(o=0;o<info->nops;o++){
        unsigned op=info->ops[o]&255u;
        if(!(op>=0x20u && op<=0x28u) && op!=0x40u && op!=0x41u)return 0;
    }
    for(g=0,o=0;g<info->ngroups;){
        const DlGroup *first=&info->groups[g];
        uint32_t last=g,k,s;
        DlWriter wr;
        unsigned order[3]={0,1,2};
        /* (its groups: up to the next with another mark) */
        while(last+1u<info->ngroups && mark[last+1u]==mark[g])last++;
        if(runs>=maxRuns)return 0;
        /* (the state set before it, and the BEGIN_VTXS it starts in) */
        for(;o<first->begin;o++){
            unsigned op=info->ops[o]&255u;
            if(op>=0x20u && op<=0x22u){value[op-0x20u]=sv->w[info->ops[o]>>8];when[op-0x20u]=o+1u;}
            else if(op==0x40u)begin=o;
        }
        memset(&wr,0,sizeof(wr));
        wr.out=(uint32_t *)((uint8_t *)dst+at);
        wr.cap=(dstBytes-at)/4u;
        /* (after its BEGIN_VTXS: a NORMAL's colour is worked out with the
         * lights the polygon attributes give at the BEGIN before it, and
         * before this one the shape's own aren't yet) */
        dl_put(&wr,0x40u,&sv->w[info->ops[begin]>>8]);
        for(k=0;k<3;k++)for(s=k+1u;s<3;s++)if(when[order[s]]<when[order[k]]){unsigned t=order[k];order[k]=order[s];order[s]=t;}
        for(k=0;k<3;k++)if(when[order[k]])dl_put(&wr,0x20u+order[k],&value[order[k]]);
        /* (its commands, the first vertex its own coordinates) */
        {
            int vertex=0;
            for(;o<info->groups[last].end;o++){
                unsigned op=info->ops[o]&255u;
                const uint32_t *p=&sv->w[info->ops[o]>>8];
                if(op>=0x20u && op<=0x22u){value[op-0x20u]=p[0];when[op-0x20u]=o+1u;}
                else if(op==0x40u)begin=o;
                if(op>=0x23u && op<=0x28u && !vertex){
                    vertex=1;
                    if(op>=0x25u){dl_vtx16(&wr,info->xyz[o]);continue;}
                }
                dl_put(&wr,op,p);
            }
        }
        dl_put(&wr,0x41u,NULL);
        if(wr.full)return 0;
        sizes[runs]=wr.n*4u;
        runMark[runs]=mark[g];
        at+=wr.n*4u;
        runs++;
        g=last+1u;
    }
    /* (and what it leaves set, after its last vertex too: what's sent after
     * it takes that, whichever runs were sent) */
    for(;o<info->nops;o++){
        unsigned op=info->ops[o]&255u;
        if(op>=0x20u && op<=0x22u){value[op-0x20u]=sv->w[info->ops[o]>>8];when[op-0x20u]=o+1u;}
    }
    if(when[0] || when[1] || when[2]){
        DlWriter wr;
        unsigned order[3]={0,1,2},k,s;
        if(runs>=maxRuns)return 0;
        memset(&wr,0,sizeof(wr));
        wr.out=(uint32_t *)((uint8_t *)dst+at);
        wr.cap=(dstBytes-at)/4u;
        /* (in a BEGIN_VTXS of its own, no vertex: its NORMAL lit with the
         * shape's lights, whether or not any of its runs were sent) */
        dl_put(&wr,0x40u,&sv->w[info->ops[begin]>>8]);
        for(k=0;k<3;k++)for(s=k+1u;s<3;s++)if(when[order[s]]<when[order[k]]){unsigned t=order[k];order[k]=order[s];order[s]=t;}
        for(k=0;k<3;k++)if(when[order[k]])dl_put(&wr,0x20u+order[k],&value[order[k]]);
        dl_put(&wr,0x41u,NULL);
        if(wr.full)return 0;
        sizes[runs]=wr.n*4u;
        runMark[runs]=0xFFu;
        runs++;
    }
    return runs;
}
uint32_t NitroPort_Gx3dSurveyMoved(const NitroGx3dListSurvey *sv,void *dst,uint32_t dstBytes,const uint8_t *keep,uint32_t nkeep,
                                   int32_t originX,int32_t num,int32_t den,int32_t dx,int32_t dy,int32_t dz)
{
    uint32_t n,o,room;
    uint32_t *tmp;
    DlInfo *info;
    DlWriter wr;
    uint32_t held[3]={0,0,0},heldWhen[3]={0,0,0};
    unsigned k,s;
    int inside=0;
    if(!sv || !keep || !dst || den<=0)return 0;
    /* (the groups kept, as a trim has them: every vertex's coordinates
     * known from the list itself) */
    room=sv->info->nops*3u+16u;
    tmp=malloc(room*4u);
    if(!tmp)return 0;
    n=dl_trim_surveyed(sv->info,sv->w,tmp,room*4u,NULL,NULL,0u,0u,keep,nkeep);
    info=n?dl_survey(tmp,n/4u,1u):NULL;
    if(!info){free(tmp);return 0;}
    memset(&wr,0,sizeof(wr));
    wr.out=dst;wr.cap=dstBytes/4u;
    for(o=0;o<info->nops && !wr.full;o++){
        unsigned op=info->ops[o]&255u;
        const uint32_t *p=&tmp[info->ops[o]>>8];
        if(op>=0x23u && op<=0x28u){
            int32_t xyz[3];
            int64_t x=(int64_t)info->xyz[o][0]-originX;
            xyz[0]=(int32_t)(originX+(x*num+(x>=0?den/2:-den/2))/den+dx);
            xyz[1]=info->xyz[o][1]+dy;
            xyz[2]=info->xyz[o][2]+dz;
            if(!dl_s16(xyz)){dl_free(info);free(tmp);return 0;}
            dl_vtx16(&wr,xyz);
        }else if(op>=0x20u && op<=0x22u && !inside){
            /* (outside a BEGIN_VTXS: sent after the next one, a NORMAL lit
             * with the polygon's own lights) */
            held[op-0x20u]=p[0];
            heldWhen[op-0x20u]=o+1u;
        }else{
            dl_put(&wr,op,p);
            if(op==0x40u){
                unsigned order[3]={0,1,2};
                inside=1;
                for(k=0;k<3;k++)for(s=k+1u;s<3;s++)if(heldWhen[order[s]]<heldWhen[order[k]]){unsigned t=order[k];order[k]=order[s];order[s]=t;}
                for(k=0;k<3;k++)if(heldWhen[order[k]])dl_put(&wr,0x20u+order[k],&held[order[k]]);
                heldWhen[0]=heldWhen[1]=heldWhen[2]=0;
            }else if(op==0x41u){
                inside=0;
            }
        }
    }
    dl_free(info);
    free(tmp);
    if(wr.full || wr.n==0u)return 0;
    return wr.n*4u;
}
/* A NORMAL's x and z turned a quarter clockwise (seen from above, north up:
 * east to south), turn times; y kept. 10-bit signed fields. */
static uint32_t dl_normal_turned(uint32_t n,unsigned turn)
{
    int32_t x=(int32_t)(n<<22)>>22,y=(int32_t)(n<<12)>>22,z=(int32_t)(n<<2)>>22;
    unsigned t;
    for(t=0;t<(turn&3u);t++){int32_t nx=-z;z=x;x=nx>511?511:nx;}
    return (uint32_t)(x&0x3FF)|((uint32_t)(y&0x3FF)<<10)|((uint32_t)(z&0x3FF)<<20);
}
uint32_t NitroPort_Gx3dSurveyTurned(const NitroGx3dListSurvey *sv,void *dst,uint32_t dstBytes,const uint8_t *keep,uint32_t nkeep,
                                    unsigned turn,int32_t pivotX,int32_t pivotZ,int32_t dx,int32_t dy,int32_t dz)
{
    uint32_t n,o,room;
    uint32_t *tmp;
    DlInfo *info;
    DlWriter wr;
    uint32_t held[3]={0,0,0},heldWhen[3]={0,0,0};
    unsigned k,s,t;
    int inside=0;
    if(!sv || !keep || !dst)return 0;
    /* (the groups kept, as NitroPort_Gx3dSurveyMoved has them) */
    room=sv->info->nops*3u+16u;
    tmp=malloc(room*4u);
    if(!tmp)return 0;
    n=dl_trim_surveyed(sv->info,sv->w,tmp,room*4u,NULL,NULL,0u,0u,keep,nkeep);
    info=n?dl_survey(tmp,n/4u,1u):NULL;
    if(!info){free(tmp);return 0;}
    memset(&wr,0,sizeof(wr));
    wr.out=dst;wr.cap=dstBytes/4u;
    for(o=0;o<info->nops && !wr.full;o++){
        unsigned op=info->ops[o]&255u;
        const uint32_t *p=&tmp[info->ops[o]>>8];
        uint32_t normal;
        if(op>=0x23u && op<=0x28u){
            int32_t xyz[3];
            int64_t x=(int64_t)info->xyz[o][0]-pivotX,z=(int64_t)info->xyz[o][2]-pivotZ;
            for(t=0;t<(turn&3u);t++){int64_t nx=-z;z=x;x=nx;}
            xyz[0]=(int32_t)(pivotX+x+dx);
            xyz[1]=info->xyz[o][1]+dy;
            xyz[2]=(int32_t)(pivotZ+z+dz);
            if(!dl_s16(xyz)){dl_free(info);free(tmp);return 0;}
            dl_vtx16(&wr,xyz);
            continue;
        }
        /* (a NORMAL turned with its polygon) */
        if(op==0x21u){normal=dl_normal_turned(p[0],turn);p=&normal;}
        if(op>=0x20u && op<=0x22u && !inside){
            held[op-0x20u]=p[0];
            heldWhen[op-0x20u]=o+1u;
        }else{
            dl_put(&wr,op,p);
            if(op==0x40u){
                unsigned order[3]={0,1,2};
                inside=1;
                for(k=0;k<3;k++)for(s=k+1u;s<3;s++)if(heldWhen[order[s]]<heldWhen[order[k]]){unsigned u=order[k];order[k]=order[s];order[s]=u;}
                for(k=0;k<3;k++)if(heldWhen[order[k]])dl_put(&wr,0x20u+order[k],&held[order[k]]);
                heldWhen[0]=heldWhen[1]=heldWhen[2]=0;
            }else if(op==0x41u){
                inside=0;
            }
        }
    }
    dl_free(info);
    free(tmp);
    if(wr.full || wr.n==0u)return 0;
    return wr.n*4u;
}
/* ------------------------------------------ the land's trees' sun shadows
 *
 * NitroPort_Gx3dTreeLists (nitro_port_gx3d.h): a land model's tree shapes
 * read once into what's sent each scene instead: the shape's polygons with
 * its stumps apart, and its trees' shadows as polygons of their own lying
 * on the ground, which then go through the engine as any polygon does
 * (clipped at the view's sides as it is, halfway in the in-between scenes
 * with the land: nothing worked out for them a scene). */
#define TREE_VERTS 8
/* (a tree's trunk for its shadow, in world units: half as wide at the
 * ground and at its top, and how tall: the stump is about 14 across where
 * it's cut and 20 at its roots, the lowest layer starts 6.3 up and is
 * solid by 12) */
#define TREE_TRUNK_FOOT 5
#define TREE_TRUNK_TOP 4
#define TREE_TRUNK_TALL 12
typedef struct TreeQuad {          /* (a polygon of a shape's list, as read) */
    int32_t xyz[TREE_VERTS][3], st[TREE_VERTS][2];
    int32_t lo[3], hi[3];
    uint8_t n, flat;
} TreeQuad;
typedef struct TreePoly {          /* (a polygon to lay: about where its tree stands) */
    int32_t base[3];               /* x, the ground's y, z */
    int32_t u[TREE_VERTS], h[TREE_VERTS], st[TREE_VERTS][2];
    uint8_t n, first;              /* (first: its tree's, or its row's, first one) */
} TreePoly;
typedef struct TreeShape {
    uint32_t *canopy, *stumps, *tail, *shadow;
    uint32_t canopyBytes, stumpBytes, tailBytes, shadowBytes, shadowCap;
    TreePoly *polys;
    uint32_t npolys, trees;
    int32_t sun[3];
    int sunSet;
} TreeShape;
struct NitroGx3dTreeLists {
    uint32_t count;
    int32_t lift;                  /* (the shadows this far above their ground) */
    TreeShape shape[1];
};
/* (a shape's polygons, each with its vertices' coordinates and texture
 * coordinates; one of more than TREE_VERTS vertices, a strip, has n 0) */
static TreeQuad *tree_quads(const DlInfo *info,const uint32_t *w,int32_t flatSpan)
{
    TreeQuad *quads=calloc(info->ngroups?info->ngroups:1u,sizeof(*quads));
    int32_t st[2]={0,0};
    uint32_t o,g=0;
    if(!quads)return NULL;
    for(o=0;o<info->nops;o++){
        unsigned op=info->ops[o]&255u;
        while(g<info->ngroups && o>=info->groups[g].end)g++;
        if(op==0x22u){
            uint32_t v=w[info->ops[o]>>8];
            st[0]=signbits(v,16);st[1]=signbits(v>>16,16);
        }else if(op>=0x23u && op<=0x28u && g<info->ngroups && o>=info->groups[g].begin){
            TreeQuad *q=&quads[g];
            if(info->groups[g].vertices>TREE_VERTS)continue;
            memcpy(q->xyz[q->n],info->xyz[o],sizeof(q->xyz[0]));
            q->st[q->n][0]=st[0];q->st[q->n][1]=st[1];
            q->n++;
        }
    }
    for(g=0;g<info->ngroups;g++){
        TreeQuad *q=&quads[g];
        int a;
        if(q->n<3){q->n=0;continue;}
        for(a=0;a<3;a++){q->lo[a]=info->groups[g].lo[a];q->hi[a]=info->groups[g].hi[a];}
        q->flat=q->hi[1]-q->lo[1]<flatSpan;
    }
    return quads;
}
/* (the shape's list as two, its flat polygons apart, and what it leaves
 * set: NitroPort_Gx3dSurveyRuns' runs, each drawing its polygons whichever
 * were sent before, those of a kind one after another) */
static void tree_split(TreeShape *sh,const DlInfo *info,const uint32_t *w,uint32_t words,const TreeQuad *quads)
{
    NitroGx3dListSurvey sv;
    uint32_t n=info->ngroups,room=(words+n*12u+32u)*4u,runs,r,at=0,flats=0;
    uint8_t *mark=malloc(n?n:1u),*runMark=malloc(n+2u);
    uint32_t *sizes=malloc((n+2u)*sizeof(*sizes));
    uint8_t *buf=malloc(room);
    sv.w=w;sv.info=(DlInfo *)info;
    if(mark && runMark && sizes && buf && n){
        for(r=0;r<n;r++){mark[r]=quads[r].n && quads[r].flat;flats+=mark[r];}
        runs=NitroPort_Gx3dSurveyRuns(&sv,mark,n,buf,room,sizes,runMark,n+2u);
        for(r=0;r<runs;r++){
            if(runMark[r]==0u)sh->canopyBytes+=sizes[r];
            else if(runMark[r]==1u)sh->stumpBytes+=sizes[r];
            else sh->tailBytes+=sizes[r];
        }
        if(runs){
            /* (no flat polygon: the shape is drawn as it is) */
            if(flats){
                sh->canopy=malloc(sh->canopyBytes?sh->canopyBytes:4u);
                sh->stumps=malloc(sh->stumpBytes?sh->stumpBytes:4u);
            }
            /* (and room for the list's last coordinates after what it
             * leaves set: below) */
            sh->tail=malloc(sh->tailBytes+16u);
            if((flats && (!sh->canopy || !sh->stumps)) || !sh->tail){
                free(sh->canopy);free(sh->stumps);free(sh->tail);
                sh->canopy=sh->stumps=sh->tail=NULL;
            }else{
                uint32_t c=0,s=0,t=0;
                for(r=0;r<runs;r++){
                    if(runMark[r]==0u){if(flats)memcpy((uint8_t *)sh->canopy+c,buf+at,sizes[r]);c+=sizes[r];}
                    else if(runMark[r]==1u){memcpy((uint8_t *)sh->stumps+s,buf+at,sizes[r]);s+=sizes[r];}
                    else{memcpy((uint8_t *)sh->tail+t,buf+at,sizes[r]);t+=sizes[r];}
                    at+=sizes[r];
                }
            }
        }
    }
    if(!sh->canopy)sh->canopyBytes=0;
    if(!sh->stumps)sh->stumpBytes=0;
    if(!sh->tail)sh->tailBytes=0;
    else if(info->nops){
        /* The coordinates the list leaves too (the shadows' vertices come
         * after its own: a list after it whose first vertex is relative
         * to the one before must find this list's last): a vertex alone
         * in a list of triangles, no polygon. */
        DlWriter wr;
        const uint32_t mode=0;
        memset(&wr,0,sizeof(wr));
        wr.out=(uint32_t *)((uint8_t *)sh->tail+sh->tailBytes);
        wr.cap=4u;
        dl_put(&wr,0x40u,&mode);
        dl_vtx16(&wr,info->xyz[info->nops-1u]);
        dl_put(&wr,0x41u,NULL);
        if(!wr.full && dl_s16(info->xyz[info->nops-1u]))sh->tailBytes+=wr.n*4u;
    }
    free(mark);free(runMark);free(sizes);free(buf);
}
/* (a polygon cut to lo <= x <= hi: Sutherland and Hodgman's, its texture
 * coordinates with it; the vertices left) */
static unsigned tree_clip(const TreeQuad *q,int32_t lo,int32_t hi,int32_t xyz[TREE_VERTS][3],int32_t st[TREE_VERTS][2])
{
    int32_t a[TREE_VERTS][5],b[TREE_VERTS][5];
    unsigned n=q->n,m,i,pass;
    int k;
    for(i=0;i<n;i++){
        for(k=0;k<3;k++)a[i][k]=q->xyz[i][k];
        a[i][3]=q->st[i][0];a[i][4]=q->st[i][1];
    }
    for(pass=0;pass<2;pass++){
        int32_t edge=pass?hi:lo;
        m=0;
        for(i=0;i<n;i++){
            const int32_t *p=a[i],*r=a[(i+1u)%n];
            int64_t dp=pass?(int64_t)edge-p[0]:(int64_t)p[0]-edge,dr=pass?(int64_t)edge-r[0]:(int64_t)r[0]-edge;
            if(dp>=0 && m<TREE_VERTS)memcpy(b[m++],p,sizeof(b[0]));
            if((dp<0)!=(dr<0) && m<TREE_VERTS){
                for(k=0;k<5;k++){
                    int64_t num=((int64_t)r[k]-p[k])*dp,den=dp-dr;
                    b[m][k]=p[k]+(int32_t)((num+(((num<0)!=(den<0))?-den/2:den/2))/den);
                }
                b[m][0]=edge;
                m++;
            }
        }
        if(m<3)return 0;
        memcpy(a,b,sizeof(a));
        n=m;
    }
    for(i=0;i<n;i++){
        for(k=0;k<3;k++)xyz[i][k]=a[i][k];
        st[i][0]=a[i][3];st[i][1]=a[i][4];
    }
    return n;
}
static void tree_add(TreeShape *sh,uint32_t *cap,const int32_t base[3],unsigned n,
                     int32_t xyz[TREE_VERTS][3],int32_t st[TREE_VERTS][2],int first)
{
    TreePoly *p;
    unsigned i;
    if(sh->npolys>=*cap){
        TreePoly *more=realloc(sh->polys,(*cap=*cap?*cap*2u:64u)*sizeof(*more));
        if(!more)return;
        sh->polys=more;
    }
    p=&sh->polys[sh->npolys++];
    memcpy(p->base,base,sizeof(p->base));
    p->n=(uint8_t)n;
    p->first=(uint8_t)first;
    for(i=0;i<n;i++){
        p->u[i]=xyz[i][0]-base[0];
        p->h[i]=xyz[i][1]-base[1];
        p->st[i][0]=st[i][0];p->st[i][1]=st[i][1];
    }
}
/* (the shape's trees, and each one's polygons to lay; unit: a world unit
 * in the list's) */
static void tree_sources(TreeShape *sh,const TreeQuad *quads,uint32_t n,TreeQuad *const *all,const uint32_t *counts,
                         uint32_t shapes,int32_t unit)
{
    const int32_t nearBy=12*unit,over=8*unit,column=32*unit,wide=48*unit,tall=5*unit,low=unit+unit/2;
    uint32_t g=0,cap=0;
    while(g<n){
        uint32_t first=g,last,k;
        int32_t mx,mz,lo[3]={0,0,0},hi[3]={0,0,0},flo=0,fhi=0,fy=0,base[3];
        int have=0,flat=0;
        if(!quads[g].n){g++;continue;}
        mx=quads[g].lo[0]/2+quads[g].hi[0]/2;
        mz=quads[g].lo[2]/2+quads[g].hi[2]/2;
        for(last=g;last<n;last++){
            const TreeQuad *q=&quads[last];
            int32_t qx,qz;
            int a;
            if(!q->n)continue;
            qx=q->lo[0]/2+q->hi[0]/2;qz=q->lo[2]/2+q->hi[2]/2;
            if(qx-mx>nearBy || mx-qx>nearBy || qz-mz>nearBy || mz-qz>nearBy)break;
            if(q->flat){
                if(!flat || q->lo[2]<flo)flo=q->lo[2];
                if(!flat || q->hi[2]>fhi)fhi=q->hi[2];
                if(!flat || q->lo[1]<fy)fy=q->lo[1];
                flat=1;
            }
            for(a=0;a<3;a++){
                if(!have || q->lo[a]<lo[a])lo[a]=q->lo[a];
                if(!have || q->hi[a]>hi[a])hi[a]=q->hi[a];
            }
            have=1;
        }
        g=last;
        /* where it stands */
        base[0]=lo[0]/2+hi[0]/2;
        if(flat){
            base[1]=fy;
            base[2]=flo/2+fhi/2;
        }else{
            const TreeQuad *found=NULL;
            int32_t cx=base[0],cz=lo[2]/2+hi[2]/2;
            uint32_t s2,j;
            for(s2=0;s2<shapes;s2++){
                for(j=0;all[s2] && j<counts[s2];j++){
                    const TreeQuad *q=&all[s2][j];
                    if(!q->n || !q->flat)continue;
                    if(cx<q->lo[0]-over || cx>q->hi[0]+over || cz<q->lo[2]-over || cz>q->hi[2]+over ||
                       q->lo[1]>lo[1]+2*unit)
                        continue;
                    if(!found || q->lo[1]>found->lo[1])found=q;
                }
            }
            if(found){
                base[1]=found->lo[1];
                base[2]=found->lo[2]/2+found->hi[2]/2;
            }else{
                /* (its lowest vertices: their middle in depth) */
                int32_t zlo=0,zhi=0;
                int any=0;
                unsigned v;
                for(k=first;k<last;k++){
                    for(v=0;v<quads[k].n;v++){
                        const int32_t *p=quads[k].xyz[v];
                        if(p[1]>lo[1]+low)continue;
                        if(!any || p[2]<zlo)zlo=p[2];
                        if(!any || p[2]>zhi)zhi=p[2];
                        any=1;
                    }
                }
                base[1]=lo[1];
                base[2]=zlo/2+zhi/2;
            }
        }
        if(hi[1]-base[1]<tall)continue;
        /* its polygons standing up; a row's a tree at a time */
        {
            uint32_t before=sh->npolys,columns=1;
            for(k=first;k<last;k++){
                const TreeQuad *q=&quads[k];
                int32_t xyz[TREE_VERTS][3],st[TREE_VERTS][2];
                if(!q->n || q->flat)continue;
                if(q->hi[0]-q->lo[0]>wide){
                    int32_t x0,b2[3];
                    uint32_t c=0;
                    for(x0=q->lo[0];x0<q->hi[0];x0+=column,c++){
                        unsigned m=tree_clip(q,x0,x0+column,xyz,st);
                        if(m<3)continue;
                        b2[0]=x0+column/2;b2[1]=base[1];b2[2]=base[2];
                        tree_add(sh,&cap,b2,m,xyz,st,sh->npolys==before);
                    }
                    if(c>columns)columns=c;
                }else{
                    memcpy(xyz,q->xyz,sizeof(xyz));
                    memcpy(st,q->st,sizeof(st));
                    tree_add(sh,&cap,base,q->n,xyz,st,sh->npolys==before);
                }
            }
            if(sh->npolys>before){
                sh->trees+=columns;
                /* Its trunk: the stump is painted flat on the ground and
                 * the layers start some way up, so their shadows alone
                 * lie apart from the tree, with nothing joining them to
                 * its foot (2026-11-10: "do not include the trunk making
                 * the shadow feel fake"). A strip from where each stump
                 * is up into the lowest layer, laid with them; its
                 * texture coordinates all the stump's middle (opaque:
                 * the shadows are drawn where the texel is). Only with
                 * the stumps in the shape itself: the texture is theirs. */
                for(k=first;k<last && flat;k++){
                    const TreeQuad *q=&quads[k];
                    int32_t x0,x1,step;
                    if(!q->n || !q->flat)continue;
                    if(q->hi[0]-q->lo[0]>wide){x0=q->lo[0];x1=q->hi[0];step=column;}
                    else{x0=base[0]-column/2;x1=x0+1;step=column;}
                    for(;x0<x1;x0+=step){
                        const int32_t bx=x0+column/2,foot=TREE_TRUNK_FOOT*unit,top=TREE_TRUNK_TOP*unit,
                                      up=TREE_TRUNK_TALL*unit;
                        int32_t xyz[TREE_VERTS][3],st[TREE_VERTS][2],b2[3];
                        int64_t ss=0,tt=0;
                        unsigned m=tree_clip(q,bx-unit,bx+unit,xyz,st),v;
                        if(m<3)continue;
                        for(v=0;v<m;v++){ss+=st[v][0];tt+=st[v][1];}
                        b2[0]=bx;b2[1]=base[1];b2[2]=base[2];
                        for(v=0;v<4u;v++){
                            xyz[v][0]=bx+((v==0u||v==3u)?-1:1)*(v<2u?foot:top);
                            xyz[v][1]=base[1]+(v<2u?0:up);
                            xyz[v][2]=base[2];
                            st[v][0]=(int32_t)(ss/(int64_t)m);st[v][1]=(int32_t)(tt/(int64_t)m);
                        }
                        tree_add(sh,&cap,b2,4u,xyz,st,0);
                    }
                    if(q->hi[0]-q->lo[0]<=wide)break;   /* (a tree: one stump) */
                }
            }
        }
    }
}
NitroGx3dTreeLists *NitroPort_Gx3dTreeLists(const void *const *lists,const uint32_t *bytes,uint32_t count,int32_t posScale)
{
    NitroGx3dTreeLists *t;
    DlInfo **info;
    TreeQuad **quads;
    uint32_t *counts,i;
    int32_t unit;
    if(!lists || !bytes || !count || count>32u || posScale<=0)return NULL;
    unit=(int32_t)((((int64_t)4096)<<12)/posScale);
    if(unit<=0 || unit>256)return NULL;
    t=calloc(1,sizeof(*t)+(count-1u)*sizeof(TreeShape));
    info=calloc(count,sizeof(*info));
    quads=calloc(count,sizeof(*quads));
    counts=calloc(count,sizeof(*counts));
    if(!t || !info || !quads || !counts){free(t);free(info);free(quads);free(counts);return NULL;}
    t->count=count;
    t->lift=unit;
    for(i=0;i<count;i++){
        if(!lists[i] || (bytes[i]&3u) || !bytes[i])continue;
        info[i]=dl_survey(lists[i],bytes[i]/4u,1u);
        if(!info[i])continue;
        quads[i]=tree_quads(info[i],lists[i],unit*3/4);
        if(quads[i])counts[i]=info[i]->ngroups;
    }
    for(i=0;i<count;i++){
        TreeShape *sh=&t->shape[i];
        if(!quads[i])continue;
        tree_split(sh,info[i],lists[i],bytes[i]/4u,quads[i]);
        /* (no tail: the list isn't one of polygons alone, left as it is) */
        if(!sh->tail)continue;
        tree_sources(sh,quads[i],counts[i],quads,counts,count,unit);
        if(sh->npolys){
            /* (a vertex: a TEXCOORD and a VTX_16, four commands a word of
             * their own; a polygon not a quad as triangles) */
            sh->shadowCap=sh->npolys*((TREE_VERTS-2u)*3u*4u+4u)+16u;
            sh->shadow=malloc(sh->shadowCap*4u);
            if(!sh->shadow){free(sh->polys);sh->polys=NULL;sh->npolys=0;sh->trees=0;}
        }
    }
    for(i=0;i<count;i++){dl_free(info[i]);free(quads[i]);}
    free(info);free(quads);free(counts);
    return t;
}
void NitroPort_Gx3dFreeTreeLists(NitroGx3dTreeLists *t)
{
    uint32_t i;
    if(!t)return;
    for(i=0;i<t->count;i++){
        free(t->shape[i].canopy);free(t->shape[i].stumps);free(t->shape[i].tail);
        free(t->shape[i].shadow);free(t->shape[i].polys);
    }
    free(t);
}
const void *NitroPort_Gx3dTreeCanopies(const NitroGx3dTreeLists *t,uint32_t shape,uint32_t *bytes)
{
    if(bytes)*bytes=0;
    if(!t || shape>=t->count || !t->shape[shape].canopy)return NULL;
    if(bytes)*bytes=t->shape[shape].canopyBytes;
    return t->shape[shape].canopy;
}
const void *NitroPort_Gx3dTreeStumps(const NitroGx3dTreeLists *t,uint32_t shape,uint32_t *bytes)
{
    if(bytes)*bytes=0;
    if(!t || shape>=t->count || !t->shape[shape].stumps)return NULL;
    if(bytes)*bytes=t->shape[shape].stumpBytes;
    return t->shape[shape].stumps;
}
const void *NitroPort_Gx3dTreeTail(const NitroGx3dTreeLists *t,uint32_t shape,uint32_t *bytes)
{
    if(bytes)*bytes=0;
    if(!t || shape>=t->count || !t->shape[shape].tail || !t->shape[shape].tailBytes)return NULL;
    if(bytes)*bytes=t->shape[shape].tailBytes;
    return t->shape[shape].tail;
}
uint32_t NitroPort_Gx3dTreeCount(const NitroGx3dTreeLists *t,uint32_t shape)
{
    return t && shape<t->count?t->shape[shape].trees:0u;
}
static int32_t tree_fx(int64_t v)
{
    return (int32_t)((v+(v>=0?2048:-2048))/4096);
}
const void *NitroPort_Gx3dTreeShadows(NitroGx3dTreeLists *t,uint32_t shape,int32_t wayX,int32_t wayZ,int32_t stretch,
                                      uint32_t *bytes)
{
    TreeShape *sh;
    DlWriter wr;
    int32_t sx,sz;
    uint32_t i;
    if(bytes)*bytes=0;
    if(!t || shape>=t->count)return NULL;
    sh=&t->shape[shape];
    if(!sh->npolys || !sh->shadow)return NULL;
    if(sh->sunSet && sh->sun[0]==wayX && sh->sun[1]==wayZ && sh->sun[2]==stretch){
        if(bytes)*bytes=sh->shadowBytes;
        return sh->shadowBytes?sh->shadow:NULL;
    }
    sh->sun[0]=wayX;sh->sun[1]=wayZ;sh->sun[2]=stretch;
    sh->sunSet=1;
    sh->shadowBytes=0;
    memset(&wr,0,sizeof(wr));
    wr.out=sh->shadow;
    wr.cap=sh->shadowCap;
    sx=tree_fx((int64_t)wayX*stretch);
    sz=tree_fx((int64_t)wayZ*stretch);
    /* A list of vertices a tree (or a row): one out of the view is passed
     * over whole by its box (skip_hidden_vertices), not vertex by vertex.
     * Quads; a polygon that isn't one (cut at a slant) as triangles. */
    {
        int begun=0;
        uint32_t mode=1u;
        for(i=0;i<sh->npolys;i++){
            const TreePoly *p=&sh->polys[i];
            int32_t xyz[TREE_VERTS][3];
            uint32_t want=p->n==4u?1u:0u;
            unsigned k,ok=1;
            if(p->first && begun){dl_put(&wr,0x41u,NULL);begun=0;}
            /* across the tree goes across the light's way (z, -x: laid
             * towards the camera, the tree's right stays on the right),
             * up it along the way */
            for(k=0;k<p->n;k++){
                xyz[k][0]=p->base[0]+tree_fx((int64_t)wayZ*p->u[k]+(int64_t)sx*p->h[k]);
                xyz[k][1]=p->base[1]+t->lift;
                xyz[k][2]=p->base[2]+tree_fx(-(int64_t)wayX*p->u[k]+(int64_t)sz*p->h[k]);
                if(!dl_s16(xyz[k]))ok=0;
            }
            if(!ok)continue;
            if(begun && want!=mode){dl_put(&wr,0x41u,NULL);begun=0;}
            if(!begun){mode=want;dl_put(&wr,0x40u,&mode);begun=1;}
            /* (laid down, the polygon faces the other way: its vertices
             * the other way round, so it's drawn from above as the tree's
             * own is from the front) */
            if(mode==1u){
                for(k=p->n;k-->0u;){
                    uint32_t tc=((uint32_t)p->st[k][0]&0xFFFFu)|((uint32_t)p->st[k][1]<<16);
                    dl_put(&wr,0x22u,&tc);
                    dl_vtx16(&wr,xyz[k]);
                }
            }else{
                unsigned f;
                for(f=1;f+1u<p->n;f++){
                    const unsigned order[3]={0u,f+1u,f};
                    for(k=0;k<3u;k++){
                        unsigned v=order[k];
                        uint32_t tc=((uint32_t)p->st[v][0]&0xFFFFu)|((uint32_t)p->st[v][1]<<16);
                        dl_put(&wr,0x22u,&tc);
                        dl_vtx16(&wr,xyz[v]);
                    }
                }
            }
        }
        if(begun)dl_put(&wr,0x41u,NULL);
    }
    if(wr.full || !wr.n)return NULL;
    sh->shadowBytes=wr.n*4u;
    if(bytes)*bytes=sh->shadowBytes;
    return sh->shadow;
}
uint32_t NitroPort_Gx3dTranslateCommand(void *dst,uint32_t dstBytes,int32_t x,int32_t y,int32_t z)
{
    uint32_t *w=dst;
    if(!dst || dstBytes<16u)return 0;
    w[0]=0x1Cu;                                    /* (MTX_TRANS, then three NOPs) */
    w[1]=(uint32_t)x;w[2]=(uint32_t)y;w[3]=(uint32_t)z;
    return 16u;
}
uint32_t NitroPort_Gx3dTrimListRows(const void *src,uint32_t bytes,void *dst,uint32_t dstBytes,
                                    int32_t xlo,int32_t xhi,int32_t zlo,int32_t zhi,int inside)
{
    int32_t l[3]={xlo,INT32_MIN,zlo},h[3]={xhi,INT32_MAX,zhi};
    return dl_trim(src,bytes,dst,dstBytes,l,h,4u,inside?0u:4u,NULL,0u);
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
