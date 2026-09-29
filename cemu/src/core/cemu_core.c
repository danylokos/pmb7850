#include "cemu_core_private.h"
#include "cemu_core_storage_private.h"

#include <stdlib.h>
#include <string.h>

#include "battery.h"
#include "gsm_legacy_adapter.h"
#include "gsm_stub.h"
#include "keypad.h"
#include "lcd.h"
#include "sim.h"

static peripheral_t *cemu_soc_peripheral(soc_t *soc, const char *id) {
    if (!soc || !id) return NULL;
    for (int i = 0; i < soc->n_peripherals; i++) {
        peripheral_t *peripheral = soc->peripherals[i];
        if (peripheral && peripheral->id && !strcmp(peripheral->id, id))
            return peripheral;
    }
    return NULL;
}

static void cemu_machine_destroy(cemu_machine_t *machine) {
    if (!machine) return;
    cemu_soc_free(&machine->soc);
    free(machine);
}

static cemu_status_t cemu_machine_create(
    const cemu_core_options_t *options, cemu_machine_t **out,
    cemu_storage_report_t *storage_report) {
    *out = NULL;
    cemu_machine_t *machine = cemu_calloc(1, sizeof(*machine));
    if (!machine)
        return cemu_status_error(CEMU_STATUS_ALLOCATION_FAILED,
                                 "cannot allocate CEMU machine");

    cemu_status_t status = cemu_soc_init(
        &machine->soc, options->source, options->source_size,
        &options->device, options->synthetic_mask,
        options->serial_autobaud_bypass);
    if (status.code != CEMU_STATUS_OK) {
        cemu_machine_destroy(machine);
        return status;
    }
    status = cemu_core_storage_apply(
        machine, options->storage_initializer, options->storage_opaque,
        CEMU_STORAGE_PRE_RESET, storage_report);
    if (status.code != CEMU_STATUS_OK) {
        cemu_machine_destroy(machine);
        return status;
    }
    cemu_cpu_init(&machine->cpu, &machine->soc.bus);
    cemu_soc_attach_cpu(&machine->soc, &machine->cpu);
    cemu_audio_attach(&machine->soc.audio, options->audio_output,
                      options->audio_reset, options->audio_opaque);
    cemu_cpu_reset(&machine->cpu);
    if (options->gsm_stub_enabled &&
        (!cemu_gsm_stub_available(machine->soc.gsm_stub_periph) ||
         options->device.baseband.software_version <= 0)) {
        cemu_machine_destroy(machine);
        return cemu_status_error(
            CEMU_STATUS_UNSUPPORTED,
            "no qualified baseband/L1 profile for selected software");
    }
    cemu_gsm_stub_set_enabled(
        machine->soc.gsm_stub_periph, &machine->soc,
        options->gsm_stub_enabled);
    cemu_gsm_legacy_adapter_set_enabled(
        machine->soc.gsm_legacy_adapter_periph,
        options->gsm_stub_enabled);
    *out = machine;
    return cemu_status_ok();
}

static void cemu_storage_report_deliver(
        const cemu_core_options_t *options,
        const cemu_storage_report_t *report) {
    if (!options->storage_result || !report) return;
    for (size_t i = 0; i < report->count; i++) {
        const cemu_storage_report_entry_t *entry = &report->entries[i];
        options->storage_result(options->storage_opaque, entry->stage,
                                entry->group, entry->disposition);
    }
}

