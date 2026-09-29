#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_support.h"
#include "emu_eeprom.h"
#include "emu_eeprom_trace.h"
#include "emu_trace_schema.h"

typedef struct {
    uint8_t *bytes[3];
    size_t sizes[3];
    int fail_copy;
} images_t;

typedef struct {
    char kind[24];
    char access[16];
    char area[32];
    char attribution[32];
    char change[16];
    long block_id;
    int block_id_null;
    long chip_index;
    long chip_offset;
    long generation;
    long mutation_offset;
    long mutation_size;
    long tick;
    int has_addr, has_size, has_value, size;
    uint32_t addr, value;
} captured_t;

typedef struct {
    captured_t events[256];
    size_t count;
} event_log_t;

static void put16(uint8_t *bytes, uint16_t value) {
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
}

static void put_record(uint8_t *flash, size_t offset, uint16_t id,
                       uint16_t length, uint32_t linear, uint16_t version,
                       uint16_t marker) {
    uint8_t low = marker == 0xF000 ? 0xF0
                : marker == 0xF800 ? 0xF8 : 0xFC;
    put16(flash + offset, (uint16_t)((version << 8) | low));
    put16(flash + offset + 2, length);
    put16(flash + offset + 4, (uint16_t)linear);
    put16(flash + offset + 6, (uint16_t)(linear >> 16));
    put16(flash + offset + 8, id);
    put16(flash + offset + 10, marker);
}

static uint8_t *make_image(size_t *size_out, size_t base, uint32_t linear) {
    size_t size = base + EMU_EEPROM_REGION_SIZE;
    uint8_t *flash = malloc(size);
    EMU_CHECK(flash != NULL);
    memset(flash, 0xFF, size);
    size_t magic = base + 0x12;
    flash[magic - 2] = flash[magic - 1] = 0xFE;
    memcpy(flash + magic, "EELITE", 6);
    memcpy(flash + magic + 0x20000, "EEFULL", 6);
    memcpy(flash + magic + 0x40000, "EEFULL", 6);
    put_record(flash, base + 0x200, 20, 16,
               linear + 0x50000, 2, 0xFC00);
    put_record(flash, base + 0x220, 20, 16,
               linear + 0x50100, 3, 0xF800);
    put_record(flash, base + 0x240, 30, 16,
               linear + 0x01000, 4, 0xF000);
    *size_out = size;
    return flash;
}

static int copy_image(void *opaque, int chip_index, size_t offset,
                      uint8_t *bytes, size_t size) {
    images_t *images = opaque;
    if (images->fail_copy || chip_index < 0 || chip_index >= 3 ||
        offset > images->sizes[chip_index] ||
        size > images->sizes[chip_index] - offset)
        return -1;
    memcpy(bytes, images->bytes[chip_index] + offset, size);
    return 0;
}

static const emu_trace_field_t *field(
        const emu_trace_event_t *event, const char *key) {
    for (int i = 0; i < event->info.n; i++)
        if (!strcmp(event->info.kv[i].key, key)) return &event->info.kv[i];
    return NULL;
}

static void capture_string(char *out, size_t size,
                           const emu_trace_event_t *event, const char *key) {
    const emu_trace_field_t *value = field(event, key);
    if (value && value->kind == EMU_TRACE_VALUE_STRING)
        snprintf(out, size, "%s", value->sval);
}

static long capture_int(const emu_trace_event_t *event, const char *key,
                        int *is_null) {
    const emu_trace_field_t *value = field(event, key);
    if (is_null) *is_null = value && value->kind == EMU_TRACE_VALUE_NULL;
    return value && value->kind == EMU_TRACE_VALUE_I64 ? value->ival : -1;
}

