/*
 * The 3DS program: Pokémon Platinum's own code, run on the 3DS.
 *
 * Needs the player's data pack on the SD card (see the game README):
 *   sdmc:/3ds/pokeplatinum/platinum3ds.pak
 *   sdmc:/3ds/pokeplatinum/manifest.json
 * keeps the game's save in sdmc:/3ds/pokeplatinum/platinum.sav (a raw 512 KB
 * save, as emulators keep them: one can be copied in or out), and writes
 * everything the port logs to sdmc:/3ds/pokeplatinum/log.txt.
 *
 * Screens: the DS's two screens, drawn by the port's 2D renderer
 * (nitro/src/port_render2d.c), in the middle of the 3DS's (the upper one
 * 400x240, the lower one 320x240). L + R + SELECT switches to the log and
 * back: the program's status above, the log below. The log also comes up by
 * itself when something goes wrong (the game stuck, a crash, OS_Terminate).
 *
 * Speed: the New 3DS's faster clock is switched on. Sound has a thread of
 * its own on CPU core 1 (the system's core, of which a program may use a
 * share), as it had a CPU of its own on the DS. Drawing the screens costs
 * more than the game itself, so on a New 3DS it's done by a thread on core 2
 * while the game goes on with the next frame (as the DS draws while the game
 * runs); otherwise it's done between frames, and frames are left out when
 * the game would fall behind: the game keeps its speed, the screens show
 * fewer frames. Every two seconds a [perf] line in the log says where the
 * time went.
 *
 * Threads: the game runs on a 3DS thread of its own (it becomes NitroSDK's
 * launcher thread, and the SDK's other threads get 3DS threads of their
 * own; nitro/src/port_thread.c). The program's main thread waits until
 * something asks to quit (HOME menu "close", the game stopping, a crash),
 * then shuts down. Meanwhile it watches that frames keep coming: a game
 * waiting for a part of the port that isn't done yet can loop without
 * ever reaching the next frame, and then nothing would answer the HOME
 * button. After a few seconds without a frame it says so in the log, and
 * looks after the HOME menu itself from then on.
 */

#include <3ds.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "platinum.h"

/* The 2D renderer (nitro_port.h, which can't be included next to 3ds.h). */
void NitroPort_RenderScreens(uint16_t *top, uint16_t *bottom);

/* Audio uses the port's plain-C mixer; it deliberately does not include
 * NitroSDK headers alongside libctru. */
void NitroPort_Log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void NitroPort_SoundRender(int16_t *stereo, size_t frames);
void NitroPort_SoundPumpReplies(void);

#define AUDIO_FRAMES 512
#define AUDIO_BUFFERS 3
static Thread sSoundThread;
static bool sSoundThreadRunning;
static bool sNdspAvailable;
static int16_t *sSoundSamples;
static ndspWaveBuf sSoundBuf[AUDIO_BUFFERS];
static int sSoundCore = -1;
/* Time spent mixing, and sound mixed (in samples), for the [perf] line. */
static volatile u64 sSoundMixTicks;
static volatile u64 sSoundMixSamples;

static void TimedSoundRender(int16_t *stereo, size_t frames)
{
    u64 start = svcGetSystemTick();

    NitroPort_SoundRender(stereo, frames);
    sSoundMixTicks += svcGetSystemTick() - start;
    sSoundMixSamples += frames;
}

static void SoundMain(void *unused)
{
    int16_t silent[AUDIO_FRAMES * 2];
    int i;
    (void)unused;
    while (__atomic_load_n(&sSoundThreadRunning, __ATOMIC_ACQUIRE)) {
        if (sNdspAvailable) {
            for (i = 0; i < AUDIO_BUFFERS; ++i) {
                ndspWaveBuf *buf = &sSoundBuf[i];
                if (buf->status == NDSP_WBUF_FREE ||
                    buf->status == NDSP_WBUF_DONE) {
                    TimedSoundRender((int16_t *)buf->data_vaddr, AUDIO_FRAMES);
                    DSP_FlushDataCache(buf->data_vaddr, AUDIO_FRAMES * 4);
                    ndspChnWaveBufAdd(0, buf);
                }
            }
            svcSleepThread(1000000LL);
        } else {
            /* Even without dspfirm.cdc, continue driving Nintendo's ARM7
             * sequencer and releasing its commands from real audio time. */
            TimedSoundRender(silent, AUDIO_FRAMES);
            svcSleepThread(15625000LL);
        }
    }
    threadExit(0);
}

