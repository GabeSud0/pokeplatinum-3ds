/*
 * The sound chip model as it was before it was made faster (PR #50's
 * version), kept for test_sound_chip: the faster one must give exactly the
 * same samples, registers and captured data. Its own names and its own
 * register block, so both can run side by side.
 */
#define NitroPort_SoundRegisters RefSoundRegisters
#define NitroSound_Reset RefSound_Reset
#define NitroSound_Mix RefSound_Mix
#include <nitro_port_sound_chip.h>
#include <limits.h>
#include <stdint.h>
#include <string.h>

#define SOUND_CLOCK 16756991ull
#define PHASE_ONE (1ull << 32)
#define CHANNEL_BASE(ch) (0x400u + (unsigned)(ch) * 16u)
#define REG_CNT     0u
#define REG_SAD     4u
#define REG_TIMER   8u
#define REG_LOOP    10u
#define REG_LENGTH  12u

volatile uint8_t NitroPort_SoundRegisters[NITRO_SOUND_REG_SIZE];

static unsigned char reg8(unsigned offset)
{
    return NitroPort_SoundRegisters[offset];
}

static uint16_t reg16(unsigned offset)
{
    return (uint16_t)(reg8(offset) | ((uint16_t)reg8(offset + 1) << 8));
}

static uint32_t reg32(unsigned offset)
{
    return (uint32_t)reg16(offset) | ((uint32_t)reg16(offset + 2) << 16);
}

static void put8(unsigned offset, uint8_t value)
{
    NitroPort_SoundRegisters[offset] = value;
}

static void put32(unsigned offset, uint32_t value)
{
    put8(offset, (uint8_t)value);
    put8(offset + 1, (uint8_t)(value >> 8));
    put8(offset + 2, (uint8_t)(value >> 16));
    put8(offset + 3, (uint8_t)(value >> 24));
}

static int16_t saturate(int32_t value)
{
    if (value < INT16_MIN) return INT16_MIN;
    if (value > INT16_MAX) return INT16_MAX;
    return (int16_t)value;
}

static int8_t pcm8(const uint8_t *data, uint32_t offset)
{
    return (int8_t)data[offset];
}

static int16_t pcm16(const uint8_t *data, uint32_t offset)
{
    return (int16_t)((unsigned)data[offset] | ((unsigned)data[offset + 1] << 8));
}

static const uint16_t sStep[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31,
    34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118,
    130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371,
    408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166,
    1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024,
    3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845,
    8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818,
    18500, 20350, 22385, 24623, 27086, 29794, 32767
};
static const int8_t sIndexDelta[8] = {-1,-1,-1,-1,2,4,6,8};

static void adpcm_init(NitroSoundVoice *v, const uint8_t *source)
{
    v->predictor = pcm16(source, 0);
    v->stepIndex = source[2] <= 88 ? source[2] : 88;
}

static void adpcm_step(NitroSoundVoice *v, uint8_t nibble)
{
    int32_t step = sStep[v->stepIndex];
    int32_t change = step >> 3;
    if (nibble & 1u) change += step >> 2;
    if (nibble & 2u) change += step >> 1;
    if (nibble & 4u) change += step;
    v->predictor += (nibble & 8u) ? -change : change;
    v->predictor = saturate(v->predictor);
    v->stepIndex += sIndexDelta[nibble & 7u];
    if (v->stepIndex < 0) v->stepIndex = 0;
    if (v->stepIndex > 88) v->stepIndex = 88;
}

static int16_t adpcm_at(NitroSoundVoice *v, const uint8_t *source,
                        uint32_t index)
{
    uint32_t nibble = index - 1;
    uint8_t byte = source[4u + nibble / 2u];
    adpcm_step(v, (nibble & 1u) ? byte >> 4 : byte & 15u);
    return (int16_t)v->predictor;
}

static uint32_t total_samples(unsigned format, uint32_t bytes)
{
    if (format == 0) return bytes;
    if (format == 1) return bytes / 2u;
    if (format == 2) return bytes >= 4u ? 1u + (bytes - 4u) * 2u : 0u;
    return 0;
}

