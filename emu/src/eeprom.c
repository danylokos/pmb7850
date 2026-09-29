#include "emu_eeprom.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EEPROM_NAME_OFFSET 0x12u
#define EEPROM_LINEAR_DERIVE_MAX_BLOCK_SIZE 0x600u
#define EEPROM_HEADER_SPAN 8u

static emu_error_code_t eeprom_fail(emu_error_t *error,
                                    emu_error_code_t code,
                                    const char *format, ...) {
    if (error) {
        va_list arguments;
        va_start(arguments, format);
        error->code = code;
        vsnprintf(error->message, sizeof error->message, format, arguments);
        va_end(arguments);
    }
    return code;
}

static void eeprom_ok(emu_error_t *error) {
    if (error) {
        error->code = EMU_OK;
        error->message[0] = 0;
    }
}

static uint16_t read16(const uint8_t *bytes) {
    return (uint16_t)(bytes[0] | ((uint16_t)bytes[1] << 8));
}

static int record_marker_valid(const uint8_t *record) {
    uint8_t start = (uint8_t)read16(record);
    uint16_t end = read16(record + 10);
    return (start == 0xFC || start == 0xF8 || start == 0xF0) &&
           (end == 0xFC00 || end == 0xF800 || end == 0xF000);
}

static int locate_region(const uint8_t *chip, size_t chip_size,
                         emu_eeprom_region_t *region) {
    static const uint32_t mirror_offsets[][2] = {
        {0x20000u, 0x40000u},
        {0x10000u, 0x20000u},
        {0x10000u, 0x12000u},
    };
    for (size_t magic = 2; magic + 6u <= chip_size; magic++) {
        if (memcmp(chip + magic, "EELITE", 6) ||
            chip[magic - 2u] != 0xFE || chip[magic - 1u] != 0xFE ||
            magic < EEPROM_NAME_OFFSET)
            continue;
        size_t file_base =
            (magic - EEPROM_NAME_OFFSET) & ~(size_t)0xFFFFu;
        size_t mirror_pair = sizeof mirror_offsets / sizeof mirror_offsets[0];
        for (size_t i = 0;
             i < sizeof mirror_offsets / sizeof mirror_offsets[0]; i++) {
            size_t first = magic + mirror_offsets[i][0];
            size_t second = magic + mirror_offsets[i][1];
            if (first + 6u <= chip_size && second + 6u <= chip_size &&
                !memcmp(chip + first, "EEFULL", 6) &&
                !memcmp(chip + second, "EEFULL", 6)) {
                mirror_pair = i;
                break;
            }
        }
        if (mirror_pair == sizeof mirror_offsets / sizeof mirror_offsets[0])
            continue;

        size_t size = chip_size - file_base;
        if (size > EMU_EEPROM_REGION_SIZE) size = EMU_EEPROM_REGION_SIZE;
        uint16_t minimum_segment = UINT16_MAX;
        for (size_t offset = file_base;
             offset + EMU_EEPROM_DIRECTORY_RECORD_SIZE <= file_base + size;
             offset++) {
            const uint8_t *record = chip + offset;
            uint16_t length = read16(record + 2);
            uint16_t segment = read16(record + 6);
            if (record_marker_valid(record) && length &&
                length <= EEPROM_LINEAR_DERIVE_MAX_BLOCK_SIZE &&
                segment <= 0xFF && segment < minimum_segment)
                minimum_segment = segment;
        }
        if (minimum_segment == UINT16_MAX) continue;

        *region = (emu_eeprom_region_t){
            .chip_offset = file_base,
            .linear_base = (uint32_t)minimum_segment << 16,
            .size = size,
            .header_chip_offsets = {
                magic - 2u,
                magic + mirror_offsets[mirror_pair][0] - 2u,
                magic + mirror_offsets[mirror_pair][1] - 2u,
            },
            .header_count = EMU_EEPROM_HEADER_COUNT,
        };
        return 1;
    }
    return 0;
}

static int parse_record(const uint8_t *view, size_t view_size,
                        size_t view_chip_offset,
                        const emu_eeprom_region_t *region, size_t offset,
                        emu_eeprom_record_t *record) {
    if (offset < view_chip_offset ||
        offset - view_chip_offset > view_size ||
        EMU_EEPROM_DIRECTORY_RECORD_SIZE >
            view_size - (offset - view_chip_offset))
        return 0;
    const uint8_t *bytes = view + (offset - view_chip_offset);
    uint16_t start_marker = read16(bytes);
    uint16_t length = read16(bytes + 2);
    uint16_t segment = read16(bytes + 6);
    uint16_t marker = read16(bytes + 10);
    uint16_t segment_low = (uint16_t)(region->linear_base >> 16);
    uint16_t segment_high =
        (uint16_t)((region->linear_base + region->size - 1u) >> 16);
    if (!record_marker_valid(bytes) || !length || length > region->size ||
        segment < segment_low || segment > segment_high)
        return 0;
    uint32_t linear = ((uint32_t)segment << 16) | read16(bytes + 4);
    if (linear < region->linear_base) return 0;
    uint64_t relative = (uint64_t)linear - region->linear_base;
    if (relative > region->size || length > region->size - relative)
        return 0;
    uint64_t chip_offset = (uint64_t)region->chip_offset + relative;
    if (chip_offset > region->chip_offset + region->size ||
        length > region->chip_offset + region->size - chip_offset)
        return 0;
    *record = (emu_eeprom_record_t){
        .id = read16(bytes + 8),
        .length = length,
        .linear = linear,
        .chip_offset = (size_t)chip_offset,
        .directory_chip_offset = offset,
        .version = start_marker >> 8,
        .start_marker = start_marker,
        .marker = marker,
        .active = marker == 0xFC00 || marker == 0xF800,
    };
    return 1;
}

