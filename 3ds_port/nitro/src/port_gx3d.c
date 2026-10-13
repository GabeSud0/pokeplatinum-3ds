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
    int32_t lightHalf[4][3]; /* half vectors: (light + (0,0,-1)) / 2 */
    uint16_t lightColor[4];
    int viewport[4], primitive, nverts;
    GeoVertex vertices[4];
    int inBegin, boxVisible, stripStarted;
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
/* The last normal's colour: flat surfaces repeat the same normal, vertex
 * after vertex. Forgotten when anything it depends on changes (matrices,
 * lights, materials, the polygon's lights at BEGIN_VTXS). */
static struct { int valid; uint32_t value; uint16_t rgb; } sNormalCache;
static void shade_normal(uint32_t value)
{
    int32_t normal[3], transformed[3];
    unsigned r=s.emission&31u,g=(s.emission>>5)&31u,b=(s.emission>>10)&31u;
    int i,j;
    for(j=0;j<3;j++){
        s.normal[j]=signbits(value>>(j*10),10)*8;
        normal[j]=s.normal[j];
    }
    if(sNormalCache.valid && sNormalCache.value==value){
        s.rgb=sNormalCache.rgb;
        return;
    }
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
    s.rgb=(uint16_t)((r>31u?31u:r)|((g>31u?31u:g)<<5)|((b>31u?31u:b)<<10));
    sNormalCache.valid=1;
    sNormalCache.value=value;
    sNormalCache.rgb=s.rgb;
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
static void ensure_clip(GeoState *g)
{
    unsigned j;
    if(!g->clipDirty)return;
    g->clipDirty=0;
    multiply(&g->clip,&g->pos,&g->proj);
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
    if(all){sDiag.clipped++;return 1;}
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
    GeoVertex *vp;
    int32_t src[4];
    int n=s.nverts;
    if(!s.inBegin)return;
    /* Built where it's kept (no copying): a triangle strip's third and
     * later vertices go where the one before the last two was. */
    vp=(s.primitive==2 && n>=2)?&s.vertices[3-s.stripOld-s.stripNew]:&s.vertices[n];
    src[0]=s.coord[0];src[1]=s.coord[1];src[2]=s.coord[2];src[3]=4096;
    ensure_clip(&s);
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
            if(!polygon_hidden(s.vertices[0].outside|s.vertices[1].outside|s.vertices[2].outside,
                               s.vertices[0].outside&s.vertices[1].outside&s.vertices[2].outside)){
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
            if(!polygon_hidden(s.vertices[0].outside|s.vertices[1].outside|
                               s.vertices[2].outside|s.vertices[3].outside,
                               s.vertices[0].outside&s.vertices[1].outside&
                               s.vertices[2].outside&s.vertices[3].outside)){
                sPolygonFront=polygon_front(&s.vertices[0],&s.vertices[1],&s.vertices[2]);
                stage_triangle(&s.vertices[0],&s.vertices[1],&s.vertices[2]);
                stage_triangle(&s.vertices[0],&s.vertices[2],&s.vertices[3]);
                end_polygon(4);
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
    case 0x1b:identity(&m);m.a[0]=(int32_t)p[0];m.a[5]=(int32_t)p[1];m.a[10]=(int32_t)p[2];
        mul_matrix(g,&m,1);break;
    case 0x1c:identity(&m);m.a[12]=(int32_t)p[0];m.a[13]=(int32_t)p[1];m.a[14]=(int32_t)p[2];
        mul_matrix(g,&m,0);break;
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
                base[j]=(int64_t)origin[0]*g->clip.a[j]+(int64_t)origin[1]*g->clip.a[4+j]+
                        (int64_t)origin[2]*g->clip.a[8+j]+((int64_t)g->clip.a[12+j]<<12);
                for(i=0;i<3;i++)edge[i][j]=(int64_t)size[i]*g->clip.a[i*4+j];
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
        matrix_command(&s,op,p);
        sNormalCache.valid=0;
        if(s.stackError&&!before)sDiag.stackErrors++;
        /* Stack commands change GXSTAT (the shadow's, with a thread). */
        if(op>=0x11u && op<=0x14u && sResults==&s)refresh_status(&s);
        return;
    }
    switch(op){
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
    case 0x30:s.diffuse=p[0]&0x7FFF;s.ambient=(p[0]>>16)&0x7FFF;sLightCache.dirty=1;sNormalCache.valid=0;
        if(p[0]&(1u<<15))s.rgb=s.diffuse;break;
    case 0x31:s.specular=p[0]&0x7FFF;s.emission=(p[0]>>16)&0x7FFF;sLightCache.dirty=1;sNormalCache.valid=0;
        s.useShininess=(p[0]&(1u<<15))!=0;break;
    case 0x32:{
        int id=(p[0]>>30)&3;
        int32_t incoming[4],transformed[4];
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
    case 0x33:s.lightColor[(p[0]>>30)&3]=p[0]&0x7FFF;sLightCache.dirty=1;sNormalCache.valid=0;break;
    case 0x34:memcpy(s.shininess,p,sizeof(s.shininess));sNormalCache.valid=0;break;
    case 0x40:s.primitive=p[0]&3;s.nverts=0;s.inBegin=1;s.stripStarted=0;s.attr=s.pendingAttr;
        sNormalCache.valid=0;break;
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
    case 0x70:case 0x71:case 0x72:test_command(&s,op,p);break;
    case OP_CLEAR_STACK_ERROR:s.stackError=0;s.pjLevel=0;break;
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
static void wait_for_room(uint32_t words)
{
    /* Usually there's room as far as the game's thread already knows. */
    if(RING_WORDS-(sWriteLocal-sReadSeen)>=words)return;
    for(;;){
        uint32_t used=sWriteLocal-(sReadSeen=__atomic_load_n(&sRingRead,__ATOMIC_SEQ_CST));
        if(RING_WORDS-used>=words)return;
        publish();
        __atomic_store_n(&sProducerWaiting,1,__ATOMIC_SEQ_CST);
        used=sWriteLocal-(sReadSeen=__atomic_load_n(&sRingRead,__ATOMIC_SEQ_CST));
        if(RING_WORDS-used>=words){
            __atomic_store_n(&sProducerWaiting,0,__ATOMIC_SEQ_CST);
            return;
        }
        sOps.wait_producer(sOps.context);
    }
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
/* A ring entry; returns its length in words. */
static uint32_t work(const uint32_t *entry)
{
    uint32_t header=entry[0];
    unsigned op=header&255u;
    int skip=(header&RING_SKIP)!=0;
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
        return 2u+count;
    }
    work_op(op,entry+1,skip);
    return 1u+((header>>8)&255u);
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
            r+=work(&sRing[r&RING_MASK]);
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
        if(sOps.ticks)__atomic_store_n(&sWorkerBusy,sWorkerBusy+(sOps.ticks(sOps.context)-start),
                                       __ATOMIC_RELAXED);
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
void NitroPort_Gx3dThreadStats(uint64_t *busy,uint32_t *scenes,uint32_t *skipped)
{
    if(busy)*busy=__atomic_load_n(&sWorkerBusy,__ATOMIC_RELAXED);
    if(scenes)*scenes=__atomic_load_n(&sSwapsDone,__ATOMIC_ACQUIRE);
    if(skipped)*skipped=__atomic_load_n(&sSwapsSkipped,__ATOMIC_RELAXED);
}

/* ------------------------------------------------- the game's side */
static NitroGx3dRecorder sRecorder;
void NitroPort_Gx3dSetRecorder(NitroGx3dRecorder recorder){sRecorder=recorder;}
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
    memset(&sDec,0,sizeof(sDec));
    sLightCache.dirty=1;
    sNormalCache.valid=0;
    NitroPort_MailboxInit(&sMailbox);
    sBuilding=0;sDisplayed=1;sSerial=0;
    identity(&s.proj);identity(&s.pos);identity(&s.vec);identity(&s.tex);
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
