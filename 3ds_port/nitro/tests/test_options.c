/*
 * Tests for the port's own settings (src/port_options.c): defaults, values
 * kept within range, the listener told of changes only, a file saved and
 * read back, unknown and bad lines skipped, nothing written when nothing
 * changed.
 */
#include <nitro_port_options.h>
#include <stdio.h>
#include <string.h>

static int sChecks, sFailures;
#define CHECK(cond) do { sChecks++; if (!(cond)) { sFailures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

void NitroPort_Log(const char *fmt, ...)
{
    (void)fmt;
}

static int sHeardId = -1, sHeardValue = -1, sHeard;

static void listener(int id, int value)
{
    sHeardId = id;
    sHeardValue = value;
    sHeard++;
}

int main(void)
{
    const char *path = "test_options.tmp";
    FILE *f;
    char line[64];
    int lines = 0;

    remove(path);

    /* Defaults, and the platform's. */
    CHECK(NitroPort_Option(NITRO_OPT_FPS60) == 1);
    CHECK(NitroPort_Option(NITRO_OPT_SMOOTH3D) == 2);
    CHECK(NitroPort_Option(NITRO_OPT_START_TAB) == 0);
    CHECK(NitroPort_OptionValues(NITRO_OPT_SHINY) == 6);
    CHECK(NitroPort_OptionValues(NITRO_OPT_COUNT) == 0);
    NitroPort_OptionSetDefault(NITRO_OPT_FPS60, 0);
    CHECK(NitroPort_Option(NITRO_OPT_FPS60) == 0);

    /* No file: the defaults stay; nothing to save. */
    CHECK(NitroPort_OptionsLoad(path) == 0);
    CHECK(NitroPort_Option(NITRO_OPT_FPS60) == 0);
    CHECK(NitroPort_OptionsSave() == 1);
    CHECK(fopen(path, "r") == NULL);

    /* Changes: within range, the listener told once each. */
    NitroPort_OptionsSetListener(listener);
    NitroPort_OptionSet(NITRO_OPT_SPEED, 9);
    CHECK(NitroPort_Option(NITRO_OPT_SPEED) == 4);
    CHECK(sHeard == 1 && sHeardId == NITRO_OPT_SPEED && sHeardValue == 4);
    NitroPort_OptionSet(NITRO_OPT_SPEED, 4);
    CHECK(sHeard == 1);
    NitroPort_OptionSet(NITRO_OPT_START_TAB, 1);
    NitroPort_OptionSet(NITRO_OPT_SHINY, -3);
    CHECK(NitroPort_Option(NITRO_OPT_SHINY) == 0);
    CHECK(sHeard == 2);
    NitroPort_OptionSet(NITRO_OPT_COUNT, 1);
    CHECK(sHeard == 2);

    /* Saved: every setting a line. */
    CHECK(NitroPort_OptionsSave() == 1);
    f = fopen(path, "r");
    CHECK(f != NULL);
    if (f) {
        while (fgets(line, sizeof(line), f)) {
            lines++;
        }
        fclose(f);
    }
    CHECK(lines == 2);   /* (only the two chosen: speed, start_tab; shiny stayed at its default) */

    /* Read back over other values (the listener not told: loading). */
    NitroPort_OptionSetDefault(NITRO_OPT_SPEED, 0);
    NitroPort_OptionSetDefault(NITRO_OPT_START_TAB, 0);
    CHECK(NitroPort_OptionsLoad(path) == 1);
    CHECK(NitroPort_Option(NITRO_OPT_SPEED) == 4);
    CHECK(NitroPort_Option(NITRO_OPT_START_TAB) == 1);
    CHECK(NitroPort_Option(NITRO_OPT_FPS60) == 0);   /* (not in the file: the platform's default) */
    CHECK(sHeard == 2);

    /* The power off mid-save: only the new file there. */
    {
        char tmp[64];

        snprintf(tmp, sizeof(tmp), "%s.new", path);
        CHECK(rename(path, tmp) == 0);
        NitroPort_OptionSetDefault(NITRO_OPT_SPEED, 0);
        CHECK(NitroPort_OptionsLoad(path) == 1);
        CHECK(NitroPort_Option(NITRO_OPT_SPEED) == 4);
        remove(tmp);
    }

    /* Unknown names, bad lines and values out of range. */
    f = fopen(path, "w");
    CHECK(f != NULL);
    if (f) {
        fputs("speed 2\nnonsense 5\nshiny\n\nencounters 77\n", f);
        fclose(f);
    }
    CHECK(NitroPort_OptionsLoad(path) == 1);
    CHECK(NitroPort_Option(NITRO_OPT_SPEED) == 2);
    CHECK(NitroPort_Option(NITRO_OPT_ENCOUNTERS) == 4);

    /* SPEED above 1x turns 60 FPS off, 1x on again; set by the player in
     * between, it stays as set (and with 60 FPS off to begin with, SPEED
     * leaves it). Kept in the file. */
    NitroPort_OptionSet(NITRO_OPT_SPEED, 0);
    NitroPort_OptionSet(NITRO_OPT_FPS60, 1);
    NitroPort_OptionSet(NITRO_OPT_SPEED, 4);
    CHECK(NitroPort_Option(NITRO_OPT_FPS60) == 0 && NitroPort_Option(NITRO_OPT_FPS60_HELD) == 1);
    NitroPort_OptionSet(NITRO_OPT_SPEED, 2);
    CHECK(NitroPort_Option(NITRO_OPT_FPS60) == 0 && NitroPort_Option(NITRO_OPT_FPS60_HELD) == 1);
    CHECK(NitroPort_OptionsSave() == 1);
    NitroPort_OptionSetDefault(NITRO_OPT_FPS60_HELD, 0);
    CHECK(NitroPort_OptionsLoad(path) == 1);
    CHECK(NitroPort_Option(NITRO_OPT_FPS60_HELD) == 1);
    NitroPort_OptionSet(NITRO_OPT_SPEED, 0);
    CHECK(NitroPort_Option(NITRO_OPT_FPS60) == 1 && NitroPort_Option(NITRO_OPT_FPS60_HELD) == 0);
    NitroPort_OptionSet(NITRO_OPT_SPEED, 3);
    CHECK(NitroPort_Option(NITRO_OPT_FPS60) == 0);
    NitroPort_OptionSet(NITRO_OPT_FPS60, 1);   /* (the player's: on till SPEED changes) */
    CHECK(NitroPort_Option(NITRO_OPT_FPS60) == 1 && NitroPort_Option(NITRO_OPT_FPS60_HELD) == 0);
    NitroPort_OptionSet(NITRO_OPT_SPEED, 1);   /* (still above 1x: off again, held) */
    CHECK(NitroPort_Option(NITRO_OPT_FPS60) == 0 && NitroPort_Option(NITRO_OPT_FPS60_HELD) == 1);
    NitroPort_OptionSet(NITRO_OPT_SPEED, 0);
    CHECK(NitroPort_Option(NITRO_OPT_FPS60) == 1);
    NitroPort_OptionSet(NITRO_OPT_FPS60, 0);   /* (off by the player: stays off at 1x) */
    NitroPort_OptionSet(NITRO_OPT_SPEED, 0);
    CHECK(NitroPort_Option(NITRO_OPT_FPS60) == 0);
    NitroPort_OptionSet(NITRO_OPT_SPEED, 4);
    NitroPort_OptionSet(NITRO_OPT_SPEED, 0);
    CHECK(NitroPort_Option(NITRO_OPT_FPS60) == 0);

    remove(path);
    printf("options: %d checks, %d failures\n", sChecks, sFailures);
    return sFailures ? 1 : 0;
}
