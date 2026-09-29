/* Shared GPT12/CAPCOM timer primitives — see base.h. */
#include "base.h"
#include "soc.h"

#define CON_RUN (1u << 6)
#define CON_UD  (1u << 7)
#define CON_RC  (1u << 9)

const int cemu_GPT_BPS1_BASE[4] = {8, 4, 32, 16};   /* GPT1 (T3/T2/T4), Tables 12-2/12-7  */
const int cemu_GPT_BPS2_BASE[4] = {4, 2, 16, 8};    /* GPT2 (T6/T5),    Tables 12-11/12-13 */

int cemu_capcom_tick(soc_t *s, uint32_t *presc, uint32_t counter, uint32_t reload_addr,
                uint32_t con_addr, int shift, uint32_t ic_addr) {
    uint16_t con = memory_controller_sfr_get(&s->memory, con_addr);
    uint16_t run_bit = (uint16_t)(1u << (shift + 6));
    uint16_t mode_bit = (uint16_t)(1u << (shift + 3));
    if (!(con & run_bit) || (con & mode_bit)) return 0;   /* run + Timer Mode */

    *presc += 1;
    uint32_t div = 1u << (((con >> shift) & 0x7) + 3);   /* 2**(TxI+3) */
    if (*presc < div) return 0;
    uint32_t steps = *presc / div;
    *presc %= div;
    if (!steps) return 0;

    uint32_t value = memory_controller_sfr_get(&s->memory, counter);
    uint32_t reload = memory_controller_sfr_get(&s->memory, reload_addr);
    uint32_t span = 0x10000u - reload;
    uint32_t total = value + steps;
    if (total > 0xFFFFu) {
        value = span ? reload + (total - 0x10000u) % span : reload;
        cemu_memory_controller_sfr_put(&s->memory, ic_addr, memory_controller_sfr_get(&s->memory, ic_addr) | XIC_IR_BIT);
        cemu_memory_controller_sfr_put(&s->memory, counter, value & 0xFFFFu);
        return 1;
    }
    cemu_memory_controller_sfr_put(&s->memory, counter, total & 0xFFFFu);
    return 0;
}

int cemu_capcom_running(soc_t *s, uint32_t con_addr, int shift) {
    uint16_t con = memory_controller_sfr_get(&s->memory, con_addr);
    return (con & (1u << (shift + 6))) && !(con & (1u << (shift + 3)));
}

static int gpt_run_selected(soc_t *s, uint16_t con, uint32_t remote_con_addr) {
    if (remote_con_addr && (con & CON_RC))
        return (memory_controller_sfr_get(&s->memory, remote_con_addr) & CON_RUN) != 0;
    return (con & CON_RUN) != 0;
}

static int gpt_count_step(soc_t *s, uint32_t counter, uint32_t con_addr,
                          uint32_t ic_addr) {
    uint16_t con = memory_controller_sfr_get(&s->memory, con_addr);
    uint16_t value = memory_controller_sfr_get(&s->memory, counter);
    int wrapped;
    uint32_t raw;
    if (con & CON_UD) { raw = value - 1u; wrapped = value == 0; }
    else              { raw = value + 1u; wrapped = value == 0xFFFFu; }
    cemu_memory_controller_sfr_put(&s->memory, counter, raw & 0xFFFFu);
    if (wrapped) cemu_memory_controller_sfr_put(&s->memory, ic_addr, memory_controller_sfr_get(&s->memory, ic_addr) | XIC_IR_BIT);
    return wrapped;
}

static int gpt_tick_common(soc_t *s, uint32_t *presc, uint32_t counter, uint32_t con_addr,
                           uint32_t ic_addr, uint32_t bps_reg, int bps_shift,
                           const int *bps_base, uint32_t remote_con_addr) {
    uint16_t con = memory_controller_sfr_get(&s->memory, con_addr);
    if (!gpt_run_selected(s, con, remote_con_addr) || ((con >> 3) & 0x7) != 0)
        return 0;   /* run + Timer Mode */
    int bps = (memory_controller_sfr_get(&s->memory, bps_reg) >> bps_shift) & 0x3;
    uint32_t div = (uint32_t)bps_base[bps] << (con & 0x7);
    *presc += 1;
    if (*presc < div) return 0;
    uint32_t steps = *presc / div;
    *presc %= div;
    if (!steps) return 0;
    uint16_t value = memory_controller_sfr_get(&s->memory, counter);
    int wrapped;
    uint32_t raw;
    if (con & CON_UD) { raw = value - steps; wrapped = ((int32_t)raw < 0); }
    else              { raw = value + steps; wrapped = (raw > 0xFFFF); }
    cemu_memory_controller_sfr_put(&s->memory, counter, raw & 0xFFFF);
    if (wrapped) cemu_memory_controller_sfr_put(&s->memory, ic_addr, memory_controller_sfr_get(&s->memory, ic_addr) | XIC_IR_BIT);
    return wrapped;
}

