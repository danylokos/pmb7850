#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif
#define _DEFAULT_SOURCE
#define _XOPEN_SOURCE 700

#include "emu_serial.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

static const char EXIT_PREFIX[] = "EXIT:";

static int is_printable(uint8_t byte) {
    return byte >= 0x20u && byte <= 0x7eu;
}

static int is_control(uint8_t byte) {
    return byte < 0x20u || byte == 0x7fu;
}

static void reset_candidate(emu_serial_exit_tracker_t *tracker) {
    tracker->prefix_len = 0;
    tracker->record_len = 0;
    tracker->payload_len = 0;
    tracker->invalid = 0;
    tracker->record[0] = '\0';
}

void emu_serial_exit_tracker_init(emu_serial_exit_tracker_t *tracker,
                                  size_t baseline) {
    if (!tracker) return;
    memset(tracker, 0, sizeof *tracker);
    tracker->offset = baseline;
}

int emu_serial_exit_tracker_scan(emu_serial_exit_tracker_t *tracker,
                                 const uint8_t *bytes, size_t length,
                                 char *record, size_t record_cap) {
    if (!tracker || (!bytes && length)) return -1;
    if (length < tracker->offset) {
        emu_serial_exit_tracker_init(tracker, length);
        return 0;
    }
    while (tracker->offset < length) {
        uint8_t byte = bytes[tracker->offset++];
        if (tracker->prefix_len == sizeof EXIT_PREFIX - 1u) {
            if (is_printable(byte)) {
                tracker->payload_len++;
                if (tracker->record_len + 1u < sizeof tracker->record)
                    tracker->record[tracker->record_len++] = (char)byte;
                else
                    tracker->invalid = 1;
                continue;
            }
            if (is_control(byte) && tracker->payload_len && !tracker->invalid) {
                tracker->record[tracker->record_len] = '\0';
                if (record_cap)
                    snprintf(record, record_cap, "%s", tracker->record);
                reset_candidate(tracker);
                return 1;
            }
            reset_candidate(tracker);
            continue;
        }
        if (byte == (uint8_t)EXIT_PREFIX[tracker->prefix_len]) {
            if (tracker->record_len + 1u < sizeof tracker->record)
                tracker->record[tracker->record_len++] = (char)byte;
            else
                tracker->invalid = 1;
            tracker->prefix_len++;
        } else {
            reset_candidate(tracker);
            if (byte == (uint8_t)EXIT_PREFIX[0]) {
                tracker->record[0] = (char)byte;
                tracker->record_len = 1;
                tracker->prefix_len = 1;
            }
        }
    }
    return 0;
}

size_t emu_serial_console_escape(char *out, size_t cap,
                                 const uint8_t *bytes, size_t length) {
    static const char HEX[] = "0123456789ABCDEF";
    size_t needed = 0;
    for (size_t i = 0; i < length; i++) {
        uint8_t byte = bytes[i];
        if (is_printable(byte)) {
            if (needed + 1u < cap) out[needed] = (char)byte;
            needed++;
        } else {
            const char escaped[4] = {
                '\\', 'x', HEX[byte >> 4], HEX[byte & 0x0fu],
            };
            for (size_t j = 0; j < sizeof escaped; j++) {
                if (needed + 1u < cap) out[needed] = escaped[j];
                needed++;
            }
        }
    }
    if (cap) out[needed < cap ? needed : cap - 1u] = '\0';
    return needed;
}

void emu_serial_console_init(emu_serial_console_t *console, size_t baseline) {
    if (console) console->offset = baseline;
}

int emu_serial_console_take(emu_serial_console_t *console,
                            const uint8_t *bytes, size_t length,
                            int force, char **escaped) {
    if (!console || !escaped || (!bytes && length)) {
        errno = EINVAL;
        return -1;
    }
    *escaped = NULL;
    if (length < console->offset) console->offset = length;
    if (console->offset == length) return 0;
    if (!force) {
        int boundary = 0;
        for (size_t i = console->offset; i < length; i++)
            if (is_control(bytes[i])) { boundary = 1; break; }
        if (!boundary) return 0;
    }
    size_t size = length - console->offset;
    size_t escaped_size = emu_serial_console_escape(
        NULL, 0, bytes + console->offset, size);
    char *result = malloc(escaped_size + 1u);
    if (!result) return -1;
    emu_serial_console_escape(result, escaped_size + 1u,
                              bytes + console->offset, size);
    console->offset = length;
    *escaped = result;
    return 1;
}

static int session_rx(void *opaque, const uint8_t *bytes, size_t size) {
    emu_serial_session_binding_t *binding = opaque;
    emu_error_t error = {0};
    return emu_session_serial_rx(binding->session, bytes, size, &error) == EMU_OK
         ? 0 : -1;
}

