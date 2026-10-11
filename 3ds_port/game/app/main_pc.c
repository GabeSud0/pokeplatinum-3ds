/*
 * The game on a PC, without a screen: for finding out how far it gets, and
 * where it stops, faster than on a 3DS.
 *
 *   pokeplatinum-pc <platinum3ds.pak> <manifest.json> [--save FILE]
 *                   [--frames N] [--stop-at-missing-file] [--stall-seconds N]
 *                   [--screenshots DIR] [--screenshot-every N]
 *                   [--press FRAME:BUTTONS,...]
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
 * --screenshots DIR saves what the two screens show (port_render2d.c) as
 * PNG files, the upper screen above the lower one, every 60 frames (one
 * second of game time; --screenshot-every N to change it), and once more
 * when the runner stops. --press presses buttons at given frames, since the
 * PC runner has none: "--press 8700:A,9000:START,9100:UP+B" holds each for
 * 6 frames (A B X Y L R START SELECT UP DOWN LEFT RIGHT).
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
static const char *sScreenshotDir;
static long sScreenshotEvery = 60;

#define MAX_PRESSES 256
static struct {
    long frame;
    uint16_t buttons;
} sPresses[MAX_PRESSES];
static int sPressCount;

/* ------------------------------------------------------------ screenshots */

static uint32_t sCrcTable[256];

static uint32_t Crc(uint32_t crc, const unsigned char *p, size_t n)
{
    size_t i;

    if (sCrcTable[1] == 0) {
        uint32_t c, k;

        for (k = 0; k < 256; k++) {
            c = k;
            for (i = 0; i < 8; i++) {
                c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            }
            sCrcTable[k] = c;
        }
    }
    crc = ~crc;
    for (i = 0; i < n; i++) {
        crc = sCrcTable[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    }
    return ~crc;
}

static void Put32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)(v >> 24);
    p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);
    p[3] = (unsigned char)v;
}

static void Chunk(FILE *f, const char *type, const unsigned char *data, uint32_t len)
{
    unsigned char head[8], tail[4];
    uint32_t crc;

    Put32(head, len);
    memcpy(head + 4, type, 4);
    crc = Crc(0, head + 4, 4);
    crc = Crc(crc, data, len);
    Put32(tail, crc);
    fwrite(head, 1, 8, f);
    fwrite(data, 1, len, f);
    fwrite(tail, 1, 4, f);
}

/* A PNG of DS colours (15-bit), uncompressed (deflate's stored blocks), so
 * no image library is needed. */
static int WritePng(const char *path, const uint16_t *pixels, int w, int h)
{
    size_t raw = (size_t)h * (size_t)(1 + w * 3);
    size_t blocks = (raw + 65534) / 65535;
    size_t zlen = 2 + raw + blocks * 5 + 4;
    unsigned char *rows = malloc(raw);
    unsigned char *z = malloc(zlen);
    unsigned char ihdr[13];
    uint32_t a = 1, b = 0;
    size_t i, pos = 0, done = 0;
    FILE *f;
    int x, y;

    if (rows == NULL || z == NULL) {
        free(rows);
        free(z);
        return -1;
    }
    for (y = 0; y < h; y++) {
        unsigned char *r = rows + (size_t)y * (size_t)(1 + w * 3);

        *r++ = 0;   /* filter: none */
        for (x = 0; x < w; x++) {
            uint16_t c = pixels[y * w + x];

            *r++ = (unsigned char)(((c & 31) << 3) | ((c & 31) >> 2));
            *r++ = (unsigned char)((((c >> 5) & 31) << 3) | (((c >> 5) & 31) >> 2));
            *r++ = (unsigned char)((((c >> 10) & 31) << 3) | (((c >> 10) & 31) >> 2));
        }
    }
    z[pos++] = 0x78;
    z[pos++] = 0x01;
    while (done < raw) {
        size_t n = raw - done < 65535 ? raw - done : 65535;

        z[pos++] = done + n == raw ? 1 : 0;
        z[pos++] = (unsigned char)n;
        z[pos++] = (unsigned char)(n >> 8);
        z[pos++] = (unsigned char)~n;
        z[pos++] = (unsigned char)(~n >> 8);
        memcpy(z + pos, rows + done, n);
        pos += n;
        done += n;
    }
    for (i = 0; i < raw; i++) {
        a = (a + rows[i]) % 65521;
        b = (b + a) % 65521;
    }
    Put32(z + pos, (b << 16) | a);
    pos += 4;

    f = fopen(path, "wb");
    if (f == NULL) {
        free(rows);
        free(z);
        return -1;
    }
    fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
    Put32(ihdr, (uint32_t)w);
    Put32(ihdr + 4, (uint32_t)h);
    ihdr[8] = 8;    /* bits per channel */
    ihdr[9] = 2;    /* RGB */
    ihdr[10] = ihdr[11] = ihdr[12] = 0;
    Chunk(f, "IHDR", ihdr, 13);
    Chunk(f, "IDAT", z, (uint32_t)pos);
    Chunk(f, "IEND", NULL, 0);
    fclose(f);
    free(rows);
    free(z);
    return 0;
}

