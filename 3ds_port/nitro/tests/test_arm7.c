/*
 * The tests exercise original NitroSDK ARM9 APIs, not ARM7 handler internals.
 * A temporary 512 KiB image is created outside Git and deleted on exit.
 *
 * The SDK is started as the game starts it (firmware state, OS_Init, which
 * starts the card library's thread, interrupts on), because the services'
 * answers arrive through the PXI receive interrupt, as on the DS.
 */
#define _POSIX_C_SOURCE 200809L
#include <nitro.h>
#include <nitro_port.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int sChecks;
static int sFailed;

#define CHECK(cond) do {                                                    \
    ++sChecks;                                                               \
    if (!(cond)) {                                                           \
        ++sFailed;                                                           \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
    }                                                                        \
} while (0)

static int sAsyncCardDone;
static void card_done(void *arg)
{
    ++*(int *)arg;
}

static int sAsyncRtcDone;
static RTCResult sAsyncRtcResult;
static void rtc_done(RTCResult result, void *arg)
{
    ++*(int *)arg;
    sAsyncRtcResult = result;
}

static int sAsyncPmDone;
static u32 sAsyncPmResult;
static void pm_done(u32 result, void *arg)
{
    ++*(int *)arg;
    sAsyncPmResult = result;
}

static int same_clock(const RTCDate *date, const RTCTime *rtc, const struct tm *local)
{
    if (date->year != (u32)((local->tm_year + 1900) % 100) ||
        date->month != (u32)(local->tm_mon + 1) ||
        date->day != (u32)local->tm_mday ||
        date->week != (RTCWeek)local->tm_wday)
        return 0;
    return rtc->hour == (u32)local->tm_hour &&
           rtc->minute == (u32)local->tm_min &&
           (rtc->second == (u32)local->tm_sec ||
            rtc->second == (u32)((local->tm_sec + 1) % 60));
}

static void test_rtc(void)
{
    RTCDate date, date2;
    RTCTime clock, clock2;
    struct tm before, after;
    time_t then, now;
    then = time(NULL);
    before = *localtime(&then);
    RTC_Init();
    CHECK(RTC_GetDate(&date) == RTC_RESULT_SUCCESS);
    CHECK(RTC_GetTime(&clock) == RTC_RESULT_SUCCESS);
    CHECK(RTC_GetDateTime(&date2, &clock2) == RTC_RESULT_SUCCESS);
    CHECK(RTC_GetDateTimeAsync(&date2, &clock2, rtc_done, &sAsyncRtcDone) ==
          RTC_RESULT_SUCCESS);
    CHECK(sAsyncRtcDone == 1);
    CHECK(sAsyncRtcResult == RTC_RESULT_SUCCESS);
    now = time(NULL);
    after = *localtime(&now);
    CHECK(same_clock(&date2, &clock2, &before) ||
          same_clock(&date2, &clock2, &after));
    CHECK(date.month >= 1 && date.month <= 12);
    CHECK(clock.hour < 24 && clock.minute < 60 && clock.second < 60);
}

static void test_power(void)
{
    PMBattery battery = PM_BATTERY_LOW;
    PMBackLightSwitch top, bottom;
    PM_Init();
    CHECK(PM_GetBattery(&battery) == PM_SUCCESS);
    CHECK(battery == PM_BATTERY_HIGH);
    CHECK(PM_GetBackLight(&top, &bottom) == PM_SUCCESS);
    CHECK(top == PM_BACKLIGHT_ON && bottom == PM_BACKLIGHT_ON);
    CHECK(PM_SetBackLight(PM_LCD_TOP, PM_BACKLIGHT_OFF) == PM_SUCCESS);
    CHECK(PM_GetBackLight(&top, &bottom) == PM_SUCCESS);
    CHECK(top == PM_BACKLIGHT_OFF && bottom == PM_BACKLIGHT_ON);
    CHECK(PM_SetBackLightAsync(PM_LCD_TOP, PM_BACKLIGHT_ON,
                               pm_done, &sAsyncPmDone) == PM_SUCCESS);
    CHECK(sAsyncPmDone == 1);
    CHECK(sAsyncPmResult == PM_RESULT_SUCCESS);
    CHECK(PM_GetBackLight(&top, &bottom) == PM_SUCCESS);
    CHECK(top == PM_BACKLIGHT_ON && bottom == PM_BACKLIGHT_ON);
}

static void test_touch(void)
{
    TPData sample, ring[4], display;
    TPCalibrateParam calibration;
    TP_Init();
    /* As the game does (system.c): the calibration from the firmware. */
    CHECK(TP_GetUserInfo(&calibration) == TRUE);
    TP_SetCalibrateParam(&calibration);
    NitroPort_SetTouch(TRUE, 123, 78);
    CHECK(TP_RequestRawSampling(&sample) == 0);
    CHECK(sample.x == 123 * NITRO_PORT_TP_RAW_SCALE && sample.y == 78 * NITRO_PORT_TP_RAW_SCALE);
    CHECK(TP_RequestCalibratedSampling(&sample) == 0);
    CHECK(sample.touch == TP_TOUCH_ON);
    CHECK(sample.validity == TP_VALIDITY_VALID);
    CHECK(sample.x == 123 && sample.y == 78);
    CHECK(TP_RequestRawSampling(&sample) == 0);
    TP_GetCalibratedPoint(&display, &sample);
    CHECK(display.x == 123 && display.y == 78);

    CHECK(TP_RequestAutoSamplingStart(10, 1, ring, 4) == 0);
    NitroPort_SetTouch(TRUE, 201, 144);
    TP_GetLatestCalibratedPointInAuto(&display);
    CHECK(display.touch == TP_TOUCH_ON);
    CHECK(display.x == 201 && display.y == 144);
    CHECK(TP_RequestAutoSamplingStop() == 0);

    NitroPort_SetTouch(FALSE, 99, 99);
    CHECK(TP_RequestRawSampling(&sample) == 0);
    CHECK(sample.touch == TP_TOUCH_OFF);
    CHECK(sample.x == 0 && sample.y == 0);
    NitroPort_SetTouch(TRUE, 999, 999);
    CHECK(TP_RequestCalibratedSampling(&sample) == 0);
    CHECK(sample.x == 255 && sample.y == 191);
}

