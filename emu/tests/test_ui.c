#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>
#include <unistd.h>

#include "fake_engine.h"
#include "test_support.h"
#include "emu_ui.h"

typedef struct {
    emu_ui_socket_t *ui;
    uint8_t serial[64];
    size_t serial_size;
    char deferred[64][16];
    size_t deferred_count;
} test_callbacks_t;

typedef struct {
    emu_ui_session_binding_t session;
    test_callbacks_t *observations;
} test_binding_t;

static const uint8_t test_identity[16] = {0x12, 0x34};

static uint16_t get16(const uint8_t *p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t get32(const uint8_t *p) {
    uint32_t value = 0;
    for (int i = 3; i >= 0; i--) value = (value << 8) | p[i];
    return value;
}

static uint64_t get64(const uint8_t *p) {
    uint64_t value = 0;
    for (int i = 7; i >= 0; i--) value = (value << 8) | p[i];
    return value;
}

static void put16(uint8_t *p, uint16_t value) {
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

static void put32(uint8_t *p, uint32_t value) {
    for (int i = 0; i < 4; i++) p[i] = (uint8_t)(value >> (8 * i));
}

static void put64(uint8_t *p, uint64_t value) {
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)(value >> (8 * i));
}

static size_t make_message(uint8_t *message, uint16_t type,
                           const void *payload, uint32_t payload_size) {
    memset(message, 0, EMU_UI_HEADER_SIZE);
    put32(message, EMU_UI_MAGIC);
    put16(message + 4, EMU_UI_VERSION);
    put16(message + 6, type);
    put32(message + 8, payload_size);
    memcpy(message + 32, test_identity, sizeof test_identity);
    if (payload_size)
        memcpy(message + EMU_UI_HEADER_SIZE, payload, payload_size);
    return EMU_UI_HEADER_SIZE + payload_size;
}

static int send_all(int fd, const void *bytes, size_t size) {
    const uint8_t *p = bytes;
    while (size) {
        ssize_t sent = send(fd, p, size, 0);
        if (sent <= 0) return -1;
        p += sent;
        size -= (size_t)sent;
    }
    return 0;
}

static int receive_all(int fd, void *bytes, size_t size) {
    uint8_t *p = bytes;
    while (size) {
        ssize_t received = recv(fd, p, size, 0);
        if (received <= 0) return -1;
        p += received;
        size -= (size_t)received;
    }
    return 0;
}

static int receive_message(int fd, uint16_t *type, uint64_t *sequence,
                           uint64_t *icount, uint8_t *payload,
                           size_t capacity, uint32_t *payload_size) {
    uint8_t header[EMU_UI_HEADER_SIZE];
    if (receive_all(fd, header, sizeof header)) return -1;
    uint32_t size = get32(header + 8);
    if (get32(header) != EMU_UI_MAGIC ||
        get16(header + 4) != EMU_UI_VERSION || size > capacity ||
        get32(header + 28) != 0)
        return -1;
    *type = get16(header + 6);
    *sequence = get64(header + 12);
    *icount = get64(header + 20);
    *payload_size = size;
    return receive_all(fd, payload, size);
}

static void frame_callback(void *opaque, const emu_frame_t *frame) {
    test_callbacks_t *callbacks = opaque;
    if (frame->kind == EMU_FRAME_DISPLAY)
        EMU_CHECK(emu_ui_socket_frame(
                      callbacks->ui, frame->width, frame->height,
                      frame->rgb, frame->rgb_size, frame->icount) == 0);
}

static void serial_callback(void *opaque, const uint8_t *bytes, size_t size) {
    test_callbacks_t *callbacks = opaque;
    EMU_CHECK(callbacks->serial_size + size <= sizeof callbacks->serial);
    memcpy(callbacks->serial + callbacks->serial_size, bytes, size);
    callbacks->serial_size += size;
    EMU_CHECK(emu_serial_history_append(callbacks->ui->serial, bytes, size) == 0);
}

static int connect_client(const char *path) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    EMU_CHECK(fd >= 0);
    struct timeval timeout = {0, 100000};
    EMU_CHECK(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO,
                         &timeout, sizeof timeout) == 0);
    struct sockaddr_un address;
    memset(&address, 0, sizeof address);
    address.sun_family = AF_UNIX;
    snprintf(address.sun_path, sizeof address.sun_path, "%s", path);
    socklen_t length = (socklen_t)(offsetof(struct sockaddr_un, sun_path) +
                                  strlen(address.sun_path) + 1);
    EMU_CHECK(connect(fd, (struct sockaddr *)&address, length) == 0);
    return fd;
}

/* Test host owns service lifetime and polls it before transport input. */
static void input_trace(void *opaque, const emu_input_event_t *event) {
    emu_ui_engine_t *engine = opaque;
    test_binding_t *binding = engine->opaque;
    test_callbacks_t *callbacks = binding->observations;
    if (strcmp(event->action, "queued") && strcmp(event->action, "sampled")) return;
    EMU_CHECK(event->name && event->key < 2);
    EMU_CHECK(callbacks->deferred_count < 64);
    snprintf(callbacks->deferred[callbacks->deferred_count++],
             sizeof callbacks->deferred[0], "%s", event->action);
}
static int input_set(void *opaque, const char *name, int down) {
    emu_ui_engine_t *e = opaque;
    return e->key_set(e->opaque, name, down);
}
static int input_sampled(void *opaque, const char *name, int *sampled) {
    emu_ui_engine_t *e = opaque;
    return e->key_sampled(e->opaque, name, sampled);
}
static int open_transport(emu_ui_socket_t *ui, const char *path, const char *model,
                           int serial, int audio, emu_ui_engine_t *engine) {
    int rc = emu_ui_socket_open(ui, path, model, serial, audio, engine);
    if (rc) return rc;
    const char *names[EMU_INPUT_MAX_KEYS];
    size_t count = engine->key_count(engine->opaque);
    for (size_t k = 0; k < count; k++) names[k] = engine->key_name(engine->opaque, k);
    emu_input_callbacks_t cb = {.opaque = engine, .set = input_set,
        .sampled = input_sampled, .trace = input_trace};
    emu_input_t *input = emu_input_create(names, count, &cb);
    EMU_CHECK(input != NULL);
    emu_ui_socket_bind_input(ui, input);
    emu_ui_socket_bind_serial(ui, emu_serial_history_create(NULL));
    EMU_CHECK(ui->serial != NULL);
    emu_runtime_descriptor_t descriptor = {
        .width = 1, .height = 1, .key_count = count,
        .asc0_available = serial, .audio_available = audio,
    };
    snprintf(descriptor.model, sizeof descriptor.model, "%s", model);
    for (size_t k = 0; k < count; k++)
        snprintf(descriptor.keys[k], sizeof descriptor.keys[k], "%s", names[k]);
    emu_runtime_state_t *runtime = emu_runtime_state_create(&descriptor, test_identity);
    EMU_CHECK(runtime != NULL);
    emu_ui_socket_bind_runtime(ui, runtime);
    return 0;
}
static void close_transport(emu_ui_socket_t *ui) {
    emu_ui_socket_close(ui);
    EMU_CHECK(emu_input_destroy(ui->input) == 0);
    ui->input = NULL;
    emu_serial_history_destroy(ui->serial);
    ui->serial = NULL;
    emu_runtime_state_destroy(ui->runtime);
    ui->runtime = NULL;
}
static int host_poll(emu_ui_socket_t *ui, const emu_ui_stats_t *stats, int timeout) {
    emu_input_context(ui->input, stats->icount, stats->pc);
    EMU_CHECK(emu_input_poll(ui->input) == 0);
    return emu_ui_socket_poll(ui, stats, timeout);
}

