/*
 * Tests for the pacing faster than 1x (src/port_speed.c): the halts shown
 * at each speed, the field's frames whose scene is wanted (the ones with a
 * halt the screens are composed for), every scene wanted outside the
 * field, at 1x and without the geometry thread, and the screens' rate:
 * every other V-blank above 1x, every third while frames come late, back
 * when they're composed early enough, not back and forth.
 */
#include <nitro_port_speed.h>
#include <stdio.h>

static int sChecks, sFailures;
#define CHECK(cond) do { sChecks++; if (!(cond)) { sFailures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

/* `frames` of the game's frames, `halts` halts each, the field drawing in
 * each if `field`: how many scenes were wanted, halts shown, and the
 * longest run of frames left out. */
static int sShownHalts, sLongestRun;

static int run_frames(int frames, int halts, int halves, int field, int leaveOut)
{
    int f, h, wanted = 0, run = 0;

    sShownHalts = 0;
    sLongestRun = 0;
    for (f = 0; f < frames; f++) {
        int w = NitroPort_SpeedFrameBegins(leaveOut);

        CHECK(w == NitroPort_SpeedSceneWanted());
        if (field) {
            CHECK(NitroPort_FieldSceneWanted() == w);
        }
        wanted += w;
        run = w ? 0 : run + 1;
        if (run > sLongestRun) {
            sLongestRun = run;
        }
        for (h = 0; h < halts; h++) {
            sShownHalts += NitroPort_SpeedHalt(halves);
        }
    }
    return wanted;
}

/* A frame left out has no halt the screens are composed for (every other
 * one shown); a wanted one has (the field at 4x, two halts a frame). */
static void check_wanted_frames_have_the_shown_halt(void)
{
    int f, h, shownSoFar = 0, wanted = 0;

    NitroPort_SpeedReset();
    run_frames(4, 2, 8, 1, 1);
    for (f = 0; f < 40; f++) {
        int w = NitroPort_SpeedFrameBegins(1), shown = 0;

        NitroPort_FieldSceneWanted();
        for (h = 0; h < 2; h++) {
            shown += NitroPort_SpeedHalt(8);
        }
        CHECK(!w || shown != 0);
        shownSoFar += shown;
        wanted += w;
    }
    CHECK(shownSoFar == 20 && wanted == 10);
}

static void note(int count, int late, unsigned span)
{
    int i;

    for (i = 0; i < count; i++) {
        NitroPort_SpeedNote(late, span);
    }
}

