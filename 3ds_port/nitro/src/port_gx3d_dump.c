/*
 * Debug-only image export shared by the 3DS worker and PC runner.
 * Uncompressed PNG RGBA8 keeps the DS RGB555 colours and BG0 coverage;
 * no game data or graphics library is needed to test or use it.
 * Rows are written in top-to-bottom, left-to-right DS screen order, so
 * PICA display-transfer orientation errors are apparent when compared.
 */
#include <nitro_port_gx3d.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W NITRO_GX3D_WIDTH
#define H NITRO_GX3D_HEIGHT
#define RAW_ROW (1u + 4u * W)
#define RAW_BYTES (RAW_ROW * H)
#define STORED_BLOCKS ((RAW_BYTES + 65534u) / 65535u)
#define Z_BYTES (2u + RAW_BYTES + STORED_BLOCKS * 5u + 4u)

static uint32_t crc32(const unsigned char *p,size_t len,uint32_t crc)
{
    while(len--){
        unsigned j;
        crc ^= *p++;
        for(j=0;j<8;j++)
            crc=(crc>>1) ^ ((crc & 1u) ? 0xedb88320u : 0u);
    }
    return crc;
}
static void be32(unsigned char *p,uint32_t v)
{
    p[0]=(unsigned char)(v>>24);
    p[1]=(unsigned char)(v>>16);
    p[2]=(unsigned char)(v>>8);
    p[3]=(unsigned char)v;
}
static int png_chunk(FILE *fp,const char *type,
                     const unsigned char *data,uint32_t size)
{
    unsigned char hdr[8],tail[4];
    uint32_t crc=0xffffffffu;
    be32(hdr,size);
    memcpy(hdr+4,type,4);
    crc=crc32(hdr+4,4,crc);
    if(size)crc=crc32(data,size,crc);
    be32(tail,~crc);
    return fwrite(hdr,1,8,fp)==8 &&
           (!size || fwrite(data,1,size,fp)==size) &&
           fwrite(tail,1,4,fp)==4;
}
static uint8_t expand5(unsigned n){return (uint8_t)((n<<3)|(n>>2));}

/* Returns 1 on success, 0 on an I/O or allocation failure.
 * The caller must already own a published image, or a synthetic test image. */
int NitroPort_Gx3dWritePng(const char *path,const uint16_t *image)
{
    static const unsigned char signature[8]=
        {137,80,78,71,13,10,26,10};
    unsigned char *rows=NULL,*deflate=NULL,ihdr[13];
    size_t y,x,pos=0,offset=0;
    uint32_t a=1u,b=0u;
    FILE *fp=NULL;
    int ok=0;
    if(!path||!image)return 0;
    rows=malloc(RAW_BYTES);
    deflate=malloc(Z_BYTES);
    if(!rows||!deflate)goto done;
    for(y=0;y<H;y++){
        unsigned char *row=rows+y*RAW_ROW;
        row[0]=0; /* PNG filter: none */
        for(x=0;x<W;x++){
            uint16_t c=image[y*W+x];
            unsigned char *d=row+1u+4u*x;
            d[0]=expand5(c&31u);
            d[1]=expand5((c>>5)&31u);
            d[2]=expand5((c>>10)&31u);
            d[3]=(c&0x8000u)?255u:0u;
        }
    }
    deflate[pos++]=0x78;
    deflate[pos++]=0x01; /* zlib, no compression */
    while(offset<RAW_BYTES){
        size_t n=RAW_BYTES-offset;
        if(n>65535u)n=65535u;
        deflate[pos++]=(unsigned char)(offset+n==RAW_BYTES);
        deflate[pos++]=(unsigned char)n;
        deflate[pos++]=(unsigned char)(n>>8);
        deflate[pos++]=(unsigned char)(~n);
        deflate[pos++]=(unsigned char)((~n)>>8);
        memcpy(deflate+pos,rows+offset,n);
        pos+=n;
        offset+=n;
    }
    for(offset=0;offset<RAW_BYTES;offset++){
        a=(a+rows[offset])%65521u;
        b=(b+a)%65521u;
    }
    be32(deflate+pos,(b<<16)|a);
    pos+=4;
    if(pos>Z_BYTES)goto done;
    fp=fopen(path,"wb");
    if(!fp)goto done;
    if(fwrite(signature,1,8,fp)!=8)goto done;
    be32(ihdr,W);
    be32(ihdr+4,H);
    ihdr[8]=8; /* bits per component */
    ihdr[9]=6; /* RGBA */
    ihdr[10]=ihdr[11]=ihdr[12]=0;
    if(!png_chunk(fp,"IHDR",ihdr,13) ||
       !png_chunk(fp,"IDAT",deflate,(uint32_t)pos) ||
       !png_chunk(fp,"IEND",NULL,0))goto done;
    ok=1;
done:
    if(fp && fclose(fp)!=0)ok=0;
    if(!ok && fp)remove(path);
    free(rows);
    free(deflate);
    return ok;
}
int NitroPort_Gx3dDumpLatest(const char *path)
{
    const uint16_t *image=NitroPort_Gx3dAcquireImage();
    int ok;
    if(!image)return 0;
    ok=NitroPort_Gx3dWritePng(path,image);
    NitroPort_Gx3dReleaseImage(image);
    return ok;
}
