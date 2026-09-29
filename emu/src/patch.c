#include "emu_patch.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

static const uint8_t SQWE3_EXPECTED[] = {0x02};
static const uint8_t SQWE3_REPLACEMENT[] = {0x03};
static const emu_patch_hunk_t SQWE3_HUNKS[] = {
    {0, 0x4300C4, SQWE3_EXPECTED, SQWE3_REPLACEMENT, 1},
};
static const uint8_t AIRCHECK_EXPECTED[] = {0x3D};
static const uint8_t AIRCHECK_REPLACEMENT[] = {0x0D};
static const emu_patch_hunk_t AIRCHECK_HUNKS[] = {
    {0, 0x522058, AIRCHECK_EXPECTED, AIRCHECK_REPLACEMENT, 1},
};
static const uint8_t VOLUME_1_EXPECTED[] = {0x2D,0x02};
static const uint8_t VOLUME_1_REPLACEMENT[] = {0x0D,0x11};
static const uint8_t VOLUME_2_EXPECTED[] = {0x3D};
static const uint8_t VOLUME_2_REPLACEMENT[] = {0x0D};
static const uint8_t VOLUME_3_EXPECTED[] = {0x2D};
static const uint8_t VOLUME_3_REPLACEMENT[] = {0x0D};
static const emu_patch_hunk_t VOLUME_HUNKS[] = {
    {0,0x4CE010,VOLUME_1_EXPECTED,VOLUME_1_REPLACEMENT,2},
    {0,0x519146,VOLUME_2_EXPECTED,VOLUME_2_REPLACEMENT,1},
    {0,0x522A24,VOLUME_3_EXPECTED,VOLUME_3_REPLACEMENT,1},
};

static const emu_patch_definition_t PATCHES[] = {
    {"c55-sqwe3",
     "raise the recovered SQWE parser upper bound from 2 to 3",
     "c55","C55",24,"recovered C55 SW24 SQWE parser bound",
     SQWE3_HUNKS,1},
    {"c55-aircheck-off",
     "disable the C55 startup aircraft-mode confirmation",
     "c55","C55",24,"Kibab patch 161 (avkiev)",AIRCHECK_HUNKS,1},
    {"c55-volume-check-off",
     "disable the maximum speaker-volume warning dialogs",
     "c55","C55",24,"Kibab patch 160 (SiNgle & alex_itd)",
     VOLUME_HUNKS,3},
};

static emu_error_code_t patch_fail(emu_error_t *error,
                                   emu_error_code_t code,
                                   const char *format, ...) {
    if (error) {
        va_list args;
        va_start(args, format);
        error->code = code;
        vsnprintf(error->message, sizeof error->message, format, args);
        va_end(args);
    }
    return code;
}

static void patch_ok(emu_error_t *error) {
    if (error) { error->code = EMU_OK; error->message[0] = 0; }
}

const emu_patch_definition_t *emu_patch_catalog(size_t *count) {
    if (count) *count = sizeof PATCHES / sizeof PATCHES[0];
    return PATCHES;
}

int emu_patch_id_by_name(const char *name) {
    for (size_t i = 0; name && i < sizeof PATCHES / sizeof PATCHES[0]; i++)
        if (!strcmp(name, PATCHES[i].name)) return (int)i;
    return -1;
}

int emu_patch_parse(emu_patch_set_t *set, const char *csv,
                    char *bad, size_t bad_capacity) {
    if (!set || !csv || !*csv) {
        if (bad && bad_capacity) snprintf(bad, bad_capacity, "%s", csv ? csv : "");
        return -1;
    }
    emu_patch_set_t parsed = *set;
    const char *cursor = csv;
    while (*cursor) {
        while (isspace((unsigned char)*cursor)) cursor++;
        const char *start = cursor;
        while (*cursor && *cursor != ',') cursor++;
        const char *end = cursor;
        while (end > start && isspace((unsigned char)end[-1])) end--;
        size_t length = (size_t)(end - start);
        char name[96];
        if (!length || length >= sizeof name) {
            if (bad && bad_capacity)
                snprintf(bad, bad_capacity, "%.*s", (int)length, start);
            return -1;
        }
        memcpy(name, start, length); name[length] = 0;
        int id = emu_patch_id_by_name(name);
        if (id < 0 || id >= 64) {
            if (bad && bad_capacity) snprintf(bad, bad_capacity, "%s", name);
            return -1;
        }
        parsed |= UINT64_C(1) << id;
        if (*cursor == ',') {
            cursor++;
            if (!*cursor) {
                if (bad && bad_capacity) bad[0] = 0;
                return -1;
            }
        }
    }
    *set = parsed;
    if (bad && bad_capacity) bad[0] = 0;
    return 0;
}

