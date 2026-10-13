/* All fixtures are invented waveforms; no SDAT, ROM or extracted data. */
#include <nitro_port_sound_chip.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* tests/sound_chip_reference.c: the mixer before it was made faster. */
extern volatile uint8_t RefSoundRegisters[NITRO_SOUND_REG_SIZE];
void RefSound_Reset(NitroSoundChip *chip, uint32_t outputRate);
void RefSound_Mix(NitroSoundChip *chip, int16_t *stereo, size_t frames);

static int checks, failures;
#define CHECK(c) do { ++checks; if (!(c)) { ++failures; fprintf(stderr,     "%s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static void reg8(unsigned at, unsigned data)
{
    NitroPort_SoundRegisters[at] = (uint8_t)data;
}
static void reg16(unsigned at, uint16_t data)
{
    reg8(at, data);
    reg8(at + 1u, data >> 8);
}
static void reg32(unsigned at, uint32_t data)
{
    reg16(at, (uint16_t)data);
    reg16(at + 2u, (uint16_t)(data >> 16));
}

static int16_t frame(int16_t *output, unsigned at, unsigned side)
{
    return output[at * 2u + side];
}

static void setup_pcm(unsigned format, const void *bytes, unsigned loopStart,
                      unsigned loopLength, unsigned mode, unsigned shift,
                      unsigned vol, unsigned pan)
{
    reg32(0x400, (1u << 31) | (format << 29) | (mode << 27) |
          (pan << 16) | (shift << 8) | vol);
    reg32(0x404, (uint32_t)(uintptr_t)bytes);
    reg16(0x408, (uint16_t)(65536u - 512u));
    reg16(0x40a, (uint16_t)loopStart);
    reg32(0x40c, loopLength);
}

static void test_pcm16(void)
{
    static int16_t source[12] = {10000, -10000, 20000, -20000};
    int16_t out[64];
    NitroSoundChip chip;
    NitroSound_Reset(&chip, NITRO_SOUND_RATE);
    setup_pcm(1, source, 0, 1, 2, 0, 127, 0);
    memset(out, 0x55, sizeof(out));
    NitroSound_Mix(&chip, out, 16);
    CHECK(frame(out, 0, 0) == 10000);
    CHECK(frame(out, 0, 1) == 0);
    CHECK(frame(out, 2, 0) != frame(out, 0, 0));
    CHECK(frame(out, 15, 0) == 0);
    CHECK(!(NitroPort_SoundRegisters[0x403] & 0x80));
}

static void test_pcm8_loop(void)
{
    static int8_t source[12] = {25, -25, 50, -50, 90, 90};
    int16_t out[64];
    NitroSoundChip chip;
    NitroSound_Reset(&chip, NITRO_SOUND_RATE);
    setup_pcm(0, source, 0, 1, 1, 0, 127, 127);
    NitroSound_Mix(&chip, out, 16);
    CHECK(frame(out, 0, 1) == 25 * 256);
    CHECK(frame(out, 0, 0) == 0);
    CHECK(frame(out, 2, 1) != frame(out, 0, 1));
    CHECK(NitroPort_SoundRegisters[0x403] & 0x80);
}

static void test_shift_and_master(void)
{
    static int16_t source[16] = {12000, 12000, 12000, 12000};
    int16_t out[32];
    NitroSoundChip chip;
    NitroSound_Reset(&chip, NITRO_SOUND_RATE);
    setup_pcm(1, source, 0, 2, 1, 3, 127, 0);
    reg8(0x500, 64);
    NitroSound_Mix(&chip, out, 2);
    CHECK(frame(out, 0, 0) == (12000 >> 4) * 64 / 127);
    CHECK(frame(out, 0, 1) == 0);
}

