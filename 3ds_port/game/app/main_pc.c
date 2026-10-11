/*
 * The game on a PC, without a screen: for finding out how far it gets, and
 * where it stops, faster than on a 3DS.
 *
 *   pokeplatinum-pc <platinum3ds.pak> <manifest.json> [--save FILE]
 *                   [--frames N] [--stop-at-missing-file] [--stall-seconds N]
 *   pokeplatinum-pc --check-overlays
 *
 * Runs the same game code as the 3DS program, with the same data pack, as
 * fast as the PC goes. The save is a raw 512 KB file, as emulators keep
 * them: --save FILE, or platinum.sav next to the data pack; it's made if
 * missing. Everything the port logs goes to stderr, each line
 * with the frame number. With --frames N it stops after N frames (exit
 * status 0); with --stop-at-missing-file, at the first file the game asks
 * for that the pack doesn't have (exit status 5). If the game crashes, the
 * crash is reported with a backtrace.
 *
 * If no frame comes for 5 seconds (--stall-seconds N to change it), the game
 * is stuck: waiting in a loop for something the port doesn't do yet. The
 * runner then prints where every thread is, as function names and file:line
 * (with addr2line, from binutils), and stops (exit status 6). The thread
 * that isn't waiting in the port's thread switching is the one that's stuck.
 *
 * --check-overlays loads every overlay with the game's overlay manager and
 * checks that each load resets the overlay's data (exit status 1 if not).
 * It needs no data pack.
 *
 * Build: make -C 3ds_port/game TARGET=host32 program (32-bit, as on the
 * 3DS). TARGET=host (64-bit) builds too, but Nintendo's graphics formats
 * hold 4-byte pointers, so there the game can't get far once it loads them.
 */

#define _GNU_SOURCE
#include <dirent.h>
#include <execinfo.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <nitro.h>
#include <nitro_port.h>

#include "platinum.h"

static volatile long sFrame;
static long sFrameLimit = -1;
static int sStopAtMissingFile;
static int sStallSeconds = 5;

void NitroPort_Log(const char *fmt, ...)
{
    va_list args;

    fprintf(stderr, "[%6ld] ", sFrame);
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fputc('\n', stderr);
}

void NitroPort_OnFrame(void)
{
    sFrame++;
    if (sFrameLimit >= 0 && sFrame >= sFrameLimit) {
        NitroPort_Log("[pc] stopping after %ld frames, as asked", sFrame);
        fflush(stderr);
        _exit(0);
    }
}

void NitroPort_Terminate(void)
{
    NitroPort_Log("[pc] the game stopped itself (OS_Terminate)");
    fflush(stderr);
    _exit(3);
}

void NitroPort_ResetSystem(u32 parameter)
{
    NitroPort_Log("[pc] the game asked to restart (OS_ResetSystem %lu); stopping",
                  (unsigned long)parameter);
    fflush(stderr);
    _exit(4);
}

void NitroPort_FileMissing(const char *path)
{
    NitroPort_Log("[nitro] the game asked for a file that isn't in the data pack: %s", path);
    if (sStopAtMissingFile) {
        NitroPort_Log("[pc] stopping there, as asked");
        fflush(stderr);
        _exit(5);
    }
}

static void OnCrash(int sig)
{
    void *frames[64];
    int n = backtrace(frames, 64);

    fprintf(stderr, "[%6ld] [pc] crashed (signal %d). Backtrace:\n", sFrame, sig);
    backtrace_symbols_fd(frames, n, 2);
    _exit(128 + sig);
}

/* ------------------------------------------------- where a stuck game is */

#define MAX_DUMPED_THREADS 64
#define MAX_FRAMES 40

static struct {
    pid_t tid;
    int count;
    void *frames[MAX_FRAMES];
} sDumps[MAX_DUMPED_THREADS];
static volatile int sDumpCount;

/* Runs on each thread the watchdog signals: note where that thread is. */
static void OnDumpSignal(int sig)
{
    int i = __atomic_fetch_add(&sDumpCount, 1, __ATOMIC_SEQ_CST);

    (void)sig;
    if (i < MAX_DUMPED_THREADS) {
        sDumps[i].tid = (pid_t)syscall(SYS_gettid);
        sDumps[i].count = backtrace(sDumps[i].frames, MAX_FRAMES);
    }
}

