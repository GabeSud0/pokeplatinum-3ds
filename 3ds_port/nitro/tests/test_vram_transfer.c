/*
 * NitroSystem's VRAM transfer manager (the patched original,
 * libraries/gfd/src/VramTransferMan/gfd_VramTransferManager.c) with the
 * port's OS_Halt.
 *
 * The game queues transfers for its V-blank work (NNS_GfdDoVramTransfer).
 * On the DS the 3D engine draws the scene the game has just ended after
 * that work; the port draws it at the OS_Halt that ends the frame. So the
 * 3D transfers (texture, texture palette) are done at the halt, and the
 * 2D ones still at the V-blank. The bytes are made up here.
 */

#include <nitro.h>
#include <nitro_port.h>
#include <nnsys/gfd/VramTransferMan/gfd_VramTransferManager.h>
#include <stdio.h>
#include <string.h>

extern unsigned char NitroPort_LcdcVram[];

static int sChecks;
static int sFailures;

#define CHECK(cond)                                                          \
    do {                                                                     \
        sChecks++;                                                           \
        if (!(cond)) {                                                       \
            sFailures++;                                                     \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);           \
        }                                                                    \
    } while (0)

/* The SDK copies these by DMA, whose registers hold 32-bit addresses:
 * static, not on a 64-bit PC's stack. */
static NNSGfdVramTransferTask sTasks[8];
static u32 sTex[16], sPltt[8], sBg[16], sTex2[16];

static int Filled(const u8 *p, u32 size, u8 value)
{
    u32 i;

    for (i = 0; i < size; i++) {
        if (p[i] != value) {
            return 0;
        }
    }
    return 1;
}

static void test_3d_transfers_at_the_halt(void)
{
    /* Bank A for textures (LCDC 0x00000), F for texture palettes (slot 0,
     * LCDC 0x90000), B for engine A's BGs, BG0's characters at 0. */
    GX_Init();
    GX_SetPower(GX_POWER_ALL);
    GX_SetBankForTex(GX_VRAM_TEX_0_A);
    GX_SetBankForTexPltt(GX_VRAM_TEXPLTT_0_F);
    GX_SetBankForBG(GX_VRAM_BG_128_B);
    GX_SetGraphicsMode(GX_DISPMODE_GRAPHICS, GX_BGMODE_0, GX_BG0_AS_2D);
    G2_SetBG0Control(GX_BG_SCRSIZE_TEXT_256x256, GX_BG_COLORMODE_16, GX_BG_SCRBASE_0x0000,
                     GX_BG_CHARBASE_0x00000, GX_BG_EXTPLTT_01);
    memset(NitroPort_LcdcVram, 0, 0x200);
    memset(NitroPort_LcdcVram + 0x90000, 0, 0x100);
    memset(NitroPort_BgVram, 0, 0x200);

    memset(sTex, 0x11, sizeof(sTex));
    memset(sPltt, 0x22, sizeof(sPltt));
    memset(sBg, 0x33, sizeof(sBg));
    NNS_GfdInitVramTransferManager(sTasks, 8);
    CHECK(NNS_GfdRegisterNewVramTransferTask(NNS_GFD_DST_2D_BG0_CHAR_MAIN, 0x40, sBg, sizeof(sBg)));
    CHECK(NNS_GfdRegisterNewVramTransferTask(NNS_GFD_DST_3D_TEX_VRAM, 0x100, sTex, sizeof(sTex)));
    CHECK(NNS_GfdRegisterNewVramTransferTask(NNS_GFD_DST_3D_TEX_PLTT, 0x20, sPltt, sizeof(sPltt)));
    CHECK(NNS_GfdGetVramTransferTaskTotalSize() == 64 + 64 + 32);

    /* The frame ends: the 3D ones are done, the 2D one waits. */
    NitroPort_Halt();
    CHECK(Filled(NitroPort_LcdcVram + 0x100, 64, 0x11));
    CHECK(Filled(NitroPort_LcdcVram + 0x90020, 32, 0x22));
    CHECK(Filled(NitroPort_BgVram + 0x40, 64, 0));
    /* Still queued, as on the DS until the V-blank work. */
    CHECK(NNS_GfdGetVramTransferTaskTotalSize() == 64 + 64 + 32);

    /* The V-blank work: the 2D one now; the 3D ones aren't done again (the
     * game may have reused their source). */
    memset(sTex, 0x44, sizeof(sTex));
    NNS_GfdDoVramTransfer();
    CHECK(Filled(NitroPort_BgVram + 0x40, 64, 0x33));
    CHECK(Filled(NitroPort_LcdcVram + 0x100, 64, 0x11));
    CHECK(NNS_GfdGetVramTransferTaskTotalSize() == 0);

    /* A frame with nothing queued; then the queue goes on as before, the
     * slots of the tasks done at the halt reused (the queue wraps). */
    NitroPort_Halt();
    memset(sTex2, 0x55, sizeof(sTex2));
    CHECK(NNS_GfdRegisterNewVramTransferTask(NNS_GFD_DST_3D_TEX_VRAM, 0x180, sTex2, sizeof(sTex2)));
    NNS_GfdDoVramTransfer();      /* without a halt first: done here */
    CHECK(Filled(NitroPort_LcdcVram + 0x180, 64, 0x55));
    {
        int i;

        for (i = 0; i < 8; i++) {
            CHECK(NNS_GfdRegisterNewVramTransferTask(NNS_GFD_DST_3D_TEX_VRAM, 0x1c0, sTex, 4));
        }
        CHECK(!NNS_GfdRegisterNewVramTransferTask(NNS_GFD_DST_3D_TEX_VRAM, 0x1c0, sTex, 4));
        NitroPort_Halt();
        CHECK(Filled(NitroPort_LcdcVram + 0x1c0, 4, 0x44));
        NNS_GfdDoVramTransfer();
        CHECK(NNS_GfdGetVramTransferTaskTotalSize() == 0);
    }
}

int main(void)
{
    OS_Init();
    test_3d_transfers_at_the_halt();
    printf("VRAM transfer manager: %d checks, %d failures\n", sChecks, sFailures);
    return sFailures ? 1 : 0;
}
