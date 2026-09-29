#include "cemu_core_storage_private.h"

#include <stdlib.h>
#include <string.h>

#include "flash.h"

typedef struct {
    uint64_t group;
    size_t chip_index;
    cemu_storage_space_t space;
    size_t offset;
    uint8_t *expected;
    size_t expected_size;
    uint8_t *replacement;
    size_t replacement_size;
} cemu_storage_operation_t;

struct cemu_storage_transaction {
    cemu_machine_t *machine;
    cemu_storage_stage_t stage;
    cemu_storage_operation_t *operations;
    size_t count;
    size_t capacity;
};

static long storage_clone_fail_after = -1;

void cemu_test_fail_storage_clone_after(long successful_clones) {
    storage_clone_fail_after = successful_clones;
}

static void cemu_storage_operation_free(cemu_storage_operation_t *operation) {
    if (!operation) return;
    free(operation->expected);
    free(operation->replacement);
    memset(operation, 0, sizeof(*operation));
}

static void cemu_storage_transaction_free(
        cemu_storage_transaction_t *transaction) {
    if (!transaction) return;
    for (size_t i = 0; i < transaction->count; i++)
        cemu_storage_operation_free(&transaction->operations[i]);
    free(transaction->operations);
    memset(transaction, 0, sizeof(*transaction));
}

cemu_status_t cemu_storage_transaction_add(
        cemu_storage_transaction_t *transaction, uint64_t group,
        size_t chip_index, cemu_storage_space_t space, size_t offset,
        const uint8_t *expected, size_t expected_size,
        const uint8_t *replacement, size_t replacement_size) {
    if (!transaction || !transaction->machine || !group ||
        (expected_size && !expected) || !replacement || !replacement_size ||
        (unsigned)space > CEMU_STORAGE_AM29_CUSTOMER_SECSI)
        return cemu_status_error(CEMU_STATUS_INVALID_ARGUMENT,
                                 "invalid CEMU storage operation");

    uint8_t *expected_copy = NULL;
    if (expected_size) {
        expected_copy = cemu_calloc(expected_size, 1);
        if (!expected_copy)
            return cemu_status_error(CEMU_STATUS_ALLOCATION_FAILED,
                                     "cannot copy expected storage bytes");
        memcpy(expected_copy, expected, expected_size);
    }
    uint8_t *replacement_copy = cemu_calloc(replacement_size, 1);
    if (!replacement_copy) {
        free(expected_copy);
        return cemu_status_error(CEMU_STATUS_ALLOCATION_FAILED,
                                 "cannot copy replacement storage bytes");
    }
    memcpy(replacement_copy, replacement, replacement_size);

    if (transaction->count == transaction->capacity) {
        size_t capacity = transaction->capacity
                        ? transaction->capacity * 2u : 8u;
        if (capacity < transaction->capacity ||
            capacity > SIZE_MAX / sizeof(*transaction->operations)) {
            free(expected_copy);
            free(replacement_copy);
            return cemu_status_error(CEMU_STATUS_ALLOCATION_FAILED,
                                     "too many CEMU storage operations");
        }
        void *replacement_operations = cemu_realloc(
            transaction->operations,
            capacity * sizeof(*transaction->operations));
        if (!replacement_operations) {
            free(expected_copy);
            free(replacement_copy);
            return cemu_status_error(CEMU_STATUS_ALLOCATION_FAILED,
                                     "cannot grow CEMU storage transaction");
        }
        transaction->operations = replacement_operations;
        transaction->capacity = capacity;
    }
    transaction->operations[transaction->count++] =
        (cemu_storage_operation_t){
            .group = group,
            .chip_index = chip_index,
            .space = space,
            .offset = offset,
            .expected = expected_copy,
            .expected_size = expected_size,
            .replacement = replacement_copy,
            .replacement_size = replacement_size,
        };
    return cemu_status_ok();
}

static cemu_status_t cemu_storage_operation_validate(
        const cemu_storage_transaction_t *transaction,
        const cemu_storage_operation_t *operation) {
    memory_controller_t *memory = &transaction->machine->soc.memory;
    if (operation->chip_index >= (size_t)memory->n_flash_chips)
        return cemu_status_error(CEMU_STATUS_INVALID_CONFIGURATION,
                                 "storage operation has invalid flash chip");
    const flash_chip_config_t *chip =
        &memory->flash_chips[operation->chip_index];
    const flash_state_t *state = cemu_memory_controller_flash_state(
        memory, (int)operation->chip_index);
    if (!state)
        return cemu_status_error(CEMU_STATUS_INVALID_CONFIGURATION,
                                 "storage operation has no flash state");

    if (operation->space == CEMU_STORAGE_MAIN_ARRAY) {
        size_t source_offset =
            memory->flash_file_offsets[operation->chip_index];
        if (source_offset > memory->flash_len ||
            chip->chip_size > memory->flash_len - source_offset ||
            operation->offset > chip->chip_size ||
            operation->replacement_size > chip->chip_size - operation->offset ||
            operation->expected_size > chip->chip_size - operation->offset ||
            operation->offset > UINT32_MAX)
            return cemu_status_error(CEMU_STATUS_INVALID_CONFIGURATION,
                                     "main-array storage range is invalid");
        return cemu_status_ok();
    }
    if (operation->space == CEMU_STORAGE_FACTORY_UID) {
        if (operation->offset || operation->replacement_size != 8u ||
            (operation->expected_size && operation->expected_size != 8u) ||
            (state->kind != FLASH_MODEL_M58LW064D &&
             state->kind != FLASH_MODEL_W30))
            return cemu_status_error(CEMU_STATUS_INVALID_CONFIGURATION,
                                     "factory-UID storage operation is invalid");
        return cemu_status_ok();
    }

    const am29lv_state_t *am29 = cemu_flash_am29_state_const(state);
    if (!am29 || operation->offset > sizeof(am29->secsi) ||
        operation->replacement_size > sizeof(am29->secsi) - operation->offset ||
        operation->expected_size > sizeof(am29->secsi) - operation->offset ||
        operation->offset > UINT16_MAX)
        return cemu_status_error(CEMU_STATUS_INVALID_CONFIGURATION,
                                 "SecSi storage operation is invalid");
    return cemu_status_ok();
}

