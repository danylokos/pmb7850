#ifndef CEMU_PERIPH_CAPCOM_COMMON_H
#define CEMU_PERIPH_CAPCOM_COMMON_H

#include <stdint.h>
#include "peripheral.h"

typedef struct {
    uint32_t prescaler;
    uint8_t compare_ready;
    uint8_t input_level;
} capcom_timer_state_t;

typedef struct {
    uint8_t period_blocked;
    uint8_t output_level;
    uint8_t input_level;
} capcom_channel_state_t;

typedef struct {
    capcom_timer_state_t timers[2];
    capcom_channel_state_t channels[16];
    uint16_t irq_output_mask;
} capcom_unit_state_t;

typedef struct {
    const char *id;
    const char *timer_name[2];
    uint32_t con_addr;
    uint32_t timer_addr[2];
    uint32_t reload_addr[2];
    uint32_t timer_ic_addr[2];
    uint32_t ccm_addr[4];
    uint32_t cc_addr[16];
    uint32_t cc_ic_addr[16];
    int channel_count;
    int base_channel;
    int no_output_lo;
    int no_output_hi;
} capcom_unit_desc_t;

void cemu_capcom_unit_reset_state(capcom_unit_state_t *st);
void cemu_capcom_unit_set_channel_irq_output(capcom_unit_state_t *st,
                                        int local_channel, int enabled);
void cemu_capcom_unit_tick(capcom_unit_state_t *st, soc_t *s, const capcom_unit_desc_t *desc, int n);
void cemu_capcom_unit_on_sfr_write(capcom_unit_state_t *st, soc_t *s, const capcom_unit_desc_t *desc,
                              uint32_t word_addr, uint16_t stored);
int cemu_capcom_unit_timer_running(capcom_unit_state_t *st, soc_t *s, const capcom_unit_desc_t *desc,
                              uint32_t ic_addr);
int cemu_capcom_unit_ic_will_fire(capcom_unit_state_t *st, soc_t *s, const capcom_unit_desc_t *desc,
                             uint32_t ic_addr);
uint64_t cemu_capcom_unit_next_event(capcom_unit_state_t *st, soc_t *s,
                                const capcom_unit_desc_t *desc);
void cemu_capcom_unit_advance_quiet(capcom_unit_state_t *st, soc_t *s,
                               const capcom_unit_desc_t *desc, uint64_t ticks);
void cemu_capcom_unit_inject_timer_edge(capcom_unit_state_t *st, soc_t *s, const capcom_unit_desc_t *desc,
                                   int timer_idx, int rising);
void cemu_capcom_unit_inject_channel_edge(capcom_unit_state_t *st, soc_t *s, const capcom_unit_desc_t *desc,
                                     int local_channel, int rising);
int cemu_capcom_unit_output_level(const capcom_unit_state_t *st, const capcom_unit_desc_t *desc,
                             int local_channel);
int cemu_capcom_unit_output_driven(soc_t *s, const capcom_unit_desc_t *desc,
                              int local_channel);

#endif /* CEMU_PERIPH_CAPCOM_COMMON_H */
