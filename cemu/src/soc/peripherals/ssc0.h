/* SSC0 high-speed synchronous serial channel. */
#ifndef CEMU_PERIPH_SSC0_H
#define CEMU_PERIPH_SSC0_H

#include <stdint.h>
#include "peripheral.h"

typedef void (*ssc0_slave_start_fn)(void *ctx, soc_t *s, uint16_t tx,
                                    unsigned frame_bits, int msb_first);
typedef uint16_t (*ssc0_slave_complete_fn)(void *ctx, soc_t *s, uint16_t tx,
                                           unsigned frame_bits, int msb_first);
typedef void (*ssc0_slave_abort_fn)(void *ctx, soc_t *s);

typedef struct {
    uint16_t config;
    uint16_t status;
    int rb_full;

    int shift_active;
    uint16_t shift_tx;
    unsigned shift_bits;
    int shift_msb_first;
    uint64_t shift_start_tick;
    uint64_t completion_tick;

    int tb_full;
    uint16_t tb;

    ssc0_slave_start_fn slave_start;
    ssc0_slave_complete_fn slave_complete;
    ssc0_slave_abort_fn slave_abort;
    void *slave_ctx;
} ssc0_state_t;

void cemu_ssc0_periph_init(peripheral_t *p, ssc0_state_t *st);
void cemu_ssc0_attach_slave(peripheral_t *p,
                       ssc0_slave_start_fn start,
                       ssc0_slave_complete_fn complete,
                       ssc0_slave_abort_fn abort,
                       void *ctx);

#endif /* CEMU_PERIPH_SSC0_H */
