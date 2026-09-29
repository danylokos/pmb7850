/* CAPCOM2 block — T7/T8 plus CC16..CC31. */
#include "capcom2.h"
#include "capcom_common.h"
#include "soc.h"

#define T7     0xF050u
#define T8     0xF052u
#define T7REL  0xF054u
#define T8REL  0xF056u
#define CC16   0xFE60u
#define CC17   0xFE62u
#define CC18   0xFE64u
#define CC19   0xFE66u
#define CC20   0xFE68u
#define CC21   0xFE6Au
#define CC22   0xFE6Cu
#define CC23   0xFE6Eu
#define CC24   0xFE70u
#define CC25   0xFE72u
#define CC26   0xFE74u
#define CC27   0xFE76u
#define CC28   0xFE78u
#define CC29   0xFE7Au
#define CC30   0xFE7Cu
#define CC31   0xFE7Eu
#define T78CON 0xFF20u
#define CCM4   0xFF22u
#define CCM5   0xFF24u
#define CCM6   0xFF26u
#define CCM7   0xFF28u
#define CC16IC 0xF160u
#define CC17IC 0xF162u
#define CC18IC 0xF164u
#define CC19IC 0xF166u
#define CC20IC 0xF168u
#define CC21IC 0xF16Au
#define CC22IC 0xF16Cu
#define IRQ55IC 0xF16Eu
#define CC24IC 0xF170u
#define CC25IC 0xF172u
#define CC26IC 0xF174u
#define CC27IC 0xF176u
#define CC28IC 0xF178u
#define T7IC   0xF17Au
#define T8IC   0xF17Cu
#define CC29IC 0xF184u
#define CC30IC 0xF18Cu
#define CC31IC 0xF194u

static const uint32_t CAPCOM2_SFR_WORDS[] = {
    T7, T8, T7REL, T8REL,
    CC16, CC17, CC18, CC19, CC20, CC21, CC22, CC23,
    CC24, CC25, CC26, CC27, CC28, CC29, CC30, CC31,
    T78CON, CCM4, CCM5, CCM6, CCM7,
    CC16IC, CC17IC, CC18IC, CC19IC, CC20IC, CC21IC, CC22IC, IRQ55IC,
    CC24IC, CC25IC, CC26IC, CC27IC, CC28IC, T7IC, T8IC
};

static const reg_name_t CAPCOM2_REG_NAMES[] = {
    { T7, "T7" }, { T8, "T8" }, { T7REL, "T7REL" }, { T8REL, "T8REL" },
    { CC16, "CC16" }, { CC17, "CC17" }, { CC18, "CC18" }, { CC19, "CC19" },
    { CC20, "CC20" }, { CC21, "CC21" }, { CC22, "CC22" }, { CC23, "CC23" },
    { CC24, "CC24" }, { CC25, "CC25" }, { CC26, "CC26" }, { CC27, "CC27" },
    { CC28, "CC28" }, { CC29, "CC29" }, { CC30, "CC30" }, { CC31, "CC31" },
    { T78CON, "T78CON" }, { CCM4, "CCM4" }, { CCM5, "CCM5" }, { CCM6, "CCM6" }, { CCM7, "CCM7" },
    { CC16IC, "CC16IC" }, { CC17IC, "CC17IC" }, { CC18IC, "CC18IC" }, { CC19IC, "CC19IC" },
    { CC20IC, "CC20IC" }, { CC21IC, "CC21IC" }, { CC22IC, "CC22IC" }, { IRQ55IC, "IRQ55IC" },
    { CC24IC, "CC24IC" }, { CC25IC, "CC25IC" }, { CC26IC, "CC26IC" }, { CC27IC, "CC27IC" },
    { CC28IC, "CC28IC" }, { T7IC, "T7IC" }, { T8IC, "T8IC" },
};

static const ic_node_t CAPCOM2_IC[] = {
    {CC16IC, 0x30, 0}, {CC17IC, 0x31, 0}, {CC18IC, 0x32, 0}, {CC19IC, 0x33, 0},
    {CC20IC, 0x34, 0}, {CC21IC, 0x35, 0}, {CC22IC, 0x36, 0}, {IRQ55IC, 0x37, 0},
    {CC24IC, 0x38, 0}, {CC25IC, 0x39, 0}, {CC26IC, 0x3A, 0}, {CC27IC, 0x3B, 0},
    {CC28IC, 0x3C, 0}, {T7IC, 0x3D, 1}, {T8IC, 0x3E, 1},
};