static void StartSound(s32 priority, int soundCore)
{
    Result result = ndspInit();
    int i;
    if (R_SUCCEEDED(result)) {
        sSoundSamples = linearAlloc(AUDIO_BUFFERS * AUDIO_FRAMES * 4);
        if (!sSoundSamples) {
            NitroPort_Log("[audio] not enough linear memory for DSP buffers; no output");
            ndspExit();
        } else {
            sNdspAvailable = true;
            ndspSetOutputMode(NDSP_OUTPUT_STEREO);
            ndspChnReset(0);
            ndspChnSetInterp(0, NDSP_INTERP_LINEAR);
            ndspChnSetRate(0, 32768.0f);
            ndspChnSetFormat(0, NDSP_FORMAT_STEREO_PCM16);
            for (i = 0; i < AUDIO_BUFFERS; ++i) {
                memset(&sSoundBuf[i], 0, sizeof(sSoundBuf[i]));
                sSoundBuf[i].data_vaddr = &sSoundSamples[i * AUDIO_FRAMES * 2];
                sSoundBuf[i].nsamples = AUDIO_FRAMES;
            }
            NitroPort_Log("[audio] NDSP stereo initialized at 32768 Hz");
        }
    } else {
        NitroPort_Log("[audio] DSP firmware unavailable (check sdmc:/3ds/dspfirm.cdc); "
                      "running sound driver without speaker output");
    }
    __atomic_store_n(&sSoundThreadRunning, true, __ATOMIC_RELEASE);
    /* Another core than the game's: core 1 (the system's, of which the
     * program may use a share: 30%, set in main). Ahead of the game's
     * threads, as it must keep up with the speaker. Failing that, the
     * game's core. */
    sSoundCore = soundCore;
    sSoundThread = threadCreate(SoundMain, NULL, 64 * 1024, priority - 2, soundCore, false);
    if (!sSoundThread && soundCore != -2) {
        sSoundCore = -2;
        sSoundThread = threadCreate(SoundMain, NULL, 64 * 1024, priority - 2, -2, false);
    }
    if (!sSoundThread) {
        __atomic_store_n(&sSoundThreadRunning, false, __ATOMIC_RELEASE);
        NitroPort_Log("[audio] couldn't start independent audio thread");
    }
}

static void StopSound(void)
{
    if (sSoundThread) {
        __atomic_store_n(&sSoundThreadRunning, false, __ATOMIC_RELEASE);
        threadJoin(sSoundThread, UINT64_MAX);
        threadFree(sSoundThread);
        sSoundThread = NULL;
    }
    if (sNdspAvailable) {
        ndspChnWaveBufClear(0);
        ndspExit();
        linearFree(sSoundSamples);
        sNdspAvailable = false;
        sSoundSamples = NULL;
    }
}

#define DATA_DIR      "sdmc:/3ds/pokeplatinum"
#define PAK_PATH      DATA_DIR "/platinum3ds.pak"
#define MANIFEST_PATH DATA_DIR "/manifest.json"
#define LOG_PATH      DATA_DIR "/log.txt"
#define SAVE_PATH     DATA_DIR "/platinum.sav"

/* The game's main thread: the DS gave it 16 KB; GCC's code and the port's
 * own want more. */
#define GAME_STACK_SIZE (1024 * 1024)

static PrintConsole sTop;
static PrintConsole sBottom;
static FILE *sLog;
static volatile unsigned long sFrame;
static LightEvent sQuit;
static PlatinumSettings sSettings;
static volatile bool sWatchdogHasHome;   /* the main thread handles HOME */
static volatile bool sShowLog = true;     /* the log instead of the game */
static volatile bool sScreensDirty = true;/* redraw the borders / consoles */

/* ---------------------------------------------------------------- logging */

/* The last lines, to show again when switching to the log. */
#define KEEP_LINES 24
static char sLines[KEEP_LINES][128];
static int sLineCount;
static char sStatus[64] = "Starting";

