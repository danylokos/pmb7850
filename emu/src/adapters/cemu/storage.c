#include <stdio.h>
#include <string.h>

#include "cemu_storage.h"
#include "emu_cemu_storage.h"

/* This adapter is intentionally the only storage-plan implementation that
 * translates host operations into CEMU flash mutations. */
#include "flash.h"
#include "soc.h"

static int fail_copy_after = -1;

void emu_cemu_test_fail_storage_copy_after(int successful_copies) {
    fail_copy_after = successful_copies;
    cemu_test_fail_storage_clone_after(successful_copies);
}

static emu_error_code_t fail(emu_error_t *error, emu_error_code_t code,
                             const char *message) {
    if (error) {
        error->code = code;
        snprintf(error->message, sizeof error->message, "%s", message);
    }
    return code;
}

static int operation_matches(
        const emu_prepared_session_t *prepared, flash_state_t *state,
        const emu_storage_operation_t *operation, const uint8_t *bytes,
        size_t size) {
    if (!bytes || !size) return 0;
    if (operation->space == EMU_STORAGE_MAIN_ARRAY) {
        const emu_chip_view_t *chip = &prepared->chips[operation->chip_index];
        if (operation->offset > chip->size ||
            size > chip->size - operation->offset)
            return 0;
        const uint8_t *source = prepared->source.bytes + chip->source_offset;
        for (size_t i = 0; i < size; i++)
            if (cemu_flash_array_read8(source, chip->size, state,
                                  (uint32_t)(operation->offset + i)) !=
                bytes[i])
                return 0;
        return 1;
    }
    if (operation->space == EMU_STORAGE_FACTORY_UID) {
        if (operation->offset || size != 8) return 0;
        if (state->kind == FLASH_MODEL_M58LW064D)
            return state->u.m58.factory_uid_set &&
                   !memcmp(state->u.m58.factory_uid, bytes, size);
        if (state->kind == FLASH_MODEL_W30)
            return state->u.w30.factory_uid_set &&
                   !memcmp(state->u.w30.factory_uid, bytes, size);
        return 0;
    }
    if (operation->space == EMU_STORAGE_AM29_FACTORY_SECSI ||
        operation->space == EMU_STORAGE_AM29_CUSTOMER_SECSI) {
        am29lv_state_t *am29 = cemu_flash_am29_state(state);
        return am29 && operation->offset <= sizeof am29->secsi &&
               size <= sizeof am29->secsi - operation->offset &&
               !memcmp(am29->secsi + operation->offset, bytes, size);
    }
    return 0;
}

static int apply_operation(
        const emu_prepared_session_t *prepared, flash_state_t *state,
        const emu_storage_operation_t *operation) {
    if (operation->space == EMU_STORAGE_MAIN_ARRAY) {
        const emu_chip_view_t *chip = &prepared->chips[operation->chip_index];
        return operation->offset <= UINT32_MAX &&
               cemu_flash_seed_bytes(
                   prepared->source.bytes + chip->source_offset, chip->size,
                   state, (uint32_t)operation->offset,
                   operation->replacement, operation->replacement_size);
    }
    if (operation->space == EMU_STORAGE_FACTORY_UID)
        return operation->offset == 0 && operation->replacement_size == 8 &&
               cemu_flash_factory_uid_set(state, operation->replacement);
    if (operation->space == EMU_STORAGE_AM29_FACTORY_SECSI ||
        operation->space == EMU_STORAGE_AM29_CUSTOMER_SECSI) {
        am29lv_state_t *am29 = cemu_flash_am29_state(state);
        return am29 && operation->offset <= UINT16_MAX &&
               cemu_am29lv_secsi_factory_set(
                   am29, (uint16_t)operation->offset,
                   operation->replacement, operation->replacement_size);
    }
    return 0;
}

static void free_staged(flash_state_t staged[EMU_MAX_CHIPS],
                        const int affected[EMU_MAX_CHIPS]) {
    for (size_t i = 0; i < EMU_MAX_CHIPS; i++)
        if (affected[i]) cemu_flash_state_free(&staged[i]);
}

