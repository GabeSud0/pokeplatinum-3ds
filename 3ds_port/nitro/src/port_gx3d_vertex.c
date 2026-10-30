#include <nitro_port_gx3d_vertex.h>
#include <string.h>
#include <math.h>
static uint8_t color8(unsigned v) { return (uint8_t)((v<<3)|(v>>2)); }
/* PICA depth is mapped to 0 (near) ... 1 (far) by citro3d's
 * viewport mapping. With GPU_LESS and a maximum-depth clear, near wins. */
float NitroPort_Gx3dDepthValue(const NitroGx3dVertex *vertex,
                              uint32_t swap_param)
{
    float d;
    if (!vertex) return 1.0f;
    /* W-buffer (SWAP_BUFFERS bit 1): the DS stores W itself, 20.12 fixed
     * point in its 24-bit depth buffer (as melonDS does), so depth runs
     * to W = 4096.0. The title screen's Giratina is 300 units away. */
    d = (swap_param & 2u)
        ? ((float)vertex->clip[3] / 16777215.0f)
        : ((float)vertex->depth / 16777215.0f);
    return d < 0.0f ? 0.0f : d > 1.0f ? 1.0f : d;
}
/* The GPU's clip-space z for a vertex. Z-buffer scenes: depth * W, so the
 * PICA's z / W gives the DS depth (C3D_DepthMap(true, -1, 0)). W-buffer
 * scenes (SWAP_BUFFERS bit 1): the PICA is put in its own W-buffer mode
 * (C3D_DepthMap(false, -1, 0)), where the stored depth is z / W
 * interpolated across the screen times the perspective-correct W. Giving
 * z the depth itself makes that the perspective-correct DS W, as the DS
 * stores it. Interpolating W linearly instead (the Z-buffer mode with
 * z = depth * W) puts the middle of a big polygon too far away, and the
 * intro's tunnel and Giratina came out with holes and slivers. */
float NitroPort_Gx3dClipZ(const NitroGx3dVertex *vertex,uint32_t swap_param)
{
    float d=NitroPort_Gx3dDepthValue(vertex,swap_param);
    if(swap_param&2u)return -d;
    return -d*((float)vertex->clip[3]/4096.0f);
}
size_t NitroPort_Gx3dCountDroppedTriangles(const NitroGx3dScene *scene,
                                            size_t capacity)
{
    size_t i, eligible = 0, fits;
    if (!scene) return 0;
    if (capacity > NITRO_GX3D_PICA_MAX_VERTICES)
        capacity = NITRO_GX3D_PICA_MAX_VERTICES;
    for (i = 0; i < scene->count; i++) {
        if (((scene->triangles[i].polygon_attr >> 16) & 31u) == 31u)
            ++eligible;
    }
    fits = capacity / 3u;
    return eligible > fits ? eligible - fits : 0u;
}
size_t NitroPort_Gx3dBuildOpaqueVertices(const NitroGx3dScene *scene,
                                         NitroGx3dGpuVertex *out,
                                         size_t capacity)
{
    size_t i, count=0, k;
    if(!scene||!out)return 0;
    if(capacity>NITRO_GX3D_PICA_MAX_VERTICES)
        capacity=NITRO_GX3D_PICA_MAX_VERTICES;
    for(i=0;i<scene->count && count+3<=capacity;i++){
        const NitroGx3dTriangle *tri=&scene->triangles[i];
        unsigned alpha=(tri->polygon_attr>>16)&31u;
        float tw=(float)(8u<<((tri->texture_param>>20)&7u));
        float th=(float)(8u<<((tri->texture_param>>23)&7u));
        /* PR1 renders every opaque polygon as untextured vertex colour,
         * INCLUDING polygons whose original material is textured. Dropping
         * them would hide almost all overworld model geometry. Texture
         * mapping and per-texel transparency follow in PR2/PR3. */
        if(alpha!=31u)continue;
        for(k=0;k<3;k++){
            const NitroGx3dVertex *v=&tri->v[k];
            NitroGx3dGpuVertex *dst=&out[count++];
            float w=v->clip[3]/4096.0f;
            float x=v->x/256.0f;
            float y=v->y/256.0f;
            /* Map final DS pixels into NDC without applying the DS viewport
             * twice. PICA uses 0 for near and -1 for far (reverse depth). */
            float ndc_x=x/128.0f-1.0f;
            float ndc_y=1.0f-y/96.0f;
            /* W scenes still approximate DS W depth. PICA's stored depth
             * increases with distance; GPU_LESS and a maximum clear ensure
             * a nearer triangle occludes one farther away. */
            dst->position[0]=ndc_x*w;
            dst->position[1]=ndc_y*w;
            dst->position[2]=NitroPort_Gx3dClipZ(v,scene->swap_param);
            dst->position[3]=w;
            dst->color[0]=color8(v->color&31u);
            dst->color[1]=color8((v->color>>5)&31u);
            dst->color[2]=color8((v->color>>10)&31u);
            dst->color[3]=255;
            dst->uv[0]=v->s/tw;
            dst->uv[1]=v->t/th;
        }
    }
    return count;
}

