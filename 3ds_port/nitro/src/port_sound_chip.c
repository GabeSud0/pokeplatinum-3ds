/*
 * Software model of the DS sound hardware.
 * Only this file owns the playback cursor; the original ARM7 driver writes
 * its control/timer/source registers. All public operations are plain C99.
 *
 * The ARM7 audio service serializes register access with mixing. Registers
 * retain the DS layout so SDK ARM7 snd_channel.c/snd_capture.c can run
 * without modification to their command logic.
 */
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
    voice->loopSaved = 0;
    voice->adpcmClean = 0;
    if (format == 2u && source) {
        adpcm_init(voice, source);
        voice->adpcmClean = 1;
        voice->adpcmSource = (uint32_t)(uintptr_t)source;
    }
}

/*
 * One channel during one NitroSound_Mix call. Only the mixer itself changes
 * a channel's registers while it mixes (it switches a channel off at the end
 * of its sound), so they're read once per call, not once per sample, and the
 * sound's length, loop point and step are worked out once too. What comes
 * out is exactly what reading them every sample gave.
 */
typedef struct Channel {
    NitroSoundVoice *v;
    unsigned channel;
    unsigned base;
    unsigned format;
    unsigned repeat;
    uint32_t ctrl;
    uint32_t sourceAddr;
    const uint8_t *source;
    uint32_t nSamples;
    uint32_t loopAt;
    uint64_t step;
    int32_t volume;
    int32_t pan;
    unsigned shift;
} Channel;

static int32_t wave_sample(const Channel *c)
{
    const NitroSoundVoice *v = c->v;

    switch (c->format) {
    case 0:
        return c->source ? (int32_t)pcm8(c->source, v->sampleIndex) * 256 : 0;
    case 1:
        return c->source ? pcm16(c->source, v->sampleIndex * 2u) : 0;
    case 2:
        return c->source ? v->predictor : 0;
    default:
        if (c->channel >= 14) return (v->noise & 1u) ? -32767 : 32767;
        if (c->channel < 8) return 0;
        return (v->sampleIndex & 7u) <= ((c->ctrl >> 24) & 7u) ? 32767 : -32767;
    }
}

/* The ADPCM decoder's state at the loop start, as it was when the sound
 * first got there; without one saved (or for other data), decoded again
 * from the start. */
static void adpcm_to_loop(NitroSoundVoice *v, const Channel *c)
{
    uint32_t i;

    if (c->loopAt != 0 && v->loopSaved && v->loopSource == c->sourceAddr &&
        v->loopAt == c->loopAt) {
        v->predictor = v->loopPredictor;
        v->stepIndex = v->loopStepIndex;
        return;
    }
    adpcm_init(v, c->source);
    for (i = 1u; i <= c->loopAt; ++i) (void)adpcm_at(v, c->source, i);
    v->adpcmClean = 1;
    v->adpcmSource = c->sourceAddr;
    if (c->loopAt != 0) {
        v->loopSaved = 1;
        v->loopSource = c->sourceAddr;
        v->loopAt = c->loopAt;
        v->loopPredictor = v->predictor;
        v->loopStepIndex = v->stepIndex;
    }
}

static void advance_voice(const Channel *c)
{
    NitroSoundVoice *v = c->v;

    if (c->format == 3u) {
        v->sampleIndex = (v->sampleIndex + 1u) & 7u;
        if (c->channel >= 14u) {
            unsigned carry = (v->noise ^ (v->noise >> 1u)) & 1u;
            v->noise = (uint16_t)((v->noise >> 1u) | (carry << 14u));
        }
        return;
    }
    if (!c->source || !c->nSamples) {
        disable_channel(c->base, v);
        return;
    }

    v->sampleIndex++;
    if (v->sampleIndex >= c->nSamples) {
        if (c->repeat != 1u || c->loopAt >= c->nSamples) {
            disable_channel(c->base, v);
            return;
        }
        v->sampleIndex = c->loopAt;
        if (c->format == 2u) adpcm_to_loop(v, c);
    } else if (c->format == 2u) {
        (void)adpcm_at(v, c->source, v->sampleIndex);
        if (v->sampleIndex == c->loopAt && v->adpcmClean) {
            v->loopSaved = 1;
            v->loopSource = c->sourceAddr;
            v->loopAt = c->loopAt;
            v->loopPredictor = v->predictor;
            v->loopStepIndex = v->stepIndex;
        }
    }
}

