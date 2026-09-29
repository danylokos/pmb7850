/* C166S Peripheral Event Controller.
 *
 * This models the V1 short-transfer path plus the PMB7850 extended long
 * counters on channels 0 and 2. Channel linking remains unimplemented.
 */
#ifndef CEMU_PERIPH_PEC_H
#define CEMU_PERIPH_PEC_H

#include <stdint.h>
#include "peripheral.h"

typedef struct pec_engine {
    int unused;
} pec_engine_t;

typedef struct {
    int channel;
    int is_long;
    int size;
    int count_before;
    int count_after;
    int clear_source_ir;
    uint32_t ic_addr;
    uint32_t counter_addr;
    uint32_t src;
    uint32_t dst;
    uint16_t pecc;
    uint16_t src_off;
    uint16_t dst_off;
} pec_transfer_t;

void cemu_pec_endpoint_init(peripheral_t *p, pec_engine_t *pec);

/* Prepare one selected request without mutating state. The interrupt subsystem
 * applies the source-IR disposition before executing the transfer. */
int cemu_pec_prepare_interrupt(
    pec_engine_t *pec, soc_t *s, uint32_t ic_addr, int ilvl,
    pec_transfer_t *transfer);
void cemu_pec_execute_transfer(
    pec_engine_t *pec, soc_t *s, const pec_transfer_t *transfer);

/* Return 1 when a finite, supported transfer can consume a pending or
 * autonomously scheduled interrupt source under the supplied CPU PSW. */
int cemu_pec_channel_has_finite_transfer(
    pec_engine_t *pec, soc_t *s, uint32_t ic_addr, int ilvl);

#endif /* CEMU_PERIPH_PEC_H */
