/*
 * The game on a PC, without a screen: for finding out how far it gets, and
 * where it stops, faster than on a 3DS.
 *
 *   pokeplatinum-pc <platinum3ds.pak> <manifest.json> [--frames N]
 *                   [--stop-at-missing-file]
 *   pokeplatinum-pc --check-overlays
 *
 * Runs the same game code as the 3DS program, with the same data pack, as
 * fast as the PC goes. Everything the port logs goes to stderr, each line
 * with the frame number. With --frames N it stops after N frames (exit
 * status 0); with --stop-at-missing-file, at the first file the game asks
 * for that the pack doesn't have (exit status 5). If the game crashes, the
 * crash is reported with a backtrace.
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
#include <execinfo.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <nitro.h>
#include <nitro_port.h>

#include "platinum.h"

static long sFrame;
static long sFrameLimit = -1;
static int sStopAtMissingFile;

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

int main(int argc, char **argv)
{
    PlatinumSettings settings;
    const char *error;
    int i;

    if (argc == 2 && strcmp(argv[1], "--check-overlays") == 0) {
        return Platinum_CheckOverlays() == 0 ? 0 : 1;
    }
    if (argc < 3) {
        fprintf(stderr, "usage: %s <platinum3ds.pak> <manifest.json> [--frames N] "
                "[--stop-at-missing-file]\n       %s --check-overlays\n", argv[0], argv[0]);
        return 2;
    }
    for (i = 3; i < argc; i++) {
        if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            sFrameLimit = strtol(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--stop-at-missing-file") == 0) {
            sStopAtMissingFile = 1;
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

    Platinum_DefaultSettings(&settings);
    Platinum_Run(&settings);
}