static void test_backup(void)
{
    char save_path[] = "/tmp/platinum-arm7-XXXXXX";
    /* NitroSDK stores CARD source/destination pointers in u32. On a
     * 64-bit host the stack lives above 4 GiB; static data stays below. */
    static unsigned char source[320], read_back[320], erased[32];
    FILE *image;
    u16 lock_id;
    int descriptor, i;

    descriptor = mkstemp(save_path);
    CHECK(descriptor >= 0);
    if (descriptor < 0) return;
    close(descriptor);
    remove(save_path);

    NitroPort_SetSavePath(save_path);
    CARD_Init();
    lock_id = OS_GetLockID();
    CARD_LockBackup(lock_id);
    CHECK(CARD_IdentifyBackup(CARD_BACKUP_TYPE_FLASH_4MBITS));
    CHECK(CARD_GetBackupTotalSize() == 512u * 1024u);
    CHECK(CARD_GetBackupPageSize() == 256u);
    CHECK(CARD_GetBackupSectorSize() == 65536u);

    image = fopen(save_path, "rb");
    CHECK(image != NULL);
    if (image) {
        CHECK(fseek(image, 0, SEEK_END) == 0);
        CHECK(ftell(image) == 512L * 1024L);
        CHECK(fseek(image, 0, SEEK_SET) == 0);
        CHECK(fread(erased, 1, sizeof(erased), image) == sizeof(erased));
        for (i = 0; i < (int)sizeof(erased); ++i) CHECK(erased[i] == 0xff);
        fclose(image);
    }
    for (i = 0; i < (int)sizeof(source); ++i) source[i] = (unsigned char)(i * 5);
    memset(read_back, 0, sizeof(read_back));
    CHECK(CARD_WriteBackup(0x1000u, source, sizeof(source)));
    CHECK(CARD_ReadBackup(0x1000u, read_back, sizeof(read_back)));
    CHECK(memcmp(source, read_back, sizeof(source)) == 0);

    memset(read_back, 0, sizeof(read_back));
    CARD_ReadBackupAsync(0x1000u, read_back, sizeof(read_back),
                         card_done, &sAsyncCardDone);
    CHECK(CARD_WaitBackupAsync());
    CHECK(sAsyncCardDone == 1);
    CHECK(memcmp(source, read_back, sizeof(source)) == 0);

    /* A new write must persist before even the async callback completes. */
    CARD_WriteBackupAsync(0x2000u, source, sizeof(source),
                          card_done, &sAsyncCardDone);
    CHECK(CARD_WaitBackupAsync());
    CHECK(sAsyncCardDone == 2);
    image = fopen(save_path, "rb");
    CHECK(image != NULL);
    if (image) {
        CHECK(fseek(image, 0x2000, SEEK_SET) == 0);
        CHECK(fread(read_back, 1, sizeof(read_back), image) == sizeof(read_back));
        CHECK(memcmp(read_back, source, sizeof(source)) == 0);
        fclose(image);
    }

    CHECK(CARD_EraseBackupSector(0, 0x10000u));
    CHECK(CARD_ReadBackup(0x1000u, erased, sizeof(erased)));
    for (i = 0; i < (int)sizeof(erased); ++i) CHECK(erased[i] == 0xff);
    CARD_UnlockBackup(lock_id);
    NitroPort_SetSavePath(NULL);
    CHECK(remove(save_path) == 0);
}

/* The sound service is silent, but must take every request: the library
 * reuses its 256 commands only once the "ARM7" has counted them finished.
 * 3,000 frames' worth of requests would run out without that. */
static void test_sound(void)
{
    static SNDDriverInfo info;
    u32 tag;
    int frame;

    SND_Init();
    for (frame = 0; frame < 3000; ++frame) {
        SND_SetMasterVolume(frame & 127);
        SND_StartSeq(0, NULL, 0, NULL);
        (void)SND_FlushCommand(SND_COMMAND_NOBLOCK);   /* as SND_Main does */
    }
    CHECK(SND_CountFreeCommand() > 200);
    tag = SND_GetCurrentCommandTag();
    CHECK(SND_FlushCommand(SND_COMMAND_BLOCK));
    SND_WaitForCommandProc(tag);
    CHECK(SND_IsFinishedCommandTag(tag));
    CHECK(SND_GetPlayerStatus() == 0);              /* nothing plays */

    memset(&info, 0x55, sizeof(info));
    SND_ReadDriverInfo(&info);
    tag = SND_GetCurrentCommandTag();
    (void)SND_FlushCommand(SND_COMMAND_BLOCK);
    SND_WaitForCommandProc(tag);
    CHECK(info.workAddress != NULL);
    CHECK(info.lockedChannels == 0 && info.chCtrl[0] == 0);
}

int main(void)
{
    NitroPortFirmware firmware;

    memset(&firmware, 0, sizeof(firmware));
    NitroPort_BootFirmware(&firmware);
    NitroPort_InitArm7Services();
    OS_Init();
    (void)OS_EnableIrq();
    (void)OS_EnableInterrupts();
    test_backup();
    test_rtc();
    test_power();
    test_touch();
    test_sound();
    printf("virtual ARM7 services: %d checks, %d failures\n", sChecks, sFailed);
    return sFailed ? 1 : 0;
}
