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
 * Screens, for now (the DS's graphics come with the renderer): the top
 * screen shows the program's status, the bottom one the log.
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
                    NitroPort_SoundRender((int16_t *)buf->data_vaddr, AUDIO_FRAMES);
                    DSP_FlushDataCache(buf->data_vaddr, AUDIO_FRAMES * 4);
                    ndspChnWaveBufAdd(0, buf);
                }
            }
            svcSleepThread(1000000LL);
        } else {
            /* Even without dspfirm.cdc, continue driving Nintendo's ARM7
             * sequencer and releasing its commands from real audio time. */
            NitroPort_SoundRender(silent, AUDIO_FRAMES);
            svcSleepThread(15625000LL);
        }
    }
    threadExit(0);
}

static void StartSound(s32 priority)
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
    sSoundThread = threadCreate(SoundMain, NULL, 64 * 1024, priority - 2, -2, false);
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

/* ---------------------------------------------------------------- logging */

void NitroPort_Log(const char *fmt, ...)
{
    char line[256];
    va_list args;

    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);

    consoleSelect(&sBottom);
    printf("%s\n", line);
    if (sLog != NULL) {
        fprintf(sLog, "[%6lu] %s\n", sFrame, line);
        fflush(sLog);
    }
}

static void Status(const char *fmt, ...)
{
    va_list args;

    consoleSelect(&sTop);
    printf("\x1b[8;0H\x1b[2K");
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
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

/* Leave a message on screen until the player presses START, then quit. */
static void StopWithMessage(void) __attribute__((noreturn));

static void StopWithMessage(void)
{
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

/* Once a frame, from the game's idle thread (NitroPort_Halt): show the
 * frame, wait for the 3DS's V-blank, read the buttons. */
void NitroPort_OnFrame(void)
{
    u32 held;

    NitroPort_SoundPumpReplies(); /* never raise ARM9 FIFO IRQ from DSP thread */
    sFrame++;
    if (!sWatchdogHasHome && !aptMainLoop()) {
        Quit();   /* closed from the HOME menu */
    }

    hidScanInput();
    held = hidKeysHeld();
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

    gfxFlushBuffers();
    gfxSwapBuffers();
    gspWaitForVBlank();
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

static void ClearTopScreen(void)
{
    int i;

    for (i = 0; i < 2; i++) {
        u16 width, height;
        u8 *fb = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, &width, &height);

        memset(fb, 0, (size_t)width * height * 3);
        gfxFlushBuffers();
        gfxSwapBuffers();
    }
}

int main(int argc, char **argv)
{
    const char *error;
    s32 priority = 0x30;

    (void)argc;
    (void)argv;
    gfxInitDefault();
    ClearTopScreen();
    consoleInit(GFX_TOP, &sTop);
    consoleInit(GFX_BOTTOM, &sBottom);
    LightEvent_Init(&sQuit, RESET_ONESHOT);
    CatchCrashes();

    consoleSelect(&sTop);
    printf("\n  Pokemon Platinum - 3DS port\n");
    printf("  The game's own code, running natively.\n\n");
    printf("  No graphics yet: the DS screens come\n");
    printf("  with the renderer. Log: below and in\n");
    printf("  " LOG_PATH "\n");

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
        StartSound(priority);
        if (threadCreate(GameThread, NULL, GAME_STACK_SIZE, priority, -2, true) == NULL) {
            NitroPort_Log("Can't start the game's thread.");
        } else {
            Status("Running");
            /* A step above the game's threads, so the watch below runs even
             * if the game never stops to wait. */
            svcSetThreadPriority(CUR_THREAD_HANDLE, priority - 1);
            Watch();
        }
    }

    StopSound();
    if (sLog != NULL) {
        fclose(sLog);
        sLog = NULL;
    }
    gfxExit();
    return 0;
}
