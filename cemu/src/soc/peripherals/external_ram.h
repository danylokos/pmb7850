/* Dynamically mapped external SRAM/PSRAM behind an ADDRSEL chip select. */
#ifndef CEMU_PERIPH_EXTERNAL_RAM_H
#define CEMU_PERIPH_EXTERNAL_RAM_H

#include <stdint.h>
#include "devices.h"
#include "peripheral.h"
#include "cemu_status.h"

typedef struct {
    const char *model;
    uint32_t chip_size;
    int addrsel_index;
    uint8_t *bytes;
    uint64_t mutation_seq;
} external_ram_state_t;

cemu_status_t cemu_external_ram_periph_init(peripheral_t *p,
                                       external_ram_state_t *st,
                                       const external_ram_config_t *cfg);
void cemu_external_ram_state_free(external_ram_state_t *st);
uint32_t cemu_external_ram_offset(const external_ram_state_t *st,
                             uint32_t window_start, uint32_t cpu_addr);

#endif
