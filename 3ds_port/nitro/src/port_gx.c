/*
 * Nintendo DS geometry FIFO status and its level-triggered interrupt.
 *
 * The stage-1 geometry engine in port_gx3d.c decodes the incoming command
 * stream immediately. Its command FIFO therefore drains before the SDK can
 * observe it, even for GXFIFO DMA bursts. Preserve real matrix stack levels,
 * overflow and test results while advertising FIFO empty/under-half, so
 * MI_SendGXCommandAsync keeps its original DMA IRQ/callback protocol.
 */
#include <nitro.h>
#include <nitro_port.h>
#include <nitro_port_gx3d.h>

#define FIFO_SET   (REG_G3X_GXSTAT_E_MASK | REG_G3X_GXSTAT_H_MASK)
#define FIFO_CLEAR (REG_G3X_GXSTAT_B_MASK | REG_G3X_GXSTAT_F_MASK | \
                    REG_G3X_GXSTAT_FIFOCNT_MASK)

void NitroPort_GxRefresh(void)
{
    u32 stat, cond;
    NitroPort_Gx3dRefreshStatus();
    stat=reg_G3X_GXSTAT;
    cond=(stat & REG_G3X_GXSTAT_FI_MASK)>>REG_G3X_GXSTAT_FI_SHIFT;
    stat=(stat & ~FIFO_CLEAR)|FIFO_SET;
    reg_G3X_GXSTAT=stat;
    if(cond==GX_FIFOINTR_COND_UNDERHALF||cond==GX_FIFOINTR_COND_EMPTY)
        reg_OS_IF|=OS_IE_GXFIFO;
}