static const capcom_unit_desc_t CAPCOM2_DESC = {
    .id = "capcom2",
    .timer_name = {"T7", "T8"},
    .con_addr = T78CON,
    .timer_addr = {T7, T8},
    .reload_addr = {T7REL, T8REL},
    .timer_ic_addr = {T7IC, T8IC},
    .ccm_addr = {CCM4, CCM5, CCM6, CCM7},
    .cc_addr = {CC16, CC17, CC18, CC19, CC20, CC21, CC22, CC23, CC24, CC25, CC26, CC27, CC28, CC29, CC30, CC31},
    .cc_ic_addr = {CC16IC, CC17IC, CC18IC, CC19IC, CC20IC, CC21IC, CC22IC, IRQ55IC,
                   CC24IC, CC25IC, CC26IC, CC27IC, CC28IC, CC29IC, CC30IC, CC31IC},
    .channel_count = 13,
    .base_channel = 16,
    .no_output_lo = 24,
    .no_output_hi = 27,
};

static void capcom2_tick(peripheral_t *self, soc_t *s, int n) {
    capcom2_state_t *st = (capcom2_state_t *)self->state;
    cemu_capcom_unit_tick(&st->unit, s, &CAPCOM2_DESC, n);
}

static void capcom2_on_sfr_write(peripheral_t *self, soc_t *s, uint32_t word_addr, uint16_t stored) {
    capcom2_state_t *st = (capcom2_state_t *)self->state;
    cemu_capcom_unit_on_sfr_write(&st->unit, s, &CAPCOM2_DESC, word_addr, stored);
}

static int capcom2_timer_running(peripheral_t *self, soc_t *s, uint32_t ic_addr) {
    capcom2_state_t *st = (capcom2_state_t *)self->state;
    return cemu_capcom_unit_timer_running(&st->unit, s, &CAPCOM2_DESC, ic_addr);
}

static int capcom2_ic_will_fire(peripheral_t *self, soc_t *s, uint32_t ic_addr) {
    capcom2_state_t *st = (capcom2_state_t *)self->state;
    return cemu_capcom_unit_ic_will_fire(&st->unit, s, &CAPCOM2_DESC, ic_addr);
}

static uint64_t capcom2_next_event(peripheral_t *self, soc_t *s) {
    capcom2_state_t *st = (capcom2_state_t *)self->state;
    return cemu_capcom_unit_next_event(&st->unit, s, &CAPCOM2_DESC);
}
static void capcom2_advance_quiet(peripheral_t *self, soc_t *s, uint64_t ticks) {
    capcom2_state_t *st = (capcom2_state_t *)self->state;
    cemu_capcom_unit_advance_quiet(&st->unit, s, &CAPCOM2_DESC, ticks);
}

void cemu_capcom2_periph_init(peripheral_t *p, capcom2_state_t *st,
                         int cc23_irq_enabled) {
    cemu_capcom_unit_reset_state(&st->unit);
    cemu_capcom_unit_set_channel_irq_output(&st->unit, 23 - 16,
                                       cc23_irq_enabled);
    p->id = "capcom2";
    p->state = st;
    p->sfr_words = CAPCOM2_SFR_WORDS;
    p->n_sfr_words = (int)(sizeof(CAPCOM2_SFR_WORDS) / sizeof(CAPCOM2_SFR_WORDS[0]));
    p->byte_ranges = NULL;
    p->n_byte_ranges = 0;
    p->ic_nodes = CAPCOM2_IC;
    p->n_ic_nodes = (int)(sizeof(CAPCOM2_IC) / sizeof(CAPCOM2_IC[0]));
    PERIPHERAL_REG_NAMES(p, CAPCOM2_REG_NAMES, (int)(sizeof(CAPCOM2_REG_NAMES) / sizeof(CAPCOM2_REG_NAMES[0])));
    p->read8 = NULL;
    p->peek8 = NULL;
    p->write8 = NULL;
    p->read_sfr_word = NULL;
    p->on_sfr_poll = NULL;
    p->on_sfr_write = capcom2_on_sfr_write;
    p->tick = capcom2_tick;
    p->next_event_ticks = capcom2_next_event;
    p->advance_quiet = capcom2_advance_quiet;
    p->timer_running = capcom2_timer_running;
    p->ic_will_fire = capcom2_ic_will_fire;
}

void cemu_capcom2_inject_timer_edge(capcom2_state_t *st, soc_t *s, int timer_idx, int rising) {
    cemu_capcom_unit_inject_timer_edge(&st->unit, s, &CAPCOM2_DESC, timer_idx, rising);
}

void cemu_capcom2_inject_channel_edge(capcom2_state_t *st, soc_t *s, int local_channel, int rising) {
    cemu_capcom_unit_inject_channel_edge(&st->unit, s, &CAPCOM2_DESC, local_channel, rising);
}

int cemu_capcom2_output_level(capcom2_state_t *st, int local_channel) {
    return cemu_capcom_unit_output_level(&st->unit, &CAPCOM2_DESC, local_channel);
}

int cemu_capcom2_output_driven(soc_t *s, int local_channel) {
    return cemu_capcom_unit_output_driven(s, &CAPCOM2_DESC, local_channel);
}
