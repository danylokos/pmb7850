/* Direct Parquet trace finalization, batching, schema, and selector tests. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "emu_trace.h"
#include "emu_trace_schema.h"

static int g_fail, g_total_fail, g_total_run;
#define CHECK(cond) do { if (!(cond)) { g_fail++; \
    printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

static emu_trace_event_t base_event(const char *kind, uint64_t icount) {
    emu_trace_event_t event = {0};
    event.kind = kind; event.icount = icount; event.pc = 0x800000;
    event.detail = "fixture";
    return event;
}

static void test_empty_trace_is_queryable(void) {
    const char *path = "/tmp/cemu-empty-trace.parquet";
    emu_trace_sink_t *sink = emu_trace_open(path, EMU_TRACE_MASK_ALL);
    CHECK(sink != NULL);
    CHECK(emu_trace_close(sink) == 0);
    char manifest[256];
    snprintf(manifest, sizeof manifest, "%s/manifest.json", path);
    CHECK(access(manifest, F_OK) == 0);
}

static int write_many_fixture(const char *path) {
    emu_trace_sink_t *sink = emu_trace_open(path, EMU_TRACE_MASK_ALL);
    if (!sink) return 2;
    emu_trace_event_t event = base_event("exec", 0);
    event.has_addr = event.has_size = event.has_value = 1;
    event.size = 2; event.detail = "repeated repeated repeated";
    for (uint32_t i = 0; i < EMU_TRACE_BATCH_EVENTS + 1u; i++) {
        event.icount = i; event.pc = 0x800000u + (i & 0xffffu);
        event.addr = event.pc; event.value = i & 0xffu;
        emu_trace_emit(sink, &event);
    }
    return emu_trace_close(sink) == 0 ? 0 : 2;
}

static void test_batch_boundary(void) {
    const char *path = "/tmp/cemu-multibatch-trace.parquet";
    emu_trace_sink_t *sink = emu_trace_open(path, EMU_TRACE_EXEC);
    CHECK(sink != NULL);
    if (!sink) return;
    emu_trace_event_t event = base_event("exec", 0);
    for (uint32_t i = 0; i < EMU_TRACE_BATCH_EVENTS + 1u; i++) {
        event.icount = i; event.pc = 0x800000u + (i & 0xffffu);
        emu_trace_emit(sink, &event);
    }
    CHECK(sink->count == EMU_TRACE_BATCH_EVENTS + 1u);
    CHECK(emu_trace_close(sink) == 0);
}

static void test_close_reports_schema_failure(void) {
    emu_trace_sink_t *sink = emu_trace_open("/tmp/cemu-invalid-trace.parquet", EMU_TRACE_EXEC);
    CHECK(sink != NULL);
    emu_trace_event_t event = base_event("undeclared", 1);
    emu_trace_emit(sink, &event);
    CHECK(emu_trace_close(sink) == -1);
}

static void test_duplicate_info_fails(void) {
    emu_trace_sink_t *sink = emu_trace_open("/tmp/cemu-duplicate-trace.parquet", EMU_TRACE_MASK_LCD);
    CHECK(sink != NULL);
    if (!sink) return;
    emu_trace_event_t event = base_event("lcd_select", 1);
    emu_trace_info_bool(&event.info, "selected", 1);
    emu_trace_info_bool(&event.info, "selected", 0);
    emu_trace_emit(sink, &event);
    CHECK(emu_trace_close(sink) == -1);
}

static void test_shared_schema_validation(void) {
    char error[256];
    CHECK(emu_trace_schema_validate_catalog(error, sizeof error) == 0);

    emu_trace_event_t event = base_event("lcd_data", 1);
    emu_trace_info_int(&event.info, "command", 0x2a);
    emu_trace_info_int(&event.info, "parameter_index", 0);
    CHECK(emu_trace_schema_validate_event(&event, error, sizeof error) == 0);

    event.info.n = 0;
    emu_trace_info_bool(&event.info, "command", 1);
    CHECK(emu_trace_schema_validate_event(&event, error, sizeof error) == -1);
    CHECK(strstr(error, "lcd_data.command type 1") != NULL);

    event.info.n = 0;
    emu_trace_info_int(&event.info, "not_declared", 1);
    CHECK(emu_trace_schema_validate_event(&event, error, sizeof error) == -1);

    event.info.n = 0;
    emu_trace_info_int(&event.info, "command", 1);
    emu_trace_info_int(&event.info, "command", 2);
    CHECK(emu_trace_schema_validate_event(&event, error, sizeof error) == -1);
    CHECK(strstr(error, "duplicate trace field") != NULL);

    event = base_event("not_declared", 1);
    CHECK(emu_trace_schema_validate_event(&event, error, sizeof error) == -1);
}

#if CEMU_TRACE_PARQUET
static void add_catalog_value(emu_trace_info_t *info,
                              const emu_trace_schema_key_t *key) {
    if (key->types & EMU_TRACE_SCHEMA_I64)
        emu_trace_info_int(info, key->name, 42);
    else if (key->types & EMU_TRACE_SCHEMA_BOOL)
        emu_trace_info_bool(info, key->name, 1);
    else if (key->types & EMU_TRACE_SCHEMA_STR)
        emu_trace_info_str(info, key->name, "fixture");
    else
        emu_trace_info_null(info, key->name);
}

static int write_catalog_fixture(const char *path) {
    emu_trace_sink_t *sink = emu_trace_open(path, EMU_TRACE_MASK_ALL);
    if (!sink) return 2;

    size_t kind_n = 0;
    const emu_trace_schema_kind_t *catalog = emu_trace_schema_catalog(&kind_n);
    uint64_t expected = 0;
    for (size_t i = 0; i < kind_n; i++) {
        emu_trace_event_t event = base_event(catalog[i].kind, expected);
        int has_nullable = 0;
        for (size_t j = 0; j < catalog[i].key_n; j++) {
            add_catalog_value(&event.info, &catalog[i].keys[j]);
            has_nullable |= !!(catalog[i].keys[j].types & EMU_TRACE_SCHEMA_NULL);
        }
        emu_trace_emit(sink, &event);
        expected++;

        if (has_nullable) {
            event = base_event(catalog[i].kind, expected);
            for (size_t j = 0; j < catalog[i].key_n; j++)
                if (catalog[i].keys[j].types & EMU_TRACE_SCHEMA_NULL)
                    emu_trace_info_null(&event.info, catalog[i].keys[j].name);
            emu_trace_emit(sink, &event);
            expected++;
        }
    }
    if (sink->count != expected) {
        emu_trace_close(sink);
        return 2;
    }
    return emu_trace_close(sink) == 0 ? 0 : 2;
}

static void test_every_catalog_field_serializes(void) {
    CHECK(write_catalog_fixture("/tmp/cemu-all-schema-trace.parquet") == 0);
}
#endif

static void test_selectors(void) {
    emu_trace_mask_t mask = 0;
    char bad[32] = {0};
    CHECK(emu_trace_parse_selectors(
        "exec,mem,sfr,serial,xbus,port,pec,lcd,keypad,tdma,ssc0,sim,gsm,battery,capcom,twi,flash,eeprom,lifecycle,patch",
        &mask, bad, sizeof bad) == 0);
    CHECK(mask == EMU_TRACE_MASK_ALL);
    CHECK(emu_trace_parse_selectors("exec,nope", &mask, bad, sizeof bad) == -1);
    CHECK(strcmp(bad, "nope") == 0);
    CHECK(emu_trace_mask_accepts(EMU_TRACE_MASK_XBUS, "xbus_mailbox_complete"));
    CHECK(emu_trace_mask_accepts(EMU_TRACE_MASK_XBUS, "xbus_audio_packet"));
    CHECK(emu_trace_mask_accepts(EMU_TRACE_MASK_KEYPAD,
                                 "keypad_startup_release"));
    CHECK(emu_trace_mask_accepts(EMU_TRACE_MASK_FLASH, "flash_erase_complete"));
    CHECK(emu_trace_mask_accepts(EMU_TRACE_MASK_FLASH, "flash_program_complete"));
    CHECK(emu_trace_mask_accepts(EMU_TRACE_MASK_FLASH, "flash_rejected_command"));
    CHECK(emu_trace_mask_accepts(EMU_TRACE_MASK_FIRMWARE_PATCH, "firmware_patch"));
    CHECK(emu_trace_mask_accepts(EMU_TRACE_MASK_GSM, "gsm_l1_firmware"));
    CHECK(!emu_trace_mask_accepts(EMU_TRACE_EXEC, "mem_read"));
    CHECK(strstr(emu_trace_selector_list_text(), "capcom_output") != NULL);
    CHECK(strstr(emu_trace_selector_list_text(), "twi_event") != NULL);
    CHECK(strstr(emu_trace_selector_list_text(), "firmware_patch") != NULL);
}

static int write_fixture(const char *path) {
    emu_trace_sink_t *sink = emu_trace_open(path, EMU_TRACE_MASK_ALL);
    if (!sink) return 2;
    emu_trace_event_t a = base_event("xbus_access", 10);
    a.pc = 0x123456; a.has_addr = 1; a.addr = 0x00ef12;
    a.has_size = 1; a.size = 2; a.has_value = 1; a.value = 0xfeed;
    a.detail = "all fields";
    emu_trace_info_str(&a.info, "access", "write");
    emu_trace_info_null(&a.info, "xbus_start");
    emu_trace_emit(sink, &a);
    emu_trace_event_t b = base_event("lcd_select", 10);
    b.pc = 0x654321; b.detail = "sparse";
    emu_trace_info_bool(&b.info, "selected", 1);
    emu_trace_emit(sink, &b);
    emu_trace_event_t c = base_event("xbus_access", 11);
    c.pc = 0x123450; c.has_addr = 1; c.addr = 0x00ef10;
    c.detail = "all fields";
    emu_trace_info_str(&c.info, "access", "read");
    emu_trace_info_int(&c.info, "xbus_start", 0xE000);
    emu_trace_emit(sink, &c);
    return emu_trace_close(sink) == 0 ? 0 : 2;
}

static void emit_xbus(emu_trace_sink_t *sink, const char *kind, uint64_t icount,
                      uint32_t addr, uint32_t value, const char *access) {
    emu_trace_event_t event = base_event(kind, icount);
    event.has_addr = event.has_size = event.has_value = 1;
    event.addr = addr; event.size = 2; event.value = value;
    if (access) emu_trace_info_str(&event.info, "access", access);
    emu_trace_emit(sink, &event);
}
static int write_xbus_fixture(const char *path) {
    emu_trace_sink_t *sink = emu_trace_open(path, EMU_TRACE_MASK_ALL);
    if (!sink) return 2;
    emit_xbus(sink, "xbus_access", 10, 0xEC14, 2, "write");
    emit_xbus(sink, "xbus_access", 11, 0xEC12, 1, "write");
    emu_trace_event_t completion = base_event("xbus_mailbox_complete", 11);
    completion.has_addr = completion.has_size = completion.has_value = 1;
    completion.addr = 0xEC12; completion.size = 2; completion.value = 2;
    emu_trace_info_int(&completion.info, "transaction_id", 7);
    emu_trace_info_str(&completion.info, "phase", "sync-ready");
    emu_trace_info_int(&completion.info, "deadline", 43);
    emu_trace_info_bool(&completion.info, "irq_asserted", 0);
    emu_trace_info_int(&completion.info, "result", 16);
    emu_trace_emit(sink, &completion);
    emit_xbus(sink, "xbus_access", 12, 0xEC12, 2, "read");
    emit_xbus(sink, "xbus_access", 13, 0xEC16, 16, "read");
    emit_xbus(sink, "xbus_access", 14, 0xEC12, 0, "write");
    emit_xbus(sink, "mem_write", 15, 0x3828, 0x1234, NULL);
    emit_xbus(sink, "xbus_access", 100, 0xEC12, 1, "write");
    completion.icount = 132; completion.value = 4; completion.info.n = 0;
    emu_trace_info_int(&completion.info, "transaction_id", 8);
    emu_trace_info_str(&completion.info, "phase", "irq-promoted");
    emu_trace_info_int(&completion.info, "deadline", 132);
    emu_trace_info_bool(&completion.info, "irq_asserted", 1);
    emu_trace_info_int(&completion.info, "irq_value", 209);
    emu_trace_info_int(&completion.info, "result", 16);
    emu_trace_emit(sink, &completion);
    return emu_trace_close(sink) == 0 ? 0 : 2;
}

typedef struct {
    int active;
    long descriptor, length, linear, payload, version;
} eeprom_mapping_t;

static void emit_eeprom_access(
        emu_trace_sink_t *sink, uint64_t icount, uint32_t pc,
        const char *access, const char *area, const char *attribution,
        int chip_index, const char *chip_name, const char *model,
        long chip_offset, long generation, long block_id, long block_offset,
        int size, long tick) {
    emu_trace_event_t event = base_event("eeprom_access", icount);
    event.pc = pc; event.has_size = 1; event.size = size;
    if (!strcmp(access, "read")) {
        event.has_addr = event.has_value = 1;
        event.addr = 0xF00000u + (uint32_t)chip_offset;
        event.value = 0xA5;
    }
    emu_trace_info_str(&event.info, "access", access);
    emu_trace_info_str(&event.info, "area", area);
    emu_trace_info_int(&event.info, "area_offset", block_offset >= 0 ? block_offset : 0);
    emu_trace_info_str(&event.info, "attribution", attribution);
    if (block_id >= 0) {
        emu_trace_info_int(&event.info, "block_id", block_id);
        emu_trace_info_int(&event.info, "block_offset", block_offset);
    } else {
        emu_trace_info_null(&event.info, "block_id");
        emu_trace_info_null(&event.info, "block_offset");
    }
    emu_trace_info_int(&event.info, "chip_index", chip_index);
    emu_trace_info_str(&event.info, "chip_name", chip_name);
    emu_trace_info_int(&event.info, "chip_offset", chip_offset);
    emu_trace_info_int(&event.info, "mapping_generation", generation);
    emu_trace_info_str(&event.info, "model", model);
    if (!strcmp(access, "read")) {
        emu_trace_info_null(&event.info, "mutation_offset");
        emu_trace_info_null(&event.info, "mutation_size");
    } else {
        emu_trace_info_int(&event.info, "mutation_offset", chip_offset);
        emu_trace_info_int(&event.info, "mutation_size", size);
    }
    emu_trace_info_int(&event.info, "tick", tick);
    emu_trace_emit(sink, &event);
}

static void add_eeprom_mapping_fields(
        emu_trace_info_t *info, const char *prefix,
        const eeprom_mapping_t *mapping) {
    const char *active = !strcmp(prefix, "current")
                       ? "current_active" : "previous_active";
    const char *descriptor = !strcmp(prefix, "current")
                           ? "current_descriptor_offset"
                           : "previous_descriptor_offset";
    const char *length = !strcmp(prefix, "current")
                       ? "current_length" : "previous_length";
    const char *linear = !strcmp(prefix, "current")
                       ? "current_linear" : "previous_linear";
    const char *payload = !strcmp(prefix, "current")
                        ? "current_payload_offset"
                        : "previous_payload_offset";
    const char *version = !strcmp(prefix, "current")
                        ? "current_version" : "previous_version";
    if (!mapping) {
        emu_trace_info_null(info, active);
        emu_trace_info_null(info, descriptor);
        emu_trace_info_null(info, length);
        emu_trace_info_null(info, linear);
        emu_trace_info_null(info, payload);
        emu_trace_info_null(info, version);
        return;
    }
    emu_trace_info_bool(info, active, mapping->active);
    emu_trace_info_int(info, descriptor, mapping->descriptor);
    emu_trace_info_int(info, length, mapping->length);
    emu_trace_info_int(info, linear, mapping->linear);
    emu_trace_info_int(info, payload, mapping->payload);
    emu_trace_info_int(info, version, mapping->version);
}

static void emit_eeprom_map(
        emu_trace_sink_t *sink, uint64_t icount, uint32_t pc,
        const char *change, int chip_index, const char *chip_name,
        const char *model, long generation, long block_id,
        const eeprom_mapping_t *previous, const eeprom_mapping_t *current,
        long tick) {
    emu_trace_event_t event = base_event("eeprom_map", icount);
    event.pc = pc;
    emu_trace_info_int(&event.info, "block_id", block_id);
    emu_trace_info_str(&event.info, "change", change);
    emu_trace_info_int(&event.info, "chip_index", chip_index);
    emu_trace_info_str(&event.info, "chip_name", chip_name);
    add_eeprom_mapping_fields(&event.info, "current", current);
    emu_trace_info_int(&event.info, "mapping_generation", generation);
    emu_trace_info_str(&event.info, "model", model);
    add_eeprom_mapping_fields(&event.info, "previous", previous);
    emu_trace_info_int(&event.info, "tick", tick);
    emu_trace_emit(sink, &event);
}

static eeprom_mapping_t eeprom_mapping(long block_id) {
    eeprom_mapping_t mapping = {
        1, 0x1000 + block_id * 0x10, 0x20,
        0xFA0000 + block_id * 0x40, 0x4000 + block_id * 0x40, 1,
    };
    return mapping;
}

static int write_eeprom_fixture(const char *path, int access_only) {
    emu_trace_sink_t *sink = emu_trace_open(path, EMU_TRACE_MASK_ALL);
    if (!sink) return 2;
    const char *name0 = "flash", *model0 = "m58lw064d";
    const char *name1 = "flash1", *model1 = "am29lv640mh";
    eeprom_mapping_t mappings[10];
    const long ids[] = {0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x90, 0xA0};
    for (size_t i = 0; i < sizeof ids / sizeof ids[0]; i++) {
        mappings[i] = eeprom_mapping(ids[i]);
        if (ids[i] == 0x90) mappings[i].length = 8;
        if (!access_only) {
            emit_eeprom_map(sink, 100 + i, 0x800100u + (uint32_t)i,
                            "initial", 0, name0, model0, 0, ids[i],
                            NULL, &mappings[i], 10 + (long)i);
        }
    }
    if (!access_only) {
        mappings[9] = eeprom_mapping(0x10);
        emit_eeprom_map(sink, 120, 0x810100, "initial", 1, name1, model1,
                        0, 0x10, NULL, &mappings[9], 20);
    }

    emit_eeprom_access(sink, 200, 0x900010, "read", "resolved-payload",
                       "resolved", 0, name0, model0, 0x4400, 0, 0x10, 0, 4, 30);
    emit_eeprom_access(sink, 201, 0x900011, "read", "resolved-payload",
                       "resolved", 0, name0, model0, 0x4402, 0, 0x10, 2, 4, 31);
    emit_eeprom_access(sink, 202, 0x900012, "program", "resolved-payload",
                       "resolved", 0, name0, model0, 0x4405, 0, 0x10, 5, 2, 32);
    emit_eeprom_access(sink, 203, 0x900020, "program", "resolved-payload",
                       "resolved", 0, name0, model0, 0x4800, 0, 0x20, 0, 2, 33);
    emit_eeprom_access(sink, 204, 0x900021, "read", "resolved-payload",
                       "resolved", 0, name0, model0, 0x4801, 0, 0x20, 1, 3, 34);
    emit_eeprom_access(sink, 205, 0x900030, "read", "resolved-payload",
                       "resolved", 0, name0, model0, 0x4C08, 0, 0x30, 8, 2, 35);
    emit_eeprom_access(sink, 206, 0x900031, "read", "resolved-payload",
                       "resolved", 0, name0, model0, 0x4C0C, 0, 0x30, 12, 2, 36);
    emit_eeprom_access(sink, 207, 0x900040, "erase", "resolved-payload",
                       "resolved", 0, name0, model0, 0x5004, 0, 0x40, 4, 4, 37);
    emit_eeprom_access(sink, 208, 0x900041, "program", "resolved-payload",
                       "resolved", 0, name0, model0, 0x5004, 0, 0x40, 4, 4, 38);
    emit_eeprom_access(sink, 209, 0x900041, "program", "resolved-payload",
                       "resolved", 0, name0, model0, 0x5004, 0, 0x40, 4, 4, 39);
    emit_eeprom_access(sink, 210, 0x900060, "read", "descriptor", "metadata",
                       0, name0, model0, mappings[5].descriptor + 1, 0,
                       0x60, 1, 2, 40);
    emit_eeprom_access(sink, 211, 0x900070, "read", "shadowed-payload",
                       "shadowed", 0, name0, model0, 0x5C00, 0, 0x70, 0, 1, 41);
    emit_eeprom_access(sink, 212, 0x900080, "read", "header", "metadata",
                       0, name0, model0, 0x10, 0, -1, -1, 2, 42);
    emit_eeprom_access(sink, 213, 0x900081, "read", "unallocated", "unattributed",
                       0, name0, model0, 0x300, 0, -1, -1, 2, 43);
    emit_eeprom_access(sink, 214, 0x900082, "program", "unallocated",
                       "unattributed-journal", 0, name0, model0,
                       0x100, 0, -1, -1, 0x110, 44);
    if (!access_only) {
        eeprom_mapping_t added = {1, 0x100, 8, 0xFA0200, 0x200, 2};
        emit_eeprom_map(sink, 215, 0x900083, "added", 0, name0, model0,
                        1, 0x80, NULL, &added, 45);
    }
    emit_eeprom_access(sink, 216, 0x900090, "program", "unallocated",
                       "unattributed-journal", 0, name0, model0,
                       0x400, 1, -1, -1, 24, 46);
    if (!access_only) {
        eeprom_mapping_t previous = mappings[7];
        eeprom_mapping_t current = {1, 0x400, 20, 0xFA0410, 0x410, 2};
        emit_eeprom_map(sink, 217, 0x900091, "remapped", 0, name0, model0,
                        2, 0x90, &previous, &current, 47);
        emit_eeprom_access(sink, 218, 0x900092, "read", "resolved-payload",
                           "resolved", 0, name0, model0, 0x422, 2,
                           0x90, 18, 2, 48);
        emit_eeprom_map(sink, 219, 0x9000A0, "removed", 0, name0, model0,
                        3, 0xA0, &mappings[8], NULL, 49);
        emit_eeprom_access(sink, 220, 0x910010, "read", "resolved-payload",
                           "resolved", 1, name1, model1, 0x4400, 0,
                           0x10, 0, 1, 50);
    }
    return emu_trace_close(sink) == 0 ? 0 : 2;
}

static int write_eeprom_map_only_fixture(const char *path) {
    emu_trace_sink_t *sink = emu_trace_open(path, EMU_TRACE_MASK_ALL);
    if (!sink) return 2;
    eeprom_mapping_t mapping = eeprom_mapping(0x55);
    emit_eeprom_map(sink, 10, 0x800055, "initial", 0, "flash",
                    "m58lw064d", 0, 0x55, NULL, &mapping, 3);
    return emu_trace_close(sink) == 0 ? 0 : 2;
}

typedef struct { const char *name; void (*fn)(void); } entry_t;
static const entry_t TESTS[] = {
#if CEMU_TRACE_PARQUET
    {"empty_trace_is_queryable", test_empty_trace_is_queryable},
    {"batch_boundary", test_batch_boundary},
    {"close_reports_schema_failure", test_close_reports_schema_failure},
    {"duplicate_info_fails", test_duplicate_info_fails},
    {"every_catalog_field_serializes", test_every_catalog_field_serializes},
#endif
    {"shared_schema_validation", test_shared_schema_validation},
    {"selectors", test_selectors},
};
int main(int argc, char **argv) {
    if (argc == 3 && !strcmp(argv[1], "--fixture")) return write_fixture(argv[2]);
    if (argc == 3 && !strcmp(argv[1], "--xbus-fixture")) return write_xbus_fixture(argv[2]);
    if (argc == 3 && !strcmp(argv[1], "--eeprom-fixture"))
        return write_eeprom_fixture(argv[2], 0);
    if (argc == 3 && !strcmp(argv[1], "--eeprom-access-only-fixture"))
        return write_eeprom_fixture(argv[2], 1);
    if (argc == 3 && !strcmp(argv[1], "--eeprom-map-only-fixture"))
        return write_eeprom_map_only_fixture(argv[2]);
    if (argc == 3 && !strcmp(argv[1], "--many-fixture")) return write_many_fixture(argv[2]);
#if CEMU_TRACE_PARQUET
    if (argc == 3 && !strcmp(argv[1], "--catalog-fixture"))
        return write_catalog_fixture(argv[2]);
#endif
    if (argc == 3 && !strcmp(argv[1], "--empty-fixture")) {
        emu_trace_sink_t *sink = emu_trace_open(argv[2], EMU_TRACE_MASK_ALL);
        return sink && emu_trace_close(sink) == 0 ? 0 : 2;
    }
    int n = (int)(sizeof TESTS / sizeof TESTS[0]);
    for (int i = 0; i < n; i++) {
        g_fail = 0; TESTS[i].fn(); g_total_run++;
        if (g_fail) { g_total_fail++; printf("[FAIL] %s (%d)\n", TESTS[i].name, g_fail); }
    }
    printf("\n%d tests, %d failed\n", g_total_run, g_total_fail);
    return g_total_fail ? 1 : 0;
}
