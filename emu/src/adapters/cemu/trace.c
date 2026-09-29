#include "emu_cemu_trace_adapter.h"

#include <string.h>
#include <stdlib.h>

#include "cemu_core.h"
#include "cemu_core_diagnostics.h"
#include "cemu_event.h"
#include "emu_eeprom_trace.h"
#include "emu_trace_schema.h"

struct emu_cemu_trace_adapter {
    cemu_core_t *core;
    emu_trace_sink_t *sink;
    emu_eeprom_trace_t *eeprom;
    unsigned subscription;
    int enabled;
};

static int emu_cemu_trace_translate(
        const cemu_native_trace_event_t *native, emu_trace_event_t *normalized,
        char *error, size_t capacity) {
    if (!native || !normalized || native->info.n < 0 ||
        native->info.n > (int)(sizeof normalized->info.kv /
                               sizeof normalized->info.kv[0])) {
        if (error && capacity)
            snprintf(error, capacity, "invalid CEMU trace event");
        return -1;
    }
    memset(normalized, 0, sizeof *normalized);
    normalized->kind = native->kind;
    normalized->icount = native->icount;
    normalized->pc = native->pc;
    normalized->has_addr = native->has_addr;
    normalized->addr = native->addr;
    normalized->has_size = native->has_size;
    normalized->size = native->size;
    normalized->has_value = native->has_value;
    normalized->value = native->value;
    normalized->detail = native->detail;
    normalized->info.n = native->info.n;
    for (int i = 0; i < native->info.n; i++) {
        normalized->info.kv[i].key = native->info.kv[i].key;
        normalized->info.kv[i].kind =
            (emu_trace_value_kind_t)native->info.kv[i].kind;
        normalized->info.kv[i].ival = native->info.kv[i].ival;
        normalized->info.kv[i].sval = native->info.kv[i].sval;
    }
    if (error && capacity) error[0] = 0;
    return 0;
}

void emu_cemu_trace_emit(
        emu_trace_sink_t *sink, const cemu_native_trace_event_t *event) {
    emu_trace_event_t normalized;
    if (!emu_cemu_trace_translate(event, &normalized, NULL, 0))
        emu_trace_emit(sink, &normalized);
}

int emu_cemu_trace_validate_event(
        const cemu_native_trace_event_t *event, char *error, size_t capacity) {
    emu_trace_event_t normalized;
    if (emu_cemu_trace_translate(
            event, &normalized, error, capacity))
        return -1;
    return emu_trace_schema_validate_event(
        &normalized, error, capacity);
}

static int emu_cemu_trace_filter(void *opaque, const char *kind) {
    emu_cemu_trace_adapter_t *adapter = opaque;
    return adapter->enabled &&
           emu_trace_sink_accepts(adapter->sink, kind);
}

static void emu_cemu_trace_consumer(
        void *opaque, const cemu_event_t *event) {
    emu_cemu_trace_adapter_t *adapter = opaque;
    if (!adapter->enabled) return;
    const cemu_native_trace_event_t *trace = NULL;
    if (event->type == CEMU_EVENT_BUS)
        trace = event->as.bus.trace;
    else if (event->type == CEMU_EVENT_PERIPHERAL)
        trace = event->as.peripheral.trace;
    else if (event->type == CEMU_EVENT_INSTRUCTION) {
        const cemu_instruction_event_t *instruction = &event->as.instruction;
        cemu_native_trace_event_t generated = {
            .kind = "exec",
            .icount = instruction->icount,
            .pc = instruction->pc_after,
            .has_addr = 1,
            .addr = instruction->pc_before,
            .has_size = 1,
            .size = instruction->size,
            .detail = instruction->detail ? instruction->detail : "",
        };
        if (emu_trace_sink_accepts(adapter->sink, generated.kind))
            emu_cemu_trace_emit(adapter->sink, &generated);
        return;
    } else if (event->type == CEMU_EVENT_FLASH_MUTATION) {
        const cemu_flash_mutation_event_t *mutation =
            &event->as.flash_mutation;
        emu_eeprom_trace_mutation(
            adapter->eeprom, mutation->chip_index,
            mutation->kind == CEMU_FLASH_MUTATION_ERASE
                ? EMU_EEPROM_MUTATION_ERASE
                : EMU_EEPROM_MUTATION_PROGRAM,
            mutation->offset, mutation->size, mutation->tick,
            mutation->icount, mutation->pc);
        return;
    }
    if (trace && emu_trace_sink_accepts(adapter->sink, trace->kind))
        emu_cemu_trace_emit(adapter->sink, trace);
    if (event->type == CEMU_EVENT_BUS) {
        const cemu_bus_event_t *bus = &event->as.bus;
        const bus_transaction_t *transaction = bus->transaction;
        if (transaction && transaction->kind == BUS_ACCESS_READ &&
            bus->has_flash_target && bus->flash_array_data)
            emu_eeprom_trace_read(
                adapter->eeprom, bus->chip_index, bus->chip_offset,
                transaction->addr, transaction->value, transaction->size,
                bus->tick, bus->icount, bus->pc);
    }
}

