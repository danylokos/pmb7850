#include "emu_prepare.h"
#include "emu_qemu_snapshot.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static emu_error_code_t prepare_fail(emu_error_t *error,
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

static void prepare_ok(emu_error_t *error) {
    if (error) {
        error->code = EMU_OK;
        error->message[0] = 0;
    }
}

static char *read_document(const char *path, size_t *size_out) {
    FILE *file = fopen(path, "rb");
    if (!file || fseek(file, 0, SEEK_END)) {
        if (file) fclose(file);
        return NULL;
    }
    long length = ftell(file);
    if (length < 0 || fseek(file, 0, SEEK_SET)) {
        fclose(file);
        return NULL;
    }
    char *document = malloc((size_t)length + 1u);
    if (!document) {
        fclose(file);
        return NULL;
    }
    size_t read_size = fread(document, 1, (size_t)length, file);
    int close_result = fclose(file);
    if (read_size != (size_t)length || close_result) {
        free(document);
        return NULL;
    }
    document[length] = 0;
    if (size_out) *size_out = (size_t)length;
    return document;
}

static const char *skip_space(const char *cursor) {
    while (*cursor == ' ' || *cursor == '\t' || *cursor == '\r' ||
           *cursor == '\n')
        cursor++;
    return cursor;
}

static const char *find_json_value(const char *document, const char *key) {
    char pattern[96];
    if (snprintf(pattern, sizeof pattern, "\"%s\"", key) < 0)
        return NULL;
    const char *cursor = strstr(document, pattern);
    if (!cursor) return NULL;
    cursor = skip_space(cursor + strlen(pattern));
    if (*cursor++ != ':') return NULL;
    return skip_space(cursor);
}

static int parse_json_string(const char *cursor, char *output,
                             size_t capacity, const char **end_out) {
    if (!cursor || *cursor++ != '"' || !output || !capacity) return 0;
    size_t used = 0;
    while (*cursor && *cursor != '"') {
        unsigned char value = (unsigned char)*cursor++;
        if (value == '\\') {
            value = (unsigned char)*cursor++;
            if (value != '\\' && value != '"' && value != '/') return 0;
        }
        if (used + 1u >= capacity) return 0;
        output[used++] = (char)value;
    }
    if (*cursor != '"') return 0;
    output[used] = 0;
    if (end_out) *end_out = cursor + 1u;
    return 1;
}

emu_error_code_t emu_snapshot_read_startup_provenance(
    const char *snapshot_path, char *source_path, size_t source_capacity,
    emu_patch_set_t *inherited_patches, emu_error_t *error) {
    if (!snapshot_path || !*snapshot_path || !source_path ||
        !source_capacity)
        return prepare_fail(error, EMU_ERR_ARGUMENT,
                            "invalid snapshot provenance request");
    char path[1024];
    int written = snprintf(path, sizeof path, "%s/snapshot.json",
                           snapshot_path);
    if (written < 0 || (size_t)written >= sizeof path)
        return prepare_fail(error, EMU_ERR_ARGUMENT,
                            "snapshot path is too long");
    char *document = read_document(path, NULL);
    if (!document)
        return prepare_fail(error, EMU_ERR_IO,
                            "cannot read snapshot provenance from %s",
                            snapshot_path);
    char engine[32];
    const char *engine_value = find_json_value(document, "engine");
    if (engine_value && (!parse_json_string(engine_value, engine, sizeof engine, NULL) ||
                        strcmp(engine, "cemu"))) {
        free(document);
        return prepare_fail(error, EMU_ERR_UNSUPPORTED,
                            "snapshot belongs to another engine; use --engine qemu for QEMU snapshots");
    }
    const char *flash = find_json_value(document, "flash");
    if (!parse_json_string(flash, source_path, source_capacity, NULL)) {
        free(document);
        return prepare_fail(error, EMU_ERR_INVALID_PREPARED_SESSION,
                            "snapshot provenance has no valid flash path");
    }

    emu_patch_set_t patches = 0;
    const char *array = find_json_value(document, "firmware_patches");
    if (array) {
        if (*array++ != '[') {
            free(document);
            return prepare_fail(error, EMU_ERR_INVALID_PREPARED_SESSION,
                                "snapshot firmware patch provenance is malformed");
        }
        for (;;) {
            array = skip_space(array);
            if (*array == ']') break;
            char name[96], bad[96];
            const char *end = NULL;
            if (!parse_json_string(array, name, sizeof name, &end) ||
                emu_patch_parse(&patches, name, bad, sizeof bad)) {
                free(document);
                return prepare_fail(
                    error, EMU_ERR_INVALID_PREPARED_SESSION,
                    "snapshot firmware patch provenance is malformed");
            }
            array = skip_space(end);
            if (*array == ',') {
                array++;
                continue;
            }
            if (*array != ']') {
                free(document);
                return prepare_fail(
                    error, EMU_ERR_INVALID_PREPARED_SESSION,
                    "snapshot firmware patch provenance is malformed");
            }
        }
    }
    free(document);
    if (inherited_patches) *inherited_patches = patches;
    prepare_ok(error);
    return EMU_OK;
}

