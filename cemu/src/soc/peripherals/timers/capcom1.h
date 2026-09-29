/* CAPCOM1 block (T0/T1 + CC0..CC15) — common-core CAPCOM behavior backed by
 * the C167-family timer/capture/compare semantics used by the PMB7850-visible
 * register surface. */
#ifndef CEMU_PERIPH_CAPCOM1_H
#define CEMU_PERIPH_CAPCOM1_H

#include <stdint.h>
#include "peripheral.h"
#include "capcom_common.h"

typedef struct { capcom_unit_state_t unit; } capcom1_state_t;

void cemu_capcom1_periph_init(peripheral_t *p, capcom1_state_t *st);
void cemu_capcom1_inject_timer_edge(capcom1_state_t *st, soc_t *s, int timer_idx, int rising);
void cemu_capcom1_inject_channel_edge(capcom1_state_t *st, soc_t *s, int local_channel, int rising);
int cemu_capcom1_output_level(capcom1_state_t *st, int local_channel);
int cemu_capcom1_output_driven(soc_t *s, int local_channel);

#endif /* CEMU_PERIPH_CAPCOM1_H */
