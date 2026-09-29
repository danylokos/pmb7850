#ifndef EMU_INPUT_H
#define EMU_INPUT_H

#include <stddef.h>
#include <stdint.h>

#define EMU_INPUT_MAX_KEYS 32u
#define EMU_INPUT_KEY_NAME_MAX 31u
#define EMU_INPUT_POLL_TICKS UINT64_C(50000)
typedef uint64_t emu_input_handle_t;
typedef struct emu_input emu_input_t;
typedef enum {
    EMU_INPUT_RELEASE = 0, EMU_INPUT_PRESS = 1, EMU_INPUT_RELEASE_SAMPLED = 2
} emu_input_action_t;
typedef struct {
    uint64_t icount;
    uint32_t pc;
    emu_input_handle_t owner;
    size_t key;
    const char *name, *action;
    unsigned holders;
    int down, pending, changed, result;
} emu_input_event_t;
typedef struct {
    void *opaque;
    int (*set)(void *, const char *, int);
    int (*sampled)(void *, const char *, int *);
    void (*trace)(void *, const emu_input_event_t *);
} emu_input_callbacks_t;
typedef struct {
    unsigned holders;
    emu_input_handle_t pending_owner;
    int down, last_error;
} emu_input_key_state_t;

/* Definitions are copied. Creating the service never touches guest input.
 * Handles are unique within an invocation and are never reused. Calls are
 * serialized by the host. Backend failures leave ownership intact for retry;
 * last_error and trace retain the failing transition. */
emu_input_t *emu_input_create(const char *const *, size_t, const emu_input_callbacks_t *);
int emu_input_destroy(emu_input_t *);
void emu_input_context(emu_input_t *, uint64_t, uint32_t);
emu_input_handle_t emu_input_scope_open(emu_input_t *);
emu_input_handle_t emu_input_owner_open(emu_input_t *, emu_input_handle_t);
int emu_input_key(emu_input_t *, emu_input_handle_t, size_t, emu_input_action_t);
int emu_input_owner_release_all(emu_input_t *, emu_input_handle_t);
int emu_input_owner_close(emu_input_t *, emu_input_handle_t);
int emu_input_scope_release_all(emu_input_t *, emu_input_handle_t);
int emu_input_scope_close(emu_input_t *, emu_input_handle_t);
/* Reapply current host electrical state after a guest checkpoint restore. */
int emu_input_reconcile(emu_input_t *);
int emu_input_poll(emu_input_t *);
int emu_input_pending(const emu_input_t *);
int emu_input_query(const emu_input_t *, size_t, emu_input_key_state_t *);
int emu_input_owner_holds(const emu_input_t *, emu_input_handle_t, size_t);
uint32_t emu_input_down_mask(const emu_input_t *);
uint32_t emu_input_pending_mask(const emu_input_t *);
#endif