static uint32_t loop_sample(unsigned format, uint32_t loopBytes)
{
    if (format == 0) return loopBytes;
    if (format == 1) return loopBytes / 2u;
    return loopBytes >= 4u ? 1u + (loopBytes - 4u) * 2u : 0u;
}

static void disable_channel(unsigned base, NitroSoundVoice *voice)
{
    put32(base + REG_CNT, reg32(base + REG_CNT) & 0x7fffffffu);
    voice->running = 0;
    voice->previousCtrl &= 0x7fffffffu;
}

static void restart_channel(NitroSoundVoice *voice, uint32_t ctrl,
                            unsigned base)
{
    const uint8_t *source = (const uint8_t *)(uintptr_t)reg32(base + REG_SAD);
    unsigned format = (ctrl >> 29) & 3u;
    voice->running = 1;
    voice->sampleIndex = 0;
    voice->phase = 0;
    voice->noise = 0x7fffu;
    voice->predictor = 0;
    voice->stepIndex = 0;
    voice->lastSample = 0;
    if (format == 2u && source) adpcm_init(voice, source);
}

static int32_t wave_sample(NitroSoundVoice *v, unsigned channel,
                           unsigned base, uint32_t ctrl)
{
    const uint8_t *source = (const uint8_t *)(uintptr_t)reg32(base + REG_SAD);
    unsigned format = (ctrl >> 29) & 3u;
    if (format == 3u) {
        if (channel >= 14) return (v->noise & 1u) ? -32767 : 32767;
        if (channel < 8) return 0;
        return (v->sampleIndex & 7u) <= ((ctrl >> 24) & 7u) ? 32767 : -32767;
    }
    if (!source) return 0;
    if (format == 0u) return (int32_t)pcm8(source, v->sampleIndex) * 256;
    if (format == 1u) return pcm16(source, v->sampleIndex * 2u);
    return v->predictor;
}

static void advance_voice(NitroSoundVoice *v, unsigned channel,
                          unsigned base, uint32_t ctrl)
{
    const unsigned format = (ctrl >> 29) & 3u;
    const unsigned repeat = (ctrl >> 27) & 3u;
    const uint8_t *source = (const uint8_t *)(uintptr_t)reg32(base + REG_SAD);
    uint32_t startByte = (uint32_t)reg16(base + REG_LOOP) * 4u;
    uint32_t endByte = startByte + (reg32(base + REG_LENGTH) & 0x3fffffu) * 4u;
    uint32_t nSamples = total_samples(format, endByte);
    uint32_t loopAt = loop_sample(format, startByte);

    if (format == 3u) {
        v->sampleIndex = (v->sampleIndex + 1u) & 7u;
        if (channel >= 14u) {
            unsigned carry = (v->noise ^ (v->noise >> 1u)) & 1u;
            v->noise = (uint16_t)((v->noise >> 1u) | (carry << 14u));
        }
        return;
    }
    if (!source || !nSamples) {
        disable_channel(base, v);
        return;
    }

    v->sampleIndex++;
    if (v->sampleIndex >= nSamples) {
        if (repeat != 1u || loopAt >= nSamples) {
            disable_channel(base, v);
            return;
        }
        v->sampleIndex = loopAt;
        if (format == 2u) {
            uint32_t i;
            adpcm_init(v, source);
            for (i = 1u; i <= loopAt; ++i) (void)adpcm_at(v, source, i);
        }
    } else if (format == 2u) {
        (void)adpcm_at(v, source, v->sampleIndex);
    }
}

