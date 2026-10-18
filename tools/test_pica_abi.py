"""Exercise the real ABI bridge across enum modes, using installed 3DS headers.

Runs in the devkitPro CI container with its native PC gcc. No GPU, SDK or
game data is needed. The lock shim only supplies newlib's unrelated lock
typedefs, which glibc does not have; framebuffer/shader types are the real
libctru and citro3d declarations, not copies in this test.
"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
CTRU = Path(os.environ.get("PICA_TEST_CTRU_INCLUDE",
                          str(Path(os.environ.get("DEVKITPRO", "/opt/devkitpro")) / "libctru/include")))
C3D = Path(os.environ.get("PICA_TEST_C3D_INCLUDE", str(CTRU)))


@unittest.skipUnless(shutil.which("gcc") and (CTRU / "3ds.h").is_file()
                     and (C3D / "citro3d.h").is_file(),
                     "requires gcc and devkitPro headers (or PICA_TEST_*_INCLUDE)")
class PicaAbiTest(unittest.TestCase):
    def test_framebuffer_and_dvle_bridge_between_enum_modes(self):
        with tempfile.TemporaryDirectory() as temp:
            tmp = Path(temp)
            (tmp / "sys").mkdir()
            (tmp / "sys/lock.h").write_text(
                "typedef int _LOCK_T;\n"
                "typedef struct { int lock, owner, count; } _LOCK_RECURSIVE_T;\n")
            (tmp / "library_side.c").write_text(r'''
#include <3ds.h>
#include <citro3d.h>
#include <assert.h>
static C3D_RenderTarget target;
static DVLE_s dvles[2];
static DVLB_s dvlb;
static unsigned color, depth, replacement;
void *test_target(void) {
    C3D_FrameBufAttrib(&target.frameBuf, 240, 400, true);
    C3D_FrameBufColor(&target.frameBuf, &color, GPU_RB_RGBA8);
    C3D_FrameBufDepth(&target.frameBuf, &depth, GPU_RB_DEPTH24_STENCIL8);
    return &target;
}
void *test_depth(void) { return &replacement; }
unsigned test_fb_size(void) { return sizeof(C3D_FrameBuf); }
void test_verify(const void *out, int replaced) {
    const C3D_FrameBuf *fb = out;
    assert(fb->colorBuf == &color);
    assert(fb->depthBuf == (replaced ? &replacement : &depth));
    assert(fb->width == 240 && fb->height == 400 && fb->block32);
    assert(fb->colorFmt == GPU_RB_RGBA8);
    assert(fb->depthFmt == GPU_RB_DEPTH24_STENCIL8);
    assert(fb->colorMask == 15 && fb->depthMask == 3);
}
void *test_dvlb(void) { dvlb.DVLE = dvles; dvlb.numDVLE = 2; return &dvlb; }
void *test_dvle(unsigned i) { return &dvles[i]; }
''')
            (tmp / "port_side.c").write_text(r'''
#include <3ds.h>
#include <citro3d.h>
#include <assert.h>
#include <string.h>
unsigned NitroPort_PicaFrameBufSize(void);
int NitroPort_PicaFrameBufWithDepth(void *, unsigned, const void *, void *);
void *NitroPort_PicaDvle(void *, unsigned);
void *test_target(void);
void *test_depth(void);
unsigned test_fb_size(void);
void test_verify(const void *, int);
void *test_dvlb(void);
void *test_dvle(unsigned);
int main(void) {
    union { unsigned long align; unsigned char bytes[32]; } out;
    unsigned i, n = NitroPort_PicaFrameBufSize();
    void *target = test_target(), *dvlb = test_dvlb();
    assert(n == test_fb_size() && n < sizeof(C3D_FrameBuf));
    memset(out.bytes, 0xA5, sizeof(out.bytes));
    assert(!NitroPort_PicaFrameBufWithDepth(out.bytes, n-1, target, test_depth()));
    for(i=0;i<sizeof(out.bytes);i++) assert(out.bytes[i] == 0xA5);
    assert(!NitroPort_PicaFrameBufWithDepth(0, sizeof(out.bytes), target, test_depth()));
    assert(!NitroPort_PicaFrameBufWithDepth(out.bytes, sizeof(out.bytes), 0, test_depth()));
    assert(NitroPort_PicaFrameBufWithDepth(out.bytes, sizeof(out.bytes), target, test_depth()));
    test_verify(out.bytes, 1);
    for(i=n;i<sizeof(out.bytes);i++) assert(out.bytes[i] == 0xA5);
    /* The bridge must not modify the render target it copied. */
    test_verify(&((C3D_RenderTarget *)target)->frameBuf, 0);
    assert(NitroPort_PicaDvle(dvlb, 0) == test_dvle(0));
    assert(NitroPort_PicaDvle(dvlb, 1) == test_dvle(1));
    assert(!NitroPort_PicaDvle(dvlb, 2));
    assert(!NitroPort_PicaDvle(0, 0));
    return 0;
}
''')
            common = ["gcc", "-std=gnu99", "-O2", "-Wno-pointer-to-int-cast",
                      "-Wno-int-to-pointer-cast", f"-I{tmp}", f"-I{CTRU}", f"-I{C3D}"]
            sources = [(ROOT / "3ds_port/game/app/port_pica_abi.c", "-fshort-enums"),
                       (tmp / "library_side.c", "-fshort-enums"),
                       (tmp / "port_side.c", "-fno-short-enums")]
            objects = []
            for i, (source, mode) in enumerate(sources):
                obj = tmp / f"{i}.o"
                subprocess.run(common + [mode, "-c", str(source), "-o", str(obj)], check=True,
                               capture_output=True, text=True)
                objects.append(str(obj))
            binary = tmp / "test_pica_abi"
            subprocess.run(["gcc", *objects, "-o", str(binary)], check=True,
                           capture_output=True, text=True)
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()
