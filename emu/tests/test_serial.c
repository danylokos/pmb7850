#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

#include "fake_engine.h"
#include "test_support.h"
#include "emu_serial.h"

typedef struct {
    uint8_t bytes[64];
    size_t size;
} serial_output_t;

typedef struct {
    int attachment;
} failing_engine_t;

static int failing_rx(void *opaque, const uint8_t *bytes, size_t size) {
    (void)opaque;
    (void)bytes;
    (void)size;
    return -1;
}

static int track_link(void *opaque,
                      emu_serial_link_attachment_t attachment) {
    ((failing_engine_t *)opaque)->attachment = (int)attachment;
    return 0;
}

static void serial_callback(void *opaque, const uint8_t *bytes, size_t size) {
    serial_output_t *output = opaque;
    EMU_CHECK(output->size + size <= sizeof output->bytes);
    memcpy(output->bytes + output->size, bytes, size);
    output->size += size;
}

static void test_console(void) {
    uint8_t bytes[128] = "boot E";
    size_t length = strlen((char *)bytes);
    emu_serial_exit_tracker_t tracker;
    emu_serial_exit_tracker_init(&tracker, 0);
    char record[64];
    EMU_CHECK(emu_serial_exit_tracker_scan(
                  &tracker, bytes, length, record, sizeof record) == 0);
    memcpy(bytes + length, "XIT: B102", 9);
    length += 9;
    EMU_CHECK(emu_serial_exit_tracker_scan(
                  &tracker, bytes, length, record, sizeof record) == 0);
    memcpy(bytes + length, " 08 0095\r", 9);
    length += 9;
    EMU_CHECK(emu_serial_exit_tracker_scan(
                  &tracker, bytes, length, record, sizeof record) == 1);
    EMU_CHECK(!strcmp(record, "EXIT: B102 08 0095"));

    const uint8_t raw[] = {'A', '\n', 0, 0x7f, 'Z'};
    char escaped[32];
    EMU_CHECK(emu_serial_console_escape(
                  escaped, sizeof escaped, raw, sizeof raw) == 14);
    EMU_CHECK(!strcmp(escaped, "A\\x0A\\x00\\x7FZ"));

    emu_serial_console_t console;
    emu_serial_console_init(&console, 0);
    char *chunk = NULL;
    EMU_CHECK(emu_serial_console_take(
                  &console, (const uint8_t *)"partial", 7, 0, &chunk) == 0);
    EMU_CHECK(!chunk && console.offset == 0);
    const uint8_t line[] = "partial\rnext";
    EMU_CHECK(emu_serial_console_take(
                  &console, line, sizeof line - 1u, 0, &chunk) == 1);
    EMU_CHECK(!strcmp(chunk, "partial\\x0Dnext"));
    free(chunk);
    chunk = NULL;
    EMU_CHECK(emu_serial_console_take(
                  &console, line, sizeof line - 1u, 1, &chunk) == 0);
}