cemu_status_t cemu_core_create(cemu_core_t **out,
                               const cemu_core_options_t *options) {
    if (out) *out = NULL;
    if (!out || !options ||
        (options->source_size && !options->source))
        return cemu_status_error(CEMU_STATUS_INVALID_ARGUMENT,
                                 "invalid CEMU core creation request");

    cemu_core_t *core = cemu_calloc(1, sizeof(*core));
    if (!core)
        return cemu_status_error(CEMU_STATUS_ALLOCATION_FAILED,
                                 "cannot allocate CEMU core");
    core->options = *options;
    cemu_storage_report_t storage_report = {0};
    cemu_status_t status = cemu_machine_create(
        options, &core->machine,
        options->storage_result ? &storage_report : NULL);
    if (status.code != CEMU_STATUS_OK) {
        cemu_core_storage_report_free(&storage_report);
        cemu_core_destroy(core);
        return status;
    }
    if (!options->defer_post_reset_storage) {
        status = cemu_core_storage_apply(
            core->machine, options->storage_initializer,
            options->storage_opaque, CEMU_STORAGE_POST_RESET,
            options->storage_result ? &storage_report : NULL);
        if (status.code != CEMU_STATUS_OK) {
            cemu_core_storage_report_free(&storage_report);
            cemu_core_destroy(core);
            return status;
        }
        core->post_reset_storage_applied = 1;
    }
    *out = core;
    cemu_storage_report_deliver(options, &storage_report);
    cemu_core_storage_report_free(&storage_report);
    return cemu_status_ok();
}

cemu_status_t cemu_core_apply_post_restore_storage(cemu_core_t *core) {
    if (!core || !core->machine)
        return cemu_status_error(CEMU_STATUS_INVALID_ARGUMENT,
                                 "invalid post-restore storage request");
    if (core->post_reset_storage_applied)
        return cemu_status_error(CEMU_STATUS_LIFECYCLE,
                                 "post-restore storage already applied");
    cemu_storage_report_t storage_report = {0};
    cemu_status_t status = cemu_core_storage_apply(
        core->machine, core->options.storage_initializer,
        core->options.storage_opaque, CEMU_STORAGE_POST_RESET,
        core->options.storage_result ? &storage_report : NULL);
    if (status.code == CEMU_STATUS_OK) {
        core->post_reset_storage_applied = 1;
        cemu_storage_report_deliver(&core->options, &storage_report);
    }
    cemu_core_storage_report_free(&storage_report);
    return status;
}

cemu_status_t cemu_core_run_request(cemu_core_t *core,
                                    const cemu_run_request_t *request,
                                    cemu_run_result_t *result) {
    if (!core || !core->machine || !request || !request->tick_budget ||
        !result)
        return cemu_status_error(CEMU_STATUS_INVALID_ARGUMENT,
                                 "invalid CEMU run slice");
    memset(result, 0, sizeof(*result));
    result->reason = core->stop_requested
                   ? CEMU_RUN_STOPPED : CEMU_RUN_SLICE_LIMIT;
    while (result->ticks < request->tick_budget &&
           result->reason == CEMU_RUN_SLICE_LIMIT) {
        if (core->machine->soc.runtime_error.code != CEMU_STATUS_OK) {
            result->reason = CEMU_RUN_ERROR;
            break;
        }
#if CEMU_INSTRUMENTED
        uint32_t pc_before = cpu_pc(&core->machine->cpu);
        char instruction_detail[128] = "";
        if (core->instruction_detail)
            core->instruction_detail(
                core->instruction_detail_opaque, instruction_detail,
                sizeof instruction_detail);
#endif
        uint64_t advanced = request->allow_idle_batch
            ? cemu_soc_batch_idle(
                  &core->machine->soc,
                  request->tick_budget - result->ticks)
            : 0;
        if (advanced) {
            result->ticks += advanced;
#if CEMU_INSTRUMENTED
            cemu_soc_instrument_idle_span(
                &core->machine->soc, pc_before, instruction_detail,
                core->machine->cpu.icount - advanced + 1u, advanced);
#endif
            if (core->after_step)
                core->after_step(core->after_step_opaque);
            continue;
        }
        step_result_t step = cemu_cpu_step(&core->machine->cpu);
#if CEMU_INSTRUMENTED
        cemu_soc_instrument_instruction(
            &core->machine->soc, pc_before, instruction_detail,
            core->machine->cpu.last_ran
                ? core->machine->cpu.fetch_len : 0);
#endif
        result->ticks++;
        if (core->machine->cpu.last_ran) {
            result->guest_instructions++;
            core->total_guest_instructions++;
        }
        if (core->after_step)
            core->after_step(core->after_step_opaque);
        if (step == STEP_UNIMPL)
            result->reason = CEMU_RUN_UNIMPLEMENTED;
        else if (core->machine->cpu.halted)
            result->reason = CEMU_RUN_HALTED;
        else if (core->stop_requested)
            result->reason = CEMU_RUN_STOPPED;
    }
    if (core->machine->soc.runtime_error.code != CEMU_STATUS_OK)
        result->reason = CEMU_RUN_ERROR;
    result->total_guest_instructions = core->total_guest_instructions;
    result->instruction_count = core->machine->cpu.icount;
    result->pc = cpu_pc(&core->machine->cpu);
    return cemu_status_ok();
}

