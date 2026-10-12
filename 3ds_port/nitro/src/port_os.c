/*
 * The DS CPU and system, as far as NitroSDK's OS library needs them.
 *
 * - The status register (interrupt enable bits, CPU mode) is a variable.
 * - Interrupts are raised by the port's emulated hardware, which runs
 *   OS_IrqHandler just like the real interrupt vector would, in the CPU's
 *   interrupt mode.
 * - OS_Halt ("sleep until the next interrupt") advances one frame.
 * - Thread switching is in port_thread.c. Reset and OS_Terminate have hooks
 *   for the main program.
 * - Replacements for SDK files that only configure the DS's ARM946 CPU
 *   (caches, tightly-coupled memory, the protection unit): there is nothing
 *   to configure on the 3DS, so they do nothing.
 */

#include <nitro.h>
#include <nitro_port.h>
#include <stdlib.h>

/* ---------------------------------------------------------------- the CPU */

/* System mode, interrupts enabled: how the DS CPU is set up before main. */
u32 OSi_PortCpsr = HW_PSR_SYS_MODE;

/* ------------------------------------------------------------- interrupts */

void NitroPort_RaiseIrq(u32 mask)
{
    int guard;
    u32 saved;

    reg_OS_IF |= mask;

    /* Run handlers while interrupts are enabled and something is pending,
     * one interrupt per call, lowest number first, as on the DS. The guard
     * only stops a handler that never clears its interrupt from hanging the
     * program; level-triggered ones (the 3D FIFO's) legitimately come back
     * many times in a row. */
    for (guard = 0; guard < 65536; guard++) {
        NitroPort_GxRefresh();   /* GXSTAT, and the GXFIFO level interrupt */
        if (OSi_PortCpsr & HW_PSR_IRQ_DISABLE) {
            return;
        }
        if (reg_OS_IME == 0 || (reg_OS_IE & reg_OS_IF) == 0) {
            return;
        }
        /* Taking an interrupt puts the CPU in interrupt mode with further
         * interrupts masked until the handler returns, as the ARM CPU does.
         * Anything raised meanwhile stays pending and runs on the next pass
         * of this loop. In interrupt mode the SDK's scheduler only notes
         * that a switch is wanted; OS_IrqHandler switches at its end
         * (port_thread.c), and this thread carries on from there when it's
         * switched back to. */
        saved = OSi_PortCpsr;
        OSi_PortCpsr = (saved & ~HW_PSR_CPU_MODE_MASK) | HW_PSR_IRQ_MODE | HW_PSR_IRQ_DISABLE;
        OS_IrqHandler();
        OSi_PortCpsr = saved;
    }
}

__attribute__((weak)) void NitroPort_OnFrame(void)
{
}

/* V-blank starts: its interrupt is requested and V-blank DMAs run (masked
 * meanwhile, so the interrupts are then taken lowest first). */
static void StartVBlank(void)
{
    u32 saved = OSi_PortCpsr;

    OSi_PortCpsr |= HW_PSR_IRQ_DISABLE;
    NitroPort_VBlankStarts();
    NitroPort_GxRefresh();
    reg_OS_IF |= OS_IE_V_BLANK;
    NitroPort_DmaVBlank();
    OSi_PortCpsr = saved;
    NitroPort_RaiseIrq(0);
}

/* DS time spent this frame on work the port does instantly (game card
 * reads); see NitroPort_SpendDsTime. */
static u64 sWorkCycles;

__attribute__((weak)) void NitroPort_Halt(void)
{
    /* The DS sleeps until an interrupt arrives. In a game the next one is
     * (almost always) V-blank, so: finish this frame, then raise it. If the
     * CPU has interrupts masked (OS_WaitInterrupt, OS_Terminate), it wakes
     * up but doesn't take them: they stay pending until it unmasks them. */
    NitroPort_OnFrame();
    sWorkCycles = 0;
    StartVBlank();
}

