/* XBUS audio transport — see xbus_audio.h. */
#include "xbus_audio.h"

#include <limits.h>
#include <string.h>

#include "audio.h"
#include "soc.h"

#define AUDIO_SETTLE_TICKS 32u
#define AUDIO_ACCEPTED 0x5555u
#define AUDIO_RETRY 0xAAAAu

static uint16_t ram_word(soc_t *s, uint32_t addr) {
    return (uint16_t)(memory_controller_ram_get(&s->memory, addr) |
                      ((uint16_t)memory_controller_ram_get(&s->memory, addr + 1u) << 8));
}

static void set_ram_word(soc_t *s, uint32_t addr, uint16_t value) {
    memory_controller_ram_set(&s->memory, addr, (uint8_t)value);
    memory_controller_ram_set(&s->memory, addr + 1u, (uint8_t)(value >> 8));
}

static const char *stream_wav_packet(soc_t *s, xbus_audio_state_t *st,
                                     const uint8_t *packet, size_t size) {
    cemu_ima_adpcm_stream_t *state = &st->decoded_audio;
    if (!state->active) return "unsupported-rate";
    if (state->failed) return "failed";
    int16_t samples[XBUS_AUDIO_PACKET_MAX * 2u];
    int count = cemu_ima_adpcm_packet(state, packet, size, samples,
                                     sizeof samples / sizeof samples[0]);
    if (count < 0) {
        state->failed = 1;
        return "malformed-envelope";
    }
    int queued = cemu_audio_push_pcm(&s->audio, CEMU_AUDIO_SOURCE_XBUS,
                                    s->ticks, state->sample_rate, 1u,
                                    samples, (size_t)count);
    if (queued > 0) st->output_overflows++;
    return queued < 0 ? "output-failed" : queued > 0 ? "output-overflow" : "ok";
}

static const char *stream_si3_packet(soc_t *s, xbus_audio_state_t *st,
                                     const uint8_t *packet, size_t size) {
    const int16_t *samples = NULL;
    size_t sample_count = 0;
    if (cemu_si3_process_packet(
            &st->si3, packet, size, &samples, &sample_count) != 0)
        return "malformed";
    if (sample_count) {
        int queued = cemu_audio_push_pcm(
            &s->audio, CEMU_AUDIO_SOURCE_XBUS, s->ticks,
            CEMU_SI3_SAMPLE_RATE, 1u, samples, sample_count);
        if (queued < 0) return "output-failed";
        if (queued > 0) {
            st->output_overflows++;
            return "output-overflow";
        }
    }
    return sample_count ? "pcm" : "ok";
}

/* Keep output and outstanding packet acknowledgements alive after drain. */
static void stream_sampled_retire(xbus_audio_state_t *st) {
    cemu_ima_adpcm_stream_reset(&st->decoded_audio);
    st->active_stream_kind = XBUS_AUDIO_STREAM_UNCLASSIFIED;
}

static void stream_cancel(xbus_audio_state_t *st) {
    st->stream_collecting = 0;
    st->stream_deadline = 0;
    st->stream_head = 0;
    st->stream_count = 0;
    st->stream_drain_deadline = 0;
    st->unclassified_pending = 0;
    st->unclassified_length = 0;
    st->active_stream_kind = XBUS_AUDIO_STREAM_UNCLASSIFIED;
    cemu_ima_adpcm_stream_reset(&st->decoded_audio);
    cemu_si3_reset(&st->si3);
}

static int stream_profile_enabled(const xbus_audio_state_t *st) {
    return st && st->enabled;
}

static const char *stream_profile_name(const xbus_audio_state_t *st) {
    return stream_profile_enabled(st) ? "xbus-unknown1-v1" : "none";
}

static const char *stream_kind_name(xbus_audio_stream_kind_t kind) {
    switch (kind) {
    case XBUS_AUDIO_STREAM_SAMPLED: return "sampled";
    case XBUS_AUDIO_STREAM_SI3: return "si3";
    default: return "unclassified";
    }
}

