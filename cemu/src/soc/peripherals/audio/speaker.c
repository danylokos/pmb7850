#include "speaker.h"

#include <limits.h>
#include <string.h>

#include "soc.h"

#define RINGER_AMPLITUDE 6553
#define RINGER_TAIL_DIVISOR 20u

static int samples_ready(const cemu_speaker_state_t *state) {
    if (state->ringer_active) return 1;
    for (size_t source = 0; source < CEMU_AUDIO_SOURCE_COUNT; source++)
        if (state->pcm[source].count) return 1;
    return 0;
}

static int sources_open(const cemu_speaker_state_t *state) {
    for (size_t source = 0; source < CEMU_AUDIO_SOURCE_COUNT; source++)
        if (state->pcm[source].input_rate &&
            !state->pcm_draining[source]) return 1;
    return 0;
}

static void emit(cemu_speaker_state_t *state, uint64_t tick) {
    if (!state->output_count) return;
    if (state->callback)
        state->callback(state->callback_opaque, tick, state->output,
                        state->output_count);
    state->output_blocks++;
    state->output_frames += state->output_count;
    state->output_count = 0;
}

static void finish_if_idle(cemu_speaker_state_t *state, uint64_t tick) {
    for (size_t source = 0; source < CEMU_AUDIO_SOURCE_COUNT; source++) {
        if (!state->pcm[source].count && state->pcm_draining[source]) {
            state->pcm_draining[source] = 0;
            cemu_audio_source_queue_reset(&state->pcm[source]);
        }
    }
    if (!samples_ready(state) && !sources_open(state)) {
        emit(state, tick);
        state->clock_accumulator = 0;
    }
}

static void render_frame(cemu_speaker_state_t *state, uint64_t tick) {
    int64_t mixed = 0;
    for (size_t source = 0; source < CEMU_AUDIO_SOURCE_COUNT; source++) {
        int16_t sample = 0;
        if (cemu_audio_source_queue_pop(&state->pcm[source], &sample))
            mixed += sample;
    }
    if (state->ringer_active)
        mixed += state->ringer_level ? RINGER_AMPLITUDE : -RINGER_AMPLITUDE;
    if (mixed < INT16_MIN) {
        mixed = INT16_MIN;
        state->clipped_frames++;
    } else if (mixed > INT16_MAX) {
        mixed = INT16_MAX;
        state->clipped_frames++;
    }
    state->output[state->output_count++] = (int16_t)mixed;
    if (state->output_count == CEMU_AUDIO_BLOCK_FRAMES) emit(state, tick);
    finish_if_idle(state, tick);
}

static void speaker_tick(peripheral_t *peripheral, soc_t *soc, int n) {
    (void)n;
    cemu_speaker_state_t *state = peripheral->state;
    if (!samples_ready(state)) {
        finish_if_idle(state, soc->ticks);
        return;
    }
    state->clock_accumulator += CEMU_AUDIO_OUTPUT_RATE;
    if (state->clock_accumulator >= state->clock_hz) {
        state->clock_accumulator -= state->clock_hz;
        render_frame(state, soc->ticks);
    }
    if (state->ringer_active && soc->ticks >= state->ringer_tail_deadline) {
        state->ringer_active = 0;
        finish_if_idle(state, soc->ticks);
    }
}

static uint64_t speaker_next_event(peripheral_t *peripheral, soc_t *soc) {
    (void)soc;
    cemu_speaker_state_t *state = peripheral->state;
    return samples_ready(state) ? 1u : UINT64_MAX;
}

static void speaker_advance_quiet(peripheral_t *peripheral, soc_t *soc,
                                  uint64_t ticks) {
    (void)peripheral; (void)soc; (void)ticks;
    /* Active output returns a one-tick deadline, so only quiescent speakers
     * reach this hook. No private timebase advances while all sources pause. */
}

