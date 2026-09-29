#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "si3.h"

static int failures;
#define CHECK(condition) do { \
    if (!(condition)) { \
        failures++; \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
    } \
} while (0)

static size_t transitions(const int16_t *samples, size_t count) {
    size_t result = 0;
    for (size_t i = 1; i < count; i++)
        result += (samples[i - 1u] < 0) != (samples[i] < 0);
    return result;
}

int main(void) {
    cemu_si3_state_t state;
    cemu_si3_reset(&state);
    const int16_t *samples = NULL;
    size_t count = 0;
    const uint8_t note_a[] = {
        0x01,0x91,0x10,0x18,0x14,0x73,0x14,0x14,
        0x0a,0x4d,0x72,0x52,0xf8,0xff,0xa0,0xa0,
    };
    const uint8_t wait[] = {0xcc,0xc1};
    CHECK(cemu_si3_process_packet(
              &state, note_a, sizeof note_a, &samples, &count) == 0);
    CHECK(count == 0 && state.voice_starts == 1 && state.voices[0].active);
    CHECK(state.start_family_91 == 1 && state.start_family_11 == 0);
    CHECK(state.voices[0].frequency_number == 460);
    CHECK(state.voices[0].octave == 5 && state.voices[0].level == 0x18);
    CHECK(cemu_si3_process_packet(
              &state, wait, sizeof wait, &samples, &count) == 0);
    CHECK(count == 73);
    CHECK(transitions(samples, count) >= 5);
    int16_t first_quantum[74];
    memcpy(first_quantum, samples, count * sizeof *samples);
    CHECK(cemu_si3_process_packet(
              &state, wait, sizeof wait, &samples, &count) == 0);
    CHECK(count == 74); /* 120000/26MHz is retained fractionally. */

    cemu_si3_reset(&state);
    uint8_t note_b[sizeof note_a];
    memcpy(note_b, note_a, sizeof note_b);
    note_b[3] = 0x19;
    note_b[4] = 0xD4;
    note_b[5] = 0xA2;
    CHECK(cemu_si3_process_packet(
              &state, note_b, sizeof note_b, &samples, &count) == 0);
    CHECK(cemu_si3_process_packet(
              &state, wait, sizeof wait, &samples, &count) == 0);
    CHECK(transitions(samples, count) > transitions(first_quantum, 73));

    /* Sibling firmware uses the same measured layout with an 0x11xx start
     * family. The high-bit distinction remains observable, not named. */
    cemu_si3_reset(&state);
    uint8_t sibling_note[sizeof note_a];
    memcpy(sibling_note, note_a, sizeof sibling_note);
    sibling_note[1] = 0x11;
    CHECK(cemu_si3_process_packet(
              &state, sibling_note, sizeof sibling_note,
              &samples, &count) == 0);
    CHECK(state.voice_starts == 1 && state.start_family_11 == 1 &&
          state.start_family_91 == 0 && state.voices[0].active);
    CHECK(state.voices[0].frequency_number == 460 &&
          state.voices[0].octave == 5 && state.voices[0].level == 0x18);
    CHECK(cemu_si3_process_packet(
              &state, wait, sizeof wait, &samples, &count) == 0);
    CHECK(count == 73 && transitions(samples, count) >= 5);

    /* A matching stop enters a bounded release and terminates cleanly. */
    const uint8_t stop[] = {0x01,0x81};
    CHECK(cemu_si3_process_packet(
              &state, stop, sizeof stop, &samples, &count) == 0);
    CHECK(state.voices[0].release_samples == 1024);
    CHECK(cemu_si3_process_packet(
              &state, wait, sizeof wait, &samples, &count) == 0);
    CHECK(state.voices[0].active && state.voice_stops == 1);
    CHECK(samples[0] != 0);
    CHECK(cemu_si3_process_packet(
              &state, wait, sizeof wait, &samples, &count) == 0);
    CHECK(state.voices[0].active);

    /* Distinct slots mix deterministically and clamp without wraparound. */
    cemu_si3_reset(&state);
    uint8_t two_notes[sizeof note_a * 2u];
    memcpy(two_notes, note_a, sizeof note_a);
    memcpy(two_notes + sizeof note_a, note_b, sizeof note_b);
    two_notes[sizeof note_a] = 0x11;
    CHECK(cemu_si3_process_packet(
              &state, two_notes, sizeof two_notes, &samples, &count) == 0);
    CHECK(state.voice_starts == 2 && state.voices[0].active &&
          state.voices[1].active);
    CHECK(cemu_si3_process_packet(
              &state, wait, sizeof wait, &samples, &count) == 0);
    CHECK(count == 73);
    for (size_t i = 0; i < count; i++)
        CHECK(samples[i] >= -12000 && samples[i] <= 12000);

    const uint8_t combined[] = {
        0x01,0x01,
        0x21,0x91,0x10,0x18,0x14,0x7a,0x14,0x14,
        0x0a,0x4d,0x72,0x52,0xf8,0xff,0xa0,0xa0,
    };
    CHECK(cemu_si3_process_packet(
              &state, combined, sizeof combined, &samples, &count) == 0);
    CHECK(state.voices[0].release_samples == 1024 && state.voices[2].active);

    const uint8_t truncated[] = {0x01,0x91};
    CHECK(cemu_si3_process_packet(
              &state, truncated, sizeof truncated, &samples, &count) != 0);
    CHECK(state.malformed_packets == 1);

    /* Extended starts, packed fields, controls, and whole-packet atomicity. */
    cemu_si3_reset(&state);
    uint8_t extended[26] = {1, 0x91, 0, 0x9f, 0xfc, 0xff};
    CHECK(cemu_si3_process_packet(&state, extended, 26, &samples, &count) == 0);
    CHECK(state.voices[0].frequency_number == 1023 && state.voices[0].octave == 15);
    CHECK(state.voices[0].level == 31 && state.voices[0].parameter_length == 24);
    uint64_t expected_step = (((uint64_t)1023 * 32768 * 190 % 64000000) << 32) / 64000000;
    CHECK(state.voices[0].phase_step == expected_step);
    uint8_t control[] = {1, 0xa2, 0xcc, 0xc1, 0xff, 0xff};
    CHECK(cemu_si3_process_packet(&state, control, 6, &samples, &count) == 0);
    CHECK(state.controls == 1 && count == 0 && !memcmp(state.last_control, control, 6));
    cemu_si3_state_t before = state;
    uint8_t malformed[] = {0xcc, 0xc1, 1, 0x91};
    CHECK(cemu_si3_process_packet(&state, malformed, 4, &samples, &count) == -1);
    CHECK(!samples && count == 0);
    before.malformed_packets++;
    CHECK(!memcmp(&state, &before, sizeof state));
    for (unsigned kind = 0; kind < 8; kind++) {
        extended[3] = (uint8_t)((kind << 5) | 7);
        int valid = kind < 2 || kind >= 4;
        CHECK((cemu_si3_process_packet(&state, extended, kind < 4 ? 16 : 26,
                                      &samples, &count) == 0) == valid);
    }
    uint8_t waits[128];
    for (size_t i = 0; i < sizeof waits; i += 2) { waits[i] = 0xcc; waits[i+1] = 0xc1; }
    state.sample_remainder = CEMU_SI3_CLOCK_HZ - 1;
    CHECK(cemu_si3_process_packet(&state, waits, sizeof waits, &samples, &count) == 0);
    CHECK(count == 4727);
    for (unsigned octave = 0; octave < 15; octave++) {
        unsigned packed = (460u << 6) | (octave << 2);
        extended[3] = 7; extended[4] = packed; extended[5] = packed >> 8;
        CHECK(cemu_si3_process_packet(&state, extended, 16, &samples, &count) == 0);
        CHECK(state.voices[0].octave == octave && state.voices[0].frequency_number == 460);
        CHECK(state.voices[0].phase_step ==
              (((uint64_t)460 * (1u << octave) * 190 % 64000000) << 32) / 64000000);
    }
    puts(failures ? "X55 SI3 sequencer: FAIL" : "X55 SI3 sequencer: PASS");
    return failures ? 1 : 0;
}
