/*
 * Virtual ARM7 service registration. Always call before initializing
 * CARD / RTC / PM / TP from the original NitroSDK.
 */
#include <nitro.h>
#include <nitro_port.h>

extern void NitroPort_HandleCard(int tag, u32 data, BOOL err);
extern void NitroPort_HandleRtc(int tag, u32 data, BOOL err);
extern void NitroPort_HandlePm(int tag, u32 data, BOOL err);
extern void NitroPort_HandleTp(int tag, u32 data, BOOL err);
extern void NitroPort_HandleSound(int tag, u32 data, BOOL err);
extern void NitroPort_ResetPmState(void);
extern void NitroPort_ResetTouchState(void);
extern void NitroPort_ResetSoundState(void);

void NitroPort_InitArm7Services(void)
{
    static BOOL initialized;
    if (initialized) return;
    initialized = TRUE;
    PXI_Init();
    NitroPort_ResetPmState();
    NitroPort_ResetTouchState();
    NitroPort_ResetSoundState();
    /* The original backup SPI service uses the FS tag, not CARD. */
    NitroPort_SetArm7Handler(PXI_FIFO_TAG_FS, NitroPort_HandleCard);
    NitroPort_SetArm7Handler(PXI_FIFO_TAG_RTC, NitroPort_HandleRtc);
    NitroPort_SetArm7Handler(PXI_FIFO_TAG_PM, NitroPort_HandlePm);
    NitroPort_SetArm7Handler(PXI_FIFO_TAG_TOUCHPANEL, NitroPort_HandleTp);
    /* Silent for now: takes the sound library's requests so it never runs
     * out of commands (port_snd.c). */
    NitroPort_SetArm7Handler(PXI_FIFO_TAG_SOUND, NitroPort_HandleSound);
}
