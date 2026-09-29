#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_support.h"
#include "bundled_firmware.h"
#include "emu_eeprom.h"

typedef enum {
    LAYOUT_C55,
    LAYOUT_M55,
    LAYOUT_S55,
} layout_t;

static void put16(uint8_t *bytes, uint16_t value) {
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
}

static void put_record(uint8_t *flash, size_t offset, uint16_t id,
                       uint16_t length, uint32_t linear, uint16_t version,
                       uint16_t marker) {
    uint8_t low = marker == 0xF000 ? 0xF0 : marker == 0xF800 ? 0xF8 : 0xFC;
    put16(flash + offset, (uint16_t)((version << 8) | low));
    put16(flash + offset + 2, length);
    put16(flash + offset + 4, (uint16_t)linear);
    put16(flash + offset + 6, (uint16_t)(linear >> 16));
    put16(flash + offset + 8, id);
    put16(flash + offset + 10, marker);
}

static uint8_t *synthetic_catalog(layout_t layout, size_t *size_out,
                                  size_t *base_out, uint32_t *linear_out) {
    size_t base = 0x10000;
    size_t size = base + EMU_EEPROM_REGION_SIZE;
    size_t name_offset = layout == LAYOUT_C55 ? 0x12 : 0x82;
    size_t first = layout == LAYOUT_C55 ? 0x20000 : 0x10000;
    size_t second = layout == LAYOUT_C55 ? 0x40000
                  : layout == LAYOUT_M55 ? 0x20000 : 0x12000;
    uint32_t linear = layout == LAYOUT_C55 ? 0xFA0000u : 0xFC0000u;
    uint8_t *flash = malloc(size);
    EMU_CHECK(flash != NULL);
    memset(flash, 0xFF, size);
    size_t magic = base + name_offset;
    flash[magic - 2] = flash[magic - 1] = 0xFE;
    memcpy(flash + magic, "EELITE", 6);
    memcpy(flash + magic + first, "EEFULL", 6);
    memcpy(flash + magic + second, "EEFULL", 6);

    /* Physical order deliberately disagrees with resolution priority. */
    put_record(flash, base + 0x200, 100, 16, linear + 0x50000, 2, 0xFC00);
    put_record(flash, base + 0x220, 100, 16, linear + 0x50100, 3, 0xF800);
    put_record(flash, base + 0x240, 200, 16, linear + 0x50200, 1, 0xF000);
    put_record(flash, base + 0x260, 200, 16, linear + 0x50300, 4, 0xFC00);
    put_record(flash, base + 0x280, 300, 16, linear + 0x01000, 5, 0xF000);
    put_record(flash, base + 0x2A0, 400, 0x1200,
               linear + 0x40000, 6, 0xFC00);
    /* Structurally marker-valid, but its payload extends past the region. */
    put_record(flash, base + 0x2C0, 500, 0x200,
               linear + EMU_EEPROM_REGION_SIZE - 0x100, 7, 0xFC00);
    *size_out = size;
    *base_out = base;
    *linear_out = linear;
    return flash;
}

static void check_layout(layout_t layout) {
    size_t size, base;
    uint32_t linear;
    uint8_t *flash = synthetic_catalog(layout, &size, &base, &linear);
    emu_eeprom_catalog_t catalog = {0};
    emu_error_t error = {0};
    EMU_CHECK(emu_eeprom_catalog_parse(
                  flash, size, &catalog, &error) == EMU_OK);
    EMU_CHECK(catalog.region.chip_offset == base);
    EMU_CHECK(catalog.region.linear_base == linear);
    EMU_CHECK(catalog.region.size == EMU_EEPROM_REGION_SIZE);
    EMU_CHECK(catalog.region.header_count == EMU_EEPROM_HEADER_COUNT);
    EMU_CHECK(catalog.record_count == 6);
    EMU_CHECK(catalog.resolved_count == 4);

    const emu_eeprom_record_t *record =
        emu_eeprom_catalog_resolve(&catalog, 100);
    EMU_CHECK(record != NULL && record->active && record->resolved);
    EMU_CHECK(record->version == 2 && record->marker == 0xFC00);
    EMU_CHECK(record->chip_offset == base + 0x50000);
    record = emu_eeprom_catalog_resolve(&catalog, 200);
    EMU_CHECK(record != NULL && record->active && record->version == 4);
    EMU_CHECK(record->chip_offset == base + 0x50300);
    record = emu_eeprom_catalog_resolve(&catalog, 300);
    EMU_CHECK(record != NULL && !record->active && record->version == 5);
    record = emu_eeprom_catalog_resolve(&catalog, 400);
    EMU_CHECK(record != NULL && record->length == 0x1200);
    EMU_CHECK(emu_eeprom_catalog_resolve(&catalog, 500) == NULL);
    EMU_CHECK(emu_eeprom_catalog_resolve(&catalog, 999) == NULL);

    emu_eeprom_match_t match;
    EMU_CHECK(emu_eeprom_catalog_classify(
                  &catalog, base - 1, &match) == EMU_EEPROM_AREA_OUTSIDE);
    EMU_CHECK(!strcmp(emu_eeprom_area_name(match.area), "outside"));
    EMU_CHECK(emu_eeprom_catalog_classify(
                  &catalog, catalog.region.header_chip_offsets[0] + 3,
                  &match) == EMU_EEPROM_AREA_HEADER);
    EMU_CHECK(match.record == NULL && match.offset == 3);
    EMU_CHECK(emu_eeprom_catalog_classify(
                  &catalog, base + 0x202,
                  &match) == EMU_EEPROM_AREA_DESCRIPTOR);
    EMU_CHECK(match.record != NULL && match.record->id == 100 &&
              match.offset == 2);
    EMU_CHECK(emu_eeprom_catalog_classify(
                  &catalog, base + 0x50005,
                  &match) == EMU_EEPROM_AREA_RESOLVED_PAYLOAD);
    EMU_CHECK(match.record != NULL && match.record->id == 100 &&
              match.offset == 5);
    EMU_CHECK(emu_eeprom_catalog_classify(
                  &catalog, base + 0x50105,
                  &match) == EMU_EEPROM_AREA_SHADOWED_PAYLOAD);
    EMU_CHECK(match.record != NULL && match.record->id == 100 &&
              match.offset == 5);
    EMU_CHECK(emu_eeprom_catalog_classify(
                  &catalog, base + 0x30000,
                  &match) == EMU_EEPROM_AREA_UNALLOCATED);
    EMU_CHECK(match.record == NULL && match.offset == 0x30000);

    emu_eeprom_location_t location;
    EMU_CHECK(emu_eeprom_directory_resolve(
                  flash, size, 200, &location, &error) == EMU_OK);
    EMU_CHECK(location.chip_offset == base + 0x50300);
    EMU_CHECK(emu_eeprom_directory_resolve(
                  flash, size, 999, &location, &error) ==
              EMU_ERR_INVALID_PREPARED_SESSION);

    emu_eeprom_catalog_free(&catalog);
    EMU_CHECK(catalog.records == NULL && catalog.record_count == 0);
    free(flash);
}

