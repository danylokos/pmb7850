#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_support.h"
#include "bundled_firmware.h"
#include "emu_identity.h"
#include "emu_product.h"

static void put16(uint8_t *bytes, uint16_t value) {
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
}

static void put_record(uint8_t *flash, size_t offset, uint16_t id,
                       uint16_t length, uint32_t linear) {
    put16(flash + offset, 0x00FC);
    put16(flash + offset + 2, length);
    put16(flash + offset + 4, (uint16_t)linear);
    put16(flash + offset + 6, (uint16_t)(linear >> 16));
    put16(flash + offset + 8, id);
    put16(flash + offset + 10, 0xFC00);
}

static uint8_t *synthetic_flash(size_t *size_out) {
    static const uint16_t ids[] = {76, 5008, 5009, 5077};
    static const uint16_t sizes[] = {10, 224, 10, 232};
    static const uint32_t offsets[] = {0x50000, 0x50100, 0x50200, 0x50300};
    size_t size = 0x60000;
    uint8_t *flash = malloc(size);
    EMU_CHECK(flash != NULL);
    memset(flash, 0xFF, size);
    flash[0x10] = flash[0x11] = 0xFE;
    memcpy(flash + 0x12, "EELITE", 6);
    memcpy(flash + 0x20012, "EEFULL", 6);
    memcpy(flash + 0x40012, "EEFULL", 6);
    for (size_t i = 0; i < 4; i++) {
        put_record(flash, 0x100 + 12u * i, ids[i], sizes[i],
                   0xFA0000u + offsets[i]);
        memset(flash + offsets[i], (int)(0x10u + i), sizes[i]);
    }
    put_record(flash, 0x140, 76, 10, 0xFA5400);
    *size_out = size;
    return flash;
}

static void prepared_from_bytes(emu_prepared_session_t *prepared,
                                uint8_t *bytes, size_t size,
                                const char *model,
                                emu_identity_kind_t kind) {
    emu_prepared_init(prepared);
    prepared->source.locator = malloc(17);
    EMU_CHECK(prepared->source.locator != NULL);
    memcpy(prepared->source.locator, "memory:identity", 16);
    prepared->source.bytes = bytes;
    prepared->source.size = size;
    emu_sha256(bytes, size, prepared->source.sha256);
    emu_sha256_hex(prepared->source.sha256,
                   prepared->source.sha256_hex);
    snprintf(prepared->selected_device,
             sizeof prepared->selected_device, "synthetic");
    snprintf(prepared->metadata.model,
             sizeof prepared->metadata.model, "SYNTH");
    prepared->chip_count = 1;
    prepared->identity_kind = kind;
    prepared->identity_chip_index = 0;
    snprintf(prepared->chips[0].role,
             sizeof prepared->chips[0].role, "flash");
    snprintf(prepared->chips[0].model,
             sizeof prepared->chips[0].model, "%s", model);
    prepared->chips[0].size = size;
    emu_sha256(bytes, size, prepared->chips[0].sha256);
    emu_sha256_hex(prepared->chips[0].sha256,
                   prepared->chips[0].sha256_hex);
}

static char *read_text(const char *path, size_t *size_out) {
    FILE *file = fopen(path, "rb");
    EMU_CHECK(file != NULL);
    EMU_CHECK(fseek(file, 0, SEEK_END) == 0);
    long length = ftell(file);
    EMU_CHECK(length >= 0);
    EMU_CHECK(fseek(file, 0, SEEK_SET) == 0);
    char *text = malloc((size_t)length + 1u);
    EMU_CHECK(text != NULL);
    EMU_CHECK(fread(text, 1, (size_t)length, file) == (size_t)length);
    EMU_CHECK(fclose(file) == 0);
    text[length] = 0;
    *size_out = (size_t)length;
    return text;
}