void NitroPort_SpendDsTime(u64 cycles)
{
    /* A long load takes the DS several frames, and V-blank interrupts keep
     * coming meanwhile: the game relies on them, e.g. its V-blank task
     * list (32 entries) is emptied by the V-blank handler while a field
     * loads dozens of models that each queue a texture upload there. The
     * port reads instantly, so it raises those V-blanks itself: one for
     * every frame's worth of DS time the work in this frame adds up to.
     * The frame isn't shown; the next OS_Halt still ends it. */
    sWorkCycles += cycles;
    while (sWorkCycles >= NITRO_PORT_CYCLES_PER_FRAME) {
        sWorkCycles -= NITRO_PORT_CYCLES_PER_FRAME;
        StartVBlank();
    }
}

/* ----------------------------------------------------------- OS_Terminate */

__attribute__((weak, noreturn)) void NitroPort_Terminate(void)
{
    NitroPort_Log("[nitro] OS_Terminate: the game stopped itself");
    abort();
}

void OS_Terminate(void)
{
    /* The DS stops here for good (interrupts off, halted). The port tells
     * the main program, which reports it. */
    (void)OS_DisableInterrupts();
    NitroPort_Terminate();
}

/* ----------------------------------------------------- reset (os_reset.c) */

static BOOL sResetInitialized;

void OS_InitReset(void)
{
    sResetInitialized = TRUE;
}

BOOL OS_IsResetOccurred(void)
{
    return FALSE;
}

__attribute__((weak, noreturn)) void NitroPort_ResetSystem(u32 parameter)
{
    NitroPort_Log("[nitro] OS_ResetSystem(%lu): restarting isn't implemented yet",
                  (unsigned long)parameter);
    abort();
}

void OS_ResetSystem(u32 parameter)
{
    /* The DS keeps the parameter in main memory across the reset;
     * OS_GetResetParameter reads it back after the restart. */
    *(u32 *)HW_RESET_PARAMETER_BUF = parameter;
    NitroPort_ResetSystem(parameter);
}

/* ---------------------------------------------- exceptions (os_exception.c) */

static OSExceptionHandler sUserExceptionHandler;
static void *sUserExceptionArg;

void OS_InitException(void)
{
}

void OS_SetUserExceptionHandler(OSExceptionHandler handler, void *arg)
{
    /* Stored for completeness; crashes on the 3DS are reported by the 3DS. */
    sUserExceptionHandler = handler;
    sUserExceptionArg = arg;
}

void OS_EnableUserExceptionHandlerOnDebugger(void)
{
}

void OS_SetExceptionVectorUpper(void)
{
}

void OS_SetExceptionVectorLower(void)
{
}

/* ------------------------------------- CPU configuration (os_cache.c etc.) */
/* The DS's ARM946 has caches that must be flushed before DMA and the GPU
 * read memory, tightly-coupled memories, and a protection unit. The port's
 * "DMA" and "GPU" are ordinary code reading ordinary memory, so none of
 * this is needed. */

BOOL DC_Enable(void) { return TRUE; }
BOOL DC_Disable(void) { return TRUE; }
BOOL DC_Restore(BOOL enable) { (void)enable; return TRUE; }
void DC_InvalidateAll(void) {}
void DC_StoreAll(void) {}
void DC_FlushAll(void) {}
void DC_InvalidateRange(void *startAddr, u32 nBytes) { (void)startAddr; (void)nBytes; }
void DC_StoreRange(const void *startAddr, u32 nBytes) { (void)startAddr; (void)nBytes; }
void DC_FlushRange(const void *startAddr, u32 nBytes) { (void)startAddr; (void)nBytes; }
void DC_TouchRange(const void *startAddr, u32 nBytes) { (void)startAddr; (void)nBytes; }
void DC_LockdownRange(const void *startAddr, u32 nBytes) { (void)startAddr; (void)nBytes; }
void DC_UnlockdownAll(void) {}
void DC_Unlockdown(u32 num) { (void)num; }
void DC_WaitWriteBufferEmpty(void) {}