static void capture_event(void *opaque, const emu_trace_event_t *event,
                          uint64_t sequence) {
    event_log_t *log = opaque;
    (void)sequence;
    EMU_CHECK(log->count < sizeof log->events / sizeof log->events[0]);
    char error[256];
    int valid = emu_trace_schema_validate_event(event, error, sizeof error);
    if (valid) fprintf(stderr, "schema: %s\n", error);
    EMU_CHECK(valid == 0);
    if (log->count >= sizeof log->events / sizeof log->events[0]) return;
    captured_t *item = &log->events[log->count++];
    memset(item, 0, sizeof *item);
    snprintf(item->kind, sizeof item->kind, "%s", event->kind);
    capture_string(item->access, sizeof item->access, event, "access");
    capture_string(item->area, sizeof item->area, event, "area");
    capture_string(item->attribution, sizeof item->attribution,
                   event, "attribution");
    capture_string(item->change, sizeof item->change, event, "change");
    item->block_id = capture_int(event, "block_id", &item->block_id_null);
    item->chip_index = capture_int(event, "chip_index", NULL);
    item->chip_offset = capture_int(event, "chip_offset", NULL);
    item->generation = capture_int(event, "mapping_generation", NULL);
    item->mutation_offset = capture_int(event, "mutation_offset", NULL);
    item->mutation_size = capture_int(event, "mutation_size", NULL);
    item->tick = capture_int(event, "tick", NULL);
    item->has_addr = event->has_addr;
    item->has_size = event->has_size;
    item->has_value = event->has_value;
    item->size = event->size;
    item->addr = event->addr;
    item->value = event->value;
}

static size_t find_after(const event_log_t *log, size_t start,
                         const char *kind, const char *value) {
    for (size_t i = start; i < log->count; i++) {
        const char *actual = !strcmp(kind, "map")
                           ? log->events[i].change
                           : log->events[i].attribution;
        if (!strcmp(actual, value)) return i;
    }
    return SIZE_MAX;
}