static int session_link(void *opaque,
                        emu_serial_link_attachment_t attachment) {
    emu_serial_session_binding_t *binding = opaque;
    emu_serial_link_state_t state;
    emu_error_t error = {0};
    return emu_session_serial_link(binding->session, attachment, &state,
                                   &error) == EMU_OK ? 0 : -1;
}

int emu_serial_bind_session(emu_serial_engine_t *engine,
                            emu_serial_session_binding_t *binding) {
    if (!engine || !binding || !binding->session) return -1;
    *engine = (emu_serial_engine_t){binding, session_rx, session_link};
    return 0;
}

static int set_error(char *out, size_t cap, const char *operation,
                     const char *path) {
    if (out && cap)
        snprintf(out, cap, "%s %s: %s", operation, path ? path : "",
                 strerror(errno));
    return -1;
}

int emu_serial_pty_open(emu_serial_pty_t *pty, const char *link_path,
                        const emu_serial_engine_t *engine,
                        char *error, size_t error_cap) {
    if (!pty || !link_path || !link_path[0] || !engine || !engine->rx) {
        errno = EINVAL;
        return set_error(error, error_cap, "invalid PTY path", link_path);
    }
    memset(pty, 0, sizeof *pty);
    pty->master_fd = -1;
    struct stat st;
    if (lstat(link_path, &st) == 0 || errno != ENOENT) {
        errno = EEXIST;
        return set_error(error, error_cap, "PTY link already exists", link_path);
    }
    if (strlen(link_path) >= sizeof pty->link_path) {
        errno = ENAMETOOLONG;
        return set_error(error, error_cap, "PTY link path too long", link_path);
    }
    int fd = posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0)
        return set_error(error, error_cap, "cannot allocate PTY", link_path);
    if (grantpt(fd) != 0 || unlockpt(fd) != 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return set_error(error, error_cap, "cannot initialize PTY", link_path);
    }
    char *slave = ptsname(fd);
    if (!slave || strlen(slave) >= sizeof pty->slave_path) {
        int saved = slave ? ENAMETOOLONG : errno;
        close(fd);
        errno = saved;
        return set_error(error, error_cap, "cannot resolve PTY slave", link_path);
    }
    int slave_fd = open(slave, O_RDWR | O_NOCTTY);
    if (slave_fd < 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return set_error(error, error_cap, "cannot open PTY slave", slave);
    }
    struct termios attributes;
    if (tcgetattr(slave_fd, &attributes) != 0) {
        int saved = errno;
        close(slave_fd);
        close(fd);
        errno = saved;
        return set_error(error, error_cap, "cannot read PTY attributes", slave);
    }
    cfmakeraw(&attributes);
    if (tcsetattr(slave_fd, TCSANOW, &attributes) != 0) {
        int saved = errno;
        close(slave_fd);
        close(fd);
        errno = saved;
        return set_error(error, error_cap, "cannot set raw PTY mode", slave);
    }
    close(slave_fd);
    if (symlink(slave, link_path) != 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return set_error(error, error_cap, "cannot create PTY link", link_path);
    }
    pty->master_fd = fd;
    pty->owns_link = 1;
    memcpy(pty->link_path, link_path, strlen(link_path) + 1);
    memcpy(pty->slave_path, slave, strlen(slave) + 1);
    pty->engine = *engine;
    if (pty->engine.link &&
        pty->engine.link(pty->engine.opaque, EMU_SERIAL_LINK_HOST) != 0) {
        int saved = EIO;
        emu_serial_pty_close(pty);
        errno = saved;
        return set_error(error, error_cap, "cannot attach PTY link", link_path);
    }
    pty->link_attached = 1;
    return 0;
}

static int pty_read_input(emu_serial_pty_t *pty) {
    uint8_t input[4096];
    for (;;) {
        ssize_t count = read(pty->master_fd, input, sizeof input);
        if (count > 0) {
            if (pty->engine.rx(pty->engine.opaque, input, (size_t)count)) {
                errno = EIO;
                return -1;
            }
            continue;
        }
        if (count == 0 || errno == EAGAIN || errno == EWOULDBLOCK ||
            errno == EIO)
            break;
        return -1;
    }
    return 0;
}

static int pty_write_output(emu_serial_pty_t *pty, const uint8_t *tx, size_t tx_size) {
    if (pty->tx_offset > tx_size) pty->tx_offset = tx_size;
    while (pty->tx_offset < tx_size) {
        ssize_t count = write(pty->master_fd, tx + pty->tx_offset,
                              tx_size - pty->tx_offset);
        if (count > 0) {
            pty->tx_offset += (size_t)count;
            continue;
        }
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK ||
                          errno == EIO))
            break;
        return -1;
    }
    return 0;
}

