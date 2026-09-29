/* PMB7850 port surface for the currently observed P3/P6/P7/P8 pins.
 *
 * This is intentionally narrow: it models data/direction storage and explicit
 * external pin levels for probes. It does not assign product-specific meaning
 * to the nearby 0xF12x/0xF13x control band. */
#ifndef CEMU_PERIPH_PORTS_H
#define CEMU_PERIPH_PORTS_H

#include <stdint.h>
#include "peripheral.h"

typedef struct {
    uint16_t input_level[4];
    uint16_t input_valid[4];
} ports_state_t;

void cemu_ports_periph_init(peripheral_t *p, ports_state_t *st);

/* Return SOC_PORT_EDGE_* from soc.h. */
int cemu_ports_input_level(ports_state_t *st, soc_t *s, int port, int bit, int level);
void cemu_ports_input_restore_level(ports_state_t *st, int port, int bit, int level);
void cemu_ports_input_release(ports_state_t *st, int port, int bit);

#endif /* CEMU_PERIPH_PORTS_H */