static const uint8_t *cemu_storage_chip_source(
        const cemu_storage_transaction_t *transaction, size_t chip_index) {
    const memory_controller_t *memory = &transaction->machine->soc.memory;
    return memory->flash_data + memory->flash_file_offsets[chip_index];
}

static int cemu_storage_operation_matches(
        const cemu_storage_transaction_t *transaction,
        const flash_state_t *state,
        const cemu_storage_operation_t *operation,
        const uint8_t *bytes, size_t size) {
    if (!bytes || !size) return 0;
    if (operation->space == CEMU_STORAGE_MAIN_ARRAY) {
        const memory_controller_t *memory = &transaction->machine->soc.memory;
        size_t chip_size = memory->flash_chips[operation->chip_index].chip_size;
        const uint8_t *source = cemu_storage_chip_source(
            transaction, operation->chip_index);
        for (size_t i = 0; i < size; i++)
            if (cemu_flash_array_read8(source, chip_size, state,
                                  (uint32_t)(operation->offset + i)) !=
                bytes[i])
                return 0;
        return 1;
    }
    if (operation->space == CEMU_STORAGE_FACTORY_UID) {
        if (state->kind == FLASH_MODEL_M58LW064D)
            return state->u.m58.factory_uid_set &&
                   !memcmp(state->u.m58.factory_uid, bytes, size);
        return state->u.w30.factory_uid_set &&
               !memcmp(state->u.w30.factory_uid, bytes, size);
    }
    const am29lv_state_t *am29 = cemu_flash_am29_state_const(state);
    return am29 && !memcmp(am29->secsi + operation->offset, bytes, size);
}

static int cemu_storage_operation_apply(
        const cemu_storage_transaction_t *transaction, flash_state_t *state,
        const cemu_storage_operation_t *operation) {
    if (operation->space == CEMU_STORAGE_MAIN_ARRAY) {
        const memory_controller_t *memory = &transaction->machine->soc.memory;
        return cemu_flash_seed_bytes(
            cemu_storage_chip_source(transaction, operation->chip_index),
            memory->flash_chips[operation->chip_index].chip_size, state,
            (uint32_t)operation->offset, operation->replacement,
            operation->replacement_size);
    }
    if (operation->space == CEMU_STORAGE_FACTORY_UID)
        return cemu_flash_factory_uid_set(state, operation->replacement);
    am29lv_state_t *am29 = cemu_flash_am29_state(state);
    return cemu_am29lv_secsi_factory_set(
        am29, (uint16_t)operation->offset, operation->replacement,
        operation->replacement_size);
}

static void cemu_storage_staged_free(
        flash_state_t staged[MAX_FLASH_CHIPS],
        const int affected[MAX_FLASH_CHIPS]) {
    for (size_t i = 0; i < MAX_FLASH_CHIPS; i++)
        if (affected[i]) cemu_flash_state_free(&staged[i]);
}

static cemu_status_t cemu_storage_report_add(
        cemu_storage_report_t *report, cemu_storage_stage_t stage,
        uint64_t group, cemu_storage_disposition_t disposition) {
    if (!report) return cemu_status_ok();
    if (report->count == report->capacity) {
        size_t capacity = report->capacity ? report->capacity * 2u : 8u;
        if (capacity < report->capacity ||
            capacity > SIZE_MAX / sizeof(*report->entries))
            return cemu_status_error(CEMU_STATUS_ALLOCATION_FAILED,
                                     "too many storage results");
        void *entries = cemu_realloc(
            report->entries, capacity * sizeof(*report->entries));
        if (!entries)
            return cemu_status_error(CEMU_STATUS_ALLOCATION_FAILED,
                                     "cannot grow storage results");
        report->entries = entries;
        report->capacity = capacity;
    }
    report->entries[report->count++] = (cemu_storage_report_entry_t){
        .stage = stage,
        .group = group,
        .disposition = disposition,
    };
    return cemu_status_ok();
}

