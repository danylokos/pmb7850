#define _GNU_SOURCE

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>

#include "emu_cemu_adapter.h"
#include "cemu_storage.h"

/* CEMU-native configuration, lifecycle, and diagnostics are the adapter's
 * entire engine-facing surface. CPU, SoC, and peripheral state stay opaque. */
#include "cemu_core.h"
#include "cemu_core_diagnostics.h"
#include "devices.h"
#include "synth.h"
#include "cemu_core_private.h"
#include "snapshot.h"
#if CEMU_INSTRUMENTED
#include "debugger.h"
#include "emu_eeprom_debugger.h"
#include "gsm_l1_probe.h"
#include "monitor.h"
#include "summary.h"
#include "emu_cemu_drcov_adapter.h"
#include "emu_cemu_trace_adapter.h"
#endif

typedef struct {
    const emu_prepared_session_t *prepared;
    device_config_t config;
    cemu_core_t *core;
    emu_cemu_storage_context_t storage;
    emu_callbacks_t callbacks;
    size_t serial_offset;
    uint64_t event_sequence;
    uint64_t frame_sequence;
    uint64_t artifact_sequence;
    uint64_t event_mask;
    emu_serial_link_attachment_t serial_attachment;
    int capture_strict;
    int capture_raw;
    uint64_t capture_strict_sequence;
    uint64_t capture_raw_sequence;
    int capture_failed;
#if CEMU_INSTRUMENTED
    emu_cemu_trace_adapter_t *trace_adapter;
    emu_cemu_drcov_adapter_t *drcov_adapter;
    gsm_l1_probe_t gsm_l1_probe;
    monitor_t *monitor;
    loop_report_t monitor_loop;
    int monitor_have_loop;
    unsigned monitor_stop_subscription;
#endif
} emu_cemu_session_t;

static void emu_cemu_audio_output(void *opaque, uint64_t completion_tick,
                                  const int16_t *samples,
                                  size_t frame_count) {
    emu_cemu_session_t *session = opaque;
    if (!session || !session->callbacks.audio_output) return;
    emu_audio_output_t output = {
        .completion_tick = completion_tick,
        .samples = samples,
        .frame_count = frame_count,
    };
    session->callbacks.audio_output(session->callbacks.opaque, &output);
}

static void emu_cemu_audio_reset(
        emu_cemu_session_t *session, emu_audio_reset_reason_t reason) {
    if (!session || !session->callbacks.audio_reset) return;
    cemu_core_state_t state = {0};
    (void)cemu_core_query(session->core, &state);
    emu_audio_reset_t reset = {
        .reason = reason,
        .icount = state.instruction_count,
    };
    session->callbacks.audio_reset(session->callbacks.opaque, &reset);
}

static void emu_cemu_audio_discontinuity(
        void *opaque, uint64_t tick, cemu_audio_discontinuity_t reason) {
    emu_cemu_session_t *session = opaque;
    if (!session || !session->callbacks.audio_reset) return;
    (void)tick;
    cemu_core_state_t state = {0};
    (void)cemu_core_query(session->core, &state);
    emu_audio_reset_t reset = {
        .reason = reason == CEMU_AUDIO_DISCONTINUITY_SOURCE_OVERFLOW
                ? EMU_AUDIO_RESET_SOURCE_OVERFLOW
                : EMU_AUDIO_RESET_SOURCE_CANCELLED,
        .icount = state.instruction_count,
    };
    session->callbacks.audio_reset(session->callbacks.opaque, &reset);
}

#if CEMU_INSTRUMENTED
typedef struct {
    emu_cemu_session_t *session;
    debugger_t *debugger;
    emu_eeprom_debugger_t *eeprom;
    unsigned subscription;
} emu_cemu_eeprom_debug_context_t;

static int emu_cemu_debug_eeprom_copy(
        void *opaque, int chip_index, size_t offset,
        uint8_t *bytes, size_t size) {
    emu_cemu_eeprom_debug_context_t *context = opaque;
    return cemu_core_diagnostic_flash_array_copy(
               context->session->core, (size_t)chip_index,
               offset, bytes, size).code == CEMU_STATUS_OK ? 0 : -1;
}

static void emu_cemu_debug_eeprom_stop(
        void *opaque, const char *kind, uint32_t pc, uint64_t icount,
        const char *reason) {
    emu_cemu_eeprom_debug_context_t *context = opaque;
    debugger_request_external_stop(
        context->debugger, kind, pc, icount, reason);
}

static void emu_cemu_debug_eeprom_event(
        void *opaque, const cemu_event_t *event) {
    emu_cemu_eeprom_debug_context_t *context = opaque;
    if (event->type == CEMU_EVENT_BUS) {
        const cemu_bus_event_t *bus = &event->as.bus;
        const bus_transaction_t *transaction = bus->transaction;
        if (transaction && transaction->kind == BUS_ACCESS_READ &&
            bus->has_flash_target && bus->flash_array_data)
            emu_eeprom_debugger_read(
                context->eeprom, bus->chip_index, bus->chip_offset,
                transaction->addr, transaction->value, transaction->size,
                bus->tick, bus->icount, bus->pc);
    } else if (event->type == CEMU_EVENT_FLASH_MUTATION) {
        const cemu_flash_mutation_event_t *mutation =
            &event->as.flash_mutation;
        emu_eeprom_debugger_mutation(
            context->eeprom, mutation->chip_index,
            mutation->kind == CEMU_FLASH_MUTATION_ERASE
                ? EMU_EEPROM_MUTATION_ERASE
                : EMU_EEPROM_MUTATION_PROGRAM,
            mutation->offset, mutation->size, mutation->tick,
            mutation->icount, mutation->pc);
    }
}

static int emu_cemu_debug_eeprom_command(
        void *opaque, int argc, char *const *argv, FILE *out) {
    emu_cemu_eeprom_debug_context_t *context = opaque;
    return emu_eeprom_debugger_command(
        context->eeprom, argc, argv, out);
}

static void emu_cemu_debug_eeprom_help(void *opaque, FILE *out) {
    emu_cemu_eeprom_debug_context_t *context = opaque;
    emu_eeprom_debugger_help(context->eeprom, out);
}

static void emu_cemu_debug_eeprom_restore(void *opaque) {
    emu_cemu_eeprom_debug_context_t *context = opaque;
    cemu_core_state_t state;
    if (cemu_core_query(context->session->core, &state).code == CEMU_STATUS_OK)
        emu_eeprom_debugger_resync(
            context->eeprom, state.ticks, state.instruction_count, state.pc);
}

static int emu_cemu_debug_eeprom_attach(
        emu_cemu_eeprom_debug_context_t *context,
        emu_cemu_session_t *session, debugger_t *debugger) {
    *context = (emu_cemu_eeprom_debug_context_t){
        .session = session,
        .debugger = debugger,
    };
    size_t count = 0;
    if (cemu_core_diagnostic_flash_chip_count(
            session->core, &count).code != CEMU_STATUS_OK)
        return -1;
    emu_eeprom_trace_chip_t *chips = calloc(
        count ? count : 1u, sizeof *chips);
    if (!chips) return -1;
    int failed = 0;
    for (size_t i = 0; i < count; i++) {
        cemu_diagnostic_flash_chip_t chip;
        if (cemu_core_diagnostic_flash_chip_info(
                session->core, i, &chip).code != CEMU_STATUS_OK) {
            failed = 1;
            break;
        }
        chips[i] = (emu_eeprom_trace_chip_t){
            .chip_index = chip.chip_index,
            .chip_name = chip.chip_name,
            .model = chip.model,
            .size = chip.size,
        };
    }
    cemu_core_state_t state;
    if (!failed && (cemu_core_query(session->core, &state).code != CEMU_STATUS_OK ||
        emu_eeprom_debugger_create(
            &context->eeprom, chips, count,
            emu_cemu_debug_eeprom_copy, context,
            emu_cemu_debug_eeprom_stop, context,
            state.ticks, state.instruction_count, state.pc)))
        failed = 1;
    free(chips);
    if (failed) return -1;
    cemu_status_t status = cemu_core_subscribe_events(
        session->core,
        (cemu_event_mask_t)(CEMU_EVENT_BUS | CEMU_EVENT_FLASH_MUTATION),
        emu_cemu_debug_eeprom_event, NULL, context,
        &context->subscription);
    if (status.code != CEMU_STATUS_OK) {
        emu_eeprom_debugger_destroy(&context->eeprom);
        return -1;
    }
    debugger_set_extension(
        debugger, emu_cemu_debug_eeprom_command,
        emu_cemu_debug_eeprom_help, emu_cemu_debug_eeprom_restore, context);
    return 0;
}