void NitroPort_Log(const char *fmt, ...)
{
    char line[256];
    va_list args;

    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);

    snprintf(sLines[sLineCount % KEEP_LINES], sizeof(sLines[0]), "%s", line);
    sLineCount++;
    if (sShowLog && !sScreensDirty) {
        consoleSelect(&sBottom);
        printf("%s\n", line);
    }
    if (sLog != NULL) {
        fprintf(sLog, "[%6lu] %s\n", sFrame, line);
        fflush(sLog);
    }
}

static void DrawStatus(void)
{
    consoleSelect(&sTop);
    printf("\x1b[8;0H\x1b[2K%s", sStatus);
}

/* Put the consoles back on both screens: the title and status above, the
 * kept log lines below. */
static void DrawLogScreens(void)
{
    int i, first = sLineCount > KEEP_LINES ? sLineCount - KEEP_LINES : 0;

    consoleInit(GFX_TOP, &sTop);
    consoleInit(GFX_BOTTOM, &sBottom);
    consoleSelect(&sTop);
    printf("\n  Pokemon Platinum - 3DS port\n");
    printf("  The game's own code, running natively.\n\n");
    printf("  L + R + SELECT: back to the game.\n");
    printf("  Log: " LOG_PATH "\n");
    DrawStatus();
    consoleSelect(&sBottom);
    for (i = first; i < sLineCount; i++) {
        printf("%s\n", sLines[i % KEEP_LINES]);
    }
}

static void ShowLog(bool on)
{
    if (sShowLog != on) {
        sShowLog = on;
        sScreensDirty = true;
    }
}

static void Status(const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    vsnprintf(sStatus, sizeof(sStatus), fmt, args);
    va_end(args);
    if (sShowLog && !sScreensDirty) {
        DrawStatus();
    }
}

/* ------------------------------------------------------------- quitting */

/* Ask the main thread to shut down, and stop this thread for good. */
static void Quit(void) __attribute__((noreturn));

static void Quit(void)
{
    LightEvent_Signal(&sQuit);
    for (;;) {
        svcSleepThread(1000000000LL);
    }
}

static void WaitForDrawing(void);

/* Leave a message on screen until the player presses START, then quit. */
static void StopWithMessage(void) __attribute__((noreturn));

static void StopWithMessage(void)
{
    WaitForDrawing();
    ShowLog(true);
    if (sScreensDirty) {
        sScreensDirty = false;
        DrawLogScreens();
    }
    NitroPort_Log("Press START to quit.");
    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & KEY_START) {
            break;
        }
        gfxFlushBuffers();
        gfxSwapBuffers();
        gspWaitForVBlank();
    }
    Quit();
}

void NitroPort_Terminate(void)
{
    NitroPort_Log("The game stopped itself (OS_Terminate).");
    StopWithMessage();
}

void NitroPort_ResetSystem(uint32_t parameter)
{
    NitroPort_Log("The game asked to restart (OS_ResetSystem %lu). Restarting isn't "
                  "supported yet.", (unsigned long)parameter);
    StopWithMessage();
}

/* ------------------------------------------------------------- crashes */

static void OnException(ERRF_ExceptionInfo *info, CpuRegisters *regs)
{
    static const char *const kinds[] = {
        "prefetch abort", "data abort", "undefined instruction", "VFP exception"
    };
    const char *kind = (unsigned)info->type < 4 ? kinds[info->type] : "exception";

    NitroPort_Log("CRASH: %s at pc=%08lx (address %08lx)", kind,
                  (unsigned long)regs->pc, (unsigned long)info->far);
    NitroPort_Log("lr=%08lx sp=%08lx cpsr=%08lx", (unsigned long)regs->lr,
                  (unsigned long)regs->sp, (unsigned long)regs->cpsr);
    NitroPort_Log("r0=%08lx r1=%08lx r2=%08lx r3=%08lx", (unsigned long)regs->r[0],
                  (unsigned long)regs->r[1], (unsigned long)regs->r[2],
                  (unsigned long)regs->r[3]);
    NitroPort_Log("Look up pc and lr in pokeplatinum.elf (arm-none-eabi-addr2line).");
    StopWithMessage();
}

static void CatchCrashes(void)
{
    threadOnException(OnException, RUN_HANDLER_ON_FAULTING_STACK, WRITE_DATA_TO_FAULTING_STACK);
}

