#define _POSIX_C_SOURCE 200809L

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "test_support.h"
#include "emu_trace.h"
#include "emu_trace_parquet.h"
#include "emu_trace_schema.h"

static char *read_file(const char *path, size_t *size_out) {
    FILE *file = fopen(path, "rb");
    EMU_CHECK(file != NULL);
    EMU_CHECK(fseek(file, 0, SEEK_END) == 0);
    long size = ftell(file);
    EMU_CHECK(size >= 0);
    EMU_CHECK(fseek(file, 0, SEEK_SET) == 0);
    char *data = malloc((size_t)size + 1u);
    EMU_CHECK(data != NULL);
    EMU_CHECK(fread(data, 1, (size_t)size, file) == (size_t)size);
    EMU_CHECK(fclose(file) == 0);
    data[size] = 0;
    if (size_out) *size_out = (size_t)size;
    return data;
}

static int remove_tree(const char *path) {
    struct stat state;
    if (lstat(path, &state))
        return errno == ENOENT ? 0 : -1;
    if (!S_ISDIR(state.st_mode)) return unlink(path);
    DIR *directory = opendir(path);
    if (!directory) return -1;
    struct dirent *entry;
    int result = 0;
    while ((entry = readdir(directory))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        char child[1024];
        EMU_CHECK(snprintf(child, sizeof child, "%s/%s", path,
                           entry->d_name) < (int)sizeof child);
        if (remove_tree(child)) result = -1;
    }
    EMU_CHECK(closedir(directory) == 0);
    return result ? result : rmdir(path);
}

static emu_trace_event_t event(const char *kind, uint64_t icount) {
    emu_trace_event_t value = {0};
    value.kind = kind;
    value.icount = icount;
    value.pc = UINT32_C(0x800000) + (uint32_t)icount;
    value.detail = "host fixture";
    return value;
}

static void test_selectors_and_schema(void) {
    const char *actual = emu_trace_selector_list_text();
    EMU_CHECK(strstr(actual, "exec") != NULL);
    EMU_CHECK(strstr(actual, "lifecycle") != NULL);
    EMU_CHECK(strstr(actual, "eeprom_access") != NULL);

    emu_trace_mask_t mask = 0;
    char bad[64];
    EMU_CHECK(emu_trace_parse_selectors(
                  "exec,mem,sfr,serial,serial_history,xbus,port,pec,lcd,keypad,tdma,"
                  "ssc0,sim,gsm,battery,capcom,twi,eeprom,flash,lifecycle,patch",
                  &mask, bad, sizeof bad) == 0);
    EMU_CHECK(mask == EMU_TRACE_MASK_ALL);
    EMU_CHECK(emu_trace_parse_selectors(
                  "exec,nope", &mask, bad, sizeof bad) == -1);
    EMU_CHECK(!strcmp(bad, "nope"));
    EMU_CHECK(emu_trace_mask_accepts(
                  EMU_TRACE_MASK_FLASH, "flash_program_complete"));
    EMU_CHECK(emu_trace_parse_selectors(
                  "eeprom_access,eeprom_map", &mask, bad, sizeof bad) == 0);
    EMU_CHECK(mask == EMU_TRACE_EEPROM);
    EMU_CHECK(emu_trace_parse_selectors(
                  "eeprom_access", &mask, bad, sizeof bad) == 0);
    EMU_CHECK(mask == EMU_TRACE_EEPROM_ACCESS);
    EMU_CHECK(!emu_trace_mask_accepts(mask, "eeprom_map"));
    EMU_CHECK(emu_trace_parse_selectors(
                  "eeprom_map", &mask, bad, sizeof bad) == 0);
    EMU_CHECK(mask == EMU_TRACE_EEPROM_MAP);
    EMU_CHECK(!emu_trace_mask_accepts(mask, "eeprom_access"));
    EMU_CHECK(!emu_trace_mask_accepts(EMU_TRACE_EXEC, "mem_read"));

    char error[256];
    EMU_CHECK(emu_trace_schema_validate_catalog(
                  error, sizeof error) == 0);
    emu_trace_event_t value = event("lcd_data", 1);
    emu_trace_info_int(&value.info, "command", 0x2A);
    emu_trace_info_int(&value.info, "parameter_index", 0);
    EMU_CHECK(emu_trace_schema_validate_event(
                  &value, error, sizeof error) == 0);
    value.info.n = 0;
    emu_trace_info_bool(&value.info, "command", 1);
    EMU_CHECK(emu_trace_schema_validate_event(
                  &value, error, sizeof error) == -1);
    EMU_CHECK(strstr(error, "lcd_data.command type 1"));

    value = event("battery_sample", 2);
    emu_trace_info_bool(&value.info, "result_word", 1);
    EMU_CHECK(emu_trace_schema_validate_event(
                  &value, error, sizeof error) == -1);
    EMU_CHECK(strstr(error, "battery_sample.result_word type 1"));
}