/* A translucent polygon, as the DS sorts them: alpha 1-30, or a texture
 * with alpha (A3I5, A5I3). Other textures' transparent texels (colour 0,
 * direct colour, compressed) are simply not drawn: those polygons draw
 * with the opaque ones. */
int NitroPort_Gx3dTriangleTranslucent(const NitroGx3dTriangle *tri)
{
    unsigned fmt=(tri->texture_param>>26)&7u;
    unsigned alpha=(tri->polygon_attr>>16)&31u;
    return (alpha>0u && alpha<31u) || fmt==1u || fmt==6u;
}

/* The order the DS draws a scene's triangles in: opaque polygons first,
 * then translucent ones, as submitted, except that with SWAP_BUFFERS
 * bit 0 clear (automatic sorting) translucent polygons go by their bottom
 * screen line, then their top line (a stable sort: ties keep their order).
 * Opaque polygons keep their order here: the depth test decides between
 * them. (melonDS sorts them by line too; only ties of equal depth could
 * tell the difference.) */
size_t NitroPort_Gx3dDrawOrder(const NitroGx3dScene *scene,uint16_t *order,uint16_t *scratch)
{
    size_t i,n=0,first,count,width;
    if(!scene)return 0;
    for(i=0;i<scene->count;i++)
        if(!NitroPort_Gx3dTriangleTranslucent(&scene->triangles[i]))order[n++]=(uint16_t)i;
    first=n;
    for(i=0;i<scene->count;i++)
        if(NitroPort_Gx3dTriangleTranslucent(&scene->triangles[i]))order[n++]=(uint16_t)i;
    count=n-first;
    if(scene->swap_param&1u || count<2)return n;
    /* Bottom-up merge sort of order[first..n): stable. */
    for(width=1;width<count;width*=2){
        uint16_t *src=order+first;
        size_t lo;
        for(lo=0;lo<count;lo+=2*width){
            size_t mid=lo+width<count?lo+width:count;
            size_t hi=lo+2*width<count?lo+2*width:count;
            size_t a=lo,b=mid,k=lo;
            while(a<mid && b<hi){
                if(scene->triangles[src[b]].sort_key<scene->triangles[src[a]].sort_key)
                    scratch[k++]=src[b++];
                else scratch[k++]=src[a++];
            }
            while(a<mid)scratch[k++]=src[a++];
            while(b<hi)scratch[k++]=src[b++];
        }
        memcpy(src,scratch,count*sizeof(*src));
    }
    return n;
}

/* Each edge of a screen-space triangle moved out by a tenth of a pixel
 * (corners by at most three times that). The DS's rasterizer covers the
 * pixels on a shared edge from both sides, so where two models' vertices
 * don't quite meet nothing shows through; the 3DS's GPU, which covers a
 * pixel only when its centre is inside, showed dots of background there. */