/* Every thread the port starts for the game catches its crashes too. */
void NitroPort_PlatformThreadStarted(void)
{
    CatchCrashes();
}

/* -------------------------------------------------------------- a frame */

static uint16_t DsButtons(u32 held)
{
    uint16_t ds = 0;

    if (held & KEY_A)      ds |= PLATINUM_BUTTON_A;
    if (held & KEY_B)      ds |= PLATINUM_BUTTON_B;
    if (held & KEY_X)      ds |= PLATINUM_BUTTON_X;
    if (held & KEY_Y)      ds |= PLATINUM_BUTTON_Y;
    if (held & KEY_L)      ds |= PLATINUM_BUTTON_L;
    if (held & KEY_R)      ds |= PLATINUM_BUTTON_R;
    if (held & KEY_START)  ds |= PLATINUM_BUTTON_START;
    if (held & KEY_SELECT) ds |= PLATINUM_BUTTON_SELECT;
    /* The D-pad or the Circle Pad. */
    if (held & KEY_UP)     ds |= PLATINUM_KEY_UP;
    if (held & KEY_DOWN)   ds |= PLATINUM_KEY_DOWN;
    if (held & KEY_LEFT)   ds |= PLATINUM_KEY_LEFT;
    if (held & KEY_RIGHT)  ds |= PLATINUM_KEY_RIGHT;
    return ds;
}

/* ------------------------------------------------------------- the screens */

/* DS colour (bits 0-4 red, 5-9 green, 10-14 blue) to the 3DS's RGB565. */
static u16 sToRgb565[32768];

static void MakeColorTable(void)
{
    int c;

    for (c = 0; c < 32768; c++) {
        int r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;

        sToRgb565[c] = (u16)((r << 11) | (g << 6) | (g >> 4) | b);
    }
}

/* The 3DS's framebuffers are stored rotated: each column of the screen,
 * bottom to top, is a run of 240 pixels. */
static void Blit(gfxScreen_t screen, const u16 *ds, int left)
{
    u16 width, height;
    u16 *fb = (u16 *)gfxGetFramebuffer(screen, GFX_LEFT, &width, &height);
    int x, y;

    for (x = 0; x < 256; x++) {
        u16 *column = fb + (size_t)(left + x) * 240 + (239 - 24);
        const u16 *src = ds + x;

        for (y = 0; y < 192; y++) {
            column[-y] = sToRgb565[src[y * 256] & 0x7FFF];
        }
    }
}

static void ClearScreens(void)
{
    int i;

    for (i = 0; i < 2; i++) {
        u16 width, height;
        u8 *top = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, &width, &height);

        memset(top, 0, (size_t)width * height * 2);
        u8 *bottom = gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, &width, &height);

        memset(bottom, 0, (size_t)width * height * 2);
        gfxFlushBuffers();
        gfxSwapBuffers();
    }
}

static void ShowGame(void)
{
    static u16 top[256 * 192];
    static u16 bottom[256 * 192];

    NitroPort_RenderScreens(top, bottom);
    Blit(GFX_TOP, top, (400 - 256) / 2);
    Blit(GFX_BOTTOM, bottom, (320 - 256) / 2);
    gfxFlushBuffers();
    gfxSwapBuffers();
}

/* ------------------------------------------------------- drawing thread */

/* On a New 3DS: a thread on core 2 draws the screens while the game goes on
 * with its next frame. It reads the game's display state as the game
 * changes it, as the DS's display does; the game does most of that during
 * V-blank, as it must on the DS. */
static Thread sDrawThread;
static LightEvent sDrawGo;
static volatile bool sDrawBusy;
static volatile bool sDrawQuit;

/* For the [perf] line. */
static u64 sPerfStart;            /* the first frame counted */
static u64 sPerfGameTicks;        /* in the game's own code */
static u64 sPerfDrawTicks;        /* drawing the screens */
static unsigned sPerfFrames, sPerfShown;
static u64 sPerfSoundTicks, sPerfSoundSamples;
static volatile u64 sDrawTicks;   /* the drawing thread's */
static volatile unsigned sDrawCount;

