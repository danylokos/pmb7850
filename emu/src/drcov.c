#include "emu_drcov.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint32_t address;
    uint32_t size;
    size_t module;
} emu_drcov_block_t;

struct emu_drcov {
    emu_drcov_module_t *modules;
    unsigned char *module_used;
    size_t module_count;
    size_t covered_modules;
    emu_drcov_block_t *blocks;
    size_t block_count;
    size_t block_capacity;
    size_t *hash;
    size_t hash_capacity;
    uint64_t execution_events;
};

static int fail_alloc_after = -1;

void emu_drcov_test_fail_alloc_after(int successful_allocations) {
    fail_alloc_after = successful_allocations;
}

static int fail_allocation(void) {
    if (fail_alloc_after < 0) return 0;
    if (fail_alloc_after == 0) {
        fail_alloc_after = -1;
        return 1;
    }
    fail_alloc_after--;
    return 0;
}

static void *drcov_calloc(size_t count, size_t size) {
    if (fail_allocation()) return NULL;
    return calloc(count, size);
}

static void *drcov_realloc(void *pointer, size_t size) {
    if (fail_allocation()) return NULL;
    return realloc(pointer, size);
}

static char *drcov_strdup(const char *text) {
    size_t size = strlen(text) + 1u;
    char *copy = drcov_calloc(size, 1u);
    if (copy) memcpy(copy, text, size);
    return copy;
}

const char *emu_drcov_result_name(emu_drcov_result_t result) {
    static const char *const names[] = {
        "ok", "argument", "no-memory", "validation", "io"
    };
    return (unsigned)result < sizeof names / sizeof names[0]
         ? names[result] : "invalid";
}

static int valid_modules(
        const emu_drcov_module_t *modules, size_t module_count) {
    if (!modules || !module_count || module_count > (size_t)UINT16_MAX + 1u)
        return 0;
    for (size_t i = 0; i < module_count; i++) {
        const emu_drcov_module_t *module = &modules[i];
        if (!module->name || !module->name[0] ||
            strchr(module->name, '\n') || strchr(module->name, '\r') ||
            module->base >= module->end)
            return 0;
        if (i && modules[i - 1u].end > module->base)
            return 0;
    }
    return 1;
}

void emu_drcov_destroy(emu_drcov_t **collector) {
    if (!collector || !*collector) return;
    emu_drcov_t *drcov = *collector;
    for (size_t i = 0; i < drcov->module_count; i++)
        free((char *)drcov->modules[i].name);
    free(drcov->modules);
    free(drcov->module_used);
    free(drcov->blocks);
    free(drcov->hash);
    free(drcov);
    *collector = NULL;
}

emu_drcov_result_t emu_drcov_create(
        const emu_drcov_module_t *modules, size_t module_count,
        emu_drcov_t **out) {
    if (!out || *out) return EMU_DRCOV_ERR_ARGUMENT;
    if (!valid_modules(modules, module_count))
        return EMU_DRCOV_ERR_VALIDATION;
    emu_drcov_t *drcov = drcov_calloc(1u, sizeof *drcov);
    if (!drcov) return EMU_DRCOV_ERR_NOMEM;
    drcov->modules = drcov_calloc(module_count, sizeof *drcov->modules);
    drcov->module_used = drcov_calloc(module_count, 1u);
    if (!drcov->modules || !drcov->module_used) {
        emu_drcov_destroy(&drcov);
        return EMU_DRCOV_ERR_NOMEM;
    }
    drcov->module_count = module_count;
    for (size_t i = 0; i < module_count; i++) {
        drcov->modules[i] = modules[i];
        drcov->modules[i].name = drcov_strdup(modules[i].name);
        if (!drcov->modules[i].name) {
            emu_drcov_destroy(&drcov);
            return EMU_DRCOV_ERR_NOMEM;
        }
    }
    *out = drcov;
    return EMU_DRCOV_OK;
}

static uint64_t block_key(uint32_t address, uint32_t size) {
    return ((uint64_t)address << 16) | size;
}