int emu_serial_pty_poll(emu_serial_pty_t *pty, const uint8_t *tx, size_t tx_size) {
    if (!pty || pty->master_fd < 0 || (!tx && tx_size)) { errno = EINVAL; return -1; }
    if (pty_read_input(pty)) return -1;
    return pty_write_output(pty, tx, tx_size);
}

void emu_serial_pty_close(emu_serial_pty_t *pty) {
    if (!pty) return;
    if (pty->link_attached && pty->engine.link)
        (void)pty->engine.link(pty->engine.opaque, EMU_SERIAL_LINK_DETACHED);
    pty->link_attached = 0;
    if (pty->master_fd >= 0) close(pty->master_fd);
    pty->master_fd = -1;
    if (pty->owns_link && pty->link_path[0]) unlink(pty->link_path);
    pty->owns_link = 0;
}

/* Host serial history deliberately has no guest snapshot representation. */
typedef struct serial_subscription {
    struct serial_subscription *next;
    uint64_t id, boundary, cursor;
} serial_subscription_t;
struct emu_serial_history {
    uint8_t *bytes;
    size_t size, capacity;
    uint64_t next_id, icount;
    uint32_t pc;
    serial_subscription_t *subscriptions;
    emu_serial_history_callbacks_t cb;
    void *trace_opaque;
};
static void *history_resize(emu_serial_history_t *s, void *p, size_t n) {
    return s->cb.resize ? s->cb.resize(s->cb.opaque, p, n) : realloc(p, n);
}
static void history_free(emu_serial_history_t *s, void *p) {
    if (s->cb.release) s->cb.release(s->cb.opaque, p); else free(p);
}
static serial_subscription_t *subscription(emu_serial_history_t *s, uint64_t id) {
    for (serial_subscription_t *p = s ? s->subscriptions : NULL; p; p = p->next)
        if (p->id == id) return p;
    return NULL;
}
static int history_event(emu_serial_history_t *s, uint64_t id, const char *action,
                          uint64_t start, uint64_t end, int changed, int result) {
    if (s && s->cb.trace) {
        serial_subscription_t *p = subscription(s, id);
        emu_serial_event_t e = {.icount = s->icount, .pc = s->pc,
            .subscription = id, .action = action, .start = start, .end = end,
            .boundary = p ? p->boundary : 0, .cursor = p ? p->cursor : 0,
            .tail = s->size, .changed = changed, .result = result};
        s->cb.trace(s->trace_opaque, &e);
    }
    if (result) errno = result;
    return result ? -1 : 0;
}
emu_serial_history_t *emu_serial_history_create(const emu_serial_history_callbacks_t *cb) {
    if (cb && (!!cb->resize != !!cb->release)) return NULL;
    emu_serial_history_t *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    if (cb) { s->cb = *cb; s->trace_opaque = cb->opaque; }
    s->next_id = 1;
    return s;
}
void emu_serial_history_context(emu_serial_history_t *s, uint64_t icount, uint32_t pc) {
    if (s) { s->icount = icount; s->pc = pc; }
}
void emu_serial_history_trace(emu_serial_history_t *s,
                              void (*trace)(void *, const emu_serial_event_t *), void *opaque) {
    if (s) { s->cb.trace = trace; s->trace_opaque = opaque; }
}
uint64_t emu_serial_history_tail(const emu_serial_history_t *s) { return s ? s->size : 0; }
int emu_serial_history_append(emu_serial_history_t *s, const uint8_t *data, size_t n) {
    uint64_t start = s ? s->size : 0;
    if (!s || (!data && n)) return history_event(s, 0, "append", start, start, 0, EINVAL);
    if (n > SIZE_MAX - s->size || n > UINT64_MAX - start)
        return history_event(s, 0, "append", start, start, 0, EOVERFLOW);
    /* Appending a borrowed retained slice must survive realloc as well. */
    uintptr_t source = (uintptr_t)data, base = (uintptr_t)s->bytes;
    int retained = s->bytes && source >= base && source - base <= s->size;
    size_t source_offset = retained ? (size_t)(source - base) : 0;
    if (retained && n > s->size - source_offset)
        return history_event(s, 0, "append", start, start, 0, EINVAL);
    size_t needed = s->size + n;
    if (needed > s->capacity) {
        size_t capacity = s->capacity ? s->capacity : 4096u;
        while (capacity < needed) {
            if (capacity > SIZE_MAX / 2u) { capacity = needed; break; }
            capacity *= 2u;
        }
        uint8_t *bytes = history_resize(s, s->bytes, capacity);
        if (!bytes) return history_event(s, 0, "append", start, start + n, 0, ENOMEM);
        s->bytes = bytes; s->capacity = capacity;
    }
    if (retained) data = s->bytes + source_offset;
    if (n) memcpy(s->bytes + s->size, data, n);
    s->size = needed;
    return history_event(s, 0, "append", start, needed, n != 0, 0);
}
uint64_t emu_serial_history_subscribe(emu_serial_history_t *s, uint64_t start) {
    int error = !s || start > s->size ? EINVAL : !s->next_id ? EOVERFLOW : 0;
    serial_subscription_t *p = error ? NULL : history_resize(s, NULL, sizeof *p);
    if (!p) {
        history_event(s, 0, "subscribe", start, start, 0, error ? error : ENOMEM);
        return 0;
    }
    *p = (serial_subscription_t){s->subscriptions, s->next_id++, start, start};
    s->subscriptions = p;
    history_event(s, p->id, "subscribe", start, start, 1, 0);
    return p->id;
}
uint64_t emu_serial_history_subscribe_live(emu_serial_history_t *s) {
    return emu_serial_history_subscribe(s, emu_serial_history_tail(s));
}
int emu_serial_history_close(emu_serial_history_t *s, uint64_t id) {
    serial_subscription_t *p = subscription(s, id);
    if (!p) return history_event(s, id, "close", 0, 0, 0, EINVAL);
    history_event(s, id, "close", p->boundary, p->cursor, 1, 0);
    serial_subscription_t **link = &s->subscriptions;
    while (*link != p) link = &(*link)->next;
    *link = p->next; history_free(s, p);
    return 0;
}
void emu_serial_history_destroy(emu_serial_history_t *s) {
    if (!s) return;
    while (s->subscriptions) emu_serial_history_close(s, s->subscriptions->id);
    history_event(s, 0, "destroy", 0, s->size, 1, 0);
    history_free(s, s->bytes); free(s);
}
int emu_serial_history_bounds(emu_serial_history_t *s, uint64_t id,
                              uint64_t *boundary, uint64_t *cursor, uint64_t *tail) {
    serial_subscription_t *p = subscription(s, id);
    if (!p) return history_event(s, id, "bounds", 0, 0, 0, EINVAL);
    if (boundary) *boundary = p->boundary;
    if (cursor) *cursor = p->cursor;
    if (tail) *tail = s->size;
    return 0;
}
static int history_view(emu_serial_history_t *s, uint64_t id, uint64_t start,
                         uint64_t end, size_t max, emu_serial_view_t *view,
                         const char *action) {
    serial_subscription_t *p = subscription(s, id);
    if (!p || !view || start < p->boundary || start > end || end > s->size)
        return history_event(s, id, action, start, end, 0, EINVAL);
    size_t n = (size_t)(end - start);
    if (n > max) n = max;
    *view = (emu_serial_view_t){s->bytes ? s->bytes + (size_t)start : NULL, n, start, start + n};
    return history_event(s, id, action, start, start + n, 0, 0);
}
int emu_serial_history_read(emu_serial_history_t *s, uint64_t id, uint64_t start,
                            uint64_t end, size_t max, emu_serial_view_t *view) {
    return history_view(s, id, start, end, max, view, "read");
}
int emu_serial_history_peek(emu_serial_history_t *s, uint64_t id, size_t max,
                            emu_serial_view_t *view) {
    serial_subscription_t *p = subscription(s, id);
    return history_view(s, id, p ? p->cursor : 0, s ? s->size : 0, max, view, "peek");
}
int emu_serial_history_advance(emu_serial_history_t *s, uint64_t id, uint64_t count) {
    serial_subscription_t *p = subscription(s, id);
    uint64_t start = p ? p->cursor : 0;
    if (!p || count > s->size - start)
        return history_event(s, id, "advance", start, start, 0, EINVAL);
    p->cursor += count;
    return history_event(s, id, "advance", start, p->cursor, count != 0, 0);
}
int emu_serial_history_console_take(emu_serial_history_t *s, uint64_t id,
                                    int force, char **escaped) {
    emu_serial_view_t view;
    if (emu_serial_history_peek(s, id, SIZE_MAX, &view)) return -1;
    emu_serial_console_t console = {0};
    int rc = emu_serial_console_take(&console, view.data, view.size, force, escaped);
    if (rc > 0) emu_serial_history_advance(s, id, console.offset);
    return rc;
}
int emu_serial_history_pty_poll(emu_serial_history_t *s, uint64_t id, emu_serial_pty_t *pty) {
    emu_serial_view_t view;
    if (!pty || pty->master_fd < 0) { errno = EINVAL; return -1; }
    if (emu_serial_history_bounds(s, id, NULL, NULL, NULL)) return -1;
    if (pty_read_input(pty)) return -1;
    /* RX callbacks may append and relocate storage, so acquire the view afterward. */
    if (emu_serial_history_peek(s, id, SIZE_MAX, &view)) return -1;
    pty->tx_offset = 0;
    int rc = pty_write_output(pty, view.data, view.size);
    emu_serial_history_advance(s, id, pty->tx_offset);
    pty->tx_offset = 0;
    return rc;
}
