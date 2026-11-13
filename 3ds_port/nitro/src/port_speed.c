/*
 * The options page's SPEED faster than 1x: the halts shown, the field's
 * scenes made, the screens' rate (nitro_port_speed.h). Plain C, no SDK.
 */
#include <nitro_port_speed.h>

/* A V-blank, in microseconds (59.8 a second). */
#define FRAME_MICROS 16715u
/* Faster than 1x the screens are composed every other V-blank at the
 * soonest: the same rate in a cave as in a town. (Every V-blank where it
 * fit was tried first, 2026-11-10: light maps at 60 frames a second and
 * towns at 30 is felt as the game slowing down on the way into a town,
 * and each step between the two, 28 of them in a quarter of an hour, as a
 * dip, most of them where a new map came into view with its sign.) Every
 * third when a frame takes over two V-blanks, and back when every frame
 * in half a second was composed within a V-blank and a half. */
#define EVERY_LEAST 2
#define SPAN_3_TO_2 (FRAME_MICROS * 150u / 100u)
#define EVERY_MOST 3
/* Late frames: each counts LATE_WEIGHT, a halt shown takes 1 off; a V-blank
 * more at LATE_LIMIT (a second one within 32 halts of the first: one alone,
 * a file read's, changes nothing). */
#define LATE_WEIGHT 32
#define LATE_LIMIT (LATE_WEIGHT + 1)
#define WINDOW 32            /* (halts shown: about half a second) */
#define WAIT_LEAST 240       /* (4 seconds) */
#define WAIT_MOST 1920       /* (32 seconds) */
#define TRIAL 96             /* (a second and a half) */
/* Halts without a new frame of the game's: a loop of its own (it waits for
 * V-blank itself), every scene wanted there. */
#define HALTS_A_FRAME_MOST 4
/* Frames left out in a row at most (4x, composed every third V-blank, a
 * halt a frame: 11). */
#define LEFT_OUT_MOST 16

static int sHalves = 2;
static int sCredit;
static unsigned sShown;          /* halts shown so far */
static int sHaltsThisFrame;
static int sFieldAsked;          /* (the field drew this frame) */
static int sWanted = 1;
static int sLeftOutRun;

static int sEvery = 1;           /* (read by the display thread too) */
static int sLateLevel;
static int sWindowHalts;
static unsigned sWindowSpan;     /* (the longest in the window; 0: none composed) */
static int sWindowLate;
static int sWait;                /* halts shown till a V-blank fewer may be tried */
static int sWaitNext = WAIT_LEAST;
static int sTrial;               /* halts shown left of a V-blank fewer's trial */

static void set_every(int every)
{
    __atomic_store_n(&sEvery, every, __ATOMIC_RELEASE);
}

static void governor_reset(void)
{
    set_every(1);
    sLateLevel = 0;
    sWindowHalts = 0;
    sWindowSpan = 0;
    sWindowLate = 0;
    sWait = 0;
    sWaitNext = WAIT_LEAST;
    sTrial = 0;
}

void NitroPort_SpeedReset(void)
{
    sHalves = 2;
    sCredit = 0;
    sShown = 0;
    sHaltsThisFrame = 0;
    sFieldAsked = 0;
    sWanted = 1;
    sLeftOutRun = 0;
    governor_reset();
}

int NitroPort_SpeedHalt(int halves)
{
    int shown = 0;

    if (halves < 2) {
        halves = 2;
    }
    sHalves = halves;
    if (sHaltsThisFrame < HALTS_A_FRAME_MOST + 1) {
        sHaltsThisFrame++;
    }
    if (sHaltsThisFrame > HALTS_A_FRAME_MOST) {
        sWanted = 1;
    }
    if (halves <= 2) {
        sCredit = 0;
        sWanted = 1;
        if (sEvery != 1 || sLateLevel != 0 || sWindowHalts != 0 || sWait != 0) {
            governor_reset();
        }
        shown = 1;
    } else {
        if (sEvery < EVERY_LEAST) {
            set_every(EVERY_LEAST);
        }
        sCredit += 2;
        if (sCredit >= halves) {
            sCredit -= halves;
            if (sCredit >= halves) {
                sCredit = 0;     /* (the speed turned down since) */
            }
            shown = 1;
        }
    }
    if (shown) {
        sShown++;
    }
    return shown;
}

int NitroPort_SpeedFrameBegins(int leaveOut)
{
    int halts = sHaltsThisFrame, wasField = sFieldAsked, wanted = 1;

    sHaltsThisFrame = 0;
    sFieldAsked = 0;
    if (leaveOut && wasField && sHalves > 2 && halts >= 1 && halts <= HALTS_A_FRAME_MOST) {
        int credit = sCredit, every = sEvery, i;
        unsigned shown = sShown;

        /* (its halts, as NitroPort_SpeedHalt will count them: one of them
         * shown, and that one composed for?) */
        wanted = 0;
        for (i = 0; i < halts; i++) {
            credit += 2;
            if (credit >= sHalves) {
                credit -= sHalves;
                shown++;
                if (shown % (unsigned)every == 0) {
                    wanted = 1;
                }
            }
        }
        if (!wanted && sLeftOutRun >= LEFT_OUT_MOST) {
            wanted = 1;
        }
    }
    sLeftOutRun = wanted ? 0 : sLeftOutRun + 1;
    sWanted = wanted;
    return wanted;
}

int NitroPort_SpeedSceneWanted(void)
{
    return sWanted;
}

int NitroPort_FieldSceneWanted(void)
{
    sFieldAsked = 1;
    return sWanted;
}

int NitroPort_SpeedComposeEvery(void)
{
    return __atomic_load_n(&sEvery, __ATOMIC_ACQUIRE);
}

void NitroPort_SpeedNote(int late, unsigned spanMicros)
{
    if (sHalves <= 2) {
        return;
    }
    if (sWait > 0) {
        sWait--;
    }
    if (sLateLevel > 0) {
        sLateLevel--;
    }
    if (late) {
        sLateLevel += LATE_WEIGHT;
        sWindowLate++;
    }
    if (sLateLevel >= LATE_LIMIT) {
        /* A V-blank more (at the most already: as it is). After a trial
         * that didn't hold, longer till the next. */
        if (sTrial > 0) {
            sWaitNext = sWaitNext * 2 > WAIT_MOST ? WAIT_MOST : sWaitNext * 2;
        }
        sTrial = 0;
        if (sEvery < EVERY_MOST) {
            set_every(sEvery + 1);
        }
        sWait = sWaitNext;
        sLateLevel = 0;
        sWindowHalts = 0;
        sWindowSpan = 0;
        sWindowLate = 0;
        return;
    }
    if (sTrial > 0 && --sTrial == 0) {
        sWaitNext = WAIT_LEAST;      /* (it held) */
    }
    if (spanMicros > sWindowSpan) {
        sWindowSpan = spanMicros;
    }
    if (++sWindowHalts >= WINDOW) {
        if (sEvery > EVERY_LEAST && sWindowLate == 0 && sLateLevel == 0 && sWait == 0 && sWindowSpan != 0 &&
            sWindowSpan <= SPAN_3_TO_2) {
            set_every(sEvery - 1);
            sTrial = TRIAL;
            sWait = WAIT_LEAST;
        }
        sWindowHalts = 0;
        sWindowSpan = 0;
        sWindowLate = 0;
    }
}
