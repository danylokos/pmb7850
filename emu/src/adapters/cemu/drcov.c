#include "emu_cemu_drcov_adapter.h"

#include <stdlib.h>

#include "cemu_core.h"
#include "cemu_event.h"
#include "memory_regions.h"

struct emu_cemu_drcov_adapter {
    emu_drcov_t *collector;
    cemu_core_t *core;
    unsigned subscription;
    emu_drcov_result_t result;
};

static void instruction_event(
    void *opaque, const cemu_event_t *event);

emu_drcov_result_t emu_cemu_drcov_create(emu_drcov_t **collector) {
    if (!collector || *collector) return EMU_DRCOV_ERR_ARGUMENT;
    emu_drcov_module_t modules[CEMU_MEMORY_REGION_COUNT];
    for (int i = 0; i < CEMU_MEMORY_REGION_COUNT; i++) {
        modules[i].name = CEMU_MEMORY_REGIONS[i].name;
        modules[i].base = CEMU_MEMORY_REGIONS[i].start;
        modules[i].end = CEMU_MEMORY_REGIONS[i].end + 1u;
    }
    return emu_drcov_create(
        modules, (size_t)CEMU_MEMORY_REGION_COUNT, collector);
}

emu_drcov_result_t emu_cemu_drcov_attach_core(
        emu_drcov_t *collector, cemu_core_t *core,
        emu_cemu_drcov_adapter_t **out) {
    if (!collector || !core || !out || *out)
        return EMU_DRCOV_ERR_ARGUMENT;
    emu_cemu_drcov_adapter_t *adapter = calloc(1u, sizeof *adapter);
    if (!adapter) return EMU_DRCOV_ERR_NOMEM;
    adapter->collector = collector;
    adapter->core = core;
    adapter->result = EMU_DRCOV_OK;
    cemu_status_t status = cemu_core_subscribe_events(
        core, CEMU_EVENT_INSTRUCTION, instruction_event, NULL, adapter,
        &adapter->subscription);
    if (status.code != CEMU_STATUS_OK) {
        free(adapter);
        return status.code == CEMU_STATUS_ALLOCATION_FAILED
             ? EMU_DRCOV_ERR_NOMEM : EMU_DRCOV_ERR_VALIDATION;
    }
    *out = adapter;
    return EMU_DRCOV_OK;
}

static void instruction_event(
        void *opaque, const cemu_event_t *event) {
    emu_cemu_drcov_adapter_t *adapter = opaque;
    if (adapter->result != EMU_DRCOV_OK) return;
    const cemu_instruction_event_t *instruction = &event->as.instruction;
    if (instruction->size == 0) return;
    if (instruction->size < 0) {
        adapter->result = EMU_DRCOV_ERR_VALIDATION;
        return;
    }
    adapter->result = emu_drcov_add_instruction(
        adapter->collector, instruction->pc_before & 0x00ffffffu,
        (uint32_t)instruction->size);
}

emu_drcov_result_t emu_cemu_drcov_result(
        const emu_cemu_drcov_adapter_t *adapter) {
    return adapter ? adapter->result : EMU_DRCOV_ERR_ARGUMENT;
}

void emu_cemu_drcov_detach(emu_cemu_drcov_adapter_t **pointer) {
    if (!pointer || !*pointer) return;
    emu_cemu_drcov_adapter_t *adapter = *pointer;
    if (adapter->subscription && adapter->core)
        (void)cemu_core_unsubscribe_events(
            adapter->core, adapter->subscription);
    free(adapter);
    *pointer = NULL;
}
