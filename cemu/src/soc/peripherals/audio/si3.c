#include "si3.h"

#include <limits.h>
#include <string.h>

/* Terminal-speaker calibration against the indexed C55 reference recording.
 * This is not an identification of the D0961 analog response. */
const cemu_si3_profile_t cemu_si3_production_profile = {
    .frequency_millihz_per_unit = 190u,
    .waveform = CEMU_SI3_WAVE_TRIANGLE,
    .gain_per_level = 57u,
    .release_samples = 1024u,
};

static uint16_t le16(const uint8_t *p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static unsigned voice_index(uint16_t operation) {
    return (operation & 0xffu) >> 4;
}

static uint32_t pitch_step(uint16_t frequency_number, uint8_t octave, int recovered) {
    /* Reduce whole cycles before the phase shift: octave 15 must not overflow. */
    uint64_t numerator = (uint64_t)frequency_number * (1u << octave) *
                         cemu_si3_production_profile.frequency_millihz_per_unit;
    uint64_t denominator = (uint64_t)CEMU_SI3_SAMPLE_RATE * 1000u *
                           (recovered ? 4u : 1u);
    return (uint32_t)(((numerator % denominator) << 32) / denominator);
}

static int16_t clamp16(int32_t value) {
    if (value < INT16_MIN) return INT16_MIN;
    if (value > INT16_MAX) return INT16_MAX;
    return (int16_t)value;
}

static int render_quantum(cemu_si3_state_t *state, size_t *used) {
    state->sample_remainder +=
        (uint64_t)CEMU_SI3_QUANTUM_TICKS * CEMU_SI3_SAMPLE_RATE;
    size_t frames = (size_t)(state->sample_remainder / CEMU_SI3_CLOCK_HZ);
    state->sample_remainder %= CEMU_SI3_CLOCK_HZ;
    if (frames > CEMU_SI3_MAX_PACKET_SAMPLES - *used) return -1;
    for (size_t frame = 0; frame < frames; frame++) {
        int32_t mixed = 0;
        for (size_t index = 0; index < CEMU_SI3_VOICE_COUNT; index++) {
            cemu_si3_voice_t *voice = &state->voices[index];
            if (!voice->active) continue;
            int32_t gain = voice->gain;
            if (voice->release_samples)
                gain = gain * (int32_t)voice->release_samples /
                       (int32_t)cemu_si3_production_profile.release_samples;
            uint32_t x = voice->phase >> 16;
            int32_t triangle = (int32_t)(x < 32768u ? x : 65535u - x) * 4 - 65534;
            mixed += triangle * gain / 65534;
            voice->phase += voice->phase_step;
            if (voice->release_samples && --voice->release_samples == 0)
                voice->active = 0;
        }
        state->output[(*used)++] = clamp16(mixed);
    }
    state->quanta++;
    return 0;
}

void cemu_si3_reset(cemu_si3_state_t *state) {
    if (state) memset(state, 0, sizeof *state);
}

int cemu_si3_process_packet_profile(cemu_si3_state_t *state,
                            const uint8_t *packet, size_t packet_size,
                            const int16_t **samples, size_t *sample_count, int recovered) {
    if (samples) *samples = NULL;
    if (sample_count) *sample_count = 0;
    if (!state || !samples || !sample_count) return -1;
    if (!packet || !packet_size || (packet_size & 1u)) goto malformed;
    /* Preflight complete grammar and output bound before mutating musical state. */
    size_t waits = 0;
    if (packet_size > 128u) goto malformed;
    for (size_t pos = 0; pos < packet_size;) {
        uint16_t op = le16(packet + pos);
        unsigned family = op >> 8;
        size_t length = 2;
        if (op == 0xc1cc) waits++;
        else if (family == 0x11 || family == 0x91) {
            if (packet_size - pos < 4) goto malformed;
            unsigned kind = packet[pos + 3] >> 5;
            if (recovered && (kind == 2 || kind == 3)) goto malformed;
            length = recovered && kind >= 4 ? 26 : 16;
        } else if (recovered && (family & 0x70) == 0x20) length = 6;
        else if (family != 0x01 && family != 0x81) goto malformed;
        if (packet_size - pos < length) goto malformed;
        pos += length;
    }
    if ((state->sample_remainder + waits * (uint64_t)CEMU_SI3_QUANTUM_TICKS *
         CEMU_SI3_SAMPLE_RATE) / CEMU_SI3_CLOCK_HZ > CEMU_SI3_MAX_PACKET_SAMPLES)
        goto malformed;
    size_t offset = 0, used = 0;
    while (offset < packet_size) {
        uint16_t operation = le16(packet + offset);
        if (operation == 0xC1CCu) {
            if (render_quantum(state, &used) != 0) goto malformed;
            offset += 2u;
            continue;
        }
        if ((operation & 0xff00u) == 0x9100u ||
            (operation & 0xff00u) == 0x1100u) {
            if (packet_size - offset < 16u) goto malformed;
            unsigned index = voice_index(operation);
            cemu_si3_voice_t *voice = &state->voices[index];
            voice->active = 1;
            voice->phase = 0;
            voice->level = packet[offset + 3u] & (recovered ? 31u : 255u);
            uint16_t packed = le16(packet + offset + 4u);
            voice->frequency_number = recovered ? packed >> 6 : packed >> 4;
            voice->octave = (packed >> 2) & (recovered ? 15u : 3u);
            voice->phase_step = pitch_step(
                voice->frequency_number, voice->octave, recovered);
            size_t length = recovered && (packet[offset + 3] >> 5) >= 4 ? 26 : 16;
            memset(voice->parameters, 0, sizeof voice->parameters);
            memcpy(voice->parameters, packet + offset + 2, length - 2);
            voice->parameter_length = (uint8_t)(length - 2);
            voice->release_samples = 0;
            voice->gain = voice->level *
                          (int32_t)cemu_si3_production_profile.gain_per_level;
            state->voice_starts++;
            if ((operation & 0xff00u) == 0x1100u)
                state->start_family_11++;
            else
                state->start_family_91++;
            offset += length;
            continue;
        }
        if ((operation & 0xff00u) == 0x8100u ||
            (operation & 0xff00u) == 0x0100u) {
            cemu_si3_voice_t *voice = &state->voices[voice_index(operation)];
            if (voice->active)
                voice->release_samples =
                    cemu_si3_production_profile.release_samples;
            state->voice_stops++;
            offset += 2u;
            continue;
        }
        if (recovered && ((operation >> 8) & 0x70u) == 0x20u) {
            memcpy(state->last_control, packet + offset, 6);
            state->controls++;
            offset += 6;
            continue;
        }
        goto malformed;
    }
    *samples = state->output;
    *sample_count = used;
    return 0;

malformed:
    state->malformed_packets++;
    return -1;
}

int cemu_si3_process_packet(cemu_si3_state_t *state,
    const uint8_t *packet, size_t packet_size, const int16_t **samples,
    size_t *sample_count) {
    return cemu_si3_process_packet_profile(state, packet, packet_size,
                                            samples, sample_count, 1);
}
