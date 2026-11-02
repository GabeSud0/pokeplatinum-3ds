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

/* The save kept in memory (port_card.c): flash programming ANDs into
 * what's there, verify compares, and all of it is in the file (read again
 * when the save is opened again). */
static void test_backup_program_and_reopen(void)
{
    char save_path[] = "/tmp/platinum-arm7-XXXXXX";
    static unsigned char first[512], mask[512], expect[512], read_back[512];
    FILE *image;
    u16 lock_id;
    int descriptor, i;

    descriptor = mkstemp(save_path);
    CHECK(descriptor >= 0);
    if (descriptor < 0) return;
    close(descriptor);
    remove(save_path);

    NitroPort_SetSavePath(save_path);
    lock_id = OS_GetLockID();
    CARD_LockBackup(lock_id);
    CHECK(CARD_IdentifyBackup(CARD_BACKUP_TYPE_FLASH_4MBITS));
    for (i = 0; i < (int)sizeof(first); ++i) {
        first[i] = (unsigned char)(i * 7 + 3);
        mask[i] = (unsigned char)(0xF0 | (i & 0x0F));
        expect[i] = (unsigned char)(first[i] & mask[i]);
    }
    CHECK(CARD_WriteBackup(0x7F00u, first, sizeof(first)));
    CHECK(CARD_ProgramBackup(0x7F00u, mask, sizeof(mask)));
    CHECK(CARD_ReadBackup(0x7F00u, read_back, sizeof(read_back)));
    CHECK(memcmp(read_back, expect, sizeof(expect)) == 0);
    CHECK(CARD_VerifyBackup(0x7F00u, expect, sizeof(expect)));
    CHECK(!CARD_VerifyBackup(0x7F00u, first, sizeof(first)));
    image = fopen(save_path, "rb");
    CHECK(image != NULL);
    if (image) {
        CHECK(fseek(image, 0x7F00, SEEK_SET) == 0);
        CHECK(fread(read_back, 1, sizeof(read_back), image) == sizeof(read_back));
        CHECK(memcmp(read_back, expect, sizeof(expect)) == 0);
        fclose(image);
    }
    /* opened again: what the file has */
    NitroPort_SetSavePath(save_path);
    memset(read_back, 0, sizeof(read_back));
    CHECK(CARD_ReadBackup(0x7F00u, read_back, sizeof(read_back)));
    CHECK(memcmp(read_back, expect, sizeof(expect)) == 0);
    CHECK(CARD_EraseBackupSector(0, 0x10000u));
    NitroPort_SetSavePath(save_path);
    CHECK(CARD_ReadBackup(0x7F00u, read_back, sizeof(read_back)));
    for (i = 0; i < (int)sizeof(read_back); ++i) CHECK(read_back[i] == 0xff);
    CARD_UnlockBackup(lock_id);
    NitroPort_SetSavePath(NULL);
    CHECK(remove(save_path) == 0);
}

/* A write of many pages (the SDK asks for one page at a time) goes into the
 * file once, with its last page: all of it there when it completes, async
 * or not (a full save was 512 writes to the SD card, 3.3 s). */
static int FileHas(const char *path, long at, const unsigned char *what, size_t n)
{
    static unsigned char got[8192];
    FILE *image = fopen(path, "rb");
    int same;

    if (image == NULL || n > sizeof(got)) {
        if (image) fclose(image);
        return 0;
    }
    same = fseek(image, at, SEEK_SET) == 0 && fread(got, 1, n, image) == n && memcmp(got, what, n) == 0;
    fclose(image);
    return same;
}

