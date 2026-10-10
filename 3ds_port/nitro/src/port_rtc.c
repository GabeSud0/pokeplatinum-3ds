/*
 * Virtual ARM7 RTC: ARM9 NitroSDK requests a BCD-filled RTCRawData through
 * PXI and copies it from OS system work when the acknowledgement arrives.
 */
#include <nitro.h>
#include <nitro_port.h>
#include <nitro/rtc/common/fifo.h>
#include <nitro/rtc/common/type.h>

#include <time.h>
#include <string.h>

static u32 bcd(unsigned int decimal)
{
    return ((decimal / 10u) << 4) | (decimal % 10u);
}

void NitroPort_HandleRtc(int tag, u32 data, BOOL err)
{
    u32 command = (data & RTC_PXI_COMMAND_MASK) >> RTC_PXI_COMMAND_SHIFT;
    u32 status = RTC_PXI_RESULT_SUCCESS;
    RTCRawData *raw = (RTCRawData *)OS_GetSystemWork()->real_time_clock;
    (void)tag;

    if (err) {
        NitroPort_Arm7Reply(PXI_FIFO_TAG_RTC,
                            (command << RTC_PXI_COMMAND_SHIFT) |
                            RTC_PXI_RESULT_FATAL_ERROR, FALSE);
        return;
    }

    switch (command) {
    case RTC_PXI_COMMAND_READ_DATETIME:
    case RTC_PXI_COMMAND_READ_DATE:
    case RTC_PXI_COMMAND_READ_TIME: {
        time_t now = time(NULL);
        struct tm *local;
        struct tm snapshot;
        if (now == (time_t)-1 || !(local = localtime(&now))) {
            status = RTC_PXI_RESULT_FATAL_ERROR;
            break;
        }
        snapshot = *local; /* localtime's static storage may be reused. */
        if (command != RTC_PXI_COMMAND_READ_TIME) {
            raw->t.date.year = bcd((unsigned int)((snapshot.tm_year + 1900) % 100));
            raw->t.date.month = bcd((unsigned int)(snapshot.tm_mon + 1));
            raw->t.date.day = bcd((unsigned int)snapshot.tm_mday);
            raw->t.date.week = (u32)snapshot.tm_wday;
        }
        if (command != RTC_PXI_COMMAND_READ_DATE) {
            raw->t.time.hour = bcd((unsigned int)snapshot.tm_hour);
            raw->t.time.afternoon = snapshot.tm_hour >= 12 ? 1u : 0u;
            raw->t.time.minute = bcd((unsigned int)snapshot.tm_min);
            raw->t.time.second = bcd((unsigned int)snapshot.tm_sec);
        }
        break;
    }
    case RTC_PXI_COMMAND_RESET:
    case RTC_PXI_COMMAND_SET_HOUR_FORMAT:
        break;
    case RTC_PXI_COMMAND_READ_STATUS1:
    case RTC_PXI_COMMAND_READ_STATUS2:
        /* No alarm asserted, use the normal 24-hour format. */
        memset(raw, 0, sizeof(*raw));
        break;
    default:
        status = RTC_PXI_RESULT_INVALID_COMMAND;
        break;
    }
    NitroPort_Arm7Reply(PXI_FIFO_TAG_RTC,
                        (command << RTC_PXI_COMMAND_SHIFT) | status, FALSE);
}
