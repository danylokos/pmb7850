#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fake_engine.h"

typedef struct {
    char name[16];
    int requested;
    int sampled;
} emu_fake_key_t;

typedef struct {
    emu_callbacks_t callbacks;
    uint8_t *storage;
    size_t storage_size;
    uint64_t ticks, guest, event_sequence, frame_sequence;
    uint64_t artifact_sequence, serial_rx_bytes, serial_tx_bytes;
    uint64_t event_mask;
    int stop, sim, coverage;
    unsigned battery;
    emu_serial_link_attachment_t serial_attachment;
    emu_fake_key_t keys[8];
    size_t key_count;
    char pending[32];
    size_t pending_size;
} emu_fake_session_t;

static int emu_fake_live_count;
static int emu_fake_fail_create;
static int emu_fake_fail_artifact;

int emu_fake_live_sessions(void) { return emu_fake_live_count; }
void emu_fake_fail_next_create(void) { emu_fake_fail_create = 1; }
void emu_fake_fail_next_artifact(void) { emu_fake_fail_artifact = 1; }

static emu_error_code_t emu_fake_error(emu_error_t *error,
                                        emu_error_code_t code,
                                        const char *message) {
    if (error) {
        error->code = code;
        snprintf(error->message, sizeof error->message, "%s", message);
    }
    return code;
}

static void emu_fake_ok(emu_error_t *error) {
    if (error) { error->code = EMU_OK; error->message[0] = 0; }
}

static emu_error_code_t emu_fake_create(const emu_prepared_session_t *p,
                                         const emu_callbacks_t *callbacks,
                                         void **out, emu_error_t *error) {
    emu_fake_session_t *s = calloc(1, sizeof *s);
    if (!s) return emu_fake_error(error, EMU_ERR_NOMEM,
                                  "fake allocation failed");
    emu_fake_live_count++;
    s->event_mask = EMU_EVENT_MASK_ALL;
    s->storage_size = p->source.size;
    s->storage = malloc(s->storage_size);
    if (!s->storage) {
        free(s); emu_fake_live_count--;
        return emu_fake_error(error, EMU_ERR_NOMEM, "fake storage failed");
    }
    memcpy(s->storage, p->source.bytes, s->storage_size);
    if (callbacks) s->callbacks = *callbacks;
    for (size_t i = 0; i < p->operation_count; i++) {
        const emu_storage_operation_t *operation = &p->operations[i];
        size_t base = p->chips[operation->chip_index].source_offset;
        if (operation->space != EMU_STORAGE_MAIN_ARRAY) base = 0;
        if (base + operation->offset + operation->replacement_size >
            s->storage_size) {
            free(s->storage); free(s); emu_fake_live_count--;
            return emu_fake_error(error, EMU_ERR_INVALID_PREPARED_SESSION,
                                  "fake operation bounds");
        }
        uint8_t *target = s->storage + base + operation->offset;
        if (operation->expected &&
            memcmp(target, operation->expected, operation->expected_size)) {
            free(s->storage); free(s); emu_fake_live_count--;
            return emu_fake_error(error, EMU_ERR_ENGINE,
                                  "fake expected bytes mismatch");
        }
        memcpy(target, operation->replacement, operation->replacement_size);
    }
    if (emu_fake_fail_create) {
        emu_fake_fail_create = 0; *out = s;
        return emu_fake_error(error, EMU_ERR_ENGINE,
                              "injected partial creation failure");
    }
    *out = s; emu_fake_ok(error); return EMU_OK;
}

static void emu_fake_sample_keys(emu_fake_session_t *s) {
    for (size_t i = 0; i < s->key_count; i++)
        s->keys[i].sampled = s->keys[i].requested;
}

static void emu_fake_frame(emu_fake_session_t *s, emu_frame_kind_t kind) {
    if (!s->callbacks.frame) return;
    uint8_t rgb[] = {
        kind == EMU_FRAME_DDRAM ? 4u : 1u,
        (uint8_t)(s->frame_sequence + 1u),
        (uint8_t)s->ticks,
    };
    emu_frame_t frame = {
        kind, ++s->frame_sequence, s->ticks, 1, 1, rgb, 3,
    };
    s->callbacks.frame(s->callbacks.opaque, &frame);
}

static void emu_fake_event(emu_fake_session_t *s, emu_event_kind_t kind,
                           const char *operation) {
    if (!s->callbacks.event || !(s->event_mask & EMU_EVENT_MASK(kind))) return;
    emu_event_t event = {0};
    event.sequence = ++s->event_sequence;
    event.tick = s->ticks;
    event.icount = s->ticks;
    event.pc = (uint32_t)s->ticks;
    event.kind = kind;
    event.fields[0].name = "engine";
    event.fields[0].kind = EMU_EVENT_VALUE_STRING;
    event.fields[0].value.string = "fake";
    event.fields[1].name = "operation";
    event.fields[1].kind = EMU_EVENT_VALUE_STRING;
    event.fields[1].value.string = operation;
    event.field_count = 2;
    s->callbacks.event(s->callbacks.opaque, &event);
}

