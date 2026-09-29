/* ASC0 asynchronous serial channel — see serial.h. */
#include <string.h>
#include "serial.h"
#include "soc.h"

#define S0TBUF 0xFEB0u
#define S0RBUF 0xFEB2u
#define S0BG   0xFEB4u   /* baud rate generator/reload */
#define S0FDV  0xFEB6u   /* fractional divider */
#define S0TIC  0xFF6Cu   /* transmit IC (bit7 = TIR) */
#define S0RIC  0xFF6Eu   /* receive IC (bit7 = RIR) */
#define S0EIC  0xFF70u   /* error IC (bit7 = EIR) */
#define S0CON  0xFFB0u   /* control register */
#define BSL_SYNC_FLAG 0xFF10u  /* boot flag; bit0 releases the autobaud loop */
#define S0CON_MODE_MASK 0x0007u
#define S0CON_STP (1u << 3)
#define S0CON_REN (1u << 4)
#define S0CON_OEN (1u << 7)
#define S0CON_OE  (1u << 10)
#define S0CON_FDE (1u << 11)
#define S0CON_BRS (1u << 13)
#define S0CON_R   (1u << 15)

/* Words this peripheral reacts to: RX-buffer consumption, control changes,
 * BSL sync flag, and S0TBUF. BG/FDV are claimed so their ASC identity and
 * writes remain tied to this model even though frame configuration is sampled
 * only when a frame starts.
 * P7 is owned by the ports peripheral; it handles autobaud line-sense toggling. */
static const uint32_t SERIAL_SFR_WORDS[] = {
    S0TBUF, S0RBUF, S0BG, S0FDV, S0RIC, S0CON, BSL_SYNC_FLAG,
};

/* Registers this peripheral NAMES (mirror serial.py REGISTER_NAMES). Naming a
 * register is what marks it "modeled" (green) in the run summary.
 * BSL_SYNC_FLAG is claimed for behavior but NOT named here because it aliases
 * PSW's address (a synthetic overlay). */
static const reg_name_t SERIAL_REG_NAMES[] = {
    { S0TBUF, "S0TBUF" }, { S0RBUF, "S0RBUF" }, { S0BG, "S0BG" },
    { S0FDV, "FDV" },
    { S0TIC, "S0TIC" },   { S0RIC, "S0RIC" },   { S0EIC, "S0EIC" },
    { S0CON, "S0CON" },
};

/* IC nodes: S0TIC=trap 0x2A, S0RIC=trap 0x2B, S0EIC=trap 0x2C. */
static const ic_node_t SERIAL_IC_NODES[] = {
    { S0TIC, 0x2A, 0 },
    { S0RIC, 0x2B, 0 },
    { S0EIC, 0x2C, 0 },
};

static unsigned serial_frame_bits(uint16_t con) {
    unsigned mode = con & S0CON_MODE_MASK;
    if (mode == 0) return 8;                 /* synchronous byte */
    if (mode == 1 || mode == 3)              /* 8-bit field */
        return 1u + 8u + 1u + ((con & S0CON_STP) != 0);
    if (mode == 4 || mode == 5 || mode == 7) /* 9-bit field */
        return 1u + 9u + 1u + ((con & S0CON_STP) != 0);
    return 0;                                /* reserved modes 010/110 */
}

static uint64_t serial_frame_ticks(uint16_t con, uint16_t bg,
                                   uint16_t fdv, unsigned frame_bits) {
    uint64_t reload = (uint64_t)(bg & 0x1FFFu) + 1u;
    unsigned mode = con & S0CON_MODE_MASK;
    if (mode == 0) {
        /* C166S V1 UM Table 10-5: synchronous baudrate uses /8 or /12. */
        uint64_t bit_ticks = (con & S0CON_BRS) ? 12u : 8u;
        return bit_ticks * reload * frame_bits;
    }
    if (!(con & S0CON_FDE)) {
        /* C166S V1 UM Table 10-1: fixed asynchronous /32 or /48. */
        uint64_t bit_ticks = (con & S0CON_BRS) ? 48u : 32u;
        return bit_ticks * reload * frame_bits;
    }
    fdv &= 0x01FFu;
    if (!fdv) return 16u * reload * frame_bits;
    /* Table 10-3: baud = fclk*FDV/(8192*(BG+1)). Round a
     * complete host frame up to the next emulator tick. */
    uint64_t numerator = 8192u * reload * frame_bits;
    return (numerator + fdv - 1u) / fdv;
}