cemu_status_t cemu_core_set_after_step(
        cemu_core_t *core, cemu_core_after_step_fn callback, void *opaque) {
    if (!core || !core->machine)
        return cemu_status_error(CEMU_STATUS_INVALID_ARGUMENT,
                                 "invalid CEMU after-step callback");
    core->after_step = callback;
    core->after_step_opaque = callback ? opaque : NULL;
    return cemu_status_ok();
}

cemu_status_t cemu_core_set_instruction_detail(
        cemu_core_t *core, cemu_core_instruction_detail_fn callback,
        void *opaque) {
    if (!core || !core->machine)
        return cemu_status_error(CEMU_STATUS_INVALID_ARGUMENT,
                                 "invalid CEMU instruction-detail callback");
    core->instruction_detail = callback;
    core->instruction_detail_opaque = callback ? opaque : NULL;
    return cemu_status_ok();
}

cemu_status_t cemu_core_flush_lcd_frame(cemu_core_t *core) {
    soc_t *soc = cemu_core_private_soc(core);
    if (!soc || !soc->lcd_periph)
        return cemu_status_error(CEMU_STATUS_UNSUPPORTED,
                                 "LCD frame is unavailable");
    (void)cemu_lcd_flush_pending_frame(soc->lcd_periph, soc);
    return cemu_status_ok();
}

cemu_status_t cemu_core_lcd_frame_state(
        cemu_core_t *core, uint64_t *sequence, uint64_t *icount) {
    soc_t *soc = cemu_core_private_soc(core);
    if (!soc || !soc->lcd_periph || !sequence || !icount)
        return cemu_status_error(CEMU_STATUS_INVALID_ARGUMENT,
                                 "invalid LCD frame-state request");
    *sequence = cemu_lcd_frame_sequence(soc->lcd_periph);
    *icount = cemu_lcd_last_frame_icount(soc->lcd_periph);
    return cemu_status_ok();
}

cemu_status_t cemu_core_run_slice(cemu_core_t *core, uint64_t budget,
                                  cemu_run_result_t *result) {
    cemu_run_request_t request = {.tick_budget = budget};
    return cemu_core_run_request(core, &request, result);
}

