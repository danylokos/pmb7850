#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include "emu_ui.h"

#define EMU_UI_AUDIO_QUEUE_MAX (2u * 1024u * 1024u)
#define EMU_UI_AUDIO_PREFIX_SIZE 8u

struct emu_ui_audio_packet {
    struct emu_ui_audio_packet *next;
    uint16_t type;
    uint64_t icount;
    uint32_t size;
    uint8_t data[];
};

static size_t session_key_count(void *opaque) {
    return ((emu_ui_session_binding_t *)opaque)->key_count;
}

static const char *session_key_name(void *opaque, size_t index) {
    emu_ui_session_binding_t *binding = opaque;
    return index < binding->key_count ? binding->keys[index] : NULL;
}

static int session_key_set(void *opaque, const char *name, int pressed) {
    emu_ui_session_binding_t *binding = opaque;
    emu_error_t error = {0};
    return emu_session_key(binding->session, name, pressed, &error) == EMU_OK
         ? 0 : -1;
}

static int session_key_sampled(void *opaque, const char *name, int *sampled) {
    emu_ui_session_binding_t *binding = opaque;
    emu_key_sample_t sample;
    emu_error_t error = {0};
    if (!sampled || emu_session_key_query(binding->session, name, &sample,
                                           &error) != EMU_OK)
        return -1;
    *sampled = sample.sampled_pressed;
    return 0;
}

static int session_serial_rx(void *opaque, const uint8_t *bytes, size_t size) {
    emu_ui_session_binding_t *binding = opaque;
    emu_error_t error = {0};
    return emu_session_serial_rx(binding->session, bytes, size, &error) == EMU_OK
         ? 0 : -1;
}

static int session_serial_link(void *opaque, int attached) {
    emu_ui_session_binding_t *binding = opaque;
    emu_serial_link_state_t state;
    emu_error_t error = {0};
    return emu_session_serial_link(
               binding->session,
               attached ? EMU_SERIAL_LINK_UI : EMU_SERIAL_LINK_DETACHED,
               &state, &error) == EMU_OK ? 0 : -1;
}

int emu_ui_bind_session(emu_ui_engine_t *engine,
                        emu_ui_session_binding_t *binding) {
    if (!engine || !binding || !binding->session || !binding->keys ||
        !binding->key_count || binding->key_count > EMU_UI_MAX_KEYS)
        return -1;
    *engine = (emu_ui_engine_t){
        .opaque = binding,
        .key_count = session_key_count,
        .key_name = session_key_name,
        .key_set = session_key_set,
        .key_sampled = session_key_sampled,
        .serial_rx = session_serial_rx,
        .serial_link = session_serial_link,
    };
    return 0;
}

static void put16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v) {
    for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i));
}

static void put64(uint8_t *p, uint64_t v) {
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i));
}

static uint16_t get16(const uint8_t *p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t get32(const uint8_t *p) {
    uint32_t v = 0;
    for (int i = 3; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}

static void clear_audio(emu_ui_socket_t *ui) {
    while (ui->audio_head) {
        emu_ui_audio_packet_t *packet = ui->audio_head;
        ui->audio_head = packet->next;
        free(packet);
    }
    ui->audio_tail = NULL;
    ui->audio_queued_bytes = 0;
}

static int append_audio_packet(emu_ui_socket_t *ui, uint16_t type,
                               uint64_t icount, const uint8_t *data,
                               uint32_t size) {
    if (!ui || !ui->audio_available || !data || !size ||
        size > EMU_UI_MAX_PAYLOAD)
        return -1;
    emu_ui_audio_packet_t *packet = malloc(sizeof *packet + size);
    if (!packet) return -1;
    *packet = (emu_ui_audio_packet_t){
        .type = type, .icount = icount, .size = size,
    };
    memcpy(packet->data, data, size);
    while (ui->audio_head &&
           ui->audio_queued_bytes + size > EMU_UI_AUDIO_QUEUE_MAX) {
        emu_ui_audio_packet_t *dropped = ui->audio_head;
        ui->audio_head = dropped->next;
        if (!ui->audio_head) ui->audio_tail = NULL;
        ui->audio_queued_bytes -= dropped->size;
        ui->audio_dropped_bytes += dropped->size;
        free(dropped);
    }
    if (ui->audio_queued_bytes + size > EMU_UI_AUDIO_QUEUE_MAX) {
        ui->audio_dropped_bytes += size;
        free(packet);
        return 0;
    }
    if (ui->audio_tail) ui->audio_tail->next = packet;
    else ui->audio_head = packet;
    ui->audio_tail = packet;
    ui->audio_queued_bytes += size;
    return 0;
}

static void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) (void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int fill_unix_addr(struct sockaddr_un *addr, const char *path,
                          socklen_t *addr_len) {
    size_t path_len = path ? strlen(path) : 0;
    if (path_len == 0 || path_len >= sizeof addr->sun_path) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memset(addr, 0, sizeof *addr);
    addr->sun_family = AF_UNIX;
    memcpy(addr->sun_path, path, path_len + 1);
    *addr_len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) +
                            path_len + 1);
#ifdef __APPLE__
    addr->sun_len = (uint8_t)*addr_len;
#endif
    return 0;
}

