/* All fixtures are invented waveforms; no SDAT, ROM or extracted data. */
#include <nitro_port_sound_chip.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

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

static void test_capture(void)
{
    static int16_t source[64], capture[8];
    int16_t out[64];
    NitroSoundChip chip;
    unsigned i;
    for (i = 0; i < 64; ++i) source[i] = 7000;
    memset(capture, 0, sizeof(capture));
    NitroSound_Reset(&chip, NITRO_SOUND_RATE);
    setup_pcm(1, source, 0, 16, 1, 0, 127, 0);
    reg32(0x510, (uint32_t)(uintptr_t)capture);
    reg16(0x514, sizeof(capture) / 4);
    reg8(0x508, 0x80u | 0x04u); /* enabled, PCM16, repeating */
    NitroSound_Mix(&chip, out, 25);
    CHECK(capture[0] != 0);
    CHECK(capture[2] != 0);
    CHECK(capture[0] == frame(out, 16, 0));
    reg8(0x508, 0);
    NitroSound_Mix(&chip, out, 1);
    CHECK(chip.capture[0].running == 0);
}

int main(void)
{
    test_pcm16();
    test_pcm8_loop();
    test_shift_and_master();
    test_adpcm();
    test_psg();
    test_noise();
    test_capture();
    printf("software DS sound chip: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