int main(void)
{
    /* Nothing calling it: every scene wanted, composed every V-blank. */
    CHECK(NitroPort_SpeedSceneWanted() == 1);
    CHECK(NitroPort_FieldSceneWanted() == 1);
    CHECK(NitroPort_SpeedComposeEvery() == 1);

    /* 1x: every halt shown, every scene wanted. */
    NitroPort_SpeedReset();
    CHECK(run_frames(60, 2, 2, 1, 1) == 60);
    CHECK(sShownHalts == 120);

    /* The halts shown: 2 of 3 at 1.5x, every other at 2x, every third at
     * 3x, every fourth at 4x. */
    NitroPort_SpeedReset();
    run_frames(60, 2, 3, 0, 1);
    CHECK(sShownHalts == 80);
    run_frames(60, 2, 4, 0, 1);
    CHECK(sShownHalts == 60);
    run_frames(60, 2, 6, 0, 1);
    CHECK(sShownHalts == 40);
    run_frames(60, 2, 8, 0, 1);
    CHECK(sShownHalts == 30);

    /* The field (two halts a frame), the screens composed for every other
     * halt shown: a scene for each of those. 1.5x: 40 of 60 frames'
     * scenes (two in three); 2x: every other; 3x: one in three; 4x: one in
     * four. */
    NitroPort_SpeedReset();
    run_frames(4, 2, 3, 1, 1);
    CHECK(NitroPort_SpeedComposeEvery() == 2);
    CHECK(run_frames(60, 2, 3, 1, 1) == 40);
    run_frames(4, 2, 4, 1, 1);
    CHECK(run_frames(60, 2, 4, 1, 1) == 30);
    CHECK(sLongestRun == 1);
    run_frames(6, 2, 6, 1, 1);
    CHECK(run_frames(60, 2, 6, 1, 1) == 20);
    CHECK(sLongestRun == 2);
    run_frames(4, 2, 8, 1, 1);
    CHECK(run_frames(60, 2, 8, 1, 1) == 15);
    CHECK(sLongestRun == 3);
    check_wanted_frames_have_the_shown_halt();

    /* Not the field's frames (a battle, a menu: nothing asked), or no
     * geometry thread to leave scenes out: every scene wanted at 4x. */
    NitroPort_SpeedReset();
    run_frames(4, 1, 8, 0, 1);
    CHECK(run_frames(60, 1, 8, 0, 1) == 60);
    run_frames(4, 2, 8, 1, 0);
    CHECK(run_frames(60, 2, 8, 1, 0) == 60);

    /* The first frame of the field after something else is wanted (the
     * frame before didn't ask). */
    NitroPort_SpeedReset();
    run_frames(8, 2, 8, 0, 1);
    CHECK(NitroPort_SpeedFrameBegins(1) == 1);

    /* The game halting on without a new frame (a loop of its own): the
     * scene wanted again, whatever the frame was. */
    NitroPort_SpeedReset();
    run_frames(4, 2, 8, 1, 1);
    while (NitroPort_SpeedFrameBegins(1)) {
        NitroPort_FieldSceneWanted();
        NitroPort_SpeedHalt(8);
        NitroPort_SpeedHalt(8);
    }
    CHECK(NitroPort_SpeedSceneWanted() == 0);
    {
        int i;

        for (i = 0; i < 4; i++) {
            NitroPort_SpeedHalt(8);
        }
        CHECK(NitroPort_SpeedSceneWanted() == 0);
        NitroPort_SpeedHalt(8);
        CHECK(NitroPort_SpeedSceneWanted() == 1);
    }

    /* Back at 1x in the middle (the start menu holds it): wanted from the
     * first halt at 1x on (the frame it falls in may have been left out:
     * NitroPort_Gx3dLeaveOut leaves that one out whole). */
    NitroPort_SpeedReset();
    run_frames(5, 2, 8, 1, 1);
    NitroPort_SpeedFrameBegins(1);
    NitroPort_FieldSceneWanted();
    NitroPort_SpeedHalt(2);
    CHECK(NitroPort_SpeedSceneWanted() == 1);
    NitroPort_SpeedHalt(2);
    CHECK(run_frames(10, 2, 2, 1, 1) == 10);

    /* The screens' rate: every other V-blank faster than 1x, whatever the
     * frames take, as long as they're on time. One late frame now and
     * then changes nothing. */
    NitroPort_SpeedReset();
    CHECK(NitroPort_SpeedComposeEvery() == 1);
    run_frames(4, 2, 8, 1, 1);
    CHECK(NitroPort_SpeedComposeEvery() == 2);
    note(2000, 0, 3000);
    CHECK(NitroPort_SpeedComposeEvery() == 2);
    note(1, 1, 0);
    note(40, 0, 12000);
    note(1, 1, 0);
    note(40, 0, 12000);
    CHECK(NitroPort_SpeedComposeEvery() == 2);
    /* Two close together: every third (at 4x one scene in six), and no
     * further. */
    note(1, 1, 0);
    note(5, 0, 12000);
    note(1, 1, 0);
    CHECK(NitroPort_SpeedComposeEvery() == 3);
    run_frames(6, 2, 8, 1, 1);
    CHECK(run_frames(60, 2, 8, 1, 1) == 10);
    CHECK(sLongestRun == 5);
    note(6, 1, 0);
    CHECK(NitroPort_SpeedComposeEvery() == 3);

    /* On time but not within a V-blank and a half, or nothing composed at
     * all: it stays. */
    note(2016, 0, 26000);
    CHECK(NitroPort_SpeedComposeEvery() == 3);
    note(320, 0, 0);
    CHECK(NitroPort_SpeedComposeEvery() == 3);
    /* Early enough for half a second (one frame that isn't keeps it):
     * every other again, and never every one. */
    note(31, 0, 20000);
    note(1, 0, 26000);
    CHECK(NitroPort_SpeedComposeEvery() == 3);
    note(31, 0, 20000);
    CHECK(NitroPort_SpeedComposeEvery() == 3);
    note(1, 0, 20000);
    CHECK(NitroPort_SpeedComposeEvery() == 2);
    note(2000, 0, 2000);
    CHECK(NitroPort_SpeedComposeEvery() == 2);

    /* Back to every other that doesn't hold: every third again, and longer
     * till the next try each time (4 seconds, then 8). */
    NitroPort_SpeedReset();
    run_frames(4, 2, 8, 1, 1);
    note(2, 1, 0);
    CHECK(NitroPort_SpeedComposeEvery() == 3);
    note(240, 0, 9000);
    note(32, 0, 9000);
    CHECK(NitroPort_SpeedComposeEvery() == 2);
    note(2, 1, 0);                    /* (late again at once) */
    CHECK(NitroPort_SpeedComposeEvery() == 3);
    note(448, 0, 9000);               /* (7 seconds: not yet) */
    CHECK(NitroPort_SpeedComposeEvery() == 3);
    note(64, 0, 9000);
    CHECK(NitroPort_SpeedComposeEvery() == 2);
    /* One that holds: the next wait is the short one again. */
    note(200, 0, 12000);
    note(2, 1, 0);
    CHECK(NitroPort_SpeedComposeEvery() == 3);
    note(288, 0, 9000);
    CHECK(NitroPort_SpeedComposeEvery() == 2);

    /* At 1x: every V-blank again, and nothing noted. */
    NitroPort_SpeedReset();
    run_frames(4, 2, 8, 1, 1);
    note(2, 1, 0);
    CHECK(NitroPort_SpeedComposeEvery() == 3);
    NitroPort_SpeedHalt(2);
    CHECK(NitroPort_SpeedComposeEvery() == 1);
    note(10, 1, 0);
    CHECK(NitroPort_SpeedComposeEvery() == 1);
    /* (and above 1x again: every other from its first halt) */
    NitroPort_SpeedHalt(4);
    CHECK(NitroPort_SpeedComposeEvery() == 2);

    printf("speed: %d checks, %d failures\n", sChecks, sFailures);
    return sFailures ? 1 : 0;
}
