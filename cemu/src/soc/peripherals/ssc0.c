/* SSC0 high-speed synchronous serial channel. */
#include <string.h>
#include "ssc0.h"
#include "soc.h"

#define SSC0TB    0xF0B0u
#define SSC0RB    0xF0B2u
#define SSC0BR    0xF0B4u
#define SSC0PISEL 0xF0B6u
#define SSC0TIC   0xFF72u
#define SSC0RIC   0xFF74u
#define SSC0EIC   0xFF76u
#define SSC0CON   0xFFB2u

#define SSC_CON_BM_MASK 0x000Fu
#define SSC_CON_HB      (1u << 4)
#define SSC_CON_LB      (1u << 7)
#define SSC_CON_REN     (1u << 9)
#define SSC_CON_MS      (1u << 14)
#define SSC_CON_EN      (1u << 15)
#define SSC_STAT_RE     (1u << 9)
#define SSC_STAT_BSY    (1u << 12)
#define SSC_STAT_ERR_MASK ((1u << 8) | (1u << 9) | (1u << 10) | (1u << 11))

static const uint32_t SSC0_SFR_WORDS[] = {
    SSC0TB, SSC0RB, SSC0BR, SSC0PISEL, SSC0CON,
};

static const reg_name_t SSC0_REG_NAMES[] = {
    { SSC0TB, "SSC0TB" },       { SSC0RB, "SSC0RB" },
    { SSC0BR, "SSC0BR" },       { SSC0PISEL, "SSC0PISEL" },
    { SSC0TIC, "SSC0TIC" },     { SSC0RIC, "SSC0RIC" },
    { SSC0EIC, "SSC0EIC" },     { SSC0CON, "SSC0CON" },
};

static const ic_node_t SSC0_IC_NODES[] = {
    { SSC0TIC, 0x2D, 0 },
    { SSC0RIC, 0x2E, 0 },
    { SSC0EIC, 0x2F, 0 },
};

static uint16_t ssc0_visible_con(const ssc0_state_t *st) {
    return (uint16_t)(st->config | st->status);
}

static void ssc0_store_con(ssc0_state_t *st, soc_t *s) {
    cemu_memory_controller_sfr_put(&s->memory, SSC0CON, ssc0_visible_con(st));
}

static uint16_t ssc0_frame_mask(const ssc0_state_t *st) {
    unsigned bits = (st->config & SSC_CON_BM_MASK) + 1u;
    return bits >= 16u ? 0xFFFFu : (uint16_t)((1u << bits) - 1u);
}

static uint64_t live_icount(const soc_t *s) {
    return s && s->cpu ? s->cpu->icount : 0;
}

static void ssc0_emit_start(soc_t *s, const ssc0_state_t *st, int queued,
                            uint32_t baud_divisor) {
    if (!cemu_event_native_trace_active(&s->instrumentation, "ssc0_transfer_start"))
        return;
    cemu_native_trace_event_t ev;
    memset(&ev, 0, sizeof ev);
    ev.kind = "ssc0_transfer_start";
    ev.icount = live_icount(s);
    ev.pc = s->cpu ? cpu_pc(s->cpu) : 0;
    ev.has_addr = 1; ev.addr = SSC0TB;
    ev.has_size = 1; ev.size = st->shift_bits <= 8 ? 1 : 2;
    ev.has_value = 1; ev.value = st->shift_tx;
    ev.detail = "SSC0 shift start";
    cemu_event_field_i64(&ev.info, "baud_divisor", (long)baud_divisor);
    cemu_event_field_i64(&ev.info, "bits", (long)st->shift_bits);
    cemu_event_field_i64(&ev.info, "completion_tick", (long)st->completion_tick);
    cemu_event_field_bool(&ev.info, "queued", queued);
    cemu_event_field_i64(&ev.info, "start_tick", (long)st->shift_start_tick);
    cemu_event_emit_native_trace(&s->instrumentation, &ev, CEMU_EVENT_PERIPHERAL);
}

