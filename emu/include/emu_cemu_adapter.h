#ifndef EMU_CEMU_ADAPTER_H
#define EMU_CEMU_ADAPTER_H

#include "emu_engine.h"

extern const emu_engine_descriptor_t emu_cemu_engine_descriptor;

emu_error_code_t emu_cemu_raw_run(const emu_prepared_session_t *prepared,
                                   uint64_t budget, emu_run_result_t *result,
                                   emu_error_t *error);

/* Deterministic adapter allocation-failure hook used by transaction tests. */
void emu_cemu_test_fail_storage_copy_after(int successful_copies);

#endif