static int append_record(emu_eeprom_catalog_t *catalog, size_t *capacity,
                         const emu_eeprom_record_t *record) {
    if (catalog->record_count == *capacity) {
        size_t next = *capacity ? *capacity * 2u : 128u;
        if (next < *capacity || next > SIZE_MAX / sizeof *catalog->records)
            return 0;
        emu_eeprom_record_t *records = realloc(
            catalog->records, next * sizeof *records);
        if (!records) return 0;
        catalog->records = records;
        *capacity = next;
    }
    catalog->records[catalog->record_count++] = *record;
    return 1;
}

static int mark_resolved(emu_eeprom_catalog_t *catalog) {
    size_t *selected = malloc(65536u * sizeof *selected);
    if (!selected) return 0;
    for (size_t i = 0; i < 65536u; i++) selected[i] = SIZE_MAX;
    for (int active = 1; active >= 0; active--) {
        for (size_t i = 0; i < catalog->record_count; i++) {
            emu_eeprom_record_t *record = &catalog->records[i];
            if (record->active != active || selected[record->id] != SIZE_MAX)
                continue;
            selected[record->id] = i;
            record->resolved = 1;
            catalog->resolved_count++;
        }
    }
    free(selected);
    return 1;
}

void emu_eeprom_catalog_free(emu_eeprom_catalog_t *catalog) {
    if (!catalog) return;
    free(catalog->records);
    *catalog = (emu_eeprom_catalog_t){0};
}

emu_error_code_t emu_eeprom_catalog_parse(
        const uint8_t *chip, size_t chip_size, emu_eeprom_catalog_t *catalog,
        emu_error_t *error) {
    if (!chip || !catalog)
        return eeprom_fail(error, EMU_ERR_ARGUMENT,
                           "invalid EEPROM catalog arguments");
    emu_eeprom_catalog_t parsed = {0};
    if (!locate_region(chip, chip_size, &parsed.region))
        return eeprom_fail(error, EMU_ERR_INVALID_PREPARED_SESSION,
                           "could not locate EELITE/EEFULL EEPROM mirrors");

    return emu_eeprom_catalog_parse_region(
        chip, chip_size, &parsed.region, catalog, error);
}

static emu_error_code_t catalog_parse_view(
    const uint8_t *view, size_t view_size, size_t view_chip_offset,
    const emu_eeprom_region_t *region, emu_eeprom_catalog_t *catalog,
    emu_error_t *error);

emu_error_code_t emu_eeprom_catalog_parse_region(
        const uint8_t *chip, size_t chip_size,
        const emu_eeprom_region_t *region, emu_eeprom_catalog_t *catalog,
        emu_error_t *error) {
    return catalog_parse_view(chip, chip_size, 0, region, catalog, error);
}

static emu_error_code_t catalog_parse_view(
        const uint8_t *view, size_t view_size, size_t view_chip_offset,
        const emu_eeprom_region_t *region, emu_eeprom_catalog_t *catalog,
        emu_error_t *error) {
    if (!view || !region || !catalog ||
        region->chip_offset < view_chip_offset ||
        region->chip_offset - view_chip_offset > view_size ||
        region->size > view_size - (region->chip_offset - view_chip_offset) ||
        !region->size || region->header_count > EMU_EEPROM_HEADER_COUNT)
        return eeprom_fail(error, EMU_ERR_ARGUMENT,
                           "invalid EEPROM catalog region");
    for (size_t i = 0; i < region->header_count; i++)
        if (region->header_chip_offsets[i] < region->chip_offset ||
            region->header_chip_offsets[i] - region->chip_offset >
                region->size ||
            8u > region->size -
                (region->header_chip_offsets[i] - region->chip_offset))
            return eeprom_fail(error, EMU_ERR_ARGUMENT,
                               "invalid EEPROM header region");

    emu_eeprom_catalog_t parsed = {.region = *region};

    size_t capacity = 0;
    size_t end = parsed.region.chip_offset + parsed.region.size;
    for (size_t offset = parsed.region.chip_offset;
         offset + EMU_EEPROM_DIRECTORY_RECORD_SIZE <= end; offset++) {
        emu_eeprom_record_t record;
        if (!parse_record(view, view_size, view_chip_offset,
                          &parsed.region, offset, &record))
            continue;
        if (!append_record(&parsed, &capacity, &record)) {
            emu_eeprom_catalog_free(&parsed);
            return eeprom_fail(error, EMU_ERR_NOMEM,
                               "out of memory parsing EEPROM directory");
        }
    }
    if (!mark_resolved(&parsed)) {
        emu_eeprom_catalog_free(&parsed);
        return eeprom_fail(error, EMU_ERR_NOMEM,
                           "out of memory resolving EEPROM directory");
    }
    emu_eeprom_catalog_free(catalog);
    *catalog = parsed;
    eeprom_ok(error);
    return EMU_OK;
}

