/*
 * The DS's four hardware timers, and the V-count, driven by a real clock.
 *
 * Each DS timer is a 16-bit counter that counts up at the bus clock
 * (33,513,982 Hz) divided by 1, 64, 256 or 1024, reloads a set value when
 * it overflows, and can request an interrupt then (or make the next timer
 * count one up: "cascade"). NitroSDK's tick (OS_GetTick: timer 0), alarms
 * (timer 1) and the game's play-time clock (timer 3) are all built on them.
 *
 * The port can't see reads of the counter registers, so it brings the
 * counters up to date ("syncs") at the moments that matter: when the SDK
 * writes a timer (the patched OS_SetTimerCount/OS_SetTimerControl call
 * NitroPort_TimerSetReload/NitroPort_TimerSetControl), when it reads the
 * tick (the patched OS_GetTick calls NitroPort_SyncTimers), and every frame.
 * Between syncs a counter reads its last value; with a sync at least every
 * frame that's within 1/60 s.
 *
 * Overflow interrupts are never lost: if a timer would overflow again while
 * the interrupt for its previous overflow is still waiting to be taken, it
 * stops just short and counts the rest at the next sync (the 64-bit tick is
 * the counter plus the number of those interrupts). That only happens if
 * the game keeps interrupts off for longer than an overflow period (125 ms
 * at the tick's speed), or the program was suspended.
 */

#include <nitro.h>
#include <nitro_port.h>

#include "port_platform.h"

#define DS_BUS_HZ        33513982ULL
#define CYCLES_PER_LINE  2130ULL       /* 355 dots of 6 cycles */
#define LINES_PER_FRAME  263

#define TMCNT_PRESCALER  0x0003
#define TMCNT_CASCADE    0x0004
#define TMCNT_IRQ        0x0040
#define TMCNT_ENABLE     0x0080

#define REG_COUNT(i)     (*(REGType16v *)((u32)REG_TM0CNT_L_ADDR + (u32)(i) * 4))
#define REG_CONTROL(i)   (*(REGType16v *)((u32)REG_TM0CNT_H_ADDR + (u32)(i) * 4))

typedef struct {
    u16 reload;
    u16 control;
    u32 counter;
    u64 last;      /* clock (in bus cycles) the counter is up to date with */
} Timer;

static const u32 sPrescale[4] = { 1, 64, 256, 1024 };

static Timer sTimers[4];
static u64 (*sClockSource)(void);
static BOOL sClockStarted;
static u64 sClockStart;
static BOOL sVBlankSeen;
static u64 sVBlankStart;

/* ------------------------------------------------------------------ clock */

u64 NitroPort_Cycles(void)
{
    u64 ns;

    if (sClockSource != NULL) {
        return sClockSource();
    }
    if (!sClockStarted) {
        sClockStarted = TRUE;
        sClockStart = NitroPortPlatform_Nanoseconds();
    }
    ns = NitroPortPlatform_Nanoseconds() - sClockStart;
    return (ns / 1000000000ULL) * DS_BUS_HZ + (ns % 1000000000ULL) * DS_BUS_HZ / 1000000000ULL;
}

void NitroPort_SetClockSource(u64 (*source)(void))
{
    int i;
    u64 now;

    sClockSource = source;
    /* Counting continues from the new clock's "now". */
    now = NitroPort_Cycles();
    for (i = 0; i < 4; i++) {
        sTimers[i].last = now;
    }
}

/* ----------------------------------------------------------------- timers */

/* Count `counts` more on timer i. Returns how many were counted (fewer if
 * it stopped short of an overflow whose interrupt is still pending). */
static u64 Count(int i, u64 counts)
{
    Timer *t = &sTimers[i];
    u32 irq = (u32)OS_IE_TIMER0 << i;
    BOOL wantsIrq = (t->control & TMCNT_IRQ) && (reg_OS_IE & irq);
    u64 used = 0;

    while (counts > 0) {
        u32 toOverflow = 0x10000 - t->counter;

        if (counts < toOverflow) {
            t->counter += (u32)counts;
            used += counts;
            break;
        }
        if (wantsIrq && (reg_OS_IF & irq)) {
            /* The last overflow hasn't been handled: wait just before the
             * next one. */
            t->counter += toOverflow - 1;
            used += toOverflow - 1;
            break;
        }
        counts -= toOverflow;
        used += toOverflow;
        t->counter = t->reload;
        if (t->control & TMCNT_IRQ) {
            reg_OS_IF |= irq;
        }
        if (i < 3 && (sTimers[i + 1].control & (TMCNT_ENABLE | TMCNT_CASCADE)) ==
                         (TMCNT_ENABLE | TMCNT_CASCADE)) {
            (void)Count(i + 1, 1);
        }
        if (!wantsIrq && counts > 0) {
            /* Nobody counts these interrupts: skip whole periods at once. */
            u64 period = 0x10000 - t->reload;
            u64 whole = counts / period;

            if (whole > 0 && i < 3 &&
                (sTimers[i + 1].control & (TMCNT_ENABLE | TMCNT_CASCADE)) ==
                    (TMCNT_ENABLE | TMCNT_CASCADE)) {
                (void)Count(i + 1, whole);
            }
            counts -= whole * period;
            used += whole * period;
        }
    }
    REG_COUNT(i) = (u16)t->counter;
    return used;
}

static void Advance(int i, u64 now)
{
    Timer *t = &sTimers[i];
    u32 div;
    u64 counts;

    if (!(t->control & TMCNT_ENABLE) || (t->control & TMCNT_CASCADE) || now <= t->last) {
        return;
    }
    div = sPrescale[t->control & TMCNT_PRESCALER];
    counts = (now - t->last) / div;
    if (counts > 0) {
        t->last += Count(i, counts) * div;
    }
}

void NitroPort_SyncTimers(void)
{
    u64 now = NitroPort_Cycles();
    int i;

    for (i = 0; i < 4; i++) {
        Advance(i, now);
    }
    if (sVBlankSeen) {
        u64 line = (now - sVBlankStart) / CYCLES_PER_LINE;

        reg_GX_VCOUNT = (u16)((192 + line) % LINES_PER_FRAME);
    }
}

void NitroPort_TimerSetReload(int id, u16 count)
{
    if (id < 0 || id > 3) {
        return;
    }
    NitroPort_SyncTimers();
    /* Writing TMxCNT_L sets the reload value; reading it gives the count. */
    sTimers[id].reload = count;
}

void NitroPort_TimerSetControl(int id, u16 control)
{
    Timer *t;
    u16 old;

    if (id < 0 || id > 3) {
        return;
    }
    NitroPort_SyncTimers();
    t = &sTimers[id];
    old = t->control;
    t->control = control;
    REG_CONTROL(id) = control;
    if (!(old & TMCNT_ENABLE) && (control & TMCNT_ENABLE)) {
        /* Starting: the counter takes the reload value. */
        t->counter = t->reload;
        t->last = NitroPort_Cycles();
        REG_COUNT(id) = (u16)t->counter;
    }
}

/* ---------------------------------------------------------------- V-blank */

void NitroPort_VBlankStarts(void)
{
    sVBlankSeen = TRUE;
    sVBlankStart = NitroPort_Cycles();
    NitroPort_SyncTimers();
    reg_GX_VCOUNT = 192;
}