static void test_bundle_validation(void) {
    emu_error_t error = {0};
    emu_identity_bundle_t bundle;
    EMU_CHECK(emu_identity_bundle_load("../cemu/eeprom.json", &bundle,
                                       &error) == EMU_OK);
    EMU_CHECK(bundle.fsn == 0x1234ABCDu);
    EMU_CHECK(!strcmp(bundle.imei, "11223344556677"));
    EMU_CHECK(bundle.blocks[3].id == 5077);
    EMU_CHECK(bundle.blocks[3].length == 232);
    EMU_CHECK(bundle.blocks[4].id == 67);

    const emu_identity_bundle_t *built_in = emu_identity_default_bundle();
    EMU_CHECK(built_in->fsn == bundle.fsn);
    EMU_CHECK(!strcmp(built_in->imei, bundle.imei));
    for (size_t i = 0; i < 4; i++) {
        EMU_CHECK(built_in->blocks[i].id == bundle.blocks[i].id);
        EMU_CHECK(built_in->blocks[i].length == bundle.blocks[i].length);
        EMU_CHECK(!memcmp(built_in->blocks[i].bytes, bundle.blocks[i].bytes,
                          bundle.blocks[i].length));
    }
    EMU_CHECK(built_in->blocks[4].id == 0);

    size_t size = 0;
    char *document = read_text("../cemu/eeprom.json", &size);
    char *field = strstr(document, "cemu.eeprom-identity-overlay");
    EMU_CHECK(field != NULL);
    field[0] = 'X';
    EMU_CHECK(emu_identity_bundle_parse(document, size, &bundle, &error) ==
              EMU_ERR_INVALID_PREPARED_SESSION);
    field[0] = 'c';
    field = strstr(document, "1234ABCD");
    EMU_CHECK(field != NULL);
    field[0] = 'G';
    EMU_CHECK(emu_identity_bundle_parse(document, size, &bundle, &error) ==
              EMU_ERR_INVALID_PREPARED_SESSION);
    field[0] = '1';
    field = strstr(document, "\"5077\"");
    EMU_CHECK(field != NULL);
    memcpy(field + 1, "9999", 4);
    EMU_CHECK(emu_identity_bundle_parse(document, size, &bundle, &error) ==
              EMU_ERR_INVALID_PREPARED_SESSION);
    free(document);
}

static void test_identity_vectors(void) {
    static const uint8_t zero[8] = {0};
    static const uint8_t ones[8] = {
        0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
    };
    static const uint8_t pattern[8] = {
        0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77,
    };
    static const uint8_t expected[8] = {
        0x00,0x00,0x00,0x00,0x41,0x71,0x35,0xCF,
    };
    uint8_t identity[8];
    EMU_CHECK(emu_identity_fsn_from_am29_secsi(
                  zero, 0x0001, 0x227E) == 0x9F880100u);
    EMU_CHECK(emu_identity_fsn_from_am29_secsi(
                  ones, 0x0001, 0x227E) == 0x9F880100u);
    EMU_CHECK(emu_identity_fsn_from_am29_secsi(
                  pattern, 0x0001, 0x227E) == 0x8A9D8BB9u);
    emu_identity_am29_secsi_from_fsn(
        0xA35F2F28u, 0x0001, 0x227E, identity);
    EMU_CHECK(!memcmp(identity, expected, sizeof expected));
    EMU_CHECK(emu_identity_fsn_from_am29_secsi(
                  identity, 0x0001, 0x227E) == 0xA35F2F28u);

    emu_identity_factory_uid_from_fsn(
        0x1234ABCDu, 0x0089, 0x8856, identity);
    EMU_CHECK(emu_identity_fsn_from_factory_uid(
                  identity, 0x0089, 0x8856) == 0x1234ABCDu);
    static const char imei[15] = "35335000894548";
    static const uint8_t expected_mirror[8] = {
        0x53,0x33,0x05,0x00,0x98,0x54,0x84,0x90,
    };
    emu_identity_imei_mirror(imei, identity);
    EMU_CHECK(!memcmp(identity, expected_mirror, sizeof expected_mirror));
}