static void test_pty(void) {
    emu_prepared_session_t prepared;
    emu_test_prepared(&prepared);
    emu_error_t error = {0};
    emu_engine_registry_t registry = {0};
    EMU_CHECK(emu_registry_register(
                  &registry, &emu_fake_engine_descriptor, &error) == EMU_OK);
    serial_output_t output = {0};
    emu_callbacks_t callbacks = {
        .opaque = &output,
        .serial = serial_callback,
    };
    emu_session_t *session = NULL;
    EMU_CHECK(emu_session_create(&registry, "fake", &prepared, &callbacks,
                                  &session, &error) == EMU_OK);
    emu_serial_session_binding_t binding = {.session = session};
    emu_serial_engine_t engine;
    EMU_CHECK(emu_serial_bind_session(&engine, &binding) == 0);

    char path[96];
    snprintf(path, sizeof path, "/tmp/x55-serial-%ld.pty", (long)getpid());
    char message[256];
    emu_serial_pty_t pty;
    EMU_CHECK(emu_serial_pty_open(
                  &pty, path, &engine, message, sizeof message) == 0);
    struct stat st;
    EMU_CHECK(lstat(path, &st) == 0 && S_ISLNK(st.st_mode));
    int slave = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
    EMU_CHECK(slave >= 0);
    struct termios attributes;
    EMU_CHECK(tcgetattr(slave, &attributes) == 0);
    EMU_CHECK(!(attributes.c_lflag & (ICANON | ECHO)));

    emu_serial_link_state_t link;
    EMU_CHECK(emu_session_serial_link(session, EMU_SERIAL_LINK_HOST,
                                       &link, &error) == EMU_OK);
    EMU_CHECK(link.attachment == EMU_SERIAL_LINK_HOST);
    EMU_CHECK(write(slave, "ping", 4) == 4);
    EMU_CHECK(emu_serial_pty_poll(&pty, NULL, 0) == 0);
    EMU_CHECK(emu_session_poll(session, &error) == EMU_OK);
    EMU_CHECK(output.size == 4 && !memcmp(output.bytes, "ping", 4));
    EMU_CHECK(emu_serial_pty_poll(&pty, output.bytes, output.size) == 0);
    uint8_t echoed[4];
    EMU_CHECK(read(slave, echoed, sizeof echoed) == 4);
    EMU_CHECK(!memcmp(echoed, "ping", 4));

    size_t large_size = 1024u * 1024u;
    uint8_t *large = malloc(large_size);
    EMU_CHECK(large != NULL);
    memcpy(large, output.bytes, output.size);
    memset(large + output.size, 0x5a, large_size - output.size);
    emu_serial_history_t *history = emu_serial_history_create(NULL);
    EMU_CHECK(history != NULL && !emu_serial_history_append(history, large, large_size));
    uint64_t subscription = emu_serial_history_subscribe(history, output.size), cursor;
    EMU_CHECK(emu_serial_history_pty_poll(history, subscription, &pty) == 0);
    EMU_CHECK(!emu_serial_history_bounds(history, subscription, NULL, &cursor, NULL));
    EMU_CHECK(cursor > output.size && cursor < large_size);
    size_t received = output.size;
    uint8_t chunk[8192];
    for (unsigned tries = 0; received < large_size && tries < 10000; tries++) {
        ssize_t count = read(slave, chunk, sizeof chunk);
        if (count > 0) {
            EMU_CHECK((size_t)count <= large_size - received);
            EMU_CHECK(!memcmp(chunk, large + received, (size_t)count));
            received += (size_t)count;
        } else EMU_CHECK(count == -1 && (errno == EAGAIN || errno == EWOULDBLOCK));
        EMU_CHECK(emu_serial_history_pty_poll(history, subscription, &pty) == 0);
    }
    EMU_CHECK(received == large_size);
    close(slave);
    EMU_CHECK(emu_serial_history_pty_poll(history, subscription, &pty) == 0);
    emu_serial_history_destroy(history);
    free(large);
    emu_serial_pty_close(&pty);
    EMU_CHECK(lstat(path, &st) != 0 && errno == ENOENT);
    EMU_CHECK(emu_session_serial_link(session, EMU_SERIAL_LINK_DETACHED,
                                       &link, &error) == EMU_OK);
    EMU_CHECK(link.attachment == EMU_SERIAL_LINK_DETACHED);

    int collision = open(path, O_CREAT | O_EXCL | O_WRONLY, 0600);
    EMU_CHECK(collision >= 0);
    close(collision);
    EMU_CHECK(emu_serial_pty_open(
                  &pty, path, &engine, message, sizeof message) == -1);
    EMU_CHECK(strstr(message, "PTY link already exists") != NULL);
    EMU_CHECK(unlink(path) == 0);

    failing_engine_t failing = {0};
    emu_serial_engine_t failing_engine = {
        .opaque = &failing,
        .rx = failing_rx,
        .link = track_link,
    };
    EMU_CHECK(emu_serial_pty_open(
                  &pty, path, &failing_engine, message, sizeof message) == 0);
    EMU_CHECK(failing.attachment == EMU_SERIAL_LINK_HOST);
    slave = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
    EMU_CHECK(slave >= 0 && write(slave, "x", 1) == 1);
    EMU_CHECK(emu_serial_pty_poll(&pty, NULL, 0) == -1 && errno == EIO);
    close(slave);
    emu_serial_pty_close(&pty);
    EMU_CHECK(failing.attachment == EMU_SERIAL_LINK_DETACHED);
    EMU_CHECK(lstat(path, &st) != 0 && errno == ENOENT);

    emu_session_destroy(&session);
    emu_prepared_free(&prepared);
    EMU_CHECK(emu_fake_live_sessions() == 0);
}