static void test_backup_one_file_write(void)
{
    char save_path[] = "/tmp/platinum-arm7-XXXXXX";
    static unsigned char big[4096 + 100], back[4096 + 100];
    u32 before;
    u16 lock_id;
    int descriptor, i;

    descriptor = mkstemp(save_path);
    CHECK(descriptor >= 0);
    if (descriptor < 0) return;
    close(descriptor);
    remove(save_path);

    NitroPort_SetSavePath(save_path);
    lock_id = OS_GetLockID();
    CARD_LockBackup(lock_id);
    CHECK(CARD_IdentifyBackup(CARD_BACKUP_TYPE_FLASH_4MBITS));
    for (i = 0; i < (int)sizeof(big); ++i) big[i] = (unsigned char)(i * 13 + 1);

    before = NitroPort_CardFileWrites();
    CHECK(CARD_WriteAndVerifyBackup(0x10000u, big, sizeof(big)));   /* (17 pages) */
    CHECK(NitroPort_CardFileWrites() - before == 1);
    CHECK(FileHas(save_path, 0x10000, big, sizeof(big)));
    CHECK(CARD_ReadBackup(0x10000u, back, sizeof(back)) && memcmp(back, big, sizeof(big)) == 0);

    for (i = 0; i < (int)sizeof(big); ++i) big[i] = (unsigned char)(i * 7 + 5);
    before = NitroPort_CardFileWrites();
    sAsyncCardDone = 0;
    CARD_WriteAndVerifyBackupAsync(0x20100u, big, sizeof(big), card_done, &sAsyncCardDone);
    CHECK(CARD_WaitBackupAsync());
    CHECK(sAsyncCardDone == 1);
    CHECK(NitroPort_CardFileWrites() - before == 1);
    CHECK(FileHas(save_path, 0x20100, big, sizeof(big)));

    /* nothing left to write: nothing written */
    before = NitroPort_CardFileWrites();
    NitroPort_CardFlush();
    CHECK(NitroPort_CardFileWrites() == before);
    /* opened again: what the file has */
    NitroPort_SetSavePath(save_path);
    memset(back, 0, sizeof(back));
    CHECK(CARD_ReadBackup(0x20100u, back, sizeof(back)) && memcmp(back, big, sizeof(big)) == 0);
    CARD_UnlockBackup(lock_id);
    NitroPort_SetSavePath(NULL);
    CHECK(remove(save_path) == 0);
}

/* The sound service is silent, but must take every request: the library
 * reuses its 256 commands only once the "ARM7" has counted them finished.
 * 3,000 frames' worth of requests would run out without that. */
/* Pull the actual 16-bit mixer while testing ARM9 sound APIs. */
extern void NitroPort_SoundRender(int16_t *stereo, size_t frames);

static void test_sound(void)
{
    static SNDDriverInfo info;
    int16_t samples[2 * 96];
    u32 tag;
    int frame;

    SND_Init();
    /* More lists than ARM9's pool: every command must come back even
     * though the virtual ARM7 now executes real sequencer code. NULL
     * sequence pointers are invalid; STOP_SEQ requires no sample data. */
    for (frame = 0; frame < 3000; ++frame) {
        SND_SetMasterVolume(frame & 127);
        SND_StopSeq(0);
        (void)SND_FlushCommand(SND_COMMAND_NOBLOCK);
        NitroPort_SoundRender(samples, 96);
    }
    CHECK(SND_CountFreeCommand() > 200);
    tag = SND_GetCurrentCommandTag();
    CHECK(SND_FlushCommand(SND_COMMAND_BLOCK));
    NitroPort_SoundRender(samples, 96);
    SND_WaitForCommandProc(tag);
    CHECK(SND_IsFinishedCommandTag(tag));
    CHECK(SND_GetPlayerStatus() == 0);

    memset(&info, 0x55, sizeof(info));
    SND_ReadDriverInfo(&info);
    tag = SND_GetCurrentCommandTag();
    (void)SND_FlushCommand(SND_COMMAND_BLOCK);
    NitroPort_SoundRender(samples, 96);
    SND_WaitForCommandProc(tag);
    CHECK(info.workAddress != NULL);
    CHECK(info.lockedChannels == 0 && info.chCtrl[0] == 0);
}

/* ----------------------------------------------------------- wireless */

static int sWvrDone;
static WVRResult sWvrResult;
static void wvr_done(void *arg, WVRResult result)
{
    ++*(int *)arg;
    sWvrResult = result;
}

