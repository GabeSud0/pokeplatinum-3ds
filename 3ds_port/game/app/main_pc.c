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
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <ucontext.h>
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

/* Print return addresses as function, file:line (addr2line, which also
 * names static functions and inlined calls); raw symbols if that fails. */
static void PrintFrames(void *const *frames, int count)
{
    char command[64 * 24 + 128];
    size_t used = (size_t)snprintf(command, sizeof(command),
                                   "addr2line -f -i -p -e /proc/%d/exe", (int)getpid());
    int f;

    for (f = 0; f < count && used < sizeof(command) - 24; f++) {
        /* A return address points after the call: look up the call. */
        used += (size_t)snprintf(command + used, sizeof(command) - used, " %#lx",
                                 (unsigned long)((const char *)frames[f] - 1));
    }
    snprintf(command + used, sizeof(command) - used, " 1>&2");
    fflush(stderr);
    if (system(command) != 0) {
        backtrace_symbols_fd(frames, count, 2);
    }
}

static void OnCrash(int sig)
{
    void *frames[64];
    int n = backtrace(frames, 64);

    fprintf(stderr, "[%6ld] [pc] crashed (signal %d). Backtrace:\n", sFrame, sig);
    /* Frames 0 and 1: this handler and the signal trampoline. */
    PrintFrames(frames + 2, n - 2);
    _exit(128 + sig);
}

/* Integer division by zero. The 3DS has no divide instruction: GCC calls a
 * library routine, which gives 0 for x / 0 and doesn't stop the program
 * (nor did the DS's). x86 traps instead. Some of Nintendo's code divides by
 * zero harmlessly (a value it then never uses), so here a division by zero
 * does what it does on the 3DS: quotient 0, remainder the dividend. Each
 * place is logged once. Any other arithmetic fault is a crash. */
static void OnArithmeticFault(int sig, siginfo_t *info, void *context)
{
#if defined(__i386__) || defined(__x86_64__)
    ucontext_t *uc = context;
#if defined(__x86_64__)
    greg_t *ip = &uc->uc_mcontext.gregs[REG_RIP];
    greg_t *ax = &uc->uc_mcontext.gregs[REG_RAX];
    greg_t *dx = &uc->uc_mcontext.gregs[REG_RDX];
#else
    greg_t *ip = &uc->uc_mcontext.gregs[REG_EIP];
    greg_t *ax = &uc->uc_mcontext.gregs[REG_EAX];
    greg_t *dx = &uc->uc_mcontext.gregs[REG_EDX];
#endif
    const unsigned char *code = (const unsigned char *)*ip;
    const unsigned char *p = code;
    static const unsigned char *logged[32];
    static int loggedCount;
    int wide = 0, rex = 0, mod, rm, i, seen = 0;

    if (info->si_code != FPE_INTDIV) {
        OnCrash(sig);
    }
    /* DIV/IDIV r/m: [66] [REX] F6|F7 ModRM [SIB] [disp] */
    if (*p == 0x66) {
        p++;
    }
#if defined(__x86_64__)
    if ((*p & 0xF0) == 0x40) {
        rex = *p++;
    }
#endif
    if ((*p != 0xF6 && *p != 0xF7) || ((p[1] >> 3) & 7) < 6) {
        OnCrash(sig);
    }
    wide = *p == 0xF7;
    p++;
    mod = *p >> 6;
    rm = *p & 7;
    p++;
    if (mod != 3 && rm == 4) {               /* SIB byte */
        int base = *p & 7;

        p++;
        if (mod == 0 && base == 5) {
            p += 4;
        }
    } else if (mod == 0 && rm == 5) {        /* [disp32] / [rip+disp32] */
        p += 4;
    }
    p += mod == 1 ? 1 : mod == 2 ? 4 : 0;

    for (i = 0; i < loggedCount; i++) {
        seen |= logged[i] == code;
    }
    if (!seen && loggedCount < 32) {
        void *where[1] = { (void *)(code + 1) };

        logged[loggedCount++] = code;
        NitroPort_Log("[pc] integer division by zero; continuing with 0, as on the 3DS, at:");
        PrintFrames(where, 1);
    }
    if (wide) {
        *dx = *ax;                           /* remainder: the dividend */
        *ax = 0;                             /* quotient: 0 */
    } else {
        *ax = (*ax & ~(greg_t)0xFFFF) | ((*ax & 0xFF) << 8);   /* AH = AL % 0, AL = 0 */
    }
    (void)rex;
    *ip = (greg_t)(uintptr_t)p;
#else
    (void)info;
    (void)context;
    OnCrash(sig);
#endif
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
        fprintf(stderr, "---- thread %d:\n", (int)sDumps[i].tid);
        /* Frames 0 and 1: the signal handler and the signal trampoline. */
        PrintFrames(sDumps[i].frames + 2, sDumps[i].count - 2);
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
    {
        struct sigaction fault;

        memset(&fault, 0, sizeof(fault));
        fault.sa_sigaction = OnArithmeticFault;
        fault.sa_flags = SA_SIGINFO;
        sigaction(SIGFPE, &fault, NULL);
    }
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
