#include "audio.h"

#include <string.h>

void cemu_audio_init(cemu_audio_t *audio,
                     const device_audio_config_t *config) {
    if (!audio) return;
    memset(audio, 0, sizeof *audio);
    if (config) audio->config = *config;
}

int cemu_audio_periph_init(cemu_audio_t *audio, peripheral_t *peripheral,
                           cemu_audio_state_storage_t *storage) {
    if (!audio || !peripheral || !storage || !cemu_audio_available(audio))
        return 0;
    cemu_speaker_periph_init(peripheral, storage, audio->config.clock_hz,
                             audio->config.capcom_ringer_present);
    audio->speaker = storage;
    return 1;
}

void cemu_audio_attach(cemu_audio_t *audio, cemu_speaker_output_fn output,
                       cemu_speaker_reset_fn reset, void *opaque) {
    if (audio && audio->speaker)
        cemu_speaker_attach(audio->speaker, output, reset, opaque);
}

void cemu_audio_route_ringer(cemu_audio_t *audio, uint64_t tick,
                             int channel, int level) {
    if (!audio || !audio->speaker || !audio->config.capcom_ringer_present ||
        channel != audio->config.capcom_ringer_channel) return;
    cemu_speaker_ringer_transition(audio->speaker, tick, level);
}

int cemu_audio_push_pcm(cemu_audio_t *audio, cemu_audio_source_t source,
                        uint64_t tick,
                        uint32_t sample_rate, uint16_t channels,
                        const int16_t *samples, size_t sample_count) {
    if (!cemu_audio_available(audio) || !audio->speaker) return -1;
    return cemu_speaker_push_pcm(audio->speaker, source, tick, sample_rate,
                                 channels, samples, sample_count);
}

void cemu_audio_drain_pcm(cemu_audio_t *audio, cemu_audio_source_t source,
                          uint64_t tick) {
    if (audio && audio->speaker)
        cemu_speaker_drain_pcm(audio->speaker, source, tick);
}

void cemu_audio_cancel_pcm(cemu_audio_t *audio, cemu_audio_source_t source,
                           uint64_t tick, int notify) {
    if (audio && audio->speaker)
        cemu_speaker_cancel_pcm(audio->speaker, source, tick, notify);
}

void cemu_audio_reset(cemu_audio_t *audio) {
    if (audio && audio->speaker) cemu_speaker_reset(audio->speaker);
}

void cemu_audio_free(cemu_audio_t *audio) {
    if (audio && audio->speaker) cemu_speaker_free(audio->speaker);
}

int cemu_audio_available(const cemu_audio_t *audio) {
    return audio && (audio->config.capcom_ringer_present ||
        audio->config.stream_profile != DEVICE_AUDIO_STREAM_NONE);
}

int cemu_audio_xbus_available(const cemu_audio_t *audio) {
    return audio && audio->config.stream_profile ==
        DEVICE_AUDIO_STREAM_XBUS_UNKNOWN1_V1;
}
