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
 * Speed: the New 3DS's faster clock is switched on. The game's own code
 * takes little of a frame; drawing the two screens costs much more. So
 * drawing has threads of its own that work while the game goes on with its
 * next frame (as the DS draws while the game runs): on a New 3DS the screen
 * of engine A (the one with the 3D, whichever screen that is) on core 2 and
 * the other on core 1 (or core 0), otherwise both on core 0.
 * On core 0 they run below the game: the game always comes first and
 * keeps its speed, and a screen that isn't done when the next frame is
 * ready leaves that frame out. The 3D's GPU thread (drawing each 3D frame
 * with the 3DS's GPU) is on core 0 too, below the game, so engine A's
 * drawer has core 2 to itself; while the 3D keeps changing, that screen is
 * drawn once for each new 3D frame. Sound has a thread of its own on core 0,
 * above the game, as it must keep up with the speaker. The DS's geometry
 * engine (vertices, lighting, clipping: most of a 3D frame's work) has a
 * thread on core 1 (of which a program gets a share), so the game keeps
 * its speed, and the music its sync, whatever the 3D costs. Every two
 * seconds a [perf] line in the log says where the time went.
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
#include "port_pica.h"
#include <nitro_port_gx3d.h>
#include <nitro_port_render2d.h>
#include <nitro_port_wide.h>

/* The 2D renderer (nitro_port.h, which can't be included next to 3ds.h). */
void NitroPort_Render2D(int engine, uint16_t *out);
int NitroPort_Render2DIfChanged(int engine, uint16_t *out);
int NitroPort_EngineAOnTop(void);

/* Audio uses the port's plain-C mixer; it deliberately does not include
 * NitroSDK headers alongside libctru. */
void NitroPort_Log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void NitroPort_SoundRender(int16_t *stereo, size_t frames);
void NitroPort_SoundPumpReplies(void);

#define AUDIO_FRAMES 512
#define AUDIO_BUFFERS 4
static Thread sSoundThread;
static bool sSoundThreadRunning;
static bool sNdspAvailable;
static int16_t *sSoundSamples;
static ndspWaveBuf sSoundBuf[AUDIO_BUFFERS];
/* Time spent mixing, and sound mixed (in samples), for the [perf] line. */
static volatile u64 sSoundMixTicks;
static volatile u64 sSoundMixSamples;
/* Times the speaker ran out of sound: every buffer had been played by the
 * time the sound thread looked (a click or gap). */
static volatile unsigned sSoundUnderruns;

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
            static bool started;
            int done = 0;

            for (i = 0; i < AUDIO_BUFFERS; ++i) {
                done += sSoundBuf[i].status == NDSP_WBUF_DONE;
            }
            if (started && done == AUDIO_BUFFERS) {
                sSoundUnderruns++;
            }
            started = true;
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
    /* On the game's core (0), ahead of the game's threads, as it must keep
     * up with the speaker. (Core 1, the system's, gives a program only a
     * share of its time, and too little: in Azahar sound fell behind.) */
    sSoundThread = threadCreate(SoundMain, NULL, 64 * 1024, priority - 2, 0, false);
    if (!sSoundThread) {
        __atomic_store_n(&sSoundThreadRunning, false, __ATOMIC_RELEASE);
        NitroPort_Log("[audio] couldn't start independent audio thread");
    }
}

