#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio_common.h"
#include "speaker.h"
#include "soc.h"

static int failures;
#define CHECK(condition) do { \
    if (!(condition)) { \
        failures++; \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
    } \
} while (0)

typedef struct {
    int16_t samples[4096];
    size_t frames;
    size_t calls;
    uint64_t tick;
    size_t resets;
    cemu_audio_discontinuity_t reason;
} output_log_t;

static void record_output(void *opaque, uint64_t tick,
                          const int16_t *samples, size_t frames) {
    output_log_t *log = opaque;
    CHECK(log->frames + frames <= 4096);
    if (log->frames + frames <= 4096)
        memcpy(log->samples + log->frames, samples,
               frames * sizeof *samples);
    log->frames += frames;
    log->calls++;
    log->tick = tick;
}

static void record_reset(void *opaque, uint64_t tick,
                         cemu_audio_discontinuity_t reason) {
    output_log_t *log = opaque;
    log->resets++;
    log->reason = reason;
    log->tick = tick;
}

static void run_ticks(peripheral_t *speaker, soc_t *soc, size_t ticks) {
    for (size_t i = 0; i < ticks; i++) {
        soc->ticks++;
        speaker->tick(speaker, soc, 1);
    }
}

static void test_queue_resampling_and_downmix(void) {
    cemu_audio_source_queue_t a = {0}, b = {0};
    const int16_t stereo[] = {100, -99, 300, 100};
    CHECK(cemu_audio_source_queue_push(&a, 16000, 2, stereo, 4) == 0);
    CHECK(a.count == 3 && a.has_held_frame && a.held_frame == 200);
    cemu_audio_source_queue_drain(&a);
    CHECK(a.count == 6);
    const int16_t expected_stereo[] = {0, 66, 133, 200, 200, 200};
    CHECK(!memcmp(a.frames, expected_stereo, sizeof expected_stereo));

    cemu_audio_source_queue_reset(&a);
    const int16_t positive[] = {0, 5};
    const int16_t negative[] = {0, -5};
    const int16_t expected_positive[] =
        {0, 0, 1, 2, 3, 4, 5, 5, 5, 5, 5, 5};
    const int16_t expected_negative[] =
        {0, 0, -1, -2, -3, -4, -5, -5, -5, -5, -5, -5};
    CHECK(cemu_audio_source_queue_push(&a, 8000, 1, positive, 2) == 0);
    cemu_audio_source_queue_drain(&a);
    CHECK(a.count == 12 &&
          !memcmp(a.frames, expected_positive, sizeof expected_positive));
    cemu_audio_source_queue_reset(&a);
    CHECK(cemu_audio_source_queue_push(&a, 8000, 1, negative, 2) == 0);
    cemu_audio_source_queue_drain(&a);
    CHECK(a.count == 12 &&
          !memcmp(a.frames, expected_negative, sizeof expected_negative));

    /* Retained frame and phase make every producer chunking byte-identical. */
    const int16_t mono[] = {-300, -101, 0, 99, 301, 700};
    const uint32_t rates[] = {8000, 16000, 44100, 48000};
    for (size_t rate = 0; rate < sizeof rates / sizeof rates[0]; rate++) {
        cemu_audio_source_queue_reset(&a);
        cemu_audio_source_queue_reset(&b);
        CHECK(cemu_audio_source_queue_push(&a, rates[rate], 1, mono, 6) == 0);
        for (size_t frame = 0; frame < 6; frame++)
            CHECK(cemu_audio_source_queue_push(
                      &b, rates[rate], 1, mono + frame, 1) == 0);
        cemu_audio_source_queue_drain(&a);
        cemu_audio_source_queue_drain(&b);
        size_t expected_count =
            (6u * CEMU_AUDIO_OUTPUT_RATE + rates[rate] - 1u) / rates[rate];
        CHECK(a.count == expected_count && a.count == b.count);
        CHECK(!memcmp(a.frames, b.frames, a.count * sizeof *a.frames));
        if (rates[rate] == CEMU_AUDIO_OUTPUT_RATE)
            CHECK(!memcmp(a.frames, mono, sizeof mono));
    }
    cemu_audio_source_queue_reset(&a);
    CHECK(cemu_audio_source_queue_push(&a, 16000, 1, mono, 1) == 0);
    CHECK(cemu_audio_source_queue_push(&a, 8000, 1, mono + 1, 1) == -1);
    cemu_audio_source_queue_free(&a);
    cemu_audio_source_queue_free(&b);
}

