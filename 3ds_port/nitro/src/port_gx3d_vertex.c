#include <nitro_port_gx3d_vertex.h>
static uint8_t color8(unsigned v) { return (uint8_t)((v<<3)|(v>>2)); }
/* PICA depth is mapped to 0 (near) ... 1 (far) by citro3d's
 * viewport mapping. With GPU_LESS and a maximum-depth clear, near wins. */
float NitroPort_Gx3dDepthValue(const NitroGx3dVertex *vertex,
                              uint32_t swap_param)
{
    float d;
    if (!vertex) return 1.0f;
    d = (swap_param & 2u)
        ? ((float)vertex->clip[3] / (4096.0f * 16.0f))
        : ((float)vertex->depth / 16777215.0f);
    return d < 0.0f ? 0.0f : d > 1.0f ? 1.0f : d;
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
            float z=-NitroPort_Gx3dDepthValue(v,scene->swap_param);
            dst->position[0]=ndc_x*w;
            dst->position[1]=ndc_y*w;
            dst->position[2]=z*w;
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

int NitroPort_Gx3dTriangleTranslucent(const NitroGx3dTriangle *tri)
{
    unsigned fmt=(tri->texture_param>>26)&7u;
    unsigned alpha=(tri->polygon_attr>>16)&31u;
    /* The same rule as the PC reference (port_gx3d_raster.c). */
    return alpha<31u || fmt==1u || fmt==6u ||
           (fmt>=2u && fmt<=4u && (tri->texture_param&(1u<<29))) ||
           fmt==5u || fmt==7u;
}

static void put_vertex(NitroGx3dGpuVertex *dst,const NitroGx3dVertex *v,
                       uint32_t swap_param,unsigned alpha,float tw,float th)
{
    float w=v->clip[3]/4096.0f;
    float ndc_x=(v->x/256.0f)/128.0f-1.0f;
    float ndc_y=1.0f-(v->y/256.0f)/96.0f;
    float z=-NitroPort_Gx3dDepthValue(v,swap_param);
    dst->position[0]=ndc_x*w;
    dst->position[1]=ndc_y*w;
    dst->position[2]=z*w;
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
    *attr=tri->polygon_attr&((3u<<4)|(1u<<11)|(1u<<14)|(31u<<16));
}

size_t NitroPort_Gx3dBuildBatches(const NitroGx3dScene *scene,
                                  NitroGx3dGpuVertex *out, size_t capacity,
                                  NitroGx3dBatch *batches, size_t max_batches,
                                  size_t *batch_count, unsigned *dropped)
{
    size_t i,count=0,nb=0;
    unsigned pass,lost=0;
    if(batch_count)*batch_count=0;
    if(dropped)*dropped=0;
    if(!scene||!out||!batches)return 0;
    if(capacity>NITRO_GX3D_PICA_MAX_VERTICES)
        capacity=NITRO_GX3D_PICA_MAX_VERTICES;
    for(pass=0;pass<2;pass++)for(i=0;i<scene->count;i++){
        const NitroGx3dTriangle *tri=&scene->triangles[i];
        unsigned alpha=(tri->polygon_attr>>16)&31u;
        uint32_t param,palette,attr;
        float tw,th;
        NitroGx3dBatch *batch;
        unsigned k;
        if(!alpha)continue;
        if((unsigned)NitroPort_Gx3dTriangleTranslucent(tri)!=pass)continue;
        batch_state(tri,&param,&palette,&attr);
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
        for(k=0;k<3;k++)
            put_vertex(&out[count++],&tri->v[k],scene->swap_param,alpha,tw,th);
        batch->count+=3;
    }
    /* A batch opened for a triangle that then didn't fit stays empty. */
    while(nb && batches[nb-1].count==0)nb--;
    if(batch_count)*batch_count=nb;
    if(dropped)*dropped=lost;
    return count;
}
