/* Shared fixed-format source queues used by the terminal speaker. */
#ifndef CEMU_PERIPH_AUDIO_COMMON_H
#define CEMU_PERIPH_AUDIO_COMMON_H

#include <stddef.h>
#include <stdint.h>

#define CEMU_AUDIO_OUTPUT_RATE 48000u
#define CEMU_AUDIO_BLOCK_FRAMES 256u
#define CEMU_AUDIO_SOURCE_MAX_FRAMES (8u * CEMU_AUDIO_OUTPUT_RATE)

typedef enum {
    CEMU_AUDIO_SOURCE_XBUS = 0,
    CEMU_AUDIO_SOURCE_YAMAHA,
    CEMU_AUDIO_SOURCE_COUNT,
} cemu_audio_source_t;

typedef struct {
    int16_t *frames;
    size_t head;
    size_t count;
    size_t capacity;
    uint32_t input_rate;
    uint32_t resample_phase;
    int16_t held_frame;
    int has_held_frame;
    uint64_t pushed_input_frames;
    uint64_t produced_output_frames;
    uint64_t overflow_count;
} cemu_audio_source_queue_t;

void cemu_audio_source_queue_reset(cemu_audio_source_queue_t *queue);
void cemu_audio_source_queue_free(cemu_audio_source_queue_t *queue);

/* Downmix and linearly resample one chunk into the 48-kHz mono queue. The last
 * input frame is retained as lookahead state until another chunk or drain.
 * Returns 1 when the eight-second bound would be crossed, -1 for invalid input
 * or allocation failure, and 0 on success. Overflow clears the source. */
int cemu_audio_source_queue_push(cemu_audio_source_queue_t *queue,
                                 uint32_t sample_rate, uint16_t channels,
                                 const int16_t *samples, size_t sample_count);
void cemu_audio_source_queue_drain(cemu_audio_source_queue_t *queue);
int cemu_audio_source_queue_pop(cemu_audio_source_queue_t *queue,
                                int16_t *sample);

#endif /* CEMU_PERIPH_AUDIO_COMMON_H */