#define EXPAND_PX 0.1f
void NitroPort_Gx3dExpandTriangle(const float sx[3],const float sy[3],float dx[3],float dy[3])
{
    float nx[3],ny[3],area;
    int i;
    for(i=0;i<3;i++)dx[i]=dy[i]=0.0f;
    area=(sx[1]-sx[0])*(sy[2]-sy[0])-(sy[1]-sy[0])*(sx[2]-sx[0]);
    if(area>-1e-6f && area<1e-6f)return;
    for(i=0;i<3;i++){
        int j=(i+1)%3;
        float ex=sx[j]-sx[i],ey=sy[j]-sy[i],len=sqrtf(ex*ex+ey*ey);
        if(len<1e-6f)return;
        /* Outward normal: right of the edge for one winding, left for the other. */
        nx[i]=(area>0? ey:-ey)/len;
        ny[i]=(area>0?-ex: ex)/len;
    }
    for(i=0;i<3;i++){
        int p=(i+2)%3;   /* the edge ending at vertex i */
        float denom=1.0f+nx[p]*nx[i]+ny[p]*ny[i];
        float mx,my,m2,limit=3.0f*EXPAND_PX;
        if(denom<1e-3f)denom=1e-3f;
        mx=EXPAND_PX*(nx[p]+nx[i])/denom;
        my=EXPAND_PX*(ny[p]+ny[i])/denom;
        m2=mx*mx+my*my;
        if(m2>limit*limit){float k=limit/sqrtf(m2);mx*=k;my*=k;}
        dx[i]=mx;dy[i]=my;
    }
}

/* PIXEL PERFECT's sprites (the people, the Pokémon, the item balls: the
 * game's billboards, each a textured quad facing the camera, all three
 * corners at one depth) a texel a pixel, their texels on the screen's
 * pixels. Where the field is at the DS's pixel size (the scene 240 lines
 * tall: a DS pixel a screen pixel) the camera's perspective still makes
 * them a little bigger or smaller than their texels (a person near the
 * screen's bottom 32 texels over 35.7 pixels), and wherever they stand a
 * fraction of a pixel off: the DS's nearest-texel drawing doubles or drops
 * a texel here and there, and the smooth 3D (drawn 2x2 and averaged)
 * blends the texels' edges into the pixels beside them, blurring the
 * pixel art. Laid out at a texel a pixel instead, the texture's bottom
 * middle (the feet) on the nearest pixel corner to where it was, each
 * texel covers whole pixels and stays sharp, smooth 3D or not. Only quads
 * near their texels' size (a sprite scaled on purpose, a Pokémon growing
 * out of its ball, is left as it is), not mirrored differently each way.
 * sx/sy: the triangle's corners in the DS's units, changed if so.
 *
 * Moving on the screen (the camera going along, a person walking), a
 * sprite keeps the fraction of a pixel it was moved by when it stood
 * (snap_offset): each put on its own nearest pixel, a scene at a time,
 * they stepped a whole pixel or none while the ground under them slid
 * smoothly, and wobbled on it (2026-10-29). Standing still again, on its
 * nearest pixel. */
typedef struct SnapMark {
    uint32_t key;      /* the texture's format, size and palette */
    float x,y;         /* its feet, where the scene has them (screen pixels) */
    float rx,ry;       /* moved by */
} SnapMark;
#define SNAP_MARKS 256u
#define SNAP_NEAR 12.0f    /* (pixels: the same sprite, a scene later) */
#define SNAP_SCENES 4u     /* (scenes on: one or two the display didn't draw) */
static SnapMark sSnapBefore[SNAP_MARKS],sSnapNow[SNAP_MARKS];
static unsigned sSnapBeforeCount,sSnapNowCount;
static const NitroGx3dScene *sSnapScene;
static uint32_t sSnapSerial;
static int sSnapValid,sSnapCarry,sSnapKeep;

/* Before a scene's batches: whether its sprites can be told from the scene
 * before's (a scene on, or the in-between scene before it), and kept
 * for the next (not the same scene made again). */
