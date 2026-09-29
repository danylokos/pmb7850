#define _POSIX_C_SOURCE 200809L
#include "emu_runtime_state.h"
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

struct emu_runtime_state {
    uint8_t identity[16];
    emu_runtime_descriptor_t descriptor;
    emu_runtime_lifecycle_t lifecycle;
    emu_runtime_snapshot_t snapshot;
    /* 100 Hz plus the retained endpoint before the one-second cutoff. */
    emu_runtime_snapshot_t history[128];
    size_t first, count;
    emu_runtime_callback_t callback;
    void *opaque;
};

uint64_t emu_runtime_monotonic_ns(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) + now.tv_nsec;
}

emu_runtime_state_t *emu_runtime_state_create(
        const emu_runtime_descriptor_t *descriptor, const uint8_t *identity) {
    if (!descriptor || !descriptor->width || !descriptor->height ||
        descriptor->key_count > EMU_RUNTIME_MAX_KEYS || !descriptor->key_count ||
        !descriptor->model[0] || !memchr(descriptor->model, 0, 32)) return NULL;
    for (size_t i = 0; i < descriptor->key_count; i++)
        if (!descriptor->keys[i][0] || !memchr(descriptor->keys[i], 0, 32)) return NULL;
    emu_runtime_state_t *state = calloc(1, sizeof *state);
    if (!state) return NULL;
    state->descriptor = *descriptor;
    if (identity) memcpy(state->identity, identity, 16);
    else {
        int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
        if (fd < 0) { free(state); return NULL; }
        size_t used = 0;
        while (used < 16) {
            ssize_t n = read(fd, state->identity + used, 16 - used);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) { close(fd); free(state); return NULL; }
            used += (size_t)n;
        }
        close(fd);
    }
    return state;
}
void emu_runtime_state_destroy(emu_runtime_state_t *s) { free(s); }
void emu_runtime_state_subscribe(emu_runtime_state_t *s, emu_runtime_callback_t cb,
                                  void *opaque) {
    s->callback = cb;
    s->opaque = opaque;
}
void emu_runtime_state_identity(const emu_runtime_state_t *s, uint8_t id[16]) {
    memcpy(id, s->identity, 16);
}
void emu_runtime_state_descriptor(const emu_runtime_state_t *s,
                                   emu_runtime_descriptor_t *d) {
    *d = s->descriptor;
}
void emu_runtime_state_lifecycle(const emu_runtime_state_t *s,
                                  emu_runtime_lifecycle_t *l) {
    *l = s->lifecycle;
}
void emu_runtime_state_snapshot(const emu_runtime_state_t *s,
                                 emu_runtime_snapshot_t *v) {
    *v = s->snapshot;
}
int emu_runtime_state_transition(emu_runtime_state_t *s, emu_runtime_phase_t phase,
                                 const char *status, const char *reason) {
    if (!s || phase <= s->lifecycle.phase || phase > EMU_RUNTIME_STOPPED ||
        (status && strlen(status) >= sizeof s->lifecycle.status) ||
        (reason && strlen(reason) >= sizeof s->lifecycle.reason)) return -1;
    s->lifecycle.phase = phase;
    strcpy(s->lifecycle.status, status ? status : "");
    strcpy(s->lifecycle.reason, reason ? reason : "");
    if (s->callback) s->callback(s->opaque, s, EMU_RUNTIME_LIFECYCLE);
    return 0;
}
int emu_runtime_state_publish(emu_runtime_state_t *s, const emu_runtime_snapshot_t *v, int force) {
    if (!s || !v || s->lifecycle.phase == EMU_RUNTIME_STOPPED ||
        s->snapshot.sample_sequence == UINT64_MAX ||
        (s->snapshot.sample_sequence && (v->measured_ns < s->snapshot.measured_ns ||
         v->elapsed_ns < s->snapshot.elapsed_ns || v->ticks < s->snapshot.ticks ||
         v->icount < s->snapshot.icount ||
         v->guest_instructions < s->snapshot.guest_instructions))) return -1;
    if (!force && s->snapshot.sample_sequence &&
        v->measured_ns - s->snapshot.measured_ns < UINT64_C(10000000)) return 1;
    uint64_t sequence = s->snapshot.sample_sequence + 1;
    s->snapshot = *v;
    s->snapshot.sample_sequence = sequence;
    s->snapshot.window_ns = 0;
    s->snapshot.rates_valid = 0;
    s->snapshot.ticks_per_s = s->snapshot.guest_instructions_per_s = 0;
    /* Keep the newest endpoint at or before the cutoff, without interpolation. */
    if (v->measured_ns >= UINT64_C(1000000000)) {
        uint64_t cutoff = v->measured_ns - UINT64_C(1000000000);
        while (s->count > 1 && s->history[(s->first + 1) % 128].measured_ns <= cutoff) {
            s->first = (s->first + 1) % 128;
            s->count--;
        }
        if (s->count && s->history[s->first].measured_ns <= cutoff) {
            const emu_runtime_snapshot_t *old = &s->history[s->first];
            s->snapshot.window_ns = v->measured_ns - old->measured_ns;
            s->snapshot.ticks_per_s = (double)(v->ticks - old->ticks) * 1e9 / s->snapshot.window_ns;
            s->snapshot.guest_instructions_per_s =
                (double)(v->guest_instructions - old->guest_instructions) * 1e9 / s->snapshot.window_ns;
            s->snapshot.rates_valid = 1;
        }
    }
    if (s->count && s->history[(s->first + s->count - 1) % 128].measured_ns == v->measured_ns)
        s->count--;
    if (s->count == 128) { s->first = (s->first + 1) % 128; s->count--; }
    s->history[(s->first + s->count++) % 128] = s->snapshot;
    if (s->callback) s->callback(s->opaque, s, EMU_RUNTIME_SNAPSHOT);
    return 0;
}

int emu_runtime_state_rebase(emu_runtime_state_t *s, const emu_runtime_snapshot_t *v) {
    if (!s || !v || s->lifecycle.phase == EMU_RUNTIME_STOPPED ||
        s->snapshot.sample_sequence == UINT64_MAX ||
        v->measured_ns < s->snapshot.measured_ns ||
        v->elapsed_ns < s->snapshot.elapsed_ns ||
        v->ticks < s->snapshot.ticks ||
        v->guest_instructions < s->snapshot.guest_instructions) return -1;
    s->first = s->count = 0;
    s->snapshot.icount = v->icount;
    return emu_runtime_state_publish(s, v, 1);
}
