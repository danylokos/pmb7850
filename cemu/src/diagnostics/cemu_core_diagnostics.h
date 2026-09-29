/* CEMU-only diagnostics over the opaque core; excluded from core archives. */
#ifndef CEMU_CORE_DIAGNOSTICS_H
#define CEMU_CORE_DIAGNOSTICS_H

#include <stddef.h>
#include <stdint.h>

#include "cemu_status.h"

typedef struct cemu_core cemu_core_t;
typedef struct cpu cpu_t;
typedef struct soc soc_t;

typedef struct {
    int chip_index;
    const char *chip_name;
    const char *model;
    size_t size;
} cemu_diagnostic_flash_chip_t;

cemu_status_t cemu_core_diagnostic_wrap_legacy(cemu_core_t **out,
                                               cpu_t *cpu, soc_t *soc);
uint64_t cemu_core_diagnostic_state_digest(cemu_core_t *core);
void cemu_core_diagnostic_deferred_key_release(
    cemu_core_t *core, const char *name, size_t index, const char *phase);
void cemu_core_diagnostic_instruction_detail(
    void *core, char *buffer, size_t capacity);
cemu_status_t cemu_core_diagnostic_flash_chip_count(
    cemu_core_t *core, size_t *count);
cemu_status_t cemu_core_diagnostic_flash_chip_info(
    cemu_core_t *core, size_t chip_index,
    cemu_diagnostic_flash_chip_t *info);
cemu_status_t cemu_core_diagnostic_flash_array_copy(
    cemu_core_t *core, size_t chip_index, size_t offset,
    uint8_t *bytes, size_t size);

#endif /* CEMU_CORE_DIAGNOSTICS_H */