static cemu_status_t cemu_storage_transaction_commit(
        cemu_storage_transaction_t *transaction,
        cemu_storage_report_t *report) {
    flash_state_t staged[MAX_FLASH_CHIPS] = {0};
    int affected[MAX_FLASH_CHIPS] = {0};
    for (size_t i = 0; i < transaction->count; i++) {
        cemu_status_t status = cemu_storage_operation_validate(
            transaction, &transaction->operations[i]);
        if (status.code != CEMU_STATUS_OK) return status;
        affected[transaction->operations[i].chip_index] = 1;
    }

    long clone_count = 0;
    for (size_t i = 0; i < MAX_FLASH_CHIPS; i++) {
        if (!affected[i]) continue;
        flash_state_t *source = cemu_memory_controller_flash_state(
            &transaction->machine->soc.memory, (int)i);
        if ((storage_clone_fail_after >= 0 &&
             clone_count >= storage_clone_fail_after) ||
            !source || !cemu_flash_state_copy(&staged[i], source)) {
            storage_clone_fail_after = -1;
            cemu_storage_staged_free(staged, affected);
            return cemu_status_error(CEMU_STATUS_ALLOCATION_FAILED,
                                     "cannot clone CEMU storage transaction");
        }
        clone_count++;
    }
    storage_clone_fail_after = -1;

    for (size_t first = 0; first < transaction->count; first++) {
        uint64_t group = transaction->operations[first].group;
        int seen = 0;
        for (size_t i = 0; i < first; i++)
            seen |= transaction->operations[i].group == group;
        if (seen) continue;

        int any_expected = 0;
        int any_replaced = 0;
        for (size_t i = first; i < transaction->count; i++) {
            const cemu_storage_operation_t *operation =
                &transaction->operations[i];
            if (operation->group != group) continue;
            flash_state_t *state = &staged[operation->chip_index];
            int replaced = cemu_storage_operation_matches(
                transaction, state, operation, operation->replacement,
                operation->replacement_size);
            int expected = operation->expected_size
                         ? cemu_storage_operation_matches(
                               transaction, state, operation,
                               operation->expected,
                               operation->expected_size)
                         : !replaced;
            if (!expected && !replaced) {
                cemu_storage_staged_free(staged, affected);
                return cemu_status_error(
                    CEMU_STATUS_INVALID_CONFIGURATION,
                    "storage operation found unexpected current bytes");
            }
            if (expected && replaced) continue;
            any_expected |= expected;
            any_replaced |= replaced;
        }
        if (any_expected && any_replaced) {
            cemu_storage_staged_free(staged, affected);
            return cemu_status_error(
                CEMU_STATUS_INVALID_CONFIGURATION,
                "storage operation group is partially applied");
        }
        if (any_expected) {
            for (size_t i = first; i < transaction->count; i++) {
                const cemu_storage_operation_t *operation =
                    &transaction->operations[i];
                if (operation->group != group) continue;
                if (!cemu_storage_operation_apply(
                        transaction, &staged[operation->chip_index],
                        operation)) {
                    cemu_storage_staged_free(staged, affected);
                    return cemu_status_error(
                        CEMU_STATUS_ALLOCATION_FAILED,
                        "cannot apply CEMU storage operation");
                }
            }
        }
        cemu_status_t status = cemu_storage_report_add(
            report, transaction->stage, group,
            any_replaced ? CEMU_STORAGE_ALREADY_APPLIED
                         : CEMU_STORAGE_APPLIED);
        if (status.code != CEMU_STATUS_OK) {
            cemu_storage_staged_free(staged, affected);
            return status;
        }
    }

    for (size_t i = 0; i < MAX_FLASH_CHIPS; i++) {
        if (!affected[i]) continue;
        flash_state_t *destination = cemu_memory_controller_flash_state(
            &transaction->machine->soc.memory, (int)i);
        cemu_flash_state_free(destination);
        *destination = staged[i];
        memset(&staged[i], 0, sizeof(staged[i]));
    }
    return cemu_status_ok();
}

cemu_status_t cemu_core_storage_apply(
        cemu_machine_t *machine, cemu_storage_initializer_t initializer,
        void *opaque, cemu_storage_stage_t stage,
        cemu_storage_report_t *report) {
    if (!machine || (unsigned)stage > CEMU_STORAGE_POST_RESET)
        return cemu_status_error(CEMU_STATUS_INVALID_ARGUMENT,
                                 "invalid storage initialization stage");
    if (!initializer) return cemu_status_ok();
    cemu_storage_transaction_t transaction = {
        .machine = machine,
        .stage = stage,
    };
    cemu_status_t status = initializer(opaque, stage, &transaction);
    if (status.code == CEMU_STATUS_OK)
        status = cemu_storage_transaction_commit(&transaction, report);
    cemu_storage_transaction_free(&transaction);
    return status;
}

void cemu_core_storage_report_free(cemu_storage_report_t *report) {
    if (!report) return;
    free(report->entries);
    memset(report, 0, sizeof(*report));
}