static void test_adpcm(void)
{
    /* Header: predictor=1000, stepIndex=0. Nibbles 0x7,0x7 produce
     * positive deltas, 0x8 produces a negative step. */
    static uint8_t source[16] = {0xe8, 0x03, 0, 0, 0x77, 0x88, 0, 0};
    int16_t out[96];
    NitroSoundChip chip;
    NitroSound_Reset(&chip, NITRO_SOUND_RATE);
    setup_pcm(2, source, 0, 2, 2, 0, 127, 0);
    NitroSound_Mix(&chip, out, 20);
    CHECK(frame(out, 0, 0) == 1000);
    CHECK(frame(out, 2, 0) > frame(out, 0, 0));
    CHECK(frame(out, 5, 0) < frame(out, 3, 0));
    CHECK(frame(out, 0, 1) == 0);
}

static void test_psg(void)
{
    int16_t out[512];
    NitroSoundChip chip;
    unsigned i, positive = 0, negative = 0;
    NitroSound_Reset(&chip, NITRO_SOUND_RATE);
    reg32(0x480, 0x80000000u | (3u << 29) | (3u << 24) |
          (0u << 16) | 127u);
    reg16(0x488, 65536u - 512u);
    NitroSound_Mix(&chip, out, 100);
    for (i = 0; i < 100; ++i) {
        positive += frame(out, i, 0) > 0;
        negative += frame(out, i, 0) < 0;
    }
    CHECK(positive > 10);
    CHECK(negative > 10);
    CHECK(frame(out, 0, 1) == 0);
}

static void test_noise(void)
{
    int16_t out[1024];
    NitroSoundChip chip;
    unsigned i, positive = 0, negative = 0;
    NitroSound_Reset(&chip, NITRO_SOUND_RATE);
    reg32(0x4e0, 0x80000000u | (3u << 29) | (0u << 16) | 127u);
    reg16(0x4e8, 65536u - 512u);
    NitroSound_Mix(&chip, out, 200);
    for (i = 0; i < 200; ++i) {
        positive += frame(out, i, 0) > 0;
        negative += frame(out, i, 0) < 0;
    }
    CHECK(positive > 0);
    CHECK(negative > 0);
}

/* Channel ch: PCM16 `bytes`, looping, at volume 127 and the given pan and
 * timer (sample rate 16756991 / timer). */
static void setup_pcm16_on(unsigned ch, const void *bytes, unsigned words,
                           unsigned pan, unsigned timer)
{
    unsigned base = 0x400u + ch * 16u;

    reg32(base, (1u << 31) | (1u << 29) | (1u << 27) | (pan << 16) | 127u);
    reg32(base + 4u, (uint32_t)(uintptr_t)bytes);
    reg16(base + 8u, (uint16_t)(65536u - timer));
    reg16(base + 10u, 0);
    reg32(base + 12u, words);
}

/* Capture as NitroSystem's reverb uses it: the left mixer into a looping
 * buffer, at the rate of channel 1's timer (16000 a second here), on and on
 * (bit 2 clear: SND_CAPTURE_REPEAT_YES). */
