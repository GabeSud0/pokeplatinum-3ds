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
#include <nitro_port_gx3d.h>
#include <limits.h>
#include <string.h>
#include <math.h>

extern unsigned char NitroPort_IoReg[];
typedef struct Matrix { int32_t a[16]; } Matrix;
typedef struct GeoVertex {
    int32_t clip[4];
    int32_t s,t;
    uint16_t color;
} GeoVertex;
typedef struct GeoState {
    Matrix proj, pos, vec, tex;
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
    int inBegin, boxVisible;
    int packedIndex, packedCount, need, used, directOpcode, directNeed;
    uint8_t packed[4];
    uint32_t params[32];
    int ready;
} GeoState;

static GeoState s;
/* Two ~0.6 MiB triangle buffers, owned by this engine. A published frame
 * remains stable until the next SWAP, for the PC renderer and future GPU. */
static NitroGx3dScene sScenes[2];
static unsigned sBuilding, sDisplayed;
static uint32_t sSerial;

static int32_t saturation(int64_t n)
{
    return n > INT32_MAX ? INT32_MAX : n < INT32_MIN ? INT32_MIN : (int32_t)n;
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
static void multiply(Matrix *out, const Matrix *lhs, const Matrix *rhs)
{
    Matrix result;
    int i,j,k;
    for (i=0; i<4; ++i) for(j=0; j<4; ++j) {
        int64_t acc=0;
        for(k=0;k<4;k++) acc+=(int64_t)lhs->a[i*4+k]*rhs->a[k*4+j];
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
static void mul_matrix(const Matrix *m, int scale)
{
    Matrix newpos, newvec;
    multiply(&newpos,selected(),m);
    if(s.mode==2 && !scale) multiply(&newvec,&s.vec,m);
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
            if(index<0||index>31){s.stackError=1;return;}
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
static uint16_t rgb_add(uint16_t a,uint16_t b)
{
    unsigned r=(a&31)+(b&31),g=((a>>5)&31)+((b>>5)&31),bl=((a>>10)&31)+((b>>10)&31);
    return (uint16_t)((r>31?31:r)|((g>31?31:g)<<5)|((bl>31?31:bl)<<10));
}
static uint16_t rgb_mul(uint16_t c,int intensity)
{
    unsigned r,g,b;
    if(intensity<0)intensity=0;
    if(intensity>4096)intensity=4096;
    r=((c&31)*(unsigned)intensity+2048)>>12;
    g=(((c>>5)&31)*(unsigned)intensity+2048)>>12;
    b=(((c>>10)&31)*(unsigned)intensity+2048)>>12;
    return (uint16_t)(r|(g<<5)|(b<<10));
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

/* GBATEK: NORMAL applies the material per enabled light, then emission
 * exactly once. The half vector uses a fixed (0,0,-1) view direction.
 * Shininess entries are 128 unsigned 0.8 values, not 32 32-bit values. */
static void shade_normal(uint32_t value)
{
    int32_t normal[4], transformed[4];
    uint16_t rgb=s.emission;
    int i,j;
    for(j=0;j<3;j++){
        s.normal[j]=signbits(value>>(j*10),10)*8;
        normal[j]=s.normal[j];
    }
    normal[3]=0;
    transform(normal,&s.vec,transformed);
    for(i=0;i<4;i++)if(s.attr&(1u<<i)){
        int64_t dot=0,halfDot=0;
        int diffuse,specIntensity;
        uint16_t light=s.lightColor[i];
        for(j=0;j<3;j++){
            int32_t half=(s.lightDir[i][j]+(j==2?-4096:0))/2;
            dot+=(int64_t)transformed[j]*s.lightDir[i][j];
            halfDot+=(int64_t)transformed[j]*half;
        }
        diffuse=(int)(-dot>>12);
        if(diffuse<0)diffuse=0;
        if(diffuse>4096)diffuse=4096;
        specIntensity=(int)(-halfDot>>12);
        if(specIntensity<0)specIntensity=0;
        if(specIntensity>4096)specIntensity=4096;
        specIntensity=(int)(((int64_t)specIntensity*specIntensity)>>12);
        if(s.useShininess){
            unsigned index=(unsigned)(((uint64_t)specIntensity*128u)>>12);
            unsigned entry;
            if(index>127u)index=127u;
            entry=(s.shininess[index>>2]>>((index&3u)*8u))&255u;
            specIntensity=(int)(entry*4096u/255u);
        }
        rgb=rgb_add(rgb,rgb_product(s.ambient,light));
        rgb=rgb_add(rgb,rgb_mul(rgb_product(s.diffuse,light),diffuse));
        rgb=rgb_add(rgb,rgb_mul(rgb_product(s.specular,light),specIntensity));
    }
    s.rgb=rgb;
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
    else if(cmd==0x28){int i;for(i=0;i<3;i++)s.coord[i]+=signbits(p[0]>>(i*10),10)*8;}
}

/* Sutherland-Hodgman clipping in clip space, preserving interpolation
 * attributes. The real GX clips to six homogeneous planes ±x/y/z <= w. */
static int64_t plane_distance(const GeoVertex *v,int plane)
{
    return (int64_t)v->clip[3]+(plane&1?-(int64_t)v->clip[plane>>1]:v->clip[plane>>1]);
}
static GeoVertex interpolate(const GeoVertex *a,const GeoVertex *b,double t)
{
    GeoVertex v;
    int i;
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
    double nx=(double)v->clip[0]*inv,ny=(double)v->clip[1]*inv;
    double nz=(double)v->clip[2]*inv;
    int i;
    memset(&out,0,sizeof(out));
    for(i=0;i<4;i++)out.clip[i]=v->clip[i];
    /* DS viewport edges are inclusive: width/height contain a +1. */
    out.x=(int32_t)((s.viewport[0]+(nx+1)*(s.viewport[2]-s.viewport[0]+1)/2)*256);
    out.y=(int32_t)((191-(s.viewport[1]+(ny+1)*(s.viewport[3]-s.viewport[1]+1)/2))*256);
    if(nz<-1.0)nz=-1.0;
    if(nz>1.0)nz=1.0;
    out.depth=(uint32_t)((nz+1)*8388607.5);
    out.inv_w=(float)inv;
    out.s=v->s/16.0f;out.t=v->t/16.0f;
    out.color=v->color;
    return out;
}
static void append_triangle(const GeoVertex *a,const GeoVertex *b,const GeoVertex *c)
{
    NitroGx3dScene *scene=&sScenes[sBuilding];
    GeoVertex v[12],tmp[12];
    unsigned i,p,n=3;
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
            }
        }
        if(m<3)return;
        memcpy(v,tmp,m*sizeof(GeoVertex));n=m;
    }
    for(i=1;i+1<n;i++){
        NitroGx3dTriangle *tri;
        NitroGx3dVertex aa=to_screen(&v[0]),bb=to_screen(&v[i]),cc=to_screen(&v[i+1]);
        int64_t cross=(int64_t)(bb.x-aa.x)*(cc.y-aa.y)-
                      (int64_t)(bb.y-aa.y)*(cc.x-aa.x);
        int front=cross<0, mask=(s.attr>>6)&3;
        if(!cross||!(mask & (front?2:1)))continue;
        if(scene->count>=NITRO_GX3D_MAX_TRIANGLES){scene->dropped++;continue;}
        tri=&scene->triangles[scene->count++];
        tri->v[0]=aa;tri->v[1]=bb;tri->v[2]=cc;
        tri->polygon_attr=s.attr;
        tri->texture_param=s.texparam;
        tri->palette_base=s.plttbase;
        
    }
}
/* DS display-list RAM holds at most 2048 polygons and 6144 original
 * vertices, regardless of how many triangles clipping or quads produce. */
static int reserve_polygon(void)
{
    NitroGx3dScene *scene=&sScenes[sBuilding];
    if(s.framePolygons >= 2048u) {
        scene->dropped++;
        return 0;
    }
    s.framePolygons++;
    return 1;
}
static void submit_vertex(void)
{
    GeoVertex v;
    int32_t src[4],eye[4];
    int n=s.nverts;
    if(!s.inBegin)return;
    if(s.frameVertices >= 6144u){
        sScenes[sBuilding].dropped++;
        return;
    }
    s.frameVertices++;
    src[0]=s.coord[0];src[1]=s.coord[1];src[2]=s.coord[2];src[3]=4096;
    transform(src,&s.pos,eye);
    transform(eye,&s.proj,v.clip);
    if(((s.texparam>>30)&3u)==3u)transform_texcoords(src,3,s.resolvedTexcoord);
    v.s=s.resolvedTexcoord[0];v.t=s.resolvedTexcoord[1];v.color=s.rgb;
    if(s.primitive==0){
        s.vertices[n++]=v;
        if(n==3){if(reserve_polygon())append_triangle(&s.vertices[0],&s.vertices[1],&s.vertices[2]);n=0;}
    }else if(s.primitive==1){
        s.vertices[n++]=v;
        if(n==4){if(reserve_polygon()){
                     append_triangle(&s.vertices[0],&s.vertices[1],&s.vertices[2]);
                     append_triangle(&s.vertices[0],&s.vertices[2],&s.vertices[3]);
                 }n=0;}
    }else if(s.primitive==2){
        if(n<2)s.vertices[n++]=v;
        else{
            if(reserve_polygon())append_triangle(&s.vertices[0],&s.vertices[1],&v);
            s.vertices[0]=s.vertices[1];s.vertices[1]=v;n++;
            if(n&1){GeoVertex tmp=s.vertices[0];s.vertices[0]=s.vertices[1];s.vertices[1]=tmp;}
        }
    }else{
        s.vertices[n++]=v;
        if(n==4){
            if(reserve_polygon()){
                append_triangle(&s.vertices[0],&s.vertices[1],&s.vertices[3]);
                append_triangle(&s.vertices[0],&s.vertices[3],&s.vertices[2]);
            }
            s.vertices[0]=s.vertices[2];s.vertices[1]=s.vertices[3];n=2;
        }
    }
    s.nverts=n;
}
static void refresh_matrix_results(void);
static int param_count(unsigned op)
{
    switch(op){
    case 0x10:case 0x12:case 0x13:case 0x14:
    case 0x20:case 0x21:case 0x22:case 0x24:case 0x25:case 0x26:
    case 0x27:case 0x28:case 0x29:case 0x2a:case 0x2b:case 0x30:
    case 0x31:case 0x32:case 0x33:case 0x40:case 0x50:case 0x60:
    case 0x72:return 1;
    case 0x23:case 0x71:return 2;
    case 0x1b:case 0x1c:case 0x70:return 3;
    case 0x1a:return 9;
    case 0x17:case 0x19:return 12;
    case 0x16:case 0x18:return 16;
    case 0x34:return 32;
    default:return 0;
    }
}
static void execute(unsigned op,const uint32_t *p)
{
    Matrix m;
    int i;
    switch(op){
    case 0x10:s.mode=p[0]&3;break;
    case 0x11:case 0x12:case 0x13:case 0x14:matrix_stack(op,op==0x11?0:(int32_t)p[0]);break;
    case 0x15:identity(&m);set_matrix(&m,0);break;
    case 0x16:case 0x17:m=read_matrix(op,p);set_matrix(&m,0);break;
    case 0x18:case 0x19:case 0x1a:m=read_matrix(op,p);mul_matrix(&m,0);break;
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
    case 0x30:s.diffuse=p[0]&0x7FFF;s.ambient=(p[0]>>16)&0x7FFF;
        if(p[0]&(1u<<15))s.rgb=s.diffuse;break;
    case 0x31:s.specular=p[0]&0x7FFF;s.emission=(p[0]>>16)&0x7FFF;
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
    case 0x33:s.lightColor[(p[0]>>30)&3]=p[0]&0x7FFF;break;
    case 0x34:memcpy(s.shininess,p,sizeof(s.shininess));break;
    case 0x40:s.primitive=p[0]&3;s.nverts=0;s.inBegin=1;s.attr=s.pendingAttr;break;
    case 0x41:s.inBegin=0;s.nverts=0;break;
    case 0x50:{
        unsigned tmp=sBuilding;
        NitroGx3dScene *scene=&sScenes[tmp];
        scene->serial=++sSerial;
        scene->swap_param=p[0];
        sBuilding=sDisplayed;
        sDisplayed=tmp;
        sScenes[sBuilding].count=0;
        sScenes[sBuilding].dropped=0;
        s.frameVertices=0;
        s.framePolygons=0;
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
        int32_t origin[3], size[3], corner[4], eye[4], clip[4];
        int plane,mask;
        int64_t maxDistance;
        origin[0]=signbits(p[0],16);
        origin[1]=signbits(p[0]>>16,16);
        origin[2]=signbits(p[1],16);
        size[0]=signbits(p[1]>>16,16);
        size[1]=signbits(p[2],16);
        size[2]=signbits(p[2]>>16,16);
        s.boxVisible=1;
        for(plane=0;plane<6;plane++){
            maxDistance=INT64_MIN;
            for(mask=0;mask<8;mask++){
                int64_t d;
                for(i=0;i<3;i++)corner[i]=origin[i]+((mask>>i)&1 ? size[i]:0);
                corner[3]=4096;
                transform(corner,&s.pos,eye);
                transform(eye,&s.proj,clip);
                d=(int64_t)clip[3]+(plane&1?-(int64_t)clip[plane>>1]:clip[plane>>1]);
                if(d>maxDistance)maxDistance=d;
            }
            if(maxDistance<0){s.boxVisible=0;break;}
        }
        break;
    }
    case 0x71:{
        int32_t src[4],eye[4],result[4];
        src[0]=signbits(p[0],16);src[1]=signbits(p[0]>>16,16);
        src[2]=signbits(p[1],16);src[3]=4096;
        transform(src,&s.pos,eye);transform(eye,&s.proj,result);
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
    if(op>=0x10u && op<=0x1cu)refresh_matrix_results();
    NitroPort_Gx3dRefreshStatus();
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
    sBuilding=0;sDisplayed=1;sSerial=0;
    identity(&s.proj);identity(&s.pos);identity(&s.vec);identity(&s.tex);
    s.viewport[0]=0;s.viewport[1]=0;s.viewport[2]=255;s.viewport[3]=191;
    s.rgb=0x7fff;s.diffuse=0x7fff;s.attr=(3u<<6)|(31u<<16);
    s.pendingAttr=s.attr;
    s.boxVisible=1;s.ready=1;
    refresh_matrix_results();
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
void NitroPort_GxFifoWord(uint32_t word)
{
    initialize();
    if(!s.packedCount){
        int j;
        for(j=0;j<4;j++)s.packed[j]=(uint8_t)(word>>(8*j));
        s.packedCount=4;s.packedIndex=0;s.need=0;s.used=0;
    }else{
        if(s.need>0){
            s.params[s.used++]=word;
            if(s.used==s.need){
                execute(s.packed[s.packedIndex],s.params);
                s.packedIndex++;s.need=0;s.used=0;
            }
        }
    }
    while(s.packedCount && s.need==0){
        if(s.packedIndex>=4){
            s.packedIndex=0;s.packedCount=0;break;
        }
        s.need=param_count(s.packed[s.packedIndex]);
        if(s.need==0){execute(s.packed[s.packedIndex],s.params);s.packedIndex++;}
    }
    NitroPort_Gx3dRefreshStatus();
}
const NitroGx3dScene *NitroPort_Gx3dScene(void)
{
    initialize();
    return &sScenes[sDisplayed];
}
/* Hardware matrix readback is regenerated only when matrix state changes.
 * A geometry display list contains thousands of non-matrix words; polling
 * GXSTAT or writing a vertex must not multiply the entire clip matrix. */
static void refresh_matrix_results(void)
{
    Matrix clip;
    unsigned j;
    multiply(&clip,&s.pos,&s.proj);
    for(j=0;j<16;j++)put32(0x640+j*4,(uint32_t)clip.a[j]);
    for(j=0;j<9;j++)put32(0x680+j*4,(uint32_t)s.vec.a[(j/3)*4+(j%3)]);
}
void NitroPort_Gx3dRefreshStatus(void)
{
    uint32_t status=io32(0x600);
    status &= ~((3u<<0)|(31u<<8)|(1u<<13)|(1u<<14)|(1u<<15)|
                (511u<<16)|(1u<<25)|(1u<<26)|(1u<<27));
    status |= ((uint32_t)s.boxVisible<<1) |
              ((uint32_t)(s.pvLevel&31)<<8) |
              ((uint32_t)(s.pjLevel&1)<<13)|
              ((uint32_t)(s.stackError!=0)<<15)|
              (1u<<25)|(1u<<26);
    put32(0x600,status);
}
