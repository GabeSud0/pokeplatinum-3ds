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
