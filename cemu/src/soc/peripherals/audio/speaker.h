/* Terminal handset speaker: final 48-kHz mono signed-16 output. */
#ifndef CEMU_PERIPH_AUDIO_SPEAKER_H
#define CEMU_PERIPH_AUDIO_SPEAKER_H

#include "audio_common.h"
#include "peripheral.h"

typedef enum {
    CEMU_AUDIO_DISCONTINUITY_SOURCE_CANCELLED = 0,
    CEMU_AUDIO_DISCONTINUITY_SOURCE_OVERFLOW,
} cemu_audio_discontinuity_t;

typedef void (*cemu_speaker_output_fn)(void *, uint64_t,
                                       const int16_t *, size_t);
typedef void (*cemu_speaker_reset_fn)(void *, uint64_t,
                                      cemu_audio_discontinuity_t);

typedef struct {
    uint32_t clock_hz;
    cemu_audio_source_queue_t pcm[CEMU_AUDIO_SOURCE_COUNT];
    int pcm_draining[CEMU_AUDIO_SOURCE_COUNT];
    int ringer_present;
    int ringer_level;
    int ringer_active;
    uint64_t ringer_last_transition;
    uint64_t ringer_tail_deadline;
    uint64_t clock_accumulator;
    int16_t output[CEMU_AUDIO_BLOCK_FRAMES];
    size_t output_count;
    uint64_t output_blocks;
    uint64_t output_frames;
    uint64_t clipped_frames;
    cemu_speaker_output_fn callback;
    cemu_speaker_reset_fn reset_callback;
    void *callback_opaque;
} cemu_speaker_state_t;

void cemu_speaker_periph_init(peripheral_t *peripheral,
                              cemu_speaker_state_t *state,
                              uint32_t clock_hz, int ringer_present);
void cemu_speaker_attach(cemu_speaker_state_t *state,
                         cemu_speaker_output_fn output,
                         cemu_speaker_reset_fn reset, void *opaque);
void cemu_speaker_ringer_transition(cemu_speaker_state_t *state,
                                    uint64_t tick, int level);
int cemu_speaker_push_pcm(cemu_speaker_state_t *state,
                          cemu_audio_source_t source, uint64_t tick,
                          uint32_t sample_rate, uint16_t channels,
                          const int16_t *samples, size_t sample_count);
void cemu_speaker_drain_pcm(cemu_speaker_state_t *state,
                            cemu_audio_source_t source, uint64_t tick);
void cemu_speaker_cancel_pcm(cemu_speaker_state_t *state,
                             cemu_audio_source_t source, uint64_t tick,
                             int notify);
void cemu_speaker_reset(cemu_speaker_state_t *state);
void cemu_speaker_free(cemu_speaker_state_t *state);

#endif /* CEMU_PERIPH_AUDIO_SPEAKER_H */