static void emu_cemu_debug_eeprom_detach(
        emu_cemu_eeprom_debug_context_t *context) {
    if (context->subscription)
        (void)cemu_core_unsubscribe_events(
            context->session->core, context->subscription);
    debugger_set_extension(context->debugger, NULL, NULL, NULL, NULL);
    emu_eeprom_debugger_destroy(&context->eeprom);
    context->subscription = 0;
}

static void emu_cemu_emit_patch_trace(emu_cemu_session_t *session) {
    size_t count = 0;
    const emu_patch_definition_t *catalog = emu_patch_catalog(&count);
    uint64_t set = session->prepared->options.firmware_patches;
    const emu_cemu_storage_result_t *result = &session->storage.result;
    soc_t *soc = cemu_core_private_soc(session->core);
    for (size_t i = 0; i < count; i++) {
        if (!(set & (UINT64_C(1) << i))) continue;
        const char *status = result->already_applied & (UINT64_C(1) << i)
                           ? "already_applied" : "applied";
        for (size_t j = 0; j < catalog[i].hunk_count; j++) {
            const emu_patch_hunk_t *hunk = &catalog[i].hunks[j];
            size_t hex_size = hunk->size * 2u + 1u;
            char *expected = malloc(hex_size);
            char *replacement = malloc(hex_size);
            if (!expected || !replacement) {
                free(expected);
                free(replacement);
                continue;
            }
            for (size_t k = 0; k < hunk->size; k++) {
                snprintf(expected + k * 2u, 3u, "%02X", hunk->expected[k]);
                snprintf(replacement + k * 2u, 3u, "%02X", hunk->replacement[k]);
            }
            cemu_event_fields_t info = {0};
            cemu_event_field_i64(&info, "chip_index", hunk->chip_index);
            cemu_event_field_i64(&info, "chip_offset", hunk->chip_offset);
            cemu_event_field_string(&info, "expected", expected);
            cemu_event_field_string(&info, "patch", catalog[i].name);
            cemu_event_field_string(&info, "provenance", catalog[i].provenance);
            cemu_event_field_string(&info, "replacement", replacement);
            cemu_event_field_string(&info, "status", status);
            cemu_soc_emit_native_trace(soc, "firmware_patch", 1,
                hunk->chip_offset, 1, (int)hunk->size, 0, 0,
                catalog[i].description, &info);
            free(expected);
            free(replacement);
        }
    }
}
#endif

static emu_error_code_t emu_cemu_fail(emu_error_t *error,
                                      emu_error_code_t code,
                                      const char *message) {
    if (error) {
        error->code = code;
        snprintf(error->message, sizeof error->message, "%s", message);
    }
    return code;
}

static emu_error_code_t emu_cemu_status(
        cemu_status_t status, emu_error_t *error) {
    if (status.code == CEMU_STATUS_OK) {
        if (error) {
            error->code = EMU_OK;
            error->message[0] = 0;
        }
        return EMU_OK;
    }
    emu_error_code_t code = EMU_ERR_ENGINE;
    if (status.code == CEMU_STATUS_INVALID_ARGUMENT)
        code = EMU_ERR_ARGUMENT;
    else if (status.code == CEMU_STATUS_INVALID_CONFIGURATION ||
             status.code == CEMU_STATUS_TOPOLOGY_FAILED)
        code = EMU_ERR_INVALID_PREPARED_SESSION;
    else if (status.code == CEMU_STATUS_UNSUPPORTED)
        code = EMU_ERR_UNSUPPORTED;
    else if (status.code == CEMU_STATUS_ALLOCATION_FAILED)
        code = EMU_ERR_NOMEM;
    return emu_cemu_fail(error, code, status.message);
}

static int emu_cemu_config_from_prepared(const emu_prepared_session_t *p,
                                         device_config_t *cfg,
                                         char *detail, size_t detail_size) {
    const char *models[EMU_MAX_CHIPS] = {0};
    size_t offsets[EMU_MAX_CHIPS] = {0};
    size_t sizes[EMU_MAX_CHIPS] = {0};
    if (!p->selected_device[0] || !p->chip_count ||
        p->chip_count > EMU_MAX_CHIPS) {
        snprintf(detail, detail_size,
                 "prepared device is absent from the host catalog");
        return 0;
    }
    for (size_t i = 0; i < p->chip_count; i++) {
        models[i] = p->chips[i].model;
        offsets[i] = p->chips[i].source_offset;
        sizes[i] = p->chips[i].size;
    }
    if (!cemu_device_config_from_prepared(
            p->selected_device, models, offsets, sizes, p->chip_count,
            p->source.size, cfg, detail, detail_size))
        return 0;
    for (size_t i = 0; i < p->chip_count; i++) {
        if (strcmp(p->chips[i].role, cfg->flash.chips[i].name) ||
            strcmp(p->chips[i].model, cfg->flash.chips[i].model) ||
            p->chips[i].source_offset != cfg->flash_image.file_offsets[i] ||
            p->chips[i].size != cfg->flash.chips[i].chip_size) {
            snprintf(detail, detail_size,
                     "prepared chip %zu disagrees with CEMU board wiring", i);
            return 0;
        }
    }
    emu_identity_kind_t identity =
        cfg->flash.eeprom_overlay_identity.kind ==
            EEPROM_OVERLAY_IDENTITY_AM29_SECSI
        ? EMU_IDENTITY_AM29_SECSI : EMU_IDENTITY_FACTORY_UID;
    if (p->identity_kind != identity ||
        p->identity_chip_index !=
            (size_t)cfg->flash.eeprom_overlay_identity.chip_index ||
        strcmp(p->metadata.flash_engine,
               cfg->flash.chips[p->identity_chip_index].model)) {
        snprintf(detail, detail_size,
                 "prepared primary flash identity disagrees with CEMU board wiring");
        return 0;
    }
    return 1;
}

static cemu_core_options_t emu_cemu_core_options(emu_cemu_session_t *session) {
    return (cemu_core_options_t){
        .source = session->prepared->source.bytes,
        .source_size = session->prepared->source.size,
        .device = session->config,
        .synthetic_mask = session->prepared->options.synthetic_mask,
        .gsm_stub_enabled =
            (session->prepared->options.synthetic_mask >> SYN_GSM) & 1u,
        .serial_autobaud_bypass =
            session->prepared->options.serial_autobaud_bypass,
        .defer_post_reset_storage =
            session->prepared->options.defer_post_restore_storage,
        .storage_initializer = emu_cemu_storage_initialize,
        .storage_result = emu_cemu_storage_record_result,
        .storage_opaque = &session->storage,
        .audio_output = emu_cemu_audio_output,
        .audio_reset = emu_cemu_audio_discontinuity,
        .audio_opaque = session,
    };
}

