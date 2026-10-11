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
} NitroSoundVoice;

typedef struct NitroSoundCapture {
    uint32_t index;
    uint8_t running;
} NitroSoundCapture;

typedef struct NitroSoundChip {
    NitroSoundVoice voice[NITRO_SOUND_CHANNEL_COUNT];
    NitroSoundCapture capture[2];
    uint32_t outputRate;
} NitroSoundChip;

void NitroSound_Reset(NitroSoundChip *chip, uint32_t outputRate);
void NitroSound_Mix(NitroSoundChip *chip, int16_t *stereo, size_t frames);
#endif