static size_t key_count(const emu_ui_socket_t *ui) {
    return ui->descriptor.key_count;
}

static const char *key_name(const emu_ui_socket_t *ui, size_t index) {
    return index < ui->descriptor.key_count ? ui->descriptor.keys[index] : NULL;
}

struct emu_ui_owner {
    struct emu_ui_owner *next;
    uint64_t token;
    emu_input_handle_t handle;
};

static int release_keys(emu_ui_socket_t *ui) {
    int rc = ui->input_scope ? emu_input_scope_release_all(ui->input, ui->input_scope) : 0;
    if (rc) ui->input_failed = 1;
    return rc;
}

static void close_input_scope(emu_ui_socket_t *ui) {
    if (ui->input_scope && emu_input_scope_close(ui->input, ui->input_scope))
        ui->input_failed = 1;
    ui->input_scope = ui->implicit_owner = 0;
    while (ui->owners) {
        struct emu_ui_owner *o = ui->owners;
        ui->owners = o->next;
        free(o);
    }
}

void emu_ui_socket_bind_input(emu_ui_socket_t *ui, emu_input_t *input) {
    ui->input = input;
}

static void disconnect_client(emu_ui_socket_t *ui) {
    if (ui->client_fd >= 0) close(ui->client_fd);
    ui->client_fd = -1;
    ui->rx_len = 0;
    ui->tx_len = ui->tx_off = 0;
    ui->tx_type = 0;
    ui->latest_frame_pending = 0;
    ui->need_hello = ui->need_stats = 0;
    if (ui->subscription) emu_serial_history_close(ui->serial, ui->subscription);
    ui->subscription = 0;
    ui->need_subscribed = ui->history_pending = ui->replay_turn = 0;
    ui->history_request = 0;
    ui->asc0_ready_after = 0;
    ui->asc0_open_pending = 0;
    ui->asc0_ready = 0;
    clear_audio(ui);
    ui->audio_reset_pending = 0;
    close_input_scope(ui);
}

static void make_packet(emu_ui_socket_t *ui, uint16_t type, const void *payload,
                        uint32_t payload_len, uint64_t icount) {
    uint8_t *h = ui->tx;
    put32(h + 0, EMU_UI_MAGIC);
    put16(h + 4, EMU_UI_VERSION);
    put16(h + 6, type);
    put32(h + 8, payload_len);
    put64(h + 12, ui->next_sequence++);
    put64(h + 20, icount);
    put32(h + 28, 0);
    memcpy(h + 32, ui->run_id, 16);
    if (payload_len) memcpy(h + EMU_UI_HEADER_SIZE, payload, payload_len);
    ui->tx_len = EMU_UI_HEADER_SIZE + payload_len;
    ui->tx_off = 0;
    ui->tx_type = type;
}

static uint64_t get64(const uint8_t *p) {
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; i++) value |= (uint64_t)p[i] << (8 * i);
    return value;
}

static int serial_pending(emu_ui_socket_t *ui) {
    uint64_t cursor, tail;
    return ui->asc0_available && ui->subscription &&
        !emu_serial_history_bounds(ui->serial, ui->subscription, NULL, &cursor, &tail) && cursor < tail;
}

