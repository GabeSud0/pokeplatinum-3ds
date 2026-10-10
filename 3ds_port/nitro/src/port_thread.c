/*
 * Thread switching.
 *
 * NitroSDK schedules its threads itself (os_thread.c): it keeps them in a
 * list by priority, and to switch it saves the running thread's CPU
 * registers into its OSContext (OS_SaveContext) and loads the next one's
 * (OS_LoadContext). That code is kept unchanged. What the port changes is
 * what "saving" and "loading" a context mean.
 *
 * Each SDK thread is played by a thread of the machine (a 3DS thread, or a
 * POSIX thread on a PC; src/port_platform.h), with its own stack. Only one
 * of them runs game code at any time: the others wait on their signal. To
 * switch, the running one wakes the next one and waits on its own signal.
 * So the CPU state is kept by the machine's thread, and an OSContext only
 * says which machine thread plays that SDK thread:
 *
 *   OS_SaveContext(ctx)  remembers that the running machine thread plays
 *                        ctx, and returns FALSE ("saved, now switch").
 *   OS_LoadContext(ctx)  wakes the machine thread that plays ctx, then
 *                        waits until something switches back to the caller.
 *                        It then returns, which brings the caller back out
 *                        of the scheduler exactly where OS_SaveContext's
 *                        "resumed" return would have.
 *
 * A context that OS_InitContext has just set up (a new thread, or one being
 * restarted at another function by OS_KillThread) has a non-zero pc_plus4:
 * loading it starts a new machine thread at that function. Whenever a
 * machine thread takes over a context the port sets pc_plus4 to 0, so a
 * non-zero value always means "set up again since".
 *
 * Also kept per thread, as the DS's context switch keeps them: the status
 * register (the port's OSi_PortCpsr: interrupt-disable bit and CPU mode) and
 * the maths coprocessor's operands.
 *
 * Interrupts: the DS switches threads at the end of an interrupt if the
 * interrupt woke a thread (os_irqHandler.c, OS_IrqHandler_ThreadSwitch).
 * The SDK's C version calls NitroPort_IrqThreadSwitch below, which does the
 * same as the assembly.
 */

#include <nitro.h>
#include <nitro_port.h>
#include <stdint.h>
#include <stdlib.h>

#include "port_platform.h"

extern OSThreadQueue OSi_IrqThreadQueue;

#define MAX_THREADS 64

enum {
    SLOT_FREE,
    SLOT_STARTING,  /* machine thread started, waiting for its first turn */
    SLOT_RUNNING,
    SLOT_SUSPENDED,
    SLOT_ABANDONED  /* its context was set up again: it must just end */
};

typedef struct PortThread {
    int state;                 /* read and written with GetState/SetState */
    OSContext *context;        /* the SDK thread this machine thread plays */
    NitroPortSignal *wake;
    u32 cpsr;                  /* OSi_PortCpsr while it waits */
    BOOL adopted;              /* the program's own thread: never ends */
} PortThread;

static PortThread sThreads[MAX_THREADS];
static PortThread *sRunning;   /* the machine thread running game code */

/* A slot's state is the one thing a machine thread that isn't running game
 * code may change (an abandoned one freeing its slot on the way out), so
 * it's read and written atomically. Everything else is only touched by the
 * running thread; the signals order the hand-overs. */
static int GetState(const PortThread *t)
{
    return __atomic_load_n(&t->state, __ATOMIC_ACQUIRE);
}

static void SetState(PortThread *t, int state)
{
    __atomic_store_n(&t->state, state, __ATOMIC_RELEASE);
}

static void Fatal(const char *what) __attribute__((noreturn));

static void Fatal(const char *what)
{
    NitroPort_Log("[nitro] thread switching: %s", what);
    abort();
}

static PortThread *NewSlot(void)
{
    int i;

    for (i = 0; i < MAX_THREADS; i++) {
        PortThread *t = &sThreads[i];

        if (GetState(t) == SLOT_FREE) {
            if (t->wake == NULL) {
                t->wake = NitroPortPlatform_NewSignal();
            }
            t->context = NULL;
            t->adopted = FALSE;
            return t;
        }
    }
    Fatal("more than 64 threads");
}

static PortThread *FindByContext(const OSContext *context)
{
    int i;

    for (i = 0; i < MAX_THREADS; i++) {
        PortThread *t = &sThreads[i];
        int state = GetState(t);

        if (state != SLOT_FREE && state != SLOT_ABANDONED && t->context == context) {
            return t;
        }
    }
    return NULL;
}

/* The machine thread running now. The first time, that is the program's
 * own thread (the one that called OS_InitThread: the SDK's launcher thread). */
static PortThread *Running(void)
{
    if (sRunning == NULL) {
        sRunning = NewSlot();
        sRunning->adopted = TRUE;
        SetState(sRunning, SLOT_RUNNING);
    }
    return sRunning;
}

/* Record that `self` plays `context`. */
static void Bind(PortThread *self, OSContext *context)
{
    if (self->context != context) {
        PortThread *other = FindByContext(context);

        if (other != NULL && other != self) {
            Fatal("a context is played by two threads");
        }
        self->context = context;
    }
    context->pc_plus4 = 0;
}

static u32 StackSize(const OSContext *context)
{
    /* The context is the first member of its OSThread. */
    const OSThread *thread = (const OSThread *)context;
    u32 size = thread->stackBottom - thread->stackTop;

    return (size < 0x100000) ? size * 4 : 0;
}

static void RunFresh(PortThread *self) __attribute__((noreturn));

/* Run the function a fresh context was set up with, as the DS would: r0 is
 * its argument, and it returns to lr (OS_ExitThread for a new thread). */
