#ifndef NITRO_PORT_OPTIONS_H
#define NITRO_PORT_OPTIONS_H

/* The port's own settings, as the options page in the lower menu sets them
 * (the game's own six stay in its save): a whole number each, kept in a
 * text file beside the save ("name value" a line). The platform gives each
 * its default (from remaster.txt, where it has one), then loads the file;
 * a setting changed calls the platform's listener (the ones it applies
 * while the game runs: 60 FPS, smooth 3D). Game thread. */

enum {
    NITRO_OPT_FPS60,             /* 0 off, 1 on */
    NITRO_OPT_SMOOTH3D,          /* 0 off, 1 with the slider up only, 2 on */
    NITRO_OPT_START_TAB,         /* the panel when the game starts: 0 the map, 1 the Pokétch */
    NITRO_OPT_SPEED,             /* 0 1x, 1 1.5x, 2 2x, 3 3x, 4 4x */
    NITRO_OPT_EXP_ON_CATCH,      /* 0 off, 1 on (the rest too) */
    NITRO_OPT_EXP_SHARE,
    NITRO_OPT_TRADE_EVO,
    NITRO_OPT_HM_FREE,
    NITRO_OPT_AUTO_RUN,
    NITRO_OPT_ALWAYS_CATCH,
    NITRO_OPT_INSTANT_VICTORY,
    NITRO_OPT_FAST_EGGS,
    NITRO_OPT_INFINITE_MONEY,
    NITRO_OPT_ENCOUNTERS,        /* 0 normal, 1 off, 2 1/4, 3 1/2, 4 2x */
    NITRO_OPT_SHINY,             /* 0 1/8192 (the game's), 1 1/4096, 2 1/1024, 3 1/256, 4 1/64, 5 always */
    NITRO_OPT_FPS60_HELD,        /* (not on the page) 1: 60 FPS turned off by SPEED, on again at 1x */
    NITRO_OPT_ITEM2,             /* (not on the page) the lower menu's second square's item (0 none) */
    NITRO_OPT_COUNT
};

/* How many values it has (0 to that less one). */
int NitroPort_OptionValues(int id);
int NitroPort_Option(int id);
/* Changed (kept within its values): the listener told, saved by the next
 * NitroPort_OptionsSave. SPEED above 1x turns 60 FPS off (if it's on) and
 * 1x turns it on again; 60 FPS set itself is the player's choice from then
 * on (NITRO_OPT_FPS60_HELD). */
void NitroPort_OptionSet(int id, int value);
/* (the platform's, before NitroPort_OptionsLoad) */
void NitroPort_OptionSetDefault(int id, int value);
void NitroPort_OptionsSetListener(void (*listener)(int id, int value));
/* The file read (a missing one: the defaults), its path kept for saving.
 * 1 if it was read. SPEED is back at 1x after it (each time the game
 * starts), 60 FPS on again if SPEED had turned it off. */
int NitroPort_OptionsLoad(const char *path);
/* Written if anything changed since (to a new file put in its place): the
 * settings chosen on the page or read from the file, the others left to
 * their defaults. 1 if written or nothing to write. */
int NitroPort_OptionsSave(void);

/* SPEED kept at 1x for a while, whatever it's set to: the game's, where
 * it waits for the player (the battle's menus). Each who holds it has a
 * bit of its own; at 1x while any is held. */
void NitroPort_SpeedHold(unsigned bit, int on);
int NitroPort_SpeedHeld(void);

#endif
