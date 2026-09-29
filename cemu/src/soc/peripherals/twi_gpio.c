/* GPIO bit-banged TWI register-file slave.
 *
 * The peripheral observes canonical port latch/direction storage once per
 * guest instruction. A line is driven low only when DP=1 and P=0; every other
 * master state releases it. The slave only pulls SDA low and publishes the
 * resolved levels through the ports peripheral. */
#include <string.h>
#include "soc.h"
#include "twi_gpio.h"

#define P3  0xFFC4u
#define DP3 0xFFC6u
#define P6  0xFFCCu
#define DP6 0xFFCEu
#define P7  0xFFD0u
#define DP7 0xFFD2u
#define P8  0xFFD4u
#define DP8 0xFFD6u

static uint32_t data_addr(int port) {
    switch (port) {
    case SOC_PORT_P3: return P3;
    case SOC_PORT_P6: return P6;
    case SOC_PORT_P7: return P7;
    case SOC_PORT_P8: return P8;
    default: return 0;
    }
}

static uint32_t direction_addr(int port) {
    switch (port) {
    case SOC_PORT_P3: return DP3;
    case SOC_PORT_P6: return DP6;
    case SOC_PORT_P7: return DP7;
    case SOC_PORT_P8: return DP8;
    default: return 0;
    }
}

static int master_drives_low(soc_t *soc, int port, int bit) {
    uint16_t mask = (uint16_t)(1u << bit);
    uint16_t data = memory_controller_sfr_get(&soc->memory, data_addr(port));
    uint16_t direction =
        memory_controller_sfr_get(&soc->memory, direction_addr(port));
    return (direction & mask) && !(data & mask);
}

static void emit_twi_event(soc_t *soc, const char *event,
                           int has_address, int address,
                           int has_read, int read,
                           int has_accepted, int accepted,
                           int has_register, int reg,
                           int has_data, int data,
                           int has_ack, int ack) {
    if (!cemu_event_native_trace_active(&soc->instrumentation, "twi_event"))
        return;
    cemu_event_fields_t info = {0};
    if (has_address) cemu_event_field_i64(&info, "address", address);
    if (has_accepted) cemu_event_field_bool(&info, "accepted", accepted);
    if (has_ack) cemu_event_field_bool(&info, "ack", ack);
    if (has_data) cemu_event_field_i64(&info, "data", data);
    cemu_event_field_string(&info, "event", event);
    if (has_read) cemu_event_field_bool(&info, "read", read);
    if (has_register) cemu_event_field_i64(&info, "register", reg);
    cemu_soc_emit_native_trace(soc, "twi_event", 0, 0, 0, 0, 0, 0, event, &info);
}

static void set_slave_sda(twi_gpio_state_t *st, int low) {
    st->slave_sda_low = low ? 1 : 0;
}

static void begin_receive(twi_gpio_state_t *st, twi_gpio_rx_kind_t kind) {
    st->phase = TWI_GPIO_RX_BITS;
    st->rx_kind = (uint8_t)kind;
    st->rx_byte = 0;
    st->bit_count = 0;
}

static uint8_t register_value(const twi_gpio_state_t *st) {
    if (st->register_pointer >= st->register_count) return 0xFF;
    return st->registers[st->register_pointer];
}

static void begin_transmit(twi_gpio_state_t *st, soc_t *soc) {
    st->tx_byte = register_value(st);
    st->bit_count = 0;
    st->phase = TWI_GPIO_TX_BITS;
    set_slave_sda(st, !(st->tx_byte & 0x80u));
    st->register_reads++;
    emit_twi_event(soc, "read", 0, 0, 0, 0, 0, 0,
                   1, st->register_pointer, 1, st->tx_byte, 0, 0);
}

static void on_start(twi_gpio_state_t *st, soc_t *soc) {
    set_slave_sda(st, 0);
    st->selected = 0;
    st->read = 0;
    st->starts++;
    begin_receive(st, TWI_GPIO_RX_ADDRESS);
    emit_twi_event(soc, "start", 0, 0, 0, 0, 0, 0,
                   0, 0, 0, 0, 0, 0);
}