static void test_capture(void)
{
    static int16_t source[64], capture[2048];
    int16_t out[2 * 512];
    NitroSoundChip chip;
    unsigned i, n;

    for (i = 0; i < 64; ++i) source[i] = 7000;
    memset(capture, 0, sizeof(capture));
    NitroSound_Reset(&chip, NITRO_SOUND_RATE);
    setup_pcm16_on(0, source, 16, 0, 512);            /* left */
    reg16(0x418, (uint16_t)(65536u - 1047u));          /* channel 1's timer: 16 kHz */
    reg32(0x510, (uint32_t)(uintptr_t)capture);
    reg16(0x514, sizeof(capture) / 4);
    reg8(0x508, 0x80u);                                /* on, PCM16, looping, mixer */
    for (n = 0; n < 8; ++n) NitroSound_Mix(&chip, out, 512);   /* 4096 samples */
    /* 4096 * 16000 / 32768 = 2000 recorded */
    CHECK(chip.capture[0].index >= 2 * 1998 && chip.capture[0].index <= 2 * 2002);
    CHECK(capture[0] == 7000 && capture[1000] == 7000);
    CHECK(capture[2047] == 0);                         /* not there yet */
    for (n = 0; n < 8; ++n) NitroSound_Mix(&chip, out, 512);   /* past the end */
    CHECK(NitroPort_SoundRegisters[0x508] & 0x80);     /* still recording */
    CHECK(chip.capture[0].running);
    CHECK(capture[2047] == 7000);
    CHECK(chip.capture[0].index >= 2 * (4000 - 2048 - 2) &&
          chip.capture[0].index <= 2 * (4000 - 2048 + 2));   /* went round */
    reg8(0x508, 0);
    NitroSound_Mix(&chip, out, 1);
    CHECK(chip.capture[0].running == 0);

    /* one-shot (bit 2): stops at the end of its buffer */
    memset(capture, 0, sizeof(capture));
    NitroSound_Reset(&chip, NITRO_SOUND_RATE);
    setup_pcm16_on(0, source, 16, 0, 512);
    reg16(0x418, (uint16_t)(65536u - 512u));           /* ~32 kHz */
    reg32(0x510, (uint32_t)(uintptr_t)capture);
    reg16(0x514, 4);                                   /* 8 samples */
    reg8(0x508, 0x80u | 0x04u);
    NitroSound_Mix(&chip, out, 64);
    CHECK(!(NitroPort_SoundRegisters[0x508] & 0x80));
    CHECK(chip.capture[0].running == 0);
    CHECK(capture[7] == 7000 && capture[8] == 0);
}

/* SOUNDCNT: channel 1 kept out of the mixer (bit 12) and played on the left
 * output by itself (bits 8-9 = 1), the right output from the mixer: as
 * NitroSystem's capture "effect" sets it. */
static void test_output_selector(void)
{
    static int16_t three[64], five[64];
    int16_t out[2 * 4];
    NitroSoundChip chip;
    unsigned i;

    for (i = 0; i < 64; ++i) {
        three[i] = 3000;
        five[i] = 5000;
    }
    NitroSound_Reset(&chip, NITRO_SOUND_RATE);
    setup_pcm16_on(0, three, 16, 127, 512);            /* right */
    setup_pcm16_on(1, five, 16, 64, 512);              /* middle */
    reg8(0x501, 0x80u | 0x10u | 0x01u);
    NitroSound_Mix(&chip, out, 2);
    CHECK(frame(out, 1, 0) == 5000 * 63 / 127);        /* channel 1's left half */
    CHECK(frame(out, 1, 1) == 3000);                   /* the mixer: no channel 1 */
    reg8(0x501, 0x80u);                                /* back to normal */
    NitroSound_Mix(&chip, out, 2);
    CHECK(frame(out, 1, 0) == 5000 * 63 / 127);
    CHECK(frame(out, 1, 1) == 3000 + 5000 * 64 / 127);
}

/* A new note on a channel that's still playing: the driver stops it, sets it
 * up and starts it between two mixes. The DS starts it over (the enable bit
 * went 0 -> 1): the new sound from its first sample. */
static void test_restart_between_mixes(void)
{
    static int16_t first[64], second[64];
    int16_t out[2 * 8];
    NitroSoundChip chip;
    unsigned i;

    for (i = 0; i < 64; ++i) {
        first[i] = 1000;
        second[i] = (int16_t)(i == 0 ? 20000 : 3000);
    }
    NitroSound_Reset(&chip, NITRO_SOUND_RATE);
    setup_pcm(1, first, 0, 32, 1, 0, 127, 0);           /* looping */
    NitroSound_Mix(&chip, out, 8);
    CHECK(frame(out, 7, 0) == 1000);
    /* stop, set up the new sound (enable bit 0), then start it */
    reg8(0x403, NitroPort_SoundRegisters[0x403] & 0x7F);
    setup_pcm(1, second, 0, 32, 1, 0, 127, 0);
    reg8(0x403, NitroPort_SoundRegisters[0x403] & 0x7F);
    NitroSound_KeyOn(&chip, 0);
    reg8(0x403, NitroPort_SoundRegisters[0x403] | 0x80);
    NitroSound_Mix(&chip, out, 2);
    CHECK(frame(out, 0, 0) == 20000);                    /* from its start */
    CHECK(frame(out, 1, 0) == 3000 || frame(out, 1, 0) == 20000);
    /* starting a channel that's already on doesn't restart it */
    NitroSound_KeyOn(&chip, 0);
    NitroSound_Mix(&chip, out, 1);
    CHECK(frame(out, 0, 0) == 3000);
}