/* Read a channel's registers at the start of a mix; 0 if it's off. */
static int start_channel(NitroSoundChip *chip, unsigned channel, Channel *c)
{
    NitroSoundVoice *v = &chip->voice[channel];
    unsigned base = CHANNEL_BASE(channel);
    uint32_t ctrl = reg32(base);
    uint32_t period, startByte, endByte, shift;

    if (!(ctrl & 0x80000000u)) {
        v->running = 0;
        v->previousCtrl = ctrl;
        return 0;
    }
    if (!v->running || !(v->previousCtrl & 0x80000000u))
        restart_channel(v, ctrl, base);
    v->previousCtrl = ctrl;

    c->v = v;
    c->channel = channel;
    c->base = base;
    c->ctrl = ctrl;
    c->format = (ctrl >> 29) & 3u;
    c->repeat = (ctrl >> 27) & 3u;
    c->sourceAddr = reg32(base + REG_SAD);
    c->source = (const uint8_t *)(uintptr_t)c->sourceAddr;
    if (v->adpcmClean && v->adpcmSource != c->sourceAddr) {
        /* The data changed under a sound playing on: its decoder state no
         * longer comes from this data alone; don't keep it for the loop. */
        v->adpcmClean = 0;
    }
    startByte = (uint32_t)reg16(base + REG_LOOP) * 4u;
    endByte = startByte + (reg32(base + REG_LENGTH) & 0x3fffffu) * 4u;
    c->nSamples = total_samples(c->format, endByte);
    c->loopAt = loop_sample(c->format, startByte);
    c->volume = (int32_t)(ctrl & 127u);
    c->pan = (int32_t)((ctrl >> 16) & 127u);
    shift = (ctrl >> 8) & 3u;
    c->shift = shift == 3u ? 4u : shift;

    /* The step only changes with the timer: no 64-bit division (slow on
     * the 3DS's CPU, which has no divide instruction) for every sample. */
    period = 0x10000u - reg16(base + REG_TIMER);
    if (v->stepPeriod != period || v->stepRate != chip->outputRate) {
        v->stepPeriod = period;
        v->stepRate = chip->outputRate;
        v->step = (SOUND_CLOCK << 32) / ((uint64_t)period * chip->outputRate);
    }
    c->step = v->step;
    return 1;
}

/* One sample of a running channel: adds it to the mix, returns it. */
static int32_t voice_mix(Channel *c, int32_t *left, int32_t *right)
{
    NitroSoundVoice *v = c->v;
    int32_t scaled = (wave_sample(c) * c->volume) / 127;
    unsigned guard = 0;

    scaled >>= c->shift;
    *left += (scaled * (127 - c->pan)) / 127;
    *right += (scaled * c->pan) / 127;

    v->phase += c->step;
    while (v->phase >= PHASE_ONE && v->running && guard++ < 2048u) {
        v->phase -= PHASE_ONE;
        advance_voice(c);
    }
    return scaled;
}

/*
 * Sound capture (SNDCAP0CNT/SNDCAP1CNT, 0x508/0x509): bit 7 on, bit 3 PCM8
 * (else PCM16), bit 2 one-shot (else it loops over its buffer: the SDK's
 * SND_CAPTURE_REPEAT_YES is 0), bit 1 records channel 0/2 (else the left /
 * right mixer). It records at the rate of channel 1's / channel 3's timer,
 * the channel that plays its buffer back: NitroSystem's reverb records the
 * mix into a looping buffer and plays it back quietly on those channels.
 * (Bit 0, adding channel 1/3 into channel 0/2, isn't done: nothing in the
 * game uses it.)
 */