static void DrawMain(void *unused)
{
    (void)unused;
    CatchCrashes();
    for (;;) {
        u64 start;

        LightEvent_Wait(&sDrawGo);
        if (sDrawQuit) {
            break;
        }
        start = svcGetSystemTick();
        ShowGame();
        sDrawTicks += svcGetSystemTick() - start;
        sDrawCount++;
        __atomic_store_n(&sDrawBusy, false, __ATOMIC_RELEASE);
    }
    threadExit(0);
}

static void StartDrawThread(s32 priority)
{
    LightEvent_Init(&sDrawGo, RESET_ONESHOT);
    sDrawThread = threadCreate(DrawMain, NULL, 64 * 1024, priority, 2, false);
}

static void StopDrawThread(void)
{
    if (sDrawThread) {
        sDrawQuit = true;
        LightEvent_Signal(&sDrawGo);
        threadJoin(sDrawThread, UINT64_MAX);
        threadFree(sDrawThread);
        sDrawThread = NULL;
    }
}

/* Before the program draws on the screens itself (the log): wait until the
 * drawing thread is done with them. */
static void WaitForDrawing(void)
{
    if (sDrawThread != NULL && threadGetCurrent() == sDrawThread) {
        return;   /* (the drawing thread itself crashed) */
    }
    while (__atomic_load_n(&sDrawBusy, __ATOMIC_ACQUIRE)) {
        svcSleepThread(500000LL);
    }
}

/* ---------------------------------------------------------------- pacing */

/* The DS's (and the 3DS's) frame: 59.83 a second. */
#define FRAME_TICKS ((u64)(SYSCLOCK_ARM11 / 59.8261))
/* Without the drawing thread, at least one frame in this many is shown. */
#define MAX_SKIPPED 3

static u64 sDeadline;        /* when the current frame should be done */
static u64 sFrameExit;       /* when the last NitroPort_OnFrame returned */
static int sSkipped;

static void PerfFrame(u64 enter, bool shown)
{
    u64 now = svcGetSystemTick();
    double seconds, perFrame;
    unsigned drawn;
    u64 soundTicks, soundSamples;

    if (sPerfStart == 0) {
        sPerfStart = now;
        sPerfSoundTicks = sSoundMixTicks;
        sPerfSoundSamples = sSoundMixSamples;
        sDrawTicks = 0;
        sDrawCount = 0;
        return;
    }
    if (sFrameExit != 0) {
        sPerfGameTicks += enter - sFrameExit;
    }
    sPerfFrames++;
    sPerfShown += shown;
    if (sPerfFrames < 120) {
        return;
    }

    seconds = (double)(now - sPerfStart) / SYSCLOCK_ARM11;
    perFrame = 1000.0 / SYSCLOCK_ARM11 / sPerfFrames;
    drawn = sDrawThread ? sDrawCount : sPerfShown;
    soundTicks = sSoundMixTicks - sPerfSoundTicks;
    soundSamples = sSoundMixSamples - sPerfSoundSamples;
    NitroPort_Log("[perf] %.1f game frames/s, %.1f shown/s; game %.1f ms/frame, "
                  "drawing %.1f ms/screen pair; sound mixing %.0f%% of a core (core %d)%s",
                  sPerfFrames / seconds, (sDrawThread ? sDrawCount : sPerfShown) / seconds,
                  (double)sPerfGameTicks * perFrame,
                  drawn ? (double)(sDrawThread ? sDrawTicks : sPerfDrawTicks) * 1000.0 / SYSCLOCK_ARM11 / drawn : 0.0,
                  soundSamples ? 100.0 * ((double)soundTicks / SYSCLOCK_ARM11) / ((double)soundSamples / 32768.0) : 0.0,
                  sSoundCore, sDrawThread ? ", drawing on core 2" : "");
    sPerfStart = now;
    sPerfGameTicks = sPerfDrawTicks = 0;
    sPerfFrames = sPerfShown = 0;
    sPerfSoundTicks = sSoundMixTicks;
    sPerfSoundSamples = sSoundMixSamples;
    sDrawTicks = 0;
    sDrawCount = 0;
}

/* Once a frame, from the game's idle thread (NitroPort_Halt): show the
 * frame, wait for the 3DS's V-blank, read the buttons. */
