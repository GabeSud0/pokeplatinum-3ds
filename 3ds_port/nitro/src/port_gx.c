/*
 * The 3D engine's status, as the game reads it.
 *
 * Nothing draws the DS's 3D yet: commands written to the 3D FIFO (by the CPU
 * or by DMA) are taken and dropped. So the engine is always idle and its
 * command FIFO always empty, and GXSTAT must say so:
 *
 *   - FIFO empty and less than half full, 0 entries, not busy, no box or
 *     position test running, no matrix stack overflow.
 *   - The interrupt condition the game chose (GXSTAT bits 30-31: "FIFO less
 *     than half full" or "FIFO empty") always holds, so with it set the
 *     GXFIFO interrupt is requested. On the DS that interrupt is level
 *     triggered: it comes back for as long as the condition holds, even right
 *     after the handler acknowledged it.
 *
 * This matters for MI_SendGXCommandAsync, which NitroSystem uses to send a
 * model's display list: it waits for "less than half full", then sends the
 * list 118 words at a time, one piece per GXFIFO interrupt. With GXSTAT
 * reading 0 (as G3X_Init leaves it) it waited forever; that stopped
 * Pokemon Platinum's opening at its first 3D model.
 *
 * GXSTAT is plain memory (port_ioreg.c), so its status bits are put back
 * whenever the port gets control: every frame, before interrupts are
 * taken, and after each DMA.
 */

#include <nitro.h>
#include <nitro_port.h>

#define STATUS_SET   (REG_G3X_GXSTAT_E_MASK | REG_G3X_GXSTAT_H_MASK)
#define STATUS_CLEAR (REG_G3X_GXSTAT_B_MASK | REG_G3X_GXSTAT_F_MASK | \
                      REG_G3X_GXSTAT_FIFOCNT_MASK | REG_G3X_GXSTAT_SE_MASK | \
                      REG_G3X_GXSTAT_TB_MASK)

void NitroPort_GxRefresh(void)
{
    u32 stat = reg_G3X_GXSTAT;
    u32 cond = (stat & REG_G3X_GXSTAT_FI_MASK) >> REG_G3X_GXSTAT_FI_SHIFT;

    stat = (stat & ~STATUS_CLEAR) | STATUS_SET;
    if (stat != reg_G3X_GXSTAT) {
        reg_G3X_GXSTAT = stat;
    }
    /* 1: less than half full, 2: empty. Both always hold. */
    if (cond == GX_FIFOINTR_COND_UNDERHALF || cond == GX_FIFOINTR_COND_EMPTY) {
        reg_OS_IF |= OS_IE_GXFIFO;
    }
}