int emu_patch_active_names(emu_patch_set_t set,
                           char *buffer, size_t capacity) {
    size_t used = 0;
    int active = 0;
    if (capacity) buffer[0] = 0;
    for (size_t i = 0; i < sizeof PATCHES / sizeof PATCHES[0]; i++) {
        if (!(set & (UINT64_C(1) << i))) continue;
        int written = snprintf(buffer && used < capacity ? buffer + used : NULL,
                               used < capacity ? capacity - used : 0,
                               "%s%s", active ? "," : "", PATCHES[i].name);
        if (written > 0) used += (size_t)written;
        active++;
    }
    return active;
}

void emu_patch_print_json(FILE *out, emu_patch_set_t set) {
    fputc('[', out);
    int first = 1;
    for (size_t i = 0; i < sizeof PATCHES / sizeof PATCHES[0]; i++) {
        if (!(set & (UINT64_C(1) << i))) continue;
        fprintf(out, "%s\"%s\"", first ? "" : ",", PATCHES[i].name);
        first = 0;
    }
    fputc(']', out);
}

void emu_patch_print(FILE *out) {
    fputs("FIRMWARE patches (--patch NAMES; repeatable comma-separated names):\n", out);
    for (size_t i = 0; i < sizeof PATCHES / sizeof PATCHES[0]; i++) {
        const emu_patch_definition_t *patch = &PATCHES[i];
        fprintf(out, "  %s\n    %s\n    target: %s SW%d (device %s)\n",
                patch->name, patch->description, patch->image_model,
                patch->software_version, patch->device_name);
        for (size_t j = 0; j < patch->hunk_count; j++) {
            const emu_patch_hunk_t *hunk = &patch->hunks[j];
            fprintf(out, "    hunk: chip %d + 0x%06X (%zu byte%s): ",
                    hunk->chip_index, hunk->chip_offset, hunk->size,
                    hunk->size == 1 ? "" : "s");
            for (size_t k = 0; k < hunk->size; k++)
                fprintf(out, "%s%02X", k ? " " : "", hunk->expected[k]);
            fputs(" -> ", out);
            for (size_t k = 0; k < hunk->size; k++)
                fprintf(out, "%s%02X", k ? " " : "", hunk->replacement[k]);
            fputc('\n', out);
        }
        fprintf(out, "    provenance: %s\n", patch->provenance);
    }
}

static void discard_operations(emu_prepared_session_t *prepared,
                               size_t first) {
    while (prepared->operation_count > first) {
        emu_storage_operation_t *operation =
            &prepared->operations[--prepared->operation_count];
        free(operation->expected);
        free(operation->replacement);
        memset(operation, 0, sizeof *operation);
    }
}

