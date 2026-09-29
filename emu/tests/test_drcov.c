#define _GNU_SOURCE

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "test_support.h"
#include "emu_drcov.h"

static const emu_drcov_module_t MODULES[] = {
    {"vectors", 0x000000u, 0x000200u},
    {"internal_io_xbus", 0x00e000u, 0x00f000u},
    {"flash_native", 0x800000u, 0x1000000u},
};

static unsigned char *slurp(const char *path, size_t *size) {
    *size = 0;
    FILE *file = fopen(path, "rb");
    EMU_CHECK(file != NULL);
    EMU_CHECK(fseek(file, 0, SEEK_END) == 0);
    long length = ftell(file);
    EMU_CHECK(length >= 0 && fseek(file, 0, SEEK_SET) == 0);
    unsigned char *bytes = malloc(length ? (size_t)length : 1u);
    EMU_CHECK(bytes != NULL);
    EMU_CHECK(fread(bytes, 1u, (size_t)length, file) == (size_t)length);
    EMU_CHECK(fclose(file) == 0);
    *size = (size_t)length;
    return bytes;
}

static void append_record(
        unsigned char *record, uint32_t offset, uint16_t size, uint16_t id) {
    record[0] = offset & 0xffu;
    record[1] = (offset >> 8) & 0xffu;
    record[2] = (offset >> 16) & 0xffu;
    record[3] = (offset >> 24) & 0xffu;
    record[4] = size & 0xffu;
    record[5] = (size >> 8) & 0xffu;
    record[6] = id & 0xffu;
    record[7] = (id >> 8) & 0xffu;
}

static void test_empty(const char *path) {
    static const char expected[] =
        "DRCOV VERSION: 2\n"
        "DRCOV FLAVOR: drcov\n"
        "Module Table: version 2, count 0\n"
        "Columns: id, base, end, entry, checksum, timestamp, path\n"
        "BB Table: 0 bbs\n";
    emu_drcov_t *collector = NULL;
    EMU_CHECK(emu_drcov_create(
                  MODULES, sizeof MODULES / sizeof MODULES[0],
                  &collector) == EMU_DRCOV_OK);
    emu_drcov_statistics_t statistics = {99, 99, 99};
    EMU_CHECK(emu_drcov_write(collector, path, &statistics) == EMU_DRCOV_OK);
    EMU_CHECK(statistics.execution_events == 0);
    EMU_CHECK(statistics.unique_blocks == 0);
    EMU_CHECK(statistics.covered_modules == 0);
    size_t size;
    unsigned char *bytes = slurp(path, &size);
    EMU_CHECK(size == sizeof expected - 1u);
    EMU_CHECK(!memcmp(bytes, expected, size));
    free(bytes);
    EMU_CHECK(unlink(path) == 0);
    emu_drcov_destroy(&collector);
    EMU_CHECK(collector == NULL);
}