static void test_open_pause_drain_and_cancel(void) {
    peripheral_t peripheral;
    cemu_speaker_state_t speaker;
    soc_t soc = {0};
    output_log_t log = {0};
    cemu_speaker_periph_init(&peripheral, &speaker, 48000, 0);
    cemu_speaker_attach(&speaker, record_output, record_reset, &log);

    const int16_t first = 10, second = 20;
    CHECK(cemu_speaker_push_pcm(&speaker, CEMU_AUDIO_SOURCE_XBUS,
                                0, 48000, 1, &first, 1) == 0);
    CHECK(speaker.pcm[CEMU_AUDIO_SOURCE_XBUS].count == 0);
    CHECK(peripheral.next_event_ticks(&peripheral, &soc) == UINT64_MAX);
    run_ticks(&peripheral, &soc, 100);
    CHECK(log.calls == 0 && speaker.output_count == 0 &&
          speaker.clock_accumulator == 0);
    /* Batch-IDLE uses advance_quiet for this same open-but-starved span. */
    soc.ticks += 100;
    peripheral.advance_quiet(&peripheral, &soc, 100);
    CHECK(log.calls == 0 && speaker.output_count == 0 &&
          speaker.clock_accumulator == 0);

    CHECK(cemu_speaker_push_pcm(&speaker, CEMU_AUDIO_SOURCE_XBUS,
                                soc.ticks, 48000, 1, &second, 1) == 0);
    run_ticks(&peripheral, &soc, 1);
    CHECK(log.calls == 0 && speaker.output_count == 1 &&
          speaker.output[0] == first);
    run_ticks(&peripheral, &soc, 100);
    CHECK(log.calls == 0 && speaker.output_count == 1);
    cemu_speaker_drain_pcm(&speaker, CEMU_AUDIO_SOURCE_XBUS, soc.ticks);
    run_ticks(&peripheral, &soc, 1);
    CHECK(log.calls == 1 && log.frames == 2 &&
          log.samples[0] == first && log.samples[1] == second);

    CHECK(cemu_speaker_push_pcm(&speaker, CEMU_AUDIO_SOURCE_XBUS,
                                soc.ticks, 16000, 1, &first, 1) == 0);
    cemu_speaker_cancel_pcm(&speaker, CEMU_AUDIO_SOURCE_XBUS,
                            soc.ticks, 1);
    CHECK(log.resets == 1 &&
          log.reason == CEMU_AUDIO_DISCONTINUITY_SOURCE_CANCELLED);
    CHECK(!speaker.pcm[CEMU_AUDIO_SOURCE_XBUS].count &&
          !speaker.pcm[CEMU_AUDIO_SOURCE_XBUS].has_held_frame &&
          !speaker.pcm[CEMU_AUDIO_SOURCE_XBUS].input_rate);
    cemu_speaker_free(&speaker);
}