void NitroPort_OnFrame(void)
{
    u32 held, down;
    u64 enter = svcGetSystemTick();
    bool shown = false;

    NitroPort_SoundPumpReplies(); /* never raise ARM9 FIFO IRQ from DSP thread */
    sFrame++;
    if (!sWatchdogHasHome && !aptMainLoop()) {
        Quit();   /* closed from the HOME menu */
    }

    hidScanInput();
    held = hidKeysHeld();
    down = hidKeysDown();
    if ((held & (KEY_L | KEY_R)) == (KEY_L | KEY_R) && (down & KEY_SELECT)) {
        ShowLog(!sShowLog);
    }
    Platinum_SetButtons(DsButtons(held));
    if (held & KEY_TOUCH) {
        touchPosition touch;

        hidTouchRead(&touch);
        /* The DS's 256x192 screen sits in the middle of the 320x240 one. */
        Platinum_SetTouch(1, touch.px - 32, touch.py - 24);
    } else {
        Platinum_SetTouch(0, 0, 0);
    }

    if (sFrame % 60 == 0) {
        Status("Running: frame %lu", sFrame);
    }

    if (sScreensDirty) {
        WaitForDrawing();
        sScreensDirty = false;
        if (sShowLog) {
            DrawLogScreens();
        } else {
            /* (the consoles turned double buffering off) */
            gfxSetScreenFormat(GFX_TOP, GSP_RGB565_OES);
            gfxSetScreenFormat(GFX_BOTTOM, GSP_RGB565_OES);
            gfxSetDoubleBuffering(GFX_TOP, true);
            gfxSetDoubleBuffering(GFX_BOTTOM, true);
            ClearScreens();
        }
    }

    if (sShowLog) {
        if (!__atomic_load_n(&sDrawBusy, __ATOMIC_ACQUIRE)) {
            gfxFlushBuffers();
            gfxSwapBuffers();
        }
    } else if (sDrawThread) {
        /* The drawing thread takes this frame, unless it's still on the
         * last one: then this one isn't shown. */
        if (!__atomic_load_n(&sDrawBusy, __ATOMIC_ACQUIRE)) {
            __atomic_store_n(&sDrawBusy, true, __ATOMIC_RELEASE);
            LightEvent_Signal(&sDrawGo);
            shown = true;
        }
    } else if (sDeadline == 0 || enter <= sDeadline || sSkipped >= MAX_SKIPPED) {
        /* On time (or too many left out): draw it. */
        u64 start = svcGetSystemTick();

        ShowGame();
        sPerfDrawTicks += svcGetSystemTick() - start;
        sSkipped = 0;
        shown = true;
    } else {
        sSkipped++;   /* late: leave this one out */
    }

    /* Wait for the V-blank only when ahead: a frame that took too long
     * isn't made to wait for another one as well. When far behind, stop
     * trying to catch up. */
    {
        u64 now = svcGetSystemTick();

        if (sDeadline == 0 || now < sDeadline) {
            gspWaitForVBlank();
            sDeadline = svcGetSystemTick() + FRAME_TICKS;
        } else if (now - sDeadline > 3 * FRAME_TICKS) {
            sDeadline = now + FRAME_TICKS;
        } else {
            sDeadline += FRAME_TICKS;
        }
    }
    PerfFrame(enter, shown);
    sFrameExit = svcGetSystemTick();
}

/* ------------------------------------------------------------- starting */

/* The console's profile stands in for the DS's user settings. */
static void ReadProfile(PlatinumSettings *settings)
{
    u8 language;
    u16 name[0x1C / 2];
    u8 birthday[2];
    int i;

    Platinum_DefaultSettings(settings);
    if (R_FAILED(cfguInit())) {
        return;
    }
    /* The DS's language codes are the 3DS's first six. */
    if (R_SUCCEEDED(CFGU_GetSystemLanguage(&language)) && language <= 5) {
        settings->language = language;
    }
    memset(name, 0, sizeof(name));
    if (R_SUCCEEDED(CFGU_GetConfigInfoBlk2(sizeof(name), 0x000A0000, name)) && name[0] != 0) {
        for (i = 0; i < 10 && name[i] != 0; i++) {
            settings->nickname[i] = name[i];
        }
        settings->nickname[i] = 0;
    }
    if (R_SUCCEEDED(CFGU_GetConfigInfoBlk2(sizeof(birthday), 0x000A0001, birthday)) &&
        birthday[0] >= 1 && birthday[0] <= 12 && birthday[1] >= 1 && birthday[1] <= 31) {
        settings->birthMonth = birthday[0];
        settings->birthDay = birthday[1];
    }
    cfguExit();
}

