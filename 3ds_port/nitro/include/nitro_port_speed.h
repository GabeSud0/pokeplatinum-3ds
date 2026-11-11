#ifndef NITRO_PORT_SPEED_H
#define NITRO_PORT_SPEED_H

/* The options page's SPEED faster than 1x: which of the game's halts are
 * shown, which of the field's frames make their 3D scene, and how often
 * the screens are composed.
 *
 * Faster than 1x the game runs several frames a V-blank and only some are
 * shown. A frame of the field that isn't shown still made its whole 3D
 * scene: the game's thread sent the ground and the buildings, the geometry
 * thread worked them out, and the display thread, below the game on its
 * core, was left with what time there was. In a town at 4x (Sandgem, a log
 * of 2026-11-10) that was 12 ms of game a V-blank, the screens composed 20
 * times a second from scenes chosen by whichever the geometry thread had
 * time for, and the game between 150 and 240 frames a second. A frame that
 * won't be shown doesn't send its ground or buildings now, and what it
 * does send is left out (NitroPort_Gx3dLeaveOut): the scenes made are the
 * ones shown, evenly.
 *
 * The platform calls NitroPort_SpeedHalt at each of the game's halts and
 * NitroPort_SpeedFrameBegins when the game's V-blank work is done (a frame
 * of its begins); the field asks NitroPort_FieldSceneWanted before it
 * draws. All on the game's thread, but NitroPort_SpeedComposeEvery (any
 * thread). Without a platform calling them (the tests, the PC runner at
 * 1x) every frame is shown and every scene wanted. */

/* A halt: the game waits for V-blank. halves: the speed, in halves of the
 * game's frames for each one shown (2: 1x, 3: 1.5x, 4: 2x, 6: 3x, 8: 4x).
 * 1: this halt is shown (drawn, and waited for). */
int NitroPort_SpeedHalt(int halves);

/* A frame of the game's begins. leaveOut: scenes can be left out (the
 * geometry has its thread). 1: its 3D scene is wanted; 0: it won't be
 * shown (the platform leaves it out, NitroPort_Gx3dLeaveOut). Wanted
 * unless the frame before was the field's (it asked), the speed is above
 * 1x, and none of its halts is one the screens are composed for (as the
 * frame before's halts went: two a frame in the field). */
int NitroPort_SpeedFrameBegins(int leaveOut);

/* This frame's (as NitroPort_SpeedFrameBegins said; wanted again when the
 * game halts on without a new frame: a loop of its own). */
int NitroPort_SpeedSceneWanted(void);

/* The field, before it draws: the same, and from the next frame on the
 * field's frames are the ones left out. */
int NitroPort_FieldSceneWanted(void);

/* V-blanks from one composed frame to the next. 1 at 1x. Faster than 1x:
 * 2 everywhere (30 frames a second, the same in a cave as in a town), or
 * 3 while frames take over two V-blanks. */
int NitroPort_SpeedComposeEvery(void);

/* After each halt shown faster than 1x. late: a frame was late since the
 * note before (not composed by the V-blank the next one is due: at every
 * other V-blank a frame has two); spanMicros: the longest a frame took
 * from its V-blank to composed since then (0: none composed).
 *
 * Two late within about half a second: every third V-blank, and with that
 * fewer of the field's scenes (one late alone, a file read's, changes
 * nothing). Half a second with none late and every frame composed within
 * a V-blank and a half: every other again, 4 seconds after the last step
 * at the soonest; if that turns out late again within a second and a
 * half, the wait doubles (to 32 seconds at most). */
void NitroPort_SpeedNote(int late, unsigned spanMicros);

/* As at the start (for the tests). */
void NitroPort_SpeedReset(void);

#endif
