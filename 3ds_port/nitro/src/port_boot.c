/*
 * What the DS's firmware and ARM7 have set up by the time a game's main
 * starts, and the buttons.
 *
 * Before the game runs, the DS firmware copies the cartridge's header into
 * main memory and the user's settings (name, birthday, language, touch
 * screen calibration ...) next to it, and the ARM7 keeps the state of the
 * X/Y buttons and the lid there. Games and the SDK read all of these
 * directly. NitroPort_BootFirmware writes the same things into the port's
 * main memory; the program calls it before the game's main.
 */

#include <nitro.h>
#include <nitro_port.h>
#include <string.h>

/* HW_BUTTON_XY_BUF, as the ARM7 keeps it: X, Y and the debug button
 * (1 = not pressed) and the lid (1 = closed). */
#define XY_BUTTON_X     0x0400
#define XY_BUTTON_Y     0x0800
#define XY_BUTTON_DEBUG 0x2000
#define XY_FOLD         0x8000

void NitroPort_BootFirmware(const NitroPortFirmware *fw)
{
    OSSystemWork *work = OS_GetSystemWork();
    NVRAMConfig *nvram = (NVRAMConfig *)work->nvramUserInfo;
    u8 *mac = work->nvramUserInfo + ((sizeof(NVRAMConfig) + 3) & ~3u);
    u32 i;

    /* The cartridge header (the CARD library copies it on to
     * HW_CARD_ROM_HEADER itself). */
    MI_CpuClear8((void *)HW_ROM_HEADER_BUF, HW_ROM_HEADER_BUF_END - HW_ROM_HEADER_BUF);
    if (fw->romHeader != NULL) {
        u32 size = fw->romHeaderSize;

        if (size > HW_ROM_HEADER_BUF_END - HW_ROM_HEADER_BUF) {
            size = HW_ROM_HEADER_BUF_END - HW_ROM_HEADER_BUF;
        }
        MI_CpuCopy8(fw->romHeader, (void *)HW_ROM_HEADER_BUF, size);
    }

    /* The user's settings. */
    memset(nvram, 0, sizeof(*nvram));
    nvram->ncd.version = NVRAM_CONFIG_DATA_VERSION;
    nvram->ncd.owner.favoriteColor = (u8)(fw->favoriteColor & 0xF);
    nvram->ncd.owner.birthday.month = fw->birthMonth;
    nvram->ncd.owner.birthday.day = fw->birthDay;
    for (i = 0; i < NVRAM_CONFIG_NICKNAME_LENGTH && fw->nickname != NULL && fw->nickname[i]; i++) {
        nvram->ncd.owner.nickname.str[i] = fw->nickname[i];
    }
    nvram->ncd.owner.nickname.length = (u8)i;
    nvram->ncd.option.language = (u16)(fw->language & 7);
    nvram->ncd.option.rtcOffset = fw->rtcOffset;
    nvram->ncd.option.input_favoriteColor = 1;
    nvram->ncd.option.input_tp = 1;
    nvram->ncd.option.input_language = 1;
    nvram->ncd.option.input_rtc = 1;
    nvram->ncd.option.input_nickname = 1;
    /* Touch screen calibration: two points, raw readings = 16 x screen
     * position (see NITRO_PORT_TP_RAW_SCALE). */
    nvram->ncd.tp.dx1 = 32;
    nvram->ncd.tp.dy1 = 24;
    nvram->ncd.tp.raw_x1 = 32 * NITRO_PORT_TP_RAW_SCALE;
    nvram->ncd.tp.raw_y1 = 24 * NITRO_PORT_TP_RAW_SCALE;
    nvram->ncd.tp.dx2 = 224;
    nvram->ncd.tp.dy2 = 168;
    nvram->ncd.tp.raw_x2 = 224 * NITRO_PORT_TP_RAW_SCALE;
    nvram->ncd.tp.raw_y2 = 168 * NITRO_PORT_TP_RAW_SCALE;
    memcpy(mac, fw->macAddress, 6);

    /* No buttons held, lid open. */
    NitroPort_SetButtons(0, FALSE);
    *(u32 *)HW_RESET_PARAMETER_BUF = 0;
}

void NitroPort_SetButtons(u16 held, BOOL lidClosed)
{
    u16 xy = XY_BUTTON_X | XY_BUTTON_Y | XY_BUTTON_DEBUG;

    /* Both are active-low: a 0 bit is a held button. (The SDK declares the
     * key register read-only, as it is to the DS's CPU.) */
    *(vu16 *)REG_KEYINPUT_ADDR = (u16)(PAD_KEYPORT_MASK & ~held);
    if (held & PAD_BUTTON_X) {
        xy &= ~XY_BUTTON_X;
    }
    if (held & PAD_BUTTON_Y) {
        xy &= ~XY_BUTTON_Y;
    }
    if (lidClosed) {
        xy |= XY_FOLD;
    }
    *(vu16 *)HW_BUTTON_XY_BUF = xy;
}
