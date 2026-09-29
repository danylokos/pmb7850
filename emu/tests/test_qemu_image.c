#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "test_support.h"
#include "bundled_firmware.h"
#include "emu_prepare.h"
#include "emu_patch.h"
#include "emu_product.h"
#include "emu_qemu_image.h"

#define MIB(n) ((size_t)(n) * 1024u * 1024u)

static int is_am29(const char *model) {
    return !strncmp(model, "am29lv", 6);
}

static void rehash(emu_prepared_session_t *prepared) {
    emu_sha256(prepared->source.bytes, prepared->source.size,
               prepared->source.sha256);
    emu_sha256_hex(prepared->source.sha256,
                   prepared->source.sha256_hex);
    for (size_t i = 0; i < prepared->chip_count; i++) {
        emu_chip_view_t *chip = &prepared->chips[i];
        emu_sha256(prepared->source.bytes + chip->source_offset,
                   chip->size, chip->sha256);
        emu_sha256_hex(chip->sha256, chip->sha256_hex);
    }
}

static void prepared_product(emu_prepared_session_t *prepared,
                             const char *device,
                             const char *primary_model,
                             int secondary_first) {
    const emu_product_t *product = emu_product_by_name(device);
    EMU_CHECK(product != NULL);
    emu_prepared_init(prepared);
    prepared->source.locator = malloc(20u);
    EMU_CHECK(prepared->source.locator != NULL);
    memcpy(prepared->source.locator, "memory:qemu-product", 20u);
    for (size_t i = 0; i < product->chip_count; i++)
        prepared->source.size += product->chips[i].size;
    prepared->source.bytes = malloc(prepared->source.size);
    EMU_CHECK(prepared->source.bytes != NULL);
    snprintf(prepared->selected_device, sizeof prepared->selected_device,
             "%s", device);
    snprintf(prepared->metadata.model, sizeof prepared->metadata.model,
             "%s", product->image_model);
    snprintf(prepared->metadata.flash_engine,
             sizeof prepared->metadata.flash_engine, "%s", primary_model);
    prepared->chip_count = product->chip_count;
    prepared->identity_chip_index = product->identity_chip_index;
    for (size_t i = 0; i < product->chip_count; i++) {
        emu_chip_view_t *chip = &prepared->chips[i];
        const emu_product_chip_t *catalog = &product->chips[i];
        snprintf(chip->role, sizeof chip->role, "%s", catalog->name);
        snprintf(chip->model, sizeof chip->model, "%s",
                 i == product->primary_chip_index
                    ? primary_model : catalog->default_model);
        chip->size = catalog->size;
        if (product->chip_count == 1) {
            chip->source_offset = 0;
        } else if (i == product->primary_chip_index) {
            chip->source_offset = secondary_first
                ? product->chips[1u - product->primary_chip_index].size : 0;
        } else {
            chip->source_offset = secondary_first
                ? 0 : product->chips[product->primary_chip_index].size;
        }
        memset(prepared->source.bytes + chip->source_offset,
               (int)(0x31u + i), chip->size);
    }
    prepared->identity_kind = is_am29(
        prepared->chips[prepared->identity_chip_index].model)
        ? EMU_IDENTITY_AM29_SECSI : EMU_IDENTITY_FACTORY_UID;
    rehash(prepared);
}

static void add_operation(emu_prepared_session_t *prepared,
                          emu_storage_stage_t stage, size_t group,
                          size_t chip_index, emu_storage_space_t space,
                          size_t offset, const void *expected,
                          size_t expected_size, const void *replacement,
                          size_t replacement_size) {
    emu_error_t error = {0};
    EMU_CHECK(emu_prepared_add_grouped_storage_operation(
        prepared, stage, group, chip_index, space, offset,
        expected, expected_size, replacement, replacement_size,
        "qemu-image-test", &error) == EMU_OK);
}

static void read_at(const char *path, size_t offset, uint8_t *bytes,
                    size_t size) {
    FILE *file = fopen(path, "rb");
    EMU_CHECK(file != NULL);
    EMU_CHECK(fseek(file, (long)offset, SEEK_SET) == 0);
    EMU_CHECK(fread(bytes, 1, size, file) == size);
    EMU_CHECK(fclose(file) == 0);
}

