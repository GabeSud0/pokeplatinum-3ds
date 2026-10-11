#ifndef NITRO_PORT_SOUND_CHIP_H
#define NITRO_PORT_SOUND_CHIP_H
/* Portable software sound chip: no SDK, data packs or libctru required. */
#include <stddef.h>
#include <stdint.h>
#define DS_SOUND_CHANNELS 16
#define DS_SOUND_CAPTURES 2
#define DS_SOUND_CLOCK 33513982u
#define DS_SOUND_RATE 32768u
typedef enum {
    DS_SOUND_PCM8 = 0, DS_SOUND_PCM16 = 1, DS_SOUND_ADPCM = 2,
    DS_SOUND_PSG = 3, DS_SOUND_NOISE = 4
} DsSoundFormat;
typedef struct {
    const uint8_t *data;
    uint32_t byte_count;
    uint32_t loop_start, loop_end, pos;
    uint64_t phase;
    uint32_t step;
    uint16_t timer, noise;
    int16_t predictor, loop_predictor, sample;
    uint8_t volume, pan, shift, format, duty, active, repeat;
    int8_t index, loop_index;
    uint8_t loop_initialized;
} DsSoundChannel;
typedef struct {
    int16_t *buffer;
    uint32_t capacity, position;
    uint8_t enabled, repeat, source;
} DsSoundCapture;
typedef struct {
    DsSoundChannel channels[DS_SOUND_CHANNELS];
    DsSoundCapture captures[DS_SOUND_CAPTURES];
    uint32_t output_rate;
    uint8_t master_volume;
} DsSoundChip;
void ds_sound_init(DsSoundChip *chip, uint32_t rate);
void ds_sound_set_pcm(DsSoundChip *chip, int channel, DsSoundFormat format,
                      const void *data, uint32_t bytes, uint32_t loop_start,
                      uint32_t loop_end, int repeat);
void ds_sound_set_generator(DsSoundChip *chip, int channel, DsSoundFormat format, uint8_t duty);
void ds_sound_control(DsSoundChip *chip, int channel, uint16_t timer,
                      uint8_t volume, uint8_t pan, uint8_t shift, int active);
void ds_sound_capture(DsSoundChip *chip, int unit, int16_t *buffer,
                      uint32_t frames, int repeat, int source);
void ds_sound_mix(DsSoundChip *chip, int16_t *stereo, size_t frames);
#endif