typedef struct {
    uint64_t sequences[4];
    const char *kinds[4];
    size_t count;
} callback_state_t;

static void trace_callback(void *opaque, const emu_trace_event_t *value,
                           uint64_t sequence) {
    callback_state_t *state = opaque;
    state->sequences[state->count] = sequence;
    state->kinds[state->count++] = value->kind;
}

static void test_filtering_and_order(void) {
    callback_state_t state = {0};
    emu_trace_sink_t *sink = emu_trace_open_callback(
        EMU_TRACE_MASK_LCD, trace_callback, &state);
    EMU_CHECK(sink != NULL);
    emu_trace_event_t exec = event("exec", 1);
    emu_trace_event_t select = event("lcd_select", 2);
    emu_trace_info_bool(&select.info, "selected", 1);
    if (emu_trace_sink_accepts(sink, exec.kind))
        emu_trace_emit(sink, &exec);
    if (emu_trace_sink_accepts(sink, select.kind))
        emu_trace_emit(sink, &select);
    select.icount = 3;
    if (emu_trace_sink_accepts(sink, select.kind))
        emu_trace_emit(sink, &select);
    EMU_CHECK(state.count == 2);
    EMU_CHECK(state.sequences[0] == 0 && state.sequences[1] == 1);
    EMU_CHECK(!strcmp(state.kinds[0], "lcd_select"));
    EMU_CHECK(emu_trace_close(sink) == 0);
}

static void emit_fixture(emu_trace_sink_t *sink, uint64_t base) {
    emu_trace_event_t exec = event("exec", base);
    exec.has_addr = exec.has_size = exec.has_value = 1;
    exec.addr = exec.pc; exec.size = 2; exec.value = 0xA5;
    emu_trace_emit(sink, &exec);
    emu_trace_event_t select = event("lcd_select", base + 1u);
    emu_trace_info_bool(&select.info, "selected", 1);
    emu_trace_emit(sink, &select);
    emu_trace_event_t access = event("eeprom_access", base + 2u);
    access.has_addr = access.has_size = access.has_value = 1;
    access.addr = 0xFA1234; access.size = 2; access.value = 0x55AA;
    emu_trace_info_str(&access.info, "access", "read");
    emu_trace_info_str(&access.info, "area", "resolved-payload");
    emu_trace_info_int(&access.info, "area_offset", 4);
    emu_trace_info_str(&access.info, "attribution", "resolved");
    emu_trace_info_int(&access.info, "block_id", 5009);
    emu_trace_info_int(&access.info, "block_offset", 4);
    emu_trace_info_int(&access.info, "chip_index", 0);
    emu_trace_info_str(&access.info, "chip_name", "flash");
    emu_trace_info_int(&access.info, "chip_offset", 0x7A1234);
    emu_trace_info_int(&access.info, "mapping_generation", 0);
    emu_trace_info_str(&access.info, "model", "m58lw064d");
    emu_trace_info_null(&access.info, "mutation_offset");
    emu_trace_info_null(&access.info, "mutation_size");
    emu_trace_info_int(&access.info, "tick", 17);
    emu_trace_emit(sink, &access);
    emu_trace_event_t map = event("eeprom_map", base + 3u);
    emu_trace_info_int(&map.info, "block_id", 5009);
    emu_trace_info_str(&map.info, "change", "initial");
    emu_trace_info_int(&map.info, "chip_index", 0);
    emu_trace_info_str(&map.info, "chip_name", "flash");
    emu_trace_info_bool(&map.info, "current_active", 1);
    emu_trace_info_int(&map.info, "current_descriptor_offset", 0x7A0200);
    emu_trace_info_int(&map.info, "current_length", 16);
    emu_trace_info_int(&map.info, "current_linear", 0xFA1230);
    emu_trace_info_int(&map.info, "current_payload_offset", 0x7A1230);
    emu_trace_info_int(&map.info, "current_version", 2);
    emu_trace_info_int(&map.info, "mapping_generation", 0);
    emu_trace_info_str(&map.info, "model", "m58lw064d");
    emu_trace_info_null(&map.info, "previous_active");
    emu_trace_info_null(&map.info, "previous_descriptor_offset");
    emu_trace_info_null(&map.info, "previous_length");
    emu_trace_info_null(&map.info, "previous_linear");
    emu_trace_info_null(&map.info, "previous_payload_offset");
    emu_trace_info_null(&map.info, "previous_version");
    emu_trace_info_int(&map.info, "tick", 17);
    emu_trace_emit(sink, &map);
}

