#include <string.h>
#include "virtual_cell.h"

#define GSM_COMMAND_ACCUMULATE 0x00ABu
#define GSM_COMMAND_MEASURE    0x00A9u
#define GSM_MEASUREMENTS       102u
#define GSM_SYNC_SUCCESSES     52u

void cemu_virtual_cell_init(virtual_cell_state_t *cell) {
    memset(cell, 0, sizeof *cell);
    cell->available = 1;
    cell->service_allowed = 1;
    /* 255/01, matching the deterministic SIM's home PLMN byte order. */
    cell->plmn[0] = 0x01;
    cell->plmn[1] = 0x21;
    cell->plmn[2] = 0x10;
    cell->arfcn = 1;
    cell->quality = 80;
    cell->level = 64;
}

virtual_cell_result_t cemu_virtual_cell_complete_command(
    virtual_cell_state_t *cell, uint16_t command) {
    virtual_cell_result_t result = {0};
    if (!cell || !cell->available) return result;

    if (command == GSM_COMMAND_ACCUMULATE) {
        result.recognized = 1;
        cell->measurement_index = 0;
        return result;
    }
    if (command != GSM_COMMAND_MEASURE) return result;

    result.recognized = 1;
    result.synchronized =
        cell->measurement_index < GSM_SYNC_SUCCESSES;
    if (result.synchronized) {
        result.word0 = cell->quality;
        result.word1 = cell->level;
        result.arfcn = cell->arfcn;
        cell->synchronized = 1;
    }
    cell->measurement_index =
        (cell->measurement_index + 1u) % GSM_MEASUREMENTS;
    return result;
}