/* The retained packet owns its bytes before advancing the live subscription.
 * OPEN's boundary takes precedence over replay, including its READY barrier. */
static int queue_serial(emu_ui_socket_t *ui, const emu_ui_stats_t *stats) {
    if (!ui->asc0_available || !ui->subscription) return 0;
    uint64_t cursor, tail;
    if (emu_serial_history_bounds(ui->serial, ui->subscription, NULL, &cursor, &tail)) {
        ui->serial_failed = 1;
        return 0;
    }
    uint8_t payload[EMU_UI_MAX_PAYLOAD];
    if (ui->asc0_open_pending && cursor >= ui->asc0_ready_after) {
        put64(payload, ui->subscription);
        put64(payload + 8, ui->asc0_ready_after);
        make_packet(ui, EMU_UI_ASC0_READY, payload, 16, stats->icount);
        ui->asc0_open_pending = 0;
        ui->asc0_ready = 1;
        return 1;
    }
    emu_serial_view_t view;
    if (!ui->asc0_open_pending && ui->history_pending && (ui->replay_turn || cursor == tail)) {
        if (emu_serial_history_read(ui->serial, ui->subscription,
                ui->history_start, ui->history_end, EMU_UI_MAX_PAYLOAD - 32, &view)) {
            ui->serial_failed = 1;
            return 0;
        }
        put64(payload, ui->subscription); put64(payload + 8, ui->history_request);
        put64(payload + 16, view.start); put64(payload + 24, view.next);
        if (view.size) memcpy(payload + 32, view.data, view.size);
        make_packet(ui, EMU_UI_ASC0_HISTORY_DATA, payload, 32 + view.size, stats->icount);
        /* Outstanding until the retained response has actually been sent. */
        ui->history_pending = 2;
        ui->replay_turn = 0;
        return 1;
    }
    if (cursor < tail) {
        uint64_t end = ui->asc0_open_pending ? ui->asc0_ready_after : tail;
        if (emu_serial_history_read(ui->serial, ui->subscription, cursor, end,
                                    EMU_UI_MAX_PAYLOAD - 16, &view)) {
            ui->serial_failed = 1;
            return 0;
        }
        put64(payload, ui->subscription); put64(payload + 8, view.start);
        memcpy(payload + 16, view.data, view.size);
        make_packet(ui, EMU_UI_ASC0_TX, payload, 16 + view.size, stats->icount);
        if (emu_serial_history_advance(ui->serial, ui->subscription, view.size)) ui->serial_failed = 1;
        ui->asc0_tx_messages++;
        ui->replay_turn = 1;
        return 1;
    }
    return 0;
}