/* ------------------------------------------------------------------------
 * The faster mixer against the old one: random channels (all formats,
 * pitches, loops, volumes), random register changes between mixes of random
 * lengths, as the ARM7 driver makes them. Samples, registers and channel
 * states must all be the same. (Captures aren't compared: the old mixer
 * had them wrong - looping and one-shot swapped, recording at the output
 * rate - and test_capture checks them instead.)
 */
#define ARENA_SIZE 0x10000u
#define CAPTURE_AT 0xC000u   /* captures write here; ADPCM never reads it */
/* (with the same bytes past the end: a channel whose format changes while
 * it plays may read on past its data) */
#define ARENA_PAD 0x8000u
static uint8_t sArenaNew[ARENA_SIZE + ARENA_PAD] __attribute__((aligned(4)));
static uint8_t sArenaRef[ARENA_SIZE + ARENA_PAD] __attribute__((aligned(4)));
static uint32_t sSeed = 12345;

static uint32_t rnd(void)
{
    sSeed = sSeed * 1103515245u + 12345u;
    return sSeed >> 8;
}

static void both8(unsigned at, uint8_t value)
{
    NitroPort_SoundRegisters[at] = value;
    RefSoundRegisters[at] = value;
}
static void both16(unsigned at, uint16_t value)
{
    both8(at, (uint8_t)value);
    both8(at + 1, (uint8_t)(value >> 8));
}
static void both32(unsigned at, uint32_t value)
{
    both16(at, (uint16_t)value);
    both16(at + 2, (uint16_t)(value >> 16));
}
/* An address in the sound data: each mixer gets its own copy's. */
static void bothAddr(unsigned at, uint32_t offset)
{
    uint32_t a = (uint32_t)(uintptr_t)(sArenaNew + offset);
    uint32_t b = (uint32_t)(uintptr_t)(sArenaRef + offset);
    unsigned i;

    for (i = 0; i < 4; i++) {
        NitroPort_SoundRegisters[at + i] = (uint8_t)(a >> (i * 8));
        RefSoundRegisters[at + i] = (uint8_t)(b >> (i * 8));
    }
}

static void random_channel(unsigned ch)
{
    unsigned base = 0x400u + ch * 16u;
    uint32_t format = rnd() % 4u;
    uint32_t repeat = rnd() % 4u;
    uint32_t ctrl = (1u << 31) | (format << 29) | (repeat << 27) |
                    ((rnd() % 128u) << 16) | ((rnd() % 4u) << 8) |
                    (rnd() % 128u) | ((rnd() % 8u) << 24);
    uint32_t loopWords = rnd() % 4u == 0 ? 0 : rnd() % 64u;
    uint32_t lengthWords = rnd() % 8u == 0 ? 0 : 1 + rnd() % 300u;
    uint32_t limit = format == 2u ? CAPTURE_AT : ARENA_SIZE;
    uint32_t offset = (rnd() % (limit - 4u * (loopWords + lengthWords) - 4u)) & ~3u;
    uint16_t timer;

    switch (rnd() % 6u) {
    case 0: timer = (uint16_t)rnd(); break;                    /* anything */
    case 1: timer = (uint16_t)(0xFFFFu - rnd() % 16u); break;  /* very low */
    case 2: timer = (uint16_t)(rnd() % 64u); break;            /* very high */
    default: timer = (uint16_t)(0xFC00u + rnd() % 0x300u); break;
    }
    if (rnd() % 10u == 0) ctrl &= 0x7FFFFFFFu;                 /* off */
    if (rnd() % 20u == 0) {
        both32(base + 4u, 0);                                  /* no data */
    } else {
        bothAddr(base + 4u, offset);
    }
    both16(base + 8u, timer);
    both16(base + 10u, (uint16_t)loopWords);
    both32(base + 12u, lengthWords);
    both32(base, ctrl);
}