static void poll_transport(emu_ui_socket_t *ui, uint64_t icount) {
    emu_ui_stats_t stats = {
        .icount = icount,
        .elapsed_ns = icount * 1000,
        .ticks = icount + 1,
        .guest_instructions = icount + 2,
        .pc = 0x800000u + (uint32_t)icount,
    };
    /* Bootstrap one sample. Later transport polls do not manufacture samples;
     * tests requiring new measurements publish them explicitly. */
    emu_runtime_snapshot_t published;
    emu_runtime_state_snapshot(ui->runtime, &published);
    if (!published.sample_sequence) {
        stats.measured_ns = stats.elapsed_ns;
        EMU_CHECK(emu_runtime_state_publish(ui->runtime, &stats, 1) == 0);
    }
    emu_runtime_state_snapshot(ui->runtime, &stats);
    EMU_CHECK(host_poll(ui, &stats, 0) == 0);
}

static void test_key_cleanup(emu_ui_engine_t *engine, emu_session_t *session) {
    emu_ui_socket_t ui;
    char path[96];
    snprintf(path, sizeof path, "/tmp/emu-ui-keys-%ld.sock", (long)getpid());
    EMU_CHECK(open_transport(&ui, path, "fake", 1, 1, engine) == 0);
    for (int mode = 0; mode < 4; mode++) {
        int fd = connect_client(path);
        poll_transport(&ui, 1);
        uint8_t packets[256], down[] = {0, 1}, up[] = {0, 0}, index = 0;
        size_t size = make_message(packets, EMU_UI_KEY, down, sizeof down);
        size += make_message(packets + size, EMU_UI_KEY, down, sizeof down);
        size += make_message(packets + size, EMU_UI_KEY_RELEASE_AFTER_SAMPLE, &index, 1);
        EMU_CHECK(send_all(fd, packets, size) == 0);
        poll_transport(&ui, 2);
        EMU_CHECK(emu_input_down_mask(ui.input) == 1 && emu_input_pending_mask(ui.input) == 1);
        if (mode == 3) {
            close(fd);
        } else {
            size = mode == 2
                ? make_message(packets, EMU_UI_RELEASE_ALL, NULL, 0)
                : make_message(packets, EMU_UI_KEY,
                               mode == 0 ? down : up, 2);
            EMU_CHECK(send_all(fd, packets, size) == 0);
        }
        poll_transport(&ui, 3);
        emu_key_sample_t sample;
        emu_error_t error = {0};
        EMU_CHECK(emu_session_key_query(session, "power", &sample, &error) == EMU_OK);
        EMU_CHECK(emu_input_pending_mask(ui.input) == 0);
        EMU_CHECK(sample.requested_pressed == (mode == 0));
        if (mode != 3) {
            /* Duplicate raw releases are idempotent at the electrical boundary. */
            size = make_message(packets, EMU_UI_KEY, up, 2);
            size += make_message(packets + size, EMU_UI_KEY, up, 2);
            EMU_CHECK(send_all(fd, packets, size) == 0);
            poll_transport(&ui, 4);
            EMU_CHECK(emu_session_key_query(session, "power", &sample, &error) == EMU_OK);
            EMU_CHECK(!sample.requested_pressed && emu_input_down_mask(ui.input) == 0);
            close(fd);
            poll_transport(&ui, 5);
        }
    }
    close_transport(&ui);
}

