/* GPT1 timer block (core T3 + auxiliary T2/T4). T3 toggles T3OTL on wrap;
 * T2 and T4 share the T3CON block prescaler select (BPS1 at bits[12:11]). */
#ifndef CEMU_PERIPH_GPT1_H
#define CEMU_PERIPH_GPT1_H

#include <stdint.h>
#include "peripheral.h"

typedef struct { uint32_t presc_t3, presc_t2, presc_t4; } gpt1_state_t;

void cemu_gpt1_periph_init(peripheral_t *p, gpt1_state_t *st);

#endif /* CEMU_PERIPH_GPT1_H */
