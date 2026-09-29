/* CAPCOM2 block (T7/T8 + CC16..CC31) — common-core CAPCOM behavior backed by
 * the C167-family timer/capture/compare semantics used by the PMB7850-visible
 * register surface. */
#ifndef CEMU_PERIPH_CAPCOM2_H
#define CEMU_PERIPH_CAPCOM2_H

#include <stdint.h>
#include "peripheral.h"
#include "capcom_common.h"

typedef struct { capcom_unit_state_t unit; } capcom2_state_t;

void cemu_capcom2_periph_init(peripheral_t *p, capcom2_state_t *st,
                         int cc23_irq_enabled);
void cemu_capcom2_inject_timer_edge(capcom2_state_t *st, soc_t *s, int timer_idx, int rising);
void cemu_capcom2_inject_channel_edge(capcom2_state_t *st, soc_t *s, int local_channel, int rising);
int cemu_capcom2_output_level(capcom2_state_t *st, int local_channel);
int cemu_capcom2_output_driven(soc_t *s, int local_channel);

#endif /* CEMU_PERIPH_CAPCOM2_H */
