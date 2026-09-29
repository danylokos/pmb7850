/* CAPCOM1 block — T0/T1 plus CC0..CC15. */
#include "capcom1.h"
#include "capcom_common.h"
#include "soc.h"

#define T0     0xFE50u
#define T1     0xFE52u
#define T0REL  0xFE54u
#define T1REL  0xFE56u
#define CC0    0xFE80u
#define CC1    0xFE82u
#define CC2    0xFE84u
#define CC3    0xFE86u
#define CC4    0xFE88u
#define CC5    0xFE8Au
#define CC6    0xFE8Cu
#define CC7    0xFE8Eu
#define CC8    0xFE90u
#define CC9    0xFE92u
#define CC10   0xFE94u
#define CC11   0xFE96u
#define CC12   0xFE98u
#define CC13   0xFE9Au
#define CC14   0xFE9Cu
#define CC15   0xFE9Eu
#define T01CON 0xFF50u
#define CCM0   0xFF52u
#define CCM1   0xFF54u
#define CCM2   0xFF56u
#define CCM3   0xFF58u
#define CC0IC  0xFF78u
#define CC1IC  0xFF7Au
#define CC2IC  0xFF7Cu
#define CC3IC  0xFF7Eu
#define CC4IC  0xFF80u
#define CC5IC  0xFF82u
#define CC6IC  0xFF84u
#define CC7IC  0xFF86u
#define CC8IC  0xFF88u
#define CC9IC  0xFF8Au
#define CC10IC 0xFF8Cu
#define CC11IC 0xFF8Eu
#define CC12IC 0xFF90u
#define CC13IC 0xFF92u
#define CC14IC 0xFF94u
#define CC15IC 0xFF96u
#define T0IC   0xFF9Cu
#define T1IC   0xFF9Eu

static const uint32_t CAPCOM1_SFR_WORDS[] = {
    T0, T1, T0REL, T1REL,
    CC0, CC1, CC2, CC3, CC4, CC5, CC6, CC7,
    CC8, CC9, CC10, CC11, CC12, CC13, CC14, CC15,
    T01CON, CCM0, CCM1, CCM2, CCM3,
    CC0IC, CC1IC, CC2IC, CC3IC, CC4IC, CC5IC, CC6IC, CC7IC,
    CC8IC, CC9IC, CC10IC, CC11IC, CC12IC, CC13IC, CC14IC, CC15IC,
    T0IC, T1IC,
};

static const reg_name_t CAPCOM1_REG_NAMES[] = {
    { T0, "T0" }, { T1, "T1" }, { T0REL, "T0REL" }, { T1REL, "T1REL" },
    { CC0, "CC0" }, { CC1, "CC1" }, { CC2, "CC2" }, { CC3, "CC3" },
    { CC4, "CC4" }, { CC5, "CC5" }, { CC6, "CC6" }, { CC7, "CC7" },
    { CC8, "CC8" }, { CC9, "CC9" }, { CC10, "CC10" }, { CC11, "CC11" },
    { CC12, "CC12" }, { CC13, "CC13" }, { CC14, "CC14" }, { CC15, "CC15" },
    { T01CON, "T01CON" }, { CCM0, "CCM0" }, { CCM1, "CCM1" }, { CCM2, "CCM2" }, { CCM3, "CCM3" },
    { CC0IC, "CC0IC" }, { CC1IC, "CC1IC" }, { CC2IC, "CC2IC" }, { CC3IC, "CC3IC" },
    { CC4IC, "CC4IC" }, { CC5IC, "CC5IC" }, { CC6IC, "CC6IC" }, { CC7IC, "CC7IC" },
    { CC8IC, "CC8IC" }, { CC9IC, "CC9IC" }, { CC10IC, "CC10IC" }, { CC11IC, "CC11IC" },
    { CC12IC, "CC12IC" }, { CC13IC, "CC13IC" }, { CC14IC, "CC14IC" }, { CC15IC, "CC15IC" },
    { T0IC, "T0IC" }, { T1IC, "T1IC" },
};

static const ic_node_t CAPCOM1_IC[] = {
    {CC0IC, 0x10, 0}, {CC1IC, 0x11, 0}, {CC2IC, 0x12, 0}, {CC3IC, 0x13, 0},
    {CC4IC, 0x14, 0}, {CC5IC, 0x15, 0}, {CC6IC, 0x16, 0}, {CC7IC, 0x17, 0},
    {CC8IC, 0x18, 0}, {CC9IC, 0x19, 0}, {CC10IC, 0x1A, 0}, {CC11IC, 0x1B, 0},
    {CC12IC, 0x1C, 0}, {CC13IC, 0x1D, 0}, {CC14IC, 0x1E, 0}, {CC15IC, 0x1F, 0},
    {T0IC, 0x20, 1}, {T1IC, 0x21, 1},
};

