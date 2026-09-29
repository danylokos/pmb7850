#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_support.h"
#include "bundled_firmware.h"
#include "emu_patch.h"
#include "emu_product.h"

#define FLASH_SIZE (8u * 1024u * 1024u)

static void seed_catalog_originals(uint8_t *flash) {
    flash[0x4300C4] = 0x02;
    flash[0x522058] = 0x3D;
    flash[0x4CE010] = 0x2D;
    flash[0x4CE011] = 0x02;
    flash[0x519146] = 0x3D;
    flash[0x522A24] = 0x2D;
}

static void prepared_c55(emu_prepared_session_t *prepared, uint8_t *flash) {
    emu_prepared_init(prepared);
    prepared->source.locator = malloc(13);
    EMU_CHECK(prepared->source.locator != NULL);
    memcpy(prepared->source.locator, "memory:patch", 13);
    prepared->source.bytes = flash;
    prepared->source.size = FLASH_SIZE;
    emu_sha256(flash, FLASH_SIZE, prepared->source.sha256);
    emu_sha256_hex(prepared->source.sha256,
                   prepared->source.sha256_hex);
    snprintf(prepared->selected_device, sizeof prepared->selected_device,
             "c55");
    snprintf(prepared->metadata.model, sizeof prepared->metadata.model,
             "C55");
    prepared->metadata.software_version = 24;
    prepared->chip_count = 1;
    snprintf(prepared->chips[0].role, sizeof prepared->chips[0].role,
             "primary");
    snprintf(prepared->chips[0].model, sizeof prepared->chips[0].model,
             "am29lv640");
    prepared->chips[0].size = FLASH_SIZE;
    emu_sha256(flash, FLASH_SIZE, prepared->chips[0].sha256);
    emu_sha256_hex(prepared->chips[0].sha256,
                   prepared->chips[0].sha256_hex);
}

static char *read_stream(FILE *stream, size_t *size_out) {
    EMU_CHECK(fflush(stream) == 0);
    EMU_CHECK(fseek(stream, 0, SEEK_END) == 0);
    long length = ftell(stream);
    EMU_CHECK(length >= 0);
    EMU_CHECK(fseek(stream, 0, SEEK_SET) == 0);
    char *text = malloc((size_t)length + 1u);
    EMU_CHECK(text != NULL);
    EMU_CHECK(fread(text, 1, (size_t)length, stream) == (size_t)length);
    text[length] = 0;
    *size_out = (size_t)length;
    return text;
}

static emu_patch_set_t parse_set(const char *names) {
    emu_patch_set_t set = 0;
    char bad[96];
    EMU_CHECK(emu_patch_parse(&set, names, bad, sizeof bad) == 0);
    return set;
}

static void test_catalog_and_frozen_output(void) {
    size_t count = 0;
    const emu_patch_definition_t *catalog = emu_patch_catalog(&count);
    EMU_CHECK(count == 3);
    EMU_CHECK(!strcmp(catalog[0].name, "c55-sqwe3"));
    EMU_CHECK(!strcmp(catalog[1].name, "c55-aircheck-off"));
    EMU_CHECK(!strcmp(catalog[2].name, "c55-volume-check-off"));
    EMU_CHECK(catalog[2].hunk_count == 3);

    emu_patch_set_t set = parse_set(
        " c55-sqwe3, c55-aircheck-off,c55-volume-check-off ");
    EMU_CHECK(set == 7);
    char names[160];
    EMU_CHECK(emu_patch_active_names(set, names, sizeof names) == 3);
    EMU_CHECK(!strcmp(names, "c55-sqwe3,c55-aircheck-off,"
                             "c55-volume-check-off"));
    emu_patch_set_t before = set;
    char bad[96];
    EMU_CHECK(emu_patch_parse(&set, "unknown", bad, sizeof bad) == -1);
    EMU_CHECK(set == before && !strcmp(bad, "unknown"));

    FILE *listing = tmpfile();
    EMU_CHECK(listing != NULL);
    emu_patch_print(listing);
    size_t actual_size = 0;
    char *actual = read_stream(listing, &actual_size);
    EMU_CHECK(actual_size != 0);
    EMU_CHECK(strstr(actual, "c55-sqwe3") != NULL);
    free(actual);
    EMU_CHECK(fclose(listing) == 0);

    FILE *json = tmpfile();
    EMU_CHECK(json != NULL);
    emu_patch_print_json(json, set);
    actual = read_stream(json, &actual_size);
    static const char expected_json[] =
        "[\"c55-sqwe3\",\"c55-aircheck-off\","
        "\"c55-volume-check-off\"]";
    EMU_CHECK(actual_size == sizeof expected_json - 1u);
    EMU_CHECK(!strcmp(actual, expected_json));
    free(actual);
    EMU_CHECK(fclose(json) == 0);
}

