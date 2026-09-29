/* GPT2 timer block (core T6 + auxiliary T5). T5 and T6 count through the
 * T6CON block prescaler select (BPS2 at bits[12:11]). */
#ifndef CEMU_PERIPH_GPT2_H
#define CEMU_PERIPH_GPT2_H

#include <stdint.h>
#include "peripheral.h"

typedef struct { uint32_t presc_t6, presc_t5; } gpt2_state_t;

void cemu_gpt2_periph_init(peripheral_t *p, gpt2_state_t *st);

#endif /* CEMU_PERIPH_GPT2_H */