static void snap_begin(const NitroGx3dScene *scene)
{
    int same=sSnapValid && scene==sSnapScene && scene->serial==sSnapSerial;
    sSnapKeep=!same;
    sSnapCarry=!same && sSnapValid && (uint32_t)(scene->serial-sSnapSerial)<=SNAP_SCENES;
    sSnapNowCount=0;
}
static void snap_end(const NitroGx3dScene *scene)
{
    if(!sSnapKeep)return;
    memcpy(sSnapBefore,sSnapNow,sSnapNowCount*sizeof(SnapMark));
    sSnapBeforeCount=sSnapNowCount;
    sSnapScene=scene;sSnapSerial=scene->serial;sSnapValid=1;
}
/* What a sprite whose feet are at (x, y) is moved by: as a triangle of
 * its quad was; as it was the scene before, if it moved since (still: the
 * pixel corner nearest to where that put it); else to its nearest pixel
 * corner. */
static void snap_offset(uint32_t key,float x,float y,float *rx,float *ry)
{
    unsigned i;
    const SnapMark *best=NULL;
    float bestD=SNAP_NEAR*SNAP_NEAR;
    for(i=0;i<sSnapNowCount;i++){
        const SnapMark *m=&sSnapNow[i];
        if(m->key==key && fabsf(m->x-x)<1e-3f && fabsf(m->y-y)<1e-3f){*rx=m->rx;*ry=m->ry;return;}
    }
    if(!sSnapKeep){
        /* (the same scene made again: as it was made) */
        for(i=0;i<sSnapBeforeCount;i++){
            const SnapMark *m=&sSnapBefore[i];
            if(m->key==key && fabsf(m->x-x)<1e-3f && fabsf(m->y-y)<1e-3f){*rx=m->rx;*ry=m->ry;return;}
        }
    }
    if(sSnapCarry){
        for(i=0;i<sSnapBeforeCount;i++){
            const SnapMark *m=&sSnapBefore[i];
            float d=(m->x-x)*(m->x-x)+(m->y-y)*(m->y-y);
            if(m->key==key && d<bestD){bestD=d;best=m;}
        }
    }
    if(best && (fabsf(best->x-x)>0.02f || fabsf(best->y-y)>0.02f)){
        *rx=best->rx;*ry=best->ry;
    }else if(best){
        *rx=floorf(x+best->rx+0.5f)-x;*ry=floorf(y+best->ry+0.5f)-y;
    }else{
        *rx=floorf(x+0.5f)-x;*ry=floorf(y+0.5f)-y;
    }
    if(sSnapNowCount<SNAP_MARKS){
        SnapMark *m=&sSnapNow[sSnapNowCount++];
        m->key=key;m->x=x;m->y=y;m->rx=*rx;m->ry=*ry;
    }
}

static int sprite_texel_a_pixel(const NitroGx3dScene *scene,const NitroGx3dTriangle *tri,
                                float tw,float th,float *sx,float *sy)
{
    const NitroGx3dVertex *v=tri->v;
    float fx,fy,px[3],py[3],ds1,dt1,ds2,dt2,det,a,b,e,f,x0,y0,ax,ay,ar,anchor_t;
    float sa,sf,rx,ry;
    int k;
    if(!scene || scene->screen_height!=240u || ((tri->texture_param>>26)&7u)==0u)return 0;
    if(v[0].clip[3]!=v[1].clip[3] || v[0].clip[3]!=v[2].clip[3] || v[0].clip[3]<=0)return 0;
    /* (screen pixels: the view's width over the DS's 256 units, its 240 lines over 192) */
    fx=(float)scene->screen_width/256.0f;
    fy=240.0f/192.0f;
    for(k=0;k<3;k++){px[k]=v[k].x/256.0f*fx;py[k]=v[k].y/256.0f*fy;}
    ds1=v[1].s-v[0].s;dt1=v[1].t-v[0].t;ds2=v[2].s-v[0].s;dt2=v[2].t-v[0].t;
    det=ds1*dt2-ds2*dt1;
    if(det>-1e-3f && det<1e-3f)return 0;
    /* screen = (a s + b t, e s + f t) + origin */
    a=((px[1]-px[0])*dt2-(px[2]-px[0])*dt1)/det;
    b=((px[2]-px[0])*ds1-(px[1]-px[0])*ds2)/det;
    e=((py[1]-py[0])*dt2-(py[2]-py[0])*dt1)/det;
    f=((py[2]-py[0])*ds1-(py[1]-py[0])*ds2)/det;
    ax=a<0?-a:a; ay=f<0?-f:f;
    if(ax<0.85f || ax>1.25f || ay<0.85f || ay>1.25f)return 0;      /* (scaled on purpose) */
    if((b<0?-b:b)>0.02f*ax || (e<0?-e:e)>0.02f*ay)return 0;           /* (not upright) */
    ar=ax-ay; if(ar<0)ar=-ar; if(ar>0.1f)return 0;
    sa=a<0?-1.0f:1.0f; sf=f<0?-1.0f:1.0f;
    x0=px[0]-a*v[0].s-b*v[0].t;
    y0=py[0]-e*v[0].s-f*v[0].t;
    /* the texture's bottom middle: its feet */
    anchor_t=f>0?th:0.0f;
    ax=x0+a*(tw*0.5f);
    ay=y0+f*anchor_t;
    snap_offset((tri->texture_param>>16)^(tri->palette_base*2654435761u),ax,ay,&rx,&ry);
    ax+=rx;ay+=ry;
    for(k=0;k<3;k++){
        sx[k]=(ax+sa*(v[k].s-tw*0.5f))/fx;
        sy[k]=(ay+sf*(v[k].t-anchor_t))/fy;
    }
    return 1;
}

