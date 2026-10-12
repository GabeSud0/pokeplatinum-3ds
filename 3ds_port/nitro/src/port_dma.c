/*
 * The DS's four DMA channels, in software.
 *
 * NitroSDK starts a DMA by writing a channel's source, destination and
 * control registers; patches/NitroSDK/0009 then calls NitroPort_DmaStart.
 * On the DS the transfer then runs by itself, and the hardware clears the
 * control register's enable ("busy") bit when it's done; the SDK waits for
 * that bit. Here:
 *
 * - Immediate and geometry-FIFO DMAs run straight away. FIFO data goes to
 *   the GXFIFO register, the same place 3D commands the CPU writes itself
 *   go; the 3D command translator will take them from there.
 * - V-blank DMAs run at the next V-blank (NitroPort_DmaVBlank, called by
 *   NitroPort_Halt), and at every V-blank after that if they repeat.
 * - H-blank, display-start, main-memory-display, DS card and GBA cartridge
 *   timings need the renderer or the card. They're reported as not
 *   implemented and stay pending, as if the trigger never came.
 *
 * If the control register asks for an interrupt, the channel's interrupt
 * is raised at the end; that's how the SDK runs DMA completion callbacks.
 */

#include <nitro.h>
#include <nitro_port.h>
#include <nitro_port_gx3d.h>

#define PORT_DMA_CHANNELS 4

/* Where a repeating transfer carries on from. The DS keeps these in
 * internal registers: the source always continues, the destination
 * continues unless the control register says "reload". */
static u32 sNextSrc[PORT_DMA_CHANNELS];
static u32 sNextDest[PORT_DMA_CHANNELS];

static vu32 *ChannelRegs(u32 dmaNo)
{
    /* Source, destination, control: three words per channel. */
    return (vu32 *)((u32)REG_DMA0SAD_ADDR + dmaNo * 12);
}

static s32 Step(u32 mode, u32 unit)
{
    /* The source and destination address modes share one encoding
     * (increment, decrement, fixed); destination mode 3 is
     * "increment, then reload" and a source never uses 3. */
    switch (mode) {
    case 0: return (s32)unit;
    case 1: return -(s32)unit;
    case 2: return 0;
    default: return (s32)unit;
    }
}

static void Transfer(u32 dmaNo, u32 cnt)
{
    u32 unit = (cnt & MI_DMA_32BIT_BUS) ? 4 : 2;
    u32 count = (cnt & REG_MI_DMA0CNT_WORDCNT_MASK) >> REG_MI_DMA0CNT_WORDCNT_SHIFT;
    u32 srcMode = (cnt & REG_MI_DMA0CNT_SAR_MASK) >> REG_MI_DMA0CNT_SAR_SHIFT;
    u32 destMode = (cnt & REG_MI_DMA0CNT_DAR_MASK) >> REG_MI_DMA0CNT_DAR_SHIFT;
    s32 srcStep = Step(srcMode, unit);
    s32 destStep = Step(destMode, unit);
    u32 src = sNextSrc[dmaNo];
    u32 dest = sNextDest[dmaNo];
    u32 i;

    /* On the ARM9, a count of 0 means the maximum (0x200000 units). */
    if (count == 0) {
        count = (REG_MI_DMA0CNT_WORDCNT_MASK >> REG_MI_DMA0CNT_WORDCNT_SHIFT) + 1;
    }

    /* One unit at a time, in order, like the hardware: a fixed destination
     * (an I/O port such as the 3D FIFO) sees every value, and overlapping
     * copies behave as on the DS. */
    if (unit == 4) {
        for (i = 0; i < count; i++) {
            u32 word = *(const vu32 *)src;
            /* GXFIFO is a write-only stream, not a memory location. Every
             * DMA word must be consumed in order (including packed commands
             * whose parameters span multiple interrupt-sized bursts). */
            if (dest >= (u32)REG_GXFIFO_ADDR &&
                dest < (u32)REG_GXFIFO_ADDR + 0x40) {
                NitroPort_GxFifoWord(word);
            } else {
                *(vu32 *)dest = word;
            }
            src += (u32)srcStep;
            dest += (u32)destStep;
        }
    } else {
        for (i = 0; i < count; i++) {
            *(vu16 *)dest = *(vu16 *)src;
            src += (u32)srcStep;
            dest += (u32)destStep;
        }
    }

    sNextSrc[dmaNo] = src;
    sNextDest[dmaNo] = (destMode == 3) ? ChannelRegs(dmaNo)[1] : dest;
}

static void Finish(u32 dmaNo, u32 cnt, BOOL repeats)
{
    /* A one-shot transfer is done: the hardware clears the enable bit.
     * A repeating one stays armed until the game stops it (MI_StopDma). */
    if (!repeats) {
        ChannelRegs(dmaNo)[2] = cnt & ~MI_DMA_ENABLE;
    }
    if (cnt & REG_MI_DMA0CNT_I_MASK) {
        NitroPort_RaiseIrq(OS_IE_DMA0 << dmaNo);
    }
}

void NitroPort_DmaStart(u32 dmaNo)
{
    vu32 *regs;
    u32 cnt;

    if (dmaNo >= PORT_DMA_CHANNELS) {
        return;
    }
    regs = ChannelRegs(dmaNo);
    cnt = regs[2];
    if (!(cnt & MI_DMA_ENABLE)) {
        return;
    }
    sNextSrc[dmaNo] = regs[0];
    sNextDest[dmaNo] = regs[1];

    switch (cnt & MI_DMA_TIMING_MASK) {
    case MI_DMA_TIMING_IMM:
    case MI_DMA_TIMING_GXFIFO:
        /* The FIFO DMA really feeds the 3D engine in bursts as it drains;
         * the port's FIFO never fills, so it all goes at once. */
        Transfer(dmaNo, cnt);
        NitroPort_GxRefresh();   /* the FIFO is empty again (port_gx.c) */
        Finish(dmaNo, cnt, FALSE);
        break;
    case MI_DMA_TIMING_V_BLANK:
        break; /* NitroPort_DmaVBlank runs it */
    case MI_DMA_TIMING_H_BLANK:
        NitroPort_Unimplemented("DMA timing: H-blank");
        break;
    case MI_DMA_TIMING_DISP:
        NitroPort_Unimplemented("DMA timing: display start");
        break;
    case MI_DMA_TIMING_DISP_MMEM:
        NitroPort_Unimplemented("DMA timing: main memory display");
        break;
    case MI_DMA_TIMING_CARD:
        NitroPort_Unimplemented("DMA timing: DS card");
        break;
    default:
        NitroPort_Unimplemented("DMA timing: GBA cartridge");
        break;
    }
}

void NitroPort_DmaVBlank(void)
{
    u32 dmaNo;

    for (dmaNo = 0; dmaNo < PORT_DMA_CHANNELS; dmaNo++) {
        u32 cnt = ChannelRegs(dmaNo)[2];

        if ((cnt & MI_DMA_ENABLE) && (cnt & MI_DMA_TIMING_MASK) == MI_DMA_TIMING_V_BLANK) {
            Transfer(dmaNo, cnt);
            Finish(dmaNo, cnt, (cnt & MI_DMA_CONTINUOUS_ON) != 0);
        }
    }
}
