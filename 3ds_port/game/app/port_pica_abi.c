/*
 * libctru's structs, as libctru itself lays them out.
 *
 * The port is compiled with -fno-short-enums (the DS's code expects enums
 * the size of an int); devkitARM's libraries, libctru among them, with the
 * toolchain's default, short enums (ARM EABI: as small as their values
 * allow). A struct with an enum field is then a different size on the two
 * sides: DVLE_s is (its type, its geometry shader mode). The port's own
 * &dvlb->DVLE[1] pointed into the middle of libctru's DVLE[0..1], whose
 * "type" there wasn't a vertex shader's: shaderProgramSetVsh failed with
 * -2, and the sun shadows' two programs (DVLE 1 and 2) never set up on a
 * 3DS (DVLE 0, at the start of the array, did).
 *
 * This file is compiled without -fno-short-enums (game/Makefile), so its
 * DVLE_s is libctru's.
 */
#include <3ds.h>

/* (declared in port_pica.h, not included here: its headers are the port's) */
void *NitroPort_PicaDvle(void *dvlb, unsigned index);
unsigned NitroPort_PicaDvleSize(void);
unsigned NitroPort_PicaFrameBufSize(void);
int NitroPort_PicaFrameBufWithDepth(void *out, unsigned outSize, const void *target, void *depth);

void *NitroPort_PicaDvle(void *dvlb, unsigned index)
{
    DVLB_s *b = dvlb;
    return b && index < b->numDVLE ? &b->DVLE[index] : NULL;
}

unsigned NitroPort_PicaDvleSize(void)
{
    return (unsigned)sizeof(DVLE_s);
}

/* C3D_FrameBuf has enum fields too (its colour and depth formats): 16
 * bytes here, 24 in the port. A copy of a render target's, with another
 * depth/stencil buffer (D24S8), in libctru's layout: into out (outSize
 * bytes, NitroPort_PicaFrameBufSize or more). */
unsigned NitroPort_PicaFrameBufSize(void)
{
    return (unsigned)sizeof(C3D_FrameBuf);
}

int NitroPort_PicaFrameBufWithDepth(void *out, unsigned outSize, const void *target, void *depth)
{
    const C3D_RenderTarget *t = target;
    C3D_FrameBuf *fb = out;
    if (!out || !t || outSize < sizeof(C3D_FrameBuf)) return 0;
    *fb = t->frameBuf;
    C3D_FrameBufDepth(fb, depth, GPU_RB_DEPTH24_STENCIL8);
    return 1;
}
