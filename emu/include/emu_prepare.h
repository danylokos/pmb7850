#ifndef EMU_PREPARE_H
#define EMU_PREPARE_H

#include "emu_identity.h"
#include "emu_patch.h"
#include "emu_product.h"

typedef enum {
    EMU_STARTUP_FRESH = 0,
    EMU_STARTUP_RESUME,
} emu_startup_mode_t;

typedef enum {
    EMU_IDENTITY_SOURCE_DEFAULT = 0,
    EMU_IDENTITY_SOURCE_FSN,
    EMU_IDENTITY_SOURCE_FILE,
    EMU_IDENTITY_SOURCE_NONE,
} emu_identity_source_t;

typedef enum {
    EMU_PREPARATION_PHASE_NONE = 0,
    EMU_PREPARATION_PHASE_PROVENANCE,
    EMU_PREPARATION_PHASE_SOURCE,
    EMU_PREPARATION_PHASE_PRODUCT,
    EMU_PREPARATION_PHASE_IDENTITY,
    EMU_PREPARATION_PHASE_PATCH,
} emu_preparation_phase_t;

typedef struct {
    emu_startup_mode_t mode;
    const char *source_path;
    const char *requested_device;
    const char *snapshot_path;
    emu_identity_source_t identity_source;
    const char *identity_path;
    uint32_t fsn;
    const char *imei;
    emu_patch_set_t selected_patches;
    emu_runtime_options_t runtime_options;
} emu_preparation_request_t;

typedef struct {
    char source_path[512];
    char snapshot_path[512];
    emu_patch_set_t inherited_patches;
    emu_patch_set_t effective_patches;
    emu_identity_plan_result_t identity;
    int identity_planned;
    emu_preparation_phase_t failed_phase;
} emu_preparation_result_t;

emu_error_code_t emu_snapshot_read_startup_provenance(
    const char *snapshot_path, char *source_path, size_t source_capacity,
    emu_patch_set_t *inherited_patches, emu_error_t *error);
emu_error_code_t emu_prepare_startup(
    const emu_preparation_request_t *request,
    emu_prepared_session_t *prepared, emu_preparation_result_t *result,
    emu_error_t *error);

#endif