static void test_mapping_and_access(void) {
    const size_t base0 = 0x10000, base1 = 0x20000;
    images_t images = {0};
    images.bytes[0] = make_image(&images.sizes[0], base0, 0xFA0000);
    images.bytes[1] = make_image(&images.sizes[1], base1, 0xF90000);
    images.sizes[2] = 0x1000;
    images.bytes[2] = malloc(images.sizes[2]);
    EMU_CHECK(images.bytes[2] != NULL);
    memset(images.bytes[2], 0xFF, images.sizes[2]);
    emu_eeprom_trace_chip_t chips[] = {
        {0, "flash", "m58lw064d", images.sizes[0]},
        {1, "flash1", "am29lv640mh", images.sizes[1]},
        {2, "flash2", "w30", images.sizes[2]},
    };
    event_log_t log = {0};
    emu_trace_sink_t *sink = emu_trace_open_callback(
        EMU_TRACE_EEPROM, capture_event, &log);
    EMU_CHECK(sink != NULL);
    emu_eeprom_trace_t *trace = NULL;
    EMU_CHECK(emu_eeprom_trace_create(
                  &trace, sink, chips, 3, copy_image, &images) == 0);

    /* Deferred activity is absent; arming snapshots live bytes. */
    emu_eeprom_trace_read(trace, 0, (uint32_t)(base0 + 0x50000),
                          0xFA0000, 0xAA, 1, 1, 2, 3);
    EMU_CHECK(log.count == 0);
    EMU_CHECK(emu_eeprom_trace_arm(trace, 7, 11, 0x123456) == 0);
    EMU_CHECK(log.count == 4);
    EMU_CHECK(log.events[0].block_id == 20 &&
              log.events[1].block_id == 30 &&
              log.events[2].block_id == 20 &&
              log.events[3].block_id == 30);
    EMU_CHECK(log.events[0].chip_index == 0 &&
              log.events[2].chip_index == 1);
    for (size_t i = 0; i < 4; i++) {
        EMU_CHECK(!strcmp(log.events[i].change, "initial"));
        EMU_CHECK(log.events[i].generation == 0 && log.events[i].tick == 7);
    }

    size_t first = log.count;
    emu_eeprom_trace_read(trace, 0, (uint32_t)(base0 + 0x50004),
                          0xFA0004, 0xBBAA, 2, 9, 12, 0xABCDEF);
    emu_eeprom_trace_read(trace, 0, (uint32_t)(base0 + 0x50103),
                          0xFA0103, 0x44, 1, 10, 13, 0xABCDE0);
    emu_eeprom_trace_read(trace, 0, (uint32_t)(base0 + 0x202),
                          0xFA0202, 0x55, 1, 11, 14, 0xABCDE1);
    emu_eeprom_trace_read(trace, 0, (uint32_t)(base0 + 0x13),
                          0xFA0013, 0x66, 1, 12, 15, 0xABCDE2);
    emu_eeprom_trace_read(trace, 0, (uint32_t)(base0 + 0x30000),
                          0xFD0000, 0x77, 1, 13, 16, 0xABCDE3);
    EMU_CHECK(log.count == first + 5);
    captured_t *resolved = &log.events[first];
    EMU_CHECK(!strcmp(resolved->attribution, "resolved") &&
              resolved->block_id == 20 && resolved->size == 2);
    EMU_CHECK(resolved->has_addr && resolved->has_value &&
              resolved->addr == 0xFA0004 && resolved->value == 0xBBAA);
    EMU_CHECK(!strcmp(log.events[first + 1].attribution, "shadowed"));
    EMU_CHECK(!strcmp(log.events[first + 2].attribution, "metadata") &&
              log.events[first + 2].block_id == 20);
    EMU_CHECK(!strcmp(log.events[first + 3].area, "header") &&
              log.events[first + 3].block_id_null);
    EMU_CHECK(!strcmp(log.events[first + 4].attribution, "unattributed"));

    /* One original mutation splits at both descriptor boundaries. */
    first = log.count;
    emu_eeprom_trace_mutation(
        trace, 0, EMU_EEPROM_MUTATION_PROGRAM,
        (uint32_t)(base0 + 0x1FF), 14, 20, 21, 0x100000);
    EMU_CHECK(log.count == first + 3);
    EMU_CHECK(!strcmp(log.events[first].attribution,
                      "unattributed-journal"));
    EMU_CHECK(!strcmp(log.events[first + 1].area, "descriptor") &&
              log.events[first + 1].size == 12);
    EMU_CHECK(log.events[first + 2].mutation_offset ==
              (long)(base0 + 0x1FF) &&
              log.events[first + 2].mutation_size == 14);
    EMU_CHECK(!log.events[first].has_addr && !log.events[first].has_value);

    /* A descriptor-completing journal program is attributed from the old
     * catalog, then publishes an ordered generation-one add. */
    first = log.count;
    put_record(images.bytes[0], base0 + 0x260, 10, 16,
               0xFA0000 + 0x50300, 5, 0xFC00);
    emu_eeprom_trace_mutation(
        trace, 0, EMU_EEPROM_MUTATION_PROGRAM,
        (uint32_t)(base0 + 0x260), 12, 30, 31, 0x200000);
    EMU_CHECK(log.count == first + 2);
    EMU_CHECK(!strcmp(log.events[first].attribution,
                      "unattributed-journal"));
    EMU_CHECK(!strcmp(log.events[first + 1].change, "added") &&
              log.events[first + 1].block_id == 10 &&
              log.events[first + 1].generation == 1);

    /* Removing the selected descriptor remaps to its shadowed peer. */
    first = log.count;
    memset(images.bytes[0] + base0 + 0x200, 0xFF, 12);
    emu_eeprom_trace_mutation(
        trace, 0, EMU_EEPROM_MUTATION_ERASE,
        (uint32_t)(base0 + 0x200), 12, 40, 41, 0x300000);
    size_t remap = find_after(&log, first, "map", "remapped");
    EMU_CHECK(remap == first + 1 && log.events[remap].block_id == 20 &&
              log.events[remap].generation == 2);

    /* The only physical record for 30 disappears. */
    first = log.count;
    memset(images.bytes[0] + base0 + 0x240, 0xFF, 12);
    emu_eeprom_trace_mutation(
        trace, 0, EMU_EEPROM_MUTATION_ERASE,
        (uint32_t)(base0 + 0x240), 12, 50, 51, 0x400000);
    size_t removed = find_after(&log, first, "map", "removed");
    EMU_CHECK(removed == first + 1 && log.events[removed].block_id == 30 &&
              log.events[removed].generation == 3);

    /* Accepted no-op/repeated programs remain independent access records and
     * do not manufacture mapping generations. */
    first = log.count;
    emu_eeprom_trace_mutation(
        trace, 0, EMU_EEPROM_MUTATION_PROGRAM,
        (uint32_t)(base0 + 0x30000), 2, 60, 61, 0x500000);
    emu_eeprom_trace_mutation(
        trace, 0, EMU_EEPROM_MUTATION_PROGRAM,
        (uint32_t)(base0 + 0x30000), 2, 60, 61, 0x500000);
    EMU_CHECK(log.count == first + 2);
    EMU_CHECK(log.events[first].generation == 3 &&
              log.events[first + 1].generation == 3);

    /* A whole-region erase is split at every old classification boundary;
     * both resolved mappings are removed in block-ID order afterward. */
    first = log.count;
    memset(images.bytes[1] + base1, 0xFF, EMU_EEPROM_REGION_SIZE);
    emu_eeprom_trace_mutation(
        trace, 1, EMU_EEPROM_MUTATION_ERASE, (uint32_t)base1,
        EMU_EEPROM_REGION_SIZE, 65, 66, 0x550000);
    size_t removed20 = find_after(&log, first, "map", "removed");
    size_t removed30 = removed20 == SIZE_MAX ? SIZE_MAX
                     : find_after(&log, removed20 + 1, "map", "removed");
    EMU_CHECK(removed20 != SIZE_MAX && removed30 != SIZE_MAX);
    EMU_CHECK(log.events[removed20].block_id == 20 &&
              log.events[removed30].block_id == 30);
    EMU_CHECK(log.events[removed20].generation == 1 &&
              log.events[removed30].generation == 1);
    for (size_t i = first; i < removed20; i++) {
        EMU_CHECK(log.events[i].mutation_offset == (long)base1);
        EMU_CHECK(log.events[i].mutation_size == EMU_EEPROM_REGION_SIZE);
    }

    /* Runtime tracking failures poison the trace so a file sink cannot
     * finalize a partial logical history. */
    images.fail_copy = 1;
    emu_eeprom_trace_mutation(
        trace, 0, EMU_EEPROM_MUTATION_PROGRAM,
        (uint32_t)(base0 + 0x30002), 1, 70, 71, 0x600000);
    EMU_CHECK(sink->failed);

    emu_eeprom_trace_destroy(&trace);
    EMU_CHECK(trace == NULL);
    EMU_CHECK(emu_trace_close(sink) == -1);
    for (size_t i = 0; i < 3; i++) free(images.bytes[i]);
}

static void test_initialization_failure(void) {
    images_t images = {.fail_copy = 1};
    images.sizes[0] = 16;
    images.bytes[0] = malloc(16);
    emu_eeprom_trace_chip_t chip = {0, "flash", "w30", 16};
    event_log_t log = {0};
    emu_trace_sink_t *sink = emu_trace_open_callback(
        EMU_TRACE_EEPROM, capture_event, &log);
    emu_eeprom_trace_t *trace = NULL;
    EMU_CHECK(emu_eeprom_trace_create(
                  &trace, sink, &chip, 1, copy_image, &images) == 0);
    EMU_CHECK(emu_eeprom_trace_arm(trace, 0, 0, 0) == -1);
    EMU_CHECK(sink->failed && log.count == 0);
    emu_eeprom_trace_destroy(&trace);
    EMU_CHECK(emu_trace_close(sink) == -1);
    free(images.bytes[0]);
}

int main(void) {
    test_mapping_and_access();
    test_initialization_failure();
    puts("engine-neutral EEPROM tracing: PASS");
    return 0;
}