static void serial_tx_start(serial_state_t *st, soc_t *s, uint8_t byte) {
    uint16_t con = memory_controller_sfr_get(&s->memory, S0CON);
    unsigned frame_bits = serial_frame_bits(con);
    if (!(con & S0CON_R) || !frame_bits) {
        st->tx_buffer_byte = byte;
        st->tx_buffer_full = 1;
        return;
    }

    st->tx_active = 1;
    st->tx_tir_raised = 0;
    st->tx_byte = byte;
    st->tx_con = con;
    st->tx_bg = memory_controller_sfr_get(&s->memory, S0BG) & 0x1FFFu;
    st->tx_fdv = memory_controller_sfr_get(&s->memory, S0FDV) & 0x01FFu;
    st->tx_frame_bits = frame_bits;
    st->tx_start_tick = s->ticks;
    st->tx_completion_tick = s->ticks +
        serial_frame_ticks(con, st->tx_bg, st->tx_fdv, frame_bits);

    unsigned mode = con & S0CON_MODE_MASK;
    unsigned stop_bits = mode == 0 ? 0u : 1u + ((con & S0CON_STP) != 0);
    unsigned tir_bits = frame_bits - stop_bits;
    st->tx_tir_tick = s->ticks +
        serial_frame_ticks(con, st->tx_bg, st->tx_fdv, tir_bits);
}

static void serial_tx_kick(serial_state_t *st, soc_t *s) {
    if (st->tx_active || !st->tx_buffer_full) return;
    uint8_t byte = st->tx_buffer_byte;
    st->tx_buffer_full = 0;
    serial_tx_start(st, s, byte);
}

static void serial_tx_raise_tir(serial_state_t *st, soc_t *s) {
    if (st->tx_tir_raised) return;
    st->tx_tir_raised = 1;
    cemu_memory_controller_sfr_put(
        &s->memory, S0TIC,
        memory_controller_sfr_get(&s->memory, S0TIC) | XIC_IR_BIT);
}

static void serial_tx_complete(serial_state_t *st, soc_t *s) {
    serial_tx_raise_tir(st, s);
    cemu_soc_serial_tx_push(s, st->tx_byte);
    cemu_soc_trace_serial(s, 1, st->tx_byte);
    st->tx_active = 0;
    st->tx_tir_raised = 0;
    st->tx_frame_bits = 0;
    st->tx_start_tick = 0;
    st->tx_tir_tick = 0;
    st->tx_completion_tick = 0;
    serial_tx_kick(st, s);
}

void cemu_serial_rx_kick(peripheral_t *self, soc_t *s) {
    serial_state_t *st = (serial_state_t *)self->state;
    if (st->rx_active || s->serial_rx_head >= s->serial_rx_len) return;
    uint16_t con = memory_controller_sfr_get(&s->memory, S0CON);
    if ((con & (S0CON_R | S0CON_REN)) != (S0CON_R | S0CON_REN)) return;
    unsigned frame_bits = serial_frame_bits(con);
    if (!frame_bits) return;

    st->rx_active = 1;
    st->rx_byte = s->serial_rx[s->serial_rx_head];
    st->rx_con = con;
    st->rx_bg = memory_controller_sfr_get(&s->memory, S0BG) & 0x1FFFu;
    st->rx_fdv = memory_controller_sfr_get(&s->memory, S0FDV) & 0x01FFu;
    st->rx_frame_bits = frame_bits;
    st->rx_start_tick = s->ticks;
    st->rx_completion_tick = s->ticks +
        serial_frame_ticks(con, st->rx_bg, st->rx_fdv, frame_bits);
}

static void serial_rx_abort(serial_state_t *st) {
    st->rx_active = 0;
    st->rx_byte = 0;
    st->rx_frame_bits = 0;
    st->rx_start_tick = 0;
    st->rx_completion_tick = 0;
}

static void serial_rx_complete(peripheral_t *self, soc_t *s) {
    serial_state_t *st = (serial_state_t *)self->state;
    uint8_t byte = st->rx_byte;
    if (st->rx_full && (st->rx_con & S0CON_OEN)) {
        cemu_memory_controller_sfr_put(
            &s->memory, S0CON,
            memory_controller_sfr_get(&s->memory, S0CON) | S0CON_OE);
        cemu_memory_controller_sfr_put(
            &s->memory, S0EIC,
            memory_controller_sfr_get(&s->memory, S0EIC) | XIC_IR_BIT);
    }
    cemu_memory_controller_sfr_put(&s->memory, S0RBUF, byte);
    st->rx_full = 1;
    cemu_memory_controller_sfr_put(
        &s->memory, S0RIC,
        memory_controller_sfr_get(&s->memory, S0RIC) | XIC_IR_BIT);
    if (s->serial_rx_head < s->serial_rx_len) s->serial_rx_head++;
    serial_rx_abort(st);
    cemu_soc_trace_serial(s, 0, byte);
    cemu_serial_rx_kick(self, s);
}

