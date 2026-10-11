#include "sound_chip.h"
#include <string.h>
static const int8_t index_table[16] = {-1,-1,-1,-1,2,4,6,8,-1,-1,-1,-1,2,4,6,8};
static const int16_t step_table[89] = {
    7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,
    34,37,41,45,50,55,60,66,73,80,88,97,107,118,130,143,
    157,173,190,209,230,253,279,307,337,371,408,449,494,544,
    598,658,724,796,876,963,1060,1166,1282,1411,1552,1707,
    1878,2066,2272,2499,2749,3024,3327,3660,4026,4428,4871,
    5358,5894,6484,7132,7845,8630,9493,10442,11487,12635,
    13899,15289,16818,18500,20350,22385,24623,27086,29794,32767
};
static int clamp16(int x) { return x < -32768 ? -32768 : x > 32767 ? 32767 : x; }
static uint32_t sample_count(const DsSoundChannel *c) {
    if (c->format == DS_SOUND_PCM8) return c->byte_count;
    if (c->format == DS_SOUND_PCM16) return c->byte_count / 2;
    if (c->format == DS_SOUND_ADPCM)
        return c->byte_count < 4 ? 0 : 1 + (c->byte_count - 4) * 2;
    return 0xffffffffu;
}
void ds_sound_init(DsSoundChip *chip, uint32_t rate) {
    if (!chip) return;
    memset(chip, 0, sizeof(*chip));
    chip->output_rate = rate ? rate : DS_SOUND_RATE;
    chip->master_volume = 127;
}
void ds_sound_set_pcm(DsSoundChip *chip, int id, DsSoundFormat fmt,
                      const void *data, uint32_t bytes, uint32_t loop_start,
                      uint32_t loop_end, int repeat) {
    DsSoundChannel *c;
    uint32_t count;
    if (!chip || id < 0 || id >= DS_SOUND_CHANNELS || fmt > DS_SOUND_ADPCM) return;
    c = &chip->channels[id];
    memset(c, 0, sizeof(*c));
    c->data = data;
    c->byte_count = bytes;
    c->format = (uint8_t)fmt;
    c->repeat = repeat != 0;
    c->noise = 0x7fff;
    c->volume = 127;
    c->pan = 64;
    count = sample_count(c);
    c->loop_start = loop_start < count ? loop_start : 0;
    c->loop_end = loop_end && loop_end <= count ? loop_end : count;
    if (c->loop_end <= c->loop_start) c->repeat = 0;
    if (fmt == DS_SOUND_ADPCM && bytes >= 4 && data) {
        const uint8_t *p = data;
        c->predictor = (int16_t)((unsigned)p[0] | ((unsigned)p[1] << 8));
        c->index = p[2] > 88 ? 88 : (int8_t)p[2];
    }
}
void ds_sound_set_generator(DsSoundChip *chip, int id, DsSoundFormat fmt, uint8_t duty) {
    DsSoundChannel *c;
    if (!chip || id < 0 || id >= DS_SOUND_CHANNELS) return;
    if ((fmt == DS_SOUND_PSG && (id < 8 || id > 13)) ||
        (fmt == DS_SOUND_NOISE && (id < 14 || id > 15)) ||
        (fmt != DS_SOUND_PSG && fmt != DS_SOUND_NOISE)) return;
    c = &chip->channels[id];
    memset(c, 0, sizeof(*c));
    c->format = (uint8_t)fmt;
    c->duty = duty & 7;
    c->noise = 0x7fff;
    c->volume = 127;
    c->pan = 64;
}
void ds_sound_control(DsSoundChip *chip, int id, uint16_t timer, uint8_t volume,
                      uint8_t pan, uint8_t shift, int active) {
    DsSoundChannel *c;
    if (!chip || id < 0 || id >= DS_SOUND_CHANNELS) return;
    c = &chip->channels[id];
    c->timer = timer;
    c->volume = volume > 127 ? 127 : volume;
    c->pan = pan > 127 ? 127 : pan;
    c->shift = shift == 3 ? 4 : shift > 4 ? 4 : shift;
    c->active = active != 0;
    c->step = 2u * (65536u - timer);
}
void ds_sound_capture(DsSoundChip *chip, int unit, int16_t *buffer,
                      uint32_t frames, int repeat, int source) {
    DsSoundCapture *c;
    if (!chip || unit < 0 || unit >= DS_SOUND_CAPTURES) return;
    c = &chip->captures[unit];
    c->buffer = buffer;
    c->capacity = frames;
    c->position = 0;
    c->enabled = buffer != NULL && frames > 0;
    c->repeat = repeat != 0;
    c->source = source != 0;
}
static int adpcm_sample(DsSoundChannel *c, uint32_t sample) {
    unsigned int nibble;
    int step, diff, idx;
    if (sample == 0) return c->predictor;
    sample--;
    nibble = (c->data[4 + sample / 2] >> ((sample & 1) * 4)) & 15;
    idx = c->index;
    step = step_table[idx];
    diff = step >> 3;
    if (nibble & 1) diff += step >> 2;
    if (nibble & 2) diff += step >> 1;
    if (nibble & 4) diff += step;
    if (nibble & 8) diff = -diff;
    c->predictor = (int16_t)clamp16((int)c->predictor + diff);
    idx += index_table[nibble];
    c->index = (int8_t)(idx < 0 ? 0 : idx > 88 ? 88 : idx);
    return c->predictor;
}
static int advance_sample(DsSoundChannel *c) {
    uint32_t pos = c->pos;
    int value;
    if (!c->active) return 0;
    if (c->format == DS_SOUND_PSG) {
        value = (pos & 7u) <= c->duty ? -32767 : 32767;
        c->pos = (pos + 1) & 7;
        return value;
    }
    if (c->format == DS_SOUND_NOISE) {
        unsigned feedback = (c->noise ^ (c->noise >> 1)) & 1;
        value = c->noise & 1 ? -32767 : 32767;
        c->noise = (uint16_t)((c->noise >> 1) | (feedback << 14));
        return value;
    }
    if (!c->data || c->loop_end == 0) {
        c->active = 0;
        return 0;
    }
    if (pos >= c->loop_end) {
        if (!c->repeat || c->loop_start >= c->loop_end) {
            c->active = 0;
            return 0;
        }
        pos = c->loop_start;
        if (c->format == DS_SOUND_ADPCM && c->loop_initialized) {
            c->predictor = c->loop_predictor;
            c->index = c->loop_index;
        }
    }
    if (c->format == DS_SOUND_PCM8) {
        value = (int)(int8_t)c->data[pos] * 256;
    } else if (c->format == DS_SOUND_PCM16) {
        uint32_t off = pos * 2;
        value = (int16_t)((unsigned)c->data[off] | ((unsigned)c->data[off+1] << 8));
    } else {
        value = adpcm_sample(c, pos);
    }
    c->pos = pos + 1;
    if (c->format == DS_SOUND_ADPCM && c->pos == c->loop_start && !c->loop_initialized) {
        c->loop_predictor = c->predictor;
        c->loop_index = c->index;
        c->loop_initialized = 1;
    }
    return value;
}
static int channel_sample(DsSoundChip *chip, DsSoundChannel *c) {
    uint64_t threshold;
    if (!c->active || !c->step) return 0;
    threshold = (uint64_t)c->step * chip->output_rate;
    c->phase += DS_SOUND_CLOCK;
    while (c->phase >= threshold && c->active) {
        c->phase -= threshold;
        c->sample = (int16_t)advance_sample(c);
    }
    return c->sample;
}
void ds_sound_mix(DsSoundChip *chip, int16_t *out, size_t frames) {
    size_t i;
    int ch;
    if (!chip || !out || !chip->output_rate) return;
    for (i = 0; i < frames; i++) {
        int32_t left = 0, right = 0;
        for (ch = 0; ch < DS_SOUND_CHANNELS; ch++) {
            DsSoundChannel *c = &chip->channels[ch];
            int32_t value = channel_sample(chip, c);
            value = (value * c->volume) / 127;
            value >>= c->shift;
            left += (value * (127 - c->pan)) / 127;
            right += (value * c->pan) / 127;
        }
        left = (left * chip->master_volume) / 127;
        right = (right * chip->master_volume) / 127;
        out[2*i] = (int16_t)clamp16(left);
        out[2*i+1] = (int16_t)clamp16(right);
        for (ch = 0; ch < DS_SOUND_CAPTURES; ch++) {
            DsSoundCapture *cap = &chip->captures[ch];
            if (!cap->enabled) continue;
            cap->buffer[cap->position++] = cap->source ? out[2*i+1] : out[2*i];
            if (cap->position == cap->capacity) {
                if (cap->repeat) cap->position = 0;
                else cap->enabled = 0;
            }
        }
    }
}