static size_t count_session_directories(void) {
    DIR *directory = opendir("/tmp");
    EMU_CHECK(directory != NULL);
    size_t count = 0;
    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL)
        if (!strncmp(entry->d_name, "emu-qemu-", 9u)) count++;
    EMU_CHECK(closedir(directory) == 0);
    return count;
}

static void check_private_files(const emu_qemu_image_t *image) {
    struct stat status;
    EMU_CHECK(stat(image->directory, &status) == 0);
    EMU_CHECK((status.st_mode & 0777) == 0700);
    for (size_t i = 0; i < image->chip_count; i++) {
        EMU_CHECK(stat(image->chips[i].path, &status) == 0);
        EMU_CHECK((status.st_mode & 0777) == 0600);
        EMU_CHECK((size_t)status.st_size == image->chips[i].size);
    }
}

static void test_unique_topologies(void) {
    static const struct {
        const char *device;
        const char *model;
        int secondary_first;
    } cases[] = {
        {"a52", "m58lw064d", 0},
        {"c55", "m58lw064d", 0},
        {"a60", "am29lv640mh", 0},
        {"m55", "am29lv128mh", 0},
        {"c60", "am29lv128mh", 0},
        {"c60", "w30-128mbit-top", 0},
        {"s55", "w30-64mbit-top", 0},
        {"s55", "w30-64mbit-top", 1},
        {"sl55", "w30-64mbit-top", 0},
        {"sl55", "w30-64mbit-top", 1},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        emu_prepared_session_t prepared;
        emu_qemu_image_t image = {0};
        emu_error_t error = {0};
        prepared_product(&prepared, cases[i].device, cases[i].model,
                         cases[i].secondary_first);
        EMU_CHECK(emu_qemu_image_materialize(
            &prepared, &image, &error) == EMU_OK);
        EMU_CHECK(image.chip_count == prepared.chip_count);
        EMU_CHECK(image.combined_size == prepared.source.size);
        check_private_files(&image);
        uint8_t first = 0, last = 0;
        EMU_CHECK(emu_qemu_image_read(
            &image, 0, &first, 1, &error) == EMU_OK);
        EMU_CHECK(emu_qemu_image_read(
            &image, prepared.source.size - 1u, &last, 1,
            &error) == EMU_OK);
        EMU_CHECK(first == prepared.source.bytes[0]);
        EMU_CHECK(last == prepared.source.bytes[prepared.source.size - 1u]);
        char directory[EMU_QEMU_PATH_MAX];
        snprintf(directory, sizeof directory, "%s", image.directory);
        emu_qemu_image_destroy(&image);
        EMU_CHECK(access(directory, F_OK) != 0);
        emu_prepared_free(&prepared);
    }
}