static void record_patch_group(const emu_storage_operation_t *operation,
                               int already_applied,
                               emu_cemu_storage_result_t *result) {
    static const char prefix[] = "firmware-patch:";
    if (!result || strncmp(operation->provenance, prefix,
                           sizeof prefix - 1u))
        return;
    const char *name = operation->provenance + sizeof prefix - 1u;
    const char *end = strchr(name, ':');
    if (!end || end == name || (size_t)(end - name) >= 64u) return;
    char copy[64];
    memcpy(copy, name, (size_t)(end - name));
    copy[end - name] = 0;
    int id = emu_patch_id_by_name(copy);
    if (id < 0 || id >= 64) return;
    if (already_applied)
        result->already_applied |= UINT64_C(1) << id;
    else
        result->applied |= UINT64_C(1) << id;
}

void emu_cemu_storage_context_init(
        emu_cemu_storage_context_t *context,
        const emu_prepared_session_t *prepared) {
    if (!context) return;
    *context = (emu_cemu_storage_context_t){.prepared = prepared};
}

static int core_stage_from_x55(emu_storage_stage_t stage,
                               cemu_storage_stage_t *core_stage) {
    if (stage == EMU_STORAGE_STAGE_PRE_RESET)
        *core_stage = CEMU_STORAGE_PRE_RESET;
    else if (stage == EMU_STORAGE_STAGE_POST_RESTORE)
        *core_stage = CEMU_STORAGE_POST_RESET;
    else
        return 0;
    return 1;
}

static int core_space_from_x55(emu_storage_space_t space,
                               cemu_storage_space_t *core_space) {
    switch (space) {
        case EMU_STORAGE_MAIN_ARRAY:
            *core_space = CEMU_STORAGE_MAIN_ARRAY;
            return 1;
        case EMU_STORAGE_FACTORY_UID:
            *core_space = CEMU_STORAGE_FACTORY_UID;
            return 1;
        case EMU_STORAGE_AM29_FACTORY_SECSI:
            *core_space = CEMU_STORAGE_AM29_FACTORY_SECSI;
            return 1;
        case EMU_STORAGE_AM29_CUSTOMER_SECSI:
            *core_space = CEMU_STORAGE_AM29_CUSTOMER_SECSI;
            return 1;
    }
    return 0;
}

cemu_status_t emu_cemu_storage_initialize(
        void *opaque, cemu_storage_stage_t stage,
        cemu_storage_transaction_t *transaction) {
    emu_cemu_storage_context_t *context = opaque;
    if (!context || !context->prepared || !transaction)
        return cemu_status_error(CEMU_STATUS_INVALID_ARGUMENT,
                                 "invalid x55 CEMU storage initialization");
    /* The public test hook arms both the legacy and core transaction paths.
     * Consuming the core path must not leave a later legacy transaction
     * unexpectedly armed. */
    fail_copy_after = -1;
    const emu_prepared_session_t *prepared = context->prepared;
    for (size_t i = 0; i < prepared->operation_count; i++) {
        const emu_storage_operation_t *operation = &prepared->operations[i];
        cemu_storage_stage_t operation_stage;
        cemu_storage_space_t operation_space;
        if (!core_stage_from_x55(operation->stage, &operation_stage) ||
            !core_space_from_x55(operation->space, &operation_space))
            return cemu_status_error(
                CEMU_STATUS_INVALID_CONFIGURATION,
                "prepared storage operation has invalid CEMU type");
        if (operation_stage != stage) continue;
        cemu_status_t status = cemu_storage_transaction_add(
            transaction, operation->group, operation->chip_index,
            operation_space, operation->offset, operation->expected,
            operation->expected_size, operation->replacement,
            operation->replacement_size);
        if (status.code != CEMU_STATUS_OK) return status;
    }
    return cemu_status_ok();
}

void emu_cemu_storage_record_result(
        void *opaque, cemu_storage_stage_t stage, uint64_t group,
        cemu_storage_disposition_t disposition) {
    emu_cemu_storage_context_t *context = opaque;
    if (!context || !context->prepared) return;
    for (size_t i = 0; i < context->prepared->operation_count; i++) {
        const emu_storage_operation_t *operation =
            &context->prepared->operations[i];
        cemu_storage_stage_t operation_stage;
        if (!core_stage_from_x55(operation->stage, &operation_stage) ||
            operation_stage != stage || operation->group != group)
            continue;
        record_patch_group(operation,
                           disposition == CEMU_STORAGE_ALREADY_APPLIED,
                           &context->result);
        return;
    }
}

