#ifndef EMU_CEMU_DRCOV_ADAPTER_H
#define EMU_CEMU_DRCOV_ADAPTER_H

#include "emu_drcov.h"

typedef struct cemu_core cemu_core_t;
typedef struct emu_cemu_drcov_adapter emu_cemu_drcov_adapter_t;

/* Convert CEMU's engine-owned memory regions to shared exclusive modules. */
emu_drcov_result_t emu_cemu_drcov_create(emu_drcov_t **collector);

/* Subscribe native instruction events through the opaque core. */
emu_drcov_result_t emu_cemu_drcov_attach_core(
    emu_drcov_t *collector, cemu_core_t *core,
    emu_cemu_drcov_adapter_t **adapter);

/* The first event-translation or collection failure remains latched. */
emu_drcov_result_t emu_cemu_drcov_result(
    const emu_cemu_drcov_adapter_t *adapter);

/* Unsubscribe, free the adapter, and clear the caller's pointer. */
void emu_cemu_drcov_detach(emu_cemu_drcov_adapter_t **adapter);

#endif
