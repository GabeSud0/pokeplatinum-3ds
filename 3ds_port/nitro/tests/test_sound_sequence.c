/*
 * Tiny synthetic SDAT-style sequence, bank and PCM wave archive.
 * Exercises original ARM9 SND_StartSeq -> PXI -> original ARM7 sequencer
 * -> 16-channel sound registers -> portable stereo mixer. No Nintendo data.
 */
#include <nitro.h>
#include <nitro_port.h>
#include <nitro_port_sound_chip.h>
#include <nitro/snd/common/bank.h>
#include <nitro/snd/common/mml.h>

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int sChecks, sFailures;
#define CHECK(test) do { ++sChecks; if (!(test)) { ++sFailures; \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #test); \
} } while (0)

static union { uint64_t align; uint8_t bytes[512]; } sBankStorage;
static union { uint64_t align; uint8_t bytes[512]; } sArcStorage;
static int sAlarmCount;
static void synthetic_alarm(void *arg)
{
    ++*(int *)arg;
}
extern void NitroPort_SoundPumpReplies(void);
extern void NitroPort_SoundRender(int16_t *stereo, size_t frames);

static const uint8_t sSequence[] = {
    SND_MML_PRG, 0,
    60, 120, 0x7f,                   /* MIDI note: middle C, velocity, length */
    SND_MML_WAIT, 0x7f,
    SND_MML_FIN
};

static SNDBankData *synthetic_bank(void)
{
    SNDBankData *bank = (SNDBankData *)sBankStorage.bytes;
    SNDWaveArc *arc = (SNDWaveArc *)sArcStorage.bytes;
    uint32_t *instrument = (uint32_t *)(sBankStorage.bytes +
                                        offsetof(SNDBankData, instOffset));
    uint32_t *waveOffset = (uint32_t *)(sArcStorage.bytes +
                                      offsetof(SNDWaveArc, waveOffset));
    SNDInstParam *param = (SNDInstParam *)(sBankStorage.bytes + 192u);
    SNDWaveData *wave = (SNDWaveData *)(sArcStorage.bytes + 128u);
    unsigned i;

    memset(&sBankStorage, 0, sizeof(sBankStorage));
    memset(&sArcStorage, 0, sizeof(sArcStorage));
    bank->instCount = 1;
    *instrument = (192u << 8) | SND_INST_PCM;
    param->wave[0] = 0;
    param->wave[1] = 0;
    param->original_key = 60;
    param->attack = 0;
    param->decay = 0;
    param->sustain = 127;
    param->release = 10;
    param->pan = 64;
    bank->waveArcLink[0].waveArc = arc;
    arc->waveCount = 1;
    *waveOffset = 128u;

    wave->param.format = SND_WAVE_FORMAT_PCM8;
    wave->param.loopflag = TRUE;
    wave->param.rate = NITRO_SOUND_RATE;
    wave->param.timer = 512;
    wave->param.loopstart = 0;
    wave->param.looplen = 32; /* 128 invented PCM8 samples */
    for (i = 0; i < 128; ++i)
        wave->samples[i] = (i & 3u) < 2u ? 96u : (uint8_t)-96;
    return bank;
}

int main(void)
{
    NitroPortFirmware firmware;
    SNDBankData *bank = synthetic_bank();
    int16_t stereo[256 * 2];   /* the largest render below: 256 frames */
    uint32_t tag;
    unsigned i, audible = 0;

    memset(&firmware, 0, sizeof(firmware));
    NitroPort_BootFirmware(&firmware);
    NitroPort_InitArm7Services();
    OS_Init();
    (void)OS_EnableIrq();
    (void)OS_EnableInterrupts();
    SND_Init();

    SND_StartSeq(0, sSequence, 0, bank);
    tag = SND_GetCurrentCommandTag();
    CHECK(SND_FlushCommand(SND_COMMAND_BLOCK));
    NitroPort_SoundRender(stereo, 128);
    SND_WaitForCommandProc(tag);
    CHECK((SND_GetPlayerStatus() & 1u) != 0);

    for (i = 0; i < 80; ++i) {
        unsigned s;
        NitroPort_SoundRender(stereo, 128);
        for (s = 0; s < 128 * 2; ++s)
            audible += stereo[s] != 0;
    }
    CHECK(audible > 128u);
    CHECK((SND_GetPlayerStatus() & 1u) != 0);

    SND_StopSeq(0);
    tag = SND_GetCurrentCommandTag();
    CHECK(SND_FlushCommand(SND_COMMAND_BLOCK));
    NitroPort_SoundRender(stereo, 128);
    SND_WaitForCommandProc(tag);
    CHECK((SND_GetPlayerStatus() & 1u) == 0);

    /* NitroSystem's stream player needs a real ARM7 alarm -> PXI reply.
     * Alarm callback must run on the ARM9/game thread, not audio thread. */
    SND_SetupAlarm(0, 1, 0, synthetic_alarm, &sAlarmCount);
    SND_StartTimer(0, 0, 1u << 0, 0);
    tag = SND_GetCurrentCommandTag();
    CHECK(SND_FlushCommand(SND_COMMAND_BLOCK));
    NitroPort_SoundRender(stereo, 256);
    SND_WaitForCommandProc(tag);
    NitroPort_SoundPumpReplies();
    CHECK(sAlarmCount == 1);
    NitroPort_SoundRender(stereo, 256);
    NitroPort_SoundPumpReplies();
    CHECK(sAlarmCount == 1);  /* one-shot, not periodic */

    /* Sound time runs at the DS's speed: the driver ticks every 2728 OS
     * ticks (5.2 ms), and a periodic alarm of 4 such ticks (20.8 ms) fires
     * 48 times in a second of sound (32768 samples), not 91 (which is what
     * taking the tick for 2728 microseconds gave). */
    sAlarmCount = 0;
    SND_SetupAlarm(1, 2728u * 4u, 2728u * 4u, synthetic_alarm, &sAlarmCount);
    SND_StartTimer(0, 0, 1u << 1, 0);
    tag = SND_GetCurrentCommandTag();
    CHECK(SND_FlushCommand(SND_COMMAND_BLOCK));
    for (i = 0; i < 32768u / 256u; ++i) {
        NitroPort_SoundRender(stereo, 256);
        NitroPort_SoundPumpReplies();
    }
    SND_WaitForCommandProc(tag);
    SND_StopTimer(0, 0, 1u << 1, 0);
    CHECK(SND_FlushCommand(SND_COMMAND_BLOCK));
    NitroPort_SoundRender(stereo, 256);
    printf("periodic alarm: %d times in a second of sound\n", sAlarmCount);
    CHECK(sAlarmCount >= 46 && sAlarmCount <= 49);

    printf("synthetic ARM9->ARM7 sequence: %d checks, %d failures\n",
           sChecks, sFailures);
    return sFailures != 0;
}
