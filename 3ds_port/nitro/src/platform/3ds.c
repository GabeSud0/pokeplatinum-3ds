/*
 * The port's platform layer on the 3DS (src/port_platform.h), with libctru.
 *
 * Every game thread becomes a 3DS thread at the same priority as the main
 * one, on the same CPU core. Only one of them runs game code at a time (the
 * others wait on their signal; see src/port_thread.c), so the 3DS
 * scheduler's choices don't matter.
 */

#include <3ds.h>
#include <stdlib.h>

#include "../port_platform.h"

/* From nitro_port.h, which can't be included next to libctru's headers. */
void NitroPort_Log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

struct NitroPortSignal {
    LightSemaphore sem;
};

NitroPortSignal *NitroPortPlatform_NewSignal(void)
{
    NitroPortSignal *signal = calloc(1, sizeof(*signal));

    if (signal == NULL) {
        NitroPort_Log("[nitro] out of memory for a thread signal");
        svcBreak(USERBREAK_PANIC);
    }
    LightSemaphore_Init(&signal->sem, 0, 0x7FFF);
    return signal;
}

void NitroPortPlatform_Wait(NitroPortSignal *signal)
{
    LightSemaphore_Acquire(&signal->sem, 1);
}

void NitroPortPlatform_Post(NitroPortSignal *signal)
{
    LightSemaphore_Release(&signal->sem, 1);
}

/* The program can watch every new thread (to install its crash handler). */
__attribute__((weak)) void NitroPort_PlatformThreadStarted(void)
{
}

typedef struct {
    void (*entry)(void *);
    void *arg;
} StartInfo;

static void ThreadMain(void *p)
{
    StartInfo info = *(StartInfo *)p;

    free(p);
    NitroPort_PlatformThreadStarted();
    info.entry(info.arg);
    threadExit(0);
}

int NitroPortPlatform_StartThread(void (*entry)(void *), void *arg, uint32_t stackSize)
{
    StartInfo *info = malloc(sizeof(*info));
    s32 priority = 0x30;
    Thread thread;

    if (info == NULL) {
        return 0;
    }
    info->entry = entry;
    info->arg = arg;

    /* The game's stacks were sized for the DS (the card thread has 1 KB);
     * the port's own code and GCC's code need more. */
    if (stackSize < 64 * 1024) {
        stackSize = 64 * 1024;
    }
    svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
    thread = threadCreate(ThreadMain, info, stackSize, priority, -2, true);
    if (thread == NULL) {
        free(info);
        return 0;
    }
    return 1;
}

void NitroPortPlatform_ExitThread(void)
{
    threadExit(0);
}

uint64_t NitroPortPlatform_Nanoseconds(void)
{
    u64 ticks = svcGetSystemTick();

    /* Split so the multiplication can't overflow. */
    return (ticks / SYSCLOCK_ARM11) * 1000000000ULL +
           (ticks % SYSCLOCK_ARM11) * 1000000000ULL / SYSCLOCK_ARM11;
}

void NitroPortPlatform_Sleep(uint32_t microseconds)
{
    /* (svcSleepThread(0) would only let threads of the same priority run) */
    svcSleepThread((s64)(microseconds ? microseconds : 1) * 1000);
}
