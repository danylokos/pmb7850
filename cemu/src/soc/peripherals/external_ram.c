/* External SRAM/PSRAM storage. The memory controller supplies the aperture. */
#include <stdlib.h>
#include <string.h>
#include "external_ram.h"

cemu_status_t cemu_external_ram_periph_init(peripheral_t *p,
                                       external_ram_state_t *st,
                                       const external_ram_config_t *cfg) {
    if (!p || !st || !cfg || !cfg->chip_size)
        return cemu_status_error(CEMU_STATUS_INVALID_CONFIGURATION,
                                 "invalid external-RAM configuration");
    memset(st, 0, sizeof *st);
    st->model = cfg->model;
    st->chip_size = cfg->chip_size;
    st->addrsel_index = cfg->addrsel_index;
    st->bytes = cemu_calloc(st->chip_size, 1);
    if (!st->bytes)
        return cemu_status_error(CEMU_STATUS_ALLOCATION_FAILED,
                                 "cannot allocate external-RAM backing");

    memset(p, 0, sizeof *p);
    p->id = "external-ram";
    p->model = cfg->model;
    p->state = st;
    return cemu_status_ok();
}

void cemu_external_ram_state_free(external_ram_state_t *st) {
    free(st->bytes);
    st->bytes = NULL;
}

uint32_t cemu_external_ram_offset(const external_ram_state_t *st,
                             uint32_t window_start, uint32_t cpu_addr) {
    return (cpu_addr - window_start) % st->chip_size;
}