static void emit_stream_packet(soc_t *s, xbus_audio_state_t *st,
                               const char *phase, const char *reason,
                               const char *decoder_outcome,
                               xbus_audio_stream_kind_t kind,
                               const uint8_t *payload, uint32_t length,
                               uint16_t old_token, uint16_t new_token) {
    if (!cemu_event_native_trace_active(&s->instrumentation,
                                        "xbus_audio_packet")) return;
    cemu_native_trace_event_t ev;
    memset(&ev, 0, sizeof ev);
    ev.kind = "xbus_audio_packet";
    ev.icount = s->cpu ? s->cpu->icount : 0;
    ev.pc = s->cpu ? cpu_pc(s->cpu) : 0;
    ev.has_addr = 1; ev.addr = XBUS_AUDIO_STREAM_BASE;
    ev.has_size = 1; ev.size = length;
    ev.has_value = 1; ev.value = new_token;
    ev.detail = "xbus-audio-stream";
    char payload_hex[XBUS_AUDIO_PACKET_MAX * 2u + 1u];
    static const char hex[] = "0123456789abcdef";
    for (uint32_t i = 0; i < length; i++) {
        uint8_t byte = payload ? payload[i] : 0;
        payload_hex[i * 2u] = hex[byte >> 4];
        payload_hex[i * 2u + 1u] = hex[byte & 0x0fu];
    }
    payload_hex[length * 2u] = '\0';
    cemu_event_field_i64(&ev.info, "command", st->stream_command);
    cemu_event_field_string(&ev.info, "completion_reason", reason);
    cemu_event_field_string(&ev.info, "decoder_outcome", decoder_outcome);
    cemu_event_field_i64(&ev.info, "new_token", new_token);
    cemu_event_field_i64(&ev.info, "old_token", old_token);
    cemu_event_field_string(&ev.info, "phase", phase);
    cemu_event_field_i64(&ev.info, "payload_length", length);
    cemu_event_field_string(&ev.info, "payload_hex", payload_hex);
    cemu_event_field_string(&ev.info, "profile", stream_profile_name(st));
    cemu_event_field_i64(&ev.info, "queue_depth", st->stream_count);
    cemu_event_field_i64(&ev.info, "sequence", (long)st->stream_packets);
    cemu_event_field_string(&ev.info, "stream_kind", stream_kind_name(kind));
    cemu_event_emit_native_trace(&s->instrumentation, &ev,
                                 CEMU_EVENT_PERIPHERAL);
}

static void stream_accept_packet(soc_t *s, xbus_audio_state_t *st,
                                 const uint8_t *payload, uint32_t length,
                                 uint16_t old_token, const char *phase) {
    if (length < 2u || length > XBUS_AUDIO_PACKET_MAX ||
        st->stream_count >= XBUS_AUDIO_QUEUE_CAP) {
        set_ram_word(s, XBUS_AUDIO_STREAM_BASE, AUDIO_RETRY);
        st->stream_retried++;
        emit_stream_packet(s, st, phase, "backpressure", "not-run",
                           st->active_stream_kind, payload, length,
                           old_token, AUDIO_RETRY);
        return;
    }

    uint8_t tail = (uint8_t)((st->stream_head + st->stream_count) %
                             XBUS_AUDIO_QUEUE_CAP);
    memcpy(st->stream_data[tail], payload, length);
    st->stream_length[tail] = (uint8_t)length;
    st->stream_kind[tail] = (uint8_t)st->active_stream_kind;
    st->stream_count++;
    st->stream_accepted++;
    const char *decoder_outcome = "not-run";
    if (st->active_stream_kind == XBUS_AUDIO_STREAM_SAMPLED) {
        decoder_outcome = stream_wav_packet(
            s, st, st->stream_data[tail], length);
    }
    else if (st->active_stream_kind == XBUS_AUDIO_STREAM_SI3)
        decoder_outcome = stream_si3_packet(
            s, st, st->stream_data[tail], length);
    set_ram_word(s, XBUS_AUDIO_STREAM_BASE, AUDIO_ACCEPTED);
    st->stream_drain_deadline = s->ticks + 1u;
    emit_stream_packet(s, st, phase, "accepted", decoder_outcome,
                       st->active_stream_kind, st->stream_data[tail], length,
                       old_token, AUDIO_ACCEPTED);
}