static void RunFresh(PortThread *self)
{
    OSContext *context = self->context;
    void (*entry)(void *) = (void (*)(void *))(uintptr_t)(context->pc_plus4 - 4);
    void *arg = (void *)(uintptr_t)context->r[0];
    void (*after)(void) = (void (*)(void))(uintptr_t)context->lr;

    context->pc_plus4 = 0;
    SetState(self, SLOT_RUNNING);
    OSi_PortCpsr = context->cpsr;
    entry(arg);
    if (after != NULL) {
        after();
    }
    OS_ExitThread();
    Fatal("a thread came back from OS_ExitThread");
}

static void ThreadMain(void *arg)
{
    PortThread *self = arg;

    NitroPortPlatform_Wait(self->wake);
    if (GetState(self) == SLOT_ABANDONED) {
        SetState(self, SLOT_FREE);
        return;
    }
    RunFresh(self);
}

/* Give the CPU to `next` and wait for it to come back. */
static void HandOver(PortThread *self, PortThread *next)
{
    OSThread *thread = (OSThread *)self->context;
    BOOL ending = thread != NULL && thread->state == OS_THREAD_STATE_TERMINATED && !self->adopted;

    if (self->context != NULL) {
        CP_SaveContext(&self->context->cp_context);
    }
    self->cpsr = OSi_PortCpsr;
    sRunning = next;

    if (ending) {
        /* OS_ExitThread: this SDK thread is gone; so is its machine thread. */
        self->context = NULL;
        SetState(self, SLOT_FREE);
        NitroPortPlatform_Post(next->wake);
        NitroPortPlatform_ExitThread();
    }

    SetState(self, SLOT_SUSPENDED);
    NitroPortPlatform_Post(next->wake);
    NitroPortPlatform_Wait(self->wake);

    /* Whoever woke us made us the running thread again (sRunning == self),
     * unless it only woke us to end. */
    if (GetState(self) == SLOT_ABANDONED) {
        SetState(self, SLOT_FREE);
        NitroPortPlatform_ExitThread();
    }
    SetState(self, SLOT_RUNNING);
    OSi_PortCpsr = self->cpsr;
    CPi_RestoreContext(&self->context->cp_context);
}

/* Switch from the running machine thread to the one that plays `context`
 * (starting it if the context is fresh). Returns when switched back to. */
static void SwitchTo(OSContext *context)
{
    PortThread *self = Running();
    PortThread *next;

    if (context->pc_plus4 != 0) {
        /* Set up by OS_InitContext since it last ran. */
        if (self->context == context) {
            RunFresh(self);   /* restarting ourselves: run it right here */
        }
        next = FindByContext(context);
        if (next != NULL) {
            /* Its old machine thread is waiting somewhere it must never
             * return to: wake it only to end. */
            SetState(next, SLOT_ABANDONED);
            NitroPortPlatform_Post(next->wake);
        }
        next = NewSlot();
        next->context = context;
        SetState(next, SLOT_STARTING);
        if (!NitroPortPlatform_StartThread(ThreadMain, next, StackSize(context))) {
            Fatal("couldn't start a thread");
        }
    } else {
        next = FindByContext(context);
        if (next == NULL || GetState(next) != SLOT_SUSPENDED) {
            Fatal("switching to a thread that isn't waiting");
        }
    }
    HandOver(self, next);
}

BOOL NitroPort_SaveContext(void *context)
{
    Bind(Running(), context);
    /* FALSE: "just saved". The resumed thread comes back through
     * NitroPort_LoadContext instead (see the top of this file). */
    return FALSE;
}

void NitroPort_LoadContext(void *context)
{
    SwitchTo(context);
}

/* The end of every interrupt (OS_IrqHandler_ThreadSwitch), as the DS's
 * assembly does it: every thread waiting in OS_WaitIrq becomes ready; then,
 * if that or anything during the interrupt asked for rescheduling, the
 * highest-priority ready thread runs. The interrupted thread continues from
 * here when it's switched back to. */
void NitroPort_IrqThreadSwitch(void)
{
    OSThreadInfo *info = &OSi_ThreadInfo;
    OSThread *t;
    OSThread *current;

    if (OSi_IrqThreadQueue.head != NULL) {
        t = OSi_IrqThreadQueue.head;
        while (t != NULL) {
            OSThread *next = t->link.next;

            t->state = OS_THREAD_STATE_READY;
            t->queue = NULL;
            t->link.prev = NULL;
            t->link.next = NULL;
            t = next;
        }
        OSi_IrqThreadQueue.head = NULL;
        OSi_IrqThreadQueue.tail = NULL;
        info->isNeedRescheduling = TRUE;
    }

    if (!info->isNeedRescheduling) {
        return;
    }
    info->isNeedRescheduling = FALSE;

    for (t = info->list; t != NULL && t->state != OS_THREAD_STATE_READY; t = t->next) {
    }
    current = info->current;
    if (t == NULL || t == current) {
        return;
    }
    if (info->switchCallback != NULL) {
        ((OSSwitchThreadCallback)info->switchCallback)(current, t);
    }
    info->current = t;
    Bind(Running(), &current->context);
    SwitchTo(&t->context);
}

/* For tests: how many machine threads play SDK threads (not counting ones
 * that are only on their way out). */
int NitroPort_ThreadsInUse(void)
{
    int i;
    int n = 0;

    for (i = 0; i < MAX_THREADS; i++) {
        int state = GetState(&sThreads[i]);

        if (state != SLOT_FREE && state != SLOT_ABANDONED) {
            n++;
        }
    }
    return n;
}
