/*
 * Tests for the port's threads, interrupts-with-threads and timers
 * (src/port_thread.c, src/port_timer.c, src/pxi.c), through NitroSDK's own
 * API: OS_Init, OS_CreateThread, OS_WaitIrq, mutexes, OS_Sleep ...
 *
 * A separate program from test_nitro: once OS_InitThread has run, the
 * program's thread is the SDK's launcher thread for good.
 *
 * The clock is driven by the test (NitroPort_SetClockSource), so timings
 * are exact. Each check's expected value comes from how the DS behaves:
 * NitroSDK's scheduler always runs the highest-priority ready thread, and
 * only switches inside an interrupt at the interrupt's end.
 */

#include <nitro.h>
#include <nitro_port.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int sChecks, sFailures;

#define CHECK(cond)                                                          \
    do {                                                                     \
        sChecks++;                                                           \
        if (!(cond)) {                                                       \
            sFailures++;                                                     \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);           \
        }                                                                    \
    } while (0)

#define CHECK_EQ(actual, expected)                                           \
    do {                                                                     \
        long long a_ = (long long)(actual), e_ = (long long)(expected);      \
        sChecks++;                                                           \
        if (a_ != e_) {                                                      \
            sFailures++;                                                     \
            printf("FAIL %s:%d: %s = %lld, expected %lld\n",                 \
                   __FILE__, __LINE__, #actual, a_, e_);                     \
        }                                                                    \
    } while (0)

/* ------------------------------------------------------------- the clock */

static u64 sClock;

static u64 FakeClock(void)
{
    return sClock;
}

/* DS bus cycles in `ms` milliseconds. */
static u64 Ms(u64 ms)
{
    return ms * 33513982ULL / 1000;
}

/* ------------------------------------------------------------ the frames */

static int sFrames;

/* The frame hook (the port's default is empty and weak). Every frame
 * advances the clock by one DS frame (560190 cycles). */
void NitroPort_OnFrame(void)
{
    sFrames++;
    sClock += 560190;
    if (sFrames > 100000) {
        printf("FAIL: runaway frame loop\n");
        exit(1);
    }
}

static int sVBlanks;

static void OnVBlank(void)
{
    sVBlanks++;
    OS_SetIrqCheckFlag(OS_IE_V_BLANK);
}

/* --------------------------------------------------------- a trace log */

static char sTrace[256];

static void Trace(const char *s)
{
    strncat(sTrace, s, sizeof(sTrace) - strlen(sTrace) - 1);
}

static void ClearTrace(void)
{
    sTrace[0] = '\0';
}

/* ------------------------------------------------------------------ boot */

static void test_boot(void)
{
    OSThread *launcher;

    /* What the game's InitSystem does first. OS_Init starts the card
     * library's thread (priority 4), which runs at once and goes to sleep. */
    OS_Init();
    launcher = OS_GetCurrentThread();
    CHECK(launcher != NULL);
    CHECK_EQ(OS_GetThreadPriority(launcher), OS_THREAD_LAUNCHER_PRIORITY);
    CHECK_EQ(OS_GetProcMode(), HW_PSR_SYS_MODE);
    CHECK_EQ(NitroPort_ThreadsInUse(), 2);   /* launcher + card thread */

    OS_SetIrqFunction(OS_IE_V_BLANK, OnVBlank);
    (void)OS_EnableIrqMask(OS_IE_V_BLANK);
    (void)OS_EnableIrq();
    (void)OS_EnableInterrupts();
}

/* ------------------------------------------------------------ priorities */

static OSThread sThreadA, sThreadB, sThreadC;
static u32 sStackA[1024], sStackB[1024], sStackC[1024];

static void RecordAndExit(void *arg)
{
    Trace((const char *)arg);
}

static void test_priority(void)
{
    u32 me = OS_GetThreadPriority(OS_GetCurrentThread());

    /* A higher-priority thread runs as soon as it's woken. */
    ClearTrace();
    OS_CreateThread(&sThreadA, RecordAndExit, "A", sStackA + 1024, sizeof(sStackA), me - 1);
    Trace("1");
    OS_WakeupThreadDirect(&sThreadA);
    Trace("2");
    CHECK(strcmp(sTrace, "1A2") == 0);
    CHECK(OS_IsThreadTerminated(&sThreadA));

    /* A lower-priority one waits until this thread sleeps. */
    ClearTrace();
    OS_CreateThread(&sThreadB, RecordAndExit, "B", sStackB + 1024, sizeof(sStackB), me + 1);
    OS_WakeupThreadDirect(&sThreadB);
    Trace("1");
    CHECK(!OS_IsThreadTerminated(&sThreadB));
    OS_JoinThread(&sThreadB);
    Trace("2");
    CHECK(strcmp(sTrace, "1B2") == 0);

    /* A finished thread's machine thread ends with it. (The idle thread
     * hasn't run yet: B was ready when this thread slept.) */
    CHECK_EQ(NitroPort_ThreadsInUse(), 2);

    /* The same OSThread can be used again. */
    ClearTrace();
    OS_CreateThread(&sThreadA, RecordAndExit, "a", sStackA + 1024, sizeof(sStackA), me - 1);
    OS_WakeupThreadDirect(&sThreadA);
    CHECK(strcmp(sTrace, "a") == 0);
    CHECK_EQ(NitroPort_ThreadsInUse(), 2);
}

/* ---------------------------------------------------- sleeping, waking */

static OSThreadQueue sQueue;

static void SleepTwice(void *arg)
{
    (void)arg;
    Trace("s");
    OS_SleepThread(&sQueue);
    Trace("t");
    OS_SleepThread(&sQueue);
    Trace("u");
}

static void test_sleep_wakeup(void)
{
    u32 me = OS_GetThreadPriority(OS_GetCurrentThread());

    ClearTrace();
    OS_InitThreadQueue(&sQueue);
    OS_CreateThread(&sThreadA, SleepTwice, NULL, sStackA + 1024, sizeof(sStackA), me - 1);
    OS_WakeupThreadDirect(&sThreadA);
    Trace("1");
    OS_WakeupThread(&sQueue);
    Trace("2");
    OS_WakeupThread(&sQueue);
    Trace("3");
    CHECK(strcmp(sTrace, "s1t2u3") == 0);
    CHECK(OS_IsThreadTerminated(&sThreadA));
}

/* --------------------------------------------------- waiting for V-blank */

static void test_wait_vblank(void)
{
    int frames = sFrames;
    int vblanks = sVBlanks;

    /* The game's main loop: this thread sleeps in OS_WaitIrq, the idle
     * thread halts (one frame), V-blank's interrupt wakes this thread. */
    OS_WaitIrq(TRUE, OS_IE_V_BLANK);
    CHECK_EQ(sFrames, frames + 1);
    CHECK_EQ(sVBlanks, vblanks + 1);
    OS_WaitIrq(TRUE, OS_IE_V_BLANK);
    OS_WaitIrq(TRUE, OS_IE_V_BLANK);
    CHECK_EQ(sFrames, frames + 3);
    CHECK_EQ(sVBlanks, vblanks + 3);
    CHECK_EQ(reg_GX_VCOUNT, 192);   /* the frame starts in V-blank */
}

/* -------------------------------------- switching at an interrupt's end */

static BOOL sWaiterRan;
static BOOL sRanDuringHandler;
static BOOL sHandlerInIrqMode;

static void WaitTimer0(void *arg)
{
    (void)arg;
    for (;;) {
        OS_WaitIrq(TRUE, OS_IE_TIMER0);
        sWaiterRan = TRUE;
        Trace("w");
    }
}

static void OnTimer0(void)
{
    sHandlerInIrqMode = (OS_GetProcMode() == OS_PROCMODE_IRQ);
    OS_SetIrqCheckFlag(OS_IE_TIMER0);
    /* Waking a thread inside an interrupt only asks for a switch. */
    OS_WakeupThreadDirect(&sThreadC);
    sRanDuringHandler = sWaiterRan;
    Trace("h");
}

static void Noop(void *arg)
{
    (void)arg;
    Trace("c");
}

static void test_irq_switch(void)
{
    u32 me = OS_GetThreadPriority(OS_GetCurrentThread());

    ClearTrace();
    sWaiterRan = FALSE;
    OS_SetIrqFunction(OS_IE_TIMER0, OnTimer0);
    (void)OS_EnableIrqMask(OS_IE_TIMER0);
    OS_CreateThread(&sThreadB, WaitTimer0, NULL, sStackB + 1024, sizeof(sStackB), me - 2);
    OS_WakeupThreadDirect(&sThreadB);   /* runs, and waits for timer 0 */
    OS_CreateThread(&sThreadC, Noop, NULL, sStackC + 1024, sizeof(sStackC), me - 1);

    /* The interrupt arrives while this thread runs: the handler runs in
     * interrupt mode, the woken threads don't run until it ends, and then
     * the highest-priority one (B, waiting for the timer) goes first. */
    NitroPort_RaiseIrq(OS_IE_TIMER0);
    Trace("1");
    CHECK(sHandlerInIrqMode);
    CHECK(!sRanDuringHandler);
    CHECK(sWaiterRan);
    CHECK(strcmp(sTrace, "hwc1") == 0);
    CHECK_EQ(OS_GetProcMode(), HW_PSR_SYS_MODE);
    CHECK(OS_IsThreadTerminated(&sThreadC));

    /* With interrupts off, it waits until they're back on. (The SDK calls
     * a timer's handler once per OS_SetIrqFunction.) */
    ClearTrace();
    OS_SetIrqFunction(OS_IE_TIMER0, OnTimer0);
    OS_CreateThread(&sThreadC, Noop, NULL, sStackC + 1024, sizeof(sStackC), me - 1);
    {
        OSIntrMode old = OS_DisableInterrupts();

        NitroPort_RaiseIrq(OS_IE_TIMER0);
        Trace("1");
        (void)OS_RestoreInterrupts(old);
    }
    Trace("2");
    CHECK(strcmp(sTrace, "1hwc2") == 0);

    OS_DestroyThread(&sThreadB);
    (void)OS_DisableIrqMask(OS_IE_TIMER0);
}

/* --------------------------------- each thread keeps its interrupt state */

static BOOL sResumedMasked;

static void MaskAndSleep(void *arg)
{
    OSIntrMode old;

    (void)arg;
    old = OS_DisableInterrupts();
    OS_SleepThread(NULL);
    /* Comes back with interrupts still masked, as it left. */
    sResumedMasked = (OS_GetCpsrIrq() == OS_INTRMODE_IRQ_DISABLE);
    (void)OS_RestoreInterrupts(old);
}

static int sOtherQuotient;

static void Divide(void *arg)
{
    (void)arg;
    CP_SetDiv32_32(1000, 3);
    sOtherQuotient = CP_GetDivResult32();
}

static void test_cpu_state(void)
{
    u32 me = OS_GetThreadPriority(OS_GetCurrentThread());

    CHECK_EQ(OS_GetCpsrIrq(), OS_INTRMODE_IRQ_ENABLE);
    OS_CreateThread(&sThreadA, MaskAndSleep, NULL, sStackA + 1024, sizeof(sStackA), me - 1);
    OS_WakeupThreadDirect(&sThreadA);
    /* Back here: this thread's interrupts are still enabled. */
    CHECK_EQ(OS_GetCpsrIrq(), OS_INTRMODE_IRQ_ENABLE);
    OS_WakeupThreadDirect(&sThreadA);
    CHECK(sResumedMasked);
    CHECK(OS_IsThreadTerminated(&sThreadA));
    CHECK_EQ(OS_GetCpsrIrq(), OS_INTRMODE_IRQ_ENABLE);

    /* The divider's operands belong to the thread too: another thread's
     * division in between doesn't change this one's result. */
    CP_SetDiv32_32(100, 7);
    OS_CreateThread(&sThreadA, Divide, NULL, sStackA + 1024, sizeof(sStackA), me - 1);
    OS_WakeupThreadDirect(&sThreadA);
    CHECK_EQ(sOtherQuotient, 333);
    CHECK_EQ(CP_GetDivResult32(), 14);
    CHECK_EQ(CP_GetDivRemainder32(), 2);
}

/* ------------------------------------------------------------- mutexes */

static OSMutex sMutex;

static void LockBriefly(void *arg)
{
    OS_LockMutex(&sMutex);
    Trace((const char *)arg);
    OS_UnlockMutex(&sMutex);
}

static void test_mutex(void)
{
    u32 me = OS_GetThreadPriority(OS_GetCurrentThread());

    ClearTrace();
    OS_InitMutex(&sMutex);
    OS_LockMutex(&sMutex);
    /* A higher-priority thread blocks on the mutex this thread holds ... */
    OS_CreateThread(&sThreadA, LockBriefly, "A", sStackA + 1024, sizeof(sStackA), me - 1);
    OS_WakeupThreadDirect(&sThreadA);
    Trace("1");
    CHECK(!OS_IsThreadTerminated(&sThreadA));
    /* ... and gets it the moment it's released. */
    OS_UnlockMutex(&sMutex);
    Trace("2");
    CHECK(strcmp(sTrace, "1A2") == 0);
}

/* -------------------------------------------- yielding to equal priority */

static void YieldLoop(void *arg)
{
    int i;

    for (i = 0; i < 3; i++) {
        Trace((const char *)arg);
        OS_YieldThread();
    }
}

static void test_yield(void)
{
    u32 me = OS_GetThreadPriority(OS_GetCurrentThread());

    ClearTrace();
    OS_CreateThread(&sThreadA, YieldLoop, "a", sStackA + 1024, sizeof(sStackA), me + 1);
    OS_CreateThread(&sThreadB, YieldLoop, "b", sStackB + 1024, sizeof(sStackB), me + 1);
    OS_WakeupThreadDirect(&sThreadA);
    OS_WakeupThreadDirect(&sThreadB);
    OS_JoinThread(&sThreadA);
    OS_JoinThread(&sThreadB);
    /* The SDK puts a new thread ahead of others of its priority, so B
     * (created second) goes first; then each yield passes to the other. */
    CHECK(strcmp(sTrace, "bababa") == 0);
}

/* ------------------------------------------------------- killing a thread */

static int sDestructorRan;

static void Destructor(void *arg)
{
    (void)arg;
    sDestructorRan++;
}

static void SleepForever(void *arg)
{
    (void)arg;
    OS_SetThreadDestructor(OS_GetCurrentThread(), Destructor);
    for (;;) {
        OS_SleepThread(NULL);
    }
}

static void test_kill(void)
{
    u32 me = OS_GetThreadPriority(OS_GetCurrentThread());
    int before = NitroPort_ThreadsInUse();

    sDestructorRan = 0;
    OS_CreateThread(&sThreadA, SleepForever, NULL, sStackA + 1024, sizeof(sStackA), me - 1);
    OS_WakeupThreadDirect(&sThreadA);
    CHECK_EQ(NitroPort_ThreadsInUse(), before + 1);
    /* A sleeping thread is restarted in its destructor, which ends it. */
    OS_KillThread(&sThreadA, NULL);
    OS_JoinThread(&sThreadA);
    CHECK_EQ(sDestructorRan, 1);
    CHECK(OS_IsThreadTerminated(&sThreadA));
    CHECK_EQ(NitroPort_ThreadsInUse(), before);
}

/* ------------------------------------------------- timers, tick, alarms */

static void WaitFrames(int n)
{
    while (n-- > 0) {
        OS_WaitIrq(TRUE, OS_IE_V_BLANK);
    }
}

static void test_tick(void)
{
    u64 start;

    OS_InitTick();
    start = sClock;
    CHECK_EQ(OS_GetTick(), 0);

    /* A second of frames. The tick counts at 1/64 of the bus clock, a
     * 16-bit timer whose overflow interrupts extend it to 64 bits. */
    WaitFrames(60);
    CHECK_EQ(OS_GetTick(), (sClock - start) / 64);
    CHECK_EQ(OS_TicksToMilliSeconds(OS_GetTick()), 60 * 560190ULL * 1000 / 33513982);

    /* A long gap (the program was suspended): several overflows at once.
     * None is lost; the count catches up over the next frames. */
    sClock += Ms(300);
    WaitFrames(10);
    CHECK_EQ(OS_GetTick(), (sClock - start) / 64);

    /* The game's own timer 3 (timer.c) the same way. */
    {
        u64 t3start = sClock;
        u16 count;

        OS_SetTimerControl(OS_TIMER_3, 0);
        OS_SetTimerCount(OS_TIMER_3, 0);
        OS_SetTimerControl(OS_TIMER_3, (u16)(REG_OS_TM3CNT_H_E_MASK | OS_TIMER_PRESCALER_1024));
        sClock += 1024 * 100 + 5;
        NitroPort_SyncTimers();
        count = *(REGType16v *)((u32)REG_TM0CNT_L_ADDR + OS_TIMER_3 * 4);
        CHECK_EQ(count, 100);
        CHECK_EQ(sClock - t3start, 1024 * 100 + 5);
        OS_SetTimerControl(OS_TIMER_3, 0);
    }
}

static BOOL sSleeperWoke;

static void SleepTenMs(void *arg)
{
    (void)arg;
    OS_Sleep(10);
    sSleeperWoke = TRUE;
}

static void test_alarm(void)
{
    u32 me = OS_GetThreadPriority(OS_GetCurrentThread());
    u64 start;

    OS_InitAlarm();
    sSleeperWoke = FALSE;
    start = sClock;
    OS_CreateThread(&sThreadA, SleepTenMs, NULL, sStackA + 1024, sizeof(sStackA), me - 1);
    OS_WakeupThreadDirect(&sThreadA);
    CHECK(!sSleeperWoke);
    /* Frames go by (16.7 ms each): the alarm fires within the first. */
    OS_WaitIrq(TRUE, OS_IE_V_BLANK);
    CHECK(sSleeperWoke);
    CHECK(sClock - start >= Ms(10));
    CHECK(OS_IsThreadTerminated(&sThreadA));
}

/* ------------------------------------- ARM7 answers arrive by interrupt */

static int sReplies;
static BOOL sReplyInIrqMode;

static void OnUser0(PXIFifoTag tag, u32 data, BOOL err)
{
    (void)tag;
    (void)err;
    sReplies += (int)data;
    sReplyInIrqMode = (OS_GetProcMode() == OS_PROCMODE_IRQ);
}

static void EchoArm7(int tag, u32 data, BOOL err)
{
    NitroPort_Arm7Reply(tag, data + 1, err);
}

static void test_pxi_interrupt(void)
{
    OSIntrMode old;

    PXI_SetFifoRecvCallback(PXI_FIFO_TAG_USER_0, OnUser0);
    NitroPort_SetArm7Handler(PXI_FIFO_TAG_USER_0, EchoArm7);
    sReplies = 0;
    CHECK_EQ(PXI_SendWordByFifo(PXI_FIFO_TAG_USER_0, 1, FALSE), PXI_FIFO_SUCCESS);
    CHECK_EQ(sReplies, 2);
    CHECK(sReplyInIrqMode);

    /* Sent with interrupts off: the answer waits in the FIFO. */
    old = OS_DisableInterrupts();
    CHECK_EQ(PXI_SendWordByFifo(PXI_FIFO_TAG_USER_0, 10, FALSE), PXI_FIFO_SUCCESS);
    CHECK_EQ(PXI_SendWordByFifo(PXI_FIFO_TAG_USER_0, 20, FALSE), PXI_FIFO_SUCCESS);
    CHECK_EQ(sReplies, 2);
    (void)OS_RestoreInterrupts(old);
    CHECK_EQ(sReplies, 2 + 11 + 21);
    NitroPort_SetArm7Handler(PXI_FIFO_TAG_USER_0, NULL);
}

/* -------------------------------------------------- OS_Halt with masking */

static void test_halt_masked(void)
{
    int vblanks = sVBlanks;
    OSIntrMode old = OS_DisableInterrupts();

    /* OS_WaitInterrupt (no threads): halted with interrupts masked, the
     * CPU wakes but takes V-blank only once it unmasks. */
    OS_WaitInterrupt(TRUE, OS_IE_V_BLANK);
    CHECK_EQ(sVBlanks, vblanks + 1);
    (void)OS_RestoreInterrupts(old);
}

/* ------------------------------------------------- 3D FIFO, sent by DMA */

static int sGxDone;

static void GxDone(void *arg)
{
    (void)arg;
    sGxDone++;
}

/* NitroSystem sends a model's display list with MI_SendGXCommandAsync: it
 * waits for the FIFO to be less than half full, then sends 118 words per
 * GXFIFO interrupt. The port's FIFO is always empty, so GXSTAT must say so
 * and the (level-triggered) interrupt must keep coming until it's done. */
static void test_gx_fifo(void)
{
    static u32 list[1000];   /* 9 pieces: 8 x 118 words + the rest */
    int i;

    G3X_Init();
    OS_WaitVBlankIntr();   /* the port puts GXSTAT's status back every frame */
    CHECK(G3X_GetCommandFifoStatus() & GX_FIFOSTAT_UNDERHALF);
    CHECK(G3X_GetCommandFifoStatus() & GX_FIFOSTAT_EMPTY);
    CHECK(!G3X_IsGeometryBusy());
    CHECK_EQ(G3X_GetCommandFifoCount(), 0);

    for (i = 0; i < 1000; i++) {
        list[i] = 0;   /* NOPs */
    }
    sGxDone = 0;
    MI_SendGXCommandAsync(1, list, sizeof(list), GxDone, NULL);
    CHECK_EQ(sGxDone, 1);
    MI_SendGXCommandAsync(1, list, sizeof(list), GxDone, NULL);   /* not left busy */
    CHECK_EQ(sGxDone, 2);
    CHECK_EQ(reg_OS_IE & OS_IE_GXFIFO, 0);   /* put back as it was */
}

int main(void)
{
    NitroPort_SetClockSource(FakeClock);

    test_boot();
    test_priority();
    test_sleep_wakeup();
    test_wait_vblank();
    test_irq_switch();
    test_cpu_state();
    test_mutex();
    test_yield();
    test_kill();
    test_tick();
    test_alarm();
    test_pxi_interrupt();
    test_halt_masked();
    test_gx_fifo();

    printf("threads and timers: %d checks, %d failed\n", sChecks, sFailures);
    return sFailures == 0 ? 0 : 1;
}
