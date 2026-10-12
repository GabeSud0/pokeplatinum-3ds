/*
 * Nintendo DS 3D geometry engine (ARM9), reference implementation.
 *
 * GXFIFO is drained synchronously. This is intentional: the game's
 * MI_SendGXCommandAsync expects a less-than-half-full FIFO interrupt, and
 * rendering is not yet paced to DS geometry-engine clock cycles.
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
#include <limits.h>
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
    NitroGx3dVertex screen;
} GeoVertex;
typedef struct GeoState {
    Matrix proj, pos, vec, tex;
    Matrix clip;        /* pos x proj, as the DS keeps it; see ensure_clip */
    int clipDirty;
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
    uint16_t lightColor[4];
    int viewport[4], primitive, nverts;
    GeoVertex vertices[4];
    int inBegin, boxVisible, stripStarted;
    int packedIndex, packedCount, need, used, directOpcode, directNeed;
    uint8_t packed[4];
    uint32_t params[32];
    int ready;
} GeoState;

static GeoState s;

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
static uint32_t sSerial;

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
/* Matrices the game multiplies are mostly zeros (scales, translations,
 * 4x3 and 3x3 matrices, the projection): only the products of non-zero
 * elements are worked out. Leaving out a zero term doesn't change a sum. */
static void multiply(Matrix *out, const Matrix *lhs, const Matrix *rhs)
{
    Matrix result;
    unsigned rows[4]={0,0,0,0},cols[4]={0,0,0,0};
    int i,j,k;
    for(i=0;i<4;i++)for(k=0;k<4;k++){
        if(lhs->a[i*4+k])rows[i]|=1u<<k;   /* row i's non-zero columns */
        if(rhs->a[i*4+k])cols[k]|=1u<<i;   /* column k's non-zero rows */
    }
    for(i=0;i<4;++i)for(j=0;j<4;++j){
        unsigned terms=rows[i]&cols[j];
        int64_t acc=0;
        for(k=0;terms;k++,terms>>=1)
            if(terms&1u)acc+=(int64_t)lhs->a[i*4+k]*rhs->a[k*4+j];
        result.a[i*4+j]=saturation(acc >> 12);
    }
    *out=result;
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
/* transform() for a point (w = 1.0): what every vertex goes through. */
static void transform_point(const int32_t in[4],const Matrix *m,int32_t out[4])
{
    int64_t x=in[0],y=in[1],z=in[2];
    int j;
    for(j=0;j<4;j++){
        int64_t v=x*m->a[j]+y*m->a[4+j]+z*m->a[8+j]+((int64_t)m->a[12+j]<<12);
        out[j]=saturation(v>>12);
    }
}
static Matrix *selected(void)
{
    if (s.mode==0) return &s.proj;
    if (s.mode==3) return &s.tex;
    return &s.pos;
}
static void set_matrix(const Matrix *m, int scale)
{
    *selected()=*m;
    if(s.mode==2 && !scale) s.vec=*m;
}
/* MTX_MULT/SCALE/TRANS: Current = Parameter x Current (GBATEK). With
 * row vectors (v' = v x M), the new matrix applies to a vertex first:
 * a model's own transform goes before the camera loaded earlier. */
static void mul_matrix(const Matrix *m, int scale)
{
    Matrix newpos, newvec;
    multiply(&newpos,m,selected());
    if(s.mode==2 && !scale) multiply(&newvec,m,&s.vec);
    *selected()=newpos;
    if(s.mode==2 && !scale) s.vec=newvec;
}
static void matrix_stack(unsigned opcode,int32_t param)
{
    int index;
    if(s.mode==0){
        if(opcode==0x11){if(s.pjLevel>=1) s.stackError=1;else {s.pjStack=s.proj;s.pjLevel++;}}
        else if(opcode==0x12){if(!s.pjLevel)s.stackError=1;else{s.proj=s.pjStack;s.pjLevel=0;}}
        else if(opcode==0x13){if(param!=0)s.stackError=1;else s.pjStack=s.proj;}
        else if(opcode==0x14){if(param!=0)s.stackError=1;else s.proj=s.pjStack;}
    }else if(s.mode==3){
        if(opcode==0x11){if(s.texLevel)s.stackError=1;else{s.texStack=s.tex;s.texLevel=1;}}
        else if(opcode==0x12){if(!s.texLevel)s.stackError=1;else{s.tex=s.texStack;s.texLevel=0;}}
        else if(opcode==0x13)s.texStack=s.tex;
        else if(opcode==0x14)s.tex=s.texStack;
    }else{
        if(opcode==0x11){
            if(s.pvLevel>=31) s.stackError=1;
            else{ s.posStack[s.pvLevel]=s.pos;s.vecStack[s.pvLevel]=s.vec; ++s.pvLevel; }
        }else if(opcode==0x12){
            index=s.pvLevel-signbits((uint32_t)param,6);
            /* Level 31 is the next free slot, not a valid saved entry. */
            if(index<0||index>30){s.stackError=1;return;}
            s.pvLevel=index;
            s.pos=s.posStack[index];s.vec=s.vecStack[index];
        }else if(opcode==0x13 || opcode==0x14){
            index=param & 31;
            if(index>30){s.stackError=1;return;}
            if(opcode==0x13){s.posStack[index]=s.pos;s.vecStack[index]=s.vec;}
            else{s.pos=s.posStack[index];s.vec=s.vecStack[index];}
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
static void split_product(uint8_t out[3],uint16_t a,uint16_t b)
{
    uint16_t c=rgb_product(a,b);
    out[0]=(uint8_t)(c&31);out[1]=(uint8_t)((c>>5)&31);out[2]=(uint8_t)((c>>10)&31);
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
static void shade_normal(uint32_t value)
{
    int32_t normal[3], transformed[3];
    unsigned r=s.emission&31u,g=(s.emission>>5)&31u,b=(s.emission>>10)&31u;
    int i,j;
    for(j=0;j<3;j++){
        s.normal[j]=signbits(value>>(j*10),10)*8;
        normal[j]=s.normal[j];
    }
    transform_vector(normal,&s.vec,transformed);
    if(sLightCache.dirty)update_light_cache();
    for(i=0;i<4;i++)if(s.attr&(1u<<i)){
        int64_t dot=0,halfDot=0;
        unsigned diffuse,specIntensity;
        int d,h;
        for(j=0;j<3;j++){
            int32_t half=(s.lightDir[i][j]+(j==2?-4096:0))/2;
            dot+=(int64_t)transformed[j]*s.lightDir[i][j];
            halfDot+=(int64_t)transformed[j]*half;
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
    s.rgb=(uint16_t)((r>31u?31u:r)|((g>31u?31u:g)<<5)|((b>31u?31u:b)<<10));
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
static void ensure_clip(void)
{
    unsigned j;
    if(!s.clipDirty)return;
    s.clipDirty=0;
    multiply(&s.clip,&s.pos,&s.proj);
    for(j=0;j<16;j++)put32(0x640+j*4,(uint32_t)s.clip.a[j]);
    for(j=0;j<9;j++)put32(0x680+j*4,(uint32_t)s.vec.a[(j/3)*4+(j%3)]);
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
            out.x=(int32_t)floor(((double)v->clip[0]+w)*width/(2*w)*256+x0*256+0.5);
            out.y=(int32_t)floor((w-(double)v->clip[1])*height/(2*w)*256+top*256+0.5);
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
    int32_t lastClip[4];
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
                  "last attr %08lx, last clip (%ld,%ld,%ld,%ld)",
                  sDiag.projLoads, sDiag.projMults, sDiag.projStackOps, sDiag.stackErrors,
                  (long)s.proj.a[0], (long)s.proj.a[1], (long)s.proj.a[2], (long)s.proj.a[3],
                  (long)s.proj.a[4], (long)s.proj.a[5], (long)s.proj.a[6], (long)s.proj.a[7],
                  (long)s.proj.a[8], (long)s.proj.a[9], (long)s.proj.a[10], (long)s.proj.a[11],
                  (long)s.proj.a[12], (long)s.proj.a[13], (long)s.proj.a[14], (long)s.proj.a[15],
                  (unsigned long)sDiag.lastAttr,
                  (long)sDiag.lastClip[0], (long)sDiag.lastClip[1],
                  (long)sDiag.lastClip[2], (long)sDiag.lastClip[3]);
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
/* Cull a screen-space triangle and stage what's left. */
static void add_screen_triangle(const NitroGx3dVertex *aa,const NitroGx3dVertex *bb,
                                const NitroGx3dVertex *cc)
{
    NitroGx3dTriangle *tri;
    int64_t cross=(int64_t)(bb->x-aa->x)*(cc->y-aa->y)-
                  (int64_t)(bb->y-aa->y)*(cc->x-aa->x);
    int front=sPolygonFront, mask=(s.attr>>6)&3;
    sDiag.lastAttr=s.attr;
    if(!cross){sDiag.degenerate++;return;}
    if(!mask){sDiag.cullBoth++;return;}
    if(!(mask & (front?2:1))){sDiag.cullFacing++;return;}
    if(sStagedCount>=sizeof(sStaged)/sizeof(sStaged[0]))return;
    tri=&sStaged[sStagedCount++];
    tri->v[0]=*aa;tri->v[1]=*bb;tri->v[2]=*cc;
    tri->polygon_attr=s.attr;
    tri->texture_param=s.texparam;
    tri->palette_base=s.plttbase;
}
static void stage_triangle(GeoVertex *a,GeoVertex *b,GeoVertex *c)
{
    GeoVertex v[12],tmp[12];
    unsigned i,p,n=3,clipped=0;
    /* All three outside one plane: nothing is left. None outside any: no
     * clipping needed. Most of a map is one or the other. */
    if(a->outside & b->outside & c->outside){sDiag.clipped++;return;}
    if(!(a->outside | b->outside | c->outside)){
        add_screen_triangle(screen_of(a),screen_of(b),screen_of(c));
        return;
    }
    v[0]=*a;v[1]=*b;v[2]=*c;
    for(p=0;p<6;p++){
        unsigned m=0;
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

/* The DS stores each polygon that survives culling and clipping in its
 * polygon RAM (2048 polygons) and its vertices in vertex RAM (6144); what
 * doesn't fit is dropped. Polygons that are culled or entirely outside the
 * view take no room, which matters for big maps that are mostly off-screen.
 * newVertices: the vertices this polygon adds when it isn't clipped (strips
 * share the previous polygon's). A clipped polygon stores its clipped
 * outline instead. */
static void end_polygon(unsigned newVertices)
{
    NitroGx3dScene *scene=&sScenes[sBuilding];
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

static void submit_vertex(void)
{
    GeoVertex strip, *vp;
    int32_t src[4];
    int n=s.nverts;
    if(!s.inBegin)return;
    /* Built where it's kept (no copying), except for a triangle strip's
     * third and later vertices, which shift the two before. */
    vp=(s.primitive==2 && n>=2)?&strip:&s.vertices[n];
    src[0]=s.coord[0];src[1]=s.coord[1];src[2]=s.coord[2];src[3]=4096;
    ensure_clip();
    transform_point(src,&s.clip,vp->clip);
    vp->outside=(uint8_t)outcode(vp->clip);
    vp->hasScreen=0;
    sDiag.vertices++;
    if(vp->clip[3]<=0)sDiag.behind++;
    if(((s.texparam>>30)&3u)==3u)transform_texcoords(src,3,s.resolvedTexcoord);
    vp->s=s.resolvedTexcoord[0];vp->t=s.resolvedTexcoord[1];vp->color=s.rgb;
    if(s.primitive==0){
        n++;
        if(n==3){
            begin_polygon();
            if(!far_hidden(s.vertices[0].outside|s.vertices[1].outside|s.vertices[2].outside)){
                sPolygonFront=polygon_front(&s.vertices[0],&s.vertices[1],&s.vertices[2]);
                stage_triangle(&s.vertices[0],&s.vertices[1],&s.vertices[2]);
                end_polygon(3);
            }
            n=0;
        }
    }else if(s.primitive==1){
        n++;
        if(n==4){
            begin_polygon();
            if(!far_hidden(s.vertices[0].outside|s.vertices[1].outside|
                           s.vertices[2].outside|s.vertices[3].outside)){
                sPolygonFront=polygon_front(&s.vertices[0],&s.vertices[1],&s.vertices[2]);
                stage_triangle(&s.vertices[0],&s.vertices[1],&s.vertices[2]);
                stage_triangle(&s.vertices[0],&s.vertices[2],&s.vertices[3]);
                end_polygon(4);
            }
            n=0;
        }
    }else if(s.primitive==2){
        /* Triangle strip: vertices[0] the older of the last two, [1] the
         * newer. GBATEK: v0 v1 v2, v2 v1 v3, v2 v3 v4, v4 v3 v5, ...: every
         * other triangle has its first two vertices the other way round. */
        if(n<2)n++;
        else{
            GeoVertex *first=(n&1)?&s.vertices[1]:&s.vertices[0];
            GeoVertex *second=(n&1)?&s.vertices[0]:&s.vertices[1];
            begin_polygon();
            if(!far_hidden(first->outside|second->outside|vp->outside)){
                sPolygonFront=polygon_front(first,second,vp);
                stage_triangle(first,second,vp);
                end_polygon(n==2?3:1);
            }
            s.vertices[0]=s.vertices[1];s.vertices[1]=*vp;n++;
        }
    }else{
        n++;
        if(n==4){
            begin_polygon();
            if(!far_hidden(s.vertices[0].outside|s.vertices[1].outside|
                           s.vertices[2].outside|s.vertices[3].outside)){
                sPolygonFront=polygon_front(&s.vertices[0],&s.vertices[1],&s.vertices[3]);
                stage_triangle(&s.vertices[0],&s.vertices[1],&s.vertices[3]);
                stage_triangle(&s.vertices[0],&s.vertices[3],&s.vertices[2]);
                /* The first quad of a strip brings 4 vertices, later ones 2. */
                end_polygon(s.stripStarted?2:4);
            }
            s.stripStarted=1;
            s.vertices[0]=s.vertices[2];s.vertices[1]=s.vertices[3];n=2;
        }
    }
    sDiag.lastClip[0]=vp->clip[0];sDiag.lastClip[1]=vp->clip[1];
    sDiag.lastClip[2]=vp->clip[2];sDiag.lastClip[3]=vp->clip[3];
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
static void execute(unsigned op,const uint32_t *p)
{
    Matrix m;
    int i;
    switch(op){
    case 0x10:s.mode=p[0]&3;break;
    case 0x11:case 0x12:case 0x13:case 0x14:
        if(s.mode==0)sDiag.projStackOps++;
        {int before=s.stackError;matrix_stack(op,op==0x11?0:(int32_t)p[0]);if(s.stackError&&!before)sDiag.stackErrors++;}
        break;
    case 0x15:identity(&m);set_matrix(&m,0);if(s.mode==0)sDiag.projLoads++;break;
    case 0x16:case 0x17:m=read_matrix(op,p);set_matrix(&m,0);if(s.mode==0)sDiag.projLoads++;break;
    case 0x18:case 0x19:case 0x1a:m=read_matrix(op,p);mul_matrix(&m,0);if(s.mode==0)sDiag.projMults++;break;
    case 0x1b:identity(&m);m.a[0]=(int32_t)p[0];m.a[5]=(int32_t)p[1];m.a[10]=(int32_t)p[2];
        mul_matrix(&m,1);break;
    case 0x1c:identity(&m);m.a[12]=(int32_t)p[0];m.a[13]=(int32_t)p[1];m.a[14]=(int32_t)p[2];
        mul_matrix(&m,0);break;
    case 0x20:s.rgb=p[0]&0x7FFF;break;
    case 0x21:
        shade_normal(p[0]);
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
    case 0x30:s.diffuse=p[0]&0x7FFF;s.ambient=(p[0]>>16)&0x7FFF;sLightCache.dirty=1;
        if(p[0]&(1u<<15))s.rgb=s.diffuse;break;
    case 0x31:s.specular=p[0]&0x7FFF;s.emission=(p[0]>>16)&0x7FFF;sLightCache.dirty=1;
        s.useShininess=(p[0]&(1u<<15))!=0;break;
    case 0x32:{
        int id=(p[0]>>30)&3;
        int32_t incoming[4],transformed[4];
        for(i=0;i<3;i++)incoming[i]=signbits(p[0]>>(i*10),10)*8;
        incoming[3]=0;
        /* LIGHT_VECTOR samples the directional matrix at command time. */
        transform(incoming,&s.vec,transformed);
        for(i=0;i<3;i++)s.lightDir[id][i]=transformed[i];
        break;
    }
    case 0x33:s.lightColor[(p[0]>>30)&3]=p[0]&0x7FFF;sLightCache.dirty=1;break;
    case 0x34:memcpy(s.shininess,p,sizeof(s.shininess));break;
    case 0x40:s.primitive=p[0]&3;s.nverts=0;s.inBegin=1;s.stripStarted=0;s.attr=s.pendingAttr;break;
    case 0x41:s.inBegin=0;s.nverts=0;break;
    case 0x50:{
        int next=-1;
        uint32_t serial=0;
        NitroGx3dScene *scene=&sScenes[sBuilding];
        /* All vertex and attribute writes precede the release publication. */
        scene->swap_param=p[0];
        scene->serial=sSerial+1;
        if(NitroPort_MailboxPublish(&sMailbox,&next,&serial)){
            sDisplayed=sBuilding;
            sBuilding=(unsigned)next;
            sSerial=serial;
            sScenes[sBuilding].count=0;
            sScenes[sBuilding].dropped=0;
            NitroPort_Gx3dScenePublished();
        }else{
            /* Keep the newest image and reuse this building slot on the
             * exceptionally slow consumer path; never stall the game. */
            scene->count=0;scene->dropped++;
        }
        s.frameVertices=0;s.framePolygons=0;
        if(++sDiag.scenes>=120)diag_log();
        break;
    }
    case 0x60:
        s.viewport[0]=(int)(p[0]&255);
        s.viewport[1]=(int)((p[0]>>8)&255);
        s.viewport[2]=(int)((p[0]>>16)&255);
        s.viewport[3]=(int)((p[0]>>24)&255);
        break;
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
        ensure_clip();
        {
            /* A corner's sum before the shift is the origin's plus the
             * edges' it adds (exact, in 64 bits): 8 corners for the cost
             * of 4 transforms. */
            int64_t base[4],edge[3][4];
            int j;
            for(j=0;j<4;j++){
                base[j]=(int64_t)origin[0]*s.clip.a[j]+(int64_t)origin[1]*s.clip.a[4+j]+
                        (int64_t)origin[2]*s.clip.a[8+j]+((int64_t)s.clip.a[12+j]<<12);
                for(i=0;i<3;i++)edge[i][j]=(int64_t)size[i]*s.clip.a[i*4+j];
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
        s.boxVisible=all?0:1;
        break;
    }
    case 0x71:{
        int32_t src[4],eye[4],result[4];
        src[0]=signbits(p[0],16);src[1]=signbits(p[0]>>16,16);
        src[2]=signbits(p[1],16);src[3]=4096;
        ensure_clip();
        transform(src,&s.clip,result);
        for(i=0;i<4;i++)put32(0x620+i*4,(uint32_t)result[i]);break;
    }
    case 0x72:{
        int32_t src[4],res[4];
        for(i=0;i<3;i++)src[i]=signbits(p[0]>>(i*10),10)*8;
        src[3]=0;transform(src,&s.vec,res);
        for(i=0;i<3;i++){
            /* Both the vector input/output are 4.12; no extra >>3. */
            uint16_t val=(uint16_t)res[i];
            memcpy(NitroPort_IoReg+0x630+i*2,&val,sizeof(val));
        }
        break;
    }
    default:break;
    }
    /* Matrix commands (0x11-0x1c) change the clip matrix, worked out again
     * when a vertex, a test or the SDK (NitroPort_Gx3dSyncResults) next
     * needs it: models change matrices several times between vertices.
     * Stack commands and the box test change GXSTAT. */
    if(op>=0x11u && op<=0x1cu)s.clipDirty=1;
    if((op>=0x11u && op<=0x14u)||op==0x70u)NitroPort_Gx3dRefreshStatus();
}
void NitroPort_Gx3dClearStackError(void)
{
    if(!s.ready) NitroPort_Gx3dReset();
    s.stackError=0;
    s.pjLevel=0;
    NitroPort_Gx3dRefreshStatus();
}
void NitroPort_Gx3dReset(void)
{
    memset(&s,0,sizeof(s));memset(sScenes,0,sizeof(sScenes));
    sLightCache.dirty=1;
    NitroPort_MailboxInit(&sMailbox);
    sBuilding=0;sDisplayed=1;sSerial=0;
    identity(&s.proj);identity(&s.pos);identity(&s.vec);identity(&s.tex);
    s.viewport[0]=0;s.viewport[1]=0;s.viewport[2]=255;s.viewport[3]=191;
    s.rgb=0x7fff;s.diffuse=0x7fff;s.attr=(3u<<6)|(31u<<16);
    s.pendingAttr=s.attr;
    s.boxVisible=1;s.ready=1;
    s.clipDirty=1;
    NitroPort_Gx3dRefreshStatus();
}
static void initialize(void){if(!s.ready)NitroPort_Gx3dReset();}
void NitroPort_GxDirectWrite(int opcode,uint32_t value)
{
    int needed;
    initialize();
    if(s.directNeed && s.directOpcode==opcode){
        s.params[s.used++]=value;
        if(s.used>=s.directNeed){execute((unsigned)opcode,s.params);s.directNeed=0;s.used=0;}
        return;
    }
    if(s.directNeed){s.directNeed=0;s.used=0;} /* incorrect partial stream */
    needed=param_count((unsigned)opcode);
    if(!needed){execute((unsigned)opcode,s.params);return;}
    s.directOpcode=opcode;s.directNeed=needed;s.used=0;
    s.params[s.used++]=value;
    if(s.used>=needed){execute((unsigned)opcode,s.params);s.directNeed=0;s.used=0;}
}
/* After a command: the next command of the packed word that needs
 * parameters (running those that don't), or none left. */
static void next_packed(void)
{
    while(s.packedIndex<4){
        unsigned op=s.packed[s.packedIndex];
        int need=param_count(op);
        if(need){s.need=need;s.used=0;return;}
        execute(op,s.params);
        s.packedIndex++;
    }
    s.packedIndex=0;s.packedCount=0;s.need=0;s.used=0;
}
/* Words written to GXFIFO: packed command words (up to four commands),
 * each followed by its commands' parameters. A command's parameters may
 * arrive in several calls (DMA bursts). */
void NitroPort_GxFifoWords(const uint32_t *words,size_t count)
{
    initialize();
    while(count){
        if(s.need>0){
            unsigned op=s.packed[s.packedIndex];
            size_t take=(size_t)(s.need-s.used);
            if(s.used==0 && count>=take){
                /* All of them here: straight from the stream. */
                execute(op,words);
            }else{
                if(take>count)take=count;
                memcpy(&s.params[s.used],words,take*sizeof(*words));
                s.used+=(int)take;
                words+=take;count-=take;
                if(s.used<s.need)return;
                execute(op,s.params);
                take=0;
            }
            words+=take;count-=take;
            s.packedIndex++;s.need=0;s.used=0;
            next_packed();
        }else{
            uint32_t word=*words++;
            count--;
            s.packed[0]=(uint8_t)word;s.packed[1]=(uint8_t)(word>>8);
            s.packed[2]=(uint8_t)(word>>16);s.packed[3]=(uint8_t)(word>>24);
            s.packedCount=4;s.packedIndex=0;
            next_packed();
        }
    }
}
void NitroPort_GxFifoWord(uint32_t word)
{
    /* Most words are a command's parameters: just keep them. */
    if(s.need>0 && s.used+1<s.need){
        s.params[s.used++]=word;
        return;
    }
    NitroPort_GxFifoWords(&word,1);
}
const NitroGx3dScene *NitroPort_Gx3dScene(void)
{
    initialize();
    return &sScenes[sDisplayed];
}
const NitroGx3dScene *NitroPort_Gx3dAcquireScene(uint32_t *serial)
{
    int slot;
    initialize();
    slot=NitroPort_MailboxAcquire(&sMailbox,serial);
    return slot<0?NULL:&sScenes[slot];
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
static void initialize(void);
/* GXSTAT and the matrix result registers, for the SDK to read. The port
 * can't see register reads, so GXSTAT is brought up to date after stack
 * commands and box tests and at every interrupt check, and the matrix
 * results when the SDK reads them (NitroPort_Gx3dSyncResults). */
void NitroPort_Gx3dSyncResults(void)
{
    initialize();
    ensure_clip();
}
void NitroPort_Gx3dRefreshStatus(void)
{
    uint32_t status;
    initialize();
    status=io32(0x600);
    status &= ~((3u<<0)|(31u<<8)|(1u<<13)|(1u<<14)|(1u<<15)|
                (511u<<16)|(1u<<25)|(1u<<26)|(1u<<27));
    status |= ((uint32_t)s.boxVisible<<1) |
              ((uint32_t)(s.pvLevel&31)<<8) |
              ((uint32_t)(s.pjLevel&1)<<13)|
              ((uint32_t)(s.stackError!=0)<<15)|
              (1u<<25)|(1u<<26);
    put32(0x600,status);
}