static emu_error_code_t emu_cemu_create(
        const emu_prepared_session_t *prepared,
        const emu_callbacks_t *callbacks, void **out, emu_error_t *error) {
    if (out) *out = NULL;
    if (!prepared || !out)
        return emu_cemu_fail(error, EMU_ERR_ARGUMENT,
                             "invalid CEMU creation request");
    emu_cemu_session_t *session = calloc(1, sizeof *session);
    if (!session)
        return emu_cemu_fail(error, EMU_ERR_NOMEM,
                             "cannot allocate CEMU session");
    session->prepared = prepared;
    session->event_mask = EMU_EVENT_MASK_ALL;
    if (callbacks) session->callbacks = *callbacks;
    char detail[256];
    if (!emu_cemu_config_from_prepared(
            prepared, &session->config, detail, sizeof detail)) {
        free(session);
        return emu_cemu_fail(
            error, EMU_ERR_INVALID_PREPARED_SESSION, detail);
    }
    if (((prepared->options.synthetic_mask >> SYN_GSM) & 1u) &&
        session->config.baseband.software_version !=
            prepared->metadata.software_version) {
        snprintf(detail, sizeof detail,
                 "no qualified baseband/L1 profile for %s software %d",
                 prepared->metadata.model,
                 prepared->metadata.software_version);
        free(session);
        return emu_cemu_fail(error, EMU_ERR_UNSUPPORTED, detail);
    }
    emu_cemu_storage_context_init(&session->storage, prepared);
    cemu_core_options_t options = emu_cemu_core_options(session);
    cemu_status_t status = cemu_core_create(&session->core, &options);
    if (status.code != CEMU_STATUS_OK) {
        free(session);
        return emu_cemu_status(status, error);
    }
    *out = session;
    return emu_cemu_status(cemu_status_ok(), error);
}

static int emu_cemu_emit_frame(emu_cemu_session_t *session, int raw) {
    if (!session->callbacks.frame) return 0;
    uint8_t rgb[CEMU_FRAME_MAX_RGB_SIZE];
    cemu_frame_info_t info;
    cemu_status_t status = cemu_core_render_frame(
        session->core, raw, rgb, sizeof rgb, &info);
    if (status.code != CEMU_STATUS_OK) return 0;
    emu_frame_t frame = {
        .kind = raw ? EMU_FRAME_DDRAM : EMU_FRAME_DISPLAY,
        .sequence = ++session->frame_sequence,
        .icount = info.instruction_count,
        .width = info.width,
        .height = info.height,
        .rgb = rgb,
        .rgb_size = info.size,
    };
    session->callbacks.frame(session->callbacks.opaque, &frame);
    return 1;
}

static void emu_cemu_emit_event(emu_cemu_session_t *session,
                                emu_event_kind_t kind,
                                const char *operation) {
    if (!session->callbacks.event ||
        !(session->event_mask & EMU_EVENT_MASK(kind)))
        return;
    cemu_core_state_t state;
    if (cemu_core_query(session->core, &state).code != CEMU_STATUS_OK)
        return;
    emu_event_t event = {0};
    event.sequence = ++session->event_sequence;
    event.tick = state.ticks;
    event.icount = state.instruction_count;
    event.pc = state.pc;
    event.kind = kind;
    event.fields[0].name = "engine";
    event.fields[0].kind = EMU_EVENT_VALUE_STRING;
    event.fields[0].value.string = "cemu";
    event.fields[1].name = "operation";
    event.fields[1].kind = EMU_EVENT_VALUE_STRING;
    event.fields[1].value.string = operation;
    event.field_count = 2;
    session->callbacks.event(session->callbacks.opaque, &event);
}

static void emu_cemu_deliver(emu_cemu_session_t *session,
                             emu_event_kind_t kind,
                             const char *operation) {
    emu_cemu_emit_frame(session, 0);
    if (session->callbacks.serial) {
        cemu_serial_tx_view_t view;
        if (cemu_core_serial_tx_view(
                session->core, session->serial_offset, &view).code ==
                CEMU_STATUS_OK && view.size) {
            session->callbacks.serial(
                session->callbacks.opaque, view.data, view.size);
            session->serial_offset = view.next_cursor;
        }
    }
    emu_cemu_emit_event(session, kind, operation);
}

static int emu_cemu_emit_capture_frame(
        emu_cemu_session_t *session, int raw) {
    if (!session->callbacks.capture_frame) return 0;
    uint64_t sequence = 0, icount = 0;
    cemu_status_t frame_state = cemu_core_lcd_frame_state(
        session->core, &sequence, &icount);
    uint64_t *last = raw ? &session->capture_raw_sequence
                         : &session->capture_strict_sequence;
    if (frame_state.code != CEMU_STATUS_OK) return -1;
    if (sequence <= *last) return 0;
    const size_t capacity = (size_t)132u * 162u * 16u * 3u;
    uint8_t *rgb = malloc(capacity);
    if (!rgb) return -1;
    cemu_frame_info_t info;
    cemu_status_t status = cemu_core_render_capture_frame(
        session->core, raw, 4u, rgb, capacity, &info);
    if (status.code == CEMU_STATUS_OK && info.sequence > *last) {
        emu_frame_t frame = {
            .kind = raw ? EMU_FRAME_DDRAM : EMU_FRAME_DISPLAY,
            .sequence = info.sequence,
            .icount = icount,
            .width = info.width,
            .height = info.height,
            .rgb = rgb,
            .rgb_size = info.size,
        };
        session->callbacks.capture_frame(
            session->callbacks.opaque, &frame);
        *last = info.sequence;
    }
    free(rgb);
    return status.code == CEMU_STATUS_OK ? 0 : -1;
}

static void emu_cemu_after_step(void *opaque) {
    emu_cemu_session_t *session = opaque;
    if (session->capture_strict)
        session->capture_failed |=
            emu_cemu_emit_capture_frame(session, 0) != 0;
    if (session->capture_raw)
        session->capture_failed |=
            emu_cemu_emit_capture_frame(session, 1) != 0;
}

static emu_run_status_t emu_cemu_run_status(cemu_run_reason_t reason) {
    if (reason == CEMU_RUN_STOPPED) return EMU_RUN_STOPPED;
    if (reason == CEMU_RUN_HALTED) return EMU_RUN_HALTED;
    if (reason == CEMU_RUN_UNIMPLEMENTED) return EMU_RUN_UNIMPLEMENTED;
    return EMU_RUN_LIMIT;
}

static void emu_cemu_result(emu_cemu_session_t *session,
                            const cemu_run_result_t *core_result,
                            int compute_digest, emu_run_result_t *result) {
    cemu_core_state_t state = {0};
    (void)cemu_core_query(session->core, &state);
    *result = (emu_run_result_t){
        .status = emu_cemu_run_status(core_result->reason),
        .ticks = core_result->ticks,
        .guest_instructions = core_result->guest_instructions,
        .end_icount = core_result->instruction_count,
        .pc = core_result->pc,
        .digest = compute_digest
                ? cemu_core_diagnostic_state_digest(session->core) : 0,
        .unimplemented_opcode = state.unimplemented_opcode,
        .unimplemented_pc = state.unimplemented_pc,
    };
}

static emu_error_code_t emu_cemu_run(
        void *opaque, const emu_run_request_t *request,
        emu_run_result_t *result, emu_error_t *error) {
    emu_cemu_session_t *session = opaque;
    if (!session || !result || !request || !request->tick_budget)
        return emu_cemu_fail(error, EMU_ERR_ARGUMENT, "invalid CEMU run");
    cemu_run_result_t core_result;
    cemu_run_request_t core_request = {
        .tick_budget = request->tick_budget,
        .allow_idle_batch = request->allow_idle_batch,
    };
    cemu_status_t status = cemu_core_run_request(
        session->core, &core_request, &core_result);
    if (status.code != CEMU_STATUS_OK) return emu_cemu_status(status, error);
    if (core_result.reason == CEMU_RUN_ERROR) {
        cemu_core_state_t state;
        if (cemu_core_query(session->core, &state).code == CEMU_STATUS_OK)
            return emu_cemu_status(state.error, error);
        return emu_cemu_fail(error, EMU_ERR_ENGINE,
                             "CEMU core entered an error state");
    }
    emu_cemu_result(session, &core_result, request->compute_digest, result);
    emu_cemu_deliver(session, EMU_EVENT_SLICE, "run");
    return emu_cemu_status(cemu_status_ok(), error);
}