static void test_parquet_lifecycle(void) {
    char temporary[] = "/tmp/emu-trace-test-XXXXXX";
    char *root = mkdtemp(temporary);
    EMU_CHECK(root != NULL);
    char output[1024], manifest[1024], marker[1024], failed[1024];
    EMU_CHECK(snprintf(output, sizeof output, "%s/trace.parquet", root) <
              (int)sizeof output);
    EMU_CHECK(snprintf(manifest, sizeof manifest, "%s/manifest.json", output) <
              (int)sizeof manifest);

    emu_trace_sink_t *sink = emu_trace_open(output, EMU_TRACE_MASK_ALL);
    EMU_CHECK(sink != NULL);
    emit_fixture(sink, 10);
    EMU_CHECK(emu_trace_close(sink) == 0);
    EMU_CHECK(access(manifest, F_OK) == 0);
    size_t manifest_size = 0;
    char *document = read_file(manifest, &manifest_size);
    EMU_CHECK(manifest_size > 0);
    EMU_CHECK(strstr(document, "\"events\": 4"));
    EMU_CHECK(strstr(document, "\"format\": \"cemu-trace-parquet-v1\""));
    const char *exec_partition = strstr(document, "\"kind\": \"exec\"");
    const char *eeprom_partition =
        strstr(document, "\"kind\": \"eeprom_access\"");
    const char *eeprom_map_partition =
        strstr(document, "\"kind\": \"eeprom_map\"");
    const char *lcd_partition = strstr(document, "\"kind\": \"lcd_select\"");
    EMU_CHECK(exec_partition && eeprom_partition && eeprom_map_partition &&
              lcd_partition && eeprom_partition < eeprom_map_partition &&
              eeprom_map_partition < exec_partition &&
              exec_partition < lcd_partition);
    free(document);

    EMU_CHECK(snprintf(marker, sizeof marker, "%s/old-marker", output) <
              (int)sizeof marker);
    FILE *old = fopen(marker, "wb");
    EMU_CHECK(old != NULL && fputs("old", old) >= 0 && fclose(old) == 0);
    sink = emu_trace_open(output, EMU_TRACE_EXEC);
    EMU_CHECK(sink != NULL);
    emu_trace_event_t exec = event("exec", 20);
    emu_trace_emit(sink, &exec);
    EMU_CHECK(emu_trace_close(sink) == 0);
    EMU_CHECK(access(marker, F_OK) != 0);
    document = read_file(manifest, NULL);
    EMU_CHECK(strstr(document, "\"events\": 1"));
    free(document);

    EMU_CHECK(snprintf(failed, sizeof failed, "%s/failed.parquet", root) <
              (int)sizeof failed);
    emu_trace_test_fail_worker_after(0);
    sink = emu_trace_open(failed, EMU_TRACE_EXEC);
    EMU_CHECK(sink != NULL);
    exec = event("exec", 30);
    emu_trace_emit(sink, &exec);
    EMU_CHECK(emu_trace_close(sink) == -1);
    EMU_CHECK(access(failed, F_OK) != 0);
    DIR *directory = opendir(root);
    EMU_CHECK(directory != NULL);
    struct dirent *entry;
    while ((entry = readdir(directory)))
        EMU_CHECK(!strstr(entry->d_name, "failed.parquet.partial-"));
    EMU_CHECK(closedir(directory) == 0);

    EMU_CHECK(remove_tree(root) == 0);
}

int main(void) {
    test_selectors_and_schema();
    test_filtering_and_order();
    test_parquet_lifecycle();
    puts("host normalized tracing and Parquet: PASS");
    return 0;
}