static void emu_fake_callbacks(emu_fake_session_t *s,
                               emu_event_kind_t event_kind,
                               const char *operation) {
    emu_fake_sample_keys(s);
    emu_fake_frame(s, EMU_FRAME_DISPLAY);
    if (s->callbacks.serial && s->pending_size) {
        s->callbacks.serial(s->callbacks.opaque,
                            (const uint8_t *)s->pending, s->pending_size);
        s->serial_tx_bytes += s->pending_size;
        s->pending_size = 0;
    }
    emu_fake_event(s, event_kind, operation);
}

static emu_error_code_t emu_fake_run(
        void *opaque, const emu_run_request_t *request,
        emu_run_result_t *result, emu_error_t *error) {
    emu_fake_session_t *s = opaque;
    uint64_t budget = request ? request->tick_budget : 0;
    uint64_t ticks = s->stop ? 0 : budget;
    s->ticks += ticks; s->guest += ticks;
    *result = (emu_run_result_t){
        .status = s->stop ? EMU_RUN_STOPPED : EMU_RUN_LIMIT,
        .ticks = ticks,
        .guest_instructions = ticks,
        .end_icount = s->ticks,
        .pc = (uint32_t)s->ticks,
        .digest = request->compute_digest
                ? UINT64_C(0xf000000000000000) | s->ticks : 0,
    };
    emu_fake_callbacks(s, EMU_EVENT_SLICE, "run");
    emu_fake_ok(error); return EMU_OK;
}

static emu_error_code_t emu_fake_reset(void *opaque, emu_error_t *error) {
    emu_fake_session_t *s = opaque;
    s->ticks = s->guest = 0; s->stop = 0;
    emu_fake_callbacks(s, EMU_EVENT_RESET, "reset");
    emu_fake_ok(error); return EMU_OK;
}

static emu_error_code_t emu_fake_stop(void *opaque, emu_error_t *error) {
    ((emu_fake_session_t *)opaque)->stop = 1;
    emu_fake_ok(error); return EMU_OK;
}

static emu_error_code_t emu_fake_query(void *opaque, int compute_digest,
                                        emu_engine_state_t *state,
                                        emu_error_t *error) {
    emu_fake_session_t *s = opaque;
    *state = (emu_engine_state_t){
        .ticks = s->ticks,
        .guest_instructions = s->guest,
        .icount = s->ticks,
        .pc = (uint32_t)s->ticks,
        .digest = compute_digest
                ? UINT64_C(0xf000000000000000) | s->ticks : 0,
        .stop_requested = s->stop,
    };
    emu_fake_ok(error); return EMU_OK;
}

static void emu_fake_destroy(void *opaque) {
    emu_fake_session_t *s = opaque;
    if (!s) return;
    free(s->storage); free(s); emu_fake_live_count--;
}

static emu_fake_key_t *emu_fake_find_key(emu_fake_session_t *s,
                                         const char *name, int create) {
    for (size_t i = 0; i < s->key_count; i++)
        if (!strcmp(s->keys[i].name, name)) return &s->keys[i];
    if (!create || s->key_count == sizeof s->keys / sizeof s->keys[0])
        return NULL;
    emu_fake_key_t *key = &s->keys[s->key_count++];
    snprintf(key->name, sizeof key->name, "%s", name);
    return key;
}

static emu_error_code_t emu_fake_key(void *opaque, const char *name,
                                      int pressed, emu_error_t *error) {
    emu_fake_session_t *s = opaque;
    if (!name) return emu_fake_error(error, EMU_ERR_ARGUMENT, "missing key");
    emu_fake_key_t *key = emu_fake_find_key(s, name, 1);
    if (!key) return emu_fake_error(error, EMU_ERR_ARGUMENT, "too many keys");
    key->requested = pressed != 0;
    emu_fake_ok(error); return EMU_OK;
}

static emu_error_code_t emu_fake_key_query(void *opaque, const char *name,
                                            emu_key_sample_t *sample,
                                            emu_error_t *error) {
    emu_fake_session_t *s = opaque;
    emu_fake_key_t *key = emu_fake_find_key(s, name, 0);
    if (!key) return emu_fake_error(error, EMU_ERR_ARGUMENT, "unknown key");
    *sample = (emu_key_sample_t){key->requested, key->sampled, s->ticks};
    emu_fake_ok(error); return EMU_OK;
}

static emu_error_code_t emu_fake_serial(void *opaque, const uint8_t *data,
                                         size_t size, emu_error_t *error) {
    emu_fake_session_t *s = opaque;
    if (size > sizeof s->pending)
        return emu_fake_error(error, EMU_ERR_ARGUMENT, "serial too large");
    memcpy(s->pending, data, size); s->pending_size = size;
    s->serial_rx_bytes += size;
    emu_fake_ok(error); return EMU_OK;
}

