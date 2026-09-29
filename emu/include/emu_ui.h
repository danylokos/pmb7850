#ifndef EMU_UI_H
#define EMU_UI_H

#include <stddef.h>
#include <stdint.h>

#include "emu_engine.h"
#include "emu_input.h"
#include "emu_serial.h"
#include "emu_runtime_state.h"

#define EMU_UI_MAGIC 0x55353543u
#define EMU_UI_VERSION 13u
#define EMU_UI_HEADER_SIZE 48u
#define EMU_UI_MODEL_MAX 31u
#define EMU_UI_KEY_NAME_MAX 31u
#define EMU_UI_MAX_KEYS 32u
#define EMU_UI_MAX_FRAME_SIZE (128u * 162u * 3u)
#define EMU_UI_MAX_PAYLOAD EMU_UI_MAX_FRAME_SIZE
#define EMU_UI_RX_CAP (EMU_UI_HEADER_SIZE + EMU_UI_MAX_PAYLOAD)

enum emu_ui_message_type {
    EMU_UI_HELLO = 1,
    EMU_UI_FRAME = 2,
    EMU_UI_STATS = 3,
    EMU_UI_KEY = 4,
    EMU_UI_RELEASE_ALL = 5,
    EMU_UI_KEY_RELEASE_AFTER_SAMPLE = 6,
    EMU_UI_ERROR = 7,
    EMU_UI_ASC0_OPEN = 8,
    EMU_UI_ASC0_READY = 9,
    EMU_UI_ASC0_RX = 10,
    EMU_UI_ASC0_TX = 11,
    EMU_UI_ASC0_CLOSE = 12,
    EMU_UI_AUDIO_RESET = 13,
    EMU_UI_SPEAKER_PCM = 14,
    EMU_UI_LIFECYCLE = 15,
    EMU_UI_OWNER_OPEN = 16,
    EMU_UI_OWNER_CLOSE = 17,
    EMU_UI_OWNER_RELEASE_ALL = 18,
    EMU_UI_OWNER_KEY = 19,
    EMU_UI_ASC0_SUBSCRIBED = 20,
    EMU_UI_ASC0_HISTORY_READ = 21,
    EMU_UI_ASC0_HISTORY_DATA = 22,
};

#define EMU_UI_CAP_ASC0 0x0001u
#define EMU_UI_CAP_AUDIO 0x0002u

typedef emu_runtime_snapshot_t emu_ui_stats_t;

typedef struct {
    void *opaque;
    size_t (*key_count)(void *);
    const char *(*key_name)(void *, size_t);
    int (*key_set)(void *, const char *, int);
    int (*key_sampled)(void *, const char *, int *);
    int (*serial_rx)(void *, const uint8_t *, size_t);
    int (*serial_link)(void *, int);
} emu_ui_engine_t;

/* Standard binding for a host session.  The key list is the stable product
 * order advertised on the protocol wire; transport callbacks remain confined
 * to explicit session calls. */
typedef struct {
    emu_session_t *session;
    const char *const *keys;
    size_t key_count;
} emu_ui_session_binding_t;

typedef struct emu_ui_audio_packet emu_ui_audio_packet_t;

typedef struct {
    int listen_fd;
    int client_fd;
    char path[108];
    char model[EMU_UI_MODEL_MAX + 1u];
    uint8_t rx[EMU_UI_RX_CAP];
    size_t rx_len;
    uint8_t tx[EMU_UI_HEADER_SIZE + EMU_UI_MAX_PAYLOAD];
    size_t tx_len;
    size_t tx_off;
    uint16_t tx_type;
    uint8_t latest_frame[EMU_UI_MAX_FRAME_SIZE];
    uint8_t last_render[EMU_UI_MAX_FRAME_SIZE];
    unsigned frame_width;
    unsigned frame_height;
    size_t frame_size;
    int latest_frame_pending;
    int last_render_valid;
    int need_hello;
    int need_stats;
    int need_lifecycle;
    uint8_t run_id[16];
    emu_runtime_state_t *runtime;
    emu_runtime_descriptor_t descriptor;
    uint64_t drain_deadline;
    emu_runtime_lifecycle_t lifecycle;
    uint64_t sent_sample;
    emu_serial_history_t *serial;
    emu_serial_subscription_t subscription;
    uint64_t asc0_ready_after;
    uint64_t history_request, history_start, history_end;
    int need_subscribed, history_pending, replay_turn, serial_failed;
    int asc0_available;
    int audio_available;
    int asc0_open_pending;
    int asc0_ready;
    emu_input_t *input;
    emu_input_handle_t input_scope, implicit_owner;
    struct emu_ui_owner *owners;
    int input_failed;
    uint64_t next_sequence;
    uint64_t frame_messages;
    uint64_t asc0_tx_messages;
    uint64_t audio_messages;
    uint64_t audio_dropped_bytes;
    size_t audio_queued_bytes;
    int audio_reset_pending;
    emu_ui_audio_packet_t *audio_head;
    emu_ui_audio_packet_t *audio_tail;
    uint64_t connections;
    emu_ui_engine_t engine;
} emu_ui_socket_t;

int emu_ui_socket_open(emu_ui_socket_t *, const char *, const char *,
                       int, int, const emu_ui_engine_t *);
void emu_ui_socket_bind_input(emu_ui_socket_t *, emu_input_t *);
/* The caller owns the runtime and must bind it before polling. Supply its
 * published snapshots to poll/drain; the socket has no sampling policy.
 * Unbound polling fails with EINVAL; unbound draining is a no-op. Close the
 * socket before destroying its bound services. Partial initialization may close. */
void emu_ui_socket_bind_runtime(emu_ui_socket_t *, emu_runtime_state_t *);
void emu_ui_socket_drain(emu_ui_socket_t *, const emu_ui_stats_t *);
void emu_ui_socket_stop_input(emu_ui_socket_t *);
void emu_ui_socket_close(emu_ui_socket_t *);
int emu_ui_socket_poll(emu_ui_socket_t *, const emu_ui_stats_t *, int);
int emu_ui_socket_frame(emu_ui_socket_t *, unsigned, unsigned,
                        const uint8_t *, size_t, uint64_t);
void emu_ui_socket_bind_serial(emu_ui_socket_t *, emu_serial_history_t *);
int emu_ui_socket_speaker_pcm(emu_ui_socket_t *, const emu_audio_output_t *);
void emu_ui_socket_audio_reset(emu_ui_socket_t *, uint64_t);
int emu_ui_bind_session(emu_ui_engine_t *, emu_ui_session_binding_t *);

#endif
