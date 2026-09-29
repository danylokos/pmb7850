#ifndef CEMU_PERIPH_VIRTUAL_CELL_H
#define CEMU_PERIPH_VIRTUAL_CELL_H

#include <stdint.h>

/* Pure deterministic radio policy.  This module deliberately has no SoC,
 * memory, interrupt, or CPU access; the baseband transport owns all guest
 * side effects. */
typedef struct {
    int available;
    int synchronized;
    int service_allowed;
    uint8_t plmn[3];
    uint16_t arfcn;
    uint16_t quality;
    uint16_t level;
    unsigned measurement_index;
} virtual_cell_state_t;

typedef struct {
    int recognized;
    int synchronized;
    uint16_t word0;
    uint16_t word1;
    uint16_t arfcn;
} virtual_cell_result_t;

void cemu_virtual_cell_init(virtual_cell_state_t *cell);
virtual_cell_result_t cemu_virtual_cell_complete_command(
    virtual_cell_state_t *cell, uint16_t command);

#endif /* CEMU_PERIPH_VIRTUAL_CELL_H */
