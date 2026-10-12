#include <nitro_port_gx3d_vertex.h>
static uint8_t color8(unsigned v) { return (uint8_t)((v<<3)|(v>>2)); }
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
        unsigned format=(tri->texture_param>>26)&7u;
        float tw=(float)(8u<<((tri->texture_param>>20)&7u));
        float th=(float)(8u<<((tri->texture_param>>23)&7u));
        if(alpha!=31u || format!=0u)continue;
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
            float z=-(float)v->depth/16777215.0f;
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