emu_error_code_t emu_prepare_startup(
    const emu_preparation_request_t *request,
    emu_prepared_session_t *prepared, emu_preparation_result_t *result,
    emu_error_t *error) {
    if (!request || !prepared || !result ||
        (unsigned)request->mode > EMU_STARTUP_RESUME ||
        (unsigned)request->identity_source > EMU_IDENTITY_SOURCE_NONE)
        return prepare_fail(error, EMU_ERR_ARGUMENT,
                            "invalid startup preparation request");
    memset(result, 0, sizeof *result);
    const char *source = request->source_path;
    char inherited_source[512];
    emu_qemu_snapshot_t native_snapshot = {0};
    int native_resume = request->mode == EMU_STARTUP_RESUME &&
                        request->runtime_options.qemu_binary[0];
    if (request->mode == EMU_STARTUP_RESUME) {
        if (!request->snapshot_path || !*request->snapshot_path)
            return prepare_fail(error, EMU_ERR_ARGUMENT,
                                "resume preparation requires a snapshot path");
        result->failed_phase = EMU_PREPARATION_PHASE_PROVENANCE;
        emu_error_code_t code;
        if (native_resume) {
            code = emu_qemu_snapshot_read(request->snapshot_path,
                                           &native_snapshot, error);
            snprintf(inherited_source, sizeof inherited_source, "%s",
                     native_snapshot.flash);
            result->inherited_patches = native_snapshot.firmware_patches;
            if (code == EMU_OK &&
                ((request->selected_patches & ~result->inherited_patches) ||
                 (request->runtime_options.sim_stub && !native_snapshot.sim_stub)))
                code = prepare_fail(error, EMU_ERR_INVALID_PREPARED_SESSION,
                                    "QEMU snapshot configuration conflicts with requested patches or SIM");
        } else {
            code = emu_snapshot_read_startup_provenance(
                request->snapshot_path, inherited_source,
                sizeof inherited_source, &result->inherited_patches, error);
        }
        if (code != EMU_OK) return code;
        if (!source || !*source) source = inherited_source;
        snprintf(result->snapshot_path, sizeof result->snapshot_path,
                 "%s", request->snapshot_path);
    }
    if (!source || !*source)
        return prepare_fail(error, EMU_ERR_ARGUMENT,
                            "startup preparation has no flash image");
    snprintf(result->source_path, sizeof result->source_path, "%s", source);
    result->effective_patches = request->selected_patches |
                                result->inherited_patches;

    emu_prepared_init(prepared);
    result->failed_phase = EMU_PREPARATION_PHASE_SOURCE;
    emu_error_code_t code = emu_prepared_load_source(prepared, source, error);
    if (code == EMU_OK) {
        result->failed_phase = native_resume ? EMU_PREPARATION_PHASE_PROVENANCE
                                             : EMU_PREPARATION_PHASE_PRODUCT;
        const char *device = request->requested_device;
        if (native_resume && (!device || !*device))
            device = native_snapshot.device;
        code = emu_product_prepare_image(prepared, device, error);
    }
    prepared->options = request->runtime_options;
    prepared->options.snapshot_path = request->mode == EMU_STARTUP_RESUME
                                   ? request->snapshot_path : NULL;
    prepared->options.firmware_patches = result->effective_patches;
    prepared->options.defer_post_restore_storage =
        request->mode == EMU_STARTUP_RESUME;
    if (code != EMU_OK) {
        emu_prepared_free(prepared);
        return code;
    }
    if (native_resume) {
        result->failed_phase = EMU_PREPARATION_PHASE_PROVENANCE;
        prepared->options.sim_stub = native_snapshot.sim_stub;
        code = emu_qemu_snapshot_validate(request->snapshot_path,
                                           &native_snapshot, prepared, error);
        if (code != EMU_OK) emu_prepared_free(prepared);
        else result->failed_phase = EMU_PREPARATION_PHASE_NONE;
        return code;
    }
    if (request->mode == EMU_STARTUP_FRESH &&
        request->identity_source != EMU_IDENTITY_SOURCE_NONE) {
        result->failed_phase = EMU_PREPARATION_PHASE_IDENTITY;
        if (request->identity_source == EMU_IDENTITY_SOURCE_DEFAULT)
            code = emu_identity_plan_default(prepared, &result->identity,
                                             error);
        else if (request->identity_source == EMU_IDENTITY_SOURCE_FSN)
            code = request->imei
                 ? emu_identity_plan_fsn_imei(
                       prepared, request->fsn, request->imei,
                       "explicit-fsn-imei", &result->identity, error)
                 : emu_identity_plan_fsn(
                       prepared, request->fsn, "explicit-fsn",
                       &result->identity, error);
        else if (!request->identity_path || !*request->identity_path)
            code = prepare_fail(error, EMU_ERR_ARGUMENT,
                                "identity file path is missing");
        else
            code = emu_identity_plan_file(prepared, request->identity_path,
                                          &result->identity, error);
        if (code == EMU_OK) result->identity_planned = 1;
    }
    if (code == EMU_OK) {
        result->failed_phase = EMU_PREPARATION_PHASE_PATCH;
        code = emu_patch_plan(prepared, result->effective_patches, NULL,
                              error);
    }
    if (code != EMU_OK) {
        emu_prepared_free(prepared);
        return code;
    }
    result->failed_phase = EMU_PREPARATION_PHASE_NONE;
    prepare_ok(error);
    return EMU_OK;
}
