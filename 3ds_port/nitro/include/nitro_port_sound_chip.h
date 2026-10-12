/* Independent DS sound register engine (plain C99, no libctru/NitroSDK).
 * ARM7 SDK sound-register writes target NitroPort_SoundRegisters; the mixer
 * consumes their state at 32,768 samples/s, producing interleaved signed PCM.
 */
#ifndef NITRO_PORT_SOUND_CHIP_H
#define NITRO_PORT_SOUND_CHIP_H
#include <stddef.h>
#include <stdint.h>
#define NITRO_SOUND_RATE 32768u
#define NITRO_SOUND_REG_SIZE 0x600u
#define NITRO_SOUND_CHANNEL_COUNT 16u
extern volatile uint8_t NitroPort_SoundRegisters[NITRO_SOUND_REG_SIZE];

typedef struct NitroSoundVoice {
    uint32_t previousCtrl;
    uint32_t sampleIndex;
    uint64_t phase;
    int32_t predictor;
    int32_t lastSample;
    int32_t stepIndex;
    uint16_t noise;
    uint8_t adpcmByte;
    uint8_t adpcmNibble;
    uint8_t running;
    /* Kept by the mixer so it doesn't redo work every sample: the step for
     * the last timer value, and the ADPCM decoder's state at the loop
     * start (as the DS keeps it), for the loop start and data it was
     * saved for. */
    uint8_t loopSaved;
    uint8_t adpcmClean;    /* the ADPCM state is adpcmSource's, decoded from its start */
    uint32_t adpcmSource;
    uint32_t stepPeriod;
    uint32_t stepRate;
    uint64_t step;
    uint32_t loopSource;
    uint32_t loopAt;
    int32_t loopPredictor;
    int32_t loopStepIndex;
} NitroSoundVoice;

typedef struct NitroSoundCapture {
    uint32_t index;
    uint8_t running;
    /* A capture takes samples at the rate of channel 1's (capture 0) or
     * channel 3's (capture 1) timer, as the DS's does. */
    uint64_t phase;
    uint32_t stepPeriod;
    uint32_t stepRate;
    uint64_t step;
} NitroSoundCapture;

typedef struct NitroSoundChip {
    NitroSoundVoice voice[NITRO_SOUND_CHANNEL_COUNT];
    NitroSoundCapture capture[2];
    uint32_t outputRate;
} NitroSoundChip;

void NitroSound_Reset(NitroSoundChip *chip, uint32_t outputRate);
void NitroSound_Mix(NitroSoundChip *chip, int16_t *stereo, size_t frames);
/* The driver is about to set channel ch's enable bit: if it's 0 now (the
 * channel stopped, perhaps just before, and set up again), the channel
 * starts over from its beginning, as on the DS. */
void NitroSound_KeyOn(NitroSoundChip *chip, unsigned channel);
#endif