static void random_capture(unsigned slot)
{
    uint32_t size = 1 + rnd() % 256u;   /* in words */

    bothAddr(0x510u + slot * 8u, CAPTURE_AT + slot * 0x1000u + (rnd() % 64u) * 4u);
    both16(0x514u + slot * 8u, (uint16_t)size);
    both8(0x508u + slot, (uint8_t)(rnd() % 3u == 0 ? 0 : (0x80u | (rnd() % 16u))));
}

static int same_voice(const NitroSoundVoice *a, const NitroSoundVoice *b)
{
    return a->previousCtrl == b->previousCtrl && a->sampleIndex == b->sampleIndex &&
           a->phase == b->phase && a->predictor == b->predictor &&
           a->stepIndex == b->stepIndex && a->noise == b->noise &&
           a->running == b->running;
}

static void test_matches_reference(void)
{
    static NitroSoundChip chipNew, chipRef;
    static int16_t outNew[2 * 600], outRef[2 * 600];
    unsigned scenario, step, ch, i;
    int mismatches = 0;

    CHECK(((uintptr_t)sArenaNew >> 31) == 0 && ((uintptr_t)sArenaRef >> 31) == 0);
    for (i = 0; i < ARENA_SIZE + ARENA_PAD; i++) {
        sArenaNew[i] = sArenaRef[i] = (uint8_t)rnd();
    }
    for (scenario = 0; scenario < 300 && mismatches < 5; scenario++) {
        NitroSound_Reset(&chipNew, NITRO_SOUND_RATE);
        RefSound_Reset(&chipRef, NITRO_SOUND_RATE);
        both8(0x500u, (uint8_t)(rnd() % 128u));
        both8(0x501u, rnd() % 8u == 0 ? 0 : 0x80u);
        for (ch = 0; ch < 16; ch++) {
            if (rnd() % 3u) random_channel(ch);
        }

        for (step = 0; step < 40 && mismatches < 5; step++) {
            size_t frames = rnd() % 4u == 0 ? 1 : 1 + rnd() % 600u;

            /* What the driver does between mixes. */
            switch (rnd() % 8u) {
            case 0: random_channel(rnd() % 16u); break;
            case 1: both16(0x408u + (rnd() % 16u) * 16u, (uint16_t)(0xF000u + rnd() % 0x1000u)); break;
            case 2: ch = rnd() % 16u;
                    both8(0x400u + ch * 16u, (uint8_t)(rnd() % 128u));          /* volume */
                    break;
            case 3: ch = rnd() % 16u;                                          /* stop */
                    both8(0x403u + ch * 16u, (uint8_t)(NitroPort_SoundRegisters[0x403u + ch * 16u] & 0x7F));
                    break;
            default: break;
            }
            NitroSound_Mix(&chipNew, outNew, frames);
            RefSound_Mix(&chipRef, outRef, frames);

            if (memcmp(outNew, outRef, frames * 4u) != 0) {
                {
                    size_t f;
                    for (f = 0; f < frames * 2u && outNew[f] == outRef[f]; f++) {
                    }
                    fprintf(stderr, "scenario %u step %u: samples differ at %u of %u: %d/%d\n", scenario, step,
                            (unsigned)f, (unsigned)frames * 2u, outNew[f], outRef[f]);
                    for (ch = 0; ch < 16; ch++)
                        fprintf(stderr, "  ch%u ctrl %08x idx %u run %u pred %d/%d step %d/%d\n", ch,
                                (unsigned)chipNew.voice[ch].previousCtrl, chipNew.voice[ch].sampleIndex,
                                chipNew.voice[ch].running, chipNew.voice[ch].predictor, chipRef.voice[ch].predictor,
                                chipNew.voice[ch].stepIndex, chipRef.voice[ch].stepIndex);
                }
                mismatches++;
            }
            for (ch = 0; ch < 16; ch++) {
                if (!same_voice(&chipNew.voice[ch], &chipRef.voice[ch]) ||
                    memcmp((const void *)&NitroPort_SoundRegisters[0x400u + ch * 16u],
                           (const void *)&RefSoundRegisters[0x400u + ch * 16u], 4) != 0) {
                    fprintf(stderr, "scenario %u step %u: channel %u differs (idx %u/%u run %u/%u ctrl %08x/%08x)\n", scenario, step, ch,
                            chipNew.voice[ch].sampleIndex, chipRef.voice[ch].sampleIndex, chipNew.voice[ch].running, chipRef.voice[ch].running,
                            chipNew.voice[ch].previousCtrl, chipRef.voice[ch].previousCtrl);
                    mismatches++;
                    break;
                }
            }
            if (NitroPort_SoundRegisters[0x508] != RefSoundRegisters[0x508] ||
                NitroPort_SoundRegisters[0x509] != RefSoundRegisters[0x509] ||
                chipNew.capture[0].index != chipRef.capture[0].index ||
                chipNew.capture[1].index != chipRef.capture[1].index ||
                chipNew.capture[0].running != chipRef.capture[0].running ||
                chipNew.capture[1].running != chipRef.capture[1].running ||
                memcmp(sArenaNew, sArenaRef, ARENA_SIZE) != 0) {
                fprintf(stderr, "scenario %u step %u: captures differ\n", scenario, step);
                mismatches++;
            }
        }
    }
    CHECK(mismatches == 0);
}