static emu_error_code_t emu_cemu_reset(void *opaque, emu_error_t *error) {
    emu_cemu_session_t *session = opaque;
    if (!session)
        return emu_cemu_fail(error, EMU_ERR_ARGUMENT,
                             "invalid CEMU reset");
    cemu_status_t status = cemu_core_reset(session->core);
    if (status.code != CEMU_STATUS_OK) return emu_cemu_status(status, error);
    session->serial_offset = 0;
    emu_cemu_audio_reset(session, EMU_AUDIO_RESET_ENGINE);
    emu_cemu_deliver(session, EMU_EVENT_RESET, "reset");
    return emu_cemu_status(cemu_status_ok(), error);
}

static emu_error_code_t emu_cemu_stop(void *opaque, emu_error_t *error) {
    emu_cemu_session_t *session = opaque;
    if (!session)
        return emu_cemu_fail(error, EMU_ERR_ARGUMENT, "invalid CEMU stop");
    cemu_status_t status = cemu_core_request_stop(session->core);
    if (status.code == CEMU_STATUS_OK) {
        cemu_soc_audio_reset(cemu_core_private_soc(session->core));
        emu_cemu_audio_reset(session, EMU_AUDIO_RESET_STOP);
    }
    return emu_cemu_status(status, error);
}

static emu_error_code_t emu_cemu_query(void *opaque,
                                       int compute_digest,
                                       emu_engine_state_t *state,
                                       emu_error_t *error) {
    emu_cemu_session_t *session = opaque;
    if (!session || !state)
        return emu_cemu_fail(error, EMU_ERR_ARGUMENT,
                             "invalid CEMU query");
    cemu_core_state_t core_state;
    cemu_status_t status = cemu_core_query(session->core, &core_state);
    if (status.code != CEMU_STATUS_OK) return emu_cemu_status(status, error);
    if (core_state.error.code != CEMU_STATUS_OK)
        return emu_cemu_status(core_state.error, error);
    *state = (emu_engine_state_t){
        .ticks = core_state.ticks,
        .guest_instructions = core_state.total_guest_instructions,
        .icount = core_state.instruction_count,
        .pc = core_state.pc,
        .digest = compute_digest
                ? cemu_core_diagnostic_state_digest(session->core) : 0,
        .stop_requested = core_state.stop_requested,
        .halted = core_state.halted,
        .idle = core_state.idle,
        .idle_wake_possible = core_state.idle_wake_possible,
        .audio_available = cemu_soc_audio_available(
            cemu_core_private_soc(session->core)),
        .unimplemented_opcode = core_state.unimplemented_opcode,
        .unimplemented_pc = core_state.unimplemented_pc,
        .interrupts_delivered = core_state.interrupts_delivered,
        .traps_taken = core_state.traps_taken,
        .xbus_unknown1_id_reads = core_state.xbus_unknown1_id_reads,
        .xbus_unknown1_status_reads = core_state.xbus_unknown1_status_reads,
        .xbus_unknown1_doorbell_rings =
            core_state.xbus_unknown1_doorbell_rings,
        .interrupt_cache_queries = core_state.interrupt_cache_queries,
        .interrupt_cache_hits = core_state.interrupt_cache_hits,
        .interrupt_cache_scans = core_state.interrupt_cache_scans,
        .interrupt_cache_invalidations =
            core_state.interrupt_cache_invalidations,
    };
    return emu_cemu_status(cemu_status_ok(), error);
}

static void emu_cemu_destroy(void *opaque) {
    emu_cemu_session_t *session = opaque;
    if (!session) return;
#if CEMU_INSTRUMENTED
    if (session->monitor_stop_subscription)
        (void)cemu_core_unsubscribe_events(
            session->core, session->monitor_stop_subscription);
    if (session->monitor) {
        monitor_free(session->monitor);
        free(session->monitor);
        cemu_soc_disable_stats(cemu_core_private_soc(session->core));
    }
    gsm_l1_probe_detach(&session->gsm_l1_probe);
    emu_cemu_trace_detach(&session->trace_adapter);
    emu_cemu_drcov_detach(&session->drcov_adapter);
#endif
    cemu_core_destroy(session->core);
    memset(session, 0, sizeof *session);
    free(session);
}

static emu_error_code_t emu_cemu_key(
        void *opaque, const char *key, int pressed, emu_error_t *error) {
    emu_cemu_session_t *session = opaque;
    if (!session)
        return emu_cemu_fail(error, EMU_ERR_ARGUMENT, "unknown key");
    return emu_cemu_status(
        cemu_core_set_key(session->core, key, pressed), error);
}

static emu_error_code_t emu_cemu_serial(
        void *opaque, const uint8_t *data, size_t size, emu_error_t *error) {
    emu_cemu_session_t *session = opaque;
    if (!session)
        return emu_cemu_fail(error, EMU_ERR_ARGUMENT,
                             "invalid serial input");
    return emu_cemu_status(
        cemu_core_feed_serial(session->core, data, size), error);
}

static emu_error_code_t emu_cemu_sim(
        void *opaque, int attached, emu_error_t *error) {
    emu_cemu_session_t *session = opaque;
    if (!session)
        return emu_cemu_fail(error, EMU_ERR_ARGUMENT,
                             "invalid SIM control");
    return emu_cemu_status(
        cemu_core_set_sim_attached(session->core, attached), error);
}

static emu_error_code_t emu_cemu_battery(
        void *opaque, unsigned level, int charging, emu_error_t *error) {
    emu_cemu_session_t *session = opaque;
    if (!session)
        return emu_cemu_fail(error, EMU_ERR_ARGUMENT,
                             "invalid battery control");
    return emu_cemu_status(
        cemu_core_set_battery(session->core, level, charging), error);
}

static emu_error_code_t emu_cemu_poll(void *opaque, emu_error_t *error) {
    emu_cemu_session_t *session = opaque;
    if (!session)
        return emu_cemu_fail(error, EMU_ERR_ARGUMENT, "invalid poll");
    emu_cemu_deliver(session, EMU_EVENT_POLL, "poll");
    return emu_cemu_status(cemu_status_ok(), error);
}

static emu_error_code_t emu_cemu_select_events(
        void *opaque, const emu_event_selection_t *selection,
        emu_error_t *error) {
    emu_cemu_session_t *session = opaque;
    if (!session || !selection)
        return emu_cemu_fail(error, EMU_ERR_ARGUMENT,
                             "invalid event selection");
    session->event_mask = selection->kind_mask;
    return emu_cemu_status(cemu_status_ok(), error);
}

static emu_error_code_t emu_cemu_key_query(
        void *opaque, const char *name, emu_key_sample_t *sample,
        emu_error_t *error) {
    emu_cemu_session_t *session = opaque;
    if (!session || !sample)
        return emu_cemu_fail(error, EMU_ERR_ARGUMENT,
                             "invalid key query");
    cemu_key_state_t state;
    cemu_status_t status = cemu_core_query_key(session->core, name, &state);
    if (status.code != CEMU_STATUS_OK) return emu_cemu_status(status, error);
    *sample = (emu_key_sample_t){
        state.pressed, state.sampled, state.instruction_count,
    };
    return emu_cemu_status(cemu_status_ok(), error);
}