cemu_status_t cemu_core_reset(cemu_core_t *core) {
    if (!core || !core->machine)
        return cemu_status_error(CEMU_STATUS_INVALID_ARGUMENT,
                                 "invalid CEMU reset request");
    cemu_machine_t *replacement = NULL;
    cemu_storage_report_t storage_report = {0};
    cemu_status_t status = cemu_machine_create(
        &core->options, &replacement,
        core->options.storage_result ? &storage_report : NULL);
    if (status.code != CEMU_STATUS_OK) {
        cemu_core_storage_report_free(&storage_report);
        return status;
    }
    if (!core->options.defer_post_reset_storage) {
        status = cemu_core_storage_apply(
            replacement, core->options.storage_initializer,
            core->options.storage_opaque, CEMU_STORAGE_POST_RESET,
            core->options.storage_result ? &storage_report : NULL);
        if (status.code != CEMU_STATUS_OK) {
            cemu_machine_destroy(replacement);
            cemu_core_storage_report_free(&storage_report);
            return status;
        }
    }
    if (core->statistics_enabled) {
        status = cemu_soc_enable_stats(&replacement->soc);
        if (status.code != CEMU_STATUS_OK) {
            cemu_machine_destroy(replacement);
            cemu_core_storage_report_free(&storage_report);
            return status;
        }
    }
    cemu_soc_serial_link_attached(&replacement->soc,
                             core->serial_link_attached);
#if CEMU_INSTRUMENTED
    replacement->soc.instrumentation = core->machine->soc.instrumentation;
    replacement->cpu.instrumentation = &replacement->soc.instrumentation;
#endif

    cemu_machine_t *previous = core->machine;
    core->machine = replacement;
    core->total_guest_instructions = 0;
    core->stop_requested = 0;
    core->post_reset_storage_applied =
        !core->options.defer_post_reset_storage;
    cemu_machine_destroy(previous);
    cemu_storage_report_deliver(&core->options, &storage_report);
    cemu_core_storage_report_free(&storage_report);
    return cemu_status_ok();
}

cemu_status_t cemu_core_request_stop(cemu_core_t *core) {
    if (!core || !core->machine)
        return cemu_status_error(CEMU_STATUS_INVALID_ARGUMENT,
                                 "invalid CEMU stop request");
    core->stop_requested = 1;
    return cemu_status_ok();
}

cemu_status_t cemu_core_query(const cemu_core_t *core,
                              cemu_core_state_t *state) {
    soc_t *soc = cemu_core_private_soc((cemu_core_t *)core);
    const cpu_t *cpu = cemu_core_private_cpu_const(core);
    if (!soc || !cpu || !state)
        return cemu_status_error(CEMU_STATUS_INVALID_ARGUMENT,
                                 "invalid CEMU state query");
    uint64_t id_reads = 0, status_reads = 0, doorbell_rings = 0;
    int idle_wake_possible = cpu->idle
                          ? cemu_soc_idle_wake_possible(soc) : 0;
    interrupt_cache_stats_t interrupt_cache =
        cemu_interrupt_subsystem_cache_stats(&soc->interrupts);
    cemu_soc_xbus_unknown1_counts(
        soc, &id_reads, &status_reads, &doorbell_rings);
    *state = (cemu_core_state_t){
        .ticks = soc->ticks,
        .total_guest_instructions = core->total_guest_instructions,
        .instruction_count = cpu->icount,
        .pc = cpu_pc(cpu),
        .halted = cpu->halted,
        .unimplemented = cpu->unimpl,
        .unimplemented_opcode = cpu->unimpl_op,
        .unimplemented_pc = cpu->unimpl_pc,
        .stop_requested = core->stop_requested,
        .statistics_enabled = core->statistics_enabled,
        .interrupts_delivered = cpu->interrupts_delivered,
        .traps_taken = cpu->traps_taken,
        .idle = cpu->idle,
        .idle_wake_possible = idle_wake_possible,
        .xbus_unknown1_id_reads = id_reads,
        .xbus_unknown1_status_reads = status_reads,
        .xbus_unknown1_doorbell_rings = doorbell_rings,
        .interrupt_cache_queries = interrupt_cache.queries,
        .interrupt_cache_hits = interrupt_cache.hits,
        .interrupt_cache_scans = interrupt_cache.scans,
        .interrupt_cache_invalidations = interrupt_cache.invalidations,
        .error = soc->runtime_error,
    };
    return cemu_status_ok();
}