static void test_owner_protocol(emu_ui_engine_t *engine, emu_session_t *session) {
    emu_ui_socket_t ui;
    char path[96];
    snprintf(path, sizeof path, "/tmp/emu-ui-owners-%ld.sock", (long)getpid());
    EMU_CHECK(open_transport(&ui, path, "fake", 1, 0, engine) == 0);
    uint64_t external_scope = emu_input_scope_open(ui.input);
    uint64_t external = emu_input_owner_open(ui.input, external_scope);
    int fd = connect_client(path);
    poll_transport(&ui, 1);
    uint8_t packets[1024], a[10] = {1}, b[10] = {2};
    size_t n = make_message(packets, EMU_UI_OWNER_OPEN, a, 8);
    /* Fragment registration, then coalesce duplicate registration and controls. */
    EMU_CHECK(send_all(fd, packets, 49) == 0);
    poll_transport(&ui, 2);
    EMU_CHECK(ui.rx_len == 49);
    EMU_CHECK(send_all(fd, packets + 49, n - 49) == 0);
    n = make_message(packets, EMU_UI_OWNER_OPEN, a, 8);
    n += make_message(packets + n, EMU_UI_OWNER_OPEN, b, 8);
    a[9] = b[9] = EMU_INPUT_PRESS;
    n += make_message(packets + n, EMU_UI_OWNER_KEY, a, 10);
    n += make_message(packets + n, EMU_UI_OWNER_KEY, b, 10);
    EMU_CHECK(send_all(fd, packets, n) == 0);
    poll_transport(&ui, 3);
    emu_input_key_state_t state;
    EMU_CHECK(emu_input_query(ui.input, 0, &state) == 0 && state.holders == 2);
    a[9] = EMU_INPUT_RELEASE_SAMPLED;
    n = make_message(packets, EMU_UI_OWNER_KEY, a, 10);
    n += make_message(packets + n, EMU_UI_OWNER_CLOSE, a, 8);
    EMU_CHECK(send_all(fd, packets, n) == 0);
    poll_transport(&ui, 4);
    EMU_CHECK(emu_input_query(ui.input, 0, &state) == 0 && state.holders == 1 && state.down);
    b[9] = EMU_INPUT_RELEASE_SAMPLED;
    n = make_message(packets, EMU_UI_OWNER_KEY, b, 10);
    EMU_CHECK(send_all(fd, packets, n) == 0);
    poll_transport(&ui, 5);
    EMU_CHECK(emu_input_pending(ui.input));
    n = make_message(packets, EMU_UI_OWNER_CLOSE, b, 8);
    EMU_CHECK(send_all(fd, packets, n) == 0);
    poll_transport(&ui, 6);
    EMU_CHECK(!emu_input_pending(ui.input) && !emu_input_down_mask(ui.input));
    /* Legacy release-all covers the connection, never an external scope. */
    EMU_CHECK(emu_input_key(ui.input, external, 0, EMU_INPUT_PRESS) == 0);
    n = make_message(packets, EMU_UI_OWNER_OPEN, a, 8);
    a[9] = EMU_INPUT_PRESS;
    n += make_message(packets + n, EMU_UI_OWNER_KEY, a, 10);
    uint8_t raw[] = {0, 1};
    n += make_message(packets + n, EMU_UI_KEY, raw, 2);
    n += make_message(packets + n, EMU_UI_RELEASE_ALL, NULL, 0);
    EMU_CHECK(send_all(fd, packets, n) == 0);
    poll_transport(&ui, 7);
    EMU_CHECK(emu_input_query(ui.input, 0, &state) == 0 && state.holders == 1);
    /* Connection replacement destroys its scope; external owner survives. */
    int replacement = connect_client(path);
    poll_transport(&ui, 8);
    close(fd); fd = replacement;
    n = make_message(packets, EMU_UI_OWNER_KEY, a, 10);
    EMU_CHECK(send_all(fd, packets, n) == 0);
    poll_transport(&ui, 9);
    EMU_CHECK(ui.client_fd == -1);
    close(fd);
    EMU_CHECK(emu_input_owner_holds(ui.input, external, 0) == 1);
    /* Invalid action/length/index closes the connection without applying it. */
    for (int mode = 0; mode < 3; mode++) {
        fd = connect_client(path); poll_transport(&ui, 10);
        n = make_message(packets, EMU_UI_OWNER_OPEN, a, 8);
        a[8] = mode == 2 ? 32 : 0; a[9] = mode == 0 ? 3 : 1;
        n += make_message(packets + n, EMU_UI_OWNER_KEY, a, mode == 1 ? 9 : 10);
        EMU_CHECK(send_all(fd, packets, n) == 0);
        poll_transport(&ui, 11);
        EMU_CHECK(ui.client_fd == -1);
        close(fd);
    }
    EMU_CHECK(emu_input_scope_close(ui.input, external_scope) == 0);
    emu_key_sample_t sample; emu_error_t error = {0};
    EMU_CHECK(emu_session_key_query(session, "power", &sample, &error) == EMU_OK);
    EMU_CHECK(!sample.requested_pressed);
    close_transport(&ui);
}

static uint64_t serial_cursor(emu_ui_socket_t *ui) {
    uint64_t cursor;
    EMU_CHECK(emu_serial_history_bounds(ui->serial, ui->subscription, NULL, &cursor, NULL) == 0);
    return cursor;
}
/* Existing fixtures supply the complete producer history. */
static void append_serial(emu_ui_socket_t *ui, const uint8_t *bytes, size_t size) {
    uint64_t tail = emu_serial_history_tail(ui->serial);
    EMU_CHECK(size >= tail);
    EMU_CHECK(emu_serial_history_append(ui->serial, bytes + tail, size - tail) == 0);
}

static void test_serial_backpressure(emu_ui_engine_t *engine) {
    emu_ui_socket_t ui;
    char path[96];
    snprintf(path, sizeof path, "/tmp/emu-ui-serial-%ld.sock", (long)getpid());
    EMU_CHECK(open_transport(&ui, path, "fake", 1, 1, engine) == 0);
    const size_t history = 13, live = 2 * 1024 * 1024, capacity = live + 65536;
    uint8_t *bytes = malloc(history + live + 16), *wire = malloc(capacity);
    EMU_CHECK(bytes && wire);
    /* Avoid a short repeating pattern that could hide whole-packet reordering. */
    uint32_t pattern = 0x12345678u;
    for (size_t i = 0; i < history + live + 16; i++) {
        pattern ^= pattern << 13;
        pattern ^= pattern >> 17;
        pattern ^= pattern << 5;
        bytes[i] = (uint8_t)pattern;
    }
    append_serial(&ui, bytes, history);
    int fd = connect_client(path);
    poll_transport(&ui, 1);
    int small = 4096;
    EMU_CHECK(setsockopt(ui.client_fd, SOL_SOCKET, SO_SNDBUF, &small, sizeof small) == 0);
    append_serial(&ui, bytes, history + live);
    poll_transport(&ui, 1);
    EMU_CHECK(ui.tx_off < ui.tx_len && serial_cursor(&ui) < history + live);
    size_t used = 0;
    for (unsigned tries = 0; tries < 10000; tries++) {
        EMU_CHECK(used < capacity);
        ssize_t n = recv(fd, wire + used, capacity - used, MSG_DONTWAIT);
        if (n > 0) used += (size_t)n;
        else {
            EMU_CHECK(n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK));
            if (serial_cursor(&ui) == history + live && ui.tx_off == ui.tx_len) break;
        }
        poll_transport(&ui, 1);
    }
    size_t offset = 0, delivered = 0;
    uint64_t previous_sequence = 0;
    while (offset < used) {
        EMU_CHECK(used - offset >= EMU_UI_HEADER_SIZE);
        uint8_t *header = wire + offset;
        size_t size = get32(header + 8);
        EMU_CHECK(offset + EMU_UI_HEADER_SIZE + size <= used);
        EMU_CHECK(get64(header + 12) > previous_sequence);
        previous_sequence = get64(header + 12);
        if (get16(header + 6) == EMU_UI_ASC0_TX) {
            EMU_CHECK(size > 16 && delivered + size - 16 <= live);
            EMU_CHECK(get64(header + EMU_UI_HEADER_SIZE) == ui.subscription);
            EMU_CHECK(get64(header + EMU_UI_HEADER_SIZE + 8) == history + delivered);
            EMU_CHECK(!memcmp(header + EMU_UI_HEADER_SIZE + 16,
                             bytes + history + delivered, size - 16));
            delivered += size - 16;
        }
        offset += EMU_UI_HEADER_SIZE + size;
    }
    EMU_CHECK(delivered == live);
    close(fd);
    poll_transport(&ui, 2);
    /* Bytes produced while detached belong before the next live boundary. */
    append_serial(&ui, bytes, history + live + 5);
    fd = connect_client(path);
    poll_transport(&ui, 3);
    uint8_t payload[EMU_UI_MAX_PAYLOAD];
    uint16_t type;
    uint32_t size;
    uint64_t sequence, icount;
    for (int i = 0; i < 5; i++) {
        EMU_CHECK(receive_message(fd, &type, &sequence, &icount,
                                  payload, sizeof payload, &size) == 0);
        EMU_CHECK(type != EMU_UI_ASC0_TX);
    }
    append_serial(&ui, bytes, history + live + 8);
    poll_transport(&ui, 4);
    EMU_CHECK(receive_message(fd, &type, &sequence, &icount,
                              payload, sizeof payload, &size) == 0);
    EMU_CHECK(type == EMU_UI_ASC0_TX && size == 19 &&
              !memcmp(payload + 16, bytes + history + live + 5, 3));
    close(fd);
    close_transport(&ui);
    free(bytes);
    free(wire);
}