emu_error_code_t emu_cemu_apply_storage_stage(
        const emu_prepared_session_t *prepared, emu_storage_stage_t stage,
        soc_t *soc, emu_cemu_storage_result_t *result, emu_error_t *error) {
    emu_cemu_storage_result_t local = {0};
    if (result) *result = local;
    if (!prepared || !soc || (unsigned)stage > EMU_STORAGE_STAGE_POST_RESTORE)
        return fail(error, EMU_ERR_ARGUMENT,
                    "invalid CEMU storage transaction");

    flash_state_t staged[EMU_MAX_CHIPS];
    int affected[EMU_MAX_CHIPS] = {0};
    memset(staged, 0, sizeof staged);
    for (size_t i = 0; i < prepared->operation_count; i++) {
        const emu_storage_operation_t *operation = &prepared->operations[i];
        if (operation->stage != stage) continue;
        if (operation->chip_index >= prepared->chip_count ||
            operation->chip_index >= (size_t)soc->memory.n_flash_chips)
            return fail(error, EMU_ERR_INVALID_PREPARED_SESSION,
                        "storage operation has invalid CEMU flash chip");
        affected[operation->chip_index] = 1;
    }

    int copied = 0;
    for (size_t i = 0; i < prepared->chip_count; i++) {
        if (!affected[i]) continue;
        flash_state_t *source =
            cemu_memory_controller_flash_state(&soc->memory, (int)i);
        if ((fail_copy_after >= 0 && copied >= fail_copy_after) ||
            !source || !cemu_flash_state_copy(&staged[i], source)) {
            fail_copy_after = -1;
            free_staged(staged, affected);
            return fail(error, EMU_ERR_NOMEM,
                        "cannot clone CEMU storage transaction");
        }
        copied++;
    }
    fail_copy_after = -1;

    size_t first = 0;
    while (first < prepared->operation_count) {
        if (prepared->operations[first].stage != stage) {
            first++;
            continue;
        }
        size_t end = first + 1u;
        while (end < prepared->operation_count &&
               prepared->operations[end].stage == stage &&
               prepared->operations[end].group ==
                   prepared->operations[first].group)
            end++;
        int any_expected = 0;
        int any_replaced = 0;
        for (size_t i = first; i < end; i++) {
            const emu_storage_operation_t *operation =
                &prepared->operations[i];
            flash_state_t *state = &staged[operation->chip_index];
            int replaced = operation_matches(
                prepared, state, operation, operation->replacement,
                operation->replacement_size);
            int expected = operation->expected_size
                         ? operation_matches(
                               prepared, state, operation,
                               operation->expected, operation->expected_size)
                         : !replaced;
            if (!expected && !replaced) {
                free_staged(staged, affected);
                return fail(error, EMU_ERR_INVALID_PREPARED_SESSION,
                            "storage operation found unexpected current bytes");
            }
            /* An idempotent hunk whose expected and replacement bytes are
             * identical does not decide the state of its operation group. */
            if (expected && replaced) continue;
            any_expected |= expected;
            any_replaced |= replaced;
        }
        if (any_expected && any_replaced) {
            free_staged(staged, affected);
            return fail(error, EMU_ERR_INVALID_PREPARED_SESSION,
                        "storage operation group is partially applied");
        }
        if (any_expected) {
            for (size_t i = first; i < end; i++) {
                const emu_storage_operation_t *operation =
                    &prepared->operations[i];
                if (!apply_operation(
                        prepared, &staged[operation->chip_index], operation)) {
                    free_staged(staged, affected);
                    return fail(error, EMU_ERR_INVALID_PREPARED_SESSION,
                                "cannot apply CEMU storage operation");
                }
            }
        }
        record_patch_group(&prepared->operations[first], any_replaced,
                           &local);
        first = end;
    }

    for (size_t i = 0; i < prepared->chip_count; i++) {
        if (!affected[i]) continue;
        flash_state_t *destination =
            cemu_memory_controller_flash_state(&soc->memory, (int)i);
        cemu_flash_state_free(destination);
        *destination = staged[i];
        memset(&staged[i], 0, sizeof staged[i]);
    }
    if (result) *result = local;
    if (error) {
        error->code = EMU_OK;
        error->message[0] = 0;
    }
    return EMU_OK;
}
