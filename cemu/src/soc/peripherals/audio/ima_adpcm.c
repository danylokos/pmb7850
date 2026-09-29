#include "ima_adpcm.h"

#include <limits.h>
#include <string.h>

#define WAVE_FORMAT_IMA_ADPCM 0x0011u

static uint16_t le16(const uint8_t *p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int cemu_wav_parse_ima(const uint8_t *data, size_t size,
                       cemu_wav_format_t *format) {
    if (!data || !format || size < 12u || memcmp(data, "RIFF", 4) ||
        memcmp(data + 8, "WAVE", 4)) return -1;
    uint32_t riff_size = le32(data + 4);
    if (riff_size < 4u || (uint64_t)riff_size + 8u > size) return -1;

    cemu_wav_format_t parsed = {0};
    int have_fmt = 0, have_fact = 0, have_data = 0;
    size_t end = (size_t)riff_size + 8u;
    for (size_t offset = 12u; offset + 8u <= end;) {
        const uint8_t *chunk = data + offset;
        uint32_t chunk_size = le32(chunk + 4);
        size_t payload = offset + 8u;
        if ((uint64_t)payload + chunk_size > end) return -1;
        if (!memcmp(chunk, "fmt ", 4)) {
            if (have_fmt || chunk_size < 20u) return -1;
            parsed.format_tag = le16(data + payload);
            parsed.channels = le16(data + payload + 2u);
            parsed.sample_rate = le32(data + payload + 4u);
            parsed.average_bytes_per_second = le32(data + payload + 8u);
            parsed.block_align = le16(data + payload + 12u);
            parsed.bits_per_sample = le16(data + payload + 14u);
            uint16_t extension_size = le16(data + payload + 16u);
            if (extension_size < 2u || (uint32_t)extension_size + 18u > chunk_size)
                return -1;
            parsed.samples_per_block = le16(data + payload + 18u);
            have_fmt = 1;
        } else if (!memcmp(chunk, "fact", 4)) {
            if (have_fact || chunk_size < 4u) return -1;
            parsed.fact_sample_frames = le32(data + payload);
            have_fact = 1;
        } else if (!memcmp(chunk, "data", 4)) {
            if (have_data) return -1;
            parsed.data_offset = payload;
            parsed.data_size = chunk_size;
            have_data = 1;
        }
        size_t advance = 8u + (size_t)chunk_size + (chunk_size & 1u);
        if (advance < 8u || advance > end - offset) return -1;
        offset += advance;
    }

    if (!have_fmt || !have_fact || !have_data ||
        parsed.format_tag != WAVE_FORMAT_IMA_ADPCM ||
        (parsed.channels != 1u && parsed.channels != 2u) ||
        !parsed.sample_rate || !parsed.average_bytes_per_second ||
        parsed.bits_per_sample != 4u ||
        parsed.block_align < parsed.channels * 4u ||
        parsed.block_align > CEMU_IMA_ADPCM_MAX_BLOCK ||
        !parsed.fact_sample_frames || !parsed.data_size) return -1;

    uint32_t encoded = parsed.block_align - parsed.channels * 4u;
    if (encoded % (parsed.channels * 4u)) return -1;
    uint32_t expected_frames = 1u + encoded * 2u / parsed.channels;
    uint32_t full_blocks = parsed.data_size / parsed.block_align;
    uint32_t partial_size = parsed.data_size % parsed.block_align;
    uint32_t header_size = parsed.channels * 4u;
    if (partial_size &&
        (partial_size < header_size ||
         (partial_size - header_size) % (parsed.channels * 4u)))
        return -1;
    uint64_t complete_frames = (uint64_t)full_blocks * expected_frames;
    uint64_t maximum_frames = complete_frames;
    uint64_t minimum_frames;
    if (partial_size) {
        uint32_t partial_frames = 1u +
            (partial_size - header_size) * 2u / parsed.channels;
        maximum_frames += partial_frames;
        minimum_frames = complete_frames + 1u;
    } else {
        if (!full_blocks) return -1;
        minimum_frames = (uint64_t)(full_blocks - 1u) * expected_frames + 1u;
    }
    if (parsed.samples_per_block != expected_frames ||
        parsed.fact_sample_frames < minimum_frames ||
        parsed.fact_sample_frames > maximum_frames ||
        (uint64_t)expected_frames * parsed.channels >
            CEMU_IMA_ADPCM_MAX_SAMPLES) return -1;
    *format = parsed;
    return 0;
}

static const int IMA_INDEX_TABLE[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8,
    -1, -1, -1, -1, 2, 4, 6, 8,
};

static const int IMA_STEP_TABLE[89] = {
       7,     8,     9,    10,    11,    12,    13,    14,
      16,    17,    19,    21,    23,    25,    28,    31,
      34,    37,    41,    45,    50,    55,    60,    66,
      73,    80,    88,    97,   107,   118,   130,   143,
     157,   173,   190,   209,   230,   253,   279,   307,
     337,   371,   408,   449,   494,   544,   598,   658,
     724,   796,   876,   963,  1060,  1166,  1282,  1411,
    1552,  1707,  1878,  2066,  2272,  2499,  2749,  3024,
    3327,  3660,  4026,  4428,  4871,  5358,  5894,  6484,
    7132,  7845,  8630,  9493, 10442, 11487, 12635, 13899,
   15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
   32767,
};

typedef struct {
    int predictor;
    int index;
} ima_channel_t;

static int16_t ima_nibble(ima_channel_t *channel, uint8_t nibble) {
    int step = IMA_STEP_TABLE[channel->index];
    int difference = step >> 3;
    if (nibble & 1u) difference += step >> 2;
    if (nibble & 2u) difference += step >> 1;
    if (nibble & 4u) difference += step;
    channel->predictor += (nibble & 8u) ? -difference : difference;
    if (channel->predictor < INT16_MIN) channel->predictor = INT16_MIN;
    if (channel->predictor > INT16_MAX) channel->predictor = INT16_MAX;
    channel->index += IMA_INDEX_TABLE[nibble & 0x0fu];
    if (channel->index < 0) channel->index = 0;
    if (channel->index > 88) channel->index = 88;
    return (int16_t)channel->predictor;
}

int cemu_ima_adpcm_decode_block(const cemu_wav_format_t *format,
                                const uint8_t *block, size_t block_size,
                                int16_t *output, size_t output_capacity,
                                size_t *sample_count) {
    if (sample_count) *sample_count = 0;
    if (!format || !block || !output || !sample_count ||
        (format->channels != 1u && format->channels != 2u) ||
        block_size > format->block_align ||
        block_size < format->channels * 4u ||
        (block_size - format->channels * 4u) %
            (format->channels * 4u)) return -1;
    size_t frames = 1u +
        (block_size - format->channels * 4u) * 2u / format->channels;
    if (block_size == format->block_align &&
        frames != format->samples_per_block) return -1;
    size_t required = frames * format->channels;
    if (required > output_capacity) return -1;

    ima_channel_t channels[2] = {{0}};
    for (uint16_t channel = 0; channel < format->channels; channel++) {
        size_t header = (size_t)channel * 4u;
        channels[channel].predictor = (int16_t)le16(block + header);
        channels[channel].index = block[header + 2u];
        if (channels[channel].index > 88 || block[header + 3u] != 0) return -1;
        output[channel] = (int16_t)channels[channel].predictor;
    }

    size_t frame = 1u;
    size_t offset = (size_t)format->channels * 4u;
    while (offset < block_size) {
        int16_t decoded[2][8];
        for (uint16_t channel = 0; channel < format->channels; channel++) {
            for (unsigned byte_index = 0; byte_index < 4u; byte_index++) {
                uint8_t byte = block[offset++];
                decoded[channel][byte_index * 2u] =
                    ima_nibble(&channels[channel], byte & 0x0fu);
                decoded[channel][byte_index * 2u + 1u] =
                    ima_nibble(&channels[channel], byte >> 4);
            }
        }
        for (unsigned i = 0; i < 8u; i++, frame++)
            for (uint16_t channel = 0; channel < format->channels; channel++)
                output[frame * format->channels + channel] = decoded[channel][i];
    }
    if (frame != frames) return -1;
    *sample_count = required;
    return 0;
}

void cemu_ima_adpcm_stream_reset(cemu_ima_adpcm_stream_t *state) {
    if (state) memset(state, 0, sizeof *state);
}

int cemu_ima_adpcm_packet(cemu_ima_adpcm_stream_t *state,
                          const uint8_t *packet, size_t size,
                          int16_t *output, size_t capacity) {
    if (!state || !packet || !output || size < 6u || (size & 1u) ||
        le16(packet) != 0xadbc) return -1;
    size_t size_data = size - 4u;
    uint16_t marker = le16(packet + 2u);
    size_t header = marker == 0xffffu ? size_data : (size_t)marker * 2u;
    if ((marker != 0xffffu &&
         (size_data < 4u || header > size_data - 4u || packet[4u + header + 2u] > 88u ||
          packet[4u + header + 3u])) ||
        (!state->have_header && (marker == 0xffffu || header != 0u)))
        return -1;
    size_t required = size_data * 2u - (marker == 0xffffu ? 0u : 7u);
    if (required > capacity) return -1;
    ima_channel_t channel = { state->predictor, state->index };
    size_t used = 0;
    for (size_t pos = 0; pos < size_data;) {
        if (marker != 0xffffu && pos == header) {
            channel.predictor = (int16_t)le16(packet + 4u + pos);
            channel.index = packet[4u + pos + 2u];
            output[used++] = (int16_t)channel.predictor;
            pos += 4u;
        } else {
            uint8_t byte = packet[4u + pos++];
            output[used++] = ima_nibble(&channel, byte & 15u);
            output[used++] = ima_nibble(&channel, byte >> 4);
        }
    }
    state->predictor = channel.predictor;
    state->index = channel.index;
    state->have_header = 1;
    state->compressed_received += size_data;
    state->sample_frames_emitted += used;
    return (int)used;
}
