/*
 * The port's platform layer on a PC (src/port_platform.h): POSIX threads and
 * the monotonic clock. Used by the test programs and the PC runner.
 */

#define _POSIX_C_SOURCE 200809L

#include "../port_platform.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

struct NitroPortSignal {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    int posted;
};

NitroPortSignal *NitroPortPlatform_NewSignal(void)
{
    NitroPortSignal *signal = calloc(1, sizeof(*signal));

    if (signal == NULL) {
        fprintf(stderr, "[nitro] out of memory for a thread signal\n");
        abort();
    }
    pthread_mutex_init(&signal->lock, NULL);
    pthread_cond_init(&signal->cond, NULL);
    return signal;
}

void NitroPortPlatform_Wait(NitroPortSignal *signal)
{
    pthread_mutex_lock(&signal->lock);
    while (!signal->posted) {
        pthread_cond_wait(&signal->cond, &signal->lock);
    }
    signal->posted = 0;
    pthread_mutex_unlock(&signal->lock);
}

void NitroPortPlatform_Post(NitroPortSignal *signal)
{
    pthread_mutex_lock(&signal->lock);
    signal->posted = 1;
    pthread_cond_signal(&signal->cond);
    pthread_mutex_unlock(&signal->lock);
}

typedef struct {
    void (*entry)(void *);
    void *arg;
} StartInfo;

static void *ThreadMain(void *p)
{
    StartInfo info = *(StartInfo *)p;

    free(p);
    info.entry(info.arg);
    return NULL;
}

int NitroPortPlatform_StartThread(void (*entry)(void *), void *arg, uint32_t stackSize)
{
    pthread_attr_t attr;
    pthread_t thread;
    StartInfo *info = malloc(sizeof(*info));
    int err;

    if (info == NULL) {
        return 0;
    }
    info->entry = entry;
    info->arg = arg;

    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (stackSize < 256 * 1024) {
        stackSize = 256 * 1024;
    }
    pthread_attr_setstacksize(&attr, stackSize);
    err = pthread_create(&thread, &attr, ThreadMain, info);
    pthread_attr_destroy(&attr);
    if (err != 0) {
        free(info);
        return 0;
    }
    return 1;
}

void NitroPortPlatform_ExitThread(void)
{
    pthread_exit(NULL);
}

uint64_t NitroPortPlatform_Nanoseconds(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}
