#ifndef EMU_CEMU_STORAGE_H
#define EMU_CEMU_STORAGE_H

#include "emu_patch.h"

typedef struct soc soc_t;

typedef struct {
    emu_patch_set_t applied;
    emu_patch_set_t already_applied;
} emu_cemu_storage_result_t;

emu_error_code_t emu_cemu_apply_storage_stage(
    const emu_prepared_session_t *prepared, emu_storage_stage_t stage,
    soc_t *soc, emu_cemu_storage_result_t *result, emu_error_t *error);

/* Allocation-failure hook used by the CEMU adapter contract tests. */
void emu_cemu_test_fail_storage_copy_after(int successful_copies);

#endif
