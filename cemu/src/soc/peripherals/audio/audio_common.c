#include "audio_common.h"

#include <stdlib.h>
#include <string.h>

static size_t output_count(uint32_t rate, uint64_t phase, size_t frames,
                           uint32_t *next_phase) {
    size_t produced = 0;
    for (size_t frame = 0; frame < frames; frame++) {
        while (phase < CEMU_AUDIO_OUTPUT_RATE) {
            produced++;
            phase += rate;
        }
        phase -= CEMU_AUDIO_OUTPUT_RATE;
    }
    if (next_phase) *next_phase = phase;
    return produced;
}

void cemu_audio_source_queue_reset(cemu_audio_source_queue_t *queue) {
    if (!queue) return;
    queue->head = 0;
    queue->count = 0;
    queue->input_rate = 0;
    queue->resample_phase = 0;
    queue->held_frame = 0;
    queue->has_held_frame = 0;
    queue->pushed_input_frames = 0;
    queue->produced_output_frames = 0;
}

void cemu_audio_source_queue_free(cemu_audio_source_queue_t *queue) {
    if (!queue) return;
    free(queue->frames);
    memset(queue, 0, sizeof *queue);
}

static int reserve(cemu_audio_source_queue_t *queue, size_t additional) {
    size_t required = queue->count + additional;
    if (required <= queue->capacity &&
        queue->head + required <= queue->capacity) return 0;
    if (queue->head && queue->count)
        memmove(queue->frames, queue->frames + queue->head,
                queue->count * sizeof *queue->frames);
    queue->head = 0;
    if (required <= queue->capacity) return 0;
    size_t capacity = queue->capacity ? queue->capacity : 4096u;
    while (capacity < required) {
        size_t grown = capacity * 2u;
        if (grown < capacity || grown > CEMU_AUDIO_SOURCE_MAX_FRAMES)
            grown = CEMU_AUDIO_SOURCE_MAX_FRAMES;
        capacity = grown;
    }
    int16_t *replacement = realloc(queue->frames,
                                   capacity * sizeof *replacement);
    if (!replacement) return -1;
    queue->frames = replacement;
    queue->capacity = capacity;
    return 0;
}

int cemu_audio_source_queue_push(cemu_audio_source_queue_t *queue,
                                 uint32_t sample_rate, uint16_t channels,
                                 const int16_t *samples, size_t sample_count) {
    if (!queue || !sample_rate || (channels != 1u && channels != 2u) ||
        !samples || !sample_count || sample_count % channels) return -1;
    if (queue->input_rate && queue->input_rate != sample_rate) return -1;
    size_t input_frames = sample_count / channels;
    size_t rendered_frames = queue->has_held_frame
                           ? input_frames : input_frames - 1u;
    uint32_t next_phase = queue->resample_phase;
    size_t produced = output_count(sample_rate, queue->resample_phase,
                                   rendered_frames, &next_phase);
    size_t deferred = output_count(sample_rate, next_phase, 1u, NULL);
    if (produced > CEMU_AUDIO_SOURCE_MAX_FRAMES - queue->count ||
        deferred > CEMU_AUDIO_SOURCE_MAX_FRAMES - queue->count - produced) {
        queue->overflow_count++;
        cemu_audio_source_queue_reset(queue);
        return 1;
    }
    /* Reserve the drain output now so finalization cannot fail after the
     * producer has already been told that its chunk was accepted. */
    if (reserve(queue, produced + deferred) != 0) return -1;
    size_t target = queue->head + queue->count;
    uint64_t phase = queue->resample_phase;
    size_t frame = 0;
    if (!queue->has_held_frame) {
        int32_t mono = samples[frame * channels];
        if (channels == 2u)
            mono = (mono + (int32_t)samples[frame * 2u + 1u]) / 2;
        queue->held_frame = (int16_t)mono;
        queue->has_held_frame = 1;
        frame++;
    }
    for (; frame < input_frames; frame++) {
        int32_t mono = samples[frame * channels];
        if (channels == 2u)
            mono = (mono + (int32_t)samples[frame * 2u + 1u]) / 2;
        while (phase < CEMU_AUDIO_OUTPUT_RATE) {
            int64_t delta = mono - (int32_t)queue->held_frame;
            int64_t interpolated = queue->held_frame +
                delta * (int64_t)phase / CEMU_AUDIO_OUTPUT_RATE;
            queue->frames[target++] = (int16_t)interpolated;
            phase += sample_rate;
        }
        phase -= CEMU_AUDIO_OUTPUT_RATE;
        queue->held_frame = (int16_t)mono;
    }
    queue->count += produced;
    queue->input_rate = sample_rate;
    queue->resample_phase = (uint32_t)phase;
    queue->pushed_input_frames += input_frames;
    queue->produced_output_frames += produced;
    return 0;
}

void cemu_audio_source_queue_drain(cemu_audio_source_queue_t *queue) {
    if (!queue || !queue->has_held_frame) return;
    size_t target = queue->head + queue->count;
    size_t produced = 0;
    uint64_t phase = queue->resample_phase;
    while (phase < CEMU_AUDIO_OUTPUT_RATE) {
        queue->frames[target++] = queue->held_frame;
        phase += queue->input_rate;
        produced++;
    }
    phase -= CEMU_AUDIO_OUTPUT_RATE;
    queue->resample_phase = (uint32_t)phase;
    queue->count += produced;
    queue->produced_output_frames += produced;
    queue->held_frame = 0;
    queue->has_held_frame = 0;
}

int cemu_audio_source_queue_pop(cemu_audio_source_queue_t *queue,
                                int16_t *sample) {
    if (!queue || !sample || !queue->count) return 0;
    *sample = queue->frames[queue->head++];
    queue->count--;
    if (!queue->count) queue->head = 0;
    return 1;
}