static uint64_t hash64(uint64_t value) {
    value ^= value >> 33;
    value *= UINT64_C(0xff51afd7ed558ccd);
    value ^= value >> 33;
    value *= UINT64_C(0xc4ceb9fe1a85ec53);
    value ^= value >> 33;
    return value;
}

static void hash_insert(
        size_t *table, size_t capacity,
        const emu_drcov_block_t *blocks, size_t block_index) {
    size_t mask = capacity - 1u;
    uint64_t key = block_key(
        blocks[block_index].address, blocks[block_index].size);
    size_t slot = (size_t)hash64(key) & mask;
    while (table[slot]) slot = (slot + 1u) & mask;
    table[slot] = block_index + 1u;
}

static emu_drcov_result_t grow_hash(emu_drcov_t *drcov) {
    size_t capacity = drcov->hash_capacity
                    ? drcov->hash_capacity * 2u : 1024u;
    if (capacity < drcov->hash_capacity ||
        capacity > SIZE_MAX / sizeof *drcov->hash)
        return EMU_DRCOV_ERR_NOMEM;
    size_t *table = drcov_calloc(capacity, sizeof *table);
    if (!table) return EMU_DRCOV_ERR_NOMEM;
    for (size_t i = 0; i < drcov->block_count; i++)
        hash_insert(table, capacity, drcov->blocks, i);
    free(drcov->hash);
    drcov->hash = table;
    drcov->hash_capacity = capacity;
    return EMU_DRCOV_OK;
}

static size_t module_for(const emu_drcov_t *drcov, uint32_t address) {
    size_t low = 0, high = drcov->module_count;
    while (low < high) {
        size_t middle = low + (high - low) / 2u;
        const emu_drcov_module_t *module = &drcov->modules[middle];
        if (address < module->base)
            high = middle;
        else if (address >= module->end)
            low = middle + 1u;
        else
            return middle;
    }
    return SIZE_MAX;
}

emu_drcov_result_t emu_drcov_add_instruction(
        emu_drcov_t *drcov, uint32_t address, uint32_t size) {
    if (!drcov) return EMU_DRCOV_ERR_ARGUMENT;
    if (!size || size > UINT16_MAX) return EMU_DRCOV_ERR_VALIDATION;
    size_t module_index = module_for(drcov, address);
    if (module_index == SIZE_MAX ||
        size > drcov->modules[module_index].end - address)
        return EMU_DRCOV_ERR_VALIDATION;

    if (drcov->hash_capacity) {
        size_t mask = drcov->hash_capacity - 1u;
        uint64_t key = block_key(address, size);
        size_t slot = (size_t)hash64(key) & mask;
        while (drcov->hash[slot]) {
            size_t index = drcov->hash[slot] - 1u;
            if (drcov->blocks[index].address == address &&
                drcov->blocks[index].size == size) {
                drcov->execution_events++;
                return EMU_DRCOV_OK;
            }
            slot = (slot + 1u) & mask;
        }
    }

    if (drcov->block_count == drcov->block_capacity) {
        size_t capacity = drcov->block_capacity
                        ? drcov->block_capacity * 2u : 4096u;
        if (capacity < drcov->block_capacity ||
            capacity > SIZE_MAX / sizeof *drcov->blocks)
            return EMU_DRCOV_ERR_NOMEM;
        void *blocks = drcov_realloc(
            drcov->blocks, capacity * sizeof *drcov->blocks);
        if (!blocks) return EMU_DRCOV_ERR_NOMEM;
        drcov->blocks = blocks;
        drcov->block_capacity = capacity;
    }
    if (!drcov->hash_capacity ||
        (drcov->block_count + 1u) * 10u >= drcov->hash_capacity * 7u) {
        emu_drcov_result_t result = grow_hash(drcov);
        if (result != EMU_DRCOV_OK) return result;
    }

    size_t index = drcov->block_count;
    drcov->blocks[index] = (emu_drcov_block_t){
        .address = address, .size = size, .module = module_index,
    };
    hash_insert(drcov->hash, drcov->hash_capacity, drcov->blocks, index);
    drcov->block_count++;
    drcov->execution_events++;
    if (!drcov->module_used[module_index]) {
        drcov->module_used[module_index] = 1;
        drcov->covered_modules++;
    }
    return EMU_DRCOV_OK;
}

