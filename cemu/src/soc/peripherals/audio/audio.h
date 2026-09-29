/* Model-neutral SoC audio façade. */
#ifndef CEMU_PERIPH_AUDIO_H
#define CEMU_PERIPH_AUDIO_H

#include "devices.h"
#include "speaker.h"

typedef struct {
    device_audio_config_t config;
    cemu_speaker_state_t *speaker;
} cemu_audio_t;

typedef cemu_speaker_state_t cemu_audio_state_storage_t;

void cemu_audio_init(cemu_audio_t *audio,
                     const device_audio_config_t *config);
int cemu_audio_periph_init(cemu_audio_t *audio, peripheral_t *peripheral,
                           cemu_audio_state_storage_t *storage);
void cemu_audio_attach(cemu_audio_t *audio, cemu_speaker_output_fn output,
                       cemu_speaker_reset_fn reset, void *opaque);
void cemu_audio_route_ringer(cemu_audio_t *audio, uint64_t tick,
                             int channel, int level);
int cemu_audio_push_pcm(cemu_audio_t *audio, cemu_audio_source_t source,
                        uint64_t tick,
                        uint32_t sample_rate, uint16_t channels,
                        const int16_t *samples, size_t sample_count);
void cemu_audio_drain_pcm(cemu_audio_t *audio, cemu_audio_source_t source,
                          uint64_t tick);
void cemu_audio_cancel_pcm(cemu_audio_t *audio, cemu_audio_source_t source,
                           uint64_t tick, int notify);
void cemu_audio_reset(cemu_audio_t *audio);
void cemu_audio_free(cemu_audio_t *audio);
int cemu_audio_available(const cemu_audio_t *audio);
int cemu_audio_xbus_available(const cemu_audio_t *audio);

#endif /* CEMU_PERIPH_AUDIO_H */