/* The mixer works a block of frames at a time, channel by channel, and
 * frame by frame only for channels reading a capture's buffer. With
 * captures running (reverb), mixing many frames at once must give exactly
 * what mixing them one at a time does (where nothing can go out of order). */
static void test_blocks_match_single_frames(void)
{
    static NitroSoundChip chipA, chipB;
    static int16_t outA[2 * 600], outB[2 * 600];
    static uint8_t arenaB[ARENA_SIZE + ARENA_PAD];
    unsigned scenario, step, ch;
    int mismatches = 0;

    for (scenario = 0; scenario < 200 && mismatches < 5; scenario++) {
        unsigned i;
        for (i = 0; i < ARENA_SIZE + ARENA_PAD; i++) sArenaNew[i] = (uint8_t)rnd();
        NitroSound_Reset(&chipA, NITRO_SOUND_RATE);
        both8(0x500u, (uint8_t)(rnd() % 128u));
        both8(0x501u, (uint8_t)(0x80u | (rnd() % 64u)));
        for (ch = 0; ch < 16; ch++) {
            if (rnd() % 3u) random_channel(ch);
        }
        random_capture(0);
        random_capture(1);
        if (rnd() % 2u) {
            /* Reverb as NitroSystem sets it up: channels 1 and 3 play the
             * capture buffers back. */
            for (ch = 1; ch < 4; ch += 2) {
                unsigned base = 0x400u + ch * 16u;
                unsigned slot = ch == 1 ? 0u : 1u;
                both32(base + 4u, (uint32_t)(uintptr_t)(sArenaNew + CAPTURE_AT + slot * 0x1000u));
                both16(base + 10u, 0);
                both32(base + 12u, NitroPort_SoundRegisters[0x514u + slot * 8u] |
                                   ((uint32_t)NitroPort_SoundRegisters[0x515u + slot * 8u] << 8));
                both32(base, (1u << 31) | (1u << 29) | (1u << 27) | (64u << 16) | 90u);
            }
        }
        /* The same start for the frame-by-frame copy. */
        chipB = chipA;
        memcpy(arenaB, sArenaNew, sizeof(arenaB));
        {
            static uint8_t regs[NITRO_SOUND_REG_SIZE];
            memcpy(regs, (const void *)NitroPort_SoundRegisters, sizeof(regs));
            for (step = 0; step < 20 && mismatches < 5; step++) {
                size_t frames = 1 + rnd() % 600u, f;
                static uint8_t regsA[NITRO_SOUND_REG_SIZE];
                static uint8_t arenaA[ARENA_SIZE + ARENA_PAD];

                /* A: all at once. */
                NitroSound_Mix(&chipA, outA, frames);
                memcpy(regsA, (const void *)NitroPort_SoundRegisters, sizeof(regsA));
                memcpy(arenaA, sArenaNew, sizeof(arenaA));
                /* B: one frame at a time, from B's own state. */
                memcpy((void *)NitroPort_SoundRegisters, regs, sizeof(regs));
                memcpy(sArenaNew, arenaB, sizeof(arenaB));
                for (f = 0; f < frames; f++) NitroSound_Mix(&chipB, outB + 2 * f, 1);
                memcpy(regs, (const void *)NitroPort_SoundRegisters, sizeof(regs));
                memcpy(arenaB, sArenaNew, sizeof(arenaB));
                if (memcmp(outA, outB, frames * 4u) != 0 || memcmp(regsA, regs, sizeof(regs)) != 0 ||
                    memcmp(arenaA, arenaB, ARENA_SIZE) != 0 ||
                    chipA.capture[0].index != chipB.capture[0].index ||
                    chipA.capture[1].index != chipB.capture[1].index) {
                    fprintf(stderr, "blocks: scenario %u step %u differs\n", scenario, step);
                    mismatches++;
                }
                /* Back to A's state for its next step. */
                memcpy((void *)NitroPort_SoundRegisters, regsA, sizeof(regsA));
                memcpy(sArenaNew, arenaA, sizeof(arenaA));
            }
        }
    }
    CHECK(mismatches == 0);
}