void cemu_speaker_periph_init(peripheral_t *peripheral,
                              cemu_speaker_state_t *state,
                              uint32_t clock_hz, int ringer_present) {
    memset(state, 0, sizeof *state);
    state->clock_hz = clock_hz;
    state->ringer_present = ringer_present != 0;
    memset(peripheral, 0, sizeof *peripheral);
    peripheral->id = "speaker";
    peripheral->state = state;
    peripheral->tick = speaker_tick;
    peripheral->next_event_ticks = speaker_next_event;
    peripheral->advance_quiet = speaker_advance_quiet;
}

void cemu_speaker_attach(cemu_speaker_state_t *state,
                         cemu_speaker_output_fn output,
                         cemu_speaker_reset_fn reset, void *opaque) {
    if (!state) return;
    state->callback = output;
    state->reset_callback = reset;
    state->callback_opaque = output || reset ? opaque : NULL;
}

void cemu_speaker_ringer_transition(cemu_speaker_state_t *state,
                                    uint64_t tick, int level) {
    if (!state || !state->ringer_present) return;
    state->ringer_level = level != 0;
    state->ringer_active = 1;
    state->ringer_last_transition = tick;
    state->ringer_tail_deadline = tick + state->clock_hz / RINGER_TAIL_DIVISOR;
}

int cemu_speaker_push_pcm(cemu_speaker_state_t *state,
                          cemu_audio_source_t source, uint64_t tick,
                          uint32_t sample_rate, uint16_t channels,
                          const int16_t *samples, size_t sample_count) {
    if (!state || source < 0 || source >= CEMU_AUDIO_SOURCE_COUNT) return -1;
    int result = cemu_audio_source_queue_push(
        &state->pcm[source], sample_rate, channels, samples, sample_count);
    if (result == 0) state->pcm_draining[source] = 0;
    if (result == 1) {
        state->output_count = 0;
        state->clock_accumulator = 0;
        if (state->reset_callback)
            state->reset_callback(state->callback_opaque, tick,
                                  CEMU_AUDIO_DISCONTINUITY_SOURCE_OVERFLOW);
    }
    return result;
}

void cemu_speaker_drain_pcm(cemu_speaker_state_t *state,
                            cemu_audio_source_t source, uint64_t tick) {
    if (!state || source < 0 || source >= CEMU_AUDIO_SOURCE_COUNT) return;
    cemu_audio_source_queue_drain(&state->pcm[source]);
    state->pcm_draining[source] = 1;
    finish_if_idle(state, tick);
}

void cemu_speaker_cancel_pcm(cemu_speaker_state_t *state,
                             cemu_audio_source_t source, uint64_t tick,
                             int notify) {
    if (!state || source < 0 || source >= CEMU_AUDIO_SOURCE_COUNT) return;
    cemu_audio_source_queue_reset(&state->pcm[source]);
    state->pcm_draining[source] = 0;
    state->output_count = 0;
    state->clock_accumulator = 0;
    if (notify && state->reset_callback)
        state->reset_callback(state->callback_opaque, tick,
                              CEMU_AUDIO_DISCONTINUITY_SOURCE_CANCELLED);
}

void cemu_speaker_reset(cemu_speaker_state_t *state) {
    if (!state) return;
    for (size_t source = 0; source < CEMU_AUDIO_SOURCE_COUNT; source++) {
        cemu_audio_source_queue_reset(&state->pcm[source]);
        state->pcm_draining[source] = 0;
    }
    state->ringer_level = 0;
    state->ringer_active = 0;
    state->ringer_last_transition = 0;
    state->ringer_tail_deadline = 0;
    state->clock_accumulator = 0;
    state->output_count = 0;
}

void cemu_speaker_free(cemu_speaker_state_t *state) {
    if (!state) return;
    for (size_t source = 0; source < CEMU_AUDIO_SOURCE_COUNT; source++)
        cemu_audio_source_queue_free(&state->pcm[source]);
}