static int32_t voice_mix(NitroSoundChip *chip, unsigned channel,
                         int32_t *left, int32_t *right)
{
    NitroSoundVoice *v = &chip->voice[channel];
    unsigned base = CHANNEL_BASE(channel);
    uint32_t ctrl = reg32(base);
    uint32_t period = 0x10000u - reg16(base + REG_TIMER);
    uint32_t shift = (ctrl >> 8) & 3u;
    uint32_t volume = ctrl & 127u;
    uint32_t pan = (ctrl >> 16) & 127u;
    int32_t sample, scaled;
    uint64_t step;
    unsigned guard = 0;

    if (!(ctrl & 0x80000000u)) {
        v->running = 0;
        v->previousCtrl = ctrl;
        return 0;
    }
    if (!v->running || !(v->previousCtrl & 0x80000000u))
        restart_channel(v, ctrl, base);
    v->previousCtrl = ctrl;

    sample = wave_sample(v, channel, base, ctrl);
    /* 00=none, 01=1 bit, 10=2 bits, 11=4 bits */
    scaled = (sample * (int32_t)volume) / 127;
    scaled >>= shift == 3u ? 4u : shift;
    *left += (scaled * (int32_t)(127u - pan)) / 127;
    *right += (scaled * (int32_t)pan) / 127;

    step = (SOUND_CLOCK << 32) / ((uint64_t)period * chip->outputRate);
    v->phase += step;
    while (v->phase >= PHASE_ONE && v->running && guard++ < 2048u) {
        v->phase -= PHASE_ONE;
        advance_voice(v, channel, base, ctrl);
    }
    return scaled;
}

static void do_capture(NitroSoundChip *chip, unsigned slot,
                       int32_t mixerL, int32_t mixerR,
                       int32_t ch0, int32_t ch2)
{
    NitroSoundCapture *cap = &chip->capture[slot];
    unsigned controlReg = 0x508u + slot;
    unsigned base = 0x510u + slot * 8u;
    uint8_t ctl = reg8(controlReg);
    unsigned bytesPerSample = (ctl & 8u) ? 1u : 2u;
    uint32_t bufferSize = (uint32_t)reg16(base + 4u) * 4u;
    uint8_t *dst = (uint8_t *)(uintptr_t)reg32(base);
    int32_t signal;

    if (!(ctl & 128u)) {
        cap->running = 0;
        return;
    }
    if (!cap->running) {
        cap->index = 0;
        cap->running = 1;
    }
    if (!dst || bufferSize < bytesPerSample) return;
    signal = (ctl & 2u) ? (slot == 0 ? ch0 : ch2)
                       : (slot == 0 ? mixerL : mixerR);
    if (cap->index + bytesPerSample > bufferSize) {
        if (!(ctl & 4u)) {
            put8(controlReg, (uint8_t)(ctl & 127u));
            cap->running = 0;
            return;
        }
        cap->index = 0;
    }
    if (bytesPerSample == 1u) {
        dst[cap->index] = (uint8_t)((int8_t)(saturate(signal) >> 8));
    } else {
        int16_t value = saturate(signal);
        dst[cap->index] = (uint8_t)value;
        dst[cap->index + 1u] = (uint8_t)((uint16_t)value >> 8);
    }
    cap->index += bytesPerSample;
}

void NitroSound_Reset(NitroSoundChip *chip, uint32_t outputRate)
{
    if (!chip) return;
    memset(chip, 0, sizeof(*chip));
    memset((void *)NitroPort_SoundRegisters, 0, sizeof(NitroPort_SoundRegisters));
    chip->outputRate = outputRate ? outputRate : NITRO_SOUND_RATE;
    put8(0x500u, 127u);
    put8(0x501u, 128u);
}

void NitroSound_Mix(NitroSoundChip *chip, int16_t *stereo, size_t frames)
{
    size_t i;
    if (!chip || !stereo || !chip->outputRate) return;
    for (i = 0; i < frames; ++i) {
        int32_t mixL = 0, mixR = 0;
        int32_t special0 = 0, special2 = 0;
        int32_t outputL, outputR;
        unsigned channel, outputSelector = reg8(0x501u);
        for (channel = 0; channel < NITRO_SOUND_CHANNEL_COUNT; ++channel) {
            int32_t value = voice_mix(chip, channel, &mixL, &mixR);
            if (channel == 0) special0 = value;
            if (channel == 2) special2 = value;
        }
        do_capture(chip, 0, mixL, mixR, special0, special2);
        do_capture(chip, 1, mixL, mixR, special0, special2);
        if (outputSelector & 128u) {
            uint32_t volume = reg8(0x500u) & 127u;
            outputL = (mixL * (int32_t)volume) / 127;
            outputR = (mixR * (int32_t)volume) / 127;
        } else {
            outputL = outputR = 0;
        }
        stereo[2u * i] = saturate(outputL);
        stereo[2u * i + 1u] = saturate(outputR);
    }
}