/* Both screens, the upper one above the lower one. */
static void Screenshot(void)
{
    static uint16_t both[256 * 384];
    char path[1024];

    if (sScreenshotDir == NULL) {
        return;
    }
    NitroPort_RenderScreens(both, both + 256 * 192);
    snprintf(path, sizeof(path), "%s/frame_%07ld.png", sScreenshotDir, sFrame);
    if (WritePng(path, both, 256, 384) != 0) {
        NitroPort_Log("[pc] can't write %s", path);
    }
}

/* ---------------------------------------------------------------- buttons */

static int ParsePresses(const char *spec)
{
    static const struct { const char *name; uint16_t bit; } names[] = {
        { "A", PLATINUM_BUTTON_A }, { "B", PLATINUM_BUTTON_B },
        { "X", PLATINUM_BUTTON_X }, { "Y", PLATINUM_BUTTON_Y },
        { "L", PLATINUM_BUTTON_L }, { "R", PLATINUM_BUTTON_R },
        { "START", PLATINUM_BUTTON_START }, { "SELECT", PLATINUM_BUTTON_SELECT },
        { "UP", PLATINUM_KEY_UP }, { "DOWN", PLATINUM_KEY_DOWN },
        { "LEFT", PLATINUM_KEY_LEFT }, { "RIGHT", PLATINUM_KEY_RIGHT },
    };
    const char *p = spec;

    while (*p != '\0') {
        char *end;
        long frame = strtol(p, &end, 10);
        uint16_t buttons = 0;

        if (end == p || *end != ':' || sPressCount >= MAX_PRESSES) {
            return -1;
        }
        p = end + 1;
        for (;;) {
            size_t len = strcspn(p, "+,");
            size_t k;
            int found = 0;

            for (k = 0; k < sizeof(names) / sizeof(names[0]); k++) {
                if (strlen(names[k].name) == len && strncmp(p, names[k].name, len) == 0) {
                    buttons |= names[k].bit;
                    found = 1;
                }
            }
            if (!found) {
                return -1;
            }
            p += len;
            if (*p != '+') {
                break;
            }
            p++;
        }
        sPresses[sPressCount].frame = frame;
        sPresses[sPressCount].buttons = buttons;
        sPressCount++;
        if (*p == ',') {
            p++;
        }
    }
    return 0;
}

static uint16_t HeldButtons(long frame)
{
    uint16_t held = 0;
    int i;

    for (i = 0; i < sPressCount; i++) {
        if (frame >= sPresses[i].frame && frame < sPresses[i].frame + 6) {
            held |= sPresses[i].buttons;
        }
    }
    return held;
}

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
    if (sScreenshotDir != NULL && sScreenshotEvery > 0 && sFrame % sScreenshotEvery == 0) {
        Screenshot();
    }
    if (sFrameLimit >= 0 && sFrame >= sFrameLimit) {
        Screenshot();
        NitroPort_Log("[pc] stopping after %ld frames, as asked", sFrame);
        fflush(stderr);
        _exit(0);
    }
    Platinum_SetButtons(HeldButtons(sFrame));
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

static void OnInterrupt(int sig)
{
    (void)sig;
    Screenshot();
    fprintf(stderr, "[%6ld] [pc] stopped with Ctrl+C\n", sFrame);
    _exit(130);
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
            Screenshot();
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
                "[--frames N] [--stop-at-missing-file] [--stall-seconds N]\n"
                "       [--screenshots DIR] [--screenshot-every N] [--press FRAME:BUTTONS,...]\n       %s --check-overlays\n", argv[0], argv[0]);
        return 2;
    }
    for (i = 3; i < argc; i++) {
        if (strcmp(argv[i], "--save") == 0 && i + 1 < argc) {
            savePath = argv[++i];
        } else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            sFrameLimit = strtol(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--stop-at-missing-file") == 0) {
            sStopAtMissingFile = 1;
        } else if (strcmp(argv[i], "--screenshots") == 0 && i + 1 < argc) {
            sScreenshotDir = argv[++i];
        } else if (strcmp(argv[i], "--screenshot-every") == 0 && i + 1 < argc) {
            sScreenshotEvery = strtol(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--press") == 0 && i + 1 < argc) {
            if (ParsePresses(argv[++i]) != 0) {
                fprintf(stderr, "--press: expected FRAME:BUTTON[+BUTTON],... e.g. 8700:A,9000:START\n");
                return 2;
            }
        } else if (strcmp(argv[i], "--stall-seconds") == 0 && i + 1 < argc) {
            sStallSeconds = atoi(argv[++i]);
        } else {
            fprintf(stderr, "unknown option %s\n", argv[i]);
            return 2;
        }
    }

    signal(SIGINT, OnInterrupt);
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
