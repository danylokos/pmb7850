/* PMB7850/C166S memory map, storage, routing, and bus-facing dispatch. */
#ifndef CEMU_MEMORY_CONTROLLER_H
#define CEMU_MEMORY_CONTROLLER_H

#include <stddef.h>
#include <stdint.h>

#include "bus.h"
#include "devices.h"
#include "peripheral.h"
#include "flash.h"
#include "external_ram.h"
#include "cemu_status.h"

/* Address classes. */
#define INTERNAL_IO_START 0xE000u
#define INTERNAL_IO_END   0xF000u
#define DPRAM_START       0xF200u
#define DPRAM_END         0xFE00u
#define ESFR_START        0xF000u
#define ESFR_END          0xF200u
#define SFR_START         0xFE00u
#define SFR_END           0x10000u
#define SFR_BASE          0xF000u
#define SFR_WORDS         0x0800u
#define INTERNAL_IO_WORDS ((INTERNAL_IO_END - INTERNAL_IO_START) / 2)
#define DPRAM_WORDS       ((DPRAM_END - DPRAM_START) / 2)
#define ADDR_SPACE        (1u << 24)

/* Internal Local Memory aliases. */
#define LM_LOW_SIZE       0x008000u
#define LM_HIGH_START     0x018000u
#define LM_SIZE           0x040000u
#define LM_SEG1_START     0x010000u
#define LM_SEG1_END       0x050000u
#define LOW_MIRROR_END    0x800000u

/* Runtime memory-routing controls. */
#define SYSCON_ADDR       0xFF12u
#define SYSCON_XPEN       (1u << 2)
#define SYSCON_ROMEN      (1u << 10)
#define SYSCON_ROMS1      (1u << 12)
#define XPERCON_ADDR      0xF024u
#define XADRS1_ADDR       0xF014u
#define XBCON1_ADDR       0xF114u
#define ADDRSEL1_ADDR     0xFE18u
#define BUSCON0_ADDR      0xFF0Cu
#define BUSCON1_ADDR      0xFF14u
#define BUSCON_BUSACT     (1u << 10)

typedef enum {
    MEMORY_BUS_WINDOW_XBUS = 1,
    MEMORY_BUS_WINDOW_ADDRSEL = 2,
    MEMORY_BUS_WINDOW_BUSCON0 = 3,
} memory_bus_window_kind_t;

typedef struct {
    memory_bus_window_kind_t kind;
    int index;
    uint32_t selector_addr;
    uint32_t control_addr;
    uint16_t selector;
    uint16_t control;
    long start;
    long end;
    long size;
    int configured;
    int active;
    int reserved;
    int bus_active;
    int xpen;
    int xper_enabled;
    int rgsad;
    int rgsz;
    int mctc;
    int rwdc;
    int mttc;
    int btyp;
    int ewen;
    int alectl;
    int bswc;
    int rdyen;
    int csren;
    int cswen;
    char reason[32];
} memory_bus_window_info_t;

typedef struct {
    int chip_index;
    uint32_t chip_offset;
    int command_visible;
} memory_flash_target_t;

/* Immutable physical view used by consumers that must not depend on a
 * transient CPU mapping. */
typedef struct {
    const char *model;
    const uint8_t *bytes;
    size_t size;
} memory_external_ram_view_t;

typedef struct memory_controller {
    soc_t *soc;

    const uint8_t *flash_data;
    size_t flash_len;
    flash_chip_config_t flash_chips[MAX_FLASH_CHIPS];
    size_t flash_file_offsets[MAX_FLASH_CHIPS];
    int n_flash_chips;
    flash_window_config_t flash_windows[MAX_FLASH_WINDOWS];
    int n_flash_windows;
    peripheral_t *flash_endpoints[MAX_FLASH_CHIPS];
    uint8_t *ram;
    uint8_t *present;
    uint8_t *lm;
    uint32_t lm_size;
    uint16_t sfr[SFR_WORDS];

    peripheral_t *sfr_hook[SFR_WORDS];
    peripheral_t *byte_endpoints[MAX_PERIPHERALS];
    int n_byte_endpoints;
    peripheral_t *external_ram_endpoints[MAX_EXTERNAL_RAM_DEVICES];
    int n_external_ram;

    uint32_t *sfr_read_counts;
    uint32_t *sfr_write_counts;
    uint32_t *internal_io_read_counts;
    uint32_t *internal_io_write_counts;
    uint32_t *dpram_read_counts;
    uint32_t *dpram_write_counts;
    uint32_t dpram_recent[16];
    int dpram_recent_head;
    int dpram_recent_len;
    int suppress_dpram_count;
    uint64_t mem_write_seq;
    uint8_t *mem_write_blocks;
    uint32_t mem_write_footprint;
    uint64_t mem_write_segs[256];
    uint64_t xbus_configured_access[8];
    uint64_t xbus_window_access[8];
    uint64_t xbus_unmodeled_access[8];
    uint64_t addrsel_configured_access[5];
    uint64_t addrsel_window_access[5];
    uint64_t buscon0_access;
} memory_controller_t;