/* The last answer each WM callback got (copied: the records are reused). */
static int sWmCalls[WM_APIID_ASYNC_KIND_MAX];
static u16 sWmErr[WM_APIID_ASYNC_KIND_MAX];
static u16 sWmState[WM_APIID_ASYNC_KIND_MAX];
static int sBeacons;
static int sScanCalls;
static u16 sScanChannel;
static void wm_done(void *arg)
{
    WMCallback *cb = arg;
    u16 id = cb->apiid;

    if (id >= WM_APIID_ASYNC_KIND_MAX) {
        return;
    }
    sWmCalls[id]++;
    sWmErr[id] = cb->errcode;
    if (id == WM_APIID_START_PARENT) {
        sWmState[id] = ((WMStartParentCallback *)arg)->state;
        if (sWmState[id] == WM_STATECODE_BEACON_SENT) {
            sBeacons++;
        }
    } else if (id == WM_APIID_START_SCAN) {
        sWmState[id] = ((WMStartScanCallback *)arg)->state;
        sScanChannel = ((WMStartScanCallback *)arg)->channel;
        sScanCalls++;
    } else if (id == WM_APIID_START_MP) {
        sWmState[id] = ((WMStartMPCallback *)arg)->state;
    } else if (id == WM_APIID_MEASURE_CHANNEL) {
        sWmState[id] = ((WMMeasureChannelCallback *)arg)->ccaBusyRatio;
    }
}

static void vblanks(int n)
{
    while (n-- > 0) {
        NitroPort_WmVBlank();
    }
}

static u32 sWmBuf[WM_SYSTEM_BUF_SIZE / sizeof(u32)] ATTRIBUTE_ALIGN(32);
static u16 sMpRecv[2048] ATTRIBUTE_ALIGN(32);
static u16 sMpSend[512] ATTRIBUTE_ALIGN(32);
static WMParentParam sParent ATTRIBUTE_ALIGN(32);
static WMBssDesc sBss ATTRIBUTE_ALIGN(32);

