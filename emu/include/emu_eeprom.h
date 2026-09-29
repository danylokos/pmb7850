#ifndef EMU_EEPROM_H
#define EMU_EEPROM_H

#include <stddef.h>
#include <stdint.h>

#include "emu_engine.h"

#define EMU_EEPROM_REGION_SIZE 0x60000u
#define EMU_EEPROM_DIRECTORY_RECORD_SIZE 12u
#define EMU_EEPROM_HEADER_COUNT 3u

typedef struct {
    size_t chip_offset;
    uint32_t linear_base;
    size_t size;
    size_t header_chip_offsets[EMU_EEPROM_HEADER_COUNT];
    size_t header_count;
} emu_eeprom_region_t;

typedef struct {
    uint16_t id;
    uint16_t length;
    uint32_t linear;
    size_t chip_offset;
    size_t directory_chip_offset;
    uint16_t version;
    uint16_t start_marker;
    uint16_t marker;
    int active;
    int resolved;
} emu_eeprom_record_t;

/* Kept as the identity API's location type for source compatibility. */
typedef emu_eeprom_record_t emu_eeprom_location_t;

typedef struct {
    emu_eeprom_region_t region;
    emu_eeprom_record_t *records;
    size_t record_count;
    size_t resolved_count;
} emu_eeprom_catalog_t;

typedef enum {
    EMU_EEPROM_AREA_OUTSIDE = 0,
    EMU_EEPROM_AREA_HEADER,
    EMU_EEPROM_AREA_DESCRIPTOR,
    EMU_EEPROM_AREA_RESOLVED_PAYLOAD,
    EMU_EEPROM_AREA_SHADOWED_PAYLOAD,
    EMU_EEPROM_AREA_UNALLOCATED,
} emu_eeprom_area_t;

typedef struct {
    emu_eeprom_area_t area;
    const emu_eeprom_record_t *record;
    size_t offset;
} emu_eeprom_match_t;

/* Parse one physical flash-chip image. Initialize catalog to zero before the
 * first call. A successful call atomically replaces any previous contents. */
emu_error_code_t emu_eeprom_catalog_parse(
    const uint8_t *chip, size_t chip_size, emu_eeprom_catalog_t *catalog,
    emu_error_t *error);
/* Reparse using already established physical geometry. This remains useful
 * after a guest erase destroys one or more identifying mirror headers. */
emu_error_code_t emu_eeprom_catalog_parse_region(
    const uint8_t *chip, size_t chip_size,
    const emu_eeprom_region_t *region, emu_eeprom_catalog_t *catalog,
    emu_error_t *error);
/* Reparse a cache whose first byte corresponds to region->chip_offset. */
emu_error_code_t emu_eeprom_catalog_parse_cached_region(
    const uint8_t *bytes, size_t size,
    const emu_eeprom_region_t *region, emu_eeprom_catalog_t *catalog,
    emu_error_t *error);
void emu_eeprom_catalog_free(emu_eeprom_catalog_t *catalog);

/* Return the firmware-visible record selected by active-before-fallback and
 * first-physical-record priority, or NULL when the ID is absent. */
const emu_eeprom_record_t *emu_eeprom_catalog_resolve(
    const emu_eeprom_catalog_t *catalog, uint16_t block_id);

/* Classify one chip-relative byte. Descriptor and payload matches identify
 * their physical record and set offset relative to that descriptor/payload. */
emu_eeprom_area_t emu_eeprom_catalog_classify(
    const emu_eeprom_catalog_t *catalog, size_t chip_offset,
    emu_eeprom_match_t *match);
const char *emu_eeprom_area_name(emu_eeprom_area_t area);

/* Convenience resolver retained for callers that need only one ID. */
emu_error_code_t emu_eeprom_directory_resolve(
    const uint8_t *chip, size_t chip_size, uint16_t block_id,
    emu_eeprom_location_t *location, emu_error_t *error);

#endif