static inline int memory_controller_sfr_index(uint32_t word_addr) {
    return (int)((word_addr - SFR_BASE) >> 1);
}

static inline uint32_t memory_controller_word_addr(uint32_t addr) {
    return addr & 0xFFFFFEu;
}

static inline uint16_t memory_controller_sfr_get(
    const memory_controller_t *mc, uint32_t word_addr) {
    return mc->sfr[memory_controller_sfr_index(word_addr)];
}

void cemu_memory_controller_sfr_put(
    memory_controller_t *mc, uint32_t word_addr, uint16_t value);

static inline int memory_controller_ram_present(
    const memory_controller_t *mc, uint32_t addr) {
    return mc->present[addr];
}

static inline uint8_t memory_controller_ram_get(
    const memory_controller_t *mc, uint32_t addr) {
    return mc->ram[addr];
}

static inline void memory_controller_ram_set(
    memory_controller_t *mc, uint32_t addr, uint8_t value) {
    mc->ram[addr] = value;
    mc->present[addr] = 1;
}

cemu_status_t cemu_memory_controller_init(
    memory_controller_t *mc, soc_t *soc, const uint8_t *flash,
    size_t flash_len, const device_config_t *cfg);
void cemu_memory_controller_free(memory_controller_t *mc);
cemu_status_t cemu_memory_controller_register_endpoint(
    memory_controller_t *mc, peripheral_t *peripheral);
cemu_status_t cemu_memory_controller_register_flash(
    memory_controller_t *mc, int chip_index, peripheral_t *peripheral);
cemu_status_t cemu_memory_controller_register_external_ram(
    memory_controller_t *mc, peripheral_t *peripheral);

void cemu_memory_controller_access(
    memory_controller_t *mc, bus_transaction_t *transaction);
uint8_t cemu_memory_controller_peek8(memory_controller_t *mc, uint32_t addr);
uint16_t cemu_memory_controller_peek16(memory_controller_t *mc, uint32_t addr);
void cemu_memory_controller_poke8(
    memory_controller_t *mc, uint32_t addr, uint8_t value);
void cemu_memory_controller_poke16(
    memory_controller_t *mc, uint32_t addr, uint16_t value);

int cemu_memory_controller_flash_translate(
    const memory_controller_t *mc, uint32_t addr,
    memory_flash_target_t *target);
uint8_t cemu_memory_controller_flash_dump_read8(
    const memory_controller_t *mc, size_t file_offset);
cemu_status_t cemu_memory_controller_flash_array_copy(
    const memory_controller_t *mc, int chip_index, size_t offset,
    uint8_t *bytes, size_t size);
flash_state_t *cemu_memory_controller_flash_state(
    const memory_controller_t *mc, int chip_index);
int cemu_memory_controller_lm_translate(
    const memory_controller_t *mc, uint32_t addr, uint32_t *offset);

external_ram_state_t *cemu_memory_controller_external_ram_state(
    const memory_controller_t *mc, int index);
size_t cemu_memory_controller_external_ram_count(
    const memory_controller_t *mc);
int cemu_memory_controller_external_ram_view(
    const memory_controller_t *mc, size_t index,
    memory_external_ram_view_t *view);
uint64_t cemu_memory_controller_external_ram_mutation_seq(
    const memory_controller_t *mc);

const char *cemu_memory_bus_window_kind_name(memory_bus_window_kind_t kind);
int cemu_memory_controller_bus_window_decode(
    memory_controller_t *mc, memory_bus_window_kind_t kind, int index,
    memory_bus_window_info_t *out);
int cemu_memory_controller_bus_route_decode(
    memory_controller_t *mc, uint32_t addr,
    memory_bus_window_info_t *out);
void cemu_memory_controller_xbus_decode(
    memory_controller_t *mc, int index, long *start, long *end,
    int *active, int *configured, char *reason, int reason_cap);

cemu_status_t cemu_memory_controller_enable_stats(memory_controller_t *mc);

#endif /* CEMU_MEMORY_CONTROLLER_H */