static emu_error_code_t emu_cemu_serial_link(
        void *opaque, emu_serial_link_attachment_t attachment,
        emu_serial_link_state_t *state, emu_error_t *error) {
    emu_cemu_session_t *session = opaque;
    if (!session || !state)
        return emu_cemu_fail(error, EMU_ERR_ARGUMENT,
                             "invalid serial-link request");
    cemu_serial_link_state_t core_state;
    cemu_status_t status = cemu_core_set_serial_link(
        session->core, attachment != EMU_SERIAL_LINK_DETACHED, &core_state);
    if (status.code != CEMU_STATUS_OK) return emu_cemu_status(status, error);
    session->serial_attachment = attachment;
    *state = (emu_serial_link_state_t){
        attachment, core_state.available, core_state.received_bytes,
        core_state.transmitted_bytes,
    };
    return emu_cemu_status(cemu_status_ok(), error);
}

static emu_error_code_t emu_cemu_artifact(
        void *opaque, const emu_artifact_request_t *request,
        emu_artifact_result_t *result, emu_error_t *error) {
    emu_cemu_session_t *session = opaque;
    if (!session || !request || !result ||
        (request->kind != EMU_ARTIFACT_LCD_FRAME &&
         request->kind != EMU_ARTIFACT_LCD_DDRAM))
        return emu_cemu_fail(error, EMU_ERR_UNSUPPORTED,
                             "CEMU artifact request is unsupported");
    int raw = request->kind == EMU_ARTIFACT_LCD_DDRAM;
    int *enabled = raw ? &session->capture_raw : &session->capture_strict;
    if (request->action == EMU_ARTIFACT_START) {
        *enabled = 1;
        cemu_status_t hook = cemu_core_set_after_step(
            session->core, emu_cemu_after_step, session);
        if (hook.code != CEMU_STATUS_OK) return emu_cemu_status(hook, error);
    } else if (request->action == EMU_ARTIFACT_STOP) {
        *enabled = 0;
        if (!session->capture_strict && !session->capture_raw) {
            cemu_status_t hook = cemu_core_set_after_step(
                session->core, NULL, NULL);
            if (hook.code != CEMU_STATUS_OK)
                return emu_cemu_status(hook, error);
        }
    } else if (request->action == EMU_ARTIFACT_CAPTURE) {
        cemu_status_t flush = cemu_core_flush_lcd_frame(session->core);
        if (flush.code != CEMU_STATUS_OK)
            return emu_cemu_status(flush, error);
        if (!emu_cemu_emit_frame(session, raw))
            return emu_cemu_fail(error, EMU_ERR_UNSUPPORTED,
                                 "CEMU LCD frame is unavailable");
        emu_cemu_after_step(session);
        if (session->capture_failed)
            return emu_cemu_fail(error, EMU_ERR_ENGINE,
                                 "cannot render LCD capture frame");
    }
    cemu_core_state_t state;
    cemu_status_t status = cemu_core_query(session->core, &state);
    if (status.code != CEMU_STATUS_OK) return emu_cemu_status(status, error);
    *result = (emu_artifact_result_t){
        request->kind, ++session->artifact_sequence,
        state.instruction_count,
    };
    emu_cemu_emit_event(session, EMU_EVENT_ARTIFACT,
                        emu_artifact_request_kind_name(request->kind));
    return emu_cemu_status(cemu_status_ok(), error);
}

static emu_error_code_t emu_cemu_snapshot_restore(
        void *opaque, const char *path, emu_error_t *error) {
    emu_cemu_session_t *session = opaque;
    if (!session || !path || !*path)
        return emu_cemu_fail(error, EMU_ERR_ARGUMENT,
                             "invalid CEMU snapshot restore request");
    cpu_t *cpu = cemu_core_private_cpu(session->core);
    soc_t *soc = cemu_core_private_soc(session->core);
    if (!cpu || !soc || snapshot_read_dir(path, cpu, soc) != 0) {
        char detail[256];
        snprintf(detail, sizeof detail,
                 "cannot restore snapshot from %s", path);
        return emu_cemu_fail(error, EMU_ERR_IO, detail);
    }
    cemu_status_t status =
        cemu_core_apply_post_restore_storage(session->core);
    if (status.code != CEMU_STATUS_OK)
        return emu_cemu_status(status, error);
    session->serial_offset = 0;
    cemu_soc_audio_reset(soc);
    emu_cemu_audio_reset(session, EMU_AUDIO_RESET_SNAPSHOT);
    emu_cemu_deliver(session, EMU_EVENT_RESET, "snapshot-restore");
    return emu_cemu_status(cemu_status_ok(), error);
}

#if CEMU_INSTRUMENTED
static int emu_cemu_snapshot_provenance(
        const emu_cemu_session_t *session, char *output, size_t capacity) {
    if (!session || !output || !capacity ||
        strpbrk(session->prepared->source.locator, "\"\\\r\n"))
        return -1;
    int used = snprintf(output, capacity,
        "    \"flash\": \"%s\",\n    \"firmware_patches\": [",
        session->prepared->source.locator);
    if (used < 0 || (size_t)used >= capacity) return -1;
    size_t count = 0;
    const emu_patch_definition_t *catalog = emu_patch_catalog(&count);
    int first = 1;
    for (size_t i = 0; i < count; i++) {
        if (!(session->prepared->options.firmware_patches &
              (UINT64_C(1) << i)))
            continue;
        int written = snprintf(
            output + used, capacity - (size_t)used, "%s\"%s\"",
            first ? "" : ", ", catalog[i].name);
        if (written < 0 || (size_t)written >= capacity - (size_t)used)
            return -1;
        used += written;
        first = 0;
    }
    int written = snprintf(output + used, capacity - (size_t)used, "],\n");
    return written >= 0 && (size_t)written < capacity - (size_t)used
         ? 0 : -1;
}

static emu_error_code_t emu_cemu_snapshot_write_diagnostic(
        void *opaque, const char *path, const char *provenance,
        int full_bins, emu_error_t *error) {
    emu_cemu_session_t *session = opaque;
    char expected[2048];
    if (!session || !path || !*path || !provenance ||
        emu_cemu_snapshot_provenance(
            session, expected, sizeof expected) != 0 ||
        strcmp(expected, provenance))
        return emu_cemu_fail(error, EMU_ERR_ARGUMENT,
                             "invalid CEMU snapshot provenance");
    snapshot_write_options_t options = {
        .full_bins = full_bins != 0,
        .host_json = provenance,
    };
    if (snapshot_write_dir_ex(
            path, cemu_core_private_cpu(session->core),
            cemu_core_private_soc(session->core), NULL, options) != 0)
        return emu_cemu_fail(error, EMU_ERR_IO,
                             "cannot write CEMU snapshot");
    return emu_cemu_status(cemu_status_ok(), error);
}

static emu_error_code_t emu_cemu_flash_size_diagnostic(
        void *opaque, size_t *size, emu_error_t *error) {
    emu_cemu_session_t *session = opaque;
    soc_t *soc = session ? cemu_core_private_soc(session->core) : NULL;
    if (!soc || !size)
        return emu_cemu_fail(error, EMU_ERR_ARGUMENT,
                             "invalid CEMU flash export");
    *size = soc->memory.flash_len;
    return emu_cemu_status(cemu_status_ok(), error);
}

static emu_error_code_t emu_cemu_flash_read_diagnostic(
        void *opaque, size_t offset, uint8_t *bytes, size_t size,
        emu_error_t *error) {
    emu_cemu_session_t *session = opaque;
    soc_t *soc = session ? cemu_core_private_soc(session->core) : NULL;
    if (!soc || (!bytes && size) || offset > soc->memory.flash_len ||
        size > soc->memory.flash_len - offset)
        return emu_cemu_fail(error, EMU_ERR_ARGUMENT,
                             "invalid CEMU flash export range");
    for (size_t i = 0; i < size; i++)
        bytes[i] = cemu_memory_controller_flash_dump_read8(
            &soc->memory, offset + i);
    return emu_cemu_status(cemu_status_ok(), error);
}