static void test_wireless(void)
{
    WMScanParam scan;
    const WMStatus *status;
    u16 data[8] ATTRIBUTE_ALIGN(32);

    /* The wireless driver starts (the Underground stops the game if not),
     * and its answer comes at a V-blank, not inside the request. */
    CHECK(WVR_StartUpAsync(GX_VRAM_ARM7_128_D, wvr_done, &sWvrDone) == WVR_RESULT_OPERATING);
    CHECK(sWvrDone == 0);
    vblanks(3);
    CHECK(sWvrDone == 1);
    CHECK(sWvrResult == WVR_RESULT_SUCCESS);

    /* The WM library hands the ARM7 its buffers by address: only where
     * addresses are 32 bits, as on the DS and the 3DS (host32). */
    if (sizeof(void *) == 4) {
        CHECK(WM_Initialize(sWmBuf, wm_done, 2) == WM_ERRCODE_OPERATING);
        CHECK(sWmCalls[WM_APIID_INITIALIZE] == 0);
        vblanks(1);
        CHECK(sWmCalls[WM_APIID_INITIALIZE] == 1);
        CHECK(sWmErr[WM_APIID_INITIALIZE] == WM_ERRCODE_SUCCESS);
        status = WMi_GetStatusAddress();
        CHECK(status != NULL && status->state == WM_STATE_IDLE);
        CHECK(WM_GetAllowedChannel() == NITRO_PORT_WM_CHANNELS);

        /* Every channel is quiet. */
        CHECK(WM_MeasureChannel(wm_done, 3, 17, 1, 30) == WM_ERRCODE_OPERATING);
        vblanks(2);
        CHECK(sWmCalls[WM_APIID_MEASURE_CHANNEL] == 1 && sWmState[WM_APIID_MEASURE_CHANNEL] == 0);

        /* A parent starts, starts MP and sends its beacons; nobody joins. */
        memset(&sParent, 0, sizeof(sParent));
        sParent.ggid = 0x12345;
        sParent.tgid = 1;
        sParent.channel = 7;
        sParent.maxEntry = 4;
        sParent.beaconPeriod = 200;
        sParent.parentMaxSize = 192;
        sParent.childMaxSize = 38;
        sParent.entryFlag = 1;
        CHECK(WM_SetParentParameter(wm_done, &sParent) == WM_ERRCODE_OPERATING);
        CHECK(WM_StartParent(wm_done) == WM_ERRCODE_OPERATING);
        vblanks(1);
        CHECK(sWmCalls[WM_APIID_SET_P_PARAM] == 1);
        CHECK(sWmCalls[WM_APIID_START_PARENT] == 1 && sWmState[WM_APIID_START_PARENT] == WM_STATECODE_PARENT_START);
        CHECK(WM_GetMPSendBufferSize() == WM_SIZE_MP_PARENT_SEND_BUFFER(192, 0));
        CHECK(WM_GetMPReceiveBufferSize() == WM_SIZE_MP_PARENT_RECEIVE_BUFFER(38, 4, 0));
        CHECK(WM_StartMP(wm_done, sMpRecv, (u16)WM_GetMPReceiveBufferSize(), sMpSend,
                         (u16)WM_GetMPSendBufferSize(), 1) == WM_ERRCODE_OPERATING);
        vblanks(1);
        CHECK(sWmState[WM_APIID_START_MP] == WM_STATECODE_MP_START && WMi_IsMP());
        vblanks(60);
        CHECK(sBeacons >= 4 && sBeacons <= 6);
        CHECK(WM_GetConnectedAIDs() == 0);
        CHECK(WM_GetLinkLevel() == WM_LINK_LEVEL_0);
        /* (no child: the library itself says so) */
        CHECK(WM_SetMPDataToPortEx(wm_done, NULL, data, 8, 0xffff, 14, WM_PRIORITY_NORMAL) == WM_ERRCODE_NO_CHILD);
        CHECK(WM_EndMP(wm_done) == WM_ERRCODE_OPERATING);
        vblanks(1);
        CHECK(WM_EndParent(wm_done) == WM_ERRCODE_OPERATING);
        vblanks(1);
        CHECK(sWmCalls[WM_APIID_END_PARENT] == 1 && status->state == WM_STATE_IDLE);
        sBeacons = 0;
        vblanks(30);
        CHECK(sBeacons == 0);

        /* A scan finds no parent, after its channel time. */
        memset(&scan, 0, sizeof(scan));
        scan.scanBuf = &sBss;
        scan.channel = 13;
        scan.maxChannelTime = 100;
        memset(scan.bssid, 0xff, sizeof(scan.bssid));
        CHECK(WM_StartScan(wm_done, &scan) == WM_ERRCODE_OPERATING);
        vblanks(5);
        CHECK(sScanCalls == 0);
        vblanks(1);
        CHECK(sScanCalls == 1 && sWmState[WM_APIID_START_SCAN] == WM_STATECODE_PARENT_NOT_FOUND);
        CHECK(sScanChannel == 13);
        CHECK(WM_EndScan(wm_done) == WM_ERRCODE_OPERATING);
        vblanks(1);
        CHECK(sWmCalls[WM_APIID_END_SCAN] == 1);

        /* And the radio closes: the library lets go of its buffers. */
        CHECK(WM_End(wm_done) == WM_ERRCODE_OPERATING);
        vblanks(1);
        CHECK(sWmCalls[WM_APIID_END] == 1 && sWmErr[WM_APIID_END] == WM_ERRCODE_SUCCESS);
        CHECK(WMi_GetStatusAddress() == NULL);
    }

    CHECK(WVR_TerminateAsync(wvr_done, &sWvrDone) == WVR_RESULT_OPERATING);
    vblanks(3);
    CHECK(sWvrDone == 2 && sWvrResult == WVR_RESULT_SUCCESS);
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
    test_backup_program_and_reopen();
    test_backup_one_file_write();
    test_rtc();
    test_power();
    test_touch();
    test_sound();
    test_wireless();
    printf("virtual ARM7 services: %d checks, %d failures\n", sChecks, sFailed);
    return sFailed ? 1 : 0;
}
