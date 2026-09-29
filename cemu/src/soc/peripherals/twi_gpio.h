/* Open-drain, GPIO-observed TWI register-file slave. */
#ifndef CEMU_PERIPH_TWI_GPIO_H
#define CEMU_PERIPH_TWI_GPIO_H

#include <stdint.h>
#include "devices.h"
#include "peripheral.h"

typedef enum {
    TWI_GPIO_IDLE = 0,
    TWI_GPIO_RX_BITS,
    TWI_GPIO_ACK_ASSERT,
    TWI_GPIO_ACK_HIGH,
    TWI_GPIO_ACK_RELEASE,
    TWI_GPIO_TX_BITS,
    TWI_GPIO_TX_RELEASE,
    TWI_GPIO_MASTER_ACK,
    TWI_GPIO_MASTER_ACK_RELEASE,
} twi_gpio_phase_t;

typedef enum {
    TWI_GPIO_RX_ADDRESS = 0,
    TWI_GPIO_RX_REGISTER,
    TWI_GPIO_RX_DATA,
} twi_gpio_rx_kind_t;

typedef enum {
    TWI_GPIO_AFTER_IDLE = 0,
    TWI_GPIO_AFTER_RX,
    TWI_GPIO_AFTER_TX,
} twi_gpio_after_ack_t;

typedef struct {
    uint8_t address;
    uint8_t port;
    uint8_t scl_bit;
    uint8_t sda_bit;
    uint16_t register_count;
    uint8_t registers[MAX_TWI_REGISTERS];

    uint8_t phase;
    uint8_t rx_kind;
    uint8_t after_ack;
    uint8_t rx_byte;
    uint8_t tx_byte;
    uint8_t bit_count;
    uint8_t register_pointer;
    uint8_t selected;
    uint8_t read;
    uint8_t ack;
    uint8_t master_ack;
    uint8_t slave_sda_low;
    uint8_t prev_scl;
    uint8_t prev_sda;
    uint8_t lines_initialized;

    uint64_t starts;
    uint64_t stops;
    uint64_t register_reads;
    uint64_t register_writes;
} twi_gpio_state_t;

void cemu_twi_gpio_periph_init(peripheral_t *p, twi_gpio_state_t *st,
                          const device_twi_config_t *cfg);
void cemu_twi_gpio_finish_restore(peripheral_t *p, soc_t *soc);
int cemu_twi_gpio_debug_write(twi_gpio_state_t *st, soc_t *soc,
                         unsigned reg, uint8_t value);

#endif /* CEMU_PERIPH_TWI_GPIO_H */
