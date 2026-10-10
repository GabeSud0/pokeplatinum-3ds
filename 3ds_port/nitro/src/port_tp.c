/*
 * Virtual ARM7 touch panel. ARM9 NitroSDK reads four shared bytes from
 * OSSystemWork after receiving sampling-complete PXI packets.
 *
 * X/Y are already calibrated to the 256x192 DS bottom screen. In the
 * absence of console calibration the SDK's TP_SetCalibrateParam(NULL)
 * keeps them in those units.
 */
#include <nitro.h>
#include <nitro_port.h>
#include <nitro/spi/common/type.h>

#include <string.h>

static BOOL sAutoSampling;
static BOOL sAwaitAutoConfig;

static void publish_touch(BOOL down, u16 x, u16 y)
{
    SPITpData sample;
    memset(&sample, 0, sizeof(sample));
    sample.e.touch = down ? SPI_TP_TOUCH_ON : SPI_TP_TOUCH_OFF;
    sample.e.x = down ? (x > 255 ? 255 : x) : 0;
    sample.e.y = down ? (y > 191 ? 191 : y) : 0;
    sample.e.validity = SPI_TP_VALIDITY_VALID;
    memcpy((void *)OS_GetSystemWork()->touch_panel, sample.bytes, sizeof(sample.bytes));
}

void NitroPort_ResetTouchState(void)
{
    sAutoSampling = FALSE;
    sAwaitAutoConfig = FALSE;
    publish_touch(FALSE, 0, 0);
}

void NitroPort_SetTouch(BOOL down, u16 x, u16 y)
{
    publish_touch(down, x, y);
    if (sAutoSampling) {
        /* AUTO_SAMPLING packets don't carry END/RESULT_BIT. The original
         * SDK's callback increments its ring-buffer index and copies the
         * current shared sample into the caller's TPData array. */
        NitroPort_Arm7Reply(PXI_FIFO_TAG_TOUCHPANEL,
                            SPI_PXI_COMMAND_TP_AUTO_SAMPLING << 8, FALSE);
    }
}

static void complete(u32 command)
{
    NitroPort_Arm7Reply(PXI_FIFO_TAG_TOUCHPANEL,
                        SPI_PXI_END_BIT | SPI_PXI_RESULT_COMMAND_BIT |
                        (command << 8) | SPI_PXI_RESULT_SUCCESS, FALSE);
}

void NitroPort_HandleTp(int tag, u32 data, BOOL err)
{
    u32 command;
    (void)tag;
    if (err) {
        NitroPort_Arm7Reply(PXI_FIFO_TAG_TOUCHPANEL, 0, TRUE);
        return;
    }
    if (sAwaitAutoConfig) {
        if (data & SPI_PXI_END_BIT) {
            sAwaitAutoConfig = FALSE;
            sAutoSampling = TRUE;
            complete(SPI_PXI_COMMAND_TP_AUTO_ON);
        }
        return;
    }
    if (!(data & SPI_PXI_START_BIT)) return;
    command = (data & SPI_PXI_RESULT_COMMAND_MASK) >>
              SPI_PXI_RESULT_COMMAND_SHIFT;

    switch (command) {
    case SPI_PXI_COMMAND_TP_SAMPLING:
        complete(command);
        break;
    case SPI_PXI_COMMAND_TP_AUTO_ON:
        if (!(data & SPI_PXI_END_BIT)) sAwaitAutoConfig = TRUE;
        else {
            sAutoSampling = TRUE;
            complete(command);
        }
        break;
    case SPI_PXI_COMMAND_TP_AUTO_OFF:
        sAutoSampling = FALSE;
        complete(command);
        break;
    case SPI_PXI_COMMAND_TP_SETUP_STABILITY:
        complete(command);
        break;
    default:
        NitroPort_Arm7Reply(PXI_FIFO_TAG_TOUCHPANEL,
                            SPI_PXI_END_BIT | SPI_PXI_RESULT_COMMAND_BIT |
                            (command << 8) |
                            SPI_PXI_RESULT_INVALID_COMMAND, FALSE);
        break;
    }
}