static int definitions_conflict(const emu_patch_definition_t *definitions,
                                size_t count, emu_patch_set_t set,
                                emu_error_t *error) {
    for (size_t i = 0; i < count; i++) {
        if (!(set & (UINT64_C(1) << i))) continue;
        for (size_t j = i + 1; j < count; j++) {
            if (!(set & (UINT64_C(1) << j))) continue;
            for (size_t a = 0; a < definitions[i].hunk_count; a++) {
                const emu_patch_hunk_t *left = &definitions[i].hunks[a];
                for (size_t b = 0; b < definitions[j].hunk_count; b++) {
                    const emu_patch_hunk_t *right = &definitions[j].hunks[b];
                    if (left->chip_index != right->chip_index) continue;
                    uint64_t left_end =
                        (uint64_t)left->chip_offset + left->size;
                    uint64_t right_end =
                        (uint64_t)right->chip_offset + right->size;
                    uint64_t start = left->chip_offset > right->chip_offset
                                   ? left->chip_offset : right->chip_offset;
                    uint64_t end = left_end < right_end
                                 ? left_end : right_end;
                    for (uint64_t offset = start; offset < end; offset++) {
                        size_t li = (size_t)(offset - left->chip_offset);
                        size_t ri = (size_t)(offset - right->chip_offset);
                        if (left->expected[li] == right->expected[ri] &&
                            left->replacement[li] ==
                                right->replacement[ri])
                            continue;
                        patch_fail(
                            error, EMU_ERR_INVALID_PREPARED_SESSION,
                            "conflicting firmware patches %s and %s at chip %d + 0x%06llX",
                            definitions[i].name, definitions[j].name,
                            left->chip_index,
                            (unsigned long long)offset);
                        return 1;
                    }
                }
            }
        }
    }
    return 0;
}

static int source_hunk_state(const emu_prepared_session_t *prepared,
                             const emu_patch_hunk_t *hunk) {
    const emu_chip_view_t *chip = &prepared->chips[hunk->chip_index];
    const uint8_t *actual = prepared->source.bytes + chip->source_offset +
                            hunk->chip_offset;
    int original = !memcmp(actual, hunk->expected, hunk->size);
    int replacement = !memcmp(actual, hunk->replacement, hunk->size);
    if (original) return 1;
    if (replacement) return 2;
    return 0;
}

static emu_error_code_t validate_selected_definitions(
    const emu_prepared_session_t *prepared,
    const emu_patch_definition_t *definitions, size_t definition_count,
    emu_patch_set_t set, emu_error_t *error) {
    for (size_t i = 0; i < definition_count; i++) {
        if (!(set & (UINT64_C(1) << i))) continue;
        const emu_patch_definition_t *patch = &definitions[i];
        if (!patch->name || !patch->description || !patch->device_name ||
            !patch->image_model || !patch->provenance) {
            return patch_fail(error, EMU_ERR_ARGUMENT,
                              "invalid firmware patch definition");
        }
        if (strcmp(prepared->selected_device, patch->device_name) ||
            strcmp(prepared->metadata.model, patch->image_model) ||
            prepared->metadata.software_version != patch->software_version) {
            return patch_fail(
                error, EMU_ERR_INVALID_PREPARED_SESSION,
                "%s requires device %s with %s SW%d (selected device %s, image %s SW%d)",
                patch->name, patch->device_name, patch->image_model,
                patch->software_version, prepared->selected_device,
                prepared->metadata.model,
                prepared->metadata.software_version);
        }
        if (!patch->hunks || !patch->hunk_count) {
            return patch_fail(error, EMU_ERR_ARGUMENT,
                              "%s has no firmware patch hunks", patch->name);
        }
        for (size_t j = 0; j < patch->hunk_count; j++) {
            const emu_patch_hunk_t *hunk = &patch->hunks[j];
            if (hunk->chip_index < 0 ||
                (size_t)hunk->chip_index >= prepared->chip_count ||
                !hunk->size || !hunk->expected || !hunk->replacement ||
                hunk->chip_offset >
                    prepared->chips[hunk->chip_index].size ||
                hunk->size > prepared->chips[hunk->chip_index].size -
                                     hunk->chip_offset) {
                return patch_fail(
                    error, EMU_ERR_INVALID_PREPARED_SESSION,
                    "%s hunk is out of range at chip %d + 0x%06X",
                    patch->name, hunk->chip_index, hunk->chip_offset);
            }
            const emu_chip_view_t *chip =
                &prepared->chips[hunk->chip_index];
            if (!prepared->source.bytes ||
                chip->source_offset > prepared->source.size ||
                chip->size > prepared->source.size - chip->source_offset ||
                hunk->chip_offset > chip->size ||
                hunk->size > chip->size - hunk->chip_offset) {
                return patch_fail(
                    error, EMU_ERR_INVALID_PREPARED_SESSION,
                    "%s hunk is outside the mapped input at chip %d + 0x%06X",
                    patch->name, hunk->chip_index, hunk->chip_offset);
            }
        }
    }
    return EMU_OK;
}