cemu_status_t cemu_core_render_frame(cemu_core_t *core, int raw_ddram,
                                     uint8_t *rgb, size_t capacity,
                                     cemu_frame_info_t *info) {
    soc_t *soc = cemu_core_private_soc(core);
    cpu_t *cpu = cemu_core_private_cpu(core);
    if (!soc || !rgb || !info)
        return cemu_status_error(CEMU_STATUS_INVALID_ARGUMENT,
                                 "invalid frame request");
    peripheral_t *lcd = soc->lcd_periph;
    if (!lcd)
        return cemu_status_error(CEMU_STATUS_UNSUPPORTED,
                                 "LCD frame is unavailable");
    unsigned width = 0, height = 0;
    cemu_lcd_dimensions(lcd, raw_ddram != 0, &width, &height);
    size_t size = (size_t)width * height * 3u;
    if (!width || !height || size > capacity)
        return cemu_status_error(CEMU_STATUS_INVALID_ARGUMENT,
                                 "frame buffer is too small");
    if (cemu_lcd_render_rgb(lcd, raw_ddram != 0, 1, rgb, size) != 0)
        return cemu_status_error(CEMU_STATUS_INITIALIZER_FAILED,
                                 "cannot render LCD frame");
    *info = (cemu_frame_info_t){
        .width = width,
        .height = height,
        .size = size,
        .sequence = cemu_lcd_frame_sequence(lcd),
        .instruction_count = cpu ? cpu->icount : 0,
    };
    return cemu_status_ok();
}

cemu_status_t cemu_core_render_capture_frame(
        cemu_core_t *core, int raw_ddram, unsigned scale,
        uint8_t *rgb, size_t capacity, cemu_frame_info_t *info) {
    soc_t *soc = cemu_core_private_soc(core);
    if (!soc || !soc->lcd_periph || !scale || !rgb || !info)
        return cemu_status_error(CEMU_STATUS_INVALID_ARGUMENT,
                                 "invalid LCD capture frame request");
    unsigned native_width = 0, native_height = 0;
    cemu_lcd_dimensions(soc->lcd_periph, raw_ddram != 0,
                        &native_width, &native_height);
    unsigned width = native_width * scale;
    unsigned height = native_height * scale;
    size_t size = (size_t)width * height * 3u;
    if (!native_width || !native_height || width / scale != native_width ||
        height / scale != native_height || size > capacity)
        return cemu_status_error(CEMU_STATUS_INVALID_ARGUMENT,
                                 "LCD capture frame buffer is too small");
    if (cemu_lcd_render_rgb(
            soc->lcd_periph, raw_ddram != 0, scale, rgb, size) != 0)
        return cemu_status_error(CEMU_STATUS_INITIALIZER_FAILED,
                                 "cannot render LCD capture frame");
    *info = (cemu_frame_info_t){
        .width = width,
        .height = height,
        .size = size,
        .sequence = cemu_lcd_frame_sequence(soc->lcd_periph),
        .instruction_count =
            cemu_lcd_last_frame_icount(soc->lcd_periph),
    };
    return cemu_status_ok();
}

cemu_status_t cemu_core_set_key(cemu_core_t *core, const char *name,
                                int pressed) {
    soc_t *soc = cemu_core_private_soc(core);
    if (!soc || !name)
        return cemu_status_error(CEMU_STATUS_INVALID_ARGUMENT,
                                 "invalid key request");
    peripheral_t *peripheral = cemu_soc_peripheral(soc, "keypad");
    keypad_state_t *keypad = peripheral ? peripheral->state : NULL;
    if (!keypad || cemu_keypad_set_button(keypad, soc, name, pressed != 0) < 0)
        return cemu_status_error(CEMU_STATUS_INVALID_ARGUMENT,
                                 "unknown key: %s", name);
    return cemu_status_ok();
}

