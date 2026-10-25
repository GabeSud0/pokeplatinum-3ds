/*
 * The port's own settings (nitro_port_options.h): the options page's 60 FPS,
 * smooth 3D, start tab, enhancements and cheats, kept in a text file beside
 * the save, a "name value" line each.
 */

#include "nitro_port_options.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern void NitroPort_Log(const char *fmt, ...);

static const struct {
    const char *name;
    int values;
    int initial;
} sDefs[NITRO_OPT_COUNT] = {
    [NITRO_OPT_FPS60] = { "fps60", 2, 1 },
    [NITRO_OPT_SMOOTH3D] = { "smooth3d", 3, 2 },
    [NITRO_OPT_START_TAB] = { "start_tab", 2, 0 },
    [NITRO_OPT_SPEED] = { "speed", 5, 0 },
    [NITRO_OPT_EXP_ON_CATCH] = { "exp_on_catch", 2, 0 },
    [NITRO_OPT_EXP_SHARE] = { "exp_share", 2, 0 },
    [NITRO_OPT_TRADE_EVO] = { "trade_evo", 2, 0 },
    [NITRO_OPT_HM_FREE] = { "hm_free", 2, 0 },
    [NITRO_OPT_AUTO_RUN] = { "auto_run", 2, 0 },
    [NITRO_OPT_ALWAYS_CATCH] = { "always_catch", 2, 0 },
    [NITRO_OPT_INSTANT_VICTORY] = { "instant_victory", 2, 0 },
    [NITRO_OPT_FAST_EGGS] = { "fast_eggs", 2, 0 },
    [NITRO_OPT_INFINITE_MONEY] = { "infinite_money", 2, 0 },
    [NITRO_OPT_ENCOUNTERS] = { "encounters", 5, 0 },
    [NITRO_OPT_SHINY] = { "shiny", 6, 0 },
};

static int sValue[NITRO_OPT_COUNT];
static int sReady;
static int sDirty;
static char sPath[256];
static void (*sListener)(int id, int value);

static void ready(void)
{
    int i;

    if (!sReady) {
        for (i = 0; i < NITRO_OPT_COUNT; i++) {
            sValue[i] = sDefs[i].initial;
        }
        sReady = 1;
    }
}

static int clamp(int id, int value)
{
    return value < 0 ? 0 : value >= sDefs[id].values ? sDefs[id].values - 1 : value;
}

int NitroPort_OptionValues(int id)
{
    return id >= 0 && id < NITRO_OPT_COUNT ? sDefs[id].values : 0;
}

int NitroPort_Option(int id)
{
    ready();
    return id >= 0 && id < NITRO_OPT_COUNT ? sValue[id] : 0;
}

void NitroPort_OptionSet(int id, int value)
{
    ready();
    if (id < 0 || id >= NITRO_OPT_COUNT) {
        return;
    }
    value = clamp(id, value);
    if (value != sValue[id]) {
        sValue[id] = value;
        sDirty = 1;
        if (sListener) {
            sListener(id, value);
        }
    }
}

void NitroPort_OptionSetDefault(int id, int value)
{
    ready();
    if (id >= 0 && id < NITRO_OPT_COUNT) {
        sValue[id] = clamp(id, value);
    }
}

void NitroPort_OptionsSetListener(void (*listener)(int id, int value))
{
    sListener = listener;
}

int NitroPort_OptionsLoad(const char *path)
{
    FILE *f;
    char line[96];
    int read = 0;

    ready();
    sDirty = 0;
    if (path == NULL || strlen(path) >= sizeof(sPath)) {
        sPath[0] = 0;
        return 0;
    }
    strcpy(sPath, path);
    f = fopen(path, "r");
    if (f == NULL) {
        return 0;
    }
    while (fgets(line, sizeof(line), f)) {
        char name[48];
        int value, i;

        if (sscanf(line, "%47s %d", name, &value) != 2) {
            continue;
        }
        for (i = 0; i < NITRO_OPT_COUNT; i++) {
            if (strcmp(name, sDefs[i].name) == 0) {
                sValue[i] = clamp(i, value);
                read++;
            }
        }
    }
    fclose(f);
    NitroPort_Log("[options] %d settings read from %s", read, path);
    return 1;
}

int NitroPort_OptionsSave(void)
{
    char tmp[sizeof(sPath) + 8];
    FILE *f;
    int i, ok;

    if (!sDirty) {
        return 1;
    }
    if (sPath[0] == 0) {
        return 0;
    }
    snprintf(tmp, sizeof(tmp), "%s.new", sPath);
    f = fopen(tmp, "w");
    if (f == NULL) {
        NitroPort_Log("[options] can't write %s", tmp);
        return 0;
    }
    for (i = 0; i < NITRO_OPT_COUNT; i++) {
        fprintf(f, "%s %d\n", sDefs[i].name, sValue[i]);
    }
    ok = fflush(f) == 0;
    ok = fclose(f) == 0 && ok;
    if (ok) {
        remove(sPath);   /* (some file systems won't rename over a file) */
        ok = rename(tmp, sPath) == 0;
    }
    if (!ok) {
        NitroPort_Log("[options] couldn't save %s", sPath);
        return 0;
    }
    sDirty = 0;
    NitroPort_Log("[options] saved to %s", sPath);
    return 1;
}
