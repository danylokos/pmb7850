/* Fixed CPU-visible import regions used by snapshot full-bin export. `end` is
 * inclusive. File names match snapshot/full-bins/ artifacts. The x55 CEMU
 * DRcov adapter converts these engine-owned ranges to exclusive modules. */
#ifndef CEMU_MEMORY_REGIONS_H
#define CEMU_MEMORY_REGIONS_H

#include <stdint.h>

typedef struct {
    const char *name;
    uint32_t start;
    uint32_t end;      /* inclusive */
    const char *file;
} cemu_memory_region_t;

static const cemu_memory_region_t CEMU_MEMORY_REGIONS[] = {
    {"vectors",             0x000000u, 0x0001FFu, "000000_0001ff_vectors.bin"},
    {"low_external_pre_io", 0x000200u, 0x00DFFFu, "000200_00dfff_low_external_pre_io.bin"},
    {"internal_io_xbus",    0x00E000u, 0x00EFFFu, "00e000_00efff_internal_io_xbus.bin"},
    {"esfr",                0x00F000u, 0x00F1FFu, "00f000_00f1ff_esfr.bin"},
    {"dpram",               0x00F200u, 0x00FDFFu, "00f200_00fdff_dpram.bin"},
    {"sfr",                 0x00FE00u, 0x00FFFFu, "00fe00_00ffff_sfr.bin"},
    {"lm_segment1_window",  0x010000u, 0x04FFFFu, "010000_04ffff_lm_segment1_window.bin"},
    {"low_external_rest",   0x050000u, 0x7FFFFFu, "050000_7fffff_low_external_rest.bin"},
    {"flash_native",        0x800000u, 0xFFFFFFu, "800000_ffffff_flash_native.bin"},
};

#define CEMU_MEMORY_REGION_COUNT ((int)(sizeof(CEMU_MEMORY_REGIONS) / sizeof(CEMU_MEMORY_REGIONS[0])))

#endif /* CEMU_MEMORY_REGIONS_H */
