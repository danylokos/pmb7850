/* EB80/E836 audio transport delegated by the mixed-purpose XBUS device. */
#ifndef CEMU_PERIPH_AUDIO_XBUS_AUDIO_H
#define CEMU_PERIPH_AUDIO_XBUS_AUDIO_H

#include <stddef.h>
#include <stdint.h>

#include "ima_adpcm.h"
#include "si3.h"

#define XBUS_AUDIO_PACKET_MAX 128u
#define XBUS_AUDIO_QUEUE_CAP 8u
#define XBUS_AUDIO_STREAM_BASE 0xEB80u
#define XBUS_AUDIO_STREAM_END  0xEBFFu
#define XBUS_AUDIO_COMMAND     0xE836u

typedef enum {
    XBUS_AUDIO_STREAM_UNCLASSIFIED = 0,
    XBUS_AUDIO_STREAM_SAMPLED,
    XBUS_AUDIO_STREAM_SI3,
} xbus_audio_stream_kind_t;

typedef struct {
    int enabled;
    int stream_collecting;
    uint32_t stream_max_written;
    uint64_t stream_deadline;
    uint64_t stream_packets;
    uint64_t stream_accepted;
    uint64_t stream_retried;
    uint64_t stream_drained;
    uint64_t stream_replaced;
    uint64_t output_overflows;
    uint16_t stream_command;
    xbus_audio_stream_kind_t active_stream_kind;
    uint8_t stream_data[XBUS_AUDIO_QUEUE_CAP][XBUS_AUDIO_PACKET_MAX];
    uint8_t stream_length[XBUS_AUDIO_QUEUE_CAP];
    uint8_t stream_kind[XBUS_AUDIO_QUEUE_CAP];
    uint8_t stream_head;
    uint8_t stream_count;
    uint64_t stream_drain_deadline;
    uint8_t unclassified_data[XBUS_AUDIO_PACKET_MAX];
    uint8_t unclassified_length;
    int unclassified_pending;
    int command_pending;
    cemu_ima_adpcm_stream_t decoded_audio;
    cemu_si3_state_t si3;
} xbus_audio_state_t;

typedef struct soc soc_t;

void cemu_xbus_audio_init(xbus_audio_state_t *state, int enabled);
int cemu_xbus_audio_handles(const xbus_audio_state_t *state, uint32_t addr);
int cemu_xbus_audio_write8(xbus_audio_state_t *state, soc_t *soc,
                           uint32_t addr);
void cemu_xbus_audio_tick(xbus_audio_state_t *state, soc_t *soc);
uint64_t cemu_xbus_audio_next_event(const xbus_audio_state_t *state,
                                    const soc_t *soc);
void cemu_xbus_audio_reset(xbus_audio_state_t *state, soc_t *soc, int notify);

#endif /* CEMU_PERIPH_AUDIO_XBUS_AUDIO_H */
