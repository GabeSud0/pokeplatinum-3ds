/*
 * The DS CPU and system, as far as NitroSDK's OS library needs them.
 *
 * - The status register (interrupt enable bits, CPU mode) is a variable.
 * - Interrupts are raised by the port's emulated hardware, which runs
 *   OS_IrqHandler just like the real interrupt vector would.
 * - OS_Halt ("sleep until the next interrupt") advances one frame.
 * - Thread switching, reset and exceptions have hooks for the 3DS main
 *   program; until it provides them, they report "not implemented".
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

    reg_OS_IF |= mask;

    /* Run handlers while interrupts are enabled and something is pending,
     * one interrupt per call, lowest number first, as on the DS. */
    for (guard = 0; guard < 32; guard++) {
        if (OSi_PortCpsr & HW_PSR_IRQ_DISABLE) {
            return;
        }
        if (reg_OS_IME == 0 || (reg_OS_IE & reg_OS_IF) == 0) {
            return;
        }
        OS_IrqHandler();
    }
}

__attribute__((weak)) void NitroPort_OnFrame(void)
{
}

__attribute__((weak)) void NitroPort_Halt(void)
{
    /* The DS sleeps until an interrupt arrives. In a game the next one is
     * (almost always) V-blank, so: finish this frame, then raise it. While
     * the CPU sleeps, interrupts are taken even if the CPSR masks them,
     * because OS_WaitInterrupt re-enables them right after. */
    u32 saved = OSi_PortCpsr;

    NitroPort_OnFrame();
    reg_GX_VCOUNT = 192; /* the line where V-blank starts */
    OSi_PortCpsr &= ~HW_PSR_IRQ_DISABLE;
    NitroPort_RaiseIrq(OS_IE_V_BLANK);
    OSi_PortCpsr = saved;
    reg_GX_VCOUNT = 0;
}

/* ---------------------------------------------------------------- threads */

__attribute__((weak)) BOOL NitroPort_SaveContext(void *context)
{
    (void)context;
    NitroPort_Unimplemented("OS_SaveContext (thread switching)");
    return FALSE;
}

__attribute__((weak)) void NitroPort_LoadContext(void *context)
{
    (void)context;
    NitroPort_Unimplemented("OS_LoadContext (thread switching)");
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
