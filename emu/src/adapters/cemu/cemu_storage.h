#ifndef EMU_CEMU_STORAGE_ADAPTER_PRIVATE_H
#define EMU_CEMU_STORAGE_ADAPTER_PRIVATE_H

#include "cemu_core_storage.h"
#include "emu_cemu_storage.h"

typedef struct {
    const emu_prepared_session_t *prepared;
    emu_cemu_storage_result_t result;
} emu_cemu_storage_context_t;

void emu_cemu_storage_context_init(
    emu_cemu_storage_context_t *context,
    const emu_prepared_session_t *prepared);
cemu_status_t emu_cemu_storage_initialize(
    void *opaque, cemu_storage_stage_t stage,
    cemu_storage_transaction_t *transaction);
void emu_cemu_storage_record_result(
    void *opaque, cemu_storage_stage_t stage, uint64_t group,
    cemu_storage_disposition_t disposition);

#endif /* EMU_CEMU_STORAGE_ADAPTER_PRIVATE_H */