static void stream_finish_write(soc_t *s, xbus_audio_state_t *st) {
    uint32_t length = st->stream_max_written - XBUS_AUDIO_STREAM_BASE + 1u;
    uint16_t old_token = ram_word(s, XBUS_AUDIO_STREAM_BASE);
    uint8_t payload[XBUS_AUDIO_PACKET_MAX];
    for (uint32_t i = 0; i < length && i < sizeof payload; i++)
        payload[i] = memory_controller_ram_get(
            &s->memory, XBUS_AUDIO_STREAM_BASE + i);
    st->stream_collecting = 0;
    st->stream_deadline = 0;
    st->stream_packets++;

    if (st->active_stream_kind == XBUS_AUDIO_STREAM_UNCLASSIFIED) {
        const char *reason = st->unclassified_pending
                           ? "replaced-unclassified" : "awaiting-command";
        if (st->unclassified_pending) st->stream_replaced++;
        memcpy(st->unclassified_data, payload, length);
        st->unclassified_length = (uint8_t)length;
        st->unclassified_pending = 1;
        emit_stream_packet(s, st, "settled", reason, "not-run",
                           XBUS_AUDIO_STREAM_UNCLASSIFIED, payload, length,
                           old_token, old_token);
        return;
    }
    stream_accept_packet(s, st, payload, length, old_token, "settled");
}

static void stream_drain_one(xbus_audio_state_t *st) {
    if (!st->stream_count) {
        st->stream_drain_deadline = 0;
        return;
    }
    st->stream_head = (uint8_t)((st->stream_head + 1u) %
                                XBUS_AUDIO_QUEUE_CAP);
    st->stream_count--;
    st->stream_drained++;
    st->stream_drain_deadline = st->stream_count ? 1u : 0u;
}

int cemu_xbus_audio_handles(const xbus_audio_state_t *state, uint32_t addr) {
    return stream_profile_enabled(state) &&
           ((addr >= XBUS_AUDIO_STREAM_BASE && addr <= XBUS_AUDIO_STREAM_END) ||
            (addr >= XBUS_AUDIO_COMMAND && addr <= XBUS_AUDIO_COMMAND + 5u) ||
            addr == 0xEF3Au);
}

void cemu_xbus_audio_init(xbus_audio_state_t *state, int enabled) {
    if (!state) return;
    memset(state, 0, sizeof *state);
    state->enabled = enabled != 0;
}