typedef struct {
    int fail, allocations, frees, events;
    emu_serial_event_t last;
} history_test_t;
static void *test_resize(void *opaque, void *ptr, size_t size) {
    history_test_t *t = opaque;
    if (t->fail) return NULL;
    if (!ptr) t->allocations++;
    return realloc(ptr, size);
}
static void test_release(void *opaque, void *ptr) {
    if (ptr) ((history_test_t *)opaque)->frees++;
    free(ptr);
}
static void history_trace(void *opaque, const emu_serial_event_t *event) {
    history_test_t *t = opaque;
    t->events++; t->last = *event;
}
static void test_history(void) {
    history_test_t t = {0};
    emu_serial_history_callbacks_t cb = {&t, test_resize, test_release, history_trace};
    emu_serial_history_t *s = emu_serial_history_create(&cb);
    EMU_CHECK(s != NULL);
    emu_serial_history_context(s, 123, 0xABCDEF);
    uint64_t a = emu_serial_history_subscribe_live(s);
    EMU_CHECK(a != 0 && t.last.icount == 123 && t.last.pc == 0xABCDEF);
    emu_serial_view_t v = {0};
    EMU_CHECK(emu_serial_history_read(s, a, 0, 0, SIZE_MAX, &v) == 0 && !v.size);
    EMU_CHECK(emu_serial_history_append(s, (const uint8_t *)"old", 3) == 0);
    uint64_t b = emu_serial_history_subscribe_live(s), c = emu_serial_history_subscribe(s, 1);
    EMU_CHECK(a != b && b != c && a != c);
    EMU_CHECK(emu_serial_history_peek(s, a, 2, &v) == 0 && v.size == 2 && !memcmp(v.data, "ol", 2));
    EMU_CHECK(emu_serial_history_advance(s, a, 1) == 0);
    EMU_CHECK(emu_serial_history_peek(s, a, SIZE_MAX, &v) == 0 && v.start == 1 && v.size == 2);
    EMU_CHECK(emu_serial_history_peek(s, b, SIZE_MAX, &v) == 0 && v.start == 3 && !v.size);
    for (int i = 0; i < 3; i++) {
        EMU_CHECK(emu_serial_history_read(s, a, 0, 3, SIZE_MAX, &v) == 0 && !memcmp(v.data, "old", 3));
        uint64_t cursor;
        EMU_CHECK(emu_serial_history_bounds(s, a, NULL, &cursor, NULL) == 0 && cursor == 1);
    }
    EMU_CHECK(emu_serial_history_read(s, b, 0, 3, 0, &v) == -1);
    EMU_CHECK(emu_serial_history_read(s, a, 3, 2, 0, &v) == -1);
    EMU_CHECK(emu_serial_history_read(s, a, 0, UINT64_MAX, 0, &v) == -1);
    EMU_CHECK(emu_serial_history_read(s, a, 0, 0, 0, NULL) == -1);
    EMU_CHECK(emu_serial_history_advance(s, a, UINT64_MAX) == -1);
    EMU_CHECK(emu_serial_history_append(s, (const uint8_t *)"x", SIZE_MAX) == -1 && errno == EOVERFLOW);
    EMU_CHECK(emu_serial_history_append(s, NULL, 1) == -1);
    EMU_CHECK(emu_serial_history_subscribe(s, UINT64_MAX) == 0);
    EMU_CHECK(emu_serial_history_close(s, c) == 0);
    EMU_CHECK(emu_serial_history_close(s, c) == -1);
    EMU_CHECK(emu_serial_history_peek(s, c, 0, &v) == -1);
    EMU_CHECK(emu_serial_history_bounds(s, c, NULL, NULL, NULL) == -1);
    EMU_CHECK(emu_serial_history_advance(s, c, 0) == -1);
    EMU_CHECK(emu_serial_history_subscribe(s, 0) > c);
    size_t n = 2 * 1024 * 1024 + 17;
    uint8_t *bytes = malloc(n);
    EMU_CHECK(bytes != NULL);
    for (size_t i = 0; i < n; i++) bytes[i] = (uint8_t)(i * 43 + i / 251);
    t.fail = 1;
    EMU_CHECK(emu_serial_history_subscribe_live(s) == 0);
    EMU_CHECK(emu_serial_history_append(s, bytes, n) == -1 && errno == ENOMEM);
    EMU_CHECK(emu_serial_history_tail(s) == 3);
    EMU_CHECK(emu_serial_history_peek(s, a, SIZE_MAX, &v) == 0 && v.start == 1 && v.size == 2);
    t.fail = 0;
    EMU_CHECK(emu_serial_history_append(s, bytes, n) == 0);
    for (uint64_t offset = 3; offset < n + 3;) {
        EMU_CHECK(emu_serial_history_read(s, b, offset, n + 3, 7001, &v) == 0);
        EMU_CHECK(v.size && !memcmp(v.data, bytes + offset - 3, v.size));
        offset = v.next;
    }
    EMU_CHECK(emu_serial_history_peek(s, b, SIZE_MAX, &v) == 0 && v.size == n && v.start == 3);
    EMU_CHECK(emu_serial_history_advance(s, b, n) == 0);
    EMU_CHECK(emu_serial_history_read(s, b, n + 3, n + 3, SIZE_MAX, &v) == 0 && !v.size);
    EMU_CHECK(emu_serial_history_read(s, a, 3, n + 3, SIZE_MAX, &v) == 0);
    EMU_CHECK(emu_serial_history_append(s, v.data, v.size) == 0);
    EMU_CHECK(emu_serial_history_read(s, a, n + 3, 2 * n + 3, SIZE_MAX, &v) == 0 && !memcmp(v.data, bytes, n));
    emu_serial_history_destroy(s);
    EMU_CHECK(t.allocations == t.frees && t.events > 300);
    free(bytes);

    s = emu_serial_history_create(NULL);
    a = emu_serial_history_subscribe_live(s);
    char *escaped = NULL;
    EMU_CHECK(emu_serial_history_append(s, (const uint8_t *)"abc", 3) == 0);
    EMU_CHECK(emu_serial_history_console_take(s, a, 0, &escaped) == 0 && !escaped);
    EMU_CHECK(emu_serial_history_append(s, (const uint8_t *)"\n", 1) == 0);
    EMU_CHECK(emu_serial_history_console_take(s, a, 0, &escaped) == 1 && !strcmp(escaped, "abc\\x0A"));
    free(escaped);
    EMU_CHECK(emu_serial_history_console_take(s, a, 1, &escaped) == 0 && !escaped);
    emu_serial_history_destroy(s);
}

int main(void) {
    test_history();
    test_console();
    test_pty();
    puts("host serial PTY and console policy: PASS");
    return 0;
}