static void receive_kind(int fd, uint16_t expected, uint8_t *payload, uint32_t *size) {
    uint16_t type;
    uint64_t seq, icount;
    EMU_CHECK(receive_message(fd, &type, &seq, &icount, payload, EMU_UI_MAX_PAYLOAD, size) == 0);
    EMU_CHECK(type == expected);
}
static void serial_bootstrap(emu_ui_socket_t *ui, int fd, uint64_t boundary) {
    uint8_t payload[EMU_UI_MAX_PAYLOAD];
    uint32_t size;
    receive_kind(fd, EMU_UI_HELLO, payload, &size);
    receive_kind(fd, EMU_UI_LIFECYCLE, payload, &size);
    receive_kind(fd, EMU_UI_STATS, payload, &size);
    receive_kind(fd, EMU_UI_ASC0_SUBSCRIBED, payload, &size);
    EMU_CHECK(size == 16 && get64(payload) == ui->subscription && get64(payload + 8) == boundary);
}
static void test_serial_replay(emu_ui_engine_t *engine) {
    emu_ui_socket_t ui;
    char path[96];
    snprintf(path, sizeof path, "/tmp/emu-ui-replay-%ld.sock", (long)getpid());
    EMU_CHECK(open_transport(&ui, path, "fake", 1, 0, engine) == 0);
    EMU_CHECK(emu_serial_history_append(ui.serial, (const uint8_t *)"old", 3) == 0);
    int fd = connect_client(path);
    poll_transport(&ui, 1);
    uint64_t first = ui.subscription;
    serial_bootstrap(&ui, fd, 3);
    size_t n = EMU_UI_MAX_PAYLOAD + 500;
    uint8_t *data = malloc(n);
    EMU_CHECK(data != NULL);
    for (size_t i = 0; i < n; i++) data[i] = (uint8_t)(i * 43 + i / 251);
    EMU_CHECK(emu_serial_history_append(ui.serial, data, n) == 0);
    uint8_t range[32], packet[256], payload[EMU_UI_MAX_PAYLOAD];
    put64(range, first); put64(range + 8, 1); put64(range + 16, 3); put64(range + 24, 3 + n);
    size_t size = make_message(packet, EMU_UI_ASC0_HISTORY_READ, range, 32);
    /* Fragmented request: no request state changes until its final byte. */
    EMU_CHECK(send_all(fd, packet, size - 1) == 0);
    poll_transport(&ui, 2);
    EMU_CHECK(!ui.history_pending && !ui.history_request);
    uint32_t length;
    receive_kind(fd, EMU_UI_ASC0_TX, payload, &length);
    EMU_CHECK(get64(payload + 8) == 3 && !memcmp(payload + 16, data, length - 16));
    receive_kind(fd, EMU_UI_ASC0_TX, payload, &length);
    uint64_t cursor = serial_cursor(&ui);
    EMU_CHECK(cursor == 3 + n);
    EMU_CHECK(send_all(fd, packet + size - 1, 1) == 0);
    /* New live bytes compete with replay at the serial scheduling position. */
    EMU_CHECK(emu_serial_history_append(ui.serial, data, n) == 0);
    poll_transport(&ui, 3);
    receive_kind(fd, EMU_UI_ASC0_HISTORY_DATA, payload, &length);
    EMU_CHECK(get64(payload) == first && get64(payload + 8) == 1 && get64(payload + 16) == 3);
    EMU_CHECK(length == EMU_UI_MAX_PAYLOAD && !memcmp(payload + 32, data, length - 32));
    uint64_t next = get64(payload + 24);
    receive_kind(fd, EMU_UI_ASC0_TX, payload, &length);
    EMU_CHECK(get64(payload + 8) == cursor && !memcmp(payload + 16, data, length - 16));
    receive_kind(fd, EMU_UI_ASC0_TX, payload, &length);
    EMU_CHECK(serial_cursor(&ui) == 3 + 2 * n);
    /* Coalesce the continuation with OPEN. READY must precede replay. */
    put64(range + 8, 2); put64(range + 16, next);
    size = make_message(packet, EMU_UI_ASC0_HISTORY_READ, range, 32);
    size += make_message(packet + size, EMU_UI_ASC0_OPEN, NULL, 0);
    EMU_CHECK(emu_serial_history_append(ui.serial, (const uint8_t *)"pre", 3) == 0);
    EMU_CHECK(send_all(fd, packet, size) == 0);
    poll_transport(&ui, 4);
    receive_kind(fd, EMU_UI_ASC0_TX, payload, &length);
    EMU_CHECK(length == 19 && !memcmp(payload + 16, "pre", 3));
    receive_kind(fd, EMU_UI_ASC0_READY, payload, &length);
    EMU_CHECK(length == 16 && get64(payload) == first && get64(payload + 8) == 6 + 2 * n);
    receive_kind(fd, EMU_UI_ASC0_HISTORY_DATA, payload, &length);
    EMU_CHECK(get64(payload + 16) == next && get64(payload + 24) == 3 + n);
    EMU_CHECK(!memcmp(payload + 32, data + next - 3, length - 32));
    /* Empty replay at tail succeeds, and CLOSE retains observation. */
    put64(range + 8, 3); put64(range + 16, 6 + 2 * n); put64(range + 24, 6 + 2 * n);
    size = make_message(packet, EMU_UI_ASC0_HISTORY_READ, range, 32);
    size += make_message(packet + size, EMU_UI_ASC0_CLOSE, NULL, 0);
    EMU_CHECK(send_all(fd, packet, size) == 0);
    poll_transport(&ui, 5);
    receive_kind(fd, EMU_UI_ASC0_HISTORY_DATA, payload, &length);
    EMU_CHECK(length == 32 && get64(payload + 16) == get64(payload + 24));
    EMU_CHECK(!ui.asc0_ready && serial_cursor(&ui) == 6 + 2 * n);
    /* Replacement closes the old handle; the new boundary excludes detached history. */
    int replacement = connect_client(path);
    poll_transport(&ui, 6);
    EMU_CHECK(ui.subscription != first);
    serial_bootstrap(&ui, replacement, 6 + 2 * n);
    emu_serial_view_t view;
    EMU_CHECK(emu_serial_history_read(ui.serial, first, 3, 3, 0, &view) == -1);
    close(fd); close(replacement); close_transport(&ui); free(data);
}
static void test_serial_invalid_requests(emu_ui_engine_t *engine) {
    for (unsigned scenario = 0; scenario < 8; scenario++) {
        emu_ui_socket_t ui;
        char path[96];
        snprintf(path, sizeof path, "/tmp/emu-ui-bad-serial-%ld.sock", (long)getpid());
        EMU_CHECK(open_transport(&ui, path, "fake", 1, 0, engine) == 0);
        EMU_CHECK(emu_serial_history_append(ui.serial, (const uint8_t *)"old", 3) == 0);
        int fd = connect_client(path);
        poll_transport(&ui, 1);
        serial_bootstrap(&ui, fd, 3);
        uint8_t range[32], packet[256];
        put64(range, ui.subscription); put64(range + 8, 1);
        put64(range + 16, 3); put64(range + 24, 3);
        if (scenario == 0) put64(range, ui.subscription + 1);
        if (scenario == 1) put64(range + 8, 0);
        if (scenario == 2) put64(range + 16, 2);
        if (scenario == 3) put64(range + 24, UINT64_MAX);
        if (scenario == 4) put64(range + 16, 4);
        size_t size = make_message(packet, EMU_UI_ASC0_HISTORY_READ, range, scenario == 5 ? 31 : 32);
        if (scenario == 6) size += make_message(packet + size, EMU_UI_ASC0_HISTORY_READ, range, 32);
        if (scenario == 7) {
            size = make_message(packet, EMU_UI_ASC0_OPEN, NULL, 0);
            size += make_message(packet + size, EMU_UI_ASC0_RX, "X", 1);
        }
        EMU_CHECK(send_all(fd, packet, size) == 0);
        poll_transport(&ui, 2);
        EMU_CHECK(ui.client_fd == -1 && !ui.subscription);
        EMU_CHECK(emu_serial_history_tail(ui.serial) == 3);
        close(fd); close_transport(&ui);
    }
}