static emu_error_code_t emu_cemu_trace_attach_diagnostic(
        void *opaque, emu_trace_sink_t *sink, int deferred,
        int gsm_l1_explicit,
        emu_error_t *error) {
    emu_cemu_session_t *session = opaque;
    if (!session || !sink || session->trace_adapter)
        return emu_cemu_fail(error, EMU_ERR_ARGUMENT,
                             "invalid CEMU trace attachment");
    if (emu_trace_sink_accepts(sink, "gsm_l1_firmware")) {
        cemu_diagnostic_image_t image = {
            .model = session->prepared->metadata.model,
            .software_version = session->prepared->metadata.software_version,
        };
        char detail[256] = "";
        int result = gsm_l1_probe_attach(
            &session->gsm_l1_probe,
            cemu_core_private_soc(session->core),
            cemu_core_private_cpu(session->core), &session->config,
            &image, detail, sizeof detail);
        if (result < 0 || (result > 0 && gsm_l1_explicit && detail[0]))
            return emu_cemu_fail(error, EMU_ERR_ENGINE, detail);
    }
    if (emu_cemu_trace_attach_deferred(
            session->core, sink, deferred,
            &session->trace_adapter) != 0) {
        gsm_l1_probe_detach(&session->gsm_l1_probe);
        return emu_cemu_fail(error, EMU_ERR_ENGINE,
                             "cannot attach trace translator");
    }
    if (!deferred)
        emu_cemu_emit_patch_trace(session);
    return emu_cemu_status(cemu_status_ok(), error);
}

static emu_error_code_t emu_cemu_trace_arm_diagnostic(
        void *opaque, emu_error_t *error) {
    emu_cemu_session_t *session = opaque;
    if (!session || !session->trace_adapter)
        return emu_cemu_fail(error, EMU_ERR_LIFECYCLE,
                             "CEMU trace is not attached");
    if (emu_cemu_trace_set_enabled(session->trace_adapter, 1))
        return emu_cemu_fail(error, EMU_ERR_ENGINE,
                             "cannot initialize logical EEPROM trace");
    emu_cemu_emit_patch_trace(session);
    return emu_cemu_status(cemu_status_ok(), error);
}

static emu_error_code_t emu_cemu_trace_detach_diagnostic(
        void *opaque, emu_error_t *error) {
    emu_cemu_session_t *session = opaque;
    if (!session)
        return emu_cemu_fail(error, EMU_ERR_ARGUMENT,
                             "invalid CEMU trace detachment");
    gsm_l1_probe_detach(&session->gsm_l1_probe);
    emu_cemu_trace_detach(&session->trace_adapter);
    return emu_cemu_status(cemu_status_ok(), error);
}

static emu_error_code_t emu_cemu_drcov_attach_diagnostic(
        void *opaque, emu_drcov_t **collector, emu_error_t *error) {
    emu_cemu_session_t *session = opaque;
    if (!session || !collector || *collector || session->drcov_adapter)
        return emu_cemu_fail(error, EMU_ERR_ARGUMENT,
                             "invalid CEMU drcov attachment");
    emu_drcov_result_t result = emu_cemu_drcov_create(collector);
    if (result == EMU_DRCOV_OK)
        result = emu_cemu_drcov_attach_core(
            *collector, session->core, &session->drcov_adapter);
    if (result != EMU_DRCOV_OK) {
        emu_drcov_destroy(collector);
        return emu_cemu_fail(error,
            result == EMU_DRCOV_ERR_NOMEM ? EMU_ERR_NOMEM : EMU_ERR_ENGINE,
            "cannot attach drcov collector");
    }
    return emu_cemu_status(cemu_status_ok(), error);
}

static emu_error_code_t emu_cemu_drcov_detach_diagnostic(
        void *opaque, emu_error_t *error) {
    emu_cemu_session_t *session = opaque;
    if (!session)
        return emu_cemu_fail(error, EMU_ERR_ARGUMENT,
                             "invalid CEMU drcov detachment");
    emu_drcov_result_t result =
        emu_cemu_drcov_result(session->drcov_adapter);
    emu_cemu_drcov_detach(&session->drcov_adapter);
    if (result != EMU_DRCOV_OK)
        return emu_cemu_fail(error, EMU_ERR_ENGINE,
                             "drcov collection failed");
    return emu_cemu_status(cemu_status_ok(), error);
}

typedef struct {
    emu_text_io_t io;
    int failed;
} emu_cemu_text_cookie_t;

static ssize_t emu_cemu_text_read(void *opaque, char *bytes, size_t size) {
    emu_cemu_text_cookie_t *cookie = opaque;
    if (!cookie->io.read) return 0;
    ptrdiff_t count = cookie->io.read(cookie->io.opaque, bytes, size);
    if (count == -2) { errno = EAGAIN; return -1; }
    if (count < 0 || (size_t)count > size) {
        cookie->failed = 1;
        errno = EIO;
        return -1;
    }
    return (ssize_t)count;
}

static ssize_t emu_cemu_text_write(
        void *opaque, const char *bytes, size_t size) {
    emu_cemu_text_cookie_t *cookie = opaque;
    if (!cookie->io.write || cookie->io.write(
            cookie->io.opaque, bytes, size) != 0) {
        cookie->failed = 1;
        errno = EIO;
        return -1;
    }
    return (ssize_t)size;
}

#if defined(__APPLE__)
static int emu_cemu_text_read_bsd(void *opaque, char *bytes, int size) {
    return (int)emu_cemu_text_read(opaque, bytes, (size_t)size);
}

static int emu_cemu_text_write_bsd(
        void *opaque, const char *bytes, int size) {
    return (int)emu_cemu_text_write(opaque, bytes, (size_t)size);
}
#endif

static FILE *emu_cemu_text_stream(
        emu_cemu_text_cookie_t *cookie, const emu_text_io_t *io,
        int input) {
    *cookie = (emu_cemu_text_cookie_t){.io = *io};
#if defined(__APPLE__)
    FILE *stream = funopen(
        cookie, input ? emu_cemu_text_read_bsd : NULL,
        input ? NULL : emu_cemu_text_write_bsd, NULL, NULL);
#else
    cookie_io_functions_t functions = {0};
    if (input) functions.read = emu_cemu_text_read;
    else functions.write = emu_cemu_text_write;
    FILE *stream = fopencookie(cookie, input ? "r" : "w", functions);
#endif
    if (stream && !input) setvbuf(stream, NULL, _IONBF, 0);
    return stream;
}

typedef struct {
    emu_cemu_session_t *session;
    const emu_debugger_request_t *request;
    uint64_t next_service_ns;
} emu_cemu_debug_service_t;

static int emu_cemu_debug_service(void *opaque, int phase,
                                   uint64_t ticks, uint64_t guest) {
    emu_cemu_debug_service_t *context = opaque;
    emu_cemu_session_t *session = context->session;
    struct timespec time;
    clock_gettime(CLOCK_MONOTONIC, &time);
    uint64_t now = (uint64_t)time.tv_sec * UINT64_C(1000000000) + time.tv_nsec;
    if (phase == 1 && now < context->next_service_ns) return 0;
    context->next_service_ns = now + UINT64_C(10000000);
    if (phase == 2) {
        soc_t *soc = cemu_core_private_soc(session->core);
        /* History is host-owned and append-only, even when guest TX rewinds. */
        session->serial_offset = soc->serial_tx_len;
        cemu_serial_link_state_t state;
        if (cemu_core_set_serial_link(session->core,
                session->serial_attachment != EMU_SERIAL_LINK_DETACHED,
                &state).code != CEMU_STATUS_OK) return -1;
        cemu_audio_reset(&soc->audio);
        emu_cemu_audio_reset(session, EMU_AUDIO_RESET_SNAPSHOT);
    }
    emu_cemu_deliver(session, EMU_EVENT_POLL, "debugger-service");
    if (session->capture_failed) return -1;
    return context->request->service(context->request->service_opaque,
                                      phase, ticks, guest);
}