static void test_combined_read_export_and_atomic_cross_chip(void) {
    for (int secondary_first = 0; secondary_first <= 1; secondary_first++) {
        emu_prepared_session_t prepared;
        emu_qemu_image_t image = {0};
        emu_error_t error = {0};
        prepared_product(&prepared, "s55", "w30-64mbit-top",
                         secondary_first);
        uint8_t old_primary = prepared.source.bytes[
            prepared.chips[0].source_offset + 0x100u];
        uint8_t old_secondary = prepared.source.bytes[
            prepared.chips[1].source_offset + 0x200u];
        const uint8_t new_primary = 0xA1;
        const uint8_t new_secondary = 0xB2;
        add_operation(&prepared, EMU_STORAGE_STAGE_PRE_RESET, 1, 0,
                      EMU_STORAGE_MAIN_ARRAY, 0x100, &old_primary, 1,
                      &new_primary, 1);
        add_operation(&prepared, EMU_STORAGE_STAGE_PRE_RESET, 1, 1,
                      EMU_STORAGE_MAIN_ARRAY, 0x200, &old_secondary, 1,
                      &new_secondary, 1);
        uint8_t source_hash[EMU_SHA256_SIZE];
        memcpy(source_hash, prepared.source.sha256, sizeof source_hash);
        EMU_CHECK(emu_qemu_image_materialize(
            &prepared, &image, &error) == EMU_OK);
        uint8_t value = 0;
        EMU_CHECK(emu_qemu_image_read(
            &image, prepared.chips[0].source_offset + 0x100u,
            &value, 1, &error) == EMU_OK);
        EMU_CHECK(value == new_primary);
        EMU_CHECK(emu_qemu_image_read(
            &image, prepared.chips[1].source_offset + 0x200u,
            &value, 1, &error) == EMU_OK);
        EMU_CHECK(value == new_secondary);
        EMU_CHECK(prepared.source.bytes[
            prepared.chips[0].source_offset + 0x100u] == old_primary);
        EMU_CHECK(!memcmp(source_hash, prepared.source.sha256,
                          sizeof source_hash));

        char export_path[160];
        EMU_CHECK(snprintf(export_path, sizeof export_path,
                           "/tmp/emu-qemu-export-%ld-%d.bin",
                           (long)getpid(), secondary_first) > 0);
        EMU_CHECK(emu_qemu_image_export(
            &image, export_path, &error) == EMU_OK);
        read_at(export_path, prepared.chips[0].source_offset + 0x100u,
                &value, 1);
        EMU_CHECK(value == new_primary);
        read_at(export_path, prepared.chips[1].source_offset + 0x200u,
                &value, 1);
        EMU_CHECK(value == new_secondary);
        EMU_CHECK(remove(export_path) == 0);
        emu_qemu_image_destroy(&image);
        emu_prepared_free(&prepared);
    }
}

static void test_identity_spaces(void) {
    static const uint8_t uid[8] =
        {0x00,0x00,0x00,0x00,0x40,0x79,0xA7,0xE9};
    emu_prepared_session_t prepared;
    emu_qemu_image_t image = {0};
    emu_error_t error = {0};
    prepared_product(&prepared, "c55", "m58lw064d", 0);
    uint8_t old = prepared.source.bytes[0x100];
    const uint8_t replacement = 0x55;
    add_operation(&prepared, EMU_STORAGE_STAGE_PRE_RESET, 1, 0,
                  EMU_STORAGE_MAIN_ARRAY, 0x100, &old, 1,
                  &replacement, 1);
    add_operation(&prepared, EMU_STORAGE_STAGE_PRE_RESET, 1, 0,
                  EMU_STORAGE_FACTORY_UID, 0, NULL, 0, uid, sizeof uid);
    EMU_CHECK(emu_qemu_image_materialize(
        &prepared, &image, &error) == EMU_OK);
    EMU_CHECK(image.chips[0].factory_uid_set);
    EMU_CHECK(!memcmp(image.chips[0].factory_uid, uid, sizeof uid));
    emu_qemu_image_destroy(&image);
    emu_prepared_free(&prepared);

    static const uint8_t factory[8] = {1,2,3,4,5,6,7,8};
    static const uint8_t customer[8] = {8,7,6,5,4,3,2,1};
    prepared_product(&prepared, "m55", "am29lv128mh", 0);
    add_operation(&prepared, EMU_STORAGE_STAGE_PRE_RESET, 1, 0,
                  EMU_STORAGE_AM29_FACTORY_SECSI, 0, NULL, 0,
                  factory, sizeof factory);
    add_operation(&prepared, EMU_STORAGE_STAGE_PRE_RESET, 1, 0,
                  EMU_STORAGE_AM29_CUSTOMER_SECSI, 0x10, NULL, 0,
                  customer, sizeof customer);
    EMU_CHECK(emu_qemu_image_materialize(
        &prepared, &image, &error) == EMU_OK);
    EMU_CHECK(image.chips[0].am29_secsi_set);
    EMU_CHECK(!memcmp(image.chips[0].am29_secsi, factory,
                      sizeof factory));
    EMU_CHECK(!memcmp(image.chips[0].am29_secsi + 0x10, customer,
                      sizeof customer));
    emu_qemu_image_destroy(&image);
    emu_prepared_free(&prepared);
}

