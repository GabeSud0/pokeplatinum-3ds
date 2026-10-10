/*
 * PXI: the message channel between the DS's two CPUs, and the port's
 * "virtual ARM7" that answers it.
 *
 * On the DS, the ARM9 (the game) asks the ARM7 to read the clock, the touch
 * screen, the power chip, the save chip, to play sound and to run wireless,
 * by sending 32-bit messages through a hardware FIFO; the ARM7 answers the
 * same way, and the ARM9 library's callback for that "tag" gets the answer.
 *
 * Replaces NitroSDK's pxi_fifo.c and pxi_init.c (which drive the hardware
 * FIFO and handshake with the ARM7 at startup). The interface the SDK
 * libraries use is unchanged; behind it, each tag can have a port handler
 * (NitroPort_SetArm7Handler) that plays the ARM7's part.
 *
 * Tags without a handler: the message is accepted (so the SDK doesn't retry
 * forever) and reported once as not implemented. Anything waiting for an
 * answer on that tag will keep waiting, which the report makes easy to spot.
 *
 * Answers arrive as on the DS: queued in the receive FIFO, which raises the
 * FIFO-receive interrupt; its handler (PXIi_HandlerRecvFifoNotEmpty) passes
 * each message to the callback for its tag. So callbacks run in interrupt
 * mode, and only when the CPU takes interrupts.
 */

#include <nitro.h>
#include <nitro_port.h>

static BOOL sInitialized;
static PXIFifoCallback sRecvCallbacks[PXI_MAX_FIFO_TAG];
static NitroPortArm7Handler sArm7Handlers[PXI_MAX_FIFO_TAG];

/* The ARM7-to-ARM9 FIFO. The DS's holds 16 words; a bigger one means a
 * burst of answers is never dropped. */
#define RECV_FIFO_SIZE 256

static PXIFifoMessage sRecvFifo[RECV_FIFO_SIZE];
static u32 sRecvHead;
static u32 sRecvCount;

static const char *const sTagNames[PXI_MAX_FIFO_TAG] = {
    [PXI_FIFO_TAG_EX] = "PXI tag EX",
    [PXI_FIFO_TAG_USER_0] = "PXI tag USER_0",
    [PXI_FIFO_TAG_USER_1] = "PXI tag USER_1",
    [PXI_FIFO_TAG_SYSTEM] = "ARM7 service: system",
    [PXI_FIFO_TAG_NVRAM] = "ARM7 service: NVRAM (user settings)",
    [PXI_FIFO_TAG_RTC] = "ARM7 service: RTC (clock)",
    [PXI_FIFO_TAG_TOUCHPANEL] = "ARM7 service: touch screen",
    [PXI_FIFO_TAG_SOUND] = "ARM7 service: sound",
    [PXI_FIFO_TAG_PM] = "ARM7 service: power management",
    [PXI_FIFO_TAG_MIC] = "ARM7 service: microphone",
    [PXI_FIFO_TAG_WM] = "ARM7 service: wireless",
    [PXI_FIFO_TAG_FS] = "ARM7 service: file system",
    [PXI_FIFO_TAG_OS] = "ARM7 service: OS (reset)",
    [PXI_FIFO_TAG_CTRDG] = "ARM7 service: GBA cartridge",
    [PXI_FIFO_TAG_CARD] = "ARM7 service: game card",
    [PXI_FIFO_TAG_WVR] = "ARM7 service: wireless (WVR)",
    [PXI_FIFO_TAG_CTRDG_Ex] = "ARM7 service: GBA cartridge (Ex)",
    [PXI_FIFO_TAG_CTRDG_PHI] = "ARM7 service: GBA cartridge (PHI)",
};

void PXI_Init(void)
{
    PXI_InitFifo();
}

void PXI_InitFifo(void)
{
    OSSystemWork *p = OS_GetSystemWork();
    OSIntrMode enabled = OS_DisableInterrupts();

    if (!sInitialized) {
        int i;

        sInitialized = TRUE;
        for (i = 0; i < PXI_MAX_FIFO_TAG; i++) {
            sRecvCallbacks[i] = NULL;
        }
        p->pxiHandleChecker[PXI_PROC_ARM9] = 0;
        /* The virtual ARM7 is always "up": SDK libraries check this before
         * talking to it, and would otherwise wait forever. */
        p->pxiHandleChecker[PXI_PROC_ARM7] = 0xFFFFFFFF;
        sRecvHead = 0;
        sRecvCount = 0;
        /* As the SDK's PXI_InitFifo: answers come in by interrupt. */
        (void)OS_ResetRequestIrqMask(OS_IE_FIFO_RECV);
        (void)OS_SetIrqFunction(OS_IE_FIFO_RECV, PXIi_HandlerRecvFifoNotEmpty);
        (void)OS_EnableIrqMask(OS_IE_FIFO_RECV);
    }
    (void)OS_RestoreInterrupts(enabled);
}