static void put_vertex(NitroGx3dGpuVertex *dst,const NitroGx3dVertex *v,
                       uint32_t swap_param,unsigned alpha,float tw,float th,
                       float sx,float sy)
{
    float w=v->clip[3]/4096.0f;
    float ndc_x=sx/128.0f-1.0f;
    float ndc_y=1.0f-sy/96.0f;
    dst->position[0]=ndc_x*w;
    dst->position[1]=ndc_y*w;
    dst->position[2]=NitroPort_Gx3dClipZ(v,swap_param);
    dst->position[3]=w;
    dst->color[0]=color8(v->color&31u);
    dst->color[1]=color8((v->color>>5)&31u);
    dst->color[2]=color8((v->color>>10)&31u);
    dst->color[3]=color8(alpha);
    /* Texels: (s / width, t / height) samples DS texel (s, t), see
     * NitroPort_Gx3dTileRGBA8. */
    dst->uv[0]=v->s/tw;
    dst->uv[1]=v->t/th;
}

/* What decides the GPU state of a batch. */
static void batch_state(const NitroGx3dTriangle *tri,uint32_t *param,
                        uint32_t *palette,uint32_t *attr)
{
    unsigned fmt=(tri->texture_param>>26)&7u;
    *param=fmt?tri->texture_param:0u;
    *palette=(fmt && fmt!=7u)?tri->palette_base:0u;
    /* The mode, depth bits, alpha, and bit 15: fog on the polygon. */
    *attr=tri->polygon_attr&((3u<<4)|(1u<<11)|(1u<<14)|(1u<<15)|(31u<<16));
}

