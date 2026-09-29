#ifndef EMU_RUNTIME_STATE_H
#define EMU_RUNTIME_STATE_H

#include <stddef.h>
#include <stdint.h>

#define EMU_RUNTIME_ID_SIZE 16u
#define EMU_RUNTIME_MAX_KEYS 32u

typedef struct {
    char model[32];
    unsigned width, height;
    size_t key_count;
    char keys[EMU_RUNTIME_MAX_KEYS][32];
    int asc0_available, audio_available;
} emu_runtime_descriptor_t;

typedef struct {
    uint64_t icount, elapsed_ns, ticks, guest_instructions;
    uint32_t pc;
    uint64_t sample_sequence, measured_ns, window_ns;
    double ticks_per_s, guest_instructions_per_s;
    uint32_t rates_valid;
} emu_runtime_snapshot_t;

typedef enum {
    EMU_RUNTIME_INITIALIZED = 0, EMU_RUNTIME_RUNNING, EMU_RUNTIME_STOPPED
} emu_runtime_phase_t;

typedef struct {
    emu_runtime_phase_t phase;
    char status[32], reason[704];
} emu_runtime_lifecycle_t;

typedef struct emu_runtime_state emu_runtime_state_t;
typedef enum { EMU_RUNTIME_LIFECYCLE, EMU_RUNTIME_SNAPSHOT } emu_runtime_event_t;
/* Synchronous callbacks may copy out state, but must not mutate/destroy it.
 * The owner serializes all calls. No socket or guest state is required. */
typedef void (*emu_runtime_callback_t)(void *, const emu_runtime_state_t *,
                                       emu_runtime_event_t);
/* NULL identity selects OS randomness. Non-NULL is for deterministic tests. */
emu_runtime_state_t *emu_runtime_state_create(const emu_runtime_descriptor_t *,
                                              const uint8_t *identity);
void emu_runtime_state_destroy(emu_runtime_state_t *);
void emu_runtime_state_subscribe(emu_runtime_state_t *, emu_runtime_callback_t, void *);
void emu_runtime_state_identity(const emu_runtime_state_t *, uint8_t[16]);
void emu_runtime_state_descriptor(const emu_runtime_state_t *, emu_runtime_descriptor_t *);
void emu_runtime_state_lifecycle(const emu_runtime_state_t *, emu_runtime_lifecycle_t *);
void emu_runtime_state_snapshot(const emu_runtime_state_t *, emu_runtime_snapshot_t *);
int emu_runtime_state_transition(emu_runtime_state_t *, emu_runtime_phase_t,
                                const char *status, const char *reason);
/* Deterministic host CLOCK_MONOTONIC input. Returns 0 published, 1 ineligible,
 * -1 invalid. Force is reserved for initial/final snapshots. Equal timestamps
 * replace the history endpoint; they never create a zero-duration window. */
int emu_runtime_state_publish(emu_runtime_state_t *, const emu_runtime_snapshot_t *, int force);
/* Clear rate history after guest rewind, preserving identity and sequence. */
int emu_runtime_state_rebase(emu_runtime_state_t *, const emu_runtime_snapshot_t *);
uint64_t emu_runtime_monotonic_ns(void);
#endif