static int emu_cemu_eeprom_copy(
        void *opaque, int chip_index, size_t offset,
        uint8_t *bytes, size_t size) {
    emu_cemu_trace_adapter_t *adapter = opaque;
    return cemu_core_diagnostic_flash_array_copy(
               adapter->core, (size_t)chip_index, offset, bytes, size).code ==
           CEMU_STATUS_OK ? 0 : -1;
}

static int emu_cemu_eeprom_create(emu_cemu_trace_adapter_t *adapter) {
    if (!(adapter->sink->mask & EMU_TRACE_MASK_EEPROM)) return 0;
    size_t count = 0;
    if (cemu_core_diagnostic_flash_chip_count(
            adapter->core, &count).code != CEMU_STATUS_OK)
        return -1;
    emu_eeprom_trace_chip_t *chips =
        calloc(count ? count : 1u, sizeof *chips);
    if (!chips) return -1;
    int failed = 0;
    for (size_t i = 0; i < count; i++) {
        cemu_diagnostic_flash_chip_t chip;
        if (cemu_core_diagnostic_flash_chip_info(
                adapter->core, i, &chip).code != CEMU_STATUS_OK) {
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
    if (!failed && emu_eeprom_trace_create(
            &adapter->eeprom, adapter->sink, chips, count,
            emu_cemu_eeprom_copy, adapter))
        failed = 1;
    free(chips);
    return failed ? -1 : 0;
}

int emu_cemu_trace_attach_deferred(
        cemu_core_t *core, emu_trace_sink_t *sink, int deferred,
        emu_cemu_trace_adapter_t **out) {
    if (!core || !sink || !out || *out) return -1;
    emu_cemu_trace_adapter_t *adapter = calloc(1, sizeof *adapter);
    if (!adapter) return -1;
    adapter->core = core;
    adapter->sink = sink;
    if (emu_cemu_eeprom_create(adapter)) {
        free(adapter);
        return -1;
    }
    cemu_event_mask_t mask = (cemu_event_mask_t)(
        CEMU_EVENT_INSTRUCTION | CEMU_EVENT_BUS | CEMU_EVENT_PERIPHERAL);
    if (adapter->eeprom)
        mask = (cemu_event_mask_t)(mask | CEMU_EVENT_FLASH_MUTATION);
    cemu_status_t status = cemu_core_subscribe_events(
        core, mask,
        emu_cemu_trace_consumer, emu_cemu_trace_filter, adapter,
        &adapter->subscription);
    if (status.code != CEMU_STATUS_OK) {
        emu_eeprom_trace_destroy(&adapter->eeprom);
        free(adapter);
        return -1;
    }
    *out = adapter;
    if (!deferred && emu_cemu_trace_set_enabled(adapter, 1)) {
        emu_cemu_trace_detach(out);
        return -1;
    }
    return 0;
}

int emu_cemu_trace_attach(
        cemu_core_t *core, emu_trace_sink_t *sink,
        emu_cemu_trace_adapter_t **out) {
    return emu_cemu_trace_attach_deferred(core, sink, 0, out);
}

int emu_cemu_trace_set_enabled(
        emu_cemu_trace_adapter_t *adapter, int enabled) {
    if (!adapter) return -1;
    if (enabled && !adapter->enabled && adapter->eeprom) {
        cemu_core_state_t state;
        if (cemu_core_query(adapter->core, &state).code != CEMU_STATUS_OK ||
            emu_eeprom_trace_arm(adapter->eeprom, state.ticks,
                                 state.instruction_count, state.pc))
            return -1;
    }
    adapter->enabled = enabled != 0;
    if (emu_trace_sink_accepts(adapter->sink, "exec")) {
        cemu_status_t status = cemu_core_set_instruction_detail(
            adapter->core,
            adapter->enabled ? cemu_core_diagnostic_instruction_detail
                             : NULL,
            adapter->enabled ? adapter->core : NULL);
        if (status.code != CEMU_STATUS_OK) return -1;
    }
    return 0;
}

void emu_cemu_trace_detach(emu_cemu_trace_adapter_t **pointer) {
    if (!pointer || !*pointer) return;
    emu_cemu_trace_adapter_t *adapter = *pointer;
    if (emu_trace_sink_accepts(adapter->sink, "exec"))
        (void)cemu_core_set_instruction_detail(adapter->core, NULL, NULL);
    if (adapter->subscription)
        (void)cemu_core_unsubscribe_events(
            adapter->core, adapter->subscription);
    emu_eeprom_trace_destroy(&adapter->eeprom);
    free(adapter);
    *pointer = NULL;
}