static void queue_next(emu_ui_socket_t *ui, const emu_ui_stats_t *stats) {
    if (ui->tx_len != ui->tx_off) return;
    if (ui->tx_type == EMU_UI_ASC0_HISTORY_DATA && ui->history_pending == 2)
        ui->history_pending = 0;
    ui->tx_len = ui->tx_off = 0;
    if (ui->need_hello) {
        uint8_t payload[9 + EMU_UI_MODEL_MAX +
                        EMU_UI_MAX_KEYS * (1 + EMU_UI_KEY_NAME_MAX)];
        size_t model_len = strlen(ui->model);
        size_t count = key_count(ui);
        if (count == 0 || count > EMU_UI_MAX_KEYS) return;
        put16(payload + 0, (uint16_t)ui->frame_width);
        put16(payload + 2, (uint16_t)ui->frame_height);
        put16(payload + 4, (uint16_t)count);
        put16(payload + 6,
              (ui->asc0_available ? EMU_UI_CAP_ASC0 : 0) |
              (ui->audio_available ? EMU_UI_CAP_AUDIO : 0));
        payload[8] = (uint8_t)model_len;
        size_t payload_len = 9;
        memcpy(payload + payload_len, ui->model, model_len);
        payload_len += model_len;
        for (size_t i = 0; i < count; i++) {
            const char *name = key_name(ui, i);
            size_t name_len = name ? strlen(name) : 0;
            if (!name_len || name_len > EMU_UI_KEY_NAME_MAX) return;
            payload[payload_len++] = (uint8_t)name_len;
            memcpy(payload + payload_len, name, name_len);
            payload_len += name_len;
        }
        ui->need_hello = 0;
        make_packet(ui, EMU_UI_HELLO, payload, (uint32_t)payload_len,
                    stats->icount);
    } else if (ui->need_lifecycle) {
        uint8_t payload[740];
        size_t status_len = strlen(ui->lifecycle.status);
        size_t reason_len = strlen(ui->lifecycle.reason);
        payload[0] = (uint8_t)ui->lifecycle.phase;
        payload[1] = (uint8_t)status_len;
        put16(payload + 2, (uint16_t)reason_len);
        memcpy(payload + 4, ui->lifecycle.status, status_len);
        memcpy(payload + 4 + status_len, ui->lifecycle.reason, reason_len);
        ui->need_lifecycle = 0;
        make_packet(ui, EMU_UI_LIFECYCLE, payload,
                    (uint32_t)(4 + status_len + reason_len), stats->icount);
    } else if (ui->need_stats || stats->sample_sequence != ui->sent_sample) {
        uint8_t payload[72];
        put64(payload + 0, stats->elapsed_ns);
        put64(payload + 8, stats->ticks);
        put64(payload + 16, stats->guest_instructions);
        put32(payload + 24, stats->pc);
        put64(payload + 28, stats->sample_sequence);
        put64(payload + 36, stats->measured_ns);
        put64(payload + 44, stats->window_ns);
        _Static_assert(sizeof(double) == 8, "UI requires binary64 double");
        double tick_rate = stats->rates_valid ? stats->ticks_per_s : 0;
        double guest_rate = stats->rates_valid ? stats->guest_instructions_per_s : 0;
        uint64_t bits;
        memcpy(&bits, &tick_rate, 8); put64(payload + 52, bits);
        memcpy(&bits, &guest_rate, 8); put64(payload + 60, bits);
        put32(payload + 68, stats->rates_valid);
        ui->sent_sample = stats->sample_sequence;
        ui->need_stats = 0;
        make_packet(ui, EMU_UI_STATS, payload, sizeof payload, stats->icount);
    } else if (ui->need_subscribed) {
        uint8_t payload[16];
        uint64_t boundary;
        if (emu_serial_history_bounds(ui->serial, ui->subscription, &boundary, NULL, NULL)) {
            ui->serial_failed = 1;
            return;
        }
        put64(payload, ui->subscription); put64(payload + 8, boundary);
        ui->need_subscribed = 0;
        make_packet(ui, EMU_UI_ASC0_SUBSCRIBED, payload, sizeof payload, stats->icount);
    } else if (ui->audio_reset_pending) {
        ui->audio_reset_pending = 0;
        make_packet(ui, EMU_UI_AUDIO_RESET, NULL, 0, stats->icount);
    } else if (ui->audio_head) {
        emu_ui_audio_packet_t *packet = ui->audio_head;
        make_packet(ui, packet->type, packet->data, packet->size,
                    packet->icount);
        ui->audio_head = packet->next;
        if (!ui->audio_head) ui->audio_tail = NULL;
        ui->audio_queued_bytes -= packet->size;
        ui->audio_messages++;
        free(packet);
    } else if (queue_serial(ui, stats)) {
        /* Serial occupies the same scheduling position as before. */
    } else if (ui->latest_frame_pending) {
        ui->latest_frame_pending = 0;
        ui->frame_messages++;
        make_packet(ui, EMU_UI_FRAME, ui->latest_frame,
                    (uint32_t)ui->frame_size, stats->icount);
    }
}

