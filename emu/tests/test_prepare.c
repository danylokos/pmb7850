#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "test_support.h"
#include "bundled_firmware.h"
#include "emu_prepare.h"

static const char *const C55_FLASH =
    BUNDLED_C55;
static const char *const M55_FLASH =
    BUNDLED_M55;

static void write_snapshot(const char *directory, const char *patch_field) {
    char path[512];
    EMU_CHECK(snprintf(path, sizeof path, "%s/snapshot.json", directory) > 0);
    FILE *file = fopen(path, "wb");
    EMU_CHECK(file != NULL);
    EMU_CHECK(fprintf(file,
                      "{\n  \"schema\": 7,\n  \"provenance\": {\n"
                      "    \"flash\": \"%s\"%s%s\n  }\n}\n",
                      C55_FLASH, patch_field ? ",\n" : "",
                      patch_field ? patch_field : "") > 0);
    EMU_CHECK(fclose(file) == 0);
}

static void test_fresh_preparation(void) {
    emu_preparation_request_t request = {
        .mode = EMU_STARTUP_FRESH,
        .source_path = C55_FLASH,
        .requested_device = "c55",
        .identity_source = EMU_IDENTITY_SOURCE_DEFAULT,
        .selected_patches = 1,
        .runtime_options = {.requested_slice_ticks = 17,
                            .synthetic_mask = 3,
                            .serial_autobaud_bypass = 1},
    };
    emu_prepared_session_t prepared;
    emu_preparation_result_t result;
    emu_error_t error = {0};
    EMU_CHECK(emu_prepare_startup(&request, &prepared, &result, &error) ==
              EMU_OK);
    EMU_CHECK(result.identity_planned);
    EMU_CHECK(result.identity.fsn == 0x1234ABCDu);
    EMU_CHECK(result.inherited_patches == 0);
    EMU_CHECK(result.effective_patches == 1);
    EMU_CHECK(prepared.operation_count >= 2);
    size_t patch_index = prepared.operation_count - 1u;
    EMU_CHECK(prepared.operations[0].stage == EMU_STORAGE_STAGE_PRE_RESET);
    EMU_CHECK(prepared.operations[patch_index].stage ==
              EMU_STORAGE_STAGE_POST_RESTORE);
    EMU_CHECK(prepared.options.requested_slice_ticks == 17);
    EMU_CHECK(prepared.options.synthetic_mask == 3);
    EMU_CHECK(prepared.options.serial_autobaud_bypass == 1);
    EMU_CHECK(emu_prepared_rehash_source(&prepared, &error) == EMU_OK);
    emu_prepared_free(&prepared);
}

static void test_resume_preparation(const char *directory) {
    write_snapshot(directory,
                   "    \"firmware_patches\": [\"c55-aircheck-off\"]");
    emu_preparation_request_t request = {
        .mode = EMU_STARTUP_RESUME,
        .snapshot_path = directory,
        .requested_device = "c55",
        .identity_source = EMU_IDENTITY_SOURCE_DEFAULT,
        .selected_patches = 1,
    };
    emu_prepared_session_t prepared;
    emu_preparation_result_t result;
    emu_error_t error = {0};
    EMU_CHECK(emu_prepare_startup(&request, &prepared, &result, &error) ==
              EMU_OK);
    EMU_CHECK(!result.identity_planned);
    EMU_CHECK(result.inherited_patches == 2);
    EMU_CHECK(result.effective_patches == 3);
    EMU_CHECK(!strcmp(result.source_path, C55_FLASH));
    EMU_CHECK(!strcmp(result.snapshot_path, directory));
    EMU_CHECK(prepared.operation_count == 2);
    for (size_t i = 0; i < prepared.operation_count; i++)
        EMU_CHECK(prepared.operations[i].stage ==
                  EMU_STORAGE_STAGE_POST_RESTORE);
    emu_prepared_free(&prepared);

    write_snapshot(directory, NULL);
    request.selected_patches = 0;
    EMU_CHECK(emu_prepare_startup(&request, &prepared, &result, &error) ==
              EMU_OK);
    EMU_CHECK(result.inherited_patches == 0);
    EMU_CHECK(prepared.operation_count == 0);
    emu_prepared_free(&prepared);
}

static void test_override_and_rejection(const char *directory) {
    write_snapshot(directory,
                   "    \"firmware_patches\": [\"c55-sqwe3\"]");
    emu_preparation_request_t request = {
        .mode = EMU_STARTUP_RESUME,
        .source_path = C55_FLASH,
        .snapshot_path = directory,
        .requested_device = "c55",
        .identity_source = EMU_IDENTITY_SOURCE_NONE,
    };
    emu_prepared_session_t prepared;
    emu_preparation_result_t result;
    emu_error_t error = {0};
    EMU_CHECK(emu_prepare_startup(&request, &prepared, &result, &error) ==
              EMU_OK);
    EMU_CHECK(result.effective_patches == 1);
    emu_prepared_free(&prepared);

    write_snapshot(directory,
                   "    \"firmware_patches\": [\"unknown\"]");
    EMU_CHECK(emu_prepare_startup(&request, &prepared, &result, &error) ==
              EMU_ERR_INVALID_PREPARED_SESSION);
    EMU_CHECK(strstr(error.message, "patch provenance") != NULL);
}

static void test_am29_fsn_imei_preparation(void) {
    emu_preparation_request_t request = {
        .mode = EMU_STARTUP_FRESH,
        .source_path = M55_FLASH,
        .requested_device = "m55",
        .identity_source = EMU_IDENTITY_SOURCE_FSN,
        .fsn = 0xC8AAE55Fu,
        .imei = "35202600729559",
    };
    emu_prepared_session_t prepared;
    emu_preparation_result_t result;
    emu_error_t error = {0};
    EMU_CHECK(emu_prepare_startup(&request, &prepared, &result, &error) ==
              EMU_OK);
    EMU_CHECK(result.identity_planned && result.identity.record_count == 0);
    EMU_CHECK(prepared.operation_count == 2);
    EMU_CHECK(prepared.operations[0].space == EMU_STORAGE_AM29_FACTORY_SECSI);
    EMU_CHECK(prepared.operations[1].space == EMU_STORAGE_AM29_CUSTOMER_SECSI);
    for (size_t i = 0; i < prepared.operation_count; i++)
        EMU_CHECK(prepared.operations[i].space != EMU_STORAGE_MAIN_ARRAY);
    emu_prepared_free(&prepared);
}

int main(void) {
    char directory[160];
    EMU_CHECK(snprintf(directory, sizeof directory,
                       "/tmp/x55-prepare-%ld", (long)getpid()) > 0);
    EMU_CHECK(mkdir(directory, 0700) == 0);
    test_fresh_preparation();
    test_resume_preparation(directory);
    test_override_and_rejection(directory);
    test_am29_fsn_imei_preparation();
    char path[512];
    EMU_CHECK(snprintf(path, sizeof path, "%s/snapshot.json", directory) > 0);
    EMU_CHECK(remove(path) == 0);
    EMU_CHECK(rmdir(directory) == 0);
    puts("host startup preparation: PASS");
    return 0;
}