cemu_status_t cemu_core_key_count(cemu_core_t *core, size_t *count) {
    soc_t *soc = cemu_core_private_soc(core);
    if (!soc || !count)
        return cemu_status_error(CEMU_STATUS_INVALID_ARGUMENT,
                                 "invalid key-count query");
    peripheral_t *peripheral = cemu_soc_peripheral(soc, "keypad");
    keypad_state_t *keypad = peripheral ? peripheral->state : NULL;
    if (!keypad || !cemu_keypad_buttons(keypad, count))
        return cemu_status_error(CEMU_STATUS_UNSUPPORTED,
                                 "keypad is unavailable");
    return cemu_status_ok();
}

cemu_status_t cemu_core_key_name(cemu_core_t *core, size_t index,
                                 const char **name) {
    soc_t *soc = cemu_core_private_soc(core);
    if (name) *name = NULL;
    if (!soc || !name)
        return cemu_status_error(CEMU_STATUS_INVALID_ARGUMENT,
                                 "invalid key-name query");
    peripheral_t *peripheral = cemu_soc_peripheral(soc, "keypad");
    keypad_state_t *keypad = peripheral ? peripheral->state : NULL;
    size_t count = 0;
    const keypad_button_t *buttons = cemu_keypad_buttons(keypad, &count);
    if (!buttons || index >= count)
        return cemu_status_error(CEMU_STATUS_INVALID_ARGUMENT,
                                 "invalid key index");
    *name = buttons[index].name;
    return cemu_status_ok();
}

cemu_status_t cemu_core_query_key(cemu_core_t *core, const char *name,
                                  cemu_key_state_t *state) {
    soc_t *soc = cemu_core_private_soc(core);
    cpu_t *cpu = cemu_core_private_cpu(core);
    if (!soc || !name || !state)
        return cemu_status_error(CEMU_STATUS_INVALID_ARGUMENT,
                                 "invalid key query");
    peripheral_t *peripheral = cemu_soc_peripheral(soc, "keypad");
    keypad_state_t *keypad = peripheral ? peripheral->state : NULL;
    const keypad_button_t *button = keypad
        ? cemu_keypad_button_by_name(keypad, name) : NULL;
    if (!button)
        return cemu_status_error(CEMU_STATUS_INVALID_ARGUMENT,
                                 "unknown key: %s", name);
    size_t index = (size_t)(button - keypad->buttons);
    uint32_t bit = UINT32_C(1) << index;
    *state = (cemu_key_state_t){
        .pressed = (keypad->pressed & bit) != 0,
        .sampled = (cemu_keypad_sampled_buttons(keypad) & bit) != 0,
        .instruction_count = cpu ? cpu->icount : 0,
    };
    return cemu_status_ok();
}

cemu_status_t cemu_core_feed_serial(cemu_core_t *core, const uint8_t *data,
                                    size_t size) {
    soc_t *soc = cemu_core_private_soc(core);
    if (!soc || (!data && size))
        return cemu_status_error(CEMU_STATUS_INVALID_ARGUMENT,
                                 "invalid serial input");
    cemu_status_t status = cemu_soc_feed_serial(soc, data, size);
    if (status.code == CEMU_STATUS_OK)
        core->serial_received_bytes += size;
    return status;
}

cemu_status_t cemu_core_serial_tx_view(cemu_core_t *core, size_t cursor,
                                       cemu_serial_tx_view_t *view) {
    soc_t *soc = cemu_core_private_soc(core);
    if (!soc || !view || cursor > soc->serial_tx_len)
        return cemu_status_error(CEMU_STATUS_INVALID_ARGUMENT,
                                 "invalid serial transmit cursor");
    *view = (cemu_serial_tx_view_t){
        .data = soc->serial_tx ? soc->serial_tx + cursor : NULL,
        .size = soc->serial_tx_len - cursor,
        .next_cursor = soc->serial_tx_len,
    };
    return cemu_status_ok();
}

