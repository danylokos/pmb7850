#ifndef EMU_PATCH_H
#define EMU_PATCH_H

#include <stdio.h>

#include "emu_engine.h"

typedef uint64_t emu_patch_set_t;

typedef struct {
    int chip_index;
    uint32_t chip_offset;
    const uint8_t *expected;
    const uint8_t *replacement;
    size_t size;
} emu_patch_hunk_t;

typedef struct {
    const char *name;
    const char *description;
    const char *device_name;
    const char *image_model;
    int software_version;
    const char *provenance;
    const emu_patch_hunk_t *hunks;
    size_t hunk_count;
} emu_patch_definition_t;

typedef struct {
    emu_patch_set_t applicable;
    emu_patch_set_t already_applied;
} emu_patch_plan_result_t;

const emu_patch_definition_t *emu_patch_catalog(size_t *count);
int emu_patch_id_by_name(const char *name);
int emu_patch_parse(emu_patch_set_t *set, const char *csv,
                    char *bad, size_t bad_capacity);
int emu_patch_active_names(emu_patch_set_t set,
                           char *buffer, size_t capacity);
void emu_patch_print(FILE *out);
void emu_patch_print_json(FILE *out, emu_patch_set_t set);

emu_error_code_t emu_patch_plan_definitions(
    emu_prepared_session_t *prepared,
    const emu_patch_definition_t *definitions, size_t definition_count,
    emu_patch_set_t set, emu_patch_plan_result_t *result,
    emu_error_t *error);
emu_error_code_t emu_patch_plan(
    emu_prepared_session_t *prepared, emu_patch_set_t set,
    emu_patch_plan_result_t *result, emu_error_t *error);

#endif