static void test_runtime_required(emu_ui_engine_t *engine) {
    emu_ui_socket_t ui;
    char path[96];
    snprintf(path, sizeof path, "/tmp/emu-ui-unbound-%ld.sock", (long)getpid());
    EMU_CHECK(open_transport(&ui, path, "fake", 1, 0, engine) == 0);
    /* Leave input and serial bound, isolating the runtime precondition. */
    emu_runtime_state_t *runtime = ui.runtime;
    emu_runtime_state_subscribe(runtime, NULL, NULL);
    ui.runtime = NULL;
    int fd = connect_client(path);
    uint8_t packet[64], key[] = {0, 1};
    size_t length = make_message(packet, EMU_UI_KEY, key, sizeof key);
    EMU_CHECK(send_all(fd, packet, length) == 0);
    emu_ui_stats_t stats = {.icount = 100000};
    errno = 0;
    EMU_CHECK(emu_ui_socket_poll(&ui, &stats, 0) == -1 && errno == EINVAL);
    EMU_CHECK(ui.client_fd == -1 && ui.connections == 0);
    EMU_CHECK(emu_input_down_mask(ui.input) == 0);
    emu_ui_socket_drain(&ui, &stats);
    EMU_CHECK(ui.client_fd == -1 && ui.tx_len == 0);
    close(fd);
    close_transport(&ui);
    emu_runtime_state_destroy(runtime);

    EMU_CHECK(emu_ui_socket_open(&ui, path, "fake", 0, 0, engine) == 0);
    emu_ui_socket_close(&ui); /* No bound services: partial-init cleanup. */
}