static void serial_on_poll(peripheral_t *self, soc_t *s, uint32_t word_addr) {
    serial_state_t *st = (serial_state_t *)self->state;
    if (word_addr == S0RBUF) {
        st->rx_full = 0;
        return;
    }
    if (s->serial_autobaud_bypass && word_addr == BSL_SYNC_FLAG)
        cemu_memory_controller_sfr_put(&s->memory, BSL_SYNC_FLAG, memory_controller_sfr_get(&s->memory, BSL_SYNC_FLAG) | 0x0001);
}

static void serial_on_write(peripheral_t *self, soc_t *s, uint32_t word_addr, uint16_t stored) {
    serial_state_t *st = (serial_state_t *)self->state;
    if (word_addr == S0TBUF) {
        uint8_t byte = stored & 0xFFu;
        if (!st->tx_active)
            serial_tx_start(st, s, byte);
        else {
            /* TBUF is one byte deep.  A write while it is already full has
             * normal register semantics: the newer value replaces it. */
            st->tx_buffer_byte = byte;
            st->tx_buffer_full = 1;
        }
        return;
    }
    if (word_addr == S0CON) {
        if (!(stored & S0CON_R)) {
            /* The active byte remains at serial_rx_head for a later restart. */
            if (st->rx_active) serial_rx_abort(st);
            return;
        }
        serial_tx_kick(st, s);
        cemu_serial_rx_kick(self, s);
    }
}

static void serial_tick(peripheral_t *self, soc_t *s, int n) {
    (void)n;
    serial_state_t *st = (serial_state_t *)self->state;
    if (st->tx_active && !st->tx_tir_raised && s->ticks >= st->tx_tir_tick)
        serial_tx_raise_tir(st, s);
    if (st->tx_active && s->ticks >= st->tx_completion_tick)
        serial_tx_complete(st, s);
    if (st->rx_active && s->ticks >= st->rx_completion_tick)
        serial_rx_complete(self, s);
}

static uint64_t serial_next_event(peripheral_t *self, soc_t *s) {
    serial_state_t *st = (serial_state_t *)self->state;
    uint64_t next = UINT64_MAX;
    if (st->tx_active) {
        uint64_t deadline = st->tx_tir_raised
                          ? st->tx_completion_tick : st->tx_tir_tick;
        next = deadline <= s->ticks ? 1u : deadline - s->ticks;
    }
    if (st->rx_active) {
        uint64_t rx = st->rx_completion_tick <= s->ticks
                    ? 1u : st->rx_completion_tick - s->ticks;
        if (rx < next) next = rx;
    }
    return next;
}

static void serial_advance_quiet(peripheral_t *self, soc_t *s,
                                 uint64_t ticks) {
    (void)self; (void)s; (void)ticks;
}

static int serial_ic_will_fire(peripheral_t *self, soc_t *s,
                               uint32_t ic_addr) {
    (void)s;
    serial_state_t *st = (serial_state_t *)self->state;
    if (ic_addr == S0TIC) return st->tx_active && !st->tx_tir_raised;
    if (ic_addr == S0RIC) return st->rx_active;
    if (ic_addr == S0EIC)
        return st->rx_active && st->rx_full && (st->rx_con & S0CON_OEN);
    return 0;
}

void cemu_serial_periph_init(peripheral_t *p, serial_state_t *st) {
    memset(st, 0, sizeof(*st));
    p->id = "asc0";
    p->state = st;
    p->sfr_words = SERIAL_SFR_WORDS;
    p->n_sfr_words = (int)(sizeof SERIAL_SFR_WORDS /
                           sizeof SERIAL_SFR_WORDS[0]);
    p->byte_ranges = NULL;            p->n_byte_ranges = 0;
    p->ic_nodes = SERIAL_IC_NODES;    p->n_ic_nodes = 3;
    PERIPHERAL_REG_NAMES(p, SERIAL_REG_NAMES, 8);
    p->read8 = NULL;  p->peek8 = NULL;  p->write8 = NULL;
    p->read_sfr_word = NULL;
    p->on_sfr_poll = serial_on_poll;
    p->on_sfr_write = serial_on_write;
    p->tick = serial_tick;
    p->next_event_ticks = serial_next_event;
    p->advance_quiet = serial_advance_quiet;
    p->timer_running = serial_ic_will_fire;
    p->ic_will_fire = serial_ic_will_fire;
}
