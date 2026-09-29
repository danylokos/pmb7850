#ifndef EMU_MANIFEST_H
#define EMU_MANIFEST_H

#include "emu_engine.h"
#include "emu_prepare.h"

typedef struct {
    char kind[32];
    char path[256];
    uint64_t size;
    char sha256[EMU_SHA256_HEX_SIZE];
} emu_artifact_t;

typedef struct {
    const emu_prepared_session_t *prepared;
    const emu_engine_descriptor_t *engine;
    emu_run_result_t result;
    unsigned schema_version;
    emu_startup_mode_t startup_mode;
    emu_identity_source_t identity_source;
    char snapshot_path[512];
    emu_patch_set_t inherited_patches;
    emu_patch_set_t selected_patches;
    emu_patch_set_t effective_patches;
    emu_identity_plan_result_t identity;
    int identity_planned;
    char serial_transport[24];
    char frame_transport[24];
    char event_transport[24];
    char artifact_state[24];
    int explicit_writeback;
    emu_artifact_t artifacts[EMU_MAX_ARTIFACTS];
    size_t artifact_count;
} emu_manifest_t;

void emu_manifest_init(emu_manifest_t *manifest,
                       const emu_prepared_session_t *prepared,
                       const emu_engine_descriptor_t *engine,
                       const emu_run_result_t *result);
void emu_manifest_init_v2(emu_manifest_t *manifest,
                          const emu_prepared_session_t *prepared,
                          const emu_engine_descriptor_t *engine,
                          const emu_run_result_t *result,
                          const emu_preparation_request_t *request,
                          const emu_preparation_result_t *preparation);
emu_error_code_t emu_manifest_add_artifact(emu_manifest_t *manifest,
                                            const char *kind, const char *path,
                                            uint64_t size, const char *sha256,
                                            emu_error_t *error);
emu_error_code_t emu_manifest_serialize(const emu_manifest_t *manifest,
                                         char **json, size_t *size,
                                         emu_error_t *error);

#endif
