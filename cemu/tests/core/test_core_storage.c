#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cemu_core.h"
#include "cemu_core_private.h"
#include "flash.h"

static int failures;

#define CHECK(condition) do { \
    if (!(condition)) { \
        failures++; \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
    } \
} while (0)

typedef struct {
    cemu_storage_stage_t stage;
    uint64_t group;
    size_t chip_index;
    cemu_storage_space_t space;
    size_t offset;
    const uint8_t *expected;
    size_t expected_size;
    const uint8_t *replacement;
    size_t replacement_size;
} storage_spec_t;

typedef struct {
    const storage_spec_t *specs;
    size_t spec_count;
    cemu_storage_stage_t calls[16];
    size_t call_count;
    cemu_storage_stage_t result_stages[32];
    uint64_t result_groups[32];
    cemu_storage_disposition_t dispositions[32];
    size_t result_count;
    int fail_add;
    int add_invalid_operation;
} storage_context_t;

static cemu_status_t initialize_storage(
        void *opaque, cemu_storage_stage_t stage,
        cemu_storage_transaction_t *transaction) {
    storage_context_t *context = opaque;
    if (context->call_count < sizeof(context->calls) / sizeof(context->calls[0]))
        context->calls[context->call_count++] = stage;
    for (size_t i = 0; i < context->spec_count; i++) {
        const storage_spec_t *spec = &context->specs[i];
        if (spec->stage != stage) continue;
        if (context->fail_add) cemu_test_fail_alloc_after(0);
        cemu_status_t status = cemu_storage_transaction_add(
            transaction, spec->group, spec->chip_index, spec->space,
            spec->offset, spec->expected, spec->expected_size,
            spec->replacement, spec->replacement_size);
        if (context->fail_add) cemu_test_clear_alloc_failure();
        if (status.code != CEMU_STATUS_OK) return status;
    }
    if (context->add_invalid_operation && stage == CEMU_STORAGE_PRE_RESET) {
        static const uint8_t expected = 0;
        static const uint8_t replacement = 1;
        return cemu_storage_transaction_add(
            transaction, UINT64_C(99), 0, CEMU_STORAGE_MAIN_ARRAY,
            0x103, &expected, 1, &replacement, 1);
    }
    return cemu_status_ok();
}

static void storage_result(
        void *opaque, cemu_storage_stage_t stage, uint64_t group,
        cemu_storage_disposition_t disposition) {
    storage_context_t *context = opaque;
    if (context->result_count >=
        sizeof(context->result_groups) / sizeof(context->result_groups[0]))
        return;
    size_t i = context->result_count++;
    context->result_stages[i] = stage;
    context->result_groups[i] = group;
    context->dispositions[i] = disposition;
}

static uint8_t *make_flash(size_t size) {
    uint8_t *flash = malloc(size);
    if (!flash) return NULL;
    memset(flash, 0xFF, size);
    flash[0] = 0xFA; flash[1] = 0x80;
    flash[2] = 0x04; flash[3] = 0x00;
    flash[4] = 0xCC; flash[5] = 0x00;
    flash[6] = 0x0D; flash[7] = 0xFE;
    return flash;
}

static cemu_core_options_t storage_options(
        uint8_t *flash, size_t size, const char *device,
        storage_context_t *context) {
    return (cemu_core_options_t){
        .source = flash,
        .source_size = size,
        .device = *cemu_device_by_name(device),
        .storage_initializer = initialize_storage,
        .storage_result = storage_result,
        .storage_opaque = context,
    };
}

static uint8_t core_flash_byte(cemu_core_t *core, size_t chip_index,
                               uint32_t offset) {
    memory_controller_t *memory =
        &cemu_core_private_soc(core)->memory;
    flash_state_t *state = cemu_memory_controller_flash_state(
        memory, (int)chip_index);
    return cemu_flash_array_read8(
        memory->flash_data + memory->flash_file_offsets[chip_index],
        memory->flash_chips[chip_index].chip_size, state, offset);
}