emu_error_code_t emu_eeprom_catalog_parse_cached_region(
        const uint8_t *bytes, size_t size,
        const emu_eeprom_region_t *region, emu_eeprom_catalog_t *catalog,
        emu_error_t *error) {
    if (!region)
        return eeprom_fail(error, EMU_ERR_ARGUMENT,
                           "invalid EEPROM catalog region");
    return catalog_parse_view(bytes, size, region->chip_offset,
                              region, catalog, error);
}

const emu_eeprom_record_t *emu_eeprom_catalog_resolve(
        const emu_eeprom_catalog_t *catalog, uint16_t block_id) {
    if (!catalog) return NULL;
    for (size_t i = 0; i < catalog->record_count; i++) {
        const emu_eeprom_record_t *record = &catalog->records[i];
        if (record->resolved && record->id == block_id) return record;
    }
    return NULL;
}

static int contains(size_t start, size_t length, size_t offset) {
    return offset >= start && offset - start < length;
}

emu_eeprom_area_t emu_eeprom_catalog_classify(
        const emu_eeprom_catalog_t *catalog, size_t chip_offset,
        emu_eeprom_match_t *match) {
    emu_eeprom_match_t found = {.area = EMU_EEPROM_AREA_OUTSIDE};
    if (!catalog || !contains(catalog->region.chip_offset,
                              catalog->region.size, chip_offset)) {
        if (match) *match = found;
        return found.area;
    }
    found.area = EMU_EEPROM_AREA_UNALLOCATED;
    found.offset = chip_offset - catalog->region.chip_offset;
    for (size_t i = 0; i < catalog->region.header_count; i++) {
        size_t header = catalog->region.header_chip_offsets[i];
        if (!contains(header, EEPROM_HEADER_SPAN, chip_offset)) continue;
        found.area = EMU_EEPROM_AREA_HEADER;
        found.offset = chip_offset - header;
        if (match) *match = found;
        return found.area;
    }
    for (size_t i = 0; i < catalog->record_count; i++) {
        const emu_eeprom_record_t *record = &catalog->records[i];
        if (!contains(record->directory_chip_offset,
                      EMU_EEPROM_DIRECTORY_RECORD_SIZE, chip_offset))
            continue;
        found.area = EMU_EEPROM_AREA_DESCRIPTOR;
        found.record = record;
        found.offset = chip_offset - record->directory_chip_offset;
        if (match) *match = found;
        return found.area;
    }
    for (int resolved = 1; resolved >= 0; resolved--) {
        for (size_t i = 0; i < catalog->record_count; i++) {
            const emu_eeprom_record_t *record = &catalog->records[i];
            if (record->resolved != resolved ||
                !contains(record->chip_offset, record->length, chip_offset))
                continue;
            found.area = resolved ? EMU_EEPROM_AREA_RESOLVED_PAYLOAD
                                  : EMU_EEPROM_AREA_SHADOWED_PAYLOAD;
            found.record = record;
            found.offset = chip_offset - record->chip_offset;
            if (match) *match = found;
            return found.area;
        }
    }
    if (match) *match = found;
    return found.area;
}

const char *emu_eeprom_area_name(emu_eeprom_area_t area) {
    switch (area) {
        case EMU_EEPROM_AREA_OUTSIDE: return "outside";
        case EMU_EEPROM_AREA_HEADER: return "header";
        case EMU_EEPROM_AREA_DESCRIPTOR: return "descriptor";
        case EMU_EEPROM_AREA_RESOLVED_PAYLOAD: return "resolved-payload";
        case EMU_EEPROM_AREA_SHADOWED_PAYLOAD: return "shadowed-payload";
        case EMU_EEPROM_AREA_UNALLOCATED: return "unallocated";
    }
    return "unknown";
}

emu_error_code_t emu_eeprom_directory_resolve(
        const uint8_t *chip, size_t chip_size, uint16_t block_id,
        emu_eeprom_location_t *location, emu_error_t *error) {
    if (!location)
        return eeprom_fail(error, EMU_ERR_ARGUMENT,
                           "invalid EEPROM directory arguments");
    emu_eeprom_catalog_t catalog = {0};
    emu_error_code_t code = emu_eeprom_catalog_parse(
        chip, chip_size, &catalog, error);
    if (code != EMU_OK) return code;
    const emu_eeprom_record_t *record =
        emu_eeprom_catalog_resolve(&catalog, block_id);
    if (!record) {
        emu_eeprom_catalog_free(&catalog);
        return eeprom_fail(error, EMU_ERR_INVALID_PREPARED_SESSION,
                           "EEPROM block %u is missing", block_id);
    }
    *location = *record;
    emu_eeprom_catalog_free(&catalog);
    eeprom_ok(error);
    return EMU_OK;
}
