/* Microsoft IMA ADPCM decoding for firmware-produced audio streams. */
#ifndef CEMU_PERIPH_AUDIO_IMA_ADPCM_H
#define CEMU_PERIPH_AUDIO_IMA_ADPCM_H

#include <stddef.h>
#include <stdint.h>

#define CEMU_IMA_ADPCM_MAX_BLOCK 4096u
#define CEMU_IMA_ADPCM_MAX_SAMPLES 16384u

typedef struct {
    uint16_t format_tag;
    uint16_t channels;
    uint32_t sample_rate;
    uint32_t average_bytes_per_second;
    uint16_t block_align;
    uint16_t bits_per_sample;
    uint16_t samples_per_block;
    uint32_t fact_sample_frames;
    size_t data_offset;
    uint32_t data_size;
} cemu_wav_format_t;

/* Incremental mono endpoint state; contains no source-file metadata. */
typedef struct {
    int active;
    int failed;
    uint32_t sample_rate;
    int predictor;
    int index;
    int have_header;
    uint64_t compressed_received;
    uint64_t sample_frames_emitted;
} cemu_ima_adpcm_stream_t;

/* ADBC envelope, word offset of a four-byte IMA header (FFFF: none).
 * Returns samples produced, or -1 without changing decoder state. */
int cemu_ima_adpcm_packet(cemu_ima_adpcm_stream_t *state,
                          const uint8_t *packet, size_t size,
                          int16_t *output, size_t capacity);

/* Parse a complete RIFF/WAVE image. Only Microsoft IMA ADPCM (format 0x11)
 * is accepted because it is the only sampled X55 stream qualified so far. */
int cemu_wav_parse_ima(const uint8_t *data, size_t size,
                       cemu_wav_format_t *format);

/* Decode one complete Microsoft IMA ADPCM block. `sample_count` is the number
 * of interleaved signed-16 samples, not frames. */
int cemu_ima_adpcm_decode_block(const cemu_wav_format_t *format,
                                const uint8_t *block, size_t block_size,
                                int16_t *output, size_t output_capacity,
                                size_t *sample_count);

void cemu_ima_adpcm_stream_reset(cemu_ima_adpcm_stream_t *state);

#endif /* CEMU_PERIPH_AUDIO_IMA_ADPCM_H */