static emu_error_code_t emu_cemu_debugger_run_diagnostic(
        void *opaque, const emu_debugger_request_t *request,
        emu_error_t *error) {
    emu_cemu_session_t *session = opaque;
    if (!session || !request || !request->output.write)
        return emu_cemu_fail(error, EMU_ERR_ARGUMENT,
                             "invalid CEMU debugger request");
    emu_cemu_text_cookie_t output_cookie;
    FILE *output = emu_cemu_text_stream(
        &output_cookie, &request->output, 0);
    if (!output)
        return emu_cemu_fail(error, EMU_ERR_IO,
                             "cannot open CEMU debugger output");
    emu_cemu_text_cookie_t input_cookie = {0};
    FILE *input = NULL;
    int wants_repl = request->interactive || request->default_interactive;
    if (wants_repl) {
        if (!request->input.read)
            output_cookie.failed = 1;
        else
            input = emu_cemu_text_stream(
                &input_cookie, &request->input, 1);
    }
    debugger_t debugger;
    debugger_init(
        &debugger, cemu_core_private_cpu(session->core),
        cemu_core_private_soc(session->core), 1);
    emu_cemu_debug_service_t service = {.session = session, .request = request};
    if (request->service) {
        debugger.service = emu_cemu_debug_service;
        debugger.service_opaque = &service;
    }
    debugger.input_wait = request->input_wait;
    debugger.input_wait_opaque = request->input.opaque;
    if (input && request->input_wait) setvbuf(input, NULL, _IONBF, 0);
    debugger_set_batch_idle(&debugger, request->batch_idle);
    if (request->monitor) debugger_set_monitor(&debugger, 1);
    if (session->capture_strict || session->capture_raw)
        debugger_set_after_step(
            &debugger, emu_cemu_after_step, session);
    emu_cemu_eeprom_debug_context_t eeprom_debug = {0};
    if (emu_cemu_debug_eeprom_attach(
            &eeprom_debug, session, &debugger)) {
        debugger_detach(&debugger);
        (void)debugger_free(&debugger);
        fclose(output);
        if (input) fclose(input);
        return emu_cemu_fail(error, EMU_ERR_IO,
                             "cannot attach EEPROM debugger observer");
    }
    int quit = 0;
    if (request->script)
        quit = debugger_run_script(&debugger, request->script, output);
    if (!quit && request->command)
        quit = debugger_run_script(&debugger, request->command, output);
    if (!quit && wants_repl && input)
        debugger_repl(&debugger, input, output);
    emu_cemu_debug_eeprom_detach(&eeprom_debug);
    debugger_detach(&debugger);
    int service_failed = debugger.service_result < 0;
    int cleanup_failed = debugger_free(&debugger) != 0;
    if (input && fclose(input) != 0) input_cookie.failed = 1;
    if (fflush(output) != 0 || ferror(output)) output_cookie.failed = 1;
    if (fclose(output) != 0) output_cookie.failed = 1;
    if (service_failed || cleanup_failed || input_cookie.failed || output_cookie.failed)
        return emu_cemu_fail(error, EMU_ERR_IO,
                             service_failed ? "CEMU debugger service failed" :
                             cleanup_failed
                                 ? "cannot finalize CEMU debugger trace"
                                 : "CEMU debugger text I/O failed");
    return emu_cemu_status(cemu_status_ok(), error);
}

static void emu_cemu_monitor_reason(
        const loop_report_t *loop, char *output, size_t capacity) {
    if (!strcmp(loop->verdict, "waiting_io"))
        snprintf(output, capacity,
                 "loop at 0x%06x polling SFR(s) the model never updates "
                 "(likely unmodeled hardware) after %llu iterations",
                 loop->back_edge_pc,
                 (unsigned long long)loop->iterations);
    else if (!strcmp(loop->verdict, "waiting_dpram"))
        snprintf(output, capacity,
                 "loop at 0x%06x depends on DPRAM with no currently "
                 "modeled producer after %llu iterations",
                 loop->back_edge_pc,
                 (unsigned long long)loop->iterations);
    else
        snprintf(output, capacity,
                 "stuck spin at 0x%06x: no state change after %llu iterations",
                 loop->back_edge_pc,
                 (unsigned long long)loop->iterations);
}

static void emu_cemu_monitor_stop_event(
        void *opaque, const cemu_event_t *event) {
    emu_cemu_session_t *session = opaque;
    if (event->type == CEMU_EVENT_INSTRUCTION && session->monitor &&
        session->monitor->verdict_pending)
        (void)cemu_core_request_stop(session->core);
}

static emu_error_code_t emu_cemu_monitor_start_diagnostic(
        void *opaque, uint64_t loop_threshold, uint64_t stall_window,
        emu_error_t *error) {
    emu_cemu_session_t *session = opaque;
    if (!session || session->monitor)
        return emu_cemu_fail(error, EMU_ERR_LIFECYCLE,
                             "invalid CEMU monitor start");
    soc_t *soc = cemu_core_private_soc(session->core);
    cpu_t *cpu = cemu_core_private_cpu(session->core);
    if (!soc || !cpu)
        return emu_cemu_fail(error, EMU_ERR_ENGINE,
                             "CEMU diagnostic state is unavailable");
    cemu_status_t status = cemu_soc_enable_stats(soc);
    if (status.code != CEMU_STATUS_OK)
        return emu_cemu_status(status, error);
    session->monitor = calloc(1, sizeof *session->monitor);
    if (!session->monitor) {
        cemu_soc_disable_stats(soc);
        return emu_cemu_fail(error, EMU_ERR_NOMEM,
                             "cannot allocate CEMU monitor");
    }
    monitor_init(
        session->monitor, soc, cpu, loop_threshold, stall_window);
    unsigned subscription = 0;
    status = cemu_core_subscribe_events(
        session->core, CEMU_EVENT_INSTRUCTION,
        emu_cemu_monitor_stop_event, NULL, session, &subscription);
    if (status.code != CEMU_STATUS_OK) {
        monitor_free(session->monitor);
        free(session->monitor);
        session->monitor = NULL;
        cemu_soc_disable_stats(soc);
        return emu_cemu_status(status, error);
    }
    session->monitor_stop_subscription = subscription;
    session->monitor_have_loop = 0;
    memset(&session->monitor_loop, 0, sizeof session->monitor_loop);
    return emu_cemu_status(cemu_status_ok(), error);
}

static emu_error_code_t emu_cemu_monitor_poll_diagnostic(
        void *opaque, emu_monitor_verdict_t *verdict,
        emu_error_t *error) {
    emu_cemu_session_t *session = opaque;
    if (!session || !session->monitor || !verdict)
        return emu_cemu_fail(error, EMU_ERR_LIFECYCLE,
                             "CEMU monitor is not active");
    memset(verdict, 0, sizeof *verdict);
    loop_report_t loop;
    if (monitor_take_verdict(session->monitor, &loop)) {
        session->monitor_loop = loop;
        session->monitor_have_loop = 1;
        snprintf(verdict->status, sizeof verdict->status, "%s",
                 loop.verdict);
        emu_cemu_monitor_reason(
            &loop, verdict->reason, sizeof verdict->reason);
    }
    return emu_cemu_status(cemu_status_ok(), error);
}