static void DumpThreads(void)
{
    pid_t self = (pid_t)syscall(SYS_gettid);
    DIR *dir = opendir("/proc/self/task");
    struct dirent *entry;
    int i, n;

    signal(SIGUSR1, OnDumpSignal);
    while (dir != NULL && (entry = readdir(dir)) != NULL) {
        pid_t tid = (pid_t)atoi(entry->d_name);

        if (tid > 0 && tid != self) {
            syscall(SYS_tgkill, getpid(), tid, SIGUSR1);
        }
    }
    if (dir != NULL) {
        closedir(dir);
    }
    usleep(300 * 1000);

    n = sDumpCount < MAX_DUMPED_THREADS ? sDumpCount : MAX_DUMPED_THREADS;
    for (i = 0; i < n; i++) {
        char command[MAX_FRAMES * 24 + 128];
        size_t used;
        int f;

        fprintf(stderr, "---- thread %d:\n", (int)sDumps[i].tid);
        fflush(stderr);
        /* Frame 0 and 1 are this signal handler and the signal trampoline. */
        used = (size_t)snprintf(command, sizeof(command),
                                "addr2line -f -i -p -e /proc/%d/exe", (int)getpid());
        for (f = 2; f < sDumps[i].count && used < sizeof(command) - 24; f++) {
            /* A return address points after the call: look up the call. */
            used += (size_t)snprintf(command + used, sizeof(command) - used, " %#lx",
                                     (unsigned long)((char *)sDumps[i].frames[f] - 1));
        }
        snprintf(command + used, sizeof(command) - used, " 1>&2");
        if (system(command) != 0) {
            backtrace_symbols_fd(sDumps[i].frames + 2, sDumps[i].count - 2, 2);
        }
    }
}

/* If frames stop coming, say where the game is and stop. */
static void *Watch(void *arg)
{
    long last = -1;
    int still = 0;

    (void)arg;
    for (;;) {
        long now;

        sleep(1);
        now = sFrame;
        still = now == last ? still + 1 : 0;
        last = now;
        if (still >= sStallSeconds) {
            NitroPort_Log("[pc] no frame for %d seconds: the game is stuck. Where each thread "
                          "is (the one not waiting in port_thread.c is the stuck one):",
                          sStallSeconds);
            DumpThreads();
            NitroPort_Log("[pc] stopping (stuck)");
            fflush(stderr);
            _exit(6);
        }
    }
    return NULL;
}

int main(int argc, char **argv)
{
    PlatinumSettings settings;
    const char *error;
    const char *savePath = NULL;
    char *defaultSave = NULL;
    int i;

    if (argc == 2 && strcmp(argv[1], "--check-overlays") == 0) {
        return Platinum_CheckOverlays() == 0 ? 0 : 1;
    }
    if (argc < 3) {
        fprintf(stderr, "usage: %s <platinum3ds.pak> <manifest.json> [--save FILE] "
                "[--frames N] [--stop-at-missing-file] [--stall-seconds N]\n       %s --check-overlays\n", argv[0], argv[0]);
        return 2;
    }
    for (i = 3; i < argc; i++) {
        if (strcmp(argv[i], "--save") == 0 && i + 1 < argc) {
            savePath = argv[++i];
        } else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            sFrameLimit = strtol(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--stop-at-missing-file") == 0) {
            sStopAtMissingFile = 1;
        } else if (strcmp(argv[i], "--stall-seconds") == 0 && i + 1 < argc) {
            sStallSeconds = atoi(argv[++i]);
        } else {
            fprintf(stderr, "unknown option %s\n", argv[i]);
            return 2;
        }
    }

    signal(SIGSEGV, OnCrash);
    signal(SIGBUS, OnCrash);
    signal(SIGFPE, OnCrash);
    signal(SIGILL, OnCrash);
    signal(SIGABRT, OnCrash);

    error = Platinum_OpenPak(argv[1], argv[2]);
    if (error != NULL) {
        fprintf(stderr, "%s: %s\n", argv[1], error);
        return 2;
    }

    if (savePath == NULL) {
        /* platinum.sav in the data pack's folder */
        const char *slash = strrchr(argv[1], '/');
        size_t dir = slash != NULL ? (size_t)(slash - argv[1] + 1) : 0;

        defaultSave = malloc(dir + sizeof("platinum.sav"));
        if (defaultSave == NULL) {
            return 2;
        }
        memcpy(defaultSave, argv[1], dir);
        strcpy(defaultSave + dir, "platinum.sav");
        savePath = defaultSave;
    }

    Platinum_DefaultSettings(&settings);
    settings.savePath = savePath;

    {
        pthread_t watcher;
        void *warm[2];

        backtrace(warm, 2);   /* loads libgcc now, not inside a signal handler */
        if (sStallSeconds > 0) {
            pthread_create(&watcher, NULL, Watch, NULL);
        }
    }
    Platinum_Run(&settings);
}