/* test_sound_chip --bench: how long one second of 16 busy channels takes
 * to mix, with the old mixer and the new one. */
static double seconds_now(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

static void bench(void)
{
    static NitroSoundChip chip;
    static int16_t out[2 * 512];
    int which;

    for (which = 0; which < 2; which++) {
        double start;
        unsigned ch, n;

        if (which == 0) RefSound_Reset(&chip, NITRO_SOUND_RATE);
        else NitroSound_Reset(&chip, NITRO_SOUND_RATE);
        sSeed = 99;
        both8(0x500u, 127);
        both8(0x501u, 0x80u);
        for (ch = 0; ch < 16; ch++) {
            unsigned base = 0x400u + ch * 16u;

            /* ADPCM instruments: a short loop after a longer start. */
            bothAddr(base + 4u, ch * 0x800u);
            both16(base + 8u, (uint16_t)(0xFE00u + ch * 8u));
            both16(base + 10u, 200);
            both32(base + 12u, 50);
            both32(base, (1u << 31) | (2u << 29) | (1u << 27) | (64u << 16) | 100u);
        }
        start = seconds_now();
        for (n = 0; n < 64; n++) {
            if (which == 0) RefSound_Mix(&chip, out, 512);
            else {
                unsigned k;
                /* as port_snd.c now mixes: a tick's worth (~89) at a time */
                for (k = 0; k < 512; k += 128) NitroSound_Mix(&chip, out + 2 * k, 128);
            }
        }
        printf("%s mixer: %.1f ms per second of sound\n", which == 0 ? "old" : "new",
               (seconds_now() - start) * 1000.0);
    }
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "--bench") == 0) {
        bench();
        return 0;
    }
    test_pcm16();
    test_pcm8_loop();
    test_shift_and_master();
    test_adpcm();
    test_psg();
    test_noise();
    test_capture();
    test_output_selector();
    test_restart_between_mixes();
    test_matches_reference();
    test_blocks_match_single_frames();
    printf("software DS sound chip: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