static void test_directory_and_plans(void) {
    size_t size = 0;
    uint8_t *flash = synthetic_flash(&size);
    emu_eeprom_location_t location;
    emu_error_t error = {0};
    EMU_CHECK(emu_eeprom_directory_resolve(
                  flash, size, 76, &location, &error) == EMU_OK);
    EMU_CHECK(location.chip_offset == 0x50000);
    EMU_CHECK(location.linear == 0xFF0000);

    emu_prepared_session_t prepared;
    prepared_from_bytes(&prepared, flash, size, "m58lw064d",
                        EMU_IDENTITY_FACTORY_UID);
    emu_identity_plan_result_t result;
    EMU_CHECK(emu_identity_plan_bundle(
                  &prepared, emu_identity_default_bundle(), "test-bundle",
                  0, &result, &error) == EMU_OK);
    EMU_CHECK(prepared.operation_count == 5);
    static const size_t offsets[] = {0x50000,0x50100,0x50200,0x50300};
    for (size_t i = 0; i < 4; i++) {
        const emu_storage_operation_t *operation = &prepared.operations[i];
        EMU_CHECK(operation->order == i);
        EMU_CHECK(operation->space == EMU_STORAGE_MAIN_ARRAY);
        EMU_CHECK(operation->offset == offsets[i]);
        EMU_CHECK(operation->expected[0] == (uint8_t)(0x10u + i));
        EMU_CHECK(operation->replacement_size ==
                  emu_identity_default_bundle()->blocks[i].length);
    }
    EMU_CHECK(prepared.operations[4].space == EMU_STORAGE_FACTORY_UID);
    EMU_CHECK(result.record_count == 4 && !result.fsn_only_fallback);
    EMU_CHECK(emu_prepared_rehash_source(&prepared, &error) == EMU_OK);
    emu_prepared_free(&prepared);

    flash = synthetic_flash(&size);
    prepared_from_bytes(&prepared, flash, size, "am29lv128mh",
                        EMU_IDENTITY_AM29_SECSI);
    EMU_CHECK(emu_identity_plan_bundle(
                  &prepared, emu_identity_default_bundle(), "test-am29",
                  0, &result, &error) == EMU_OK);
    EMU_CHECK(prepared.operation_count == 6);
    EMU_CHECK(prepared.operations[4].space ==
              EMU_STORAGE_AM29_FACTORY_SECSI);
    EMU_CHECK(prepared.operations[5].space ==
              EMU_STORAGE_AM29_CUSTOMER_SECSI);
    EMU_CHECK(prepared.operations[5].offset == 0x10);
    emu_prepared_free(&prepared);

    flash = synthetic_flash(&size);
    prepared_from_bytes(&prepared, flash, size, "am29lv128mh",
                        EMU_IDENTITY_AM29_SECSI);
    EMU_CHECK(emu_identity_plan_fsn_imei(
                  &prepared, 0xC8AAE55Fu, "35202600729559",
                  "native-am29", &result, &error) == EMU_OK);
    EMU_CHECK(prepared.operation_count == 2);
    EMU_CHECK(prepared.operations[0].space == EMU_STORAGE_AM29_FACTORY_SECSI);
    EMU_CHECK(prepared.operations[1].space == EMU_STORAGE_AM29_CUSTOMER_SECSI);
    EMU_CHECK(prepared.operations[1].offset == 0x10);
    EMU_CHECK(!strcmp(result.imei, "35202600729559"));
    emu_prepared_free(&prepared);

    flash = synthetic_flash(&size);
    prepared_from_bytes(&prepared, flash, size, "m58lw064d",
                        EMU_IDENTITY_FACTORY_UID);
    EMU_CHECK(emu_identity_plan_fsn_imei(
                  &prepared, 0xA35F2F28u, "35335000894548",
                  "invalid-m58", &result, &error) ==
              EMU_ERR_INVALID_PREPARED_SESSION);
    EMU_CHECK(prepared.operation_count == 0);
    emu_prepared_free(&prepared);

    flash = malloc(0x1000);
    EMU_CHECK(flash != NULL);
    memset(flash, 0xFF, 0x1000);
    prepared_from_bytes(&prepared, flash, 0x1000, "m58lw064d",
                        EMU_IDENTITY_FACTORY_UID);
    EMU_CHECK(emu_identity_plan_bundle(
                  &prepared, emu_identity_default_bundle(), "explicit",
                  0, &result, &error) == EMU_ERR_INVALID_PREPARED_SESSION);
    EMU_CHECK(prepared.operation_count == 0);
    EMU_CHECK(emu_identity_plan_default(&prepared, &result, &error) == EMU_OK);
    EMU_CHECK(prepared.operation_count == 1);
    EMU_CHECK(result.fsn_only_fallback && result.record_count == 0);
    emu_prepared_free(&prepared);

    flash = malloc(0x1000);
    EMU_CHECK(flash != NULL);
    memset(flash, 0xFF, 0x1000);
    prepared_from_bytes(&prepared, flash, 0x1000, "am29lv128mh",
                        EMU_IDENTITY_AM29_SECSI);
    EMU_CHECK(emu_identity_plan_fsn(&prepared, 0xA35F2F28,
                                    "explicit-fsn", &result,
                                    &error) == EMU_OK);
    EMU_CHECK(prepared.operation_count == 1);
    EMU_CHECK(prepared.operations[0].space ==
              EMU_STORAGE_AM29_FACTORY_SECSI);
    emu_prepared_free(&prepared);
}

