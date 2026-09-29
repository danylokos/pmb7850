#ifndef CEMU_CORE_STORAGE_PRIVATE_H
#define CEMU_CORE_STORAGE_PRIVATE_H

#include "cemu_core_private.h"

typedef struct {
    cemu_storage_stage_t stage;
    uint64_t group;
    cemu_storage_disposition_t disposition;
} cemu_storage_report_entry_t;

typedef struct {
    cemu_storage_report_entry_t *entries;
    size_t count;
    size_t capacity;
} cemu_storage_report_t;

cemu_status_t cemu_core_storage_apply(
    cemu_machine_t *machine, cemu_storage_initializer_t initializer,
    void *opaque, cemu_storage_stage_t stage, cemu_storage_report_t *report);
void cemu_core_storage_report_free(cemu_storage_report_t *report);

#endif /* CEMU_CORE_STORAGE_PRIVATE_H */