static void flush_tx(emu_ui_socket_t *ui, const emu_ui_stats_t *stats) {
    for (;;) {
        if (ui->drain_deadline && emu_runtime_monotonic_ns() >= ui->drain_deadline) return;
        queue_next(ui, stats);
        if (ui->tx_len == ui->tx_off) return;
#ifdef MSG_NOSIGNAL
        ssize_t n = send(ui->client_fd, ui->tx + ui->tx_off,
                         ui->tx_len - ui->tx_off, MSG_NOSIGNAL);
#else
        ssize_t n = send(ui->client_fd, ui->tx + ui->tx_off,
                         ui->tx_len - ui->tx_off, 0);
#endif
        if (n > 0) {
            ui->tx_off += (size_t)n;
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        disconnect_client(ui);
        return;
    }
}

static int handle_message(emu_ui_socket_t *ui, uint16_t type,
                          const uint8_t *payload, uint32_t len) {
    size_t count = key_count(ui);
    if (type >= EMU_UI_OWNER_OPEN && type <= EMU_UI_OWNER_KEY) {
        if (len != (type == EMU_UI_OWNER_KEY ? 10u : 8u)) return -1;
        uint64_t token = 0;
        for (int i = 7; i >= 0; i--) token = (token << 8) | payload[i];
        struct emu_ui_owner **link = &ui->owners;
        while (*link && (*link)->token != token) link = &(*link)->next;
        struct emu_ui_owner *o = *link;
        if (type == EMU_UI_OWNER_OPEN) {
            if (o) return 0;
            o = calloc(1, sizeof *o);
            if (!o) return -1;
            o->handle = emu_input_owner_open(ui->input, ui->input_scope);
            if (!o->handle) { free(o); return -1; }
            o->token = token; *link = o;
            return 0;
        }
        if (!o) return -1;
        if (type == EMU_UI_OWNER_KEY) {
            if (payload[8] >= count || payload[9] > EMU_INPUT_RELEASE_SAMPLED) return -1;
            return emu_input_key(ui->input, o->handle, payload[8], payload[9]);
        }
        if (type == EMU_UI_OWNER_RELEASE_ALL)
            return emu_input_owner_release_all(ui->input, o->handle);
        int rc = emu_input_owner_close(ui->input, o->handle);
        if (!rc) { *link = o->next; free(o); }
        return rc;
    }
    if (type == EMU_UI_KEY) {
        if (len != 2 || payload[0] >= count || payload[1] > 1) return -1;
        return emu_input_key(ui->input, ui->implicit_owner, payload[0], payload[1]);
    }
    if (type == EMU_UI_KEY_RELEASE_AFTER_SAMPLE) {
        if (len != 1 || payload[0] >= count) return -1;
        return emu_input_key(ui->input, ui->implicit_owner, payload[0], EMU_INPUT_RELEASE_SAMPLED);
    }
    if (type == EMU_UI_RELEASE_ALL) {
        if (len != 0) return -1;
        return release_keys(ui);
    }
    if (type == EMU_UI_ASC0_HISTORY_READ) {
        if (len != 32 || !ui->subscription || ui->history_pending) return -1;
        uint64_t id = get64(payload), request = get64(payload + 8);
        uint64_t start = get64(payload + 16), end = get64(payload + 24);
        emu_serial_view_t view;
        if (id != ui->subscription || !request || request <= ui->history_request ||
            emu_serial_history_read(ui->serial, id, start, end, 0, &view)) return -1;
        ui->history_request = request; ui->history_start = start; ui->history_end = end;
        ui->history_pending = 1;
        return 0;
    }
    if (type == EMU_UI_ASC0_OPEN) {
        if (len != 0 || !ui->asc0_available) return -1;
        ui->asc0_ready = 0;
        ui->asc0_open_pending = 1;
        ui->asc0_ready_after = emu_serial_history_tail(ui->serial);
        if (ui->engine.serial_link &&
            ui->engine.serial_link(ui->engine.opaque, 1)) return -1;
        return 0;
    }
    if (type == EMU_UI_ASC0_RX) {
        if (!ui->asc0_available || !ui->asc0_ready ||
            ui->asc0_open_pending || len == 0) return -1;
        return ui->engine.serial_rx
             ? ui->engine.serial_rx(ui->engine.opaque, payload, len) : -1;
    }
    if (type == EMU_UI_ASC0_CLOSE) {
        if (len != 0 || !ui->asc0_available) return -1;
        ui->asc0_ready = 0;
        ui->asc0_open_pending = 0;
        if (ui->engine.serial_link &&
            ui->engine.serial_link(ui->engine.opaque, 0)) return -1;
        return 0;
    }
    return -1;
}

static void send_protocol_error(emu_ui_socket_t *ui, uint64_t icount) {
    static const char message[] = "invalid protocol message";
    if (ui->client_fd < 0 || (ui->tx_off && ui->tx_off < ui->tx_len)) return;
    make_packet(ui, EMU_UI_ERROR, message, sizeof message - 1u, icount);
#ifdef MSG_NOSIGNAL
    (void)send(ui->client_fd, ui->tx, ui->tx_len, MSG_NOSIGNAL);
#else
    (void)send(ui->client_fd, ui->tx, ui->tx_len, 0);
#endif
}

static int consume_rx(emu_ui_socket_t *ui) {
    size_t used = 0;
    while (ui->rx_len - used >= EMU_UI_HEADER_SIZE) {
        const uint8_t *h = ui->rx + used;
        uint32_t len = get32(h + 8);
        if (get32(h + 0) != EMU_UI_MAGIC ||
            get16(h + 4) != EMU_UI_VERSION || get32(h + 28) != 0 ||
            memcmp(h + 32, ui->run_id, 16) ||
            len > EMU_UI_MAX_PAYLOAD)
            return -1;
        size_t total = EMU_UI_HEADER_SIZE + (size_t)len;
        if (ui->rx_len - used < total) break;
        if (handle_message(ui, get16(h + 6),
                           h + EMU_UI_HEADER_SIZE, len) != 0)
            return -1;
        used += total;
    }
    if (used) {
        memmove(ui->rx, ui->rx + used, ui->rx_len - used);
        ui->rx_len -= used;
    }
    return 0;
}

static void read_rx(emu_ui_socket_t *ui, uint64_t icount) {
    for (;;) {
        if (ui->rx_len == sizeof ui->rx) {
            disconnect_client(ui);
            return;
        }
        ssize_t n = recv(ui->client_fd, ui->rx + ui->rx_len,
                         sizeof ui->rx - ui->rx_len, 0);
        if (n > 0) {
            ui->rx_len += (size_t)n;
            if (consume_rx(ui) != 0) {
                send_protocol_error(ui, icount);
                disconnect_client(ui);
                return;
            }
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        disconnect_client(ui);
        return;
    }
}

static void accept_client(emu_ui_socket_t *ui) {
    int fd = accept(ui->listen_fd, NULL, NULL);
    if (fd < 0) return;
    set_nonblocking(fd);
    if (ui->client_fd >= 0) disconnect_client(ui);
    ui->input_scope = emu_input_scope_open(ui->input);
    ui->implicit_owner = emu_input_owner_open(ui->input, ui->input_scope);
    if (!ui->implicit_owner) {
        close(fd);
        close_input_scope(ui);
        ui->input_failed = 1;
        return;
    }
    ui->client_fd = fd;
    ui->rx_len = ui->tx_len = ui->tx_off = 0;
    ui->tx_type = 0;
    ui->need_hello = 1;
    ui->need_stats = 1;
    ui->need_lifecycle = 1;
    if (ui->asc0_available) {
        ui->subscription = emu_serial_history_subscribe_live(ui->serial);
        if (!ui->subscription) {
            ui->serial_failed = 1;
            disconnect_client(ui);
            return;
        }
        ui->need_subscribed = 1;
    }
    ui->asc0_ready_after = emu_serial_history_tail(ui->serial);
    ui->asc0_open_pending = 0;
    ui->asc0_ready = 0;
    ui->latest_frame_pending = ui->last_render_valid;
    ui->audio_reset_pending = ui->audio_available;
    ui->connections++;
}

int emu_ui_socket_open(emu_ui_socket_t *ui, const char *path, const char *model,
                       int asc0_available, int audio_available,
                       const emu_ui_engine_t *engine) {
    memset(ui, 0, sizeof *ui);
    ui->listen_fd = ui->client_fd = -1;
    ui->next_sequence = 1;
    ui->asc0_available = asc0_available ? 1 : 0;
    ui->audio_available = audio_available ? 1 : 0;
    if (!path || !path[0] || !model || !model[0] || !engine ||
        !engine->key_count || !engine->key_name || !engine->key_set ||
        !engine->key_sampled) {
        errno = EINVAL;
        return -1;
    }
    struct sockaddr_un addr;
    socklen_t addr_len;
    if (fill_unix_addr(&addr, path, &addr_len) != 0) return -1;
    size_t path_len = strlen(path);
    size_t model_len = strlen(model);
    if (path_len >= sizeof ui->path || model_len > EMU_UI_MODEL_MAX) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(ui->path, path, path_len + 1);
    memcpy(ui->model, model, model_len + 1);
    ui->engine = *engine;
    ui->listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ui->listen_fd < 0) return -1;
    set_nonblocking(ui->listen_fd);
    unlink(path);
    if (bind(ui->listen_fd, (struct sockaddr *)&addr, addr_len) != 0 ||
        listen(ui->listen_fd, 1) != 0) {
        int saved_errno = errno;
        close(ui->listen_fd);
        ui->listen_fd = -1;
        unlink(path);
        errno = saved_errno;
        return -1;
    }
    return 0;
}

static void runtime_event(void *opaque, const emu_runtime_state_t *runtime,
                          emu_runtime_event_t event) {
    emu_ui_socket_t *ui = opaque;
    if (event == EMU_RUNTIME_LIFECYCLE) {
        emu_runtime_state_lifecycle(runtime, &ui->lifecycle);
        ui->need_lifecycle = 1;
    } else {
        ui->need_stats = 1;
        if (ui->tx_type == EMU_UI_STATS && ui->tx_off == 0)
            ui->tx_len = 0;
    }
}

void emu_ui_socket_bind_runtime(emu_ui_socket_t *ui, emu_runtime_state_t *runtime) {
    ui->runtime = runtime;
    emu_runtime_state_identity(runtime, ui->run_id);
    emu_runtime_state_lifecycle(runtime, &ui->lifecycle);
    emu_runtime_descriptor_t descriptor;
    emu_runtime_state_descriptor(runtime, &descriptor);
    ui->descriptor = descriptor;
    strcpy(ui->model, descriptor.model);
    ui->frame_width = descriptor.width;
    ui->frame_height = descriptor.height;
    ui->asc0_available = descriptor.asc0_available;
    ui->audio_available = descriptor.audio_available;
    emu_runtime_state_subscribe(runtime, runtime_event, ui);
}

void emu_ui_socket_drain(emu_ui_socket_t *ui, const emu_ui_stats_t *stats) {
    if (!ui || !ui->runtime || !stats) return;
    uint64_t deadline = emu_runtime_monotonic_ns() + UINT64_C(100000000);
    ui->drain_deadline = deadline;
    while (ui->client_fd >= 0 && emu_runtime_monotonic_ns() < deadline) {
        flush_tx(ui, stats);
        if (ui->tx_len == ui->tx_off && !ui->need_stats && !ui->need_lifecycle &&
            !ui->latest_frame_pending && !ui->audio_head &&
            (!serial_pending(ui) && !ui->history_pending && !ui->need_subscribed && !ui->asc0_open_pending)) break;
        struct pollfd fd = {.fd = ui->client_fd, .events = POLLOUT};
        (void)poll(&fd, 1, 1);
    }
    ui->drain_deadline = 0;
}

void emu_ui_socket_stop_input(emu_ui_socket_t *ui) {
    close_input_scope(ui);
    if (ui->asc0_available && ui->engine.serial_link)
        (void)ui->engine.serial_link(ui->engine.opaque, 0);
}

void emu_ui_socket_close(emu_ui_socket_t *ui) {
    if (!ui) return;
    if (ui->runtime) emu_runtime_state_subscribe(ui->runtime, NULL, NULL);
    disconnect_client(ui);
    if (ui->asc0_available && ui->engine.serial_link)
        (void)ui->engine.serial_link(ui->engine.opaque, 0);
    if (ui->listen_fd >= 0) close(ui->listen_fd);
    ui->listen_fd = -1;
    if (ui->path[0]) unlink(ui->path);
    clear_audio(ui);
}

int emu_ui_socket_poll(emu_ui_socket_t *ui,
                       const emu_ui_stats_t *stats, int timeout_ms) {
    if (!ui || ui->listen_fd < 0 || !ui->runtime || !stats) {
        errno = EINVAL;
        return -1;
    }

    if (!ui->input || ui->input_failed) return -1;

    struct pollfd fds[2];
    nfds_t nfds = 1;
    fds[0].fd = ui->listen_fd;
    fds[0].events = POLLIN;
    fds[0].revents = 0;
    if (ui->client_fd >= 0) {
        fds[1].fd = ui->client_fd;
        fds[1].events = POLLIN | POLLOUT;
        fds[1].revents = 0;
        nfds = 2;
    }
    int rc = poll(fds, nfds, timeout_ms);
    if (rc < 0 && errno != EINTR) return -1;
    if (fds[0].revents & POLLIN) accept_client(ui);
    if (ui->client_fd >= 0 && nfds == 2) {
        if (fds[1].revents & (POLLERR | POLLHUP | POLLNVAL))
            disconnect_client(ui);
        else {
            if (fds[1].revents & POLLIN) read_rx(ui, stats->icount);
            if (ui->client_fd >= 0 && (fds[1].revents & POLLOUT))
                flush_tx(ui, stats);
        }
    }
    if (ui->client_fd >= 0) flush_tx(ui, stats);
    return ui->input_failed || ui->serial_failed ? -1 : 0;
}

int emu_ui_socket_frame(emu_ui_socket_t *ui, unsigned width, unsigned height,
                        const uint8_t *rendered, size_t frame_size,
                        uint64_t icount) {
    if (!ui || !rendered || !width || !height ||
        frame_size != (size_t)width * height * 3u ||
        frame_size > EMU_UI_MAX_FRAME_SIZE ||
        (ui->frame_size && (ui->frame_width != width ||
                            ui->frame_height != height)))
        return -1;
    ui->frame_width = width;
    ui->frame_height = height;
    ui->frame_size = frame_size;
    if (!ui->last_render_valid ||
        memcmp(rendered, ui->last_render, frame_size) != 0) {
        memcpy(ui->last_render, rendered, frame_size);
        memcpy(ui->latest_frame, rendered, frame_size);
        ui->last_render_valid = 1;
        ui->latest_frame_pending = ui->client_fd >= 0;
        if (ui->tx_type == EMU_UI_FRAME && ui->tx_off == 0) {
            ui->frame_messages++;
            make_packet(ui, EMU_UI_FRAME, ui->latest_frame,
                        (uint32_t)frame_size, icount);
            ui->latest_frame_pending = 0;
        }
    }
    return 0;
}

void emu_ui_socket_bind_serial(emu_ui_socket_t *ui, emu_serial_history_t *serial) {
    ui->serial = serial;
}

int emu_ui_socket_speaker_pcm(
        emu_ui_socket_t *ui, const emu_audio_output_t *output) {
    if (!ui || !output || output->frame_count > SIZE_MAX / 2u ||
        (!output->samples && output->frame_count))
        return -1;
    if (!ui->audio_available || ui->client_fd < 0 || !output->frame_count)
        return 0;
    size_t max_frames = (EMU_UI_MAX_PAYLOAD - EMU_UI_AUDIO_PREFIX_SIZE) / 2u;
    for (size_t offset = 0; offset < output->frame_count;) {
        size_t count = output->frame_count - offset;
        if (count > max_frames) count = max_frames;
        uint32_t size = (uint32_t)(EMU_UI_AUDIO_PREFIX_SIZE + count * 2u);
        uint8_t *payload = malloc(size);
        if (!payload) return -1;
        put32(payload, 48000u);
        put16(payload + 4, 1u);
        put16(payload + 6, 0);
        for (size_t i = 0; i < count; i++)
            put16(payload + EMU_UI_AUDIO_PREFIX_SIZE + i * 2u,
                  (uint16_t)output->samples[offset + i]);
        int result = append_audio_packet(
            ui, EMU_UI_SPEAKER_PCM, output->completion_tick, payload, size);
        free(payload);
        if (result != 0) return result;
        offset += count;
    }
    return 0;
}

void emu_ui_socket_audio_reset(emu_ui_socket_t *ui, uint64_t icount) {
    (void)icount;
    if (!ui) return;
    clear_audio(ui);
    ui->audio_reset_pending = ui->audio_available && ui->client_fd >= 0;
}