cemu_status_t cemu_core_set_serial_link(cemu_core_t *core, int attached,
                                        cemu_serial_link_state_t *state) {
    soc_t *soc = cemu_core_private_soc(core);
    if (!soc || !state)
        return cemu_status_error(CEMU_STATUS_INVALID_ARGUMENT,
                                 "invalid serial link request");
    core->serial_link_attached = attached != 0;
    cemu_soc_serial_link_attached(soc, core->serial_link_attached);
    *state = (cemu_serial_link_state_t){
        .available = soc->serial_link.available,
        .attached = core->serial_link_attached,
        .received_bytes = core->serial_received_bytes,
        .transmitted_bytes = soc->serial_tx_len,
    };
    return cemu_status_ok();
}

cemu_status_t cemu_core_set_sim_attached(cemu_core_t *core, int attached) {
    soc_t *soc = cemu_core_private_soc(core);
    if (!soc || !soc->sim_periph)
        return cemu_status_error(CEMU_STATUS_UNSUPPORTED,
                                 "SIM control is unavailable");
    cemu_sim_set_mode(soc->sim_periph, soc,
                 attached ? SIM_MODE_STUB : SIM_MODE_NONE);
    return cemu_status_ok();
}

cemu_status_t cemu_core_set_battery(cemu_core_t *core, unsigned level,
                                    int charging) {
    soc_t *soc = cemu_core_private_soc(core);
    if (!soc)
        return cemu_status_error(CEMU_STATUS_INVALID_ARGUMENT,
                                 "invalid battery request");
    peripheral_t *battery = soc->battery_periph;
    if (!cemu_battery_available(battery))
        return cemu_status_error(CEMU_STATUS_UNSUPPORTED,
                                 "battery control is unavailable");
    if (!cemu_battery_set_state(battery, soc, level, charging != 0, "core"))
        return cemu_status_error(CEMU_STATUS_INVALID_ARGUMENT,
                                 "invalid battery state");
    return cemu_status_ok();
}

cemu_status_t cemu_core_enable_statistics(cemu_core_t *core) {
    soc_t *soc = cemu_core_private_soc(core);
    if (!soc)
        return cemu_status_error(CEMU_STATUS_INVALID_ARGUMENT,
                                 "invalid statistics request");
    cemu_status_t status = cemu_soc_enable_stats(soc);
    if (status.code == CEMU_STATUS_OK) core->statistics_enabled = 1;
    return status;
}

cemu_status_t cemu_core_subscribe_events(
        cemu_core_t *core, cemu_event_mask_t mask,
        cemu_event_consumer_fn consumer, cemu_event_filter_fn filter,
        void *opaque, unsigned *subscription) {
    if (subscription) *subscription = 0;
    soc_t *soc = cemu_core_private_soc(core);
    if (!soc || !mask || !consumer || !subscription)
        return cemu_status_error(CEMU_STATUS_INVALID_ARGUMENT,
                                 "invalid core event subscription");
    unsigned id = cemu_event_subscribe_filtered(
        &soc->instrumentation, mask, consumer, filter, opaque);
    if (!id)
        return cemu_status_error(
#if CEMU_INSTRUMENTED
            CEMU_STATUS_ALLOCATION_FAILED,
            "CEMU event subscription capacity is exhausted"
#else
            CEMU_STATUS_UNSUPPORTED,
            "CEMU events are unavailable in the plain profile"
#endif
        );
    *subscription = id;
    return cemu_status_ok();
}

cemu_status_t cemu_core_unsubscribe_events(cemu_core_t *core,
                                            unsigned subscription) {
    soc_t *soc = cemu_core_private_soc(core);
    if (!soc || !subscription)
        return cemu_status_error(CEMU_STATUS_INVALID_ARGUMENT,
                                 "invalid core event unsubscription");
    cemu_event_unsubscribe(&soc->instrumentation, subscription);
    return cemu_status_ok();
}

void cemu_core_destroy(cemu_core_t *core) {
    if (!core) return;
    cemu_machine_destroy(core->machine);
    memset(core, 0, sizeof(*core));
    free(core);
}
