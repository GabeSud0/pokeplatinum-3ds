/*
 * Synthetic-only portable mixer tests. No ROM, SDAT, sound bank or game data.
 * Use the sound-chip interface directly; the real ARM7 integration is a
 * separate, unfinished milestone.
 */
#include "../src/sound_chip.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_pcm(void)
{
    DsSoundChip chip;
    int16_t pcm[16];
    const uint8_t pcm8[] = { 0, 64, 128, 192 };
    const uint8_t pcm16[] = { 0x34,0x12, 0xcc,0xed };
    ds_sound_init(&chip, 32728);
    ds_sound_set_pcm(&chip, 0, DS_SOUND_PCM8, pcm8, sizeof(pcm8), 0, 4, 0);
    ds_sound_control(&chip, 0, 65024, 127, 0, 0, 1);
    ds_sound_mix(&chip, pcm, 4);
    assert(pcm[0] == 0 && pcm[2] == 16384);
    assert(pcm[4] == -32768 && pcm[6] == -16384);
    ds_sound_init(&chip, 32728);
    ds_sound_set_pcm(&chip, 1, DS_SOUND_PCM16, pcm16, sizeof(pcm16), 0, 2, 1);
    ds_sound_control(&chip, 1, 65024, 127, 127, 0, 1);
    ds_sound_mix(&chip, pcm, 4);
    assert(pcm[1] == 4660 && pcm[3] == -4660);
    assert(pcm[5] == 4660 && pcm[7] == -4660);
}
static void test_adpcm(void)
{
    DsSoundChip chip;
    int16_t pcm[8];
    /* Initial predictor = 0, step index 0, four +1 IMA nibbles. */
    const uint8_t block[] = { 0,0,0,0, 0x11,0x11 };
    ds_sound_init(&chip, 32728);
    ds_sound_set_pcm(&chip, 0, DS_SOUND_ADPCM, block, sizeof(block), 0, 5, 0);
    ds_sound_control(&chip, 0, 65024, 127, 0, 0, 1);
    ds_sound_mix(&chip, pcm, 4);
    assert(pcm[0] == 0 && pcm[2] == 1 && pcm[4] == 2 && pcm[6] == 3);
}
static void test_generators_and_capture(void)
{
    DsSoundChip chip;
    int16_t pcm[16], left[8];
    int i;
    ds_sound_init(&chip, 32728);
    ds_sound_set_generator(&chip, 8, DS_SOUND_PSG, 1);
    ds_sound_control(&chip, 8, 65024, 127, 0, 0, 1);
    ds_sound_capture(&chip, 0, left, 8, 0, 0);
    ds_sound_mix(&chip, pcm, 8);
    for (i = 0; i < 8; i++) assert(left[i] == pcm[2*i]);
    assert(pcm[0] < 0 && pcm[2] < 0 && pcm[4] > 0);
    ds_sound_set_generator(&chip, 14, DS_SOUND_NOISE, 0);
    ds_sound_control(&chip, 14, 65024, 127, 127, 0, 1);
    ds_sound_mix(&chip, pcm, 8);
    for (i = 0; i < 8; i++) assert(pcm[2*i+1] != 0);
}
static void test_pan_shift_loop_and_master_volume(void)
{
    DsSoundChip chip;
    int16_t pcm[16], right[4];
    const uint8_t wave[] = { 64,64,64,64 };
    int i;
    ds_sound_init(&chip, 32728);
    ds_sound_set_pcm(&chip, 0, DS_SOUND_PCM8, wave, sizeof(wave), 1, 3, 1);
    ds_sound_control(&chip, 0, 65024, 127, 0, 1, 1);
    ds_sound_mix(&chip, pcm, 8);
    assert(pcm[0] == 8192 && pcm[1] == 0);
    assert(chip.channels[0].active && chip.channels[0].pos == 2);
    ds_sound_control(&chip, 0, 65024, 127, 127, 1, 1);
    ds_sound_capture(&chip, 1, right, 4, 0, 1);
    ds_sound_mix(&chip, pcm, 4);
    for (i = 0; i < 4; ++i) {
        assert(pcm[2*i] == 0 && pcm[2*i+1] == 8192);
        assert(right[i] == 8192);
    }
    chip.master_volume = 0;
    ds_sound_mix(&chip, pcm, 4);
    assert(pcm[0] == 0 && pcm[1] == 0);
}
int main(void)
{
    test_pcm();
    test_adpcm();
    test_generators_and_capture();
    test_pan_shift_loop_and_master_volume();
    puts("portable DS sound-chip synthetic tests passed");
    return 0;
}