static void test_exact_records(const char *path) {
    static const char header[] =
        "DRCOV VERSION: 2\n"
        "DRCOV FLAVOR: drcov\n"
        "Module Table: version 2, count 2\n"
        "Columns: id, base, end, entry, checksum, timestamp, path\n"
        "0, 0x0, 0x200, 0x0, 0x0, 0x0, vectors\n"
        "1, 0x800000, 0x1000000, 0x0, 0x0, 0x0, flash_native\n"
        "BB Table: 4 bbs\n";
    emu_drcov_t *collector = NULL;
    EMU_CHECK(emu_drcov_create(MODULES, 3, &collector) == EMU_DRCOV_OK);
    EMU_CHECK(emu_drcov_add_instruction(
                  collector, 0x802008, 2) == EMU_DRCOV_OK);
    for (int i = 0; i < 5; i++)
        EMU_CHECK(emu_drcov_add_instruction(
                      collector, 0x000100, 2) == EMU_DRCOV_OK);
    EMU_CHECK(emu_drcov_add_instruction(
                  collector, 0x802000, 4) == EMU_DRCOV_OK);
    EMU_CHECK(emu_drcov_add_instruction(
                  collector, 0x0001fe, 2) == EMU_DRCOV_OK);
    emu_drcov_statistics_t statistics;
    emu_drcov_get_statistics(collector, &statistics);
    EMU_CHECK(statistics.execution_events == 8);
    EMU_CHECK(statistics.unique_blocks == 4);
    EMU_CHECK(statistics.covered_modules == 2);
    EMU_CHECK(emu_drcov_write(collector, path, &statistics) == EMU_DRCOV_OK);

    size_t size;
    unsigned char *bytes = slurp(path, &size);
    EMU_CHECK(size == sizeof header - 1u + 32u);
    EMU_CHECK(!memcmp(bytes, header, sizeof header - 1u));
    unsigned char expected[32];
    append_record(expected, 0x100, 2, 0);
    append_record(expected + 8, 0x1fe, 2, 0);
    append_record(expected + 16, 0x2000, 4, 1);
    append_record(expected + 24, 0x2008, 2, 1);
    EMU_CHECK(!memcmp(bytes + sizeof header - 1u, expected, sizeof expected));
    EMU_CHECK(memmem(bytes, size, "internal_io_xbus", 16) == NULL);
    free(bytes);
    EMU_CHECK(unlink(path) == 0);
    emu_drcov_destroy(&collector);
}

static void test_validation(void) {
    emu_drcov_t *collector = NULL;
    EMU_CHECK(!strcmp(emu_drcov_result_name(EMU_DRCOV_ERR_VALIDATION),
                      "validation"));
    EMU_CHECK(!strcmp(emu_drcov_result_name((emu_drcov_result_t)99),
                      "invalid"));
    EMU_CHECK(emu_drcov_create(NULL, 1, &collector) ==
              EMU_DRCOV_ERR_VALIDATION);
    EMU_CHECK(emu_drcov_create(MODULES, 0, &collector) ==
              EMU_DRCOV_ERR_VALIDATION);
    EMU_CHECK(emu_drcov_create(MODULES, 3, NULL) ==
              EMU_DRCOV_ERR_ARGUMENT);

    emu_drcov_module_t invalid[] = {
        {"one", 0x100, 0x200}, {"two", 0x1ff, 0x300},
    };
    EMU_CHECK(emu_drcov_create(invalid, 2, &collector) ==
              EMU_DRCOV_ERR_VALIDATION);
    invalid[1] = (emu_drcov_module_t){"two", 0x050, 0x100};
    EMU_CHECK(emu_drcov_create(invalid, 2, &collector) ==
              EMU_DRCOV_ERR_VALIDATION);
    invalid[0] = (emu_drcov_module_t){"bad\nname", 0x100, 0x200};
    EMU_CHECK(emu_drcov_create(invalid, 1, &collector) ==
              EMU_DRCOV_ERR_VALIDATION);
    invalid[0] = (emu_drcov_module_t){"empty", 0x100, 0x100};
    EMU_CHECK(emu_drcov_create(invalid, 1, &collector) ==
              EMU_DRCOV_ERR_VALIDATION);

    EMU_CHECK(emu_drcov_create(MODULES, 3, &collector) == EMU_DRCOV_OK);
    EMU_CHECK(emu_drcov_add_instruction(NULL, 0, 2) ==
              EMU_DRCOV_ERR_ARGUMENT);
    EMU_CHECK(emu_drcov_add_instruction(collector, 0, 0) ==
              EMU_DRCOV_ERR_VALIDATION);
    EMU_CHECK(emu_drcov_add_instruction(collector, 0, 65536) ==
              EMU_DRCOV_ERR_VALIDATION);
    EMU_CHECK(emu_drcov_add_instruction(collector, 0x1ff, 2) ==
              EMU_DRCOV_ERR_VALIDATION);
    EMU_CHECK(emu_drcov_add_instruction(collector, 0x200, 2) ==
              EMU_DRCOV_ERR_VALIDATION);
    emu_drcov_statistics_t statistics = {1, 1, 1};
    emu_drcov_get_statistics(collector, &statistics);
    EMU_CHECK(!statistics.execution_events && !statistics.unique_blocks &&
              !statistics.covered_modules);
    EMU_CHECK(emu_drcov_write(NULL, "unused", NULL) ==
              EMU_DRCOV_ERR_ARGUMENT);
    EMU_CHECK(emu_drcov_write(collector, "", NULL) ==
              EMU_DRCOV_ERR_ARGUMENT);
    emu_drcov_destroy(&collector);
}