static const capcom_unit_desc_t CAPCOM1_DESC = {
    .id = "capcom1",
    .timer_name = {"T0", "T1"},
    .con_addr = T01CON,
    .timer_addr = {T0, T1},
    .reload_addr = {T0REL, T1REL},
    .timer_ic_addr = {T0IC, T1IC},
    .ccm_addr = {CCM0, CCM1, CCM2, CCM3},
    .cc_addr = {CC0, CC1, CC2, CC3, CC4, CC5, CC6, CC7, CC8, CC9, CC10, CC11, CC12, CC13, CC14, CC15},
    .cc_ic_addr = {CC0IC, CC1IC, CC2IC, CC3IC, CC4IC, CC5IC, CC6IC, CC7IC,
                   CC8IC, CC9IC, CC10IC, CC11IC, CC12IC, CC13IC, CC14IC, CC15IC},
    .channel_count = 16,
    .base_channel = 0,
    .no_output_lo = -1,
    .no_output_hi = -1,
};

static void capcom1_tick(peripheral_t *self, soc_t *s, int n) {
    capcom1_state_t *st = (capcom1_state_t *)self->state;
    cemu_capcom_unit_tick(&st->unit, s, &CAPCOM1_DESC, n);
}

static void capcom1_on_sfr_write(peripheral_t *self, soc_t *s, uint32_t word_addr, uint16_t stored) {
    capcom1_state_t *st = (capcom1_state_t *)self->state;
    cemu_capcom_unit_on_sfr_write(&st->unit, s, &CAPCOM1_DESC, word_addr, stored);
}

static int capcom1_timer_running(peripheral_t *self, soc_t *s, uint32_t ic_addr) {
    capcom1_state_t *st = (capcom1_state_t *)self->state;
    return cemu_capcom_unit_timer_running(&st->unit, s, &CAPCOM1_DESC, ic_addr);
}

static int capcom1_ic_will_fire(peripheral_t *self, soc_t *s, uint32_t ic_addr) {
    capcom1_state_t *st = (capcom1_state_t *)self->state;
    return cemu_capcom_unit_ic_will_fire(&st->unit, s, &CAPCOM1_DESC, ic_addr);
}

static uint64_t capcom1_next_event(peripheral_t *self, soc_t *s) {
    capcom1_state_t *st = (capcom1_state_t *)self->state;
    return cemu_capcom_unit_next_event(&st->unit, s, &CAPCOM1_DESC);
}
static void capcom1_advance_quiet(peripheral_t *self, soc_t *s, uint64_t ticks) {
    capcom1_state_t *st = (capcom1_state_t *)self->state;
    cemu_capcom_unit_advance_quiet(&st->unit, s, &CAPCOM1_DESC, ticks);
}

void cemu_capcom1_periph_init(peripheral_t *p, capcom1_state_t *st) {
    cemu_capcom_unit_reset_state(&st->unit);
    p->id = "capcom1";
    p->state = st;
    p->sfr_words = CAPCOM1_SFR_WORDS;
    p->n_sfr_words = (int)(sizeof(CAPCOM1_SFR_WORDS) / sizeof(CAPCOM1_SFR_WORDS[0]));
    p->byte_ranges = NULL;
    p->n_byte_ranges = 0;
    p->ic_nodes = CAPCOM1_IC;
    p->n_ic_nodes = (int)(sizeof(CAPCOM1_IC) / sizeof(CAPCOM1_IC[0]));
    PERIPHERAL_REG_NAMES(p, CAPCOM1_REG_NAMES, (int)(sizeof(CAPCOM1_REG_NAMES) / sizeof(CAPCOM1_REG_NAMES[0])));
    p->read8 = NULL;
    p->peek8 = NULL;
    p->write8 = NULL;
    p->read_sfr_word = NULL;
    p->on_sfr_poll = NULL;
    p->on_sfr_write = capcom1_on_sfr_write;
    p->tick = capcom1_tick;
    p->next_event_ticks = capcom1_next_event;
    p->advance_quiet = capcom1_advance_quiet;
    p->timer_running = capcom1_timer_running;
    p->ic_will_fire = capcom1_ic_will_fire;
}

void cemu_capcom1_inject_timer_edge(capcom1_state_t *st, soc_t *s, int timer_idx, int rising) {
    cemu_capcom_unit_inject_timer_edge(&st->unit, s, &CAPCOM1_DESC, timer_idx, rising);
}

void cemu_capcom1_inject_channel_edge(capcom1_state_t *st, soc_t *s, int local_channel, int rising) {
    cemu_capcom_unit_inject_channel_edge(&st->unit, s, &CAPCOM1_DESC, local_channel, rising);
}

int cemu_capcom1_output_level(capcom1_state_t *st, int local_channel) {
    return cemu_capcom_unit_output_level(&st->unit, &CAPCOM1_DESC, local_channel);
}

int cemu_capcom1_output_driven(soc_t *s, int local_channel) {
    return cemu_capcom_unit_output_driven(s, &CAPCOM1_DESC, local_channel);
}