static void expect_materialize_error(emu_prepared_session_t *prepared,
                                     emu_error_code_t expected,
                                     const char *message) {
    emu_qemu_image_t image = {0};
    emu_error_t error = {0};
    emu_error_code_t actual = emu_qemu_image_materialize(
        prepared, &image, &error);
    if (actual != expected)
        fprintf(stderr, "expected error %d, got %d: %s\n",
                expected, actual, error.message);
    EMU_CHECK(actual == expected);
    EMU_CHECK(strstr(error.message, message) != NULL);
    EMU_CHECK(!image.directory[0] && !image.chip_count);
    emu_prepared_free(prepared);
}

static void test_rejections(void) {
    emu_prepared_session_t prepared;
    uint8_t old = 0x31;
    const uint8_t replacement = 0x77;
    static const uint8_t identity[8] = {0};

    prepared_product(&prepared, "c55", "m58lw064d", 0);
    snprintf(prepared.chips[0].model, sizeof prepared.chips[0].model,
             "am29lv640mh");
    expect_materialize_error(&prepared, EMU_ERR_INVALID_PREPARED_SESSION,
                             "does not permit model");

    prepared_product(&prepared, "c55", "m58lw064d", 0);
    snprintf(prepared.chips[0].role, sizeof prepared.chips[0].role,
             "secondary");
    expect_materialize_error(&prepared, EMU_ERR_INVALID_PREPARED_SESSION,
                             "has role");

    prepared_product(&prepared, "c55", "m58lw064d", 0);
    prepared.chips[0].size--;
    rehash(&prepared);
    expect_materialize_error(&prepared, EMU_ERR_INVALID_PREPARED_SESSION,
                             "physical chip views do not cover");

    prepared_product(&prepared, "s55", "w30-64mbit-top", 0);
    prepared.identity_chip_index = 1;
    expect_materialize_error(&prepared, EMU_ERR_INVALID_PREPARED_SESSION,
                             "identity owner");

    prepared_product(&prepared, "c55", "m58lw064d", 0);
    add_operation(&prepared, EMU_STORAGE_STAGE_POST_RESTORE, 1, 0,
                  EMU_STORAGE_FACTORY_UID, 0, NULL, 0,
                  identity, sizeof identity);
    expect_materialize_error(&prepared, EMU_ERR_UNSUPPORTED,
                             "require the main array");

    prepared_product(&prepared, "c55", "m58lw064d", 0);
    prepared.options.snapshot_path = "/tmp/saved-qemu-snapshot";
    add_operation(&prepared, EMU_STORAGE_STAGE_POST_RESTORE, 1, 0,
                  EMU_STORAGE_MAIN_ARRAY, 0, &old, 1, &replacement, 1);
    expect_materialize_error(&prepared, EMU_ERR_UNSUPPORTED,
                             "snapshot resume cannot apply post-restore");

    prepared_product(&prepared, "c55", "m58lw064d", 0);
    add_operation(&prepared, EMU_STORAGE_STAGE_PRE_RESET, 1, 0,
                  EMU_STORAGE_AM29_FACTORY_SECSI, 0, NULL, 0,
                  identity, sizeof identity);
    expect_materialize_error(&prepared, EMU_ERR_INVALID_PREPARED_SESSION,
                             "invalid AM29 SecSi");

    prepared_product(&prepared, "m55", "am29lv128mh", 0);
    add_operation(&prepared, EMU_STORAGE_STAGE_PRE_RESET, 1, 0,
                  EMU_STORAGE_FACTORY_UID, 0, NULL, 0,
                  identity, sizeof identity);
    expect_materialize_error(&prepared, EMU_ERR_INVALID_PREPARED_SESSION,
                             "invalid factory UID");

    prepared_product(&prepared, "c55", "m58lw064d", 0);
    add_operation(&prepared, EMU_STORAGE_STAGE_PRE_RESET, 1, 0,
                  EMU_STORAGE_MAIN_ARRAY, prepared.chips[0].size,
                  &old, 1, &replacement, 1);
    expect_materialize_error(&prepared, EMU_ERR_INVALID_PREPARED_SESSION,
                             "out of bounds");

    prepared_product(&prepared, "c55", "m58lw064d", 0);
    old = 0x00;
    add_operation(&prepared, EMU_STORAGE_STAGE_PRE_RESET, 1, 0,
                  EMU_STORAGE_MAIN_ARRAY, 0x100, &old, 1,
                  &replacement, 1);
    expect_materialize_error(&prepared, EMU_ERR_INVALID_PREPARED_SESSION,
                             "conflicts on chip");

    prepared_product(&prepared, "c55", "m58lw064d", 0);
    old = prepared.source.bytes[0x100];
    const uint8_t other = 0x88;
    add_operation(&prepared, EMU_STORAGE_STAGE_POST_RESTORE, 1, 0,
                  EMU_STORAGE_MAIN_ARRAY, 0x100, &old, 1,
                  &replacement, 1);
    add_operation(&prepared, EMU_STORAGE_STAGE_POST_RESTORE, 1, 0,
                  EMU_STORAGE_MAIN_ARRAY, 0x100, &old, 1, &other, 1);
    expect_materialize_error(&prepared, EMU_ERR_INVALID_PREPARED_SESSION,
                             "conflicting replacements");

    prepared_product(&prepared, "s55", "w30-64mbit-top", 0);
    uint8_t old_primary = prepared.source.bytes[
        prepared.chips[0].source_offset + 0x100u];
    uint8_t old_secondary = prepared.source.bytes[
        prepared.chips[1].source_offset + 0x100u];
    add_operation(&prepared, EMU_STORAGE_STAGE_POST_RESTORE, 1, 0,
                  EMU_STORAGE_MAIN_ARRAY, 0x100, &old_primary, 1,
                  &replacement, 1);
    add_operation(&prepared, EMU_STORAGE_STAGE_POST_RESTORE, 1, 1,
                  EMU_STORAGE_MAIN_ARRAY, 0x100, &old_secondary, 1,
                  &other, 1);
    prepared.source.bytes[prepared.chips[0].source_offset + 0x100u] =
        replacement;
    rehash(&prepared);
    expect_materialize_error(&prepared, EMU_ERR_INVALID_PREPARED_SESSION,
                             "partially applied");
}

