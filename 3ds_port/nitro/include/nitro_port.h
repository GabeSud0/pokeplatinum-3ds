/*
 * nitro_port.h: the 3DS port's side of libnitro.
 *
 * NitroSDK talks to DS hardware. On the 3DS that hardware doesn't exist, so
 * the port provides it in software. This header is the interface between
 * the two: logging, "not implemented yet" reporting, and the hooks the rest
 * of the port (the 3DS main loop, the renderer, the virtual ARM7) plugs into.
 */

#ifndef NITRO_PORT_H_
#define NITRO_PORT_H_

#include <nitro/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ logging */

/* printf-style log line. Goes to stderr on PC; on 3DS, wherever the port's
 * main program sends it (weak, so the program can replace it). */
void NitroPort_Log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Report that `what` was called but isn't implemented in the port yet.
 * Logged once per name, so a running game produces a short to-do list
 * rather than a flood. Returns how many distinct names have been reported. */
int NitroPort_Unimplemented(const char *what);

#define NITRO_PORT_UNIMPLEMENTED() ((void)NitroPort_Unimplemented(__func__))

/* Has `what` been reported? (For tests.) */
BOOL NitroPort_WasReported(const char *what);

/* ---------------------------------------------------------------- the CPU */

/* The DS CPU's status register, as far as the SDK cares: the mode bits and
 * the IRQ/FIQ disable bits (see OS_DisableInterrupts in os_system.c). */
extern u32 OSi_PortCpsr;

/* ------------------------------------------------------------- interrupts */

/* Hardware raising an interrupt: sets the bits in IF and, if the CPU has
 * interrupts enabled, runs OS_IrqHandler for each pending one. */
void NitroPort_RaiseIrq(u32 mask);

/* Called by OS_Halt: the DS CPU sleeps until the next interrupt. Weak.
 * The default advances one frame: it calls NitroPort_OnFrame() and raises
 * the V-blank interrupt. */
void NitroPort_Halt(void);

/* The port's per-frame hook (present the image, read input, run sound ...).
 * Weak, empty by default. Called from the default NitroPort_Halt. */
void NitroPort_OnFrame(void);

/* -------------------------------------------------------------------- DMA */

/* Called by NitroSDK after it writes a DMA channel's registers. Runs
 * immediate and 3D-FIFO transfers now; V-blank transfers wait for
 * NitroPort_DmaVBlank. Clears the busy bit and raises the channel's
 * interrupt when done, as the DS does. (src/port_dma.c) */
void NitroPort_DmaStart(u32 dmaNo);

/* Runs the transfers waiting for V-blank. The default NitroPort_Halt calls
 * it as V-blank starts; a replacement Halt must call it too. */
void NitroPort_DmaVBlank(void);

/* ---------------------------------------------------------------- threads */

/* OS_SaveContext / OS_LoadContext. Weak; real thread switching comes with
 * the 3DS main program. The defaults only report that they were called. */
BOOL NitroPort_SaveContext(void *context);
void NitroPort_LoadContext(void *context);

/* ----------------------------------------------------------------- reset */

/* OS_ResetSystem: restart the game. Weak; the default reports and stops. */
void NitroPort_ResetSystem(u32 parameter) __attribute__((noreturn));

/* ------------------------------------------------------- the virtual ARM7 */

/* The DS's second CPU runs the touch screen, clock, power, sound, save chip
 * and wireless; the main CPU asks it for things through the PXI FIFO. The
 * port answers those requests in software, one handler per PXI tag. */
typedef void (*NitroPortArm7Handler)(int tag, u32 data, BOOL err);

/* Register the port's answer to messages the game sends on `tag`. */
void NitroPort_SetArm7Handler(int tag, NitroPortArm7Handler handler);

/* Send a message from the "ARM7" back to the game's callback for `tag`. */
void NitroPort_Arm7Reply(int tag, u32 data, BOOL err);

/* Register the emulated save-card, RTC, power and touch services.
 * Call before CARD/RTC/PM/TP initialization; registration is idempotent.
 * No DS ARM7 thread or libctru is required. */
void NitroPort_InitArm7Services(void);

/* Save-card backing store: raw 512 KiB FLASH image, emulator .sav layout.
 * Set before CARD_IdentifyBackup. A missing file is created filled with 0xFF.
 * Changing the path while a card request is active is not supported. */
void NitroPort_SetSavePath(const char *path);

/* The 3DS main loop supplies DS bottom-screen coordinates once per frame.
 * x is clamped to 0..255, y to 0..191; release ignores coordinates.
 * When TP auto-sampling is enabled, each call publishes one sample. */
void NitroPort_SetTouch(BOOL down, u16 x, u16 y);

#ifdef __cplusplus
}
#endif

#endif