static void test_catalog_planning_and_provenance(void) {
    uint8_t *flash = calloc(FLASH_SIZE, 1);
    EMU_CHECK(flash != NULL);
    seed_catalog_originals(flash);
    emu_prepared_session_t prepared;
    prepared_c55(&prepared, flash);
    emu_error_t error = {0};
    emu_patch_plan_result_t result;
    EMU_CHECK(emu_patch_plan(&prepared, 7, &result, &error) == EMU_OK);
    EMU_CHECK(result.applicable == 7 && result.already_applied == 0);
    EMU_CHECK(prepared.operation_count == 5);
    static const size_t offsets[] = {
        0x4300C4, 0x522058, 0x4CE010, 0x519146, 0x522A24,
    };
    for (size_t i = 0; i < 5; i++) {
        const emu_storage_operation_t *operation = &prepared.operations[i];
        EMU_CHECK(operation->order == i);
        EMU_CHECK(operation->stage == EMU_STORAGE_STAGE_POST_RESTORE);
        EMU_CHECK(operation->chip_index == 0);
        EMU_CHECK(operation->space == EMU_STORAGE_MAIN_ARRAY);
        EMU_CHECK(operation->offset == offsets[i]);
        EMU_CHECK(operation->group == (i < 1 ? 1u : i < 2 ? 2u : 3u));
        EMU_CHECK(operation->expected_size == operation->replacement_size);
        EMU_CHECK(strstr(operation->provenance, "firmware-patch:") ==
                  operation->provenance);
    }
    EMU_CHECK(!memcmp(prepared.source.bytes + 0x4300C4, "\x02", 1));
    emu_prepared_free(&prepared);
}

static void test_already_applied_and_target_mismatch(void) {
    uint8_t *flash = calloc(FLASH_SIZE, 1);
    EMU_CHECK(flash != NULL);
    seed_catalog_originals(flash);
    flash[0x4300C4] = 0x03;
    flash[0x522058] = 0x0D;
    flash[0x4CE010] = 0x0D;
    flash[0x4CE011] = 0x11;
    flash[0x519146] = 0x0D;
    flash[0x522A24] = 0x0D;
    emu_prepared_session_t prepared;
    prepared_c55(&prepared, flash);
    emu_error_t error = {0};
    emu_patch_plan_result_t result;
    EMU_CHECK(emu_patch_plan(&prepared, 7, &result, &error) == EMU_OK);
    EMU_CHECK(result.applicable == 0 && result.already_applied == 7);
    EMU_CHECK(prepared.operation_count == 5);
    emu_prepared_free(&prepared);

    flash = calloc(FLASH_SIZE, 1);
    EMU_CHECK(flash != NULL);
    seed_catalog_originals(flash);
    prepared_c55(&prepared, flash);
    snprintf(prepared.selected_device, sizeof prepared.selected_device,
             "a55");
    EMU_CHECK(emu_patch_plan(&prepared, 1, &result, &error) ==
              EMU_ERR_INVALID_PREPARED_SESSION);
    EMU_CHECK(strstr(error.message, "requires device c55") != NULL);
    EMU_CHECK(prepared.operation_count == 0);
    emu_prepared_free(&prepared);
}

