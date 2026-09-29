#include "cemu_core_diagnostics.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "cemu_core_private.h"
#include "flash.h"
#include "keypad.h"
#include "memory_controller.h"
#include "state_digest.h"
#if CEMU_INSTRUMENTED
#include "disasm.h"
#endif

cemu_status_t cemu_core_diagnostic_wrap_legacy(cemu_core_t **out,
                                               cpu_t *cpu, soc_t *soc) {
    if (out) *out = NULL;
    if (!out || !soc)
        return cemu_status_error(CEMU_STATUS_INVALID_ARGUMENT,
                                 "invalid legacy CEMU core bridge");
    cemu_core_t *core = calloc(1, sizeof(*core));
    if (!core)
        return cemu_status_error(CEMU_STATUS_ALLOCATION_FAILED,
                                 "cannot allocate legacy CEMU core bridge");
    core->borrowed_cpu = cpu;
    core->borrowed_soc = soc;
    *out = core;
    return cemu_status_ok();
}

uint64_t cemu_core_diagnostic_state_digest(cemu_core_t *core) {
    cpu_t *cpu = cemu_core_private_cpu(core);
    soc_t *soc = cemu_core_private_soc(core);
    return cpu && soc ? state_digest(cpu, soc) : 0;
}

void cemu_core_diagnostic_instruction_detail(
        void *opaque, char *buffer, size_t capacity) {
    cemu_core_t *core = opaque;
    if (!buffer || !capacity) return;
    buffer[0] = 0;
#if CEMU_INSTRUMENTED
    cpu_t *cpu = cemu_core_private_cpu(core);
    soc_t *soc = cemu_core_private_soc(core);
    if (cpu && soc)
        (void)cpu_disasm(cpu, soc, buffer, (int)capacity);
#else
    (void)core;
#endif
}

void cemu_core_diagnostic_deferred_key_release(
        cemu_core_t *core, const char *name, size_t index,
        const char *phase) {
    cpu_t *cpu = cemu_core_private_cpu(core);
    soc_t *soc = cemu_core_private_soc(core);
    if (!soc || !name || !phase) return;
    keypad_state_t *keypad = NULL;
    for (int i = 0; i < soc->n_peripherals; i++) {
        peripheral_t *peripheral = soc->peripherals[i];
        if (peripheral && peripheral->id &&
            !strcmp(peripheral->id, "keypad")) {
            keypad = peripheral->state;
            break;
        }
    }
    const keypad_button_t *button = cemu_keypad_button_by_name(keypad, name);
    if (!button ||
        !cemu_event_native_trace_active(&soc->instrumentation,
                                        "keypad_deferred_release"))
        return;
    cemu_native_trace_event_t event = {0};
    event.kind = "keypad_deferred_release";
    event.icount = cpu ? cpu->icount : 0;
    event.pc = cpu ? cpu_pc(cpu) : 0;
    event.has_value = 1;
    event.value = button->raw_code;
    event.detail = "browser keypad release lifecycle";
    cemu_event_field_string(&event.info, "button", name);
    cemu_event_field_i64(&event.info, "key_index", (long)index);
    cemu_event_field_string(&event.info, "phase", phase);
    cemu_event_emit_native_trace(&soc->instrumentation, &event,
                                 CEMU_EVENT_PERIPHERAL);
}

cemu_status_t cemu_core_diagnostic_flash_chip_count(
        cemu_core_t *core, size_t *count) {
    soc_t *soc = cemu_core_private_soc(core);
    if (!soc || !count)
        return cemu_status_error(CEMU_STATUS_INVALID_ARGUMENT,
                                 "invalid diagnostic flash query");
    *count = (size_t)soc->memory.n_flash_chips;
    return cemu_status_ok();
}

cemu_status_t cemu_core_diagnostic_flash_chip_info(
        cemu_core_t *core, size_t chip_index,
        cemu_diagnostic_flash_chip_t *info) {
    soc_t *soc = cemu_core_private_soc(core);
    if (!soc || !info || chip_index >= (size_t)soc->memory.n_flash_chips ||
        !soc->memory.flash_endpoints[chip_index])
        return cemu_status_error(CEMU_STATUS_INVALID_ARGUMENT,
                                 "invalid diagnostic flash-chip query");
    peripheral_t *periph = soc->memory.flash_endpoints[chip_index];
    flash_state_t *state = periph->state;
    *info = (cemu_diagnostic_flash_chip_t){
        .chip_index = (int)chip_index,
        .chip_name = periph->id,
        .model = cemu_flash_model_str(state),
        .size = soc->memory.flash_chips[chip_index].chip_size,
    };
    return cemu_status_ok();
}

cemu_status_t cemu_core_diagnostic_flash_array_copy(
        cemu_core_t *core, size_t chip_index, size_t offset,
        uint8_t *bytes, size_t size) {
    soc_t *soc = cemu_core_private_soc(core);
    if (!soc || chip_index > INT_MAX)
        return cemu_status_error(CEMU_STATUS_INVALID_ARGUMENT,
                                 "invalid diagnostic flash-array copy");
    return cemu_memory_controller_flash_array_copy(
        &soc->memory, (int)chip_index, offset, bytes, size);
}
