/*
 * What the port needs from the machine it runs on: threads, a way for one
 * thread to wake another, and a clock. Two implementations:
 *
 *   src/platform/host.c   the PC (POSIX threads), for the tests
 *   src/platform/3ds.c    the 3DS (libctru)
 *
 * The Makefile compiles the one for TARGET. Private to libnitro.
 */

#ifndef NITRO_PORT_PLATFORM_H_
#define NITRO_PORT_PLATFORM_H_

/* Plain C types only: the 3DS implementation includes libctru's headers,
 * whose u32 & co. would clash with NitroSDK's. */
#include <stdint.h>

/* A wake-up call between two threads: Post lets one Wait return. A Post
 * with nobody waiting is kept until the next Wait (a counting semaphore
 * that never goes above 1 in the way the port uses it). Created once and
 * never freed. */
typedef struct NitroPortSignal NitroPortSignal;

NitroPortSignal *NitroPortPlatform_NewSignal(void);
void NitroPortPlatform_Wait(NitroPortSignal *signal);
void NitroPortPlatform_Post(NitroPortSignal *signal);

/* Start a thread of the machine running entry(arg), with at least
 * stackSize bytes of stack. Returns 0 if it couldn't. The thread is
 * detached: it ends with NitroPortPlatform_ExitThread. */
int NitroPortPlatform_StartThread(void (*entry)(void *), void *arg, uint32_t stackSize);

/* End the calling thread (one started with NitroPortPlatform_StartThread). */
void NitroPortPlatform_ExitThread(void) __attribute__((noreturn));

/* A clock that only goes forward, in nanoseconds since some fixed point. */
uint64_t NitroPortPlatform_Nanoseconds(void);

#endif