static void test_runtime_publication_only(emu_ui_engine_t *engine) {
    emu_ui_socket_t ui;
    char path[96];
    snprintf(path, sizeof path, "/tmp/emu-ui-publication-%ld.sock", (long)getpid());
    EMU_CHECK(open_transport(&ui, path, "fake", 1, 0, engine) == 0);
    int fd = connect_client(path);
    poll_transport(&ui, 1);
    serial_bootstrap(&ui, fd, 0);
    emu_ui_stats_t stats;
    emu_runtime_state_snapshot(ui.runtime, &stats);
    uint64_t first_sequence = stats.sample_sequence;
    stats.icount += 100000;
    stats.ticks += 100000;
    stats.guest_instructions += 100000;
    stats.measured_ns += 1000000;
    /* Counter progress alone cannot publish within the service's 10-ms gate. */
    EMU_CHECK(emu_runtime_state_publish(ui.runtime, &stats, 0) == 1);
    EMU_CHECK(host_poll(&ui, &stats, 0) == 0);
    uint8_t payload[EMU_UI_MAX_PAYLOAD];
    EMU_CHECK(recv(fd, payload, 1, MSG_DONTWAIT) == -1 &&
              (errno == EAGAIN || errno == EWOULDBLOCK));
    EMU_CHECK(ui.sent_sample == first_sequence);

    /* A zero-progress sample is still new once its timestamp is eligible. */
    emu_runtime_state_snapshot(ui.runtime, &stats);
    stats.measured_ns += 10000000;
    EMU_CHECK(emu_runtime_state_publish(ui.runtime, &stats, 0) == 0);
    emu_runtime_state_snapshot(ui.runtime, &stats);
    EMU_CHECK(host_poll(&ui, &stats, 0) == 0);
    uint32_t size;
    receive_kind(fd, EMU_UI_STATS, payload, &size);
    EMU_CHECK(size == 72 && get64(payload + 28) == first_sequence + 1);
    EMU_CHECK(get64(payload + 8) == stats.ticks && stats.icount == 1);
    EMU_CHECK(host_poll(&ui, &stats, 0) == 0);
    EMU_CHECK(recv(fd, payload, 1, MSG_DONTWAIT) == -1 &&
              (errno == EAGAIN || errno == EWOULDBLOCK));
    close(fd);
    close_transport(&ui);
}

static void test_runtime_delivery(emu_ui_engine_t *engine) {
    emu_ui_socket_t ui;
    char path[96];
    snprintf(path, sizeof path, "/tmp/emu-ui-state-%ld.sock", (long)getpid());
    EMU_CHECK(open_transport(&ui, path, "fake", 1, 0, engine) == 0);
    emu_runtime_state_t *runtime = ui.runtime;
    emu_ui_stats_t stats = {.icount = UINT64_C(9007199254740999), .measured_ns = 100};
    EMU_CHECK(emu_runtime_state_publish(runtime, &stats, 1) == 0);
    EMU_CHECK(emu_runtime_state_transition(runtime, EMU_RUNTIME_RUNNING, "", "") == 0);
    emu_runtime_state_snapshot(runtime, &stats);
    int fd = connect_client(path);
    EMU_CHECK(host_poll(&ui, &stats, 0) == 0);
    uint8_t header[EMU_UI_HEADER_SIZE], payload[EMU_UI_MAX_PAYLOAD];
    EMU_CHECK(receive_all(fd, header, sizeof header) == 0);
    EMU_CHECK(get16(header + 4) == EMU_UI_VERSION && get16(header + 6) == EMU_UI_HELLO &&
              !memcmp(header + 32, test_identity, 16));
    EMU_CHECK(receive_all(fd, payload, get32(header + 8)) == 0);
    uint16_t type;
    uint32_t size;
    uint64_t sequence, icount;
    EMU_CHECK(receive_message(fd, &type, &sequence, &icount, payload, sizeof payload, &size) == 0);
    EMU_CHECK(type == EMU_UI_LIFECYCLE && payload[0] == EMU_RUNTIME_RUNNING);
    EMU_CHECK(receive_message(fd, &type, &sequence, &icount, payload, sizeof payload, &size) == 0);
    EMU_CHECK(type == EMU_UI_STATS && size == 72 && get64(payload + 28) == 1 &&
              get64(payload + 36) == 100 && icount == stats.icount);
    receive_kind(fd, EMU_UI_ASC0_SUBSCRIBED, payload, &size);
    /* The latest sample replaces pending measurements without changing counters. */
    for (int i = 0; i < 100; i++) {
        stats.ticks++; stats.icount++; stats.measured_ns += 10000000;
        EMU_CHECK(emu_runtime_state_publish(runtime, &stats, 1) == 0);
    }
    emu_runtime_state_snapshot(runtime, &stats);
    EMU_CHECK(host_poll(&ui, &stats, 0) == 0);
    EMU_CHECK(receive_message(fd, &type, &sequence, &icount, payload, sizeof payload, &size) == 0);
    EMU_CHECK(type == EMU_UI_STATS && get64(payload + 28) == 101 && get64(payload + 8) == 100);
    EMU_CHECK(get64(payload + 44) == 1000000000 && get32(payload + 68) == 1);
    uint64_t rate_bits = get64(payload + 52);
    double rate; memcpy(&rate, &rate_bits, 8);
    EMU_CHECK(rate == 100);
    /* Fragmented controls have no electrical effect until their packet completes. */
    uint8_t command[64], key[] = {0, 1};
    size_t length = make_message(command, EMU_UI_KEY, key, 2);
    memcpy(command + 32, test_identity, 16);
    EMU_CHECK(send_all(fd, command, 39) == 0);
    EMU_CHECK(host_poll(&ui, &stats, 0) == 0 && emu_input_down_mask(ui.input) == 0);
    EMU_CHECK(send_all(fd, command + 39, length - 39) == 0);
    EMU_CHECK(host_poll(&ui, &stats, 0) == 0 && emu_input_down_mask(ui.input) == 1);
    command[32] ^= 1;
    EMU_CHECK(send_all(fd, command, length) == 0);
    EMU_CHECK(host_poll(&ui, &stats, 0) == 0 && ui.client_fd == -1 && emu_input_down_mask(ui.input) == 0);
    close(fd);
    fd = connect_client(path);
    EMU_CHECK(host_poll(&ui, &stats, 0) == 0);
    EMU_CHECK(receive_all(fd, header, sizeof header) == 0 && !memcmp(header + 32, test_identity, 16));
    EMU_CHECK(receive_all(fd, payload, get32(header + 8)) == 0);
    for (int i = 0; i < 3; i++)
        EMU_CHECK(receive_message(fd, &type, &sequence, &icount, payload, sizeof payload, &size) == 0);
    stats.ticks++; stats.icount++; stats.measured_ns += 10000000;
    EMU_CHECK(emu_runtime_state_publish(runtime, &stats, 1) == 0);
    emu_ui_socket_stop_input(&ui);
    EMU_CHECK(emu_input_down_mask(ui.input) == 0 && emu_input_pending_mask(ui.input) == 0);
    EMU_CHECK(emu_runtime_state_transition(runtime, EMU_RUNTIME_STOPPED, "limit", "test bound") == 0);
    emu_runtime_state_snapshot(runtime, &stats);
    emu_ui_socket_drain(&ui, &stats);
    EMU_CHECK(receive_message(fd, &type, &sequence, &icount, payload, sizeof payload, &size) == 0);
    EMU_CHECK(type == EMU_UI_LIFECYCLE && payload[0] == EMU_RUNTIME_STOPPED &&
              payload[1] == 5 && !memcmp(payload + 4, "limit", 5));
    EMU_CHECK(receive_message(fd, &type, &sequence, &icount, payload, sizeof payload, &size) == 0);
    EMU_CHECK(type == EMU_UI_STATS && get64(payload + 28) == 102 && get64(payload + 8) == 101);
    /* A receiver that stops reading cannot keep normal termination alive. */
    int buffer_size = 1024;
    EMU_CHECK(setsockopt(ui.client_fd, SOL_SOCKET, SO_SNDBUF, &buffer_size, sizeof buffer_size) == 0);
    uint8_t *serial = calloc(1024 * 1024, 1);
    EMU_CHECK(serial != NULL);
    append_serial(&ui, serial, 1024 * 1024);
    uint64_t start = emu_runtime_monotonic_ns();
    emu_ui_socket_drain(&ui, &stats);
    uint64_t duration = emu_runtime_monotonic_ns() - start;
    EMU_CHECK(duration >= UINT64_C(90000000) && duration < UINT64_C(200000000));
    EMU_CHECK(ui.tx_off > 0 && ui.tx_off < ui.tx_len);
    /* PTY-owned serial data is not pending UI traffic. */
    ui.asc0_available = 0;
    ui.tx_len = ui.tx_off = 0;
    start = emu_runtime_monotonic_ns();
    emu_ui_socket_drain(&ui, &stats);
    EMU_CHECK(emu_runtime_monotonic_ns() - start < UINT64_C(50000000));
    close(fd);
    close_transport(&ui);
    free(serial);
}

