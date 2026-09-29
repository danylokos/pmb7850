/* Shared timer primitives — C mirror of emu/src/soc/peripherals/timers/base.py.
 *
 * The GPT12E timers (T2/T3/T4 in GPT1, T5/T6 in GPT2) share one prescaler-through-
 * block-control-register count path; CAPCOM T0/T1/T7/T8 are separate reload
 * counters with pair-shared control words. Each timer's counter value lives in
 * the memory controller's SFR store; a block owns only its timers' private
 * prescaler-phase accumulators.
 * One instruction-tick per tick(1): the ratio between timers is faithful,
 * absolute Hz is not. */
#ifndef CEMU_PERIPH_TIMERS_BASE_H
#define CEMU_PERIPH_TIMERS_BASE_H

#include <stdint.h>
#include "peripheral.h"

/* GPT block prescaler-base tables (BPS field -> base divisor). */
extern const int cemu_GPT_BPS1_BASE[4];   /* GPT1 (T3/T2/T4), c166s_v1 Tables 12-2/12-7  */
extern const int cemu_GPT_BPS2_BASE[4];   /* GPT2 (T6/T5),    c166s_v1 Tables 12-11/12-13 */

/* Advance one CAPCOM core timer (T0/T1/T7/T8). Each timer owns its own reload
 * register and interrupt-control word, while two timers share one control word:
 * `shift` selects the timer half (0 for low byte, 8 for high byte). Returns 1
 * on overflow/reload and sets the timer's xIC request flag. */
int cemu_capcom_tick(soc_t *s, uint32_t *presc, uint32_t counter, uint32_t reload_addr,
                uint32_t con_addr, int shift, uint32_t ic_addr);

/* CAPCOM-timer running predicate: run bit set and in Timer Mode (TxM==0). */
int cemu_capcom_running(soc_t *s, uint32_t con_addr, int shift);

/* Advance one GPT12 timer through its block prescaler; returns 1 on wrap (and
 * sets the timer's xIC request flag). Counter/CON/xIC are SFR word addresses;
 * bps_reg/bps_shift select the 2-bit BPS field in the block control register. */
int cemu_gpt_tick(soc_t *s, uint32_t *presc, uint32_t counter, uint32_t con_addr,
             uint32_t ic_addr, uint32_t bps_reg, int bps_shift, const int *bps_base);

/* Advance one GPT12 auxiliary timer in Timer Mode. If bit TxRC is set, the timer
 * run source is the core timer's run bit in remote_con_addr, not its own TxR bit. */
int cemu_gpt_aux_tick(soc_t *s, uint32_t *presc, uint32_t counter, uint32_t con_addr,
                 uint32_t ic_addr, uint32_t bps_reg, int bps_shift,
                 const int *bps_base, uint32_t remote_con_addr);

uint64_t cemu_gpt_next_step(soc_t *s, uint32_t presc, uint32_t con_addr,
                       uint32_t bps_reg, int bps_shift,
                       const int *bps_base, uint32_t remote_con_addr);
void cemu_gpt_advance_quiet(soc_t *s, uint32_t *presc, uint32_t con_addr,
                       uint32_t bps_reg, int bps_shift,
                       const int *bps_base, uint32_t remote_con_addr,
                       uint64_t ticks);

/* Apply one internal core-toggle edge to an auxiliary timer in Counter Mode
 * (TxI selects T3OTL/T6OTL rising/falling/both). External TxIN edges are not
 * synthesized here. */
int cemu_gpt_aux_counter_pulse(soc_t *s, uint32_t counter, uint32_t con_addr,
                          uint32_t ic_addr, uint32_t remote_con_addr, int rising);

/* GPT-timer running predicate: run bit set and in Timer Mode (TxM==0). */
int cemu_gpt_running(soc_t *s, uint32_t con_addr);

/* Auxiliary GPT running predicate with TxRC remote run control. */
int cemu_gpt_aux_running(soc_t *s, uint32_t con_addr, uint32_t remote_con_addr);

#endif /* CEMU_PERIPH_TIMERS_BASE_H */