int cemu_xbus_audio_write8(xbus_audio_state_t *st, soc_t *s, uint32_t addr) {
    if (!stream_profile_enabled(st)) return 0;
    if (addr >= XBUS_AUDIO_STREAM_BASE && addr <= XBUS_AUDIO_STREAM_END) {
        if (addr == XBUS_AUDIO_STREAM_BASE) {
            if (ram_word(s, XBUS_AUDIO_STREAM_BASE) == 0u) return 0;
            st->stream_collecting = 1;
            st->stream_max_written = addr;
        } else if (!st->stream_collecting) {
            if (!st->unclassified_pending) return 0;
            /* An interrupt can outlast settlement while the packet is unclaimed. */
            uint32_t end = (addr & 1u) == 0u && addr < XBUS_AUDIO_STREAM_END
                         ? addr + 1u : addr;
            for (uint32_t a = addr; a <= end; a++)
                st->unclassified_data[a - XBUS_AUDIO_STREAM_BASE] =
                    memory_controller_ram_get(&s->memory, a);
            uint32_t length = end - XBUS_AUDIO_STREAM_BASE + 1u;
            if (length > st->unclassified_length)
                st->unclassified_length = (uint8_t)length;
            emit_stream_packet(s, st, "retained", "continued-write", "not-run",
                               XBUS_AUDIO_STREAM_UNCLASSIFIED,
                               st->unclassified_data, st->unclassified_length,
                               ram_word(s, XBUS_AUDIO_STREAM_BASE),
                               ram_word(s, XBUS_AUDIO_STREAM_BASE));
            return 1;
        }
        uint32_t written_end = (addr & 1u) == 0u &&
                               addr < XBUS_AUDIO_STREAM_END
                             ? addr + 1u : addr;
        if (written_end > st->stream_max_written)
            st->stream_max_written = written_end;
        st->stream_deadline = s->ticks + AUDIO_SETTLE_TICKS;
        return 1;
    }
    if (addr == XBUS_AUDIO_COMMAND) {
        st->command_pending = 1;
        return 1;
    }
    if (addr != 0xEF3Au || !(ram_word(s, addr) & 4u) ||
        !st->command_pending) return 0;
    uint16_t command = ram_word(s, XBUS_AUDIO_COMMAND);
    st->command_pending = 0;
    if (command < 0x10u || command > 0x23u) return 0;
    /* Firmware writes command and arguments before the EF3A notification. */
    uint16_t argument = ram_word(s, XBUS_AUDIO_COMMAND + 2u);
    st->stream_command = command;
    if (command == 0x14u) {
        cemu_audio_cancel_pcm(&s->audio, CEMU_AUDIO_SOURCE_XBUS,
                              s->ticks, 0);
        cemu_ima_adpcm_stream_reset(&st->decoded_audio);
        cemu_si3_reset(&st->si3);
        st->decoded_audio.sample_rate = argument == 1u ? 8000u :
                                         argument == 2u ? 16000u : 0u;
        st->decoded_audio.active = st->decoded_audio.sample_rate != 0;
        st->active_stream_kind = XBUS_AUDIO_STREAM_SAMPLED;
    } else if (command == 0x15u) {
        if (st->stream_collecting) stream_finish_write(s, st);
        const char *outcome = st->decoded_audio.failed ? "failed" : "delivered-only";
        cemu_audio_drain_pcm(&s->audio, CEMU_AUDIO_SOURCE_XBUS, s->ticks);
        emit_stream_packet(s, st, "command", "sampled-drain", outcome,
                           st->active_stream_kind, NULL, 0, 0, 0);
        if (st->active_stream_kind == XBUS_AUDIO_STREAM_SAMPLED)
            stream_sampled_retire(st);
    } else if (command == 0x16u) {
        cemu_audio_cancel_pcm(&s->audio, CEMU_AUDIO_SOURCE_XBUS,
                              s->ticks, 0);
        cemu_ima_adpcm_stream_reset(&st->decoded_audio);
        cemu_si3_reset(&st->si3);
        st->active_stream_kind = XBUS_AUDIO_STREAM_SI3;
    } else if (command == 0x17u) {
        stream_cancel(st);
        cemu_audio_cancel_pcm(&s->audio, CEMU_AUDIO_SOURCE_XBUS,
                              s->ticks, 1);
        st->stream_command = command;
        emit_stream_packet(s, st, "command", "cancelled", "reset",
                           XBUS_AUDIO_STREAM_UNCLASSIFIED,
                           NULL, 0, 0, 0);
    }
    if ((command == 0x14u || command == 0x16u) &&
        st->unclassified_pending) {
        uint8_t length = st->unclassified_length;
        uint16_t token = length >= 2u
            ? (uint16_t)(st->unclassified_data[0] |
                ((uint16_t)st->unclassified_data[1] << 8)) : 0;
        st->unclassified_pending = 0;
        st->unclassified_length = 0;
        stream_accept_packet(s, st, st->unclassified_data, length,
                             token, "command-classified");

    }
    set_ram_word(s, XBUS_AUDIO_COMMAND, command | 0x8000u);
    uint8_t arguments[4];
    for (unsigned i = 0; i < sizeof arguments; i++)
        arguments[i] = memory_controller_ram_get(&s->memory, XBUS_AUDIO_COMMAND + 2u + i);
    emit_stream_packet(s, st, "submitted", "ef3a", "ack",
                       st->active_stream_kind, arguments, sizeof arguments,
                       command, command | 0x8000u);
    return 1;
}

void cemu_xbus_audio_tick(xbus_audio_state_t *st, soc_t *s) {
    if (!st || !s) return;
    if (st->stream_collecting && s->ticks >= st->stream_deadline)
        stream_finish_write(s, st);
    else if (st->stream_drain_deadline &&
             s->ticks >= st->stream_drain_deadline) {
        stream_drain_one(st);
        if (st->stream_drain_deadline)
            st->stream_drain_deadline = s->ticks + 1u;
    }
}

uint64_t cemu_xbus_audio_next_event(const xbus_audio_state_t *st,
                                    const soc_t *s) {
    uint64_t next = UINT64_MAX;
    if (!st || !s) return next;
    if (st->stream_collecting)
        next = st->stream_deadline > s->ticks
             ? st->stream_deadline - s->ticks : 1u;
    if (st->stream_drain_deadline) {
        uint64_t drain = st->stream_drain_deadline > s->ticks
                       ? st->stream_drain_deadline - s->ticks : 1u;
        if (drain < next) next = drain;
    }
    return next;
}

void cemu_xbus_audio_reset(xbus_audio_state_t *st, soc_t *s, int notify) {
    if (!st) return;
    stream_cancel(st);
    st->stream_command = 0;
    st->command_pending = 0;
    if (s) cemu_audio_cancel_pcm(&s->audio, CEMU_AUDIO_SOURCE_XBUS,
                                 s->ticks, notify);
}