void emu_drcov_get_statistics(
        const emu_drcov_t *drcov, emu_drcov_statistics_t *statistics) {
    if (!statistics) return;
    memset(statistics, 0, sizeof *statistics);
    if (!drcov) return;
    statistics->execution_events = drcov->execution_events;
    statistics->unique_blocks = drcov->block_count;
    statistics->covered_modules = drcov->covered_modules;
}

static int compare_block(const void *left, const void *right) {
    const emu_drcov_block_t *a = left, *b = right;
    if (a->address != b->address) return a->address < b->address ? -1 : 1;
    if (a->size != b->size) return a->size < b->size ? -1 : 1;
    return 0;
}

static int write_text(FILE *file, const char *format, ...) {
    va_list arguments;
    va_start(arguments, format);
    int result = vfprintf(file, format, arguments);
    va_end(arguments);
    return result >= 0;
}

emu_drcov_result_t emu_drcov_write(
        const emu_drcov_t *drcov, const char *path,
        emu_drcov_statistics_t *statistics) {
    if (!drcov || !path || !path[0]) return EMU_DRCOV_ERR_ARGUMENT;
    emu_drcov_block_t *blocks = NULL;
    uint16_t *module_ids = NULL;
    if (drcov->block_count) {
        if (drcov->block_count > SIZE_MAX / sizeof *blocks)
            return EMU_DRCOV_ERR_NOMEM;
        blocks = drcov_calloc(drcov->block_count, sizeof *blocks);
        if (!blocks) return EMU_DRCOV_ERR_NOMEM;
        memcpy(blocks, drcov->blocks, drcov->block_count * sizeof *blocks);
        qsort(blocks, drcov->block_count, sizeof *blocks, compare_block);
    }
    module_ids = drcov_calloc(drcov->module_count, sizeof *module_ids);
    if (!module_ids) {
        free(blocks);
        return EMU_DRCOV_ERR_NOMEM;
    }
    uint16_t next_id = 0;
    for (size_t i = 0; i < drcov->module_count; i++)
        if (drcov->module_used[i]) module_ids[i] = next_id++;

    FILE *file = fopen(path, "wb");
    if (!file) {
        free(module_ids);
        free(blocks);
        return EMU_DRCOV_ERR_IO;
    }
    int okay =
        write_text(file, "DRCOV VERSION: 2\n") &&
        write_text(file, "DRCOV FLAVOR: drcov\n") &&
        write_text(file, "Module Table: version 2, count %zu\n",
                   drcov->covered_modules) &&
        write_text(file,
                   "Columns: id, base, end, entry, checksum, timestamp, path\n");
    for (size_t i = 0; okay && i < drcov->module_count; i++) {
        if (!drcov->module_used[i]) continue;
        const emu_drcov_module_t *module = &drcov->modules[i];
        okay = write_text(
            file, "%u, 0x%x, 0x%x, 0x0, 0x0, 0x0, %s\n",
            module_ids[i], module->base, module->end, module->name);
    }
    if (okay)
        okay = write_text(file, "BB Table: %zu bbs\n", drcov->block_count);
    for (size_t i = 0; okay && i < drcov->block_count; i++) {
        const emu_drcov_block_t *block = &blocks[i];
        uint32_t offset = block->address - drcov->modules[block->module].base;
        uint16_t size = (uint16_t)block->size;
        uint16_t id = module_ids[block->module];
        unsigned char record[8] = {
            offset & 0xffu, (offset >> 8) & 0xffu,
            (offset >> 16) & 0xffu, (offset >> 24) & 0xffu,
            size & 0xffu, (size >> 8) & 0xffu,
            id & 0xffu, (id >> 8) & 0xffu,
        };
        okay = fwrite(record, 1u, sizeof record, file) == sizeof record;
    }
    if (fclose(file) != 0) okay = 0;
    free(module_ids);
    free(blocks);
    if (!okay) return EMU_DRCOV_ERR_IO;
    emu_drcov_get_statistics(drcov, statistics);
    return EMU_DRCOV_OK;
}
