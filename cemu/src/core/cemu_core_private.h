/* CEMU-only raw access for native diagnostics; never include from EMU. */
#ifndef CEMU_CORE_PRIVATE_H
#define CEMU_CORE_PRIVATE_H

#include "cemu_core.h"
#include "cpu.h"
#include "soc.h"

typedef struct {
    soc_t soc;
    cpu_t cpu;
} cemu_machine_t;

struct cemu_core {
    cemu_core_options_t options;
    cemu_machine_t *machine;
    /* CEMU's legacy driver is intentionally not part of the core lifecycle.
     * Its diagnostics bridge may borrow the existing stack-owned machine so
     * transport compatibility code can use the same opaque controls. */
    cpu_t *borrowed_cpu;
    soc_t *borrowed_soc;
    uint64_t total_guest_instructions;
    uint64_t serial_received_bytes;
    int stop_requested;
    int serial_link_attached;
    int statistics_enabled;
    int post_reset_storage_applied;
    cemu_core_after_step_fn after_step;
    void *after_step_opaque;
    cemu_core_instruction_detail_fn instruction_detail;
    void *instruction_detail_opaque;
};

static inline cpu_t *cemu_core_private_cpu(cemu_core_t *core) {
    if (!core) return NULL;
    return core->machine ? &core->machine->cpu : core->borrowed_cpu;
}

static inline soc_t *cemu_core_private_soc(cemu_core_t *core) {
    if (!core) return NULL;
    return core->machine ? &core->machine->soc : core->borrowed_soc;
}

static inline const cpu_t *cemu_core_private_cpu_const(
        const cemu_core_t *core) {
    if (!core) return NULL;
    return core->machine ? &core->machine->cpu : core->borrowed_cpu;
}

static inline const soc_t *cemu_core_private_soc_const(
        const cemu_core_t *core) {
    if (!core) return NULL;
    return core->machine ? &core->machine->soc : core->borrowed_soc;
}

#endif /* CEMU_CORE_PRIVATE_H */
