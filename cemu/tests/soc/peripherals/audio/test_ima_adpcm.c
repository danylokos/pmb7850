#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ima_adpcm.h"

static int failures;
#define CHECK(condition) do { \
    if (!(condition)) { \
        failures++; \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
    } \
} while (0)

static void put16(uint8_t *p, uint16_t value) {
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

static void put32(uint8_t *p, uint32_t value) {
    put16(p, (uint16_t)value);
    put16(p + 2, (uint16_t)(value >> 16));
}

static void make_mono_wav(uint8_t wav[68]) {
    memset(wav, 0, 68);
    memcpy(wav, "RIFF", 4); put32(wav + 4, 60); memcpy(wav + 8, "WAVE", 4);
    memcpy(wav + 12, "fmt ", 4); put32(wav + 16, 20);
    put16(wav + 20, 0x11); put16(wav + 22, 1); put32(wav + 24, 8000);
    put32(wav + 28, 7111); put16(wav + 32, 8); put16(wav + 34, 4);
    put16(wav + 36, 2); put16(wav + 38, 9);
    memcpy(wav + 40, "fact", 4); put32(wav + 44, 4); put32(wav + 48, 9);
    memcpy(wav + 52, "data", 4); put32(wav + 56, 8);
    wav[60] = 0; wav[61] = 0; wav[62] = 0; wav[63] = 0;
    wav[64] = 0x11; wav[65] = 0x11; wav[66] = 0x11; wav[67] = 0x11;
}

int main(void) {
    uint8_t wav[68];
    make_mono_wav(wav);
    cemu_wav_format_t format;
    CHECK(cemu_wav_parse_ima(wav, sizeof wav, &format) == 0);
    CHECK(format.format_tag == 0x11);
    CHECK(format.channels == 1);
    CHECK(format.sample_rate == 8000);
    CHECK(format.block_align == 8);
    CHECK(format.samples_per_block == 9);
    CHECK(format.fact_sample_frames == 9);
    CHECK(format.data_offset == 60);
    CHECK(format.data_size == 8);

    int16_t decoded[32] = {0};
    size_t count = 0;
    CHECK(cemu_ima_adpcm_decode_block(
              &format, wav + format.data_offset, format.block_align,
              decoded, 32, &count) == 0);
    CHECK(count == 9);
    for (size_t i = 0; i < count; i++) CHECK(decoded[i] == (int16_t)i);

    /* Microsoft IMA permits a final partial block. It still contains complete
     * four-byte-per-channel nibble groups and the fact count trims padding. */
    uint8_t partial_wav[72];
    memcpy(partial_wav, wav, sizeof wav);
    memset(partial_wav + sizeof wav, 0, 4);
    put32(partial_wav + 4, 64);
    put32(partial_wav + 48, 10);
    put32(partial_wav + 56, 12);
    CHECK(cemu_wav_parse_ima(
              partial_wav, sizeof partial_wav, &format) == 0);
    CHECK(cemu_ima_adpcm_decode_block(
              &format, partial_wav + 68, 4, decoded, 32, &count) == 0);
    CHECK(count == 1 && decoded[0] == 0);
    partial_wav[71] = 1;
    CHECK(cemu_ima_adpcm_decode_block(
              &format, partial_wav + 68, 4, decoded, 32, &count) != 0);
    partial_wav[71] = 0;

    /* A partial block must contain its headers plus whole interleave groups. */
    put32(partial_wav + 4, 62);
    put32(partial_wav + 56, 10);
    CHECK(cemu_wav_parse_ima(partial_wav, 70, &format) != 0);

    cemu_wav_format_t stereo = {
        .format_tag = 0x11, .channels = 2, .sample_rate = 8000,
        .block_align = 16, .bits_per_sample = 4, .samples_per_block = 9,
    };
    const uint8_t stereo_block[16] = {
        100, 0, 0, 0, 0x9c, 0xff, 0, 0,
        0x11, 0x11, 0x11, 0x11, 0x99, 0x99, 0x99, 0x99,
    };
    CHECK(cemu_ima_adpcm_decode_block(
              &stereo, stereo_block, sizeof stereo_block,
              decoded, 32, &count) == 0);
    CHECK(count == 18);
    for (size_t frame = 0; frame < 9; frame++) {
        CHECK(decoded[frame * 2] == (int16_t)(100 + frame));
        CHECK(decoded[frame * 2 + 1] == (int16_t)(-100 - (int)frame));
    }

    /* Reject truncation, inconsistent sample geometry, and invalid channel
     * headers deterministically. */
    CHECK(cemu_wav_parse_ima(wav, sizeof wav - 1u, &format) != 0);
    make_mono_wav(wav); put16(wav + 38, 10);
    CHECK(cemu_wav_parse_ima(wav, sizeof wav, &format) != 0);
    make_mono_wav(wav); wav[63] = 1;
    CHECK(cemu_wav_parse_ima(wav, sizeof wav, &format) == 0);
    CHECK(cemu_ima_adpcm_decode_block(
              &format, wav + format.data_offset, format.block_align,
              decoded, 32, &count) != 0);

    puts(failures ? "WAV IMA ADPCM: FAIL" : "WAV IMA ADPCM: PASS");
    return failures ? 1 : 0;
}