static void on_stop(twi_gpio_state_t *st, soc_t *soc) {
    set_slave_sda(st, 0);
    st->selected = 0;
    st->read = 0;
    st->phase = TWI_GPIO_IDLE;
    st->stops++;
    emit_twi_event(soc, "stop", 0, 0, 0, 0, 0, 0,
                   0, 0, 0, 0, 0, 0);
}

static void receive_byte(twi_gpio_state_t *st, soc_t *soc) {
    uint8_t value = st->rx_byte;
    st->ack = 0;
    st->after_ack = TWI_GPIO_AFTER_IDLE;

    if (st->rx_kind == TWI_GPIO_RX_ADDRESS) {
        uint8_t address = (uint8_t)(value >> 1);
        st->read = value & 1u;
        st->selected = address == st->address;
        st->ack = st->selected;
        if (st->selected)
            st->after_ack = st->read ? TWI_GPIO_AFTER_TX
                                     : TWI_GPIO_AFTER_RX;
        emit_twi_event(soc, "address", 1, address, 1, st->read,
                       1, st->selected, 0, 0, 0, 0, 1, st->ack);
    } else if (st->rx_kind == TWI_GPIO_RX_REGISTER) {
        st->register_pointer = value;
        st->ack = value < st->register_count;
        st->after_ack = st->ack ? TWI_GPIO_AFTER_RX : TWI_GPIO_AFTER_IDLE;
        emit_twi_event(soc, "register_pointer", 0, 0, 0, 0,
                       1, st->ack, 1, value, 0, 0, 1, st->ack);
    } else {
        uint8_t reg = st->register_pointer;
        st->ack = reg < st->register_count;
        if (st->ack) {
            st->registers[reg] = value;
            st->register_pointer++;
            st->register_writes++;
        }
        st->after_ack = st->ack ? TWI_GPIO_AFTER_RX : TWI_GPIO_AFTER_IDLE;
        emit_twi_event(soc, "write", 0, 0, 0, 0, 1, st->ack,
                       1, reg, 1, value, 1, st->ack);
    }
    st->phase = TWI_GPIO_ACK_ASSERT;
}

static void rising_edge(twi_gpio_state_t *st, soc_t *soc, int sda) {
    switch ((twi_gpio_phase_t)st->phase) {
    case TWI_GPIO_RX_BITS:
        st->rx_byte = (uint8_t)((st->rx_byte << 1) | (sda ? 1u : 0u));
        if (++st->bit_count == 8) receive_byte(st, soc);
        break;
    case TWI_GPIO_ACK_HIGH:
        st->phase = TWI_GPIO_ACK_RELEASE;
        break;
    case TWI_GPIO_TX_BITS:
        if (++st->bit_count == 8) st->phase = TWI_GPIO_TX_RELEASE;
        break;
    case TWI_GPIO_MASTER_ACK:
        st->master_ack = sda ? 0 : 1;
        st->phase = TWI_GPIO_MASTER_ACK_RELEASE;
        emit_twi_event(soc, "master_ack", 0, 0, 0, 0, 0, 0,
                       1, st->register_pointer, 0, 0, 1, st->master_ack);
        break;
    default:
        break;
    }
}

static void falling_edge(twi_gpio_state_t *st, soc_t *soc) {
    switch ((twi_gpio_phase_t)st->phase) {
    case TWI_GPIO_ACK_ASSERT:
        set_slave_sda(st, st->ack);
        st->phase = TWI_GPIO_ACK_HIGH;
        break;
    case TWI_GPIO_ACK_RELEASE:
        set_slave_sda(st, 0);
        if (!st->ack || st->after_ack == TWI_GPIO_AFTER_IDLE) {
            st->phase = TWI_GPIO_IDLE;
        } else if (st->after_ack == TWI_GPIO_AFTER_TX) {
            begin_transmit(st, soc);
        } else {
            begin_receive(st, st->rx_kind == TWI_GPIO_RX_ADDRESS
                                  ? TWI_GPIO_RX_REGISTER
                                  : TWI_GPIO_RX_DATA);
        }
        break;
    case TWI_GPIO_TX_BITS:
        set_slave_sda(st, !(st->tx_byte & (uint8_t)(0x80u >> st->bit_count)));
        break;
    case TWI_GPIO_TX_RELEASE:
        set_slave_sda(st, 0);
        st->phase = TWI_GPIO_MASTER_ACK;
        break;
    case TWI_GPIO_MASTER_ACK_RELEASE:
        if (st->master_ack) {
            st->register_pointer++;
            begin_transmit(st, soc);
        } else {
            st->selected = 0;
            st->phase = TWI_GPIO_IDLE;
        }
        break;
    default:
        break;
    }
}