static emu_error_code_t emu_fake_sim(void *opaque, int attached,
                                      emu_error_t *error) {
    ((emu_fake_session_t *)opaque)->sim = attached;
    emu_fake_ok(error); return EMU_OK;
}

static emu_error_code_t emu_fake_battery(void *opaque, unsigned level,
                                          int charging, emu_error_t *error) {
    (void)charging;
    if (level > 100)
        return emu_fake_error(error, EMU_ERR_ARGUMENT, "battery level");
    ((emu_fake_session_t *)opaque)->battery = level;
    emu_fake_ok(error); return EMU_OK;
}

static emu_error_code_t emu_fake_snapshot(void *opaque, const char *path,
                                           int load, emu_error_t *error) {
    (void)opaque; (void)load;
    if (!path) return emu_fake_error(error, EMU_ERR_ARGUMENT,
                                     "snapshot path");
    emu_fake_ok(error); return EMU_OK;
}

static emu_error_code_t emu_fake_debug(void *opaque, const char *command,
                                        emu_error_t *error) {
    (void)opaque;
    if (!command) return emu_fake_error(error, EMU_ERR_ARGUMENT,
                                        "debug command");
    emu_fake_ok(error); return EMU_OK;
}

static emu_error_code_t emu_fake_coverage(void *opaque, int enabled,
                                           emu_error_t *error) {
    ((emu_fake_session_t *)opaque)->coverage = enabled;
    emu_fake_ok(error); return EMU_OK;
}

static emu_error_code_t emu_fake_poll(void *opaque, emu_error_t *error) {
    emu_fake_callbacks(opaque, EMU_EVENT_POLL, "poll");
    emu_fake_ok(error); return EMU_OK;
}

static emu_error_code_t emu_fake_select_events(
        void *opaque, const emu_event_selection_t *selection,
        emu_error_t *error) {
    ((emu_fake_session_t *)opaque)->event_mask = selection->kind_mask;
    emu_fake_ok(error); return EMU_OK;
}

static emu_error_code_t emu_fake_serial_link(
        void *opaque, emu_serial_link_attachment_t attachment,
        emu_serial_link_state_t *state, emu_error_t *error) {
    emu_fake_session_t *s = opaque;
    s->serial_attachment = attachment;
    *state = (emu_serial_link_state_t){
        attachment, 1, s->serial_rx_bytes, s->serial_tx_bytes,
    };
    emu_fake_ok(error); return EMU_OK;
}

static emu_error_code_t emu_fake_artifact(
        void *opaque, const emu_artifact_request_t *request,
        emu_artifact_result_t *result, emu_error_t *error) {
    emu_fake_session_t *s = opaque;
    if (emu_fake_fail_artifact) {
        emu_fake_fail_artifact = 0;
        return emu_fake_error(error, EMU_ERR_ENGINE,
                              "injected artifact failure");
    }
    if (request->action == EMU_ARTIFACT_CAPTURE &&
        (request->kind == EMU_ARTIFACT_LCD_FRAME ||
         request->kind == EMU_ARTIFACT_LCD_DDRAM))
        emu_fake_frame(s, request->kind == EMU_ARTIFACT_LCD_DDRAM
                         ? EMU_FRAME_DDRAM : EMU_FRAME_DISPLAY);
    result->kind = request->kind;
    result->sequence = ++s->artifact_sequence;
    result->icount = s->ticks;
    emu_fake_event(s, EMU_EVENT_ARTIFACT,
                   emu_artifact_request_kind_name(request->kind));
    emu_fake_ok(error); return EMU_OK;
}

static const char *const emu_fake_models[] = {"fake"};

const emu_engine_descriptor_t emu_fake_engine_descriptor = {
    .name = "fake",
    .version = "deterministic-2",
    .supported_models = emu_fake_models,
    .supported_model_count = 1,
    .capabilities = EMU_CAP_KEYS | EMU_CAP_SERIAL | EMU_CAP_SIM |
        EMU_CAP_BATTERY | EMU_CAP_STORAGE_INIT | EMU_CAP_SNAPSHOTS |
        EMU_CAP_DEBUGGER | EMU_CAP_COVERAGE | EMU_CAP_FRAMES |
        EMU_CAP_EVENTS | EMU_CAP_KEY_SAMPLING | EMU_CAP_SERIAL_LINK |
        EMU_CAP_ARTIFACT_REQ,
    .ops = {
        .create = emu_fake_create,
        .run = emu_fake_run,
        .reset = emu_fake_reset,
        .request_stop = emu_fake_stop,
        .query = emu_fake_query,
        .destroy = emu_fake_destroy,
        .key = emu_fake_key,
        .serial_rx = emu_fake_serial,
        .sim = emu_fake_sim,
        .battery = emu_fake_battery,
        .snapshot = emu_fake_snapshot,
        .debugger = emu_fake_debug,
        .coverage = emu_fake_coverage,
        .poll = emu_fake_poll,
        .select_events = emu_fake_select_events,
        .key_query = emu_fake_key_query,
        .serial_link = emu_fake_serial_link,
        .artifact = emu_fake_artifact,
    },
};