void PXI_SetFifoRecvCallback(int fifotag, PXIFifoCallback callback)
{
    OSSystemWork *p = OS_GetSystemWork();
    OSIntrMode enabled = OS_DisableInterrupts();

    sRecvCallbacks[fifotag] = callback;
    if (callback) {
        p->pxiHandleChecker[PXI_PROC_ARM9] |= (1UL << fifotag);
    } else {
        p->pxiHandleChecker[PXI_PROC_ARM9] &= ~(1UL << fifotag);
    }
    (void)OS_RestoreInterrupts(enabled);
}

BOOL PXI_IsCallbackReady(int fifotag, PXIProc proc)
{
    OSSystemWork *p = OS_GetSystemWork();

    return (p->pxiHandleChecker[proc] & (1UL << fifotag)) ? TRUE : FALSE;
}

void PXI_SetFifoSendCallback(PXIFifoEmtpyCallback callback)
{
    /* Our FIFO is never full, so "space available" never needs signalling. */
    (void)callback;
}

int PXI_SendWordByFifo(int fifotag, u32 data, BOOL err)
{
    if (fifotag < 0 || fifotag >= PXI_MAX_FIFO_TAG) {
        return PXI_FIFO_FAIL_SEND_ERR;
    }
    if (sArm7Handlers[fifotag]) {
        sArm7Handlers[fifotag](fifotag, data, err);
    } else {
        NitroPort_Unimplemented(sTagNames[fifotag] ? sTagNames[fifotag] : "ARM7 service: unknown");
    }
    return PXI_FIFO_SUCCESS;
}

void PXIi_HandlerRecvFifoNotEmpty(void)
{
    /* The FIFO-receive interrupt: hand every waiting message to its tag's
     * callback, in arrival order. */
    for (;;) {
        PXIFifoMessage msg;
        OSIntrMode enabled = OS_DisableInterrupts();
        int tag;

        if (sRecvCount == 0) {
            (void)OS_RestoreInterrupts(enabled);
            break;
        }
        msg = sRecvFifo[sRecvHead];
        sRecvHead = (sRecvHead + 1) % RECV_FIFO_SIZE;
        sRecvCount--;
        (void)OS_RestoreInterrupts(enabled);

        tag = (int)msg.e.tag;
        if (tag > 0 && tag < PXI_MAX_FIFO_TAG && sRecvCallbacks[tag]) {
            sRecvCallbacks[tag]((PXIFifoTag)tag, msg.e.data, (BOOL)msg.e.err);
        }
    }
}

/* ------------------------------------------------------- port side */

void NitroPort_SetArm7Handler(int tag, NitroPortArm7Handler handler)
{
    if (tag >= 0 && tag < PXI_MAX_FIFO_TAG) {
        sArm7Handlers[tag] = handler;
    }
}

void NitroPort_Arm7Reply(int tag, u32 data, BOOL err)
{
    PXIFifoMessage msg;
    OSIntrMode enabled;

    if (tag <= 0 || tag >= PXI_MAX_FIFO_TAG) {
        return;
    }
    msg.raw = 0;
    msg.e.tag = (PXIFifoTag)tag;
    msg.e.err = (u32)(err ? 1 : 0);
    msg.e.data = data;

    enabled = OS_DisableInterrupts();
    if (sRecvCount == RECV_FIFO_SIZE) {
        (void)OS_RestoreInterrupts(enabled);
        NitroPort_Log("[nitro] PXI: receive FIFO full, answer on tag %d dropped", tag);
        return;
    }
    sRecvFifo[(sRecvHead + sRecvCount) % RECV_FIFO_SIZE] = msg;
    sRecvCount++;
    (void)OS_RestoreInterrupts(enabled);

    /* The FIFO is no longer empty: request its interrupt. It is taken now
     * if the CPU takes interrupts, otherwise as soon as it does. */
    NitroPort_RaiseIrq(OS_IE_FIFO_RECV);
}