BOOL IC_Enable(void) { return TRUE; }
BOOL IC_Disable(void) { return TRUE; }
BOOL IC_Restore(BOOL enable) { (void)enable; return TRUE; }
void IC_InvalidateAll(void) {}
void IC_InvalidateRange(void *startAddr, u32 nBytes) { (void)startAddr; (void)nBytes; }
void IC_PrefetchRange(const void *startAddr, u32 nBytes) { (void)startAddr; (void)nBytes; }
void IC_LockdownRange(const void *startAddr, u32 nBytes) { (void)startAddr; (void)nBytes; }
void IC_UnlockdownAll(void) {}
void IC_Unlockdown(u32 num) { (void)num; }

void OS_EnableITCM(void) {}
void OS_DisableITCM(void) {}
void OS_SetITCMParam(u32 param) { (void)param; }
u32 OS_GetITCMParam(void) { return 0; }
void OS_EnableDTCM(void) {}
void OS_DisableDTCM(void) {}
void OS_SetDTCMParam(u32 param) { (void)param; }
u32 OS_GetDTCMParam(void) { return 0; }
void OS_SetDTCMAddress(u32 address) { (void)address; }
u32 OS_GetDTCMAddress(void) { return HW_DTCM; }

void OS_EnableProtectionUnit(void) {}
void OS_DisableProtectionUnit(void) {}
void OS_EnableICacheForProtectionRegion(u32 flags) { (void)flags; }
void OS_DisableICacheForProtectionRegion(u32 flags) { (void)flags; }
u32 OS_GetICacheEnableFlagsForProtectionRegion(void) { return 0; }
void OS_SetIPermissionsForProtectionRegion(u32 setMask, u32 flags) { (void)setMask; (void)flags; }
u32 OS_GetIPermissionsForProtectionRegion(void) { return 0; }
void OS_EnableDCacheForProtectionRegion(u32 flags) { (void)flags; }
void OS_DisableDCacheForProtectionRegion(u32 flags) { (void)flags; }
u32 OS_GetDCacheEnableFlagsForProtectionRegion(u32 flags) { (void)flags; return 0; }
void OS_SetDPermissionsForProtectionRegion(u32 setMask, u32 flags) { (void)setMask; (void)flags; }
u32 OS_GetDPermissionsForProtectionRegion(void) { return 0; }
void OS_EnableWriteBufferForProtectionRegion(u32 flags) { (void)flags; }
void OS_DisableWriteBufferForProtectionRegion(u32 flags) { (void)flags; }
u32 OS_GetWriteBufferEnableFlagsForProtectionRegion(u32 flags) { (void)flags; return 0; }
void OS_SetProtectionRegion0(u32 param) { (void)param; }
void OS_SetProtectionRegion1(u32 param) { (void)param; }
void OS_SetProtectionRegion2(u32 param) { (void)param; }
void OS_SetProtectionRegion3(u32 param) { (void)param; }
void OS_SetProtectionRegion4(u32 param) { (void)param; }
void OS_SetProtectionRegion5(u32 param) { (void)param; }
void OS_SetProtectionRegion6(u32 param) { (void)param; }
void OS_SetProtectionRegion7(u32 param) { (void)param; }
u32 OS_GetProtectionRegion0(void) { return 0; }
u32 OS_GetProtectionRegion1(void) { return 0; }
u32 OS_GetProtectionRegion2(void) { return 0; }
u32 OS_GetProtectionRegion3(void) { return 0; }
u32 OS_GetProtectionRegion4(void) { return 0; }
u32 OS_GetProtectionRegion5(void) { return 0; }
u32 OS_GetProtectionRegion6(void) { return 0; }
u32 OS_GetProtectionRegion7(void) { return 0; }