static emu_error_code_t emu_cemu_summary_format_diagnostic(
        void *opaque, const emu_summary_request_t *request,
        char *output, size_t capacity, emu_error_t *error) {
    emu_cemu_session_t *session = opaque;
    if (!session || !session->monitor || !request || !output || !capacity ||
        capacity > INT_MAX)
        return emu_cemu_fail(error, EMU_ERR_ARGUMENT,
                             "invalid CEMU summary request");
    run_result_t run = {
        .status = request->status,
        .reason = request->reason,
        .steps = request->steps,
        .pc = request->pc,
        .elapsed_s = request->elapsed_seconds,
        .have_loop = session->monitor_have_loop,
        .serial_already_printed = request->serial_already_printed,
    };
    if (session->monitor_have_loop) run.loop = session->monitor_loop;
    int used = format_run_summary(
        output, (int)capacity, &run,
        cemu_core_private_soc(session->core),
        cemu_core_private_cpu(session->core), session->monitor,
        request->color, request->show_writes);
    if (used < 0 || (size_t)used >= capacity)
        return emu_cemu_fail(error, EMU_ERR_IO,
                             "CEMU summary output is too large");
    return emu_cemu_status(cemu_status_ok(), error);
}

static emu_error_code_t emu_cemu_monitor_stop_diagnostic(
        void *opaque, emu_error_t *error) {
    emu_cemu_session_t *session = opaque;
    if (!session)
        return emu_cemu_fail(error, EMU_ERR_ARGUMENT,
                             "invalid CEMU monitor stop");
    if (session->monitor) {
        if (session->monitor_stop_subscription)
            (void)cemu_core_unsubscribe_events(
                session->core, session->monitor_stop_subscription);
        session->monitor_stop_subscription = 0;
        monitor_free(session->monitor);
        free(session->monitor);
        session->monitor = NULL;
        cemu_soc_disable_stats(cemu_core_private_soc(session->core));
    }
    session->monitor_have_loop = 0;
    return emu_cemu_status(cemu_status_ok(), error);
}
#endif

static const emu_engine_diagnostic_ops_t emu_cemu_diagnostic_ops = {
    .snapshot_restore = emu_cemu_snapshot_restore,
#if CEMU_INSTRUMENTED
    .snapshot_write = emu_cemu_snapshot_write_diagnostic,
    .trace_attach = emu_cemu_trace_attach_diagnostic,
    .trace_arm = emu_cemu_trace_arm_diagnostic,
    .trace_detach = emu_cemu_trace_detach_diagnostic,
    .drcov_attach = emu_cemu_drcov_attach_diagnostic,
    .drcov_detach = emu_cemu_drcov_detach_diagnostic,
    .flash_size = emu_cemu_flash_size_diagnostic,
    .flash_read = emu_cemu_flash_read_diagnostic,
    .debugger_run = emu_cemu_debugger_run_diagnostic,
    .monitor_start = emu_cemu_monitor_start_diagnostic,
    .monitor_poll = emu_cemu_monitor_poll_diagnostic,
    .summary_format = emu_cemu_summary_format_diagnostic,
    .monitor_stop = emu_cemu_monitor_stop_diagnostic,
#endif
};

static const char *const emu_cemu_models[] = {
    "a52", "a55", "a60", "a62", "a65", "c55", "c60", "cf62",
    "m55", "mc60", "s55", "sl55",
};

static void emu_cemu_synthetic_print(FILE *out, unsigned mask) {
    fputs("SYNTHETIC behaviors (--synth NAMES; NAMES = comma list of name / no-name):\n",
          out);
    for (int i = 0; i < SYN_COUNT; i++) {
        int active = (mask >> i) & 1;
        fprintf(out, "  %-16s %s [default %s]%s\n",
                synth_registry[i].name, synth_registry[i].desc,
                synth_registry[i].dflt ? "on" : "off",
                active ? "  (ACTIVE)" : "");
    }
}

static const emu_engine_diagnostics_t emu_cemu_diagnostics = {
    .engine_name = "cemu",
#if CEMU_INSTRUMENTED
    .profile_name = "instrumented",
    .capabilities = EMU_DIAG_MONITOR | EMU_DIAG_ARTIFACTS |
                    EMU_DIAG_DEBUGGER | EMU_DIAG_SNAPSHOT |
                    EMU_DIAG_COVERAGE | EMU_DIAG_RAW_LCD |
                    EMU_DIAG_SNAPSHOT_RESTORE,
    .trace_available = CEMU_TRACE_PARQUET,
    .trace_mask = CEMU_TRACE_PARQUET ? EMU_TRACE_MASK_ALL : 0,
#else
    .profile_name = "plain",
    .capabilities = EMU_DIAG_SNAPSHOT_RESTORE,
    .trace_available = 0,
#endif
    .synthetic_defaults = 0,
    .synthetic_parse = synth_parse,
    .synthetic_print = emu_cemu_synthetic_print,
    .synthetic_names = synth_active_names,
};

const emu_engine_descriptor_t emu_cemu_engine_descriptor = {
    .name = "cemu",
    .version = "phase1-in-tree",
    .supported_models = emu_cemu_models,
    .supported_model_count = sizeof emu_cemu_models / sizeof emu_cemu_models[0],
    .capabilities = EMU_CAP_KEYS | EMU_CAP_SERIAL | EMU_CAP_SIM |
                    EMU_CAP_BATTERY | EMU_CAP_STORAGE_INIT | EMU_CAP_FRAMES |
                    EMU_CAP_EVENTS | EMU_CAP_KEY_SAMPLING |
                    EMU_CAP_SERIAL_LINK | EMU_CAP_ARTIFACT_REQ |
                    EMU_CAP_AUDIO,
    .diagnostics = &emu_cemu_diagnostics,
    .diagnostic_ops = &emu_cemu_diagnostic_ops,
    .ops = {
        .create = emu_cemu_create,
        .run = emu_cemu_run,
        .reset = emu_cemu_reset,
        .request_stop = emu_cemu_stop,
        .query = emu_cemu_query,
        .destroy = emu_cemu_destroy,
        .key = emu_cemu_key,
        .serial_rx = emu_cemu_serial,
        .sim = emu_cemu_sim,
        .battery = emu_cemu_battery,
        .poll = emu_cemu_poll,
        .select_events = emu_cemu_select_events,
        .key_query = emu_cemu_key_query,
        .serial_link = emu_cemu_serial_link,
        .artifact = emu_cemu_artifact,
    },
};

emu_error_code_t emu_cemu_raw_run(
        const emu_prepared_session_t *prepared, uint64_t budget,
        emu_run_result_t *result, emu_error_t *error) {
    if (!prepared || !result || !budget)
        return emu_cemu_fail(error, EMU_ERR_ARGUMENT, "invalid raw run");
    emu_cemu_session_t session = {.prepared = prepared};
    char detail[256];
    if (!emu_cemu_config_from_prepared(
            prepared, &session.config, detail, sizeof detail))
        return emu_cemu_fail(
            error, EMU_ERR_INVALID_PREPARED_SESSION, detail);
    emu_cemu_storage_context_init(&session.storage, prepared);
    cemu_core_options_t options = emu_cemu_core_options(&session);
    cemu_status_t status = cemu_core_create(&session.core, &options);
    if (status.code != CEMU_STATUS_OK) return emu_cemu_status(status, error);
    cemu_run_result_t core_result;
    status = cemu_core_run_slice(session.core, budget, &core_result);
    if (status.code == CEMU_STATUS_OK && core_result.reason != CEMU_RUN_ERROR)
        emu_cemu_result(&session, &core_result, 1, result);
    else if (status.code == CEMU_STATUS_OK) {
        cemu_core_state_t state;
        if (cemu_core_query(session.core, &state).code == CEMU_STATUS_OK)
            status = state.error;
        else
            status = cemu_status_error(CEMU_STATUS_LIFECYCLE,
                                       "CEMU core entered an error state");
    }
    cemu_core_destroy(session.core);
    return emu_cemu_status(status, error);
}
