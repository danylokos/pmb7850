/* GPT1 timer block — see gpt1.h. */
#include "gpt1.h"
#include "base.h"
#include "soc.h"

#define T2     0xFE40u
#define T2CON  0xFF40u
#define T2IC   0xFF60u
#define T3     0xFE42u
#define T3CON  0xFF42u
#define T3IC   0xFF62u
#define T3CON_OTL (1u << 10)   /* T3 output toggle latch (flips on wrap) */
#define T4     0xFE44u
#define T4CON  0xFF44u
#define T4IC   0xFF64u

static const reg_name_t GPT1_REG_NAMES[] = {
    { T3, "T3" }, { T3CON, "T3CON" }, { T3IC, "T3IC" },
    { T2, "T2" }, { T2CON, "T2CON" }, { T2IC, "T2IC" },
    { T4, "T4" }, { T4CON, "T4CON" }, { T4IC, "T4IC" },
};
/* T2/T3/T4 -> traps 0x22/0x23/0x24 (GPT12E), all timer-driven. */
static const ic_node_t GPT1_IC[] = { {T3IC, 0x23, 1}, {T2IC, 0x22, 1}, {T4IC, 0x24, 1} };

static void gpt1_tick(peripheral_t *self, soc_t *s, int n) {
    gpt1_state_t *st = (gpt1_state_t *)self->state;
    for (int i = 0; i < n; i++) {
        if (cemu_gpt_tick(s, &st->presc_t3, T3, T3CON, T3IC, T3CON, 11, cemu_GPT_BPS1_BASE)) {
            uint16_t t3con = memory_controller_sfr_get(&s->memory, T3CON);
            int rising = (t3con & T3CON_OTL) == 0;
            cemu_memory_controller_sfr_put(&s->memory, T3CON, t3con ^ T3CON_OTL);  /* core T3OTL */
            cemu_gpt_aux_counter_pulse(s, T2, T2CON, T2IC, T3CON, rising);
            cemu_gpt_aux_counter_pulse(s, T4, T4CON, T4IC, T3CON, rising);
        }
        cemu_gpt_aux_tick(s, &st->presc_t2, T2, T2CON, T2IC, T3CON, 11, cemu_GPT_BPS1_BASE, T3CON);
        cemu_gpt_aux_tick(s, &st->presc_t4, T4, T4CON, T4IC, T3CON, 11, cemu_GPT_BPS1_BASE, T3CON);
    }
}
static uint64_t gpt1_next_event(peripheral_t *self, soc_t *s) {
    gpt1_state_t *st = (gpt1_state_t *)self->state;
    uint64_t next = cemu_gpt_next_step(s, st->presc_t3, T3CON, T3CON, 11, cemu_GPT_BPS1_BASE, 0);
    uint64_t t2 = cemu_gpt_next_step(s, st->presc_t2, T2CON, T3CON, 11, cemu_GPT_BPS1_BASE, T3CON);
    uint64_t t4 = cemu_gpt_next_step(s, st->presc_t4, T4CON, T3CON, 11, cemu_GPT_BPS1_BASE, T3CON);
    if (t2 < next) next = t2;
    if (t4 < next) next = t4;
    return next;
}
static void gpt1_advance_quiet(peripheral_t *self, soc_t *s, uint64_t ticks) {
    gpt1_state_t *st = (gpt1_state_t *)self->state;
    cemu_gpt_advance_quiet(s, &st->presc_t3, T3CON, T3CON, 11, cemu_GPT_BPS1_BASE, 0, ticks);
    cemu_gpt_advance_quiet(s, &st->presc_t2, T2CON, T3CON, 11, cemu_GPT_BPS1_BASE, T3CON, ticks);
    cemu_gpt_advance_quiet(s, &st->presc_t4, T4CON, T3CON, 11, cemu_GPT_BPS1_BASE, T3CON, ticks);
}
static int gpt1_timer_running(peripheral_t *self, soc_t *s, uint32_t ic_addr) {
    (void)self;
    if (ic_addr == T3IC) return cemu_gpt_running(s, T3CON);
    if (ic_addr == T2IC) return cemu_gpt_aux_running(s, T2CON, T3CON);
    if (ic_addr == T4IC) return cemu_gpt_aux_running(s, T4CON, T3CON);
    return 0;
}

void cemu_gpt1_periph_init(peripheral_t *p, gpt1_state_t *st) {
    st->presc_t3 = st->presc_t2 = st->presc_t4 = 0;
    p->id = "gpt1"; p->state = st;
    p->sfr_words = NULL; p->n_sfr_words = 0;
    p->byte_ranges = NULL; p->n_byte_ranges = 0;
    p->ic_nodes = GPT1_IC; p->n_ic_nodes = 3;
    PERIPHERAL_REG_NAMES(p, GPT1_REG_NAMES, 9);
    p->read8 = NULL; p->peek8 = NULL; p->write8 = NULL;
    p->read_sfr_word = NULL; p->on_sfr_poll = NULL; p->on_sfr_write = NULL;
    p->tick = gpt1_tick; p->timer_running = gpt1_timer_running;
    p->next_event_ticks = gpt1_next_event; p->advance_quiet = gpt1_advance_quiet;
    p->ic_will_fire = gpt1_timer_running;
}