static void test_post_restore_patch(void) {
    emu_preparation_request_t request = {
        .mode = EMU_STARTUP_FRESH,
        .source_path = BUNDLED_C55,
        .requested_device = "c55",
        .identity_source = EMU_IDENTITY_SOURCE_DEFAULT,
    };
    char bad[64];
    EMU_CHECK(emu_patch_parse(&request.selected_patches, "c55-sqwe3",
                             bad, sizeof bad) == 0);
    emu_prepared_session_t prepared;
    emu_preparation_result_t result;
    emu_qemu_image_t image = {0};
    emu_error_t error = {0};
    EMU_CHECK(emu_prepare_startup(&request, &prepared, &result, &error) ==
              EMU_OK);
    EMU_CHECK(prepared.source.bytes[0x4300c4] == 0x02);
    EMU_CHECK(emu_qemu_image_materialize(&prepared, &image, &error) == EMU_OK);
    uint8_t byte = 0;
    read_at(image.chips[0].path, 0x4300c4, &byte, 1);
    EMU_CHECK(byte == 0x03);
    EMU_CHECK(prepared.source.bytes[0x4300c4] == 0x02);
    emu_qemu_image_destroy(&image);
    emu_prepared_free(&prepared);
}

static void test_post_restore_group_order(void) {
    emu_prepared_session_t prepared;
    prepared_product(&prepared, "c55", "m58lw064d", 0);
    const uint8_t original = 0x31, first = 0x42, second = 0x53;
    add_operation(&prepared, EMU_STORAGE_STAGE_PRE_RESET, 1, 0,
                  EMU_STORAGE_MAIN_ARRAY, 0x100, &original, 1, &first, 1);
    add_operation(&prepared, EMU_STORAGE_STAGE_POST_RESTORE, 2, 0,
                  EMU_STORAGE_MAIN_ARRAY, 0x100, &first, 1, &second, 1);
    emu_qemu_image_t image = {0};
    emu_error_t error = {0};
    EMU_CHECK(emu_qemu_image_materialize(&prepared, &image, &error) == EMU_OK);
    uint8_t byte = 0;
    read_at(image.chips[0].path, 0x100, &byte, 1);
    EMU_CHECK(byte == second && prepared.source.bytes[0x100] == original);
    EMU_CHECK(image.already_applied_operations == 0);
    emu_qemu_image_destroy(&image);
    emu_prepared_free(&prepared);

    prepared_product(&prepared, "c55", "m58lw064d", 0);
    prepared.source.bytes[0x100] = second;
    rehash(&prepared);
    add_operation(&prepared, EMU_STORAGE_STAGE_POST_RESTORE, 1, 0,
                  EMU_STORAGE_MAIN_ARRAY, 0x100, &original, 1, &second, 1);
    EMU_CHECK(emu_qemu_image_materialize(&prepared, &image, &error) == EMU_OK);
    EMU_CHECK(image.already_applied_operations == 1);
    emu_qemu_image_destroy(&image);
    emu_prepared_free(&prepared);
}