static void test_blocks_mix_tail_cancel_and_overflow(void) {
    peripheral_t peripheral;
    cemu_speaker_state_t speaker;
    soc_t soc = {0};
    output_log_t log = {0};
    cemu_speaker_periph_init(&peripheral, &speaker, 48000, 1);
    cemu_speaker_attach(&speaker, record_output, record_reset, &log);
    CHECK(peripheral.next_event_ticks(&peripheral, &soc) == UINT64_MAX);

    int16_t full[256];
    for (size_t i = 0; i < 256; i++) full[i] = (int16_t)i;
    CHECK(cemu_speaker_push_pcm(&speaker, CEMU_AUDIO_SOURCE_XBUS,
                                 0, 48000, 1,
                                 full, 256) == 0);
    /* Active audio forces a one-tick deadline, making IDLE-batched and
     * per-tick execution traverse the identical speaker sample path. */
    CHECK(peripheral.next_event_ticks(&peripheral, &soc) == 1);
    cemu_speaker_drain_pcm(&speaker, CEMU_AUDIO_SOURCE_XBUS, 0);
    run_ticks(&peripheral, &soc, 256);
    CHECK(log.calls == 1 && log.frames == 256 && log.tick == 256);
    CHECK(!memcmp(log.samples, full, sizeof full));

    const int16_t partial[] = {10, 20, 30};
    CHECK(cemu_speaker_push_pcm(&speaker, CEMU_AUDIO_SOURCE_XBUS,
                                 soc.ticks, 48000, 1,
                                 partial, 3) == 0);
    cemu_speaker_drain_pcm(&speaker, CEMU_AUDIO_SOURCE_XBUS, soc.ticks);
    run_ticks(&peripheral, &soc, 3);
    CHECK(log.calls == 2 && log.frames == 259 && log.samples[258] == 30);

    const int16_t xbus = 1000, yamaha = -250;
    CHECK(cemu_speaker_push_pcm(&speaker, CEMU_AUDIO_SOURCE_XBUS,
                                 soc.ticks, 48000, 1, &xbus, 1) == 0);
    CHECK(cemu_speaker_push_pcm(&speaker, CEMU_AUDIO_SOURCE_YAMAHA,
                                 soc.ticks, 48000, 1, &yamaha, 1) == 0);
    cemu_speaker_drain_pcm(&speaker, CEMU_AUDIO_SOURCE_XBUS, soc.ticks);
    cemu_speaker_drain_pcm(&speaker, CEMU_AUDIO_SOURCE_YAMAHA, soc.ticks);
    run_ticks(&peripheral, &soc, 1);
    CHECK(log.calls == 3 && log.frames == 260 && log.samples[259] == 750);
    CHECK(peripheral.next_event_ticks(&peripheral, &soc) == UINT64_MAX);

    int16_t loud = INT16_MAX;
    CHECK(cemu_speaker_push_pcm(&speaker, CEMU_AUDIO_SOURCE_XBUS,
                                 soc.ticks, 48000, 1,
                                 &loud, 1) == 0);
    cemu_speaker_drain_pcm(&speaker, CEMU_AUDIO_SOURCE_XBUS, soc.ticks);
    cemu_speaker_ringer_transition(&speaker, soc.ticks, 1);
    run_ticks(&peripheral, &soc, 1);
    CHECK(speaker.clipped_frames == 1);
    cemu_speaker_cancel_pcm(&speaker, CEMU_AUDIO_SOURCE_XBUS,
                            soc.ticks, 1);
    CHECK(log.resets == 1 &&
          log.reason == CEMU_AUDIO_DISCONTINUITY_SOURCE_CANCELLED);

    cemu_speaker_reset(&speaker);
    cemu_speaker_ringer_transition(&speaker, soc.ticks, 0);
    run_ticks(&peripheral, &soc, 2400);
    CHECK(log.frames == 2660 && !speaker.ringer_active);
    for (size_t i = log.frames - 2400; i < log.frames; i++)
        CHECK(log.samples[i] == -6553);

    int16_t *overflow = malloc((CEMU_AUDIO_SOURCE_MAX_FRAMES + 1u) *
                               sizeof *overflow);
    CHECK(overflow != NULL);
    if (overflow) {
        memset(overflow, 0, (CEMU_AUDIO_SOURCE_MAX_FRAMES + 1u) *
                            sizeof *overflow);
        CHECK(cemu_speaker_push_pcm(
                  &speaker, CEMU_AUDIO_SOURCE_XBUS, soc.ticks, 48000, 1, overflow,
                  CEMU_AUDIO_SOURCE_MAX_FRAMES + 1u) == 1);
        CHECK(speaker.pcm[CEMU_AUDIO_SOURCE_XBUS].overflow_count == 1);
        CHECK(log.resets == 2 &&
              log.reason == CEMU_AUDIO_DISCONTINUITY_SOURCE_OVERFLOW);
        free(overflow);
    }

    /* The retained final frame counts against the same eight-second bound
     * even though it is not materialized until drain. */
    int16_t slow[9] = {0};
    CHECK(cemu_speaker_push_pcm(&speaker, CEMU_AUDIO_SOURCE_XBUS,
                                soc.ticks, 1, 1, slow, 8) == 0);
    CHECK(speaker.pcm[CEMU_AUDIO_SOURCE_XBUS].count ==
          CEMU_AUDIO_SOURCE_MAX_FRAMES - CEMU_AUDIO_OUTPUT_RATE);
    CHECK(cemu_speaker_push_pcm(&speaker, CEMU_AUDIO_SOURCE_XBUS,
                                soc.ticks, 1, 1, slow + 8, 1) == 1);
    CHECK(speaker.pcm[CEMU_AUDIO_SOURCE_XBUS].overflow_count == 2);
    CHECK(log.resets == 3 &&
          log.reason == CEMU_AUDIO_DISCONTINUITY_SOURCE_OVERFLOW);
    cemu_speaker_free(&speaker);
}

int main(void) {
    test_queue_resampling_and_downmix();
    test_open_pause_drain_and_cancel();
    test_blocks_mix_tail_cancel_and_overflow();
    puts(failures ? "terminal speaker: FAIL" : "terminal speaker: PASS");
    return failures ? 1 : 0;
}