static void ssc0_emit_complete(soc_t *s, const ssc0_state_t *st,
                               uint16_t rx, int queued) {
    if (!cemu_event_native_trace_active(&s->instrumentation, "ssc0_transfer_complete"))
        return;
    cemu_native_trace_event_t ev;
    memset(&ev, 0, sizeof ev);
    ev.kind = "ssc0_transfer_complete";
    ev.icount = live_icount(s);
    ev.pc = s->cpu ? cpu_pc(s->cpu) : 0;
    ev.has_addr = 1; ev.addr = SSC0RB;
    ev.has_size = 1; ev.size = st->shift_bits <= 8 ? 1 : 2;
    ev.has_value = 1; ev.value = rx;
    ev.detail = "SSC0 shift complete";
    cemu_event_field_i64(&ev.info, "bits", (long)st->shift_bits);
    cemu_event_field_bool(&ev.info, "queued", queued);
    cemu_event_field_i64(&ev.info, "rx", rx);
    cemu_event_field_i64(&ev.info, "start_tick", (long)st->shift_start_tick);
    cemu_event_field_i64(&ev.info, "tx", st->shift_tx);
    cemu_event_emit_native_trace(&s->instrumentation, &ev, CEMU_EVENT_PERIPHERAL);
}

static void ssc0_start_transfer(ssc0_state_t *st, soc_t *s,
                                uint16_t tx, int queued) {
    unsigned bits = (st->config & SSC_CON_BM_MASK) + 1u;
    uint16_t br = memory_controller_sfr_get(&s->memory, SSC0BR);
    uint32_t baud_divisor = 2u * ((uint32_t)br + 1u);
    uint64_t duration = (uint64_t)baud_divisor * bits;

    st->shift_active = 1;
    st->shift_tx = tx & ssc0_frame_mask(st);
    st->shift_bits = bits;
    st->shift_msb_first = (st->config & SSC_CON_HB) != 0;
    st->shift_start_tick = s->ticks;
    st->completion_tick = s->ticks + duration;
    st->status |= SSC_STAT_BSY;
    ssc0_store_con(st, s);

    if (st->slave_start)
        st->slave_start(st->slave_ctx, s, st->shift_tx, bits,
                        st->shift_msb_first);
    cemu_memory_controller_sfr_put(&s->memory, SSC0TIC, memory_controller_sfr_get(&s->memory, SSC0TIC) | XIC_IR_BIT);
    ssc0_emit_start(s, st, queued, baud_divisor);
}

static void ssc0_abort_transfer(ssc0_state_t *st, soc_t *s) {
    if (st->shift_active && st->slave_abort)
        st->slave_abort(st->slave_ctx, s);
    st->shift_active = 0;
    st->tb_full = 0;
    st->status &= (uint16_t)~SSC_STAT_BSY;
}

static void ssc0_complete_transfer(ssc0_state_t *st, soc_t *s) {
    uint16_t tx = st->shift_tx;
    uint16_t mask = st->shift_bits >= 16u
                  ? 0xFFFFu
                  : (uint16_t)((1u << st->shift_bits) - 1u);
    uint16_t slave_rx = mask;
    if (st->slave_complete)
        slave_rx = st->slave_complete(st->slave_ctx, s, tx,
                                      st->shift_bits,
                                      st->shift_msb_first) & mask;
    uint16_t rx = (st->config & SSC_CON_LB) ? tx : slave_rx;
    int queued = st->tb_full;

    if (st->rb_full && (st->config & SSC_CON_REN)) {
        st->status |= SSC_STAT_RE;
        cemu_memory_controller_sfr_put(&s->memory, SSC0EIC, memory_controller_sfr_get(&s->memory, SSC0EIC) | XIC_IR_BIT);
    }

    cemu_memory_controller_sfr_put(&s->memory, SSC0RB, rx);
    st->rb_full = 1;
    st->shift_active = 0;
    if (!queued) st->status &= (uint16_t)~SSC_STAT_BSY;
    ssc0_store_con(st, s);
    cemu_memory_controller_sfr_put(&s->memory, SSC0RIC, memory_controller_sfr_get(&s->memory, SSC0RIC) | XIC_IR_BIT);
    ssc0_emit_complete(s, st, rx, queued);

    if (queued) {
        uint16_t next = st->tb;
        st->tb_full = 0;
        ssc0_start_transfer(st, s, next, 1);
    }
}

static uint16_t ssc0_read_word(peripheral_t *self, soc_t *s,
                               uint32_t word_addr, uint16_t stored) {
    (void)s;
    ssc0_state_t *st = (ssc0_state_t *)self->state;
    if (word_addr == SSC0CON) return ssc0_visible_con(st);
    return stored;
}

static void ssc0_on_poll(peripheral_t *self, soc_t *s, uint32_t word_addr) {
    (void)s;
    ssc0_state_t *st = (ssc0_state_t *)self->state;
    if (word_addr == SSC0RB) st->rb_full = 0;
}

