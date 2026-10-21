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
#include <nitro_port_render2d.h>

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
 * interrupts enabled, runs OS_IrqHandler for each pending one (in the CPU's
 * interrupt mode; a thread switch the interrupt asks for happens at its
 * end). NitroPort_RaiseIrq(0) takes interrupts that are already pending. */
void NitroPort_RaiseIrq(u32 mask);

/* Called by OS_Halt: the DS CPU sleeps until the next interrupt. Weak.
 * The default advances one frame: it calls NitroPort_OnFrame(), starts
 * V-blank (NitroPort_VBlankStarts, V-blank DMAs) and requests its
 * interrupt, which is taken if the CPU has interrupts enabled. */
void NitroPort_Halt(void);

/* The port's per-frame hook (present the image, read input, run sound ...).
 * Weak, empty by default. Called from the default NitroPort_Halt. */
void NitroPort_OnFrame(void);

/* The game has done what it does after V-blank for the next picture
 * (screen fades, its V-blank callback's OAM, palette and VRAM updates, its
 * post-V-blank tasks): the display is now as the DS shows it this frame.
 * The game calls it (patches/pokeplatinum/0011); a port that draws the
 * screens on other threads starts drawing here, not halfway through those
 * changes. Weak, empty by default. */
void NitroPort_VBlankWorkDone(void);

/* DS bus cycles in one frame: 263 lines of 2130 cycles (59.83 frames/s). */
#define NITRO_PORT_CYCLES_PER_FRAME (263ULL * 2130ULL)

/* Work the port does instantly but the DS takes time for (reading the game
 * card) reports that time here, in DS bus cycles. When this frame's work
 * adds up to a whole frame, V-blank starts and its interrupt is taken (if
 * the CPU has interrupts enabled), as it would during a long load on the
 * DS; without a new frame being shown. The count restarts each frame. */
void NitroPort_SpendDsTime(u64 cycles);

/* ------------------------------------------------------------ timers, clock */

/* The DS's four timers and the V-count run in software from a real clock
 * (src/port_timer.c). */

/* The clock, in DS bus cycles (33,513,982 a second) since it started. */
u64 NitroPort_Cycles(void);

/* Tests: drive the clock yourself (NULL: back to the real clock). */
void NitroPort_SetClockSource(u64 (*source)(void));

/* Bring the timers (and V-count) up to date with the clock, requesting
 * their overflow interrupts. The port does this every frame and whenever
 * the SDK reads the tick. */
void NitroPort_SyncTimers(void);

/* V-blank starts now (V-count 192). Called by NitroPort_Halt. */
void NitroPort_VBlankStarts(void);

/* The 3D engine's status register (GXSTAT): idle, FIFO empty; and the
 * GXFIFO interrupt requested while its condition holds (port_gx.c). The
 * port calls it each frame, before taking interrupts and after DMA. */
void NitroPort_GxRefresh(void);

/* --------------------------------------------------------------- display */

/* Draw what a 2D engine (0: A, 1: B) shows, from the registers, palettes,
 * OAM and VRAM as they are now: 256x192 pixels, the DS's 15-bit colour
 * (bits 0-4 red, 5-9 green, 10-14 blue). See port_render2d.c. */
#define NITRO_PORT_SCREEN_W 256
#define NITRO_PORT_SCREEN_H 192
void NitroPort_Render2D(int engine, u16 *out);
/* The same, unless nothing it's drawn from changed since the last call for
 * that engine: then out isn't touched, and it returns 0 (1 when drawn). */
int NitroPort_Render2DIfChanged(int engine, u16 *out);

/* The same picture in two layers, for a compositor that draws the 3D
 * between them itself: see nitro_port_render2d.h (included above). */

/* Whether engine A's picture is on the upper screen (POWCNT1 bit 15). */
BOOL NitroPort_EngineAOnTop(void);

/* Both screens, each 256x192: what the DS's upper and lower screens show. */
void NitroPort_RenderScreens(u16 *top, u16 *bottom);

/* -------------------------------------------------------------------- DMA */

/* Called by NitroSDK after it writes a DMA channel's registers. Runs
 * immediate and 3D-FIFO transfers now; V-blank transfers wait for
 * NitroPort_DmaVBlank. Clears the busy bit and raises the channel's
 * interrupt when done, as the DS does. (src/port_dma.c) */
void NitroPort_DmaStart(u32 dmaNo);

/* Runs the transfers waiting for V-blank. The default NitroPort_Halt calls
 * it as V-blank starts; a replacement Halt must call it too. */
void NitroPort_DmaVBlank(void);

/* An H-blank: the H-blank transfers' next unit each (NitroPort_HBlankLines
 * goes through a frame's lines); whether any is armed. */
void NitroPort_DmaHBlank(void);
int NitroPort_DmaHBlankArmed(void);

/* ------------------------------------------------- H-blank (line by line) */

/* What the DS's display registers are on each line of a frame, where the
 * game changes them line by line (an H-blank interrupt handler or DMA:
 * the battles' start, a wavy screen): each engine's first 0x70 bytes
 * (DISPCNT, the BGs, the windows, the colour effects; engine B's at
 * 0x1000), for each of the 192 lines. active: the lines differ this frame
 * (else the frame's own registers are each line's). */
