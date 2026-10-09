/*
 * The DS's I/O registers, as ordinary memory.
 *
 * On the DS, address 0x04000000 onwards is hardware: writing a register
 * changes what the display, sound or divider does. The 3DS has nothing there,
 * so with SDK_PORT_3DS the SDK's HW_IOREG points at this array instead
 * (see patches/NitroSDK). Code that only stores and reads back register
 * values works unchanged; registers with side effects (the divider, the 3D
 * command FIFO, DMA ...) get a software implementation that reads this array.
 *
 * Covers 0x04000000-0x0410FFFF: the ARM9 registers plus the IPC/card data
 * ports at 0x04100000.
 */

#include <nitro/types.h>
#include <nitro/hw/ARM9/mmap_global.h>

unsigned char NitroPort_IoReg[HW_IOREG_SIZE] __attribute__((aligned(32)));
