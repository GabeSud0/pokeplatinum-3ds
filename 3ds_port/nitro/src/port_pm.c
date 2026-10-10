/*
 * Virtual ARM7 power-management chip.
 * PM ARM9 requests use SPI-style PXI framing; register reads respond with
 * PM_REGnVALUE instead of PM_REG_READ so the SDK populates its result buffer.
 * The 3DS power APIs remain owned by the main program, not this shim.
 */
#include <nitro.h>
#include <nitro_port.h>
#include <nitro/spi/common/type.h>
#include <nitro/spi/common/pm_common.h>

static u8 sRegisters[PMIC_REG_NUMS];
static u32 sPendingCommand;
static u32 sFirstByte;
static BOOL sPending;
static PMLEDPattern sPattern;

void NitroPort_ResetPmState(void)
{
    unsigned int i;
    for (i = 0; i < PMIC_REG_NUMS; ++i) sRegisters[i] = 0;
    /* Battery-high means PMIC_STAT_VDET is clear. */
    sRegisters[REG_PMIC_CTL_ADDR] = PMIC_CTL_BKLT1 | PMIC_CTL_BKLT2;
    sPending = FALSE;
    sPendingCommand = 0;
    sFirstByte = 0;
    sPattern = PM_LED_PATTERN_ON;
}

static void apply_utility(u32 utility)
{
    switch (utility) {
    case PM_UTIL_LCD1_BACKLIGHT_ON:
        sRegisters[REG_PMIC_CTL_ADDR] |= PMIC_CTL_BKLT1;
        break;
    case PM_UTIL_LCD1_BACKLIGHT_OFF:
        sRegisters[REG_PMIC_CTL_ADDR] &= (u8)~PMIC_CTL_BKLT1;
        break;
    case PM_UTIL_LCD2_BACKLIGHT_ON:
        sRegisters[REG_PMIC_CTL_ADDR] |= PMIC_CTL_BKLT2;
        break;
    case PM_UTIL_LCD2_BACKLIGHT_OFF:
        sRegisters[REG_PMIC_CTL_ADDR] &= (u8)~PMIC_CTL_BKLT2;
        break;
    case PM_UTIL_LCD12_BACKLIGHT_ON:
        sRegisters[REG_PMIC_CTL_ADDR] |= PMIC_CTL_BKLT1 | PMIC_CTL_BKLT2;
        break;
    case PM_UTIL_LCD12_BACKLIGHT_OFF:
        sRegisters[REG_PMIC_CTL_ADDR] &= (u8)~(PMIC_CTL_BKLT1 | PMIC_CTL_BKLT2);
        break;
    case PM_UTIL_SOUND_POWER_ON:
        sRegisters[REG_PMIC_CTL_ADDR] |= PMIC_CTL_SND_PWR;
        break;
    case PM_UTIL_SOUND_POWER_OFF:
        sRegisters[REG_PMIC_CTL_ADDR] &= (u8)~PMIC_CTL_SND_PWR;
        break;
    case PM_UTIL_SOUND_VOL_CTRL_ON:
        sRegisters[REG_PMIC_CTL_ADDR] |= PMIC_CTL_SND_VOLCTRL;
        break;
    case PM_UTIL_SOUND_VOL_CTRL_OFF:
        sRegisters[REG_PMIC_CTL_ADDR] &= (u8)~PMIC_CTL_SND_VOLCTRL;
        break;
    default:
        /* LED and power-off requests are intentionally acknowledged but
         * never turn off the 3DS itself. */
        break;
    }
}

static void finish_pm_command(u32 command, u32 byte, u32 next)
{
    u32 reply_command = command;
    u32 reply = PM_RESULT_SUCCESS;

    switch (command) {
    case SPI_PXI_COMMAND_PM_REG_READ:
        if (byte >= PMIC_REG_NUMS) {
            reply = PM_RESULT_ERROR;
        } else {
            reply_command = SPI_PXI_COMMAND_PM_REG0VALUE + byte;
            reply = sRegisters[byte];
        }
        break;
    case SPI_PXI_COMMAND_PM_REG_WRITE:
        if (byte >= PMIC_REG_NUMS) reply = PM_RESULT_ERROR;
        else if (byte != REG_PMIC_STAT_ADDR)
            sRegisters[byte] = (u8)next;
        break;
    case SPI_PXI_COMMAND_PM_UTILITY:
        apply_utility((byte << 16) | next);
        break;
    case SPI_PXI_COMMAND_PM_SELF_BLINK:
        sPattern = (PMLEDPattern)byte;
        break;
    case SPI_PXI_COMMAND_PM_GET_BLINK:
        reply = (u32)sPattern;
        break;
    case SPI_PXI_COMMAND_PM_SYNC:
    case SPI_PXI_COMMAND_PM_SLEEP_START:
        break;
    default:
        reply = PM_RESULT_ERROR;
        break;
    }
    NitroPort_Arm7Reply(PXI_FIFO_TAG_PM,
                        (reply_command << SPI_PXI_RESULT_COMMAND_SHIFT) |
                        (reply & SPI_PXI_RESULT_DATA_MASK), FALSE);
}

void NitroPort_HandlePm(int tag, u32 data, BOOL err)
{
    u32 command;
    (void)tag;
    if (err) {
        sPending = FALSE;
        NitroPort_Arm7Reply(PXI_FIFO_TAG_PM, 0, TRUE);
        return;
    }
    if (data & SPI_PXI_START_BIT) {
        command = (data & SPI_PXI_RESULT_COMMAND_MASK) >>
                  SPI_PXI_RESULT_COMMAND_SHIFT;
        sPendingCommand = command;
        sFirstByte = data & 0xffu;
        sPending = !(data & SPI_PXI_END_BIT);
        if (!sPending) finish_pm_command(command, sFirstByte, 0);
        return;
    }
    if (!sPending) return;
    if (data & SPI_PXI_END_BIT) {
        command = sPendingCommand;
        sPending = FALSE;
        finish_pm_command(command, sFirstByte, data & 0xffffu);
    }
}