static void test_real_image_targets(void) {
    static const struct {
        const char *device;
        const char *path;
        size_t offsets[4];
    } fixtures[] = {
        {"a52", BUNDLED_A52, {0x3a1bc3,0x3c01ae,0x3c028e,0x3c222e}},
        {"a55", BUNDLED_A55, {0x7a1bc3,0x7c01ae,0x7c028e,0x7c24dc}},
        {"a60", BUNDLED_A60, {0x7c0ee4,0x7d0192,0x7d0272,0x7d1fcc}},
        {"a62", BUNDLED_A62, {0x7c0ee4,0x7d0192,0x7d0272,0x7d1fcc}},
        {"a65", BUNDLED_A65, {0x7c17b7,0x7d0192,0x7d0272,0x7d1fcc}},
        {"c55", BUNDLED_C55, {0x7a1b7b,0x7c01ae,0x7c028e,0x7c24f4}},
        {"c60", BUNDLED_C60, {0x7c0ee4,0x7d0192,0x7d0272,0x7d1fcc}},
        {"cf62", BUNDLED_CF62, {0xfc0ee4,0xfd0192,0xfd0272,0xfd1306}},
        {"m55", BUNDLED_M55, {0xfc0ee4,0xfd0192,0xfd0272,0xfd1304}},
        {"mc60", BUNDLED_MC60, {0xfc0ee4,0xfd0192,0xfd0272,0xfd1fca}},
        {"s55", BUNDLED_S55, {0xbe0e7a,0xbf0192,0xbf0272,0xbf135a}},
        {"sl55", BUNDLED_SL55, {0xbe0ee4,0xbf018c,0xbf026c,0xbf1358}},
    };
    emu_identity_bundle_t bundle;
    emu_error_t error = {0};
    EMU_CHECK(emu_identity_bundle_load("../cemu/eeprom.json", &bundle,
                                       &error) == EMU_OK);
    size_t exercised = 0;
    for (size_t i = 0; i < sizeof fixtures / sizeof fixtures[0]; i++) {
        FILE *probe = fopen(fixtures[i].path, "rb");
        EMU_CHECK(probe != NULL);
        fclose(probe);
        emu_prepared_session_t prepared;
        emu_prepared_init(&prepared);
        EMU_CHECK(emu_prepared_load_source(
                      &prepared, fixtures[i].path, &error) == EMU_OK);
        EMU_CHECK(emu_product_prepare_image(
                      &prepared, fixtures[i].device, &error) == EMU_OK);
        emu_identity_plan_result_t result;
        EMU_CHECK(emu_identity_plan_bundle(
                      &prepared, &bundle, "real-image", 0,
                      &result, &error) == EMU_OK);
        EMU_CHECK(result.record_count == 5);
        for (size_t block = 0; block < 4; block++) {
            const emu_storage_operation_t *operation =
                &prepared.operations[block];
            EMU_CHECK(operation->stage == EMU_STORAGE_STAGE_PRE_RESET);
            EMU_CHECK(operation->group == 1);
            size_t absolute =
                prepared.chips[operation->chip_index].source_offset +
                operation->offset;
            EMU_CHECK(absolute == fixtures[i].offsets[block]);
        }
        EMU_CHECK(emu_prepared_rehash_source(&prepared, &error) == EMU_OK);
        emu_prepared_free(&prepared);
        exercised++;
    }
    EMU_CHECK(exercised == sizeof fixtures / sizeof fixtures[0]);
}

int main(void) {
    test_bundle_validation();
    test_identity_vectors();
    test_directory_and_plans();
    test_real_image_targets();
    puts("host EEPROM identity planning: PASS");
    return 0;
}