static void GameThread(void *arg)
{
    (void)arg;
    CatchCrashes();
    Platinum_Run(&sSettings);
}

/* Wait for a request to quit, watching that frames keep coming. */
static void Watch(void)
{
    unsigned long lastFrame = sFrame;
    int quietTicks = 0;

    while (!LightEvent_TryWait(&sQuit)) {
        svcSleepThread(250 * 1000000LL);
        if (sFrame != lastFrame || (!sWatchdogHasHome && !aptIsActive())) {
            /* Frames are coming, or the HOME menu is up (no frames then). */
            lastFrame = sFrame;
            quietTicks = 0;
            continue;
        }
        if (++quietTicks == 20 && !sWatchdogHasHome) {
            sWatchdogHasHome = true;
            WaitForDrawing();
            ShowLog(true);
            if (sScreensDirty) {
                sScreensDirty = false;
                DrawLogScreens();
            }
            NitroPort_Log("The game hasn't shown a frame for 5 seconds (stuck at frame %lu). "
                          "It's probably waiting for a part of the port that isn't done yet: "
                          "see the lines above. Close it from the HOME menu.", lastFrame);
            Status("Stuck at frame %lu (see the log)", lastFrame);
        }
        if (sWatchdogHasHome && !aptMainLoop()) {
            break;
        }
    }
}

int main(int argc, char **argv)
{
    const char *error;
    s32 priority = 0x30;

    (void)argc;
    (void)argv;
    osSetSpeedupEnable(true);   /* New 3DS: 804 MHz and the L2 cache */
    gfxInitDefault();
    gfxSetScreenFormat(GFX_TOP, GSP_RGB565_OES);
    gfxSetScreenFormat(GFX_BOTTOM, GSP_RGB565_OES);
    ClearScreens();
    MakeColorTable();
    LightEvent_Init(&sQuit, RESET_ONESHOT);
    CatchCrashes();
    sScreensDirty = false;
    DrawLogScreens();

    mkdir("sdmc:/3ds", 0777);
    mkdir(DATA_DIR, 0777);
    sLog = fopen(LOG_PATH, "w");

    error = Platinum_OpenPak(PAK_PATH, MANIFEST_PATH);
    if (error != NULL) {
        NitroPort_Log("Can't start: %s.", error);
        NitroPort_Log("Put platinum3ds.pak and manifest.json (made with the builder "
                      "from your own cartridge) in " DATA_DIR "/.");
        NitroPort_Log("Press START to quit.");
        while (aptMainLoop()) {
            hidScanInput();
            if (hidKeysDown() & KEY_START) {
                break;
            }
            gspWaitForVBlank();
        }
    } else {
        ReadProfile(&sSettings);
        sSettings.savePath = SAVE_PATH;
        svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
        /* Core 1 is the system's; the program may use a share of it. */
        APT_SetAppCpuTimeLimit(30);
        {
            bool isNew3ds = false;

            APT_CheckNew3DS(&isNew3ds);
            if (isNew3ds) {
                StartDrawThread(priority);
            }
            NitroPort_Log("[perf] %s 3DS; drawing %s", isNew3ds ? "New" : "Old",
                          sDrawThread ? "on core 2" : "between frames");
        }
        StartSound(priority, 1);
        if (threadCreate(GameThread, NULL, GAME_STACK_SIZE, priority, -2, true) == NULL) {
            NitroPort_Log("Can't start the game's thread.");
        } else {
            Status("Running");
            ShowLog(false);   /* the game's screens from the first frame */
            /* A step above the game's threads, so the watch below runs even
             * if the game never stops to wait. */
            svcSetThreadPriority(CUR_THREAD_HANDLE, priority - 1);
            Watch();
        }
    }

    StopDrawThread();
    StopSound();
    if (sLog != NULL) {
        fclose(sLog);
        sLog = NULL;
    }
    gfxExit();
    return 0;
}