static uint8_t *read_file(const char *path, size_t *size_out) {
    FILE *file = fopen(path, "rb");
    EMU_CHECK(file != NULL);
    EMU_CHECK(fseek(file, 0, SEEK_END) == 0);
    long length = ftell(file);
    EMU_CHECK(length >= 0 && fseek(file, 0, SEEK_SET) == 0);
    uint8_t *bytes = malloc((size_t)length);
    EMU_CHECK(bytes != NULL);
    EMU_CHECK(fread(bytes, 1, (size_t)length, file) == (size_t)length);
    EMU_CHECK(fclose(file) == 0);
    *size_out = (size_t)length;
    return bytes;
}

static void check_real_catalog(const char *path, size_t file_base,
                               uint32_t linear_base, size_t records,
                               size_t resolved, uint16_t largest) {
    size_t size;
    uint8_t *flash = read_file(path, &size);
    emu_eeprom_catalog_t catalog = {0};
    emu_error_t error = {0};
    EMU_CHECK(emu_eeprom_catalog_parse(
                  flash, size, &catalog, &error) == EMU_OK);
    EMU_CHECK(catalog.region.chip_offset == file_base);
    EMU_CHECK(catalog.region.linear_base == linear_base);
    EMU_CHECK(catalog.record_count == records);
    EMU_CHECK(catalog.resolved_count == resolved);
    uint16_t maximum = 0;
    for (size_t i = 0; i < catalog.record_count; i++)
        if (catalog.records[i].length > maximum)
            maximum = catalog.records[i].length;
    EMU_CHECK(maximum == largest);
    emu_eeprom_catalog_free(&catalog);
    free(flash);
}

static void test_real_catalogs(void) {
    check_real_catalog(BUNDLED_C55,
                       0x7A0000, 0xFA0000, 305, 305, 4452);
    check_real_catalog(BUNDLED_M55,
                       0xFC0000, 0xFC0000, 383, 383, 2048);
    check_real_catalog(
        BUNDLED_S55,
        0xBE0000, 0xFE0000, 383, 383, 2048);
}

static void test_errors_are_atomic(void) {
    size_t size, base;
    uint32_t linear;
    uint8_t *flash = synthetic_catalog(LAYOUT_C55, &size, &base, &linear);
    emu_eeprom_catalog_t catalog = {0};
    emu_error_t error = {0};
    EMU_CHECK(emu_eeprom_catalog_parse(
                  flash, size, &catalog, &error) == EMU_OK);
    emu_eeprom_record_t *records = catalog.records;
    uint8_t invalid[64];
    memset(invalid, 0xFF, sizeof invalid);
    EMU_CHECK(emu_eeprom_catalog_parse(
                  invalid, sizeof invalid, &catalog, &error) ==
              EMU_ERR_INVALID_PREPARED_SESSION);
    EMU_CHECK(catalog.records == records && catalog.record_count == 6);
    EMU_CHECK(emu_eeprom_catalog_parse(
                  NULL, 0, &catalog, &error) == EMU_ERR_ARGUMENT);
    EMU_CHECK(emu_eeprom_directory_resolve(
                  flash, size, 100, NULL, &error) == EMU_ERR_ARGUMENT);
    emu_eeprom_catalog_free(&catalog);
    free(flash);
}

int main(void) {
    check_layout(LAYOUT_C55);
    check_layout(LAYOUT_M55);
    check_layout(LAYOUT_S55);
    test_real_catalogs();
    test_errors_are_atomic();
    puts("host EEPROM catalog: PASS");
    return 0;
}