int main(void) {
    emu_prepared_session_t prepared;
    emu_test_prepared(&prepared);
    emu_error_t error = {0};
    emu_engine_registry_t registry = {0};
    EMU_CHECK(emu_registry_register(
                  &registry, &emu_fake_engine_descriptor, &error) == EMU_OK);

    emu_ui_socket_t ui;
    test_callbacks_t callbacks = {.ui = &ui};
    emu_callbacks_t callback_set = {
        .opaque = &callbacks,
        .frame = frame_callback,
        .serial = serial_callback,
    };
    emu_session_t *session = NULL;
    EMU_CHECK(emu_session_create(&registry, "fake", &prepared, &callback_set,
                                  &session, &error) == EMU_OK);

    static const char *const keys[] = {"power", "soft_left"};
    test_binding_t binding = {
        .session = {.session = session, .keys = keys, .key_count = 2},
        .observations = &callbacks,
    };
    emu_ui_engine_t engine;
    EMU_CHECK(emu_ui_bind_session(&engine, &binding.session) == 0);
    EMU_CHECK(emu_ui_bind_session(NULL, &binding.session) == -1);

    char path[96];
    snprintf(path, sizeof path, "/tmp/emu-ui-fake-%ld.sock", (long)getpid());
    int open_result = open_transport(&ui, path, "fake", 1, 1, &engine);
    if (open_result != 0) perror(path);
    EMU_CHECK(open_result == 0);

    /* A frame produced before connection becomes the reconnect baseline. */
    EMU_CHECK(emu_session_poll(session, &error) == EMU_OK);
    int fd = connect_client(path);
    poll_transport(&ui, 7);

    uint8_t payload[EMU_UI_MAX_PAYLOAD];
    uint16_t type;
    uint32_t payload_size;
    uint64_t sequence, icount;
    EMU_CHECK(receive_message(fd, &type, &sequence, &icount, payload,
                              sizeof payload, &payload_size) == 0);
    EMU_CHECK(type == EMU_UI_HELLO && sequence == 1 && icount == 7);
    EMU_CHECK(payload_size == 29 && get16(payload) == 1 &&
              get16(payload + 2) == 1 && get16(payload + 4) == 2 &&
              get16(payload + 6) ==
                  (EMU_UI_CAP_ASC0 | EMU_UI_CAP_AUDIO) && payload[8] == 4 &&
              !memcmp(payload + 9, "fake", 4) && payload[13] == 5 &&
              !memcmp(payload + 14, "power", 5) && payload[19] == 9 &&
              !memcmp(payload + 20, "soft_left", 9));
    EMU_CHECK(receive_message(fd, &type, &sequence, &icount, payload,
                              sizeof payload, &payload_size) == 0);
    EMU_CHECK(type == EMU_UI_LIFECYCLE && sequence == 2 && payload_size == 4);
    EMU_CHECK(receive_message(fd, &type, &sequence, &icount, payload,
                              sizeof payload, &payload_size) == 0);
    EMU_CHECK(type == EMU_UI_STATS && sequence == 3 && icount == 7 &&
              payload_size == 72 && get64(payload) == 7000 &&
              get64(payload + 8) == 8 && get64(payload + 16) == 9 &&
              get32(payload + 24) == 0x800007);
    EMU_CHECK(receive_message(fd, &type, &sequence, &icount, payload,
                              sizeof payload, &payload_size) == 0);
    EMU_CHECK(type == EMU_UI_ASC0_SUBSCRIBED && sequence == 4 && payload_size == 16);
    EMU_CHECK(get64(payload) == ui.subscription && get64(payload + 8) == 0);
    EMU_CHECK(receive_message(fd, &type, &sequence, &icount, payload,
                              sizeof payload, &payload_size) == 0);
    EMU_CHECK(type == EMU_UI_AUDIO_RESET && sequence == 5 && payload_size == 0);
    EMU_CHECK(receive_message(fd, &type, &sequence, &icount, payload,
                              sizeof payload, &payload_size) == 0);
    EMU_CHECK(type == EMU_UI_FRAME && sequence == 6 && icount == 7 &&
              payload_size == 3 && payload[0] == 1 && payload[1] == 1);

    /* Protocol v13 carries only final 48-kHz mono speaker PCM. */
    const int16_t samples[] = {-32768, 0, 32767, 1};
    const emu_audio_output_t output = {
        .completion_tick = 201, .samples = samples, .frame_count = 4,
    };
    EMU_CHECK(emu_ui_socket_speaker_pcm(&ui, &output) == 0);
    poll_transport(&ui, 7);
    EMU_CHECK(receive_message(fd, &type, &sequence, &icount, payload,
                              sizeof payload, &payload_size) == 0);
    EMU_CHECK(type == EMU_UI_SPEAKER_PCM && icount == 201 &&
              payload_size == 16 && get32(payload) == 48000 &&
              get16(payload + 4) == 1 && get16(payload + 8) == 0x8000 &&
              get16(payload + 12) == 0x7fff);

    /* A tap remains requested until the fake engine samples it. */
    uint8_t packets[256], down[] = {1, 1}, key_index = 1;
    size_t first = make_message(packets, EMU_UI_KEY, down, sizeof down);
    size_t second = make_message(packets + first,
                                 EMU_UI_KEY_RELEASE_AFTER_SAMPLE,
                                 &key_index, sizeof key_index);
    EMU_CHECK(send_all(fd, packets, first + second) == 0);
    poll_transport(&ui, 8);
    emu_key_sample_t sample;
    EMU_CHECK(emu_session_key_query(session, "soft_left", &sample,
                                     &error) == EMU_OK);
    EMU_CHECK(sample.requested_pressed && !sample.sampled_pressed);
    EMU_CHECK(callbacks.deferred_count == 1 &&
              !strcmp(callbacks.deferred[0], "queued"));
    EMU_CHECK(emu_session_poll(session, &error) == EMU_OK);
    poll_transport(&ui, 9);
    EMU_CHECK(emu_session_key_query(session, "soft_left", &sample,
                                     &error) == EMU_OK);
    EMU_CHECK(!sample.requested_pressed && sample.sampled_pressed);
    EMU_CHECK(callbacks.deferred_count == 2 &&
              !strcmp(callbacks.deferred[1], "sampled"));
    EMU_CHECK(receive_message(fd, &type, &sequence, &icount, payload,
                              sizeof payload, &payload_size) == 0);
    EMU_CHECK(type == EMU_UI_FRAME && payload_size == 3 &&
              payload[0] == 1 && payload[1] == 2);

    /* ASC0 attachment, readiness, RX, and echoed TX use session controls. */
    size_t size = make_message(packets, EMU_UI_ASC0_OPEN, NULL, 0);
    EMU_CHECK(send_all(fd, packets, size) == 0);
    poll_transport(&ui, 10);
    EMU_CHECK(receive_message(fd, &type, &sequence, &icount, payload,
                              sizeof payload, &payload_size) == 0);
    EMU_CHECK(type == EMU_UI_ASC0_READY && payload_size == 16 &&
              get64(payload) == ui.subscription && get64(payload + 8) == 0);
    emu_serial_link_state_t link;
    EMU_CHECK(emu_session_serial_link(session, EMU_SERIAL_LINK_UI,
                                       &link, &error) == EMU_OK);
    EMU_CHECK(link.attachment == EMU_SERIAL_LINK_UI);
    size = make_message(packets, EMU_UI_ASC0_RX, "OK", 2);
    EMU_CHECK(send_all(fd, packets, size) == 0);
    poll_transport(&ui, 11);
    EMU_CHECK(emu_session_poll(session, &error) == EMU_OK);
    poll_transport(&ui, 12);
    EMU_CHECK(receive_message(fd, &type, &sequence, &icount, payload,
                              sizeof payload, &payload_size) == 0);
    EMU_CHECK(type == EMU_UI_ASC0_TX && payload_size == 18 &&
              !memcmp(payload + 16, "OK", 2));
    EMU_CHECK(receive_message(fd, &type, &sequence, &icount, payload,
                              sizeof payload, &payload_size) == 0);
    EMU_CHECK(type == EMU_UI_FRAME && payload_size == 3 &&
              payload[0] == 1 && payload[1] == 3);
    EMU_CHECK(emu_session_serial_link(session, EMU_SERIAL_LINK_UI,
                                       &link, &error) == EMU_OK);
    EMU_CHECK(link.rx_bytes == 2 && link.tx_bytes == 2);

    size = make_message(packets, EMU_UI_ASC0_CLOSE, NULL, 0);
    EMU_CHECK(send_all(fd, packets, size) == 0);
    poll_transport(&ui, 13);
    EMU_CHECK(emu_session_serial_link(session, EMU_SERIAL_LINK_DETACHED,
                                       &link, &error) == EMU_OK);
    EMU_CHECK(link.attachment == EMU_SERIAL_LINK_DETACHED);

    /* Invalid framing gets the stable v13 rejection and drops the client. */
    size = make_message(packets, EMU_UI_RELEASE_ALL, NULL, 0);
    packets[4] = (uint8_t)(EMU_UI_VERSION + 1);
    EMU_CHECK(send_all(fd, packets, size) == 0);
    poll_transport(&ui, 14);
    EMU_CHECK(receive_message(fd, &type, &sequence, &icount, payload,
                              sizeof payload, &payload_size) == 0);
    EMU_CHECK(type == EMU_UI_ERROR && payload_size == 24 &&
              !memcmp(payload, "invalid protocol message", 24));
    EMU_CHECK(ui.client_fd == -1);
    close(fd);

    /* Reconnect restarts only connection state and re-advertises latest frame. */
    fd = connect_client(path);
    poll_transport(&ui, 15);
    EMU_CHECK(receive_message(fd, &type, &sequence, &icount, payload,
                              sizeof payload, &payload_size) == 0);
    EMU_CHECK(type == EMU_UI_HELLO && ui.connections == 2);
    close(fd);
    poll_transport(&ui, 16);

    close_transport(&ui);
    test_key_cleanup(&engine, session);
    test_owner_protocol(&engine, session);
    test_serial_backpressure(&engine);
    test_serial_replay(&engine);
    test_serial_invalid_requests(&engine);
    test_runtime_required(&engine);
    test_runtime_publication_only(&engine);
    test_runtime_delivery(&engine);
    emu_session_destroy(&session);
    emu_prepared_free(&prepared);
    EMU_CHECK(emu_fake_live_sessions() == 0);
    puts("fake engine protocol v13 transport: PASS");
    return 0;
}
