#ifndef EMU_EEPROM_TRACE_H
#define EMU_EEPROM_TRACE_H

#include <stddef.h>
#include <stdint.h>

#include "emu_trace.h"
#include "emu_eeprom.h"

typedef struct emu_eeprom_trace emu_eeprom_trace_t;

typedef struct {
    int chip_index;
    const char *chip_name;
    const char *model;
    size_t size;
} emu_eeprom_trace_chip_t;

typedef int (*emu_eeprom_trace_copy_fn)(
    void *opaque, int chip_index, size_t offset,
    uint8_t *bytes, size_t size);

typedef enum {
    EMU_EEPROM_MUTATION_PROGRAM,
    EMU_EEPROM_MUTATION_ERASE,
} emu_eeprom_trace_mutation_kind_t;

int emu_eeprom_trace_create(
    emu_eeprom_trace_t **out, emu_trace_sink_t *sink,
    const emu_eeprom_trace_chip_t *chips, size_t chip_count,
    emu_eeprom_trace_copy_fn copy, void *copy_opaque);
int emu_eeprom_trace_arm(
    emu_eeprom_trace_t *trace, uint64_t tick, uint64_t icount, uint32_t pc);
void emu_eeprom_trace_read(
    emu_eeprom_trace_t *trace, int chip_index, uint32_t chip_offset,
    uint32_t guest_addr, uint32_t value, size_t size,
    uint64_t tick, uint64_t icount, uint32_t pc);
void emu_eeprom_trace_mutation(
    emu_eeprom_trace_t *trace, int chip_index,
    emu_eeprom_trace_mutation_kind_t kind,
    uint32_t offset, uint32_t size,
    uint64_t tick, uint64_t icount, uint32_t pc);
/* Event-free rebuild used after debugger checkpoint restore. Established
 * physical geometry is retained even when a guest erased identifying headers. */
int emu_eeprom_trace_resync(emu_eeprom_trace_t *trace);
const emu_eeprom_catalog_t *emu_eeprom_trace_catalog(
    const emu_eeprom_trace_t *trace, int chip_index);
void emu_eeprom_trace_destroy(emu_eeprom_trace_t **trace);

#endif
