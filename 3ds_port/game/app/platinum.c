/*
 * The game's start: see platinum.h.
 */

#include "platinum.h"

#include <nitro.h>
#include <nitro_port.h>
#include <nitro_port_fs.h>
#include <stdlib.h>
#include <string.h>

extern const NitroPortOverlay gPlatinumOverlays[];   /* gen/overlay_table.c */
extern const u32 gPlatinumOverlayCount;

void NitroMain(void);

/* The MAC address the game sees. The game stores it in the save and
 * compares it on loading (a different DS): one fixed address keeps a save
 * "from this DS" on every console. 00:09:BF is one of Nintendo's prefixes. */
static const u8 sMacAddress[6] = { 0x00, 0x09, 0xBF, 0x50, 0x33, 0xD5 };

void Platinum_DefaultSettings(PlatinumSettings *settings)
{
    static const char name[] = "Player";
    u32 i;

    memset(settings, 0, sizeof(*settings));
    for (i = 0; name[i] != '\0'; i++) {
        settings->nickname[i] = (u16)name[i];
    }
    settings->language = NVRAM_CONFIG_LANG_ENGLISH;
    settings->birthMonth = 1;
    settings->birthDay = 1;
    settings->favoriteColor = 0;
}

const char *Platinum_OpenPak(const char *pakPath, const char *manifestPath)
{
    switch (NitroPort_OpenPak(pakPath, manifestPath, PLATINUM_PAK_BUILD_ID)) {
    case PAK_OK:
        return NULL;
    case PAK_ERR_IO:
        return "can't read the data pack or its manifest.json";
    case PAK_ERR_FORMAT:
        return "the data pack or manifest.json is damaged, or they don't belong together";
    case PAK_ERR_VERSION:
        return "the data pack was made by a different version of the builder";
    case PAK_ERR_BUILD_ID:
        return "the data pack has the wrong build ID; pack it again with --build-id "
               PLATINUM_PAK_BUILD_ID;
    case PAK_ERR_ROM_SHA1:
        return "the data pack was made from a ROM this version doesn't support";
    case PAK_ERR_MEMORY:
        return "out of memory opening the data pack";
    default:
        return "couldn't open the data pack";
    }
}

void Platinum_SetButtons(uint16_t held)
{
    NitroPort_SetButtons((u16)(held & (PAD_PLUS_KEY_MASK | PAD_BUTTON_MASK)), FALSE);
}

void Platinum_SetTouch(int down, int x, int y)
{
    if (x < 0) {
        x = 0;
    } else if (x > 255) {
        x = 255;
    }
    if (y < 0) {
        y = 0;
    } else if (y > 191) {
        y = 191;
    }
    NitroPort_SetTouch(down ? TRUE : FALSE, (u16)x, (u16)y);
}

void Platinum_Run(const PlatinumSettings *settings)
{
    CARDRomHeader header;
    NitroPortFirmware firmware;

    /* The cartridge header, as far as the game and the SDK read it: the
     * game checks the maker code at boot (boot.c). The rest of a real
     * header (offsets into the cartridge) means nothing here: files come
     * from the data pack. */
    memset(&header, 0, sizeof(header));
    memcpy(header.game_name, "POKEMON PL\0\0", 12);
    header.game_code = 'C' | ('P' << 8) | ('U' << 16) | ((u32)'E' << 24);
    header.maker_code = '0' | ('1' << 8);
    header.device_size = 10;   /* 128 MB */
    header.game_version = 1;

    memset(&firmware, 0, sizeof(firmware));
    firmware.romHeader = &header;
    firmware.romHeaderSize = sizeof(header);
    firmware.nickname = settings->nickname;
    firmware.language = settings->language;
    firmware.birthMonth = settings->birthMonth;
    firmware.birthDay = settings->birthDay;
    firmware.favoriteColor = settings->favoriteColor;
    memcpy(firmware.macAddress, sMacAddress, sizeof(sMacAddress));
    firmware.rtcOffset = 0;
    NitroPort_BootFirmware(&firmware);

    NitroPort_SetOverlays(gPlatinumOverlays, gPlatinumOverlayCount);

    /* The ARM7's services: the save chip, the clock, the touch screen and
     * power management (sound, microphone and wireless aren't done yet). */
    NitroPort_SetSavePath(settings->savePath);
    NitroPort_InitArm7Services();
    NitroPort_Log("[platinum] save file: %s",
                  settings->savePath != NULL ? settings->savePath : "(none)");

    NitroPort_Log("[platinum] starting the game");
    NitroMain();
    NitroPort_Log("[platinum] NitroMain returned");
    OS_Terminate();
    for (;;) {
    }
}

#include "game_overlay.h"   /* the game's overlay manager */

int Platinum_CheckOverlays(void)
{
    int failures = 0;
    u32 id;

    NitroPort_SetOverlays(gPlatinumOverlays, gPlatinumOverlayCount);
    FS_Init(FS_DMA_NOT_USE);
    for (id = 0; id < gPlatinumOverlayCount; id++) {
        const NitroPortOverlay *o = &gPlatinumOverlays[id];
        u32 dataSize = (u32)(o->dataEnd - o->dataStart);
        u32 bssSize = (u32)(o->bssEnd - o->bssStart);
        u8 *initial = malloc(dataSize ? dataSize : 1);
        u32 i;
        BOOL ok = TRUE;

        memcpy(initial, o->dataStart, dataSize);
        /* Whatever the overlay's memory holds before ... */
        memset(o->dataStart, 0xA5, dataSize);
        memset(o->bssStart, 0x5A, bssSize);
        /* ... loading it (normally, or as the field overlay is loaded)
         * starts it afresh. */
        if (!Overlay_LoadByID(id, (id & 1) ? OVERLAY_LOAD_ASYNC : OVERLAY_LOAD_NORMAL)) {
            NitroPort_Log("[check] overlay %lu (%s): didn't load", (unsigned long)id, o->name);
            failures++;
            free(initial);
            continue;
        }
        if (memcmp(o->dataStart, initial, dataSize) != 0) {
            ok = FALSE;
        }
        for (i = 0; i < bssSize; i++) {
            if (o->bssStart[i] != 0) {
                ok = FALSE;
                break;
            }
        }
        if (!NitroPort_IsOverlayLoaded(id)) {
            ok = FALSE;
        }
        Overlay_UnloadByID(id);
        if (NitroPort_IsOverlayLoaded(id)) {
            ok = FALSE;
        }
        if (!ok) {
            NitroPort_Log("[check] overlay %lu (%s): data not reset on loading", (unsigned long)id, o->name);
            failures++;
        }
        free(initial);
    }
    NitroPort_Log("[check] %lu overlays, %d failed", (unsigned long)gPlatinumOverlayCount, failures);
    return failures;
}

/* A failed GF_ASSERT in the game. The DS build ignores most of them; the
 * port logs where they come from (look the address up in the program's
 * .elf: arm-none-eabi-addr2line -e pokeplatinum.elf <address>). The
 * programs are linked with --wrap=ErrorHandling_AssertFail. */
void __real_ErrorHandling_AssertFail(void);

void __wrap_ErrorHandling_AssertFail(void)
{
    NitroPort_Log("[platinum] GF_ASSERT failed, called from %p", __builtin_return_address(0));
    __real_ErrorHandling_AssertFail();
}