size_t NitroPort_Gx3dBuildBatches(const NitroGx3dScene *scene,
                                  NitroGx3dGpuVertex *out, size_t capacity,
                                  NitroGx3dBatch *batches, size_t max_batches,
                                  size_t *batch_count, unsigned *dropped)
{
    size_t i,count=0,nb=0;
    unsigned lost=0;
    if(batch_count)*batch_count=0;
    if(dropped)*dropped=0;
    if(!scene||!out||!batches)return 0;
    if(capacity>NITRO_GX3D_PICA_MAX_VERTICES)
        capacity=NITRO_GX3D_PICA_MAX_VERTICES;
    static uint16_t order[NITRO_GX3D_MAX_TRIANGLES],scratch[NITRO_GX3D_MAX_TRIANGLES];
    size_t n=NitroPort_Gx3dDrawOrder(scene,order,scratch),j;
    snap_begin(scene);
    for(j=0;j<n;j++){
        const NitroGx3dTriangle *tri=&scene->triangles[order[j]];
        unsigned alpha=(tri->polygon_attr>>16)&31u;
        unsigned pass=(unsigned)NitroPort_Gx3dTriangleTranslucent(tri);
        uint32_t param,palette,attr;
        float tw,th;
        NitroGx3dBatch *batch;
        unsigned k;
        if(!alpha)continue;
        batch_state(tri,&param,&palette,&attr);
        /* The polygon ID is part of the state: translucent polygons don't
         * draw over translucent pixels of their own polygon ID, and edge
         * marking outlines opaque ones by ID. */
        attr|=tri->polygon_attr&(63u<<24);
        batch=nb?&batches[nb-1]:NULL;
        if(!batch || batch->texture_param!=param || batch->palette_base!=palette ||
           batch->polygon_attr!=attr || batch->translucent!=pass){
            if(nb>=max_batches){lost++;continue;}
            batch=&batches[nb++];
            batch->first=(uint32_t)count;
            batch->count=0;
            batch->texture_param=param;
            batch->palette_base=palette;
            batch->polygon_attr=attr;
            batch->translucent=(uint8_t)pass;
        }
        if(count+3>capacity){lost++;continue;}
        tw=(float)(8u<<((tri->texture_param>>20)&7u));
        th=(float)(8u<<((tri->texture_param>>23)&7u));
        {
            float sx[3],sy[3],dx[3],dy[3];
            if(sprite_texel_a_pixel(scene,tri,tw,th,sx,sy)){
                /* (its edges on pixel corners: not moved out) */
                for(k=0;k<3;k++)
                    put_vertex(&out[count++],&tri->v[k],scene->swap_param,alpha,tw,th,sx[k],sy[k]);
            }else{
                for(k=0;k<3;k++){sx[k]=tri->v[k].x/256.0f;sy[k]=tri->v[k].y/256.0f;}
                NitroPort_Gx3dExpandTriangle(sx,sy,dx,dy);
                for(k=0;k<3;k++)
                    put_vertex(&out[count++],&tri->v[k],scene->swap_param,alpha,tw,th,sx[k]+dx[k],sy[k]+dy[k]);
            }
        }
        batch->count+=3;
    }
    snap_end(scene);
    /* A batch opened for a triangle that then didn't fit stays empty. */
    while(nb && batches[nb-1].count==0)nb--;
    if(batch_count)*batch_count=nb;
    if(dropped)*dropped=lost;
    return count;
}

size_t NitroPort_Gx3dBuildShadowBatches(const NitroGx3dScene *scene, size_t first,
                                        NitroGx3dGpuVertex *out, size_t capacity,
                                        NitroGx3dBatch *batches, size_t max_batches,
                                        size_t *batch_count)
{
    size_t i,count=0,nb=0;
    if(batch_count)*batch_count=0;
    if(!scene||!out||!batches||!scene->shadow_triangles)return 0;
    for(i=0;i<scene->shadow_count && i<NITRO_GX3D_MAX_SHADOW_TRIANGLES;i++){
        const NitroGx3dTriangle *tri=&scene->shadow_triangles[i];
        uint32_t param,palette,attr;
        NitroGx3dBatch *batch;
        float tw,th;
        unsigned k;
        if(count+3>capacity)break;
        batch_state(tri,&param,&palette,&attr);
        batch=nb?&batches[nb-1]:NULL;
        if(!batch || batch->texture_param!=param || batch->palette_base!=palette ||
           batch->polygon_attr!=attr){
            if(nb>=max_batches)break;
            batch=&batches[nb++];
            batch->first=(uint32_t)(first+count);
            batch->count=0;
            batch->texture_param=param;
            batch->palette_base=palette;
            batch->polygon_attr=attr;
            batch->translucent=0;
        }
        tw=(float)(8u<<((tri->texture_param>>20)&7u));
        th=(float)(8u<<((tri->texture_param>>23)&7u));
        for(k=0;k<3;k++)put_vertex(&out[count++],&tri->v[k],scene->swap_param,31u,tw,th,tri->v[k].x/256.0f,tri->v[k].y/256.0f);
        batch->count+=3;
    }
    if(batch_count)*batch_count=nb;
    return count;
}
