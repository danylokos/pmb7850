/* Atomic, engine-native flash initialization for the embeddable CEMU core. */
#ifndef CEMU_CORE_STORAGE_H
#define CEMU_CORE_STORAGE_H

#include <stddef.h>
#include <stdint.h>

#include "cemu_status.h"

typedef struct cemu_storage_transaction cemu_storage_transaction_t;

typedef enum {
    CEMU_STORAGE_PRE_RESET = 0,
    CEMU_STORAGE_POST_RESET,
} cemu_storage_stage_t;

typedef enum {
    CEMU_STORAGE_MAIN_ARRAY = 0,
    CEMU_STORAGE_FACTORY_UID,
    CEMU_STORAGE_AM29_FACTORY_SECSI,
    CEMU_STORAGE_AM29_CUSTOMER_SECSI,
} cemu_storage_space_t;

typedef enum {
    CEMU_STORAGE_APPLIED = 0,
    CEMU_STORAGE_ALREADY_APPLIED,
} cemu_storage_disposition_t;

typedef cemu_status_t (*cemu_storage_initializer_t)(
    void *opaque, cemu_storage_stage_t stage,
    cemu_storage_transaction_t *transaction);

typedef void (*cemu_storage_result_callback_t)(
    void *opaque, cemu_storage_stage_t stage, uint64_t group,
    cemu_storage_disposition_t disposition);

cemu_status_t cemu_storage_transaction_add(
    cemu_storage_transaction_t *transaction, uint64_t group,
    size_t chip_index, cemu_storage_space_t space, size_t offset,
    const uint8_t *expected, size_t expected_size,
    const uint8_t *replacement, size_t replacement_size);

/* Deterministic clone-failure injection for atomicity tests. */
void cemu_test_fail_storage_clone_after(long successful_clones);

#endif /* CEMU_CORE_STORAGE_H */