static void test_partial_conflict_range_and_rollback(void) {
    static const uint8_t original[] = {0x11, 0x22};
    static const uint8_t replacement[] = {0x33, 0x44};
    static const uint8_t conflict_replacement[] = {0x33, 0x55};
    static const emu_patch_hunk_t hunks[] = {
        {0, 0x100, original, replacement, 2},
    };
    static const emu_patch_hunk_t conflict_hunks[] = {
        {0, 0x100, original, conflict_replacement, 2},
    };
    static const emu_patch_hunk_t mixed_hunks[] = {
        {0, 0x100, original, replacement, 1},
        {0, 0x101, original + 1, replacement + 1, 1},
    };
    static const emu_patch_hunk_t outside_hunks[] = {
        {0, FLASH_SIZE - 1u, original, replacement, 2},
    };
    static const emu_patch_definition_t definitions[] = {
        {"test-a", "test", "c55", "C55", 24, "test", hunks, 1},
        {"test-b", "test", "c55", "C55", 24, "test",
         conflict_hunks, 1},
    };
    static const emu_patch_definition_t mixed[] = {
        {"test-mixed", "test", "c55", "C55", 24, "test",
         mixed_hunks, 2},
    };
    static const emu_patch_definition_t outside[] = {
        {"test-outside", "test", "c55", "C55", 24, "test",
         outside_hunks, 1},
    };
    uint8_t *flash = calloc(FLASH_SIZE, 1);
    EMU_CHECK(flash != NULL);
    flash[0x100] = original[0];
    flash[0x101] = 0x99;
    emu_prepared_session_t prepared;
    prepared_c55(&prepared, flash);
    emu_error_t error = {0};
    emu_patch_plan_result_t result;
    uint8_t marker = 0xAA;
    EMU_CHECK(emu_prepared_add_storage_operation(
                  &prepared, 0, EMU_STORAGE_MAIN_ARRAY, 0x80, &marker, 1,
                  &marker, 1, "existing", &error) == EMU_OK);
    EMU_CHECK(emu_patch_plan_definitions(&prepared, definitions, 1, 1,
                                         &result, &error) ==
              EMU_ERR_INVALID_PREPARED_SESSION);
    EMU_CHECK(strstr(error.message, "partial or unexpected") != NULL);
    EMU_CHECK(prepared.operation_count == 1);

    flash[0x100] = replacement[0];
    flash[0x101] = original[1];
    EMU_CHECK(emu_patch_plan_definitions(&prepared, mixed, 1, 1,
                                         &result, &error) ==
              EMU_ERR_INVALID_PREPARED_SESSION);
    EMU_CHECK(strstr(error.message, "partially applied") != NULL);
    EMU_CHECK(prepared.operation_count == 1);

    flash[0x100] = original[0];
    flash[0x101] = original[1];
    EMU_CHECK(emu_patch_plan_definitions(&prepared, definitions, 2, 3,
                                         &result, &error) ==
              EMU_ERR_INVALID_PREPARED_SESSION);
    EMU_CHECK(strstr(error.message, "conflicting firmware patches") != NULL);
    EMU_CHECK(prepared.operation_count == 1);
    EMU_CHECK(emu_patch_plan_definitions(&prepared, outside, 1, 1,
                                         &result, &error) ==
              EMU_ERR_INVALID_PREPARED_SESSION);
    EMU_CHECK(strstr(error.message, "out of range") != NULL);
    EMU_CHECK(prepared.operation_count == 1);
    EMU_CHECK(emu_patch_plan_definitions(&prepared, definitions, 1, 2,
                                         &result, &error) ==
              EMU_ERR_ARGUMENT);
    EMU_CHECK(strstr(error.message, "selection is out of range") != NULL);
    emu_prepared_free(&prepared);
}

static void test_canonical_c55_image(void) {
    emu_prepared_session_t prepared;
    emu_prepared_init(&prepared);
    emu_error_t error = {0};
    EMU_CHECK(emu_prepared_load_source(
                  &prepared, BUNDLED_C55,
                  &error) == EMU_OK);
    EMU_CHECK(emu_product_prepare_image(&prepared, "c55", &error) == EMU_OK);
    emu_patch_plan_result_t result;
    EMU_CHECK(emu_patch_plan(&prepared, 7, &result, &error) == EMU_OK);
    EMU_CHECK(result.applicable == 7 && result.already_applied == 0);
    EMU_CHECK(prepared.operation_count == 5);
    EMU_CHECK(emu_prepared_rehash_source(&prepared, &error) == EMU_OK);
    emu_prepared_free(&prepared);
}

int main(void) {
    test_catalog_and_frozen_output();
    test_catalog_planning_and_provenance();
    test_already_applied_and_target_mismatch();
    test_partial_conflict_range_and_rollback();
    test_canonical_c55_image();
    puts("test_patch: ok");
    return 0;
}