static void publish_lines(twi_gpio_state_t *st, soc_t *soc,
                          int scl, int sda) {
    if (!st->lines_initialized || scl != st->prev_scl)
        cemu_soc_port_input_level(soc, st->port, st->scl_bit, scl);
    if (!st->lines_initialized || sda != st->prev_sda)
        cemu_soc_port_input_level(soc, st->port, st->sda_bit, sda);
    st->prev_scl = scl ? 1 : 0;
    st->prev_sda = sda ? 1 : 0;
    st->lines_initialized = 1;
}

static void twi_gpio_tick(peripheral_t *self, soc_t *soc, int n) {
    twi_gpio_state_t *st = self->state;
    for (int i = 0; i < n; i++) {
        int scl = !master_drives_low(soc, st->port, st->scl_bit);
        int sda = !(master_drives_low(soc, st->port, st->sda_bit) ||
                    st->slave_sda_low);

        if (st->lines_initialized && scl && st->prev_sda != sda) {
            if (st->prev_sda && !sda) on_start(st, soc);
            else if (!st->prev_sda && sda) on_stop(st, soc);
        }

        if (st->lines_initialized && !st->prev_scl && scl)
            rising_edge(st, soc, sda);
        else if (st->lines_initialized && st->prev_scl && !scl)
            falling_edge(st, soc);

        sda = !(master_drives_low(soc, st->port, st->sda_bit) ||
                st->slave_sda_low);
        publish_lines(st, soc, scl, sda);
    }
}

static uint64_t twi_gpio_next_event_ticks(
    peripheral_t *self, soc_t *soc) {
    (void)self;
    (void)soc;
    return UINT64_MAX;
}

static void twi_gpio_advance_quiet(
    peripheral_t *self, soc_t *soc, uint64_t ticks) {
    (void)self;
    (void)soc;
    (void)ticks;
}

void cemu_twi_gpio_periph_init(peripheral_t *p, twi_gpio_state_t *st,
                          const device_twi_config_t *cfg) {
    memset(st, 0, sizeof *st);
    st->address = cfg->address;
    st->port = cfg->port;
    st->scl_bit = cfg->scl_bit;
    st->sda_bit = cfg->sda_bit;
    st->register_count = cfg->register_count;
    if (st->register_count > MAX_TWI_REGISTERS)
        st->register_count = MAX_TWI_REGISTERS;
    memcpy(st->registers, cfg->registers, st->register_count);
    st->prev_scl = 1;
    st->prev_sda = 1;

    memset(p, 0, sizeof *p);
    p->id = "twi-gpio";
    p->model = cfg->model;
    p->state = st;
    p->tick = twi_gpio_tick;
    p->next_event_ticks = twi_gpio_next_event_ticks;
    p->advance_quiet = twi_gpio_advance_quiet;
}

void cemu_twi_gpio_finish_restore(peripheral_t *p, soc_t *soc) {
    if (!p) return;
    twi_gpio_state_t *st = p->state;
    cemu_soc_port_input_restore_level(soc, st->port, st->scl_bit, st->prev_scl);
    cemu_soc_port_input_restore_level(soc, st->port, st->sda_bit, st->prev_sda);
}

int cemu_twi_gpio_debug_write(twi_gpio_state_t *st, soc_t *soc,
                         unsigned reg, uint8_t value) {
    if (!st || reg >= st->register_count) return 0;
    st->registers[reg] = value;
    emit_twi_event(soc, "debugger_write", 0, 0, 0, 0, 1, 1,
                   1, (int)reg, 1, value, 0, 0);
    return 1;
}