static void test_allocation_failures(const char *path) {
    for (int allocation = 0; allocation < 6; allocation++) {
        emu_drcov_t *collector = NULL;
        emu_drcov_test_fail_alloc_after(allocation);
        EMU_CHECK(emu_drcov_create(MODULES, 3, &collector) ==
                  EMU_DRCOV_ERR_NOMEM);
        EMU_CHECK(collector == NULL);
        emu_drcov_destroy(&collector);
    }
    emu_drcov_t *collector = NULL;
    emu_drcov_test_fail_alloc_after(-1);
    EMU_CHECK(emu_drcov_create(MODULES, 3, &collector) == EMU_DRCOV_OK);
    emu_drcov_test_fail_alloc_after(0);
    EMU_CHECK(emu_drcov_add_instruction(collector, 0x100, 2) ==
              EMU_DRCOV_ERR_NOMEM);
    EMU_CHECK(emu_drcov_add_instruction(collector, 0x100, 2) == EMU_DRCOV_OK);
    emu_drcov_destroy(&collector);

    EMU_CHECK(emu_drcov_create(MODULES, 3, &collector) == EMU_DRCOV_OK);
    emu_drcov_test_fail_alloc_after(1);
    EMU_CHECK(emu_drcov_add_instruction(collector, 0x100, 2) ==
              EMU_DRCOV_ERR_NOMEM);
    emu_drcov_destroy(&collector);

    EMU_CHECK(emu_drcov_create(MODULES, 3, &collector) == EMU_DRCOV_OK);
    EMU_CHECK(emu_drcov_add_instruction(collector, 0x100, 2) == EMU_DRCOV_OK);
    emu_drcov_test_fail_alloc_after(0);
    EMU_CHECK(emu_drcov_write(collector, path, NULL) ==
              EMU_DRCOV_ERR_NOMEM);
    emu_drcov_test_fail_alloc_after(1);
    EMU_CHECK(emu_drcov_write(collector, path, NULL) ==
              EMU_DRCOV_ERR_NOMEM);
    emu_drcov_test_fail_alloc_after(-1);
    EMU_CHECK(emu_drcov_write(collector, path, NULL) == EMU_DRCOV_OK);
    EMU_CHECK(unlink(path) == 0);
    emu_drcov_destroy(&collector);
}

static void test_io_failures(void) {
    emu_drcov_t *collector = NULL;
    EMU_CHECK(emu_drcov_create(MODULES, 3, &collector) == EMU_DRCOV_OK);
    EMU_CHECK(emu_drcov_add_instruction(
                  collector, 0x800000, 2) == EMU_DRCOV_OK);
    EMU_CHECK(emu_drcov_write(
                  collector, "/no/such/x55-drcov/output", NULL) ==
              EMU_DRCOV_ERR_IO);
    if (access("/dev/full", W_OK) == 0)
        EMU_CHECK(emu_drcov_write(collector, "/dev/full", NULL) ==
                  EMU_DRCOV_ERR_IO);
    emu_drcov_destroy(&collector);
}

int main(void) {
    char directory[] = "/tmp/x55-drcov-XXXXXX";
    EMU_CHECK(mkdtemp(directory) != NULL);
    char path[256];
    EMU_CHECK(snprintf(path, sizeof path, "%s/cov.drcov", directory) > 0);
    test_empty(path);
    test_exact_records(path);
    test_validation();
    test_allocation_failures(path);
    test_io_failures();
    EMU_CHECK(rmdir(directory) == 0);
    puts("shared drcov collection and v2 serialization: PASS");
    return 0;
}