#define NITRO_LINE_REGS 0x70
typedef struct NitroLineRegs {
    int active;
    u32 serial;   /* counts the frames gone through (that the renderer's signature has) */
    u8 regs[2][NITRO_PORT_SCREEN_H][NITRO_LINE_REGS];
} NitroLineRegs;
extern NitroLineRegs NitroPort_LineRegs;

/* Go through the frame about to be shown a line at a time, as the DS draws
 * it: before each line, the H-blank of the line before (its H-blank DMAs,
 * then its interrupt, with the V-count that line's), each line's registers
 * kept in NitroPort_LineRegs. The game's thread, after its V-blank work,
 * before the display state is taken for drawing. Does nothing (active 0)
 * without an H-blank interrupt or DMA. (src/port_os.c) */
void NitroPort_HBlankLines(void);

/* ---------------------------------------------------------------- threads */

/* OS_SaveContext / OS_LoadContext: every SDK thread is played by a thread
 * of the machine, one running at a time (src/port_thread.c). */
BOOL NitroPort_SaveContext(void *context);
void NitroPort_LoadContext(void *context);

/* The end of an interrupt: wake the threads in OS_WaitIrq and switch if a
 * switch was asked for (OS_IrqHandler_ThreadSwitch calls it). */
void NitroPort_IrqThreadSwitch(void);

/* Tests: how many machine threads currently play SDK threads. */
int NitroPort_ThreadsInUse(void);

/* ----------------------------------------------------------------- reset */

/* OS_ResetSystem: restart the game. Weak; the default reports and stops. */
void NitroPort_ResetSystem(u32 parameter) __attribute__((noreturn));

/* OS_Terminate: the game stopped itself for good. Weak; the default
 * reports and aborts. The main program shows it instead. */
void NitroPort_Terminate(void) __attribute__((noreturn));

/* ----------------------------------------------- firmware state, buttons */

/* What the DS firmware hands a game: its cartridge header and the user's
 * settings (src/port_boot.c). */
typedef struct NitroPortFirmware {
    const void *romHeader;     /* copied to HW_ROM_HEADER_BUF */
    u32 romHeaderSize;
    const u16 *nickname;       /* UTF-16, up to 10 characters, 0-terminated */
    u8 language;               /* NVRAM_CONFIG_LANG_* */
    u8 birthMonth;
    u8 birthDay;
    u8 favoriteColor;          /* 0-15 */
    u8 macAddress[6];
    s64 rtcOffset;             /* the "clock was changed" offset games save */
} NitroPortFirmware;

/* Write the firmware's state into the port's main memory. Call before the
 * game's main (and OS_Init). */
void NitroPort_BootFirmware(const NitroPortFirmware *firmware);

/* The touch screen calibration NitroPort_BootFirmware stores maps a screen
 * position (x, y) to raw readings (x, y) * NITRO_PORT_TP_RAW_SCALE, so a
 * touch-screen ARM7 service reports raw values in that scale. */
#define NITRO_PORT_TP_RAW_SCALE 16

/* The buttons held now (PAD_BUTTON_* / PAD_KEY_* bits) and the lid: written
 * to the key register and the ARM7's X/Y/lid word, as the DS has them. */
void NitroPort_SetButtons(u16 held, BOOL lidClosed);

/* --------------------------------------------------------------- overlays */

/* Where the DS loaded an overlay (which of the game's overlay slots it uses). */
enum {
    NITRO_PORT_OVERLAY_MAIN = 0,
    NITRO_PORT_OVERLAY_ITCM = 1,
    NITRO_PORT_OVERLAY_DTCM = 2
};

/* One overlay of the program, in overlay-ID order: the bounds of its
 * initialised data and of its zero-initialised data (NULL if none). */
typedef struct NitroPortOverlay {
    const char *name;
    u32 region;
    u8 *dataStart;
    u8 *dataEnd;
    u8 *bssStart;
    u8 *bssEnd;
} NitroPortOverlay;

/* Give the port the program's overlays, before any game code runs. A copy
 * of each one's initial data is taken; FS_LoadOverlay then restores it and
 * clears the zero-initialised part, as reading the overlay from the
 * cartridge did (src/port_overlay.c). */
void NitroPort_SetOverlays(const NitroPortOverlay *overlays, u32 count);
u32 NitroPort_OverlayCount(void);
BOOL NitroPort_IsOverlayLoaded(u32 id);

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
/* The save's changes not in its file yet (the pages of a write until its
 * last: a write goes into the file whole), into it now: for the HOME menu,
 * sleep and closing (a write under way finishes it itself). And how many
 * writes into the file there have been. Any thread. */
void NitroPort_CardFlush(void);
uint32_t NitroPort_CardFileWrites(void);

/* The 3DS main loop supplies DS bottom-screen coordinates once per frame.
 * x is clamped to 0..255, y to 0..191; release ignores coordinates. The
 * panel reports them as raw readings (x, y) * NITRO_PORT_TP_RAW_SCALE, which
 * the firmware calibration turns back into screen positions.
 * When TP auto-sampling is enabled, each call publishes one sample. */
void NitroPort_SetTouch(BOOL down, u16 x, u16 y);

#ifdef __cplusplus
}
#endif

#endif