emu_error_code_t emu_patch_plan_definitions(
    emu_prepared_session_t *prepared,
    const emu_patch_definition_t *definitions, size_t definition_count,
    emu_patch_set_t set, emu_patch_plan_result_t *result,
    emu_error_t *error) {
    emu_patch_plan_result_t local = {0};
    if (result) *result = local;
    if (!set) {
        patch_ok(error);
        return EMU_OK;
    }
    if (!prepared || !definitions || definition_count > 64) {
        return patch_fail(error, EMU_ERR_ARGUMENT,
                          "invalid firmware patch arguments");
    }
    emu_patch_set_t valid_mask = definition_count == 64
                               ? UINT64_MAX
                               : (UINT64_C(1) << definition_count) - 1u;
    if (set & ~valid_mask) {
        return patch_fail(error, EMU_ERR_ARGUMENT,
                          "firmware patch selection is out of range");
    }
    emu_error_code_t code = validate_selected_definitions(
        prepared, definitions, definition_count, set, error);
    if (code != EMU_OK) return code;
    if (definitions_conflict(definitions, definition_count, set, error))
        return error ? error->code : EMU_ERR_INVALID_PREPARED_SESSION;

    size_t first_operation = prepared->operation_count;
    for (size_t i = 0; i < definition_count; i++) {
        if (!(set & (UINT64_C(1) << i))) continue;
        const emu_patch_definition_t *patch = &definitions[i];
        int any_original = 0;
        int any_replacement = 0;
        for (size_t j = 0; j < patch->hunk_count; j++) {
            int state = source_hunk_state(prepared, &patch->hunks[j]);
            if (!state) {
                discard_operations(prepared, first_operation);
                return patch_fail(
                    error, EMU_ERR_INVALID_PREPARED_SESSION,
                    "%s found partial or unexpected bytes at chip %d + 0x%06X",
                    patch->name, patch->hunks[j].chip_index,
                    patch->hunks[j].chip_offset);
            }
            any_original |= state == 1;
            any_replacement |= state == 2;
        }
        if (any_original && any_replacement) {
            discard_operations(prepared, first_operation);
            return patch_fail(
                error, EMU_ERR_INVALID_PREPARED_SESSION,
                "%s is partially applied (hunks contain both original and replacement bytes)",
                patch->name);
        }
        if (any_original) local.applicable |= UINT64_C(1) << i;
        else local.already_applied |= UINT64_C(1) << i;

        size_t group = prepared->operation_count
                     ? prepared->operations[
                           prepared->operation_count - 1u].group + 1u
                     : 1u;
        char provenance[128];
        snprintf(provenance, sizeof provenance, "firmware-patch:%s: %s",
                 patch->name, patch->provenance);
        for (size_t j = 0; j < patch->hunk_count; j++) {
            const emu_patch_hunk_t *hunk = &patch->hunks[j];
            code = emu_prepared_add_grouped_storage_operation(
                prepared, EMU_STORAGE_STAGE_POST_RESTORE, group,
                (size_t)hunk->chip_index, EMU_STORAGE_MAIN_ARRAY,
                hunk->chip_offset, hunk->expected, hunk->size,
                hunk->replacement, hunk->size, provenance, error);
            if (code != EMU_OK) {
                discard_operations(prepared, first_operation);
                return code;
            }
        }
    }
    if (result) *result = local;
    patch_ok(error);
    return EMU_OK;
}

emu_error_code_t emu_patch_plan(emu_prepared_session_t *prepared,
                                emu_patch_set_t set,
                                emu_patch_plan_result_t *result,
                                emu_error_t *error) {
    size_t count = 0;
    const emu_patch_definition_t *catalog = emu_patch_catalog(&count);
    return emu_patch_plan_definitions(prepared, catalog, count, set, result,
                                      error);
}