static void test_all_spaces_order_idempotence_and_reset(void) {
    const size_t size = 8u * 1024u * 1024u;
    uint8_t *flash = make_flash(size);
    const uint8_t erased = 0xFF;
    const uint8_t pre_value = 0x12;
    const uint8_t post_value = 0x34;
    const uint8_t impossible = 0x00;
    const uint8_t uid[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    const storage_spec_t specs[] = {
        {CEMU_STORAGE_PRE_RESET, 1, 0, CEMU_STORAGE_MAIN_ARRAY, 0x100,
         &erased, 1, &pre_value, 1},
        {CEMU_STORAGE_PRE_RESET, 2, 0, CEMU_STORAGE_FACTORY_UID, 0,
         NULL, 0, uid, sizeof uid},
        {CEMU_STORAGE_PRE_RESET, 3, 0, CEMU_STORAGE_MAIN_ARRAY, 0x102,
         &impossible, 1, &erased, 1},
        /* Seeing the pre-reset replacement here proves stage ordering. */
        {CEMU_STORAGE_POST_RESET, 4, 0, CEMU_STORAGE_MAIN_ARRAY, 0x100,
         &pre_value, 1, &post_value, 1},
    };
    storage_context_t context = {
        .specs = specs,
        .spec_count = sizeof specs / sizeof specs[0],
    };
    cemu_core_options_t options = storage_options(
        flash, size, "c55", &context);
    cemu_core_t *core = NULL;
    CHECK(cemu_core_create(&core, &options).code == CEMU_STATUS_OK);
    CHECK(core != NULL);
    CHECK(context.call_count == 2);
    CHECK(context.calls[0] == CEMU_STORAGE_PRE_RESET);
    CHECK(context.calls[1] == CEMU_STORAGE_POST_RESET);
    CHECK(context.result_count == 4);
    CHECK(context.result_stages[0] == CEMU_STORAGE_PRE_RESET);
    CHECK(context.result_stages[3] == CEMU_STORAGE_POST_RESET);
    CHECK(context.dispositions[0] == CEMU_STORAGE_APPLIED);
    CHECK(context.dispositions[2] == CEMU_STORAGE_ALREADY_APPLIED);
    CHECK(core_flash_byte(core, 0, 0x100) == post_value);
    CHECK(flash[0x100] == erased);
    flash_state_t *state = cemu_memory_controller_flash_state(
        &cemu_core_private_soc(core)->memory, 0);
    CHECK(state->kind == FLASH_MODEL_M58LW064D);
    CHECK(state->u.m58.factory_uid_set);
    CHECK(memcmp(state->u.m58.factory_uid, uid, sizeof uid) == 0);

    /* Guest mutation remains sparse and never changes the borrowed source. */
    const uint8_t guest = 0x77;
    CHECK(cemu_flash_seed_bytes(flash, size, state, 0x100, &guest, 1));
    CHECK(core_flash_byte(core, 0, 0x100) == guest);
    CHECK(flash[0x100] == erased);

    cemu_core_state_t before, after;
    CHECK(cemu_core_query(core, &before).code == CEMU_STATUS_OK);
    cemu_test_fail_storage_clone_after(0);
    CHECK(cemu_core_reset(core).code == CEMU_STATUS_ALLOCATION_FAILED);
    CHECK(cemu_core_query(core, &after).code == CEMU_STATUS_OK);
    CHECK(memcmp(&before, &after, sizeof before) == 0);
    CHECK(core_flash_byte(core, 0, 0x100) == guest);

    context.add_invalid_operation = 1;
    CHECK(cemu_core_reset(core).code == CEMU_STATUS_INVALID_CONFIGURATION);
    CHECK(core_flash_byte(core, 0, 0x100) == guest);
    context.add_invalid_operation = 0;
    CHECK(cemu_core_reset(core).code == CEMU_STATUS_OK);
    CHECK(core_flash_byte(core, 0, 0x100) == post_value);
    CHECK(flash[0x100] == erased);

    cemu_core_destroy(core);
    free(flash);
}

static void test_am29_factory_and_customer_secsi(void) {
    const size_t size = 16u * 1024u * 1024u;
    uint8_t *flash = make_flash(size);
    const uint8_t erased = 0xFF;
    const uint8_t factory[] = {0x10, 0x11, 0x12};
    const uint8_t customer[] = {0x20, 0x21, 0x22, 0x23};
    const storage_spec_t specs[] = {
        {CEMU_STORAGE_PRE_RESET, 1, 0,
         CEMU_STORAGE_AM29_FACTORY_SECSI, 0,
         &erased, 1, factory, sizeof factory},
        {CEMU_STORAGE_PRE_RESET, 2, 0,
         CEMU_STORAGE_AM29_CUSTOMER_SECSI, 16,
         &erased, 1, customer, sizeof customer},
    };
    storage_context_t context = {
        .specs = specs,
        .spec_count = sizeof specs / sizeof specs[0],
    };
    cemu_core_options_t options = storage_options(
        flash, size, "m55", &context);
    cemu_core_t *core = NULL;
    CHECK(cemu_core_create(&core, &options).code == CEMU_STATUS_OK);
    flash_state_t *state = cemu_memory_controller_flash_state(
        &cemu_core_private_soc(core)->memory, 0);
    am29lv_state_t *am29 = cemu_flash_am29_state(state);
    CHECK(am29 != NULL);
    CHECK(!memcmp(am29->secsi, factory, sizeof factory));
    CHECK(!memcmp(am29->secsi + 16, customer, sizeof customer));
    CHECK(am29->secsi_esn_set);
    CHECK(flash[0] == 0xFA && flash[16] == 0xFF);
    cemu_core_destroy(core);
    free(flash);
}

static void test_partial_group_and_add_allocation_failure(void) {
    const size_t size = 8u * 1024u * 1024u;
    uint8_t *flash = make_flash(size);
    const uint8_t erased = 0xFF;
    const uint8_t changed = 0x55;
    const uint8_t impossible = 0x00;
    const storage_spec_t partial[] = {
        {CEMU_STORAGE_PRE_RESET, 1, 0, CEMU_STORAGE_MAIN_ARRAY, 0x100,
         &erased, 1, &changed, 1},
        {CEMU_STORAGE_PRE_RESET, 1, 0, CEMU_STORAGE_MAIN_ARRAY, 0x101,
         &impossible, 1, &erased, 1},
    };
    storage_context_t context = {
        .specs = partial,
        .spec_count = sizeof partial / sizeof partial[0],
    };
    cemu_core_options_t options = storage_options(
        flash, size, "c55", &context);
    cemu_core_t *core = (cemu_core_t *)(uintptr_t)1;
    cemu_status_t status = cemu_core_create(&core, &options);
    CHECK(status.code == CEMU_STATUS_INVALID_CONFIGURATION);
    CHECK(strstr(status.message, "partially applied") != NULL);
    CHECK(core == NULL);
    CHECK(flash[0x100] == erased);
    CHECK(context.result_count == 0);

    context.specs = partial;
    context.spec_count = 1;
    context.fail_add = 1;
    core = (cemu_core_t *)(uintptr_t)1;
    status = cemu_core_create(&core, &options);
    CHECK(status.code == CEMU_STATUS_ALLOCATION_FAILED);
    CHECK(core == NULL);
    CHECK(flash[0x100] == erased);
    CHECK(context.result_count == 0);

    free(flash);
}

int main(void) {
    test_all_spaces_order_idempotence_and_reset();
    test_am29_factory_and_customer_secsi();
    test_partial_group_and_add_allocation_failure();
    printf("core storage transactions: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
