/* Sequencer for qualified firmware-rendered X55 SI3 command streams. */
#ifndef CEMU_PERIPH_AUDIO_SI3_H
#define CEMU_PERIPH_AUDIO_SI3_H

#include <stddef.h>
#include <stdint.h>

#define CEMU_SI3_VOICE_COUNT 16u
#define CEMU_SI3_MAX_PACKET_SAMPLES 4727u
#define CEMU_SI3_SAMPLE_RATE 16000u
#define CEMU_SI3_CLOCK_HZ 26000000u
#define CEMU_SI3_QUANTUM_TICKS 120000u

typedef enum {
    CEMU_SI3_WAVE_TRIANGLE = 1,
} cemu_si3_waveform_t;

typedef struct {
    uint32_t frequency_millihz_per_unit;
    cemu_si3_waveform_t waveform;
    uint32_t gain_per_level;
    uint32_t release_samples;
} cemu_si3_profile_t;

extern const cemu_si3_profile_t cemu_si3_production_profile;

typedef struct {
    int active;
    uint32_t phase;
    uint32_t phase_step;
    uint32_t release_samples;
    int32_t gain;
    uint16_t frequency_number;
    uint8_t octave;
    uint8_t level;
    uint8_t parameters[24];
    uint8_t parameter_length;
} cemu_si3_voice_t;

typedef struct {
    cemu_si3_voice_t voices[CEMU_SI3_VOICE_COUNT];
    uint64_t sample_remainder;
    uint64_t quanta;
    uint64_t voice_starts;
    uint64_t start_family_11;
    uint64_t start_family_91;
    uint64_t voice_stops;
    uint64_t malformed_packets;
    uint64_t controls;
    uint8_t last_control[6];
    int16_t output[CEMU_SI3_MAX_PACKET_SAMPLES];
} cemu_si3_state_t;

void cemu_si3_reset(cemu_si3_state_t *state);

/* Consume one complete firmware packet and render any elapsed quanta. The
 * returned samples are mono signed-16 PCM owned by `state` until the next
 * call. Unknown/truncated operations fail closed and increment diagnostics. */
int cemu_si3_process_packet(cemu_si3_state_t *state,
                            const uint8_t *packet, size_t packet_size,
                            const int16_t **samples, size_t *sample_count);

/* The alternate legacy layout is for offline historical comparisons only. */
int cemu_si3_process_packet_profile(cemu_si3_state_t *state,
    const uint8_t *packet, size_t packet_size, const int16_t **samples,
    size_t *sample_count, int recovered);

#endif /* CEMU_PERIPH_AUDIO_SI3_H */