static void ssc0_on_con_write(ssc0_state_t *st, soc_t *s, uint16_t written) {
    uint16_t old = st->config;
    if (old & SSC_CON_EN) {
        uint16_t errors = st->status & written & SSC_STAT_ERR_MASK;
        st->status = (uint16_t)(errors |
                     (st->shift_active ? SSC_STAT_BSY : 0));
        st->config = (uint16_t)((old & ~(SSC_CON_EN | SSC_CON_MS)) |
                                (written & (SSC_CON_EN | SSC_CON_MS)));
        if (!(st->config & SSC_CON_EN))
            ssc0_abort_transfer(st, s);
    } else {
        st->config = written & (uint16_t)~SSC_STAT_BSY;
        st->status = 0;
    }
    ssc0_store_con(st, s);
}

static void ssc0_on_write(peripheral_t *self, soc_t *s,
                          uint32_t word_addr, uint16_t stored) {
    ssc0_state_t *st = (ssc0_state_t *)self->state;
    if (word_addr == SSC0CON) {
        ssc0_on_con_write(st, s, stored);
        return;
    }
    if (word_addr != SSC0TB ||
        !(st->config & SSC_CON_EN) ||
        !(st->config & SSC_CON_MS))
        return;
    if (!st->shift_active)
        ssc0_start_transfer(st, s, stored, 0);
    else {
        st->tb = stored;
        st->tb_full = 1;
    }
}

static void ssc0_tick(peripheral_t *self, soc_t *s, int n) {
    (void)n;
    ssc0_state_t *st = (ssc0_state_t *)self->state;
    if (st->shift_active && s->ticks >= st->completion_tick)
        ssc0_complete_transfer(st, s);
}

static uint64_t ssc0_next_event(peripheral_t *self, soc_t *s) {
    ssc0_state_t *st = (ssc0_state_t *)self->state;
    if (!st->shift_active) return UINT64_MAX;
    return st->completion_tick <= s->ticks ? 1 : st->completion_tick - s->ticks;
}
static void ssc0_advance_quiet(peripheral_t *self, soc_t *s, uint64_t ticks) {
    (void)self; (void)s; (void)ticks;
}

static int ssc0_ic_will_fire(peripheral_t *self, soc_t *s,
                             uint32_t ic_addr) {
    (void)s;
    ssc0_state_t *st = (ssc0_state_t *)self->state;
    if (!(st->config & SSC_CON_EN) || !(st->config & SSC_CON_MS))
        return 0;
    if (ic_addr == SSC0RIC) return st->shift_active;
    if (ic_addr == SSC0TIC) return st->shift_active && st->tb_full;
    if (ic_addr == SSC0EIC)
        return st->shift_active && st->rb_full &&
               (st->config & SSC_CON_REN);
    return 0;
}

void cemu_ssc0_attach_slave(peripheral_t *p,
                       ssc0_slave_start_fn start,
                       ssc0_slave_complete_fn complete,
                       ssc0_slave_abort_fn abort,
                       void *ctx) {
    ssc0_state_t *st = (ssc0_state_t *)p->state;
    st->slave_start = start;
    st->slave_complete = complete;
    st->slave_abort = abort;
    st->slave_ctx = ctx;
}

void cemu_ssc0_periph_init(peripheral_t *p, ssc0_state_t *st) {
    memset(st, 0, sizeof(*st));
    p->id = "ssc0";
    p->state = st;
    p->sfr_words = SSC0_SFR_WORDS;  p->n_sfr_words = 5;
    p->byte_ranges = NULL;          p->n_byte_ranges = 0;
    p->ic_nodes = SSC0_IC_NODES;    p->n_ic_nodes = 3;
    PERIPHERAL_REG_NAMES(p, SSC0_REG_NAMES, 8);
    p->read8 = NULL;  p->peek8 = NULL;  p->write8 = NULL;
    p->read_sfr_word = ssc0_read_word;
    p->on_sfr_poll = ssc0_on_poll;
    p->on_sfr_write = ssc0_on_write;
    p->tick = ssc0_tick;
    p->next_event_ticks = ssc0_next_event;
    p->advance_quiet = ssc0_advance_quiet;
    p->timer_running = ssc0_ic_will_fire;
    p->ic_will_fire = ssc0_ic_will_fire;
}