int cemu_gpt_tick(soc_t *s, uint32_t *presc, uint32_t counter, uint32_t con_addr,
             uint32_t ic_addr, uint32_t bps_reg, int bps_shift, const int *bps_base) {
    return gpt_tick_common(s, presc, counter, con_addr, ic_addr, bps_reg, bps_shift,
                           bps_base, 0);
}

int cemu_gpt_aux_tick(soc_t *s, uint32_t *presc, uint32_t counter, uint32_t con_addr,
                 uint32_t ic_addr, uint32_t bps_reg, int bps_shift,
                 const int *bps_base, uint32_t remote_con_addr) {
    return gpt_tick_common(s, presc, counter, con_addr, ic_addr, bps_reg, bps_shift,
                           bps_base, remote_con_addr);
}

uint64_t cemu_gpt_next_step(soc_t *s, uint32_t presc, uint32_t con_addr,
                       uint32_t bps_reg, int bps_shift,
                       const int *bps_base, uint32_t remote_con_addr) {
    uint16_t con = memory_controller_sfr_get(&s->memory, con_addr);
    if (!gpt_run_selected(s, con, remote_con_addr) || ((con >> 3) & 0x7) != 0)
        return UINT64_MAX;
    int bps = (memory_controller_sfr_get(&s->memory, bps_reg) >> bps_shift) & 0x3;
    uint32_t div = (uint32_t)bps_base[bps] << (con & 0x7);
    return presc >= div ? 1 : div - presc;
}

void cemu_gpt_advance_quiet(soc_t *s, uint32_t *presc, uint32_t con_addr,
                       uint32_t bps_reg, int bps_shift,
                       const int *bps_base, uint32_t remote_con_addr,
                       uint64_t ticks) {
    uint16_t con = memory_controller_sfr_get(&s->memory, con_addr);
    if (!gpt_run_selected(s, con, remote_con_addr) || ((con >> 3) & 0x7) != 0)
        return;
    int bps = (memory_controller_sfr_get(&s->memory, bps_reg) >> bps_shift) & 0x3;
    uint32_t div = (uint32_t)bps_base[bps] << (con & 0x7);
    uint64_t next = *presc >= div ? 1 : div - *presc;
    if (ticks < next) *presc += (uint32_t)ticks;
}

int cemu_gpt_aux_counter_pulse(soc_t *s, uint32_t counter, uint32_t con_addr,
                          uint32_t ic_addr, uint32_t remote_con_addr, int rising) {
    uint16_t con = memory_controller_sfr_get(&s->memory, con_addr);
    if (!gpt_run_selected(s, con, remote_con_addr) || ((con >> 3) & 0x7) != 1)
        return 0;   /* run + Counter Mode */
    switch (con & 0x7) {
    case 5: if (!rising) return 0; break;   /* positive T3OTL/T6OTL transition */
    case 6: if (rising)  return 0; break;   /* negative T3OTL/T6OTL transition */
    case 7: break;                          /* any T3OTL/T6OTL transition */
    default: return 0;                      /* none or external TxIN source */
    }
    return gpt_count_step(s, counter, con_addr, ic_addr);
}

int cemu_gpt_running(soc_t *s, uint32_t con_addr) {
    uint16_t con = memory_controller_sfr_get(&s->memory, con_addr);
    return (con & CON_RUN) && ((con >> 3) & 0x7) == 0;
}

int cemu_gpt_aux_running(soc_t *s, uint32_t con_addr, uint32_t remote_con_addr) {
    uint16_t con = memory_controller_sfr_get(&s->memory, con_addr);
    return gpt_run_selected(s, con, remote_con_addr) && ((con >> 3) & 0x7) == 0;
}
