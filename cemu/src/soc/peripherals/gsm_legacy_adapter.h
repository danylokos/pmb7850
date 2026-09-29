#ifndef CEMU_PERIPH_GSM_LEGACY_ADAPTER_H
#define CEMU_PERIPH_GSM_LEGACY_ADAPTER_H

#include <stdint.h>
#include "peripheral.h"
#include "devices.h"

/* Temporary parity adapter for the C55 SW24 functional ABI experiment.  It is
 * intentionally a separate peripheral so the measured baseband transport and
 * pure virtual-cell policy do not acquire CPU or UI knowledge. */
typedef struct {
    int enabled;
    int restored;
    uint32_t adapter_pc;
    uint32_t home_plmn_addr;
    uint32_t network_state_addr;
    uint32_t candidate_ptr_addr;
    uint32_t candidate_seg_addr;
    uint32_t signal_setter_pc;
    uint16_t signal_level;
    uint64_t min_icount;
    uint64_t registration_events;
    uint64_t signal_events;
    uint32_t return_pc;
} gsm_legacy_adapter_state_t;

void cemu_gsm_legacy_adapter_periph_init(
    peripheral_t *p, gsm_legacy_adapter_state_t *st,
    const device_gsm_legacy_adapter_config_t *cfg);
void cemu_gsm_legacy_adapter_set_enabled(peripheral_t *p, int enabled);

#endif /* CEMU_PERIPH_GSM_LEGACY_ADAPTER_H */
