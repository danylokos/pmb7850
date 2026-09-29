/* GPT2 timer block — see gpt2.h. */
#include "gpt2.h"
#include "base.h"
#include "soc.h"

#define T5     0xFE46u
#define T5CON  0xFF46u
#define T5IC   0xFF66u
#define T6     0xFE48u
#define T6CON  0xFF48u
#define T6IC   0xFF68u
#define T6CON_OTL (1u << 10)   /* T6 output toggle latch (flips on wrap) */

static const reg_name_t GPT2_REG_NAMES[] = {
    { T5, "T5" }, { T5CON, "T5CON" }, { T5IC, "T5IC" },
    { T6, "T6" }, { T6CON, "T6CON" }, { T6IC, "T6IC" },
};
/* T5/T6 -> traps 0x25/0x26 (GPT12E), both timer-driven. */
static const ic_node_t GPT2_IC[] = { {T6IC, 0x26, 1}, {T5IC, 0x25, 1} };

static void gpt2_tick(peripheral_t *self, soc_t *s, int n) {
    gpt2_state_t *st = (gpt2_state_t *)self->state;
    for (int i = 0; i < n; i++) {
        if (cemu_gpt_tick(s, &st->presc_t6, T6, T6CON, T6IC, T6CON, 11, cemu_GPT_BPS2_BASE)) {
            uint16_t t6con = memory_controller_sfr_get(&s->memory, T6CON);
            int rising = (t6con & T6CON_OTL) == 0;
            cemu_memory_controller_sfr_put(&s->memory, T6CON, t6con ^ T6CON_OTL);  /* core T6OTL */
            cemu_gpt_aux_counter_pulse(s, T5, T5CON, T5IC, T6CON, rising);
            s->capcom_t6_pulse = 1;
        }
        cemu_gpt_aux_tick(s, &st->presc_t5, T5, T5CON, T5IC, T6CON, 11, cemu_GPT_BPS2_BASE, T6CON);
    }
}
static uint64_t gpt2_next_event(peripheral_t *self, soc_t *s) {
    gpt2_state_t *st = (gpt2_state_t *)self->state;
    uint64_t next = cemu_gpt_next_step(s, st->presc_t6, T6CON, T6CON, 11, cemu_GPT_BPS2_BASE, 0);
    uint64_t t5 = cemu_gpt_next_step(s, st->presc_t5, T5CON, T6CON, 11, cemu_GPT_BPS2_BASE, T6CON);
    return t5 < next ? t5 : next;
}
static void gpt2_advance_quiet(peripheral_t *self, soc_t *s, uint64_t ticks) {
    gpt2_state_t *st = (gpt2_state_t *)self->state;
    cemu_gpt_advance_quiet(s, &st->presc_t6, T6CON, T6CON, 11, cemu_GPT_BPS2_BASE, 0, ticks);
    cemu_gpt_advance_quiet(s, &st->presc_t5, T5CON, T6CON, 11, cemu_GPT_BPS2_BASE, T6CON, ticks);
}
static int gpt2_timer_running(peripheral_t *self, soc_t *s, uint32_t ic_addr) {
    (void)self;
    if (ic_addr == T6IC) return cemu_gpt_running(s, T6CON);
    if (ic_addr == T5IC) return cemu_gpt_aux_running(s, T5CON, T6CON);
    return 0;
}

void cemu_gpt2_periph_init(peripheral_t *p, gpt2_state_t *st) {
    st->presc_t6 = st->presc_t5 = 0;
    p->id = "gpt2"; p->state = st;
    p->sfr_words = NULL; p->n_sfr_words = 0;
    p->byte_ranges = NULL; p->n_byte_ranges = 0;
    p->ic_nodes = GPT2_IC; p->n_ic_nodes = 2;
    PERIPHERAL_REG_NAMES(p, GPT2_REG_NAMES, 6);
    p->read8 = NULL; p->peek8 = NULL; p->write8 = NULL;
    p->read_sfr_word = NULL; p->on_sfr_poll = NULL; p->on_sfr_write = NULL;
    p->tick = gpt2_tick; p->timer_running = gpt2_timer_running;
    p->next_event_ticks = gpt2_next_event; p->advance_quiet = gpt2_advance_quiet;
    p->ic_will_fire = gpt2_timer_running;
}