static void test_partial_creation_cleanup(void) {
    size_t before = count_session_directories();
    emu_prepared_session_t prepared;
    emu_qemu_image_t image = {0};
    emu_error_t error = {0};
    prepared_product(&prepared, "s55", "w30-64mbit-top", 0);
    emu_qemu_test_fail_chip_create_after(1);
    EMU_CHECK(emu_qemu_image_materialize(
        &prepared, &image, &error) == EMU_ERR_IO);
    EMU_CHECK(!image.directory[0] && !image.chip_count);
    EMU_CHECK(count_session_directories() == before);
    emu_prepared_free(&prepared);
}

static void test_real_default_identity(void) {
    const emu_preparation_request_t request = {
        .mode = EMU_STARTUP_FRESH,
        .source_path = BUNDLED_C55,
        .requested_device = "c55",
        .identity_source = EMU_IDENTITY_SOURCE_DEFAULT,
    };
    emu_prepared_session_t prepared;
    emu_preparation_result_t result;
    emu_qemu_image_t image = {0};
    emu_error_t error = {0};
    uint8_t bytes[256];

    EMU_CHECK(emu_prepare_startup(&request, &prepared, &result, &error) ==
              EMU_OK);
    EMU_CHECK(result.identity_planned && prepared.operation_count > 0u);
    emu_error_code_t materialize = emu_qemu_image_materialize(
        &prepared, &image, &error);
    if (materialize != EMU_OK)
        fprintf(stderr, "real materialize failed: %s\n", error.message);
    EMU_CHECK(materialize == EMU_OK);
    for (size_t i = 0; i < prepared.operation_count; i++) {
        const emu_storage_operation_t *operation = &prepared.operations[i];
        if (operation->space != EMU_STORAGE_MAIN_ARRAY) continue;
        EMU_CHECK(operation->replacement_size <= sizeof bytes);
        read_at(image.chips[operation->chip_index].path,
                operation->offset, bytes, operation->replacement_size);
        EMU_CHECK(!memcmp(bytes, operation->replacement,
                          operation->replacement_size));
    }
    EMU_CHECK(image.chips[0].factory_uid_set);
    EMU_CHECK(emu_prepared_rehash_source(&prepared, &error) == EMU_OK);
    emu_qemu_image_destroy(&image);
    emu_prepared_free(&prepared);
}

int main(void) {
    test_unique_topologies();
    test_combined_read_export_and_atomic_cross_chip();
    test_identity_spaces();
    test_rejections();
    test_post_restore_patch();
    test_post_restore_group_order();
    test_partial_creation_cleanup();
    test_real_default_identity();
    puts("QEMU multi-chip image materialization: PASS");
    return 0;
}
