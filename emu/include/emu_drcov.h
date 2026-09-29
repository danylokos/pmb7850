#ifndef EMU_DRCOV_H
#define EMU_DRCOV_H

#include <stddef.h>
#include <stdint.h>

typedef struct emu_drcov emu_drcov_t;

typedef struct {
    const char *name;
    uint32_t base;
    uint32_t end; /* exclusive */
} emu_drcov_module_t;

typedef struct {
    uint64_t execution_events;
    size_t unique_blocks;
    size_t covered_modules;
} emu_drcov_statistics_t;

typedef enum {
    EMU_DRCOV_OK = 0,
    EMU_DRCOV_ERR_ARGUMENT,
    EMU_DRCOV_ERR_NOMEM,
    EMU_DRCOV_ERR_VALIDATION,
    EMU_DRCOV_ERR_IO,
} emu_drcov_result_t;

const char *emu_drcov_result_name(emu_drcov_result_t result);

/* Module descriptors must be ordered by base, non-overlapping, and use
 * exclusive non-empty address ranges. The collector copies the descriptors. */
emu_drcov_result_t emu_drcov_create(
    const emu_drcov_module_t *modules, size_t module_count,
    emu_drcov_t **out);

/* Record one complete instruction. Identical (address,size) pairs deduplicate. */
emu_drcov_result_t emu_drcov_add_instruction(
    emu_drcov_t *collector, uint32_t address, uint32_t size);

void emu_drcov_get_statistics(
    const emu_drcov_t *collector, emu_drcov_statistics_t *statistics);

/* Write a DynamoRIO coverage file directly to path using the v2 format. */
emu_drcov_result_t emu_drcov_write(
    const emu_drcov_t *collector, const char *path,
    emu_drcov_statistics_t *statistics);

void emu_drcov_destroy(emu_drcov_t **collector);

/* Deterministic allocation-failure hook used by collector tests. */
void emu_drcov_test_fail_alloc_after(int successful_allocations);

#endif