static void do_capture(NitroSoundChip *chip, unsigned slot, int32_t signal)
{
    NitroSoundCapture *cap = &chip->capture[slot];
    unsigned controlReg = 0x508u + slot;
    unsigned base = 0x510u + slot * 8u;
    uint8_t ctl = reg8(controlReg);
    unsigned bytesPerSample = (ctl & 8u) ? 1u : 2u;
    uint32_t bufferSize = (uint32_t)reg16(base + 4u) * 4u;
    uint8_t *dst = (uint8_t *)(uintptr_t)reg32(base);
    uint32_t period;

    if (!(ctl & 128u)) {
        cap->running = 0;
        return;
    }
    if (!cap->running) {
        cap->index = 0;
        cap->phase = 0;
        cap->running = 1;
    }
    if (!dst || bufferSize < bytesPerSample) return;

    period = 0x10000u - reg16(CHANNEL_BASE(slot ? 3u : 1u) + REG_TIMER);
    if (cap->stepPeriod != period || cap->stepRate != chip->outputRate) {
        cap->stepPeriod = period;
        cap->stepRate = chip->outputRate;
        cap->step = (SOUND_CLOCK << 32) / ((uint64_t)period * chip->outputRate);
    }
    cap->phase += cap->step;
    while (cap->phase >= PHASE_ONE) {
        cap->phase -= PHASE_ONE;
        if (cap->index + bytesPerSample > bufferSize) {
            if (ctl & 4u) {             /* one-shot: done */
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

void NitroSound_KeyOn(NitroSoundChip *chip, unsigned channel)
{
    if (!chip || channel >= NITRO_SOUND_CHANNEL_COUNT) return;
    if (!(reg8(CHANNEL_BASE(channel) + 3u) & 0x80u)) {
        /* The next mix starts it again (start_channel). */
        chip->voice[channel].running = 0;
    }
}

void NitroSound_Mix(NitroSoundChip *chip, int16_t *stereo, size_t frames)
{
    Channel channels[NITRO_SOUND_CHANNEL_COUNT];
    unsigned count = 0, channel, k;
    unsigned control;
    int32_t masterVolume;
    int capturing, bypass1, bypass3;
    unsigned leftFrom, rightFrom;
    size_t i;

    if (!chip || !stereo || !chip->outputRate || frames == 0) return;
    for (channel = 0; channel < NITRO_SOUND_CHANNEL_COUNT; ++channel) {
        if (start_channel(chip, channel, &channels[count])) ++count;
    }
    /* SOUNDCNT (0x500): bits 0-6 master volume, 8-9 / 10-11 what the left /
     * right output plays (the mixer, channel 1, channel 3 or both), 12 / 13
     * channel 1 / 3 kept out of the mixer, 15 on. */
    control = reg8(0x501u);
    masterVolume = (int32_t)(reg8(0x500u) & 127u);
    leftFrom = control & 3u;
    rightFrom = (control >> 2) & 3u;
    bypass1 = (control & 0x10u) != 0;
    bypass3 = (control & 0x20u) != 0;
    capturing = ((reg8(0x508u) | reg8(0x509u)) & 128u) ||
                chip->capture[0].running || chip->capture[1].running;

    for (i = 0; i < frames; ++i) {
        int32_t mixL = 0, mixR = 0;
        int32_t ch1L = 0, ch1R = 0, ch3L = 0, ch3R = 0;
        int32_t special0 = 0, special2 = 0;
        int32_t outputL, outputR;

        for (k = 0; k < count; ++k) {
            Channel *c = &channels[k];
            int32_t value;

            if (!c->v->running) {
                /* It ended earlier in this mix: silent from then on. */
                continue;
            }
            if (c->channel == 1) {
                (void)voice_mix(c, &ch1L, &ch1R);
                if (!bypass1) {
                    mixL += ch1L;
                    mixR += ch1R;
                }
                continue;
            }
            if (c->channel == 3) {
                (void)voice_mix(c, &ch3L, &ch3R);
                if (!bypass3) {
                    mixL += ch3L;
                    mixR += ch3R;
                }
                continue;
            }
            value = voice_mix(c, &mixL, &mixR);
            if (c->channel == 0) special0 = value;
            if (c->channel == 2) special2 = value;
        }
        if (capturing) {
            do_capture(chip, 0, (reg8(0x508u) & 2u) ? special0 : mixL);
            do_capture(chip, 1, (reg8(0x509u) & 2u) ? special2 : mixR);
        }
        if (control & 128u) {
            int32_t left = leftFrom == 0 ? mixL : leftFrom == 1 ? ch1L
                         : leftFrom == 2 ? ch3L : ch1L + ch3L;
            int32_t right = rightFrom == 0 ? mixR : rightFrom == 1 ? ch1R
                          : rightFrom == 2 ? ch3R : ch1R + ch3R;

            outputL = (left * masterVolume) / 127;
            outputR = (right * masterVolume) / 127;
        } else {
            outputL = outputR = 0;
        }
        stereo[2u * i] = saturate(outputL);
        stereo[2u * i + 1u] = saturate(outputR);
    }
    /* Channels that ended: as reading their (now off) registers left them. */
    for (k = 0; k < count; ++k) {
        if (!channels[k].v->running) {
            channels[k].v->previousCtrl = reg32(channels[k].base);
        }
    }
}
