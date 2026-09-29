#ifndef EMU_SERIAL_H
#define EMU_SERIAL_H

#include <stddef.h>
#include <stdint.h>

#include "emu_engine.h"

/* Invocation-owned, append-only raw history. Calls are serialized by the host.
 * Handles are nonzero, service-local and never reused. Views remain valid until
 * append/destruction; range reads never consume a subscription's live cursor. */
typedef struct emu_serial_history emu_serial_history_t;
typedef uint64_t emu_serial_subscription_t;
typedef struct {
    const uint8_t *data;
    size_t size;
    uint64_t start, next;
} emu_serial_view_t;
typedef struct {
    uint64_t icount, subscription, start, end, boundary, cursor, tail;
    uint32_t pc;
    const char *action;
    int changed, result;
} emu_serial_event_t;
typedef struct {
    void *opaque;
    /* Optional paired allocator, primarily for embedding and fault injection. */
    void *(*resize)(void *, void *, size_t);
    void (*release)(void *, void *);
    void (*trace)(void *, const emu_serial_event_t *);
} emu_serial_history_callbacks_t;
emu_serial_history_t *emu_serial_history_create(const emu_serial_history_callbacks_t *);
void emu_serial_history_destroy(emu_serial_history_t *);
void emu_serial_history_context(emu_serial_history_t *, uint64_t, uint32_t);
void emu_serial_history_trace(emu_serial_history_t *,
                              void (*)(void *, const emu_serial_event_t *), void *);
int emu_serial_history_append(emu_serial_history_t *, const uint8_t *, size_t);
uint64_t emu_serial_history_tail(const emu_serial_history_t *);
emu_serial_subscription_t emu_serial_history_subscribe(emu_serial_history_t *, uint64_t);
emu_serial_subscription_t emu_serial_history_subscribe_live(emu_serial_history_t *);
int emu_serial_history_close(emu_serial_history_t *, emu_serial_subscription_t);
int emu_serial_history_bounds(emu_serial_history_t *, emu_serial_subscription_t,
                              uint64_t *, uint64_t *, uint64_t *);
int emu_serial_history_read(emu_serial_history_t *, emu_serial_subscription_t,
                            uint64_t, uint64_t, size_t, emu_serial_view_t *);
int emu_serial_history_peek(emu_serial_history_t *, emu_serial_subscription_t,
                            size_t, emu_serial_view_t *);
int emu_serial_history_advance(emu_serial_history_t *, emu_serial_subscription_t, uint64_t);
int emu_serial_history_console_take(emu_serial_history_t *, emu_serial_subscription_t,
                                    int, char **);

#define EMU_SERIAL_EXIT_RECORD_MAX 512u

typedef struct {
    size_t offset;
    size_t prefix_len;
    size_t record_len;
    size_t payload_len;
    int invalid;
    char record[EMU_SERIAL_EXIT_RECORD_MAX];
} emu_serial_exit_tracker_t;

typedef struct {
    size_t offset;
} emu_serial_console_t;

typedef struct {
    void *opaque;
    int (*rx)(void *, const uint8_t *, size_t);
    int (*link)(void *, emu_serial_link_attachment_t);
} emu_serial_engine_t;

typedef struct {
    emu_session_t *session;
} emu_serial_session_binding_t;

typedef struct {
    int master_fd;
    char link_path[512];
    char slave_path[512];
    size_t tx_offset;
    int owns_link;
    int link_attached;
    emu_serial_engine_t engine;
} emu_serial_pty_t;

void emu_serial_exit_tracker_init(emu_serial_exit_tracker_t *, size_t);
int emu_serial_exit_tracker_scan(emu_serial_exit_tracker_t *,
                                 const uint8_t *, size_t, char *, size_t);
size_t emu_serial_console_escape(char *, size_t, const uint8_t *, size_t);
void emu_serial_console_init(emu_serial_console_t *, size_t);
int emu_serial_console_take(emu_serial_console_t *, const uint8_t *, size_t,
                            int, char **);

int emu_serial_bind_session(emu_serial_engine_t *,
                            emu_serial_session_binding_t *);
int emu_serial_pty_open(emu_serial_pty_t *, const char *,
                        const emu_serial_engine_t *, char *, size_t);
int emu_serial_pty_poll(emu_serial_pty_t *, const uint8_t *, size_t);
int emu_serial_history_pty_poll(emu_serial_history_t *, emu_serial_subscription_t, emu_serial_pty_t *);
void emu_serial_pty_close(emu_serial_pty_t *);

#endif