static void StopSound(void)
{
    if (sSoundThread) {
        __atomic_store_n(&sSoundThreadRunning, false, __ATOMIC_RELEASE);
        if (R_SUCCEEDED(threadJoin(sSoundThread, 1000000000ULL))) {
            threadFree(sSoundThread);
        }
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

/* ------------------------------------------------------------ the display */
/* Two ways of showing the game's screens (docs/REMASTER.md):
 *  - the classic one: the 2D renderer draws each screen whole, 256x192,
 *    with the 3D (drawn by the GPU, read back) composited in, and the
 *    drawing thread copies it into the middle of the 3DS's screen;
 *  - the remaster (the default on a New 3DS): the GPU puts each screen
 *    together (port_compose_pica.c) from the 2D layers behind and in front
 *    of the 3D, upscaled with a pixel-art filter, with the 3D drawn
 *    straight into the screen at its resolution, over the upper screen's
 *    whole width, and with depth from the 3D slider. The display thread
 *    (below) composes a frame whenever something changed.
 * sdmc:/3ds/pokeplatinum/remaster.txt chooses (ReadRemasterConfig). */
static bool sRemaster;
static ComposeConfig sCompose;
static volatile bool sComposing;      /* the display thread is inside a frame */
static volatile bool sComposeForce;   /* draw everything again (back from the log) */
/* The HOME menu (or another applet) has the screens and the GPU: nothing
 * may draw (OnAptEvent). Shared by both display paths. */
static volatile bool sSuspended;

#define REMASTER_PATH "sdmc:/3ds/pokeplatinum/remaster.txt"

/* remaster.txt: a setting a line, "name value": remaster on/off, wide
 * on/off, filter sharp/nearest/smooth/scale2x (on: sharp, off: nearest),
 * stereo on/off/<strength, e.g. 0.02>, convergence <distance> (0: from
 * each scene), edges on/off, flip on/off. */
static void ReadRemasterConfig(bool new3ds)
{
    FILE *f = fopen(REMASTER_PATH, "r");
    char line[64];

    sRemaster = new3ds;
    memset(&sCompose, 0, sizeof(sCompose));
    sCompose.wide = 1;
    sCompose.filter = COMPOSE_FILTER_SHARP;
    sCompose.stereo = 1;
    sCompose.stereoStrength = 0.02f;
    sCompose.convergence = 0.0f;
    sCompose.edges = 1;
    sCompose.flipV = 0;
    if (f == NULL) {
        return;
    }
    while (fgets(line, sizeof(line), f) != NULL) {
        char name[16], value[32];

        if (sscanf(line, "%15s %31s", name, value) != 2) {
            continue;
        }
        {
            bool on = strcmp(value, "on") == 0 || strcmp(value, "yes") == 0 || strcmp(value, "1") == 0;
            bool off = strcmp(value, "off") == 0 || strcmp(value, "no") == 0 || strcmp(value, "0") == 0;

            if (strcmp(name, "remaster") == 0) {
                sRemaster = on;
            } else if (strcmp(name, "wide") == 0) {
                sCompose.wide = !off;
            } else if (strcmp(name, "filter") == 0) {
                if (strcmp(value, "nearest") == 0 || off) {
                    sCompose.filter = COMPOSE_FILTER_NEAREST;
                } else if (strcmp(value, "smooth") == 0 || strcmp(value, "bilinear") == 0) {
                    sCompose.filter = COMPOSE_FILTER_SMOOTH;
                } else if (strcmp(value, "scale2x") == 0) {
                    sCompose.filter = COMPOSE_FILTER_SCALE2X;
                } else {
                    sCompose.filter = COMPOSE_FILTER_SHARP;
                }
            } else if (strcmp(name, "stereo") == 0) {
                sCompose.stereo = !off;
                if (!on && !off) {
                    sCompose.stereoStrength = (float)atof(value);
                    sCompose.stereo = sCompose.stereoStrength > 0.0f;
                }
            } else if (strcmp(name, "convergence") == 0) {
                sCompose.convergence = (float)atof(value);
            } else if (strcmp(name, "edges") == 0) {
                sCompose.edges = !off;
            } else if (strcmp(name, "flip") == 0) {
                sCompose.flipV = on;
            }
        }
    }
    fclose(f);
}

/* -------------------------------------------------------------- 3D worker */
static void CatchCrashes(void); /* crash handler is defined below */
/* The PICA backend and all citro3d commands run on one dedicated thread.
 * Neither the game nor the 2D screen drawers wait for GPU completion. In
 * the remaster it's the display thread too: it composes the screens. */
static void DrawersImageHandedOver(void); /* (the drawing threads are below) */
static Thread sGpuThread;
static LightEvent sGpuWake, sGpuStarted;
static volatile bool sGpuRunning, sGpuWakeReady;
static int sGpuCore;
static volatile u64 sGpuTicks;
static volatile unsigned sGpuScenes, sGpuTriangles, sGpuDroppedTriangles;
static volatile bool sAOnTop;    /* engine A on the upper screen, this frame */
static volatile bool sShowLog = true;      /* the log instead of the game */
static volatile bool sScreensDirty = true; /* redraw the borders / consoles */

/* Override the port's weak callback to notify, not copy, each SWAP scene. */
void NitroPort_Gx3dScenePublished(void)
{
    if(__atomic_load_n(&sGpuWakeReady,__ATOMIC_ACQUIRE))
        LightEvent_Signal(&sGpuWake);
}
/* The game's time for a 3D scene (at its SWAP) and a 2D picture (when
 * the drawers read it): the V-blank count. The compositor pairs them and
 * shows each frame at its time. */
uint32_t NitroPort_Gx3dStamp(void)
{
    return NitroPort_ComposeVBlanks();
}
/* (the remaster) The V-blank thread: each V-blank, the frames due go onto
 * the LCD (NitroPort_ComposeVBlank), and the display thread is woken (it
 * may have a frame to draw now that a framebuffer is free). A thread of
 * its own rather than libctru's V-blank callback: citro3d takes that
 * callback back whenever the program comes back from the HOME menu. */
static Thread sVBlankThread;
static volatile bool sVBlankRunning;
static void VBlankMain(void *unused)
{
    (void)unused;
    while(__atomic_load_n(&sVBlankRunning,__ATOMIC_ACQUIRE)){
        gspWaitForVBlank();
        NitroPort_ComposeVBlank();
        if(__atomic_load_n(&sGpuWakeReady,__ATOMIC_ACQUIRE))
            LightEvent_Signal(&sGpuWake);
    }
    threadExit(0);
}
static void StartVBlankThread(s32 priority)
{
    __atomic_store_n(&sVBlankRunning,true,__ATOMIC_RELEASE);
    /* Core 0, above everything else there: it does almost nothing, at once. */
    sVBlankThread=threadCreate(VBlankMain,NULL,16*1024,priority-3,0,false);
    if(!sVBlankThread){
        __atomic_store_n(&sVBlankRunning,false,__ATOMIC_RELEASE);
        NitroPort_Log("[compose] couldn't start the V-blank thread");
    }
}
static void StopVBlankThread(void)
{
    if(!sVBlankThread)return;
    __atomic_store_n(&sVBlankRunning,false,__ATOMIC_RELEASE);
    if(R_SUCCEEDED(threadJoin(sVBlankThread,1000000000ULL)))threadFree(sVBlankThread);
    sVBlankThread=NULL;
}
/* The classic path: each scene drawn to an image as it comes. */
static void GpuClassicLoop(void)
{
    uint32_t seen=0;
    while(__atomic_load_n(&sGpuRunning,__ATOMIC_ACQUIRE)){
        const NitroGx3dScene *scene;
        LightEvent_Wait(&sGpuWake);
        if(!__atomic_load_n(&sGpuRunning,__ATOMIC_ACQUIRE))break;
        for(;;){
            u64 start;
            unsigned dropped=0;
            int drawn;
            /* Busy first, then the check (OnAptEvent: suspended first,
             * then waits for not busy): never drawing while suspended. */
            __atomic_store_n(&sComposing,true,__ATOMIC_SEQ_CST);
            if(__atomic_load_n(&sSuspended,__ATOMIC_SEQ_CST)||
               (scene=NitroPort_Gx3dAcquireScene(&seen))==NULL){
                __atomic_store_n(&sComposing,false,__ATOMIC_RELEASE);
                break;
            }
            start=svcGetSystemTick();
            drawn=NitroPort_PicaRender(scene,&dropped);
            NitroPort_Gx3dReleaseScene(scene);
            __atomic_store_n(&sComposing,false,__ATOMIC_RELEASE);
            if(drawn>=0)DrawersImageHandedOver();
            sGpuTicks += svcGetSystemTick()-start; /* sole writer, as with audio */
            __atomic_fetch_add(&sGpuScenes,1u,__ATOMIC_RELAXED);
            if(drawn>=0)__atomic_fetch_add(&sGpuTriangles,
                                          (unsigned)drawn,__ATOMIC_RELAXED);
            __atomic_fetch_add(&sGpuDroppedTriangles,dropped,__ATOMIC_RELAXED);
            if(!__atomic_load_n(&sGpuRunning,__ATOMIC_ACQUIRE))break;
        }
    }
}
/* The remaster: the newest scene is kept (drawn again whenever the
 * screen is composed again) until a newer one is out; a frame is composed
 * when something changed and the framebuffer is free. */
static void GpuComposeLoop(void)
{
    const NitroGx3dScene *scene=NULL;
    uint32_t seen=0;
    while(__atomic_load_n(&sGpuRunning,__ATOMIC_ACQUIRE)){
        bool force;
        LightEvent_Wait(&sGpuWake);
        if(!__atomic_load_n(&sGpuRunning,__ATOMIC_ACQUIRE))break;
        if(scene==NULL || NitroPort_Gx3dNewerScene(seen)){
            const NitroGx3dScene *next;
            if(scene){NitroPort_Gx3dReleaseScene(scene);scene=NULL;}
            next=NitroPort_Gx3dAcquireScene(&seen);
            if(next){
                scene=next;
                __atomic_fetch_add(&sGpuScenes,1u,__ATOMIC_RELAXED);
                __atomic_fetch_add(&sGpuTriangles,scene->count,__ATOMIC_RELAXED);
                __atomic_fetch_add(&sGpuDroppedTriangles,scene->dropped,__ATOMIC_RELAXED);
            }
        }
        /* Not while the log has the screens (WaitForDrawing, which the
         * switch to the log waits on, sees sComposing first). */
        __atomic_store_n(&sComposing,true,__ATOMIC_SEQ_CST);
        if(sShowLog||sScreensDirty||__atomic_load_n(&sSuspended,__ATOMIC_SEQ_CST)){
            __atomic_store_n(&sComposing,false,__ATOMIC_RELEASE);
            continue;
        }
        force=__atomic_exchange_n(&sComposeForce,false,__ATOMIC_ACQ_REL);
        {
            u64 start=svcGetSystemTick();
            NitroPort_ComposeFrame(scene,seen,sAOnTop,osGet3DSliderState(),force);
            __atomic_store_n(&sComposing,false,__ATOMIC_RELEASE);
            sGpuTicks+=svcGetSystemTick()-start;
        }
    }
    if(scene)NitroPort_Gx3dReleaseScene(scene);
}
static void GpuMain(void *unused)
{
    (void)unused;
    CatchCrashes();
    if(!NitroPort_PicaInit()){
        NitroPort_Log("[gx3d] citro3d init failed; 3D BG0 stays empty");
        sRemaster=false;
        LightEvent_Signal(&sGpuStarted);
        threadExit(0);
    }
    if(sRemaster){
        if(!NitroPort_ComposeInit(&sCompose)){
            NitroPort_Log("[compose] couldn't set the GPU display up: the classic display instead");
            sRemaster=false;
        }else{
            s32 priority=0x30;
            svcGetThreadPriority(&priority,CUR_THREAD_HANDLE);
            StartVBlankThread(priority-1);   /* (this thread is the game's priority + 1) */
        }
    }
    LightEvent_Signal(&sGpuStarted);
    if(sRemaster)GpuComposeLoop();
    else GpuClassicLoop();
    if(sRemaster){
        StopVBlankThread();
        NitroPort_ComposeExit();
    }
    NitroPort_PicaExit();
    threadExit(0);
}
static void StartGpu(s32 priority,bool new3ds)
{
    (void)new3ds;
    sGpuCore=0;
    LightEvent_Init(&sGpuWake,RESET_ONESHOT);
    LightEvent_Init(&sGpuStarted,RESET_ONESHOT);
    __atomic_store_n(&sGpuRunning,true,__ATOMIC_RELEASE);
    __atomic_store_n(&sGpuWakeReady,true,__ATOMIC_RELEASE);
    /* Core 0, below the game (which needs little of it now the geometry
     * has a thread of its own) and, on a New 3DS, above engine B's drawer
     * there. Not core 2: engine A's drawer, whose screen shows the 3D,
     * needs all of it, and shared with this it fell behind the 3D (fewer,
     * uneven 3D frames: a camera pan jittered). */
    sGpuThread=threadCreate(GpuMain,NULL,128*1024,priority+1,sGpuCore,false);
    if(!sGpuThread){
        __atomic_store_n(&sGpuWakeReady,false,__ATOMIC_RELEASE);
        __atomic_store_n(&sGpuRunning,false,__ATOMIC_RELEASE);
        sRemaster=false;
        NitroPort_Log("[gx3d] couldn't start 3D worker thread");
        return;
    }
    /* Until it's set up: whether the remaster display is on decides how
     * the drawing threads work. */
    LightEvent_Wait(&sGpuStarted);
}
static void StopGpu(void)
{
    if(!sGpuThread)return;
    __atomic_store_n(&sGpuRunning,false,__ATOMIC_RELEASE);
    LightEvent_Signal(&sGpuWake);
    /* (At most 2 seconds: a thread stuck on the GPU mustn't keep the
     * program from closing. Left running then; quitting ends it.) */
    if(R_SUCCEEDED(threadJoin(sGpuThread,2000000000ULL))){
        threadFree(sGpuThread);
    }else{
        NitroPort_Log("[gx3d] the 3D thread didn't stop: quitting without it");
    }
    sGpuThread=NULL;
    __atomic_store_n(&sGpuWakeReady,false,__ATOMIC_RELEASE);
}

#define DATA_DIR      "sdmc:/3ds/pokeplatinum"
#define PAK_PATH      DATA_DIR "/platinum3ds.pak"
#define MANIFEST_PATH DATA_DIR "/manifest.json"
#define LOG_PATH      DATA_DIR "/log.txt"
#define SAVE_PATH     DATA_DIR "/platinum.sav"
/* Optional, for trying where the geometry goes: "core 1" (the default),
 * "core 2", or "off" (the game's own thread, as before). */
#define GEOMETRY_PATH DATA_DIR "/geometry.txt"

/* ------------------------------------------------------ geometry thread */
/* The DS geometry engine (vertices, lighting, clipping: most of a 3D
 * frame's work) on a core of its own, so the game's thread, and with it
 * the game's speed against the music, doesn't carry it. Core 1, the
 * system's, of which a program may take a share (APT_SetAppCpuTimeLimit);
 * otherwise the New 3DS's core 2. When it falls behind, scenes are left
 * out rather than the game slowed (port_gx3d.c). */
static Thread sGeoThread;
static LightEvent sGeoWorkerEvent, sGeoProducerEvent;
static volatile int sGeoRunning;
static int sGeoCore = -1;
static u32 sGeoCpuLimit;

static void GeoWaitWorker(void *unused) { (void)unused; LightEvent_Wait(&sGeoWorkerEvent); }
static void GeoWakeWorker(void *unused) { (void)unused; LightEvent_Signal(&sGeoWorkerEvent); }
static void GeoWaitProducer(void *unused) { (void)unused; LightEvent_Wait(&sGeoProducerEvent); }
static void GeoWakeProducer(void *unused) { (void)unused; LightEvent_Signal(&sGeoProducerEvent); }
static uint64_t GeoTicks(void *unused) { (void)unused; return svcGetSystemTick(); }

static void GeoMain(void *unused)
{
    (void)unused;
    CatchCrashes();
    NitroPort_Gx3dThreadMain(&sGeoRunning);
    threadExit(0);
}

static void GeometryOps(void)
{
    NitroGx3dThreadOps ops;

    memset(&ops, 0, sizeof(ops));
    ops.wait_worker = GeoWaitWorker;
    ops.wake_worker = GeoWakeWorker;
    ops.wait_producer = GeoWaitProducer;
    ops.wake_producer = GeoWakeProducer;
    ops.ticks = GeoTicks;
    LightEvent_Init(&sGeoWorkerEvent, RESET_ONESHOT);
    LightEvent_Init(&sGeoProducerEvent, RESET_ONESHOT);
    sGeoRunning = 1;
    /* Before the thread runs: it needs the ops. Nothing has been drawn yet,
     * so if it can't start, the engine goes back to the game's thread. */
    NitroPort_Gx3dStartThread(&ops);
}

/* Core 2 (New 3DS), below its drawer: its screen shows the 3D. */
static void StartGeometryOn(s32 priority, int core)
{
    GeometryOps();
    sGeoThread = threadCreate(GeoMain, NULL, 64 * 1024, priority + 1, core, false);
    sGeoCore = sGeoThread ? core : -1;
    if (!sGeoThread) {
        NitroPort_Gx3dStopThread();
        NitroPort_Log("[perf] geometry on the game's thread (no core %d)", core);
        return;
    }
    NitroPort_Log("[perf] geometry on core %d", core);
}

static void StartGeometry(s32 priority, bool new3ds)
{
    static const u32 limits[] = { 80, 60, 40, 30 };
    char choice[16] = "";
    FILE *f = fopen(GEOMETRY_PATH, "r");
    size_t i;

    if (f != NULL) {
        if (fgets(choice, sizeof(choice), f) == NULL) {
            choice[0] = 0;
        }
        fclose(f);
    }
    if (strncmp(choice, "off", 3) == 0) {
        NitroPort_Log("[perf] geometry on the game's thread (" GEOMETRY_PATH ")");
        return;
    }
    /* The share of core 1 first, whichever core the geometry goes on:
     * engine B's drawer goes there with it (StartDrawers). */
    for (i = 0; i < sizeof(limits) / sizeof(limits[0]); i++) {
        if (R_SUCCEEDED(APT_SetAppCpuTimeLimit(limits[i]))) {
            sGeoCpuLimit = limits[i];
            break;
        }
    }
    if (strncmp(choice, "core 2", 6) == 0 && new3ds) {
        NitroPort_Log("[perf] geometry on core 2, as " GEOMETRY_PATH " says");
        StartGeometryOn(priority, 2);
        return;
    }
    if (sGeoCpuLimit == 0) {
        if (new3ds) {
            StartGeometryOn(priority, 2);
        } else {
            NitroPort_Log("[perf] geometry on the game's thread (no share of core 1)");
        }
        return;
    }
    GeometryOps();
    sGeoThread = threadCreate(GeoMain, NULL, 64 * 1024, priority - 1, 1, false);
    if (!sGeoThread) {
        NitroPort_Gx3dStopThread();
        if (new3ds) {
            StartGeometryOn(priority, 2);
        } else {
            NitroPort_Log("[perf] geometry on the game's thread (no thread on core 1)");
        }
        return;
    }
    sGeoCore = 1;
    NitroPort_Log("[perf] geometry on core 1 (%lu%% of it)", (unsigned long)sGeoCpuLimit);
}

/* ------------------------------------------------------- New 3DS speed-up */
/* 804 MHz and the L2 cache (otherwise 268 MHz, as an Old 3DS). libctru's
 * osSetSpeedupEnable does it through the ptm:sysm service, and quietly
 * nothing if the program can't use that service. The core speed check
 * (log 4d7b483a) timed jobs at about the speed of 268 MHz, so it's done
 * here again with each step's result kept for the log. libctru still does
 * it again when the program comes back from the HOME menu. */
static Result sSpeedupService = -1, sSpeedupSet = -1;

static void EnableSpeedup(void)
{
    osSetSpeedupEnable(true);
    sSpeedupService = ptmSysmInit();
    if (R_SUCCEEDED(sSpeedupService)) {
        sSpeedupSet = PTMSYSM_ConfigureNew3DSCPU(3);
        ptmSysmExit();
    }
}

static void LogSpeedup(bool new3ds)
{
    if (!new3ds) {
        return;
    }
    if (R_FAILED(sSpeedupService)) {
        NitroPort_Log("[perf] New 3DS speed-up (804 MHz, L2 cache): OFF, the ptm:sysm service isn't available to the program (%08lx)",
                      (unsigned long)sSpeedupService);
    } else if (R_FAILED(sSpeedupSet)) {
        NitroPort_Log("[perf] New 3DS speed-up (804 MHz, L2 cache): OFF, ptm:sysm refused it (%08lx)",
                      (unsigned long)sSpeedupSet);
    } else {
        NitroPort_Log("[perf] New 3DS speed-up (804 MHz, L2 cache): asked for, and accepted");
    }
}

/* ------------------------------------------------------ core speed check */
/* The same small jobs timed on each core the program uses, once at start
 * (a [perf] line): plain integer work, 64-bit multiplies (the geometry's
 * fixed point) and double divisions (its screen positions). The geometry
 * thread's scenes took several times longer on the 3DS than its work
 * suggests; this says whether a core (core 1 is shared with the system)
 * or a kind of work is the slow one. */
typedef struct SpeedJob {
    int kind;        /* 0 integer, 1 64-bit multiply, 2 double divide */
    u64 ticks;
    volatile u32 sink;
    volatile double start;   /* 1.0 */
} SpeedJob;

static void SpeedJobMain(void *arg)
{
    SpeedJob *job = arg;
    u64 start = svcGetSystemTick();
    u32 i;

    if (job->kind == 0) {
        u32 x = 12345;

        for (i = 0; i < 4000000u; i++) {
            x = x * 1103515245u + 12345u;
            x ^= x >> 7;
        }
        job->sink = x;
    } else if (job->kind == 1) {
        s64 acc = 1;
        s32 a = 4096, b = -3000;

        for (i = 0; i < 2000000u; i++) {
            acc += (s64)a * b;
            a = (s32)(acc >> 12) | 1;
            b += 7;
        }
        job->sink = (u32)acc;
    } else {
        double d = job->start;   /* (read at run time: not worked out by the compiler) */

        for (i = 0; i < 300000u; i++) {
            d = (d + 1.5) / (d * 0.5 + 2.0);
        }
        job->sink = (u32)(d * 1000.0);
    }
    job->ticks = svcGetSystemTick() - start;
    threadExit(0);
}

/* The three jobs on one core, at the geometry thread's priority; the
 * milliseconds each took into ms[], or -1 if no thread could start. */
static void TimeCore(int core, s32 priority, double ms[3])
{
    int kind;

    for (kind = 0; kind < 3; kind++) {
        static SpeedJob job;
        Thread t;

        memset(&job, 0, sizeof(job));
        job.kind = kind;
        job.start = 1.0;
        t = threadCreate(SpeedJobMain, &job, 16 * 1024, priority, core, false);
        if (t == NULL) {
            ms[kind] = -1.0;
            continue;
        }
        threadJoin(t, UINT64_MAX);
        threadFree(t);
        ms[kind] = (double)job.ticks * 1000.0 / SYSCLOCK_ARM11;
    }
}

static void CheckCoreSpeed(s32 priority, bool new3ds)
{
    double ms[3][3];
    int core, cores = new3ds ? 3 : 2;
    char line[256];
    int used = 0;

    for (core = 0; core < cores; core++) {
        TimeCore(core, priority - 1, ms[core]);
        used += snprintf(line + used, sizeof(line) - (size_t)used, "%score %d: %.1f / %.1f / %.1f",
                         core ? "; " : "", core, ms[core][0], ms[core][1], ms[core][2]);
        if (used >= (int)sizeof(line)) {
            break;
        }
    }
    LogSpeedup(new3ds);
    /* The integer job is 4M rounds of about 7 cycles: ~35 ms at 804 MHz,
     * ~105 ms at 268 MHz (core 1 a quarter more: its share is 80%). */
    NitroPort_Log("[perf] core speed, ms for the same jobs (integer / 64-bit multiply / double divide; integer ~35 ms at 804 MHz, ~105 at 268): %s", line);
}

static void StopGeometry(void)
{
    if (!sGeoThread) {
        return;
    }
    sGeoRunning = 0;
    LightEvent_Signal(&sGeoWorkerEvent);
    /* The game's thread may still be sending it work: don't wait long. */
    if (R_SUCCEEDED(threadJoin(sGeoThread, 200000000ULL))) {
        threadFree(sGeoThread);
    }
    sGeoThread = NULL;
}

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
/* (declared above) sShowLog: the log instead of the game; sScreensDirty:
 * redraw the borders / consoles. */

/* ---------------------------------------------------------------- logging */

/* The last lines, to show again when switching to the log. */
#define KEEP_LINES 24
static char sLines[KEEP_LINES][128];
static int sLineCount;
static char sStatus[64] = "Starting";
/* Lines come from several threads (the game's, the geometry thread's). */
static LightLock sLogLock;

void NitroPort_Log(const char *fmt, ...)
{
    char line[640];
    va_list args;

    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);

    LightLock_Lock(&sLogLock);
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
    LightLock_Unlock(&sLogLock);
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
    int x, y, y0;

    /* The framebuffer is in columns. Eight lines at a time: the eight
     * source lines stay in the cache while their columns are written. */
    for (y0 = 0; y0 < 192; y0 += 8) {
        for (x = 0; x < 256; x++) {
            u16 *column = fb + (size_t)(left + x) * 240 + (239 - 24) - y0;
            const u16 *src = ds + y0 * 256 + x;

            for (y = 0; y < 8; y++) {
                column[-y] = sToRgb565[src[y * 256] & 0x7FFF];
            }
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

/* -------------------------------------------------------- drawing threads */

/*
 * Each drawing thread draws one or both screens when the game hands it a
 * frame, then shows them (each screen is double buffered: the one drawn is
 * shown from the next V-blank). It reads the game's display state (the
 * registers, palettes, OAM and VRAM) as it is, as the DS's display does.
 *
 * Right after V-blank the game changes that state for its next picture
 * (screen fades, OAM, palettes, VRAM transfers). On the DS that happens
 * before the screen starts drawing. Here a screen must not be drawn in the
 * middle of it: at the start of each move a battle puts a copy of the
 * Pokemon on screen as a sprite, and gives it the Pokemon's colours in the
 * same V-blank; drawn in between, the Pokemon showed up grey for a frame.
 * So the game hands the drawers a frame once that work is done
 * (NitroPort_VBlankWorkDone), and before the next V-blank's work it waits
 * for a drawer on another core that's still reading (NitroPort_OnFrame).
 */
typedef struct Drawer {
    Thread thread;
    LightEvent go;
    int screens;             /* bit 0: engine A's screen (3D), bit 1: engine B's */
    int core;
    s32 priority;            /* while it shows what it drew */
    s32 readPriority;        /* while it reads the game's display state */
    volatile bool busy;      /* drawing a frame */
    volatile bool reading;   /* reading the game's display state for it */
    volatile bool aOnTop;    /* engine A on the upper screen, for this frame */
    volatile u32 stamp;      /* the V-blank count when it was handed this frame */
    volatile bool quit;
    u32 images;              /* 3D images out when it was handed its last frame */
    volatile u64 ticks;      /* time spent drawing, for [perf] */
    volatile unsigned count; /* frames handled */
    volatile unsigned drawn; /* screens drawn (not unchanged) */
    volatile u64 convertTicks;   /* converting 3D images between frames */
    volatile unsigned converted; /* 3D images converted between frames */
} Drawer;

/* Drawers go by engine, not by screen: engine A (with the 3D, the costly
 * one) gets the New 3DS's second core whichever screen it's on. */
#define ENGINE_A 1
#define ENGINE_B 2

static Drawer sDrawers[2];
static int sDrawerCount;
static volatile bool sDrawersStarted;   /* (for the GPU thread) */
/* The screens were cleared (back from the log): draw them even if the
 * game's picture didn't change. 0: upper, 1: lower. */
static volatile bool sMustDraw[2];

/* Each screen's picture, in the DS's colours. */
static u16 sPixels[2][256 * 192];   /* 0: upper, 1: lower */

/* Engine A shows on the upper screen when POWCNT1 says so. */
static bool OnTop(int engine, bool aOnTop)
{
    return (engine == 0) == aOnTop;
}

/* What an engine shows, from the game's display state, into its screen's
 * picture. False if it didn't change: that screen keeps showing what it
 * does (not drawn, not swapped). */
static bool RenderScreen(int engine, bool aOnTop, u32 stamp)
{
    bool top = OnTop(engine, aOnTop);
    u16 *ds = sPixels[top ? 0 : 1];

    if (sRemaster) {
        /* The remaster: the picture in two layers, into the compositor's
         * buffers for the engine; published (upscaled) by ShowScreen. */
        ComposeLayers *layers = NitroPort_ComposeLayersOf(engine);
        NitroRender2DSplit split;
        int drawn;

        memset(&split, 0, sizeof(split));
        split.over = layers->over;
        split.overAlpha = layers->overAlpha;
        /* The upper screen: the wider canvas (port_wide.c's layout). */
        if (top && sCompose.wide) {
            NitroPort_WideLayout(engine, &split);
        }
        if (sMustDraw[top ? 0 : 1]) {
            sMustDraw[top ? 0 : 1] = false;
            NitroPort_Render2DSplit(engine, layers->under, &split);
            drawn = 1;
        } else {
            drawn = NitroPort_Render2DSplitIfChanged(engine, layers->under, &split);
        }
        if (!drawn) {
            return false;
        }
        layers->has3d = split.has3d && layers->over != NULL;
        layers->scroll3d = split.scroll3d;
        layers->tint3d = split.tint3d;
        layers->eva = split.eva;
        layers->evb = split.evb;
        layers->evy = split.evy;
        layers->masterBright = split.masterBright;
        layers->backdrop = split.backdrop;
        layers->width = split.width > 256 ? split.width : 256;
        layers->stamp = stamp;
        layers->displayOff = 0;
        return true;
    }
    if (sMustDraw[top ? 0 : 1]) {
        sMustDraw[top ? 0 : 1] = false;
        NitroPort_Render2D(engine, ds);
        return true;
    }
    return NitroPort_Render2DIfChanged(engine, ds) != 0;
}

/* The picture onto its screen, shown from the next V-blank. */
static void ShowScreen(int engine, bool aOnTop)
{
    bool top = OnTop(engine, aOnTop);
    gfxScreen_t screen = top ? GFX_TOP : GFX_BOTTOM;
    u16 width, height;
    u8 *fb;

    if (sRemaster) {
        /* The layers upscaled into a texture, for the display thread. */
        NitroPort_ComposePublish(engine, top ? 0 : 1);
        if (__atomic_load_n(&sGpuWakeReady, __ATOMIC_ACQUIRE)) {
            LightEvent_Signal(&sGpuWake);
        }
        return;
    }
    Blit(screen, sPixels[top ? 0 : 1], top ? (400 - 256) / 2 : (320 - 256) / 2);
    fb = gfxGetFramebuffer(screen, GFX_LEFT, &width, &height);
    GSPGPU_FlushDataCache(fb, (u32)width * height * 2);
    gfxScreenSwapBuffers(screen, false);
}

static void DrawerMain(void *arg)
{
    Drawer *d = arg;

    CatchCrashes();
    for (;;) {
        u64 start;
        bool drawA = false, drawB = false;

        LightEvent_Wait(&d->go);
        if (d->quit) {
            break;
        }
        if (!__atomic_load_n(&d->busy, __ATOMIC_ACQUIRE)) {
            /* No frame: woken because a 3D image was handed over
             * (DrawersImageHandedOver). Converted now, it's ready when the
             * next frame comes, and the game doesn't wait for that. */
            u64 convertStart = svcGetSystemTick();

            if (!sRemaster && NitroPort_Gx3dConvertPending()) {
                d->convertTicks += svcGetSystemTick() - convertStart;
                d->converted++;
            }
            continue;
        }
        start = svcGetSystemTick();
        /* First everything that reads the game's display state, then the
         * rest: the game waits for the reading only (NitroPort_OnFrame). */
        if (d->screens & ENGINE_A) {
            drawA = RenderScreen(0, d->aOnTop, d->stamp);
        }
        if (d->screens & ENGINE_B) {
            drawB = RenderScreen(1, d->aOnTop, d->stamp);
        }
        __atomic_store_n(&d->reading, false, __ATOMIC_RELEASE);
        /* Below the threads it was above while reading, to show what it
         * drew; nothing to show (an unchanged screen): no need, and it
         * would only wait behind them to come back up. */
        if (d->readPriority != d->priority && (drawA || drawB)) {
            svcSetThreadPriority(CUR_THREAD_HANDLE, d->priority);
        }
        if (drawA) {
            ShowScreen(0, d->aOnTop);
        }
        if (drawB) {
            ShowScreen(1, d->aOnTop);
        }
        d->drawn += (unsigned)drawA + (unsigned)drawB;
        d->ticks += svcGetSystemTick() - start;
        d->count++;
        /* Back up before it can be handed the next frame: it wakes up
         * reading. */
        if (d->readPriority != d->priority && (drawA || drawB)) {
            svcSetThreadPriority(CUR_THREAD_HANDLE, d->readPriority);
        }
        __atomic_store_n(&d->busy, false, __ATOMIC_RELEASE);
    }
    threadExit(0);
}

/* (The 3D's GPU thread) a 3D image was handed over: the drawer of engine
 * A's screen converts it right away if it has a core of its own (and if
 * it's drawing a frame, once that's done). On core 0 it would take the
 * game's time, for images that may never be shown: it converts when it
 * draws, as before. */
static void DrawersImageHandedOver(void)
{
    int i;

    if (!__atomic_load_n(&sDrawersStarted, __ATOMIC_ACQUIRE)) {
        return;
    }
    for (i = 0; i < sDrawerCount; i++) {
        Drawer *d = &sDrawers[i];

        if ((d->screens & ENGINE_A) && d->core != 0) {
            LightEvent_Signal(&d->go);
        }
    }
}

static bool StartDrawer(int screens, s32 priority, s32 readPriority, int core)
{
    Drawer *d = &sDrawers[sDrawerCount];

    memset(d, 0, sizeof(*d));
    LightEvent_Init(&d->go, RESET_ONESHOT);
    d->screens = screens;
    d->core = core;
    d->priority = priority;
    d->readPriority = readPriority;
    d->thread = threadCreate(DrawerMain, d, 64 * 1024, readPriority, core, false);
    if (d->thread == NULL) {
        return false;
    }
    sDrawerCount++;
    return true;
}

/* New 3DS: engine A's screen (the 3D one) on core 2, engine B's on core 0; otherwise both
 * on core 0. Below the game on core 0, so the game comes first, and on a New 3DS below
 * the 3D's GPU thread there too. */
static void StartDrawers(s32 priority, bool isNew3ds)
{
    if (isNew3ds && StartDrawer(ENGINE_A, priority, priority, 2)) {
        /* Engine B on core 1, below the geometry thread, when the program
         * has a share of it: in battles (where engine B's touch screen is
         * the costliest of all, and the geometry thread has little to do)
         * it has most of that core; on core 0 it came after the GPU
         * thread and fell to 10-20 frames a second.
         * While it reads the game's display state, above the geometry
         * thread (priority - 1): the game waits for that reading before
         * its next V-blank work, and in the field, where the screen hardly
         * changes, the reading (working out that it didn't) is short but
         * waited behind the geometry thread's whole scene, 18 ms. */
        if (sGeoCpuLimit == 0 || !StartDrawer(ENGINE_B, priority + 1, priority - 2, 1)) {
            StartDrawer(ENGINE_B, priority + 2, priority + 2, 0);
        }
    } else {
        StartDrawer(ENGINE_A | ENGINE_B, priority + 1, priority + 1, 0);
    }
    __atomic_store_n(&sDrawersStarted, true, __ATOMIC_RELEASE);
}

static void StopDrawers(void)
{
    int i;

    __atomic_store_n(&sDrawersStarted, false, __ATOMIC_RELEASE);
    for (i = 0; i < sDrawerCount; i++) {
        Drawer *d = &sDrawers[i];

        d->quit = true;
        LightEvent_Signal(&d->go);
        if (R_SUCCEEDED(threadJoin(d->thread, 1000000000ULL))) {
            threadFree(d->thread);
        } else {
            NitroPort_Log("A drawing thread didn't stop: quitting without it");
        }
    }
    sDrawerCount = 0;
}

static bool DrawersBusy(void)
{
    int i;

    for (i = 0; i < sDrawerCount; i++) {
        if (__atomic_load_n(&sDrawers[i].busy, __ATOMIC_ACQUIRE)) {
            return true;
        }
    }
    return false;
}

/* Before the program draws on the screens itself (the log): wait until the
 * drawing threads are done with them. */
static void WaitForDrawing(void)
{
    int i;

    for (i = 0; i < sDrawerCount; i++) {
        if (threadGetCurrent() == sDrawers[i].thread) {
            return;   /* (a drawing thread itself crashed) */
        }
    }
    while (DrawersBusy() || (sRemaster && __atomic_load_n(&sComposing, __ATOMIC_ACQUIRE))) {
        svcSleepThread(500000LL);
    }
}

/* The HOME menu (or a system applet) is about to take the screens and
 * the GPU (libctru calls this from aptMainLoop, on whichever thread called
 * it, before it hands them over), or has given them back. Nothing of ours
 * may use the GPU in between: a frame sent then never finishes, and the
 * thread waiting for it would wait for ever (and closing the program from
 * the HOME menu with it). So: stop the drawing first, and wait (at most a
 * second) for what's being drawn; then, back, draw everything again. */
static aptHookCookie sAptHook;

static void OnAptEvent(APT_HookType type, void *param)
{
    (void)param;
    if (type == APTHOOK_ONSUSPEND) {
        u64 start = svcGetSystemTick();

        __atomic_store_n(&sSuspended, true, __ATOMIC_SEQ_CST);
        while ((__atomic_load_n(&sComposing, __ATOMIC_SEQ_CST) || DrawersBusy()) &&
               svcGetSystemTick() - start < SYSCLOCK_ARM11) {
            svcSleepThread(1000000LL);
        }
    } else if (type == APTHOOK_ONRESTORE) {
        sMustDraw[0] = true;
        sMustDraw[1] = true;
        __atomic_store_n(&sComposeForce, true, __ATOMIC_RELEASE);
        __atomic_store_n(&sSuspended, false, __ATOMIC_SEQ_CST);
        if (__atomic_load_n(&sGpuWakeReady, __ATOMIC_ACQUIRE)) {
            LightEvent_Signal(&sGpuWake);
        }
    }
}

/* Hand this frame to every drawing thread that's free; the others leave
 * it out. new3dOnly: only to the one with engine A's screen, and only if
 * a 3D image came out since it was last handed one. */
static void DrawFrame(bool new3dOnly)
{
    u32 images = NitroPort_Gx3dImageCount();
    int i;
    bool any = false;

    if (new3dOnly && sRemaster) {
        return;   /* (the display thread draws each new 3D scene itself) */
    }
    if (__atomic_load_n(&sSuspended, __ATOMIC_SEQ_CST)) {
        return;   /* (the HOME menu has the screens) */
    }
    for (i = 0; i < sDrawerCount; i++) {
        Drawer *d = &sDrawers[i];

        if (new3dOnly && (!(d->screens & ENGINE_A) || d->images == images)) {
            continue;
        }
        any |= !__atomic_load_n(&d->busy, __ATOMIC_ACQUIRE);
    }
    if (!any) {
        return;
    }
    sAOnTop = NitroPort_EngineAOnTop() != 0;
    for (i = 0; i < sDrawerCount; i++) {
        Drawer *d = &sDrawers[i];

        if (new3dOnly && (!(d->screens & ENGINE_A) || d->images == images)) {
            continue;
        }
        if (!__atomic_load_n(&d->busy, __ATOMIC_ACQUIRE)) {
            d->aOnTop = sAOnTop;
            d->stamp = NitroPort_ComposeVBlanks();
            d->images = images;
            __atomic_store_n(&d->reading, true, __ATOMIC_RELEASE);
            __atomic_store_n(&d->busy, true, __ATOMIC_RELEASE);
            LightEvent_Signal(&d->go);
        }
    }
}

/* A drawing thread on a core of its own still reading the game's display
 * state. (One on core 0, the game's, only runs while the game waits: the
 * game waiting for it as well would make it as slow as both together.) */
static bool DrawersReading(void)
{
    int i;

    for (i = 0; i < sDrawerCount; i++) {
        if (sDrawers[i].core != 0 && __atomic_load_n(&sDrawers[i].reading, __ATOMIC_ACQUIRE)) {
            return true;
        }
    }
    return false;
}

/* Until no drawing thread is reading the game's display state any more,
 * for at most max ticks (a drawer far behind doesn't hold the game up
 * for longer than that). */
static void WaitForReading(u64 max)
{
    u64 start = svcGetSystemTick();

    while (DrawersReading() && svcGetSystemTick() - start < max) {
        svcSleepThread(100000LL);
    }
}

/* Halts (NitroPort_OnFrame) since the game last said its V-blank work was
 * done: 0 or 1 in its main loop (one halt a frame, or two at 30 frames a
 * second), more where it waits for V-blank in a loop of its own. */
static unsigned sHaltsSinceVBlankWork = 2;

/* The game is done with what it changes after V-blank (patches/
 * pokeplatinum/0011): the display is as the DS shows it this frame, and
 * the game won't change much of it until the next V-blank. Draw it. */
void NitroPort_VBlankWorkDone(void)
{
    sHaltsSinceVBlankWork = 0;
    if (!sShowLog && !sScreensDirty) {
        DrawFrame(false);
    }
}

/* ---------------------------------------------------------------- pacing */

/* The DS's (and the 3DS's) frame: 59.83 a second. */
#define FRAME_TICKS ((u64)(SYSCLOCK_ARM11 / 59.8261))

static u64 sDeadline;        /* when the current frame should be done */
static u64 sFrameExit;       /* when the last NitroPort_OnFrame returned */

/* For the [perf] line. */
static u64 sPerfStart;            /* the first frame counted */
static u64 sPerfGameTicks;        /* in the game's own code */
static u64 sPerfReadWaitTicks;    /* waiting for drawers to finish reading */
static unsigned sPerfFrames;
static u64 sPerfSoundTicks, sPerfSoundSamples;
static u64 sPerfGpuTicks;
static unsigned sPerfGpuScenes, sPerfGpuTriangles, sPerfGpuDroppedTriangles;

static void PerfFrame(u64 enter)
{
    u64 now = svcGetSystemTick();
    double seconds, perFrame;
    u64 soundTicks, soundSamples, gpuTicks;
    unsigned gpuScenes, gpuTriangles, gpuDroppedTriangles;
    char drawing[192];
    int i, used = 0;

    if (sPerfStart == 0) {
        sPerfStart = now;
        sPerfReadWaitTicks = 0;
        sPerfSoundTicks = sSoundMixTicks;
        sPerfSoundSamples = sSoundMixSamples;
        sPerfGpuTicks = sGpuTicks;
        sPerfGpuScenes = __atomic_load_n(&sGpuScenes,__ATOMIC_RELAXED);
        sPerfGpuTriangles = __atomic_load_n(&sGpuTriangles,__ATOMIC_RELAXED);
        sPerfGpuDroppedTriangles = __atomic_load_n(&sGpuDroppedTriangles,__ATOMIC_RELAXED);
        for (i = 0; i < sDrawerCount; i++) {
            sDrawers[i].ticks = 0;
            sDrawers[i].count = 0;
        }
        return;
    }
    if (sFrameExit != 0) {
        sPerfGameTicks += enter - sFrameExit;
    }
    sPerfFrames++;
    if (sPerfFrames < 120) {
        return;
    }

    seconds = (double)(now - sPerfStart) / SYSCLOCK_ARM11;
    perFrame = 1000.0 / SYSCLOCK_ARM11 / sPerfFrames;
    drawing[0] = 0;
    for (i = 0; i < sDrawerCount; i++) {
        Drawer *d = &sDrawers[i];
        unsigned count = d->count;

        used += snprintf(drawing + used, sizeof(drawing) - (size_t)used, "%s%s %.1f/s (%.1f/s new), %.1f ms (core %d)",
                         i ? "; " : "",
                         d->screens == (ENGINE_A | ENGINE_B) ? "both screens"
                         : (d->screens == ENGINE_A) == sAOnTop ? "upper" : "lower",
                         count / seconds, d->drawn / seconds,
                         count ? (double)d->ticks * 1000.0 / SYSCLOCK_ARM11 / count : 0.0, d->core);
        if (used >= (int)sizeof(drawing)) {
            break;
        }
        d->ticks = 0;
        d->count = 0;
        d->drawn = 0;
    }
    soundTicks = sSoundMixTicks - sPerfSoundTicks;
    soundSamples = sSoundMixSamples - sPerfSoundSamples;
    gpuTicks = sGpuTicks;
    gpuScenes = __atomic_load_n(&sGpuScenes,__ATOMIC_RELAXED);
    gpuTriangles = __atomic_load_n(&sGpuTriangles,__ATOMIC_RELAXED);
    gpuDroppedTriangles = __atomic_load_n(&sGpuDroppedTriangles,__ATOMIC_RELAXED);
    {
        static u64 lastBuild, lastWait, lastPublish, lastGeoBusy, lastTexTexels, lastTexTicks;
        static uint32_t lastGeoScenes, lastGeoSkipped, lastGeoWakeups;
        uint32_t geoWakeups;
        uint64_t geoLongest;
        static u32 lastTexConverted;
        u64 build, wait, publish, texTexels, texTicks;
        u32 texConverted;
        uint64_t geoBusy;
        uint32_t geoScenes, geoSkipped;
        unsigned scenes = gpuScenes - sPerfGpuScenes;
        NitroPort_PicaTimes(&build, &wait, &publish);
        NitroPort_PicaTextureStats(&texConverted, &texTexels, &texTicks);
        NitroPort_Gx3dThreadStats(&geoBusy, &geoScenes, &geoSkipped);
        NitroPort_Gx3dThreadWakeups(&geoWakeups, &geoLongest);
        if (sGeoThread && geoScenes != lastGeoScenes) {
            double busy = (double)(geoBusy - lastGeoBusy) / SYSCLOCK_ARM11;
            NitroPort_Log("[perf] geometry (core %d): %.1f scenes/s, %.1f left out/s; %.1f ms/scene, busy %.0f%% of the time; woken %.0f times/scene, longest stretch %.1f ms",
                          sGeoCore, (geoScenes - lastGeoScenes) / seconds,
                          (geoSkipped - lastGeoSkipped) / seconds,
                          busy * 1000.0 / (geoScenes - lastGeoScenes), 100.0 * busy / seconds,
                          (double)(geoWakeups - lastGeoWakeups) / (geoScenes - lastGeoScenes),
                          (double)geoLongest * 1000.0 / SYSCLOCK_ARM11);
        }
        lastGeoWakeups = geoWakeups;
        lastGeoBusy = geoBusy;
        lastGeoScenes = geoScenes;
        lastGeoSkipped = geoSkipped;
        if (scenes && !sRemaster) {
            /* Images the 3D screen's drawer converted between frames (the
             * rest it converts when it draws them). */
            unsigned converted = 0;
            double convertMs = 0.0;

            for (i = 0; i < sDrawerCount; i++) {
                Drawer *d = &sDrawers[i];

                if (d->screens & ENGINE_A) {
                    converted = d->converted;
                    convertMs = converted ? (double)d->convertTicks * 1000.0 / SYSCLOCK_ARM11 / converted : 0.0;
                    d->converted = 0;
                    d->convertTicks = 0;
                }
            }
            /* Edge marking: images outlined, and the pixels outlined. */
            static uint32_t lastEdgeImages, lastEdgePixels;
            uint32_t edgeImages, edgePixels;
            char outlines[64] = "";

            NitroPort_Gx3dEdgeStats(&edgeImages, &edgePixels);
            if (edgeImages != lastEdgeImages) {
                snprintf(outlines, sizeof(outlines), "; outlines %.0f pixels/image",
                         (double)(edgePixels - lastEdgePixels) / (edgeImages - lastEdgeImages));
            }
            lastEdgeImages = edgeImages;
            lastEdgePixels = edgePixels;
            NitroPort_Log("[perf] 3D: %.1f scenes/s; per scene %.1f ms building (textures: %.2f decoded, %.0f texels, %.1f ms), %.1f ms GPU and copy, %.1f ms handing over; converted by the 3D screen's drawer, %.1f/s between frames (%.1f ms each)%s",
                          scenes / seconds,
                          (double)(build - lastBuild) * 1000.0 / SYSCLOCK_ARM11 / scenes,
                          (double)(texConverted - lastTexConverted) / scenes,
                          (double)(texTexels - lastTexTexels) / scenes,
                          (double)(texTicks - lastTexTicks) * 1000.0 / SYSCLOCK_ARM11 / scenes,
                          (double)(wait - lastWait) * 1000.0 / SYSCLOCK_ARM11 / scenes,
                          (double)(publish - lastPublish) * 1000.0 / SYSCLOCK_ARM11 / scenes,
                          converted / seconds, convertMs, outlines);
        }
        lastBuild = build;
        lastWait = wait;
        lastPublish = publish;
        lastTexConverted = texConverted;
        lastTexTexels = texTexels;
        lastTexTicks = texTicks;
    }
    if (sRemaster) {
        static ComposeStats last;
        static u32 lastMade;
        ComposeStats now;
        u32 frames, published, timed, made, madeScenes;
        float cmdPeak;
        unsigned cmdCut;

        NitroPort_ComposeStats(&now);
        NitroPort_Gx3dThreadStats(NULL, &made, NULL);
        madeScenes = made - lastMade;
        lastMade = made;
        NitroPort_PicaCmdStats(&cmdPeak, &cmdCut);
        frames = now.frames - last.frames;
        published = now.published - last.published;
        timed = now.timed - last.timed;
        NitroPort_Log("[perf] display: %.1f frames/s drawn (%.1f with a new 3D scene, of %.1f scenes/s), each %.1f ms building + %.1f ms GPU; 2D published %.1f/s, %.1f ms each; 3D shown %d V-blanks after the game made it, %u late; GPU command list %.0f%% full at most, %u draws cut",
                      frames / seconds, timed / seconds, madeScenes / seconds,
                      frames ? (double)(now.cpuTicks - last.cpuTicks) * 1000.0 / SYSCLOCK_ARM11 / frames : 0.0,
                      frames ? (double)(now.gpuTicks - last.gpuTicks) * 1000.0 / SYSCLOCK_ARM11 / frames : 0.0,
                      published / seconds,
                      published ? (double)(now.publishTicks - last.publishTicks) * 1000.0 / SYSCLOCK_ARM11 / published : 0.0,
                      now.lag, (unsigned)(now.late - last.late), (double)cmdPeak * 100.0, cmdCut);
        last = now;
    }
    NitroPort_Log("[perf] game %.1f frames/s, %.1f ms/frame (%.1f waiting for drawing); drawn: %s; 3D %.1f ms/scene, %u triangles/scene, %u dropped/scene (core %d); sound %.0f%% of core 0, %u underruns",
                  sPerfFrames / seconds, (double)sPerfGameTicks * perFrame,
                  (double)sPerfReadWaitTicks * perFrame, drawing,
                  gpuScenes>sPerfGpuScenes
                    ? (double)(gpuTicks-sPerfGpuTicks)*1000.0/SYSCLOCK_ARM11/
                      (gpuScenes-sPerfGpuScenes):0.0,
                  gpuScenes>sPerfGpuScenes
                    ? (gpuTriangles-sPerfGpuTriangles)/(gpuScenes-sPerfGpuScenes):0u,
                  gpuScenes>sPerfGpuScenes
                    ? (gpuDroppedTriangles-sPerfGpuDroppedTriangles)/
                      (gpuScenes-sPerfGpuScenes):0u,
                  sGpuCore,
                  soundSamples ? 100.0 * ((double)soundTicks / SYSCLOCK_ARM11) / ((double)soundSamples / 32768.0) : 0.0,
                  sSoundUnderruns);
    sPerfStart = now;
    sPerfGameTicks = 0;
    sPerfReadWaitTicks = 0;
    sPerfFrames = 0;
    sPerfSoundTicks = sSoundMixTicks;
    sPerfSoundSamples = sSoundMixSamples;
    sPerfGpuTicks = gpuTicks;
    sPerfGpuScenes = gpuScenes;
    sPerfGpuTriangles = gpuTriangles;
    sPerfGpuDroppedTriangles = gpuDroppedTriangles;
}

/* Once a frame, from the game's idle thread (NitroPort_Halt): hand the
 * frame to the drawing threads, wait for the 3DS's V-blank, read the
 * buttons. */
void NitroPort_OnFrame(void)
{
    u32 held, down;
    u64 enter = svcGetSystemTick();

    NitroPort_SoundPumpReplies(); /* never raise ARM9 FIFO IRQ from DSP thread */
    /* The remaster's wider view only while the 3D is on the upper screen
     * (the intro has it on the lower one): from the next command on. */
    if (sRemaster && sCompose.wide) {
        NitroPort_Gx3dSetWideView(NitroPort_EngineAOnTop() ? 320 : 256);
    }
    NitroPort_Gx3dFlush();        /* the geometry thread has the frame's commands */
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
    /* Compare against PC's --dump-3d reference. Read only the newest
     * finished image, never the PICA's tiled framebuffer. */
    if ((held & (KEY_L | KEY_R)) == (KEY_L | KEY_R) && (down & KEY_Y)) {
        char path[128];
        snprintf(path,sizeof(path),DATA_DIR "/3d_frame_%07lu.png",sFrame);
        if (NitroPort_Gx3dDumpLatest(path))
            NitroPort_Log("[gx3d] dumped 3D frame %lu to %s",sFrame,path);
        else
            NitroPort_Log("[gx3d] cannot dump frame %lu (no image or file error)",sFrame);
    }
    Platinum_SetButtons(DsButtons(held));
    if (held & KEY_TOUCH) {
        touchPosition touch;

        hidTouchRead(&touch);
        if (sRemaster) {
            /* The DS's 256x192 screen fills the 320x240 one. */
            Platinum_SetTouch(1, touch.px * 4 / 5, touch.py * 4 / 5);
        } else {
            /* The DS's 256x192 screen sits in the middle of the 320x240 one. */
            Platinum_SetTouch(1, touch.px - 32, touch.py - 24);
        }
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
            sMustDraw[0] = sMustDraw[1] = true;
            __atomic_store_n(&sComposeForce, true, __ATOMIC_RELEASE);
        }
    }

    {
        u64 now = svcGetSystemTick();
        bool ahead = sDeadline == 0 || now < sDeadline;
        /* Half a frame or more to go until the V-blank. */
        bool early = sDeadline == 0 || (ahead && sDeadline - now >= FRAME_TICKS / 2);

        if (sShowLog) {
            if (!DrawersBusy()) {
                gfxFlushBuffers();
                gfxSwapBuffers();
            }
        } else if (sHaltsSinceVBlankWork >= 2) {
            /* The game doesn't say when its V-blank work is done (a loop
             * of its own): drawn here, before the V-blank. */
            DrawFrame(false);
        } else if (early) {
            /* Drawn after the V-blank work (NitroPort_VBlankWorkDone); and
             * a 3D image that came out since is shown a frame sooner when
             * there's time for it before the V-blank. With little time
             * left (or none, behind) the drawer would make the game wait
             * for it below, and the next V-blank work draws soon anyway. */
            DrawFrame(true);
        }
        if (sHaltsSinceVBlankWork < 2) {
            sHaltsSinceVBlankWork++;
        }

        /* Wait for the V-blank only when ahead: a frame that took too long
         * isn't made to wait for another one as well. When far behind, stop
         * trying to catch up. */
        if (ahead) {
            gspWaitForVBlank();
            sDeadline = svcGetSystemTick() + FRAME_TICKS;
        } else if (now - sDeadline > 3 * FRAME_TICKS) {
            sDeadline = now + FRAME_TICKS;
        } else {
            sDeadline += FRAME_TICKS;
        }
    }

    /* The game changes the display as soon as this returns (its V-blank
     * work): a drawer still reading it would show some of that. Let it
     * finish (usually it has). */
    {
        u64 start = svcGetSystemTick();

        WaitForReading(FRAME_TICKS);
        sPerfReadWaitTicks += svcGetSystemTick() - start;
    }
    PerfFrame(enter);
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
            if (sWatchdogHasHome && sFrame != lastFrame) {
                /* Frames again after all (a slow start, a long load): back
                 * to the game, which looks after HOME again. */
                sWatchdogHasHome = false;
                NitroPort_Log("Frames are coming again (frame %lu): back to the game.", sFrame);
                Status("Running");
                ShowLog(false);
            }
            lastFrame = sFrame;
            quietTicks = 0;
            continue;
        }
        /* The first frame can take a while (the game loads what it starts
         * with from the SD card): a minute then, 5 seconds afterwards. */
        if (++quietTicks == (lastFrame == 0 ? 240 : 20) && !sWatchdogHasHome) {
            sWatchdogHasHome = true;
            WaitForDrawing();
            ShowLog(true);
            if (sScreensDirty) {
                sScreensDirty = false;
                DrawLogScreens();
            }
            NitroPort_Log("The game hasn't shown a frame for %d seconds (stuck at frame %lu). "
                          "It's probably waiting for a part of the port that isn't done yet: "
                          "see the lines above. Close it from the HOME menu.", lastFrame == 0 ? 60 : 5, lastFrame);
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
    LightLock_Init(&sLogLock);
    EnableSpeedup();            /* New 3DS: 804 MHz and the L2 cache */
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
        {
            bool isNew3ds = false;

            APT_CheckNew3DS(&isNew3ds);
            ReadRemasterConfig(isNew3ds);
            StartGpu(priority, isNew3ds);
            NitroPort_Gx3dSetWideView(sRemaster && sCompose.wide ? 320 : 256);
            NitroPort_SetWideScreen(sRemaster && sCompose.wide ? 32 : 0);
            StartGeometry(priority, isNew3ds);   /* first: core 1's share */
            CheckCoreSpeed(priority, isNew3ds);  /* (with that share, nothing drawing yet) */
            StartDrawers(priority, isNew3ds);
            if (sDrawerCount == 2) {
                NitroPort_Log("[perf] %s 3DS; drawing: the 3D engine's screen on core %d, the other on core %d",
                              isNew3ds ? "New" : "Old", sDrawers[0].core, sDrawers[1].core);
            } else {
                NitroPort_Log("[perf] %s 3DS; drawing: %s", isNew3ds ? "New" : "Old",
                              sDrawerCount == 1 ? "both screens on core 0" : "no thread!");
            }
        }
        StartSound(priority);
        aptHook(&sAptHook, OnAptEvent, NULL);   /* (after citro3d's: called before it) */
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

    aptUnhook(&sAptHook);
    StopDrawers();
    StopGeometry();
    StopGpu();
    StopSound();
    if (sLog != NULL) {
        fclose(sLog);
        sLog = NULL;
    }
    gfxExit();
    return 0;
}
